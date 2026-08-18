# Softmac mesh port — status & roadmap ()

Living tracker for porting the Linux GPL `morse_driver` softmac + mac80211
mesh stack so the MM6108 can run as a softmac radio (thin-LMAC firmware) with
host-side 802.11s mesh. Updated as each unit lands.

## Why this approach

The MM6108 **fullmac** firmware + morselib (what the warthog STA build uses)
cannot do mesh — confirmed by 58 phases of investigation plus Morse Micro's
own docs (their ESP32 SDK is STA-only; mesh is a softmac/mac80211 feature).
See `../../docs/history/mesh-port-scope.md`.

The softmac route: load thin-LMAC firmware (chip becomes a "dumb" radio that
delivers raw 802.11 frames), and run the MAC + mesh logic on the ESP32 host —
i.e. port the parts of `morse_driver` + `net/mac80211/mesh*` that matter.

## Architecture (target)

```
warthog app  ── halow_mesh.h ──►  mesh state machine (plink / hwmp subset)
                                        │
                                        ▼
                                 dot11ah  (S1G ⇄ 11n frame transform)
                                        │
                              raw-frame TX/RX bridge
                                        │  (over morselib transport)
                                        ▼
                           thin-LMAC firmware on MM6108
```

## Methodology (proven)

1. Kernel-compat headers under `src/compat/{linux,net}` resolve the vendored
   GPL sources' `<linux/...>` / `<net/...>` includes onto ESP-IDF/FreeRTOS.
2. Driver-private headers (`src/morse.h`, later `src/mesh.h` etc.) stand in
   for the upstream driver's internal structs — grown per unit.
3. Each vendored `.c` is compiled **and host-unit-tested** (`test/Makefile`,
   pure logic, no hardware) before it ever enters the firmware build.

This vertical-slice-first approach is **validated**: see "Done" below.

## Status

### ✅ Done & verified (host-compiled + unit-tested)

- **Kernel-compat header layer** (`src/compat/`):
  `linux/{types,kernel,bitfield,bitops,list,slab,crc32,string,version,device,
  errno,spinlock,mutex,workqueue,printk,jiffies,skbuff,etherdevice,if_ether}.h`,
  `asm/{byteorder,unaligned}.h`, `net/{cfg80211,mac80211}.h`, and `compat.c`
  (CRC32, channel/freq helpers, `cfg80211_find_ie`, chandef→op-class, jiffies).
- **Wire-exact kernel `<linux/ieee80211.h>`** (v6.6, 5163 lines) integrated
  against the compat shim — all 802.11/S1G structs + predicates.
- **`src/morse.h` / `src/mesh.h`** compat shims — `morse_vif`,
  `custom_configs`, MPM-frame inlines, AMPE/RSN constants, CSSID helper.
- Vendored morse-internal headers: `utils.h`, `s1g_ies.h`, `pv1.h`.
- **dot11ah layer — ALL 11 UNITS PORTED** (~8.5k LOC). Each compiles clean
  against the compat layer; the full set links with **zero duplicate symbols
  and zero genuinely-unresolved references** (verified by `nm`):
  `s1g_ieee80211.c`, `ie.c`, `s1g_channels.c`, `s1g_channels_rules.c`,
  `tim.c`, `reg.c`, `reg_rules.c`, `tx_11n_to_s1g.c` (S1G beacon/frame
  builder), `rx_s1g_to_11n.c` (S1G→11n RX transform), `main.c`, `debug.c`.
- **Mesh peering FSM PORTED** (`src/mesh/mesh_plink.{c,h}`) — a faithful port
  of the 802.11s MPM state machine from kernel `net/mac80211/mesh_plink.c`
  (v6.6): the LISTEN→OPN_SNT→OPN_RCVD→CNF_RCVD→ESTAB→HOLDING transition table
  and the received-frame→event mapping (`mesh_plink_get_event`), reproduced
  transition-for-transition. Restructured for the target: operates on a small
  `struct mesh_peer` + `struct mesh_plink_ctx` and routes every side effect
  (frame TX, peering timers, established/deactivated, llid generation) through
  a `struct mesh_plink_ops` vtable — no `sta_info` / `ieee80211_sub_if_data` /
  mac80211 framework dependency. Self-contained (links against the 802.11
  constant headers + libc only).
- **MPM frame bridge PORTED** (`src/mesh/mesh_mpm_frame.{c,h}`) — serializes
  the FSM's `(action, llid, plid, reason)` tuples into self-protected mesh
  peering action frames and parses received frames back into the receiver's
  local-perspective `(ftype, llid, plid, reason, mesh_id)` — ported from the
  frame layout in kernel `mesh_plink_frame_tx()` + the parsing in
  `mesh_rx_plink_frame()`/`mesh_process_plink_frame()`, including the subtle
  "frame llid is this host's plid" swap. Self-contained (raw bytes, no driver
  state), so it is the portable core of the bridge — only the thin
  hand-bytes-to-the-radio datapath is left (below).
- **Host unit tests green** (`test/`, reproducible via `make`):
  - `test_s1g_ieee80211` — freq↔channel (US/EU S1G + 2.4 GHz).
  - `test_ie_codec` — 15 functional IE parse/find/insert/mesh-detect checks
    through the real `morse_dot11ah_parse_ies` codec.
  - `test_mesh_plink` — 30 checks: full active + passive peering handshake to
    ESTABLISHED, close/teardown back to LISTEN, and the frame→event mapping.
  - `test_mpm_frame` — wire round-trip (OPEN/CONFIRM/CLOSE) **plus a full
    two-node peering handshake driven entirely through serialized+parsed
    frames** (FSM + ser/deser together reach ESTABLISHED with cross-bound link
    ids, then tear down on Close).
  - `linkcheck` — links all 11 dot11ah units + compat (self-contained).

### ⏳ Remaining — the datapath wiring (hardware-coupled + hardware-gated)

All the portable logic is ported and tested. What is left is the thin shell
that touches the chip:

1. **TX datapath** — in `mesh_plink_ops.tx_frame`, take the bytes from
   `mpm_build_frame()`, run them through the dot11ah `tx_11n_to_s1g` transform,
   and hand them to morselib TX. (The frame build is done; this is the
   ~hand-off to the radio.)
2. **RX datapath** — on a received S1G mgmt frame, run `rx_s1g_to_11n`, call
   `mpm_parse_frame()`, compute `matches_local` against the mesh config, and
   call `mesh_plink_rx()`. (The parse is done; this is the chip RX hook.)
3. **Peer lifecycle + timers** — create a `mesh_peer` on neighbour discovery
   (beacon), and back `set_timer`/`del_timer` with FreeRTOS timers.

These couple to `struct morse` (the **277-line driver private state**, via
`morse_vif_to_morse`) and the morselib transport, and — crucially — are gated
on the **hardware keystone** below: until thin-LMAC firmware is confirmed to
deliver raw RX frames over morselib, the RX datapath has nothing to hook, so
this wiring cannot be validated. It is blocked on disconnected hardware.

> **Architecture note.** morse_driver is a *softmac* (mac80211) driver: it
> calls `ieee80211_register_hw` and relies on the kernel mesh FSM. **Three**
> layers are now ported and tested in isolation — the dot11ah S1G↔11n transform
> (the reusable, chip-specific core), the MPM peering FSM (the standard-defined
> state machine), and the MPM frame ser/deser bridge between them. A two-node
> peering handshake runs to ESTABLISHED end-to-end in the host test, through
> built-and-parsed frames. The only remaining piece is the thin datapath that
> hands those bytes to/from the chip over morselib — coupled to the 277-line
> `struct morse` driver core and gated on the hardware keystone below.

### ⛔ Hardware-gated keystone (blocks end-to-end bring-up)

Before the mesh logic can run for real, one experiment must pass on
hardware: **does thin-LMAC firmware, driven over morselib's existing
transport, deliver raw 802.11 RX frames to the host?** (fullmac does not.)
The diagnostic build is ready (`morse_skbq_process_rx` RX-channel histogram
+ `morse_mac_event_recv` event counter, both in the morselib tree). Flash
`warthog-mesh-smoke` on both boards, read the `RXCHAN` / `MACEVT` lines.
- raw frames appear  → the softmac bridge is viable, continue the port.
- still nothing       → thin-LMAC needs a lower-level host protocol than
  morselib speaks, and the bridge must be written against the thin-LMAC
  pager/command ABI directly (larger scope).

This is currently blocked: the boards are physically disconnected.

## Licensing

Vendored `dot11ah` / mesh sources are GPL-2.0-or-later; the compat shim and
warthog glue are released under the same to keep the component consistently
GPL-2.0+. Gated behind `CONFIG_WARTHOG_MESH_COMPAT` (default off) so the
permissive STA-only firmware is unaffected. See `LICENSE`.

## How to work on it

```sh
# host unit tests (no hardware)
cd components/halow_mesh_compat/test && make

# port the next unit: cp from a morse_driver checkout, add its test,
# grow src/compat + src/morse.h until it compiles + tests pass, repeat.
```
