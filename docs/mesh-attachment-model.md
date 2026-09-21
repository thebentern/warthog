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

An idiomatic OpenMANET node does not run bare 802.11s. Its mesh wizard
enslaves the HaLow interface to a batman device: `proto=batadv_hardif
master=bat0`, with `bat0` running `routing_algo=BATMAN_V`. Everything else on
the box keys off `bat0` — mesh status reads `batctl meshif bat0 originators`,
the ATAK/CoT page tunes `bat0`'s multicast mode for CoT flooding, meshtasticd
pins node identity to it, and `alfred` runs over the HaLow bridge.

Warthog speaks bare 802.11s with a self-assigned `10.77.x.y/16`. So on an
untouched OpenMANET node the radio link can be perfect and Warthog is still
**not on the fabric**: absent from `batctl originators`, unreachable by the
CoT multicast path, outside the L2 domain the other tooling assumes.

Worse, the two are mutually exclusive as currently documented. Warthog's own
setup instructions require the peer's mesh interface to be un-enslaved
(`ip link set wlh0 nomaster`) and given its own 10.77 address — which pulls it
out of `bat0`. **Connecting a Warthog today means dismantling the batman
fabric on that node.** That is the real cost of having no batman-adv, and it
is larger than "some routing metadata is missing".

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

So keeping the operator's `bat0` intact needs, in order:

1. **Address Extension on receive** — accept and parse AE frames instead of
   rejecting them, so proxied traffic from a bridged peer is delivered.
2. **Address Extension on transmit** — set it for anything Warthog forwards
   on behalf of its tethered client, which is also what makes the client
   visible to the rest of the mesh.
3. **Mesh gate behaviour** — announce that Warthog bridges to a non-mesh
   segment, so peers know to send it traffic for addresses it proxies.

Warthog taking a DHCP lease on the mesh (added alongside this note) removes
the *addressing* half of the requirement and is worth having on its own, but
it does not by itself make a bridged peer work. Do not read it as closing
this gap.

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

## An open question before building forwarding

Two independent reviews disagree on whether the MM6108 will hand up a
4-address frame whose addr3 is a third party. One reads the chip as a lower
MAC that transmits and receives whatever header the host builds, making
host-side forwarding straightforward; the other found a recorded chip-firmware
addr3 filter that would make host-side RX forwarding impossible without a
firmware change. **Settle this on hardware before committing to the design** —
it is the difference between a two-week feature and a blocked one. The test is
cheap: have two peers exchange traffic addressed to a third node while a
Warthog is in range, and read whether the frames reach the host at all.

## Current on-air posture

The Mesh Capability octet advertises `0x01` — "accepting additional mesh
peerings" — and deliberately leaves the forwarding bit (0x08) clear
(`umac_mesh_ies.c:37-44`). That is correct while forwarding is unimplemented:
setting it would invite a mac80211 peer to select Warthog as an intermediate
hop and blackhole that traffic. **Setting that bit is the last step of adding
forwarding, not the first.**
