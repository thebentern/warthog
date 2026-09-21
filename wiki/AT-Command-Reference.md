# AT Command Reference

The CDC-ACM port accepts AT commands, so a node can be reconfigured and
inspected without reflashing. Any serial terminal at 115200 8-N-1 — CDC ignores
the baud rate.

Commands ending `=` set, ending `?` query. Every command answers `OK` or
`ERROR`, with any data on preceding `+VERB:` lines.

Warthog's log output is unreachable after early boot — the app hands the shared
USB PHY to USB-OTG for this console — so the diagnostic counters below are the
only visibility into the receive path. That is why there are so many.

> **Three commands need a mesh build.** `AT+MESHSTAT?`, `AT+RXHEAD?` and
> `AT+FCRING?` report a per-frame capture that only `warthog-mesh-smoke`
> records (`WARTHOG_MESH_RX_TAP`); on other builds they answer
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
| `AT+MESHCFG?` | [op] | **Read this first when a node will not join.** Every value a peer matches on — region, country, mesh ID, applied channel/bandwidth/operating class, security, DHCP — plus peers and beacons heard, plus what Warthog does not do (`forwarding=no routing=none l2=no(NAT) multicast=no batman=no`). Comparing these one verb at a time is how mismatches get missed. |
| `AT+MESHEN=<0\|1>` | [op] | Start the mesh instead of associating as a station. Persisted; takes effect on the next boot. Works on any build, including the region envs — this is how a stock image joins a mesh. |
| `AT+MESHCHAN=<chan>,<freq_hz>,<gclass>,<sclass>,<bw>` | [op] | Pin the S1G channel as a set — class and bandwidth belong to the channel, so they move together. Persisted; applied on next boot. `AT+MESHCHAN=default` clears it. |
| `AT+MESHCHAN?` | [op] | Stored set, **and whether it actually applied**. `applied=NO` means the regulatory table refused it and the radio is on its default — the usual cause of "peers with nothing, looks like range". On a region build the stored channel is matched against the country's regulatory row, so duty cycle, EIRP and airtime stay the regulator's; a channel that is not in the table is discarded rather than forced. |
| `AT+MESHFWD=<0\|1>` | [op] | 802.11s forwarding: relay other nodes' data and path selection through this node. Default 0 — the proven leaf behaviour. Persisted; next boot. |
| `AT+MESHFWD?` | [op] | Current setting. |
| `AT+MESHBRIDGE=<0\|1>` | [op] | L2 bridge mode: the USB and AP netifs join the mesh segment instead of being NATed, so tethered hosts get unique addresses on one segment. Default 0 — NAT is the proven path. Persisted; next boot. |
| `AT+MESHBRIDGE?` | [op] | Current setting. |
| `AT+MESHDHCP=<0\|1>` | [op] | Take a DHCP lease on the mesh if one is offered (default 1), else go straight to the static `10.77.x.y`. A peer that keeps its mesh interface bridged runs a DHCP server on that bridge. |
| `AT+MESHEN?` | [op] | Whether mesh mode is on. The capability envs always report 1. |
| `AT+MESHID=<id>` | [op] | Mesh ID, 1–32 chars. Persisted; next boot. Must match every peer exactly — a mismatch peers with nothing and reads as a radio fault. |
| `AT+MESHID?` | [op] | Current mesh ID. |
| `AT+MESHPASS=<pass>` | [op] | SAE passphrase, 1–63 chars. Persisted; next boot. Must match every peer. |
| `AT+MESHPASS?` | [op] | Length only, never the value — this console mirrors the logs. |
| `AT+MPMPEERS?` | [op] | Peer links and handshake state. The first thing to read on a mesh. |
| `AT+MESHRSSI?` | [op] | Per-neighbour signal: last, min, max, noise, SNR, the bandwidth that neighbour transmitted at, and a frame count. Min/max matter more than last — a link that averages fine but dips to −90 is the one that drops under load. A `bw=` that differs from your own is a configuration mismatch, not a weak link. **No MCS:** this driver's RX metadata (`struct mmdrv_rx_metadata`) carries RSSI, noise, frequency and bandwidth and no rate, so per-peer MCS is not reportable. |
| `AT+MESHSEC=<0\|1>` | [op] | Data plane open (0) or keyed (1). Re-peers within ~2 s. Persisted in NVS. |
| `AT+MESHSEC?` | [op] | Current setting. No effect on the SAE build (keys come from AMPE). |
| `AT+SAERX?` | [diag] | SAE/AMPE conversation state on the encrypted build: auth frames in/out, SAE FSM state, peering FSM, `ESTAB` count, which peer is being offered. |
| `AT+SAESTAGE?` | [diag] | Last step the SAE path reached, stored in RTC — survives a panic reboot. `AT+SAESTAGE=0` clears it. |
| `AT+SAEBRIDGE=<0\|1\|2>` | [diag] | Gate on offering discovered peers to the SAE supplicant. Defaults 1; `0` makes the node deaf to candidates (a debugging state); `2` also offers peers whose Mesh Configuration advertises a different auth protocol — needed toward OpenMANET, whose kernel MPM advertises 0 while running SAE. |
| `AT+COREDUMP?` | [diag] | Task, PC and backtrace of the last panic, read from the flash core-dump partition. Feed the addresses to `addr2line`. |
| `AT+MPING=<ipv4>[,<count>]` | [op] | ICMP echo from the node itself. Count 1–20, default 4; out-of-range is silently clamped to 4, not rejected. Blocks the AT console for the duration. |
| `AT+PEERS?` | [diag] | Datapath peer count and registration failures. |
| `AT+HWMPSTAT?` | [diag] | Path-selection counters. |
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
+HWMPSTAT: rx=234 preq_rx=75 preq_tx=142 prep_rx=159 prep_tx=75 parse_fail=0 not_ours=0
OK
```

Reading `AT+MPMPEERS?`: `estab=1` with a non-zero `plid` is a complete two-way
handshake. `plid=0` with `opens` climbing means the node is sending Opens nobody
answers — Warthog sends a Close and restarts after 8.

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

> `delivered=` in `AT+DATASTAT?` reads 0 on a perfectly healthy link. It is only
> incremented on a receive path this build does not take. Do not read it as a
> fault.

## Crypto

| Command | | Purpose |
|---|---|---|
| `AT+CCMPKAT?` | [dev] | Result of the AES-CCM known-answer test run at startup. |
| `AT+CRYPTOHOST=<0\|1>` | [dev] | Move CCMP between chip and host. |
| `AT+CRYPTOHOST?` | [dev] | Current setting, read back from the chip. |
| `AT+KEYFP?` | [dev] | Fingerprint of the installed mesh keys. |
| `AT+KEYINST?` | [dev] | Key installation results per peer. |
| `AT+REKEY=<n>` | [dev] | Reinstall keys on peer slot *n*. |
| `AT+REKEYSTAT?` | [dev] | Result of the last rekey. |

## Traffic generation

Bench tools. They put frames on air — do not leave them running on a live
deployment.

| Command | | Purpose |
|---|---|---|
| `AT+MSEND=` | [dev] | Send a single mesh frame. |
| `AT+MTPUT=` | [dev] | Throughput generator. |
| `AT+MINJECT=` | [dev] | Inject a raw frame. |
| `AT+MCAST=` / `?` | [dev] | Multicast test group. |
| `AT+MUDP?` | [dev] | UDP test-listener counters. |
| `AT+MUDPLAST?` | [dev] | Last UDP datagram received. |

---

## Worked example: bringing up a mesh link

```
AT+MESHEN=1                      mesh instead of a station uplink
AT+MESHID=halowmesh              must match the peer exactly
AT+MESHPASS=<passphrase>         SAE builds only; must match the peer
AT+RESET                         the three above take effect on boot
AT+MESHSEC=0                     match an unencrypted peer
AT+MPMPEERS?                     confirm estab=1 and a non-zero plid
AT+HWMPSTAT?                     confirm preq_tx climbing, parse_fail=0
AT+MPING=10.77.191.116,8         confirm data
```

If step 2 shows `estab=1` but step 4 fails, the peer has no *path* to us —
see [Mesh Mode](Mesh-Mode#how-paths-work-and-why-it-matters).
