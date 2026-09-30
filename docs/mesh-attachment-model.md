# Mesh attachment model: how Warthog should join an OpenMANET network

Warthog can attach to an OpenMANET HaLow network three ways. This records
which one is built, what each costs, and what each gives up — so the choice is
made on evidence rather than re-argued.

**Short version.** Warthog today is a non-forwarding 802.11s mesh point (model
A) or a plain STA leaf (model B). By default neither relays; `AT+MESHFWD=1`
adds 802.11s HWMP forwarding, host-tested and not yet on a radio. If you need a
relay — an airborne node extending coverage between two nodes that cannot hear
each other — on a bare or bridged 802.11s network the answer is HWMP
forwarding, not batman-adv. Inside a wizard-configured OpenMANET `bat0` fabric
relaying is unproven.

## The models

### A. Non-forwarding 802.11s mesh point — **built**

Warthog peers directly with OpenMANET nodes over 802.11s. SAE/AMPE peering is
verified cross-vendor (2026-09-20: three-node mesh, all links `ESTAB`, two
simultaneous AMPE pairwise keys on one Warthog). Unencrypted data passes at
0–3% loss; the encrypted data plane has not yet carried traffic cross-vendor,
see *Prerequisite: group-addressed frames*.

Gives up: relaying. Warthog is a leaf. It is reachable by, and can reach, the
peers it can hear — and nothing beyond them.

### B. STA leaf behind an AP VAP — **built, hardware-verified**

The default region builds associate to a HaLow AP. SAE association plus DHCP
is verified against a HaLowLink 2, with the full tethered-EUD path working
(`docs/napt-notes.md`).

Gives up: peer-to-peer topology. Needs infrastructure, and the AP is a single
point of failure. The AP-VAP-alongside-mesh variant on an OpenMANET node is
untested.

### C. BATMAN_V subset in firmware — **not built, and not recommended**

Ethertype 0x4305, compat 15, ELP, OGMv2, unicast/broadcast, and TT
announcement of the tethered client MAC.

## Cost

Measured from the `warthog-mesh-sae-swccmp-on` build (2026-09-21): app image
1,967,408 B in a 4 MiB factory partition — **2.12 MB flash free (53.1%)** — and
a **~170 KiB** DRAM heap arena. No PSRAM.

| Option | Effort | Flash | RAM | Notes |
|---|---|---|---|---|
| HWMP forwarding | ~600–1,200 LOC, 2–6 weeks | small | small | reuses the mesh port already in the build; range reflects two independent estimates |
| BATMAN_V subset | ~2,000–2,500 LOC, 6–10 weeks | 25–40 KB | 4–6 KB | 16-entry originator table, 4 neighbours, dedup window, one 4 KB task |

**Size is not the constraint for either option.** Do not scope this on flash
or RAM; both fit with room to spare.

## Why HWMP forwarding, not batman-adv

This applies to bare or bridged 802.11s networks: peers whose mesh interface
stands alone or sits in `br-lan`, with no `bat0`. Relaying inside a
wizard-configured OpenMANET `bat0` fabric is unproven: the wizard sets mesh11sd
`mesh_fwding='0'`, so its nodes do not forward 802.11s frames and multi-hop is
batman's job, and no Warthog has been measured against such a node.

1. **Batman-adv is broadcast-first, and encrypted broadcast is Warthog's
   least-proven path.** OGMs flood; the translation table floods.
   `AT+MESHGRP=1` sends standard 802.11s group frames, but Warthog puts only
   its own MGTK in the chip, so under SAE a peer's group frames decrypt only
   through host CCMP, which has not run on air (see *Prerequisite:
   group-addressed frames*). HWMP's control plane rides the management path
   instead, which does not use the data-plane group key; toward a peer running
   MFP (the wizard's `ieee80211w=2`) it must itself be protected (CCMP unicast;
   BIP group, which Warthog's own group frames carry only with `AT+MESHPMF=1`),
   which is implemented and not measured.
2. **HWMP is already half-present.** The mesh port answers path requests
   aimed at it today. Forwarding is the missing branch, not a new subsystem.
> **Update.** The forwarding layer described below as future work now exists
> behind `AT+MESHFWD=1`: HWMP relay (PREQ/PREP/PERR), data-plane forwarding
> with duplicate suppression, proxied endpoints via Address Extension, and
> link-loss PERRs — all as freestanding, host-tested decision code with a
> multi-node simulator over it. It is compiled and off by default; nothing in
> it has been on a radio, and the `fwdcand` measurement below still decides
> whether the chip will let a relay see the frames at all.

3. **OpenMANET interoperates at the 802.11s layer without batman-adv.** The
   verified cross-vendor result (`docs/mesh-openmanet.md`) is plain 802.11s.
   batman-adv is a routing layer *on top* of the L2 mesh, not a precondition
   for joining it.

### What not having batman-adv actually costs — read this part

Two things are true here and this section used to state only the first, as
though it were universal.

**OpenMANET's mesh wizard does build a batman fabric.** Read from the
OpenMANET 24.10 Pi image, `/www/luci-static/resources/tools/morse/`:
`wizard.js:93-94` calls `setupBatmanDeviceOnNetwork()` (`gw_mode` `server`
when `mesh_gate_announcements=1`, else `client`), then
`setupBatmanInterfaceOnDevice()`. In `uci.js:58-65` these create `bat0`
(`proto=batadv`, `routing_algo=BATMAN_V`, `fragmentation=1`,
`multicast_mode=1`, which `openmanetd` rewrites to `0` on every start), attach
the HaLow wifi-iface to `batmesh0` (`proto=batadv_hardif master=bat0`), add
`bat0` to the `br-ahwlan` bridge, and set mesh11sd `mesh_fwding='0'` and
`nolearn='1'`. Nothing reads `nolearn` on 1.8.0, so `mesh_nolearn` stays `0`;
1.8.1-dev writes `mesh_nolearn='1'`. `kmod-batman-adv`, `alfred`
and `morse_mesh11sd` ship in the image, and its tooling keys off `bat0`:
`alfred` runs with `-b bat0`, and `openmanetd` reads batman originators and
restarts when `bat0` comes up.

**A fresh image has no mesh**: its HaLow radio is an SAE access point in
`br-lan`. **A node switched to `mode='mesh'` by hand, without the wizard, is
bare bridged 802.11s.**
Measured 2026-09-21 on two Pi 4 / MM6108 nodes running OpenMANET 24.10
(`r28739-d9340319c6`), switched to mesh by hand:

```
# ip -br link show type batadv   -> (nothing)
# ip addr show bat0              -> Device "bat0" does not exist.
# ls /sys/class/net/br-lan/brif/ -> eth0  phy1-ap0  wlh0
# lsmod | grep batman            -> batman_adv 208896 0      <- refcount 0
# uci show network.bat0          -> network.bat0.multicast_mode='0'   (no proto, no device)
```

Here `wlh0` is a **direct member of `br-lan`** alongside `eth0` and the 5 GHz
AP. batman-adv is built and loaded but holds no devices and is not in the data
path; `network.bat0` is a stub `openmanetd` writes on every start, not a
configured interface.

So the fabric mismatch is a property of the peer's configuration, not of
OpenMANET as such. A hand-converted node also keeps the stock
`mesh_fwding '1'` and relays 802.11s frames; a wizard node does not.
Against a wizard-configured node it is real and Warthog is
off the fabric. Against a node like these two there is nothing to dismantle and
Warthog's documented setup costs the operator nothing. **Check which one you
have with `ip addr show bat0` before believing either story** — that one
command is the difference, and this document previously told you the answer
without asking the question.

What remains true regardless: Warthog is not a batman originator. Against a
node that *is* on `bat0` it peers at L2 but gets no IP path: a batman hardif
hands only batman frames (ethertype 0x4305) to `bat0`, so Warthog's IP and ARP
never reach `br-ahwlan` or the DHCP server on it, and Warthog falls back to its
static `10.77.x.y/16`. It is absent from `batctl originators` and outside that
L2 domain. None of this is measured; no `bat0` node has been on the bench.

Two ways out, and they are genuinely different products:

- **A forwarding 802.11s layer.** batman-adv rides over L2, and the
  forwarding engine relays any ethertype, so in principle a forwarding
  Warthog carries batman frames between OpenMANET nodes without understanding
  them and the peer keeps `bat0` intact. Inside a `bat0` fabric that is
  unproven: wizard nodes run `mesh_fwding='0'`, and nothing has been measured
  against one. It does *not* make Warthog a visible batman originator or
  announce its tethered client in the translation table.
- **A BATMAN_V subset in firmware.** Only this makes Warthog a routed member
  the rest of the OpenMANET tooling can see. Note a licensing constraint:
  batman-adv is GPL-2.0-only and morselib here is GPL-3.0-or-later, so this
  must be a clean-room implementation of the protocol, not a port of the
  Linux source.

For the drone-relay goal between bare or bridged 802.11s nodes, forwarding is
the answer; inside a `bat0` fabric it is unproven. For "Warthog appears in
`batctl originators` and ATAK discovery works end to end", only the subset
does, and it is not enough alone: OpenMANET's CoT is IPv4 multicast to
`239.2.3.1:6969` on `br-ahwlan`, which a NATed Warthog must also repeat to its
tethered client (`main/mudp.c` repeats only Meshtastic's group).

## The bridged-peer blocker is Address Extension, not addressing

It is tempting to think a Warthog could join a bridged OpenMANET node if only
it stopped self-assigning `10.77.x.y`. Addressing alone is not enough.

A mesh interface enslaved to a bridge forwards **proxied** traffic — frames
whose original source is some other device on the bridge, not the mesh node
itself. 802.11s carries that with the Mesh Control **Address Extension**
field: mode 1 adds the original source (group frames), mode 2 the final
destination and the original source (individually addressed frames).

**Receive**, in every mode (`umac_datapath.c`, counter `ae=` on
`AT+RXCHAN?`): Warthog delivers a proxied frame to the right host instead of
attributing it to the mesh node. A non-zero `ae=` shows a bridged peer is
reaching us.

**Transmit** depends on the mode:

- **Leaf (default: `AT+MESHFWD=0`, `AT+MESHBRIDGE=0`).** Warthog learns, from
  AE on frames a keyed peer carried, each host behind any mesh node, and the
  peer that carried it (its relay). A unicast for that host is addressed to its
  node with AE mode 2 and handed to the node if it is a peer, else to the
  relay; a leaf sends no path request for it. A vanilla mac80211 relay with no
  path to the node drops the reply with a PERR, so first contact from a far
  host on an idle mesh can fail. A warthog relay, and OpenMANET's patched
  mac80211 with forwarding on (from its source), hold it and discover the
  node, giving up after 6.8 s. Not measured.
  Broadcasts go out as one plain replica per peer without AE, or as a standard
  group frame with `AT+MESHGRP=1`. A unicast to an unknown destination goes to
  the first peer, and a leaf sends no path request for it. The tethered client
  is NATed (`main/nat.c`), so every frame a leaf originates is its own.
- **`AT+MESHFWD=1` or `AT+MESHBRIDGE=1`.** The forwarding engine shapes every
  frame: AE mode 2 on group replicas and on unicast to or from a proxied host,
  AE mode 1 on a standard group frame from a proxied source, and a PREQ for an
  unknown destination. In bridge mode the tethered hosts are proxied endpoints
  with their own MACs.

Mesh gate announcement is not implemented. None of the AE paths has been on a
radio against a bridged peer; all are host-tested only.

DHCP on the mesh (`AT+MESHDHCP`, default on) removes the *addressing* half of
the bridged-peer requirement. Whether a Warthog carries traffic with a peer
that keeps its mesh interface in `br-lan` is not measured; until it is, the
setup in `docs/mesh-openmanet.md` takes the interface out of the bridge.

## Prerequisite: group-addressed frames

Warthog puts one MGTK in the chip, at AID 0, while every 802.11s peer
generates its own. Under SAE that is Warthog's own TX MGTK: hostap delivers it
at mesh start, it is stored, and it goes into the chip with the first peer, so
standard group frames (`AT+MESHGRP=1`) go out under Warthog's own key id.
Peers' MGTKs stay in the host keychain only. On Warthog's chip firmware
(MM-IoT-SDK 2.10.4, `mm6108.mbin` 1.17.6) group RX from more than one peer
failed both with the key installed at each peer's AID and with it once at
AID 0. Linux (`morse_driver`) installs each peer's MGTK at that peer's AID
alongside its own at AID 0; that sequence has not been tried here, so this is
a measured limit of Warthog's firmware and key sequence, not a proven chip
limit. It is why the host software CCMP path exists, and that path (RX *and*
TX, compiled only into the `warthog-mesh-sae-swccmp` builds) is implemented
and keeps a per-transmitter key for every peer. Host CCMP refuses a unicast
frame keyed with a group key (`grpkey=` on `AT+SWCCMP?`).

What remains is **measurement**: no run has yet shown `AT+SWCCMP?` reporting
`ok > 0` on live traffic, and the 3-address group-frame AAD path has never
executed on hardware. Until that is confirmed, treat encrypted group traffic
as unproven — and note that a broadcast-dependent design (batman) inherits
that risk, while HWMP's control plane on the management path does not.

## Cheaper partial relays worth knowing about

Full 802.11s forwarding is the right answer, but two narrower relays are far
closer than it and may cover a specific need:

- **Meshtastic-only relay: one guard.** `main/mudp.c:137` refuses to re-send a
  datagram out the interface it arrived on (`if (out == skip ...) continue;`).
  Relaxing that for the mesh netif would relay Meshtastic's multicast between
  two peers that cannot hear each other. Application-layer, one group, but it
  is nearly free. No OpenMANET node sends or listens on that group
  (`239.0.0.69:4403`), so it helps only Meshtastic devices on warthogs.
- **Mesh-to-mesh IP forwarding: one lwIP define.** `IP_FORWARD_ALLOW_TX_ON_RX_NETIF`
  would let routed IP traffic cross between mesh peers. This forwards at L3,
  so it does not extend the *mesh* (no 802.11s path selection, no batman
  transparency) but it does move packets.

Both are worth doing deliberately or not at all — switching either on without
the duplicate suppression that mesh forwarding normally provides invites
loops.

## The forwarding question: does the chip hand up a relay frame?

Everything above the chip is settled by source and host tests. Nothing in
Warthog's receive path discards a 4-address frame because its mesh destination
(addr3) is a third party, except the leaf guard (`rxdrop reason=93`), which
exists only while `AT+MESHFWD=0` and `AT+MESHBRIDGE=0`, and reason 94 for the
non-conforming case of a 4-address frame without Mesh Control. The host
simulator relays such a frame end to end (`scenario_relay`,
`t_rx_forward_unicast`), and `t_rx_forward_keyed` relays a keyed frame the chip
has decrypted, sending the copy out under the pairwise key.

The chip is a closed binary. Its one receive-address filter reachable through
morselib is `BSSID_SET`, and Warthog programs it with a synthetic value that no
data frame's addr3 ever equals, yet the data plane works. That rules out a
BSSID match on addr3 — and nothing more. Every frame measured so far had addr3
equal to the receiver itself, so a firmware rule "addr3 must be me" would have
passed all of them. (The command set also defines a monitor interface type,
`ADD_INTERFACE` type 3; morselib does not use it and it is untested on this
firmware.) The comments in `mmdrv.h`, `driver.c` and `umac_mesh.c` describing a
chip "addr3 filter" were hypotheses written before mesh receive worked; nothing
measured supports or refutes them.

So two questions need a radio, and a relay needs both answered yes:

1. **Receive:** does the MM6108 deliver a 4-address data frame **addressed to
   us** (addr1) by a peer, whose addr3 names somebody else? That is the frame a
   relay receives.
2. **Transmit:** does it send a frame whose addr4 is not its own address? That
   is the frame a relay emits. Step 3 of the on-air sequence in
   `wiki/OpenMANET-Interop.md` answers it, once the first answer is yes.

**Do not test the first by overhearing.** A Warthog "in range of two peers but
addressed by neither" receives frames whose addr1 is another station. Every
802.11 receiver filters on addr1 — acknowledgement depends on it — so that setup
reads zero whether addr3 is filtered or not.

### The experiment

Two nodes: a Linux 802.11s node (an OpenMANET node, or an MM8108 adapter on a
Linux host) and one Warthog, peered — `mesh plink: ESTAB` on the Linux side.
Use `warthog-mesh-sae` against an SAE peer, the pairing that has already peered
on this hardware; `AT+MESHPASS` is ignored on an open build. The third party is
fabricated and never has to exist.

On the Warthog, keep `AT+MESHFWD=0` and `AT+MESHBRIDGE=0`, so a relay frame is
counted by `fwdcand` and then dropped with reason 93. The two are readouts of
the same check (both require Mesh Control, both compare addr3 with our
address), not independent evidence; 93 confirms the frame went on to decrypt.
Set every stored gate first and reboot once before the baseline: only a reboot
clears the counters this reads.

On the Linux node, with `W` the Warthog's MAC from `iw dev wlh0 station dump`
and an unused address on the subnet of the bridge that holds the mesh
interface:

```
X=02:00:00:de:ad:01
iw dev wlh0 mpath new $X next_hop $W
ip neigh replace 10.41.99.99 lladdr $X nud permanent dev br-lan
ping -c 50 -i 0.2 -W 1 10.41.99.99     # no replies: expected
iw dev wlh0 mpath del $X; ip neigh del 10.41.99.99 dev br-lan
```

Every echo leaves as a 4-address frame with addr1 = `W` and addr3 = `X`. Read
`tx packets` for `W` in `iw dev wlh0 station dump` before and after; it must
rise by at least 50, or nothing was sent.

In the same minute, as the positive control, run `AT+MPING=<Linux node>,20` on
the Warthog: `delivered=` in `AT+DATASTAT?` must rise by at least 20. If it
does not, the link is the finding and the run is void.

Then read `AT+RXCHAN?` and `AT+DATASTAT?`:

| Reading | Answer |
|---|---|
| `fwdcand` up by about 50, its last destination the low octets of `X`, `rxdrop` up with `reason=93` | **Yes.** The chip delivers relay frames. |
| `fwdcand` up by about 50 but `reason=4` instead of 93 | **Yes** for the chip — it delivered them — but they did not decrypt. The keyed path is the next problem, not the chip. |
| `fwdcand` flat and `data` up by no more than the control accounts for, while the Linux node's `tx packets` rose by 50 | **Probably no**: the chip filters on addr3, and host-side unicast relaying is impossible without Morse Micro. `data` is counted when a page reaches the host's page handler, so a host page-level drop (checksum, sync, allocation) would read the same — rule those out before concluding. |
| `data` up by about 50 but `fwdcand` flat | The chip delivered and the host dropped it earlier — see `AT+FILTSTAT?` and `stad_miss` in `AT+DATASTAT?`. The chip's answer is still yes. |
| The Linux node's `tx packets` did not move | Void: the path was never used. |

On a build with the receive tap (`warthog-mesh-smoke`), `AT+RXHEAD?` shows the
last data frame's addr3 directly, at hex characters 32–43; other builds return
ERROR for it.

A two-Warthog version is possible in principle — with forwarding off, a unicast
for an unknown destination goes to the first peer, addressed to it, carrying
the unknown address as addr3 — but no AT command sends to an arbitrary MAC
today, so it needs one added.

## Current on-air posture

The Mesh Capability octet sets "accepting additional mesh peerings" (0x01)
while one of the 4 peer slots is free (and in a probe response to a node that
holds one), and adds the forwarding bit (0x08) only while `AT+MESHFWD=1`, in
beacons, probe responses and peering frames (`umac_mesh_ies.c`; under SAE,
hostap's `mesh_fwding` is set from the same gate in `supplicant_core_mesh.c`,
and hostap's own peering frames always set 0x01).
The gate is read at mesh start. A leaf relays no PREQ for another node, so no
peer discovers a path through it (`umac_mesh.c`, `umac_mesh_hwmp_relay.c`).
