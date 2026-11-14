#!/usr/bin/env python3
"""Listen to the DMR traffic passing through a Pi-Star hotspot.

Pipeline:
  Pi-Star (ssh) -> remote_sniff.py -> DMRD packets -> AMBE+2 frames
  -> mbelib (ctypes) -> 8 kHz PCM -> aplay (and/or a WAV file)

Usage:
  ./dmr_listen.py                       # live, play through speakers
  ./dmr_listen.py --wav calls.wav       # also record
  ./dmr_listen.py --tg 2350 --slot 2    # filter
  ./dmr_listen.py --dump raw.bin        # save raw packets for later
  ./dmr_listen.py --replay raw.bin      # decode a saved capture
"""
import argparse, ctypes, ctypes.util, hashlib, os, struct, subprocess, sys, time, wave
from collections import OrderedDict
from pathlib import Path

HERE = Path(__file__).resolve().parent

# --- DMR AMBE+2 de-interleave tables (from DSD include/dmr_const.h, ISC) ---
rW = [0,1,0,1,0,1, 0,1,0,1,0,1, 0,1,0,1,0,1, 0,1,0,1,0,2, 0,2,0,2,0,2, 0,2,0,2,0,2]
rX = [23,10,22,9,21,8, 20,7,19,6,18,5, 17,4,16,3,15,2, 14,1,13,0,12,10, 11,9,10,8,9,7, 8,6,7,5,6,4]
rY = [0,2,0,2,0,2, 0,2,0,3,0,3, 1,3,1,3,1,3, 1,3,1,3,1,3, 1,3,1,3,1,3, 1,3,1,3,1,3]
rZ = [5,3,4,2,3,1, 2,0,1,13,0,12, 22,11,21,10,20,9, 19,8,18,7,17,6, 16,5,15,4,14,3, 13,2,12,1,11,0]


def payload_bits(data33):
    """33 bytes -> list of 264 bits, MSB first."""
    return [(b >> (7 - i)) & 1 for b in data33 for i in range(8)]


def ambe_frames(data33):
    """Split a DMR voice burst into its three 72-bit AMBE+2 frames.
    Layout: 108 voice bits | 48 sync/EMB bits | 108 voice bits."""
    bits = payload_bits(data33)
    voice = bits[0:108] + bits[156:264]          # 216 bits = 3 x 72
    frames = []
    for f in range(3):
        fb = voice[f * 72:(f + 1) * 72]
        fr = [[0] * 24 for _ in range(4)]
        for i in range(36):
            fr[rW[i]][rX[i]] = fb[2 * i]         # dibit high bit
            fr[rY[i]][rZ[i]] = fb[2 * i + 1]     # dibit low bit
        frames.append(fr)
    return frames, bytes(voice)


class Mbe:
    """Thin ctypes wrapper around mbelib's AMBE 3600x2450 decoder."""
    PARMS = 8192   # generous buffer for struct mbe_parameters

    def __init__(self, libpath=None):
        cands = [libpath, os.environ.get("MBELIB"),
                 str(HERE / "third_party/mbelib/build/libmbe.so"),
                 ctypes.util.find_library("mbe")]
        last = None
        for c in filter(None, cands):
            try:
                self.lib = ctypes.CDLL(c)
                break
            except OSError as e:
                last = e
        else:
            raise SystemExit(f"mbelib not found ({last}). Build it into third_party/mbelib "
                             "or set MBELIB=/path/to/libmbe.so")
        self.cur = ctypes.create_string_buffer(self.PARMS)
        self.prev = ctypes.create_string_buffer(self.PARMS)
        self.prev_enh = ctypes.create_string_buffer(self.PARMS)
        self.reset()

    def reset(self):
        self.lib.mbe_initMbeParms(self.cur, self.prev, self.prev_enh)

    def decode(self, fr, uvquality=3):
        ambe_fr = (ctypes.c_char * 96)(*[bytes([fr[r][c]]) for r in range(4) for c in range(24)])
        ambe_d = (ctypes.c_char * 49)()
        aout = (ctypes.c_float * 160)()
        errs, errs2 = ctypes.c_int(0), ctypes.c_int(0)
        err_str = ctypes.create_string_buffer(64)
        self.lib.mbe_processAmbe3600x2450Framef(
            aout, ctypes.byref(errs), ctypes.byref(errs2), err_str,
            ambe_fr, ambe_d, self.cur, self.prev, self.prev_enh, ctypes.c_int(uvquality))
        return list(aout), errs2.value


class Agc:
    """Simple peak AGC, similar in spirit to DSD's auto gain."""
    def __init__(self, target=20000.0, max_gain=60.0):
        self.target, self.max_gain, self.gain = target, max_gain, 20.0

    def run(self, samples):
        peak = max((abs(s) for s in samples), default=0.0)
        if peak > 0:
            want = min(self.max_gain, self.target / peak)
            # fast attack, slow release
            self.gain = want if want < self.gain else self.gain + (want - self.gain) * 0.05
        out = bytearray()
        for s in samples:
            v = int(s * self.gain)
            out += struct.pack("<h", max(-32767, min(32767, v)))
        return bytes(out)


def load_ids(host, user):
    """Pull DMRIds.dat from the Pi-Star so we can show callsigns."""
    try:
        r = subprocess.run(["ssh", "-o", "BatchMode=yes", f"{user}@{host}",
                            "cat /usr/local/etc/DMRIds.dat"],
                           capture_output=True, timeout=30)
        ids = {}
        for line in r.stdout.decode(errors="replace").splitlines():
            p = line.split("\t")
            if len(p) >= 2 and p[0].isdigit():
                ids[int(p[0])] = " ".join(x for x in p[1:3] if x)
        return ids
    except Exception:
        return {}


def packet_stream(f):
    while True:
        hdr = f.read(2)
        if len(hdr) < 2:
            return
        (n,) = struct.unpack("!H", hdr)
        if n == 0:            # heartbeat from remote_sniff.py
            continue
        data = f.read(n)
        if len(data) < n:
            return
        yield data


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="pi-star.local")
    ap.add_argument("--user", default="pi-star")
    ap.add_argument("--tg", type=int, help="only play this talkgroup / destination ID")
    ap.add_argument("--slot", type=int, choices=[1, 2], help="only play this timeslot")
    ap.add_argument("--wav", help="also record decoded audio to this WAV file")
    ap.add_argument("--dump", help="save raw captured packets to this file")
    ap.add_argument("--replay", help="decode a file saved with --dump instead of live capture")
    ap.add_argument("--no-play", action="store_true", help="don't play audio")
    ap.add_argument("--no-ids", action="store_true", help="don't fetch DMRIds.dat for callsigns")
    ap.add_argument("--mbelib", help="path to libmbe.so")
    a = ap.parse_args()

    mbe = Mbe(a.mbelib)
    agc = Agc()
    ids = {} if (a.no_ids or a.replay) else load_ids(a.host, a.user)

    if a.replay:
        src = open(a.replay, "rb")
        proc = None
    else:
        proc = subprocess.Popen(
            ["ssh", "-o", "BatchMode=yes", "-o", "ServerAliveInterval=15",
             f"{a.user}@{a.host}", "sudo -n python3 -u -"],
            stdin=open(HERE / "remote_sniff.py", "rb"), stdout=subprocess.PIPE)
        src = proc.stdout

    player = None if a.no_play else subprocess.Popen(
        ["aplay", "-q", "-t", "raw", "-f", "S16_LE", "-r", "8000", "-c", "1"],
        stdin=subprocess.PIPE, stderr=subprocess.DEVNULL)  # silence underrun spam between calls
    wav = None
    if a.wav:
        wav = wave.open(a.wav, "wb")
        wav.setnchannels(1); wav.setsampwidth(2); wav.setframerate(8000)
    dump = open(a.dump, "ab") if a.dump else None

    interrupted = False
    seen = OrderedDict()          # dedupe the same burst seen on two hops
    active = {}                   # slot -> (stream_id, src, dst, start, frames, errs)

    def name(i):
        return f"{i} {ids[i]}" if i in ids else str(i)

    def end_call(slot, why):
        st = active.pop(slot, None)
        if st:
            _, s, d, t0, n, e = st
            ber = 100.0 * e / max(1, n * 72)  # corrected errors / coded bits
            print(f"   end  TS{slot}  {time.time() - t0:5.1f}s  ~{ber:.1f}% bit errors ({why})", flush=True)
            mbe.reset()

    print("Listening for DMR on", "replay " + a.replay if a.replay else f"{a.user}@{a.host}", "... Ctrl-C to stop", flush=True)
    try:
        for pkt in packet_stream(src):
            if not pkt.startswith(b"DMRD"):   # e.g. PAGE records (pager log lines)
                continue
            if dump:
                dump.write(struct.pack("!H", len(pkt)) + pkt); dump.flush()
            srcid = int.from_bytes(pkt[5:8], "big")
            dst = int.from_bytes(pkt[8:11], "big")
            flags = pkt[15]
            stream = pkt[16:20]
            data = pkt[20:53]
            slot = 2 if flags & 0x80 else 1
            private = bool(flags & 0x40)
            ftype = (flags >> 4) & 0x03
            dtype = flags & 0x0F

            if a.slot and slot != a.slot:
                continue
            if a.tg and dst != a.tg:
                continue

            if ftype == 2:                       # data sync: header / terminator
                if dtype == 2:
                    end_call(slot, "terminator")
                elif dtype == 1 and (slot not in active or active[slot][0] != stream):
                    end_call(slot, "new header")
                    active[slot] = [stream, srcid, dst, time.time(), 0, 0]
                    kind = "PRIVATE" if private else "TG"
                    print(f"{time.strftime('%H:%M:%S')}  TS{slot}  {name(srcid)}  ->  {kind} {name(dst) if private else dst}", flush=True)
                continue

            # voice burst (ftype 0 = voice, 1 = voice sync)
            frames, key = ambe_frames(data)
            # Key on stream + packet sequence + voice bits: the same burst seen on
            # two hops matches, but repeated identical (e.g. silent) frames don't.
            h = hashlib.blake2b(stream + pkt[4:5] + key, digest_size=8).digest()
            if h in seen:
                continue
            seen[h] = True
            if len(seen) > 2000:
                seen.popitem(last=False)

            if slot not in active or active[slot][0] != stream:
                # late entry (missed header)
                end_call(slot, "new stream")
                active[slot] = [stream, srcid, dst, time.time(), 0, 0]
                print(f"{time.strftime('%H:%M:%S')}  TS{slot}  {name(srcid)}  ->  TG {dst}  (late entry)", flush=True)

            pcm = bytearray()
            for fr in frames:
                samples, errs = mbe.decode(fr)
                active[slot][4] += 1
                active[slot][5] += errs
                pcm += agc.run(samples)
            if player:
                player.stdin.write(pcm); player.stdin.flush()
            if wav:
                wav.writeframes(pcm)
    except KeyboardInterrupt:
        interrupted = True
    finally:
        for s in list(active):
            end_call(s, "stopped")
        if wav: wav.close()
        if dump: dump.close()
        if player:
            try: player.stdin.close()
            except BrokenPipeError: pass
        if proc:
            proc.terminate()
            err = proc.wait()
            if err not in (0, -15, 143) and not interrupted:
                print(f"capture over ssh exited with status {err} "
                      "(is the SSH key installed and does sudo -n work?)", file=sys.stderr)


if __name__ == "__main__":
    main()
