#include "SettingsDialog.h"
#include "TestCall.h"
#include "AudioIn.h"
#include <algorithm>
#include <cmath>
#include <QProgressBar>

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {
QSpinBox *spin(int lo, int hi, int v, const QString &suffix = {})
{
    auto *s = new QSpinBox;
    s->setRange(lo, hi);
    s->setValue(v);
    s->setSuffix(suffix);
    s->setGroupSeparatorShown(false);
    return s;
}
QLineEdit *line(const QString &v, const QString &placeholder = {})
{
    auto *e = new QLineEdit(v);
    e->setPlaceholderText(placeholder);
    return e;
}
QLabel *note(const QString &text)
{
    auto *l = new QLabel(text);
    l->setWordWrap(true);
    l->setStyleSheet("color: palette(placeholder-text);");
    return l;
}
}   // namespace

SettingsDialog::SettingsDialog(const Settings &s, QWidget *parent) : QDialog(parent), s_(s)
{
    setWindowTitle("Settings");
    auto *tabs = tabs_ = new QTabWidget;

    auto addPage = [tabs](const QString &title) {
        auto *w = new QWidget;
        auto *f = new QFormLayout(w);
        f->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        tabs->addTab(w, title);
        return f;
    };
    auto withBrowse = [this](QLineEdit *e) {
        auto *w = new QWidget;
        auto *h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        auto *b = new QPushButton("Browse…");
        connect(b, &QPushButton::clicked, this, [this, e] {
            const QString d = QFileDialog::getExistingDirectory(this, "Choose folder", e->text().isEmpty() ? e->placeholderText() : e->text());
            if (!d.isEmpty()) e->setText(d);
        });
        h->addWidget(e, 1);
        h->addWidget(b);
        return w;
    };

    // ---- Connection
    QFormLayout *f = addPage("Connection");
    f->addRow("Pi-Star host", host_ = line(s.connection.host, "IP address or hostname"));
    f->addRow("SSH user", user_ = line(s.connection.user));
    f->addRow("SSH key file", identity_ = line(s.connection.identityFile, "default (~/.ssh keys)"));
    f->addRow(note("The app logs in with ssh (key authentication) and needs passwordless sudo on the Pi. "
                   "Changing these reconnects."));

    // ---- Station
    f = addPage("Station");
    f->addRow("Callsign", callsign_ = line(s.station.callsign));
    callsign_->setValidator(new QRegularExpressionValidator(QRegularExpression("[A-Za-z0-9/]{1,8}"), callsign_));
    f->addRow("DMR ID", dmrId_ = spin(0, 16777215, int(s.station.dmrId)));
    f->addRow("Hotspot (repeater) ID", repeaterId_ = spin(0, 999999999, int(s.station.repeaterId)));
    f->addRow("NXDN ID", nxdnId_ = spin(0, 65535, int(s.station.nxdnId)));
    f->addRow("DMR timeslot", slot_ = spin(1, 2, s.station.dmrSlot));
    f->addRow("DMR colour code", cc_ = spin(0, 15, s.station.colourCode));
    f->addRow("D-Star module", dstarModule_ = line(s.station.dstarModule));
    dstarModule_->setMaxLength(1);
    f->addRow(note("Your identity for test calls and transmitting, and for labelling your own calls (useful "
                   "when your NXDN ID is not in Pi-Star's NXDN.csv). The P25 source ID is the DMR ID."));

    // ---- Pager
    f = addPage("Pager");
    f->addRow("My RICs", rics_ = line(s.pager.myRics, "e.g. 1234567, 7654321"));
    f->addRow("", onlyMine_ = new QCheckBox("Pager tab shows only my RICs"));
    onlyMine_->setChecked(s.pager.onlyMine);
    f->addRow(note("Pages to these RICs raise the banner and count as unread."));

    // ---- Audio
    f = addPage("Audio");
    f->addRow("Volume", volume_ = spin(0, 150, s.audio.volume, " %"));
    f->addRow("", mute_ = new QCheckBox("Muted"));
    mute_->setChecked(s.audio.mute);
    f->addRow("Output", backend_ = new QComboBox);
    backend_->addItem("Automatic (PulseAudio / PipeWire, else ALSA)", "auto");
    backend_->addItem("PulseAudio / PipeWire", "pulse");
    backend_->addItem("ALSA", "alsa");
    backend_->setCurrentIndex(qMax(0, backend_->findData(s.audio.backend)));
    f->addRow("Output device", device_ = new QComboBox);
    device_->setEditable(true);   // ALSA device names (e.g. hw:1,0) can be typed
    device_->addItem("System default", QString());
    for (const auto &snk : AudioIn::sinks())
        device_->addItem(snk.second.isEmpty() ? snk.first : snk.second, snk.first);
    if (!s.audio.device.isEmpty() && device_->findData(s.audio.device) < 0)
        device_->addItem(s.audio.device + " (not connected)", s.audio.device);
    device_->setCurrentIndex(qMax(0, device_->findData(s.audio.device)));
    {   // speaker test: plays through the device and volume selected here, before saving
        auto *w = new QWidget;
        auto *h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        spkTest_ = new QPushButton("Test speaker");
        spkMeter_ = new QProgressBar;
        spkMeter_->setRange(0, 100);
        spkMeter_->setTextVisible(false);
        spkMeter_->setMaximumHeight(14);
        h->addWidget(spkTest_);
        h->addWidget(spkMeter_, 1);
        f->addRow("", w);
        connect(spkTest_, &QPushButton::clicked, this, &SettingsDialog::startSpeakerTest);
    }
    f->addRow("Microphone", txDevice_ = new QComboBox);
    txDevice_->addItem("System default", QString());
    for (const auto &src : AudioIn::sources())
        if (!src.first.contains(".monitor")) txDevice_->addItem(src.second.isEmpty() ? src.first : src.second, src.first);
    if (txDevice_->findData(s.transmit.inputDevice) < 0 && !s.transmit.inputDevice.isEmpty())
        txDevice_->addItem(s.transmit.inputDevice + " (not connected)", s.transmit.inputDevice);
    txDevice_->setCurrentIndex(qMax(0, txDevice_->findData(s.transmit.inputDevice)));
    f->addRow("Mic gain", micGain_ = new QDoubleSpinBox);
    micGain_->setRange(-20, 30);
    micGain_->setSuffix(" dB");
    micGain_->setValue(s.transmit.micGainDb);
    f->addRow("", autoLevel_ = new QCheckBox("Automatic level control"));
    autoLevel_->setChecked(s.transmit.autoLevel);
    {   // microphone test: live level, optionally heard through the output above
        auto *w = new QWidget;
        auto *h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        micTest_ = new QPushButton("Test microphone");
        micTest_->setCheckable(true);
        micMeter_ = new QProgressBar;
        micMeter_->setRange(0, 100);
        micMeter_->setTextVisible(false);
        micMeter_->setMaximumHeight(14);
        micDb_ = new QLabel("—");
        micDb_->setMinimumWidth(70);
        monitor_ = new QCheckBox("Hear myself");
        monitor_->setToolTip("Play the microphone through the output device (use headphones to avoid feedback)");
        h->addWidget(micTest_);
        h->addWidget(micMeter_, 1);
        h->addWidget(micDb_);
        h->addWidget(monitor_);
        f->addRow("", w);
        connect(micTest_, &QPushButton::toggled, this, &SettingsDialog::toggleMicTest);
        // Restart the test on the new device / output if they change while it runs.
        connect(txDevice_, &QComboBox::currentIndexChanged, this, [this] { if (micTest_->isChecked()) { toggleMicTest(false); toggleMicTest(true); } });
        connect(monitor_, &QCheckBox::toggled, this, [this] { if (micTest_->isChecked()) { toggleMicTest(false); toggleMicTest(true); } });
    }
    f->addRow(note("Meters show dBFS (0 = full scale). Speech on the microphone test should mostly reach "
                   "about −20 to −6 dBFS after gain; automatic level control evens it out while transmitting."));
    f->addRow("", gate_ = new QCheckBox("Noise gate: turn background noise down between words"));
    gate_->setChecked(s.audio.noiseGate);
    auto dspin = [](double lo, double hi, double v, const QString &suffix) {
        auto *d = new QDoubleSpinBox;
        d->setRange(lo, hi);
        d->setDecimals(0);
        d->setSuffix(suffix);
        d->setValue(v);
        return d;
    };
    f->addRow("Gate opens at", gateThr_ = dspin(3, 30, s.audio.gateThresholdDb, " dB above background"));
    f->addRow("Background reduced by", gateRed_ = dspin(6, 60, s.audio.gateReductionDb, " dB"));
    f->addRow("Hold after speech", gateHold_ = spin(50, 1000, s.audio.gateHoldMs, " ms"));
    for (QWidget *w : std::initializer_list<QWidget *>{gateThr_, gateRed_, gateHold_}) {
        w->setEnabled(gate_->isChecked());
        connect(gate_, &QCheckBox::toggled, w, &QWidget::setEnabled);
    }
    f->addRow(note("Calls and recordings play through the output device. With the ALSA output you can type "
                   "an ALSA device such as hw:1,0."));

    // ---- Recording & logs
    f = addPage("Recording && logs");
    f->addRow("Keep recordings of the last", keepCalls_ = spin(0, 1000, s.recording.keepCalls, " calls"));
    f->addRow("Don't keep calls shorter than", minSeconds_ = new QDoubleSpinBox);
    minSeconds_->setRange(0, 60);
    minSeconds_->setDecimals(1);
    minSeconds_->setSuffix(" s");
    minSeconds_->setValue(s.recording.minSeconds);
    f->addRow("Longest recording", maxMinutes_ = spin(1, 240, s.recording.maxMinutes, " min"));
    Settings defaults;
    defaults.logging.directory.clear();
    defaults.recording.directory.clear();
    recDir_ = line(s.recording.directory, "default: " + s.recordingDir());
    logDir_ = line(s.logging.directory, "default: " + defaults.logDir());
    f->addRow("Recordings folder", withBrowse(recDir_));
    f->addRow("Call log (CSV) folder", withBrowse(logDir_));

    // ---- Test call
    f = addPage("Test call");
    f->addRow("Mode", testMode_ = new QComboBox);
    testMode_->addItems(TestCall::modes());
    testMode_->setCurrentText(s.testCall.mode);
    f->addRow("", testEcho_ = new QCheckBox("Echo via hotspot (sends on air)"));
    testEcho_->setChecked(s.testCall.echo);
    f->addRow("Spoken text", spoken_ = line(s.testCall.spokenText, "default: your callsign spelled out, then \"Test, test, test.\""));
    f->addRow("D-Star text message", textMsg_ = line(s.testCall.textMessage, "default: <callsign> Test test test"));
    textMsg_->setMaxLength(20);
    f->addRow("eSpeak voice", voice_ = line(s.testCall.voice));
    f->addRow("Speech rate", rate_ = spin(60, 400, s.testCall.speechRate, " wpm"));
    f->addRow("Talkgroup when not echoing", localTg_ = spin(1, 16777215, int(s.testCall.localTalkgroup)));
    f->addRow("DMR echo (private call to)", dmrEcho_ = spin(1, 16777215, int(s.testCall.dmrEchoId)));
    f->addRow("D-Star echo (UR field)", dstarEcho_ = line(s.testCall.dstarEcho));
    dstarEcho_->setMaxLength(8);
    f->addRow("YSF parrot reflector", ysfParrot_ = line(s.testCall.ysfParrot));
    f->addRow("P25 parrot talkgroup", p25Echo_ = spin(1, 65535, int(s.testCall.p25EchoTg)));
    f->addRow("NXDN parrot talkgroup", nxdnEcho_ = spin(1, 65535, int(s.testCall.nxdnEchoTg)));
    f->addRow("M17 echo destination", m17Echo_ = line(s.testCall.m17Echo));

    // ---- Transmit
    f = addPage("Transmit");
    f->addRow("Transmit time limit", txTimeout_ = spin(10, 600, s.transmit.timeoutSec, " s"));
    f->addRow("", latch_ = new QCheckBox("Latching PTT (click to start, click to stop)"));
    latch_->setChecked(s.transmit.latch);
    f->addRow("DMR talkgroup", dmrTg_ = spin(1, 16777215, int(s.transmit.dmrTalkgroup)));
    f->addRow("", dmrPrivate_ = new QCheckBox("DMR private call (to the ID above)"));
    dmrPrivate_->setChecked(s.transmit.dmrPrivate);
    f->addRow("D-Star MY suffix", dstarSuffix_ = line(s.transmit.dstarSuffix, "e.g. PC"));
    dstarSuffix_->setMaxLength(4);
    f->addRow(note("PTT sends your voice into Pi-Star as if from your radio, to whatever the mode is linked to "
                   "(DMR: the talkgroup above on BrandMeister). Your callsign and IDs are on the Station page."));

    // ---- Advanced
    f = addPage("Advanced");
    f->addRow("End a call after silence of", callTimeout_ = spin(200, 10000, s.calls.timeoutMs, " ms"));
    f->addRow("Reconnect if the Pi is silent for", linkTimeout_ = spin(2000, 120000, s.connection.linkTimeoutMs, " ms"));
    f->addRow("Retry connection after", reconnect_ = spin(1000, 120000, s.connection.reconnectDelayMs, " ms"));
    f->addRow("Pages loaded on connect", pageHistory_ = spin(0, 10000, s.connection.pageHistory));
    f->addRow("", onTop_ = new QCheckBox("Window always on top"));
    onTop_->setChecked(s.window.alwaysOnTop);

    // ---- bottom: file location + buttons
    auto *fileRow = new QHBoxLayout;
    auto *where = new QLabel(QString("Saved in <code>%1</code>").arg(Settings::path().toHtmlEscaped()));
    where->setTextInteractionFlags(Qt::TextSelectableByMouse);
    where->setWordWrap(true);   // long paths must not stretch the dialog
    auto *open = new QPushButton("Open folder");
    connect(open, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(Settings::path()).absolutePath()));
    });
    fileRow->addWidget(where, 1);
    fileRow->addWidget(open);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *root = new QVBoxLayout(this);
    root->addWidget(tabs, 1);
    root->addLayout(fileRow);
    root->addWidget(buttons);
    resize(640, 680);
}

SettingsDialog::~SettingsDialog() { stopTests(); }

void SettingsDialog::showTab(const QString &title)
{
    for (int i = 0; i < tabs_->count(); ++i)
        if (tabs_->tabText(i).remove('&') == title) tabs_->setCurrentIndex(i);
    if (qEnvironmentVariableIntValue("PISTAR_AUTOTEST") == 1) {
        micTest_->setChecked(true);
        startSpeakerTest();
    }
}

void SettingsDialog::done(int r)
{
    stopTests();
    QDialog::done(r);
}

namespace {
int meterFromRms(double rms)   // dBFS -60..0 -> 0..100
{
    const double db = 20 * std::log10(std::max(rms, 1.0) / 32767.0);
    return int(std::clamp((db + 60) * 100 / 60, 0.0, 100.0));
}
QString outputDevice(QComboBox *c)
{
    const int i = c->findText(c->currentText());
    return i >= 0 ? c->itemData(i).toString() : c->currentText().trimmed();
}
}   // namespace

void SettingsDialog::startSpeakerTest()
{
    if (spkOut_) { spkOut_.reset(); spkMeter_->setValue(0); spkTest_->setText("Test speaker"); spkTimer_.stop(); return; }
    std::vector<int16_t> pcm;
    QString err;
    if (!TestCall::speak("Speaker test. One, two, three.", voice_->text(), rate_->value(), pcm, err)) {
        pcm.resize(12000);   // no speech engine: 1.5 s of 1 kHz
        for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = int16_t(12000 * std::sin(2 * M_PI * 1000 * double(i) / 8000));
    }
    const double vol = mute_->isChecked() ? 0.0 : volume_->value() / 100.0;
    for (auto &v : pcm) v = int16_t(std::clamp(v * vol * 1.2, -32767.0, 32767.0));
    spkOut_ = std::make_unique<AudioOut>("Speaker test");
    if (!spkOut_->open(backend_->currentData().toString(), outputDevice(device_), &err)) {
        spkOut_.reset();
        spkTest_->setText("Test speaker");
        spkMeter_->setToolTip("No audio output: " + err);
        return;
    }
    spkOut_->write(pcm.data(), pcm.size());
    spkPcm_ = pcm;
    spkStart_.start();
    spkTest_->setText("Stop");
    spkTimer_.disconnect();
    connect(&spkTimer_, &QTimer::timeout, this, [this] {
        const size_t pos = size_t(spkStart_.elapsed() * 8);   // samples played so far (approximately)
        if (pos >= spkPcm_.size() + 2000) { startSpeakerTest(); return; }   // finished: reset
        double acc = 0;
        int n = 0;
        for (size_t i = pos; i < std::min(spkPcm_.size(), pos + 400); ++i, ++n) acc += double(spkPcm_[i]) * spkPcm_[i];
        spkMeter_->setValue(n ? meterFromRms(std::sqrt(acc / n)) : 0);
    });
    spkTimer_.start(50);
}

void SettingsDialog::toggleMicTest(bool on)
{
    if (!on) {
        if (micIn_) micIn_->stop();
        micIn_.reset();
        monOut_.reset();
        micMeter_->setValue(0);
        micDb_->setText("—");
        micTest_->setText("Test microphone");
        return;
    }
    QString err;
    if (monitor_->isChecked()) {
        monOut_ = std::make_unique<AudioOut>("Microphone monitor");
        if (!monOut_->open(backend_->currentData().toString(), outputDevice(device_), &err)) monOut_.reset();
    }
    micIn_ = std::make_unique<AudioIn>();
    micPeakDb_ = -90;
    if (!micIn_->start(txDevice_->currentData().toString(), this, [this](const std::vector<int16_t> &f) {
            const double gain = std::pow(10.0, micGain_->value() / 20.0);
            std::vector<int16_t> g(f.size());
            double acc = 0;
            for (size_t i = 0; i < f.size(); ++i) {
                g[i] = int16_t(std::clamp(f[i] * gain, -32767.0, 32767.0));
                acc += double(g[i]) * g[i];
            }
            const double rms = std::sqrt(acc / double(f.size()));
            const double db = 20 * std::log10(std::max(rms, 1.0) / 32767.0);
            micPeakDb_ = std::max(db, micPeakDb_ - 0.5);   // peak hold, falling 25 dB/s
            micMeter_->setValue(meterFromRms(rms));
            micDb_->setText(QString("%1 dBFS").arg(micPeakDb_, 0, 'f', 0));
            if (monOut_) monOut_->write(g.data(), g.size());
        }, &err)) {
        micIn_.reset();
        monOut_.reset();
        micDb_->setText("error");
        micDb_->setToolTip(err);
        micTest_->blockSignals(true);
        micTest_->setChecked(false);
        micTest_->blockSignals(false);
        return;
    }
    micTest_->setText("Stop");
}

void SettingsDialog::stopTests()
{
    spkTimer_.stop();
    spkOut_.reset();
    if (micIn_) micIn_->stop();
    micIn_.reset();
    monOut_.reset();
}

Settings SettingsDialog::result() const
{
    Settings s = s_;
    s.connection.host = host_->text().trimmed();
    s.connection.user = user_->text().trimmed();
    s.connection.identityFile = identity_->text().trimmed();
    s.connection.linkTimeoutMs = linkTimeout_->value();
    s.connection.reconnectDelayMs = reconnect_->value();
    s.connection.pageHistory = pageHistory_->value();

    s.station.callsign = callsign_->text().trimmed().toUpper();
    s.station.dmrId = uint32_t(dmrId_->value());
    s.station.repeaterId = uint32_t(repeaterId_->value());
    s.station.nxdnId = uint32_t(nxdnId_->value());
    s.station.dmrSlot = slot_->value();
    s.station.colourCode = cc_->value();
    s.station.dstarModule = dstarModule_->text().trimmed().toUpper().left(1);
    if (s.station.dstarModule.isEmpty()) s.station.dstarModule = "B";

    s.pager.myRics = rics_->text().trimmed();
    s.pager.onlyMine = onlyMine_->isChecked();

    s.audio.volume = volume_->value();
    s.audio.mute = mute_->isChecked();
    s.audio.backend = backend_->currentData().toString();
    s.audio.noiseGate = gate_->isChecked();
    s.audio.gateThresholdDb = gateThr_->value();
    s.audio.gateReductionDb = gateRed_->value();
    s.audio.gateHoldMs = gateHold_->value();
    {
        // A list entry gives its device name; typed text (ALSA) is used as is.
        const int i = device_->findText(device_->currentText());
        s.audio.device = i >= 0 ? device_->itemData(i).toString() : device_->currentText().trimmed();
    }

    s.recording.keepCalls = keepCalls_->value();
    s.recording.minSeconds = minSeconds_->value();
    s.recording.maxMinutes = maxMinutes_->value();
    s.recording.directory = recDir_->text().trimmed();
    s.logging.directory = logDir_->text().trimmed();

    s.calls.timeoutMs = callTimeout_->value();

    s.testCall.mode = testMode_->currentText();
    s.testCall.echo = testEcho_->isChecked();
    s.testCall.spokenText = spoken_->text();
    s.testCall.textMessage = textMsg_->text();
    s.testCall.voice = voice_->text().trimmed();
    s.testCall.speechRate = rate_->value();
    s.testCall.localTalkgroup = uint32_t(localTg_->value());
    s.testCall.dmrEchoId = uint32_t(dmrEcho_->value());
    s.testCall.dstarEcho = dstarEcho_->text().leftJustified(8, ' ', true);
    s.testCall.ysfParrot = ysfParrot_->text().trimmed();
    s.testCall.p25EchoTg = uint32_t(p25Echo_->value());
    s.testCall.nxdnEchoTg = uint32_t(nxdnEcho_->value());
    s.testCall.m17Echo = m17Echo_->text().trimmed().toUpper();

    s.transmit.inputDevice = txDevice_->currentData().toString();
    s.transmit.micGainDb = micGain_->value();
    s.transmit.autoLevel = autoLevel_->isChecked();
    s.transmit.timeoutSec = txTimeout_->value();
    s.transmit.latch = latch_->isChecked();
    s.transmit.dmrTalkgroup = uint32_t(dmrTg_->value());
    s.transmit.dmrPrivate = dmrPrivate_->isChecked();
    s.transmit.dstarSuffix = dstarSuffix_->text().trimmed().toUpper();

    s.window.alwaysOnTop = onTop_->isChecked();
    return s;
}
