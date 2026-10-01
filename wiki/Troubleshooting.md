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
mesh, and a mismatch produces no error — just silence. A peer in beaconless
mode sends no beacons and reads the same; `AT+MESHCFG?` names that case when
its probe requests name our mesh.

## Mesh: peer seen but never establishes

```
+MPMPEERS: ... 28bf74 llid=34244 plid=0 estab=0 opens=12
```

`plid=0` with `opens` climbing means the node is sending Opens nobody answers. The
usual cause is that the peer still holds an established link from before your
node rebooted, and ignores Opens carrying a new link id.

Warthog recovers on its own: after 8 unanswered Opens it sends a Close and
restarts the handshake. If it does not recover, restart the peer's mesh.

## Mesh: a fifth node never peers

A node peers with at most 4 others ([Peer capacity](Mesh-Mode#peer-capacity)).
On the full node `AT+PEERS?` shows `count=4`, and on a SAE mesh `offer_full`
climbs in `AT+MPMPEERS?`. On an open mesh a Warthog newcomer shows
`close_reason=53` in `AT+MPMSTAT?`, retries every 30 s, and peers once one of
the 4 leaves or is expired (30 s of silence). On a SAE mesh a peer that goes
silent is never expired, so its slot frees only when a Close from it is heard,
when it starts SAE again, or when the full node restarts. A node carried past 4
others it no longer hears ends up here ([Batman Mode](Batman-Mode#limits)).

## Mesh: established, but only broadcast works

The signature is distinctive — ARP arrives, pings do not, and the peer's
per-station transmit counter is frozen at a small number.

An 802.11s node will not send unicast to a neighbour it has no *path* to, and
peering does not create one. Check that the node is advertising itself:

```
AT+HWMPSTAT?
+HWMPSTAT: rx=234 preq_rx=75 preq_tx=142 prep_rx=159 prep_tx=75 parse_fail=0 not_ours=0 rann_rx=0 perr_rx=0
```

`preq_tx` should be climbing on an open mesh. It counts only the per-peer
keepalive PREQ, which SAE does not send, so under SAE it stays 0. `prep_tx`
should keep up with `preq_rx` minus `not_ours` (requests for other nodes) —
every request aimed at us answered. `parse_fail` above 0 means frames are
arriving in a shape the node does not understand; `AT+HWMPDUMP?` shows the bytes.

On the peer, `iw dev wlh0 mpath dump` should show a resolved next hop, not
`00:00:00:00:00:00`.

In batman mode the batman tables can look healthy both ways while nothing
unicast arrives from the peer: `AT+BATO=<its originator>` reads `ttvn=-` with
`tt_req_tx` rising. Under SAE, check the node's path to the Warthog: the mpath
above stays `RESOLVING` with next hop `00:00:00:00:00:00` and a climbing
`DRET`. On the Warthog, `AT+MESHFWDSTAT?` `hwmp gp` then stays flat while
`mgmt gp nodec`, `key` or `replay` rise: the node's group PREQs are refused
(`nodec` on a build without host CCMP, or with it off). An image that prints
`bipfail` there predates the fix for group path selection sent with
group-addressed privacy and refuses every such PREQ
([Batman Mode](Batman-Mode#measured-on-air)). If `hwmp gp` rises, `prep_tx`
must keep up with it.

## Mesh: small packets from a Linux node arrive, large ones never do

Pings from an OpenMANET node pass with small payloads and fail every time from
about 900 bytes, and the Warthog counts nothing arriving (no `micfail` in
`AT+SWCCMP?`, no `uc_rx` in `AT+BATSTAT?`); another node's may pass. The node's
RTS threshold (1000 on both OpenMANET 1.8.0 bench Pis) puts an RTS/CTS exchange
before those frames. On a STA chip interface, which every build but
`warthog-mesh-sae-swccmp-meshvif` runs the mesh on, the Warthog's CTS is taken
only by the peer the chip registered last; every other node times out and never
sends the frame. Measured on 2026-09-30 with the chips' MAC counters on both
ends.

Fix: flash `warthog-mesh-sae-swccmp-meshvif`; `AT+MESHCFG?` must read
`chip_vif=mesh(5)`. On other builds, on each node:

```sh
echo Y > /sys/module/mm6108_sdio/parameters/enable_cts_to_self
# or
iw dev wlh0 info | grep wiphy      # wiphy N: the phy is phyN
iw phy phyN set rts off
```

Both measured set at runtime on 2026-09-30; neither survives a reboot.
`mm6108_sdio` is the bench Pis' MM6108 SDIO driver; the module name follows the
chip and bus (`ls /sys/module/*/parameters/enable_cts_to_self`). Frames from the
Warthog are unaffected: a node's RTS threshold governs only what that node
sends. See
[OpenMANET Interop](OpenMANET-Interop#frames-over-about-1000-bytes-from-a-linux-node).

## Mesh: perfect peering, zero data in both directions

```
AT+MESHSEC?
+MESHSEC: 1 (keyed)
```

Keyed Warthog against an unencrypted peer. Peering is unaffected because it
happens in management frames; only data dies. `AT+MESHSEC=0` for an open peer
(an OpenMANET node only if an operator set `encryption='none'`; its mesh
wizard configures SAE).

## Mesh: frames arrive but nothing is delivered

```
AT+FILTSTAT?
+FILTSTAT: drop=23 last=6 | short_fc=0 rts=0 beacon=0 short_hdr=0 no_ops=0
           unknown_sender=23 sa_is_us=0 dup=0 not_ours=0
+FILTSTAT: mgmt_nours=0 last=00000000000000000000000000000000
```

Names which of the receive filter's drop paths is firing.
`unknown_sender` means frames are arriving from a station not in the peer table.
`not_ours` counts unicast data between two other stations that the radio overheard;
it rises with neighbouring traffic and is not a fault. `mgmt_nours` on the second
line counts unicast management frames between two other stations, which are not
dropped; whether the radio hands those up at all is not yet measured. On an SAE
mesh with host CCMP on, each Protected one from a peer also counts as `micfail`
in `AT+SWCCMP?`.

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
