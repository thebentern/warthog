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

    --pi-bind sets the SOURCE address. It does not reliably select which node
    you reach, and the docstring here used to claim it did. Both bench Pis
    answer on 10.41.254.1 and both gadgets sit on one /16, so the destination
    route picks the egress interface and the source address does not override
    it -- `-b` and `-B` both land on whichever node the route and ARP cache
    currently favour. It can look like it works for a while and then stop.

    The fix is to give the nodes distinct addresses (`uci set
    network.lan.ipaddr=...`), not a better bind flag. Until then the
    distinct-peer check in main() is what stops a run from testing one node
    twice and reporting two passes.
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


def audit_peer(target, bind, mesh_id, sae):
    """Read back what the peer actually has, against what we told it.

    Every uci path here is one the guides instruct an operator to set, so a
    renamed or wrong option shows up as a mismatch rather than as a silent
    no-op. `wireless.radio1` is the HaLow DEVICE; channel lives there, not on
    the wifi-iface -- getting that wrong returns empty and reads as "unset".
    """
    want = {
        "wireless.radio1.disabled": "0",
        "wireless.radio1.hwmode": "11ah",
        "wireless.default_radio1.mode": "mesh",
        "wireless.default_radio1.mesh_id": mesh_id,
        "wireless.default_radio1.encryption": "sae" if sae else "none",
        "mesh11sd.mesh_beaconless.mesh_beacon_less_mode": "1",
    }
    got = {}
    for k in list(want) + ["wireless.radio1.channel", "wireless.radio1.country"]:
        got[k] = ssh(target, bind, "uci -q get %s || echo '<unset>'" % k).strip()

    # Label by the BIND address, not only the hostname. Both gadgets sit on the
    # same /16, so source-binding is an ambiguous selector and a hostname read
    # can come back from the other node -- two rows claiming one name while
    # reporting different values is exactly that, and it is not a peer fault.
    host = ssh(target, bind, "cat /proc/sys/kernel/hostname").strip() or target
    tag = "%s@%s" % (host, bind or "default")
    for k, v in want.items():
        check("peer-cfg/%s/%s" % (tag, k.split(".")[-1]), got[k] == v,
              "%s (want %s)" % (got[k] or "<empty>", v))
    print("        channel=%s country=%s" %
          (got["wireless.radio1.channel"], got["wireless.radio1.country"]))

    # Which fabric is this node on? The answer changes whether Warthog has to
    # displace anything, and it is one command rather than an assumption.
    bat = ssh(target, bind, "ip -br addr show bat0 2>&1 | head -1").strip()
    on_bat = "does not exist" not in bat and bat != ""
    print("        bat0: %s" % ("PRESENT -- warthog will be off this fabric"
                                if on_bat else "absent (bare bridged 802.11s)"))
    brif = ssh(target, bind, "ls /sys/class/net/br-lan/brif/ 2>/dev/null").split()
    print("        br-lan members: %s" % (" ".join(brif) or "(none)"))
    return tag


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
        # No sae_pwe: mesh SAE is hunt-and-peck on both sides whatever it says.
        cmds += ["uci set wireless.default_radio1.key='%s'" % passphrase]
    cmds += [
        "uci commit wireless",
        # Not needed for discovery (warthog's SAE beacons carry RSN; from source,
        # not measured). Kept because a bench Pi's MM6108 faulted on the mesh-beacon
        # path with beaconing on (chip firmware unrecorded). Dynamic peering is not
        # set: beaconless mode bypasses it.
        "uci set mesh11sd.mesh_beaconless.mesh_beacon_less_mode='1'",
        "uci commit mesh11sd",
        "wifi down radio1", "sleep 4", "wifi up radio1",
    ]
    ssh(target, bind, " ; ".join(cmds), timeout=60)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pi", action="append", default=[], required=True,
                    help="OpenMANET ssh target, repeatable")
    ap.add_argument("--pi-bind", action="append", default=[],
                    help="source address per --pi, same order. NOTE: this does "
                         "not reliably select a node when several answer the "
                         "same address on one subnet -- see ssh() and the "
                         "distinct-peer check.")
    ap.add_argument("--warthog", action="append", default=[],
                    help="warthog serial port, repeatable. Omit to run the "
                         "peer-side checks only -- useful when no board is "
                         "attached, and it still catches a mis-set peer.")
    ap.add_argument("--mesh-id", default="halowmesh")
    ap.add_argument("--passphrase", default="warthog-mesh")
    ap.add_argument("--sae", action="store_true",
                    help="SAE mesh, as OpenMANET's mesh wizard writes; without it, "
                         "an open mesh (encryption='none')")
    ap.add_argument("--negative", action="store_true",
                    help="also run the mismatch cases")
    ap.add_argument("--fwd", action="store_true",
                    help="turn 802.11s forwarding on (AT+MESHFWD=1) and report the relay state")
    ap.add_argument("--bridge", action="store_true",
                    help="turn L2 bridge mode on (AT+MESHBRIDGE=1); implies --fwd")
    ap.add_argument("--grp", choices=["replicate", "std"], default=None,
                    help="group-frame mode: replicate (per-peer unicast, default) or std "
                         "(3-address broadcasts). The bench A/B; see AT+MESHGRP.")
    ap.add_argument("--settle", type=int, default=90)
    ap.add_argument("--audit-only", action="store_true",
                    help="read the peers and report; change nothing. Safe to "
                         "run against a mesh that is already working -- the "
                         "configure step cycles radio1 and overwrites the key.")
    a = ap.parse_args()
    binds = a.pi_bind + [None] * (len(a.pi) - len(a.pi_bind))

    if not a.warthog:
        print("NOTE: no --warthog given; running the peer side only. The "
              "warthog<->OpenMANET result is NOT covered by this run.\n")

    if a.audit_only:
        print("== audit only: nothing will be changed ==")
    else:
        print("== 0. configure peers (idiomatic uci) ==")
        for pi, b in zip(a.pi, binds):
            configure_peer(pi, b, a.mesh_id, a.sae, a.passphrase)
            host = ssh(pi, b, "cat /proc/sys/kernel/hostname").strip()
            check("peer-configured/%s" % (host or pi), bool(host), host or "unreachable")

    print("\n== 0b. read the peers back ==")
    seen = {}
    for pi, b in zip(a.pi, binds):
        tag = audit_peer(pi, b, a.mesh_id, a.sae)
        name = tag.split("@")[0]
        seen.setdefault(name, []).append(b or "default")
    # Both gadgets are on one /16, so `ssh -b` is not a guaranteed selector: a
    # run can reach the same node twice and look like two passing nodes. That
    # would halve the coverage silently, which is worse than failing.
    for name, binds_hit in seen.items():
        check("distinct-peer/%s" % name, len(binds_hit) == 1,
              "reached from %s" % ", ".join(binds_hit)
              if len(binds_hit) == 1
              else "SAME node reached from %s -- source binding did not "
                   "select; this run covered fewer nodes than it appears to"
                   % ", ".join(binds_hit))

    print("\n== 1. warthog config ==")
    for w in a.warthog:
        t = at(w, ["AT+MESHEN=1", "AT+MESHID=%s" % a.mesh_id] +
                  (["AT+MESHPASS=%s" % a.passphrase] if a.sae else []) +
                  (["AT+MESHFWD=1"] if (a.fwd or a.bridge) else ["AT+MESHFWD=0"]) +
                  (["AT+MESHBRIDGE=1"] if a.bridge else ["AT+MESHBRIDGE=0"]) +
                  (["AT+MESHGRP=%d" % (1 if a.grp == "std" else 0)] if a.grp else []) +
                  ["AT+MESHCHAN?"])
        applied = "applied=yes" in t
        check("warthog-chan-applied/%s" % w.split("/")[-1], applied,
              "channel pin applied" if applied
              else "NOT applied -- radio is on the regulatory default")

    if a.audit_only:
        print("\n== peering (as found) ==")
        for pi, b in zip(a.pi, binds):
            host = ssh(pi, b, "cat /proc/sys/kernel/hostname").strip() or pi
            dump = ssh(pi, b, "iw dev wlh0 station dump")
            estab = dump.count("ESTAB")
            check("peering/%s@%s" % (host, b or "default"), estab > 0,
                  "%d ESTAB" % estab)
        bad = [n for n, ok, _ in RESULTS if not ok]
        print("\n%d checks, %d failed%s" %
              (len(RESULTS), len(bad), (": " + ", ".join(bad)) if bad else ""))
        return 1 if bad else 0

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
        # When a node did not peer, the config surface says which of the two
        # causes it is -- printed only then, so a passing run stays readable.
        if n < want:
            for line in at(w, ["AT+MESHCFG?"]).splitlines():
                if line.strip().startswith("+MESHCFG:"):
                    print("        %s" % line.strip())
        # Diagnostics that are meaningless to assert on but decide what to do next.
        for line in at(w, ["AT+MESHRSSI?"]).splitlines():
            if line.strip().startswith("+MESHRSSI:"):
                print("        %s" % line.strip())
        for tag in ("ae=", "fwdcand=", "nodec grp="):
            m = re.search(re.escape(tag) + r"(\S+)", t)
            if m:
                print("        %s%s" % (tag, m.group(1)))
        # Relay state: reported, never asserted -- nothing here has a measured
        # baseline yet, and the first run IS the baseline.
        if a.fwd or a.bridge:
            for line in at(w, ["AT+MESHGRP?", "AT+MESHFWDSTAT?", "AT+MESHPATH?"]).splitlines():
                if line.strip().startswith(("+MESHGRP:", "+MESHFWDSTAT:", "+MESHPATH:")):
                    print("        %s" % line.strip())

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
