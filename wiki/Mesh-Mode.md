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

There is no DHCP on the mesh. Each node derives a static address from its own
MAC:

```
10.77.<mac[4]>.<mac[5]> / 255.255.0.0
```

`3c:1a:cc:4c:83:a5` becomes `10.77.131.165`. The mesh is one flat
`10.77.0.0/16`, so nodes whose third octet differs are still on-link. Ask a node
what it picked with `AT+STATUS?`.

The gateway is the node's own address, so **mesh mode has no upstream route** —
it is a network between peers, not a path to the internet. The address is also
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
changes, all of it 802.11s as mac80211 does it:

- **Path selection is relayed.** A PREQ for a third party is rebroadcast with
  the hop cost added; the path back to its originator is installed via
  whoever handed it over. A PREP is carried back along that path. A PERR from
  our next hop for a destination deactivates the path and is passed on; a
  PERR from a node that is not our next hop is ignored, so a third party
  cannot knock out routes it is not on. The same request heard twice — via a
  second neighbour, replayed, or forged with a stale number — is neither
  answered nor forwarded.
- **Data is relayed.** A unicast whose mesh destination is someone else goes
  to that destination's next hop at TTL − 1; with no path, a PERR goes back
  to the sender. A group frame is delivered locally and rebroadcast once at
  TTL − 1, with its original source and sequence number kept so every relay's
  duplicate cache sees the same identity — that cache is what stops two
  relays in range of each other rebroadcasting a frame to each other until
  TTL runs out. Under the chip's group-key constraint the rebroadcast goes
  out as one unicast per peer (excluding the sender) carrying the group
  address in Address Extension, which a mac80211 receiver rebuilds into the
  real Ethernet frame and floods on its bridge.
- **Proxied endpoints are learned.** A frame that arrived with Address
  Extension teaches which mesh node the real source sits behind; a later
  frame to that host goes to that node with both ends in AE 2.
- **Losing a neighbour** drops every path through it and announces each
  destination with a PERR at its sequence number + 1 — the +1 is what makes
  every other node accept the announcement as newer than what it holds.
- **The Mesh Configuration capability** advertises Forwarding only while the
  gate is on, so a peer never routes through a node that will drop its
  frames.

Read the state with `AT+MESHPATH?` and the counters with `AT+MESHFWDSTAT?`.

**Group frames and OpenMANET, honestly.** This chip cannot key group frames
across more than one SAE peer, so by default a broadcast leaves as one
unicast per peer with the group address in Address Extension 2. A warthog
receiver recognises that as the broadcast it is and re-floods it. A mac80211
receiver rebuilds it as an Ethernet frame to the group and delivers it
locally — on kernels before 6.3 it then floods on the bridge; on 6.3 and
later the receive path plausibly treats a unicast-addressed frame whose
extension DA is a group as one to route onward, fails the next-hop lookup and
answers with a PERR. OpenMANET 24.10 is kernel 6.6. That is not verified
either way and it is the single biggest interop question this design leaves
open. `AT+MESHGRP=1` switches to standard 3-address broadcasts, which every
mac80211 receiver floods correctly, at the cost that under SAE they decrypt
only with one peer or with host CCMP on the receivers. On an open mesh, use
it.

**How much of this is verified.** Every decision above is a freestanding
function the host suite tests directly, and `sim_mesh` drives a 3–4 node
mesh through the shipping code: unicast through a relay exactly once, a
flood reaching every node exactly once and never storming, hosts behind
opposite ends reaching each other with their real addresses, a lost link
announced and acted on, TTL dying where it should, and a leaf relaying
nothing. What is **not** verified is the radio: whether the MM6108 hands up
a 4-address frame whose mesh destination is a third party (`AT+RXCHAN?`
`fwdcand`), and whether it transmits one whose addr4 is not its own. Both
need a board. Until then forwarding is compiled, simulated and off by
default.

## Bridge mode

`AT+MESHBRIDGE=1` then `AT+RESET` replaces NAT with one layer-2 segment. The
USB netif, the Wi-Fi AP and the mesh netif become ports of an lwIP bridge
whose MAC is the mesh MAC; the bridge is the node's L3 interface and takes a
DHCP lease from the mesh (an OpenMANET node's dnsmasq) or the static
`10.77.x.y` fallback, exactly as the mesh netif did.

What this buys: a tethered host's frames leave the mesh carrying the host's
own MAC in Address Extension and come back the same way, and its address
comes from the mesh's DHCP server, so two hosts on opposite sides of a mesh
are distinct. That is the precondition CoT and mDNS discovery were missing —
under NAT every warthog's host is `192.168.4.x`, and the addresses those
protocols carry in their payloads alias the receiver's own subnet.

What it costs: the USB and AP DHCP servers stop, so a tethered host gets an
address only if the mesh has a DHCP server (on a warthog-only mesh, use a
static or link-local address); NAT and the Meshtastic multicast repeater are
off, because the bridge floods L2 multicast itself; and the node needs
`CONFIG_ESP_NETIF_BRIDGE_EN`, which the shipped `sdkconfig.defaults` now sets.
An image built without it logs the fact and stays in NAT mode rather than
pretending.

**How much of this is verified.** The address logic — which frames get
Address Extension, which mesh node a host sits behind, what a receiver
delivers — is the same freestanding code the simulator drives (scenario 4:
hosts behind opposite ends reach each other with their real addresses). The
lwIP bridge itself and its interaction with the USB and AP drivers are
compiled and reasoned, not run. Until a board is on the bench this is a mode
that builds, not one that has carried a packet.

## Encryption

Warthog has three mesh security levels. Which one the image can speak is a
build choice; the passphrase and the legacy shared key are runtime settings:

| Mode | Build | What it is |
|---|---|---|
| **SAE/AMPE** | `warthog-mesh-sae` | Real 802.11s security: SAE (Dragonfly, group 19) authentication and AMPE per-link key exchange. This is the mode to use. |
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

On boot the node authenticates each SAE peer it discovers (SAE Commit/Confirm,
NIST P-256), then AMPE derives a per-link pairwise key (MTK) and a group key
(MGTK) and installs both in the chip. Peering completes in a single
Open/Confirm exchange and data flows CCMP-encrypted end to end. Verify:

```
AT+SAERX?
+SAERX: ... ESTAB=1 ...
AT+MPMPEERS?
+MPMPEERS: ... ampe_mtk=1 ampe_mgtk=1 ...
AT+KEYINST?
+KEYINST: n=2 [aid=1 pw=1 ...] [aid=0 pw=0 ... hw=1]
```

`ampe_mtk`/`ampe_mgtk` count AMPE-derived keys installed in the chip;
`AT+KEYINST?` shows the pairwise key on the peer's AID and the group key on
AID 0. Bench-measured: peering + keying in one exchange, 8/8 pings at 0% loss,
~16 ms RTT over the keyed link.

A SAE node ignores open-mesh nodes sharing the Mesh ID (and vice versa) — the
Mesh Configuration's Authentication Protocol Identifier must match before a
candidate is even offered to the supplicant. The two security worlds coexist
on air without disturbing each other.

`AT+SAEBRIDGE=0` makes a SAE node deaf to peer candidates (a debugging state);
it defaults on.

### Legacy shared key / open

```
AT+MESHSEC?      → +MESHSEC: 1 (keyed)
AT+MESHSEC=0     → open; re-peers within ~2 s
```

Keyed uses **one hardcoded key, identical on every warthog image** — a counting
sequence, `00 11 22 ... ee ff`. Anyone holding the firmware holds the key.
Against OpenMANET you must run open (or use SAE on both sides — see the
[OpenMANET interop page](OpenMANET-Interop)).

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

Warthog participates in both directions — it advertises itself with a path
request every 2 s and answers requests aimed at it:

```
AT+HWMPSTAT?
+HWMPSTAT: rx=234 preq_rx=75 preq_tx=142 prep_rx=159 prep_tx=75 parse_fail=0 not_ours=0
```

`preq_rx` matching `prep_tx` means every request aimed at us was answered.
`parse_fail` should be 0.

## Limits

- **No forwarding.** Warthog answers path requests that target it and ignores
  the rest. Two nodes that cannot hear each other will not relay through a
  Warthog between them.
- SAE/AMPE requires the `warthog-mesh-sae` build; the default smoke build still peers open or with the fixed key.
