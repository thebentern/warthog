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
| `AT+DLMODE` | [diag] | Drop into bootloader download mode over the wire, no BOOT button. Used by `tools/bench/flash.sh`. |

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
| `AT+DNS=<ipv4>` | [op] | Resolver handed to USB and AP clients by DHCP. Validated; malformed input is rejected without touching NVS. |
| `AT+DNS?` | [op] | Current value. |
| `AT+MTU?` | [diag] | Interface MTU. |

## Mesh

| Command | | Purpose |
|---|---|---|
| `AT+MESHCFG?` | [op] | **Read this first when a node will not join.** Every value a peer matches on — region, country, mesh ID, applied channel/bandwidth/operating class, security, DHCP — plus peers and beacons heard, plus one line for the mode the mesh started in: `forwarding=yes(802.11s)\|no routing=hwmp\|none l2=bridge\|no(NAT)\|no(NAT;bridge-failed) multicast=all(bridged)\|meshtastic(239.0.0.69) batman=no`. `l2` and `multicast` follow the bridge actually running: with `AT+MESHBRIDGE=1` and a failed bridge start the node is NATed and says so. Comparing these one verb at a time is how mismatches get missed. |
| `AT+MESHEN=<0\|1>` | [op] | Start the mesh instead of associating as a station. Persisted; takes effect on the next boot. Works on any build, including the region envs — this is how a stock image joins a mesh. |
| `AT+MESHCHAN=<chan>,<freq_hz>,<gclass>,<sclass>,<bw>` | [op] | Pin the S1G channel as a set — class and bandwidth belong to the channel, so they move together. Persisted; applied on next boot. `AT+MESHCHAN=default` clears it. |
| `AT+MESHCHAN?` | [op] | Stored set, **and whether it actually applied**. `applied=NO` means the regulatory table refused it and the radio is on its default — the usual cause of "peers with nothing, looks like range". On a region build the stored channel is matched against the country's regulatory row, so duty cycle, EIRP and airtime stay the regulator's; a channel that is not in the table is discarded rather than forced. |
| `AT+MESHFWD=<0\|1>` | [op] | 802.11s forwarding: relay other nodes' data and path selection through this node, and advertise the Forwarding capability so mac80211 peers will route through it. Default 0: a leaf, which relays nothing and starts no path discovery (on an open mesh its only PREQs are the per-peer keepalive), but learns hosts behind any mesh node from Address Extension and sends to such a host addressed to its node (AE mode 2), through that node if it is a peer, else through the peer that carried the host's traffic (not measured on air). A relay without a path to that node: vanilla mac80211 drops the reply with a PERR; a warthog relay, and OpenMANET's patched mac80211 with forwarding on (from its source), hold it and discover the node. Not measured. Persisted; next boot. **Compiled and simulated, not measured on air** — see [Mesh Mode](Mesh-Mode#forwarding). |
| `AT+MESHFWD?` | [op] | Current setting. |
| `AT+MESHPATH?` | [op] | The path table and proxy table: each destination with its next hop, HWMP sequence number, metric, hop count, active/dead and time to expiry (a path is dropped 600 s after its expiry); each host known to sit behind a mesh node (`host= behind=`), on a leaf also `relay=`, the peer its traffic arrived through, and `uni` when a unicast to this node pinned the entry against eviction (cleared if the host is next heard behind another node); live duplicate-cache entries, cache evictions, and frames waiting for discovery: our own (`pending`) and relayed ones (`relay_held`). A listing longer than the 4096-byte reply buffer ends with `+MESHPATH: (truncated)`. Empty on a leaf that has heard no path selection and no Address Extension frame. |
| `AT+MESHFWDSTAT?` | [op] | Relay counters: frames forwarded (unicast, group), allocation failures, and every drop by cause — own frame echoed, duplicate, TTL, no path, forwarding off, malformed, next hop's queue full, path table full (`tblfull`: path selection naming a destination we hold no path for (a PREQ's originator or a PREP's target, including the answer to our own discovery) while every path slot is live) — plus PERRs originated (for lost neighbours only), PREQs originated, PREQ/PREP/PERR relayed, our own frames held for discovery then sent or dropped (`pend`), relayed frames held while we discover their destination then sent or lost (`hold n`/`tx`/`drop`: given up after 6.8 s, evicted, refused while the transmit pool is paused, or the next hop's queue full), and path selection: `hwmp prot` arrived protected (decrypted, replay-checked), `unprotected` refused as plaintext from a peer that protects (under SAE: runs MFP; on a relay also after it once protected), `unestab` refused under SAE, in every mode, because the sender's link is not established (a station the supplicant added before SAE and AMPE finished; mac80211 takes path selection only from an ESTAB peer), `mmie`/`nommie` group-addressed with or without an MMIE (under SAE in every mode, else on a keyed relay only), `bipfail` an MMIE refused (no IGTK for its key id, bad MIC, replayed IPN) or a Protected group frame; `hwmp tx prot` sent CCMP-protected, `tx mmie`/`tx nommie` group path selection sent under SAE with and without an MMIE (without: we hold no IGTK, `AT+MESHPMF=0`), `tx qdrop` group path selection another task sent (the event loop builds and sends it all) dropped because it could not be queued to the loop (four already waiting, or the loop's event queue full), `tx qfail` queued group path selection the loop then could not build or send (no transmit buffer; not counted in `preq_tx`, which counts PREQs handed to the radio); `mgmt prot chip`/`host`/`nodec` protected management frames on an SAE mesh opened by the chip, by host CCMP, or by neither (dropped), `grpkey` protected unicast ones refused because their key id is not the link's pairwise key; `igtk` peer IGTKs installed. `hold drop` climbing is a node asking us to relay to a destination nobody answers for. |
| `AT+MESHGRP=<0\|1>` | [op] | How group frames leave the radio. `0` (default, measured): one unicast per peer; with `AT+MESHFWD=1` or `AT+MESHBRIDGE=1` the group address rides in Address Extension, and a leaf sends the copies without it. Works under SAE, but a mac80211 node that receives one delivers it locally and does not re-flood it, so a warthog's broadcasts stop at the first Linux relay; with Address Extension it also learns a proxy entry that is not one — the sending warthog as a host behind itself, or a relayed frame's originator as a host behind the relay — which costs that node its path state every time the path ages out. Both are reasons to prefer `1` wherever a Linux node is in the mesh. `1`: standard 3-address 802.11s broadcasts, what mac80211 sends and re-floods. Under SAE they are keyed with this node's own group key (MGTK), which every peer receives in AMPE; a warthog's chip holds only its own MGTK, so a warthog decrypts other nodes' group frames only with host CCMP (`AT+SWCCMP`). Neither is measured on air. This is the bench A/B, not a fix. Persisted; next boot. |
| `AT+MESHGRP?` | [op] | Current setting. |
| `AT+MESHPMF=<0\|1>` | [op] | Management frame protection on the mesh. `0` (default): off in our own peering — measured working warthog-to-warthog, and for peering only against an OpenMANET peer, which reported `MFP: yes` for a link to a warthog that had it off. Path selection still follows each peer: a keyed SAE peer that sent an IGTK in its AMPE (hostap does exactly when it runs `ieee80211w` 1 or 2; the OpenMANET wizard sets 2), or that sent us protected path selection, gets its unicast PREQ/PREP/PERR CCMP-protected under the link key (the chip, or host CCMP on swccmp builds), and its own unprotected unicast path selection, or group path selection without a valid MMIE, is refused. `1`: MFP required — our RSN element asks for it, hostap generates our IGTK and sends it in AMPE, the IGTK is kept host-side, every keyed peer is treated as MFP, and our group path selection (a relay's broadcast PREQs and PERRs, a bridge's broadcast PREQs) carries a BIP-CMAC-128 MMIE. A relay or bridge needs `1` against an MFP peer, which drops group path selection without an MMIE; a leaf does not. A peer running MFP off drops group frames carrying that MMIE unless it parsed our IGTK. Only these two values exist here: "optional" is the setting where the two ends size the AMPE payload differently and a peer slices the frame short, taking the MIC and Peer Management elements with it, so it is deliberately unreachable. Read once while the mesh config is built, so unlike `AT+MESHSEC=` it cannot be flipped under a live mesh. Persisted; next boot. **Not measured on air**, neither setting's path selection protection. |
| `AT+MESHPMF?` | [op] | Current setting. |
| `AT+MESHBRIDGE=<0\|1>` | [op] | L2 bridge mode: USB, the Wi-Fi AP and the mesh become ports of one lwIP bridge, so tethered hosts sit on the mesh segment with their own MACs and take addresses from the mesh's DHCP server — the fix for CoT and mDNS, whose payload addresses alias under NAT, against a peer whose mesh interface is a bridge port. It does not reach a node set up by OpenMANET's mesh wizard, whose DHCP server and applications sit on `br-ahwlan` behind `bat0`. NAT and the multicast repeater are off in this mode. Default 0 — NAT is the proven path. Persisted; next boot. **Compiled, not measured on air**; see [Mesh Mode](Mesh-Mode#bridge-mode). |
| `AT+MESHBRIDGE?` | [op] | Current setting. |
| `AT+MESHDHCP=<0\|1>` | [op] | Take a DHCP lease on the mesh if one is offered (default 1), else go straight to the static `10.77.x.y`. A peer that keeps its mesh interface bridged runs a DHCP server on that bridge. |
| `AT+MESHEN?` | [op] | Whether mesh mode is on. The capability envs always report 1. |
| `AT+MESHID=<id>` | [op] | Mesh ID, 1–32 chars. Persisted; next boot. Must match every peer exactly — a mismatch peers with nothing and reads as a radio fault. |
| `AT+MESHID?` | [op] | Current mesh ID. |
| `AT+MESHPASS=<pass>` | [op] | SAE passphrase, 1–63 chars. Persisted; next boot. Must match every peer. |
| `AT+MESHPASS?` | [op] | Length only, never the value — this console mirrors the logs. |
| `AT+MPMPEERS?` | [op] | Peer links and handshake state. The first thing to read on a mesh. `ampe_mtk`/`ampe_mgtk`: AMPE keys installed (our own MGTK counts once, at its first install). `mgtk_reinst`: re-installs of our own MGTK at a fresh TX PN base, made when an AMPE Open follows group frames sent under it or still queued in the chip at its last install, so the Key RSC in that Open lies above every PN already used. `mgtk_rsc_fail`: such re-installs the chip refused; that Open advertises one below the previous base. Both stay 0 except on `warthog-mesh-sae-swccmp` and `-swccmp-on`, the only builds with `WARTHOG_MESH_MGTK_PN_BASE`; every other build installs our MGTK at PN 0 and advertises RSC 0. |
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

## Datapath diagnostics

These exist to answer "the link is up, so where are the frames going".

| Command | | Purpose |
|---|---|---|
| `AT+DATASTAT?` | [diag] | Receive and transmit totals through the datapath. |
| `AT+RXCHAN?` | [diag] | What the chip pushed to the host, by frame class, plus drop reason. |
| `AT+FILTSTAT?` | [diag] | Which of the receive filter's eight drop paths is firing. |
| `AT+RXREORD?` | [diag] | Block-ack reorder accounting: frames dropped as outdated or parked. |
| `AT+RXHEAD?` | [dev] | Hex of the head of the last data frame — MAC header, QoS, mesh control, LLC. |
| `AT+FCRING?` | [dev] | Frame control of the last 32 frames the chip delivered. |

```
AT+FILTSTAT?
+FILTSTAT: drop=23 last=6 | short_fc=0 rts=0 beacon=0 short_hdr=0 no_ops=0
           unknown_sender=23 sa_is_us=0 dup=0
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
| `AT+SWCCMP=<0\|1>` | [dev] | Turn host software CCMP off or on; not persisted. Acts only on the builds compiled with it, `warthog-mesh-sae-swccmp` and `-swccmp-on` (armed at boot). |
| `AT+SWCCMP?` | [dev] | Host CCMP counters: `on`, `tried`, `ok` (decrypted and verified), `micfail`, `tx_ok`/`tx_fail`, `nokey` (no key for the frame's key id), `grpkey` (a unicast keyed with a group key, refused), `badhdr`, `short`, and the last `keyid` and AAD. |
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
