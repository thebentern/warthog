#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Crafted and mutated batman frames for s08_hostile_live (design 6.4 item 3, live).

  hostile.py <iface> --w <engine hard MAC> --l <real neighbour originator> --fake <MAC>
             [--fuzz N] [--seed S]

Sent from the peer's end of the engine's veth, so only the engine sees them. Every
frame's link source is --fake (a made-up neighbour F) unless the case is about the
link source itself; made-up originators X/X2 are learnt through F, so the real
neighbour's sequence state is never touched. Layouts follow the clean-room spec
(packets.md). Prints one line per case: "<case> frames=<n> expect=<engine counter>".
"""
import argparse
import random
import socket
import struct
import sys
import time

BC = b'\xff' * 6


def mac(s):
    return bytes(int(x, 16) for x in s.split(':'))


class Tx:
    def __init__(self, ifc):
        self.s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
        self.s.bind((ifc, 0))
        self.n = 0

    def send(self, dst, src, body):
        f = dst + src + b'\x43\x05' + body
        self.s.send(f)
        self.n += 1
        if self.n % 32 == 0:
            time.sleep(0.004)

    def raw(self, frame):
        self.s.send(frame)
        self.n += 1


def elp(orig, seq, interval=500, pad=4):
    return b'\x03\x0f' + orig + struct.pack('>II', seq, interval) + b'\x00' * pad


def tvlv(t, v, val):
    return bytes([t, v]) + struct.pack('>H', len(val)) + val


def tt_val(flags, ttvn, vlans, entries=b''):
    v = bytes([flags, ttvn]) + struct.pack('>H', len(vlans))
    for crc, vid in vlans:
        v += struct.pack('>IHH', crc, vid, 0)
    return v + entries


def ogm(orig, seq, tvlvs=b'', ttl=50, tput=0xffffffff, tvlv_len=None):
    tl = len(tvlvs) if tvlv_len is None else tvlv_len
    return (b'\x04\x0f' + bytes([ttl, 0]) + struct.pack('>I', seq) + orig +
            struct.pack('>HI', tl, tput) + tvlvs)


def bcast(orig, seq, inner, ttl=49):
    return b'\x01\x0f' + bytes([ttl, 0]) + struct.pack('>I', seq) + orig + inner


def unicast(dest, inner, ttl=50, ttvn=0):
    return b'\x40\x0f' + bytes([ttl, ttvn]) + dest + inner


def utvlv(dest, src, tvlvs, ttl=50, tvlv_len=None):
    tl = len(tvlvs) if tvlv_len is None else tvlv_len
    return b'\x44\x0f' + bytes([ttl, 0]) + dest + src + struct.pack('>HH', tl, 0) + tvlvs


def frag(dest, orig, seq, num, total, data, ttl=50):
    return (b'\x41\x0f' + bytes([ttl, (num << 4) & 0xf0]) + dest + orig +
            struct.pack('>HH', seq & 0xffff, total) + data)


def icmp(dest, orig, mtype, ttl=50, uid=7, seq=1):
    return b'\x43\x0f' + bytes([ttl, mtype]) + dest + orig + bytes([uid, 0]) + struct.pack('>H', seq)


def eth(dst, src, et=0x0800, payload=b'\x45' + b'\x00' * 45):
    return dst + src + struct.pack('>H', et) + payload


def main():
    p = argparse.ArgumentParser()
    p.add_argument('iface')
    p.add_argument('--w', required=True)
    p.add_argument('--l', required=True)
    p.add_argument('--fake', required=True)
    p.add_argument('--fuzz', type=int, default=3000)
    p.add_argument('--seed', type=int, default=1)
    a = p.parse_args()
    rnd = random.Random(a.seed)
    W, L, F = mac(a.w), mac(a.l), mac(a.fake)
    X = F[:5] + bytes([F[5] ^ 0x10])
    X2 = F[:5] + bytes([F[5] ^ 0x20])
    U = F[:5] + bytes([F[5] ^ 0x40])          # never announced
    client = b'\x06\x66\x00\x00\x00\x01'
    tx = Tx(a.iface)
    r32 = lambda: rnd.getrandbits(32)

    def case(name, expect, frames):
        n0 = tx.n
        for dst, src, body in frames:
            tx.send(dst, src, body)
        print('%-28s frames=%-4d expect=%s' % (name, tx.n - n0, expect), flush=True)

    # F becomes a neighbour (two ELPs), X an originator routed via F (OGM with a valid TT).
    es = r32()
    case('setup-neighbour-F', 'elp_rx', [(BC, F, elp(F, es)), (BC, F, elp(F, es + 1))])
    time.sleep(0.8)                          # one own ELP later F has a throughput sample
    good_tt = tvlv(4, 1, tt_val(0x01, 1, [(0, 0)]))
    case('setup-originator-X', 'ogm_new', [(BC, F, ogm(X, r32(), good_tt))])
    time.sleep(0.2)

    case('elp-unicast-probe', 'elp_probe', [(W, F, elp(F, 0, 0, pad=184))] * 2)
    case('elp-own-originator', 'elp_own_orig', [(BC, F, elp(W, r32()))])
    case('link-src-own', 'rx_src_own', [(BC, W, elp(F, r32()))])
    case('link-src-group', 'rx_src_bad', [(BC, b'\x01\x00\x5e\x00\x00\x01', elp(F, r32()))])
    case('version-14', 'rx_version', [(BC, F, b'\x03\x0e' + elp(F, r32())[2:])])
    case('type-0x80', 'rx_type', [(BC, F, b'\x80\x0f' + b'\x00' * 30)])
    case('type-0x05-mcast', 'rx_type', [(W, F, b'\x05\x0f\x32\x00' + b'\x00' * 30)])
    tx.raw(W + F + b'\x43\x05\x04')          # 15 bytes: shorter than link header + 2
    print('%-28s frames=1    expect=rx_short' % 'short-frame', flush=True)
    case('unicast-short-header', 'rx_hdr', [(W, F, b'\x40\x0f\x32\x00' + W[:4])])
    case('elp-dst-unicast-ogm', 'rx_mgmt_dst', [(W, F, ogm(X, r32(), good_tt))])
    case('unicast-dst-not-us', 'rx_uni_dst', [(F, F[:5] + b'\x77', unicast(W, eth(BC, client)))])

    case('ogm-own-originator', 'ogm_own', [(BC, F, ogm(W, r32(), good_tt))])
    case('ogm-tvlv-overrun', 'ogm_overrun', [(BC, F, ogm(X, r32(), b'\x04\x01\x00\x02', tvlv_len=200))])
    case('ogm-bad-second-record', 'ogm_badrec',
         [(BC, F, ogm(X, r32(), good_tt) + b'\x55\x0f' + b'\x00' * 30)])
    case('ogm-tt-vlan-count-ffff', 'tt_bad',
         [(BC, F, ogm(X2, r32(), tvlv(4, 1, bytes([1, 1, 0xff, 0xff]) + b'\x00' * 8)))])
    time.sleep(0.05)
    case('ogm-tt-len-lt-4', 'tt_bad', [(BC, F, ogm(X2, r32(), tvlv(4, 1, b'\x01\x01')))])
    case('ogm-tt-partial-change', '(ignored)',
         [(BC, F, ogm(X, r32(), tvlv(4, 1, tt_val(0x01, 2, [(0, 0)], b'\x00' * 5))))])
    case('ogm-tput-zero', 'ogm_tput0', [(BC, F, ogm(X, r32(), good_tt, tput=0))])
    case('ogm-link-src-not-neigh', 'ogm_not_neigh', [(BC, U, ogm(X, r32(), good_tt))])

    inner = eth(BC, client)
    case('bcast-own-originator', 'bc_own', [(BC, F, bcast(W, r32(), inner))])
    case('bcast-ttl-0-and-1', 'bc_ttl', [(BC, F, bcast(X, r32(), inner, ttl=0)),
                                         (BC, F, bcast(X, r32(), inner, ttl=1))])
    case('bcast-unknown-originator', 'bc_unknown', [(BC, F, bcast(U, r32(), inner))])
    case('unicast-relay-ttl-0-and-1', 'uc_ttl', [(W, F, unicast(X, inner, ttl=0)),
                                                 (W, F, unicast(X, inner, ttl=1))])
    case('unicast-inner-short', 'uc_inner_bad', [(W, F, unicast(W, b'\x00' * 8))])
    case('unicast-relay-unknown-dst', 'uc_noroute', [(W, F, unicast(U, inner))])
    case('4addr-dat-subtype-2', 'uc_4a_dat',
         [(W, F, b'\x42\x0f\x32\x00' + W + X + b'\x02\x00' + eth(BC, client, 0x0806, b'\x00' * 28))])
    case('unknown-0x45-for-us', 'unk_self', [(W, F, b'\x45\x0f\x32\x00' + W + b'\x00' * 10)])
    case('unknown-0x45-relay-ttl-1', 'uc_ttl', [(W, F, b'\x45\x0f\x01\x00' + X + b'\x00' * 10)])
    case('icmp-ttl-1-echo-to-X', 'ic_ttlx', [(W, F, icmp(X2, X, 8, ttl=1))])
    case('icmp-ttl-0-reply-relay', 'uc_ttl', [(W, F, icmp(X2, X, 0, ttl=0))])
    case('icmp-tp-meter-for-us', 'ic_tp', [(W, F, icmp(W, X, 15) + b'\x00' * 8)])
    case('icmp-rr-full', 'ic_rr_full', [(W, F, icmp(W, X, 8)[:17] + b'\x10\x00\x01' + b'\x00' * 96)])

    case('utvlv-len-beyond-frame', 'ut_len', [(W, F, utvlv(W, X, b'\x04\x01\x00\x04' + b'\x00' * 4, tvlv_len=0x100))])
    case('utvlv-tt-bad-kind', 'tt_bad', [(W, F, utvlv(W, X, tvlv(4, 1, tt_val(0x08, 1, []))))])
    case('utvlv-tt-short', 'tt_bad', [(W, F, utvlv(W, X, tvlv(4, 1, b'\x02')))])
    case('utvlv-roam', 'tt_roam_rx', [(W, F, utvlv(W, X, tvlv(5, 1, client + b'\x00\x00')))])

    s = rnd.getrandbits(16)
    case('frag-total-0', 'fr_bad', [(W, F, frag(W, L, s, 0, 0, b'\x00' * 20))])
    case('frag-total-3000', 'fr_toobig', [(W, F, frag(W, L, s + 1, 0, 3000, b'\x00' * 20))])
    case('frag-zero-payload', 'fr_bad', [(W, F, frag(W, L, s + 2, 0, 100, b''))])
    case('frag-mismatched-totals', 'fr_bad', [(W, F, frag(W, L, s + 3, 0, 100, b'\x00' * 50)),
                                              (W, F, frag(W, L, s + 3, 1, 120, b'\x00' * 50))])
    case('frag-repeated-number', 'fr_dup', [(W, F, frag(W, L, s + 4, 0, 100, b'\x00' * 40)),
                                            (W, F, frag(W, L, s + 4, 0, 100, b'\x00' * 40))])
    case('frag-data-beyond-total', 'fr_bad', [(W, F, frag(W, L, s + 5, 0, 60, b'\x00' * 40)),
                                              (W, F, frag(W, L, s + 5, 1, 60, b'\x00' * 40))])
    case('frag-unknown-originator', 'fr_unknown', [(W, F, frag(W, U, s + 6, 0, 100, b'\x00' * 40))])
    case('frag-own-originator', '(dropped)', [(W, F, frag(W, W, s + 7, 0, 100, b'\x00' * 40))])
    case('frag-transit-ttl-1', 'fr_ttl', [(W, F, frag(X, L, s + 8, 0, 100, b'\x00' * 40, ttl=1))])

    def split(pkt, seq):             # two pieces; the head is the highest number
        h = len(pkt) // 2
        return [(W, F, frag(W, L, seq, 1, len(pkt), pkt[:h])), (W, F, frag(W, L, seq, 0, len(pkt), pkt[h:]))]

    case('frag-nested', 'fr_nested', split(frag(W, L, s + 20, 0, 60, b'\x00' * 40), s + 9))
    case('frag-reassembled-bcast', 'rx_type', split(bcast(X, r32(), inner), s + 10))
    case('frag-reassembled-0x45', 'unk_self', split(b'\x45\x0f\x32\x00' + W + b'\x00' * 30, s + 11))

    # Seeded mutations of every template above (payload bytes only, link header kept).
    templates = [
        (BC, elp(F, r32())), (BC, ogm(X, r32(), good_tt)), (BC, bcast(X, r32(), inner)),
        (W, unicast(W, inner)), (W, utvlv(W, X, tvlv(4, 1, tt_val(0x02, 1, [(0, 0)])))),
        (W, frag(W, L, s + 30, 1, 80, b'\x00' * 40)), (W, icmp(W, X, 8) + b'\x00' * 96),
        (W, b'\x42\x0f\x32\x00' + W + X + b'\x01\x00' + inner),
    ]
    n0 = tx.n
    for i in range(a.fuzz):
        dst, body = templates[rnd.randrange(len(templates))]
        b = bytearray(body)
        for _ in range(rnd.randint(1, 4)):
            op = rnd.randrange(3)
            if op == 0 and b:
                b[rnd.randrange(len(b))] ^= 1 << rnd.randrange(8)
            elif op == 1 and b:
                b[rnd.randrange(len(b))] = rnd.randrange(256)
            else:
                b = b[:rnd.randrange(len(b) + 1)] if rnd.random() < 0.5 else b + bytes(rnd.randrange(64))
        if len(b) < 2:
            b = bytearray(b'\x03\x0f')
        # never let a mutation name the real neighbour or the engine as an originator
        for off in (2, 4, 8, 10):
            if bytes(b[off:off + 6]) in (L, W) and not (dst == W and off == 4):
                b[off + 5] ^= 0x55
        tx.send(dst, F, bytes(b))
    print('%-28s frames=%-4d expect=(survive)' % ('fuzz', tx.n - n0), flush=True)
    print('total frames=%d' % tx.n, flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
