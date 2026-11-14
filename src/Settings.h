#pragma once
// All app configuration, stored as JSON in ~/.config/pistar-dmr/settings.json.
// Window geometry is the only thing left in QSettings (it is opaque binary state).
#include <QString>
#include <QStringList>
#include <cstdint>

struct Settings {
    struct Connection {
        QString host = "pi-star.local";
        QString user = "pi-star";
        QString identityFile;            // ssh -i; empty = ssh's default keys
        int linkTimeoutMs = 10000;       // reconnect if no heartbeat for this long
        int reconnectDelayMs = 5000;
        int pageHistory = 500;           // DAPNET pages to load on connect
    } connection;

    struct Station {
        QString callsign = "N0CALL";       // set yours in Settings → Station
        uint32_t dmrId = 0;              // also used as the P25 source ID; 0 = not set
        uint32_t repeaterId = 0;         // hotspot ID in DMRD packets; 0 = not set
        uint32_t nxdnId = 0;             // 0 = not set
        int dmrSlot = 2;
        int colourCode = 1;
        QString dstarModule = "B";       // RPT1 module letter (RPT2 is the gateway, "G")
    } station;

    struct Pager {
        QString myRics;                  // comma separated
        bool onlyMine = true;
    } pager;

    struct Audio {
        int volume = 80;                 // percent, 0..150
        bool mute = false;
        QString backend = "auto";        // auto (PulseAudio, then ALSA), pulse or alsa
        QString device;                  // Pulse sink or ALSA device; empty = system default
        bool noiseGate = true;           // turn background noise down between words
        double gateThresholdDb = 10;     // open when speech is this far above the background
        double gateReductionDb = 25;     // how much to turn the background down
        int gateHoldMs = 250;            // keep open after speech, so word endings aren't cut
    } audio;

    struct Calls {
        int timeoutMs = 1500;            // end a call if packets stop without a terminator
    } calls;

    struct Recording {
        int keepCalls = 2;               // most recent calls kept as WAV
        double minSeconds = 1.0;         // shorter calls (kerchunks) are not kept
        int maxMinutes = 10;             // per call
        QString directory;               // empty = <data dir>/recordings
    } recording;

    struct Logging {
        QString directory;               // empty = ~/.local/share/pistar-dmr/pistar-dmr
    } logging;

    struct TestCall {
        QString mode = "DMR";
        bool echo = true;                // send through the hotspot to the echo / parrot
        QString spokenText;              // empty = callsign spelled out + "Test, test, test."
        QString textMessage;             // D-Star slow data (20 chars); empty = "<callsign> Test test test"
        QString voice = "en-gb";         // eSpeak NG voice
        int speechRate = 150;            // words per minute
        uint32_t localTalkgroup = 9;     // target when not echoing
        uint32_t dmrEchoId = 9990;       // BrandMeister Parrot (private call)
        uint32_t p25EchoTg = 10;
        uint32_t nxdnEchoTg = 10;
        QString ysfParrot = "00001";     // YSFGateway's local parrot
        QString m17Echo = "ECHO";
        QString dstarEcho = "       E";  // UR field for ircDDBGateway's echo
    } testCall;

    struct Transmit {
        QString mode = "D-Star";
        QString inputDevice;             // PulseAudio source; empty = system default
        double micGainDb = 0;            // applied before the automatic level control
        bool autoLevel = true;
        int timeoutSec = 180;            // stop transmitting after this long
        bool latch = false;              // click PTT on / click off, instead of hold to talk
        uint32_t dmrTalkgroup = 9;
        bool dmrPrivate = false;
        QString dstarSuffix;             // MY suffix, up to 4 characters
    } transmit;

    struct Window {
        bool alwaysOnTop = false;
    } window;

    // Station identity is filled in (callsign and the IDs for the modes used)
    bool stationReady(QString *why = nullptr) const;
    // Test-call texts, falling back to ones built from the callsign
    QString testSpokenText() const;
    QString testTextMessage() const;

    // Resolved directories (defaults applied)
    QString logDir() const;
    QString recordingDir() const;

    static QString path();               // ~/.config/pistar-dmr/settings.json
    static Settings load();              // defaults, then file; first run migrates QSettings
    bool save(QString *err = nullptr) const;
};
