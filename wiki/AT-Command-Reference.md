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
| `AT+MPMPEERS?` | [op] | Peer links and handshake state. The first thing to read on a mesh. |
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
AT+MESHSEC=0                     match an unencrypted peer
AT+MPMPEERS?                     confirm estab=1 and a non-zero plid
AT+HWMPSTAT?                     confirm preq_tx climbing, parse_fail=0
AT+MPING=10.77.191.116,8         confirm data
```

If step 2 shows `estab=1` but step 4 fails, the peer has no *path* to us —
see [Mesh Mode](Mesh-Mode#how-paths-work-and-why-it-matters).
