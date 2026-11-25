#!/usr/bin/env python3
"""H6 test 3, the fast one: console output against refused control requests.

    usb_stress.py <usb-serial> [seconds=200] [mode=bad]

Needs a bench image built with -DWARTHOG_USBDIAG=1 -DWARTHOG_USBDIAG_SPAM=1 (main/usbdiag.c): its core-1
task writes a 90-byte line to the console every tick while the port is open. This script holds the port
open and checks for those lines while ./usbctl sends control requests at about 2,000 a second:

    bad        GET_DESCRIPTOR for string index 0xEE, which the board refuses (STALL); esp_tinyusb also
               logs a warning each time. The failing case.
    getcoding  GET_LINE_CODING, which the board answers. A control.
    none       no control requests. A control.

Pass: complete diagnostic spam lines keep arriving throughout the run. Four seconds without a spam
line fails the run, even when other console output is arriving. Short runs still require a spam line.
Measured 2026-11-11 (docs/test-sweep-2026-11-10.md, H6): without the one packet cap in tusb.c, "bad"
went silent within 150 s in every run, usually within 20 s; with it, 500 s clean on two boards. A silent
console comes back with `./usbprobe reset <usb-serial>` (a port reset; the board does not reboot).
"""
import math
import re
import subprocess
import sys
import time

import serial

import h6common as h

# usbdiag_spam_task in main/usbdiag.c prints a uint32_t padded to at least eight digits.
SPAM_PAYLOAD = b"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvw"
SPAM_LINE = re.compile(rb"spam [0-9]{8,10} " + SPAM_PAYLOAD + rb"\r")
MAX_SPAM_LINE = len(b"spam 4294967295 " + SPAM_PAYLOAD + b"\r")
SPAM_TIMEOUT = 4.0


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    usb_serial = sys.argv[1]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 200.0
    if not math.isfinite(secs) or secs <= 0:
        sys.exit("seconds must be positive and finite")
    mode = sys.argv[3] if len(sys.argv) > 3 else "bad"
    if mode not in ("bad", "getcoding", "none"):
        sys.exit(f"unknown mode {mode}")

    with serial.Serial(h.port(usb_serial), 115200, timeout=0.2, write_timeout=2) as port:
        process = None
        if mode != "none":
            process = subprocess.Popen([h.tool("usbctl"), usb_serial, mode, "100000000", "500"],
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

        started = last_spam = now = time.monotonic()
        tail = b""
        total = records = 0
        failure = None
        try:
            while now - started < secs:
                data = port.read(4096)
                now = time.monotonic()
                total += len(data)
                if process is not None and process.poll() is not None:
                    output, _ = process.communicate()
                    failure = "usbctl stopped early: " + output.strip()[:150]
                    break
                if now - last_spam >= SPAM_TIMEOUT:
                    failure = f"NO DIAGNOSTIC SPAM for {now - last_spam:.1f}s at +{now - started:.1f}s"
                    break

                lines = (tail + data).split(b"\n")
                tail = lines.pop()
                # Discard oversized lines without treating their suffix as a new line.
                if len(tail) > MAX_SPAM_LINE:
                    tail = b"\0"
                new_records = sum(SPAM_LINE.fullmatch(line) is not None for line in lines)
                if new_records:
                    records += new_records
                    last_spam = now
        finally:
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.communicate(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate()

    if not records and failure is None:
        failure = "NO DIAGNOSTIC SPAM"
    print(f"{h.name(usb_serial)}: {failure or 'survived'}; {records} diagnostic spam lines, "
          f"{total} console bytes in {now - started:.0f}s (mode {mode})")
    if not records:
        print("  Check that the bench image was built with WARTHOG_USBDIAG_SPAM=1 and the console is responding.")
    return 1 if failure else 0


if __name__ == "__main__":
    sys.exit(main())
