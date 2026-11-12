#!/usr/bin/env python3
"""Log the register stream of WARTHOG_USBDIAG bench images (main/usbdiag.c).

    usb_diag_listen.py <outfile> <seconds>

The host must hold the address the image was built with (WARTHOG_USBDIAG_DEST) on a network the boards'
HaLow side reaches. One line per datagram: host time, sender address, then the board's line. Fields
worth knowing when a console has gone silent: i2=<DIEPCTL>/<DIEPINT>/<DIEPTSIZ>/<DTXFSTS> is the console's
IN endpoint (bit 31 of the first word: armed; the last word: words free in its 16-word TX FIFO), inbusy
is TinyUSB's view of it, and poll=a/b shows 0x2010 bits when the host is polling and getting NAKs.
"""
import socket
import sys
import time

if len(sys.argv) < 3:
    sys.exit(__doc__)
out, secs = sys.argv[1], float(sys.argv[2])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("", 5005))
s.settimeout(0.5)
t0 = time.time()
n = 0
with open(out, "w") as f:
    while time.time() - t0 < secs:
        try:
            d, a = s.recvfrom(2048)
        except socket.timeout:
            continue
        n += 1
        f.write(f"{time.time():.3f} {a[0]} {d.decode(errors='replace')}\n")
        f.flush()
print("datagrams", n)
