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
<usb-serial>` recovers it). The run also fails if UDP or ping cannot generate load on any board's interface.

Test 2 fails rarely on an image without the fix (about one run in 10 to 25, measured 2026-11-11), so a
clean handful of runs proves nothing: use usb_stress.py to judge a change.
"""
import math
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from contextlib import ExitStack

import serial

import h6common as h

if len(sys.argv) < 5 or sys.argv[1] not in ("reopen", "commands"):
    sys.exit(__doc__)
mode, secs, dest, boards = sys.argv[1], float(sys.argv[2]), sys.argv[3], sys.argv[4:]
if not math.isfinite(secs) or secs <= 0:
    sys.exit("seconds must be positive and finite")
KBPS = 1500
t0 = time.monotonic()
stop = threading.Event()
failed = {}
lock = threading.Lock()


def say(msg):
    with lock:
        print(f"{time.strftime('%H:%M:%S')} +{time.monotonic() - t0:5.1f} {msg}", flush=True)


def fail(b, why):
    failed.setdefault(b, f"{why} at +{time.monotonic() - t0:.1f}s")
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
        stop.wait(max(0.05, 1.0 - (time.time() - ts)))
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
        stop.wait(max(0.05, 1.0 - (time.time() - ts)))
    say(f"{h.name(b)} reopen: {opens} opens, {fails} failures")
    if fails:
        fail(b, f"{fails} port opens failed")


def start_traffic(b, resources):
    dev = h.iface(b)
    s = resources.enter_context(socket.socket(socket.AF_INET, socket.SOCK_DGRAM))
    if sys.platform == "darwin":
        s.setsockopt(socket.IPPROTO_IP, 25, socket.if_nametoindex(dev))  # IP_BOUND_IF
    else:
        s.setsockopt(socket.SOL_SOCKET, 25, dev.encode())  # SO_BINDTODEVICE (needs root)
    via = ["-b", dev] if sys.platform == "darwin" else ["-I", dev]
    # A file lets ping keep running even if it logs repeated errors before SIGINT.
    output = resources.enter_context(tempfile.TemporaryFile(mode="w+t", encoding="utf-8", errors="replace"))
    ping = subprocess.Popen(["ping"] + via + ["-i", "0.1", "-s", "1400", dest],
                            stdout=output, stderr=subprocess.STDOUT, env={**os.environ, "LC_ALL": "C"})
    return dev, s, ping, output


def udp(b, s):
    gap = 1200 * 8 / (KBPS * 1000.0)
    payload = bytes(1200)
    sent = 0
    nxt = time.monotonic()
    try:
        while not stop.is_set():
            s.sendto(payload, (dest, 5001))
            sent += 1
            nxt += gap
            stop.wait(max(0, nxt - time.monotonic()))
    except OSError as e:
        fail(b, f"UDP send failed: {e}")
    if not sent:
        fail(b, "UDP sent no datagrams")
    say(f"{h.name(b)} udp: {sent} datagrams sent")


def finish_ping(b, ping, output, validate):
    early_exit = ping.poll()
    if early_exit is None:
        ping.send_signal(signal.SIGINT)
    try:
        ping.wait(timeout=5)
    except subprocess.TimeoutExpired:
        ping.kill()
        ping.wait()
        fail(b, "ping did not stop after SIGINT")
    if not validate:
        return
    output.seek(0)
    out = output.read().strip()
    summary = re.search(r"(\d+) packets transmitted, (\d+) (?:packets )?received", out)
    # The transmitted count can include failed send attempts.
    send_error = re.search(r"^ping: (?:sendmsg|sendto|local error):.*", out, re.MULTILINE)
    detail = f" ({out.splitlines()[-1]})" if out else ""
    problem = None
    if early_exit is not None:
        problem = f"ping exited before the test ended (status {early_exit}){detail}"
    elif send_error:
        problem = f"ping send failed: {send_error.group()}"
    elif summary is None or int(summary.group(1)) == 0:
        problem = f"ping reported no packets sent{detail}"
    if problem:
        say(f"{h.name(b)} {problem}")
        fail(b, problem)
    else:
        say(f"{h.name(b)} ping 1400 B x10/s: {summary.group(2)}/{summary.group(1)}")


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


devs = {}
workers = [threading.Thread(target=watch_usb)]
started = []
pings = []
with ExitStack() as resources:
    try:
        for b in boards:
            try:
                dev, s, ping, output = start_traffic(b, resources)
            except (OSError, RuntimeError) as e:
                fail(b, f"traffic setup failed: {e}")
                break
            devs[b] = dev
            pings.append((b, ping, output))
            workers.append(threading.Thread(target=udp, args=(b, s)))
            workers.append(threading.Thread(target=commands if mode == "commands" else reopen, args=(b,)))
        else:
            t0 = time.monotonic()
            say(f"{mode} for {secs:.0f}s, load to {dest} through {devs}")
            for t in workers:
                t.start()
                started.append(t)
            while not stop.is_set() and time.monotonic() - t0 < secs:
                if any(ping.poll() is not None for _, ping, _ in pings):
                    break
                stop.wait(min(0.5, max(0, t0 + secs - time.monotonic())))
    finally:
        stop.set()
        for b, ping, output in pings:
            finish_ping(b, ping, output, validate=bool(started))
        for t in started:
            t.join()
if not failed:
    for b in boards:
        if not h.usb_answers(b):
            fail(b, "USB control requests unanswered at the end")
print("RESULT", "FAIL " + str({h.name(b): w for b, w in failed.items()}) if failed else "pass", flush=True)
sys.exit(1 if failed else 0)
