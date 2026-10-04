# Troubleshooting

Work outward from the physical layer. Most "the network is broken" reports on
this hardware are power, DNS, or a mismatch that produces no error at all.

## The board resets at random

Almost always power. The Seeed HaLow add-on feeds the radio PA from USB VBUS
through ~20 µF, and the PA's turn-on inrush collapses the rail. Fit a
470–1000 µF low-ESR capacitor across the XIAO's 5V and GND pads.

Rule this out before debugging anything else — it presents as firmware
instability.

In mesh mode a HaLow chip restart (a failed chip health check) reloads the
chip and puts the mesh back instead of resetting the board: measured on air on
2026-10-03 on both `-meshvif` builds with `AT+CHIPRESTART`, back in about 1.1 s
with USB and the peer links up, 12 of 12 restarts in a soak. It still resets
the board when the reload fails, the chip interface cannot come back at its id,
the channel cannot be set, or `BSS_CONFIG` or `MESH_CONFIG` cannot be sent.
`AT+CHIPRESTART?` counts restarts and what did not go back
([Mesh Mode](Mesh-Mode#chip-restarts)).

## The board drops off USB

An `MMOSAL_ASSERT` in the HaLow stack ends in the ESP-IDF panic handler, from
any task, interrupt handler or critical section: it writes the core dump to
flash on its own stack (`CONFIG_ESP_COREDUMP_STACK_SIZE=1792`) and reboots,
with no console output on the way (`CONFIG_ESP_SYSTEM_PANIC_SILENT_REBOOT`,
`CONFIG_ESP_COREDUMP_LOGS` off). Measured on air on 2026-10-03 with
`AT+ASSERTTEST=at` and `=loop`: one reboot, back on USB in about 6 s with the
mesh up, `reset=PANIC crash_boots=1 safe=0`.

From the start of the app until USB has started, the RTC watchdog resets the
board after 60 s (`reset=WDT`). The HaLow start runs before USB: in mesh mode
with no link wait, in client mode with up to 20 s for the link. A normal boot
whose USB start fails leaves the watchdog running, so the board resets and
tries again; those resets are not crash boots, and after 2 of them the board
turns the watchdog off and runs without USB, HaLow up. Safe mode turns it off
either way. After 3 crash boots in a row (each after a panic or a watchdog
reset) the board starts in safe mode: USB, AT and the Wi-Fi AP come up with the
HaLow chip held in reset (a panic resets only the CPU, so the chip would
otherwise keep running as the last boot left it), and commands that need the
chip are not expected to work. `AT+RESET` or a power cycle leaves safe mode; a
normal boot clears the count once it has been up 60 s with the watchdog off.
Safe mode and the USB retries are not measured on air.

### After a panic

A panic resets the CPU only. The GPIO matrix keeps the chip's `SPI_IRQ` pin set
as a low-level interrupt, and a chip held in reset holds it low, so the GPIO
interrupt service would storm as soon as it is installed (measured: interrupt
watchdog, core dump task `ipc0` in `gpio_isr_loop`/`gpio_intr_service`).
`mmhal_init` sets `SPI_IRQ` and `BUSY` to no interrupt before it installs the
service, and `mmhal_wlan_deinit` does so before it holds the chip in reset;
measured with the `AT+ASSERTTEST` runs above. The GPIO block keeps its output
levels too, so after a panic the chip stays out of reset with its firmware
loaded until something drives `RESET_N` low; safe mode and the
`AT+ASSERTTEST=hang` stop do that first, with the interrupts off (host-tested
only).

After the board is back:

```
AT+ASSERT?
+ASSERT: count=1 kept=1 reset=PANIC up_s=<s> crash_boots=1 safe=0
+ASSERT: #0 pc=<hex> lr=<hex> line=<n> fileid=<hex> info=<hex>,<hex>,<hex>,<hex>
AT+COREDUMP?
+COREDUMP: reason=MMOSAL_ASSERT pc=<hex> fileid=<hex> line=<n>
```

`tools/assert_fileid.py <fileid>` names the source file;
`xtensa-esp32s3-elf-addr2line -pfiaC -e firmware.elf <pc> <lr>`, with the ELF of
the build that ran, names the assert and its caller. `AT+ASSERT=0` clears the
records, `AT+COREDUMP=0` the core dump
([AT reference](AT-Command-Reference#mesh)).

A soft reboot after an assert shows as `reset=PANIC` with a small `up_s`,
`count` and `crash_boots` one higher than just before, and an `AT+COREDUMP?`
reason, after `AT+COREDUMP=0`, whose pc, fileid and line match the newest
record. The records and the count are in RAM a reset keeps (`.noinit`); power
loss normally loses them, and whether a hub port power cycle does is not
measured, so a record read with `reset=POWERON` or `BROWNOUT` does not prove a
soft reboot. The core dump survives power loss.

If the board stays off USB for more than 4 minutes (three 60 s watchdog
periods and their boots), power-cycle it and read `AT+COREDUMP?`. A reason
starting `MMOSAL_ASSERT` means the panic ran and the CPU reset, and a later
boot stopped after the watchdog was turned off (USB started, or failed to
start three boots in a row) without coming up on USB. Another reason, such as
`abort() was called`, means a later boot panicked. The dump read before the
test, or none after `AT+COREDUMP=0`, means no panic was reached.

`AT+ASSERTTEST` fires an assert on demand. `AT+ASSERTTEST=hang` also stops the
next boots before the HaLow start, the chip held in reset: two `reset=WDT`
boots, then safe mode (`crash_boots=3 safe=1`).

### Task stacks

`AT+STACKS?` reports the least free stack of each task, in bytes; `drv`,
`spi_irq` and `health` start again with every chip restart, and their `_min`
fields keep the least of every instance (`health_min` after `AT+CHIPRESTART`
shows what the failure path left). The `health` task has 1024 words (4096
bytes): a restart takes it to about 1740 bytes used, and at 400 words
`AT+CHIPRESTART` ended in `stack overflow in task health` (the core dump's
reason). Measured on air on 2026-10-03, least free: `health` 2356 bytes during
`AT+CHIPRESTART` on the `-meshvif` builds; on both boards ESP-IDF's `wifi` task
4192-4388 and `warthog_led` 68-72 of 2048; the LED task has 3072 bytes (not
measured at that size).
The stack canary (`CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY`) checks the
lowest 16 bytes of a task's stack each time it is switched out and panics on an
overflow, so after a run read `AT+ASSERT?` `reset=` and `AT+COREDUMP?` too.

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

## USB networking stops after large pings

Measured on air 2026-10-03 (macOS over CDC-NCM, builds before the fix): after
`AT+RESET`, pings of 100, 1000 and 1472 bytes 3/3, 1473 bytes 1/3, 2000 bytes
0/3, then even 100-byte pings 0/3 until `AT+RESET`. `+USB:` `tx` and `drop`
stopped moving, `drop=0`: nothing reached the board, so nothing was answered.

NCM lets the host pack several frames into one transfer block: both fragments
of a ping over 1472 bytes, back-to-back TCP segments. TinyUSB's NCM driver
hands up one frame per `tud_network_recv_renew()` and the firmware never called
it, so each such block left a frame behind; once all 3 receive blocks held one,
no receive transfer was started again. The host model replays the measured run
with the old receive callback and gets the same: 5 of 12 fragments in, 2
datagrams reassembled (`AT+MTU?` `ip_reass=2`), then nothing. Builds from
2026-10-03 on call it from the receive callback, as esp_tinyusb's own glue does.

They also give the USB network class one owner, the TinyUSB task. lwIP's
thread used to write TinyUSB's transmit blocks while the TinyUSB task sent and
freed them (a frame lost, a block sent half-written, a block never freed).
lwIP now copies each frame into a 16-frame queue, which the TinyUSB task
drains when woken and after each of the network class's USB transfers
completes; at most one wake-up is queued in TinyUSB's 16-event queue. A frame
that finds the queue full is dropped (`drop_full`), never waited for on lwIP's
thread, so traffic arriving faster than USB carries it drops there and TCP
backs off.

`AT+STATUS?`, `+USBNET:` line ([AT reference](AT-Command-Reference)):

- `rx` above `rx_xfer`: the host packs blocks.
- `rx_idle_ms` rising past 1000 while the host pings: nothing is received;
  the old wedge looked like this.
- `tx_stall_ms` over 1000 with `txq` above 0: frames wait and the host is not
  reading them (asleep, or a wedged link); `drop_full` counts what the full
  queue turned away.
- `tx_busy_ms` over 1000: the USB class driver holds frames and no IN transfer
  has finished since. It rises first: the NCM driver takes up to 17 small
  frames (24 with the host's data interface off) before any wait in `txq`, and
  `tx` counts them. After the host turns the data interface off and on it can
  read 0 while the driver still holds frames, until lwIP sends another.
- `drop_nolink` rising: lwIP sent while the host was detached or suspended;
  normal across a replug.
- `rx_err` stays 0: it counts only with `CONFIG_ESP_NETIF_RECEIVE_REPORT_ERRORS`,
  which no build sets.

Measured on air 2026-10-03 (macOS over NCM, both boards): pings of 100 to 6000
bytes 3/3 with the link up throughout, bursts of 100 × 1400 and 40 × 6000 bytes
with 0% loss; `+USBNET` read `rx` above `rx_xfer` (the host packed blocks),
`drop_full` 0, `tx_stall_ms` 0. Also host-tested against TinyUSB 0.21's own NCM
and ECM drivers (`make -C components/halow_mesh_compat/test usbnet`, which the
CI runs after its build).

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

If instead the Warthog counts the frames arriving (`AT+DATASTAT?` `rx_data` and
`delivered` both climb, no drop counter moves) and the node's `morse_cli -i wlh0
stats` `TX fragment` rises, the node's chip is fragmenting them at a low rate.
`AT+DEFRAG?` `ok` climbs with each one reassembled, its drop counts at 0 (a build
without `AT+DEFRAG?` does not reassemble; flash a current one); see
[OpenMANET Interop](OpenMANET-Interop#fragmented-frames-on-low-rate-links).

The other direction: large frames from the Warthog to a node are lost while small
ones arrive, on a slow link or with `AT+FRAG` set, and the node counts `RX MPDUs
with MIC fail` or decrypts only part of each frame. The Warthog's chip
(firmware 1.17.6) is fragmenting them, and it delivers only part of what it
fragments (measured on air 2026-10-01 to 03):

- with host CCMP, fragments after the first that the Warthog cut are
  re-encapsulated by its chip, so the receiver finds no CCMP header or a MIC
  failure (captured with `AT+TXCAP` and `AT+RXCAP`); frames the chip cuts itself
  fail too (MIC failure at a Linux node; what the chip sends was not captured);
- with chip keys a frame in 2 fragments arrives; in 3, fragments 1 and 2 arrive
  32 octets longer and fragment 1 without More Fragments, and the frame is lost;
- with chip keys, a frame the chip cuts in 2 while the Warthog holds an
  originator Block Ack session with a Linux node on that TID is lost: the node
  drops fragments under its session.

Measured on 2026-10-03 at `AT+TXRATE=0,1`, an OpenMANET 1.8.0 Pi pinging the
Warthog with 1000 and 1472 bytes: host CCMP, `AT+SEALFIT=1` 8/8 and 8/8,
`AT+SEALFIT=0` 0/8 and 0/8; chip keys, `AT+HOSTFRAG=auto` 8/8 and 8/8 (the
session ended by a DELBA, 2 host fragments), `AT+HOSTFRAG=0` 0/8 and 0/8 (the
chip cut each reply in 2 under the session). Warthog to Warthog, chip keys, UDP
at 1000 and 1400 bytes: 5/5 each with either setting.

`AT+HOSTFRAG=auto` is the default on the chip-key builds (`warthog-mesh-sae`,
`-meshvif`; every other build defaults to `0`); a value stored earlier keeps
applying (`AT+HOSTFRAG?` `stored=`). It cuts in at most 2 by its own rule
(`rule=max2`, `cap_trim`, `cap_sub`), whatever `AT+SEALFIT` says. `AT+SEALFIT=1`
(the default) sends a sealed frame the chip cuts only at rates where the chip
delivers it: whole with host CCMP; in at most 2 fragments with chip keys; with
chip keys whole under the Warthog's Block Ack session on its TID, or before the
DELBA that ended one for a cut is through (`seal_ba`, not measured on air),
unless `AT+FRAG` is below the frame. Check:

- `AT+FRAG?`: on a chip-key build `0`, or 810 or more (816 for relayed or
  proxied frames, which carry Address Extension), and `0` toward Linux nodes
  with `AT+HOSTFRAG=0` (under the Warthog's Block Ack session a frame over the
  threshold is cut by the chip, counted `seal_nofit`, and lost); on a host-CCMP
  build `0`. A lower threshold makes the chip cut full-size frames at any rate
  (`AT+HOSTFRAG?` `many`, `seal_nofit` rising; a full-size host-sealed frame is
  1566 octets, 1578 with Address Extension).
- `AT+SEALFIT?` is `1`; `seal_trim` or `seal_sub` rise on a slow link, `seal_ba`
  with `AT+HOSTFRAG=0` and a Block Ack session (`AT+AMPDU?` `ours=`, or `asked=`
  while its ADDBA is unanswered), or with `auto` when a frame whose cut ended a
  session goes whole (`pool`).
- On a chip-key build: `AT+HOSTFRAG?` `mode=auto` (`0` set earlier makes it
  `off`; `AT+HOSTFRAG=auto` restores it), `rule=max2`, `msdu ok` rising with
  `msdu`, and `ba_end` with each session ended to cut. On a host-CCMP build
  `rule=off`.
- `AT+TXCAP=2,<node>`, then `AT+TXCAP?` after the traffic: each large frame's
  `r=` chain names only rates that carry it (whole, or in 2 with chip keys).
- `seal_sub` or `cap_sub` rise and large frames are still lost: the link closes
  only at 1 MHz MCS0, and those frames go at MCS1 (about 3 dB more) or MCS2
  (about 5 dB more; derived, not measured). A sending-host MTU of 1422 avoids
  MCS2 with host CCMP; 1322 keeps chip-key frames at MCS0 in 2 fragments.
- Group frames with `AT+MESHGRP=1` at 1 MHz: `AT+SEALFIT?` `grp_sub` rises for
  each one longer than 720 octets (about 660 bytes of IP), which goes at a
  faster rate; receivers drop group fragments.

See [OpenMANET Interop](OpenMANET-Interop#what-chip-firmware-1176-does-with-fragments).

## Large packets from a node at MTU 1460 go unanswered (IP fragments)

A host whose interface MTU is below the packet's size sends it as IP fragments.
An OpenMANET node's `bat0` has MTU 1460, and a node whose `br-lan` or `eth0` is
set to the same fragments every IP packet over 1460 bytes: pings from `-s 1433`
on (`-s 1432` is 1460 bytes whole). ESP-IDF's lwIP drops every fragment
addressed to itself (it forwards the rest as they came) unless built with
`CONFIG_LWIP_IP4_REASSEMBLY`, off by default. Measured on air 2026-10-03 over
two hours, both boards, every rate: from a Pi at MTU 1460, 1452- and 1472-byte
pings arrived as two fragments each and 0/5 were answered in each of 24 rounds;
the Pi at MTU 1500, whose 1472-byte pings were not fragmented, got replies.

Builds from 2026-10-03 on reassemble IPv4 (`main/nat_frag.c`, lwIP's IPv4 input
hook):

- At most 10 fragments are held at once (`CONFIG_LWIP_IP_REASS_MAX_PBUFS`), each
  datagram 3-4 s (`IP_REASS_MAXAGE` 3 on a 1 s timer). A datagram of more than
  10 fragments is never answered (at MTU 1460, pings over `-s 14392`).
- Held fragments are heap copies, under 16 KB for all 10; the radio receive
  buffer (in batman mode the delivery, `AT+BATSTAT?` `deliver_cap`) goes back at
  once. An echo reply reuses them rather than one heap block its full size.
- A fragment to a broadcast or multicast address is dropped and counted
  (`ip_reass_drop`): nothing on the Warthog takes one, and lwIP would hold it and
  answer it with ICMP time exceeded.
- IPv6 is not reassembled: no netif has an IPv6 link-local address, so ESP-IDF's
  default IPv6 input hook (`CONFIG_LWIP_HOOK_IP6_INPUT_DEFAULT`) drops every IPv6
  packet first.
- Batman mode: lwIP sends a datagram's fragments back to back, and bat0 takes 4
  frames at a time; each further one waits up to 100 ms for room, then is dropped
  with its datagram (`AT+BATSTAT?` `q_tx_full`).

Measured on air 2026-10-03, both boards: from a Pi at MTU 1460 and at 1500,
pings to the Warthog up to `-s 14392` 5/5. Host-tested on IDF 5.5.4's lwIP
(`make -C components/halow_mesh_compat/test lwip-napt`); bat0's wait on the host
harness only. Check:

- `grep REASS sdkconfig.<env>`: `CONFIG_LWIP_IP4_REASSEMBLY=y`,
  `CONFIG_LWIP_IP_REASS_MAX_PBUFS=10`. An older generated file says `is not
  set`; PlatformIO keeps it until it is edited or deleted.
- On the node, `ping -M do -s 1432 <warthog>` passes and `ping -s 1472
  <warthog>` fails: the image predates reassembly.
- `AT+MTU?` `ip_reass` rises by one per fragmented ping to the Warthog, in every
  mode.

Through NAT (a tethered host on USB or the AP): lwIP's NAPT reads ports from
every packet, and a fragment after the first has none. Stock NAPT sends such a
fragment out with the host's own address (ICMP), takes 8 bytes of its payload
for a UDP header and rewrites or drops it (UDP), or never turns it back to the
host. The Warthog therefore also reassembles a fragmented datagram NAPT would
rewrite before NAPT sees it, and lwIP cuts it again for the outgoing MTU.
`AT+MTU?` `ip_reass` counts datagrams the hook reassembled, `ip_reass_drop`
fragments or datagrams it dropped (a broadcast or multicast destination, a bad
header, no heap for the copy, a first fragment too short for its ports, the
tcpip mailbox full). Measured on air 2026-10-03: a Mac on USB pinged a Pi through
NAT with 100, 1472, 1473, 2000 and 6000 bytes, 3/3 each (one 6000-byte run 2/3),
`ip_reass` rising both ways and `ip_reass_drop` 0.

`ip_short_drop` counts whole TCP, UDP or ICMP packets, to the Warthog or through
NAT, dropped because they are shorter than their header (20, 8, 8 bytes), and UDP
packets without their destination port wherever they go. Stock NAPT reads that
header unchecked and, on a session match, rewrites its port and checksum past the
packet's end, also for a packet to 0.0.0.0 while HaLow has lost its lease. A rise
means a broken or hostile sender on the mesh, USB or the access point
(host-tested; `docs/napt-notes.md`, Short packets).

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
