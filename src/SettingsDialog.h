#pragma once
#include "Settings.h"
#include <QDialog>
#include <QElapsedTimer>
#include <QTimer>
#include <memory>
#include <vector>
#include "AudioIn.h"
#include "AudioOut.h"

class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QComboBox;
class QPushButton;
class QProgressBar;
class QLabel;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(const Settings &s, QWidget *parent = nullptr);
    ~SettingsDialog() override;
    Settings result() const;   // the edited copy
    void done(int r) override;
    void showTab(const QString &title);   // e.g. "Audio"; PISTAR_AUTOTEST=1 also starts the audio tests

private:
    void startSpeakerTest();
    void toggleMicTest(bool on);
    void stopTests();

    Settings s_;
    class QTabWidget *tabs_ = nullptr;
    // audio tests
    QPushButton *spkTest_ = nullptr, *micTest_ = nullptr;
    QProgressBar *spkMeter_ = nullptr, *micMeter_ = nullptr;
    QLabel *micDb_ = nullptr;
    QCheckBox *monitor_ = nullptr;
    std::unique_ptr<AudioOut> spkOut_, monOut_;
    std::unique_ptr<AudioIn> micIn_;
    std::vector<int16_t> spkPcm_;
    QElapsedTimer spkStart_;
    QTimer spkTimer_;
    double micPeakDb_ = -90;
    // connection
    QLineEdit *host_, *user_, *identity_;
    // station
    QLineEdit *callsign_, *dstarModule_;
    QSpinBox *dmrId_, *repeaterId_, *nxdnId_, *slot_, *cc_;
    // pager
    QLineEdit *rics_;
    QCheckBox *onlyMine_;
    // audio
    QSpinBox *volume_;
    QCheckBox *mute_;
    QComboBox *backend_;
    QComboBox *device_;
    QCheckBox *gate_;
    QDoubleSpinBox *gateThr_, *gateRed_;
    QSpinBox *gateHold_;
    // recording / logging
    QSpinBox *keepCalls_, *maxMinutes_;
    QDoubleSpinBox *minSeconds_;
    QLineEdit *recDir_, *logDir_;
    // test call
    QComboBox *testMode_;
    QCheckBox *testEcho_;
    QLineEdit *spoken_, *textMsg_, *voice_, *ysfParrot_, *m17Echo_, *dstarEcho_;
    QSpinBox *rate_, *localTg_, *dmrEcho_, *p25Echo_, *nxdnEcho_;
    // transmit
    QComboBox *txDevice_;
    QDoubleSpinBox *micGain_;
    QCheckBox *autoLevel_, *latch_, *dmrPrivate_;
    QSpinBox *txTimeout_, *dmrTg_;
    QLineEdit *dstarSuffix_;
    // advanced
    QSpinBox *callTimeout_, *linkTimeout_, *reconnect_, *pageHistory_;
    QCheckBox *onTop_;
};
