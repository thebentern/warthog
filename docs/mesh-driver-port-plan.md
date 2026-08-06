# Mesh Driver Port Plan — Warthog v0.x → "halow_mesh_compat"

> Concrete plan for porting the Linux GPL `morse_driver` to Warthog as a
> parallel mesh-capable code path. Written after Phase 4f-step33 confirmed
> the SDK's fullmac firmware does not expose mesh RX via any host-side
> chip command in the documented 0x0000–0x004F opcode range.

## Why this exists

After 33 iterations of chip-command-level experimentation (`docs/mesh-port-scope.md`
Phase 4f, all completed tasks #14–52), we definitively established:

- ✅ Chip TX works in mesh mode (probe requests go on-air, TX_STATUS fires)
- ✅ Every chip command Linux issues for mesh, we issue with byte-for-byte
  parity (steps 17, 18, 25, 26, 27, 28, 32)
- ✅ S1G beacon constructor produces correctly-formatted frames
- ✅ NDP probe support enabled (step 28)
- ✅ Shared mesh BSSID derived from CRC32(mesh_id) — both peers identical (step 32)
- ✅ Periodic broadcast probe-request burst running (step 30, mesh_beaconless
  mode equivalent)
- ✅ Opcode-space probe completed (step 33) — no undocumented opcodes
- ❌ **Chip never delivers peer mgmt frames to host** in mesh mode

The chip firmware (`rel_1_17_6_2026_Feb_23`, format `mm6108.mbin` MMFW
container) has the mesh code paths compiled in (FreeRTOS task names
`mesh_tbtt` and `mesh_delayed_start` visible in strings), but its RX
filter for foreign-BSSID mgmt frames in mesh mode does not open via any
documented chip command.

The Linux driver succeeds at mesh because:
1. It uses a **different firmware variant** (`mm6108.bin` or `mm6108-tlm.bin`
   in ELF format, ~480KB) than the SDK's `mm6108.mbin` (MMFW + deflate, 399KB)
2. That firmware variant is "softmac" or "thin-LMAC" — the chip is a
   permissive RX bridge to the host, all MAC state machines run in
   mac80211 on the host

## The hard prerequisite

**This port cannot succeed without a Morse-Micro-provided MMFW-format
firmware that has open mesh RX** — either:

- (a) A mesh-capable build of the fullmac MMFW (chip-side mesh state
  machine completed, RX filter relaxed for mesh BSS frames), or
- (b) An MMFW-wrapped version of the public Linux `mm6108-tlm.bin` so the
  SDK loader can load the thin-LMAC firmware, or
- (c) Documentation from Morse Micro of an undocumented chip command that
  relaxes RX filter in mesh mode (we tried all 256 opcodes in 0x0000-0x00FF,
  none unlocked RX)

**Without one of those, no amount of host driver code will help.** The
chip is the gate. The Linux morse_driver + mac80211 is ~100k LOC of code
that produces the same chip behavior we already have on the same firmware.

## With the prerequisite met — what porting actually means

If/when Morse provides the firmware, two paths:

### Path A — Minimal port (preferred)

**Goal:** keep morselib for everything STA + AP, add a parallel mesh
codepath that issues the right chip commands and consumes chip RX.

**Scope:** ~3,000–5,000 LOC of new Warthog code. Vendor in the dot11ah/
subsystem from Linux. Implement mesh peering on top of our existing
hostap mesh_mpm.

**File list to vendor from `/tmp/morse_driver`:**

| File | Linux LOC | Notes |
|---|---:|---|
| `dot11ah/main.c` | 367 | regdom + channelization |
| `dot11ah/tx_11n_to_s1g.c` | 763 | S1G beacon + frame construction |
| `dot11ah/rx_s1g_to_11n.c` | 1,246 | S1G → 11n RX transform |
| `dot11ah/ie.c` | 517 | IE parser |
| `dot11ah/tim.c` | 563 | TIM bitmap handling |
| `dot11ah/s1g_channels.c` | 764 | S1G channel rules |
| `dot11ah/s1g_channels_rules.c` | 290 | regdom rules |
| `dot11ah/reg.c` | 112 | regulatory init |
| `dot11ah/reg_rules.c` | 219 | regdom enforcement |
| `dot11ah/s1g_ieee80211.c` | 54 | S1G frame predicates |
| `dot11ah/debug.c` + headers | ~500 | logging + types |
| **dot11ah/ total** | **~5,400** | mostly portable |
| `mesh.c` (driver glue) | 592 | mesh state, probe-timer, MBCA |
| Selected chunks of `mac.c` | ~800 | bss_info_changed handlers, mesh path |
| Selected chunks of `command.c` | ~400 | mesh + cfg_bss helpers |
| **Total to port** | **~7,200 LOC** | |

**Linux API → ESP-IDF/FreeRTOS mapping:**

| Linux API | Warthog substitute | Notes |
|---|---|---|
| `kmalloc/kfree(GFP_*)` | `heap_caps_malloc/free` | Pass `MALLOC_CAP_DMA \| MALLOC_CAP_8BIT` for DMA buffers |
| `kzalloc` | `calloc` | |
| `kfree_skb / dev_kfree_skb_any` | `mmpkt_release` | Already have mmpkt = skb |
| `struct sk_buff *` | `struct mmpkt *` | Already used everywhere in morselib |
| `spinlock_t / spin_lock_bh / spin_unlock_bh` | `MMOSAL_MUTEX_GET_INF / RELEASE` | Mutex sufficient at RTOS task layer |
| `mutex_lock / mutex_unlock` | `mmosal_mutex_*` | Direct map |
| `struct timer_list` + `mod_timer` | `xTimerCreate` / `xTimerStart` | Or `vTaskDelay` in dedicated task |
| `INIT_DELAYED_WORK` + `schedule_delayed_work` | `xTaskCreate` + `vTaskDelay` | Or `esp_timer_*` |
| `tasklet_init/schedule` | Drop; run inline in task context | Tasklets are softirq, embedded already runs in task context |
| `printk / KERN_*` | `ESP_LOGI / ESP_LOGE / ESP_LOGW` | Already used in morselib |
| `BIT(n) / BIT_ULL(n)` | `(1u << n) / (1ULL << n)` | Header copy |
| `le16_to_cpu / cpu_to_le16` | `le16toh / htole16` | Same semantics |
| `cfg80211_chan_def` | Skip — embedded uses regdb directly | We already have `mmwlan_s1g_channel` |
| `ieee80211_hdr / ieee80211_mgmt` | Already in `dot11/dot11.h` | Header definitions match |
| `ieee80211_*_get_template` (mac80211 helpers) | Stub or port | The few helpers needed are pure math |
| `RCU` (`rcu_read_lock` etc) | Drop — single-threaded mutex sufficient | Embedded has no RCU need |
| `<linux/netdev.h>` (skb netif ops) | Map to `esp_netif` | We have `esp_netif` for this |
| `module_param` | Drop — use NVS for runtime config | |
| `MODULE_FIRMWARE` etc | Drop — firmware is built into image | |
| `debugfs` | Drop or use AT cmd surface | |

**Integration points with morselib:**

- `mmdrv_execute_command()` — already exists, raw opcode submission path
- `mmdrv_host_process_rx_frame()` — hook to receive frames; new code path
  branches on `vif_id` to mesh handler vs existing AP/STA path
- `umac_datapath_configure_mesh_mode()` — already exists, install our
  mesh datapath ops table
- Existing `hostap/wpa_supplicant/mesh.c` — keep as-is; we already
  call its `passive_init_ifmsh` from `supplicant_core_mesh.c`

### Path B — Full port (if Path A's host glue insufficient)

**Goal:** replace morselib entirely with a Linux-driver-derived stack.

**Scope:** ~40,000 LOC. Multi-engineer-year.

**Not recommended** because:
- License conflict (GPL-2.0+ vs Apache-2.0)
- morselib's chip-management layer (SPI bus, IRQ, firmware loader, pageset
  packetization) is well-understood, well-tested, and doesn't need
  replacing
- Most of morse_driver's 44k LOC is non-mesh code (TWT, RAW, hw_scan,
  beacon, pv1, etc.) that we either don't need or already have working

## License implications

`morse_driver` is **GPL-2.0-or-later**. Vendoring its source into Warthog
means:

1. **Vendored files must remain GPL-2.0+.** No mixing license headers
   on those files.
2. **The combined Warthog firmware binary** that links libmorse +
   GPL-derived halow_mesh_compat is GPL-2.0+ as a whole, per the FSF's
   standard kernel-module-derivation argument.
3. **Apache-2.0 components** (morselib, hostap) typically allow this
   composition (Apache-2.0 is GPL-compatible).
4. **Warthog's own LICENSE** would need to change from the current
   Apache-2.0 (or whatever it is) to GPL-2.0+ for the mesh build, or
   dual-license the project, or build mesh as an optional `-DWARTHOG_MESH=ON`
   compile-time variant.

**Recommendation:** dual-license. Keep the STA-only build under permissive
license; ship the mesh build under GPL-2.0+. PlatformIO envs make this
straightforward (`-DWARTHOG_BUILD_TYPE=mesh` vs default).

## Concrete next steps (when prerequisite met)

1. **Get a mesh-enabled MMFW firmware from Morse Micro support** (filed
   ticket; see `docs/morse-support-inquiry.md`)
2. **Create `components/halow_mesh_compat/` directory** with this
   structure:
   ```
   halow_mesh_compat/
     CMakeLists.txt
     Kconfig
     LICENSE (GPL-2.0+, copied from morse_driver)
     include/halow_mesh.h           (warthog public API)
     src/dot11ah/                    (vendored, GPL-2.0+)
     src/mesh/mesh_compat.c          (driver-side mesh glue, ported)
     src/mesh/mesh_compat_cmd.c     (mesh-specific chip cmds, ported)
     src/shim/linux_api_shim.h      (kmalloc → esp32 etc)
     src/shim/mac80211_stub.c       (mac80211 callbacks → our umac)
   ```
3. **Wire `halow_mesh_compat` into existing umac_mesh.c**:
   - Replace `umac_mesh_enable_mesh()` chip-command sequence with
     `halow_mesh_compat_enable()`
   - Route chip→host RX frames marked as mesh-VIF to the compat
     RX handler (which runs dot11ah/rx_s1g_to_11n then forwards to
     mesh_mpm)
4. **Test** with the new firmware on both boards
5. **If still broken**, log a second support ticket — the host-side
   work would be done at that point

## Estimate

Assuming Morse provides mesh-enabled firmware:

- **Path A scaffold + dot11ah port + minimal mesh.c port + shim:**
  3-4 weeks single-developer
- **Path B full port:** 18-24 months small team

Path A is the realistic plan. Path B is "if Morse Micro shuts down."

## What we ship today

Without the firmware prerequisite, Warthog v0.x ships **STA-only HaLow
bridging** (the original Phase 1-4 goal):
- USB RNDIS/ECM to host
- Wi-Fi AP for downstream clients
- HaLow STA uplink with NAPT
- 7+ working features, ~100% of the original product spec

Mesh remains documented as a future capability gated on Morse Micro
delivering the chip-firmware piece. All the host-side scaffolding
(periodic probe burst, S1G beacon constructor, hostap mesh_mpm,
supplicant mesh interface, datapath ops, shared BSSID derivation) stays
in the codebase under `WARTHOG_MESH_SMOKE` build flag — ready to
activate the moment chip RX opens.
