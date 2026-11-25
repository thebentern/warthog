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
them. One cause, fixed in images that carry the H6 fix, was opening and closing
the console port while the USB network carries traffic
(`docs/test-sweep-2026-11-10.md`, "The USB function stops when the console port
is opened and closed under network load"; `wiki/Troubleshooting.md`, "The board
drops off USB").

## The H6 tests

To rerun after any change to `components/esp_tinyusb`. The tools are in `h6/`;
build the two host programs once with `make -C h6` (needs libusb-1.0). Boards
are named by USB serial. The scripts were moved here from a session scratch
directory on 2026-11-11 and rewritten to stand alone; the bench was unplugged
before these copies could be run against a board, so expect to fix small things
on first use. The numbers below were measured with the originals.

1. Port opens under load, on an ordinary image:

       python3 h6/usb_console_load.py reopen 240 <dest-ip> <usb-serial>...

   Pushes 1.5 Mbit/s of UDP and ten 1400-byte pings a second through each
   board's USB network interface to `<dest-ip>`, and opens and closes each
   console port once a second, sending nothing. `RESULT pass` requires both
   traffic sources to send packets on every board's interface. Echo replies
   are optional. Without the slave mode define the whole USB function stops
   within two minutes and needs a power cycle.
2. Command sessions under the same load:

       python3 h6/usb_console_load.py commands 70 <dest-ip> <usb-serial>...

   Opens each port once a second, sends `AT+ASSERT?`, `AT+HEAP?` and
   `AT+STATUS?`, reads the replies and closes it. Without the one packet cap in
   `tusb.c` the console goes silent (network still up) in roughly one run in
   10 to 25, so a few clean runs prove nothing.
3. The fast version of 2, the one to judge a change with. It needs a bench
   image, built into its own directory so the normal build is left alone:

       PLATFORMIO_BUILD_DIR=/tmp/warthog-h6bench PLATFORMIO_BUILD_FLAGS='-DWARTHOG_USBDIAG=1 -DWARTHOG_USBDIAG_SPAM=1 -DWARTHOG_USBDIAG_DEST=\"<host ip>\"' pio run -e warthog-mesh-sae-meshvif
       python3 h6/usb_stress.py <usb-serial> 200 bad

   `main/usbdiag.c` (compiled to nothing without the flag) writes a 90-byte
   line to the console every tick from core 1, and the script holds the port
   open while `h6/usbctl` sends about 2,000 control requests a second that the
   board refuses. A pass requires complete diagnostic `spam` lines throughout
   the run. A four-second gap fails even if warnings keep arriving. Without
   the cap the stream stops within 150 s, usually within 20 s. The modes
   `getcoding` and `none` are controls and pass either way.

Run the hardware-free script regression tests from the repository root.

    python3 -m unittest discover -s tools/bench/h6

A console that has gone silent comes back with `h6/usbprobe reset <usb-serial>`
(a port reset; the board does not reboot). The bench image also streams the
USB controller's registers to UDP port 5005 at `<host ip>` over the HaLow side,
four times a second, so a board whose USB has stopped can still be read:
`python3 h6/usb_diag_listen.py <outfile> <seconds>` logs it and explains the
fields. Do not flash a bench image on a board you are handing back.

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

## openmanet_interop.py — Warthog against real OpenMANET

The other scripts here drive warthogs only, which cannot show interop. This
one puts OpenMANET nodes in the loop, configures both sides the idiomatic way
(uci, not a hand-run supplicant) and asserts, exiting non-zero on failure.

```bash
./openmanet_interop.py \
  --pi root@10.41.254.1 --pi-bind 10.41.0.230 \
  --pi root@10.41.254.1 --pi-bind 10.41.0.214 \
  --warthog /dev/cu.usbmodem11101 \
  --sae --negative
```

`--pi-bind` is not optional in practice: both Pis answer on 10.41.254.1 over
their own USB gadget, so the source address is what selects which one you
reach.

It writes uci on the peer — point it at bench hardware.

What it covers, and what it deliberately only reports:

- **Asserts** peer configured, warthog channel pin actually applied, peering
  on both sides, and ICMP in both directions.
- **Reports** `ae=`, `fwdcand=` and `nodec grp=` without asserting. These are
  the numbers that decide what to build next rather than pass/fail states:
  `ae>0` means a bridged peer is reaching us, `fwdcand>0` means the chip
  delivers third-party frames and 802.11s forwarding is host-side work (zero
  in an ordinary run only means nothing asked us to relay — the experiment
  that answers it is in `docs/mesh-attachment-model.md`), and
  `nodec grp` climbing means group frames are arriving undecryptable.
- `--negative` exercises the mismatch cases the scope calls for: an
  out-of-band frequency, an illegal bandwidth, and an empty mesh ID.

Not yet covered: wrong-passphrase and gate-reboot behaviour, and multicast
(CoT/mDNS do not cross Warthog at all — see the README).
