#!/usr/bin/env bash
# Auto-attach to whichever serial device is currently enumerated
# (macOS /dev/cu.usbmodem*, Linux /dev/ttyACM* and /dev/ttyUSB*).
# Reconnects when the port disappears, so a boot loop or a USB-OTG handoff
# (USB-Serial-JTAG -> our CDC) doesn't make you miss the log.
#
# Usage: ./scripts/watch.sh

set -u

last_port=""
while true; do
    # Match only digit-suffixed usbmodem ports: the ESP32-S3 USB-Serial-JTAG
    # (usbmodem<location>, e.g. usbmodem11401) and our TinyUSB CDC
    # (usbmodem0001). Excludes junk like a hub's usbmodemSN... gadget, which
    # watch.sh would otherwise latch onto and cat forever (it never goes away,
    # so the rescan loop never runs again).
    port=$(ls /dev/cu.usbmodem* 2>/dev/null | grep -E 'usbmodem[0-9]+$' | head -1)
    [ -n "$port" ] || port=$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1)
    if [[ -n "${port}" && -e "${port}" ]]; then
        if [[ "${port}" != "${last_port}" ]]; then
            printf '\n\033[33m=== %s connected to %s ===\033[0m\n' "$(date +%H:%M:%S)" "${port}"
            last_port="${port}"
        fi
        # CDC + USB-Serial-JTAG both ignore the host baud, but stty raw avoids
        # cooked-tty buffering swallowing partial lines.
        stty -f "${port}" raw 115200 2>/dev/null || true
        cat "${port}" 2>/dev/null
        printf '\n\033[31m=== %s %s gone ===\033[0m\n' "$(date +%H:%M:%S)" "${port}"
        last_port=""
    fi
    sleep 0.1
done
