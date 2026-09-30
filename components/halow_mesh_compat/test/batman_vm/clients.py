#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Make a Linux batman-adv node learn many LAN clients (s12).

  clients.py <iface> <first-mac> <count> [rounds=1]

Sends one broadcast frame (ethertype 0x88b5, local experimental) from each of <count>
consecutive source MACs starting at <first-mac> (last two bytes counted up), out of
<iface> into the node's bridge, so its bat0 adds each source to its local TT.
"""
import socket
import sys
import time


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    ifc, first, n = sys.argv[1], bytes(int(x, 16) for x in sys.argv[2].split(':')), int(sys.argv[3])
    rounds = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
    s.bind((ifc, 0))
    base = int.from_bytes(first[4:6], 'big')
    for _ in range(rounds):
        for i in range(n):
            src = first[:4] + ((base + i) & 0xffff).to_bytes(2, 'big')
            s.send(b'\xff' * 6 + src + b'\x88\xb5' + b'bvw-client'.ljust(46, b'\x00'))
            time.sleep(0.002)
    print('sent %d x %d' % (n, rounds))
    return 0


if __name__ == '__main__':
    sys.exit(main())
