# Mesh RX keystone — status and the one experiment left

Status: **802.11s mesh peering ESTABLISHED (warthog <-> warthog).**

## RESOLVED — peering works between warthog boards

Three boards, identical firmware, no QEMU or MM8108 involved:

    81BA51: +MPMSTAT: rx=3972 open_tx=46 conf_tx=3951 conf_rx=3923 close_rx=0 llid=33701 plid=33181 estab=1
    F8823D: +MPMSTAT: rx=4223 open_tx=37 conf_tx=4204 conf_rx=4185 close_rx=0 llid=33181 plid=33701 estab=1
    F8738D: +MPMSTAT: rx=71   open_tx=5  conf_tx=71   conf_rx=67   close_rx=0 llid=51192 plid=33181 estab=1

The link ids cross-check reciprocally -- 81BA51's llid 33701 is F8823D's plid, and
F8823D's llid 33181 is 81BA51's plid. `estab` is only set when a received Confirm echoes
our own llid back in its plid field, so each side independently confirms the other
accepted it. `close_rx=0` throughout: no rejections.

**The missing piece was initiation.** warthog only ever *answered* a peering Open, so two
warthog boards sat waiting for each other indefinitely. `umac_mesh_maybe_initiate_mpm()`
now sends an Open when a peer's probe request is heard (every 2 s, which doubles as the
retransmit timer). mac80211 does the equivalent from `mesh_neighbour_update()` when
`auto_open_plinks` is set.

Also required, in order of discovery: the mesh probe responder (peers in beaconless mode
discover by probing, and warthog was dropping every probe request), the Supported Rates IE
(`mesh_matches_local()` compares the basic rate set), Confirm retransmission (the peer
retransmits while waiting and warthog answered only once), and path-selection identifiers
of 0x01/0x01 rather than the kernel-header 0x00.

Two boards previously written off as "wedged" were fine -- AT only appears ~4 s after a
**fresh power cycle**, because `warthog_at_start()` runs last in `app_main` behind a 20 s
`halow_wait_link`. Power-cycle and poll before declaring a board dead.

Known rough edge: `conf_tx`/`conf_rx` in the thousands. warthog answers every Confirm with
another Confirm, so once peered the two ends ping-pong at ~80 frames/s. Peering is correct
but the MPM exchange should quiesce after ESTAB -- see below.

Remaining gap: peering with the **Linux MM8108** still stalls at OPN_RCVD. Everything from
here down is that investigation.

---

Historical status: **802.11s peering NOT established.** No longer blocked on flashing.

## 2026-08-15 correction — read this before the sections below

Two things in this document were wrong.

**1. Flashing was never blocked.** `AT+DLMODE` works. The ROM enumerates as
**`303a:0009`**, not `303a:1001`; `303a:4020` is warthog's own TinyUSB app PID, not a
wedged state. The recipe that works:

    AT+DLMODE  ->  303a:0009 (stable, waits indefinitely)
    esptool --before no-reset --after hard-reset write_flash 0x0 firmware.factory.bin
    uhubctl -l <hub> -p <port> -a cycle     # RTS hard-reset is a no-op on native USB

`--before default-reset` can never work here because `-DWARTHOG_DEVLOOP_DTRRTS=0` is set
in this env (the DTR/RTS callback breaks USB-OTG init). That, not a hardware fault, is
why ~70 attempts failed. Board WTHG-0272A1F8738D now runs the 12-fix image (hash verified).

**2. The peer was NOT transmitting.** The table below claims "peer transmitting: yes,
+3.1..3.8 dB". Re-measured today with a power-gated A/B at full dwell:

| radio | delta vs own off/down baseline | verdict |
|---|---|---|
| warthog MM6108 | **+10.78 dB** in-channel vs +2.94 dB out-of-channel | transmitting, real 1 MHz S1G carrier at 904.5 |
| Linux MM8108 | **-0.17 dB**, -0.52 dB after clean driver reload | **silent** |

So the "chip delivers no RX in mesh mode" result is at least partly explained by there
having been **nothing on the air to receive**. The RX-side conclusions below are not
safe until the peer is SDR-verified transmitting.

Note on method: judge a carrier by the in-channel vs out-of-channel split, not a bare
on/off delta. A raw +14 dB looked convincing here and was mostly broadband USB noise.

Frequency agreement is confirmed and is not the problem — the driver's US map has
`CHANS1GHZ(5, 904, 500, IEEE80211_CHAN_1MHZ, 3600, 36)`, i.e. S1G ch5 = 904.5 MHz
<-> 5 GHz ch36, exactly warthog's pin.

**The open blocker is now on the Linux side, and it is root-caused.**

`iw dev <wl> mesh join` can never make the MM8108 beacon. The morse driver implements
**no mac80211 `join_mesh` / `leave_mesh` op** — grep both across `mac.c` and they do not
exist. `morse_cmd_set_mesh_config()` (which is what ultimately issues
`morse_cmd_cfg_mesh(..., !mesh_beaconless_mode)` to the chip) is reachable only from:

- `command.c:622` — `MORSE_CMD_ID_SET_MESH_CONFIG`, the vendor/driver-command path
- `mac.c:4244` and `mesh.c:830` — reconfig paths, which only run once mesh is *already* active

So a bare `iw mesh join` sets up mac80211's **software** mesh while the chip is never
told to configure mesh at all. The result is exactly what the SDR shows: `iw` reports a
healthy mesh point, and the radio is mute. Confirmed against the same interface in IBSS
mode, which beacons at +26.73 dB on the identical channel — the TX path is fine.

This independently corroborates the reference implementation: the working ESP32 port
drives the chip's `MESH_CONFIG` / `BSS_BEACON_CONFIG` commands directly because that is
the *only* route to mesh on this silicon.

### SOLVED — the MM8108 now beacons in mesh mode

`/dev/morse_io` is a raw register interface and is **not** the command channel. The
driver command goes in as an **nl80211 vendor command**, and plain `iw` can send it — no
`morse_cli` required:

    iw dev <wl> vendor recv 0x0cbf74 0x0 <binfile>    # OUI 0x0CBF74, subcmd 0
    # use `recv`, NOT `send` -- send swallows the response, recv prints it

Framing, little-endian throughout:

    struct morse_cmd_header { __le16 flags, message_id, len, host_id, vif_id, pad; }  // 12B
    # len = body length EXCLUDING the header
    SET_MESH_CONFIG (0xA018) body:
        u8 mesh_id_len; u8 mesh_id[32]; u8 mesh_beaconless_mode; u8 max_plinks
    # 47 bytes total, len = 35, mesh_beaconless_mode MUST be 0

Order matters: `set type mp` -> `ip link up` -> `iw mesh join` -> vendor command. The
driver requires a mesh vif with `is_mesh_active == false`, so it has to follow the join.
Success looks like `00 00 18 a0 04 00 ...` with a zero status word. Use `GET_VERSION`
(message_id 0x0002, len 0, bare 12-byte header) as a transport control — it returns
`rel_mm8108_2_0_0_2026_Apr_21`.

Result: **-0.52 dB -> +15.96 dB**. The MM8108 is now a real, beaconing mesh peer.

`mesh_beaconless_mode` must be 0 — same trap as warthog fix #2. Beaconless is a real
operating mode, not "MBCA off", and it makes the node silent.

Two measurement traps worth remembering: TX netdev counters do **not** count
driver-generated beacons and stay flat even when beaconing correctly, and dmesg stays
silent throughout even at `debug_mask=0xFFFFFFFF`. The SDR is the only arbiter.

## Next blocker: MUTUAL deafness

**Retracted:** the earlier "warthog's TX is not decodable" reading came from a
monitor-mode capture taken while the MM8108's QEMU passthrough was **dead** — `ENODEV: 5`,
`morse_usb_reg32_read failed -110` then `-19`, with `iw` still reporting a healthy
`type mesh point` the whole time. Same trap as the one already documented below. Gate
every capture on `dmesg | grep -cE '\-19|\-110'` before believing it.

Re-run with both ends verified healthy and transmitting:

| end | state |
|---|---|
| MM8108 | beaconing **+15.24 dB**, 0 USB errors, mesh ch36, `SET_MESH_CONFIG` status 0 |
| warthog | broadcast probe every 2 s, `mmdrv_tx_frame returned 0`, **+14.08 dB** |
| warthog RXTAP | **0 in 60 s** |
| MM8108 RX | **0 bytes / 0 packets** |
| `station dump`, both sides | empty |

**Do not read that table as "mutual deafness" — two rows are invalid.**

- **`iw` monitor mode does not work on this driver.** `hw->wiphy->interface_modes` is
  `AP | STATION | MESH_POINT | ADHOC`; `NL80211_IFTYPE_MONITOR` is **absent**, and the
  interface-combination limits allow only STATION/AP/MESH_POINT. `iw set type monitor`
  still returns success and `iw info` reports `type monitor`, but the hardware never
  enters promiscuous RX — tcpdump reads 0 packets regardless of what is on the air. The
  driver's real monitor path is a separate global `morse_mon` netdev in `monitor.c`
  which `iw` never creates. **Every monitor-based capture in this project is void.**
- **netdev RX counters do not count management frames.** Beacons and probes never
  increment them, so "MM8108 RX = 0 bytes" is not evidence of anything.

**RETRACTED — warthog is not deaf. Its receive path works.**

The `RXTAP = 0` readings were a *log-capture* artifact: MMLOG lines were being missed on
the CDC console, so absence of log output was mistaken for absence of frames. Reading the
tap counters directly via `AT+MESHSTAT?`, with the MM8108 SDR-verified beaconing:

    +MESHSTAT: rx=84 mgmt=0 beacon=0 last_fc=0x001c last_ta=d0:8b:26:04:00:00

84 frames in 60 s (~1.4/s, matching the peer's ~1.5/s beacon rate — `iw mesh join`
defaults to a 1000 TU / ~1.02 s beacon interval, so ~1/s is correct, not 10/s).
`last_fc = 0x001c` = version 0, type **EXT(3)**, subtype **S1G_BEACON(1)**.

Control: with the peer brought down, the counter froze at 119 and did not advance in 45 s.
The frames were unambiguously the MM8108's.

(`mgmt`/`beacon` show 0 only because the counter buckets `type == 0` as management; these
are type 3/EXT. Not an RX defect.)

## The actual remaining gap: beacons carry unusable addresses

`last_ta` differs run to run — `d0:8b:26:04:00:00`, then `88:66:49:06:00:d5`. Neither is
the peer's MAC. This is the S1G address-compression conversion zeroing/mangling A2, which
`umac_datapath_mesh.c` already documents:

> "peer beacons arrive with A2/A3 zeroed by the S1G address-compression conversion, so
> identify peers from Action/data frames, not from the beacon's addresses."

So warthog cannot learn the peer's MAC from a beacon and cannot start MPM from one. Peer
discovery must come from probe responses or action frames. warthog transmits a broadcast
probe every 2 s; the open question is whether the MM8108 ever answers, and if not, why.

## The blocker, reduced to one thing: warthog does not beacon

The link is **asymmetric**, and that is the whole story:

| direction | works? | evidence |
|---|---|---|
| MM8108 -> warthog | **YES** | `AT+MESHSTAT?` rx climbs ~1/s with `last_fc=0x001c` (EXT/S1G_BEACON); counter freezes the instant the peer is downed |
| warthog -> MM8108 | **NO** | `iw scan` from the MM8108 finds nothing; `station dump` and `mpath dump` stay empty |

Because the reverse direction works, the PHY, the channel, and warthog's receive path are
all fine. What is missing is warthog **transmitting beacons**:

- warthog's own log only ever shows `tx_broadcast_probe` every 2 s — never a beacon TX.
- `driver.c` already documents the symptom: "the beacon IRQ fires once at startup and
  never again".
- mac80211 mesh peer discovery is **beacon-driven**. A node that never beacons cannot be
  discovered, cannot become a peer candidate, and cannot form a plink — no matter how
  correct the rest of the mesh stack is.

Also confirmed: once the MM8108's MAC is set to warthog's pre-registered peer address
(`WARTHOG_PREREG_B0..B5` = `3c:1a:cc:4c:81:9d`), warthog receives a correctly-addressed
frame from it — `last_fc=0x0088` (type 2 DATA, subtype 8 QoS Data),
`last_ta=3c:1a:cc:4c:81:9d`. So warthog *can* see a valid peer address; the beacons'
mangled A2 is a separate, secondary issue.

Note the Linux reference sends `mbca_config = 0` with `enable_beaconing = 1` and beacons
fine (`mesh.c:148-152` zeroes `mbca.config` only for beaconless mode; it is 0 by default
anyway). warthog now matches that exactly and still does not beacon, so the MBCA fields
are not the cause.

### Measured: the host beacon path is fine; the chip never asks

`AT+BCNSTAT?` was added (counters in `mmdrv_host_get_beacon`, storage in `main/at.c`).
Result, stable across 45 s:

    +BCNSTAT: req=1 served=1 null=0 inactive=0

- `served=1, null=0` — when asked, the host **built and returned a valid mesh beacon**.
  Host-side beacon construction is not the problem.
- `req` stays at **1** and never advances.

And `req=1` is not even a chip request: `morse_beacon_start()` (beacon.c:105) itself does
one manual `driver_task_notify_event(DRV_EVT_BEACON_REQ_PEND)` as a kickoff. So that
single count is the driver's own kick — **the chip's beacon IRQ has never fired at all.**

The beacon model here is IRQ-driven: `morse_beacon_start()` sets `beacon.enabled`, arms
hardware IRQ `MORSE_INT_BEACON_BASE_NUM + vif_id` via `morse_beacon_set_irq_enabled()`,
and thereafter the **chip** is expected to raise that IRQ every TBTT (100 TU here);
`morse_beacon_work_()` then serves a template per IRQ. That IRQ never arrives.

So the fault is chip-side: the firmware accepts `BSS_CONFIG` (beacon_int=100, dtim=1),
`BSS_BEACON_CONFIG(enable=1)` (`umac_mesh.c:307`), `MESH_CONFIG(START, enable_beaconing=1,
mbca_config=0)` and `mmdrv_start_beaconing()` — and still never runs a TBTT timer in mesh
mode.

### Route around it: beaconless mesh + a probe responder (IMPLEMENTED, WORKING)

Since warthog cannot beacon, discovery has to run on probes instead — and the MM8108
supports exactly that. Setting `mesh_beaconless_mode = 1` in the vendor `SET_MESH_CONFIG`
payload puts the Linux peer into probe-based discovery, which matches what warthog
already does (broadcast probe every 2 s).

That immediately changed warthog's receive mix — management frames went 0 -> 18, with
`last_fc=0x0040` (subtype 4, PROBE REQUEST) carrying the peer's **real** MAC. The peer was
actively probing for us. warthog was dropping every one of them:

    case DOT11_FC_SUBTYPE_PROBE_REQ:
        MMLOG_WRN("mesh: ignoring probe req (no responder yet)\n");

So the peer could never learn warthog existed. Implemented:

- `umac_mesh_build_discovery_ies()` (`umac_mesh_beacon.c`) — serializes Mesh ID (114) and
  Mesh Configuration (113) into a flat buffer using the **same values the beacon
  advertises**, because `mesh_matches_local()` compares them before accepting a candidate.
- `umac_mesh_tx_probe_response()` (`umac_mesh.c`) — builds a probe response via
  `frame_probe_response_build` and transmits it, addr3 = own address (mac80211 uses
  `vif->addr` as the mesh BSSID).
- Wired into the `PROBE_REQ` case in `umac_datapath_mesh.c`, replacing the drop.
- `AT+PRSPSTAT?` exposes the counters.

Measured result:

    +PRSPSTAT: req_rx=3 rsp_tx=3 rsp_fail=0
    +MESHSTAT: rx=21 mgmt=11 last_fc=0x00d0 last_ta=3c:1a:cc:4c:81:9d

Every probe request received was answered, none failed — and warthog then began receiving
**`last_fc=0x00d0`, subtype 13 = ACTION frames** from the peer. Action frames are what
802.11s MPM uses (Open / Confirm / Close). The discovery loop now closes and the peer
responds by attempting to peer.

### Peer candidate CONFIRMED — MPM reaches OPN_SNT

With the probe responder in place, the Linux peer now creates a mesh peer entry for
warthog and starts the peering handshake:

    Station a8:dd:9f:4d:c7:f8 (on wlx0cbf740028d4)
        mesh plink: OPN_SNT      <- advanced from LISTEN
        mesh llid:  29962        <- peer allocated a link ID and sent Open
        mesh plid:  0            <- warthog has NOT replied
        tx packets: 4   rx packets: 0
    estab_plinks: 0

Attribution is controlled, not assumed:

- All three warthog boards powered **off** -> **0** stations over 50 s.
- Only the flashed board powered **on** -> peer appears at **t+8 s**.

So `a8:dd:9f:4d:c7:f8` is the flashed board's HaLow (MM6108) MAC — note this differs from
the ESP32's USB-serial-derived MAC, so don't expect `WTHG-…`-style addresses on air.

### MPM responder implemented — peer now reaches OPN_RCVD

Implemented a minimal MPM responder directly in morselib (`umac_mesh.c`), because the
Phase-1 FSM port in `components/halow_mesh_compat` is host-test only and is **not linked
into the firmware** — only the `halow` component is built, so nothing in that port runs on
hardware.

- `mesh_tx_mpm_()` builds Open/Confirm/Close bodies per `mesh_mpm_frame.c`'s layout
  (category 15 SELF_PROTECTED, action, capability, [AID for Confirm], Mesh ID + Mesh
  Config IEs, Peer Management IE 117 with proto/llid/[plid]) and sends them through
  `frame_action_build` + `mmdrv_tx_frame`.
- `umac_mesh_handle_mpm()` parses the peer's Peer Management IE for their llid (which is
  our plid — the link-id perspective swap) and, on Open, replies Open then Confirm.
- Hooked into `umac_datapath_mesh.c`'s ACTION case, intercepting category 15 before the
  supplicant fan-out (that path expects hostap's `mesh_mpm_action_rx`, which this build
  never reaches — which is why peering frames were being received and silently dropped).
- Counters via `AT+MPMSTAT?`.

Measured progression of the peer's plink state across builds:

    LISTEN  ->  OPN_SNT  ->  OPN_RCVD

**OPN_RCVD means the peer received and accepted warthog's Open.** The MPM exchange is now
bidirectional; warthog is transmitting well-formed peering frames that mac80211 accepts.

**Remaining: the handshake never reaches ESTAB** (`estab_plinks` stays 0).

Two candidate causes were tested and eliminated by the counters:

- *plid parsing* — ruled out. `AT+MPMSTAT?` reports `parse_fail=0` with a plausible
  `plid` (e.g. 57788, 17162) matching the peer's llid, so our Confirm echoes the right
  link id.
- *no retransmit* — found and fixed. A run showed `conf_rx=4` against `conf_tx=1`: the
  peer retransmits Confirm while waiting for ours, and warthog answered only the first
  Open and then went silent, so one lost or too-early Confirm stalled the handshake
  permanently. `umac_mesh_handle_mpm()` now re-sends Open+Confirm on every received
  Confirm. After the fix: `open_tx=4 conf_tx=4` against `rx=4` — every peer frame
  answered.

Peer plink state is **inconsistent across runs**: LISTEN, OPN_SNT, and OPN_RCVD have all
been observed with the same firmware. OPN_RCVD proves mac80211 *can* accept warthog's
Open; the runs that stall in OPN_SNT mean the same Open was rejected. That points at an
intermittent `mesh_matches_local()` failure rather than at the link ids.

### Supported Rates IE added — Open now accepted consistently, Confirm still rejected

Added Supported Rates (IE 1) to `umac_mesh_build_discovery_ies()`: the mandatory 5 GHz
OFDM set (`8C 12 98 24 B0 48 60 6C`, 6/12/24 Mbps marked basic), since dot11ah presents
S1G as 5 GHz. Shared by the probe response and all MPM frames.

Effect: the peer now reaches **OPN_RCVD on every run** (previously it oscillated between
LISTEN / OPN_SNT / OPN_RCVD), i.e. mac80211 reliably accepts warthog's Open. Latest state:

    +MPMSTAT: rx=2 open_tx=2 conf_tx=2 conf_rx=1 close_rx=0 parse_fail=0 llid=51192 plid=50127
    peer: plink=OPN_RCVD (stable for 70 s), estab_plinks=0

So the exchange is fully bidirectional and every frame is answered, but our Confirm is
still classified `CNF_IGNR` or `CNF_RJCT` by mac80211 instead of driving OPN_RCVD -> ESTAB.

### AT+MPMDUMP? — the Confirm we transmit is well-formed

Added `AT+MPMDUMP?`, which hexdumps the last Confirm body built (there is no on-air
capture available; see the monitor-mode note above). Captured against a peer sitting in
OPN_RCVD with `llid=41141 plid=51192`:

    +MPMDUMP: len=52
    0f 02 00 00 00 00 01 08 8c 12 98 24 b0 48 60 6c
    72 11 77 61 72 74 68 6f 67 2d 6d 65 73 68 2d 74
    65 73 74 71 07 01 01 00 01 00 00 09 75 06 00 00
    f8 c7 b5 a0

Decoded: category 0x0f, action 0x02 (Confirm), capability 0, AID 0, Supported Rates
(8 rates), Mesh ID "warthog-mesh-test", Mesh Config `01 01 00 01 00 00 09`, Peer Mgmt IE
len 6 with proto 0, **llid 0xc7f8 = 51192**, **plid 0xa0b5 = 41141**.

Both link ids are correct — plid equals the peer's llid exactly, which is what
`sta->mesh->llid != plid` (CNF_IGNR) tests. So the Confirm is well-formed and correctly
addressed, and the remaining rejection is NOT a link-id problem.

### Path-selection identifiers: 0x01 is correct for this peer (measured)

The kernel constants say HWMP/Airtime are **0**, and `mesh_matches_local()` compares
`meshconf_psel`/`meshconf_pmetric` against `ifmsh->mesh_pp_id`/`mesh_pm_id`. That argues
for 0x00/0x00. It was tried on hardware and is **wrong here**:

| psel/pmetric | result |
|---|---|
| `0x01/0x01` | peer creates a candidate, sends Open, reaches OPN_RCVD |
| `0x00/0x00` | `PRSPSTAT req_rx=16 rsp_tx=16` but `MPMSTAT rx=0` — peer probes repeatedly, never initiates peering |

The morse driver evidently runs with `mesh_pp_id`/`mesh_pm_id` = 1 rather than the kernel
defaults. Do not "correct" these to 0 from the headers alone. Reverted to 0x01/0x01;
`WARTHOG_MESH_PATHSEL_ZERO` re-tests 0x00.

### Narrowed: the Confirm is IGNORED, not rejected

Added Close-reason parsing (`close_reason` in `AT+MPMSTAT?`) because an 802.11s Close
carries the reason mac80211 refused the peering. Best run so far:

    +MPMSTAT: rx=6 open_tx=6 conf_tx=6 conf_rx=4 close_rx=0 parse_fail=0 llid=51192 plid=17610
    peer: plink=OPN_RCVD, stable for 18 s+, estab_plinks=0

**`close_rx=0` is the informative part.** In `mesh_process_plink_frame()` a Confirm that
fails `mesh_matches_local()` becomes `CNF_RJCT`, which makes mac80211 send us a Close.
We never receive one. So the Confirm is being classified **`CNF_IGNR`** — silently
ignored — and that leaves exactly two possible causes:

    } else if (ftype == WLAN_SP_MESH_PEERING_CONFIRM) {
            if (!matches_local)                       -> CNF_RJCT  (ruled out: no Close)
            else if (!mesh_plink_free_count(sdata) ||
                     sta->mesh->llid != plid)         -> CNF_IGNR  (one of these)
            else                                      -> CNF_ACPT

**Cause 2 eliminated by measurement:** `iw dev <wl> get mesh_param mesh_max_peer_links`
returns **32** with `estab_plinks = 0`, so `mesh_plink_free_count()` is 32, not 0. (Note
the param is `mesh_max_peer_links`, not `dot11MeshMaxPeerLinks`, and the `max_plinks=8`
in our vendor `SET_MESH_CONFIG` payload configures the *driver's* mesh config, not
mac80211's `mshcfg` — it has no bearing here.)

**That leaves exactly one cause: `sta->mesh->llid != plid`.** Our Confirm's plid does not
equal the peer's llid *at the moment the peer processes it*, even though `AT+MPMDUMP?`
shows the plid we emit matches the llid we parsed and `parse_fail=0`.

**Most likely mechanism — the peer's confirm window is far shorter than our response:**

    mesh_confirm_timeout = 100 milliseconds   (default; iw caps manual values at 255)
    mesh_max_retries     = 3

warthog answers an Open by building and transmitting two frames (Open, then Confirm),
each through `build_mgmt_frame()` + `mmdrv_tx_frame()` with chip round-trips. If that
exceeds ~100 ms the peer's confirm timer fires, it re-mints a fresh llid and retries — so
our Confirm arrives carrying the *previous* llid as plid and is silently ignored. This
predicts exactly what is observed: a peer that keeps re-opening, a plid that changes every
run (57788 -> 50127 -> 41141 -> 17610 -> ...), Confirms that are never accepted, and no
Close (CNF_IGNR is silent).

**Attempted and inconclusive — Confirm-first ordering.** Sending the Confirm before the
Open (so the latency-critical frame goes out a full frame-build earlier) was implemented
and flashed. The run that followed left the peer in **OPN_SNT** rather than the OPN_RCVD
that Open-first reliably produced, with `rx=2 open_tx=2 conf_tx=2 conf_rx=0`. Run-to-run
variance in this setup is large enough that this is **not** a clean refutation — but
Open-first is the empirically best-observed ordering, so the firmware has been reverted to
it and the alternative is gated behind `WARTHOG_MESH_MPM_CONFIRM_FIRST`. Re-test properly
on a stable peer. The Confirm-only-on-retry change (do not re-send Open when answering a
Confirm) was kept, since it strictly reduces work inside the peer's confirm window.

The device is left flashed in this best-known state.

Two ways to test/fix, in order:
1. **Cut warthog's Open->Confirm latency.** Pre-build both frames, or send the Confirm
   first, or hold a cached template so the reply is a single queue operation. Measure with
   a timestamp counter around the handler exposed over AT.
2. Raise the peer's window: `iw dev <wl> set mesh_param mesh_confirm_timeout 255` plus
   `mesh_max_retries 15` (255 ms is the kernel's ceiling via iw). Applied successfully but
   the run was cut short by a VM crash before the handshake completed — retry this, it is
   the cheapest discriminator: if peering completes with the larger window, latency is
   confirmed as the cause.

Because `matches_local` passes, Mesh ID, Mesh Configuration, and the rate set are all
already correct — those can be struck off the list.

**What to do next.** Guessing further is low-yield — the remaining candidates all need to
be checked against the bytes actually on air, and this driver cannot do that: `iw` monitor
mode is unsupported (`NL80211_IFTYPE_MONITOR` is absent from `interface_modes`; see above),
so tcpdump captures nothing regardless. Options:
1. Instantiate the driver's real `morse_mon` netdev (monitor.c) so frames can be captured,
   then diff warthog's Confirm against a Linux-generated one field by field.
2. Add a hexdump of the built Confirm body over the AT channel and compare offline against
   `mesh_plink_frame_tx()` output.
3. Check the two fields not yet verified: AID (we send 0) and the Mesh Configuration
   capability/formation octets, both of which `mesh_matches_local()` inspects.

Superseded suspicion (kept for the record): `umac_mesh_build_
discovery_ies()` emits only Mesh ID (114) and Mesh Configuration (113).
`mesh_matches_local()` also compares the **basic rate set** (`ieee80211_sta_get_rates()`
vs `sdata->vif.bss_conf.basic_rates`); with no rates IE the parsed basic_rates is 0 and
the comparison mismatches, causing the candidate to be rejected. Add Supported Rates (1)
and Extended Supported Rates (50) — matching whatever the S1G operation advertises — to
the discovery IE blob, which is shared by both the probe response and the MPM frames.

Secondary, if that does not settle it: AID is sent as 0 in Confirm; verify mac80211's
mesh path does not validate it.

### Superseded: warthog must answer the MPM Open The peer is in OPN_SNT with
a valid llid and `plid = 0`; it is waiting for warthog's Mesh Peering Open/Confirm. warthog
*receives* those Action frames (`AT+MESHSTAT?` shows `last_fc=0x00d0`, subtype 13) but never
replies, so `rx packets` stays 0 at the peer and the plink cannot reach ESTAB.

warthog already has a faithful port of mac80211's MPM FSM from Phase 1
(`components/halow_mesh_compat/src/mesh/mesh_plink.c` and `mesh_mpm_frame.c`, host-tested),
but it is not wired into the live datapath: `process_rx_mgmt_frame_mesh()` routes ACTION
frames to `umac_datapath_process_rx_action_frame` -> SELF_PROTECTED -> supplicant fan-out,
which never reaches the ported FSM, and nothing drives MPM frame TX. Wiring that FSM to the
Action-frame RX path and to `build_mgmt_frame`/`mmdrv_tx_frame` is what closes peering.

**Environmental note on confirming the final plink.** The MM8108 under QEMU
USB passthrough repeatedly dies mid-test: `morse_cmd_health_check ... timed out (-110)`,
`morse_mac_ops_start: failed`, once a full `Kernel panic - not syncing: Fatal exception in
interrupt`, plus a latent `UBSAN: shift-out-of-bounds in mmrc.c:260` in the driver's rate
control. Peering state (`station dump` / `mpath dump`) could not be read because the peer
was gone by the time it was queried. Re-run the check on a peer that survives the window —
ideally the MM8108 on a native Linux host rather than QEMU passthrough.

Next probes for the beacon path, in order of cheapness:
1. Capture the return code of `morse_beacon_set_irq_enabled()` / `morse_hw_irq_enable()`
   into an AT-visible counter — if arming silently fails, nothing downstream can work.
2. Check whether the beacon IRQ number is right for a mesh VIF (`MORSE_INT_BEACON_BASE_NUM
   + vif_id`, vif_id=0) and whether the chip masks it outside AP mode.
3. Compare against the Linux driver: it never uses this host-served-beacon path at all
   (mac80211 supplies beacons), so a firmware that only runs the TBTT timer for AP/IBSS
   VIFs would show exactly this signature.

### IQ spectral comparison (valid — `rtl_sdr` + FFT, burst-gated)

| | centroid | 99% BW |
|---|---|---|
| warthog | 904.4741 MHz | 1.599 MHz |
| MM8108 | 904.4944 MHz | 1.620 MHz |

Bandwidths are essentially identical and the centroids differ by ~20 kHz. Spectrally
these are the same kind of signal, which argues *against* a gross PHY mismatch and
against frequency offset being the cause. Note `rtl_power` cannot make this measurement
— a fine sweep lies entirely inside the 1 MHz channel with no in-window noise floor, and
reports "no signal above floor+4 dB" for both. Use `rtl_sdr` IQ capture + FFT with burst
detection.

### Where that leaves it

Valid, surviving facts: both radios transmit on 904.5 MHz with near-identical spectra;
warthog's host datapath receives nothing from a verified-beaconing peer; no peering.

The next measurement should establish whether the MM8108 hears warthog **at all**, since
that has never been tested with a working method. Options: bring the MM8108 up as a mesh
point and look for warthog in `iw dev <wl> station dump`/`mpath dump` after warthog's
probes, watch for the driver's `MORSE_VENDOR_EVENT_MESH_PEER_ADDR` vendor event, or get
the real `morse_mon` monitor netdev instantiated instead of the `iw` type switch.

## Observability is solved — logs DO reach the CDC console

An earlier claim in this document that app logs are unreachable was wrong. MMLOG output
interleaves with AT replies on the TinyUSB CDC port; the empty captures were port/timing
failures, not a dead console. Confirmed live:

    W (141599) umac_mesh: tx_broadcast_probe#70: mmdrv_tx_frame returned 0 (vif=0 ssid_len=17 ...)

Two traps: AT and logging only come up ~4 s after a **fresh power cycle**
(`warthog_at_start()` runs last in `app_main`, behind a 20 s `halow_wait_link`), and after
an `AT+RESET` the board frequently returns with USB enumerated but AT dead. Power-cycle
and poll for AT before concluding a board is wedged — two of the three boards were written
off as wedged on exactly this mistake.

`AT+MESHSTAT?` (RX-tap counters) is implemented and builds, but is not yet flashed. Build
note: the counters must be **defined in `main/at.c`** and referenced as `extern` from
morselib — morselib links as an archive, and the linker will not extract an object from it
to satisfy a reference originating in main.

## The result (controlled, repeated, both ends verified)

> Warthog's MM6108 delivers RX frames to the host during the boot **scan**, and
> stops the instant the **mesh vif becomes active**.

Conditions under which this was measured — every variable pinned:

| Variable | Value | How it was verified |
|---|---|---|
| Channel | 904.5 MHz (S1G ch5) both ends | warthog pinned via `WARTHOG_PIN_S1G_CHAN`, `channel pin status = 0`; Linux `iw` ch36 = 904.5 |
| BSSID | matched | Linux peer's MAC forced to warthog's BSSID (mac80211 uses `vif->addr` as mesh BSSID) |
| Peer transmitting | yes | RTL-SDR A/B: +3.1..3.8 dB over its own silent baseline |
| Peer alive | yes, before AND after | `JOIN OK`, `mesh_param mesh_ttl` = 31, `dmesg | grep -c -- -19` = 0 |
| Observation point | upstream of ALL host filters | `WARTHOG_MESH_RX_TAP` at top of `umac_datapath_rx_frame_filter()` |

Outcome: `RXTAP` fires only at t≈925–1394 ms (boot scan). In steady-state mesh it is
0 across repeated 60–75 s captures. The chip emits only `RXCHAN chan=0xfe`
(command channel) — never 0x00/0x03/0x04 (data/beacon/mgmt).

This **rules out**: channel mismatch, BSSID/Addr3 mismatch, peer not transmitting,
peer dead, host-side filtering.

## Leading mechanism

`chan=0xfe`-only is the signature of **page exhaustion** on the command channel.
The working reference (`teapotlaboratories/mm-esp32-halow`, `umac_mesh.c`) warns
verbatim that this is what happens when `MESH_CONFIG(START)` runs before the host
can serve beacons:

> "MESH_CONFIG(START) must not run until the host can serve a beacon -- otherwise
> the firmware beacons into an unready host and the command channel backs up
> (page exhaustion)."

Warthog had exactly that ordering wrong. **The fix is staged but has never run.**

## Fixes staged (all verified present in source; image builds clean)

1. `mmwlan_set_channel_list()` moved before `mmwlan_boot()` — it returns
   `MMWLAN_UNAVAILABLE (3)` if called later, silently leaving the full regulatory
   list and making the operating channel unobservable. Now `status = 0`.
2. `mbca_config` non-zero. Zero selects **beaconless mode** (not "MBCA off"),
   which contradicts `enable_beaconing=1`.
3. `MESH_CONFIG(START)` issued **last**, after BSSID/BSS_CONFIG/beacon-constructor
   and `mmdrv_start_beaconing()`.
4. Pre-BSSID `WARTHOG_MESH_RX_TAP` (diagnostic).
5. BSSID mode switchable: shared/derived (warthog<->warthog) vs own-MAC
   (`WARTHOG_MESH_BSSID_OWN_MAC`, for a Linux mac80211 peer).
6. `BSS_CONFIG` cssid = 0 (warthog passed CRC32(mesh_id); reference passes 0).
   `WARTHOG_MESH_CSSID_NONZERO` restores the old value.
7. NDP probe disabled (`mmdrv_set_ndp_probe` — 0 occurrences in the reference).
   `WARTHOG_MESH_NDP_PROBE` re-enables.
8. Legacy `MGMT/BEACON` (type 0, subtype 8) added to the pre-association
   allowlist. The chip converts S1G beacons to the legacy form on the way up,
   so an EXT/S1G_BEACON-only allowlist can never pass a peer's beacon.
9. Mesh datapath ops installed immediately after VIF accept, not after
   `MESH_CONFIG(START)` — the chip can deliver RX before START returns.
10. STA pre-registration via `mmdrv_update_sta_state`
    (`WARTHOG_MESH_PREREG_PEER` + `WARTHOG_PREREG_B0..B5`). The reference always
    registers the peer with the chip; warthog never did, so the chip had no
    STA context to accept frames against.
11. Mesh Configuration IE path proto/metric = `0x01`/`0x01` (HWMP / Airtime),
    not `NONE`/`NONE`. `mesh_matches_local()` compares these fields, so any
    standards-following peer rejects warthog as a candidate outright.
    `WARTHOG_MESH_PATHSEL_NONE` restores the old value.
12. S1G beacon `frame_control` high byte `0x08`, not `0x00` (full FC `0x081c`).
    The upper octet carries the optional-field presence bits (Next-TBTT /
    Compressed-SSID / ANO). Emitting `0x001c` advertises a different header
    shape than the 15-byte header we actually lay out, so a peer parses our
    beacon at the wrong offsets and discards it — even when the beacon is
    transmitted and delivered correctly.
    `WARTHOG_MESH_BEACON_FC_HI_ZERO` restores the old value.

Items 6–12 came from a line-by-line diff against the reference's `umac_mesh.c`
while hardware was blocked. Each is independently switchable so the one that
actually mattered can be identified once two boards run the same image.

## The one experiment left

Two boards must run the **same** image. They currently do not, and every software
route into the ROM bootloader is exhausted (~60 `AT+DLMODE`, all `esptool --before`
modes, both bauds, boot racing, port hunting, isolation, 13–15 s drains, watchdog
waits, 100 s continuous hammer). OTA is impossible: single `factory` partition, no
OTA slots, no HTTP server.

    hold BOOT -> tap RESET -> release BOOT -> ./scripts/autoflash.sh    (x2 boards)

Then: RXTAP firing in steady-state mesh means the ordering bug was the cause and
peering is next. RXTAP still 0 means the gate is elsewhere in the chip command
sequence — diff warthog's sequence against the reference's `mmwlan_mesh_start()`.

## Bench notes that cost real time

- **Wedged board** = enumerates as bare `[303a:4020]`, no product string, no serial;
  ignores AT and flashing. Short `uhubctl` cycles do NOT fix it. A **13–15 s
  power-off** does (rail/PA capacitance).
- **QEMU USB passthrough silently dies** to `-19 ENODEV` while `iw` still reports a
  healthy mesh point and netdev TX counters keep incrementing. Gate every
  measurement on `dmesg | grep -c -- -19` **and** `get mesh_param mesh_ttl`, before
  and after. Two confident conclusions in this project were false for this reason.
- Use `-device usb-ehci`, not `qemu-xhci` (MM8108 is USB-2.0 high-speed).
- `insmod morse.ko country=US` does not set the kernel regdomain; run
  `iw reg set US` or 755–928 MHz stays `PASSIVE-SCAN` (no-IR).
- macOS caches stale `/dev/cu.*`; port presence never proves a board is alive.
- Never probe with `esptool chip-id` before flashing — it consumes the ROM window.
