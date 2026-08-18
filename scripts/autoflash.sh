#!/bin/sh
# Flash the mesh-smoke image the moment the XIAO is reachable in ROM download mode.
#
# WHY THIS EXISTS
#   AT+DLMODE (at.c cmd_dlmode) normally works: it sets RTC_CNTL_FORCE_DOWNLOAD_BOOT
#   and calls esp_restart(). But on the currently-flashed MBCA-ON build the restart
#   appears to hang in mesh teardown, so the board comes back into the app instead of
#   the ROM bootloader, and the download window either never opens or is too brief to
#   catch. Recover with the physical dance:
#
#       hold BOOT -> tap RESET -> release BOOT
#
#   then run this script (or run it first and do the dance while it polls).
#
# NOTE: do NOT probe with `esptool chip-id` first. That consumes the transient ROM
#   window -- observed directly: chip-id connected, and the write-flash that followed
#   then failed with "No serial data received". Go straight to write-flash.
cd "$(dirname "$0")/.."
PY="${PIO_PYTHON:-$HOME/.platformio/penv/bin/python3}"
IMG=.pio/build/warthog-mesh-smoke/firmware.factory.bin

[ -f "$IMG" ] || { echo "[autoflash] missing $IMG - build first:"; \
                   echo "  ~/.platformio/penv/bin/pio run -e warthog-mesh-smoke"; exit 1; }

echo "[autoflash] waiting for ROM download mode (hold BOOT, tap RESET, release BOOT)..."
while :; do
  for p in /dev/cu.usbmodem* /dev/ttyACM* /dev/ttyUSB*; do
    [ -e "$p" ] || continue
    if $PY -m esptool --chip esp32s3 --port "$p" --connect-attempts 1 \
         write-flash 0x0 "$IMG" 2>&1 | tee /tmp/autoflash.log | grep -q "Hash of data verified"; then
      echo "[autoflash] FLASHED OK on $p"
      echo "[autoflash] now: uhubctl -l 0-1 -p 2 -a cycle   # cold boot the board"
      exit 0
    fi
  done
  sleep 1
done
