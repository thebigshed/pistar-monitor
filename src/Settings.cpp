#include "Settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

QString Settings::path()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/settings.json";
}

QString Settings::logDir() const
{
    return logging.directory.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                                       : logging.directory;
}

QString Settings::recordingDir() const
{
    return recording.directory.isEmpty() ? logDir() + "/recordings" : recording.directory;
}

bool Settings::stationReady(QString *why) const
{
    QString w;
    if (station.callsign.isEmpty() || station.callsign.toUpper() == "N0CALL") w = "set your callsign";
    else if (station.dmrId == 0) w = "set your DMR ID (also used for P25)";
    if (why) *why = w.isEmpty() ? QString() : w + " in Settings → Station first";
    return w.isEmpty();
}

QString Settings::testSpokenText() const
{
    if (!testCall.spokenText.isEmpty()) return testCall.spokenText;
    QStringList letters;   // spell the callsign out so it is spoken as letters and digits
    for (QChar c : station.callsign) letters << QString(c);
    return letters.join(' ') + ". Test, test, test.";
}

QString Settings::testTextMessage() const
{
    return testCall.textMessage.isEmpty() ? (station.callsign + " Test test test").left(20) : testCall.textMessage;
}

// ---------------------------------------------------------------- JSON helpers
namespace {
void get(const QJsonObject &o, const char *k, QString &v) { if (o.contains(k)) v = o.value(k).toString(v); }
void get(const QJsonObject &o, const char *k, bool &v) { if (o.contains(k)) v = o.value(k).toBool(v); }
void get(const QJsonObject &o, const char *k, int &v) { if (o.contains(k)) v = o.value(k).toInt(v); }
void get(const QJsonObject &o, const char *k, double &v) { if (o.contains(k)) v = o.value(k).toDouble(v); }
void get(const QJsonObject &o, const char *k, uint32_t &v)
{
    if (o.contains(k)) v = uint32_t(o.value(k).toDouble(double(v)));
}
void get(const QJsonObject &o, const char *k, QStringList &v)
{
    if (!o.value(k).isArray()) return;
    v.clear();
    for (const QJsonValue &x : o.value(k).toArray()) v << x.toString();
}
}   // namespace

Settings Settings::load()
{
    Settings s;
    QFile f(path());
    if (!f.exists()) {
        // First run with a JSON file: carry over what the app used to keep in QSettings.
        QSettings q;
        s.connection.host = q.value("host", s.connection.host).toString();
        s.connection.user = q.value("user", s.connection.user).toString();
        s.audio.volume = q.value("volume", s.audio.volume).toInt();
        s.audio.mute = q.value("mute", s.audio.mute).toBool();
        s.pager.myRics = q.value("pagerRics", s.pager.myRics).toString();
        s.pager.onlyMine = q.value("pagerOnlyMine", s.pager.onlyMine).toBool();
        s.window.alwaysOnTop = q.value("onTop", s.window.alwaysOnTop).toBool();
        s.testCall.mode = q.value("testMode", s.testCall.mode).toString();
        s.testCall.echo = q.value("testEcho", s.testCall.echo).toBool();
        s.save();
        return s;
    }
    if (!f.open(QIODevice::ReadOnly)) return s;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();

    const QJsonObject c = root.value("connection").toObject();
    get(c, "host", s.connection.host);
    get(c, "user", s.connection.user);
    get(c, "identityFile", s.connection.identityFile);
    get(c, "linkTimeoutMs", s.connection.linkTimeoutMs);
    get(c, "reconnectDelayMs", s.connection.reconnectDelayMs);
    get(c, "pageHistory", s.connection.pageHistory);

    const QJsonObject st = root.value("station").toObject();
    get(st, "callsign", s.station.callsign);
    get(st, "dmrId", s.station.dmrId);
    get(st, "repeaterId", s.station.repeaterId);
    get(st, "nxdnId", s.station.nxdnId);
    get(st, "dmrSlot", s.station.dmrSlot);
    get(st, "colourCode", s.station.colourCode);
    get(st, "dstarModule", s.station.dstarModule);

    const QJsonObject p = root.value("pager").toObject();
    get(p, "myRics", s.pager.myRics);
    get(p, "onlyMine", s.pager.onlyMine);

    const QJsonObject a = root.value("audio").toObject();
    get(a, "volume", s.audio.volume);
    get(a, "mute", s.audio.mute);
    get(a, "backend", s.audio.backend);
    get(a, "device", s.audio.device);
    get(a, "noiseGate", s.audio.noiseGate);
    get(a, "gateThresholdDb", s.audio.gateThresholdDb);
    get(a, "gateReductionDb", s.audio.gateReductionDb);
    get(a, "gateHoldMs", s.audio.gateHoldMs);

    get(root.value("calls").toObject(), "timeoutMs", s.calls.timeoutMs);

    const QJsonObject r = root.value("recording").toObject();
    get(r, "keepCalls", s.recording.keepCalls);
    get(r, "minSeconds", s.recording.minSeconds);
    get(r, "maxMinutes", s.recording.maxMinutes);
    get(r, "directory", s.recording.directory);

    get(root.value("logging").toObject(), "directory", s.logging.directory);

    const QJsonObject t = root.value("testCall").toObject();
    get(t, "mode", s.testCall.mode);
    get(t, "echo", s.testCall.echo);
    get(t, "spokenText", s.testCall.spokenText);
    get(t, "textMessage", s.testCall.textMessage);
    get(t, "voice", s.testCall.voice);
    get(t, "speechRate", s.testCall.speechRate);
    get(t, "localTalkgroup", s.testCall.localTalkgroup);
    get(t, "dmrEchoId", s.testCall.dmrEchoId);
    get(t, "p25EchoTg", s.testCall.p25EchoTg);
    get(t, "nxdnEchoTg", s.testCall.nxdnEchoTg);
    get(t, "ysfParrot", s.testCall.ysfParrot);
    get(t, "m17Echo", s.testCall.m17Echo);
    get(t, "dstarEcho", s.testCall.dstarEcho);

    const QJsonObject x = root.value("transmit").toObject();
    get(x, "mode", s.transmit.mode);
    get(x, "inputDevice", s.transmit.inputDevice);
    get(x, "micGainDb", s.transmit.micGainDb);
    get(x, "autoLevel", s.transmit.autoLevel);
    get(x, "timeoutSec", s.transmit.timeoutSec);
    get(x, "latch", s.transmit.latch);
    get(x, "dmrTalkgroup", s.transmit.dmrTalkgroup);
    get(x, "dmrPrivate", s.transmit.dmrPrivate);
    get(x, "dstarSuffix", s.transmit.dstarSuffix);

    get(root.value("window").toObject(), "alwaysOnTop", s.window.alwaysOnTop);
    return s;
}

bool Settings::save(QString *err) const
{
    QJsonObject root;
    root["connection"] = QJsonObject{{"host", connection.host},
                                     {"user", connection.user},
                                     {"identityFile", connection.identityFile},
                                     {"linkTimeoutMs", connection.linkTimeoutMs},
                                     {"reconnectDelayMs", connection.reconnectDelayMs},
                                     {"pageHistory", connection.pageHistory}};
    root["station"] = QJsonObject{{"callsign", station.callsign},
                                  {"dmrId", double(station.dmrId)},
                                  {"repeaterId", double(station.repeaterId)},
                                  {"nxdnId", double(station.nxdnId)},
                                  {"dmrSlot", station.dmrSlot},
                                  {"colourCode", station.colourCode},
                                  {"dstarModule", station.dstarModule}};
    root["pager"] = QJsonObject{{"myRics", pager.myRics}, {"onlyMine", pager.onlyMine}};
    root["audio"] = QJsonObject{{"volume", audio.volume},
                                {"mute", audio.mute},
                                {"backend", audio.backend},
                                {"device", audio.device},
                                {"noiseGate", audio.noiseGate},
                                {"gateThresholdDb", audio.gateThresholdDb},
                                {"gateReductionDb", audio.gateReductionDb},
                                {"gateHoldMs", audio.gateHoldMs}};
    root["calls"] = QJsonObject{{"timeoutMs", calls.timeoutMs}};
    root["recording"] = QJsonObject{{"keepCalls", recording.keepCalls},
                                    {"minSeconds", recording.minSeconds},
                                    {"maxMinutes", recording.maxMinutes},
                                    {"directory", recording.directory}};
    root["logging"] = QJsonObject{{"directory", logging.directory}};
    root["testCall"] = QJsonObject{{"mode", testCall.mode},
                                   {"echo", testCall.echo},
                                   {"spokenText", testCall.spokenText},
                                   {"textMessage", testCall.textMessage},
                                   {"voice", testCall.voice},
                                   {"speechRate", testCall.speechRate},
                                   {"localTalkgroup", double(testCall.localTalkgroup)},
                                   {"dmrEchoId", double(testCall.dmrEchoId)},
                                   {"p25EchoTg", double(testCall.p25EchoTg)},
                                   {"nxdnEchoTg", double(testCall.nxdnEchoTg)},
                                   {"ysfParrot", testCall.ysfParrot},
                                   {"m17Echo", testCall.m17Echo},
                                   {"dstarEcho", testCall.dstarEcho}};
    root["transmit"] = QJsonObject{{"mode", transmit.mode},
                                   {"inputDevice", transmit.inputDevice},
                                   {"micGainDb", transmit.micGainDb},
                                   {"autoLevel", transmit.autoLevel},
                                   {"timeoutSec", transmit.timeoutSec},
                                   {"latch", transmit.latch},
                                   {"dmrTalkgroup", double(transmit.dmrTalkgroup)},
                                   {"dmrPrivate", transmit.dmrPrivate},
                                   {"dstarSuffix", transmit.dstarSuffix}};
    root["window"] = QJsonObject{{"alwaysOnTop", window.alwaysOnTop}};

    QDir().mkpath(QFileInfo(path()).absolutePath());
    QSaveFile f(path());   // atomic: never leaves a half-written file
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = f.errorString();
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (err) *err = f.errorString();
        return false;
    }
    return true;
}
