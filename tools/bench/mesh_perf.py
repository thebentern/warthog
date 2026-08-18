"""Throughput and MTU across the HaLow mesh.

Sender-side rate comes from AT+MTPUT (elapsed time over what lwIP accepted);
delivered count comes from the receiver's AT+MCAST? counter, whose socket is
bound to 0.0.0.0:4403 and so takes unicast as well as the group. Loss is the
difference -- there is no retransmission anywhere in this path.
"""
import time, re, sys, serial, serial.tools.list_ports as lp
IDS = {'F8738D':'WTHG-0272A1F8738D', '81BA51':'WTHG-021BF681BA51', 'F8823D':'WTHG-0272A1F8823D'}
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
                r = s.read(200).decode(errors='replace'); s.close()
                if 'OK' in r: out[k] = d; break
            except Exception: pass
    return out
time.sleep(int(sys.argv[1]) if len(sys.argv) > 1 else 55)
P = resolve(); print("boards:", ", ".join(sorted(P)))
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
for k in sorted(P):
    m = re.search(r'\+MTU: (.*)', q(k, b'AT+MTU?')); print("  %s MTU %s" % (k, m.group(1).strip() if m else '?'))
ip = {k: (lambda m: m.group(1) if m else '?')(re.search(r'ip=(\S+)', q(k, b'AT+STATUS?'))) for k in P}
for k in P: q(k, b'AT+MCAST=1')
time.sleep(3)
def rx(k):
    m = re.search(r'rx=(\d+)', q(k, b'AT+MCAST?')); return int(m.group(1)) if m else -1
src = sorted(P)[0]; dst = sorted(P)[1]
print("\nUnicast UDP %s -> %s (%s)" % (src, dst, ip[dst]))
for size in (64, 256, 512, 1024):
    before = rx(dst)
    out = q(src, ('AT+MTPUT=%s,200,%d' % (ip[dst], size)).encode(), w=14)
    m = re.search(r'\+MTPUT: sent=(\d+) err=(\d+) size=\d+ ms=(\d+) kbps=(\d+)', out)
    time.sleep(2)
    got = rx(dst) - before
    if m:
        sent, err, ms, kbps = m.groups()
        loss = 100.0 * (int(sent) - got) / max(int(sent), 1)
        # Delivered rate is the number that means anything. UDP here is
        # unpaced, so the sender always outruns a 1 MHz link and the offered
        # rate just measures how fast lwIP accepted the writes.
        deliv_kbps = (got * size * 8) / max(int(ms), 1)
        print("  %4dB x200: offered %s kbps in %sms (err=%s) | DELIVERED %d pkts = %.0f kbps (%.0f%% loss)"
              % (size, kbps, ms, err, got, deliv_kbps, loss))
    else:
        print("  %4dB: no result (%s)" % (size, out.strip()[:60]))
