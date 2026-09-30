#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Inject batman bytes as 802.11s QoS data frames on a monitor interface (s09).

  inj80211.py <monitor-if> <shape> <ra> <ta> <batman-hex> [count=1]

Shapes (design 4.3; the same builders batvm_node --inject uses):
  rewrite  4-address, addr1 = addr3 = ra, addr2 = addr4 = ta, Mesh Control without AE
           (warthog's pre-batman per-peer copy of a group frame: DA rewritten to the peer)
  ae2      as rewrite, Mesh Control AE mode 2, addr5 = ff:ff:ff:ff:ff:ff, addr6 = ta
  group    3-address group frame, FromDS, addr1 = ff:ff:ff:ff:ff:ff, addr2 = addr3 = ta
With count > 1 the batman sequence number is advanced per copy (ELP bytes 8-11,
OGM2 bytes 4-7), so every copy is fresh to the receiver.
"""
import random
import socket
import struct
import sys

LLC = b'\xaa\xaa\x03\x00\x00\x00\x43\x05'
RADIOTAP = b'\x00\x00\x08\x00\x00\x00\x00\x00'


def mac(s):
    return bytes(int(x, 16) for x in s.split(':'))


def frame(shape, ra, ta, body, seq80211, meshseq):
    sc = struct.pack('<H', (seq80211 & 0xfff) << 4)
    qos = b'\x00\x01'                                   # TID 0, Mesh Control present
    ms = struct.pack('<I', meshseq & 0xffffffff)
    if shape == 'group':
        hdr = b'\x88\x02\x00\x00' + b'\xff' * 6 + ta + ta + sc + qos
        mc = b'\x00\x1f' + ms
    else:
        hdr = b'\x88\x03\x00\x00' + ra + ta + ra + sc + ta + qos
        mc = (b'\x02\x1f' + ms + b'\xff' * 6 + ta) if shape == 'ae2' else (b'\x00\x1f' + ms)
    return RADIOTAP + hdr + mc + LLC + body


def main():
    if len(sys.argv) < 6 or sys.argv[2] not in ('rewrite', 'ae2', 'group'):
        print(__doc__)
        return 2
    ifc, shape, ra, ta, body = sys.argv[1], sys.argv[2], mac(sys.argv[3]), mac(sys.argv[4]), bytes.fromhex(sys.argv[5])
    n = int(sys.argv[6]) if len(sys.argv) > 6 else 1
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
    s.bind((ifc, 0))
    off = {0x03: 8, 0x04: 4}.get(body[0])
    for i in range(n):
        b = bytearray(body)
        if off is not None and i:
            v = (struct.unpack('>I', b[off:off + 4])[0] + i) & 0xffffffff
            b[off:off + 4] = struct.pack('>I', v)
        s.send(frame(shape, ra, ta, bytes(b), random.getrandbits(12), random.getrandbits(32)))
    print('sent %d %s type 0x%02x' % (n, shape, body[0]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
