# Troubleshooting

Work outward from the physical layer. Most "the network is broken" reports on
this hardware are power, DNS, or a mismatch that produces no error at all.

## The board resets at random

Almost always power. The Seeed HaLow add-on feeds the radio PA from USB VBUS
through ~20 µF, and the PA's turn-on inrush collapses the rail. Fit a
470–1000 µF low-ESR capacitor across the XIAO's 5V and GND pads.

Rule this out before debugging anything else — it presents as firmware
instability.

## AT does not answer

- The board enumerates more than one CDC port. Try each; only one answers `AT`.
- Any terminal at 115200 8-N-1. CDC ignores baud, so the value does not matter.
- Boot logs go to USB-Serial-JTAG for about a second and then the app hands the
  shared PHY to USB-OTG. If you are watching the JTAG port you will see boot and
  then silence — that is normal. `scripts/watch.sh` follows the handoff.

## The link is up but nothing routes

Check DNS first. If `ping 8.8.8.8` works and `curl example.com` hangs, the
resolver handed out by DHCP is not reachable from where the uplink lands:

```
AT+DNS=8.8.8.8
```

Then confirm the uplink actually has an address:

```
AT+STATUS?
```

## Mesh: no peers at all

```
AT+MPMPEERS?
+MPMPEERS: (none) ... s1g_bcn=0
```

`s1g_bcn=0` means no mesh beacons are being heard. Channel, mesh ID, bandwidth
or operating class do not match. Every one of them must be identical across the
mesh, and a mismatch produces no error — just silence.

## Mesh: peer seen but never establishes

```
+MPMPEERS: ... 28bf74 llid=34244 plid=0 estab=0 opens=12
```

`plid=0` with `opens` climbing means we are sending Opens nobody answers. The
usual cause is that the peer still holds an established link from before your
node rebooted, and ignores Opens carrying a new link id.

Warthog recovers on its own: after 8 unanswered Opens it sends a Close and
restarts the handshake. If it does not recover, restart the peer's mesh.

## Mesh: established, but only broadcast works

The signature is distinctive — ARP arrives, pings do not, and the peer's
per-station transmit counter is frozen at a small number.

An 802.11s node will not send unicast to a neighbour it has no *path* to, and
peering does not create one. Check we are advertising ourselves:

```
AT+HWMPSTAT?
+HWMPSTAT: rx=234 preq_rx=75 preq_tx=142 prep_rx=159 prep_tx=75 parse_fail=0 not_ours=0
```

`preq_tx` should be climbing. `preq_rx` should match `prep_tx` — every request
aimed at us answered. `parse_fail` above 0 means frames are arriving in a shape
we do not understand; `AT+HWMPDUMP?` shows the bytes.

On the peer, `iw dev wlh0 mpath dump` should show a resolved next hop, not
`00:00:00:00:00:00`.

## Mesh: perfect peering, zero data in both directions

```
AT+MESHSEC?
+MESHSEC: 1 (keyed)
```

Keyed Warthog against an unencrypted peer. Peering is unaffected because it
happens in management frames; only data dies. `AT+MESHSEC=0` for an open peer
such as stock OpenMANET.

## Mesh: frames arrive but nothing is delivered

```
AT+FILTSTAT?
+FILTSTAT: drop=23 last=6 | short_fc=0 rts=0 beacon=0 short_hdr=0 no_ops=0
           unknown_sender=23 sa_is_us=0 dup=0
```

Names which of the receive filter's drop paths is firing.
`unknown_sender` means frames are arriving from a station not in the peer table.

## Counters that look alarming and are not

| Reading | Meaning |
|---|---|
| `delivered=0` in `AT+DATASTAT?` | Expected. Only incremented on a receive path this build does not take. Not evidence of anything. |
| `rx_data` barely moving while pings succeed | Expected. It counts frames reaching the datapath, not frames delivered to the IP stack. |
| `parse_fail` climbing on `AT+HWMPSTAT?` | Real. Investigate with `AT+HWMPDUMP?`. |

## Build fails right after adding a source file

A CMake reconfigure can surface stale link errors — undefined references in
`libwpa_supplicant`, or a missing IDF header. Clean once and rebuild.

If `pio run -t clean` then fails with `No module named 'esptool'`, the clean
removed `tool-esptoolpy`:

```bash
pio pkg install -e warthog-us
```
