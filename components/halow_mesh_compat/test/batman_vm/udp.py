#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""UDP multicast / broadcast helper for the batman_vm scenarios.

  udp.py recv <group|bcast> <port> <local-ip> <timeout-s> <token>
      Joins <group> on the interface holding <local-ip> (or, for "bcast", just binds the
      port), waits for a datagram whose payload contains <token>, prints it, exits 0;
      exits 1 on timeout. Prints "ready" once bound.
  udp.py send <dst> <port> <local-ip> <token> [count=3] [gap-s=0.3]
      Sends <count> datagrams "<token> #i" <gap-s> apart from <local-ip>; multicast via
      IP_MULTICAST_IF, broadcast with SO_BROADCAST.
"""
import socket
import struct
import sys
import time


def recv(group, port, local, timeout, token):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('', port))
    if group != 'bcast':
        mreq = struct.pack('4s4s', socket.inet_aton(group), socket.inet_aton(local))
        s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    print('ready', flush=True)
    end = time.time() + timeout
    while True:
        left = end - time.time()
        if left <= 0:
            print('timeout', flush=True)
            return 1
        s.settimeout(left)
        try:
            data, src = s.recvfrom(4096)
        except socket.timeout:
            continue
        if token.encode() in data:
            print('got %r from %s' % (data.decode(errors='replace'), src[0]), flush=True)
            return 0


def send(dst, port, local, token, count, gap):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.bind((local, 0))
    first = int(dst.split('.')[0])
    if 224 <= first <= 239:
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(local))
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 4)
    else:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    for i in range(count):
        s.sendto(('%s #%d' % (token, i)).encode(), (dst, port))
        if gap > 0:
            time.sleep(gap)
    return 0


def main():
    if len(sys.argv) >= 7 and sys.argv[1] == 'recv':
        return recv(sys.argv[2], int(sys.argv[3]), sys.argv[4], float(sys.argv[5]), sys.argv[6])
    if len(sys.argv) >= 6 and sys.argv[1] == 'send':
        n = int(sys.argv[6]) if len(sys.argv) > 6 else 3
        gap = float(sys.argv[7]) if len(sys.argv) > 7 else 0.3
        return send(sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5], n, gap)
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main())
