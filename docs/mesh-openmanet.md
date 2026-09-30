# 802.11s mesh, and interoperating with OpenMANET / OpenWrt

Warthog can join an 802.11s mesh over HaLow instead of associating to an AP.
Every node is a peer: there is no gateway to elect, no association, and a node
that powers off takes only its own links with it.

This document is the setup procedure, the settings that must match, and the
failure modes — all of it measured against OpenMANET 1.8.0 on a Raspberry Pi 4
with a Seeed HaLow HAT, and two Warthog nodes.

Verified result against a peer hand-configured as an open mesh
(`encryption='none'`, mesh ID `halowmesh`; not the stock or wizard
configuration):

| Direction | Result |
|---|---|
| OpenMANET → Warthog A | 29/30, 3% loss, 8.9 / 19.3 ms |
| OpenMANET → Warthog B | 30/30, 0% loss, 8.6 / 15.2 ms |
| Warthog → OpenMANET | 8/8, 0% loss, 8 / 19 ms |
| Warthog ↔ Warthog | 6/6, 0% loss, ~15 ms |

## Build and flash

`AT+MESHEN=1` puts any build in mesh mode (stored; applied on the next boot).
This env starts in it:

```bash
pio run -e warthog-mesh-smoke
```

That env pins the radio to a single S1G channel and sets the mesh identity, so
every node agrees without any runtime configuration. The values that must match
across the whole mesh are compile-time defaults in `platformio.ini`;
`AT+MESHID=` and `AT+MESHCHAN=` override the first five at runtime (stored;
applied on the next boot):

| Flag | Default | Must match peers |
|---|---|---|
| `WARTHOG_MESH_ID` | `halowmesh` | yes |
| `WARTHOG_PIN_S1G_CHAN` | `42` | yes |
| `WARTHOG_PIN_S1G_FREQ_HZ` | `923000000` | yes |
| `WARTHOG_PIN_S1G_BW_MHZ` | `2` | yes |
| `WARTHOG_PIN_S1G_GLOBAL_OP_CLASS` | `69` | yes |
| `WARTHOG_MESH_BEACON_TU` | `1000` | no |

S1G channel 42 at 2 MHz is what Warthog pins by default. Verify your peer's
actual channel (`morse_cli -i wlh0 channel`) and match it; do not assume.
Change any of the first five and you must change them on every node, Warthog and
Linux alike — a mismatch produces a silent non-event, not an error. The
OpenMANET mesh wizard defaults to mesh ID `openmanet` and passphrase
`changeme123`; set `AT+MESHID=` (and `AT+MESHPASS=` on the SAE build) to what
the peer actually runs.

Flash as usual (hold **BOOT**, tap **RESET**, release BOOT):

```bash
pio run -e warthog-mesh-smoke -t upload
```

For several boards at once, `tools/bench/flash.sh` takes `"SERIAL HUB PORT"`
triples and power-cycles each board through `uhubctl` between writes. It flashes
the `warthog-mesh-smoke` image by default; override with `WARTHOG_ENV`:

```bash
tools/bench/flash.sh "WTHG-0272A1F8738D 0-1 1" "WTHG-021BF681BA51 0-1 2"
```

## Addressing

On its first peer establishment a node asks for a DHCP lease (`AT+MESHDHCP`,
default 1) and waits up to 6 s; a peer whose mesh interface is bridged to a
DHCP server can serve one. With no offer, the node derives a static address
from its MAC:

```
10.77.<mac[4]>.<mac[5]> / 255.255.0.0
```

So `3c:1a:cc:4c:83:a5` is `10.77.131.165`. Statically addressed nodes share one
flat `10.77.0.0/16`, which is why nodes whose third octet differs are still on-link.

**An unbridged Linux peer must use the same derivation**, or nothing will
route. For a card with MAC `e4:5f:01:28:bf:74`:

```sh
ip addr add 10.77.191.116/16 dev wlh0
```

`AT+STATUS?` reports the address the node picked.

Two behaviours worth knowing. With the static address the gateway is set to
the node's **own** address, so the node has no upstream route — the static mesh
is not a path to the internet. And the address is applied lazily, on the first
peer establishment rather than at boot, so a node with no peers yet has no mesh
address to report.

## Setting up an OpenMANET / OpenWrt peer

Tested on OpenMANET 1.8.0, Raspberry Pi 4, Seeed HaLow HAT (MM6108).

Convert the stock HaLow access point into an open mesh that matches
`warthog-mesh-smoke` run with `AT+MESHSEC=0`. This is a hand configuration, not
what the OpenMANET mesh wizard produces (SAE with `ieee80211w=2`, default mesh
ID `openmanet`, `wlh0` a batman hardif of `bat0`; see *Encryption*):

```sh
uci set wireless.radio1.hwmode='11ah'
uci set wireless.default_radio1.mode='mesh'
uci set wireless.default_radio1.mesh_id='halowmesh'
uci set wireless.default_radio1.encryption='none'
uci commit wireless && wifi reload
```

Then three steps that are easy to miss and each produce a total, silent failure.

### 1. Take the mesh interface out of the bridge

An interface converted by hand keeps the stock `network='lan'`, so `wlh0` sits
in `br-lan`. A bridged mesh interface cannot hold
its own address and traffic entering the mesh from a bridge is *proxied* traffic,
which in 802.11s needs address extension and a mesh gate. Symptom: the peer
answers nothing, `iw dev wlh0 mpath dump` stays empty, and an address configured
on `wlh0` is simply ignored.

```sh
ip link set wlh0 nomaster
ip addr add 10.77.191.116/16 dev wlh0
ip link set wlh0 up
```

This is the measured procedure for a bare node. Keeping `wlh0` in `br-lan`
(Warthog takes a lease from the bridge and exchanges Address Extension frames
with it) is implemented and not measured on air.

Run `ip addr show bat0` on the peer first. If `bat0` exists, the OpenMANET
wizard has made `wlh0` a batman hardif of `bat0`: `ip link set wlh0 nomaster`
takes that node off its own batman fabric, and a Warthog gets no IP path into
that fabric either way. Not measured; see `docs/mesh-attachment-model.md`.

### 2. Put the interface back in a firewall zone

Removing `wlh0` from `br-lan` also removes it from the `lan` firewall zone, so
OpenWrt's default policy rejects inbound traffic. Symptom: ARP resolves, your
ping leaves, and the peer answers `ICMP protocol 1 ... unreachable` — which looks
like a mesh failure and is not one.

```sh
nft insert rule inet fw4 input iifname "wlh0" accept
```

Make it permanent by assigning the interface to a zone in
`/etc/config/firewall` rather than relying on the runtime rule above.

### 3. Nothing else

In particular you do **not** need `mesh_nolearn=1`. It bypasses path discovery
for established peers and looks like a fix, but mesh11sd re-applies
`mesh11sd.mesh_params.mesh_nolearn` (stock `'0'`) every 10 s, so a value set
with `iw` works in bursts and fails in between. Warthog answers path discovery
properly — see below. A wizard node runs `mesh_nolearn` 0 on 1.8.0 (the wizard
writes a key named `nolearn`, which nothing reads) and 1 on 1.8.1-dev.

## How paths are established

This is the part that is unlike a normal Wi-Fi link, and the part worth
understanding before debugging one.

A `mac80211` mesh will not send a **unicast** data frame to a neighbour it has no
*path* to, and a peer link reaching ESTAB does not create one: `MESH_PATH_ACTIVE`
is set only by HWMP path discovery. Group-addressed frames skip path resolution
entirely.

The consequence, if a peer answers no path requests: broadcast works, unicast
does not. ARP arrives, pings do not, the peer's per-station `tx packets` counter
freezes at exactly 5 (its Open and Confirm), and every layer reports healthy.

Warthog participates in HWMP in both directions:

- it emits a PREQ to each established peer every 2 s, which is what makes it
  *routable* — a peer installs a path to the originator of any PREQ it accepts;
- it answers a PREQ that targets it with a PREP.

A healthy peer shows resolved paths at hop count 1:

```
$ iw dev wlh0 mpath dump
DEST ADDR          NEXT HOP           IFACE  SN   METRIC  ...  FLAGS  HOP_COUNT
3c:1a:cc:4c:83:a5  3c:1a:cc:4c:83:a5  wlh0   224  1261    ...  0x15   1
```

`FLAGS 0x15` is ACTIVE | RESOLVED | SN_VALID. An entry with next hop
`00:00:00:00:00:00` and a `DRET` count climbing is discovery in progress that
nobody is answering.

## Encryption

> **Real 802.11s security is the `warthog-mesh-sae` build**: SAE (Dragonfly,
> hunt-and-peck; Warthog commits in group 19 and accepts 19, 20 or 21 from a
> peer) authentication and AMPE per-link key exchange, hardware-validated
> warthog↔warthog (peering + keying in one exchange, 0% loss over the CCMP
> link). Every node needs the same passphrase (`AT+MESHPASS=`, build default
> `-DWARTHOG_MESH_PASSPHRASE`, `warthog-mesh`). OpenMANET's mesh wizard
> (`encryption='sae'`) already produces a compatible SAE setup: match its mesh
> ID and passphrase; no `sae_pwe` or group setting is needed. If you set
> `sae_group` on the node, keep 19 in it (MODP 15 and 16 are not supported),
> and do not use `ieee80211w=1` or `encryption='sae-mixed'`.
>
> The legacy `AT+MESHSEC=1` keyed mode on the non-SAE build encrypts data
> frames with a 16-byte constant compiled into every image
> (`00 11 22 ... ee ff`). Anyone holding the firmware holds the key — it
> exists to exercise the CCMP data path, not to protect traffic, and it
> interoperates with other warthogs and nothing else. Against a secured
> OpenMANET mesh use SAE on both sides; against an open one run
> `AT+MESHSEC=0`.

Warthog's mesh data plane can run keyed or open:

```
AT+MESHSEC?      → +MESHSEC: 1 (keyed)
AT+MESHSEC=0     → open, re-peers within ~2 s
```

A fresh OpenMANET image has no mesh (its HaLow radio is an SAE access point),
and the LuCI mesh wizard always writes `encryption='sae'`, which netifd-morse
runs with `ieee80211w=2`, so **a wizard node needs the `warthog-mesh-sae`
build**; an SAE node ignores an open Warthog's beacons. `AT+MESHSEC=0` is for a peer an
operator set to `encryption='none'` (by hand, as above, or in openmanetd's
setup, which offers None). Keyed mode uses a fixed shared key that a Linux
peer does not have; the two cannot carry data to each other.

The setting persists in NVS, so a node that loses power comes back able to
talk to the same peer. Before it did not, and a rebooted node would peer
perfectly and carry no data with nothing in any log to explain it.

## What it looks like when it works

From the OpenMANET node — two warthogs established, both paths resolved:

![OpenMANET view](img/openmanet-pi.svg)

From a warthog — the peering, the path-selection counters, and a ping back:

![warthog view](img/openmanet-warthog.svg)

## Verifying a link

Work outward from peering. Each step has a distinct failure signature.

**1. Peers found and established.**

```
AT+MPMPEERS?
+MPMPEERS: self=4c83a5 4dc7f8 llid=44921 plid=26523 estab=1 opens=0;
                    28bf74 llid=34244 plid=50177 estab=1 opens=0; ...
```

`estab=1` with a non-zero `plid` on both sides is a complete handshake. A peer
stuck at `plid=0` with `opens` climbing is sending Opens nobody answers; after 8
Warthog sends a Close and restarts the handshake, which recovers the common case
of one node rebooting while its neighbour did not.

From the Linux side:

```sh
iw dev wlh0 station dump | grep -E 'Station|plink'
```

**2. Path selection working.**

```
AT+HWMPSTAT?
+HWMPSTAT: rx=234 preq_rx=75 preq_tx=142 prep_rx=159 prep_tx=75 parse_fail=0 not_ours=0 rann_rx=0 perr_rx=0
```

`preq_tx` climbing means the node is advertising itself. `preq_rx` matching
`prep_tx` means it answers every request aimed at it. `parse_fail` should be
0 — anything else means frames are arriving in a shape the parser does not handle, and
`AT+HWMPDUMP?` will show the bytes.

**3. Data.**

```
AT+MPING=10.77.191.116,8
+MPING: reply from 10.77.191.116 seq=1 time=11ms
```

## Troubleshooting

| Symptom | Look at | Usual cause |
|---|---|---|
| No peers at all | `AT+MPMPEERS?` shows `(none)`, `s1g_bcn=0` | Channel, mesh ID or bandwidth mismatch. All must match exactly. |
| Beacons heard, no peers | the diagnosis line of `AT+MESHCFG?`, `AT+MESHRSSI?` | Signal at or below -80 dBm: Warthog's floor (`AT+MESHRSSI`) or the peer's `mesh_rssi_threshold` (-85 on OpenMANET 1.6.5–1.7.x and nodes upgraded from them). Or an open Warthog against an SAE peer. |
| Peer seen, never establishes | `estab=0`, `opens` climbing, `close_tx` rising | Peer holds a stale link from before your reboot. Warthog recovers after 8 Opens; if it does not, restart the peer's mesh. |
| Established, broadcast only | `iw ... mpath dump` empty or next hop all zeros | Path discovery unanswered. Check `AT+HWMPSTAT?` `preq_tx` is climbing. |
| ARP resolves, ping rejected | peer answers `ICMP ... unreachable` | Peer firewall. The mesh interface is not in a zone — see step 2 above. |
| Nothing routes, mpath empty, peer `tx packets` stuck at 5 | peer `ip -s link` vs per-station counters | Mesh interface still enslaved to a bridge — see step 1 above. |
| Peering fine, zero data both ways | `AT+MESHSEC?` | Keyed Warthog against an open peer. `AT+MESHSEC=0`. |
| Frames arrive, nothing delivered | `AT+FILTSTAT?` | Names which of the RX filter's eight drop paths is firing. |

A counter that is *not* evidence of a fault: `rx_data` counts frames reaching
the datapath, not frames delivered to the IP stack; it moving slowly while pings
succeed is normal. `delivered=` in `AT+DATASTAT?` counts frames handed to the IP
stack.

## Known gaps

- By default Warthog does not forward. It answers path requests that target it
  and ignores the rest, so a three-node mesh where two nodes cannot hear each
  other will not relay through a Warthog in the middle. `AT+MESHFWD=1` adds
  forwarding and `AT+MESHBRIDGE=1` bridge mode; both are host-tested only, and
  whether the chip hands a relay frame to the host is unmeasured
  (`docs/mesh-attachment-model.md`).
- SAE/AMPE (`warthog-mesh-sae`) peers cross-vendor, but its data plane has not
  yet carried traffic with an OpenMANET peer. Warthog's chip firmware
  (`mm6108.mbin` 1.17.6, as morselib drives it) was measured keeping one
  pairwise key, the last install winning, and failing group RX from more than
  one peer (`docs/mesh-attachment-model.md`); Warthog puts only its own group
  key in the chip. More than one keyed peer, and any peer's group frames,
  therefore need host CCMP (`warthog-mesh-sae-swccmp`), which has not run on
  air.
  OpenMANET's Linux driver runs different chip firmware (mm6108-2.0.1),
  installs each peer's keys at that peer's AID, and falls back to software
  crypto per key.
- A beaconing SAE node (the wizard default) starts SAE with Warthog only after
  a beacon or probe response from it that carries an RSN element, which
  Warthog's SAE build sends. That is from source, not measured.
- Path selection to a peer running MFP (the OpenMANET wizard's `ieee80211w=2`)
  is CCMP-protected, and group path selection from it must carry a BIP MMIE
  that verifies; a relay's own broadcast PREQs and PERRs, and a bridge's
  PREQs, carry one only with `AT+MESHPMF=1`. Not measured on air, including
  whether the chip encrypts or decrypts management frames on the mesh interface.
- `AT+MESHSEC=1` on the non-SAE build uses one published key on every node.
- batman-adv is not implemented.
