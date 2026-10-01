# Batman Mode — a BATMAN_V member on an OpenMANET mesh

`AT+MESHBATMAN=1` makes a mesh-mode Warthog a batman-adv **BATMAN_V** member
(compat version 15). It then joins a mesh built by OpenMANET's mesh wizard, where
the 802.11s interface is a hard interface of `bat0`:

- it appears in `batctl n` and `batctl o` on the OpenMANET nodes, and they in its
  own tables;
- its bat0 MAC is announced in batman's translation table, so the nodes'
  `bat0`/`br-ahwlan` side reaches it and it takes a DHCP lease from the first
  node whose DHCP server answers (else a static address, below);
- it routes and relays batman traffic for other nodes (multi-hop is batman's,
  802.11s forwarding stays off, as on the wizard nodes);
- Meshtastic's `239.0.0.69:4403` from its tethered side floods the whole batman
  mesh; it is the only multicast group that crosses the Warthog ([Limits](#limits)).

Off by default. The protocol is a clean-room implementation (`main/bat/`, written
from a specification and captures, no batman-adv source); batman-adv, batctl and
alfred are not part of the image.

**Status.** Measured on air on Warthogs on 2026-09-29 and 2026-09-30
(`warthog-mesh-sae-swccmp`, [Measured on air](#measured-on-air)) against two
OpenMANET 1.8.0 Pis, batman-adv 2025.4 (`2025.4-openwrt-2`), on an SAE mesh with
`ieee80211w=2`, every node one hop from every other: neighbours, originators
and translation tables both ways, the Warthogs choosing the gateway Pi (`gw=`
10.0/2.0), a DHCP lease from a Pi, pings from a Pi's LAN and between Warthogs,
and Meshtastic's group reaching a Pi's LAN. The Pis' `bat0` was set up by
hand at runtime, not by the wizard, with openmanetd stopped. A Linux node's
unicast above about 1000 bytes arrives from every node only on
`warthog-mesh-sae-swccmp-meshvif` or with a setting on the node; on other builds
only from the peer the chip registered last ([Limits](#limits)).

Before that: host tests (engine unit, golden-capture and multi-engine simulator
tests; the firmware port, `bat_port.c`, the AT setters and the bat0 addressing
in `mesh.c`, run against FreeRTOS, ESP-IDF and morselib fakes and a model of
lwIP's DHCP client; the air frames through the real 802.11s datapath, host CCMP
included: `make -C components/halow_mesh_compat/test`); batman-adv 2024.3 in a
Linux VM over veth links, including the gateway and router lookups bat0
addressing makes; the broadcast frame shape against mac80211
(`mac80211_hwsim`); and the engine inside a Linux host program on one OpenMANET
Pi, against batman-adv 2025.4 on a second Pi over real HaLow, through that Pi's
Linux 802.11s stack and radio. Run 3 (2026-09-28):
neighbours, routes, translation table, ping, fragments, DHCP, multicast, restarts,
a gateway appearing, changing and leaving (the gateway count 0, 1, 0), a 5-minute
soak (10 minutes in an earlier run), and broadcasts sent in the same millisecond,
in pairs and in bursts of 4 and 6. Run 4 (2026-09-28, the current engine): six
fresh joins (table held 1.1-1.7 s after start, at most 2 requests; the one join
whose first request went out before our first OGM was answered 0.95 s later, run 3
took 3.1 s), three kills with the sequence-number record kept (ELP and OGM +257 on
air, every broadcast after each start delivered, first ping 1-2 s) against a
control without it (a restart 12 s after the previous one went 18 s without
replies), pings, bursts and a 3-minute soak. The full-table request backoff, the
TTVN its unicast carries toward a node whose table it cannot take and a count of
two gateways have run on the host and in the VM only (one hop, tables in sync).
`warthog-us`, `warthog-mesh-smoke`, `warthog-mesh-sae`,
`warthog-mesh-sae-swccmp` and `-swccmp-meshvif` build. Only
`warthog-mesh-sae-swccmp` and `-swccmp-meshvif` have run batman mode on a board.

## Builds

| Build | Mesh security | Hears Linux peers' batman broadcasts | `AT+MESHBATMAN=1` |
|---|---|---|---|
| region images with `AT+MESHEN=1`, `warthog-mesh-smoke` | open with `AT+MESHSEC=0`; the default `1` keys the mesh with a key published in the source, which no Linux node joins | yes (plain group frames) | accepted |
| `warthog-mesh-sae` | SAE, chip crypto | **no**: peers' group frames are dropped | **refused** (`sae-no-host-ccmp`) |
| `warthog-mesh-sae-nochipkey` | SAE, no host CCMP | no | **refused** |
| `warthog-mesh-sae-meshvif` | SAE, chip crypto on a MESH chip interface, each peer's MGTK in the chip at its AID | in the chip ([OpenMANET Interop](OpenMANET-Interop#group-frames-in-the-chip-warthog-mesh-sae-meshvif)) | **refused** (`sae-no-host-ccmp`) |
| `warthog-mesh-sae-swccmp`, `-swccmp-on` | SAE + host CCMP | through host CCMP (`-swccmp` measured on air, 2026-09-29) | accepted; batman mode arms host CCMP at boot |
| `warthog-mesh-sae-swccmp-meshvif` | as `-swccmp`, on a MESH chip interface: takes a node's frames above its RTS threshold ([Limits](#limits)) | through host CCMP (measured on air, 2026-09-30) | accepted, as `-swccmp` |

Every ELP, OGM and broadcast a Linux node sends is an 802.11s group frame under
its own group key. Only the host-CCMP builds decrypt those. Against the wizard's
default (SAE) mesh use `warthog-mesh-sae-swccmp`; against a node set to
`encryption='none'` any open build works, with `AT+MESHSEC=0`.

## Setting it up against a wizard node

Match the node's mesh first — mesh ID, channel, bandwidth, passphrase — as in
[OpenMANET Interop](OpenMANET-Interop); leave the node as the wizard configured
it (do **not** take `wlh0` out of `bat0`). Then on the Warthog:

```
AT+MESHEN=1
AT+MESHSEC=0      open builds only, against a node set to encryption='none'
AT+MESHFWD=0
AT+MESHBRIDGE=0
AT+MESHBATMAN=1
AT+RESET
```

After boot:

```
AT+MESHBATMAN?
+MESHBATMAN: stored=1 running=1 reason=ok
+MESHBATMAN: bat0 addr=leased ip=10.41.102.111 router=10.41.0.10(ok) retry_in=- retries=0 restarts=0 gw=02:b0:1c:00:00:01(10.0/2.0)
AT+BATN?          neighbours: one line per OpenMANET node heard
AT+BATO?          originators and the next hop to each
```

On the node, `batctl n` lists the Warthog's mesh MAC and `batctl tg` its bat0
MAC (below).

Two node settings the wizard makes, and a node configured by hand may not:

- `bat0` must run `routing_algo` BATMAN_V. A `bat0` made by hand defaults to
  BATMAN_IV, whose OGMs the Warthog drops (counted as `rx_type` in
  `AT+BATSTAT?`); `AT+BATN?` then stays empty.
- 802.11s forwarding should be off on every Linux node (mesh11sd `mesh_fwding '0'`).
  A batman frame another 802.11s node relayed is dropped and counted as
  `rx_relayed`, and with `AT+MESHGRP=1` a forwarding node re-floods the Warthog's
  ELP, so nodes two hops away list it as a direct neighbour. `rx_relayed` above 0
  means some node forwards at 802.11s; on its own it is not lost traffic, since the
  direct copy usually arrives too. The cost shows beyond one hop, where a frame
  that reaches the Warthog only as a relayed copy is lost. On air, with every node
  in range and `AT+MESHGRP=0`, batman worked with forwarding on
  ([Measured on air](#measured-on-air)).

On every build but `warthog-mesh-sae-swccmp-meshvif`, one more node setting,
which the wizard does not make: with the RTS threshold of 1000 both bench
OpenMANET 1.8.0 Pis ran, a node's unicast above about 1000 bytes reaches the
Warthog only if the Warthog's chip registered that node last. Set each node to
CTS-to-self or RTS off ([Limits](#limits)).

## What changes on the Warthog

| | Batman mode |
|---|---|
| HaLow netif | becomes the soft interface (bat0): MAC = the factory MAC with byte 0 set to `06`, MTU 1460 (OpenMANET's `bat0`); the mesh MAC stays the originator address |
| Frames on the mesh | only batman (ethertype 0x4305); anything else received is dropped and counted (`rx_nonbat`). **Batman mode is a per-mesh choice**: a node that sends plain IP over 802.11s (a non-batman Warthog, an OpenMANET node without the wizard) is unreachable at L3 |
| Address | brought up when the first originator has a route. DHCP first (`AT+MESHDHCP=1`, default), for 45 s from that route (up to 10 s more if an offer is still being taken). Without a lease, a static `10.41.253.x/16` with gateway `10.41.0.1` (the address openmanetd gives only a gate): each candidate is ARP-probed before use and the next one tried if it answers; if 8 in a row answer, the ninth is taken unprobed. `x` comes from the factory MAC's last three bytes without their two lowest bits, so boards from one batch get different addresses. `10.41.253.0/24` is the range OpenMANET leaves to hand-assigned static addresses, so an operator's own static there can still collide. While a static address is held, DHCP is asked again beside it: 30 s after it was taken, then after 60 s, 120 s and so on, at most 10 min apart; a node newly announcing itself as a gateway (a second or further one too: the engine's count of gateways with a route rises; a change of best gateway, one leaving, or one leaving and another appearing between two 2 s checks does not count), or the first route after none, brings the next attempt forward to 30 s after the last (the count checked in the VM against batman-adv 2024.3, the retry on the host only). Each attempt lasts 45 s, up to 10 s more if an offer is still being taken. The address stays on bat0 throughout and only a lease replaces it (which cuts tethered hosts' open connections: USB and Wi-Fi AP, below); any DHCP server's NAK, routine with several servers (DHCP server, below), takes it away until the next 2 s check re-applies it, unless lwIP has by then bound another server's lease or is ARP-checking its offer, which then replaces it. `AT+MESHDHCP=0` goes straight to the probe, keeps the static address and never asks DHCP |
| DHCP server | the lease and its router come from the node whose offer arrives first (an OpenMANET gate and each extender point serve a pool of their own). The other servers run dnsmasq with `authoritative 1` (the OpenWrt default) and NAK the flooded request that follows (`wrong server-ID`), and lwIP starts over on any server's NAK, so an attempt binds only in a round where the chosen server's ACK arrives before every other server's NAK. With lwIP's DHCP client against 2–3 such servers on one VM link (a one-off run, not in the VM suite), 15 of 31 rounds were lost that way and every lease still came within 4.2 s; over radio hops it can take more rounds. Nothing uses the lease's DNS server (USB and Wi-Fi AP, below). Every 2 s the router's MAC, from lwIP's ARP table, is looked up in the translation table: it must resolve to an originator with a route whose last OGM is at most 10 s old (batman keeps a silent node's route 200 s). A router that sends nothing into batman for 600 s drops out of its node's translation table, so its MAC stops resolving although it is up: while the MAC is not learned yet or does not resolve, the Warthog broadcasts an ARP request for the router at the next check and every 10 s after, and the reply puts it back (seen once in the VM against batman-adv 2024.3, else host-tested); a router that does not answer still counts as lost. After 60 s of failed checks in a row DHCP is asked again beside the lease address, which stays until a new lease replaces it; with no lease within 45 s the address is kept with gateway `10.41.0.1` and retried as a static one. While each new lease's router fails the check too (the same server can hand the same router back), the wait before the next restart doubles, 120 s, 240 s and so on, at most 10 min; it is 60 s again once a check passes, after a lease without a router option, or when a node newly announces itself as a gateway or the first route after none appears. A lease without a router option is not watched. If lwIP itself drops a lease (a server NAKs its renewal, or the lease expires), bat0 has no address while lwIP asks again, usually for seconds, and `AT+MESHBATMAN?` reads `addr=dhcp`; with no lease within 45 s of the next 2 s check (up to 10 s more if an offer is still being taken) a static candidate is ARP-probed and taken as at start (lwIP's DHCP client clearing the address seen once in the VM, the rest host-tested). Gateway-steered DHCP is not implemented: it would confine every Warthog to the gate's pool (16 addresses on OpenMANET) with no fallback to the points' pools |
| USB and Wi-Fi AP | NATed behind bat0, as in every mesh mode; `AT+MESHBRIDGE=1` is refused. Their DHCP offers the `AT+DNS` resolver (default `1.1.1.1`), never the lease's. On a mesh without internet, or for names only the mesh's DNS knows, set `AT+DNS=` to a node's address, then `AT+RESET`: a gate is `10.41.0.1`, and the `router=` in `AT+MESHBATMAN?` is the node that gave the lease, whose dnsmasq answers while that node is on the mesh. Not measured. NAT uses bat0's current address, so when that changes, tethered hosts' open TCP connections drop and must reconnect, and replies still addressed to the old address are lost. It changes when a lease replaces the static address (DHCP is asked again from 30 s after that was taken), when a new lease after a router loss comes from another server, and when lwIP drops a lease (a NAK or expiry; DHCP server, above); `ip=` in `AT+MESHBATMAN?` shows the address in use. From the code; not measured |
| Meshtastic repeater | unchanged: `239.0.0.69:4403` leaves as a batman broadcast. It arrives as a broadcast from nodes on OpenMANET's default `multicast_mode 0`, or as one batman unicast per listener from nodes on `multicast_mode 1`; both are delivered. openmanetd 1.3.10, which OpenMANET 1.8.0 ships, writes `multicast_mode 1` for `batman.multicastForceflood: true`; its `main` branch, which 1.8.1-dev builds, for `false` |
| `AT+MESHFWD` | must be 0 (refused both ways) |
| `AT+SWCCMP=0` | refused while batman runs on a host-CCMP build |

Two 30 s broadcast holds meet the DHCP wait. A node's bridge loop avoidance drops
broadcasts for about 30 s after its `bat0` comes up, so a Warthog powered up
together with a node, or joining just after the node rebooted, sends its first
DISCOVERs into that hold. And a peer takes an OGM or broadcast whose sequence
number jumps (a node's first, or the first after it restarts with new ones) as a
restart of that node, and takes no further restart from it for 30 s, timed
separately for OGMs and broadcasts. A reset that keeps the board powered
(`AT+RESET`, a crash, a watchdog) continues the ELP, OGM and broadcast sequence
numbers 256 ahead from RTC memory, so peers take the next boot's frames at once.
A power-up, or a kept record that fails its check, draws random ones; a peer that
took a restart of the Warthog (first contact included) less than 30 s before then
drops its OGMs and broadcasts (ARP, DHCP) until those 30 s have passed. From the
code, host simulation and the VM (batman-adv 2024.3), and on air from the Pi
harness against 2025.4 (kept numbers: every broadcast after each start delivered;
new random numbers 12 s after the previous restart: 18 s without replies); none of
it on a board. The 45 s
wait outlasts both holds; in the VM the leases came up to 35.7 s after the first
route. A later attempt lasts 45 s too, since a gateway that just appeared may have
just booted.

## Broadcasts and link throughput

Batman-adv accepts ELP, OGM and broadcasts only with Ethernet destination
`ff:ff:ff:ff:ff:ff`. `AT+MESHGRP` picks how they leave the radio:

- `0` (default): one individually addressed copy per established peer, with
  Mesh Control Address Extension mode 2 (addr5 `ff:ff:ff:ff:ff:ff`, addr6 our
  mesh MAC), which mac80211 hands to batman as a broadcast (on air, batman-adv
  2025.4 on an OpenMANET Pi received them as Ethernet broadcasts through the
  Morse driver, under SAE). Keyed with the link's pairwise key under SAE, in the
  clear on an open mesh, and ACKed. Each broadcast is sent once.
- `1`: one standard 802.11s group frame (under our group key with SAE), unACKed;
  each broadcast is sent three times. Every batman group frame the Warthog sends
  (each copy of its own or a relayed broadcast, ELP, the OGM aggregate) is handed
  to the radio 5 ms or more after the one before on its millisecond clock (over
  4 ms), because on air (OpenMANET's MM6108) a group frame under 1 ms behind the
  sender's previous frame mostly never arrived; only an OGM aggregate sent early
  because it is full is not held. A new broadcast's first copy goes before earlier
  broadcasts' repeats. Up to 5 broadcasts are taken at once (1 sent, 4 waiting;
  4 when a group frame left under 5 ms before), then one more every 5 ms. A
  further one takes the place of the repeats still due of one already sent once
  (`bc_copy_drop` counts those copies). While all 4 waiting are still unsent it is
  dropped (`bc_queue_full`): an own broadcast is never sent and lwIP is not told;
  a relayed one is delivered here but not passed on (batman-adv queues up to 256,
  and past that does not deliver one). So a node beyond a Warthog relay gets at
  most the first 5 of a burst that reaches the relay at once. Ahead of the engine,
  the port holds at most 4 frames from lwIP and 6 from the radio that the engine
  task has not taken yet, and drops the next (`q_tx_full`, `q_rx_full` in
  `AT+BATSTAT?`). Sustained, at most one group frame leaves per 5 ms (ELP and the
  OGM aggregate included), first copies before repeats. The 5 ms run from when the
  radio driver accepted the previous group frame, also when it first had to wait
  for room in the radio's queue (host-tested); frames already waiting in that
  queue can still go out back to back. Host- and VM-tested. On air, from the
  engine in the Pi harness against batman-adv 2025.4, through the Pi's Linux radio
  path and not a Warthog: no two of its group frames left under 4 ms apart (median
  5.1 ms); two broadcasts from one millisecond interleave their copies (A1 B1 A2 B2
  A3 B3), and the second arrived as often as the first (96 % of copies; pairs
  596/600); in a burst of 6 the 1st kept one copy and the 6th was dropped
  (`bc_copy_drop`, `bc_queue_full`).

Batman unicast goes to the next hop only, as an ordinary 4-address frame.

Link throughput toward each neighbour is rate control's expected throughput of
its best rate (the value OpenMANET's Morse driver reports to batman), in 100 kbit/s
units, read from a peer-table snapshot at most 250 ms old. If taking the snapshot
fails, the last one is used for up to 2 s, then the neighbour reads as having no
estimate (sampled as 1 Mbit/s), never as absent (`tput_snap_fail` in
`AT+BATSTAT?`). `AT+MESHBATTP=<units>` overrides it for every neighbour; `0` =
automatic. On air (2026-09-29/30) the Warthogs read the Pis at 5.6–6.4 Mbit/s,
and the Pis read the Warthogs at 7.1–7.2 Mbit/s. Each figure is that end's
estimate for its own transmit direction, so the two need not match. With every
node in range the gap changes no route: a direct link always beats one relayed
over the same radio, whose throughput batman halves. No `AT+MESHBATTP` override
is needed at one hop; whether the Warthog's lower figure steers routes away from
a Warthog relay beyond one hop is not measured.

## Commands

| Command | |
|---|---|
| `AT+MESHBATMAN=<0\|1>`, `AT+MESHBATMAN?` | on/off, next boot; the query shows stored, running and the reason it is not, and while batman runs a `bat0` line: address state, router, next DHCP attempt, best gateway |
| `AT+MESHBATTP=<n>`, `AT+MESHBATTP?` | throughput override, 100 kbit/s units, next boot |
| `AT+BATN?` | neighbours (`batctl n`) |
| `AT+BATO?`, `AT+BATO=<mac>` | originators, routes and candidates (`batctl o`); `=<mac>`: that originator only. A node's originator is the `orig=` of its `AT+BATN?` line, its first active batman interface's MAC, not always its HaLow MAC: on a node that also runs OpenMANET's 2.4 GHz `batmesh1` it can be that interface's, and `AT+BATO=<HaLow MAC>` then shows only the summary line |
| `AT+BATTG?`, `AT+BATTG=<mac>` | global translation table (`batctl tg`); `=<mac>`: the rows for that client or originator |
| `AT+BATTL?` | local translation table (`batctl tl`) |
| `AT+BATSTAT?` | engine and port counters |
| `AT+MESHCFG?` | adds `batman=` to the config line, a `batman=yes(...)` or `refused(<reason>)` mode line and a `batman self=` line |

Details in the [AT Command Reference](AT-Command-Reference#batman-mode).

## Limits

- One routing interface; no bonding, no per-interface hop penalty.
- NAT only: tethered hosts are not announced; only the Warthog's own bat0 MAC is.
- Only Meshtastic's `239.0.0.69:4403` crosses to the tethered side. OpenMANET's
  other groups reach bat0 and stop there, since the Warthog joins no other group:
  CoT/ATAK SA on `239.2.3.1:6969`, push-to-talk on `239.192.41.1` and mDNS. They
  arrive as batman broadcasts, or as unicast copies from `multicast_mode 1` nodes.
  A tethered ATAK sees none of them, and its own CoT does not reach the mesh
  ([OpenMANET Interop](OpenMANET-Interop#reaching-openmanet-applications)).
- bat0 MTU 1460; USB and the access point stay at 1500, and their DHCP offers
  MTU 1500. The Warthog cannot send a tethered host ICMP "fragmentation needed":
  NAT rewrites the source first, so the error is addressed to the Warthog itself
  and lost. Tethered packets of 1461–1500 bytes with DF set are dropped silently.
  TCP is unaffected where the far end or a gate (OpenWrt `mtu_fix`) advertises or
  clamps an MSS of 1420 or less; for anything else, UDP from Linux hosts (DF set
  by default) included, set the tethered host's MTU to 1460. From lwIP's source
  and a one-off host run of it; not measured on a board. Batman fragments only
  packets it relays or reassembles above 1500 bytes. An OGM record above 1500
  bytes (Linux sends up to 1504 when a large translation-table change rides on it)
  is not forwarded; nodes behind the Warthog catch up by translation-table
  request.
- On OpenMANET's own radio path (MM6108 with the Linux Morse driver, measured Pi to
  Pi) group frames with more than 1524 bytes of batman payload never arrived. A
  Linux `bat0` left at MTU 1500, batman-adv's default, therefore loses IP
  broadcasts and multicasts of 1497–1500 bytes; the wizard's 1460 and the
  Warthog's are below the ceiling. Whether the Warthog's chip has the same one,
  with the 12 extra bytes of an AE-2 copy, is not measured.
- A Linux node's unicast frames longer than its RTS threshold (1000 on both
  OpenMANET 1.8.0 bench Pis) go out behind RTS/CTS; full-size TCP segments and
  large UDP toward the Warthog and its tethered hosts are among them (derived).
  On a STA chip interface, which every build but
  `warthog-mesh-sae-swccmp-meshvif` runs the mesh on, the Warthog's chip
  addresses its CTS from the peer it registered last, so only that node's large
  frames arrive; the others time out and never send them (the chips' MAC
  counters on both ends, 2026-09-30). `-meshvif` runs the mesh on a MESH chip
  interface, which addresses its CTS to each RTS's sender (per the chip
  firmware's disassembly) and whose CTS both Pis took: on air on 2026-09-30,
  with the Pis' threshold at 1000, 500-, 1000- and 1400-byte pings over batman
  passed 10/10 each. On the other builds set each OpenMANET node to CTS-to-self,
  `echo Y > /sys/module/mm6108_sdio/parameters/enable_cts_to_self` (the bench
  Pis' MM6108 SDIO driver; elsewhere find the module with
  `ls /sys/module/*/parameters/enable_cts_to_self`), or RTS off,
  `iw phy <phy> set rts off` (`iw dev wlh0 info` prints `wiphy N`: the phy is
  `phyN`); both measured set at runtime, and neither survives a reboot
  ([OpenMANET Interop](OpenMANET-Interop#frames-over-about-1000-bytes-from-a-linux-node)).
  Frames from the Warthog are unaffected. A radio limit, not a batman one.
- Not implemented: the distributed ARP table (DAT), multicast optimisation
  (the Warthog reads as "wants all multicast", which is what OpenMANET's forced
  flood does anyway), announcing itself as a gateway and gateway-steered DHCP (the
  gateways peers announce are read, for bat0 addressing only), bridge loop
  avoidance, network coding, ELP unicast probes (Linux neighbours still probe
  the Warthog), answering `batctl tp`, and alfred, so openmanetd's node list,
  host names and address reservation do not show the Warthog. `batctl ping` and
  `batctl tr` work.
- Tables: 8 neighbours, 32 originators, 4 802.11s peers, 256 translation-table
  entries (at most 64 of them temporary: clients seen in traffic, not yet
  announced), 3 fragment reassemblies at once. Above 32 originators the extra ones
  are unreachable; a new direct neighbour displaces the worst-routed originator
  that is no neighbour's own.
- Under SAE (`warthog-mesh-sae-swccmp`, the wizard's mesh) an 802.11s peer is
  never expired for silence: the 30 s silence expiry covers open meshes only, and
  the Warthog's mesh driver gives the supplicant no inactivity figure, so its
  300 s check always reads the peer as just active. A point that goes out of range
  keeps its peer slot until the Warthog hears a Close from it, or the point starts
  SAE with the Warthog again once back in range (the point drops the silent
  Warthog after an unanswered poll at 300 s, but its Close goes unheard). So a
  Warthog carried past 4 points it no longer hears cannot peer with a fifth:
  `offer_full` climbs in `AT+MPMPEERS?` and the fifth never appears in
  `AT+BATN?`, until one of the 4 is back in range or `AT+RESET`. Until then, with
  `AT+MESHGRP=0`, every ELP (2 a second), OGM (1 a second) and broadcast is also
  sent, ACKed, to each departed point. From the code and a one-off host
  simulation; not measured on air.
- A node's translation table reaches the Warthog as change records on its OGMs,
  or as a full table on request. Until it first holds a node's table (after it
  starts, or learns the node again) the Warthog asks at each of the node's OGMs, at
  least 500 ms apart; after that at most every 3 s, with the backoff below
  (host-tested, and in the VM against batman-adv 2024.3, `s18`). A full table
  above 2048 bytes (about 165 entries) comes in fragments the Warthog drops
  (`fr_toobig`), so it follows such a node only while it takes every change set.
  It needs the full table, and never gets it:
  - after the Warthog starts or restarts;
  - after that node's originator entry times out (400 s without its OGMs) or is
    displaced (above) and is learned again, even with the node's table unchanged;
  - after the Warthog misses every OGM that carried one of the node's change sets
    (four OGMs, fewer if the node changes again sooner) and the node has changed
    again since, or when the node's table checksums stop matching the Warthog's
    copy;
  - after the node adds more than about 121 clients within one OGM interval:
    2025.x then sends no change records, and 2024.3 (in the VM) never announced
    the burst to any peer.

  From then on the Warthog asks for the full table as the node's OGMs arrive, and
  the node, or a batman-adv relay that holds its table and answers on its behalf
  (a Linux relay usually does, cutting the answer itself), answers each request
  with more than 2 KB of fragments. The Warthog counts each such answer against
  the node, not the relay, and waits twice as long before asking for that node's
  table again (6, 12, 24, 48 s, then once a minute), and 3 s again once it is in
  sync; the relay is still asked for its own table as usual. After a missed change
  set the Warthog keeps the rows it had but takes none of the node's later adds or
  removals; after a start or a relearn it has none of the node's clients, except
  temporary rows learned from their broadcasts. While it is behind on that node's
  table (none taken since a start or relearn, or an answer too big or not fitting)
  and its next hop toward the node is another node, its unicast to the node carries
  the TTVN the node's OGMs announce, not the last one it synced: a batman-adv relay
  drops a unicast whose TTVN is older than its own for that node while its table
  still names the node for the destination, so otherwise the kept rows, the
  temporary rows and the node's own address would all be unreachable through a
  Linux relay. After a missed change set the synced TTVN still goes until the
  first answer that cannot be taken arrives. When the node is the next hop the
  synced TTVN goes, and the node re-resolves an out-of-date packet itself. If the
  lease's router is one of the clients it cannot see and sends no broadcast the
  Warthog hears, the router check (DHCP server, above) fails although the router
  is up: `AT+MESHBATMAN?` shows `router=<ip>(lost <n>s)` and DHCP is asked again
  after 60 s, then after 120 s, 240 s and so on, at most every 10 min, for as long
  as the check fails (`retry_in=` counts down to the next), and
  `AT+BATTG=<router's MAC>` shows no row for it. Signs: in
  `AT+BATO=<that node's originator>` its `ttvn=` stays below the `TTVN:` of the
  node's own `batctl tl` (or the TTVN `batctl tg` on a relay shows for the node's
  rows), or reads `ttvn=-`; in `AT+BATSTAT?`, `tt_req_tx` and `tt_req_stall` (the
  `ut` line) keep rising about once a minute, and `fr_toobig` (the `fr` line) by
  the answer's fragment count each time. Shown by the engine in host simulation
  (`test_bat_tt`) and, for the restart case, the backoff with the node or a relay
  answering, and the unicast TTVN through a relay, in the VM against batman-adv
  2024.3 (`s12`, `s16`, `s17`); not measured on air.
- When announced entries fill all 256 rows (temporary rows make way for them), a
  node whose change set or full table does not fit is asked for its full table
  again, each answer is stored only as far as it fits (`tt_rows_full` rises each
  time), and the wait before the next request doubles per answer as above (6 s up
  to once a minute, `tt_req_stall`), so once rows free up that node's table can
  take up to a minute to arrive. The clients that do not fit cannot be reached,
  `AT+BATO=<that node's originator>` reads `ttvn=-`, and `AT+BATSTAT?` shows
  `tt=256/256`. A node set up as the wizard does announced three entries of its
  own on air (its bat0 MAC untagged and on VLAN 1, and its bridge's MAC), so 32
  such nodes take 96 rows before any client. Shown in host simulation only.
- Heap: about 58 KB while batman runs (engine 32.5 KB, frame slots 15.7 KB,
  render buffer 4 KB, task stack 6 KB), allocated at mesh start; nothing when off.
  Computed from the Xtensa object sizes. On a board (`warthog-mesh-sae-swccmp`,
  2026-09-29/30) `heap_free` was about 53 KB right after boot; with 3–4 SAE
  peers and host CCMP it settled at 22–31 KB, with `heap_min` 10.9–20 KB and
  `heap_largest` 8.7–15 KB, little headroom. The engine task's `stack_free` was
  about 4.1 KB of its 6 KB. Received frames
  reach lwIP as heap copies, at most 16 at once (about 24 KB at the 1460-byte
  MTU, about 33 KB for 2 KB reassembled frames), each freed when lwIP has read it;
  a frame past 16 is dropped and counted in `deliver_cap` (`AT+BATSTAT?`). They
  take the place of the radio's receive buffers, up to 23, that lwIP holds with
  batman off, so they are not in the 58 KB, but they do show in `heap_min`.
  Flash: 31.7 KB of code (`.text` and `.literal`; 3.8 KB of constants besides)
  for the engine and port; static RAM: 239 B in the port and 124 B of bat0
  addressing state, allocated in every build, and 28 B of RTC memory for the kept
  sequence numbers (from the `warthog-us` link map).
- A relayed broadcast or forwarded OGM aggregate also goes back to the neighbour
  it came from (one extra unicast per relayed broadcast with `AT+MESHGRP=0`).

## Measured on air

2026-09-29 and 2026-09-30, `warthog-mesh-sae-swccmp`; the second day's image
added the group-privacy fix (below) and the `AT+SWCCMP?` fail line, and neither
image had the `not_ours` receive filter or the `mgmt_nours` count. Three
Warthogs on the first day, two on the second, and two OpenMANET 1.8.0 Pis
(batman-adv 2025.4), on an SAE mesh
(`ieee80211w=2` on the Pis, `AT+MESHPMF=0` on the Warthogs), all in range of each
other. The Pis' `bat0` was set up by hand at runtime: BATMAN_V, one Pi a gateway
server and the other a client, openmanetd stopped, mesh11sd `mesh_fwding` 1 and
effective `mesh_nolearn` 0; on both Pis `bat0` was a port of `br-lan`.

| | Result |
|---|---|
| Start-up | `running=1`; right after boot `heap_free` about 53 KB, the engine task's `stack_free` about 4.1 KB of 6 KB |
| Heap with 3–4 SAE peers, host CCMP and batman | 09-29: `heap_free` 22–24 KB, `heap_min` 10.9 KB, `heap_largest` 8.7–11 KB; 09-30: 26–31 KB, 18–20 KB, 15 KB |
| Host CCMP | 09-29: opened the Pis' group frames (ELP, OGM, broadcasts, group PREQs), `AT+SWCCMP?` `tried=69 ok=69` on one board, `tried=71 ok=64` with 7 `micfail` on another; no Pi sent unicast that day, so these count group frames and the Warthogs' unicast to each other. 09-30: the Pis' unicast too, once their paths formed (`batctl ping` and DHCP, below). `micfail` also counted unicast data the chip handed up between other stations (the 09-30 fail line: addr1 a Pi, addr2 the other Warthog); the current tree drops those first (`not_ours`, host-tested) |
| AE-2 broadcasts | reach batman-adv on the Pis as Ethernet broadcasts (`tcpdump` on a Pi: destination `ff:ff:ff:ff:ff:ff`) |
| Neighbours and originators | both ways: the Pis' `batctl n` lists both Warthogs and `batctl o` routes through them; `AT+BATN?` and `AT+BATO?` list both Pis |
| Link throughput | the Warthogs read the Pis at 5.6–6.4 Mbit/s (the Warthog's rate control); the Pis read the Warthogs at 7.1–7.2 Mbit/s. Each end's own transmit estimate; no override needed at one hop |
| Gateway | `AT+MESHBATMAN?` `gw=` names the gateway Pi, `(10.0/2.0)` |
| Translation table | Warthog to Warthog: checksums match. 09-30: the Warthogs take both Pis' tables (`ttvn` 3 and 4, `tt_req_tx` 3–6, one `tt_crc_fail`), and the gateway Pi's `batctl tg` holds each Warthog's bat0 MAC as an announced row (no `T` flag) whose CRC matches the Warthog's own |
| Unicast from a Pi | 09-30: the Pi's `iw dev wlh0 mpath dump` lists both Warthogs `ACTIVE`, hop count 1, next hop the Warthog; on the Warthogs `AT+MESHFWDSTAT?` `hwmp gp` 8–9, `AT+HWMPSTAT?` `preq_rx` 8–9 and `prep_tx` 4–5; `batctl ping` from the Pi to each Warthog 5/5 (10–128 ms) |
| DHCP | 09-30: each Warthog leased from a Pi's dnsmasq (10.41.0.131, 10.41.0.140), `router=10.41.254.1(ok)` |
| IP | 09-30: a host on the gateway Pi's `br-lan` pinged each Warthog's bat0 address, 9/10 and 10/10 (small packets). 09-29, Warthog to Warthog over bat0: `AT+MPING` 8/8 (69–267 ms) one way, 5/8 the other (lost while the tables were learned and while a DHCP attempt beside the static address re-applied it) |
| Meshtastic | 10/10 datagrams to `239.0.0.69:4403` from one Warthog reached the other over batman and appeared on the gateway Pi's `br-lan`, bridged from `bat0` |
| 802.11s forwarding on the Pis | `AT+BATSTAT?` `rx_relayed` 343 on one Warthog on 09-29, and from 363 to 388 between two readings on 09-30: the Pis re-sent batman frames at 802.11s, the Warthog dropped those copies, and the direct copies arrived (every result above) |
| `warthog-mesh-sae-swccmp-meshvif` | 09-30, one Warthog on a MESH chip interface, the Pis' RTS threshold at 1000: a DHCP lease from a Pi, the Pi's `batctl n` lists it, and 500-, 1000- and 1400-byte pings over batman 10/10 each |

A 1.8.0 node (effective `mesh_nolearn` 0) sends unicast only over an 802.11s
path, which its own group PREQ sets up: CCMP-protected under the node's MGTK
(group-addressed privacy, no MMIE), opened by host CCMP under the MGTK from its
AMPE, and answered by the Warthog's CCMP-protected PREP. A build without host
CCMP cannot open that PREQ. The 09-29 image refused it anyway (counted as
`bipfail`: 2199 on one board, with `mgmt prot host` 2201, so host CCMP had opened
them), so the Pis kept the Warthogs `RESOLVING` (next hop `00:00:00:00:00:00`,
frames queued) and nothing unicast from a Pi arrived: no translation-table
answers (the Warthogs kept asking, `tt_req_tx` about 70, `tt_crc_fail` 53, and a Pi
kept a Warthog's row temporary), no `batctl ping`, no IP, no DHCP OFFER or ACK,
while neighbours, originators and broadcasts looked healthy both ways. An image
that counts `bipfail` in `AT+MESHFWDSTAT?` does this against a Linux node under
SAE; the 09-30 image, which no longer has that counter, is the fix. On an open
mesh the Warthog's own PREQ every 2 s gives the node the path, and a 1.8.1-dev
node (`mesh_nolearn` 1) sends to a one-hop peer directly.

## Not yet measured

| | How to settle it |
|---|---|
| Unicast between two other stations, which the MM6108 also hands up (data seen on 2026-09-30 as host CCMP `micfail`). Data is dropped before any decryption (host-tested). Management frames between them are only counted: whether the chip hands those up is not measured, and a Protected one from a peer still reaches host CCMP (`micfail`) | while two neighbours exchange unicast, `AT+FILTSTAT?` `not_ours` rises. Its second line's `mgmt_nours` says whether their unicast management (PREPs as their path refreshes, Block Ack) reaches the Warthog; `AT+SWCCMP?` `micfail` stays flat only while `mgmt_nours` does, since between neighbours running MFP (the Pis' `ieee80211w=2`) those frames are Protected. If it rises, they need dropping before host CCMP and Block Ack as data is |
| A host tethered to the Warthog reaching the batman mesh: the AT console on USB CDC-ACM ran in batman mode, but no host on USB NCM or the Wi-Fi AP has sent traffic through NAT onto bat0, and the AP's start in batman mode is not checked | with `AT+MESHBATMAN=1` and a lease (`AT+MESHBATMAN?` `addr=leased`), a host on USB NCM (a `192.168.4.x` lease) and one on the AP (`192.168.5.x`) each ping the lease's `router=` address and a Pi's `br-lan` or `br-ahwlan` address, and resolve a name with `AT+DNS=` set to that router |
| Frames near the 1500-byte batman MTU cross the chip with AE-2 and CCMP overhead. From every Linux node only on `-meshvif` or with the node set to CTS-to-self or RTS off, on other builds only from the peer the chip registered last (Limits); 1400-byte pings passed either way | on `-meshvif`, or with the node so set, `ping -s 1432 -M do` between the node's `br-ahwlan` address (bat0 has none on a wizard node) and the Warthog's bat0 address, both ways |
| `AT+MESHGRP=1`: whether a Linux node takes the Warthog's group frames under its MGTK (on the swccmp builds only if the chip starts that key at the TX PN it was installed with; [OpenMANET Interop](OpenMANET-Interop#management-frame-protection-peering-does-not-need-it-path-selection-does)), and the airtime of AE-2 copies against group frames, 1 against 3 broadcast copies. Only `0` has run on air | `batctl n` on the node still lists the Warthog with `AT+MESHGRP=1`; Meshtastic multicast delivery and OGM loss under both settings |
| DHCP against the two 30 s holds above: in the VM (batman-adv 2024.3, veth) the leases came inside the 45 s wait; the on-air leases of 2026-09-30 were not timed against either hold; on air it depends on how long the node's mesh and the Warthog's peering take | power a node and the Warthog up together, then power-cycle the Warthog (not `AT+RESET`, which keeps its sequence numbers) within 5 s of its first route; `AT+STATUS?` must show an address from the node's DHCP pool, not `10.41.253.x` |
| Sequence numbers kept across a reset that keeps power (RTC memory): host, VM, and on air in the Pi harness (a killed process that kept its record); whether each ESP32-S3 reset path keeps RTC memory is not measured | `AT+RESET` twice, 5 s apart: the boot log's batman line ends `seq=carried`, `AT+BATO?` `ogmseq=` resumes about 256 above its value before each reset, and on the node the Warthog's last-seen in `batctl o` stays under 2 s |
| DHCP again: after the lease's router leaves, and beside a static address. Run only against a model of lwIP; the engine's lookups ran in the VM | with a lease from a point (`AT+MESHBATMAN?` `router=...(ok)`), power that point off while a gate stays up: within about 2 min the `bat0` line must show `restarts=1` and a lease from another node. Start the Warthog with no DHCP server reachable, then bring a gate up: `addr=static` must become `addr=leased` without a reboot |
| DHCP with several servers in range: run only in the VM (lwIP's DHCP client, dnsmasq on veth); on air the order of the chosen server's ACK and the other servers' NAKs depends on hop count | a gate and two or more points all within one hop of the Warthog: `AT+RESET` 20 times; each time `AT+MESHBATMAN?` must reach `addr=leased`, never `addr=static` |
| Block Ack with a wizard node under SAE: the Warthog now protects its ADDBA request and response and its DELBA to a peer that runs MFP, which the node's mac80211 dropped in the clear, so A-MPDU sessions can now form both ways, where before every attempt failed. Host-tested only; whether the node accepts them, and what aggregation then does to batman's unicast, is not known, and no setting turns A-MPDU off ([OpenMANET Interop](OpenMANET-Interop#management-frame-protection-peering-does-not-need-it-path-selection-does)) | on the Warthog, `AT+MESHFWDSTAT?` `mgmt tx host` (host CCMP, which batman mode arms) rising with `drop` at 0; on the node, `agg_status` under `/sys/kernel/debug/ieee80211/phy*/netdev:wlh0/stations/<Warthog's mesh MAC>/` lists the sessions, and 3000-byte pings through bat0 still answer both ways (on `-meshvif`, or with the node set to CTS-to-self or RTS off, Limits) |
| Roaming past more than 4 points under SAE: from the code and a one-off host simulation the Warthog strands at the fifth (Limits, above) | walk a Warthog past 5 powered points one at a time; `AT+BATN?` must list the fifth. Expected to fail until silent SAE peers are expired |
| A node set up by the wizard and run by openmanetd: the bench Pis' `bat0` was made by hand with openmanetd stopped, mesh11sd `mesh_fwding` 1, and on both Pis `bat0` in `br-lan` | against a wizard node (`br-ahwlan`, `mesh_fwding` 0, openmanetd's `multicast_mode`, alfred): the checks in [Measured on air](#measured-on-air), and a lease from the `br-ahwlan` pool |
| Whether the Warthog's lower throughput figure (0.7–1.6 Mbit/s under the Pis' for the same links) steers routes away from a Warthog relay beyond one hop | with a Warthog and a Pi both able to relay between two nodes out of each other's range, compare `batctl o` throughput via each; set `AT+MESHBATTP` only if the Warthog is never chosen where it should be |
| More than one hop: every node was in range of every other, and no route through a third node was checked | a Warthog out of the gateway's range reaches it through a Pi: a lease, pings, `AT+BATO?` next hop the Pi. Two Pis out of each other's range reach each other through a Warthog: `batctl o` routes via it, `uc_fwd` and `bc_fwd` rise in `AT+BATSTAT?`. Then three hops or more |
