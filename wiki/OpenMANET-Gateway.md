# Hanging phones and EUDs off an OpenMANET mesh

You have an OpenMANET HaLow mesh. You want phones, tablets and end-user devices
on it — ATAK, a browser, whatever — without a Raspberry Pi strapped to each one.

That is what warthog is for here. A warthog node joins your mesh as an ordinary
802.11s peer, and presents two client surfaces on the other side: a **2.4 GHz
Wi-Fi access point** and a **USB Ethernet adapter**. A phone joins the AP or
plugs into the cable, and it can reach the nodes on your mesh. On a
mesh built by OpenMANET's mesh wizard (mesh interface in `bat0`) a warthog in
plain mesh mode peers but its clients reach nothing; there it has to run as a
batman member ([Batman Mode](Batman-Mode)). See
[Setup — OpenMANET](#setup--openmanet).

```
                 ┌──── HaLow 802.11s mesh (OpenMANET) ────┐
   Pi + HaLow ───┤                                          ├─── warthog
                 └──────────────────────────────────────────┘      │
                                                       ┌───────────┴───────────┐
                                                       │                       │
                                                  2.4 GHz AP              USB (CDC-NCM)
                                                  ┌────┴────┐              ┌───┴────┐
                                                phone   tablet           laptop  iPad
```

Nothing changes on the OpenMANET side except its mesh security (step 2) and
the interface settings below. The mesh does not know or care that a peer
has clients behind it.

Verified against OpenMANET 1.8.0 on a Raspberry Pi 4 with a Seeed HaLow HAT.

## What the operator sees

Once a warthog has joined, it looks like any other station from the Pi:
established peer link, a resolved path at hop count 1, and it answers pings.

![OpenMANET view of the mesh](https://raw.githubusercontent.com/thebentern/warthog/main/docs/img/openmanet-pi.svg)

And from the warthog end, the same mesh — the peering, the path-selection
traffic that made the Pi's paths resolve, and a ping back to the Pi:

![warthog view of the same mesh](https://raw.githubusercontent.com/thebentern/warthog/main/docs/img/openmanet-warthog.svg)

`preq_rx` matching `prep_tx` (80 → 80) is warthog answering every path request
the Pi sent it. `parse_fail=0` means every frame was understood.

## Setup — warthog

**1. Build and flash the mesh image.** It always boots into mesh (any other
image: `AT+MESHEN=1`, then `AT+RESET`):

```bash
pio run -e warthog-mesh-smoke -t upload
```

Hold **BOOT**, tap **RESET**, release BOOT before uploading; tap RESET after.

**2. Match the peer's encryption.** OpenMANET's LuCI mesh wizard configures SAE
(mesh ID `openmanet`, passphrase `changeme123` unless changed); a node runs an
open mesh only if an operator set `encryption='none'`
([OpenWrt side](OpenMANET-Interop#openwrt-side)). The first table on
[OpenMANET Interop](OpenMANET-Interop) was measured against such an open node,
and the SAE results there against Pis at `encryption='sae'` and `ieee80211w=2`.
Against an open node, over the console:

```
AT+MESHSEC=0
```

This persists. Skipping it produces perfect peering and zero data — the single
most common way to end up confused.

Against an SAE node (every wizard node), flash
`warthog-mesh-sae-swccmp-meshvif` instead of step 1, set `AT+MESHID=` and
`AT+MESHPASS=` to the node's values, then `AT+RESET`. Its host CCMP must be on:
`AT+SWCCMP=1` after each boot, or batman mode (below), which arms it.
`warthog-mesh-sae-swccmp` and `warthog-mesh-sae-swccmp-on` (host CCMP on at
boot) run the mesh on the chip's STA interface, where a node's unicast over its
RTS threshold (1000 on the 1.8.0 bench Pis) arrives only from the peer the chip
registered last
([OpenMANET Interop](OpenMANET-Interop#frames-over-about-1000-bytes-from-a-linux-node)).
In plain mesh mode `warthog-mesh-sae-meshvif` (keys in the chip, no
`AT+SWCCMP`) also works; batman mode refuses it. `warthog-mesh-sae` peers with
the node but cannot open its group frames, so a 1.8.0 node gets no path to the
Warthog, and batman mode refuses that build; keep it for meshes of Warthogs
only. Encrypted data between a Warthog and OpenMANET 1.8.0 was measured on
2026-09-29/30 in batman mode against Pis whose `bat0` was set up by hand, on
`warthog-mesh-sae-swccmp` and `-swccmp-meshvif`
([Batman Mode](Batman-Mode#measured-on-air)). Plain mesh mode against SAE nodes
without `bat0` was measured from 2026-09-30 to 2026-10-03 on
`warthog-mesh-sae-swccmp-meshvif` and `warthog-mesh-sae-meshvif`
([OpenMANET Interop](OpenMANET-Interop#frames-over-about-1000-bytes-from-a-linux-node)).

**3. Confirm it joined.**

```
AT+MPMPEERS?
+MPMPEERS: self=4c83a5 28bf74 llid=8903 plid=63421 estab=1 opens=0; ...
```

`estab=1` with a non-zero `plid` is a complete handshake with the Pi.

That is the whole warthog side. Channel, bandwidth and mesh ID default to S1G
ch 42, 2 MHz, mesh ID `halowmesh`, and are settable at runtime with
`AT+MESHCHAN=` / `AT+MESHID=`.

Measured against OpenMANET 24.10 (`r28739-d9340319c6`) on a Pi 4 with a Morse
MM6108, those defaults matched that peer. Channel 42 and country US are
OpenMANET's radio defaults; its mesh ID `halowmesh` was set by hand, where the
mesh wizard's default is `openmanet`:

```
wireless.radio1.channel='42'          wireless.radio1.country='US'
wireless.default_radio1.mesh_id='halowmesh'
wireless.default_radio1.encryption='sae'
```

Do not take that as a guarantee across releases or profiles — read the peer's
actual configuration and match it. `AT+MESHCFG?` prints Warthog's side of the
same comparison.

## Setup — OpenMANET

Switching the stock HaLow interface to mesh by hand leaves `wlh0` in `br-lan`.
Two mistakes then stop traffic with no error message: an address put on `wlh0`
while it is bridged, and an unbridged `wlh0` left out of a firewall zone.

**First, run `ip addr show bat0`.** If `bat0` exists, the node was configured by
OpenMANET's mesh wizard: `wlh0` is a batman-adv (BATMAN_V) hard interface of
`bat0`, not a `br-lan` port. Against such a node a warthog in plain mesh mode
peers but gets no DHCP lease and no IP path, and the unbridging below would
remove `wlh0` from `bat0`: leave the node as it is and use
[Batman Mode](Batman-Mode) (`AT+MESHBATMAN=1`) instead, measured on air against
nodes whose `bat0` was set up by hand, not yet against a wizard node. The
steps below are for a node without `bat0`.

**Give the node a mesh-subnet address.** The interface keeps `network='lan'`,
so `wlh0` is in `br-lan`, and an address on `wlh0` is ignored. Put it on
`br-lan` (`ip addr add 10.77.191.116/16 dev br-lan`): with both 1.8.0 Pis'
`wlh0` in `br-lan`, Pi to Warthog pings of 1000 and 1472 bytes passed 476/480
in a 1-hour soak under SAE on 2026-10-03. Two Pis bridged this way share one
LAN segment, so their `br-lan` addresses must differ. Or take `wlh0` out of the
bridge, give it the address and put it back in a firewall zone, as below.
Traffic from hosts behind a bridge is *proxied*, which 802.11s handles through
a different mechanism than locally-originated frames.

```sh
ip link set wlh0 nomaster
ip addr add 10.77.191.116/16 dev wlh0    # 10.77.<mac[4]>.<mac[5]>, see below
ip link set wlh0 up
```

Symptom of an address on `wlh0` while it is still in `br-lan`: `iw dev wlh0
mpath dump` stays empty, and the peer's per-station `tx packets` freezes at
exactly 5.

**Put it back in a firewall zone.** Unbridging removed `wlh0` from the `lan`
zone, so the default policy now rejects inbound.

```sh
nft insert rule inet fw4 input iifname "wlh0" accept
```

Symptom if you skip it: ARP resolves, pings leave, and the peer answers
`ICMP protocol 1 ... unreachable`. Looks like a mesh fault. Isn't.

Make both permanent in `/etc/config/network` and `/etc/config/firewall`.

**Do not set `mesh_nolearn=1`.** It looks like a fix, and mesh11sd re-applies
the value in `/etc/config/mesh11sd` (`0` as shipped) every 10 s. warthog
answers path discovery properly; it is not needed.

## Addressing

A warthog first asks for a DHCP lease over the mesh and waits up to 6 s. A Pi
taken out of its bridge as above serves no lease on `wlh0`, so the warthog
falls back to a static address derived from its own MAC — the same form the Pi
is given by hand with the `ip addr add` above:

```
10.77.<mac[4]>.<mac[5]> / 255.255.0.0
```

A Pi that keeps `wlh0` in `br-lan` runs a DHCP server on that bridge. Both
bench Pis did (dnsmasq serving `lan`), and on 2026-10-03 both warthogs, with
`AT+MESHDHCP=1`, were still on the static address; why no lease bound is not
investigated ([Mesh Mode](Mesh-Mode#addressing)).

`AT+MESHDHCP=0` skips the lease attempt. `AT+STATUS?` reports what a warthog
picked.

## Now the clients

With the warthog on the mesh, its two client surfaces are live and share the
uplink.

**Phones and tablets — join the Wi-Fi AP.**

| | |
|---|---|
| SSID | `warthog` |
| Passphrase | `warthog-default` — **change it** |
| Client subnet | `192.168.5.0/24`, DHCP |

```
AT+WIFIAP=mymesh,a-real-passphrase,11
```

**Laptops and iPads — plug in the cable.** The USB surface is CDC-NCM, which
macOS, Linux, Windows 10+ and iOS/iPadOS all bind with an in-box driver. The
host gets `192.168.4.x` by DHCP.

Both surfaces are NAT'd onto the mesh, so clients need no route configured and
the mesh sees only the warthog's own mesh address.

## Verifying end to end

From a phone on the AP or a laptop on USB, ping a mesh node directly:

```
ping 10.77.191.116        # the Pi
```

If that works, the phone can reach that Pi's mesh-subnet address. It receives
none of OpenMANET's multicast (CoT, mDNS, voice): warthog repeats only
Meshtastic's `239.0.0.69:4403` across its NAT. If it does not work, work
outward: [Troubleshooting](Troubleshooting) has the symptom → cause table.

## What warthog does not do

- **Forward, by default.** A warthog answers path requests aimed at itself and
  relays nothing. It is a leaf with clients behind it, not a repeater. Two mesh
  nodes that cannot hear each other will not be relayed through a warthog
  between them. `AT+MESHFWD=1` makes it a relay. On 2026-10-02, on
  `warthog-mesh-sae-meshvif` under SAE, it relayed 300-, 1000- and 1400-byte
  pings between two OpenMANET Pis, 10/10 each, with both Pis' paths to each
  other set through it by hand (`iw dev wlh0 mpath new`). Path selection
  through it is not measured on air.
- **Encrypt meaningfully without SAE.** The keyed mode of the builds without
  SAE (`warthog-mesh-smoke`, step 1) is one hardcoded key on every node. SAE
  carried data between warthog and OpenMANET 1.8.0 on air from 2026-09-29 to
  2026-10-03 (step 2), but every peer of a node holds that node's group key and
  can forge its group frames. Treat the mesh as an untrusted transport, which
  for ATAK-style traffic you should do anyway.
- **Bridge at L2, by default.** Clients are NAT'd, so a mesh node cannot
  initiate a connection *to* a phone behind a warthog. Phone-initiated flows
  are fine. `AT+MESHBRIDGE=1` puts the clients on the mesh at layer 2 instead;
  that mode builds but has not carried a packet on a board.
