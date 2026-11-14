## Downloads

| File | For |
|---|---|
| `pistar-monitor-VERSION-linux-x86_64-with-codec2.tar.gz` | Most people. Includes M17 audio (needs `libcodec2`). |
| `pistar-monitor-VERSION-linux-x86_64-without-codec2.tar.gz` | Systems without Codec2: M17 calls are shown but silent. |

Built by GitHub Actions on Ubuntu 24.04 (x86-64). Runs on Ubuntu 24.04 or newer and other Linux
distributions with glibc 2.34+ and Qt 6.4+. Checksums are in `SHA256SUMS.txt`.

## Install

```sh
sudo apt install qt6-base-dev libcodec2-1.2 libpulse0 libespeak-ng1 pipewire-bin openssh-client
tar xzf pistar-monitor-VERSION-linux-x86_64-with-codec2.tar.gz
cd pistar-monitor-VERSION-linux-x86_64-with-codec2
./pistar-dmr
```

The full package list, also for building from source, is in `requirements.txt`. Then set up SSH
access to your hotspot and fill in **Settings → Connection** and **Settings → Station**, as
described in the [README](https://github.com/thebigshed/pistar-monitor#setting-up-the-hotspot).

Transmitting (echo tests and PTT) needs an amateur radio licence; the app won't transmit until
your callsign and IDs are set.
