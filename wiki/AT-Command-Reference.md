# AT Command Reference

The CDC-ACM port accepts AT commands, so a node can be reconfigured and
inspected without reflashing. Any serial terminal at 115200 8-N-1 — CDC ignores
the baud rate.

Commands ending `=` set, ending `?` query. Every command answers `OK` or
`ERROR`, with any data on preceding `+VERB:` lines.

A reply of any length arrives whole, with its `OK`, while the host keeps
reading. If the host reads nothing for 500 ms, the rest of that reply is
dropped, and until the host reads again, output that does not fit in the
512-byte transmit buffer is dropped without waiting.

The port also carries log lines (INFO and above), the lines other tasks print
(`+MPING:` per reply, `+MCAST: rx`) and the echo of typed characters. None of
these waits: each goes in whole, or is dropped whole if the transmit buffer
lacks room for it or another line is being written — a reply line included,
which holds the port while it waits for room. They can fall between the lines
of a reply, never inside one, so match reply lines by their `+VERB:` prefix. A
log line longer than 255 bytes is cut to 255, ending in a newline.

The USB-Serial-JTAG console goes dark after early boot — the app hands the
shared USB PHY to USB-OTG for this console — and the mirrored log drops lines
under load, so the diagnostic counters below are the dependable view of the
receive path. That is why there are so many.

> **Three commands need a mesh build.** `AT+MESHSTAT?`, `AT+RXHEAD?` and
> `AT+FCRING?` report a per-frame capture that only `warthog-mesh-smoke` and
> the `warthog-mesh-sae*` envs built on it record (`WARTHOG_MESH_RX_TAP`); on
> other builds they answer
> `+ERR: built without WARTHOG_MESH_RX_TAP` rather than reporting an empty
> capture as though nothing were arriving. Everything else — `AT+DATASTAT?`,
> `AT+RXCHAN?`, `AT+FILTSTAT?`, `AT+RXREORD?`, `AT+HWMPSTAT?` — works on every
> build.

Commands are tagged:
**[op]** everyday operation · **[diag]** diagnosis · **[dev]** developer

---

## Identity and lifecycle

| Command | | Purpose |
|---|---|---|
| `AT` | [op] | Liveness. Answers `OK`. |
| `AT+VERSION?` | [op] | Firmware version, baked in from the release tag. |
| `AT+STATUS?` | [op] | Address on every surface. |
| `AT+RESET` | [op] | Reboot. |
| `AT+ERASE` | [op] | Wipe the `warthog` NVS namespace — **all** stored credentials and settings. |
| `AT+DLMODE` | [diag] | Drop into bootloader download mode over the wire, no BOOT button: the board re-enumerates as the ROM device (`303a:0009`). Flash it with `esptool --before no-reset --after watchdog-reset`, which then boots the new image ([Flashing](Flashing#reflashing-a-running-board)). Used by `tools/bench/flash.sh`. |

```
AT+STATUS?
+HALOW: ip=10.77.131.165 gw=10.77.131.165
+USB: ip=192.168.4.1 mounted=1
+AP: ip=192.168.5.1
OK
```

## Uplink and downstream configuration

Everything here persists in NVS and outranks the build-time default.

| Command | | Purpose |
|---|---|---|
| `AT+HALOW=<ssid>,<psk>` | [op] | HaLow station credentials. `AT+RESET` to apply. |
| `AT+HALOW?` | [op] | Current SSID; passphrase not echoed. |
| `AT+WIFIAP=<ssid>,<psk>,<chan>` | [op] | 2.4 GHz AP settings. |
| `AT+WIFIAP?` | [op] | Current AP SSID and channel; passphrase not echoed. |
| `AT+DNS=<ipv4>` | [op] | Resolver handed to USB and AP clients by DHCP. A lease the node itself takes (upstream, mesh or batman) never changes it; while the bridge runs (`AT+MESHBRIDGE=1`) the node serves no DHCP and hosts take the mesh DHCP server's resolver. Validated; malformed input is rejected without touching NVS. Persisted; `AT+RESET` applies it. |
| `AT+DNS?` | [op] | Current value. |
| `AT+MTU?` | [diag] | Interface MTU. |

## Mesh

| Command | | Purpose |
|---|---|---|
| `AT+MESHCFG?` | [op] | **Read this first when a node will not join.** Every value a peer matches on — region, country, mesh ID, applied channel/bandwidth/operating class, security, DHCP — plus peers and beacons heard, plus one line for the mode the mesh started in: `forwarding=yes(802.11s)\|no routing=hwmp\|none\|batman_v l2=bridge\|no(NAT)\|no(NAT;bridge-failed) multicast=all(bridged)\|meshtastic(239.0.0.69) batman=no\|yes(neigh=<n> routes=<n> soft=<mac>)\|refused(<reason>)`. `l2` and `multicast` follow the bridge actually running: with `AT+MESHBRIDGE=1` and a failed bridge start the node is NATed and says so. The config line carries `batman=<stored>`; while batman runs a last line `+MESHCFG: batman self=<mesh MAC> soft=<bat0 MAC> hard_mtu=1500 soft_mtu=1460 bcast=replicate\|std copies=<n> tput_override=<n>` follows ([Batman Mode](Batman-Mode)). Comparing these one verb at a time is how mismatches get missed. |
| `AT+MESHEN=<0\|1>` | [op] | Start the mesh instead of associating as a station. Persisted; takes effect on the next boot. Works on any build, including the region envs — this is how a stock image joins a mesh. |
| `AT+MESHCHAN=<chan>,<freq_hz>,<gclass>,<sclass>,<bw>` | [op] | Pin the S1G channel as a set — class and bandwidth belong to the channel, so they move together. Persisted; applied on next boot. `AT+MESHCHAN=default` clears it. |
| `AT+MESHCHAN?` | [op] | Stored set, **and whether it actually applied**. `applied=NO` means the regulatory table refused it and the radio is on its default — the usual cause of "peers with nothing, looks like range". On a region build the stored channel is matched against the country's regulatory row, so duty cycle, EIRP and airtime stay the regulator's; a channel that is not in the table is discarded rather than forced. |
| `AT+MESHFWD=<0\|1>` | [op] | 802.11s forwarding: relay other nodes' data and path selection through this node, and advertise the Forwarding capability so mac80211 peers will route through it. Default 0: a leaf, which relays nothing and starts no path discovery (on an open mesh its only PREQs are the per-peer keepalive), but learns hosts behind any mesh node from Address Extension and sends to such a host addressed to its node (AE mode 2), through that node if it is a peer, else through the peer that carried the host's traffic (not measured on air). A relay without a path to that node: vanilla mac80211 drops the reply with a PERR; a warthog relay, and OpenMANET's patched mac80211 with forwarding on (from its source), hold it and discover the node. Not measured. Under SAE a relay takes another Warthog's group PREQs and PERRs only on a swccmp build with host CCMP on (`-swccmp-on`, or `AT+SWCCMP=1` after each boot): on `warthog-mesh-sae`, `-nochipkey`, and swccmp with host CCMP off, relays exchange no group path selection with other Warthogs, even when all run the same image ([`AT+MESHPMF`](#mesh)). Persisted; next boot. `AT+MESHFWD=1` is refused while `AT+MESHBATMAN=1` is stored (`+ERR: AT+MESHBATMAN=1 is set; batman needs it off`), except on `warthog-mesh-sae` and `-nochipkey`, where a stored `AT+MESHBATMAN=1` is inert (`batman=refused(sae-no-host-ccmp)` in `AT+MESHCFG?`) and does not block it. **Compiled and simulated, not measured on air** — see [Mesh Mode](Mesh-Mode#forwarding). |
| `AT+MESHFWD?` | [op] | Current setting. |
| `AT+MESHPATH?` | [op] | The path table and proxy table: each destination with its next hop, HWMP sequence number, metric, hop count, active/dead and time to expiry (a path is dropped 600 s after its expiry); each host known to sit behind a mesh node (`host= behind=`), on a leaf also `relay=`, the peer its traffic arrived through, and `uni` when a unicast to this node pinned the entry against eviction (cleared if the host is next heard behind another node); live duplicate-cache entries, cache evictions, and frames waiting for discovery: our own (`pending`) and relayed ones (`relay_held`). A listing longer than the 4096-byte reply buffer ends with `+MESHPATH: (truncated)`. Empty on a leaf that has heard no path selection and no Address Extension frame. |
| `AT+MESHFWDSTAT?` | [op] | Relay counters: frames forwarded (unicast, group), allocation failures, and every drop by cause — own frame echoed, duplicate, TTL, no path, forwarding off, malformed, next hop's queue full, path table full (`tblfull`: path selection naming a destination we hold no path for (a PREQ's originator or a PREP's target, including the answer to our own discovery) while every path slot is live) — plus PERRs originated (for lost neighbours only), PREQs originated, PREQ/PREP/PERR relayed, our own frames held for discovery then sent or dropped (`pend`), relayed frames held while we discover their destination then sent or lost (`hold n`/`tx`/`drop`: given up after 6.8 s, evicted, refused while the transmit pool is paused, or the next hop's queue full), and path selection: `hwmp prot` unicast arrived protected (decrypted, replay-checked), `unprotected` refused as plaintext: unicast from a peer that protects (under SAE: runs MFP; on a relay also after it once protected unicast), and under SAE all group path selection in the clear, `unestab` refused under SAE, in every mode, because the sender's link is not established (a station the supplicant added before SAE and AMPE finished; mac80211 takes path selection only from an ESTAB peer), `gp` group-addressed taken under SAE because it arrived Protected — group-addressed privacy, CCMP under the sender's MGTK, which is how mac80211 sends every group PREQ and PERR on an SAE mesh — and was opened by host CCMP under the MGTK from that peer's AMPE (only swccmp builds with host CCMP on can) above that key's management replay counter, `mmie`/`nommie` group-addressed in the clear with or without an MMIE (under SAE in every mode, where both are refused from every peer: stricter than mac80211, which takes one without an MMIE from a peer that runs no MFP, since every established SAE peer protects its group path selection under its MGTK and a relay re-sends what it takes under ours; an MMIE is found at either length mac80211 finds one, 18 or 26 octets; else a census on a keyed relay only); `hwmp tx prot` unicast sent CCMP-protected, `tx gp` group path selection sent under SAE Protected under our own MGTK (the chip seals it, as our group data; no MMIE), `tx plain` group path selection sent under SAE in the clear because hostap had not yet delivered our MGTK or its install in the chip failed (retried with the next peer), which no updated Warthog takes and an MFP node drops, `tx qdrop` group path selection another task sent (the event loop builds and sends it all) dropped because it could not be queued to the loop (four already waiting, or the loop's event queue full), `tx qfail` queued group path selection the loop then could not build or send (no transmit buffer; not counted in `preq_tx`, which counts PREQs handed to the radio); `mgmt prot chip`/`host`/`nodec` protected management frames on an SAE mesh opened by the chip (unicast only: a group one the chip opens is counted `mgmt gp own` instead), by host CCMP, or by neither (dropped; a group one also counts in `mgmt gp nodec`), `grpkey` protected unicast ones refused because their key id is not the link's pairwise key; `mgmt gp nodec`/`own`/`key`/`replay` protected group-addressed ones (group path selection) on an SAE mesh, all dropped: opened by no key (every one on a build without host CCMP or with it off, because the chip holds only our own MGTK; `AT+RXCHAN?` reason 4 is the same for group data), opened by the chip, so under our own MGTK, which every peer holds (a forgery in the sender's name; reason 95 for data), opened by host CCMP under a key id that is not the sender's MGTK, or replayed on the sender MGTK's management counter; `mgmt tx chip`/`host`/`drop` robust unicast management frames (Block Ack: ADDBA request and response, DELBA) sent to a peer that runs MFP, protected by the chip, sealed with host CCMP, or dropped because they could not be sealed (also `tx_fail` in `AT+SWCCMP?`), never sent in the clear; `igtk` peer IGTKs installed. `hold drop` climbing is a node asking us to relay to a destination nobody answers for. |
| `AT+MESHGRP=<0\|1>` | [op] | How group frames leave the radio. `0` (default, measured): one unicast per peer; with `AT+MESHFWD=1` or `AT+MESHBRIDGE=1` the group address rides in Address Extension, and a leaf sends the copies without it. Works under SAE, but a mac80211 node that receives one delivers it locally and does not re-flood it, so a warthog's broadcasts stop at the first Linux relay; with Address Extension it also learns a proxy entry that is not one — the sending warthog as a host behind itself, or a relayed frame's originator as a host behind the relay — which costs that node its path state every time the path ages out. Both are reasons to prefer `1` wherever a Linux node is in the mesh. `1`: standard 3-address 802.11s broadcasts, what mac80211 sends and re-floods. Under SAE they are keyed with this node's own group key (MGTK), which every peer receives in AMPE; a warthog's chip holds only its own MGTK, so a warthog decrypts other nodes' group frames only with host CCMP (`AT+SWCCMP`), measured on air with Linux nodes' frames (2026-09-29); a receiver taking this node's `1` frames is not measured. This is the bench A/B, not a fix. In batman mode (`AT+MESHBATMAN=1`) `0` sends each batman broadcast as one copy per peer with the group address in Address Extension mode 2 (what batman-adv on a Linux peer takes as a broadcast, measured on air), once; `1` sends a standard group frame three times, each copy 5 ms or more on its millisecond clock (over 4 ms) after the previous batman group frame was handed to the radio, and takes at most 5 broadcasts at once, dropping the rest ([Batman Mode](Batman-Mode#broadcasts-and-link-throughput)). Persisted; next boot. |
| `AT+MESHGRP?` | [op] | Current setting. |
| `AT+MESHPMF=<0\|1>` | [op] | Management frame protection on the mesh. `0` (default): off in our own peering — measured working warthog-to-warthog, and for peering only against an OpenMANET peer, which reported `MFP: yes` for a link to a warthog that had it off. Unicast path selection still follows each peer: a keyed SAE peer that sent an IGTK in its AMPE (hostap does exactly when it runs `ieee80211w` 1 or 2; the OpenMANET wizard sets 2), or that sent us protected unicast path selection, gets its unicast PREQ/PREP/PERR CCMP-protected under the link key (the chip, or host CCMP on swccmp builds), and its own unprotected unicast path selection is refused. Group path selection does not depend on this setting, as in mac80211: under SAE ours (a relay's broadcast PREQs and PERRs, a bridge's broadcast PREQs) always goes out CCMP-protected under our own MGTK (group-addressed privacy), and a peer's is taken Protected under that peer's MGTK — only on swccmp builds with host CCMP on, since the chip holds no peer's MGTK — never in the clear from any established peer, MFP or not (stricter than mac80211, which takes it in the clear from a peer without MFP), and never with a BIP MMIE, which mac80211 drops from a peer that runs MFP. So on `warthog-mesh-sae`, `-nochipkey`, and swccmp with host CCMP off, no Warthog takes another Warthog's group path selection at either setting, where before this change it was taken in the clear (`0`) or with a BIP MMIE (`1`); updating every Warthog does not change that. `1`: MFP required — our RSN element asks for it, hostap generates our IGTK and sends it in AMPE (kept host-side; nothing of ours carries an MMIE), and every keyed peer is treated as MFP. Neither setting is needed by a leaf, relay or bridge against an OpenMANET node. Only these two values exist here: "optional" is the setting where the two ends size the AMPE payload differently and a peer slices the frame short, taking the MIC and Peer Management elements with it, so it is deliberately unreachable. Read once while the mesh config is built, so unlike `AT+MESHSEC=` it cannot be flipped under a live mesh. Persisted; next boot. `0` measured on air on 2026-09-30 against OpenMANET 1.8.0 nodes with `ieee80211w=2` (`warthog-mesh-sae-swccmp`, batman mode): the nodes' protected group PREQs taken (`hwmp gp`) and answered with protected unicast PREPs, so the nodes held an `ACTIVE` path to the Warthog. Not measured: `1`, and the group path selection a relay or bridge sends under our MGTK. |
| `AT+MESHPMF?` | [op] | Current setting. |
| `AT+MESHBRIDGE=<0\|1>` | [op] | L2 bridge mode: USB, the Wi-Fi AP and the mesh become ports of one lwIP bridge, so tethered hosts sit on the mesh segment with their own MACs and take addresses from the mesh's DHCP server — the fix for CoT and mDNS, whose payload addresses alias under NAT, against a peer whose mesh interface is a bridge port. It does not reach a node set up by OpenMANET's mesh wizard, whose DHCP server and applications sit on `br-ahwlan` behind `bat0`; that takes `AT+MESHBATMAN=1`. NAT and the multicast repeater are off in this mode. Under SAE a bridge takes another Warthog's group PREQs only on a swccmp build with host CCMP on, as a relay does (`AT+MESHFWD`). Default 0 — NAT is the proven path. Persisted; next boot. `AT+MESHBRIDGE=1` is refused while `AT+MESHBATMAN=1` is stored, except on `warthog-mesh-sae` and `-nochipkey`, where a stored `AT+MESHBATMAN=1` is inert (`batman=refused(sae-no-host-ccmp)`). **Compiled, not measured on air**; see [Mesh Mode](Mesh-Mode#bridge-mode). |
| `AT+MESHBRIDGE?` | [op] | Current setting. |
| `AT+MESHDHCP=<0\|1>` | [op] | Take a DHCP lease on the mesh if one is offered (default 1), else go straight to the static `10.77.x.y`. A peer that keeps its mesh interface bridged runs a DHCP server on that bridge. In batman mode the lease is tried for 45 s from the first batman route, then an ARP-probed static `10.41.253.x/16` with gateway `10.41.0.1` is taken and DHCP asked again beside it, 30 s later and then at doubling intervals up to 10 min; a lease whose router stops resolving for 60 s is asked again beside its address, the wait doubling at each further restart up to 10 min until a router check passes or a new gateway or first route appears. `0` goes straight to the static address and never asks ([Batman Mode](Batman-Mode#what-changes-on-the-warthog)). |
| `AT+MESHEN?` | [op] | Whether mesh mode is on. The capability envs always report 1. |
| `AT+MESHID=<id>` | [op] | Mesh ID, 1–32 chars. Persisted; next boot. Must match every peer exactly — a mismatch peers with nothing and reads as a radio fault. |
| `AT+MESHID?` | [op] | Current mesh ID. |
| `AT+MESHPASS=<pass>` | [op] | SAE passphrase, 1–63 chars. Persisted; next boot. Must match every peer. |
| `AT+MESHPASS?` | [op] | Length only, never the value — this console mirrors the logs. |
| `AT+MPMPEERS?` | [op] | Peer links and handshake state. The first thing to read on a mesh. `ampe_mtk`/`ampe_mgtk`: AMPE keys installed (our own MGTK counts once, at its first install). `mgtk_reinst`: re-installs of our own MGTK at a fresh TX PN base, made when an AMPE Open follows group frames sent under it or still queued in the chip at its last install, so the Key RSC in that Open lies above every PN already used. `mgtk_rsc_fail`: such re-installs the chip refused; that Open advertises one below the previous base. Both stay 0 except on `warthog-mesh-sae-swccmp` and `-swccmp-on`, the only builds with `WARTHOG_MESH_MGTK_PN_BASE`. On those two a relay's or bridge's group PREQs and PERRs go out under our MGTK too, so `mgtk_reinst` climbs with AMPE Opens at the default `AT+MESHGRP=0`. The nonzero base and each re-install assume the chip starts the key at the TX PN `INSTALL_KEY` gives it, which is not measured: if it does not, every receiver drops our group frames as replays and each re-install reuses PNs under the same key. Every other build installs our MGTK at PN 0 and advertises RSC 0, so a peer that joins or re-peers takes 0 as its replay floor for our MGTK and each of our earlier group frames under it (`AT+MESHGRP=1` data, a relay's or bridge's group PREQs and PERRs) can be replayed into it once, until the MGTK changes. |
| `AT+MESHRSSI=<dBm>` | [op] | Candidate RSSI floor, `-255`..`0`. A neighbour heard at or below it is not offered to the SAE supplicant, and under SAE its Mesh Peering Open is refused unless it already holds a slot (the supplicant would otherwise take it on the key cached from an earlier SAE); on an open mesh no peering is started toward it from its beacons; its S1G beacons are answered at most once per 10 s. On an open mesh its probe requests still draw an Open, its own Open is still answered, and a peering already under way continues, so a link below the floor forms only when the other side initiates; two Warthogs, which probe every 2 s, still peer. Under SAE it forms only with the floor lowered on both ends, except toward OpenMANET, whose threshold does not apply to an Open: while both ends still cache the key of an earlier SAE (up to 12 h, until either reboots), Warthog's PMKSA-cached Open, sent only toward a node Warthog hears above its own floor, re-peers a link OpenMANET hears below its threshold. `0` or `-255` turns it off. Default `-80`, the `mesh_rssi_threshold` a fresh OpenMANET 1.8.0 node applies to Warthog's beacons and probe responses at its own receiver, so this approximates that gate from Warthog's side. Persisted; applies at once. |
| `AT+MESHRSSI?` | [op] | First line: the floor (`off` when disabled), `skipped` (new peerings it refused) and `passed` (ones it let through), counting only frames that name our mesh, since boot. Then per-neighbour signal: last, min, max, noise, SNR, the bandwidth that neighbour transmitted at, and a frame count. Min/max matter more than last — a link that averages fine but dips to −90 is the one that drops under load. A `bw=` that differs from your own is a configuration mismatch, not a weak link. **No MCS:** this driver's RX metadata (`struct mmdrv_rx_metadata`) carries RSSI, noise, frequency and bandwidth and no rate, so per-peer MCS is not reportable. |
| `AT+MESHSEC=<0\|1>` | [op] | Data plane open (0) or keyed (1). Re-peers within ~2 s. Persisted in NVS. |
| `AT+MESHSEC?` | [op] | Current setting. No effect on the SAE build (keys come from AMPE). |
| `AT+SAERX?` | [diag] | SAE/AMPE conversation state on the encrypted build: auth frames in/out, SAE FSM state, peering FSM, `ESTAB` count, which peer is being offered. |
| `AT+SAESTAGE?` | [diag] | Last step the SAE path reached, stored in RTC — survives a panic reboot. `AT+SAESTAGE=0` clears it. |
| `AT+SAEBRIDGE=<0\|1\|2>` | [diag] | Gate on offering discovered peers to the SAE supplicant. Defaults 1; `0` makes the node deaf to candidates (a debugging state); `2` also offers peers whose Mesh Configuration advertises a different auth protocol, for an SAE peer that advertises 0: OpenMANET probe responses were seen doing so on the bench, though its source advertises 1 under SAE and the verified OpenMANET run peered without it. Not persisted; 1 again after a reboot. |
| `AT+COREDUMP?` | [diag] | Task, PC and backtrace of the last panic, read from the flash core-dump partition. Feed the addresses to `addr2line`. |
| `AT+MPING=<ipv4>[,<count>]` | [op] | ICMP echo from the node itself. Count 1–20, default 4; out-of-range is silently clamped to 4, not rejected. Blocks the AT console for the duration. |
| `AT+PEERS?` | [diag] | Datapath peer count and registration failures. |
| `AT+HWMPSTAT?` | [diag] | Path-selection counters. `rann_rx`/`perr_rx`: root announcements (never acted on) and path errors received; neither is a parse failure. |
| `AT+HWMPDUMP?` | [dev] | Hex of the last path-selection frame received. |
| `AT+MPMSTAT?` | [diag] | Peering frame counters and last close reason. |
| `AT+MPMDUMP?` | [dev] | Hex of the last peering frame body. |
| `AT+MESHSTAT?` | [diag] | Mesh receive totals and last frame control. |
| `AT+BCNSTAT?` | [diag] | Beacon transmit/receive counters. |
| `AT+PRSPSTAT?` | [diag] | Probe response counters. |

```
AT+MPMPEERS?
+MPMPEERS: self=4c83a5 4dc7f8 llid=44921 plid=26523 estab=1 opens=0;
                    28bf74 llid=34244 plid=50177 estab=1 opens=0; ...
OK

AT+HWMPSTAT?
+HWMPSTAT: rx=234 preq_rx=75 preq_tx=142 prep_rx=159 prep_tx=75 parse_fail=0 not_ours=0 rann_rx=0 perr_rx=0
OK
```

Reading `AT+MPMPEERS?`: `estab=1` with a non-zero `plid` is a complete two-way
handshake. `plid=0` with `opens` climbing means the node is sending Opens nobody
answers — Warthog sends a Close and restarts after 8. `offer_full` counts SAE
candidates not offered to the supplicant because all 4 peer slots were taken.
`sae_fail` counts SAE handshakes that timed out and `plink_fail` peerings that
failed after SAE completed, each freeing its slot; `held` counts offers and
Mesh Peering Opens refused while such a neighbour is held off (see
[Peer capacity](Mesh-Mode#peer-capacity)).

## Batman mode

BATMAN_V member mode, see [Batman Mode](Batman-Mode). Measured on air on
2026-09-29/30 (`warthog-mesh-sae-swccmp`) against two OpenMANET 1.8.0 Pis running
batman-adv 2025.4, one hop apart: `AT+MESHBATMAN?` reached `addr=leased` with
`router=...(ok)` and a `gw=`, and the tables and counters below were read on
the boards ([Batman Mode](Batman-Mode#measured-on-air)).
Host-tested, and the engine VM-tested against batman-adv 2024.3.

| Command | | Purpose |
|---|---|---|
| `AT+MESHBATMAN=<0\|1>` | [op] | Run as a BATMAN_V member. Persisted (`mesh_bat`); next boot. `1` is refused with `AT+MESHFWD=1` or `AT+MESHBRIDGE=1` stored, and on `warthog-mesh-sae` / `-nochipkey`, which cannot hear peers' group frames under SAE (`+ERR: this build cannot hear peers' group frames under SAE; use warthog-mesh-sae-swccmp or an open mesh`). `0` is always accepted. A `1` stored from another image survives a reflash to `warthog-mesh-sae` / `-nochipkey`; there it is inert (`reason=sae-no-host-ccmp`) and does not block `AT+MESHFWD=1` or `AT+MESHBRIDGE=1`, and back on a build batman runs on with either of those stored, batman is refused at boot (`reason=fwd`, else `bridge`). |
| `AT+MESHBATMAN?` | [op] | `+MESHBATMAN: stored=<0\|1> running=<0\|1> reason=<r>`: `ok`, `off`, `mesh-off` (mesh mode off, or the mesh ID or passphrase was rejected before the mesh started), `fwd`, `bridge`, `sae-no-host-ccmp`, `nomem`, `init-failed`, `mesh-failed` (`mmwlan_mesh_enable()` failed after batman had started; the engine idles and `running=0`) — why it is not running this boot. While batman runs a second line follows, as of the last 2 s check: `+MESHBATMAN: bat0 addr=<a> ip=<ip> router=<ip>[(<r>)] retry_in=<n>s\|- retries=<n> restarts=<n> gw=<orig>(<down>/<up>)\|none`. `addr`: `idle` (no route yet), `dhcp` (the first attempt, or lwIP asking again after a DHCP server NAKed its lease renewal or the lease expired; bat0 has no address meanwhile), `leased`, `probing` (ARP-probing a static candidate), `static` (a probed candidate), `held` (a former lease whose router went), `retry` (DHCP asked again beside the held address). `router` is bat0's gateway address; while leased `<r>` is `ok`, `none` (the lease has no router), `lost <n>s` or `unknown <n>s` (its MAC not learned yet), with the seconds it has failed the check (60 s restarts DHCP; each further restart before a check passes waits twice as long, at most 600 s). While the router's MAC is not learned yet, or the translation table does not resolve it to an originator with a route, the Warthog broadcasts an ARP request for it at the next check and every 10 s after: a router that sent nothing into batman for 600 s has dropped out of its node's translation table, and its reply puts it back. `retry_in`: seconds to the next attempt while `static` or `held` (`0s`: due, waiting for a route), to the next DHCP restart while `leased` with the router `lost` or `unknown`, else `-`. `retries`: attempts beside a held address; `restarts`: router losses. `gw`: the engine's best gateway (originator, announced down/up in Mbit/s). The retry, restart and static paths are host- and VM-tested only ([Batman Mode](Batman-Mode#what-changes-on-the-warthog)). |
| `AT+MESHBATTP=<n>` | [op] | Link throughput for every neighbour in 100 kbit/s units, `0`..`4294967295`; `0` (default) uses rate control's estimate. Persisted (`mesh_battp`); next boot. |
| `AT+MESHBATTP?` | [op] | Current value; `0 (auto)`. |
| `AT+BATN?` | [op] | Neighbours: `count=<n>/8`, then per neighbour its hard (link) address, `orig=` its originator (what `AT+BATO=<mac>` takes), `seen=` age (ms), `tput=` throughput (Mbit/s), `interval=` its ELP interval (ms) and `cands=` how many originators list it as a candidate next hop. |
| `AT+BATO?` | [op] | Our originator, bat0 MAC, sequence numbers and TTVN; then per originator its age, next hop, throughput (own and forwarded), TTVN, translation-table rows and gateway announcement, and each candidate next hop (`*` default router, `J` the router for relayed traffic). |
| `AT+BATO=<mac>` | [op] | The same summary line, then only that originator and its candidates; nothing more if it is not in the table. A node's originator is the `orig=` of its `AT+BATN?` line, or on the node the MainIF/MAC in the header of `batctl meshif bat0 o`: the MAC of its first batman interface to become active (seen in the VM against batman-adv 2024.3; which comes up first on OpenMANET hardware is not measured). On a node with a second batman interface (OpenMANET's 2.4 GHz `batmesh1`, which openmanetd sets up on a node with an MT7915 or MT7916 2.4 GHz radio) that can be the other interface's MAC, not the HaLow mesh MAC the Warthog peers with (the first field of the `AT+BATN?` line), and `AT+BATO=<HaLow MAC>` then prints only the summary line. |
| `AT+BATTG?` | [op] | Global translation table: rows used, then per originator its CRC lines (one per VLAN) followed by its rows: each client MAC with VLAN (`-1` untagged), originator, TTVN and flags (`W` wireless, `I` isolated, `T` temporary). |
| `AT+BATTG=<mac>` | [op] | The rows line, then for each originator that is `<mac>` or announces a client `<mac>`: its CRC lines, then its rows (all of them when it is the originator, else the rows for that client). What the bat0 router check sees: `AT+BATTG=<router's MAC>` names the originator the router's MAC resolves to (no row: it does not resolve), and `AT+BATO=<that originator>` its route and age. |
| `AT+BATTL?` | [op] | Local translation table: our TTVN, queued changes and re-sends, and our own entries (only the bat0 MAC). |
| `AT+BATSTAT?` | [op] | Table use, then every engine counter by group (`rx`, `elp`, `ogm`, `bc`, `uc`, `ut`, `fr`, `ic`, `st`, `lk`; the translation-table counters `tt_*` are on the `ut` line; with `AT+MESHGRP=1`, `bc_copy_drop` counts broadcast copies given up for a newer broadcast and `bc_queue_full` broadcasts dropped because 4 were waiting unsent; `tt_req_stall` counts full-table answers that could not be taken, too big or not fitting the table, each doubling the wait before that node is asked again, up to a minute; an oversized answer a relay sends and cuts on a node's behalf counts against that node), then a `port` line: RX/TX slot pressure (`q_rx_full`, `q_tx_full`); frames the receive hook dropped (`rx_short`, `rx_toobig`, `rx_nonbat`, `rx_probe` Linux ELP probes, `rx_relayed` batman frames another 802.11s node relayed); transmit results (`tx_busy` radio not ready within 50 ms, `tx_nomem`, `tx_notfound` no established peer for the next hop, `tx_fail`); `deliver_nomem` (no heap to hand a frame to lwIP), `deliver_cap` (frames dropped because lwIP already holds 16 received frames), `soft_toobig` (a frame from lwIP above 1536 bytes); free slots (`rx_slots`, `tx_slots`); the engine task's free stack (`stack_free`); link throughput (`tput_cache_age`, ms since the last peer-table snapshot, `-1` none yet; `tput_snap_fail`, snapshots that could not be taken); and the heap (`heap_free`, `heap_min`, `heap_largest`). |

The render queries answer `+ERR: batman not running (<reason>)` when batman is not
running, and `+ERR: batman engine busy` if the engine does not answer (2 s per wait;
up to about 4 s right after an abandoned query). `<mac>` is exactly
`xx:xx:xx:xx:xx:xx`, hex in either case; anything else answers
`+ERR: usage: AT+BATO=<mac>` (or `AT+BATTG=<mac>`). Every listing is sent whole, in
chunks of up to 4096 bytes, one round trip to the engine each, with one `OK` at the
end; totals come first, and the `AT+BATSTAT?` port line follows the engine's
counters. A table that changes between chunks can miss an entry or show one twice.
A busy or not-running error between chunks ends the reply after the lines already
sent, `+ERR: batman render stalled` means the engine did not move on to the next
chunk, and `(truncated)` now only ends a listing at an entry longer than a chunk.
Host-tested only (the port and AT layer on stubs and on the real engine).

## Datapath diagnostics

These exist to answer "the link is up, so where are the frames going".

| Command | | Purpose |
|---|---|---|
| `AT+DATASTAT?` | [diag] | Receive and transmit totals through the datapath. |
| `AT+RXCHAN?` | [diag] | What the chip pushed to the host, by frame class, plus drop reason. |
| `AT+FILTSTAT?` | [diag] | Two lines. First, which of the receive filter's nine drop paths is firing. `not_ours` (reason 9, mesh only): unicast data whose receiver address (addr1) is another station, which the MM6108 hands up (it delivers data it overhears between other nodes); dropped before any decryption, so host CCMP never tries it. A frame relayed through this node carries this node's address there, so relaying is unaffected; group-addressed frames and management frames are not judged. Second, `+FILTSTAT: mgmt_nours=<n> last=<32 hex>` (mesh only): unicast management frames other than beacons whose addr1 is another station, counted but not dropped, and the first 16 octets of the last one (frame control, duration, addr1, addr2). Whether the MM6108 hands such frames up is not measured: two neighbours exchanging unicast also send each other unicast path replies (PREP) as their path refreshes, so 0 after minutes of such traffic means it does not. A Protected one from a peer still reaches host CCMP where it is on, which counts it in `tried` and `micfail` (`AT+SWCCMP?`); an unprotected Block Ack request or teardown still acts on this node's Block Ack state with its sender. |
| `AT+RXREORD?` | [diag] | Block-ack reorder accounting: frames dropped as outdated or parked. |
| `AT+RXHEAD?` | [dev] | Hex of the head of the last data frame — MAC header, QoS, mesh control, LLC. |
| `AT+FCRING?` | [dev] | Frame control of the last 32 frames the chip delivered. |

```
AT+FILTSTAT?
+FILTSTAT: drop=23 last=6 | short_fc=0 rts=0 beacon=0 short_hdr=0 no_ops=0
           unknown_sender=23 sa_is_us=0 dup=0 not_ours=0
+FILTSTAT: mgmt_nours=0 last=00000000000000000000000000000000
OK
```

`delivered=` in `AT+DATASTAT?` counts every data frame handed to the network
stack, on every delivery path. (Older builds counted it on only one path, one
this build never takes, so it read 0 on a healthy link; that is fixed.) It is
the positive control for any receive experiment: if it is not climbing, the
link is not carrying data and no other counter means anything.

`AT+RXCHAN?` reports `rxdrop=<count> reason=<last>`. The count covers every
frame the receive path finished with other than an ordinary delivery, so it is
not a loss counter by itself; `reason` is the most recent cause:

| Reason | Meaning |
|---|---|
| 3 | Plaintext data on a keyed link (only EAPOL may arrive unprotected) |
| 4 | Protected, but the chip did not decrypt it (no key, or wrong key) |
| 5 | CCMP header unreadable, or a replayed packet number (the replay check) |
| 6 | Too short to hold the CCMP MIC |
| 7 / 8 | Group frame with the fragment bit set / our own broadcast relayed back |
| 9 | 4-address EAPOL addressed to us (unsupported) |
| 10 | EAPOL consumed locally — **not a loss** |
| 11 / 12 | Controlled port closed / no LLC ethertype |
| 13 / 15 | No receiving interface / no network-stack callback registered |
| 14 | Delivered via the legacy callback — **not a loss**; current builds deliver without touching `rxdrop` |
| 90 / 91 / 92 | Mesh Control truncated / could not be stripped / Address Extension truncated |
| 93 | Leaf: a mesh frame for another node, dropped because forwarding and bridge are both off |
| 94 | A 4-address frame for another node **without** Mesh Control — never relayable, dropped in every mode |
| 95 | SAE: a group frame the chip decrypted. Its one group key is this node's own TX MGTK, which no peer sends under, so the frame is forged in the transmitter's name |
| 96 | SAE: a unicast frame the chip decrypted under a key id other than the link's pairwise key. The key it was opened with (this node's own MGTK, the chip's one group key) is held by every peer, so any of them could have sent it in the transmitter's name. On the `-nochipkey` and `-swccmp` builds, whose chip holds no pairwise key, every unicast the chip decrypted |
| 99 | Forwarded to the next hop — **not a loss** |
| 100 + N | Forwarding engine: 101 own frame echoed, 102 duplicate, 103 not for us, 104 would forward but `AT+MESHFWD=0`, 105 TTL, 106 no path: held while we discover its destination, see `AT+MESHFWDSTAT?` `hold` — not a loss by itself, unless its mesh destination is a group address, which is dropped (`nopath`), 107 bad Address Extension, 108 arrived with TTL 0 |

93 and 104 are configuration, not failure: they mean the node is a leaf, or
bridge-only, by setting.

## Crypto

| Command | | Purpose |
|---|---|---|
| `AT+CCMPKAT?` | [dev] | Result of the AES-CCM known-answer test run at startup. |
| `AT+CRYPTOHOST=<0\|1>` | [dev] | Move CCMP between chip and host. |
| `AT+CRYPTOHOST?` | [dev] | Current setting, read back from the chip. |
| `AT+SWCCMP=<0\|1>` | [dev] | Turn host software CCMP off or on; not persisted. Acts only on the builds compiled with it, `warthog-mesh-sae-swccmp` and `-swccmp-on` (armed at boot, and by batman mode). `AT+SWCCMP=0` is refused while batman runs (`+ERR: batman is running; host CCMP must stay on`): it would end all group RX from peers. |
| `AT+SWCCMP?` | [dev] | Two lines. First, host CCMP counters: `on`, `tried`, `ok` (decrypted and verified), `micfail`, `tx_ok`/`tx_fail`, `nokey` (no key for the frame's key id), `grpkey` (a unicast keyed with a group key, refused), `badhdr`, `short`, and the last `keyid` and AAD. Second, the last MIC failure: `+SWCCMP: fail len=<n> keyid=<k> pn=<12 hex> hdr=<64 hex>`, where `len` counts the CCMP header, ciphertext and MIC, `pn` is least significant octet first (PN 6 reads `060000000000`), and `hdr` is the first 32 octets of the frame from its frame control (addr1 at octet 4, addr2 at 10); all zero until a frame fails. Unicast data addressed to another station is dropped before host CCMP (`not_ours` in `AT+FILTSTAT?`), so it counts in neither `tried` nor `micfail`; on 2026-09-30, before that filter, the fail line held such a frame (addr1 a Linux node, addr2 another Warthog). A Protected unicast management frame addressed to another station is not dropped: if the chip hands those up (`mgmt_nours` in `AT+FILTSTAT?`), each counts in `tried` and `micfail` and replaces the fail line, whose addr1 then names that station. |
| `AT+KEYFP?` | [dev] | Fingerprint of the installed mesh keys. |
| `AT+KEYINST?` | [dev] | Key installation results per peer. |
| `AT+REKEY=<n>` | [dev] | Re-push peer slot *n*'s own pairwise key to the chip: its AMPE MTK under SAE, the built-in constant on a keyed non-SAE mesh. Nothing is installed on an open mesh, for a candidate AMPE has not keyed, or on the `-nochipkey`/`-swccmp` builds, which keep AMPE keys out of the chip. |
| `AT+REKEYSTAT?` | [dev] | Result of the last rekey: `done` counts keys actually installed; `aid` is the peer's AID, or 4294967295 when nothing went in. |

## Traffic generation

Bench tools. They put frames on air — do not leave them running on a live
deployment.

| Command | | Purpose |
|---|---|---|
| `AT+MSEND=<text>` | [dev] | Send `<text>` as one UDP datagram to 239.0.0.69:4403 on the mesh netif, from the `AT+MCAST` socket. Needs `AT+MCAST=1`. |
| `AT+MTPUT=` | [dev] | Throughput generator. |
| `AT+MINJECT=<hex>` | [dev] | Hand up to 300 bytes to the multicast repeater as a datagram arriving from the Wi-Fi AP — what a Meshtastic node on the softAP sends — and send it to 239.0.0.69:4403 on every other netif that is up. The repeater is off in bridge mode, where this reports `-1 netif(s)`. |
| `AT+MCAST=` / `?` | [dev] | `1` joins 239.0.0.69:4403 on the mesh netif with a test socket and counts what arrives; `0` closes it. `?` reports the counts and the last sender. |
| `AT+MUDP?` | [dev] | Counters of the resident Meshtastic multicast repeater: its address on USB, AP and HaLow, datagrams received and re-sent per netif, `drop_self` (sent from one of our own addresses), `drop_unknown` (arrived on a netif the repeater has not joined yet), `tx_err`. |
| `AT+MUDPLAST?` | [dev] | Last datagram the repeater received from the mesh: source address and the first 256 bytes in hex. |

---

## Worked example: bringing up a mesh link

```
AT+MESHEN=1                      mesh instead of a station uplink
AT+MESHID=halowmesh              must match the peer exactly
AT+MESHPASS=<passphrase>         SAE builds only; must match the peer
AT+RESET                         the three above take effect on boot
AT+MESHSEC=0                     match an unencrypted peer
AT+MPMPEERS?                     confirm estab=1 and a non-zero plid
AT+HWMPSTAT?                     confirm preq_tx climbing (open mesh), parse_fail=0
AT+MPING=10.77.191.116,8         confirm data
```

If `AT+MPMPEERS?` shows `estab=1` but `AT+MPING` fails, the peer has no *path* to us —
see [Mesh Mode](Mesh-Mode#how-paths-work-and-why-it-matters).
