"""Repeat the ping matrix and a multicast fan-out for N minutes.

Looks for drift the one-shot suite cannot see: links that quietly expire, key
state that decays, counters that run away, boards that fall off. Prints one
line per round and a summary; a single dropped packet is RF, a pair that fails
in EVERY round is not.
"""
import time, re, sys, serial, serial.tools.list_ports as lp
IDS = {'F8738D':'WTHG-0272A1F8738D', '81BA51':'WTHG-021BF681BA51', 'F8823D':'WTHG-0272A1F8823D'}
MINUTES = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
def resolve():
    pm = {}
    for p in lp.comports():
        if p.vid == 0x303a and p.serial_number: pm.setdefault(p.serial_number, []).append(p.device)
    out = {}
    for k, sn in IDS.items():
        for d in pm.get(sn, []):
            try:
                s = serial.Serial(d, 115200, timeout=1); time.sleep(0.35); s.reset_input_buffer()
                s.write(b'AT\r\n'); s.flush(); time.sleep(0.7)
                if 'OK' in s.read(200).decode(errors='replace'): out[k] = d; s.close(); break
                s.close()
            except Exception: pass
    return out
time.sleep(55)
P = resolve(); print("soaking %.0f min with: %s" % (MINUTES, ", ".join(sorted(P))))
def q(k, cmd, w=1.4):
    """Query a board and drain until the AT response terminator.

    Not a fixed-size read: the firmware also emits ESP_LOG lines on this port,
    and a fixed read returns whichever bytes arrived FIRST -- log spam -- while
    the reply is still in the buffer. That made working commands look like
    unreachable boards. Read until OK/ERROR or a deadline instead.

    A board that falls off USB returns "" so the caller records a miss and the
    run continues; these boards do that, and it is a bench fact, not a mesh
    result."""
    try:
        s = serial.Serial(P[k], 115200, timeout=0.4)
        time.sleep(0.3); s.reset_input_buffer()
        s.write(cmd + b'\r\n'); s.flush()
        deadline = time.time() + w + 6.0
        buf = b''
        while time.time() < deadline:
            chunk = s.read(1024)
            if chunk:
                buf += chunk
                tail = buf[-400:]
                if b'\r\nOK\r\n' in tail or b'ERROR' in tail:
                    # Give a multi-line reply a moment to finish arriving.
                    time.sleep(0.25); buf += s.read(4096)
                    break
        s.close()
        return buf.decode(errors='replace')
    except Exception as e:
        print("   !! %s unreachable (%s)" % (k, type(e).__name__)); return ""
ip = {k: (lambda m: m.group(1) if m else '?')(re.search(r'ip=(\S+)', q(k, b'AT+STATUS?'))) for k in P}
pairs = [(a, b) for a in sorted(P) for b in sorted(P) if a != b]
t_end = time.time() + MINUTES * 60
rnd = 0; totals = {p: [0, 0] for p in pairs}; bad_rounds = 0
while time.time() < t_end:
    rnd += 1; line = []
    round_bad = False
    for a, b in pairs:
        o = q(a, ('AT+MPING=%s,3' % ip[b]).encode(), w=8)
        m = re.search(r'(\d+) sent, (\d+) received', o)
        s_, r_ = (int(m.group(1)), int(m.group(2))) if m else (3, 0)
        totals[(a, b)][0] += s_; totals[(a, b)][1] += r_
        if r_ == 0: round_bad = True
        line.append("%s%s%d/%d" % (a[-3:], "->" + b[-3:], r_, s_))
    links = {k: re.search(r'\+MPMPEERS: ', q(k, b'AT+MPMPEERS?')) is not None for k in P}
    est = {k: q(k, b'AT+MPMSTAT?').count('estab=') and re.search(r'estab=(\d+)', q(k, b'AT+MPMSTAT?')).group(1) for k in P}
    if round_bad: bad_rounds += 1
    print("  r%-3d %s | estab %s" % (rnd, " ".join(line), ",".join("%s=%s" % (k, est[k]) for k in sorted(est))))
print("\n=== soak summary: %d rounds, %d with a dead pair ===" % (rnd, bad_rounds))
for p in pairs:
    s_, r_ = totals[p]
    print("  %-8s -> %-8s %4d/%4d  %.1f%% loss" % (p[0], p[1], r_, s_, 100.0 * (s_ - r_) / max(s_, 1)))
