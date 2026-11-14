# Runs ON the Pi-Star as root (sent over SSH by the desktop app for an echo test
# call). It plays a synthetic call into the hotspot's network stack exactly as
# MMDVMHost would send it after receiving it from a radio, so the network's
# echo / parrot answers and the hotspot transmits the reply.
#
# stdin: one JSON line {"mode": ..., "interval_ms": ..., "count": N,
#        "ysf_parrot": "00001", "parrot_tg": 10}
#        followed by N packets, each a 2-byte big-endian length + payload
#        (the UDP payload MMDVMHost would send to its gateway or to BrandMeister).
# stdout: progress lines "STATUS <text>", finally "DONE" or "ERROR <text>".
#
# Packets are sent from MMDVMHost's own source port with a raw socket, because
# the gateways only accept traffic from MMDVMHost's address and port.
# YSF/P25/NXDN gateways are pointed at their parrot with a remote command for
# the test and put back on their previous reflector / talkgroup afterwards.
import glob, json, re, socket, struct, sys, time

LOGS = "/var/log/pi-star"


def say(kind, text=""):
    sys.stdout.write((kind + " " + text).strip() + "\n")
    sys.stdout.flush()


def conf(path, section, key, default=None):
    cur = None
    try:
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if line.startswith("["):
                    cur = line
                elif cur == section and line.startswith(key + "="):
                    return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return default


def log_lines(prefix):
    lines = []
    for f in sorted(glob.glob("%s/%s-*.log" % (LOGS, prefix)))[-2:]:
        try:
            with open(f, errors="replace") as fh:
                lines.extend(fh.read().splitlines())
        except OSError:
            pass
    return lines


def remote_command(port, text):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(text.encode(), ("127.0.0.1", port))
    s.close()


def raw_udp(sock, src, sport, dst, dport, payload):
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


# ---------------------------------------------------------------- per-mode state
def ysf_current():
    """(command to restore the current YSF link, description)."""
    state = None
    for line in log_lines("YSFGateway"):
        m = re.search(r"Linked to (\S.*?)\s*$", line)
        if m:
            state = ("link", m.group(1))
        elif re.search(r"Disconnect|Unlinked|unlinking|Link has failed", line, re.I):
            state = ("unlink", "")
    if not state or state[0] == "unlink":
        return "UnLink", "not linked"
    name = state[1]
    if name.upper().startswith("FCS"):
        return "LinkFCS " + name.replace("-", "").replace(" ", "")[:8], name
    try:
        with open("/usr/local/etc/YSFHosts.txt", errors="replace") as fh:
            for line in fh:
                f = line.split(";")
                if len(f) > 1 and f[1].strip().upper() == name.upper():
                    return "LinkYSF " + f[0].strip(), name
    except OSError:
        pass
    return "LinkYSF " + name, name


def tg_current(prefix):
    """(command to restore the current P25/NXDN talkgroup, description)."""
    state = None
    for line in log_lines(prefix):
        m = re.search(r"(?:Statically linked to|Switched to|Linked to|Linking to) reflector (\d+)", line)
        if m:
            state = m.group(1)
        elif re.search(r"Unlink(?:ed|ing) from reflector", line):
            state = None
    return ("TalkGroup " + state, "TG " + state) if state else ("TalkGroup", "not linked")


def learn_brandmeister(timeout=30):
    """MMDVMHost's (local ip, local port, master ip, master port), from its next keepalive."""
    host = conf("/etc/mmdvmhost", "[DMR Network]", "Address") or conf("/etc/mmdvmhost", "[DMR Network]", "RemoteAddress")
    port = int(conf("/etc/mmdvmhost", "[DMR Network]", "Port", "62031"))
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0003))
    s.settimeout(1.0)
    end = time.time() + timeout
    while time.time() < end:
        try:
            pkt, addr = s.recvfrom(2048)
        except socket.timeout:
            continue
        if addr[0] == "lo" or len(pkt) < 46 or struct.unpack_from("!H", pkt, 12)[0] != 0x0800 or pkt[23] != 17:
            continue
        ihl = (pkt[14] & 0x0F) * 4
        sport, dport = struct.unpack_from("!HH", pkt, 14 + ihl)
        payload = pkt[14 + ihl + 8:]
        if dport == port and payload[:4] in (b"RPTP", b"DMRD", b"RPTC"):
            return socket.inet_ntoa(pkt[26:30]), sport, socket.inet_ntoa(pkt[30:34]), dport
    raise RuntimeError("no BrandMeister traffic from MMDVMHost (is the DMR network logged in? host %s)" % host)


def main():
    cfg = json.loads(sys.stdin.buffer.readline().decode())
    mode = cfg["mode"]
    interval = cfg.get("interval_ms", 20) / 1000.0
    packets = []
    for _ in range(int(cfg["count"])):
        n = struct.unpack("!H", read_exact(2))[0]
        packets.append(read_exact(n))

    mm = "/etc/mmdvmhost"
    restore = None   # (port, command, description)
    if mode == "DMR":
        say("STATUS", "waiting for MMDVMHost's BrandMeister keepalive")
        src, sport, dst, dport = learn_brandmeister()
    elif mode == "D-Star":
        src, dst = "127.0.0.1", conf(mm, "[D-Star Network]", "GatewayAddress", "127.0.0.1")
        sport = int(conf(mm, "[D-Star Network]", "LocalPort", "20011"))
        dport = int(conf(mm, "[D-Star Network]", "GatewayPort", "20010"))
    elif mode == "YSF":
        src, dst = "127.0.0.1", conf(mm, "[System Fusion Network]", "GatewayAddress", "127.0.0.1")
        sport = int(conf(mm, "[System Fusion Network]", "LocalPort", "3200"))
        dport = int(conf(mm, "[System Fusion Network]", "GatewayPort", "4200"))
        rport = int(conf("/etc/ysfgateway", "[Remote Commands]", "Port", "6073"))
        cmd, desc = ysf_current()
        restore = (rport, cmd, desc)
        parrot = str(cfg.get("ysf_parrot", "00001"))
        say("STATUS", "linking YSF to the parrot %s (was %s)" % (parrot, desc))
        remote_command(rport, "LinkYSF " + parrot)
        time.sleep(2.0)
    elif mode in ("P25", "NXDN"):
        sect = "[P25 Network]" if mode == "P25" else "[NXDN Network]"
        src, dst = "127.0.0.1", conf(mm, sect, "GatewayAddress", "127.0.0.1")
        sport = int(conf(mm, sect, "LocalPort", "32010" if mode == "P25" else "14021"))
        dport = int(conf(mm, sect, "GatewayPort", "42020" if mode == "P25" else "14020"))
        gw = "p25gateway" if mode == "P25" else "nxdngateway"
        rport = int(conf("/etc/" + gw, "[Remote Commands]", "Port", "6074" if mode == "P25" else "6075"))
        cmd, desc = tg_current("P25Gateway" if mode == "P25" else "NXDNGateway")
        restore = (rport, cmd, desc)
        # Link to the parrot first. The gateway queues a spoken "linked to 10" and only
        # plays it after the next end of transmission from MMDVMHost, so send the call's
        # end record on its own to get the announcement over with; otherwise it collides
        # with the parrot's playback of the call.
        tg = int(cfg.get("parrot_tg", 10))
        say("STATUS", "linking %s to the parrot, TG %d (was %s)" % (mode, tg, desc))
        remote_command(rport, "TalkGroup %d" % tg)
        time.sleep(1.0)
        flush = (src, sport, dst, dport, packets[-1])
    elif mode == "M17":
        src, dst = "127.0.0.1", conf(mm, "[M17 Network]", "GatewayAddress", "127.0.0.1")
        sport = int(conf(mm, "[M17 Network]", "LocalPort", "17011"))
        dport = int(conf(mm, "[M17 Network]", "GatewayPort", "17010"))
    else:
        raise RuntimeError("unknown mode " + mode)

    raw = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_RAW)
    if mode in ("P25", "NXDN"):
        raw_udp(raw, *flush)
        say("STATUS", "letting the gateway's link announcement play")
        time.sleep(9.0)
    say("STATUS", "sending %d packets %s:%d -> %s:%d" % (len(packets), src, sport, dst, dport))
    t0 = time.time()
    for i, p in enumerate(packets):
        delay = t0 + i * interval - time.time()
        if delay > 0:
            time.sleep(delay)
        raw_udp(raw, src, sport, dst, dport, p)
    talk = len(packets) * interval
    say("STATUS", "sent %.1f s; waiting for the echo" % talk)
    time.sleep(talk + 6.0)
    if restore:
        rport, cmd, desc = restore
        remote_command(rport, cmd)
        say("STATUS", "restored %s link: %s" % (mode, desc))
    say("DONE")


try:
    main()
except Exception as e:   # report, don't leave a traceback on the app's status line
    say("ERROR", str(e))
