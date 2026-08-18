# Bench harness

Hardware tests for the 802.11s HaLow mesh. They drive real boards over the AT
console (USB CDC) and assert on counters the firmware exposes, because the log
console is unreachable once the app boots -- the AT interface takes over the
USB-Serial-JTAG PHY.

    ./flash.sh "WTHG-0272A1F8738D 0-1 1" "WTHG-021BF681BA51 0-1 2" ...
    python3 mesh_e2e.py [settle_seconds]     # peering, unicast matrix, multicast, MeshPackets
    python3 mesh_churn.py                    # node leaves and rejoins

`mesh_e2e.py` needs the `meshtastic` package for the MeshPacket stage
(`meshtastic.protobuf.mesh_pb2`); everything else needs only `pyserial`.

Arguments to `flash.sh` are `"<serial> <uhubctl-hub> <port>"` triples. It drives
each board into ROM with AT+DLMODE, flashes, then power-cycles that one port.
Two things it encodes, both learned the hard way:

- A board can enumerate more than one CDC port and only one answers AT, so it
  probes for the live one rather than taking the first.
- Two `uhubctl -a cycle` calls on sibling ports of the VIA 2109:2817 hub within
  ~30 s have twice taken the WHOLE hub off the bus (every board, plus the SDR
  and the MM8108, until a physical replug). Hence: strictly serial, one port at
  a time, with a settle gap.

Boards also drop off USB on their own occasionally; a ~20 s power-off recovers
them.

## What passing looks like

`mesh_e2e.py` on three boards: every board holds 2 established links, all six
ordered unicast pairs pass, every sender's multicast reaches both peers, and
real Meshtastic MeshPackets arrive byte-exact. Single-packet losses (4/5) are
RF on a sub-GHz link, not a regression -- a repeat run should move which pair
shows them. A pair that fails *repeatedly*, or 0/N, is a real fault.

`mesh_churn.py`: survivors must drop to 1 link and KEEP passing traffic while a
peer is off, and all six pairs must pass again after it returns.

## Reference numbers (3 boards, 1 MHz channel, 2026-08-16)

Baselines to compare against, not targets:

    mesh_e2e    15/16 typical; the miss a single dropped ICMP packet
    mesh_churn  survivors keep passing 4/4 while a peer is off;
                all six pairs 4/4 after it returns
    mesh_soak   5 rounds x 3 boards: 0 dead pairs, estab=2 held throughout,
                5 of 6 pairs 0% loss, one 13% (2 packets of 15)
                12 min x 2 boards: 14 rounds, 0 dead pairs, 2.4-4.8% loss
    mesh_perf   MTU 1500 on all netifs; ~400 kbps delivered goodput,
                offered rate meaningless (unpaced UDP outruns the link)

A few percent packet loss is normal on this link. What is NOT normal: a pair
that fails in every round, estab dropping below the peer count, or `no_slot` /
`expired` climbing while all boards are up and in range.
