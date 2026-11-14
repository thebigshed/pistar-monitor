#pragma once
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QMainWindow>
#include <QProcess>
#include <QSet>
#include <QTimer>
#include <deque>
#include <map>
#include <optional>

#include "DmrDecoder.h"
#include "Settings.h"
#include "AudioOut.h"
#include "AudioIn.h"
#include "VoiceEncoder.h"
#include <memory>
#include <functional>

class QLabel;
class QSlider;
class QCheckBox;
class QProgressBar;
class QTableWidget;
class QFrame;
class QFile;
class QTabWidget;
class QLineEdit;
class QComboBox;
class QPushButton;
class QSpinBox;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    struct Call {
        QByteArray stream;
        uint32_t src = 0, dst = 0;
        int slot = 2;
        bool privateCall = false;
        bool lateEntry = false;
        QDateTime started;
        QElapsedTimer timer;
        qint64 lastPacketMs = 0;
        int frames = 0, errors = 0;
        bool test = false;       // synthetic test call
    };

    struct DStarCall {
        uint16_t id = 0;
        bool fromRf = false;
        bool lateEntry = false;
        QString my, suffix, your, rpt1, rpt2;
        QString reflector;   // linked reflector the call went through, if any
        QDateTime started;
        QElapsedTimer timer;
        qint64 lastPacketMs = 0;
        int frames = 0, errors = 0;
        SlowDataText text;
        DStarDecoder decoder;   // per stream: two streams can be interleaved
        bool test = false;
    };

    // YSF and P25 calls share one generic record.
    struct GenCall {
        QString mode;            // "YSF" or "P25"
        bool fromRf = false;
        bool lateEntry = false;
        bool audio = true;       // false for modes we can't decode
        QString callsign, target, note;
        uint32_t srcId = 0, dstId = 0;
        QDateTime started;
        QElapsedTimer timer;
        qint64 lastPacketMs = 0;
        int frames = 0;          // 20 ms voice frames
        int errors = 0, errorBits = 0;
        uint16_t streamId = 0;   // M17
        uint8_t l3[23]{};        // NXDN: layer 3 VCALL reassembled from SACCH (late entry)
        int l3Mask = 0;
        bool test = false;
    };

    void buildUi();
    void handleYsf(const QByteArray &rec);
    void handleP25(const QByteArray &rec);
    void handleNxdn(const QByteArray &rec);
    void handleM17(const QByteArray &rec);
    void loadNxdnIds();
    QString nxdnCall(uint32_t id) const;
    static QString genColour(const QString &mode);
    void beginGen(std::optional<GenCall> &slot, const QString &mode, bool fromRf, bool lateEntry);
    void endGen(std::optional<GenCall> &slot, const QString &why);
    QString genName(const GenCall &c) const;
    bool claimAudio(const QString &key);
    void releaseAudio(const QString &key);
    void playDecoded(const QString &key, const float *samples, int n);
    void handleDStar(const QByteArray &rec);
    void handleLinks(const QByteArray &linksLog);
    void handleModes(const QByteArray &json);       // Pi-Star "Modes Enabled"
    void handleEndpoints(const QByteArray &json);   // what each mode is linked to
    void updateEndpointsLabel();
    void changeLink(const QString &mode);    // endpoint clicked
    void runLinkHelper(const QStringList &args, std::function<void(const QJsonObject &)> done);
    bool modeAllowed(const QString &mode, bool network = false) const;
    void refreshTestModes();
    DStarCall &beginDStar(const DStarPacket &p, bool lateEntry);
    void endDStar(uint16_t id, const QString &why);
    void endAllDStar(const QString &why);
    static QString dstarKey(uint16_t id) { return QString("DSTAR%1").arg(id); }
    void addLogRow(const QStringList &cells, const QString &tooltip, const QString &recording = {});
    // recordings of the last few calls (decoded audio, all modes)
    QString saveRecording(const QString &key, const QDateTime &started, const QString &mode, const QString &who);
    void pruneRecordings();
    void toggleRecordingPlayback(int row);
    void stopRecordingPlayback();
    void refreshPlayCells();
    QString dstarName(const QString &call) const;
    static QString dstarCaller(const DStarCall &c);
    static QString dstarTarget(const DStarCall &c);
    void startCapture();
    QStringList sshArgs() const;           // ssh options + user@host
    void openLogFiles();
    void parseRics();
    void saveSettings();
    void openSettings(const QString &tab = {});
    void applySettings(const Settings &old);
    void startReplay(const QString &path);   // test aid: --replay capture.bin
    void startTestCall(const QString &mode);
    void startEchoCall(const QString &mode, const QList<QByteArray> &records, int intervalMs);
    // PTT transmit
    void startTxSession();
    void txWrite(char type, const QByteArray &body = {});
    void startTx();
    void stopTx(const QString &why = {});
    void onMicFrame(const std::vector<int16_t> &frame);
    void refreshTxModes();
    void updateTxTarget();
    bool txTarget(TxTarget &t, QString &err) const;
    void loadIds();
    void onSshData();
    void dispatchRecord(const QByteArray &rec);   // one record from the Pi sniffer
    void handlePacket(const QByteArray &raw);
    void handlePage(const QByteArray &logLine);
    void applyPagerFilter();
    void updatePagerTabTitle();
    void showPageBanner(const QDateTime &when, uint32_t ric, const QString &text);
    void beginCall(const DmrPacket &p, bool lateEntry);
    void endCall(int slot, const QString &why);
    void playPcm(const std::vector<int16_t> &pcm);
    bool ensurePlayer(std::unique_ptr<AudioOut> &out, const char *name);
    void tick();
    void setStatus(const QString &text, const QString &colour);
    void updateVolumeLabel();
    QString describe(uint32_t id) const;
    QString callsign(uint32_t id) const;
    QString target(const Call &c) const;

    // settings (settings.json); persist_ is false for test runs so they never write it
    Settings cfg_;
    bool persist_ = true;
    bool demo_ = false;   // --demo: sample data for screenshots

    // processes
    QProcess *ssh_ = nullptr;
    std::unique_ptr<AudioOut> liveOut_, replayOut_;   // built-in audio output
    qint64 audioRetryMs_ = 0;                          // after a failed open, don't retry before this
    QProcess *idsProc_ = nullptr;
    QByteArray rxBuf_;
    bool quitting_ = false;

    // decoding
    AmbeDecoder decoder_;
    Agc agc_;
    NoiseGate gate_;   // live audio; reset when a new call takes the speaker
    QSet<QByteArray> seen_;
    std::deque<QByteArray> seenOrder_;
    std::optional<Call> active_[3];   // index by slot 1/2
    QHash<uint32_t, QPair<QString, QString>> ids_;   // id -> (callsign, name)
    double level_ = 0;
    QFile *logFile_ = nullptr;

    // D-Star
    std::map<uint16_t, DStarCall> dstars_;   // active D-Star streams by stream id
    QHash<QString, QString> nameByCall_;   // callsign -> name, from DMRIds.dat
    QFile *dstarFile_ = nullptr;
    YsfDecoder ysfDecoder_;
    P25Decoder p25Decoder_;
    NxdnDecoder nxdnDecoder_;
    M17Decoder m17Decoder_;
    std::optional<GenCall> ysf_, p25_, nxdn_, m17_;
    QHash<uint32_t, QPair<QString, QString>> nxdnIds_;   // NXDN id -> (callsign, name), from NXDN.csv
    QProcess *nxdnIdsProc_ = nullptr;
    QFile *genFile_ = nullptr;
    QHash<QString, std::vector<float>> recBuf_;   // decoded audio of each in-progress call, by audio key
    QString recDir_;
    QTimer *replayTimer_ = nullptr;   // fires when a recording replay has finished
    QString playingPath_;
    QString audioOwner_;     // key of the call currently playing: DMR1/DMR2/DSTAR/YSF/P25
    QString reflector_;      // currently linked reflector, e.g. "REF001 C"

    // pager (DAPNET / POCSAG)
    QSet<QByteArray> seenPages_;
    QSet<uint32_t> myRics_;
    QFile *pageFile_ = nullptr;
    int unreadPages_ = 0;
    bool pagesLive_ = false;   // false while the Pi is sending page history (before first heartbeat)

    // ui
    QFrame *callPanel_ = nullptr;
    QLabel *statusDot_ = nullptr, *statusText_ = nullptr;
    QHash<QString, QPair<bool, bool>> modes_;   // mode -> (mode enabled, network enabled), from /etc/mmdvmhost
    bool modesKnown_ = false;                   // false until the hotspot has reported (e.g. offline replay)
    QJsonObject endpoints_;                     // mode -> linked reflector/TG/master ("" = not linked)
    QLabel *endpointsLabel_ = nullptr;
    QHash<QString, QPair<qint64, QJsonObject>> linkLists_;   // mode -> (fetched at, list), cached briefly
    QLabel *callTitle_ = nullptr, *callSub_ = nullptr, *callTime_ = nullptr;
    QProgressBar *levelBar_ = nullptr;
    QSlider *volume_ = nullptr;
    QLabel *volumeLabel_ = nullptr;
    QCheckBox *mute_ = nullptr, *onTop_ = nullptr;
    QTableWidget *log_ = nullptr;
    QTabWidget *tabs_ = nullptr;
    QTableWidget *pages_ = nullptr;
    QLineEdit *ricEdit_ = nullptr;
    QCheckBox *onlyMine_ = nullptr;
    QFrame *pageBanner_ = nullptr;
    QLabel *pageBannerText_ = nullptr;
    QTimer uiTimer_;
    QString panelStyle_;   // last stylesheet applied to the call panel
    QComboBox *testMode_ = nullptr;
    QPushButton *testBtn_ = nullptr;
    QCheckBox *testEcho_ = nullptr;
    QProcess *injectProc_ = nullptr;
    QTimer *testTimer_ = nullptr;
    bool testFeeding_ = false;   // calls that start while this is set are test calls

    // PTT transmit
    QComboBox *txMode_ = nullptr;
    QLabel *txTargetLabel_ = nullptr;
    QSpinBox *txTg_ = nullptr;
    QCheckBox *txPrivate_ = nullptr;
    QProgressBar *micLevel_ = nullptr;
    QPushButton *pttBtn_ = nullptr;
    QProcess *txSession_ = nullptr;   // remote_tx.py on the Pi, for the whole session
    bool txReady_ = false;
    bool startTxFake_ = false;   // --ptt-test: no microphone
    bool pttTest_ = false;
    bool txActive_ = false;
    qint64 txStartMs_ = 0, txMuteUntilMs_ = 0;
    double micGain_ = 1.0;            // automatic level control state
    std::unique_ptr<VoiceEncoder> txEnc_;
    std::unique_ptr<AudioIn> mic_;
    QTimer txKeepalive_;
};
