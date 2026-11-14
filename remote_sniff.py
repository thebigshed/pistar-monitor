# Runs ON the Pi-Star (piped in over SSH, executed as root).
# 1. Passively sniffs MMDVM "Homebrew" DMRD packets (MMDVMHost <-> DMRGateway on
#    loopback, and MMDVMHost/DMRGateway <-> BrandMeister on the LAN interface).
#    Also D-Star "DSRP" packets between MMDVMHost and ircDDBGateway (loopback),
#    forwarded as b"DSTR" + b"R" (from RF, heading to the gateway) or b"N"
#    (from the network, heading to the modem) + the DSRP packet.
#    YSF ("YSFD", MMDVMHost <-> YSFGateway) and P25 (MMDVMHost <-> P25Gateway)
#    packets on loopback are forwarded as b"YSFX" / b"P25X" + b"R"|b"N" + packet,
#    and likewise NXDN ("ICOM", MMDVMHost <-> NXDNGateway) as b"NXDX" and
#    M17 ("M17 ", MMDVMHost <-> M17Gateway) as b"M17X".
# 2. Follows the DAPNETGateway log and forwards every "Sending message" line
#    (each POCSAG page the hotspot transmits), prefixed with b"PAGE".
# 4. Sends the enabled modes ("Modes Enabled" on the dashboard: [<mode>] Enable=
#    in /etc/mmdvmhost, plus the matching network Enable) as b"MODE" + JSON, on
#    startup and whenever /etc/mmdvmhost changes.
# 5. Sends what each mode is linked to (reflector / talkgroup / master), read from
#    the gateway logs, as b"ENDP" + JSON {mode: text, "" = not linked}, when it changes.
# 3. Watches /var/log/pi-star/Links.log (current D-Star reflector links, as
#    rewritten by ircDDBGateway) and sends its whole content as b"LINK" + text
#    on startup and whenever it changes.
# Each record goes to stdout as: 2-byte big-endian length + payload.
# A zero-length record is a heartbeat.
# Read-only: it opens a raw packet socket and reads a log, changes nothing.
import glob, json, os, socket, struct, sys, time

ETH_P_ALL = 0x0003
PACKET_OUTGOING = 4
PAGE_LOGS = "/var/log/pi-star/DAPNETGateway-*.log"
PAGE_HISTORY = 500
LINKS_LOG = "/var/log/pi-star/Links.log"

out = sys.stdout.buffer


def mmdvm_port(section_name, key, default):
    """Read a port from /etc/mmdvmhost, e.g. ("[D-Star Network]", "GatewayPort")."""
    section = None
    try:
        with open("/etc/mmdvmhost") as fh:
            for line in fh:
                line = line.strip()
                if line.startswith("["):
                    section = line
                elif section == section_name and line.startswith(key + "="):
                    return int(line.split("=", 1)[1])
    except (OSError, ValueError):
        pass
    return default


DSTAR_GW_PORT = mmdvm_port("[D-Star Network]", "GatewayPort", 20010)
YSF_GW_PORT = mmdvm_port("[System Fusion Network]", "GatewayPort", 4200)
YSF_LOCAL_PORT = mmdvm_port("[System Fusion Network]", "LocalPort", 3200)
P25_GW_PORT = mmdvm_port("[P25 Network]", "GatewayPort", 42020)
P25_LOCAL_PORT = mmdvm_port("[P25 Network]", "LocalPort", 32010)
NXDN_GW_PORT = mmdvm_port("[NXDN Network]", "GatewayPort", 14020)
NXDN_LOCAL_PORT = mmdvm_port("[NXDN Network]", "LocalPort", 14021)
M17_GW_PORT = mmdvm_port("[M17 Network]", "GatewayPort", 17010)
M17_LOCAL_PORT = mmdvm_port("[M17 Network]", "LocalPort", 17011)


def emit(data):
    try:
        out.write(struct.pack("!H", len(data)) + data)
        out.flush()
    except (BrokenPipeError, OSError):
        sys.exit(0)


def newest_log():
    files = sorted(glob.glob(PAGE_LOGS))
    return files[-1] if files else None


def page_lines(chunk):
    for line in chunk.splitlines():
        if b"Sending message" in line:
            yield line


# Recent page history so the app has something to show straight away.
history = []
for f in sorted(glob.glob(PAGE_LOGS))[-2:]:
    try:
        with open(f, "rb") as fh:
            history.extend(page_lines(fh.read()))
    except OSError:
        pass
for line in history[-PAGE_HISTORY:]:
    emit(b"PAGE" + line)

page_file = newest_log()
page_pos = os.path.getsize(page_file) if page_file else 0


def poll_pages():
    """Forward new page lines; follows the daily log rotation."""
    global page_file, page_pos
    latest = newest_log()
    if latest != page_file:
        page_file, page_pos = latest, 0
    if not page_file:
        return
    try:
        size = os.path.getsize(page_file)
        if size < page_pos:
            page_pos = 0
        if size == page_pos:
            return
        with open(page_file, "rb") as fh:
            fh.seek(page_pos)
            chunk = fh.read()
    except OSError:
        return
    end = chunk.rfind(b"\n") + 1          # only complete lines
    page_pos += end
    for line in page_lines(chunk[:end]):
        emit(b"PAGE" + line)


last_links = None
MODE_SECTIONS = {"D-Star": "D-Star", "DMR": "DMR", "YSF": "System Fusion", "P25": "P25",
                 "NXDN": "NXDN", "M17": "M17", "POCSAG": "POCSAG", "FM": "FM"}
last_modes = None


# ---------------------------------------------------------------- endpoints
import re

LOG_DIR = "/var/log/pi-star"
_tail_cache = {}   # path -> (size, mtime, lines)


def log_tail_lines(prefix, nbytes=262144):
    """Lines from the tails of the newest two <prefix>-*.log files (oldest first)."""
    lines = []
    for path in sorted(glob.glob("%s/%s-*.log" % (LOG_DIR, prefix)))[-2:]:
        try:
            st = os.stat(path)
        except OSError:
            continue
        cached = _tail_cache.get(path)
        if cached and cached[0] == st.st_size and cached[1] == st.st_mtime:
            lines.extend(cached[2])
            continue
        try:
            with open(path, "rb") as fh:
                if st.st_size > nbytes:
                    fh.seek(st.st_size - nbytes)
                    fh.readline()                      # drop the partial first line
                got = fh.read().decode("utf-8", "replace").splitlines()
        except OSError:
            continue
        _tail_cache[path] = (st.st_size, st.st_mtime, got)
        lines.extend(got)
    return lines


def last_state(lines, rules):
    """rules: list of (regex, fn(match) -> state). The last matching line wins."""
    state = None
    for line in lines:
        for rx, fn in rules:
            m = rx.search(line)
            if m:
                state = fn(m)
                break
    return state


def conf_value(path, section, key):
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
    return ""


def dmr_master_name():
    host = conf_value("/etc/mmdvmhost", "[DMR Network]", "Address") or \
        conf_value("/etc/mmdvmhost", "[DMR Network]", "RemoteAddress")
    typ = conf_value("/etc/mmdvmhost", "[DMR Network]", "Type")
    if typ.lower() == "gateway":
        return "DMRGateway"
    h = host.lower()
    if "brandmeister" in h:
        return "BM " + host.split(".")[0]
    if "tgif" in h:
        return "TGIF"
    if "freedmr" in h:
        return "FreeDMR"
    return host or "DMR master"


TG_RULES = [
    (re.compile(r"(?:Statically linked to|Switched to|Linked to|Linking to) reflector (\d+)"),
     lambda m: "TG " + m.group(1) + (" (Parrot)" if m.group(1) == "10" else "")),
    (re.compile(r"Unlink(?:ed|ing) from reflector"), lambda m: ""),
]
RULES = {
    "DMR": ("MMDVM", [
        (re.compile(r"DMR, Logged into the master successfully"), lambda m: "@master"),
        (re.compile(r"DMR, (?:Closing DMR Network|Login to the master has failed|Connection to the master has timed out|.*master.*(?:lost|closed))"),
         lambda m: ""),
    ]),
    "YSF": ("YSFGateway", [
        (re.compile(r"Linked to (\S.*?)\s*$"), lambda m: m.group(1)),
        (re.compile(r"Disconnect|Unlinked|unlinking|Link has failed|Not linked"), lambda m: ""),
    ]),
    "P25": ("P25Gateway", TG_RULES),
    "NXDN": ("NXDNGateway", TG_RULES),
    "M17": ("M17Gateway", [
        (re.compile(r"Linked (?:at startup )?to (?:reflector )?(\S+(?: [A-Z])?)\s*$"), lambda m: m.group(1)),
        (re.compile(r"Unlinked from reflector|Unlinking from reflector"), lambda m: ""),
    ]),
    "POCSAG": ("DAPNETGateway", [
        (re.compile(r"Logged into the DAPNET network"), lambda m: "DAPNET"),
        (re.compile(r"Closing DAPNET connection|DAPNET.*(?:failed|lost)|Error"), lambda m: ""),
    ]),
}
last_endpoints = None


def poll_endpoints():
    global last_endpoints
    ep = {}
    for mode, (prefix, rules) in RULES.items():
        st = last_state(log_tail_lines(prefix), rules)
        if st == "@master":
            st = dmr_master_name()
        ep[mode] = st                                  # None = no information
    try:
        with open(LINKS_LOG, errors="replace") as fh:
            m = re.search(r"Refl: (.{8})", fh.read())
        ep["D-Star"] = m.group(1).strip() if m else ""
    except OSError:
        ep["D-Star"] = ""
    if ep != last_endpoints:
        last_endpoints = ep
        emit(b"ENDP" + json.dumps(ep).encode())


def poll_modes():
    """Emit the enabled modes when /etc/mmdvmhost changes (cheap: stat once a second)."""
    global last_modes
    try:
        stamp = os.stat("/etc/mmdvmhost").st_mtime
    except OSError:
        return
    if stamp == last_modes:
        return
    last_modes = stamp
    enabled = {}
    for name, sect in MODE_SECTIONS.items():
        enabled[name] = {"mode": mmdvm_port("[%s]" % sect, "Enable", 0) == 1,
                         "network": mmdvm_port("[%s Network]" % sect, "Enable", 0) == 1}
    emit(b"MODE" + json.dumps(enabled).encode())


def poll_links():
    global last_links
    try:
        with open(LINKS_LOG, "rb") as fh:
            links = fh.read()
    except OSError:
        links = b""
    if links != last_links:
        last_links = links
        emit(b"LINK" + links)


s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
s.settimeout(1.0)
last_beat = 0.0
last_endp = 0.0

while True:
    # Heartbeat + page poll every second. If the PC end has gone away the
    # write fails and we exit instead of lingering as an orphan root process.
    now = time.time()
    if now - last_beat >= 1.0:
        emit(b"")
        poll_pages()
        poll_links()
        poll_modes()
        if now - last_endp >= 3.0:
            poll_endpoints()
            last_endp = now
        last_beat = now
    try:
        pkt, addr = s.recvfrom(65535)
    except socket.timeout:
        continue
    # On loopback every packet is seen twice (out + in), so drop the outgoing
    # copy there. On the LAN interface outgoing packets are our own RF traffic
    # heading to BrandMeister, so they must be kept.
    if addr[0] == "lo" and addr[2] == PACKET_OUTGOING:
        continue
    if len(pkt) < 14 + 20 + 8 + 4:
        continue
    off = 14
    ethertype = struct.unpack_from("!H", pkt, 12)[0]
    if ethertype == 0x8100:              # VLAN tag
        off += 4
        ethertype = struct.unpack_from("!H", pkt, 16)[0]
    if ethertype != 0x0800:
        continue
    ihl = (pkt[off] & 0x0F) * 4
    if pkt[off + 9] != 17:               # UDP only
        continue
    udp = off + ihl
    payload = pkt[udp + 8:]
    tag = payload[:4]
    if tag == b"DMRD" and len(payload) >= 53:
        emit(payload)
    elif tag == b"DSRP" and len(payload) >= 21 and payload[4] in (0x20, 0x21):
        dport = struct.unpack_from("!H", pkt, udp + 2)[0]
        emit(b"DSTR" + (b"R" if dport == DSTAR_GW_PORT else b"N") + payload)
    elif addr[0] == "lo":
        sport, dport = struct.unpack_from("!HH", pkt, udp)
        ports = (sport, dport)
        if tag == b"YSFD" and len(payload) >= 155 and (YSF_GW_PORT in ports or YSF_LOCAL_PORT in ports):
            emit(b"YSFX" + (b"R" if dport == YSF_GW_PORT else b"N") + payload)
        elif payload and P25_GW_PORT in ports and P25_LOCAL_PORT in ports and \
                (0x62 <= payload[0] <= 0x73 or payload[0] == 0x80):
            emit(b"P25X" + (b"R" if dport == P25_GW_PORT else b"N") + payload)
        elif tag == b"ICOM" and len(payload) == 102 and NXDN_GW_PORT in ports and NXDN_LOCAL_PORT in ports:
            emit(b"NXDX" + (b"R" if dport == NXDN_GW_PORT else b"N") + payload)
        elif tag == b"M17 " and len(payload) == 54 and M17_GW_PORT in ports and M17_LOCAL_PORT in ports:
            emit(b"M17X" + (b"R" if dport == M17_GW_PORT else b"N") + payload)
