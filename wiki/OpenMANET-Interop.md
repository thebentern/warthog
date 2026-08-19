# Interoperating with OpenMANET and OpenWrt

Warthog meshes with Linux `mac80211` 802.11s peers. Verified against OpenMANET
1.8.0 on a Raspberry Pi 4 with a Seeed HaLow HAT, meshing with two Warthog nodes
at once.

Measured with the peer in its stock configuration:

| Direction | Result |
|---|---|
| OpenMANET → Warthog A | 29/30, 3% loss, 8.9 / 19.3 ms |
| OpenMANET → Warthog B | 30/30, 0% loss, 8.6 / 15.2 ms |
| Warthog → OpenMANET | 8/8, 0% loss, 8 / 19 ms |

## Pick the security mode first

Warthog and OpenMANET must agree on mesh security. Two working combinations:

| Mesh | Warthog build | OpenMANET config |
|---|---|---|
| **Encrypted (SAE/AMPE)** | `warthog-mesh-sae` | `wpa_supplicant` mesh SAE (below) |
| Open | `warthog-mesh-smoke` + `AT+MESHSEC=0` | stock `encryption='none'` |

Mismatched modes fail cleanly rather than half-working: a SAE Warthog does not
offer peering to an open node at all (the Mesh Configuration's Authentication
Protocol Identifier must match), and an open Warthog peers with a SAE node but
no keys exist so no data crosses.

## Warthog side — open

Build for mesh and set the data plane to match the peer. Stock OpenMANET runs
`encryption='none'`, so:

```bash
pio run -e warthog-mesh-smoke -t upload
```

```
AT+MESHSEC=0
```

Keyed Warthog against an open peer produces perfect peering and zero data, in
both directions — it is the first thing to check when links establish but
nothing routes.

## Encrypted meshing — SAE/AMPE on both sides

Warthog side:

```bash
pio run -e warthog-mesh-sae -t upload     # passphrase: -DWARTHOG_MESH_PASSPHRASE='"..."', default warthog-mesh
```

Nothing to configure at runtime — the node authenticates (SAE, group 19),
exchanges per-link keys (AMPE) and peers on its own. Verify with `AT+SAERX?`
(`ESTAB=1`) and `AT+MPMPEERS?` (`ampe_mtk=1 ampe_mgtk=1`).

OpenMANET side — mesh SAE is standard `wpa_supplicant`, not `iw`:

```
# /etc/wpa_supplicant-mesh.conf
network={
    ssid="halowmesh"          # the Mesh ID — must match Warthog's
    mode=5
    frequency=5180            # your S1G channel mapping
    key_mgmt=SAE
    sae_password="warthog-mesh"
    ieee80211w=2
}
```

```sh
wpa_supplicant -i wlh0 -c /etc/wpa_supplicant-mesh.conf -B
```

On OpenWrt, the equivalent uci is `encryption='sae'` +
`key='warthog-mesh'` on the mesh interface section (hostapd/wpa_supplicant
must be the `-mesh`/full variants, which OpenMANET ships).

Warthog↔Warthog SAE is hardware-validated (single-exchange peering, AMPE keys
in the chip, 0% loss over the CCMP link). Warthog↔OpenMANET SAE runs the same
hostap code on both ends but has not yet completed on hardware. What bench
testing established so far:

- **OpenMANET's kernel-MPM mesh advertises Authentication Protocol 0 even
  when running SAE** (`wpa_supplicant_s1g` with `key_mgmt=SAE`; observed in
  its probe responses). Warthog's candidate gate therefore refuses to
  initiate toward it. `AT+SAEBRIDGE=2` overrides the gate for exactly this
  case.
- `sae_pwe=1` (H2E-only) is OpenMANET's shipped default; Warthog sends
  hunt-and-peck Commits, so set `sae_pwe=0` or `2` on the Linux side.
- The Morse supplicant rejects `MESH_PEER_ADD` even with `user_mpm=1` +
  `no_auto_peer=1`, so the Linux side cannot be told to initiate; and Warthog
  does not beacon in mesh mode, so kernel-MPM candidate discovery never sees
  it. The Warthog must initiate — hence the `AT+SAEBRIDGE=2` override.

`AT+SAERX?` on the Warthog shows the SAE conversation state and which peer it
is talking to.

## OpenWrt side

```sh
uci set wireless.radio1.hwmode='11ah'
uci set wireless.default_radio1.mode='mesh'
uci set wireless.default_radio1.mesh_id='halowmesh'
uci set wireless.default_radio1.encryption='none'
uci commit wireless && wifi reload
```

Then give it an address on the mesh subnet. Use the same derivation Warthog
uses — `10.77.<mac[4]>.<mac[5]>/16` — so the whole mesh stays consistent. For a
card with MAC `e4:5f:01:28:bf:74`:

```sh
ip addr add 10.77.191.116/16 dev wlh0
```

## Two OpenWrt defaults that break this

Each produces a total failure with no error message.

### The mesh interface must not be bridged

OpenWrt puts `wlh0` in `br-lan`. A bridged mesh interface cannot hold its own
address, and traffic entering the mesh from a bridge is *proxied* traffic, which
802.11s handles through a different mechanism than locally-originated frames.

Symptom: an address configured on `wlh0` is ignored, `iw dev wlh0 mpath dump`
stays empty, and the peer's per-station `tx packets` counter sits at exactly 5 —
its Open and Confirm — no matter how much traffic you offer.

```sh
ip link set wlh0 nomaster
ip addr add 10.77.191.116/16 dev wlh0
ip link set wlh0 up
```

### Unbridging drops it out of the firewall zone

`wlh0` was in the `lan` zone by virtue of being in `br-lan`. Once removed it is
in no zone, and OpenWrt's default policy rejects inbound traffic.

Symptom: ARP resolves fine, your ping leaves, and the peer answers
`ICMP protocol 1 ... unreachable`. It reads like a mesh fault and is not one.

```sh
nft insert rule inet fw4 input iifname "wlh0" accept
```

For anything permanent, assign the interface to a zone in
`/etc/config/firewall` instead of relying on that runtime rule.

## Do not use mesh_nolearn

`mesh_nolearn=1` bypasses path discovery for established peers. It makes a
broken link start passing traffic, which makes it look like the fix. It is not:
OpenWrt resets it to `0` every ~10 seconds, so the link works in bursts and
fails in between. Warthog answers path discovery properly and does not need it.

## Verifying

On the peer, paths should resolve at hop count 1:

```sh
$ iw dev wlh0 station dump | grep -E 'Station|plink'
Station 3c:1a:cc:4c:83:a5 (on wlh0)
	mesh plink:	ESTAB

$ iw dev wlh0 mpath dump
DEST ADDR          NEXT HOP           IFACE  SN   METRIC  ...  FLAGS  HOP_COUNT
3c:1a:cc:4c:83:a5  3c:1a:cc:4c:83:a5  wlh0   224  1261    ...  0x15   1
```

`FLAGS 0x15` is ACTIVE | RESOLVED | SN_VALID. A next hop of `00:00:00:00:00:00`
with a climbing `DRET` is discovery in progress that nobody is answering.

On Warthog:

```
AT+MPMPEERS?     peers and handshake state
AT+HWMPSTAT?     path requests sent, answered, and parse failures
AT+MPING=10.77.191.116,8
```

## Vanilla OpenWrt

The same procedure applies to stock OpenWrt with a Morse Micro driver — nothing
above is OpenMANET-specific. What matters is that `mesh_id`, channel, bandwidth
and operating class match the Warthog build, and that the two interface defaults
above are dealt with.
