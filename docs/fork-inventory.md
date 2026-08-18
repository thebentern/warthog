# Fork Inventory

What's local, what's upstream, and what each piece exists to work around. Everything we ship in-tree — nothing is silently vendored or patched in a way that's hard to audit later.

## `components/halow/` — local fork of `morsemicro/halow` 2.10.4-esp32-1

**Source:** [https://github.com/MorseMicro/esp-halow](https://github.com/MorseMicro/esp-halow), tag `2.10.4-esp32-1`.

**Why forked:** the registry version doesn't expose the chip's 802.11s mesh mode — it ships only STA + AP shims. We need umac mesh (`umac_mesh.h`/`umac_mesh.c`-style additions) for the multi-board topologies. See `docs/history/mesh-port-scope.md` for the scope analysis and `docs/history/mesh-port-audit.md` for the umac API audit that informed it.

**What's changed relative to upstream:**

- Non-ESP32 BSPs stripped (the registry tarball ships a kitchen sink of Nordic / NXP / STM32 ports we don't need; dropping them shaves the build and keeps the diff focused on ESP32).
- Vendor docs stripped (not load-bearing).
- Mesh additions under `components/halow/components/morselib/` — see `docs/mesh-port-*.md`.

**How it's wired:** `main/idf_component.yml` has:

```yaml
morsemicro/halow:
  version: "2.10.4-esp32-1"
  override_path: "../components/halow"
```

The directory is named `halow` (the dependency short name) so the IDF component graph resolves `main`'s `REQUIRES halow` to this fork rather than to a registry pull.

**Upgrade path:** when Morse Micro tags `2.11.x-esp32-N`, re-vendor the new tarball, strip non-ESP32 BSPs + docs again, replay the mesh additions, and bump the `version:` line. The build script (`pre_build_morselib.py`) patches one CMakeLists line at build time; that patch will need to be re-verified against any morselib reorg upstream.

## `components/firmware/` — thin local IDF component shim (not a fork)

**Why it exists:** `morsemicro/halow`'s shims link against `idf::firmware`, but the registry publishes the binary blobs as `morsemicro/firmware` (note the namespace prefix). IDF's component graph won't resolve `idf::firmware` to a prefixed package automatically, so this 60-line shim component sits in between:

- Pulls the `.mbin` blobs from `managed_components/morsemicro__firmware/` (which the registry populates).
- Exposes them under the bare `firmware` component name so `idf::firmware` resolves.
- Picks the right BCF + firmware file based on `CONFIG_MM_BCF_FILE` / `CONFIG_MM_FW_FILE` / `CONFIG_MMHAL_CHIP_TYPE_*` (Kconfig is a direct copy of upstream's).

**Upstream changes that obsolete this:** if Morse Micro renames the registry package to drop the namespace prefix, or if their shim starts using `idf::morsemicro__firmware` directly, this shim can be deleted.

## `scripts/pre_build_morselib.py` — PlatformIO build hook

**Why it exists:** PlatformIO + ESP-IDF + the `morselib` CMake custom command don't compose cleanly out of the box. Three concrete bugs this hook works around:

1. **PlatformIO's section scanner runs `objdump -h` on every `.a` before CMake's custom commands fire** — so `libmorse.a` doesn't exist yet when scanned, the build aborts. The hook pre-runs `ar -M` + `ranlib` + `librarymangler.py` (the upstream Morse symbol-mangler) so the mangled archive exists before PIO scans.
2. **The `components/firmware/` CMake custom-command for objcopying `.mbin → .o` doesn't fire under PIO**, so the hook does the objcopy itself (same flags, same output object names).
3. **morselib's `IMPORTED` target lacks a dep on the custom command that produces its file** — we patch one line into `components/halow/components/morselib/CMakeLists.txt` (in-tree, with a `# warthog:` marker so it's idempotent) to wrap `libmorse.a` in `-Wl,--whole-archive`. A direct `add_dependencies()` would form a cycle.

The hook is wired in `platformio.ini`:

```ini
extra_scripts = pre:scripts/pre_build_morselib.py
```

**Upstream changes that obsolete this:** if PlatformIO's section scanner learns to run after CMake custom commands, or if Morse Micro restructures their CMake to not need the mangler step, this script can shrink or disappear. The `.mbin → .o` objcopy will likely always be needed under PIO until PIO + IDF custom-command timing is fixed.

## What we don't fork

- `morsemicro/firmware` — pulled from the registry as-is into `managed_components/`. No patches.
- `espressif/esp_tinyusb` — registry, no patches.
- `espressif/tinyusb` — registry, no patches.
- ESP-IDF — pinned via the `pioarduino/platform-espressif32` fork (which itself just upgrades the IDF version from PIO's bundled 5.1.4 to 5.5.4 — no code changes).

## Audit checklist for the next morsemicro/halow upgrade

1. Pull the new tarball into a scratch dir.
2. `diff -r` against `components/halow/` to identify upstream changes vs. our deletions.
3. Re-apply: strip non-ESP32 BSPs, strip docs, re-vendor mesh additions.
4. Verify the morselib `CMakeLists.txt` line that `pre_build_morselib.py` patches still exists at the expected location (search for `set_target_properties(morselib PROPERTIES IMPORTED_LOCATION`).
5. Bump `version:` in `main/idf_component.yml`.
6. Smoke build: `pio run -e warthog-us`.
7. Bench test through the full bench-test checklist in the README.
