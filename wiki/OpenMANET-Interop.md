# Interoperating with OpenMANET and OpenWrt

Warthog meshes with Linux `mac80211` 802.11s peers. Verified against OpenMANET
1.8.0 on a Raspberry Pi 4 with a Seeed HaLow HAT, meshing with two Warthog nodes
at once.

Measured with the peer hand-configured for an open mesh (`encryption='none'`);
a stock image runs no mesh (see below):

| Direction | Result |
|---|---|
| OpenMANET → Warthog A | 29/30, 3% loss, 8.9 / 19.3 ms |
| OpenMANET → Warthog B | 30/30, 0% loss, 8.6 / 15.2 ms |
| Warthog → OpenMANET | 8/8, 0% loss, 8 / 19 ms |

Encrypted (SAE, `ieee80211w=2`), `warthog-mesh-sae-swccmp` in batman mode
against OpenMANET 1.8.0 on 2026-09-30: pings from a host on a Pi's LAN,
`batctl ping` from a Pi and a DHCP lease from a Pi, with small frames ([Batman Mode](Batman-Mode#measured-on-air)).
`warthog-mesh-sae-swccmp-meshvif` also took the Pis' 1000- and 1400-byte frames
with their RTS threshold at 1000, the same day
([below](#frames-over-about-1000-bytes-from-a-linux-node)).

## Pick the security mode first

Warthog and OpenMANET must agree on mesh security. Two working combinations:

| Mesh | Warthog build | OpenMANET config |
|---|---|---|
| **Encrypted (SAE/AMPE)** | `warthog-mesh-sae-swccmp-meshvif` (host CCMP, batman mode too) or `warthog-mesh-sae-meshvif` (keys in the chip, each node's MGTK at its AID, [below](#group-frames-in-the-chip-warthog-mesh-sae-meshvif); batman mode refused); both take a node's frames above its RTS threshold from every node ([below](#frames-over-about-1000-bytes-from-a-linux-node)), `warthog-mesh-sae-swccmp` only from the peer the chip registered last; `warthog-mesh-sae` peers but gets no unicast from a 1.8.0 node | mesh wizard default, or `uci` (below) |
| Open | `warthog-mesh-smoke` + `AT+MESHSEC=0` | `encryption='none'`, set by the operator |

A fresh OpenMANET image runs no mesh: its HaLow radio is an SAE access point in
`br-lan`. The LuCI mesh wizard writes an SAE mesh (mesh ID `openmanet`,
passphrase `changeme123` unless changed); openmanetd's setup wizard defaults to
SAE and also offers none. An open mesh is always an operator's choice.

Mismatched modes fail cleanly rather than half-working: a SAE Warthog does not
offer peering to an open node at all (the Mesh Configuration's Authentication
Protocol Identifier must match), and an SAE node ignores an open Warthog (per
source, OpenMANET drops its beacons for lacking an RSN element, and hostap
drops an Open from a peer that has not completed SAE).

## Warthog side — open

Only for a peer an operator has set to `encryption='none'` (wizard nodes run
SAE by default). Build for mesh and set the data plane to match the peer:

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
pio run -e warthog-mesh-sae-swccmp-meshvif -t upload   # passphrase: -DWARTHOG_MESH_PASSPHRASE='"..."', default warthog-mesh
```

`warthog-mesh-sae` peers the same way, but its chip crypto cannot open the
node's group frames, so it gets no path and no unicast from a 1.8.0 node
([below](#management-frame-protection-peering-does-not-need-it-path-selection-does)).
`warthog-mesh-sae-meshvif` puts the node's MGTK into the chip at its AID, as
Linux does, so its chip can
([below](#group-frames-in-the-chip-warthog-mesh-sae-meshvif)).
On `warthog-mesh-sae-swccmp` and `warthog-mesh-sae-swccmp-meshvif` turn host
CCMP on after each boot (`AT+SWCCMP=1`); batman mode and the
`warthog-mesh-sae-swccmp-on` build arm it at boot.

The build flag only sets the default: `AT+MESHPASS=<pass>` and `AT+MESHID=<id>`
set the passphrase and mesh ID at runtime (persisted, applied on the next
boot). The node then authenticates (SAE, group 19), exchanges per-link keys
(AMPE) and peers on its own. Verify with `AT+SAERX?` (`ESTAB=1`) and
`AT+MPMPEERS?` (`ampe_mtk=1 ampe_mgtk=2` with one peer).

OpenMANET side — against a node set up by the mesh wizard, set Warthog's
`AT+MESHID`/`AT+MESHPASS` to the node's values. To set a node up by hand, use
`uci`. This is the idiomatic path and the one verified on hardware; a hand-run
`wpa_supplicant` fights netifd and loses its config on the next `wifi` event.

```sh
uci set wireless.radio1.disabled='0'
uci set wireless.default_radio1.mode='mesh'
uci set wireless.default_radio1.mesh_id='halowmesh'      # must match Warthog
uci set wireless.default_radio1.encryption='sae'
uci set wireless.default_radio1.key='warthog-mesh'       # must match Warthog
uci commit wireless

uci set mesh11sd.mesh_beaconless.mesh_beacon_less_mode='1'   # stock 0; optional, see trap 2
uci commit mesh11sd

wifi down radio1 && wifi up radio1                       # NOT `wifi reload`
```

Three traps, all of which cost real bench time:

1. **`wifi reload` silently ignores `mesh11sd` changes.** netifd only
   regenerates `/var/run/wpa_supplicant-wlh0.conf` when the *wireless* config
   changes. Check the file's mtime — a stale one means your change never
   applied. Cycle the radio instead.
2. **Beaconless mode is not needed for discovery** (per source; not measured
   on air). A secured mac80211 mesh drops a beacon or probe response without
   an RSN element, and answers only a probe request that carries a Mesh ID
   element. Warthog's SAE beacons and probe responses carry the RSN element its
   peering frames carry, and its probe requests carry the Mesh ID element, so a
   beaconing OpenMANET node (the wizard default) can take Warthog as a
   candidate. One bench run with beaconing on saw the MM6108 chip firmware
   fault (two `HW has stopped` events, `wlh0` down) where beaconless ran
   6 minutes with 0 crashes; the chip firmware version was not recorded and
   source cannot explain it, so if you see that fault, keep beaconless on.
   Note the uci name (`mesh_beacon_less_mode`) differs from the supplicant
   name it becomes (`mesh_beaconless_mode`).
3. **After a chip fault, only a reboot recovers it.** The driver leaks its
   `vif0` sysfs node, so every later `add_interface` returns `EEXIST`. A
   `morse_cli reset` of the chip does *not* clear it — the stale state is
   host-side. (`morse_cli reset` also needs `MM_RESET_PIN`, which
   `/etc/profile.d/morse_cli.sh` exports; a non-interactive SSH never sources
   it.)

Verify the peer side with:

```sh
iw dev wlh0 station dump | grep -E '^Station|plink:'   # expect ESTAB
morse_cli -i wlh0 channel                              # expect 923000 kHz
logread | grep MESH-PEER-CONNECTED
```

**Status (2026-09-20): Warthog↔OpenMANET SAE peering is verified on hardware.**
A three-node mesh — two OpenMANET Pis and one Warthog — reached `ESTAB` on
every link, with the Warthog installing two distinct AMPE pairwise keys
(`AT+KEYINST?` showing `aid=1 pw=1` and `aid=2 pw=1`). The peer's own log shows
`mesh plink with <warthog> established` / `MESH-PEER-CONNECTED`, at −2 dBm and
135–150 Mbit/s VHT-MCS6/7.

**The encrypted data plane passes traffic cross-vendor with host CCMP, and with
chip keys on `warthog-mesh-sae-meshvif`.**
On 2026-09-29 and 2026-09-30, `warthog-mesh-sae-swccmp` in batman mode against
two OpenMANET 1.8.0 Pis (`ieee80211w=2`): on 09-29 host CCMP opened the Pis'
group frames (`AT+SWCCMP?` `tried=69 ok=69` on one board; no Pi sent unicast
that day), and on 09-30, with the group-path-selection fix below, their unicast
too: pings, `batctl ping` and DHCP ran from the Pis to the Warthogs
([Batman Mode](Batman-Mode#measured-on-air)). Plain mesh mode
against SAE nodes was measured on both `-meshvif` builds from 2026-09-30 to
2026-10-03 ([below](#frames-over-about-1000-bytes-from-a-linux-node)).
On the chip-crypto build (`warthog-mesh-sae`), in the 2026-09-20 run, ICMP was
0/30 and every undecryptable frame was group-addressed
(`AT+RXCHAN?` showed `nodec grp` climbing 1:1 with pings while `uni` stayed
0). Measured on Warthog's chip firmware (`mm6108.mbin` 1.17.6) on the mesh
interface (the STA chip interface every build but the `-meshvif` ones runs
the mesh on): one pairwise key, where the last install wins, and a second
group-key install breaks group decryption, while every 802.11s peer generates
its own MGTK. (A Linux node installs each peer's keys at that peer's AID;
`warthog-mesh-sae-meshvif` now does the same on a MESH chip interface,
[below](#group-frames-in-the-chip-warthog-mesh-sae-meshvif).)
On every other build the only group key in the chip is
Warthog's own TX MGTK, installed with the first peer, so standard group frames
(`AT+MESHGRP=1`) go out under Warthog's own key; each peer's MGTK stays in the
host keychain. Under SAE, receiving any peer's group frames,
and any unicast with more than one peer, therefore needs host CCMP
(`warthog-mesh-sae-swccmp` or `-swccmp-meshvif`, armed with `AT+SWCCMP=1` or by
batman mode). **For encrypted cross-vendor data use
`warthog-mesh-sae-swccmp-meshvif`, or `warthog-mesh-sae-meshvif` outside batman
mode**; a 1.8.0 node gets no path to `warthog-mesh-sae` at all (below). A Linux
node's unicast above about 1000 bytes arrives from every node only on the
`-meshvif` builds (`warthog-mesh-sae-swccmp-meshvif`, `warthog-mesh-sae-meshvif`)
or with a setting on the node; on other builds only from the peer the chip
registered last ([below](#frames-over-about-1000-bytes-from-a-linux-node)).

Earlier findings, still relevant:

- **OpenMANET probe responses were observed advertising Authentication
  Protocol 0 while running SAE.** Source predicts 1: OpenMANET peers in
  userspace (`wpa_supplicant_s1g`, not kernel MPM), which sets the SAE
  protocol for the beacons and probe responses its kernel sends; the reading
  is unexplained. Warthog's candidate gate refuses a peer whose protocol
  differs from its own, and `AT+SAEBRIDGE=2` overrides the gate. The verified
  three-node run (2026-09-20) peered without it, so try without it first.
  `AT+SAEBRIDGE` is RAM-only — it resets to 1 on every boot, so anything
  depending on it is unusable on an unattended node.
- OpenMANET writes `sae_pwe=1` (H2E-only) into its generated supplicant
  config, but mesh SAE never reads it: both sides use hunt-and-peck on a mesh.
  No `sae_pwe` setting is needed on either side.
- The Morse supplicant rejects `MESH_PEER_ADD` even with `user_mpm=1` +
  `no_auto_peer=1`, so the Linux side cannot be told to initiate toward a peer
  it has not discovered.
- OpenMANET's supplicant only starts SAE with a peer its kernel raised as a
  candidate from a beacon, or from a probe response addressed to it; with
  beaconing on, a Commit from an unknown peer is dropped (a beaconless node's
  driver turns it into a candidate instead). The frame must match the node's
  Mesh ID and Mesh Configuration, carry an RSN element on an SAE mesh, and
  arrive above `mesh_rssi_threshold` (-80 dBm; -85 on 1.6.5–1.7.x or nodes
  upgraded from them). Warthog now beacons (see below), and under SAE its
  beacons and probe responses carry RSN, so OpenMANET can discover it (per
  source; not measured). Warthog discovers OpenMANET from its beacons and
  probe responses.

**Warthog mesh beaconing.** The MM6108 firmware fires its beacon TBTT once and
never re-arms it, so early builds did not beacon and were invisible to a
beacon-driven peer. A host beacon timer now re-drives the beacon at the
interval; `AT+BCNSTAT?` shows `served`/`txcomp` climbing together (~1.15/s),
i.e. the chip transmits every beacon. Warthog↔Warthog SAE is fully verified;
the Warthog↔OpenMANET SAE handshake has since completed on hardware (see the
status note above), presumably against a peer set up with the beaconless recipe
above, whose driver takes Warthog's Commit as first contact (the peer's setting
was not recorded). Discovery of Warthog from its beacons by a beaconing SAE peer
is not measured.

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

Each stops traffic to the address set above, with no error message.

### An address on a bridged mesh interface is ignored

A stock HaLow interface switched to `mode='mesh'` by hand keeps `network='lan'`,
so `wlh0` is in `br-lan`, and an address on `wlh0` is ignored. Put the address on
`br-lan`, or take `wlh0` out of the bridge (below) and give it the address. With
both 1.8.0 Pis' `wlh0` in `br-lan` and a 10.77/16 address on `br-lan`, 1000- and
1472-byte Pi to Warthog pings passed 476/480 in a 1-hour soak under SAE on
2026-10-03. Two nodes bridged this way merge their LANs into one segment. Traffic
from hosts behind the bridge is *proxied* traffic, which 802.11s handles through
a different mechanism than locally-originated frames.

The proxying half is confirmed on air. Capturing on `wlh0` of one OpenMANET
24.10 node while a laptop behind the other node's bridge sent mDNS shows the
frame crossing the mesh with the **laptop's** source MAC, not the Pi's — an
endpoint behind the bridge, carried over 802.11s, which is what Mesh Address
Extension exists to express. Warthog reads AE on receive as of
`3868453`; that it does so correctly against this peer is not yet measured.

Note for anyone trying to capture this themselves: **monitor mode is not
available on the HaLow radio while the mesh is up.** Adding a monitor VIF
succeeds but it never gets a channel, and tuning it returns `command failed:
Resource busy (-16)`. Capture on `wlh0` itself, which gives decrypted 802.3
frames — enough to see proxied source MACs, not enough to read the Mesh
Control field.

Symptom: an address configured on `wlh0` is ignored, `iw dev wlh0 mpath dump`
stays empty, and the peer's per-station `tx packets` counter sits at exactly 5 —
its Open and Confirm — no matter how much traffic you offer.

This applies to a node whose `wlh0` is a `br-lan` port. Check first with
`ip addr show bat0` (`tools/bench/openmanet_interop.py` prints the same). On a
node configured by OpenMANET's mesh wizard, `bat0` exists, `wlh0` is a
batman-adv hard interface of it (BATMAN_V), and mesh11sd runs with
`mesh_fwding='0'` (the 1.8.0 LuCI wizard also writes `nolearn='1'`, a key
nothing reads, so the effective `mesh_nolearn` is 0; 1.8.1-dev writes
`mesh_nolearn='1'`). A batman hard interface hands only batman's own ethertype
(0x4305) to `bat0`, so a Warthog in plain mesh mode peers with such a node at
802.11s but gets no DHCP lease and no IP path to it. Leave `wlh0` in `bat0` and
run the Warthog as a BATMAN_V member instead (`AT+MESHBATMAN=1`,
[Batman Mode](Batman-Mode)). `nomaster` on a wizard node removes `wlh0` from
`bat0` and takes that node off its own batman fabric. Batman mode is measured on
air (2026-09-29/30) against Pis whose `bat0` was set up by hand at runtime, not
against a wizard node ([Batman Mode](Batman-Mode#measured-on-air)).

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

`mesh_nolearn=1` bypasses path discovery for established peers. Set with `iw`,
it makes a broken link start passing traffic, which makes it look like the fix.
It is not: mesh11sd re-applies `/etc/config/mesh11sd` every 10 s (`0` as
shipped and on 1.8.0 wizard nodes), so the link works in bursts and fails in
between. 1.8.1-dev wizard nodes hold it at `1`. Warthog answers path discovery
properly and does not need it.

## Frames over about 1000 bytes from a Linux node

Both OpenMANET 1.8.0 bench Pis run an RTS threshold of 1000 (netifd-morse sets
it: its radio setup applies `set_default rts 1000` when the wifi-device has no
`rts` option; read from OpenMANET 1.8.0 source), so each unicast frame longer
than that goes out behind an RTS/CTS exchange (full-size TCP segments and large
UDP too; derived).
The Warthog's chip answers the RTS with a CTS, but on the STA chip interface
every build except the `-meshvif` ones runs the mesh on, it
addresses that CTS from the peer it registered last. Only that neighbour takes
it; every other node times out and never sends the frame. Measured on
2026-09-30 with the chips' MAC counters on both ends: after another Pi had
re-peered with the Warthog, a Pi's 1000-byte pings went 0/8, with its RTS +64,
CTS timeouts +64 and `RX CTS for RTS` +0, while the Warthog counted 63 RTS
received and 63 CTS sent; the Pi that peered last got 8/8. Frames from the
Warthog are unaffected: a node's RTS threshold governs only what that node
sends.

**Fix: a `-meshvif` build** (`warthog-mesh-sae-swccmp-meshvif` or
`warthog-mesh-sae-meshvif`). Each runs the mesh on a MESH chip
interface, as Linux does, which addresses its CTS to each RTS's sender (per the
chip firmware's disassembly); both Pis took it. Measured on air on
2026-09-30 on `-swccmp-meshvif` against the two Pis at threshold 1000: in the
same case the Pi's 1000-byte pings went 8/8, `RX CTS for RTS` +8, CTS
timeouts +0; the other Pi 8/8; a Pi to a second `-swccmp-meshvif` Warthog 8/8.
In batman mode, 500-, 1000- and 1400-byte pings over batman passed 10/10 each
([Batman Mode](Batman-Mode)).
`AT+MESHCFG?` must read `chip_vif=mesh(5)`: a fallback to `sta` behaves as the
other builds. A 54-minute soak the same day (both Warthogs on `-swccmp-meshvif`,
both Pis at threshold 1000, a round every 5 minutes): 237/240 1000-byte pings
from the Pis to the Warthogs, 60/60 Warthog to Warthog, `chip_vif=mesh(5)` with
3 peers and no fallback in all 24 readings.

A 4-hour soak of a `-swccmp-meshvif` Warthog (2026-09-30 to 10-01, a round every
9 minutes) passed 401/480: from 2 h in, every 1000-byte ping from one Pi failed
while its 300-byte pings passed. That was fragmentation, not RTS
([below](#fragmented-frames-on-low-rate-links)). On `warthog-mesh-sae-meshvif`
(AMPE keys in the chip), a 1-hour soak on 2026-10-01 passed 239/240 300- and
1000-byte pings from both Pis. After `AT+RESET` of that Warthog, all three peers
were back within 51 s.

Not measured on a MESH chip interface: an open mesh, three or more Warthogs (two
were). Recovery after a chip restart (`AT+CHIPRESTART`) was measured on
2026-10-03 on both `-meshvif` builds against these Pis
([Mesh Mode](Mesh-Mode#chip-restarts)).

**Other builds: set each OpenMANET node** to use CTS-to-self in place of
RTS/CTS, or turn RTS off. Either works alone:

```sh
echo Y > /sys/module/mm6108_sdio/parameters/enable_cts_to_self
# or
iw dev wlh0 info | grep wiphy      # wiphy N: the phy is phyN
iw phy phyN set rts off
```

`mm6108_sdio` is the bench Pis' MM6108 SDIO driver; the module name follows the
chip and bus (`ls /sys/module/*/parameters/enable_cts_to_self`). Both were
measured set at runtime on 2026-09-30: with CTS-to-self, 900-, 1000- and
1400-byte pings passed; with RTS off, 3/4, 4/4 and 3/4. Neither survives a
reboot, and the next radio setup puts the RTS threshold back to 1000. The
persistent RTS setting is `uci set wireless.<radio>.rts='-1'` and `wifi reload`;
uci drops `'off'`, and `'0'` means RTS on every frame. The persistent
CTS-to-self setting is `uci set wireless.<radio>.enable_cts_to_self=1` and a
driver reload. Both are read from OpenMANET's `netifd-morse` source, not
measured.

## Fragmented frames on low-rate links

The MM6108 firmware fragments a frame too long for one transmission at a low
rate, with no fragmentation threshold set (Morse's `mmwlan_set_fragment_threshold()`
documents this; Morse rate control names 1 MHz MCS0-2 as such rates, from its own
rough bits-per-symbol table; by the standard's, MCS2 carries a full-size frame,
[below](#host-fragmentation)). Measured on
2026-10-01: an OpenMANET 1.8.0 node (chip firmware 2.0.1) whose rate to a Warthog
had fallen to MCS0 sent each 1000-byte ping as two fragments (`morse_cli -i wlh0
stats` `TX fragment` +10 for 5 pings); 300-byte pings, and 1000-byte pings relayed
by a node at MCS7, were not fragmented and passed 8/8.

Fragments are reassembled after decryption and before Mesh Control is read
(only the first fragment carries one), with mac80211's checks (one key,
consecutive packet numbers, matching headers, 1 s limit); `AT+DEFRAG?` counts
them. At most 4 frames are reassembled at once, 2 per
peer. Measured on 2026-10-01 with a Pi forced to fragment (`iw phy phy0 set frag
512`, path pinned to the Warthog): 1000- and 1400-byte pings passed 8/8 each on
`-swccmp-meshvif` and on `warthog-mesh-sae-meshvif`, `AT+DEFRAG?` `in=56 ok=16`
with every drop counter at 0 on both.

`AT+DATASTAT?` counts `rx_data` per fragment and `delivered` per reassembled
frame, so `rx_data` minus `delivered` is the fragment count minus 1 for each
fragmented frame, intact or not; `AT+DEFRAG?` shows whether reassembly worked.

To reproduce, pin a Linux node's path through the Warthog and ping at 1000 bytes
while its rate is low:

```sh
iw dev wlh0 mpath new <warthog-mac> next_hop <warthog-mac>
ping -s 1000 <warthog-ip>
morse_cli -i wlh0 stats | grep -i 'tx fragment'
```

Pass: every ping is answered, `TX fragment` rises, and on the Warthog
`AT+DEFRAG?` `ok` rises by 1 for each fragmented ping while `nofirst`, `order`,
`pn`, `key`, `prot`, `hdr` and `oversize` stay at 0.

What the Warthog sends. When its own chip fragments (forced with `AT+FRAG=512`, or
by itself at a rate too low for the frame), a Linux node loses the frame with host
CCMP. With chip keys it takes frames in 2 fragments unless the Warthog holds a
Block Ack session with it on their TID, and loses frames in 3 (2026-10-02 and
2026-10-03, [below](#what-chip-firmware-1176-does-with-fragments)). Measured on
2026-10-01, 1000-byte replies to a pinned Pi:

| Build | Result | On the Pi |
|---|---|---|
| `-swccmp-meshvif` (host CCMP) | 0/8 | `RX MPDUs with MIC fail` +24: the chip split frames the host had already encrypted |
| `warthog-mesh-sae-meshvif` (chip crypto) | 0/8 | only the first fragment of each frame was decrypted; the rest never reached mac80211 |
| either, `AT+FRAG=0` | 8/8 | |

Warthog to Warthog, chip-crypto fragments reassemble (`AT+DEFRAG?` `in=9 ok=3`
for 3 datagrams), and Pi to Pi fragments pass. Later runs place each loss. With
host CCMP it is the sending chip, which splits frames the host has sealed and,
on 2026-10-03, re-encapsulated host-cut fragments after the first, so a Warthog
receiver lost them too. With chip keys, a Linux recipient drops fragments under
the Warthog's Block Ack session on their TID, and the 3-fragment loss is the
sending chip's.

### Host fragmentation

`AT+HOSTFRAG` has the Warthog cut such a frame itself, as Linux does when
mac80211 fragments. Each fragment is its own frame: one sequence number,
fragment numbers in order, More Fragments on all but the last, Mesh Control and
Address Extension in the first only, each encrypted on its own (the chip numbers
each as it encrypts it), each short enough that every rate in its retry chain
sends it whole.

#### What chip firmware 1.17.6 does with fragments

Measured on air on 2026-10-03 with `AT+TXCAP` on the sending Warthog and
`AT+RXCAP` on the receiving one (the two captures agree byte for byte except
Duration), chip firmware 1.17.6 on both, 1 MHz MCS0 (`AT+TXRATE=0,1`),
`AT+HOSTFRAG=auto` with no fragment cap (host CCMP cutting too):

| Sender | Fragments | What arrives |
|---|---|---|
| host CCMP (`-swccmp-meshvif`) | 2 or more | fragment 0 as sent; every fragment numbered 1 or more, the last included, re-encapsulated by the sending chip: an outer header (a copy of the Warthog's, the chip's Duration, QoS `0x0120`) and the Warthog's 32-octet header and QoS again inside the body, sometimes followed by an extra piece with the next fragment number. A Warthog finds no CCMP header (`AT+SWCCMP?` `badhdr`), a Linux node a MIC failure. Handing the chip one fragment at a time changed nothing. |
| chip keys (`-meshvif`) | 2 | intact and delivered: Warthog to Warthog UDP 2/2; Warthog to a Pi, ping 8/8 (2026-10-02; host-cut after the session ended, or chip-cut with the Warthog's Block Ack session state not recorded; a Pi drops fragments under its session, **Block Ack** below) |
| chip keys | 3 | fragment 1 with More Fragments cleared and 32 octets more inside its encrypted body, fragment 2 with 32 more: the frame is lost (the 3-fragment loss below). `AT+TIDPARAMS=0` or `1` changed nothing. |

A host-CCMP frame the chip cuts itself fails too (2026-10-01 and 02, the table
above). So, whatever `AT+HOSTFRAG` says:

- host-CCMP builds (`-swccmp`, `-swccmp-meshvif`, batman mode on them) cut
  nothing (`AT+HOSTFRAG?` `rule=off`) and send a sealed frame only at rates that
  carry it whole (`AT+SEALFIT`, [below](#sealed-and-group-frames-at-rates-the-chip-delivers));
- chip-key builds (`warthog-mesh-sae`, `-meshvif`) cut in at most 2 (`rule=max2`),
  and send a frame the chip cuts only at rates where it needs at most 2, or
  whole under the Warthog's Block Ack session on its TID (`AT+SEALFIT`).

| Setting (chip-key builds) | Cut |
|---|---|
| `AT+HOSTFRAG=0` | nothing; the chip cuts a sealed frame in at most 2 at the rates `AT+SEALFIT` leaves; under the Warthog's Block Ack session on the frame's TID not at all while `AT+FRAG` is 0 or the frame fits under it, else still in at most 2 (`seal_nofit`), which a Linux recipient drops |
| `AT+HOSTFRAG=auto` (default on `warthog-mesh-sae` and `-meshvif`) | a unicast data frame longer than `AT+FRAG` allows (CCMP and FCS counted), or than the slower of the first two rates of its retry chain carries in one transmission; in at most 2 |
| `AT+HOSTFRAG=<n>` (256 to 2346) | as `auto`, and a unicast data frame longer than *n*, counted as `iw phy set frag <n>` counts it; an *n* that would make more than 2 fragments is raised to the least that makes 2 (`clamp`; any *n* below about 800 for a full-size frame) |

Group and management frames are never cut (as mac80211). A frame is sent whole
when `AT+FRAG` alone would cut it in more than 2 (`many`; leave `AT+FRAG` at 0,
or at 810 or more, 816 for frames carrying Address Extension) or no TX buffer is
free for its fragments (`pool`). The chip
may cut those itself, so the Warthog counts every packet number it could use for
them (`chippn`); a key re-install never starts below them. This count applies
with `AT+HOSTFRAG=0` too. The setting persists and applies to the next frame. A
node that never stored one runs `auto` on the chip-key builds (`AT+HOSTFRAG?`
`stored=auto`) and off on every other build: host CCMP, `-nochipkey`, and the
builds without SAE (`warthog-us`, the region builds, `warthog-mesh-smoke`). A
stored value keeps applying.

**Block Ack.** mac80211 never fragments under A-MPDU; it sends the frame whole
(`tx.c`: `IEEE80211_TX_CTL_AMPDU` sets `DONTFRAG`). A Linux recipient ends its
Block Ack session, and drops the fragment, when a fragment after the first
arrives on a TID with a session (`rx.c` `ieee80211_rx_reorder_ampdu`). The
Warthog's chip would fragment such a frame, so when a frame over its limit is on
a TID where the Warthog holds an originator session (agreed or requested), the
Warthog ends that session with a DELBA as mac80211 sends one (`agg-tx.c`:
originator, reason 37), then cuts the frame, never as A-MPDU. mac80211 acts on a
received DELBA in deferred interface work, and drops a fragment that reaches it
before that work has run. So the Warthog holds that frame, and the peer's later
frames, until the DELBA's TX status returns and 20 ms more; 500 ms at most. A
frame over its limit that goes whole (counted `many` or `pool`, the latter also
when a fragment buffer cannot be had once cutting starts) still ends the session
first, then goes to the chip at once, without that wait, at a rate that sends it
whole (`AT+SEALFIT`, `seal_ba`), as the recipient may still hold its session;
under an `AT+FRAG` below the frame no rate does, and the chip cuts it. The
peer's later frames wait.

It sends no ADDBA on that TID until 15 s pass with no frame there that needed
cutting (mac80211's spacing of ADDBA retries, `HT_AGG_RETRIES_PERIOD`), so a
session comes back once the link is fast again. The ADDBA response timeout (100
ms, doubled with each request up to 60 s) carries over a session the Warthog
ends; once it exceeds the peer's answer time, that is one ADDBA and one DELBA per
peer and TID per 15 s. Sessions a peer starts are not touched. `AT+AMPDU=0` stops
the Warthog starting any originator session and ends those it holds, for A/B
tests.

| Counter | Counts |
|---|---|
| `AT+HOSTFRAG?` `ba_end`, `nodelba` | sessions ended to cut a frame, the DELBA handed to the chip or not |
| `ba_wait`, `ba_late` | cut frames held for that DELBA; waits ended at 500 ms without its TX status |
| `delba_noack` | DELBAs a cut frame waited on that the chip gave up on unacked (the frame still goes 20 ms later) |
| `ba_rcpt` | cut frames whose peer's reorder size toward the Warthog was set: its session, or one it ended since |
| `hold`, `held` | ADDBAs not sent (one each time a cut frame re-arms the hold); peer TIDs held now |
| `agg` | fragments the chip reports sent in an A-MPDU (the host never asks for it; measured above 0 on 2026-10-03) |
| `AT+AMPDU?` `addba_tx`, `delba_to`, `delba_end`, `delba_other` | ADDBAs sent; DELBAs sent for an unanswered ADDBA (reason 4), a stopped session (37), other |
| `rx_delba`, `rx_reason` | DELBAs a peer sent for the Warthog's sessions, the last reason (38 from a Linux node: it ended its session at a fragment, or, once per TID until a new session, it received a frame under Block Ack policy or a BAR on a TID with no session, `rx.c` 1373-1377 and 3273-3277) |

The rate rule is derived, not measured. An S1G transmission carries at most 511
OFDM symbols of data (the SIG field's length is 9 bits): 764 octets at 1 MHz
MCS0. Morse's Linux driver refuses a 1 MHz beacon of 764 - 36 = 728 octets or
more (`beacon.c` `FRAGMENTATION_OVERHEAD`), because the chip may fragment it.
The Warthog counts a 16-bit SERVICE field and an A-MPDU delimiter, then takes
the same 36 octets off every rate. The last column is the receiver sensitivity
802.11-2020 sets for S1G against 1 MHz MCS0 (-95 dBm), about the link budget
each rate costs; derived, not measured:

| Rate | Longest frame sent whole | A 1500-byte IP packet at this rate | Link budget against 1 MHz MCS0 |
|---|---|---|---|
| 1 MHz MCS10 | 340 octets | 6 fragments | +3 dB |
| 1 MHz MCS0 | 720 | 3 | 0 |
| 1 MHz MCS1 | 1488 | 2 | -3 dB |
| 1 MHz MCS2, 2 MHz MCS0 and faster | 1616 or more | whole | -5 dB (1 MHz MCS2), -3 dB (2 MHz MCS0), less above |

The first two rates are rate control's best and the next lower one, or a probe
and the best. A frame they would cut in more than 2 goes only at the chain's
rates where it needs at most 2, their attempts moved to the slowest kept
(`cap_trim`). If no rate in the chain qualifies, the chain becomes the slowest
rate at the bandwidth of its last entry that does, with all the chain's attempts
(`cap_sub`); for a full-size frame at 1 MHz that is MCS1, also under
`AT+TXRATE=0,1`. MCS1 is the most robust rate at which such a frame can arrive:
whole at 1 MHz needs MCS2, and at every rate the chain had it is lost. A later
rate that cannot carry a fragment or a whole frame is then dropped from its
chain and its attempts go to the slowest rate that can (`trim`), so no attempt is
at a rate the chip would fragment at. Rate control ends every chain with MCS0, at
1 MHz on a 1 MHz channel and at 2 MHz MCS0 dropping to 1 MHz MCS0 on a 2 MHz one.
So a full-size frame goes in 2 at MCS1 when rate control's best rate is 1 MHz
MCS0 or MCS1, in 2 sized for MCS1 at 1 MHz MCS2, whole at 2 MHz MCS0 alone, and
whole at 1 MHz MCS3, 2 MHz MCS1 and faster.

#### Sealed and group frames at rates the chip delivers

`AT+SEALFIT=1`, the default, sends a frame the host does not cut only at rates
where chip firmware 1.17.6 delivers it:

- host CCMP: a sealed unicast frame whole;
- chip keys: a sealed unicast frame in at most 2 fragments under `AT+FRAG` too,
  with `AT+HOSTFRAG=0` or when `AT+HOSTFRAG` sends it whole; whole while the
  Warthog holds an originator Block Ack session, requested or agreed, with the
  frame's peer on its TID, or until the DELBA that ended one for a cut is
  through, since a Linux recipient drops fragments under its session (**Block
  Ack**, above). Under an `AT+FRAG` below the frame no rate sends it whole, so it
  goes in at most 2 as with no session (`seal_nofit`), which a Linux recipient
  drops: toward Linux nodes keep `AT+FRAG` at 0;
- every build: a mesh group frame (`AT+MESHGRP=1`) whole, as receivers drop group
  fragments; `AT+FRAG` does not apply to it.

A rate of the frame's retry chain that fails this is dropped and its attempts go
to the slowest rate kept (`seal_trim`, `grp_trim`); with none left, the chain
becomes the slowest rate at the bandwidth of its last entry that passes, with all
its attempts (`seal_sub`, `grp_sub`, also under `AT+TXRATE`); either change made
for a Block Ack session counts `seal_ba` instead; with no such rate the chain is
left (`seal_nofit`, `grp_nofit`; a frame over `AT+FRAG` with host CCMP, or one
needing 3 under it with chip keys), except that a frame under a Block Ack session
then gets the 2-fragment rule (`seal_nofit`, then `seal_trim` or `seal_sub`). A
first rate the rule puts in asks for RTS/CTS only if rate control's first rate
did (rate control asks for it on every later rate). `AT+SEALFIT=0` keeps rate
control's chain, for A/B tests. Unsealed unicast frames (an open mesh) are never
changed.

At 1 MHz, by IP packet size (a unicast frame is the IP packet plus 66 octets; 12
more with Address Extension, about 24 less IP room in batman mode):

| Frame | 1 MHz MCS0 | Needs MCS1 (-3 dB) | Needs MCS2 (-5 dB) |
|---|---|---|---|
| host CCMP, whole | up to 654 | 655 to 1422 | over 1422 |
| chip keys, at most 2 fragments | up to 1322 | over 1322 | never |
| chip keys under the Warthog's Block Ack session (`AT+HOSTFRAG=0`), whole | up to 654 | 655 to 1422 | over 1422 |
| group (`AT+MESHGRP=1`, SAE), whole | up to 660 | 661 to 1428 | over 1428 |

With Address Extension the unicast bands are 12 lower (642, 1410, 1310), in
batman mode about 24 lower (630, 1398, 1298). The dB figures are derived, not
measured. Unicast frames in those bands are lost at 1 MHz MCS0 when the chip cuts
them (measured), so the cost is against a rate that never delivers them; group
frames that long cannot go whole at MCS0, and receivers drop group fragments
(source; not measured on air). On a link that closes only at MCS0 they are still
lost. The lever is the sending host's MTU: 1422 keeps host-CCMP frames within
MCS1, 1322 keeps chip-key frames at MCS0 in 2 fragments. This covers what the
Warthog sends; a Linux node picks its own rates.

Measured on air 2026-10-03 ([below](#measured-on-air-and-tests)): with host CCMP
`AT+SEALFIT=1` delivered replies whole; with chip keys and `AT+HOSTFRAG=0`, replies
the chip cut in 2 under the Warthog's Block Ack session were all lost. Not
measured: `seal_ba` (such a frame sent whole, under a session or before its
DELBA is through), group frames, and how the chip
picks the rate of an A-MPDU whose frames' chains differ, where `AT+SEALFIT`
changed one chain and not the next.

#### While fragments are in the chip

On chip-key builds a frame the chip encrypts between two fragments under the
same counter breaks their packet-number run, and the receiver drops the frame.
While fragments are in the chip the Warthog therefore holds back, until their TX
statuses return:

- that peer's frames on another access category;
- frames the chip encrypts for that peer's management (Block Ack, path
  selection);
- on `warthog-mesh-sae` (a STA chip interface, one pairwise counter), frames to
  every other peer.

Our own group frames are not held; whether the chip numbers them from the
pairwise counter is not known.

A peer's next frame also waits while the TX pool (20 buffers) lacks the
buffers its last cut took. While `AT+HOSTFRAG` is in force (chip-key builds),
the pool pauses the network stack 5 buffers early: a cut's second fragment for
each of up to 4 peers, and the DELBA that ends a session first. A run whose
statuses never return is released after 16 s (`stale`).

#### Measured on air and tests

Measured on 2026-10-02 against OpenMANET 1.8.0 Pis on
`warthog-mesh-sae-swccmp-meshvif` and `warthog-mesh-sae-meshvif`, the Pi's path
pinned so it pings the Warthog directly, replies sent whole under the Warthog's
originator Block Ack session with the Pi on their TID (A-MPDU sessions form with
OpenMANET nodes):

| Warthog | Replies |
|---|---|
| `AT+FRAG=512` | 0/8 at 1000, 1400 and 1472 bytes: the chip fragmented each |
| `AT+FRAG=0` | 16/16: the chip did not fragment them |

`AT+MACSTATS?` tag 4152 (on the Warthog's chip, firmware 1.17.6, the fragments
the chip transmitted, host-made ones included; measured 2026-10-01 and 02) rose
with them. On the host-CCMP build the Pi's `RX MPDUs with MIC fail` rose by 3 a
reply; on the chip-key build the chip sent about one fragment of each frame it
cut (tag 4152 +10 for 8 frames).

Measured on 2026-10-03 against OpenMANET 1.8.0 Pis (chip firmware 2.0.1), the
Warthog's chip on 1.17.6, `AT+HOSTFRAG=auto` with no fragment cap (host CCMP
cutting too, up to 3 fragments a frame), host fragments cut after the session
ends as above:

| Warthog | To | Result |
|---|---|---|
| `warthog-mesh-sae-swccmp-meshvif` (host CCMP) | a Pi | the first fragment of each frame opens; every later one counts `RX MPDUs with MIC fail` on the Pi |
| `warthog-mesh-sae-swccmp-meshvif` | the other Warthog, `AT+SWCCMP=1` | 3 frames of 3 fragments: host CCMP `ok` 4 (the 3 first fragments and another frame), `badhdr` 6 (no Ext IV bit where the CCMP header should be), `micfail` 0; `AT+DEFRAG?` `in=3 ok=0 restart=2 expired=1` |
| `warthog-mesh-sae-meshvif` (chip keys) | a Pi | frames in 2 fragments 8/8 when the host cut them (`auto` at `AT+TXRATE=0,1`, the session ended first), and 8/8 when the chip cut them, the Warthog's Block Ack session state then not recorded; in 3 fragments the Pi loses 1 or 2 of each frame (`rx drop misc`), no MIC failure, its RX PN up by 3 a frame |

On `-swccmp-meshvif` `agg` reached 4 with `ba_end` 0 while `AT+AMPDU?` showed
`orig=2`; on `-meshvif` `ba_end` was 1, then after a new session (`addba_tx` 7 to
8) frames cut later ended nothing and `agg` rose.

Measured on 2026-10-03 at `AT+TXRATE=0,1` (1 MHz MCS0), an OpenMANET 1.8.0 Pi
pinging the Warthog with 1000- and 1472-byte ICMP, every other setting at its
default:

| Warthog | Setting | 1000 bytes | 1472 bytes | Seen |
|---|---|---|---|---|
| `warthog-mesh-sae-swccmp-meshvif` (host CCMP) | `AT+SEALFIT=0` | 0/8 | 0/8 | the chip cut the sealed replies |
| `warthog-mesh-sae-swccmp-meshvif` | `AT+SEALFIT=1` | 8/8 | 8/8 | `seal_sub`; nothing cut by the chip |
| `warthog-mesh-sae-meshvif` (chip keys) | `AT+HOSTFRAG=0` | 0/8 | 0/8 | the chip cut each reply in 2 while the Warthog held an originator Block Ack session with the Pi, and the Pi drops fragments under its session (`rx.c` `ieee80211_rx_reorder_ampdu`); `AT+SEALFIT=1` too |
| `warthog-mesh-sae-meshvif` | `AT+HOSTFRAG=auto` | 8/8 | 8/8 | the session ended by a DELBA, each reply in 2 host fragments |

Warthog to Warthog with chip keys, UDP at 1 MHz MCS0, 1000 and 1400 bytes:
`AT+HOSTFRAG=auto` 5/5 and 5/5, `AT+HOSTFRAG=0` 5/5 and 5/5 (a Warthog recipient
reassembles the chip's fragments under a session). `seal_ba`, which keeps such a
frame whole with `AT+HOSTFRAG=0`, is not measured on air.

What the source shows:

- The transmit code sets fragment number and More Fragments before host CCMP
  seals each fragment, with the CCMP header between QoS Control and the body;
  fragments after the first are built in their own buffers. `AT+TXCAP` shows
  them so as handed to the chip; what the chip sends is
  [above](#what-chip-firmware-1176-does-with-fragments).
- Morselib puts the reorder size of the peer's session to the Warthog into
  every unicast frame's TX descriptor (`tid_params`), fragments included, and
  keeps it after the peer ends that session, until its next ADDBA. So a
  fragment to a Pi that holds, or held, a session to the Warthog carries a
  Block Ack field with no session of the Warthog's to end (`ba_rcpt` counts
  such cuts). morse_driver sets it only under its own agreed session.
  `AT+TIDPARAMS=1`, the default, does the same while `AT+HOSTFRAG` is in force or
  `AT+AMPDU=0`, so by default on the chip-key builds (`auto`): a fragment
  carries none. It changes whole frames when only one side holds a session:
  under the Warthog's own session a whole frame carries that session's size
  (`tp=2f` in place of `20`), to a peer holding a session only toward the
  Warthog none (`00` in place of `0f`); with sessions both ways nothing changes.
  Whether the chip aggregated on that field is not known; with 3 fragments `=0`
  and `=1` lost the frame alike (2026-10-03).
- Each fragment carries the connection's TX flags (traveling pilots, 1 MHz
  control responses), as a whole frame does.
- The 3-fragment loss with chip keys is the chip's: fragments 1 and 2 leave it
  32 octets longer and fragment 1 without More Fragments
  ([above](#what-chip-firmware-1176-does-with-fragments)). One fragment at a
  time was tried only with host CCMP (the table above).
- A Linux recipient that holds a Block Ack session for that TID drops the first
  fragment numbered 1 or more and ends its session (`rx.c`
  `ieee80211_rx_reorder_ampdu`, then `iface.c`, a DELBA with reason 38); the
  rest of that frame then fails reassembly. With `AT+HOSTFRAG=0` nothing holds
  back the Warthog's next ADDBA (the 15 s hold follows only a host cut), so a
  session can form again before the next frame (source; `addba_tx` was not
  recorded); every reply the chip cut in 2 under the Warthog's session was lost
  (0/8, the table above).

Host-tested: every fragment meets mac80211's reassembly rules and reassembles
byte-identical through the Warthog's own receive path, on both crypto builds,
relayed, with Address Extension and in batman mode; every rate in each
fragment's chain carries it; the pool cases; the Block Ack rule; the descriptor
fields under `AT+TIDPARAMS` against a fake chip that aggregates on them (`orig`
1, `ba_end` 0 and `agg` above 0 under `=0`, `agg` 0 under `=1`), also after the
peer ends its session (`ba_rcpt`); a DELBA the chip gives up on (`delba_noack`);
`AT+HOSTFRAG=0` leaves every frame's bytes unchanged (pinned to recorded frames,
with and without Block Ack). These run with `WARTHOG_MESH_HOSTFRAG_ANY` (up to
16 fragments, host CCMP cutting too), which no env sets. The shipping rule
(`test_simnode_hostfrag_rule`, all four mesh builds): at most 2 fragments with
chip keys, the chain moved to rates that need 2 or fewer or replaced by the
slowest that does, `clamp`, `many` under `AT+FRAG`; nothing cut with host CCMP;
`AT+SEALFIT` on and off, with chip keys at `AT+HOSTFRAG=0` too (whole under a
requested or agreed session, `seal_ba`; not under a refused one, on TID 6 or
with `AT+AMPDU=0`; in at most 2 under an `AT+FRAG` below the frame) and on group
frames; with `auto`, a frame whose cut ended the session but goes whole (`pool`)
sent whole before the DELBA's TX status; a first rate put in keeps rate
control's RTS choice; the pool reserve (5); the default (`auto` on the chip-key
builds, off with host CCMP; every env's flags in the glue guard) at 1 MHz MCS0
under a session: the session ended and 1000- and 1472-byte replies cut in 2 with
chip keys, whole with host CCMP.

To test, on the Warthog sending to a pinned Pi (`AT+TXRATE=0,1` forces 1 MHz
MCS0; `auto` is the default on chip-key builds):

```
AT+TXRATE=0,1
AT+HOSTFRAG=auto
AT+TXCAP=2,<pi>
AT+HOSTFRAG?
AT+TXCAP?
```

Pass on a chip-key build: 1000-, 1400- and 1472-byte pings from the Pi are
answered; 1000-byte replies go in 2 fragments at `r=0@1M...`, 1400 and 1472 in
2 at `r=1@1M...` (`cap_sub` +1 a reply); `msdu ok` rises with `msdu`; `noack`,
`fail`, `many`, `pool`, `stale`, `overlap`, `agg`, `nodelba`, `ba_late` and
`delba_noack` stay at 0; the Pi's `RX MPDUs with MIC fail` and `rx drop misc`
do not move. With `AT+HOSTFRAG=0` the same replies go whole to the chip
(`hf=0`): under the Warthog's Block Ack session with the Pi (`AT+AMPDU?`
`ours=`, or `asked=` while its ADDBA is unanswered) and `AT+FRAG=0`, 1000 bytes
at `r=1@1M...` and 1400 and 1472 at `r=2@1M...` (`seal_ba` +1 a reply), with no
session (`AT+AMPDU=0`) 1000 bytes at `r=0@1M...` and 1400 and 1472 at
`r=1@1M...` (`seal_sub` +1 a reply), cut in 2 by the chip. Pass on a
host-CCMP build: `rule=off`, `msdu` stays, every reply whole (`hf=0`), at
`r=1@1M...` for 1000 bytes and `r=2@1M...` for 1400 and 1472 (`seal_sub` +1 a
reply), and the Pi's `RX MPDUs with MIC fail` does not rise. A/B: `AT+SEALFIT=0`
(the replies go at 1 MHz MCS0 and the chip cuts them), `AT+TIDPARAMS=0`,
`AT+AMPDU=0`. Read `AT+TXCAP?` after the traffic stops; `st_lost=0` in its
header.

Before comparing fragments between two Warthogs, check the capture on frames
it does not cut: `AT+TXCAP=2,<receiver>` on the sender and `AT+RXCAP=1,<sender>`
on the receiver, small pings. Each frame's bytes match on both sides except
Duration (one calibration, 2026-10-03).

## IP fragments from nodes at bat0's MTU (1460)

These are IP fragments, not the 802.11 fragments above. OpenMANET sets `bat0`
to MTU 1460; a node whose `br-lan` or `eth0` has the same MTU sends every IP
packet over 1460 bytes in fragments, a 1472-byte ping as two. ESP-IDF's lwIP
drops IP fragments addressed to itself unless reassembly is built in, and it is
off by default: on air 2026-10-03 (two hours, both boards, every rate) no
fragmented ping from such a Pi was answered, 0/5 in each of 24 rounds at 1452
and 1472 bytes, while the Pi at MTU 1500 got 1472-byte replies. Builds from then
on reassemble IPv4 (`CONFIG_LWIP_IP4_REASSEMBLY`, `main/nat_frag.c`), up to 10
fragments a datagram, and through NAT before NAPT reads its ports, so a tethered
host's large packets cross both ways. IPv6 is not reassembled: ESP-IDF drops all
IPv6 on a netif without a link-local address, and none has one
([Troubleshooting](Troubleshooting#large-packets-from-a-node-at-mtu-1460-go-unanswered-ip-fragments)).

On the node at MTU 1460:

```sh
ip link show br-lan | grep -o 'mtu [0-9]*'
ping -c 5 -s 1472 <warthog>     # two fragments each
ping -c 5 -s 4000 <warthog>     # three
ping -c 5 -s 14392 <warthog>    # ten, the most
```

Pass: 5/5 each, and `AT+MTU?` `ip_reass` rises by 5 each run (`ip_reass_drop`
flat). From a tethered host on the Warthog's USB or access point, `ping -c 5 -s
2000 <node>`: 5/5, and `ip_reass` rises by 10 (the host's requests and the
node's replies). In batman mode `AT+BATSTAT?` `q_tx_full` stays flat.

Measured on air 2026-10-03, both boards, in plain mesh mode
(`AT+MESHBATMAN=0`): from a Pi at MTU 1460 and at 1500, pings up to `-s 14392`
5/5; from a Mac on the Warthog's USB through NAT to the Pi, 100, 1472, 1473,
2000 and 6000 bytes 3/3 (one 6000-byte run 2/3), `ip_reass` rising both ways,
`ip_reass_drop` 0. Batman mode (fragments on bat0, `q_tx_full`) is not
measured.

## Group frames in the chip (`warthog-mesh-sae-meshvif`)

Measured on air on 2026-10-01 (results [below](#measured-on-air-2026-10-01)).
`warthog-mesh-sae-meshvif` is
`warthog-mesh-sae` (AMPE keys in the chip) on the MESH chip interface of
`warthog-mesh-sae-swccmp-meshvif`.

On 2026-09-30, before the change below, it peered with both 1.8.0 Pis;
`AT+KEYINST?` showed Warthog's own MGTK at AID 0 and each Pi's MTK at that Pi's
AID, and Warthog-to-Warthog unicast passed. Every Pi group frame was
undecryptable (`AT+RXCHAN?` `nodec grp` climbing, reason 4): the Pi's broadcast
ARP, and its group PREQs (group-addressed privacy, under the Pi's MGTK). So the
Pi built no path to the Warthog and sent it no unicast: the Warthog's ARP
reached the Pi, the Pi's reply never arrived.

The Pis' morse_driver (`mac.c` `morse_mac_ops_set_key`) installs every key that
has a station into the chip at that station's AID with the key's own key id:
the peer's MTK and the peer's MGTK. A group key without a station goes at AID 0.
That is how the Pis' chips open each other's group frames. This build does the
same:

- A peer's MGTK goes into the chip at the peer's AID when AMPE delivers it, as
  a group key under its key id (hostap always uses 1), beside Warthog's own MGTK
  at AID 0. `AT+KEYINST?` lists it as `aid=<n> pw=0`. Each install starts the
  key's TX PN at a fresh 2^20 epoch above every earlier install, so if the chip
  ever sealed Warthog's group frames under it no PN would repeat;
  `AT+GTKPERSTA=2` installs at TX PN 0 instead, as Linux does.
- A group frame the chip opened is taken as its transmitter's only when all of
  these hold: that peer is keyed (its MTK installed); the chip holds the peer's
  MGTK at the peer's AID under the frame's key id; the frame was read off the
  chip after that key went in (the fence below); and no refused `DISABLE_KEY`
  left a stale key at that AID (the taint below). Anything else the chip opens
  is refused as possibly forged in the transmitter's name, among it a frame
  under Warthog's own MGTK (every peer holds it). Data: `AT+RXCHAN?` reason 95;
  group path selection: `mgmt gp own`.
- Replay is checked in the host, as mac80211 checks it behind a chip that
  decrypts: per sender, per TID for data and on the management counter for group
  path selection, above the Key RSC the peer's AMPE carried. A replay is dropped
  there (reason 5, `gp replay`) before any MIC work and is not counted in
  `rx_grp`, `mgmt_gp` or `mic_ok`.
- The MIC octets the chip leaves on a fresh frame are checked in the host under
  the sender's MGTK. Until one verifies, the check only counts (`mic_ok`,
  `mic_bad`); chip firmware 1.17.6 does not leave them intact (measured
  2026-10-01, [below](#measured-on-air-2026-10-01)). The first `mic_ok` proves
  the chip keeps them (a chance match is 2^-64) and arms the check for that
  frame class (`mic_arm=1` data, `2` group path selection, `3` both). From then
  on a frame whose MIC fails is dropped before any replay counter moves (95 or
  `gp micdrop`; `micdrop=<data>/<path selection>` in `AT+GTKSTAT?`).
- The fence. A frame the chip opened under a peer's previous MGTK can still be
  queued in the host when a different MGTK for that peer goes in (on a live link,
  or after the peer left and re-peered with the same MAC). Every frame read off
  the chip at or before that install is refused (`fence`), so it is never
  replay-checked against the new key's fresh counter. The same key installed
  again (a re-delivery, a survivor's re-install, `AT+REKEY`) moves no fence. A
  fence retires a minute after its install, on the 2 s service tick.
- The taint. If the chip refuses a `DISABLE_KEY` (`delfail`), it may still hold
  that key at that AID. Every group frame the chip then opens for that AID is
  refused (`taint`; `tainted=<slot bitmask>`) until a `DISABLE_KEY` there
  succeeds: the next install at that AID retries it first (a re-peer, a
  survivor's re-install, `AT+REKEY`), and the peer leaving takes the key at
  that index. A chip that boots clears it.
- A leaving peer's MGTK is taken out of the chip (`DISABLE_KEY`) where mac80211
  frees a station's keys. A new MGTK for the same peer replaces it. If the chip
  refuses an install, the old key is taken out and that peer's group frames stay
  unopened (reason 4) until the next delivery. A survivor's MGTK goes back in
  with its MTK when another peer leaves, and with `AT+REKEY`.
- `AT+GTKPERSTA=0|1|2` switches this at run time and persists it (NVS). `0`
  refuses every group frame the chip opens for a peer at once, and within one
  service tick (about 2 s) takes every peer's MGTK out of the chip and installs
  none: the behaviour measured before this change, on the same flash. `1` (the
  default) and `2` put them back. `AT+GTKSTAT?` shows `per_sta=off(at)`, `on`
  or `on(pn0)`.
- On a STA chip interface (every other build, and this one if the chip refused
  the MESH interface: `AT+MESHCFG?` `chip_vif=sta(1)`) peers' MGTKs stay
  host-only: a second group key there broke group decryption (measured).

### Measured on air 2026-10-01

One Warthog on this build, both 1.8.0 Pis and a `-swccmp-meshvif` Warthog:

- The chip (`mm6108.mbin` 1.17.6) opens a peer's group frames under the key at
  that peer's AID: `inst=3 fail=0`, `nodec grp` flat (before: about 32 a
  minute), group PREQs answered with PREPs, and each Pi holds an `ACTIVE` path
  to the Warthog.
- Pi to Warthog, 20/20 pings and 10/10 at 1000 bytes; Warthog to each Pi and to
  the other Warthog, 10/10 (before: 0/3). One-hour soak: 239/240 from the Pis,
  60/60 to the other Warthog, `rx_grp` 2152, `mgmt_gp` 62, `forged`, `fence`,
  `taint` and `delfail` 0.
- Warthog's own MGTK at AID 0 still seals its group frames: its broadcast ARP
  reaches the Pis.
- `AT+GTKPERSTA=0` took the 3 keys out (`del=3 delfail=0`) and the Pis' group
  frames went undecryptable again, not `forged`, so `DISABLE_KEY` removes the
  key; `=1` put them back.
- The chip does not leave the MIC octets intact: `mic_bad` counts every frame,
  `mic_ok` stays 0, so the MIC check never arms and the first residual below
  stays open.
- `AT+GTKPERSTA=2` on air (2026-10-01): keys at TX PN 0, traffic passed. Not
  measured: AID reuse after a peer leaves and another joins. Batman mode stays
  refused on this build.

Residuals, as on Linux:

- If the chip, after the key at the sender's AID failed the MIC, also tried
  Warthog's own at AID 0 under the same key id (both are key id 1), any member
  could forge a peer's group frames, at any PN: the frame is taken, and pushes
  that peer's replay counter (its data TID, or its group path selection) so its
  real frames are refused on this node until its link is re-formed. Once the
  MIC check is armed such a frame is dropped before it moves anything.
- A frame the chip opened under a previous key and handed to the host only after
  the new key's `INSTALL_KEY` answer is not caught by the fence, which goes by
  when the host read it.
- Every peer of a node holds that node's MGTK, so any of them can forge the
  node's group frames under it ([below](#management-frame-protection-peering-does-not-need-it-path-selection-does)): that frame's
  MIC verifies.

Check it on the bench with `AT+GTKSTAT?`: `per_sta=on`, one `[<mac> aid= id=1
hw=]` per keyed peer; `inst` one per keyed peer after a fresh start (it grows by
one for each survivor when a peer leaves, on `AT+REKEY` and on an MGTK
re-delivery); `rx_grp` and `mgmt_gp` climbing with the node's broadcasts and
PREQs; `mic_bad` keeping pace with them, `mic_ok` and `mic_arm` at 0 (chip
firmware 1.17.6 does not leave the MIC octets intact; measured 2026-10-01);
`fail`, `delfail`, `tainted`, `taint`, `micdrop` at 0 and `forged` flat in
steady state (`fence` may move by a frame or two when a peer re-keys).
`AT+RXCHAN?` `nodec grp` should stay flat, and on the node `iw dev wlh0 mpath
dump` should list the Warthog `ACTIVE`.

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

## When a node will not join

A mismatched node beacons happily and alone. Nothing about the radio looks
wrong, so this reads as a range problem and gets chased as one. Warthog says so
instead: about 20 seconds after the mesh starts, and every minute after that
while it has no peers, it logs every value a peer matches on and the likely
cause. The same thing is available on demand:

```
AT+MESHCFG?
+MESHCFG: region=US country=US
+MESHCFG: enable=1 secure=1 pmf=off dhcp=1 fwd=0 bridge=0 grp=replicate batman=0 id='openmanet-mesh' pass=12 chars
+MESHCFG: applied chan=42 freq=923000000 bw=2 gclass=69 sclass=2 (set_channel_list=0)
+MESHCFG: peers=0 beacons_heard=0
+MESHCFG: chip_vif=mesh(5) vif_id=0 built=mesh fallback=0 add_st=0 bssid_refused=0(st=0) mesh_config_refused=0(st=0)
+MESHCFG: chip_refused beacon_config=0(st=0) sta_state=0/0/0/0/0(st=0) install_key=0(st=0) keyidx_mismatch=0 mesh_config_sent=beaconless(2)
+MESHCFG: 0 beacons heard: nothing is audible. Wrong channel or bandwidth, or out of range. Check the channel first
+MESHCFG: forwarding=no routing=none l2=no(NAT) multicast=meshtastic(239.0.0.69) batman=no
```

The second-to-last line is the diagnosis, and the firmware logs the same one
unprompted while it has no peers. Its cause-selection is unit-tested on the
host (`components/halow_mesh_compat/test/test_mesh_diag.c`) — ordering
included, because reporting a later cause while an earlier one holds sends you
after the wrong thing.

`beacons_heard` splits the causes apart, and it is the only number worth
reading first:

| Reading | Means | Check |
|---|---|---|
| `applied chan=NONE` | The radio has the whole country list; its operating channel is neither chosen nor observable | Region builds ship unpinned. Set `AT+MESHCHAN=<chan>,<freq_hz>,<gclass>,<sclass>,<bw>` to match the peer and reboot. An unpinned radio meeting a mesh is luck, not configuration. |
| `set_channel_list` non-zero | The channel was refused; the radio is **not** on the channel printed above | The channel must exist in the country's regulatory table. `AT+MESHCHAN=default` restores the build-time pin. |
| `beacons_heard=0` | Nothing is audible | Channel, bandwidth, or range. Compare `applied chan`/`bw` against `uci get wireless.radio1.channel` on the peer — `radio1` is the HaLow device and `channel` lives on the device, not the iface; `radio0` is the 5 GHz radio and `default_radio0.channel` is unset. Fix this before looking at anything else. A peer in beaconless mode sends no beacons at all; when its probe requests name our mesh, the diagnosis line says so. |
| `beacons_heard>0`, `peers=0` | The mesh is audible and Warthog will not join it | Mesh ID (exact match, case included), operating class, or security mode. An open Warthog will not peer with an SAE mesh, and vice versa. Signal: an OpenMANET node takes no candidate it hears at or below its `mesh_rssi_threshold` (-80 dBm), and Warthog starts no peering at or below its own floor (`AT+MESHRSSI`, also -80), which the diagnosis line reports. Management frame protection is **not** on this list — peering is measured working with it off; it governs path selection (see the PMF note below). |
| `peers>0` but no traffic | Peered; this is a data-plane question | `AT+RXCHAN?` and `AT+MPING=<peer>,8`. |

### Management frame protection: peering does not need it, path selection does

An OpenMANET node configures MFP required — `ieee80211w=2` in its generated
supplicant config — and once peered its station dump reads `MFP: yes`. For the
*peering* that is not a demand on us. Warthog peered with exactly such a node
while running MFP **off**, and that peer reported `mesh plink: ESTAB`,
`authenticated: yes` and `MFP: yes` for the link at the same time. The AMPE
framing follows what our RSN element advertises, and off and required are each
self-consistent; only "optional" desyncs the two ends, which is why no setting
here can select it. Do not read `MFP: yes` as the reason a peering fails.

It does govern path selection. hostap mesh negotiates no MFP: the node marks
every SAE peer MFP whenever its own `ieee80211w` is not 0, and its mac80211 then
drops unprotected unicast Mesh Action frames (PREQ, PREP, PERR). Group Mesh
Action frames are group-addressed privacy (802.11 Table 9-47, "Group Addressed
Privacy"; `ieee80211_is_group_privacy_action` in mac80211): on an SAE mesh the
node sends every group PREQ and PERR CCMP-protected under its own MGTK, with the
Protected bit and no MMIE, whatever `ieee80211w` says, and from a peer it marks
MFP it drops a group one that is in the clear or carries a BIP MMIE. A 1.8.0 node
runs HWMP even to a one-hop peer (its effective `mesh_nolearn` is 0), and under
SAE it gets its path to Warthog only by Warthog answering its group PREQ. So
Warthog, on the SAE build:

- sends unicast path selection to a peer whose AMPE carried an IGTK (the node
  sends one because it runs MFP) CCMP-protected under that link's key, by the
  same route as its data;
- sends its group path selection (a relay's PREQs and PERRs, a bridge's PREQs)
  CCMP-protected under its own MGTK, which the node holds from our AMPE: one
  frame to every peer alike, as mac80211 sends it, sealed by the chip as our group
  data is. No MMIE, with `AT+MESHPMF` 0 or 1;
- sends that peer its Block Ack frames (the ADDBA request any unicast data frame
  on TID 0-5 can start, its ADDBA response, DELBA) protected the same way, since
  the node's mac80211 drops them unprotected too. Under host CCMP one that
  cannot be sealed is dropped, never sent in the clear. Before this every ADDBA to
  a wizard node was dropped, so no A-MPDU session formed either way and an
  ADDBA/DELBA pair repeated about every 60 s per peer; now sessions can form.
  `AT+AMPDU=0` stops the Warthog starting sessions and ends those it holds
  (host-tested). On air against OpenMANET 1.8.0 Pis at `ieee80211w=2`, the
  Warthog's originator sessions formed (2026-10-02 and 2026-10-03; `AT+AMPDU?`
  read `orig=2`);
- refuses that peer's unprotected unicast path selection, and takes its group
  path selection only Protected: opened under the node's MGTK (from its AMPE),
  by host CCMP or, on `warthog-mesh-sae-meshvif`, by the chip holding that MGTK
  at the node's AID, and above that key's management replay counter. In the
  clear (from any peer, MFP or not, stricter than mac80211), with an MMIE, under
  another key id, or opened by the chip under anything but the node's MGTK at
  its AID (on every other build the chip's only group key is Warthog's own MGTK,
  which every peer holds, so such a frame could be forged in the node's name by
  any of them), it is refused.

Group path selection proves only that its sender is a mesh member, not which
one. Every peer of a node holds that node's MGTK, and hostap never rekeys it
(`mesh_rsn.c`: "TODO: support rekeying"), so any current or former peer of node
A in radio range can forge A's group PREQ or PERR, and can push A's management
replay counter to the top, after which Warthog refuses A's own direct group path
selection until A's link to it is torn down and re-formed (the same key installed
again on a live link keeps the counter), or, if the forger keeps at it, until A
restarts its mesh. A node's group data is pushed the same way, per TID. mac80211
behaves the same way. Only the builds that can open these frames are exposed:
host CCMP on, and `warthog-mesh-sae-meshvif`.

Only a swccmp build with host CCMP on (`AT+SWCCMP=1`, or `-swccmp-on`; batman
mode arms it), or `warthog-mesh-sae-meshvif`, can open the
node's group path selection. Elsewhere the chip holds one group key, Warthog's
own, so on `warthog-mesh-sae` and the other chip-crypto images
every such frame is undecryptable, as the node's group data is (`AT+RXCHAN?`
reason 4, or 95 for one the chip opened under our MGTK): `mgmt gp nodec` climbs
and a wizard node has no path to Warthog. Another Warthog's group path selection
is undecryptable there too, so on those images Warthog relays and bridges
exchange none, whatever `AT+MESHPMF`.

Neither side needs a setting changed for this: not `AT+MESHPMF=1` on Warthog,
and not `ieee80211w='0'` on the node (which would also turn MFP off between
OpenMANET nodes; the node still sends its group path selection protected then).

Measured on 2026-09-29 before this change, against 1.8.0 Pis with
`ieee80211w=2`: Warthog refused every group PREQ the node sent (then counted
`bipfail`, though host CCMP had opened each one), so the node's `iw dev wlh0
mpath dump` showed Warthog `RESOLVING` and no unicast from the node reached it.
The rules above are from mac80211's source (`net/mac80211/rx.c`, `tx.c`,
`wpa.c`) and the host tests. **Re-measured on 2026-09-30** with the fix
(`warthog-mesh-sae-swccmp`, batman mode, `AT+MESHPMF=0`): the node's `iw dev
wlh0 mpath dump` listed both Warthogs `ACTIVE`, hop count 1, next hop the
Warthog; on the Warthogs `hwmp gp` read 8–9, `AT+HWMPSTAT?` `preq_rx` 8–9 and
`prep_tx` 4–5; unicast from the node arrived (`batctl ping` 5/5 to each, DHCP,
pings from a host on the node's LAN). That covers the node's group PREQs taken
and the Warthog's protected unicast PREP; the group path selection a Warthog
relay or bridge sends is not measured (below). On the bench:
`iw dev wlh0 mpath dump` on the node should show Warthog `ACTIVE`; on Warthog,
`AT+MESHFWDSTAT?` `hwmp gp` counts the node's group path selection taken, with
`mgmt gp nodec`, `own`, `key` and `replay` at 0 (or `nodec` alone climbing on a
build that cannot open it), `hwmp tx prot` unicast frames sent protected, `hwmp
tx gp` group ones sent under our MGTK, `mgmt tx chip` (or `host` while host CCMP
is on) Block Ack frames sent protected, with `drop` at 0, `mgmt prot chip` (or
`host` on swccmp-on) protected ones opened, `mgmt prot nodec` protected ones the
chip handed up unopened (then only host CCMP can read them), `mmie` 0 (the node
never sends one), and `igtk` whether its IGTK was installed (the node counts as
running MFP from that install). `AT+MESHCFG?` prints `pmf=` so the two sides can
be compared directly.

That check does not show whether the node takes Warthog's own group path
selection: the node's path to Warthog comes from its own PREQ and Warthog's
unicast PREP, and `hwmp tx gp` counts frames sent, not taken. Two unmeasured chip
properties decide it: whether the chip seals a group management frame under
Warthog's MGTK at all, and, on `-swccmp`, `-swccmp-meshvif` and `-swccmp-on`,
whether it starts that key at the TX PN `INSTALL_KEY` gives it. Those builds
install our MGTK at a nonzero PN (at least 2^20) and advertise one below it as
its Key RSC; if the chip starts lower, the node drops every group PREQ and PERR
of ours as a replay, and each re-install reuses PNs under the same key. To
settle both, have a Warthog relay or bridge discover a destination the node can
learn only from Warthog's PREQ and look for it in the node's
`iw dev wlh0 mpath dump`, or capture one of Warthog's group PREQs in monitor
mode and compare its CCMP PN (in the clear header) with the RSC Warthog
advertised. On Warthog, `AT+MPMPEERS?` `mgtk_reinst` climbs with the AMPE Opens
that follow such frames and `mgtk_rsc_fail` stays 0; between two Warthog relays
on these builds, the receiver's `hwmp gp` rising with `mgmt gp replay` at 0 is
the same check.

**A station entry is not a peering.** `iw dev wlh0 station dump` lists blocked
candidates too, so counting stations reports success that is not there. Only
`mesh plink: ESTAB` counts — a blocked candidate shows `llid 0`, `plid 0`,
`authenticated: no` and an airtime metric of `-1`, while still accumulating
`tx failed`. Measuring a data path across such a link returns 100% loss that
says nothing about the data path.

### Two OpenMANET nodes can fail SAE with each other

Worth knowing before blaming a Warthog for a mesh that will not form: two
OpenMANET Pis with the same `mesh_id`, passphrase, channel and `ieee80211w=2`,
RF verified at 923000 kHz / 2 MHz, peer with each other only intermittently.
They have held `ESTAB` for long stretches, and they have also gone 70 minutes
(210 samples) without it. The failing state is a loop: four
`MESH-SAE-AUTH-FAILURE`, then `MESH-SAE-AUTH-BLOCKED duration=300`, then again.

What the log lines mean, from the supplicant and the Morse driver source (an
OpenMANET 1.8.0 image runs morse_driver mm6108-2.0.1 and hostap
mm8108-2.0.0):

- `process_mesh_rx_mgmt_beaconless: Rx of Mesh Auth from unknown peer` is the
  driver's **first-contact path, not the failure**. It drops the Auth and
  immediately injects a synthesized probe response, so the receiver learns the
  sender at once and starts its own SAE. On these nodes it fires once per
  cycle, about 45 s before each `BLOCKED`.
- `MESH-SAE-AUTH-FAILURE` is a **timeout**: SAE did not reach ACCEPTED within
  10–19 s (10 s plus a random 0–9 s). It is not a password or crypto
  rejection. After four, the peer is blocked until the inactivity timer frees
  the station, `mesh_max_inactivity` (300 s by default) after it was created;
  `duration=300` prints that setting.

So both sides know each other and SAE still times out. Why is open, and the
stuck state could not be produced on demand. From a healthy start, with debug
logging on both nodes, every kind of drop recovered by itself:

| Drop | Recovered in | How |
|---|---|---|
| Clean close, keys cached both sides | ~1 s | SAE skipped: both reuse the cached key (PMKSA caching) |
| Clean close, one side's cache flushed | <10 s | The cached side cancels caching when the peer starts SAE |
| Clean close, both caches flushed | <10 s | Full SAE |
| Graceful reboot of one node | ~20 s | It sends Close frames on the way down |
| One node's supplicant killed (a crash) | ~50 s | Almost all rediscovery; the survivor, still holding the old link, tears it down itself when the restarted node's SAE Commit arrives |
| No traffic for 12 min | never dropped | At 300 s idle the peer is polled; answered, the link stays |

Rebooting **both** nodes together cleared a stuck pair within 37 s of boot;
rebooting one did not. So the stuck state depends on something a fresh pair
does not have — long uptime, accumulated driver or chip state, or RF
conditions (both radios count millions of PHY signal-field failures in
`morse_cli -i wlh0 stats`). Also ruled out: restoring a disabled `mesh11sd`,
and matching the one mesh parameter that differed (`mesh_rssi_threshold`,
`-80` vs `0`).

To trace it when it next happens, raise the supplicant's log level at runtime —
no restart, so nothing about the failure is disturbed — and stream the log
past the ring buffer:

```
wpa_cli_s1g -p /var/run/wpa_supplicant_s1g -i wlh0 log_level DEBUG
logread -f > /tmp/trace.log &        # log_level INFO and killall logread afterwards
```

`wpa_cli_s1g ... pmksa` lists the cached keys and `mesh_peer_remove <mac>`
closes one link cleanly, both without disturbing the radio.

An established link is **not** torn down for being idle. At 300 s without
traffic (`mesh_max_inactivity`) the supplicant polls the peer, and when the poll
is answered the link carries on. Measured: after 8 minutes of 1 Hz traffic
across the mesh (483/483 replies, 4–15 ms), the same link sat idle for 12
minutes and survived two polls — inactivity climbed to 300 s on both nodes and
reset together each time. Drops seen about 300 s after a link formed coincide
with that poll, which is consistent with a poll going unanswered rather than
with idleness itself. Traffic avoids the poll altogether.

Measurement traps on a pair like this:

- Poll `mesh plink:` and count; do not diagnose from `logread`, which can hold
  `plink ... established` lines old enough that nothing is establishing now.
- Compare like with like. `dmesg` counts since that node's boot. On a node
  without `bat0`, `openmanetd` logs an error every few seconds, so `logread`
  holds only about the last hour.
- The Pis' clocks are wrong and do not agree with each other (here by about a
  day and a half). Correlate the two nodes by the host's poll time or by
  uptime, never by their log timestamps.
- Once the link is up, two nodes that share a LAN address (OpenMANET's
  `10.41.254.1`) sit on one bridged segment, so a ping from that address loses
  most replies to the other node. Ping from a transient unique address instead.

The capability line is not a placeholder; it reports the gates. With the
defaults a Warthog does not forward for other nodes, does not run a routing
protocol, does not bridge the tethered client onto the mesh at layer 2, and
carries only Meshtastic's multicast group across — a node that peers correctly
is still a leaf. `AT+MESHFWD=1` turns on 802.11s forwarding and HWMP
(`forwarding=yes(802.11s) routing=hwmp`) and `AT+MESHBRIDGE=1` the layer-2
bridge (`l2=bridge multicast=all(bridged)`); both are compiled and host-tested.
Unicast relay with `AT+MESHFWD=1` was measured on air under SAE on 2026-10-02,
with two OpenMANET nodes' paths pinned through the Warthog
([below](#turning-on-the-relay-the-on-air-experiment-in-order)); path selection
through a Warthog and bridge mode were not.

`multicast=meshtastic(239.0.0.69)` is deliberate. In NAT mode the repeater in
`main/mudp.c` carries Meshtastic's UDP group, `239.0.0.69:4403`, between the
tethered links and the mesh, and no other group. Every Warthog NATs its
tethered host to the same compile-time addresses (`192.168.4.1` on USB), so two
hosts on opposite sides of a mesh are both `192.168.4.x`. Protocols that carry
the sender's address in the payload — CoT, mDNS/SD — would therefore be
repeated into a contact the receiver cannot reach, or worse, one that aliases
itself. Widening the repeater would make discovery look like it works. The fix
is one L2 segment with unique host addresses, which is bridge mode; see
`docs/mesh-attachment-model.md`. OpenMANET's own CoT (`239.2.3.1:6969`)
carries no tethered-host address, so repeating it from mesh to tether would not
alias, but in plain mesh mode it never reaches a Warthog, and in batman mode it
reaches `bat0` and is not repeated (next section).

## Reaching OpenMANET applications

OpenMANET ships nothing for Meshtastic; no OpenMANET service uses
`239.0.0.69:4403`. Its own applications — CoT on `239.2.3.1:6969`,
push-to-talk RTP on `239.192.41.1`, mDNS and alfred — run on `br-ahwlan`, which
the mesh wizard creates with `bat0` as its mesh port; a node without the wizard
has no `br-ahwlan` and does not run them. A Warthog in plain mesh mode peers with a wizard node at
802.11s but gets no IP path into `bat0` (see
[above](#an-address-on-a-bridged-mesh-interface-is-ignored)), so its tethered
host reaches none of them. Two ways in:

- **Warthog as a BATMAN_V member** (`AT+MESHBATMAN=1`, [Batman Mode](Batman-Mode)).
  Its own address comes by DHCP from the first node whose DHCP server on
  `br-ahwlan` answers, else from the static fallback after 45 s, which keeps
  asking DHCP; it asks again when its lease's router leaves; its tethered
  host is NATed behind it. Meshtastic's group is repeated; CoT's
  `239.2.3.1:6969`, push-to-talk and mDNS reach its `bat0` and are not.
  Measured on air on 2026-09-30 against Pis whose `bat0` was set up by hand,
  on each Pi a port of `br-lan` rather than the wizard's `br-ahwlan`: a lease
  from a Pi's dnsmasq, pings from a host on the gateway Pi's `br-lan`, and
  Meshtastic datagrams from a Warthog appearing on it
  ([Batman Mode](Batman-Mode#measured-on-air)).
- **Warthog as a HaLow station on an OpenMANET HaLow AP bound to `ahwlan`.**
  The operator creates the AP by hand, a `meshap_<radio>` wifi-iface with
  `mode='ap'`, `network='ahwlan'` and their own encryption and key; no wizard
  exposes it. Warthog joins it in station mode (`AT+HALOW=<ssid>,<pass>`).
  Not measured; whether the node's chip runs the AP and the mesh together is
  unknown.

## Turning on the relay: the on-air experiment, in order

Unicast relay was measured on air on 2026-10-02 under SAE: a
`warthog-mesh-sae-meshvif` Warthog with `AT+MESHFWD=1` relayed between two
OpenMANET 1.8.0 Pis whose paths were pinned through it (`iw dev wlh0 mpath new
<other-pi> next_hop <warthog>`), 300-, 1000- and 1400-byte pings 10/10 each,
`fwd uni` 106. Those frames named the other Pi in addr3, so the chip hands up
third-party frames (step 1). Path selection through a Warthog, relayed group
frames, hosts across the relay and bridge mode are host-tested and simulated
only. The order, each step a counter read rather than an argument:

1. **Does the chip hand up third-party frames at all?** One Linux node peered
   with the warthog, forwarding still off. On the Linux node, install a static
   mesh path to a fabricated address via the warthog and ping it, so every
   frame is addressed to the warthog with a third party in addr3 — the exact
   frame a relay receives. `fwdcand` in `AT+RXCHAN?` climbing, with
   `rxdrop reason=93` beside it, means the MM6108 delivers them and everything
   below is buildable. The full procedure, its positive control and the
   four-way reading are in `docs/mesh-attachment-model.md`.
   Two setups look equivalent and are not. A warthog overhearing two peers
   receives data addressed to someone else; the MM6108 hands it up, but the
   receive filter drops it on addr1 first (`not_ours` in `AT+FILTSTAT?`), so
   `fwdcand` stays at zero there, which proves nothing. And a warthog placed
   *between* two nodes with forwarding off never has a path formed through it
   (it neither advertises forwarding nor answers path selection for others),
   so nothing addressed to it carries a third party at all.
2. **Path selection through the warthog.** `AT+MESHFWD=1`, `AT+RESET`. The
   peer's beacons now see the Forwarding capability. On an OpenMANET node,
   `iw dev wlh0 mpath dump` should show the far node with the warthog as
   next hop. On the warthog, `AT+MESHPATH?` should list both nodes, each via
   itself, and `AT+MESHFWDSTAT?` should show `relay preq` and `relay prep`
   climbing. If paths never form, `parse_fail`/`not_ours` on `AT+HWMPSTAT?`
   say whether the frames were even understood.
3. **Data through the warthog.** Ping node to node. `AT+MESHFWDSTAT?`
   `fwd uni` counts each relayed frame; `hold n` means a node asked us to
   relay to a destination we had no path to, `hold tx` those we then found and
   sent, `hold drop` those we gave up on after 6.8 s. Then a
   broadcast (ARP will do): `fwd grp` should count once per frame, `drop dup`
   should catch the echo.
4. **Hosts across the relay.** A laptop behind each node; ping laptop to
   laptop. `AT+MESHPATH?` should show both laptops as `host=... behind=...`.
5. **Bridge mode.** `AT+MESHBRIDGE=1`, `AT+RESET`. The warthog's USB host
   should get a lease from the OpenMANET node's dnsmasq (or nothing, on a
   warthog-only mesh — that is the documented cost), and mDNS/CoT discovery
   between the laptop and a node's host is the thing this mode exists for.
   This needs a node without `bat0`; OpenMANET's own services are not
   reachable this way (see
   [Reaching OpenMANET applications](#reaching-openmanet-applications)).

`tools/bench/openmanet_interop.py --fwd` sets step 2 up and reads steps 2–4;
`--bridge` adds step 5. Both report; neither asserts, because only unicast
relay over pinned paths has a measured baseline.

## Vanilla OpenWrt

The same procedure applies to stock OpenWrt with a Morse Micro driver — nothing
above is OpenMANET-specific. What matters is that `mesh_id`, channel, bandwidth
and operating class match the Warthog build, and that the two interface defaults
above are dealt with.
