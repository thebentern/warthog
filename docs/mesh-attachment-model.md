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
| HWMP forwarding | ~600–800 LOC, 2–3 weeks | small | small | reuses the mesh port already in the build |
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

### What not having batman-adv does cost

Warthog is invisible to the batman-adv routing layer. On a network where
OpenMANET nodes route with batman-adv:

- Warthog does not originate OGMs, so batman peers never select it as a hop —
  which is moot today, since it cannot forward anyway.
- Warthog never announces its tethered EUD in the translation table, so
  OpenMANET-side EUDs cannot discover that client through batman. Traffic has
  to be addressed explicitly, and NAT (see the README) means inbound needs
  forwarding regardless.

If the goal is a Warthog that participates as a *routed* member of a
batman-adv network — not merely an L2 peer of one — then model C is the honest
requirement. For the drone-relay goal specifically, HWMP forwarding achieves
it at a quarter of the cost.

## Blocking prerequisite for both

Group-addressed frames. The chip holds one VIF-wide MGTK latched at aid 0
while every 802.11s peer generates its own, and Warthog's own TX MGTK was
being dropped as an unknown peer (fixed, unmeasured). Until group frames both
leave and arrive correctly, neither a batman port nor a broadcast-dependent
HWMP path can be trusted on air.

## Current on-air posture

The Mesh Capability octet advertises `0x01` — "accepting additional mesh
peerings" — and deliberately leaves the forwarding bit (0x08) clear
(`umac_mesh_ies.c:37-44`). That is correct while forwarding is unimplemented:
setting it would invite a mac80211 peer to select Warthog as an intermediate
hop and blackhole that traffic. **Setting that bit is the last step of adding
forwarding, not the first.**
