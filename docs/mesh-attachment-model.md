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
3. **OpenMANET interoperates at the 802.11s layer without batman-adv.** The
   verified cross-vendor result (`docs/mesh-openmanet.md`) is plain 802.11s.
   batman-adv is a routing layer *on top* of the L2 mesh, not a precondition
   for joining it.

### What not having batman-adv actually costs — read this part

This section previously asserted that an idiomatic OpenMANET node enslaves its
HaLow interface to `bat0` and that connecting a Warthog therefore means
dismantling the peer's batman fabric. **That was asserted, not observed, and
the observed default contradicts it.**

Measured 2026-09-21 on two Pi 4 / MM6108 nodes running OpenMANET 24.10
(`r28739-d9340319c6`), untouched apart from the mesh credentials:

```
# ip -br link show type batadv   -> (nothing)
# ip addr show bat0              -> Device "bat0" does not exist.
# ls /sys/class/net/br-lan/brif/ -> eth0  phy1-ap0  wlh0
# lsmod | grep batman            -> batman_adv 208896 0      <- refcount 0
# uci show network.bat0          -> network.bat0.multicast_mode='0'   (no proto, no device)
```

So on this release the HaLow mesh interface `wlh0` is a **direct member of
`br-lan`**, alongside `eth0` and the 5 GHz AP. batman-adv is built and loaded
but has no devices and is not in the data path. `network.bat0` is a stub the
image ships; something — a profile, or the wizard under options not exercised
here — would have to instantiate it.

What this changes: for a peer in this configuration, Warthog does **not** have
to dismantle anything, because there is no batman fabric to dismantle. The
fabric mismatch is real only against a node that has actually been put on
`bat0`, and that is a configuration to check rather than an assumption to
design around. Check it with `ip addr show bat0` before believing either
story.

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

## Settle the forwarding question before writing forwarding

Two independent reviews disagree on whether the MM6108 hands up a mesh data
frame whose destination is a third party. One reads the chip as a lower MAC
that receives whatever the host can parse; the other found a recorded
chip-firmware addr3 filter that would make host-side forwarding impossible
without a firmware change. That is the difference between a two-week feature
and a blocked one, and it is not worth writing a thousand lines of relay
logic to find out — a half-working relay blackholes traffic, which is exactly
why the Forwarding capability bit is deliberately clear today.

**The probe is in the firmware.** `AT+RXCHAN?` reports `fwdcand=N(xxxxxx)`:
mesh data frames whose destination is neither us nor a group address, counted
before any of our own drops, with the low three octets of the most recent
such destination.

Run it with three nodes: two peers exchanging traffic with each other, and a
Warthog in range of both but addressed by neither.

| Result | Meaning | Next step |
|---|---|---|
| `fwdcand` climbing | The chip delivers third-party frames. Forwarding is host-side work. | Build it: RX re-enqueue, TTL decrement, duplicate suppression by mesh sequence number, then set the Forwarding bit last. |
| `fwdcand` stays 0 | The chip filters on the mesh destination. | Host-side forwarding is impossible; raise it with Morse Micro. Do not write the relay. |

Read `ae=` in the same query: a bridged peer reaching us at all is the other
precondition for being useful on an idiomatic OpenMANET network.

## Current on-air posture

The Mesh Capability octet advertises `0x01` — "accepting additional mesh
peerings" — and deliberately leaves the forwarding bit (0x08) clear
(`umac_mesh_ies.c:37-44`). That is correct while forwarding is unimplemented:
setting it would invite a mac80211 peer to select Warthog as an intermediate
hop and blackhole that traffic. **Setting that bit is the last step of adding
forwarding, not the first.**
