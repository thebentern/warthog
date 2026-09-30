#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Queries over classic Ethernet pcaps written by the batman_vm scenarios.

  pcapq.py count   <pcap> [--src M] [--dst M] [--type 0xNN] [--minlen N]
                   -> "count=<n> maxlen=<bytes>" (frames of ethertype 0x4305; --type = batman byte 0)
  pcapq.py extract <pcap> --src M --type 0xNN [--len N] [--skip K] [--tt-change]
                   -> hex of the batman bytes (after the 14-byte link header) of the first match
  pcapq.py fragfwd <pcap> --in-src M --out-src M
                   -> "in=<n> matched=<n>": fragments (0x41) arriving from in-src that leave again
                      from out-src with identical bytes except TTL - 1
  pcapq.py dhcpuc  <pcap> --src M
                   -> "count=<n>": UNICAST (0x40) frames from src whose client frame is UDP 67 -> 68
  pcapq.py bcast   <pcap> --src M
                   -> "bcasts=<n> min=<copies> max=<copies>": BCAST (0x01) frames from src, grouped
                      by (originator, sequence number)
  pcapq.py bcgap   <pcap> --src M
                   -> "bcasts=<n> copies=<min>..<max> mingap_ms=<x>": as bcast, plus the smallest
                      time between two copies of one broadcast (-1 with no second copy)
  pcapq.py grpgap  <pcap> --src M
                   -> "frames=<n> mingap_ms=<x> close=<k>": every batman frame from src to
                      ff:ff:ff:ff:ff:ff (ELP, OGM, BCAST), the smallest time between two in a row
                      (-1 with fewer than two), and how many BCAST frames follow a copy of another
                      broadcast by under 20 ms (broadcasts that were queued together)
  pcapq.py bcttl   <pcap> --src M --orig M
                   -> "count=<n> ttls=<t,...>": BCAST (0x01) frames from link source src whose
                      originator is orig, and the distinct TTLs they carry
  pcapq.py ttjoin  <pcap> --src W --orig L
                   -> "reqs=<n> answer_ms=<x> early=<0|1>": TT requests (UNICAST_TVLV 0x44, TT flags
                      0x02) from W to L, ms from the first to L's first TT response to W (-1: none),
                      and whether that first request left before W's first own OGM2
"""
import argparse
import struct
import sys


def frames_ts(path):
    """(time in microseconds, frame bytes) of every record."""
    with open(path, 'rb') as f:
        hdr = f.read(24)
        if len(hdr) < 24:
            return
        magic = struct.unpack('<I', hdr[:4])[0]
        endian = '<' if magic in (0xa1b2c3d4, 0xa1b23c4d) else '>'
        nano = magic in (0xa1b23c4d, 0x4d3cb2a1)
        while True:
            rh = f.read(16)
            if len(rh) < 16:
                return
            sec, frac, incl, _ = struct.unpack(endian + 'IIII', rh)
            data = f.read(incl)
            if len(data) < incl:
                return
            yield sec * 1000000 + (frac // 1000 if nano else frac), data


def frames(path):
    for _, data in frames_ts(path):
        yield data


def mac(s):
    return bytes(int(x, 16) for x in s.split(':'))


def bat(fr):
    return len(fr) >= 16 and fr[12:14] == b'\x43\x05'


def match(fr, a):
    if not bat(fr):
        return False
    if a.src and fr[6:12] != mac(a.src):
        return False
    if getattr(a, 'dst', None) and fr[0:6] != mac(a.dst):
        return False
    if a.type is not None and fr[14] != int(a.type, 0):
        return False
    return True


def has_tt_change(fr):
    """OGM2 whose first TT v1 container carries at least one change entry."""
    b = fr[14:]
    if len(b) < 20 or b[0] != 0x04:
        return False
    tl = struct.unpack('>H', b[14:16])[0]
    t = b[20:20 + tl]
    while len(t) >= 4:
        ty, ver, ln = t[0], t[1], struct.unpack('>H', t[2:4])[0]
        v = t[4:4 + ln]
        if ty == 0x04 and ver == 1 and len(v) >= 4:
            nv = struct.unpack('>H', v[2:4])[0]
            return len(v) - 4 - 8 * nv >= 12
        t = t[4 + ln:]
    return False


def main():
    p = argparse.ArgumentParser()
    p.add_argument('cmd')
    p.add_argument('pcap')
    p.add_argument('--src')
    p.add_argument('--dst')
    p.add_argument('--type')
    p.add_argument('--len', type=int)
    p.add_argument('--minlen', type=int, default=0)
    p.add_argument('--skip', type=int, default=0)
    p.add_argument('--tt-change', action='store_true')
    p.add_argument('--in-src')
    p.add_argument('--out-src')
    p.add_argument('--orig')
    a = p.parse_args()

    if a.cmd == 'count':
        n = mx = 0
        for fr in frames(a.pcap):
            if match(fr, a) and len(fr) >= a.minlen:
                n += 1
                mx = max(mx, len(fr))
        print('count=%d maxlen=%d' % (n, mx))
        return 0
    if a.cmd == 'extract':
        k = a.skip
        for fr in frames(a.pcap):
            if not match(fr, a):
                continue
            if a.len is not None and len(fr) != a.len:
                continue
            if a.tt_change and not has_tt_change(fr):
                continue
            if k:
                k -= 1
                continue
            print(fr[14:].hex())
            return 0
        return 1
    if a.cmd == 'fragfwd':
        ins, outs = [], []
        for fr in frames(a.pcap):
            if not bat(fr) or fr[14] != 0x41:
                continue
            if fr[6:12] == mac(a.in_src):
                ins.append(fr[14:])
            elif fr[6:12] == mac(a.out_src):
                outs.append(fr[14:])
        matched = 0
        for f in ins:
            want = f[:2] + bytes([(f[2] - 1) & 0xff]) + f[3:]
            if want in outs:
                outs.remove(want)
                matched += 1
        print('in=%d matched=%d' % (len(ins), matched))
        return 0
    if a.cmd == 'dhcpuc':
        n = 0
        for fr in frames(a.pcap):
            if not bat(fr) or fr[14] != 0x40 or fr[6:12] != mac(a.src):
                continue
            inner = fr[14 + 10:]
            if len(inner) < 14 + 28 or inner[12:14] != b'\x08\x00':
                continue
            ihl = (inner[14] & 0x0f) * 4
            ip = inner[14:]
            if ip[9] != 17 or len(ip) < ihl + 4:
                continue
            sport, dport = struct.unpack('>HH', ip[ihl:ihl + 4])
            if sport == 67 and dport == 68:
                n += 1
        print('count=%d' % n)
        return 0
    if a.cmd == 'bcast':
        seen = {}
        for fr in frames(a.pcap):
            if bat(fr) and fr[14] == 0x01 and fr[6:12] == mac(a.src) and len(fr) >= 28:
                k = fr[14 + 4:14 + 14]
                seen[k] = seen.get(k, 0) + 1
        v = list(seen.values()) or [0]
        print('bcasts=%d min=%d max=%d' % (len(seen), min(v), max(v)))
        return 0
    if a.cmd == 'bcgap':
        seen = {}
        for t, fr in frames_ts(a.pcap):
            if bat(fr) and fr[14] == 0x01 and fr[6:12] == mac(a.src) and len(fr) >= 28:
                seen.setdefault(fr[14 + 4:14 + 14], []).append(t)
        n = [len(v) for v in seen.values()] or [0]
        gaps = [b - a for v in seen.values() for a, b in zip(v, v[1:])]
        print('bcasts=%d copies=%d..%d mingap_ms=%.1f' %
              (len(seen), min(n), max(n), min(gaps) / 1000.0 if gaps else -1))
        return 0
    if a.cmd == 'grpgap':
        last, lastk, n, gap, close = None, None, 0, None, 0
        for t, fr in frames_ts(a.pcap):
            if not bat(fr) or fr[6:12] != mac(a.src) or fr[0:6] != b'\xff' * 6:
                continue
            n += 1
            k = fr[14 + 4:14 + 14] if fr[14] == 0x01 and len(fr) >= 28 else None
            if last is not None:
                gap = t - last if gap is None else min(gap, t - last)
                close += k is not None and lastk is not None and k != lastk and t - last < 20000
            last, lastk = t, k
        print('frames=%d mingap_ms=%.1f close=%d' % (n, gap / 1000.0 if gap is not None else -1, close))
        return 0
    if a.cmd == 'bcttl':
        n, ttls = 0, set()
        for fr in frames(a.pcap):
            if bat(fr) and fr[14] == 0x01 and fr[6:12] == mac(a.src) and len(fr) >= 28 and \
                    fr[14 + 8:14 + 14] == mac(a.orig):
                n += 1
                ttls.add(fr[14 + 2])
        print('count=%d ttls=%s' % (n, ','.join(str(t) for t in sorted(ttls))))
        return 0
    if a.cmd == 'ttjoin':
        w, l = mac(a.src), mac(a.orig)
        reqs, first_req, first_ogm, answer = 0, None, None, None
        for t, fr in frames_ts(a.pcap):
            if not bat(fr):
                continue
            b = fr[14:]
            if b[0] == 0x04 and fr[6:12] == w and len(b) >= 20 and b[8:14] == w and first_ogm is None:
                first_ogm = t
            if b[0] != 0x44 or len(b) < 25 or b[20] != 0x04:
                continue
            kind = b[24] & 0x0f
            if kind == 0x02 and b[10:16] == w and b[4:10] == l:
                reqs += 1
                first_req = t if first_req is None else first_req
            elif kind == 0x04 and b[10:16] == l and b[4:10] == w and first_req is not None and answer is None:
                answer = t
        early = first_req is not None and (first_ogm is None or first_req < first_ogm)
        print('reqs=%d answer_ms=%d early=%d' %
              (reqs, (answer - first_req) // 1000 if answer is not None else -1, 1 if early else 0))
        return 0
    p.error('unknown command')


if __name__ == '__main__':
    sys.exit(main())
