# Mesh attachment model: how Warthog should join an OpenMANET network

Warthog can attach to an OpenMANET HaLow network three ways. This records
which one is built, what each costs, and what each gives up — so the choice is
made on evidence rather than re-argued.

**Short version.** Warthog today is a non-forwarding 802.11s mesh point (model
A) or a plain STA leaf (model B). Neither relays. If you need a relay — an
airborne node extending coverage between two nodes that cannot hear each other
— the answer is 802.11s HWMP forwarding, not batman-adv.

## The models

### A. Non-forwarding 802.11s mesh point — **built**

Warthog peers directly with OpenMANET nodes over 802.11s. SAE/AMPE peering is
verified cross-vendor (2026-09-20: three-node mesh, all links `ESTAB`, two
simultaneous AMPE pairwise keys on one Warthog). Unencrypted data passes at
0–3% loss; the encrypted data plane does not yet, see *Blocking prerequisite*.

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

1. **Batman-adv is broadcast-first, and Warthog cannot currently put a real
   broadcast frame on the air.** OGMs flood; the translation table floods.
   That makes ~80% of a batman port's risk the *same* group-key defect that
   already blocks the encrypted data plane. HWMP's control plane rides the
   management path instead, which the defect does not touch.
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

**OpenMANET's mesh wizard does build a batman fabric.** Read from its source:
`openmanet-meshwizard.ut:84-103,162` binds the HaLow wifi-iface with
`proto=batadv_hardif master=bat0` and gives `bat0` `routing_algo=BATMAN_V`.
`kmod-batman-adv`, `alfred` and `morse_mesh11sd` ship in every image, and the
rest of the tooling keys off `bat0` — `batctl meshif bat0 originators` for
mesh status, `network.bat0.multicast_mode` for CoT flooding, meshtasticd node
identity.

**A node that has not been through that wizard is bare bridged 802.11s.**
Measured 2026-09-21 on two Pi 4 / MM6108 nodes running OpenMANET 24.10
(`r28739-d9340319c6`), configured with mesh credentials only:

```
# ip -br link show type batadv   -> (nothing)
# ip addr show bat0              -> Device "bat0" does not exist.
# ls /sys/class/net/br-lan/brif/ -> eth0  phy1-ap0  wlh0
# lsmod | grep batman            -> batman_adv 208896 0      <- refcount 0
# uci show network.bat0          -> network.bat0.multicast_mode='0'   (no proto, no device)
```

Here `wlh0` is a **direct member of `br-lan`** alongside `eth0` and the 5 GHz
AP. batman-adv is built and loaded but holds no devices and is not in the data
path; `network.bat0` is an un-instantiated stub the image ships.

So the fabric mismatch is a property of the peer's configuration, not of
OpenMANET as such. Against a wizard-configured node it is real and Warthog is
off the fabric. Against a node like these two there is nothing to dismantle and
Warthog's documented setup costs the operator nothing. **Check which one you
have with `ip addr show bat0` before believing either story** — that one
command is the difference, and this document previously told you the answer
without asking the question.

What remains true regardless: Warthog speaks bare 802.11s with a self-assigned
`10.77.x.y/16` and does not forward, so against a node that *is* on `bat0` it
is absent from `batctl originators` and outside that L2 domain.

Two ways out, and they are genuinely different products:

- **A forwarding 802.11s layer.** batman-adv rides over L2; a mesh that
  forwards carries batman frames between OpenMANET nodes transparently,
  without Warthog understanding them. This gets the drone-relay goal and lets
  the peer keep `bat0` intact. It does *not* make Warthog a visible batman
  originator or announce its tethered client in the translation table.
- **A BATMAN_V subset in firmware.** Only this makes Warthog a routed member
  the rest of the OpenMANET tooling can see. Note a licensing constraint:
  batman-adv is GPL-2.0-only and morselib here is GPL-3.0-or-later, so this
  must be a clean-room implementation of the protocol, not a port of the
  Linux source.

For the drone-relay goal, forwarding is the answer. For "Warthog appears in
`batctl originators` and ATAK discovery works end to end", only the subset
does.

## The bridged-peer blocker is Address Extension, not addressing

It is tempting to think a Warthog could join a bridged OpenMANET node if only
it stopped self-assigning `10.77.x.y`. It cannot, and the reason is deeper.

A mesh interface enslaved to a bridge forwards **proxied** traffic — frames
whose original source is some other device on the bridge, not the mesh node
itself. 802.11s carries that with the Mesh Control **Address Extension**
field, which adds the original source (and for group frames, the original
destination) beyond the four 802.11 addresses. Warthog does not implement it:
`umac_mesh_hwmp.h:53` — "Address Extension. We never set it and we reject it
on receive." The measured symptom on the peer is its per-station `tx packets`
freezing at exactly 5.

**Receive-side AE is now implemented** (`umac_datapath.c`, counter `ae=` on
`AT+RXCHAN?`). Warthog delivers a proxied frame to the right host instead of
attributing it to the mesh node, and a non-zero `ae=` is the direct way to
see that a bridged peer is reaching us — a condition previously visible only
as a symptom on the peer.

What is left is **not** simply "AE on transmit". Transmit-side AE has nothing
to carry today, because Warthog NAPTs its tethered client: `main/nat.c` puts
NAPT on the inside netifs and rewrites the source to the HaLow address, so
every frame Warthog sends is genuinely its own. It never proxies for anyone.

So transmit-side AE only becomes useful paired with one of:

- **Forwarding** — relaying another node's traffic means re-emitting frames
  whose endpoints are not ours, which is exactly what AE encodes. This is the
  drone-relay path.
- **Bridging the tethered client instead of NATing it** — an architectural
  change (see the README's "Warthog routes, it does not bridge"), after which
  the client is a real proxied endpoint that AE must announce.

And either of those additionally wants **mesh gate announcement**, so peers
know to send Warthog traffic for the addresses it proxies.

Warthog taking a DHCP lease on the mesh removes the *addressing* half of the
bridged-peer requirement and is worth having on its own, but neither it nor
receive-side AE lets a Warthog fully participate on a peer that keeps its
mesh interface enslaved. Do not read either as closing this gap.

## Prerequisite: group-addressed frames

The chip holds one VIF-wide MGTK while every 802.11s peer generates its own.
That chip limit is not fixable in the chip — it is why the host software CCMP
path exists, and that path (RX *and* TX) is implemented and keeps a
per-transmitter key for every peer in the host keychain. A separate genuine
bug, Warthog's own TX MGTK being rejected as an unknown peer, is fixed.

What remains is **measurement**: no run has yet shown `AT+SWCCMP?` reporting
`ok > 0` on live traffic, and the 3-address group-frame AAD path has never
executed on hardware. Until that is confirmed, treat encrypted group traffic
as unproven — and note that a broadcast-dependent design (batman) inherits
that risk, while HWMP's control plane on the management path does not.

## Cheaper partial relays worth knowing about

Full 802.11s forwarding is the right answer, but two narrower relays are far
closer than it and may cover a specific need:

- **Meshtastic-only relay: one guard.** `main/mudp.c:145` refuses to re-send a
  datagram out the interface it arrived on (`if (out == in ...) continue;`).
  Relaxing that for the mesh netif would relay Meshtastic's multicast between
  two peers that cannot hear each other. Application-layer, one group, but it
  is nearly free.
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
`t_rx_forward_unicast`) — on an open mesh only; no host test runs a keyed relay.

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

The Mesh Capability octet advertises `0x01` — "accepting additional mesh
peerings" — and deliberately leaves the forwarding bit (0x08) clear
(`umac_mesh_ies.c:37-44`). That is correct while forwarding is unimplemented:
setting it would invite a mac80211 peer to select Warthog as an intermediate
hop and blackhole that traffic. **Setting that bit is the last step of adding
forwarding, not the first.**
