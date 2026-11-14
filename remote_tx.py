# Runs ON the Pi-Star as root for the whole session (started over SSH by the app)
# so that PTT transmissions start instantly. It injects voice packets exactly as
# MMDVMHost would send them after receiving them from a radio, so they go to
# whatever the mode is linked to.
#
# stdin: frames of 2-byte big-endian length + body; body[0] is the type:
#   b"B" + JSON {"mode": ...}   begin a transmission
#   b"P" + packet               one UDP payload, sent immediately
#   b"E"                        end of transmission
# A zero-length frame is a keepalive. EOF (app gone) ends the session.
# stdout: "READY", "STATUS <text>", "ERROR <text>" lines.
import json, socket, struct, sys, threading, time

MM = "/etc/mmdvmhost"


def say(kind, text=""):
    sys.stdout.write((kind + " " + text).strip() + "\n")
    sys.stdout.flush()


def conf(section, key, default=None):
    cur = None
    try:
        with open(MM) as fh:
            for line in fh:
                line = line.strip()
                if line.startswith("["):
                    cur = line
                elif cur == section and line.startswith(key + "="):
                    return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return default


bm_route = None   # (local ip, local port, master ip, master port)


def watch_brandmeister():
    """Keep MMDVMHost's BrandMeister route current from its own traffic (keepalives every few seconds)."""
    global bm_route
    port = int(conf("[DMR Network]", "Port", "62031") or 62031)
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0003))
    while True:
        pkt, addr = s.recvfrom(2048)
        if addr[0] == "lo" or len(pkt) < 46 or struct.unpack_from("!H", pkt, 12)[0] != 0x0800 or pkt[23] != 17:
            continue
        ihl = (pkt[14] & 0x0F) * 4
        sport, dport = struct.unpack_from("!HH", pkt, 14 + ihl)
        if dport == port and pkt[14 + ihl + 8:14 + ihl + 12] in (b"RPTP", b"DMRD", b"RPTC"):
            bm_route = (socket.inet_ntoa(pkt[26:30]), sport, socket.inet_ntoa(pkt[30:34]), dport)


def route(mode):
    if mode == "DMR":
        for _ in range(150):                     # up to 15 s right after start-up
            if bm_route:
                return bm_route
            time.sleep(0.1)
        raise RuntimeError("no BrandMeister traffic seen from MMDVMHost yet")
    sect = {"D-Star": "[D-Star Network]", "YSF": "[System Fusion Network]", "P25": "[P25 Network]",
            "NXDN": "[NXDN Network]", "M17": "[M17 Network]"}[mode]
    local = {"D-Star": 20011, "YSF": 3200, "P25": 32010, "NXDN": 14021, "M17": 17011}[mode]
    gw = {"D-Star": 20010, "YSF": 4200, "P25": 42020, "NXDN": 14020, "M17": 17010}[mode]
    return ("127.0.0.1", int(conf(sect, "LocalPort", local) or local),
            conf(sect, "GatewayAddress", "127.0.0.1") or "127.0.0.1", int(conf(sect, "GatewayPort", gw) or gw))


def raw_udp(sock, r, payload):
    src, sport, dst, dport = r
    udp = struct.pack("!HHHH", sport, dport, 8 + len(payload), 0) + payload   # checksum 0 = none
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 0, 0x4000, 64, 17, 0,
                     socket.inet_aton(src), socket.inet_aton(dst))
    sock.sendto(ip + udp, (dst, 0))


def read_exact(n):
    buf = b""
    while len(buf) < n:
        chunk = sys.stdin.buffer.read(n - len(buf))
        if not chunk:
            raise EOFError
        buf += chunk
    return buf


def main():
    threading.Thread(target=watch_brandmeister, daemon=True).start()
    raw = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_RAW)
    say("READY")
    r = None
    mode = None
    sent = 0
    while True:
        n = struct.unpack("!H", read_exact(2))[0]
        if n == 0:
            continue                             # keepalive
        body = read_exact(n)
        kind = body[:1]
        if kind == b"B":
            mode = json.loads(body[1:].decode())["mode"]
            try:
                r = route(mode)
                sent = 0
                say("STATUS", "transmitting %s" % mode)
            except Exception as e:
                r = None
                say("ERROR", str(e))
        elif kind == b"P" and r:
            raw_udp(raw, r, body[1:])
            sent += 1
        elif kind == b"E":
            if r:
                say("STATUS", "%s transmission ended (%d packets)" % (mode, sent))
            r = None


try:
    main()
except EOFError:
    pass
except Exception as e:
    say("ERROR", str(e))
