#include "TestCall.h"
#include "DmrDecoder.h"
#include "VoiceEncoder.h"

#include <QDateTime>
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstring>
#include <dlfcn.h>


namespace TestCall {

QStringList modes() { return {"DMR", "D-Star", "YSF", "P25", "NXDN", "M17"}; }

// ---------------------------------------------------------------- speech (eSpeak NG, loaded at run time)
namespace {
using SynthCallback = int (*)(short *, int, void *);
struct Espeak {
    int (*initialize)(int output, int buflength, const char *path, int options) = nullptr;
    void (*setSynthCallback)(SynthCallback) = nullptr;
    int (*setVoiceByName)(const char *) = nullptr;
    int (*setParameter)(int parameter, int value, int relative) = nullptr;
    int (*synth)(const void *text, size_t size, unsigned position, int positionType, unsigned endPosition,
                 unsigned flags, unsigned *uniqueId, void *userData) = nullptr;
    int (*synchronize)() = nullptr;
    int rate = 0;
    bool ok = false;
};
std::vector<int16_t> *g_capture = nullptr;

int onSynth(short *wav, int n, void *)
{
    if (wav && n > 0 && g_capture) g_capture->insert(g_capture->end(), wav, wav + n);
    return 0;
}

Espeak &espeak()
{
    static Espeak e;
    static bool tried = false;
    if (tried) return e;
    tried = true;
    void *h = dlopen("libespeak-ng.so.1", RTLD_NOW);
    if (!h) return e;
    e.initialize = reinterpret_cast<decltype(e.initialize)>(dlsym(h, "espeak_Initialize"));
    e.setSynthCallback = reinterpret_cast<decltype(e.setSynthCallback)>(dlsym(h, "espeak_SetSynthCallback"));
    e.setVoiceByName = reinterpret_cast<decltype(e.setVoiceByName)>(dlsym(h, "espeak_SetVoiceByName"));
    e.setParameter = reinterpret_cast<decltype(e.setParameter)>(dlsym(h, "espeak_SetParameter"));
    e.synth = reinterpret_cast<decltype(e.synth)>(dlsym(h, "espeak_Synth"));
    e.synchronize = reinterpret_cast<decltype(e.synchronize)>(dlsym(h, "espeak_Synchronize"));
    if (!e.initialize || !e.setSynthCallback || !e.synth) return e;
    e.rate = e.initialize(2 /* AUDIO_OUTPUT_SYNCHRONOUS */, 0, nullptr, 0);
    if (e.rate <= 0) return e;
    e.setSynthCallback(onSynth);
    e.ok = true;
    return e;
}

// Windowed-sinc resampler with a ~3.4 kHz low-pass, for speech into 8 kHz codecs.
std::vector<int16_t> resample(const std::vector<int16_t> &in, int inRate, int outRate)
{
    const double ratio = double(inRate) / outRate;
    const double fc = 3400.0 / inRate;       // cutoff as a fraction of the input rate
    const int half = 32;
    std::vector<int16_t> out(size_t(in.size() / ratio));
    for (size_t n = 0; n < out.size(); ++n) {
        const double t = n * ratio;
        const long c = long(t);
        double acc = 0, wsum = 0;
        for (long k = c - half + 1; k <= c + half; ++k) {
            if (k < 0 || k >= long(in.size())) continue;
            const double x = t - k;
            const double sinc = std::fabs(x) < 1e-9 ? 2 * fc : std::sin(2 * M_PI * fc * x) / (M_PI * x);
            const double win = 0.5 + 0.5 * std::cos(M_PI * x / half);
            acc += in[size_t(k)] * sinc * win;
            wsum += sinc * win;
        }
        out[n] = int16_t(std::clamp(wsum != 0 ? acc / wsum : 0.0, -32767.0, 32767.0));
    }
    return out;
}
}   // namespace

bool speak(const QString &text, const QString &voice, int rate, std::vector<int16_t> &pcm8k, QString &err)
{
    Espeak &e = espeak();
    if (!e.ok) { err = "eSpeak NG (libespeak-ng.so.1) is not available"; return false; }
    if (e.setVoiceByName && e.setVoiceByName(voice.toUtf8().constData()) != 0) e.setVoiceByName("en");
    if (e.setParameter) e.setParameter(1 /* espeakRATE */, rate, 0);
    std::vector<int16_t> raw;
    g_capture = &raw;
    const QByteArray utf8 = text.toUtf8();
    e.synth(utf8.constData(), size_t(utf8.size() + 1), 0, 1 /* POS_CHARACTER */, 0, 0 /* espeakCHARS_AUTO */,
            nullptr, nullptr);
    if (e.synchronize) e.synchronize();
    g_capture = nullptr;
    if (raw.empty()) { err = "eSpeak NG produced no audio"; return false; }

    std::vector<int16_t> s = resample(raw, e.rate, 8000);
    // Level it (peak ~ -6 dBFS) and pad with 200 ms of silence at each end.
    int peak = 1;
    for (int16_t v : s) peak = std::max(peak, std::abs(int(v)));
    const double g = 16000.0 / peak;
    pcm8k.assign(1600, 0);
    for (int16_t v : s) pcm8k.push_back(int16_t(std::clamp(v * g, -32767.0, 32767.0)));
    pcm8k.insert(pcm8k.end(), 1600, 0);
    // Whole 20 ms frames, and a multiple of 120 ms so every mode ends on a full packet.
    const size_t frames = (pcm8k.size() + 959) / 960 * 6;
    pcm8k.resize(frames * 160, 0);
    return true;
}

// ---------------------------------------------------------------- packet builders
QByteArray wirePayload(const QByteArray &record)
{
    if (record.startsWith("DMRD")) return record;
    return record.mid(5);   // "DSTRN", "YSFXN", "P25XN", "NXDXN", "M17XN"
}

Plan build(const QString &mode, const std::vector<int16_t> &pcm, const QString &textMessage, bool echo,
           const Settings &cfg, QString &err)
{
    TxTarget t;
    t.mode = mode;
    t.text = textMessage;
    t.dstarSuffix = "TEST";
    if (mode == "DMR") {
        t.dst = echo ? cfg.testCall.dmrEchoId : cfg.testCall.localTalkgroup;
        t.privateCall = echo;
    } else if (mode == "D-Star") {
        t.dstarYour = echo ? cfg.testCall.dstarEcho : QString("CQCQCQ");
    } else if (mode == "P25") {
        t.dst = echo ? cfg.testCall.p25EchoTg : cfg.testCall.localTalkgroup;
    } else if (mode == "NXDN") {
        t.dst = echo ? cfg.testCall.nxdnEchoTg : cfg.testCall.localTalkgroup;
    } else if (mode == "M17") {
        t.m17Dst = echo ? cfg.testCall.m17Echo : QString();
    }
    auto enc = VoiceEncoder::create(cfg, t, &err);
    if (!enc) return {};
    Plan plan;
    plan.intervalMs = enc->recordIntervalMs();
    plan.records << enc->begin();
    for (size_t f = 0; f + 160 <= pcm.size(); f += 160) plan.records << enc->frame(pcm.data() + f);
    plan.records << enc->end();
    return plan;
}

}   // namespace TestCall
