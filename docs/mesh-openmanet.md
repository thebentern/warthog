# 802.11s mesh, and interoperating with OpenMANET / OpenWrt

Warthog can join an 802.11s mesh over HaLow instead of associating to an AP.
Every node is a peer: there is no gateway to elect, no association, and a node
that powers off takes only its own links with it.

This document is the setup procedure, the settings that must match, and the
failure modes — all of it measured against OpenMANET 1.8.0 on a Raspberry Pi 4
with a Seeed HaLow HAT, and two Warthog nodes.

Verified result on that bench, stock peer configuration, unencrypted mesh:

| Direction | Result |
|---|---|
| OpenMANET → Warthog A | 29/30, 3% loss, 8.9 / 19.3 ms |
| OpenMANET → Warthog B | 30/30, 0% loss, 8.6 / 15.2 ms |
| Warthog → OpenMANET | 8/8, 0% loss, 8 / 19 ms |
| Warthog ↔ Warthog | 6/6, 0% loss, ~15 ms |

## Build and flash

Mesh mode is a build-time configuration, not a runtime toggle:

```bash
pio run -e warthog-mesh-smoke
```

That env pins the radio to a single S1G channel and sets the mesh identity, so
every node agrees without any runtime configuration. The values that must match
across the whole mesh are compile-time flags in `platformio.ini`:

| Flag | Default | Must match peers |
|---|---|---|
| `WARTHOG_MESH_ID` | `halowmesh` | yes |
| `WARTHOG_PIN_S1G_CHAN` | `42` | yes |
| `WARTHOG_PIN_S1G_FREQ_HZ` | `923000000` | yes |
| `WARTHOG_PIN_S1G_BW_MHZ` | `2` | yes |
| `WARTHOG_PIN_S1G_GLOBAL_OP_CLASS` | `69` | yes |
| `WARTHOG_MESH_BEACON_TU` | `1000` | no |

S1G channel 42 at 2 MHz is OpenMANET's default, which is why it is Warthog's.
Change any of the first five and you must change them on every node, Warthog and
Linux alike — a mismatch produces a silent non-event, not an error.

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

Mesh nodes are statically addressed. There is no DHCP on the mesh; a node
derives its own address from its MAC:

```
10.77.<mac[4]>.<mac[5]> / 255.255.0.0
```

So `3c:1a:cc:4c:83:a5` is `10.77.131.165`. The whole mesh is one flat
`10.77.0.0/16`, which is why nodes whose third octet differs are still on-link.

**A Linux peer must use the same derivation**, or nothing will route. For a card
with MAC `e4:5f:01:28:bf:74`:

```sh
ip addr add 10.77.191.116/16 dev wlh0
```

`AT+STATUS?` reports the address the node picked.

Two behaviours worth knowing. The gateway is set to the node's **own** address,
so a mesh node has no upstream route — mesh mode is not a path to the internet.
And the address is applied lazily, on the first peer establishment rather than
at boot, so a node with no peers yet has no mesh address to report.

## Setting up an OpenMANET / OpenWrt peer

Tested on OpenMANET 1.8.0, Raspberry Pi 4, Seeed HaLow HAT (MM6108).

Configure the mesh interface to match the Warthog build:

```sh
uci set wireless.radio1.hwmode='11ah'
uci set wireless.default_radio1.mode='mesh'
uci set wireless.default_radio1.mesh_id='halowmesh'
uci set wireless.default_radio1.encryption='none'
uci commit wireless && wifi reload
```

Then three steps that are easy to miss and each produce a total, silent failure.

### 1. Take the mesh interface out of the bridge

OpenWrt puts `wlh0` in `br-lan` by default. A bridged mesh interface cannot hold
its own address and traffic entering the mesh from a bridge is *proxied* traffic,
which in 802.11s needs address extension and a mesh gate. Symptom: the peer
answers nothing, `iw dev wlh0 mpath dump` stays empty, and an address configured
on `wlh0` is simply ignored.

```sh
ip link set wlh0 nomaster
ip addr add 10.77.191.116/16 dev wlh0
ip link set wlh0 up
```

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

In particular you do **not** need `mesh_nolearn=1`, and you should not use it.
It bypasses path discovery for established peers and looks like a fix, but
OpenWrt resets it to `0` every ~10 seconds, so a link built on it works in bursts
and fails in between. Warthog answers path discovery properly — see below.

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

> **The keyed mode is not security.** Every node ships the same hardcoded
> 128-bit key, used as both pairwise and group key. It keeps traffic off a
> casual listener and nothing more — anyone with the firmware has the key.
> Per-link derivation (SAE/AMPE) is not implemented. Do not describe a Warthog
> mesh as encrypted in any sense that matters.

Warthog's mesh data plane can run keyed or open:

```
AT+MESHSEC?      → +MESHSEC: 1 (keyed)
AT+MESHSEC=0     → open, re-peers within ~2 s
```

Stock OpenMANET ships `encryption='none'`, so **interoperating with it today
requires `AT+MESHSEC=0`** on every Warthog node. Keyed mode uses a fixed shared
key that a Linux peer does not have; the two cannot carry data to each other.

The setting persists in NVS, so a node that loses power comes back able to
talk to the same peer. Before it did not, and a rebooted node would peer
perfectly and carry no data with nothing in any log to explain it.

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
+HWMPSTAT: rx=234 preq_rx=75 preq_tx=142 prep_rx=159 prep_tx=75 parse_fail=0 not_ours=0
```

`preq_tx` climbing means we are advertising ourselves. `preq_rx` matching
`prep_tx` means we are answering everything asked of us. `parse_fail` should be
0 — anything else means frames are arriving in a shape we do not understand, and
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
| Peer seen, never establishes | `estab=0`, `opens` climbing, `close_tx` rising | Peer holds a stale link from before your reboot. Warthog recovers after 8 Opens; if it does not, restart the peer's mesh. |
| Established, broadcast only | `iw ... mpath dump` empty or next hop all zeros | Path discovery unanswered. Check `AT+HWMPSTAT?` `preq_tx` is climbing. |
| ARP resolves, ping rejected | peer answers `ICMP ... unreachable` | Peer firewall. The mesh interface is not in a zone — see step 2 above. |
| Nothing routes, mpath empty, peer `tx packets` stuck at 5 | peer `ip -s link` vs per-station counters | Mesh interface still enslaved to a bridge — see step 1 above. |
| Peering fine, zero data both ways | `AT+MESHSEC?` | Keyed Warthog against an open peer. `AT+MESHSEC=0`. |
| Frames arrive, nothing delivered | `AT+FILTSTAT?` | Names which of the RX filter's eight drop paths is firing. |

Two counters that are *not* evidence of a fault:

- `delivered=` in `AT+DATASTAT?` reads 0 on a healthy link — it is only
  incremented on a receive path this build does not take.
- `rx_data` counts frames reaching the datapath, not frames delivered to the IP
  stack; it moving slowly while pings succeed is normal.

## Known gaps

- Warthog does not forward. It answers path requests that target it and ignores
  the rest, so a three-node mesh where two nodes cannot hear each other will not
  relay through a Warthog in the middle.
- Keyed mesh uses a single fixed key on every node. SAE/AMPE key derivation is
  not implemented.
