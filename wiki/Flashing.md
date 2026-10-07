# Flashing

> **Easiest route: the [web flasher](https://thebentern.github.io/warthog/).**
> Chrome, Edge or Opera, nothing to install. Pick a release build or load a
> `.bin` you built, do the BOOT/RESET dance, and flash. It also configures a
> running board over the same cable. It writes a `.factory.bin` at `0x0`, which
> erases the stored settings ([below](#from-a-release-bundle)).

## Why the usual auto-reset does not work

The Warthog firmware runs its console and USB network on USB-OTG (TinyUSB)
rather than the built-in USB-Serial-JTAG, so `esptool` cannot pull the board
into download mode over DTR/RTS. Every flash needs the button sequence, or
`AT+DLMODE` on a running board ([below](#reflashing-a-running-board)):

**Hold BOOT → tap RESET → release BOOT.**

The board then enumerates as a ROM device (USB `303a:0009`) and stays there
until you flash or reset it.

## From source

```bash
pio run -e warthog-us -t upload
pio device monitor -e warthog-us
```

Tap **RESET** when the write completes. The upload writes the bootloader,
partition table and app separately and keeps the stored settings.

## From a release bundle

Releases carry per-region binaries and a POSIX flasher:

```bash
./flash.sh warthog-v0.1.0-us.factory.bin
```

A `.factory.bin` written at `0x0` is `0xFF` from `0x9000` to `0xFFFF`, which
covers the NVS partition. It erases every stored setting (HaLow credentials, AP,
mesh ID, passphrase, channel, `AT+MESHEN` and the rest), and the board comes up
on its build defaults. To keep the settings, write the three pieces instead.

| File | Offset | Use |
|---|---|---|
| `*.factory.bin` | `0x0` | Everything: bootloader + partitions + app. Also erases the stored settings |
| `*.bin` | `0x10000` | App only, e.g. for OTA |
| `*-bootloader.bin` / `*-partitions.bin` | `0x0` / `0x8000` | Piecewise flashing; with `*.bin`, keeps the stored settings |
| `*.elf` | — | Symbols, for `addr2line` on a panic backtrace |
| `SHA256SUMS.txt` | — | Covers every asset |

## Reflashing a running board

No buttons, while the firmware's CDC console answers `AT`:

1. Send `AT+DLMODE`. The board resets into the ROM bootloader and
   re-enumerates as `303a:0009`.
2. Flash that port and leave with a watchdog reset:

```bash
python -m esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX --baud 921600 \
  --before no-reset --after watchdog-reset write-flash 0x0 .pio/build/<env>/firmware.factory.bin
```

`AT+DLMODE` sets the ROM's force-download flag, which survives a chip reset:
boards were repeatedly found still in download mode after resets (boot log
`rst:0x15 (USB_UART_CHIP_RESET)`), and the watchdog reset clears it and boots
the new image. Used on the bench on 2026-09-29/30; the same esptool line boots a
board found sitting in download mode. `tools/bench/flash.sh` resets with
`--after hard-reset` and then power-cycles the hub port.

`AT+DLMODE` resets the HaLow chip first, so the node goes off the air, and arms
the RTC watchdog: a board that no reset takes out of download mode returns to
the app 1800 s after the command (`AT+ASSERT?` `reset=DLMODE`). esptool does not
stop that watchdog on the USB-OTG download port, so the whole session must end
in a reset before it fires. For a longer session enter download mode with BOOT
and RESET, which has no limit. The `warthog-mesh-*` builds (`WARTHOG_DEVLOOP`)
enter download mode the same way when the console is opened at 1200 baud.

The `.factory.bin` write above erases the stored settings. To keep them, write
the pieces with the same flags:

```bash
python -m esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX --baud 921600 \
  --before no-reset --after watchdog-reset write-flash \
  0x0 .pio/build/<env>/bootloader.bin 0x8000 .pio/build/<env>/partitions.bin \
  0x10000 .pio/build/<env>/firmware.bin
```

These are the three images `pio run -t upload` writes. The bench reflashed boards
this way after `AT+DLMODE` from 2026-09-30 to 2026-10-03, and their stored
settings survived.

## Several boards at once

`tools/bench/flash.sh` flashes a bench of boards in sequence. It finds whichever
CDC port of a board actually answers `AT` (boards enumerate more than one), drops
it into download mode over the wire with `AT+DLMODE`, flashes, then power-cycles
that hub port. Arguments are `"SERIAL HUB PORT"` triples:

```bash
tools/bench/flash.sh "WTHG-0272A1F8738D 0-1 1" "WTHG-021BF681BA51 0-1 2"
```

It settles 20 s between boards deliberately — flashing two at once through one
hub browns them out. Requires `uhubctl`. It writes
`.pio/build/$WARTHOG_ENV/firmware.factory.bin` (default `warthog-mesh-smoke`) at
`0x0`, so every board it flashes loses its stored settings.

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

This erases the stored settings too; the piecewise write
[above](#reflashing-a-running-board) keeps them.

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
