#!/usr/bin/env python3
"""Reproducible Warthog <-> OpenMANET interop test.

Every other harness here drives warthogs only. This one puts real OpenMANET
nodes in the loop, configures both sides the idiomatic way, and asserts rather
than printing for a human to interpret.

  ./openmanet_interop.py --pi root@10.41.254.1 --pi-bind 10.41.0.230 \
                         --warthog /dev/cu.usbmodem11101 [--sae] [--negative]

Exits non-zero if any check fails, so it can gate a bench run.

Deliberately NOT idempotent about the peer: it writes uci on the OpenMANET
node. Point it at bench hardware, not something you care about.
"""
import argparse, re, subprocess, sys, time

try:
    import serial
except ImportError:
    sys.exit("pyserial required: pip install pyserial")

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, ok, detail))
    print("  [%s] %-34s %s" % ("PASS" if ok else "FAIL", name, detail))
    return ok


def ssh(target, bind, cmd, timeout=25):
    """Run a command on the OpenMANET node.

    --pi-bind matters: both Pis answer on 10.41.254.1 over their own USB
    gadget, so the source address is what selects which one you reach.
    """
    argv = ["ssh", "-o", "ConnectTimeout=8", "-o", "StrictHostKeyChecking=no",
            "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR"]
    if bind:
        argv += ["-b", bind]
    argv += [target, cmd]
    try:
        return subprocess.run(argv, capture_output=True, text=True,
                              timeout=timeout).stdout
    except subprocess.TimeoutExpired:
        return ""


def at(port, cmds, settle=0.9):
    """Query a warthog. Opening the port resets the board on this hardware,
    so do it once per batch rather than per command."""
    out = []
    try:
        s = serial.Serial()
        s.port, s.baudrate, s.timeout, s.dtr = port, 115200, 1.5, True
        s.open()
    except Exception as e:
        print("   !! %s: %s" % (port, e))
        return ""
    time.sleep(0.4)
    s.reset_input_buffer()
    for c in cmds:
        s.write((c + "\r\n").encode())
        time.sleep(settle)
        out.append(s.read(9000).decode("utf-8", "replace"))
    s.close()
    return "\n".join(out)


def field(text, tag, pat):
    m = re.search(r"\+" + tag + r":.*?" + pat, text, re.S)
    return m.group(1) if m else None


def configure_peer(target, bind, mesh_id, sae, passphrase):
    """Idiomatic uci. mesh11sd changes need a radio cycle -- `wifi reload`
    silently ignores them, which is the trap that costs the most time."""
    enc = "sae" if sae else "none"
    cmds = [
        "uci set wireless.radio1.disabled='0'",
        "uci set wireless.default_radio1.mode='mesh'",
        "uci set wireless.default_radio1.mesh_id='%s'" % mesh_id,
        "uci set wireless.default_radio1.encryption='%s'" % enc,
    ]
    if sae:
        cmds += ["uci set wireless.default_radio1.key='%s'" % passphrase,
                 # OpenMANET ships H2E-only; warthog sends hunt-and-peck commits.
                 "uci set wireless.default_radio1.sae_pwe='2'"]
    cmds += [
        "uci commit wireless",
        # Beaconless is mandatory on MM6108: with beaconing the chip firmware
        # faults on the mesh-beacon path and wlh0 goes down.
        "uci set mesh11sd.mesh_beaconless.mesh_beacon_less_mode='1'",
        "uci set mesh11sd.mesh_dynamic_peering.enabled='1'",
        "uci commit mesh11sd",
        "wifi down radio1", "sleep 4", "wifi up radio1",
    ]
    ssh(target, bind, " ; ".join(cmds), timeout=60)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pi", action="append", default=[], required=True,
                    help="OpenMANET ssh target, repeatable")
    ap.add_argument("--pi-bind", action="append", default=[],
                    help="source address per --pi, same order")
    ap.add_argument("--warthog", action="append", default=[], required=True,
                    help="warthog serial port, repeatable")
    ap.add_argument("--mesh-id", default="halowmesh")
    ap.add_argument("--passphrase", default="warthog-mesh")
    ap.add_argument("--sae", action="store_true",
                    help="encrypted mesh; without it, the open mesh stock OpenMANET ships")
    ap.add_argument("--negative", action="store_true",
                    help="also run the mismatch cases")
    ap.add_argument("--settle", type=int, default=90)
    a = ap.parse_args()
    binds = a.pi_bind + [None] * (len(a.pi) - len(a.pi_bind))

    print("== 0. configure peers (idiomatic uci) ==")
    for pi, b in zip(a.pi, binds):
        configure_peer(pi, b, a.mesh_id, a.sae, a.passphrase)
        host = ssh(pi, b, "cat /proc/sys/kernel/hostname").strip()
        check("peer-configured/%s" % (host or pi), bool(host), host or "unreachable")

    print("\n== 1. warthog config ==")
    for w in a.warthog:
        t = at(w, ["AT+MESHEN=1", "AT+MESHID=%s" % a.mesh_id] +
                  (["AT+MESHPASS=%s" % a.passphrase] if a.sae else []) +
                  ["AT+MESHCHAN?"])
        applied = "applied=yes" in t
        check("warthog-chan-applied/%s" % w.split("/")[-1], applied,
              "channel pin applied" if applied
              else "NOT applied -- radio is on the regulatory default")

    print("\n== 2. settle %ds ==" % a.settle)
    time.sleep(a.settle)

    print("\n== 3. peering ==")
    want = len(a.pi) + len(a.warthog) - 1
    for pi, b in zip(a.pi, binds):
        host = ssh(pi, b, "cat /proc/sys/kernel/hostname").strip() or pi
        dump = ssh(pi, b, "iw dev wlh0 station dump")
        estab = dump.count("ESTAB")
        check("peering/%s" % host, estab >= want, "%d/%d ESTAB" % (estab, want))

    for w in a.warthog:
        t = at(w, ["AT+PEERS?", "AT+RXCHAN?"])
        m = re.search(r"count=(\d+)", t)
        n = int(m.group(1)) if m else 0
        check("peering/%s" % w.split("/")[-1], n >= want, "count=%d/%d" % (n, want))
        # Diagnostics that are meaningless to assert on but decide what to do next.
        for line in at(w, ["AT+MESHRSSI?"]).splitlines():
            if line.strip().startswith("+MESHRSSI:"):
                print("        %s" % line.strip())
        for tag in ("ae=", "fwdcand=", "nodec grp="):
            m = re.search(re.escape(tag) + r"(\S+)", t)
            if m:
                print("        %s%s" % (tag, m.group(1)))

    print("\n== 4. data plane ==")
    for pi, b in zip(a.pi, binds):
        host = ssh(pi, b, "cat /proc/sys/kernel/hostname").strip() or pi
        for w in a.warthog:
            ip = field(at(w, ["AT+STATUS?"]), "HALOW", r"ip=(\S+)")
            if not ip or ip == "0.0.0.0":
                check("data/%s->%s" % (host, w.split("/")[-1]), False, "warthog has no mesh IP")
                continue
            out = ssh(pi, b, "ping -c 10 -W 2 %s" % ip, timeout=45)
            m = re.search(r"(\d+)% packet loss", out)
            loss = int(m.group(1)) if m else 100
            check("data/%s->%s" % (host, w.split("/")[-1]), loss < 50,
                  "%s  %d%% loss" % (ip, loss))

    if a.negative:
        print("\n== 5. negative cases ==")
        for w in a.warthog:
            t = at(w, ["AT+MESHCHAN=42,999000000,69,2,2"])
            check("reject/out-of-band-freq", "ERROR" in t or "rejected" in t,
                  "999 MHz refused at the setter")
            t = at(w, ["AT+MESHCHAN=42,923000000,69,2,3"])
            check("reject/bad-bandwidth", "ERROR" in t or "rejected" in t,
                  "3 MHz is not a legal S1G bandwidth")
            t = at(w, ["AT+MESHID="])
            check("reject/empty-mesh-id", "ERROR" in t, "empty mesh ID refused")

    bad = [n for n, ok, _ in RESULTS if not ok]
    print("\n%d/%d checks passed" % (len(RESULTS) - len(bad), len(RESULTS)))
    if bad:
        print("failed: " + ", ".join(bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
