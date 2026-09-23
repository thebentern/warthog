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

OpenMANET side — use `uci`. This is the idiomatic path and the one verified on
hardware; a hand-run `wpa_supplicant` fights netifd and loses its config on the
next `wifi` event.

```sh
uci set wireless.radio1.disabled='0'
uci set wireless.default_radio1.mode='mesh'
uci set wireless.default_radio1.mesh_id='halowmesh'      # must match Warthog
uci set wireless.default_radio1.encryption='sae'
uci set wireless.default_radio1.key='warthog-mesh'       # must match Warthog
uci set wireless.default_radio1.sae_pwe='2'              # see below
uci commit wireless

uci set mesh11sd.mesh_beaconless.mesh_beacon_less_mode='1'
uci set mesh11sd.mesh_dynamic_peering.enabled='1'
uci commit mesh11sd

wifi down radio1 && wifi up radio1                       # NOT `wifi reload`
```

Three traps, all of which cost real bench time:

1. **`wifi reload` silently ignores `mesh11sd` changes.** netifd only
   regenerates `/var/run/wpa_supplicant-wlh0.conf` when the *wireless* config
   changes. Check the file's mtime — a stale one means your change never
   applied. Cycle the radio instead.
2. **Beaconless mode is not optional on the MM6108.** With beaconing on, the
   chip firmware faults on the mesh-beacon path: two `HW has stopped` events
   and `wlh0` goes down. Beaconless gives 0 crashes over a 6-minute soak.
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
every link, with the Warthog holding two distinct AMPE pairwise keys at once
(`AT+KEYINST?` showing `aid=1 pw=1` and `aid=2 pw=1`). The peer's own log shows
`mesh plink with <warthog> established` / `MESH-PEER-CONNECTED`, at −2 dBm and
135–150 Mbit/s VHT-MCS6/7.

**The encrypted data plane does not yet pass traffic cross-vendor.** ICMP is
0/30 and every undecryptable frame is group-addressed (`AT+RXCHAN?` shows
`nodec grp` climbing 1:1 with pings while `uni` stays 0). Two group-key
defects are responsible: the chip holds one VIF-wide MGTK latched at aid 0
while every 802.11s peer generates its own, and Warthog's own TX MGTK was
being dropped as an unknown peer. The second is fixed; neither is yet
confirmed on air. **For cross-vendor data today, use the unencrypted mesh
above.**

Earlier bench findings, still relevant:

- **OpenMANET's kernel-MPM mesh advertises Authentication Protocol 0 even
  when running SAE** (`wpa_supplicant_s1g` with `key_mgmt=SAE`; observed in
  its probe responses). Warthog's candidate gate therefore refuses to
  initiate toward it. `AT+SAEBRIDGE=2` overrides the gate for exactly this
  case. **Note (2026-09-20): the verified three-node run peered
  without setting it.** Either the gate no longer trips against current
  OpenMANET, or the beacon path now satisfies discovery. Try without it first.
  Also be aware `AT+SAEBRIDGE` is RAM-only — it resets to 1 on every boot, so
  anything depending on it is unusable on an unattended node.
- `sae_pwe=1` (H2E-only) is OpenMANET's shipped default; Warthog sends
  hunt-and-peck Commits, so set `sae_pwe=0` or `2` on the Linux side.
- The Morse supplicant rejects `MESH_PEER_ADD` even with `user_mpm=1` +
  `no_auto_peer=1`, so the Linux side cannot be told to initiate; and Warthog
  does not beacon in mesh mode, so kernel-MPM candidate discovery never sees
  it. The Warthog must initiate — hence the `AT+SAEBRIDGE=2` override.
- OpenMANET's kernel-MPM stack only engages SAE with peers it discovered from
  *beacons*, not from unsolicited Commits. Warthog now beacons (see below), so
  the discovery path exists in both directions: Warthog discovers OpenMANET
  from probe responses (with `AT+SAEBRIDGE=2`), and OpenMANET can discover
  Warthog from its beacons.

**Warthog mesh beaconing.** The MM6108 firmware fires its beacon TBTT once and
never re-arms it, so early builds did not beacon and were invisible to a
beacon-driven peer. A host beacon timer now re-drives the beacon at the
interval; `AT+BCNSTAT?` shows `served`/`txcomp` climbing together (~1.15/s),
i.e. the chip transmits every beacon. Warthog↔Warthog SAE is fully verified;
the Warthog↔OpenMANET SAE handshake over these beacons has since completed on
hardware (see the status note above).

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

## When a node will not join

A mismatched node beacons happily and alone. Nothing about the radio looks
wrong, so this reads as a range problem and gets chased as one. Warthog says so
instead: about 20 seconds after the mesh starts, and every minute after that
while it has no peers, it logs every value a peer matches on and which of the
two causes it is. The same thing is available on demand:

```
AT+MESHCFG?
+MESHCFG: region=US country=US
+MESHCFG: enable=1 secure=1 pmf=off dhcp=1 fwd=0 bridge=0 grp=replicate id='openmanet-mesh' pass=12 chars
+MESHCFG: applied chan=42 freq=923000000 bw=2 gclass=69 sclass=2 (set_channel_list=0)
+MESHCFG: peers=0 beacons_heard=0
+MESHCFG: 0 beacons heard: nothing is audible. Wrong channel or bandwidth, or out of range. Check the channel first
+MESHCFG: forwarding=no routing=none l2=no(NAT) multicast=no batman=no
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
| `beacons_heard=0` | Nothing is audible | Channel, bandwidth, or range. Compare `applied chan`/`bw` against `uci get wireless.radio1.channel` on the peer — `radio1` is the HaLow device and `channel` lives on the device, not the iface; `radio0` is the 5 GHz radio and `default_radio0.channel` is unset. Fix this before looking at anything else. |
| `beacons_heard>0`, `peers=0` | The mesh is audible and Warthog will not join it | Mesh ID (exact match, case included), operating class, or security mode. An open Warthog will not peer with an SAE mesh, and vice versa. Management frame protection is **not** on this list — interop is measured working with it off; see the PMF note below before chasing it. |
| `peers>0` but no traffic | Peered; this is a data-plane question | `AT+RXCHAN?` and `AT+MPING=<peer>,8`. |

### Management frame protection: `MFP: yes` on the peer is not a demand on us

An OpenMANET node configures MFP required — `ieee80211w=2` in its generated
supplicant config — and once peered its station dump reads `MFP: yes`. Neither
means a Warthog has to match it. Warthog peered with exactly such a node while
running MFP **off**, and that peer reported `mesh plink: ESTAB`,
`authenticated: yes` and `MFP: yes` for the link at the same time. The AMPE
framing follows what our RSN element advertises, and off and required are each
self-consistent; only "optional" desyncs the two ends, which is why no setting
here can select it.

So do not read `MFP: yes` on a peer as the reason a link is failing — that
reasoning has already produced one wrong diagnosis. `AT+MESHPMF=1` (then
`AT+RESET`) exists for a peer that genuinely refuses to peer unprotected, and
as a one-command A/B when SAE is failing for reasons not yet pinned down. It
has not been run on air, and off is the measured-working default.
`AT+MESHCFG?` prints `pmf=` so the two sides can be compared directly.

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

What the log lines mean, from the supplicant and the Morse driver source (which
driver release these nodes run is not established):

- `process_mesh_rx_mgmt_beaconless: Rx of Mesh Auth from unknown peer` is the
  driver's **first-contact path, not the failure**. It drops the Auth and
  immediately injects a synthesized probe response, so the receiver learns the
  sender at once and starts its own SAE. On these nodes it fires once per
  cycle, about 45 s before each `BLOCKED`.
- `MESH-SAE-AUTH-FAILURE` is a **timeout**: SAE did not reach ACCEPTED within
  10 s. It is not a password or crypto rejection. After four, the peer is
  blocked for `mesh_max_inactivity` (300 s by default).

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
does not carry multicast across — a node that peers correctly is still a leaf.
`AT+MESHFWD=1` turns on 802.11s forwarding and HWMP (`forwarding=yes(802.11s)
routing=hwmp`) and `AT+MESHBRIDGE=1` the layer-2 bridge (`l2=bridge`); both are
compiled and host-tested, and neither has yet been measured on air.

`multicast=no` is not a missing feature flag. Every Warthog NATs its tethered
host to the same compile-time addresses (`192.168.4.1` on USB), so two hosts on
opposite sides of a mesh are both `192.168.4.x`. Protocols that carry the
sender's address in the payload — CoT, mDNS/SD — would therefore be repeated
into a contact the receiver cannot reach, or worse, one that aliases itself.
Widening the repeater in `main/mudp.c` would make discovery look like it works.
The fix is one L2 segment with unique host addresses; see
`docs/mesh-attachment-model.md`.

## Turning on the relay: the on-air experiment, in order

Everything in forwarding and bridge mode is host-tested and simulated and
nothing in it has been on a radio. When a board is back, this is the order,
and each step is a counter read rather than an argument:

1. **Does the chip hand up third-party frames at all?** One Linux node peered
   with the warthog, forwarding still off. On the Linux node, install a static
   mesh path to a fabricated address via the warthog and ping it, so every
   frame is addressed to the warthog with a third party in addr3 — the exact
   frame a relay receives. `fwdcand` in `AT+RXCHAN?` climbing, with
   `rxdrop reason=93` beside it, means the MM6108 delivers them and everything
   below is buildable. The full procedure, its positive control and the
   four-way reading are in `docs/mesh-attachment-model.md`.
   Two setups look equivalent and are not. A warthog overhearing two peers
   receives frames addressed to someone else, which every 802.11 receiver
   discards on addr1 — zero there proves nothing. And a warthog placed
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
   `fwd uni` counts each relayed frame; `drop nopath` with `perr_tx` beside it
   means a node asked us to relay somewhere we have no route. Then a
   broadcast (ARP will do): `fwd grp` should count once per frame, `drop dup`
   should catch the echo.
4. **Hosts across the relay.** A laptop behind each node; ping laptop to
   laptop. `AT+MESHPATH?` should show both laptops as `host=... behind=...`.
5. **Bridge mode.** `AT+MESHBRIDGE=1`, `AT+RESET`. The warthog's USB host
   should get a lease from the OpenMANET node's dnsmasq (or nothing, on a
   warthog-only mesh — that is the documented cost), and mDNS/CoT discovery
   between the laptop and a node's host is the thing this mode exists for.

`tools/bench/openmanet_interop.py --fwd` sets step 2 up and reads steps 2–4;
`--bridge` adds step 5. Both report; neither asserts, because none of it has
a measured baseline yet.

## Vanilla OpenWrt

The same procedure applies to stock OpenWrt with a Morse Micro driver — nothing
above is OpenMANET-specific. What matters is that `mesh_id`, channel, bandwidth
and operating class match the Warthog build, and that the two interface defaults
above are dealt with.
