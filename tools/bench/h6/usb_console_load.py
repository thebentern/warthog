#!/usr/bin/env python3
"""H6 tests 1 and 2: the console port against USB network load, on ordinary images.

    usb_console_load.py reopen   <seconds> <dest-ip> <usb-serial>...
    usb_console_load.py commands <seconds> <dest-ip> <usb-serial>...

For every board named, the host pushes 1.5 Mbit/s of UDP and ten 1400-byte pings a second to <dest-ip>
through that board's USB network interface (any address the board routes to: another node's mesh address,
or the board's own 192.168.4.1 on a station image), and at the same time:

    reopen    opens and closes the console port once a second, sending nothing (test 1; run for 240 s)
    commands  opens the port once a second, sends AT+ASSERT?, AT+HEAP? and AT+STATUS?, reads the replies
              and closes it (test 2; run for 70 s, many times)

Pass: "RESULT pass". Fail: a board whose USB stops answering control requests (the DMA mode fault, needs a
power cycle), or one that misses two command rounds in a row (the console stall; `./usbprobe reset
<usb-serial>` recovers it). A single slow reply is not a failure.

Test 2 fails rarely on an image without the fix (about one run in 10 to 25, measured 2026-11-11), so a
clean handful of runs proves nothing: use usb_stress.py to judge a change.
"""
import re
import socket
import subprocess
import sys
import threading
import time

import serial

import h6common as h

if len(sys.argv) < 5 or sys.argv[1] not in ("reopen", "commands"):
    sys.exit(__doc__)
mode, secs, dest, boards = sys.argv[1], float(sys.argv[2]), sys.argv[3], sys.argv[4:]
KBPS = 1500
t0 = time.time()
stop = threading.Event()
failed = {}
lock = threading.Lock()


def say(msg):
    with lock:
        print(f"{time.strftime('%H:%M:%S')} +{time.time() - t0:5.1f} {msg}", flush=True)


def fail(b, why):
    failed.setdefault(b, f"{why} at +{time.time() - t0:.1f}s")
    stop.set()


def commands(b):
    rounds = misses = 0
    while not stop.is_set():
        ts = time.time()
        try:
            r = h.at(b, ["AT+ASSERT?", "AT+HEAP?", "AT+STATUS?"])
            if not all("OK" in v for v in r.values()):
                raise RuntimeError("no OK for " + ",".join(k for k, v in r.items() if "OK" not in v))
            rounds += 1
            misses = 0
        except Exception as e:  # a dead port raises, a silent console times out
            misses += 1
            say(f"{h.name(b)} round missed x{misses}: {repr(e)[:70]}")
            if misses >= 2:
                fail(b, "console silent")
        time.sleep(max(0.05, 1.0 - (time.time() - ts)))
    say(f"{h.name(b)} commands: {rounds} rounds answered")


def reopen(b):
    opens = fails = 0
    while not stop.is_set():
        ts = time.time()
        try:
            sp = serial.Serial(h.port(b), 115200, timeout=0.1, write_timeout=1.0)
            time.sleep(0.25)
            sp.read(4096)
            sp.close()
            opens += 1
        except Exception as e:
            fails += 1
            say(f"{h.name(b)} open failed x{fails}: {repr(e)[:70]}")
        time.sleep(max(0.05, 1.0 - (time.time() - ts)))
    say(f"{h.name(b)} reopen: {opens} opens, {fails} failures")
    if fails:
        fail(b, f"{fails} port opens failed")


def udp(b, dev):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if sys.platform == "darwin":
        s.setsockopt(socket.IPPROTO_IP, 25, socket.if_nametoindex(dev))  # IP_BOUND_IF
    else:
        s.setsockopt(socket.SOL_SOCKET, 25, dev.encode())  # SO_BINDTODEVICE (needs root)
    gap = 1200 * 8 / (KBPS * 1000.0)
    payload = bytes(1200)
    sent = err = 0
    nxt = time.time()
    while not stop.is_set():
        try:
            s.sendto(payload, (dest, 5001))
            sent += 1
        except OSError:
            err += 1
        nxt += gap
        d = nxt - time.time()
        if d > 0:
            time.sleep(d)
    s.close()
    say(f"{h.name(b)} udp: {sent} datagrams sent, {err} send errors")


def watch_usb():
    misses = {b: 0 for b in boards}
    while not stop.is_set():
        for b in boards:
            try:
                ok = h.usb_answers(b)
            except Exception as e:
                say(f"usbprobe failed: {repr(e)[:70]}")
                ok = True
            misses[b] = 0 if ok else misses[b] + 1
            if misses[b] >= 2:
                fail(b, "USB control requests unanswered")
        stop.wait(4)


devs = {b: h.iface(b) for b in boards}
say(f"{mode} for {secs:.0f}s, load to {dest} through {devs}")
threads = [threading.Thread(target=watch_usb)]
pings = {}
for b in boards:
    threads.append(threading.Thread(target=commands if mode == "commands" else reopen, args=(b,)))
    threads.append(threading.Thread(target=udp, args=(b, devs[b])))
    via = ["-b", devs[b]] if sys.platform == "darwin" else ["-I", devs[b]]
    pings[b] = subprocess.Popen(["ping"] + via + ["-i", "0.1", "-s", "1400", dest], stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, text=True)
for t in threads:
    t.start()
while not stop.is_set() and time.time() - t0 < secs:
    time.sleep(0.5)
stop.set()
for t in threads:
    t.join()
for b, p in pings.items():
    p.send_signal(2)
    try:
        out = p.communicate(timeout=5)[0]
    except Exception:
        p.kill()
        out = ""
    m = re.search(r"(\d+) packets transmitted, (\d+) (?:packets )?received", out)
    say(f"{h.name(b)} ping 1400 B x10/s: {m.group(2) + '/' + m.group(1) if m else 'n/a'}")
for b in boards:
    if b not in failed and not h.usb_answers(b):
        failed[b] = "USB control requests unanswered at the end"
print("RESULT", "FAIL " + str({h.name(b): w for b, w in failed.items()}) if failed else "pass", flush=True)
sys.exit(1 if failed else 0)
