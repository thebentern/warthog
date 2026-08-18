# Flashing

## Why the usual auto-reset does not work

The XIAO plus HaLow add-on drives USB-OTG rather than the built-in
USB-Serial-JTAG, so `esptool` cannot pull the board into download mode over
DTR/RTS. Every flash needs the button sequence:

**Hold BOOT → tap RESET → release BOOT.**

The board then enumerates as a ROM device (USB `303a:0009`) and stays there
until you flash or reset it.

## From source

```bash
pio run -e warthog-us -t upload
pio device monitor -e warthog-us
```

Tap **RESET** when the write completes.

## From a release bundle

Releases carry per-region binaries and a POSIX flasher:

```bash
./flash.sh warthog-v0.1.0-us.factory.bin
```

| File | Offset | Use |
|---|---|---|
| `*.factory.bin` | `0x0` | Everything: bootloader + partitions + app |
| `*.bin` | `0x10000` | App only, e.g. for OTA |
| `*-bootloader.bin` / `*-partitions.bin` | — | Piecewise flashing |
| `*.elf` | — | Symbols, for `addr2line` on a panic backtrace |
| `SHA256SUMS.txt` | — | Covers every asset |

## Several boards at once

`tools/bench/flash.sh` flashes a bench of boards in sequence. It finds whichever
CDC port of a board actually answers `AT` (boards enumerate more than one), drops
it into download mode over the wire with `AT+DLMODE`, flashes, then power-cycles
that hub port. Arguments are `"SERIAL HUB PORT"` triples:

```bash
tools/bench/flash.sh "WTHG-0272A1F8738D 0-1 1" "WTHG-021BF681BA51 0-1 2"
```

It settles 20 s between boards deliberately — flashing two at once through one
hub browns them out. Requires `uhubctl`.

## Recovering a board that will not enumerate

Hold **BOOT**, tap **RESET**, release, and check it appears as a ROM device:

```bash
uhubctl -l 0-1 | grep 303a:0009
```

If it does, flash directly:

```bash
python -m esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX --baud 921600 \
  --before no-reset --after hard-reset write_flash 0x0 firmware.factory.bin
```

## Toolchain traps

**`pio run -t clean` can remove `tool-esptoolpy`.** Symptom: the next build fails
with `Distribution not found at: .../tool-esptoolpy` and
`ModuleNotFoundError: No module named 'esptool'`. Recover with:

```bash
pio pkg install -e warthog-us
```

**A reconfigure can surface stale link errors.** If a build fails with undefined
references in `libwpa_supplicant` or a missing IDF header right after you add a
source file, clean once and rebuild — the second pass regenerates what the
reconfigure invalidated.
