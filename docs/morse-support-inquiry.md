# Support inquiry — mesh mode RX on `mm6108.mbin` 1.17.6

**Date:** 2026-05-25
**Hardware:** XIAO ESP32-S3 + Seeed HaLow add-on (Quectel FGH100M-H, MM6108)
**Chip ID:** 0x0306
**Firmware:** `mm6108.mbin` v1.17.6
**BCF:** `mf16858`
**Morselib:** 2.10.4-esp32-1 (Espressif Component Registry)
**Driver:** Custom embedded port (no Linux mac80211 — direct morselib + hostap on ESP-IDF 5.5.4)

## Summary

Two MM6108-based boards on the same channel and mesh_id fail to discover each
other. The chip accepts every chip-level command we send and reports
"beaconing as a mesh STA", but **never delivers a single received frame to
`mmdrv_host_process_rx_frame`** (the chip-shim RX entry) while in mesh mode.

The same `mmdrv_host_process_rx_frame` path delivers frames correctly when
the chip is in STA mode (verified by working HaLow STA association in
production). The gap is mesh-mode-specific.

## Chip-side bring-up sequence we send

In order, all accepted by the chip (every response status = 0):

| # | Opcode | Args |
|---|--------|------|
| 1 | `MORSE_CMD_ID_ADD_INTERFACE` (0x0004) | `type = MESH (5)`, MAC = NULL (chip uses factory MAC) → vif_id=0 |
| 2 | `MORSE_CMD_ID_SET_CHANNEL` (via `umac_interface_set_channel_from_regdb`) | first regdb channel (chan=1 bw=1MHz on US BCF) |
| 3 | `MORSE_CMD_ID_BSS_CONFIG` (0x0006) | beacon_interval_tu=100, dtim_period=1, cssid=CRC32(mesh_id) |
| 4 | `MORSE_CMD_ID_MESH_CONFIG` (0x0039) | opcode=START, enable_beaconing=1, all MBCA fields=0 |

After step 4 the chip log emits "firmware beaconing as a mesh STA" — i.e.
the chip considers itself operational. No further commands are sent.

## Observed vs expected

- ✅ Both boards reach the same state independently.
- ✅ CSSID on both boards = `0x46e93c69` (identical CRC32 of mesh ID `warthog-mesh-test`).
- ✅ Both antennas attached. Boards <2 feet apart on a bench.
- ✅ STA-mode association on the SAME boards via the SAME morselib version works flawlessly.
- ❌ With a counter logged at every entry to `umac_datapath_rx_frame` (called from
  `mmdrv_host_process_rx_frame` in `umac_mmdrv_shim.c`), we see **zero** received
  frames on either board for the entire duration of mesh operation. No beacons,
  no probe requests, no action frames — nothing.
- ❌ The chip event channel (`MORSE_CMD_ID_EVT_*`) is also silent for mesh —
  no beacon-loss, no peer-discovered, no unknown events. Confirmed with a
  default-case log on `MORSE_CMD_ID_EVT_*` in `event.c`.

## Specific questions

1. **Does the chip's mesh-mode beacon callback follow the same polled
   model as AP mode?** In our embedded build, `mmdrv_start_beaconing(vif_id)`
   (driver.c:573 → `morse_beacon_start()`) enables the per-VIF beacon IRQ
   that fires `mmdrv_host_process_rx_frame`'s sibling
   `mmdrv_host_get_beacon` callback. We call this for AP mode (via
   `umac_ap_start`, `umac_ap.c:267`) but not for mesh. With it not called,
   the chip is silent on `mmdrv_host_get_beacon`.

   - If the answer is **yes, mesh uses the same polled model**, adding
     `mmdrv_start_beaconing(mesh_vif_id)` after `MESH_CONFIG(START)`
     should make the chip start asking the host for mesh beacon templates,
     and the gap reduces to building a proper mesh beacon (Mesh ID IE
     113 + Mesh Configuration IE 114 + S1G Capabilities + RSN if SAE).
   - If the answer is **no, mesh uses the push model via
     `MORSE_CMD_ID_BEACON_OFFLOAD = 0x0053`**, that opcode (and its TLV
     payload format `morse_cmd_req_beacon_offload`) is in the GPL
     `morse_driver/morse_commands.h` but not in the
     `morsemicro/halow@2.10.4-esp32-1` `morse_commands.h`. Could you
     confirm whether the chip firmware in `mm6108.mbin` v1.17.6 accepts
     `0x0053`, and if so, share the expected TLV layout for the mesh
     beacon template?

2. **What's the role of `MORSE_CMD_PARAM_ID_BEACON_OFFLOAD = 24` and
   `MORSE_CMD_PARAM_ID_PROBE_RESP_OFFLOAD = 25`** in the chip's
   `GENERIC_PARAM (0x003E)` namespace? Both are defined in
   `morse_commands.h` but never set by morselib. Are these the on/off
   toggles that the chip uses to switch between polled and push beacon
   models? If yes, what's the correct value sequence for mesh?

3. **Is `MORSE_CMD_ID_MCAST_FILTER (0x003C)` required for mesh broadcast
   RX?** Mesh action frames (PLINK Open/Confirm/Close) are sent to
   broadcast `ff:ff:ff:ff:ff:ff`. Without an explicit mcast filter
   entry, the chip's hardware may be dropping them before the RX path
   delivers anything to `mmdrv_host_process_rx_frame`. We see zero RX
   in mesh mode despite verified on-air bench geometry (boards <2 ft
   apart, same channel, identical CSSID, identical mesh_id).

4. **Is there an additional opcode beyond `MESH_CONFIG (0x0039)` that
   uploads the mesh_id to the chip?** Reading the Linux driver shows
   `morse_cmd_set_mesh_config()` (driver-internal opcode `0xA018`)
   stashes mesh_id in driver RAM and then calls `cfg_mesh_bss()` →
   `cfg_mesh()` → `0x0039` to chip. The chip never sees the
   mesh_id directly. Where does the chip get its mesh_id for the
   on-air Mesh ID IE if not from the host driver? Is it derived
   from CSSID (the CRC32 of mesh_id we pass in `BSS_CONFIG (0x0006)`)?
   If so, that's not reversible — the chip can't construct a Mesh
   ID IE from a CRC32. Which means the chip's internally-generated
   mesh beacons cannot contain a proper Mesh ID IE, which would
   explain why two boards on the same `mesh_id` never see each
   other's beacons as mesh-relevant.

5. **Is there a known-good chip-command sequence for embedded
   (non-mac80211) mesh bring-up on `mm6108.mbin`?** Your forum
   post in "Sensor Mesh with Wi-Fi HaLow" says microcontroller
   stack mesh is unsupported, but the chip ABI clearly accepts
   the mesh opcodes. If "unsupported" means "we don't ship the
   morselib glue but the firmware supports it", we'd like to
   complete the glue ourselves — and your guidance on the exact
   command sequence (with vif_id ordering, expected return codes,
   and any required `GENERIC_PARAM` toggles) would short-circuit
   weeks of reverse engineering.

## Steps 18-22 — Linux MBCA defaults, QoS queues, TX/event/listen-only isolation (2026-05-25)

Each step refined our chip configuration to match Linux's `morse_driver/mac.c`
even more precisely. None unlocked mesh RX:

- **Step 18** — `MESH_CONFIG` with Linux's MBCA defaults: mbca_config=`TBTT_SEL_ENABLE`,
  min_beacon_gap=25, mbss_start_scan_duration=2048, tbtt_adj_interval=60000.
  Chip accepts (status=0). Zero RX.
- **Step 19** — `SET_QOS_PARAMS` (4× ACI=0..3, matching the conf_tx-per-AC
  sequence Linux issues during interface bring-up). Chip accepts each. Zero RX.
- **Step 20** — Instrumented `mmdrv_host_process_tx_status` to detect chip TX
  completions. Zero TX_STATUS events seen (inconclusive: chip may skip
  TX_STATUS for broadcast/beacon by design).
- **Step 21** — Bumped `morse_driver/event.c` default-case log to ERR so unknown
  chip events surface. Zero unknown events ever fired. The chip is profoundly
  silent post-init — no events, no TX_STATUS, no RX, no second beacon-template
  request.
- **Step 22 (clean isolation)** — Built a no-beaconing variant of the firmware,
  flashed it only to board A. Board B kept the full step17–19 build (active
  beacon source). Boards <2 ft apart. Board A as pure listener saw zero RX
  across the full window. **Rules out TX-induced MAC-pipeline blocking** —
  the chip's mesh RX is fully independent of any TX activity.

## Cumulative finding (2026-05-25)

Our embedded port now issues **every chip command** that `morse_driver/mac.c`
issues for mesh interface bring-up, in the same order, with equivalent
arguments:

```
ADD_INTERFACE(type=MESH, own_mac)
GET_CAPABILITIES
SET_CHANNEL(chan=1, bw=1MHz)
BSSID_SET(own_addr)            ← step15-16
4× SET_QOS_PARAMS(ACI=0..3)    ← step19
BSS_BEACON_CONFIG(enable=1)    ← step17
BSS_CONFIG(beacon_int=100TU, dtim=1, cssid=CRC32(mesh_id))
MESH_CONFIG(start=true,        ← step18
            enable_beaconing=true,
            mbca_config=TBTT_SEL_ENABLE,
            min_beacon_gap_ms=25,
            mbss_start_scan_duration_ms=2048,
            tbtt_adj_timer_interval_ms=60000)
mmdrv_start_beaconing (beacon IRQ enable)
beacon template construction (Mesh ID IE 114, Mesh Config IE 113,
                              S1G Capabilities IE 217, rate-control table)
```

The chip accepts every command (`status = 0`). Then:

| Signal | Count over 2+ minutes |
|---|---|
| Unknown chip events (`Mac EVT 0x____`) | **0** |
| Beacon template re-requests (`mmdrv_host_get_beacon#N`) | **1** (only on init, never again) |
| TX completions (`mmdrv_host_process_tx_status#N`) | **0** |
| Peer RX frames (`mmdrv_host_process_rx_frame#N`) | **0** |

The chip's mesh state machine appears **dormant** after MESH_CONFIG. Whatever
condition Linux's mac80211 / `morse_driver` satisfies that brings the chip's
mesh runtime to life, it is not addressable from the chip-command surface we've
mapped from your GPL driver source.

We are now at the limit of what host-side reverse engineering can reveal.

## Reverse-engineered the full Linux chip-command sequence (step17, added 2026-05-25)

Cloned `MorseMicro/morse_driver` (1.17.9) locally and traced every chip
command the mesh path issues, end-to-end:

| Order | Chip command | Trigger in Linux | Embedded port? |
|---|---|---|---|
| 1 | `ADD_INTERFACE(type=MESH, addr=own_mac)` | `morse_mac_ops_add_interface` | ✅ |
| 2 | `GET_CAPABILITIES` | `morse_mac_vif_init_common` | ✅ |
| 3 | `SET_CHANNEL` | `morse_mac_set_channel` | ✅ |
| 4 | `BSSID_SET(own_addr)` | `bss_info_changed`: BSS_CHANGED_BSSID | ✅ (added step15→16) |
| 5 | `BSS_CONFIG(cssid, beacon_int, dtim)` | `bss_info_changed`: BSS_CHANGED_SSID / BEACON_INT | ✅ |
| 6 | `BSS_BEACON_CONFIG(enable=1)` | `bss_info_changed`: BSS_CHANGED_BEACON_ENABLED | ✅ (added step17) |
| 7 | `MESH_CONFIG(opcode=START, enable_beaconing=1)` | `morse_cmd_cfg_mesh_bss` invoked by `morse_cmd_set_mesh_config` | ✅ |
| 8 | Beacon IRQ + polled `host_get_beacon` callback | `morse_beacon_init` → `morse_beacon_irq_enable` | ✅ (step12) |
| 9 | Beacon template handed to chip (Mesh ID IE 114 + Mesh Config IE 113 + S1G Caps) | mac80211 builds, driver passes through | ✅ (step13) |

The Linux driver's `morse_mac_ops_configure_filter` callback IGNORES all
`FIF_*` flags (mac.c:4269 — only acts on the `multicast` parameter). The
multicast filter struct is for IPv4/IPv6 multicast (first 2 bytes are
assumed `01:00` or `33:33` per morse.h:944) — does NOT cover broadcast,
which is what mesh beacons target. So Linux doesn't push anything for
broadcast either.

**Verdict:** Our embedded port now issues every chip command Linux issues
in the same order with equivalent arguments. The chip accepts every
command (all `status = 0`). The chip transmits our beacon template.
Yet **`mmdrv_host_process_rx_frame#N` remained at zero** across four
distinct configurations.

Either:
1. A timing/ordering subtlety we cannot see from command-level trace
2. A mac80211-internal mechanism (TSF sync, IRQ routing, frame
   classification) that the chip firmware relies on even though it
   doesn't manifest as a chip command
3. The firmware path for mesh RX on `mm6108.mbin` v1.17.6 has a
   dependency satisfied somewhere outside the chip ABI

Whatever this gate is, it is not addressable from the host command
surface that the GPL driver exposes.

## Additional commands tried (added 2026-05-25 step15/16)

Cross-referenced the GPL Linux `morse_driver/command.c` for every chip
opcode it issues and added the ones our embedded port wasn't sending:

| Opcode | Name | We tried |
|---|---|---|
| `0x0052` | `MORSE_CMD_ID_BSSID_SET` | with `bssid = ff:ff:ff:ff:ff:ff` and with `bssid = own_addr` — chip ACCEPTED both (status=0) |
| `0x0010` | `MORSE_CMD_ID_SCAN_CONFIG` | with `enabled=1` — chip returned `-17 (EEXIST)`, presumably already in target state for mesh mode |

Neither opened the chip→host RX path. The
`mmdrv_host_process_rx_frame` counter at the chip-shim boundary
remained at zero across multi-minute monitor windows in both
configurations, with both boards transmitting our full
mesh-beacon template (Mesh ID IE 114, Mesh Configuration IE 113,
S1G Capabilities IE 217, populated rate-control table).

## Definitive RX-wall experiment (added 2026-05-25)

Built a complete S1G mesh beacon constructor in our embedded port:

- 802.11 MAC header (broadcast addr1, own_addr addr2/3 = BSSID)
- Fixed fields (timestamp=0, beacon_interval=100 TU, capability_info)
- Mesh ID IE (element 114) with mesh_id payload
- Mesh Configuration IE (element 113, 7 B fixed)
- S1G Capabilities IE (element 217, via `ie_s1g_capabilities_build`)
- Rate-control table populated via `umac_rc_init_rate_table_mgmt`

Verified:
- `mmdrv_host_get_beacon` fires on first call (chip asks once during
  MESH_CONFIG(START) processing)
- Constructor returns a properly-tagged mmpkt with metadata
- Chip accepts the template and presumably TXes at beacon interval

To isolate the RX wall, we then added `MMLOG_ERR` at the very top of
`mmdrv_host_process_rx_frame` in `umac_mmdrv_shim.c` — the absolute
first chip→host RX entry point, upstream of `umac_core_is_running`,
`umac_datapath_rx_frame_filter`, the datapath ops gate, and every
other host filter.

**Result:** with two boards <2 feet apart, identical config (channel 1,
CSSID 0x46e93c69, mesh_id "warthog-mesh-test"), both transmitting our
proper mesh beacon template, the counter on both boards stayed at
**zero** across a 3-minute monitor window. **Not a single frame
crossed the chip→host boundary in mesh mode**, despite the same
hardware delivering STA-mode RX flawlessly.

This isolates the RX failure to the chip firmware's internal RX path,
before any morselib code runs. The host stack is verified ready to
consume any frame the chip provides — there is no host filter that
could be silently dropping these.

## What we've ruled out

- Not a host-side wiring issue — full datapath ops, supplicant context,
  hostap mesh_mpm linkage, peer-event hooks, all built and stable; the host
  is ready to consume RX frames from the moment the chip provides them.
- Not RF — STA mode on the same hardware works at the same range with the
  same antennas.
- Not the 22 `MORSE_CMD_PARAM_ID_*` keys morselib's internal enum exposes
  (the chip-side enum extends to 30, including
  `MORSE_CMD_PARAM_ID_BEACON_OFFLOAD = 24` and
  `MORSE_CMD_PARAM_ID_PROBE_RESP_OFFLOAD = 25`, which is what
  question 2 asks about).
- Not channel — `umac_interface_set_channel_from_regdb` returns SUCCESS and
  the chip reports operating on chan=1 1MHz.
- Not the missing `mmdrv_start_beaconing(mesh_vif_id)` call —
  we will test this independently before sending this inquiry. If
  that single call flips the chip from silent to asking the host
  for beacons, question 1's first branch is the answer and we
  can build a mesh beacon constructor ourselves.

## Branch artifacts (if you want to inspect our embedded port)

Open-source firmware at `github.com/<warthog repo>`. The branch
`mesh-support` has the full Phase 4 work (see `docs/mesh-port-scope.md` for
a detailed bring-up log of what worked vs. what didn't).

The relevant call sites in our fork (paths under `components/halow/`):

- Chip cmd sequence: `components/mm-iot-sdk/framework/morselib/src/umac/mesh/umac_mesh.c`
- Diagnostic instrumentation: `components/mm-iot-sdk/framework/morselib/src/umac/datapath/umac_datapath.c` (`rx#N` logging at top of `umac_datapath_rx_frame`)
- Beacon-template plumbing (currently broken for mesh): `components/mm-iot-sdk/framework/morselib/src/umac/umac_mmdrv_shim.c`

Any pointer toward what we're missing would be greatly appreciated.

---

## UPDATE — Steps 23–26 results (2026-05-26)

After this inquiry was first drafted (steps 14–22), we ran four more
experiments using the GPL morse_driver source as ground truth:

### Step 23: S1G beacon format (FC 0x001C, not 0x80)

The Linux `morse_dot11ah` subsystem transforms regular 802.11 beacons
(mac80211-generated) into S1G beacon format (`IEEE80211_FTYPE_EXT |
IEEE80211_STYPE_S1G_BEACON = 0x001C`, compact 15B header) before chip TX.
We rebuilt `umac_mesh_beacon.c` to emit the same S1G format directly
(matches `struct ieee80211_ext` in `s1g_ieee80211.h`, includes S1G_BCN_COMPAT
IE 213 / S1G_CAPABILITIES IE 217 / S1G_OPERATION IE 232 / S1G_SHORT_BCN_INTERVAL
IE 214 alongside Mesh ID 114 + Mesh Config 113).

**Result:** chip accepts the new beacon template (the
`mmdrv_host_get_beacon#1: chip requesting beacon template` callback
fires, and our `umac_mesh_get_beacon` returns the S1G-formatted bytes).
Behavior is otherwise unchanged.

### Step 24: TX-frame + beacon-IRQ counters

Added ERR-level counters at:
- `mmdrv_tx_frame` — counts every chip TX (mgmt vs data)
- `morse_beacon_work_` — counts every beacon IRQ from chip
  (offset `MORSE_INT_BEACON_BASE_NUM(17) + vif_id`)

**Result:** `beacon_irq#1` fires exactly **once** (the manual
`driver_task_notify_event` inside `morse_beacon_start`), and never
again. The chip's HW beacon timer at 100ms TBTT is not asserting the
beacon IRQ.

`mmdrv_tx_frame` is never invoked for any frame (data or mgmt) after
the one-shot beacon dispatch.

### Step 25: MBCA disabled (`mbca_config=0`)

Set `mbca_config=0` in `MORSE_CMD_ID_MESH_CONFIG` to test the hypothesis
that MBCA's TBTT-selection state machine was waiting for peer beacons to
be observed before allowing TX.

**Result:** identical to step 24 — `beacon_irq#1` once, then silence.
MBCA is not the gating factor.

### Step 26: BSSID=broadcast

Set `MORSE_CMD_ID_BSSID_SET` to `ff:ff:ff:ff:ff:ff` instead of own_addr.

**Result:** chip accepts both values; behavior is identical between
own_addr and broadcast. The BSSID we set does not appear to affect
the chip's RX filter (or any other observable chip behavior) in mesh
mode.

### Conclusive observation

The chip accepts every command in the full Linux mesh bring-up sequence
but does not emit:
- TBTT-driven beacon IRQs (beacon_irq stays at 1)
- TX_STATUS notifications for any frames
- Any chip events (unknown-event log at ERR level: zero events)
- Any peer RX frames

**Specific question for Morse support: does firmware
`rel_1_17_6_2026_Feb_23` on MM6108 implement the mesh state machine
end-to-end, or is mesh deferred to a separate firmware build?** If it's
the latter, we'd appreciate any guidance on whether a mesh-functional
MM6108 firmware build exists (publicly or under NDA).

If the answer is "yes, mesh is supported in 1.17.6", we'd appreciate
any pointers to: chip commands beyond `morse_commands.h` that mesh
requires; a known-good chip-command sequence dump from a Linux
mesh bring-up against the same firmware build; or any preconditions
(e.g., regdomain-specific channels, MCS overrides) the chip's mesh
path requires before the beacon timer arms.

The full diagnostic chain is in `docs/mesh-port-scope.md` Phase 4f
Final Report.

---

## UPDATE — Steps 27–31 results (2026-05-26, late session)

After matching Linux's command order, enabling NDP probe support, and
building a periodic broadcast probe-request loop, we now have CONCRETE
evidence that the chip's TX path is functional for mesh — but the
chip's RX path is the blocker.

### What works conclusively now

1. **`mmdrv_tx_frame`** is invoked successfully every 2 seconds with a
   broadcast probe request containing the mesh_id as SSID. Returns 0
   from chip layer. Confirmed by ESP_LOGW from the call site after
   discovering that `MMLOG_ERR` silently drops from non-umac task
   contexts (`mmosal_printf` → `vprintf` interaction with ESP-IDF
   logger).
2. **`mmdrv_host_process_tx_status`** fires for the manual probe-req at
   startup — chip reports the TX completed (i.e., went on-air).
3. Step 27 fully matched Linux's chip-command ordering
   (QoS×4 → BSS_BEACON_CONFIG → MESH_CONFIG START → BSSID_SET →
   BSS_CONFIG). No change in chip behavior — order isn't the issue.
4. Step 28 enabled NDP probe support (chip opcode
   `MORSE_CMD_ID_SET_NDP_PROBE_SUPPORT = 0x800C`). Chip accepted; no
   change.

### What still doesn't work

After 80+ seconds of 2-second-interval broadcast probe-request bursts
from BOTH boards (40+ from each, all confirmed accepted by chip):

- **Zero `mmdrv_host_process_rx_frame#N`** logs on either board — the
  chip→host RX shim never fires. Both boards are configured for the
  same mesh on the same chan=1 1MHz, sitting ~10cm apart on the same
  USB hub.
- **Zero `Mac EVT 0x____`** unknown events at ERR level.
- **Zero `beacon_irq#N`** firings after the initial manual trigger at
  startup.

### Specific updated questions for Morse support

Given the new evidence above:

1. **Does the MM6108 firmware `rel_1_17_6_2026_Feb_23` open the RX
   path for foreign-BSSID mgmt frames in mesh mode?** We've confirmed
   TX works (TX_STATUS fires) but no peer board ever sees our probes.
   The chip seems to filter inbound frames much more aggressively in
   mesh mode than in AP/STA. Is there a chip command beyond
   `BSSID_SET(broadcast)` to disable Addr3 filtering for the mesh VIF?

2. **Does the chip's mesh-mode beacon timer normally fire its HW IRQ
   (`MORSE_INT_BEACON_BASE_NUM + vif_id` = bit 17 for VIF 0)?** We
   only see the initial manual trigger (from `morse_beacon_start`'s
   own `driver_task_notify_event`), then silence — no chip-driven IRQs
   afterwards. AP mode presumably fires this each TBTT for beaconing.
   Is the chip's mesh-mode TBTT timer using a different IRQ, or is
   beaconing entirely chip-internal once the template is captured?

3. **Is `MMLOG_ERR` from non-umac task contexts expected to silently
   drop?** Separate issue but worth flagging — `mmosal_printf` via
   `vprintf` works from the umac task, fails from a user task created
   via `xTaskCreate`. Maybe a logger-init / TLS issue.

The host-driven probe-request loop ("mesh_beaconless_mode" equivalent)
is now running and tested. If Morse can point us at the missing piece
to open chip RX for mesh, peering should immediately start working
(hostap mesh_mpm is loaded, supplicant has the interface, datapath ops
are mesh-aware).
