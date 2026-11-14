# Pi-Star Monitor

A desktop app (Qt 6, C++) that turns a [Pi-Star](https://www.pistar.uk/) hotspot into a
listening post and a network radio: it decodes and plays the digital voice traffic passing
through the hotspot, shows DAPNET pages, records calls, changes reflector links and lets you
transmit from a PC headset.

It talks to the hotspot over SSH. It does not need any changes to Pi-Star.

## Features

- **Listen** to DMR, D-Star, YSF (DN), P25, NXDN and M17 calls passing through the hotspot,
  in both directions, with a live call panel, call log (and CSV logs) and level meter.
- **Recordings** of the last few calls, replayable from the log.
- **DAPNET / POCSAG pager** messages, with your own RICs highlighted.
- **Links**: shows what each mode is linked to (reflector / talkgroup / BrandMeister master)
  and lets you click to relink, using the same controls as the Pi-Star dashboard.
- **Modes**: follows Pi-Star's "Modes Enabled".
- **Test calls** in any mode, played locally or sent through the hotspot to the network's
  echo / parrot services so you hear the reply.
- **PTT transmit** from a USB headset in any mode, to whatever the mode is linked to.
- Built-in audio (PulseAudio / PipeWire, or ALSA), noise gate, speaker and microphone tests.
- All settings in one JSON file (`~/.config/pistar-dmr/pistar-dmr/settings.json`) and a
  settings page.

## Building

Debian / Ubuntu:

```sh
sudo apt install build-essential cmake qt6-base-dev libcodec2-dev
cmake -S . -B build
cmake --build build -j
./build/pistar-dmr
```

`libcodec2-dev` is optional (without it M17 calls are shown but not played).
At run time the app uses, if present: `libpulse-simple` or `libasound` (audio),
`libespeak-ng` (speech for test calls) and `pw-dump` (device lists). These are normally
already installed on a desktop.

## Setting up the hotspot

1. Let the PC log in to the Pi-Star with an SSH key (Pi-Star's root filesystem is
   read-only, so make it writable first):

   ```sh
   ssh-keygen -t ed25519          # if you don't have a key yet
   ssh pi-star@pi-star.local "sudo mount -o remount,rw / && mkdir -p ~/.ssh && \
       cat >> ~/.ssh/authorized_keys && chmod 700 ~/.ssh && chmod 600 ~/.ssh/authorized_keys; \
       sudo mount -o remount,ro /" < ~/.ssh/id_ed25519.pub
   ```

   The `pi-star` user already has passwordless `sudo`, which the app needs.

2. Start the app, open **Settings** and fill in the **Connection** (host, user) and
   **Station** pages (your callsign, DMR ID, hotspot ID, NXDN ID).

Nothing is installed on the Pi. The app streams small Python scripts to it over SSH:

| Script | Runs | What it does |
|---|---|---|
| `remote_sniff.py` | all the time | Passively reads the traffic between MMDVMHost and the gateways / BrandMeister, the DAPNET log and the link state. Changes nothing. |
| `remote_link.py` | when you relink | Sends the gateways' remote commands (or `remotecontrold` for ircDDBGateway). |
| `remote_inject.py` | echo test calls | Sends a call into the gateway as MMDVMHost would, then restores the links it changed. |
| `remote_tx.py` | while the app runs | Sends your PTT audio into the gateway / BrandMeister as MMDVMHost would. |

## Transmitting

Echo test calls and PTT put your callsign on the air through your hotspot and on the
networks. Only use them if you hold an amateur radio licence and set your own callsign and
IDs. The app refuses to transmit until the Station settings are filled in.

## Test aids

`--replay capture.bin`, `--test-call MODE` (local only), `--echo-call MODE`,
`--ptt-test MODE` (synthesised speech to the DMR parrot), `--mic-test`,
`--settings [tab]`, `--change-link MODE`. Test runs never write the settings file.

## Licence

GPL-3.0-or-later (see `LICENSE`), because it includes GPL code:

- `third_party/op25_vocoder`: AMBE/IMBE encoders from [OP25](https://github.com/boatbod/op25)
  (GPLv3), with one change (D-Star gain quantisation, see its README).
- `src/golay2087_table.h`: table from [MMDVMHost](https://github.com/g4klx/MMDVMHost) (GPLv2+);
  the network formats follow MMDVMHost and G4KLX's gateways.
- `third_party/mbelib`: [mbelib](https://github.com/szechyjs/mbelib) (ISC).

Note that AMBE and IMBE are covered by patents in some countries; check your local situation
before using the vocoders.
