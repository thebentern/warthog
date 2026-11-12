#!/usr/bin/env python3
"""H6 test 3, the fast one: console output against refused control requests.

    usb_stress.py <usb-serial> [seconds=200] [mode=bad]

Needs a bench image built with -DWARTHOG_USBDIAG=1 -DWARTHOG_USBDIAG_SPAM=1 (main/usbdiag.c): its core-1
task writes a 90-byte line to the console every tick while the port is open. This script holds the port
open, reads and discards the output, and meanwhile has ./usbctl send control requests at about 2,000 a
second:

    bad        GET_DESCRIPTOR for string index 0xEE, which the board refuses (STALL); esp_tinyusb also
               logs a warning each time. The failing case.
    getcoding  GET_LINE_CODING, which the board answers. A control.
    none       no control requests. A control.

Pass: output keeps arriving for the whole run. Fail: "CONSOLE SILENT", four seconds with no byte.
Measured 2026-11-11 (docs/test-sweep-2026-11-10.md, H6): without the one packet cap in tusb.c, "bad"
went silent within 150 s in every run, usually within 20 s; with it, 500 s clean on two boards. A silent
console comes back with `./usbprobe reset <usb-serial>` (a port reset; the board does not reboot).
"""
import subprocess
import sys
import time

import serial

import h6common as h

if len(sys.argv) < 2:
    sys.exit(__doc__)
usb_serial = sys.argv[1]
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 200.0
mode = sys.argv[3] if len(sys.argv) > 3 else "bad"
if mode not in ("bad", "getcoding", "none"):
    sys.exit(f"unknown mode {mode}")

s = serial.Serial(h.port(usb_serial), 115200, timeout=0.2, write_timeout=2)
p = None
if mode != "none":
    p = subprocess.Popen([h.tool("usbctl"), usb_serial, mode, "100000000", "500"], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True)
t0 = last = time.time()
total = 0
res = "survived"
while time.time() - t0 < secs:
    d = s.read(4096)
    if d:
        total += len(d)
        last = time.time()
    elif time.time() - last > 4:
        res = f"CONSOLE SILENT at +{last - t0:.1f}s"
        break
    if p is not None and p.poll() is not None:
        res = "usbctl stopped early: " + p.stdout.read().strip()[:150]
        break
if p is not None and p.poll() is None:
    p.terminate()
    p.wait()
s.close()
print(f"{h.name(usb_serial)}: {res}; {total} console bytes in {time.time() - t0:.0f}s (mode {mode})")
if total == 0:
    print("  no output at all: is this the WARTHOG_USBDIAG_SPAM bench image, and was the console already silent?")
sys.exit(0 if res == "survived" and total > 0 else 1)
