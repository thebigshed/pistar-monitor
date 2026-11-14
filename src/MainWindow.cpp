#include "MainWindow.h"
#include "remote_sniff.h"
#include "remote_inject.h"
#include "remote_link.h"
#include "remote_tx.h"
#include <QSpinBox>
#include "LinkDialog.h"
#include "TestCall.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCloseEvent>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QTabWidget>
#include <QTimeZone>
#include <QProgressBar>
#include <QPushButton>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include "SettingsDialog.h"
#include <QSlider>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTextStream>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <memory>

static constexpr int kSeenMax = 2000;
static constexpr int kPlayCol = 8;

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    cfg_ = Settings::load();
    const QStringList args = QApplication::arguments();
    // Test aids must not change the saved settings.
    demo_ = args.contains("--demo");
    persist_ = !(args.contains("--test-call") || args.contains("--echo-call") || args.contains("--replay") ||
                 args.contains("--change-link") || args.contains("--mic-test") || args.contains("--ptt-test") || demo_);
    if (demo_) {   // placeholder identity for screenshots
        cfg_.station.callsign = "N0CALL";
        cfg_.station.dmrId = 1234567;
        cfg_.station.repeaterId = 123456701;
        cfg_.station.nxdnId = 1234;
        cfg_.pager.myRics = "1234567";
        cfg_.testCall.echo = false;
        cfg_.audio.backend = "alsa";   // play into ALSA's silent null device
        cfg_.audio.device = "null";
    }
    pttTest_ = args.contains("--ptt-test");
    for (int i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == "--host") cfg_.connection.host = args[i + 1];
        if (args[i] == "--user") cfg_.connection.user = args[i + 1];
    }
    saveSettings();

    buildUi();

    recDir_ = cfg_.recordingDir();
    logFile_ = new QFile(this);
    dstarFile_ = new QFile(this);
    genFile_ = new QFile(this);
    pageFile_ = new QFile(this);
    openLogFiles();

    connect(&uiTimer_, &QTimer::timeout, this, &MainWindow::tick);
    uiTimer_.start(100);

    if (!demo_) {
        loadIds();
        loadNxdnIds();
    }
    if (args.contains("--mic-test")) {   // test aid: microphone levels for 3 s, nothing sent or stored
        auto *levels = new QStringList;
        auto mic = std::make_shared<AudioIn>();
        QString err;
        const bool ok = mic->start(cfg_.transmit.inputDevice, this, [levels](const std::vector<int16_t> &f) {
            double acc = 0, pk = 0;
            for (int16_t v : f) { acc += double(v) * v; pk = std::max(pk, std::abs(double(v))); }
            if (levels->size() < 1000) *levels << QString("%1/%2").arg(int(std::sqrt(acc / f.size()))).arg(int(pk));
        }, &err);
        if (!ok) { fprintf(stdout, "MIC ERROR %s\n", qPrintable(err)); fflush(stdout); }
        QTimer::singleShot(3000, this, [mic, levels] {
            mic->stop();
            fprintf(stdout, "MIC frames=%d rms/peak: %s\n", int(levels->size()), qPrintable(levels->mid(0, 150).join(' ')));
            fflush(stdout);
            QApplication::quit();
        });
    }
    if (demo_) {   // --demo [pager]: sample data for screenshots; nothing is connected or sent
        logFile_->close();
        dstarFile_->close();
        genFile_->close();
        pageFile_->close();
        recDir_ = QDir::tempPath() + "/pistar-dmr-demo-recordings";
        ids_.insert(1234567, {"N0CALL", "Demo"});
        nameByCall_.insert("N0CALL", "Demo");
        nxdnIds_.insert(1234, {"N0CALL", "Demo"});
        setStatus("Demo data (not connected)", "#1a73e8");
        QJsonObject modes;
        for (const char *m : {"D-Star", "DMR", "YSF", "P25", "NXDN", "M17", "POCSAG"})
            modes[m] = QJsonObject{{"mode", true}, {"network", true}};
        modes["FM"] = QJsonObject{{"mode", false}, {"network", false}};
        handleModes(QJsonDocument(modes).toJson(QJsonDocument::Compact));
        handleEndpoints(QJsonDocument(QJsonObject{{"D-Star", "REF001 C"}, {"DMR", "BM 2341"}, {"YSF", "US-PI-STAR-MULTI"},
                                                  {"P25", "TG 31672"}, {"NXDN", "TG 31672"}, {"M17", "M17-672 E"},
                                                  {"POCSAG", "DAPNET"}}).toJson(QJsonDocument::Compact));
        const QString day = QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd");
        const QString t1 = QDateTime::currentDateTimeUtc().addSecs(-1500).toString("HH:mm:ss");
        const QString t2 = QDateTime::currentDateTimeUtc().addSecs(-300).toString("HH:mm:ss");
        handlePage(QString("M: %1 %2.000 Sending message in slot 3 to 0000208, type 6, func Alphanumeric: \"XTIME=1200141126\"").arg(day, t1).toUtf8());
        handlePage(QString("M: %1 %2.000 Sending message in slot 4 to 1234567, type 6, func Alphanumeric: \"Club net tonight 19:30 on TG 2350\"").arg(day, t1).toUtf8());
        handlePage(QString("M: %1 %2.000 Sending message in slot 8 to 1234567, type 6, func Alphanumeric: \"Welcome to the demo of Pi-Star Monitor\"").arg(day, t2).toUtf8());
        if (args.contains("pager")) tabs_->setCurrentIndex(1);
        const QStringList order = {"DMR", "YSF", "P25", "NXDN", "M17", "D-Star"};
        for (int i = 0; i < order.size(); ++i)
            QTimer::singleShot(800 + i * 6500, this, [this, m = order[i]] { startTestCall(m); });
    }
    const int pt = args.indexOf("--ptt-test");   // test aid: PTT path with the test phrase instead of the mic
    if (pt > 0 && pt + 1 < args.size()) {
        const QString m = args[pt + 1];
        logFile_->close();          // the running app logs the real calls
        dstarFile_->close();
        genFile_->close();
        pageFile_->close();
        recDir_ = QDir::tempPath() + "/pistar-dmr-test-recordings";
        cfg_.transmit.mode = m;
        cfg_.transmit.dmrTalkgroup = cfg_.testCall.dmrEchoId;   // DMR: to the BrandMeister Parrot only
        cfg_.transmit.dmrPrivate = true;
        auto pcm = std::make_shared<std::vector<int16_t>>();
        QString err;
        TestCall::speak(cfg_.testSpokenText(), cfg_.testCall.voice, cfg_.testCall.speechRate, *pcm, err);
        auto timer = new QTimer(this);
        auto pos = std::make_shared<size_t>(0);
        connect(timer, &QTimer::timeout, this, [this, timer, pcm, pos, m] {
            if (!txActive_) {
                if (!txReady_) return;                     // wait for the transmit session
                refreshTxModes();
                txMode_->setCurrentText(m);
                startTxFake_ = true;
                startTx();
                startTxFake_ = false;
                if (!txActive_) { timer->stop(); return; }
                return;
            }
            if (*pos + 160 > pcm->size()) { timer->stop(); stopTx(); QTimer::singleShot(15000, qApp, &QApplication::quit); return; }
            onMicFrame(std::vector<int16_t>(pcm->begin() + long(*pos), pcm->begin() + long(*pos + 160)));
            *pos += 160;
        });
        QTimer::singleShot(3000, this, [timer] { timer->start(20); });
    }
    const int cl = args.indexOf("--change-link");   // test aid: open the link picker for a mode
    if (cl > 0 && cl + 1 < args.size()) {
        const QString m = args[cl + 1];
        QTimer::singleShot(4000, this, [this, m] { changeLink(m); });
    }
    const int st = args.indexOf("--settings");   // open the settings page straight away (launcher action)
    if (st > 0) {
        const QString tab = st + 1 < args.size() && !args[st + 1].startsWith("--") ? args[st + 1] : QString();
        QTimer::singleShot(0, this, [this, tab] { openSettings(tab); });
    }
    const int ec = args.indexOf("--echo-call");   // --echo-call MODE: real echo test through the hotspot
    if (ec > 0 && ec + 1 < args.size()) {   // test aid: keeps out of the CSVs and recordings like --test-call
        logFile_->close();
        dstarFile_->close();
        genFile_->close();
        pageFile_->close();
        recDir_ = QDir::tempPath() + "/pistar-dmr-test-recordings";
        testEcho_->setChecked(true);
        const QString m = args[ec + 1];
        QTimer::singleShot(4000, this, [this, m] { startTestCall(m); });   // let the capture connect first
    }
    const int tc = args.indexOf("--test-call");   // test aid: --test-call MODE (no CSV, temp recordings)
    if (tc > 0 && tc + 1 < args.size()) {
        logFile_->close();
        dstarFile_->close();
        genFile_->close();
        pageFile_->close();
        recDir_ = QDir::tempPath() + "/pistar-dmr-test-recordings";
        testEcho_->setChecked(false);
        startTestCall(args[tc + 1]);
        return;
    }
    const int r = args.indexOf("--replay");
    if (r > 0 && r + 1 < args.size()) {
        // don't put test replays in the saved logs
        logFile_->close();
        dstarFile_->close();
        genFile_->close();
        pageFile_->close();
        recDir_ = QDir::tempPath() + "/pistar-dmr-test-recordings";
        startReplay(args[r + 1]);
    }
    else if (!demo_)
        startCapture();
}

void MainWindow::startReplay(const QString &path)
{
    // Feed a capture saved by dmr_listen.py --dump at real-time pace (60 ms/burst).
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { setStatus("Cannot open " + path, "#d93025"); return; }
    const QByteArray all = f.readAll();
    auto packets = std::make_shared<QList<QByteArray>>();
    for (int pos = 0; pos + 2 <= all.size();) {
        const int n = (uint8_t(all[pos]) << 8) | uint8_t(all[pos + 1]);
        if (pos + 2 + n > all.size()) break;
        if (n) packets->append(all.mid(pos + 2, n));
        pos += 2 + n;
    }
    setStatus(QString("Replaying %1 (%2 packets)").arg(path).arg(packets->size()), "#1a73e8");
    auto *t = new QTimer(this);
    auto idx = std::make_shared<int>(0);
    connect(t, &QTimer::timeout, this, [this, t, packets, idx] {
        if (*idx >= packets->size()) {
            t->stop();
            setStatus("Replay finished", "#1a73e8");
            if (QApplication::arguments().contains("--play-newest")) toggleRecordingPlayback(0);   // test aid
            return;
        }
        dispatchRecord(packets->at((*idx)++));
    });
    t->start(20);   // D-Star frames are 20 ms; DMR bursts will replay 3x fast, which is fine for testing
}

void MainWindow::startTestCall(const QString &mode)
{
    if (testTimer_ || injectProc_) return;   // one at a time
    const bool echo = testEcho_ && testEcho_->isChecked();
    QString notReady;
    if (echo && !cfg_.stationReady(&notReady)) {   // never send on air with a placeholder identity
        setStatus("Test call: " + notReady, "#d93025");
        return;
    }
    if (!modeAllowed(mode, echo)) {
        setStatus(QString("Test call: %1%2 is not enabled in Pi-Star").arg(mode, echo ? " (network)" : ""), "#d93025");
        return;
    }
    const QString text = cfg_.testTextMessage();
    std::vector<int16_t> pcm;
    QString err;
    if (!TestCall::speak(cfg_.testSpokenText(), cfg_.testCall.voice, cfg_.testCall.speechRate, pcm, err)) {
        setStatus("Test call: " + err, "#d93025");
        return;
    }
    TestCall::Plan plan = TestCall::build(mode, pcm, text, echo, cfg_, err);
    if (plan.records.isEmpty()) {
        setStatus("Test call: " + (err.isEmpty() ? QString("nothing to send") : err), "#d93025");
        return;
    }
    if (echo) {
        startEchoCall(mode, plan.records, plan.intervalMs);
        return;
    }

    auto records = std::make_shared<QList<QByteArray>>(std::move(plan.records));
    auto idx = std::make_shared<int>(0);
    testFeeding_ = true;
    if (testBtn_) testBtn_->setEnabled(false);
    testTimer_ = new QTimer(this);
    connect(testTimer_, &QTimer::timeout, this, [this, records, idx] {
        if (*idx < records->size()) { dispatchRecord(records->at((*idx)++)); return; }
        testTimer_->stop();
        testTimer_->deleteLater();
        testTimer_ = nullptr;
        // Let the call end (terminator already sent; timeout as a fallback) before clearing the flag.
        QTimer::singleShot(2000, this, [this] {
            testFeeding_ = false;
            // --test-call: the call has ended and been recorded, so finish (used by CI).
            if (QApplication::arguments().contains("--test-call")) QTimer::singleShot(300, qApp, &QApplication::quit);
            refreshTestModes();
        });
    });
    testTimer_->start(plan.intervalMs);
}

MainWindow::~MainWindow() = default;

// ---------------------------------------------------------------- UI
void MainWindow::buildUi()
{
    setWindowTitle("Pi-Star DMR Monitor");   // window title kept: launcher/xdotool use it
    setWindowIcon(QIcon::fromTheme("audio-speakers", QIcon::fromTheme("network-wireless")));

    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    // connection status
    auto *statusRow = new QHBoxLayout;
    statusDot_ = new QLabel("●");
    statusText_ = new ElideLabel;
    statusRow->addWidget(statusDot_);
    statusRow->addWidget(statusText_, 1);

    root->addLayout(statusRow);
    // What each enabled mode is linked to (reflector / talkgroup / master)
    endpointsLabel_ = new QLabel;
    endpointsLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);   // wraps; never widens the window
    endpointsLabel_->setWordWrap(true);
    endpointsLabel_->setTextFormat(Qt::RichText);
    endpointsLabel_->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    connect(endpointsLabel_, &QLabel::linkActivated, this, [this](const QString &href) {
        if (href.startsWith("link:")) changeLink(href.mid(5));
    });
    root->addWidget(endpointsLabel_);

    // active call panel
    callPanel_ = new QFrame;
    callPanel_->setObjectName("callPanel");
    callPanel_->setMinimumHeight(110);
    auto *cp = new QGridLayout(callPanel_);
    cp->setContentsMargins(14, 10, 14, 10);
    callTitle_ = new ElideLabel;
    QFont tf = callTitle_->font(); tf.setPointSizeF(tf.pointSizeF() * 2.0); tf.setBold(true);
    callTitle_->setFont(tf);
    callSub_ = new ElideLabel;
    QFont sf = callSub_->font(); sf.setPointSizeF(sf.pointSizeF() * 1.2);
    callSub_->setFont(sf);
    callTime_ = new QLabel;
    QFont mf = callTime_->font(); mf.setPointSizeF(mf.pointSizeF() * 1.8); mf.setFamily("monospace");
    callTime_->setFont(mf);
    callTime_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    levelBar_ = new QProgressBar;
    levelBar_->setRange(0, 100);
    levelBar_->setTextVisible(false);
    levelBar_->setMaximumHeight(8);
    cp->addWidget(callTitle_, 0, 0);
    cp->addWidget(callTime_, 0, 1, 2, 1);
    cp->addWidget(callSub_, 1, 0);
    cp->addWidget(levelBar_, 2, 0, 1, 2);
    cp->setColumnStretch(0, 1);
    root->addWidget(callPanel_);

    // volume
    auto *volRow = new QHBoxLayout;
    auto *volIcon = new QLabel("Volume");
    volume_ = new QSlider(Qt::Horizontal);
    volume_->setRange(0, 150);
    volumeLabel_ = new QLabel;
    volumeLabel_->setMinimumWidth(45);
    mute_ = new QCheckBox("Mute");
    volume_->setValue(cfg_.audio.volume);
    mute_->setChecked(cfg_.audio.mute);
    connect(volume_, &QSlider::valueChanged, this, [this](int v) {
        cfg_.audio.volume = v;
        saveSettings();
        updateVolumeLabel();
    });
    connect(mute_, &QCheckBox::toggled, this, [this](bool m) {
        cfg_.audio.mute = m;
        saveSettings();
        updateVolumeLabel();
    });
    volRow->addWidget(volIcon);
    volRow->addWidget(volume_, 1);
    volRow->addWidget(volumeLabel_);
    volRow->addWidget(mute_);
    root->addLayout(volRow);
    updateVolumeLabel();

    // banner for the latest page to my pager
    pageBanner_ = new QFrame;
    pageBanner_->setObjectName("pageBanner");
    pageBanner_->setStyleSheet("#pageBanner { background: #fef7e0; border: 1px solid #f9ab00; border-radius: 8px; }"
                               "#pageBanner QLabel { color: #3c2a00; }");
    auto *pb = new QHBoxLayout(pageBanner_);
    pb->setContentsMargins(12, 6, 6, 6);
    pageBannerText_ = new QLabel;
    pageBannerText_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    pageBannerText_->setWordWrap(true);
    pageBannerText_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *dismiss = new QPushButton("✕");
    dismiss->setFlat(true);
    dismiss->setFixedWidth(28);
    dismiss->setToolTip("Dismiss");
    connect(dismiss, &QPushButton::clicked, pageBanner_, &QWidget::hide);
    pb->addWidget(pageBannerText_, 1);
    pb->addWidget(dismiss, 0, Qt::AlignTop);
    pageBanner_->hide();
    root->addWidget(pageBanner_);

    // call log
    log_ = new QTableWidget(0, 9);
    log_->setHorizontalHeaderLabels({"Time", "Mode", "Via", "Callsign", "Name", "Target", "Duration", "BER", "Recording"});
    log_->horizontalHeaderItem(kPlayCol)->setToolTip(
        QString("Audio is kept for the last %1 calls. Click ▶ (or double-click the row) to replay.").arg(cfg_.recording.keepCalls));
    connect(log_, &QTableWidget::cellClicked, this, [this](int row, int col) {
        if (col == kPlayCol) toggleRecordingPlayback(row);
    });
    connect(log_, &QTableWidget::cellDoubleClicked, this, [this](int row, int col) {
        if (col != kPlayCol) toggleRecordingPlayback(row);
    });
    log_->verticalHeader()->setVisible(false);
    log_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    log_->setSelectionBehavior(QAbstractItemView::SelectRows);
    log_->setAlternatingRowColors(true);
    log_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    log_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);   // Name and Target share the spare width
    log_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);

    // pager tab
    auto *pagerTab = new QWidget;
    auto *pv = new QVBoxLayout(pagerTab);
    pv->setContentsMargins(0, 6, 0, 0);
    auto *pagerTop = new QHBoxLayout;
    ricEdit_ = new QLineEdit(cfg_.pager.myRics);
    ricEdit_->setPlaceholderText("e.g. 1234567");
    ricEdit_->setToolTip("Your pager address(es) (RIC). Separate several with commas.");
    ricEdit_->setMaximumWidth(220);
    onlyMine_ = new QCheckBox("Only my pager");
    onlyMine_->setChecked(cfg_.pager.onlyMine);
    pagerTop->addWidget(new QLabel("My pager RIC:"));
    pagerTop->addWidget(ricEdit_);
    pagerTop->addSpacing(12);
    pagerTop->addWidget(onlyMine_);
    pagerTop->addStretch(1);
    pv->addLayout(pagerTop);
    pages_ = new QTableWidget(0, 4);
    pages_->setHorizontalHeaderLabels({"Time", "RIC", "Type", "Message"});
    pages_->verticalHeader()->setVisible(false);
    pages_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    pages_->setSelectionBehavior(QAbstractItemView::SelectRows);
    pages_->setAlternatingRowColors(true);
    pages_->setWordWrap(true);
    pages_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    pages_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    pv->addWidget(pages_, 1);

    parseRics();
    connect(ricEdit_, &QLineEdit::editingFinished, this, [this] {
        cfg_.pager.myRics = ricEdit_->text();
        saveSettings();
        parseRics();
        applyPagerFilter();
    });
    connect(onlyMine_, &QCheckBox::toggled, this, [this](bool on) {
        cfg_.pager.onlyMine = on;
        saveSettings();
        applyPagerFilter();
    });

    tabs_ = new QTabWidget;
    tabs_->addTab(log_, "Calls");
    tabs_->addTab(pagerTab, "Pager");
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int i) {
        if (i == 1) { unreadPages_ = 0; updatePagerTabTitle(); }
    });
    root->addWidget(tabs_, 1);

    // PTT transmit row
    auto *txRow = new QHBoxLayout;
    auto *txLbl = new QLabel("<b>Transmit</b>");
    txMode_ = new QComboBox;
    txMode_->setToolTip("Mode to transmit in (modes whose network is enabled in Pi-Star)");
    txTargetLabel_ = new QLabel;
    txTargetLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    txTargetLabel_->setMinimumWidth(120);
    txTg_ = new QSpinBox;
    txTg_->setRange(1, 16777215);
    txTg_->setGroupSeparatorShown(false);
    txTg_->setPrefix("TG ");
    txTg_->setValue(int(cfg_.transmit.dmrTalkgroup));
    txTg_->setToolTip("DMR talkgroup (or private-call ID) to transmit to on BrandMeister");
    txPrivate_ = new QCheckBox("Private");
    txPrivate_->setChecked(cfg_.transmit.dmrPrivate);
    micLevel_ = new QProgressBar;
    micLevel_->setRange(0, 100);
    micLevel_->setTextVisible(false);
    micLevel_->setMaximumWidth(90);
    micLevel_->setMaximumHeight(12);
    micLevel_->setToolTip("Microphone level while transmitting");
    pttBtn_ = new QPushButton("PTT");
    pttBtn_->setMinimumWidth(110);
    pttBtn_->setMinimumHeight(34);
    pttBtn_->setCheckable(cfg_.transmit.latch);
    pttBtn_->setStyleSheet("QPushButton { font-weight: bold; } QPushButton:pressed, QPushButton:checked "
                           "{ background: #d93025; color: white; }");
    connect(pttBtn_, &QPushButton::pressed, this, [this] { if (!cfg_.transmit.latch) startTx(); });
    connect(pttBtn_, &QPushButton::released, this, [this] { if (!cfg_.transmit.latch) stopTx(); });
    connect(pttBtn_, &QPushButton::clicked, this, [this](bool on) {
        if (!cfg_.transmit.latch) return;
        if (on) startTx(); else stopTx();
    });
    connect(txMode_, &QComboBox::currentTextChanged, this, [this](const QString &m) {
        if (m.isEmpty()) return;
        cfg_.transmit.mode = m;
        saveSettings();
        updateTxTarget();
    });
    connect(txTg_, &QSpinBox::valueChanged, this, [this](int v) { cfg_.transmit.dmrTalkgroup = uint32_t(v); saveSettings(); updateTxTarget(); });
    connect(txPrivate_, &QCheckBox::toggled, this, [this](bool on) { cfg_.transmit.dmrPrivate = on; saveSettings(); updateTxTarget(); });
    txRow->addWidget(txLbl);
    txRow->addWidget(txMode_);
    txRow->addWidget(txTg_);
    txRow->addWidget(txPrivate_);
    txRow->addWidget(txTargetLabel_, 1);
    txRow->addWidget(new QLabel("Mic"));
    txRow->addWidget(micLevel_);
    txRow->addWidget(pttBtn_);
    root->addLayout(txRow);
    refreshTxModes();
    connect(&txKeepalive_, &QTimer::timeout, this, [this] { if (txSession_ && txReady_) txWrite(0); });
    txKeepalive_.start(5000);

    // bottom row
    auto *bottom = new QHBoxLayout;
    onTop_ = new QCheckBox("Always on top");
    onTop_->setChecked(cfg_.window.alwaysOnTop);
    auto applyOnTop = [this](bool on) {
        cfg_.window.alwaysOnTop = on;
        saveSettings();
        setWindowFlag(Qt::WindowStaysOnTopHint, on);
        show();
    };
    connect(onTop_, &QCheckBox::toggled, this, applyOnTop);
    if (onTop_->isChecked()) setWindowFlag(Qt::WindowStaysOnTopHint, true);
    auto *clear = new QPushButton("Clear log");
    connect(clear, &QPushButton::clicked, this, [this] {
        if (tabs_->currentIndex() == 1) pages_->setRowCount(0);
        else { stopRecordingPlayback(); log_->setRowCount(0); }
    });
    bottom->addWidget(onTop_);
    bottom->addStretch(1);
    testMode_ = new QComboBox;
    testMode_->addItems(TestCall::modes());
    testMode_->setCurrentText(cfg_.testCall.mode);
    testMode_->setToolTip("Mode for the synthetic test call");
    testBtn_ = new QPushButton("Test call");
    testBtn_->setToolTip("Play a synthetic test call (spoken test message) in the selected mode");
    testEcho_ = new QCheckBox("Echo via hotspot");
    testEcho_->setChecked(cfg_.testCall.echo);
    testEcho_->setToolTip("Send the test call into Pi-Star as if from your radio, addressed to the mode's echo / parrot\n"
                          "(targets in Settings → Test call). The reply comes back over the network and the hotspot\n"
                          "transmits it on air. YSF/P25/NXDN links are restored afterwards.\n"
                          "Unticked: the call is only played through the app's decoder, nothing is sent.");
    connect(testEcho_, &QCheckBox::toggled, this, [this](bool on) {
        cfg_.testCall.echo = on;
        saveSettings();
        refreshTestModes();
    });
    connect(testMode_, &QComboBox::currentTextChanged, this, [this](const QString &m) { cfg_.testCall.mode = m; saveSettings(); });
    connect(testBtn_, &QPushButton::clicked, this, [this] { startTestCall(testMode_->currentText()); });
    auto *settingsBtn = new QPushButton("Settings…");
    settingsBtn->setToolTip("Change any setting (saved in " + Settings::path() + ")");
    connect(settingsBtn, &QPushButton::clicked, this, [this] { openSettings(); });
    bottom->addWidget(testEcho_);
    bottom->addWidget(testMode_);
    bottom->addWidget(testBtn_);
    bottom->addSpacing(12);
    bottom->addWidget(clear);
    bottom->addWidget(settingsBtn);
    root->addLayout(bottom);

    setCentralWidget(central);
    if (!restoreGeometry(QSettings().value("geometry").toByteArray()))
        resize(640, 520);

    tick();   // paint idle panel
}

void MainWindow::updateVolumeLabel()
{
    volumeLabel_->setText(mute_->isChecked() ? "muted" : QString("%1%").arg(volume_->value()));
    volume_->setEnabled(!mute_->isChecked());
}

void MainWindow::setStatus(const QString &text, const QString &colour)
{
    const QString css = QString("color: %1; font-size: 14pt;").arg(colour);
    if (statusDot_->styleSheet() != css) statusDot_->setStyleSheet(css);   // re-styling makes the window redraw
    if (statusText_->text() != text) statusText_->setText(text);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    quitting_ = true;
    QSettings().setValue("geometry", saveGeometry());
    for (int s = 1; s <= 2; ++s) endCall(s, "closed");
    endAllDStar("closed");
    endGen(ysf_, "closed");
    endGen(p25_, "closed");
    endGen(nxdn_, "closed");
    endGen(m17_, "closed");
    if (ssh_) { ssh_->kill(); ssh_->waitForFinished(1000); }   // remote sniffer exits on broken pipe
    if (idsProc_) idsProc_->kill();
    if (nxdnIdsProc_) nxdnIdsProc_->kill();
    if (txActive_) stopTx("closing");
    mic_.reset();
    if (txSession_) { txSession_->disconnect(this); txSession_->kill(); txSession_->waitForFinished(500); }
    liveOut_.reset();
    replayOut_.reset();
    e->accept();
}

// ---------------------------------------------------------------- capture
QStringList MainWindow::sshArgs() const
{
    QStringList a = {"-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
                     "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=2",
                     "-o", "LogLevel=ERROR"};
    if (!cfg_.connection.identityFile.isEmpty()) a << "-i" << cfg_.connection.identityFile;
    a << QString("%1@%2").arg(cfg_.connection.user, cfg_.connection.host);
    return a;
}

void MainWindow::saveSettings()
{
    if (!persist_) return;
    QString err;
    if (!cfg_.save(&err)) setStatus("Could not save settings: " + err, "#d93025");
}

void MainWindow::parseRics()
{
    myRics_.clear();
    for (const QString &part : ricEdit_->text().split(QRegularExpression("[,;\\s]+"), Qt::SkipEmptyParts)) {
        bool ok = false;
        const uint r = part.toUInt(&ok);
        if (ok) myRics_.insert(r);
    }
}

void MainWindow::openLogFiles()
{
    const QString dir = cfg_.logDir();
    QDir().mkpath(dir);
    struct F { QFile *f; const char *name; const char *header; };
    const F files[] = {
        {logFile_, "calls.csv", "start,slot,src_id,callsign,name,target,duration_s,ber_pct\n"},
        {dstarFile_, "dstar_calls.csv", "start,source,my,suffix,your,rpt1,rpt2,text,duration_s,ber_pct,reflector\n"},
        {genFile_, "ysf_p25_calls.csv", "start,mode,source,callsign,name,target,duration_s,ber_pct,note\n"},
        {pageFile_, "pages.csv", "time,ric,type,message\n"},
    };
    for (const F &x : files) {
        x.f->close();
        x.f->setFileName(dir + "/" + x.name);
        const bool fresh = !x.f->exists();
        if (x.f->open(QIODevice::Append | QIODevice::Text) && fresh) x.f->write(x.header);
    }
}

void MainWindow::openSettings(const QString &tab)
{
    SettingsDialog dlg(cfg_, this);
    if (!tab.isEmpty()) QTimer::singleShot(0, &dlg, [&dlg, tab] { dlg.showTab(tab); });
    if (dlg.exec() != QDialog::Accepted) return;
    const Settings old = cfg_;
    cfg_ = dlg.result();
    saveSettings();
    applySettings(old);
}

void MainWindow::applySettings(const Settings &old)
{
    // Quick controls that mirror settings
    for (QWidget *w : std::initializer_list<QWidget *>{volume_, mute_, ricEdit_, onlyMine_, testMode_, testEcho_})
        w->blockSignals(true);
    volume_->setValue(cfg_.audio.volume);
    mute_->setChecked(cfg_.audio.mute);
    ricEdit_->setText(cfg_.pager.myRics);
    onlyMine_->setChecked(cfg_.pager.onlyMine);
    testMode_->setCurrentText(cfg_.testCall.mode);
    testEcho_->setChecked(cfg_.testCall.echo);
    for (QWidget *w : std::initializer_list<QWidget *>{volume_, mute_, ricEdit_, onlyMine_, testMode_, testEcho_})
        w->blockSignals(false);
    updateVolumeLabel();
    parseRics();
    applyPagerFilter();
    if (onTop_->isChecked() != cfg_.window.alwaysOnTop) onTop_->setChecked(cfg_.window.alwaysOnTop);

    // Transmit controls
    if (txActive_) stopTx("settings changed");
    pttBtn_->setCheckable(cfg_.transmit.latch);
    txTg_->blockSignals(true);
    txTg_->setValue(int(cfg_.transmit.dmrTalkgroup));
    txTg_->blockSignals(false);
    txPrivate_->blockSignals(true);
    txPrivate_->setChecked(cfg_.transmit.dmrPrivate);
    txPrivate_->blockSignals(false);
    refreshTxModes();

    // Station: own NXDN ID label
    if (old.station.nxdnId != cfg_.station.nxdnId || old.station.callsign != cfg_.station.callsign) {
        if (nxdnIds_.value(old.station.nxdnId).first == old.station.callsign) nxdnIds_.remove(old.station.nxdnId);
        if (cfg_.station.nxdnId && !nxdnIds_.contains(cfg_.station.nxdnId)) nxdnIds_.insert(cfg_.station.nxdnId, {cfg_.station.callsign, QString()});
    }

    // Audio output: reopen on the new backend / device the next time audio plays
    if (old.audio.backend != cfg_.audio.backend || old.audio.device != cfg_.audio.device) {
        stopRecordingPlayback();
        liveOut_.reset();
        replayOut_.reset();
        audioRetryMs_ = 0;
    }

    // Logs and recordings
    if (persist_ && old.logDir() != cfg_.logDir()) openLogFiles();
    if (persist_) recDir_ = cfg_.recordingDir();
    log_->horizontalHeaderItem(kPlayCol)->setToolTip(
        QString("Audio is kept for the last %1 calls. Click ▶ (or double-click the row) to replay.").arg(cfg_.recording.keepCalls));
    pruneRecordings();
    refreshPlayCells();

    // Connection: reconnect (the capture's finished handler retries) and reload ID lists
    if (old.connection.host != cfg_.connection.host || old.connection.user != cfg_.connection.user ||
        old.connection.identityFile != cfg_.connection.identityFile || old.connection.pageHistory != cfg_.connection.pageHistory) {
        if (ssh_) ssh_->kill();
        else startCapture();
        loadIds();
        loadNxdnIds();
    }
    setStatus("Settings saved", "#1e8e3e");
}

void MainWindow::startCapture()
{
    if (quitting_) return;
    rxBuf_.clear();
    pagesLive_ = false;
    setStatus(QString("Connecting to %1@%2 …").arg(cfg_.connection.user, cfg_.connection.host), "#e6a700");

    ssh_ = new QProcess(this);
    connect(ssh_, &QProcess::started, this, [this] {
        // The page history length is a setting; patch it into the script.
        QByteArray script(kRemoteSniffPy);
        script.replace("PAGE_HISTORY = 500", "PAGE_HISTORY = " + QByteArray::number(cfg_.connection.pageHistory));
        ssh_->write(script);
        ssh_->closeWriteChannel();
    });
    connect(ssh_, &QProcess::readyReadStandardOutput, this, &MainWindow::onSshData);
    connect(ssh_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        QString err = QString::fromLocal8Bit(ssh_->readAllStandardError()).trimmed();
        ssh_->deleteLater();
        ssh_ = nullptr;
        for (int s = 1; s <= 2; ++s) endCall(s, "link lost");
        endAllDStar("link lost");
        endGen(ysf_, "link lost");
        endGen(p25_, "link lost");
        endGen(nxdn_, "link lost");
        endGen(m17_, "link lost");
        if (quitting_) return;
        QString why = err.isEmpty() ? QString("exit %1").arg(code) : err.section('\n', -1);
        setStatus(QString("Disconnected (%1) — retrying in %2 s").arg(why).arg(cfg_.connection.reconnectDelayMs / 1000.0), "#d93025");
        QTimer::singleShot(cfg_.connection.reconnectDelayMs, this, &MainWindow::startCapture);
    });
    ssh_->setProperty("lastRx", QDateTime::currentMSecsSinceEpoch());
    if ((persist_ || pttTest_) && !txSession_) startTxSession();
    ssh_->start("ssh", sshArgs() << "sudo -n python3 -u -");
}

void MainWindow::loadIds()
{
    if (idsProc_) {   // a reload while the previous one is still running
        idsProc_->disconnect(this);
        idsProc_->kill();
        idsProc_->deleteLater();
        idsProc_ = nullptr;
    }
    idsProc_ = new QProcess(this);
    connect(idsProc_, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        const QByteArray all = idsProc_->readAllStandardOutput();
        for (const QByteArray &line : all.split('\n')) {
            const QList<QByteArray> f = line.split('\t');
            bool ok = false;
            const uint32_t id = f.value(0).toUInt(&ok);
            if (ok && f.size() >= 2)
            {
                const QString call = QString::fromUtf8(f[1]).trimmed();
                const QString name = QString::fromUtf8(f.value(2)).trimmed();
                ids_.insert(id, {call, name});
                if (!name.isEmpty() && !nameByCall_.contains(call.toUpper()))
                    nameByCall_.insert(call.toUpper(), name);
            }
        }
        idsProc_->deleteLater();
        idsProc_ = nullptr;
    });
    idsProc_->start("ssh", sshArgs()
                                         << "cat /usr/local/etc/DMRIds.dat");
}

void MainWindow::startEchoCall(const QString &mode, const QList<QByteArray> &records, int intervalMs)
{
    const uint32_t parrotTg = mode == "P25" ? cfg_.testCall.p25EchoTg : cfg_.testCall.nxdnEchoTg;
    QJsonObject job{{"mode", mode}, {"interval_ms", intervalMs}, {"count", int(records.size())},
                    {"ysf_parrot", cfg_.testCall.ysfParrot}, {"parrot_tg", double(parrotTg)}};
    QByteArray input = QJsonDocument(job).toJson(QJsonDocument::Compact) + "\n";
    for (const QByteArray &r : records) {
        const QByteArray w = TestCall::wirePayload(r);
        input += char(w.size() >> 8);
        input += char(w.size() & 0xFF);
        input += w;
    }
    if (testBtn_) testBtn_->setEnabled(false);
    setStatus(QString("Echo test (%1): starting on the hotspot…").arg(mode), "#1a73e8");
    injectProc_ = new QProcess(this);
    connect(injectProc_, &QProcess::started, this, [this, input] {
        injectProc_->write(input);
        injectProc_->closeWriteChannel();
    });
    connect(injectProc_, &QProcess::readyReadStandardOutput, this, [this, mode] {
        while (injectProc_->canReadLine()) {
            const QString line = QString::fromUtf8(injectProc_->readLine()).trimmed();
            if (line.startsWith("STATUS ")) setStatus(QString("Echo test (%1): %2").arg(mode, line.mid(7)), "#1a73e8");
            else if (line == "DONE") setStatus(QString("Echo test (%1): finished").arg(mode), "#1e8e3e");
            else if (line.startsWith("ERROR ")) setStatus(QString("Echo test (%1) failed: %2").arg(mode, line.mid(6)), "#d93025");
        }
    });
    connect(injectProc_, &QProcess::finished, this, [this, mode](int code, QProcess::ExitStatus) {
        const QString err = QString::fromLocal8Bit(injectProc_->readAllStandardError()).trimmed();
        if (code != 0)
            setStatus(QString("Echo test (%1) failed: %2").arg(mode, err.isEmpty() ? QString("exit %1").arg(code)
                                                                                   : err.section('\n', -1)), "#d93025");
        injectProc_->deleteLater();
        injectProc_ = nullptr;
        refreshTestModes();
    });
    const QByteArray script = QByteArray(kRemoteInjectPy).toBase64();
    injectProc_->start("ssh", sshArgs()
                                            << QString("sudo -n python3 -u -c 'import base64,sys;"
                                                       "exec(base64.b64decode(sys.argv[1]))' %1")
                                                   .arg(QString::fromLatin1(script)));
}

// ---------------------------------------------------------------- PTT transmit
void MainWindow::startTxSession()
{
    txReady_ = false;
    txSession_ = new QProcess(this);
    connect(txSession_, &QProcess::readyReadStandardOutput, this, [this] {
        while (txSession_ && txSession_->canReadLine()) {
            const QString line = QString::fromUtf8(txSession_->readLine()).trimmed();
            if (line == "READY") { txReady_ = true; updateTxTarget(); }
            else if (line.startsWith("STATUS ")) setStatus("Transmit: " + line.mid(7), "#1a73e8");
            else if (line.startsWith("ERROR ")) {
                setStatus("Transmit failed: " + line.mid(6), "#d93025");
                if (txActive_) stopTx("error");
            }
        }
    });
    connect(txSession_, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        txSession_->deleteLater();
        txSession_ = nullptr;
        txReady_ = false;
        if (txActive_) stopTx("link lost");
        updateTxTarget();
        if (!quitting_) QTimer::singleShot(cfg_.connection.reconnectDelayMs, this, [this] { if (!txSession_) startTxSession(); });
    });
    const QString script = QString::fromLatin1(QByteArray(kRemoteTxPy).toBase64());
    txSession_->start("ssh", sshArgs() << QString("sudo -n python3 -u -c 'import base64,sys;exec(base64.b64decode(sys.argv.pop(1)))' %1").arg(script));
}

void MainWindow::txWrite(char type, const QByteArray &body)
{
    if (!txSession_) return;
    QByteArray frame;
    if (type) frame = char(type) + body;
    const QByteArray len(1, char(frame.size() >> 8));
    txSession_->write(len + char(frame.size() & 0xFF) + frame);
}

void MainWindow::refreshTxModes()
{
    if (!txMode_) return;
    QStringList allowed;
    for (const QString &m : TestCall::modes())
        if (modeAllowed(m, true)) allowed << m;
    txMode_->blockSignals(true);
    txMode_->clear();
    txMode_->addItems(allowed);
    if (allowed.contains(cfg_.transmit.mode)) txMode_->setCurrentText(cfg_.transmit.mode);
    txMode_->blockSignals(false);
    updateTxTarget();
}

bool MainWindow::txTarget(TxTarget &t, QString &err) const
{
    t.mode = txMode_->currentText();
    t.dstarSuffix = cfg_.transmit.dstarSuffix;
    const QString linked = endpoints_.value(t.mode).toString();
    if (t.mode.isEmpty()) { err = "no transmit modes are enabled in Pi-Star"; return false; }
    if (t.mode == "DMR") {
        t.dst = cfg_.transmit.dmrTalkgroup;
        t.privateCall = cfg_.transmit.dmrPrivate;
        return true;
    }
    if (linked.isEmpty()) { err = t.mode + " is not linked; click it in the link row to choose a reflector or talkgroup"; return false; }
    if (t.mode == "P25" || t.mode == "NXDN") {
        bool ok = false;
        t.dst = linked.section(' ', 1, 1).toUInt(&ok);   // "TG 31672" / "TG 10 (Parrot)"
        if (!ok) { err = "cannot tell which talkgroup " + t.mode + " is on"; return false; }
    }
    return true;   // D-Star CQCQCQ, YSF ALL and M17 ALL go to the linked reflector
}

void MainWindow::updateTxTarget()
{
    if (!txTargetLabel_) return;
    const bool dmr = txMode_->currentText() == "DMR";
    txTg_->setVisible(dmr);
    txPrivate_->setVisible(dmr);
    TxTarget t;
    QString err;
    QString text;
    if (demo_) txTarget(t, err), text = "→ " + endpoints_.value(t.mode).toString().toHtmlEscaped(), err.clear();
    else if (!cfg_.stationReady(&err)) text = QString("<span style='color: #d93025'>%1</span>").arg(err.toHtmlEscaped());
    else if (!txSession_ || !txReady_) text = "<span style='color: gray'>connecting…</span>";
    else if (!txTarget(t, err)) text = QString("<span style='color: #d93025'>%1</span>").arg(err.toHtmlEscaped());
    else if (dmr) text = QString("→ %1 on BrandMeister").arg(t.privateCall ? QString("private %1").arg(t.dst) : QString("TG %1").arg(t.dst));
    else text = "→ " + endpoints_.value(t.mode).toString().toHtmlEscaped();
    txTargetLabel_->setText(text);
    txTargetLabel_->setToolTip(err);
    pttBtn_->setEnabled(txActive_ || (txSession_ && txReady_ && err.isEmpty() && !t.mode.isEmpty()));
}

void MainWindow::startTx()
{
    if (txActive_) return;
    TxTarget t;
    QString err;
    if (!cfg_.stationReady(&err)) {}
    else if (!txSession_ || !txReady_) err = "the hotspot connection is not ready";
    else txTarget(t, err);
    if (err.isEmpty()) txEnc_ = VoiceEncoder::create(cfg_, t, &err);
    if (!err.isEmpty()) {
        setStatus("Cannot transmit: " + err, "#d93025");
        if (pttBtn_->isCheckable()) pttBtn_->setChecked(false);
        return;
    }
    mic_ = std::make_unique<AudioIn>();
    if (!startTxFake_ && !mic_->start(cfg_.transmit.inputDevice, this, [this](const std::vector<int16_t> &f) { onMicFrame(f); }, &err)) {
        txEnc_.reset();
        setStatus("Cannot transmit: " + err, "#d93025");
        if (pttBtn_->isCheckable()) pttBtn_->setChecked(false);
        return;
    }
    txActive_ = true;
    txStartMs_ = QDateTime::currentMSecsSinceEpoch();
    if (liveOut_) liveOut_->flush();
    txWrite('B', QJsonDocument(QJsonObject{{"mode", t.mode}}).toJson(QJsonDocument::Compact));
    for (const QByteArray &r : txEnc_->begin()) txWrite('P', TestCall::wirePayload(r));
    pttBtn_->setText("ON AIR");
    setStatus(QString("Transmitting %1 %2").arg(t.mode, txTargetLabel_->text().remove(QRegularExpression("<[^>]*>"))), "#d93025");
}

void MainWindow::onMicFrame(const std::vector<int16_t> &frame)
{
    if (!txActive_ || !txEnc_) return;
    // Gain, then automatic level control (fast attack, slow release, at most +18 dB) and a hard limit.
    const double gain = std::pow(10.0, cfg_.transmit.micGainDb / 20.0);
    std::vector<int16_t> pcm(frame.size());
    double peak = 1, acc = 0;
    for (int16_t v : frame) peak = std::max(peak, std::abs(v * gain));
    if (cfg_.transmit.autoLevel) {
        const double want = std::min(8.0, 16000.0 / peak);
        micGain_ = want < micGain_ ? want : micGain_ + (want - micGain_) * 0.02;
    } else {
        micGain_ = 1.0;
    }
    for (size_t i = 0; i < frame.size(); ++i) {
        const double v = std::clamp(frame[i] * gain * micGain_, -32000.0, 32000.0);
        pcm[i] = int16_t(v);
        acc += v * v;
    }
    micLevel_->setValue(int(std::min(100.0, 100.0 * std::sqrt(acc / frame.size()) / 8000.0)));
    for (const QByteArray &r : txEnc_->frame(pcm.data())) txWrite('P', TestCall::wirePayload(r));
    if (QDateTime::currentMSecsSinceEpoch() - txStartMs_ > cfg_.transmit.timeoutSec * 1000LL)
        stopTx(QString("time limit (%1 s) reached").arg(cfg_.transmit.timeoutSec));
}

void MainWindow::stopTx(const QString &why)
{
    if (!txActive_) return;
    txActive_ = false;
    if (mic_) mic_->stop();
    if (txEnc_) for (const QByteArray &r : txEnc_->end()) txWrite('P', TestCall::wirePayload(r));
    txWrite('E');
    txEnc_.reset();
    txMuteUntilMs_ = QDateTime::currentMSecsSinceEpoch() + 800;   // our own call is still arriving back
    micLevel_->setValue(0);
    pttBtn_->setText("PTT");
    if (pttBtn_->isCheckable()) pttBtn_->setChecked(false);
    const double secs = (QDateTime::currentMSecsSinceEpoch() - txStartMs_) / 1000.0;
    setStatus(QString("Transmitted %1 s%2").arg(secs, 0, 'f', 1).arg(why.isEmpty() ? QString() : " — " + why),
              why.isEmpty() ? "#1e8e3e" : "#d93025");
    updateTxTarget();
}

void MainWindow::onSshData()
{
    rxBuf_ += ssh_->readAllStandardOutput();
    ssh_->setProperty("lastRx", QDateTime::currentMSecsSinceEpoch());
    if ((persist_ || pttTest_) && !txSession_) startTxSession();
    if (!statusText_->text().startsWith("Connected"))
        setStatus(QString("Connected to %1@%2 — listening").arg(cfg_.connection.user, cfg_.connection.host), "#1e8e3e");

    int pos = 0;
    while (rxBuf_.size() - pos >= 2) {
        const int n = (uint8_t(rxBuf_[pos]) << 8) | uint8_t(rxBuf_[pos + 1]);
        if (rxBuf_.size() - pos - 2 < n) break;
        if (n == 0) pagesLive_ = true;                   // heartbeat; history has been sent
        if (n > 0) {
            dispatchRecord(rxBuf_.mid(pos + 2, n));
        }
        pos += 2 + n;
    }
    rxBuf_.remove(0, pos);
}

// ---------------------------------------------------------------- decoding
void MainWindow::dispatchRecord(const QByteArray &rec)
{
    // Only modes enabled in Pi-Star ("Modes Enabled") are handled.
    if (rec.startsWith("MODE")) handleModes(rec.mid(4));
    else if (rec.startsWith("LINK")) handleLinks(rec.mid(4));
    else if (rec.startsWith("ENDP")) handleEndpoints(rec.mid(4));
    else if (rec.startsWith("PAGE")) { if (modeAllowed("POCSAG")) handlePage(rec.mid(4)); }
    else if (rec.startsWith("DSTR")) { if (modeAllowed("D-Star")) handleDStar(rec.mid(4)); }
    else if (rec.startsWith("YSFX")) { if (modeAllowed("YSF")) handleYsf(rec.mid(4)); }
    else if (rec.startsWith("P25X")) { if (modeAllowed("P25")) handleP25(rec.mid(4)); }
    else if (rec.startsWith("NXDX")) { if (modeAllowed("NXDN")) handleNxdn(rec.mid(4)); }
    else if (rec.startsWith("M17X")) { if (modeAllowed("M17")) handleM17(rec.mid(4)); }
    else if (modeAllowed("DMR")) handlePacket(rec);
}

bool MainWindow::modeAllowed(const QString &mode, bool network) const
{
    if (!modesKnown_) return true;
    const auto m = modes_.value(mode, {false, false});
    return m.first && (!network || m.second);
}

void MainWindow::handleEndpoints(const QByteArray &json)
{
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    for (auto it = o.begin(); it != o.end(); ++it) {
        // Links.log (LINK record) also gives the D-Star protocol; keep that richer text.
        if (it.key() == "D-Star" && endpoints_.value("D-Star").toString().startsWith(it.value().toString()) &&
            !it.value().toString().isEmpty())
            continue;
        endpoints_[it.key()] = it.value();
    }
    updateEndpointsLabel();
}

void MainWindow::updateEndpointsLabel()
{
    static const QList<QPair<QString, QString>> order = {
        {"D-Star", "#1967d2"}, {"DMR", "#1e8e3e"}, {"YSF", "#8430ce"}, {"P25", "#c5221f"},
        {"NXDN", "#00796b"}, {"M17", "#e8710a"}, {"POCSAG", "#b06000"}};
    QStringList parts;
    for (const auto &[mode, colour] : order) {
        if (modesKnown_ && !modeAllowed(mode)) continue;           // disabled in Pi-Star
        if (!modesKnown_ && !endpoints_.contains(mode)) continue;  // nothing heard yet
        QString what;
        const QJsonValue v = endpoints_.value(mode);
        if (modesKnown_ && !modeAllowed(mode, true)) what = "<i>RF only</i>";
        else if (v.isUndefined() || v.isNull()) what = "<span style='color: gray'>—</span>";
        else if (v.toString().isEmpty())
            what = QString("<span style='color: gray'>%1</span>").arg(mode == "DMR" || mode == "POCSAG" ? "not connected" : "not linked");
        else what = v.toString().toHtmlEscaped();
        what.replace(' ', "&nbsp;");   // keep each entry on one line; wrap only between modes
        what.replace("<span&nbsp;", "<span ");
        what.replace("<i&nbsp;", "<i ");
        // Modes whose link can be changed from here are clickable.
        const bool linkable = mode != "DMR" && mode != "POCSAG" && !(modesKnown_ && !modeAllowed(mode, true));
        if (linkable)
            parts << QString("<a href='link:%2' style='text-decoration: none; color: %1'><b>%2</b>&nbsp;"
                             "<span style='color: palette(window-text)'>%3</span></a>").arg(colour, mode, what);
        else
            parts << QString("<span style='color: %1'><b>%2</b></span>&nbsp;%3").arg(colour, mode, what);
    }
    endpointsLabel_->setText(parts.join("&nbsp;&nbsp;· "));
    if (txTargetLabel_) updateTxTarget();
}

void MainWindow::runLinkHelper(const QStringList &args, std::function<void(const QJsonObject &)> done)
{
    auto *p = new QProcess(this);
    connect(p, &QProcess::finished, this, [p, done](int, QProcess::ExitStatus) {
        const QByteArray out = p->readAllStandardOutput();
        const QJsonObject o = QJsonDocument::fromJson(out.trimmed().split('\n').last()).object();
        p->deleteLater();
        done(o.isEmpty() ? QJsonObject{{"ok", false}, {"message", "no answer from the hotspot"}} : o);
    });
    QStringList quoted;
    for (const QString &a : args) quoted << "'" + QString(a).replace("'", "'\\''") + "'";
    const QString script = QString::fromLatin1(QByteArray(kRemoteLinkPy).toBase64());
    p->start("ssh", sshArgs() << QString("sudo -n python3 -c 'import base64,sys;exec(base64.b64decode(sys.argv.pop(1)))' %1 %2")
                                     .arg(script, quoted.join(' ')));
}

void MainWindow::changeLink(const QString &mode)
{
    const QString current = endpoints_.value(mode).toString().section(" (", 0, 0);   // drop "(DCS)" / "(Parrot)"
    auto open = [this, mode, current](const QJsonObject &list) {
        if (!list.contains("entries")) {
            setStatus(QString("%1: could not load the reflector list: %2").arg(mode, list.value("message").toString()), "#d93025");
            return;
        }
        LinkDialog dlg(mode, current, list.value("entries").toArray(), list.value("modules").toBool(), this);
        if (dlg.exec() != QDialog::Accepted) return;
        const QStringList args = dlg.unlinkRequested() ? QStringList{"unlink", mode} : QStringList{"link", mode, dlg.target()};
        setStatus(QString("%1: %2…").arg(mode, dlg.unlinkRequested() ? QString("unlinking") : "linking to " + dlg.target()), "#1a73e8");
        runLinkHelper(args, [this, mode](const QJsonObject &r) {
            if (r.value("ok").toBool())
                setStatus(r.value("message").toString() + " — the link status updates in a few seconds", "#1e8e3e");
            else
                setStatus(QString("%1 link change failed: %2").arg(mode, r.value("message").toString()), "#d93025");
        });
    };
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto cached = linkLists_.constFind(mode);
    if (cached != linkLists_.constEnd() && now - cached->first < 10 * 60 * 1000) {
        open(cached->second);
        return;
    }
    setStatus(QString("%1: loading the reflector list…").arg(mode), "#1a73e8");
    runLinkHelper({"list", mode}, [this, mode, open](const QJsonObject &list) {
        if (list.contains("entries")) {
            linkLists_[mode] = {QDateTime::currentMSecsSinceEpoch(), list};
            setStatus(QString("Connected to %1@%2 — listening").arg(cfg_.connection.user, cfg_.connection.host), "#1e8e3e");
        }
        open(list);
    });
}

void MainWindow::handleModes(const QByteArray &json)
{
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    if (o.isEmpty()) return;
    modes_.clear();
    for (auto it = o.begin(); it != o.end(); ++it) {
        const QJsonObject m = it.value().toObject();
        modes_.insert(it.key(), {m.value("mode").toBool(), m.value("network").toBool()});
    }
    modesKnown_ = true;

    QStringList on, off;
    for (const QString &m : {"D-Star", "DMR", "YSF", "P25", "NXDN", "M17", "POCSAG", "FM"}) {
        if (!modes_.contains(m)) continue;
        const auto v = modes_.value(m);
        (v.first ? on : off) << (v.first && !v.second ? m + " (RF only)" : m);
    }
    endpointsLabel_->setToolTip(QString("Click a mode to change what it is linked to.\n"
                                        "DMR talkgroups are managed in BrandMeister SelfCare.\n\n"
                                        "Modes enabled in Pi-Star (/etc/mmdvmhost)\nEnabled: %1\nDisabled: %2")
                                    .arg(on.isEmpty() ? QString("none") : on.join(", "),
                                         off.isEmpty() ? QString("none") : off.join(", ")));
    updateEndpointsLabel();

    // End calls in modes that have just been switched off.
    if (!modeAllowed("DMR")) for (int s = 1; s <= 2; ++s) endCall(s, "mode disabled");
    if (!modeAllowed("D-Star")) endAllDStar("mode disabled");
    if (!modeAllowed("YSF")) endGen(ysf_, "mode disabled");
    if (!modeAllowed("P25")) endGen(p25_, "mode disabled");
    if (!modeAllowed("NXDN")) endGen(nxdn_, "mode disabled");
    if (!modeAllowed("M17")) endGen(m17_, "mode disabled");
    tabs_->setTabEnabled(1, modeAllowed("POCSAG"));
    refreshTxModes();
    tabs_->setTabToolTip(1, modeAllowed("POCSAG") ? QString() : QString("POCSAG is disabled in Pi-Star"));
    refreshTestModes();
}

void MainWindow::refreshTestModes()
{
    // Echo calls need the mode's network side as well; local test calls only the mode.
    const bool echo = testEcho_->isChecked();
    QStringList allowed;
    for (const QString &m : TestCall::modes())
        if (modeAllowed(m, echo)) allowed << m;
    testMode_->blockSignals(true);
    testMode_->clear();
    testMode_->addItems(allowed);
    if (allowed.contains(cfg_.testCall.mode)) testMode_->setCurrentText(cfg_.testCall.mode);
    testMode_->blockSignals(false);
    testMode_->setEnabled(!allowed.isEmpty());
    if (testBtn_ && !testTimer_ && !injectProc_) testBtn_->setEnabled(!allowed.isEmpty());
    testMode_->setToolTip(allowed.isEmpty() ? "No test-call modes are enabled in Pi-Star"
                                            : "Mode for the test call (modes enabled in Pi-Star)");
}

void MainWindow::handlePacket(const QByteArray &raw)
{
    DmrPacket p;
    if (!parseDmrd(raw, p)) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (p.isTerminator()) {
        if (active_[p.slot] && active_[p.slot]->stream == p.stream)
            endCall(p.slot, "terminator");
        return;
    }
    if (p.isHeader()) {
        if (!active_[p.slot] || active_[p.slot]->stream != p.stream)
            beginCall(p, false);
        active_[p.slot]->lastPacketMs = now;
        return;
    }
    if (!p.isVoice()) return;

    // Drop the same burst seen twice (e.g. on loopback and the LAN), but not
    // repeated identical frames such as silence.
    const QByteArray key = p.stream + char(p.seq) + voiceBits(p.payload);
    if (seen_.contains(key)) return;
    seen_.insert(key);
    seenOrder_.push_back(key);
    if (int(seenOrder_.size()) > kSeenMax) { seen_.remove(seenOrder_.front()); seenOrder_.pop_front(); }

    if (!active_[p.slot] || active_[p.slot]->stream != p.stream)
        beginCall(p, true);
    Call &c = *active_[p.slot];
    c.lastPacketMs = now;

    float samples[480];
    const int errs = decoder_.decodeBurst(p.payload, samples);
    c.frames += 3;
    c.errors += errs;

    playDecoded(QString("DMR%1").arg(p.slot), samples, 480);
}

void MainWindow::beginCall(const DmrPacket &p, bool lateEntry)
{
    if (active_[p.slot]) endCall(p.slot, "new call");
    Call c;
    c.stream = p.stream;
    c.src = p.src;
    c.dst = p.dst;
    c.slot = p.slot;
    c.privateCall = p.privateCall;
    c.lateEntry = lateEntry;
    c.test = testFeeding_;
    c.started = QDateTime::currentDateTime();
    c.timer.start();
    c.lastPacketMs = QDateTime::currentMSecsSinceEpoch();
    active_[p.slot] = c;
    recBuf_[QString("DMR%1").arg(p.slot)].clear();
    decoder_.reset();
    tick();
}

void MainWindow::endCall(int slot, const QString &why)
{
    if (!active_[slot]) return;
    const Call c = *active_[slot];
    active_[slot].reset();
    releaseAudio(QString("DMR%1").arg(slot));
    decoder_.reset();

    const double secs = c.timer.elapsed() / 1000.0;
    const double ber = c.frames ? 100.0 * c.errors / (c.frames * 72.0) : 0.0;
    const QString name = ids_.value(c.src).second;
    const QString rec = saveRecording(QString("DMR%1").arg(slot), c.started, "DMR", callsign(c.src));

    if (log_) {
        const QStringList cells = {
            c.started.toString("HH:mm:ss"), "DMR", c.test ? QString("Test") : QString("TS%1").arg(c.slot), callsign(c.src), name,
            target(c), QString("%1 s").arg(secs, 0, 'f', 1), QString("%1%").arg(ber, 0, 'f', 1)};
        QString tip;
        if (c.lateEntry || why == "timeout" || why == "link lost")
            tip = QString("%1%2").arg(c.lateEntry ? "late entry; " : "", why);
        addLogRow(cells, tip, rec);
    }
    if (logFile_ && logFile_->isOpen() && !c.test) {
        QTextStream ts(logFile_);
        ts << c.started.toString(Qt::ISODate) << ',' << c.slot << ',' << c.src << ','
           << callsign(c.src) << ',' << QString(name).replace(',', ' ') << ',' << target(c) << ','
           << QString::number(secs, 'f', 1) << ',' << QString::number(ber, 'f', 1) << '\n';
        ts.flush();
    }
    if (!quitting_) tick();
}

void MainWindow::addLogRow(const QStringList &cells, const QString &tooltip, const QString &recording)
{
    log_->insertRow(0);
    for (int i = 0; i < cells.size(); ++i) {
        auto *it = new QTableWidgetItem(cells[i]);
        if (i == 3) { QFont f = it->font(); f.setBold(true); it->setFont(f); }
        if (!tooltip.isEmpty()) it->setToolTip(tooltip);
        log_->setItem(0, i, it);
    }
    auto *play = new QTableWidgetItem;
    play->setData(Qt::UserRole, recording);
    log_->setItem(0, kPlayCol, play);
    if (log_->rowCount() > 1000) log_->removeRow(log_->rowCount() - 1);
    refreshPlayCells();
}

// ---------------------------------------------------------------- recordings
QString MainWindow::saveRecording(const QString &key, const QDateTime &started, const QString &mode, const QString &who)
{
    std::vector<float> buf;
    buf.swap(recBuf_[key]);
    if (buf.size() < size_t(cfg_.recording.minSeconds * 8000) || recDir_.isEmpty() || cfg_.recording.keepCalls <= 0) return {};

    // Same AGC as live listening, at 100% volume; the volume slider applies on replay.
    Agc agc;
    std::vector<int16_t> pcm, chunk;
    pcm.reserve(buf.size());
    double rms = 0;
    for (size_t i = 0; i < buf.size(); i += 160) {
        const int n = int(std::min<size_t>(160, buf.size() - i));
        agc.process(buf.data() + i, n, 1.0, chunk, rms);
        pcm.insert(pcm.end(), chunk.begin(), chunk.end());
    }

    QDir().mkpath(recDir_);
    QString safe = who.isEmpty() ? QString("unknown") : who;
    safe.replace(QRegularExpression("[^A-Za-z0-9-]+"), "_");
    const QString path = QString("%1/%2_%3_%4.wav").arg(recDir_, started.toString("yyyyMMdd-HHmmss-zzz"), mode, safe);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return {};
    auto le32 = [](quint32 v) { return QByteArray(reinterpret_cast<const char *>(&v), 4); };   // x86/ARM are LE
    auto le16 = [](quint16 v) { return QByteArray(reinterpret_cast<const char *>(&v), 2); };
    const quint32 dataBytes = quint32(pcm.size() * 2);
    QByteArray hdr;
    hdr += "RIFF" + le32(36 + dataBytes) + "WAVE";
    hdr += "fmt " + le32(16) + le16(1) + le16(1) + le32(8000) + le32(16000) + le16(2) + le16(16);
    hdr += "data" + le32(dataBytes);
    f.write(hdr);
    f.write(reinterpret_cast<const char *>(pcm.data()), dataBytes);
    f.close();
    pruneRecordings();
    return path;
}

void MainWindow::pruneRecordings()
{
    // File names start with the call's start time, so name order is age order.
    QDir d(recDir_);
    const QStringList files = d.entryList({"*.wav"}, QDir::Files, QDir::Name);
    for (int i = 0; i + cfg_.recording.keepCalls < files.size(); ++i)
        if (d.filePath(files[i]) != playingPath_) d.remove(files[i]);
}

void MainWindow::refreshPlayCells()
{
    for (int r = 0; r < log_->rowCount(); ++r) {
        QTableWidgetItem *it = log_->item(r, kPlayCol);
        if (!it) continue;
        const QString path = it->data(Qt::UserRole).toString();
        if (path.isEmpty()) { it->setText(""); it->setToolTip(""); continue; }
        if (!QFile::exists(path)) {
            it->setData(Qt::UserRole, QString());
            it->setText("");
            it->setToolTip("");
            continue;
        }
        const bool playing = path == playingPath_;
        it->setText(playing ? "■ Stop" : "▶ Play");
        it->setForeground(QColor(playing ? "#d93025" : "#1a73e8"));
        it->setToolTip(playing ? "Stop replay" : "Replay this call\n" + path);
    }
}

void MainWindow::stopRecordingPlayback()
{
    if (replayTimer_) {
        replayTimer_->stop();
        replayTimer_->deleteLater();
        replayTimer_ = nullptr;
    }
    if (replayOut_) replayOut_->flush();
    playingPath_.clear();
    pruneRecordings();   // in case a file was kept only because it was playing
    refreshPlayCells();
}

void MainWindow::toggleRecordingPlayback(int row)
{
    QTableWidgetItem *it = log_->item(row, kPlayCol);
    const QString path = it ? it->data(Qt::UserRole).toString() : QString();
    if (path.isEmpty()) return;
    const bool wasPlaying = path == playingPath_;
    stopRecordingPlayback();
    if (wasPlaying) return;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { refreshPlayCells(); return; }
    QByteArray pcm = f.readAll().mid(44);   // our own files: fixed 44-byte header
    const double vol = mute_->isChecked() ? 0.0 : volume_->value() / 100.0;
    auto *s = reinterpret_cast<int16_t *>(pcm.data());
    const qsizetype count = pcm.size() / 2;
    std::vector<float> gated(s, s + count);
    if (cfg_.audio.noiseGate) {   // recordings are stored unprocessed by the gate; apply it on replay
        NoiseGate g;
        g.process(gated.data(), int(count), {cfg_.audio.gateThresholdDb, cfg_.audio.gateReductionDb, cfg_.audio.gateHoldMs});
    }
    for (qsizetype i = 0; i < count; ++i)
        s[i] = int16_t(std::clamp(gated[size_t(i)] * vol, -32767.0, 32767.0));

    if (!ensurePlayer(replayOut_, "Recording")) return;
    // Drop any live audio still queued so the replay isn't mixed with it (live audio is held while replaying).
    if (liveOut_) liveOut_->flush();

    playingPath_ = path;
    replayOut_->write(reinterpret_cast<const int16_t *>(pcm.constData()), size_t(pcm.size() / 2));
    replayTimer_ = new QTimer(this);
    replayTimer_->setSingleShot(true);
    connect(replayTimer_, &QTimer::timeout, this, [this] {
        replayTimer_->deleteLater();
        replayTimer_ = nullptr;
        playingPath_.clear();
        pruneRecordings();
        refreshPlayCells();
    });
    replayTimer_->start(int(pcm.size() / 2 / 8) + 400);   // length in ms + the device buffer
    refreshPlayCells();
}

// ---------------------------------------------------------------- D-Star
QString MainWindow::dstarName(const QString &call) const
{
    return nameByCall_.value(call.section(' ', 0, 0).toUpper());
}

QString MainWindow::dstarCaller(const DStarCall &c)
{
    if (c.my.isEmpty()) return "(unknown)";
    return c.suffix.isEmpty() ? c.my : c.my + "/" + c.suffix;
}

QString MainWindow::dstarTarget(const DStarCall &c)
{
    QString t = c.your.isEmpty() ? QString("CQCQCQ") : c.your;
    const QString via = !c.reflector.isEmpty() ? c.reflector : c.rpt2;
    if (!via.isEmpty()) t += QString("  via %1").arg(QString(via).simplified());
    return t;
}

void MainWindow::handleDStar(const QByteArray &rec)
{
    DStarPacket p;
    if (!parseDstr(rec, p)) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto it = dstars_.find(p.id);

    if (p.isHeader) {
        if (it == dstars_.end()) {
            beginDStar(p, false).lastPacketMs = now;
        } else {
            DStarCall &c = it->second;
            c.lastPacketMs = now;
            // The header is repeated every superframe: fill in a late entry.
            if (c.my.isEmpty() && !p.my.isEmpty()) {
                c.my = p.my; c.suffix = p.mySuffix; c.your = p.your; c.rpt1 = p.rpt1; c.rpt2 = p.rpt2;
                c.reflector = c.your == "CQCQCQ" ? reflector_ : QString();
            }
        }
        return;
    }
    DStarCall &c = it != dstars_.end() ? it->second : beginDStar(p, true);   // missed the header
    c.lastPacketMs = now;
    c.text.feed(p.seq, p.slowData);

    float samples[160];
    const int errs = c.decoder.decodeFrame(p.ambe, samples);
    c.frames += 1;
    c.errors += errs;

    playDecoded(dstarKey(p.id), samples, 160);

    if (p.isEnd) endDStar(p.id, "end");
}

MainWindow::DStarCall &MainWindow::beginDStar(const DStarPacket &p, bool lateEntry)
{
    DStarCall c;
    c.id = p.id;
    c.fromRf = p.fromRf;
    c.lateEntry = lateEntry;
    c.test = testFeeding_;
    if (!lateEntry) {
        c.my = p.my; c.suffix = p.mySuffix; c.your = p.your; c.rpt1 = p.rpt1; c.rpt2 = p.rpt2;
    }
    if (lateEntry || c.your == "CQCQCQ") c.reflector = reflector_;
    c.started = QDateTime::currentDateTime();
    c.timer.start();
    c.lastPacketMs = QDateTime::currentMSecsSinceEpoch();
    c.text.reset();
    c.decoder.reset();
    recBuf_[dstarKey(p.id)].clear();
    DStarCall &ref = dstars_[p.id] = c;
    tick();
    return ref;
}

void MainWindow::endAllDStar(const QString &why)
{
    while (!dstars_.empty()) endDStar(dstars_.begin()->first, why);
}

void MainWindow::endDStar(uint16_t id, const QString &why)
{
    auto it = dstars_.find(id);
    if (it == dstars_.end()) return;
    const DStarCall c = it->second;
    dstars_.erase(it);
    releaseAudio(dstarKey(id));

    const double secs = c.timer.elapsed() / 1000.0;
    const double ber = c.frames ? 100.0 * c.errors / (c.frames * 72.0) : 0.0;
    const QString name = dstarName(c.my);
    const QString msg = c.text.text();
    QString nameCell = name;
    if (!msg.isEmpty()) nameCell += (name.isEmpty() ? "" : "   ") + QString("“%1”").arg(msg);
    const QString rec = saveRecording(dstarKey(id), c.started, "DSTAR", dstarCaller(c));
    recBuf_.remove(dstarKey(id));
    if (log_) {
        const QStringList cells = {
            c.started.toString("HH:mm:ss"), "D-Star", c.test ? "Test" : c.fromRf ? "RF" : "Net", dstarCaller(c), nameCell,
            dstarTarget(c), QString("%1 s").arg(secs, 0, 'f', 1), QString("%1%").arg(ber, 0, 'f', 1)};
        QString tip = QString("RPT1 %1 · RPT2 %2 · UR %3").arg(c.rpt1, c.rpt2, c.your);
        if (c.lateEntry) tip += " · late entry";
        if (why == "timeout" || why == "link lost") tip += " · " + why;
        addLogRow(cells, tip, rec);
    }
    if (dstarFile_ && dstarFile_->isOpen() && !c.test) {
        auto q = [](QString s) { return "\"" + s.replace('"', "\"\"") + "\""; };
        QTextStream ts(dstarFile_);
        ts << c.started.toString(Qt::ISODate) << ',' << (c.fromRf ? "RF" : "Net") << ',' << q(c.my) << ','
           << q(c.suffix) << ',' << q(c.your) << ',' << q(c.rpt1) << ',' << q(c.rpt2) << ',' << q(msg) << ','
           << QString::number(secs, 'f', 1) << ',' << QString::number(ber, 'f', 1) << ',' << q(c.reflector) << '\n';
        ts.flush();
    }
    if (!quitting_) tick();
}

void MainWindow::handleLinks(const QByteArray &linksLog)
{
    // e.g. "2026-11-13 20:49:33: DPlus link - Type: Dongle Rptr: N0CALL B Refl: REF001 C Dir: Outgoing"
    static const QRegularExpression re(R"re((\w+) link - Type: \S+ Rptr: (.{8}) Refl: (.{8}) Dir: (\w+))re");
    QString refl, proto;
    for (const QByteArray &line : linksLog.split('\n')) {
        const auto m = re.match(QString::fromLatin1(line));
        if (m.hasMatch()) { proto = m.captured(1); refl = m.captured(3).simplified(); break; }
    }
    reflector_ = refl;
    // Shown in the endpoints row (the ENDP record carries it too; this keeps it instant).
    endpoints_["D-Star"] = proto.isEmpty() ? refl : QString("%1 (%2)").arg(refl, proto);
    updateEndpointsLabel();
    for (auto &[id, d] : dstars_)
        if (d.your == "CQCQCQ" || d.my.isEmpty()) d.reflector = reflector_;
}

// ---------------------------------------------------------------- shared audio
bool MainWindow::claimAudio(const QString &key)
{
    if (audioOwner_.isEmpty()) {
        audioOwner_ = key;
        gate_.reset();   // learn the new station's background afresh
    }
    return audioOwner_ == key;
}

void MainWindow::releaseAudio(const QString &key)
{
    if (audioOwner_ == key) audioOwner_.clear();
}

void MainWindow::playDecoded(const QString &key, const float *samples, int n)
{
    // Only one call is played at a time; overlapping calls on other modes or
    // slots are still logged (and recorded).
    std::vector<float> &rec = recBuf_[key];
    if (rec.size() < size_t(cfg_.recording.maxMinutes) * 60 * 8000) rec.insert(rec.end(), samples, samples + n);
    if (!claimAudio(key)) return;
    const double vol = mute_->isChecked() ? 0.0 : volume_->value() / 100.0;
    std::vector<int16_t> pcm;
    double rms = 0;
    agc_.process(samples, n, vol, pcm, rms);
    if (cfg_.audio.noiseGate) {
        // After the level control: gating before it would just make the AGC turn the gain back up
        // in the pauses and undo the reduction.
        std::vector<float> f(pcm.begin(), pcm.end());
        gate_.process(f.data(), n, {cfg_.audio.gateThresholdDb, cfg_.audio.gateReductionDb, cfg_.audio.gateHoldMs});
        for (int i = 0; i < n; ++i) pcm[size_t(i)] = int16_t(f[size_t(i)]);
    }
    level_ = std::max(level_, std::min(100.0, 100.0 * rms / 12000.0));
    if (!playingPath_.isEmpty()) return;   // a recording is being replayed: hold live audio
    if (txActive_ || QDateTime::currentMSecsSinceEpoch() < txMuteUntilMs_) return;   // our own transmission
    playPcm(pcm);
}

// ---------------------------------------------------------------- YSF / P25
QString MainWindow::genName(const GenCall &c) const
{
    if (c.mode == "P25" && c.srcId) return ids_.value(c.srcId).second;
    if (c.mode == "NXDN" && c.srcId) {
        const QString n = nxdnIds_.value(c.srcId).second;
        return n.isEmpty() ? dstarName(c.callsign) : n;   // fall back to callsign -> name from DMRIds
    }
    return dstarName(c.callsign);   // callsign -> name from DMRIds.dat
}

void MainWindow::beginGen(std::optional<GenCall> &slot, const QString &mode, bool fromRf, bool lateEntry)
{
    endGen(slot, "new call");
    GenCall c;
    c.mode = mode;
    c.fromRf = fromRf;
    c.lateEntry = lateEntry;
    c.test = testFeeding_;
    c.started = QDateTime::currentDateTime();
    c.timer.start();
    c.lastPacketMs = QDateTime::currentMSecsSinceEpoch();
    slot = c;
    recBuf_[mode].clear();
    if (mode == "YSF") ysfDecoder_.reset();
    else if (mode == "P25") p25Decoder_.reset();
    else if (mode == "NXDN") nxdnDecoder_.reset();
    else if (mode == "M17") m17Decoder_.reset();
    tick();
}

void MainWindow::endGen(std::optional<GenCall> &slot, const QString &why)
{
    if (!slot) return;
    const GenCall c = *slot;
    slot.reset();
    releaseAudio(c.mode);
    const QString rec = saveRecording(c.mode, c.started, c.mode, c.callsign);
    if (c.frames == 0 && why != "end") { if (!quitting_) tick(); return; }   // header-only blips

    const double secs = c.timer.elapsed() / 1000.0;
    const QString ber = c.errorBits ? QString("%1%").arg(100.0 * c.errors / c.errorBits, 0, 'f', 1) : QString("—");
    const QString name = genName(c);
    if (log_) {
        const QStringList cells = {
            c.started.toString("HH:mm:ss"), c.mode, c.test ? "Test" : c.fromRf ? "RF" : "Net",
            c.callsign.isEmpty() ? "(unknown)" : c.callsign, name, c.target,
            QString("%1 s").arg(secs, 0, 'f', 1), ber};
        QString tip = c.note;
        if (c.lateEntry) tip += (tip.isEmpty() ? "" : " · ") + QString("late entry");
        if (why == "timeout" || why == "link lost") tip += (tip.isEmpty() ? "" : " · ") + why;
        addLogRow(cells, tip, rec);
    }
    if (genFile_ && genFile_->isOpen() && !c.test) {
        auto q = [](QString s) { return "\"" + s.replace('"', "\"\"") + "\""; };
        QTextStream ts(genFile_);
        ts << c.started.toString(Qt::ISODate) << ',' << c.mode << ',' << (c.fromRf ? "RF" : "Net") << ','
           << q(c.callsign) << ',' << q(name) << ',' << q(c.target) << ',' << QString::number(secs, 'f', 1) << ','
           << (c.errorBits ? QString::number(100.0 * c.errors / c.errorBits, 'f', 1) : QString()) << ','
           << q(c.note) << '\n';
        ts.flush();
    }
    if (!quitting_) tick();
}

void MainWindow::handleYsf(const QByteArray &rec)
{
    YsfPacket p;
    if (!parseYsf(rec, p)) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    const bool header = p.fichOk && p.fi == 0;
    const bool terminator = p.end || (p.fichOk && p.fi == 2);
    if (header || !ysf_ || (ysf_->callsign != p.src && !p.src.isEmpty()))
        beginGen(ysf_, "YSF", p.fromRf, !header);
    GenCall &c = *ysf_;
    c.lastPacketMs = now;
    if (!p.src.isEmpty()) c.callsign = p.src;
    if (!p.dst.isEmpty()) c.target = p.dst;
    if (p.fichOk && p.dgid) c.target = QString("%1 · DG-ID %2").arg(c.target.isEmpty() ? "ALL" : c.target).arg(p.dgid);

    if (p.fichOk && p.fi == 1) {
        if (p.dt == 2) {
            float samples[800];
            c.errors += ysfDecoder_.decodeVD2(p.frame, samples);
            c.errorBits += 5 * 27;
            c.frames += 5;
            playDecoded("YSF", samples, 800);
        } else if (p.dt == 1) {
            c.audio = false;
            c.note = "data transmission (no voice)";
        } else if (p.dt == 3 || p.dt == 0) {
            c.frames += 5;
            c.audio = false;
            c.note = p.dt == 3 ? "Voice Wide (VW) mode: no audio decoder" : "V/D mode 1: no audio decoder";
        }
    }
    if (terminator) endGen(ysf_, "end");
}

void MainWindow::handleP25(const QByteArray &rec)
{
    if (rec.size() < 2) return;
    const bool fromRf = rec[0] == 'R';
    const QByteArray d = rec.mid(1);
    const uint8_t type = uint8_t(d[0]);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (type == 0x80) { endGen(p25_, "end"); return; }

    int off = -1;
    switch (type) {
    case 0x62: case 0x6B: off = 10; break;
    case 0x63: case 0x6C: off = 1; break;
    case 0x6A: case 0x73: off = 4; break;
    default:
        if ((type >= 0x64 && type <= 0x69) || (type >= 0x6D && type <= 0x72)) off = 5;
    }
    if (off < 0 || d.size() < off + 11) return;

    auto id24 = [&d] { return (uint32_t(uint8_t(d[1])) << 16) | (uint32_t(uint8_t(d[2])) << 8) | uint8_t(d[3]); };
    // A new source ID without an end record in between means a new call.
    if (type == 0x66 && p25_ && p25_->srcId && p25_->srcId != id24()) endGen(p25_, "new call");
    if (!p25_) beginGen(p25_, "P25", fromRf, type != 0x62);
    GenCall &c = *p25_;
    c.lastPacketMs = now;
    if (type == 0x65) { c.dstId = id24(); c.target = QString("TG %1").arg(c.dstId); }
    if (type == 0x66) { c.srcId = id24(); c.callsign = callsign(c.srcId); }

    float samples[160];
    p25Decoder_.decodeImbe(d.constData() + off, samples);
    c.frames += 1;
    playDecoded("P25", samples, 160);
}

QString MainWindow::genColour(const QString &mode)
{
    if (mode == "YSF") return "#8430ce";
    if (mode == "NXDN") return "#00796b";
    if (mode == "M17") return "#e8710a";
    return "#c5221f";   // P25
}

void MainWindow::loadNxdnIds()
{
    if (nxdnIdsProc_) {   // a reload while the previous one is still running
        nxdnIdsProc_->disconnect(this);
        nxdnIdsProc_->kill();
        nxdnIdsProc_->deleteLater();
        nxdnIdsProc_ = nullptr;
    }
    if (cfg_.station.nxdnId) nxdnIds_.insert(cfg_.station.nxdnId, {cfg_.station.callsign, QString()});   // until NXDN.csv arrives
    nxdnIdsProc_ = new QProcess(this);
    connect(nxdnIdsProc_, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        // RADIO_ID,CALLSIGN,FIRST_NAME,LAST_NAME,...
        for (const QByteArray &line : nxdnIdsProc_->readAllStandardOutput().split('\n')) {
            const QList<QByteArray> f = line.split(',');
            bool ok = false;
            const uint32_t id = f.value(0).toUInt(&ok);
            if (ok && f.size() >= 2)
                nxdnIds_.insert(id, {QString::fromUtf8(f[1]).trimmed(), QString::fromUtf8(f.value(2)).trimmed()});
        }
        // Own NXDN ID, missing from NXDN.csv
        if (cfg_.station.nxdnId && !nxdnIds_.contains(cfg_.station.nxdnId)) nxdnIds_.insert(cfg_.station.nxdnId, {cfg_.station.callsign, QString()});
        nxdnIdsProc_->deleteLater();
        nxdnIdsProc_ = nullptr;
    });
    nxdnIdsProc_->start("ssh", sshArgs()
                                             << "cut -d, -f1-3 /usr/local/etc/NXDN.csv");
}

QString MainWindow::nxdnCall(uint32_t id) const
{
    auto it = nxdnIds_.constFind(id);
    return it != nxdnIds_.constEnd() && !it->first.isEmpty() ? it->first : QString::number(id);
}

void MainWindow::handleNxdn(const QByteArray &rec)
{
    NxdnPacket p;
    if (!parseNxdn(rec, p)) return;
    const auto *d = reinterpret_cast<const uint8_t *>(p.data.constData());
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Layer 3 VCALL: type(1) ...(1) flags(1) src(2) dst(2)
    auto applyVcall = [this](GenCall &c, const uint8_t *l3) {
        c.srcId = (l3[3] << 8) | l3[4];
        c.dstId = (l3[5] << 8) | l3[6];
        c.callsign = nxdnCall(c.srcId);
        c.target = (l3[2] & 0x80) ? QString("Private %1").arg(nxdnCall(c.dstId)) : QString("TG %1").arg(c.dstId);
    };

    if (p.usc == 0) {                       // SACCH_NS: header / trailer carried in FACCH1
        const uint8_t type = d[5] & 0x3F;
        if (type == 0x01) {                 // VCALL
            const uint32_t src = (d[8] << 8) | d[9];
            if (!nxdn_ || (nxdn_->srcId && nxdn_->srcId != src)) beginGen(nxdn_, "NXDN", p.fromRf, false);
            nxdn_->lastPacketMs = now;
            applyVcall(*nxdn_, d + 5);
        } else if (type == 0x08) {          // TX_REL
            endGen(nxdn_, "end");
        }
        return;
    }
    if (!nxdn_) beginGen(nxdn_, "NXDN", p.fromRf, true);
    GenCall &c = *nxdn_;
    c.lastPacketMs = now;

    if (p.usc == 1) {                       // UDCH: data call
        c.audio = false;
        c.note = "data transmission (no voice)";
        c.frames += 1;
        return;
    }

    if (!c.srcId) {
        // Late entry: the VCALL is repeated across the 4 SACCHs of each superframe (18 bits each).
        static const int partOffset[4] = {54, 36, 18, 0};   // structure 0 = 4/4 ... 3 = 1/4
        const int structure = d[1] >> 6;
        for (int i = 0; i < 18; ++i) {
            const int dst = partOffset[structure] + i;
            const bool b = (d[2 + (i >> 3)] >> (7 - (i & 7))) & 1;   // SACCH layer 3 bits start at byte 2
            if (b) c.l3[dst >> 3] |= uint8_t(0x80 >> (dst & 7));
            else   c.l3[dst >> 3] &= uint8_t(~(0x80 >> (dst & 7)));
        }
        if (structure == 3) c.l3Mask = 1; else if (c.l3Mask) c.l3Mask |= 1 << (3 - structure);
        if (c.l3Mask == 0x0F && (c.l3[0] & 0x3F) == 0x01) applyVcall(c, c.l3);
    }

    float samples[640];
    int n = 0;
    if (p.option == 3 || p.option == 2) { nxdnDecoder_.decodeHalf(d + 5, samples + n); n += 320; }
    if (p.option == 3 || p.option == 1) { nxdnDecoder_.decodeHalf(d + 19, samples + n); n += 320; }
    if (n) {
        c.frames += n / 160;
        playDecoded("NXDN", samples, n);
    }
}

void MainWindow::handleM17(const QByteArray &rec)
{
    M17Packet p;
    if (!parseM17(rec, p)) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!m17_ || m17_->streamId != p.sid) {
        beginGen(m17_, "M17", p.fromRf, false);
        m17_->streamId = p.sid;
    }
    GenCall &c = *m17_;
    c.lastPacketMs = now;
    c.callsign = p.src;
    c.target = p.dst;
    c.frames += 2;
    if (p.encType != 0) {
        c.audio = false;
        c.note = "encrypted";
    } else if (p.dataType == 1) {
        c.audio = false;
        c.note = "data transmission (no voice)";
    } else if (!M17Decoder::available()) {
        c.audio = false;
        c.note = "no Codec2 decoder installed";
    } else {
        float samples[320];
        const int n = m17Decoder_.decode(p, samples);
        if (n) playDecoded("M17", samples, n);
    }
    if (p.end) endGen(m17_, "end");
}

// ---------------------------------------------------------------- pager
void MainWindow::handlePage(const QByteArray &logLine)
{
    // DAPNETGateway log line, e.g.
    // M: 2026-11-13 20:00:38.443 Sending message in slot 0 to 1234567, type 6, func Alphanumeric: "text"
    static const QRegularExpression re(
        R"re(^\w: (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\.\d+ Sending message in slot \d+ to (\d+), type \d+, func (\w+): "(.*)"\s*$)re");
    if (seenPages_.contains(logLine)) return;   // history is re-sent on every reconnect
    seenPages_.insert(logLine);

    const auto m = re.match(QString::fromUtf8(logLine));
    if (!m.hasMatch()) return;
    QDateTime when = QDateTime::fromString(m.captured(1), "yyyy-MM-dd HH:mm:ss");
    when.setTimeZone(QTimeZone::utc());         // Pi-Star logs in UTC
    when = when.toLocalTime();
    const uint32_t ric = m.captured(2).toUInt();
    const QString func = m.captured(3);
    const QString text = m.captured(4);
    const bool mine = myRics_.contains(ric);

    pages_->insertRow(0);
    const QStringList cells = {
        when.date() == QDate::currentDate() ? when.toString("HH:mm:ss") : when.toString("dd MMM HH:mm"),
        QString::number(ric), func.startsWith("Alpha") ? "Text" : func, text};
    for (int i = 0; i < cells.size(); ++i) {
        auto *it = new QTableWidgetItem(cells[i]);
        it->setData(Qt::UserRole, ric);
        if (mine) { QFont f = it->font(); f.setBold(true); it->setFont(f); }
        if (i == 3) it->setToolTip(text);
        pages_->setItem(0, i, it);
    }
    pages_->setRowHidden(0, onlyMine_->isChecked() && !mine);
    if (pages_->rowCount() > 2000) pages_->removeRow(pages_->rowCount() - 1);

    // Saved log, desktop alert and banner are only for real new pages, not history.
    const bool isNew = pagesLive_;
    if (isNew && pageFile_ && pageFile_->isOpen()) {
        QTextStream ts(pageFile_);
        ts << when.toString(Qt::ISODate) << ',' << ric << ',' << func << ",\""
           << QString(text).replace('"', "\"\"") << "\"\n";
        ts.flush();
    }
    if (!mine) return;
    showPageBanner(when, ric, text);
    if (!isNew) return;
    if (tabs_->currentIndex() != 1) { ++unreadPages_; updatePagerTabTitle(); }
    QProcess::startDetached("notify-send", {"-a", "Pi-Star DMR Monitor", "-i", "mail-message-new",
                                            QString("Page to %1").arg(ric), text});
    QApplication::alert(this);
}

void MainWindow::showPageBanner(const QDateTime &when, uint32_t ric, const QString &text)
{
    pageBannerText_->setText(QString("<b>📟 Page to %1</b> &nbsp;·&nbsp; %2<br>%3")
                                 .arg(ric)
                                 .arg(when.date() == QDate::currentDate() ? when.toString("HH:mm")
                                                                          : when.toString("dd MMM HH:mm"))
                                 .arg(text.toHtmlEscaped()));
    pageBanner_->show();
}

void MainWindow::applyPagerFilter()
{
    for (int r = 0; r < pages_->rowCount(); ++r) {
        const uint32_t ric = pages_->item(r, 0)->data(Qt::UserRole).toUInt();
        const bool mine = myRics_.contains(ric);
        for (int c = 0; c < pages_->columnCount(); ++c) {
            QFont f = pages_->item(r, c)->font(); f.setBold(mine); pages_->item(r, c)->setFont(f);
        }
        pages_->setRowHidden(r, onlyMine_->isChecked() && !mine);
    }
}

void MainWindow::updatePagerTabTitle()
{
    tabs_->setTabText(1, unreadPages_ ? QString("Pager (%1)").arg(unreadPages_) : "Pager");
}

// ---------------------------------------------------------------- audio
bool MainWindow::ensurePlayer(std::unique_ptr<AudioOut> &out, const char *name)
{
    if (out && out->isOpen()) return true;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now < audioRetryMs_) return false;
    if (!out) out = std::make_unique<AudioOut>(QString::fromLatin1(name));
    QString err;
    if (!out->open(cfg_.audio.backend, cfg_.audio.device, &err)) {
        audioRetryMs_ = now + 5000;   // don't hammer a missing device on every packet
        setStatus("No audio output: " + err, "#d93025");
        return false;
    }
    return true;
}

void MainWindow::playPcm(const std::vector<int16_t> &pcm)
{
    if (ensurePlayer(liveOut_, "Live")) liveOut_->write(pcm.data(), pcm.size());
}

// ---------------------------------------------------------------- display
QString MainWindow::callsign(uint32_t id) const
{
    if (id == cfg_.testCall.dmrEchoId) return "Parrot";
    auto it = ids_.constFind(id);
    return it != ids_.constEnd() && !it->first.isEmpty() ? it->first : QString::number(id);
}

QString MainWindow::describe(uint32_t id) const
{
    auto it = ids_.constFind(id);
    if (it == ids_.constEnd()) return QString::number(id);
    return it->second.isEmpty() ? QString("%1 (%2)").arg(it->first).arg(id)
                                : QString("%1 (%2) · %3").arg(it->first).arg(id).arg(it->second);
}

QString MainWindow::target(const Call &c) const
{
    if (!c.privateCall) return QString("TG %1").arg(c.dst);
    if (c.dst == cfg_.testCall.dmrEchoId) return QString("Private %1 (Parrot)").arg(c.dst);
    return QString("Private %1").arg(callsign(c.dst));
}

void MainWindow::tick()
{
    // Only touch widgets when something changed: re-applying a stylesheet re-polishes the
    // widget tree and makes the window redraw (and its frame flicker) ten times a second.
    auto setPanelStyle = [this](const QString &css) {
        if (css == panelStyle_) return;
        panelStyle_ = css;
        callPanel_->setStyleSheet(css);
    };
    auto setLabel = [](auto *l, const QString &text) {   // ElideLabel or QLabel: calls its own setText
        if (l->text() != text) l->setText(text);
    };
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // end calls whose packets stopped without a terminator
    for (int s = 1; s <= 2; ++s)
        if (active_[s] && now - active_[s]->lastPacketMs > cfg_.calls.timeoutMs)
            endCall(s, "timeout");
    for (auto it = dstars_.begin(); it != dstars_.end();) {
        const uint16_t id = it->first;
        const bool stale = now - it->second.lastPacketMs > cfg_.calls.timeoutMs;
        ++it;
        if (stale) endDStar(id, "timeout");
    }
    for (auto *o : {&ysf_, &p25_, &nxdn_, &m17_})
        if (*o && now - (*o)->lastPacketMs > cfg_.calls.timeoutMs)
            endGen(*o, "timeout");

    // force a reconnect if the link has gone silent (heartbeat is every 2 s)
    if (ssh_ && ssh_->state() == QProcess::Running &&
        now - ssh_->property("lastRx").toLongLong() > cfg_.connection.linkTimeoutMs)
        ssh_->kill();

    // Show the call that has the speaker; otherwise the most recent one.
    const Call *c = nullptr;
    const GenCall *g = nullptr;
    const DStarCall *ds = nullptr;
    if (audioOwner_ == "DMR1" || audioOwner_ == "DMR2") {
        const int s = audioOwner_.right(1).toInt();
        if (active_[s]) c = &*active_[s];
    } else if (audioOwner_.startsWith("DSTAR")) {
        auto it = dstars_.find(uint16_t(audioOwner_.mid(5).toUInt()));
        if (it != dstars_.end()) ds = &it->second;
    } else if (audioOwner_ == "YSF" || audioOwner_ == "P25" || audioOwner_ == "NXDN" || audioOwner_ == "M17") {
        for (const auto *o : {&ysf_, &p25_, &nxdn_, &m17_})
            if (*o && (*o)->mode == audioOwner_) g = &**o;
    } else {
        QDateTime newest;
        for (int s = 1; s <= 2; ++s)
            if (active_[s] && (!c || active_[s]->started > newest)) { c = &*active_[s]; newest = c->started; }
        for (const auto &[id, d] : dstars_)
            if (newest.isNull() || d.started > newest) { c = nullptr; ds = &d; newest = d.started; }
        for (const auto *o : {&ysf_, &p25_, &nxdn_, &m17_})
            if (*o && (newest.isNull() || (*o)->started > newest)) { c = nullptr; ds = nullptr; g = &**o; newest = g->started; }
    }

    if (g) {
        setPanelStyle(QString("#callPanel { background: %1; border-radius: 8px; } "
                                          "#callPanel QLabel { color: white; }").arg(genColour(g->mode)));
        setLabel(callTitle_, g->callsign.isEmpty() ? "(unknown)" : g->callsign);
        const QString name = genName(*g);
        setLabel(callSub_, QString("%1→  %2   ·   %3 %4%5%6")
                              .arg(name.isEmpty() ? "" : name + "   ")
                              .arg(g->target.isEmpty() ? "?" : g->target)
                              .arg(g->mode).arg(g->test ? "Test" : g->fromRf ? "RF" : "Net")
                              .arg(g->lateEntry ? "   · late entry" : "")
                              .arg(g->note.isEmpty() ? "" : "   · " + g->note));
        const double secs = g->timer.elapsed() / 1000.0;
        setLabel(callTime_, QString("%1:%2").arg(int(secs) / 60).arg(secs - 60 * (int(secs) / 60), 4, 'f', 1, '0'));
    } else if (ds) {
        const DStarCall &d = *ds;
        setPanelStyle("#callPanel { background: #1967d2; border-radius: 8px; } "
                                  "#callPanel QLabel { color: white; }");
        setLabel(callTitle_, dstarCaller(d));
        const QString name = dstarName(d.my);
        const QString msg = d.text.text();
        setLabel(callSub_, QString("%1%2→  %3   ·   D-Star %4%5")
                              .arg(name.isEmpty() ? "" : name + "   ")
                              .arg(msg.isEmpty() ? "" : QString("“%1”   ").arg(msg))
                              .arg(dstarTarget(d))
                              .arg(d.test ? "Test" : d.fromRf ? "RF" : "Net")
                              .arg(d.lateEntry ? "   · late entry" : ""));
        const double secs = d.timer.elapsed() / 1000.0;
        setLabel(callTime_, QString("%1:%2").arg(int(secs) / 60).arg(secs - 60 * (int(secs) / 60), 4, 'f', 1, '0'));
    } else if (c) {
        setPanelStyle("#callPanel { background: #1e8e3e; border-radius: 8px; } "
                                  "#callPanel QLabel { color: white; }");
        setLabel(callTitle_, callsign(c->src));
        const QString name = ids_.value(c->src).second;
        setLabel(callSub_, QString("%1%2  →  %3   ·   TS%4%5")
                              .arg(name.isEmpty() ? "" : name + "   ")
                              .arg(QString("(%1)").arg(c->src))
                              .arg(target(*c)).arg(c->slot)
                              .arg(c->test ? "   · Test" : c->lateEntry ? "   · late entry" : ""));
        const double secs = c->timer.elapsed() / 1000.0;
        setLabel(callTime_, QString("%1:%2").arg(int(secs) / 60).arg(secs - 60 * (int(secs) / 60), 4, 'f', 1, '0'));
    } else {
        setPanelStyle("#callPanel { background: palette(alternate-base); border: 1px solid palette(mid); border-radius: 8px; } "
                                  "#callPanel QLabel { color: palette(window-text); }");
        setLabel(callTitle_, "No active call");
        QStringList voice;
        for (const QString &m : {"DMR", "D-Star", "YSF", "P25", "NXDN", "M17"})
            if (modeAllowed(m)) voice << m;
        setLabel(callSub_, voice.isEmpty() ? QString("No voice modes are enabled in Pi-Star")
                          : voice.size() == 1 ? QString("Waiting for %1 traffic…").arg(voice.first())
                          : QString("Waiting for %1 or %2 traffic…").arg(voice.mid(0, voice.size() - 1).join(", "), voice.last()));
        setLabel(callTime_, "");
    }
    if (levelBar_->value() != int(level_)) levelBar_->setValue(int(level_));
    level_ *= 0.6;   // decay
}
