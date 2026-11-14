# Runs ON the Pi-Star (root, over SSH) to list or change what a mode is linked to.
#   argv: list <mode>                 -> JSON {"entries": [{"id", "name"}...], "modules": bool}
#         link <mode> <target>        -> JSON {"ok": bool, "message": str}
#         unlink <mode>
# Modes: D-Star, YSF, P25, NXDN, M17. Uses the same controls as the Pi-Star
# dashboard: the gateways' remote-command UDP ports, and remotecontrold for
# ircDDBGateway.
import json, os, socket, subprocess, sys

HOSTS = "/usr/local/etc"


def conf(path, section, key, default=""):
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


def lines(name):
    try:
        with open(os.path.join(HOSTS, name), errors="replace") as fh:
            return [l.rstrip("\n") for l in fh if l.strip() and not l.startswith("#")]
    except OSError:
        return []


def remote(gateway_conf, default_port, command):
    port = int(conf("/etc/" + gateway_conf, "[Remote Commands]", "Port", str(default_port)))
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(command.encode(), ("127.0.0.1", port))
    s.close()
    return port


def tg_entries(hosts, local, parrot_port_conf):
    out, seen = [], set()
    if conf("/etc/" + parrot_port_conf, "[Network]", "ParrotPort"):
        out.append({"id": "10", "name": "Parrot (echo test)"})
        seen.add("10")
    for statics in conf("/etc/" + parrot_port_conf, "[Network]", "Static").split(","):
        if statics.strip().isdigit() and statics.strip() not in seen:
            out.append({"id": statics.strip(), "name": "static talkgroup"})
            seen.add(statics.strip())
    for f in (local, hosts):
        for l in lines(f):
            p = l.split()
            if p and p[0].isdigit() and p[0] not in seen:
                out.append({"id": p[0], "name": p[1] if len(p) > 1 else ""})
                seen.add(p[0])
    return out


def entries(mode):
    if mode == "YSF":
        out = []
        if conf("/etc/ysfgateway", "[YSF Network]", "ParrotPort"):
            out.append({"id": "00001", "name": "ZZ Parrot (echo test)"})
        for l in lines("YSFHosts.txt"):
            p = l.split(";")
            if len(p) > 1:
                out.append({"id": p[0].strip(), "name": (p[1].strip() + "  " + (p[2].strip() if len(p) > 2 else "")).strip()})
        for l in lines("FCSHosts.txt"):
            p = l.split(";")
            if p and p[0].startswith("FCS"):
                out.append({"id": p[0].strip(), "name": (p[2] if len(p) > 2 else p[1] if len(p) > 1 else "").strip()})
        return out, False
    if mode == "P25":
        return tg_entries("P25Hosts.txt", "P25HostsLocal.txt", "p25gateway"), False
    if mode == "NXDN":
        return tg_entries("NXDNHosts.txt", "NXDNHostsLocal.txt", "nxdngateway"), False
    if mode == "M17":
        return [{"id": l.split()[0], "name": l.split()[1] if len(l.split()) > 1 else ""} for l in lines("M17Hosts.txt")], True
    if mode == "D-Star":
        out = []
        for f in ("DCS_Hosts.txt", "DPlus_Hosts.txt", "DExtra_Hosts.txt"):
            for l in lines(f):
                p = l.split()
                if p:
                    out.append({"id": p[0], "name": p[1] if len(p) > 1 else ""})
        for l in lines("XLXHosts.txt"):
            p = l.split(";")
            if p and p[0].isdigit():
                out.append({"id": "XLX" + p[0], "name": p[1] if len(p) > 1 else ""})
        return out, True
    raise ValueError("no links for mode " + mode)


def dstar_repeater():
    call = ircddb_value("repeaterCall1")
    band = ircddb_value("repeaterBand1") or "B"
    return (call.ljust(7) + band)[:8]


def ircddb_value(key):
    try:
        with open("/etc/ircddbgateway") as fh:
            for line in fh:
                if line.startswith(key + "="):
                    return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return ""


def link(mode, target):
    if mode == "YSF":
        if target.upper().startswith("FCS"):
            remote("ysfgateway", 6073, "LinkFCS " + target.upper())
        else:
            remote("ysfgateway", 6073, "LinkYSF " + target)
        return "YSF: linking to " + target
    if mode in ("P25", "NXDN"):
        tg = int(target)
        remote("p25gateway" if mode == "P25" else "nxdngateway", 6074 if mode == "P25" else 6075, "TalkGroup %d" % tg)
        return "%s: switching to TG %d" % (mode, tg)
    if mode == "M17":
        name = target.upper()                       # "M17-672 C"
        if len(name.split()) != 2 or len(name.split()[1]) != 1:
            raise ValueError("M17 needs a reflector and a module, e.g. M17-672 C")
        remote("m17gateway", 6076, "Reflector " + name.replace(" ", "_"))
        return "M17: linking to " + name
    if mode == "D-Star":
        name = target.upper()                       # "DCS011 C"
        refl, _, module = name.partition(" ")
        if not refl or len(module.strip()) != 1:
            raise ValueError("D-Star needs a reflector and a module, e.g. DCS011 C")
        full = refl.ljust(7)[:7] + module.strip()
        r = subprocess.run(["/usr/local/bin/remotecontrold", dstar_repeater(), "link", "never", full],
                           capture_output=True, text=True, timeout=15)
        if r.returncode != 0:
            raise RuntimeError((r.stdout + r.stderr).strip() or "remotecontrold failed")
        return "D-Star: linking to " + full
    raise ValueError("no links for mode " + mode)


def unlink(mode):
    if mode == "YSF":
        remote("ysfgateway", 6073, "UnLink")
    elif mode in ("P25", "NXDN"):
        remote("p25gateway" if mode == "P25" else "nxdngateway", 6074 if mode == "P25" else 6075, "TalkGroup")
    elif mode == "M17":
        remote("m17gateway", 6076, "Reflector")
    elif mode == "D-Star":
        r = subprocess.run(["/usr/local/bin/remotecontrold", dstar_repeater(), "unlink"],
                           capture_output=True, text=True, timeout=15)
        if r.returncode != 0:
            raise RuntimeError((r.stdout + r.stderr).strip() or "remotecontrold failed")
    else:
        raise ValueError("no links for mode " + mode)
    return mode + ": unlinking"


try:
    action, mode = sys.argv[1], sys.argv[2]
    if action == "list":
        e, modules = entries(mode)
        print(json.dumps({"entries": e, "modules": modules}))
    elif action == "link":
        print(json.dumps({"ok": True, "message": link(mode, " ".join(sys.argv[3:]))}))
    elif action == "unlink":
        print(json.dumps({"ok": True, "message": unlink(mode)}))
    else:
        raise ValueError("unknown action " + action)
except Exception as e:
    print(json.dumps({"ok": False, "message": str(e)}))
