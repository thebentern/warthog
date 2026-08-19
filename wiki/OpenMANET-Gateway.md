# Hanging phones and EUDs off an OpenMANET mesh

You have an OpenMANET HaLow mesh. You want phones, tablets and end-user devices
on it — ATAK, a browser, whatever — without a Raspberry Pi strapped to each one.

That is what warthog is for here. A warthog node joins your mesh as an ordinary
802.11s peer, and presents two client surfaces on the other side: a **2.4 GHz
Wi-Fi access point** and a **USB Ethernet adapter**. A phone joins the AP or
plugs into the cable, and it is on your mesh.

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

Nothing changes on the OpenMANET side except the two interface defaults below.
The mesh does not know or care that a peer has clients behind it.

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

**1. Build and flash the mesh image.** Mesh is a build-time mode:

```bash
pio run -e warthog-mesh-smoke -t upload
```

Hold **BOOT**, tap **RESET**, release BOOT before uploading; tap RESET after.

**2. Match the peer's encryption.** Stock OpenMANET runs `encryption='none'`.
Over the console:

```
AT+MESHSEC=0
```

This persists. Skipping it produces perfect peering and zero data — the single
most common way to end up confused.

**3. Confirm it joined.**

```
AT+MPMPEERS?
+MPMPEERS: self=4c83a5 28bf74 llid=8903 plid=63421 estab=1 opens=0; ...
```

`estab=1` with a non-zero `plid` is a complete handshake with the Pi.

That is the whole warthog side. Channel, bandwidth and mesh ID are already set
to OpenMANET's defaults (S1G ch 42, 2 MHz, `halowmesh`) in the build.

## Setup — OpenMANET

Two OpenWrt defaults will stop it dead, and neither produces an error message.
This is the part that costs people hours.

**Take the mesh interface out of the bridge.** OpenWrt puts `wlh0` in
`br-lan`. A bridged mesh interface cannot hold its own address, and traffic
entering the mesh from a bridge is *proxied* — 802.11s handles that through a
different mechanism than locally-originated frames.

```sh
ip link set wlh0 nomaster
ip addr add 10.77.191.116/16 dev wlh0    # 10.77.<mac[4]>.<mac[5]>, see below
ip link set wlh0 up
```

Symptom if you skip it: an address on `wlh0` is ignored, `iw dev wlh0 mpath
dump` stays empty, and the peer's per-station `tx packets` freezes at exactly 5.

**Put it back in a firewall zone.** Unbridging removed `wlh0` from the `lan`
zone, so the default policy now rejects inbound.

```sh
nft insert rule inet fw4 input iifname "wlh0" accept
```

Symptom if you skip it: ARP resolves, pings leave, and the peer answers
`ICMP protocol 1 ... unreachable`. Looks like a mesh fault. Isn't.

Make both permanent in `/etc/config/network` and `/etc/config/firewall`.

**Do not set `mesh_nolearn=1`.** It looks like a fix and OpenWrt resets it
every ~10 s. warthog answers path discovery properly; it is not needed.

## Addressing

There is no DHCP on the mesh. Every node — warthog and Pi alike — derives a
static address from its own MAC:

```
10.77.<mac[4]>.<mac[5]> / 255.255.0.0
```

warthog does this automatically. Give the Pi the matching address by hand (the
`ip addr add` above). `AT+STATUS?` reports what a warthog picked.

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
the mesh sees only the warthog's `10.77.x.y` address.

## Verifying end to end

From a phone on the AP or a laptop on USB, ping a mesh node directly:

```
ping 10.77.191.116        # the Pi
```

If that works, the phone is on your OpenMANET mesh. If it does not, work
outward: [Troubleshooting](Troubleshooting) has the symptom → cause table.

## What warthog does not do

- **Forward.** A warthog answers path requests aimed at itself and relays
  nothing. It is a leaf with clients behind it, not a repeater. Two mesh nodes
  that cannot hear each other will not be relayed through a warthog between
  them.
- **Encrypt meaningfully.** The keyed mode is one hardcoded key on every node.
  Run open to match OpenMANET, and treat the mesh as an untrusted transport —
  which for ATAK-style traffic you should be doing anyway.
- **Bridge at L2.** Clients are NAT'd, so a mesh node cannot initiate a
  connection *to* a phone behind a warthog. Phone-initiated flows are fine.
