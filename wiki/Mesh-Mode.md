# Mesh Mode — 802.11s peers, no infrastructure

Instead of associating to an access point, a Warthog node can join an 802.11s
mesh. Every node is a peer. There is nothing to elect, nothing to associate to,
and a node that loses power takes only its own links with it.

This is the mode to use when there is no infrastructure to join — field
deployments, convoys, anything that has to come up on its own.

## Building for mesh

Mesh is mutually exclusive with station mode, but it is a runtime choice:
`AT+MESHEN=1` then `AT+RESET` turns it on for any build, region images
included. The mesh envs exist because they pin a channel and fix the identity
at build time, so a bench of boards agrees with no configuration at all:

```bash
pio run -e warthog-mesh-smoke -t upload
```

The env pins the radio to one S1G channel and fixes the mesh identity, so nodes
agree with no configuration at all. **These settings must be identical on every
node in the mesh** — a mismatch is silent, producing no error and no peers:

| Flag | Default |
|---|---|
| `WARTHOG_MESH_ID` | `halowmesh` |
| `WARTHOG_PIN_S1G_CHAN` | `42` |
| `WARTHOG_PIN_S1G_FREQ_HZ` | `923000000` |
| `WARTHOG_PIN_S1G_BW_MHZ` | `2` |
| `WARTHOG_PIN_S1G_GLOBAL_OP_CLASS` | `69` |

Channel 42 at 2 MHz is what Warthog pins by default. It must match your peer
exactly — read the peer's actual setting rather than assuming a default; see
[OpenMANET Interop](OpenMANET-Interop).

## Addressing

A node first asks for a DHCP lease over the mesh and waits up to 6 s. A peer
whose mesh interface sits in a bridge with a DHCP server on it (an OpenMANET
node with `wlh0` in `br-lan`) can answer; that path is not yet measured on air.
With no offer — every warthog-only mesh — the node falls back to a static
address derived from its own MAC. `AT+MESHDHCP=0` skips the lease attempt.
Batman mode differs: the address comes up on the first batman route, with a 45 s
DHCP wait and an ARP-probed `10.41.253.x/16` fallback via `10.41.0.1`, beside which
DHCP keeps being asked ([Batman Mode](Batman-Mode#what-changes-on-the-warthog)).

```
10.77.<mac[4]>.<mac[5]> / 255.255.0.0
```

`3c:1a:cc:4c:83:a5` becomes `10.77.131.165`. The static mesh is one flat
`10.77.0.0/16`, so nodes whose third octet differs are still on-link. Ask a node
what it picked with `AT+STATUS?`.

On the static address the gateway is the node's own address, so **mesh mode
has no upstream route** — it is a network between peers, not a path to the
internet. The address is also
applied on first peer establishment rather than at boot, so a node that has not
peered yet has no mesh address.

## Bringing up a mesh

Flash two or more boards and power them. Peering is automatic. After ~30 s:

```
AT+MPMPEERS?
+MPMPEERS: self=4c83a5 4dc7f8 llid=44921 plid=26523 estab=1 opens=0; ...
```

`estab=1` with a non-zero `plid` means a complete two-way handshake. Then check
data:

```
AT+MPING=10.77.199.248,4
+MPING: reply from 10.77.199.248 seq=1 time=15ms
+MPING: 4 sent, 4 received, 0% loss
```

## Forwarding

`AT+MESHFWD=1` then `AT+RESET` turns a node from a leaf into a relay. What
changes, all of it 802.11s as vanilla mac80211 does it, within Warthog's own
limits, unless a point says otherwise:

- **Path selection is relayed.** A PREQ for a third party is rebroadcast with
  the hop cost added; the path back to its originator is installed via
  whoever handed it over. A PREP is carried back along that path. A PERR from
  our next hop for a destination deactivates the path and is passed on; a
  PERR from a node that is not our next hop is ignored, so a third party
  cannot knock out routes it is not on. The same request heard twice — via a
  second neighbour, replayed, or forged with a stale number — is neither
  answered nor forwarded. A route through a different next hop must be 10 %
  better to replace ours at an equal sequence number; OpenMANET's patched
  mac80211 (999-0027) demands that at a newer sequence number too.
- **Data is relayed.** A unicast whose mesh destination is someone else goes
  to that destination's next hop at TTL − 1; with no path it is held while
  the relay discovers the destination itself (below), and no PERR goes back.
  A group frame is delivered locally and rebroadcast once at
  TTL − 1, with its original source and sequence number kept so every relay's
  duplicate cache sees the same identity — that cache is what stops two
  relays in range of each other rebroadcasting a frame to each other until
  TTL runs out. Under the chip's group-key constraint the rebroadcast goes
  out as one unicast per peer (excluding the sender) carrying the group
  address in Address Extension, which a mac80211 receiver rebuilds into the
  real Ethernet frame and floods on its bridge.
- **A path lives as long as its originator says.** Its expiry is the
  Lifetime field of the PREQ or PREP that installed it (TU × 1.024, rounded
  down), only ever extended, and capped at 60 s. An upstream mac80211 node
  advertises 4882 TU (4999 ms); OpenMANET 1.8.0 advertises 48828 TU
  (49999 ms). The table holds 32 paths. A lapsed or dead path stays listed
  in `AT+MESHPATH?` until 600 s after its expiry, then is freed, as mac80211
  does.
- **Paths are refreshed before they lapse.** A path carrying traffic we
  originate triggers a fresh PREQ when under a second is left, while still
  carrying traffic, so a multi-hop flow never waits on rediscovery. A relay
  does not refresh a path it only forwards on. OpenMANET's originator
  refreshes when under 10 s is left.
- **The first frame of a flow waits for the PREP.** A unicast we originate
  to a node with no path yet is held (up to 4 frames, 2 per destination, 3 s)
  and sent when the path is installed, as mac80211 does, instead of being lost the way an
  unanswered ARP is. Held frames are released or dropped both when a
  path-selection frame arrives and on the 2 s service tick, so a peer that
  never answers cannot park transmit buffers, and a target still waiting has
  its PREQ re-asked on that tick — one broadcast PREQ is unacknowledged, so
  losing it must cost a delay rather than the frame. The store is deliberately
  shallow (4 frames, 2 per destination, 3 s) against mac80211's 10 per path
  (OpenMANET's 50) and its four-step retry ladder: these are transmit-pool
  buffers shared with our own traffic and with peering, and a burst opened before discovery completes
  keeps its newest frames, not its oldest.
- **A relayed frame with no path is held while the relay discovers it.** As
  OpenMANET's patched mac80211 does (999-0027), a unicast to relay whose mesh
  destination has no path and is not a peer (under SAE, one whose link AMPE
  has keyed) is kept, and the relay sends its
  own PREQ for that destination at 0, 0.4, 1.2, 2.8 and 4.8 s (a 400 ms
  timeout doubling to a 2 s cap, four retries), with OpenMANET's 48828 TU
  lifetime so the path its PREP installs lasts as long as the upstream's (our
  own discoveries keep 4882 TU). A step counts only once its PREQ has gone
  out, so one held back by the rate limit below, still queued for the event
  loop or left without a buffer is retried, not skipped; no two PREQs for a
  destination are allowed within the 400 ms per-destination interval. The
  PREP releases the held frames
  oldest first, exactly as forwards: the originator's mesh addresses, sequence
  number and Address Extension, one hop of TTL. Unanswered, they are dropped
  silently at 6.8 s. No PERR is sent: vanilla mac80211 sends one
  (no-forwarding, sequence number 0), no OpenMANET configuration does. The
  store takes 4 relayed frames, 2 per destination, oldest dropped first, in
  slots apart from our own 4 so relaying never evicts them. Of the 20-block
  transmit pool, 4 own + 4 relayed + 8 queued to one next hop still leaves 4
  for our own traffic and the PREQ/PREP that resolve the hold; OpenMANET
  queues 50 per destination. A leaf or bridge-only node never holds or
  discovers for anyone else, and a frame whose mesh destination is a group
  address is dropped as no path, never discovered (mac80211 refuses a path to
  one). Nothing is held while the transmit pool is paused (its PKTMEM
  flow-control source; OpenMANET's patch 900 drops forwards while the queue is
  stopped); a scan or standby pause does not refuse a hold. A released frame
  whose next hop already has 8 queued is dropped like a forward. There is no
  gate fallback: Warthog does not act on RANN. `AT+MESHFWDSTAT?` `hold` counts
  frames held, sent and lost; `AT+MESHPATH?` `relay_held` is how many wait
  now.
- **Discovery is rate-limited.** PREQs are allowed at most once per target
  per 400 ms, however many targets are asked, and at most one every 50 ms
  overall (one queued for the event loop leaves when the loop runs it), so a
  host scanning unknown addresses cannot turn the node into a broadcast
  source. A relay's PREQ held
  back by either limit goes out when it clears; its ladder step waits for it.
- **Proxied endpoints are learned.** A frame that arrived with Address
  Extension teaches which mesh node the real source sits behind; a later
  frame to that host goes to that node with both ends in AE 2.
- **Losing a neighbour** drops every path through it and announces up to 8
  of those destinations with a PERR at its sequence number + 1 — the +1 is
  what makes a node routing through us accept the announcement as newer than
  what it holds. mac80211 differs: it deletes those paths silently when a
  peer link closes, and sends a PERR for them only when transmit failures
  break a link, at most one per 100 TU.
- **The Mesh Configuration capability** advertises Forwarding only while the
  gate is on, so a peer never routes through a node that will drop its
  frames.
- **What a relay refuses.** Path selection from anyone not an established
  peer — a transmitter-address compare, not an authentication boundary, for a
  peer that runs without management-frame protection; under SAE a station the
  supplicant added before AMPE keyed its link is not one, in any mode
  (`unestab`), and no frame held for discovery is handed to it; from a keyed
  SAE peer that runs MFP, plaintext unicast path selection; from any SAE peer,
  group path selection in the clear or with an MMIE (so a relay never re-sends
  under its MGTK what anyone could have sent in a peer's name), and protected
  group path selection not opened by host CCMP under that peer's MGTK or
  replayed (see the README's MFP section; `mgmt gp`); from any SAE peer,
  protected unicast path selection under a group key (`mgmt prot grpkey`);
  and on any mesh, once a peer has sent protected unicast path selection, a
  plaintext unicast frame claiming to be it (protected group path selection
  marks nothing: mac80211 protects it whatever the sender's MFP)
  (`AT+MESHFWDSTAT?` `prot`/`unprotected`/`unestab`/`gp`/`mmie`/`nommie`);
  a proxied address that
  is us, a neighbour, or a node we hold a path to, so one Address Extension
  frame cannot redirect a neighbour's traffic; more than 8 hosts per node,
  or any newcomer while the host table is full of live entries; path
  selection naming a destination we hold no path for (a PREQ's originator or
  a PREP's target, including the answer to our own discovery) while every
  path slot is live (`AT+MESHFWDSTAT?` `tblfull`); and a forwarded
  frame when the next hop already has 8 queued, so a relay cannot starve its
  own traffic or peering of buffers.
- **Under SAE, relays reach each other's group path selection only through
  host CCMP.** A relay's or bridge's group PREQs and PERRs go out Protected
  under its own MGTK, and a chip holds only its own, so on `warthog-mesh-sae`,
  `-nochipkey`, and a swccmp build with host CCMP off, no relay takes another
  Warthog's, at either `AT+MESHPMF` setting and even when every Warthog runs
  the same image. Relay discovery between Warthogs under SAE needs a swccmp
  build with host CCMP on (`-swccmp-on`, or `AT+SWCCMP=1` after each boot).

Read the state with `AT+MESHPATH?` and the counters with `AT+MESHFWDSTAT?`.

**Group frames and OpenMANET, honestly.** Under SAE a warthog's chip cannot
decrypt a peer's group frames — its one group slot holds its own TX MGTK — so
by default a broadcast leaves as one unicast per peer. In leaf mode that copy
is a plain unicast to the peer with no Address Extension. With `AT+MESHFWD=1`
or `AT+MESHBRIDGE=1` it carries the group address in Address Extension 2,
which a warthog relay recognises as the broadcast it is and re-floods. A
mac80211 receiver — traced through the 6.6 source — rebuilds the AE 2 form as
an Ethernet frame to the group, delivers it to its own bridge, learns the
proxy, and **does not re-flood it**: no PERR, no onward broadcast; the plain
leaf copy is addressed to that node and goes no further either. So with the
default a warthog's broadcast (ARP, DHCP, mDNS, Meshtastic UDP) reaches a
Linux node and stops there; nothing beyond a Linux relay hears it. The reverse
direction works on an open mesh, because Linux sends standard frames; under
SAE, receiving them needs host CCMP (measured on air on 2026-09-29,
`warthog-mesh-sae-swccmp` against OpenMANET 1.8.0). Whenever a Linux node is
expected to relay a warthog's broadcasts, set `AT+MESHGRP=1`. `AT+MESHGRP=1` switches to standard 3-address broadcasts, which every
mac80211 receiver floods correctly. Under SAE they go out under the sender's
own MGTK, which each peer receives in AMPE; a warthog receiver decrypts them
only with host CCMP, and a Linux receiver doing so is not yet measured. On an
open mesh, use it.

**Two deliberate deviations from mac80211, both on the wire.**

- *Every PREQ we originate sets Target Only, including the first one.* Only
  the target may answer, so a relay that already holds a path to it forwards
  instead of replying and the discovery walks the full path and back.
  mac80211 sets the bit on a path refresh — where we match it exactly — but
  leaves it clear on an initial discovery, so an intermediate node may answer
  in one hop; OpenMANET's patched mac80211 (999-0027) sets it on every
  data-triggered PREQ and every retry, as we do. What we give up is that
  optimisation, not connectivity: the
  target itself answers any PREQ naming it whatever the bit says, and relays
  forward a Target Only PREQ unchanged. The path we install is then always the
  target's own answer rather than a relay's cached idea of it, which is the
  conservative reading. Changing it means changing frames that go on the air,
  so it waits for a bench. (The per-target flags have exactly two bits, Target
  Only and Unknown Sequence Number; "reply and forward" is a receive-side
  notion in mac80211, not something an originator can set.)
- *An `AT+MESHGRP=0` group replica teaches a Linux peer a proxy entry that is
  not one.* The per-peer replica carries the real source in Address Extension
  mode 2. For a frame the node originates itself that source is its own mesh
  address, so the peer learns "this node is proxied behind itself"; for a
  group frame we relay it is the *originator's* address, so the peer learns a
  third mesh node as a host sitting behind us. Our own receive side refuses
  both — a proxied address that is us, a peer, or a node we hold a path to is
  never learned — but a mac80211 receiver has no such guard. The entry does no
  harm until that peer's mesh path to the address lapses or is cleared by a
  PERR: mac80211 then finds both a proxy entry and an inactive path for the
  same address and deletes the path, losing its sequence number, metric and
  retry state and cancelling any discovery in flight, so it starts over each
  time the path ages out. Queued frames are not lost — mac80211 only queues
  while a path is resolving, and that state suppresses the proxy lookup. This
  is a second and independent reason to set `AT+MESHGRP=1` on any mesh with a
  Linux node in it; warthog-to-warthog is unaffected. Traced through the 6.6
  receive path, not measured.

**Bridge mode implies the tables.** `AT+MESHBRIDGE=1` runs the same receive
engine and path-selection handling in leaf mode even with forwarding off,
because a bridge must learn which node each remote host sits behind and hold
paths to non-neighbours; it still relays nothing for others.

**How much of this is verified.** Every decision above is a freestanding
function the host suite tests directly, and `sim_mesh` drives a 3–4 node
mesh through the shipping code — carrying the **exact bytes the firmware
emits**: the MAC header comes from the same `umac_mesh_fwd_tx_header()` the
SDK builder calls, and the receive side parses it with
`umac_mesh_fwd_parse_frame()` before the engine sees it. That binding found
two firmware bugs a struct-based simulator had passed. Eighteen scenarios:
unicast through a relay exactly once; a flood reaching every node exactly
once; a triangle and a ring under a 30-frame burst without a storm; hosts
behind opposite ends reaching each other with their real addresses; a lost
link announced two hops away and rediscovered at a newer sequence number;
TTL dying where it should; a leaf relaying nothing; a forged high-SN PREQ
from a node in range but not peered changing nothing; a bystander's PERR
ignored; a deliberately poisoned two-relay loop dying on TTL; and, under a
modelled single chip group-key slot, standard group frames failing exactly
where the measured hardware fails and recovering with host CCMP or with
per-peer replicas; the first frame of a flow held through discovery and
delivered on the PREP, a burst before the PREP bounded and rate-limited to
one PREQ, an unreachable target's frame released when it lapses, and a lost
PREQ costing a delay rather than the held frame because the tick re-asks; and
a ten-second flow over a five-second path lifetime losing nothing because the
path is refreshed in use. What is **not** verified is the radio: whether the MM6108 hands up
a 4-address frame whose mesh destination is a third party (`AT+RXCHAN?`
`fwdcand`), and whether it transmits one whose addr4 is not its own. Both
need a board. Until then forwarding is compiled, simulated and off by
default.

## Bridge mode

`AT+MESHBRIDGE=1` then `AT+RESET` replaces NAT with one layer-2 segment. The
USB netif, the Wi-Fi AP and the mesh netif become ports of an lwIP bridge
whose MAC is the mesh MAC; the bridge is the node's L3 interface and takes a
DHCP lease from the mesh (a bridged OpenMANET node's dnsmasq) or the static
`10.77.x.y` fallback, exactly as the mesh netif did.

What this buys: a tethered host's frames leave the mesh carrying the host's
own MAC in Address Extension and come back the same way, and its address
comes from the mesh's DHCP server, so two hosts on opposite sides of a mesh
are distinct. That is the precondition CoT and mDNS discovery were missing —
under NAT every warthog's host is `192.168.4.x`, and the addresses those
protocols carry in their payloads alias the receiver's own subnet. Against
OpenMANET this holds only for a node whose mesh interface is a bridge port: a
node set up by its mesh wizard keeps its DHCP server and applications on
`br-ahwlan` behind `bat0`, which a Warthog reaches only as a batman member
([Batman Mode](Batman-Mode)).

What it costs: the USB and AP DHCP servers stop, so a tethered host gets an
address only if the mesh has a DHCP server (on a warthog-only mesh, use a
static or link-local address); NAT and the Meshtastic multicast repeater are
off, because the bridge floods L2 multicast itself; and the node needs
`CONFIG_ESP_NETIF_BRIDGE_EN`, which the shipped `sdkconfig.defaults` now sets.
An image built without it logs the fact and stays in NAT mode rather than
pretending — and so does a bridge that fails to build for any other reason:
the mesh netif then takes its own address, so the node still has L3 over the
mesh. Nothing is torn down until the bridge is whole, and the setting is read
once at boot, so a node never waits on a bridge that is not coming.

**How much of this is verified.** The address logic — which frames get
Address Extension, which mesh node a host sits behind, what a receiver
delivers — is the same freestanding code the simulator drives (scenario 4:
hosts behind opposite ends reach each other with their real addresses). The
lwIP bridge itself and its interaction with the USB and AP drivers are
compiled and reasoned, not run. Until a board is on the bench this is a mode
that builds, not one that has carried a packet.

## Batman mode

`AT+MESHBATMAN=1` then `AT+RESET` runs a BATMAN_V member on the mesh instead of
plain 802.11s addressing: the mode for a mesh built by OpenMANET's mesh wizard,
where every node's 802.11s interface belongs to `bat0`. Forwarding and bridge
must be off; the HaLow netif becomes the batman soft interface, and only batman
frames cross the mesh. Measured on air on 2026-09-29/30
(`warthog-mesh-sae-swccmp`) against two OpenMANET 1.8.0 Pis one hop away. See
[Batman Mode](Batman-Mode#measured-on-air).

## Encryption

Warthog has three mesh security levels. Which one the image can speak is a
build choice; the passphrase and the legacy shared key are runtime settings:

| Mode | Build | What it is |
|---|---|---|
| **SAE/AMPE** | `warthog-mesh-sae`; `warthog-mesh-sae-swccmp` against OpenMANET | Real 802.11s security: SAE (Dragonfly, group 19) authentication and AMPE per-link key exchange. This is the mode to use. Only the host-CCMP build opens a Linux node's group frames, which its path to the Warthog needs ([OpenMANET Interop](OpenMANET-Interop#management-frame-protection-peering-does-not-need-it-path-selection-does)). |
| Shared key | `warthog-mesh-smoke` + `AT+MESHSEC=1` | One hardcoded key baked into every image — obfuscation, not security. Kept for bring-up debugging only. |
| Open | `warthog-mesh-smoke` + `AT+MESHSEC=0` | No keys. Interops with an open OpenMANET mesh. |

### SAE/AMPE

```
pio run -e warthog-mesh-sae -t upload
```

> **Mesh ID, passphrase and mesh mode are runtime settings.** `AT+MESHEN=1`
> turns mesh on (any build, including the region envs), `AT+MESHID=<id>` and
> `AT+MESHPASS=<pass>` set the credentials. All persist to NVS and take effect
> on the next boot. `AT+MESHPASS?` reports only the length, deliberately.
>
> **Channel is settable too, as a set:**
> `AT+MESHCHAN=42,923000000,69,2,2` then `AT+RESET`. Read it back with
> `AT+MESHCHAN?` — it reports `applied=yes|NO`, and `NO` means the regulatory
> table refused the set and the radio fell back, which is the usual cause of
> "peers with nothing and looks like a range problem". A refused set is
> discarded rather than retried every boot. `AT+MESHCHAN=default` restores the
> build-time pin on a mesh env, and on a region env returns the radio to the
> full country list.
>
> **The mesh envs pin a channel; the region envs do not.** A region build
> carries the whole country list, so until you set `AT+MESHCHAN=` its operating
> channel is neither chosen nor observable — pin one before expecting it to
> meet a mesh. `AT+MESHCFG?` reports the channel that actually applied, which
> is not the same thing as the one configured.
>
> **Region is still build-time.** The mesh envs hard-code `WARTHOG_REGION_US` and pin
> the radio to S1G channel 42 (923.0 MHz, 2 MHz, global op class 69). That
> matches OpenMANET's US default, which is why it is the default here. Outside
> the US, or against a peer on another channel, override the pin — the five
> values move together, because operating class and bandwidth belong to the
> channel and a mismatch peers with nothing while looking like a radio fault:
>
> ```bash
> pio run -e warthog-mesh-sae \
>   --build-flag='-UWARTHOG_REGION_US' --build-flag='-DWARTHOG_REGION_EU=1' \
>   --build-flag='-UWARTHOG_PIN_S1G_CHAN'          --build-flag='-DWARTHOG_PIN_S1G_CHAN=<ch>' \
>   --build-flag='-UWARTHOG_PIN_S1G_FREQ_HZ'       --build-flag='-DWARTHOG_PIN_S1G_FREQ_HZ=<hz>' \
>   --build-flag='-UWARTHOG_PIN_S1G_GLOBAL_OP_CLASS' --build-flag='-DWARTHOG_PIN_S1G_GLOBAL_OP_CLASS=<n>' \
>   --build-flag='-UWARTHOG_PIN_S1G_BW_MHZ'        --build-flag='-DWARTHOG_PIN_S1G_BW_MHZ=<mhz>'
> ```
>
> Take the values from your peer: `morse_cli -i wlh0 channel` on an OpenMANET
> node prints the operating frequency, bandwidth and primary width it is
> actually using. Confirm the result the same way on the Warthog side before
> trusting a link.


Set the passphrase with `AT+MESHPASS=<pass>` (persisted, next boot);
`AT+MESHPASS?` reports its length and never its value. Until one is set the
node uses the build default `warthog-mesh`, which anyone holding the image
knows — override it with `-DWARTHOG_MESH_PASSPHRASE='"your-passphrase"'` if
images must be safe before they are configured. Every node on the mesh needs
the same one.

On boot the node authenticates each SAE peer it discovers (SAE Commit/Confirm).
Warthog always starts SAE with group 19 (NIST P-256) and accepts a peer's
commit in 19, 20 or 21 (P-384, P-521). Keep 19 in an OpenMANET `sae_group`
list; a list without it relies on the peer's commit and is unmeasured. The
MODP groups 15 and 16 are not supported. AMPE then derives a per-link
pairwise key (MTK) and each side sends the other its own group key (MGTK).
The MTK and Warthog's own MGTK go
into the chip; a peer's MGTK stays in the host keychain, because the chip has
one group slot. Peering completes in a single
Open/Confirm exchange and data flows CCMP-encrypted end to end. Verify:

```
AT+SAERX?
+SAERX: ... ESTAB=1 ...
AT+MPMPEERS?
+MPMPEERS: ... ampe_mtk=1 ampe_mgtk=2 ...
AT+KEYINST?
+KEYINST: n=2 [aid=1 pw=1 ...] [aid=0 pw=0 ... hw=1]
```

`ampe_mtk` counts pairwise keys installed in the chip; `ampe_mgtk` counts
Warthog's own MGTK, installed with the first peer, plus each peer's MGTK in the
host keychain, so one peer reads 2. `AT+KEYINST?` shows the pairwise key on the
peer's AID and Warthog's own group key on AID 0. Bench-measured: peering +
keying in one exchange, 8/8 pings at 0% loss, ~16 ms RTT over the keyed link.

Each MGTK travels with a Key RSC, the receiver's replay floor for it: group
frames at or below it are dropped (rxdrop 5). Warthog installs a peer's MGTK
with the RSC that peer advertised (little-endian, as mac80211 reads it);
installing the same key again on a live link keeps its counter. The floor is
enforced only where host CCMP decrypts a peer's group frames:
`warthog-mesh-sae-swccmp` with `AT+SWCCMP=1`, or `-swccmp-on`. Other builds
drop those frames before the replay check (rxdrop 4, or 95). What Warthog
advertises for its own MGTK depends on the build: `warthog-mesh-sae-swccmp`
and `-swccmp-on` put it into the chip at a nonzero TX PN base and advertise one
below it, re-installing at a fresh base when an Open follows group traffic
(`mgtk_reinst` on `AT+MPMPEERS?`); every other build installs it at PN 0 and
advertises 0. Neither is measured on air. On the swccmp builds a relay's or
bridge's group PREQs and PERRs go out under our MGTK too, so `mgtk_reinst`
climbs with AMPE Opens at the default `AT+MESHGRP=0`, and whether any receiver
takes them rests on the chip starting the key at the PN it was installed with:
if it does not, every one is dropped as a replay and each re-install reuses PNs
under the same key. On every other build a peer that joins or re-peers takes 0
as its floor for our MGTK, so each of our earlier group frames under it (group
data with `AT+MESHGRP=1`, a relay's or bridge's group path selection) can be
replayed into it once until the MGTK changes; the swccmp builds close that.

A SAE node ignores open-mesh nodes sharing the Mesh ID (and vice versa) — the
Mesh Configuration's Authentication Protocol Identifier must match before a
candidate is even offered to the supplicant. The two security worlds coexist
on air without disturbing each other.

A SAE node's beacons and probe responses carry the RSN element its peering
frames carry (hostap's: RSN version 1, CCMP-128, AKM SAE); an open node's carry
none. A mac80211 peer drops a beacon or probe response whose RSN presence does
not match its own mesh security, so without it a beaconing SAE peer such as
OpenMANET never takes Warthog as a candidate. Probe requests carry a
zero-length SSID and the Mesh ID element, the only shape mac80211 answers.
Per source; not measured on air.

`AT+SAEBRIDGE=0` makes a SAE node deaf to peer candidates (a debugging state);
it defaults on.

### Legacy shared key / open

```
AT+MESHSEC?      → +MESHSEC: 1 (keyed)
AT+MESHSEC=0     → open; re-peers within ~2 s
```

Keyed uses **one hardcoded key, identical on every warthog image** — a counting
sequence, `00 11 22 ... ee ff`. Anyone holding the firmware holds the key.
It never matches OpenMANET. A node set up by OpenMANET's mesh wizard runs SAE
and needs `warthog-mesh-sae-swccmp` with host CCMP on (`AT+SWCCMP=1` after each
boot, or batman mode) and its mesh ID and passphrase; `warthog-mesh-sae` peers
with it but cannot open its group frames. Run open only against a node set to
`encryption='none'` (see the [OpenMANET interop page](OpenMANET-Interop)).

The setting persists in NVS, though a factory flash overwrites that partition,
so a freshly reflashed node is keyed again. It has no effect on a SAE build,
which never installs the hardcoded key.

## How paths work, and why it matters

Worth understanding before debugging a mesh, because the failure is
counter-intuitive.

An 802.11s node will not send a **unicast** frame to a neighbour it has no
*path* to, and a peer link reaching ESTAB does not create one — path discovery
(HWMP) does. Group-addressed frames skip discovery entirely.

So a node that does not answer path requests looks like this: broadcast works,
ARP arrives, unicast never leaves, and every status counter reads healthy.

Warthog participates in both directions — on an open mesh it advertises
itself with a path request every 2 s, and in every mode it answers requests
aimed at it:

```
AT+HWMPSTAT?
+HWMPSTAT: rx=234 preq_rx=75 preq_tx=142 prep_rx=159 prep_tx=75 parse_fail=0 not_ours=0 rann_rx=0 perr_rx=0
```

`preq_rx` matching `prep_tx` means every request aimed at us was answered.
`parse_fail` should be 0. `rann_rx` and `perr_rx` count root announcements
(never acted on) and path errors received.

## Peer capacity

A node peers with at most 4 others: one datapath station per peer, and on an
open mesh one peering link per peer.

- **Accepting bit.** Bit 0 of the Mesh Configuration capability octet
  ("Accepting Additional Mesh Peerings") is 1 while a peer slot is free and 0
  while all 4 are taken, in beacons and probe responses, and in Warthog's own
  peering frames on an open mesh; under SAE, hostap's Open and Confirm always
  set it, as OpenMANET's do. A probe response to a node that already holds a
  slot carries 1, so a
  peer that rebooted can peer again. Formation Info carries the number of
  established peers, capped at 63.
- **Open mesh.** An Open from a fifth node is answered with Close reason 53
  (MESH-MAX-PEERS). After receiving Close(53), Warthog sends that node no Open
  for 30 s; an Open from that node is still answered and ends the hold-off.
  While 4 nodes are held off, Warthog opens toward no new node, and a Close(53)
  arriving then holds off every new node for 30 s.
  While full, each established peer is sent a probe response carrying 1 in
  answer to its S1G beacons, at most once per 10 s. Warthog does not open
  toward a neighbour from its beacons when their Mesh Configuration clears
  bit 0 or names another authentication protocol, and answers that
  neighbour's S1G beacons at most once per 10 s (at most 8 such answers in
  any 10 s). A probe request carries no Mesh Configuration, so a
  neighbour's probe requests still draw Opens, repeated on its beacons until
  it answers or 8 go unanswered; a full neighbour's Close(53) holds them off
  for 30 s. An Open from another node takes the link of a neighbour that has
  answered none of Warthog's Opens, and that neighbour is sent Close reason
  52, so it cannot hold the last link. A peering that reaches ESTAB when no
  station can be allocated is closed with reason 53 (`add_fail` in
  `AT+PEERS?`).
- **SAE.** While full, a new node is not offered to the SAE supplicant;
  `offer_full` in `AT+MPMPEERS?` counts these. A node that holds a slot is
  still offered. An established peer that goes silent is never expired: the
  open mesh's 30 s silence expiry does not run under SAE, and the supplicant's
  300 s inactivity check reads every peer as just active from this driver. Its
  slot frees only when a Close from it is heard, when it starts SAE again, or
  when this node restarts, so a node that moves past 4 others it no longer
  hears peers with no fifth (from the code and a one-off host simulation; not
  measured on air). Every S1G beacon naming our mesh from a neighbour above the
  RSSI floor is answered with a probe response, with no 10 s limit: about one
  a second per beaconing OpenMANET neighbour. The slot is taken when the
  supplicant adds the station, before
  SAE completes, so nodes still authenticating count toward the 4 but not
  toward Formation Info. If SAE has not completed when the supplicant's
  10-19 s authentication timer fires, the station and its slot are freed at
  once (`sae_fail`), with no retries and no blocked state, and that node is
  not offered again for 30 s (`held`). An SAE Commit from the node ends the
  hold-off and it is offered at once, if the RSSI floor and a free slot allow.
  So a neighbour that never completes SAE, such as a full node or one that
  never learns Warthog, holds a slot for one timer period and then waits out
  the hold-off, rather than holding it through three retries and the blocked
  state (up to ~300 s). If SAE completes but the peering does not reach
  ESTAB, the supplicant frees the station when its peering state machine
  gives up (at most about 1.6 s of retries and holding), and the node is held
  off the same way (`plink_fail`). While a node is held off, its Mesh Peering
  Open is refused before the supplicant sees it (`held`): the supplicant would
  otherwise give it a station on a key cached from a completed SAE. No group
  frame is queued to a node until AMPE has keyed its link. Host-simulated, not
  measured on air.
- **RSSI floor.** A neighbour heard at or below `AT+MESHRSSI=` (default
  -80 dBm) is not offered to the SAE supplicant, its Mesh Peering Open is
  refused under SAE unless it already holds a slot, and on an open mesh no
  peering is started toward it from its beacons; its S1G beacons are answered
  at most once per 10 s. OpenMANET ignores beacons and probe responses it
  hears at or below its `mesh_rssi_threshold` (-80 dBm on a fresh 1.8.0
  image, -85 on 1.6.5-1.7.x and nodes upgraded from them), so under SAE it
  starts no peering with a node it hears that weakly. The threshold does not apply to an
  Open, so it still takes a PMKSA-cached one from a node it peered with before:
  hostap keeps the key of a completed SAE up to 12 h, until reboot, and
  Warthog sends such an Open toward any node it hears above its own floor.
  Warthog measures the signal at its own receiver, so the two gates agree only
  on a roughly symmetric link. On an open mesh a link below the floor forms
  only when the other side initiates:
  its own Open is answered, and its probe requests draw Warthog's Open, so two
  Warthogs below the floor still peer. Under SAE, between two Warthogs, such a
  link needs the floor lowered on both ends (`AT+MESHRSSI=0` turns it off).
  `skipped` in `AT+MESHRSSI?` counts refusals of frames that name our mesh;
  when the floor has refused new peerings and passed none since the no-peers report before
  last (or since a peer was last up), the no-peers report and `AT+MESHCFG?`
  name it as the cause.

## Limits

- **At most 4 peers per node.** See [Peer capacity](#peer-capacity).
- **No forwarding by default.** A leaf (`AT+MESHFWD=0`, `AT+MESHBRIDGE=0`)
  answers path requests that target it and ignores the rest, so two nodes that
  cannot hear each other will not relay through it. It does learn a host
  behind any mesh node from Address Extension a peer carried, and addresses
  replies to that node in AE 2, handed to the node if it is a peer, else to
  the peer that carried the host's traffic (host-tested, not yet on air). It
  sends no PREQ for them. If that relay has no path to the node, a vanilla
  mac80211 relay drops the reply with a PERR, so first contact from a far
  host on an idle mesh can fail; a warthog relay, and OpenMANET's patched
  mac80211 with forwarding on, hold it and discover the node (warthog
  simulated, OpenMANET read from its source; neither measured).
  Any other unknown unicast goes to the first peer.
  `AT+MESHFWD=1` makes the node a relay ([Forwarding](#forwarding)), which is
  host-tested and simulated, not yet run on air. In batman mode batman relays
  instead ([Batman Mode](Batman-Mode)).
- SAE/AMPE requires a `warthog-mesh-sae` build (`warthog-mesh-sae-swccmp` against OpenMANET); the default smoke build still peers open or with the fixed key.
- **A Linux node's unicast above its RTS threshold arrives from every node only
  on `warthog-mesh-sae-swccmp-meshvif`.** OpenMANET 1.8.0 nodes ran it at 1000
  on the bench, so larger frames go behind RTS/CTS. On the STA chip interface
  every other build runs the mesh on, the Warthog's CTS is taken only by the
  peer the chip registered last, and the other nodes' large frames never arrive
  (the chips' MAC counters on both ends, 2026-09-30). On other builds set each
  node to CTS-to-self or `iw phy <phy> set rts off` (both measured set at
  runtime, neither persistent); see
  [OpenMANET Interop](OpenMANET-Interop#frames-over-about-1000-bytes-from-a-linux-node).
