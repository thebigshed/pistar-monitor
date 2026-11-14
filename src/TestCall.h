#pragma once
// Synthetic test calls: speak a phrase with eSpeak NG, encode it for a digital
// voice mode and wrap it in the same network records the Pi-Star sniffer sends,
// so the call goes through the app's normal decoding path.
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <cstdint>

#include "Settings.h"
#include <vector>

namespace TestCall {


QStringList modes();   // "DMR", "D-Star", "YSF", "P25", "NXDN", "M17"

// Text-to-speech at 8 kHz, 16-bit. Returns false (with err) if eSpeak NG is unavailable.
bool speak(const QString &text, const QString &voice, int rate, std::vector<int16_t> &pcm8k, QString &err);

struct Plan {
    QList<QByteArray> records;   // as dispatchRecord() expects
    int intervalMs = 20;         // real-time pacing per record
};

// textMessage goes where the mode has room for one (D-Star slow data).
// echo = address the call to the mode's echo / parrot service (targets and the
// station identity come from cfg.testCall / cfg.station).
Plan build(const QString &mode, const std::vector<int16_t> &pcm8k, const QString &textMessage, bool echo,
           const Settings &cfg, QString &err);

// The UDP payload MMDVMHost would send for a record (record minus the sniffer tag).
QByteArray wirePayload(const QByteArray &record);

}
