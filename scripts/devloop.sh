#!/usr/bin/env bash
# Warthog dev loop: build → trigger ROM download → flash → cold-cycle → verify AT.
#
# Captures the working sequence validated in 2026-05-25 session (see Phase 5a in
# docs/mesh-port-scope.md). Assumes:
#
#   - The board(s) are running firmware that has the cdc_line_coding_cb 1200bps
#     watchdog AND/OR the AT+DLMODE command. If you're flashing a board that
#     LACKS both, you still need the manual BOOT+RESET dance once.
#   - uhubctl is installed and the Plugable USBC-HUB7BC is connected.
#   - pyserial is available via PlatformIO's venv at:
#         $HOME/.platformio/penv/bin/python3
#
# Usage:
#   scripts/devloop.sh                              # both boards, default env
#   scripts/devloop.sh --env warthog-mesh-smoke     # specify env
#   scripts/devloop.sh --board A                    # single board (A or B)
#   scripts/devloop.sh --skip-build                 # don't rebuild, just flash
#   scripts/devloop.sh --skip-verify                # don't AT-poll after cycle
#
# Board ↔ hub mapping (Plugable USBC-HUB7BC, validated 2026-05-25):
#   A → hub 0-1.3 port 1
#   B → hub 0-1   port 2

set -euo pipefail

ENV="warthog-mesh-smoke"
BOARDS="A B"
SKIP_BUILD=0
SKIP_VERIFY=0
# Derive everything from this script rather than one developer's checkout.
PIO="${PIO:-$HOME/.platformio/penv/bin/pio}"
PY="${PIO_PYTHON:-$HOME/.platformio/penv/bin/python3}"
PROJECT="${PROJECT:-$(cd "$(dirname "$0")/.." && pwd)}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --env) ENV="$2"; shift 2 ;;
        --board) BOARDS="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --skip-verify) SKIP_VERIFY=1; shift ;;
        *) echo "usage: $0 [--env <env>] [--board A|B] [--skip-build] [--skip-verify]" >&2; exit 2 ;;
    esac
done

hub_for() {
    case "$1" in
        A) echo "0-1.3 1" ;;
        B) echo "0-1 2" ;;
        *) echo "unknown board: $1" >&2; exit 2 ;;
    esac
}

# Find a board's current CDC port (running firmware) by querying AT+VERSION? on
# each present /dev/cu.usbmodem* and reporting which one responds. Returns the
# port path on stdout, or empty string if none found. (Bootloader ports are
# silent on AT-mode probes — caller should treat empty as "in bootloader".)
find_cdc_port() {
    for p in /dev/cu.usbmodem*; do
        [[ -e "$p" ]] || continue
        if "$PY" -c "
import serial, time, sys
try:
  s = serial.Serial('$p', 115200, timeout=1.0)
  time.sleep(0.2); s.write(b'AT+VERSION?\r\n'); s.flush(); time.sleep(0.3)
  out = s.read_all().decode(errors='replace')
  s.close()
  sys.exit(0 if '+VERSION' in out else 1)
except Exception: sys.exit(2)
" 2>/dev/null; then
            echo "$p"
            return 0
        fi
    done
    return 1
}

# Trigger ROM download mode via 1200bps line-coding touch. The firmware's
# cdc_line_coding_cb watches for baud==1200 and sets FORCE_DOWNLOAD_BOOT before
# esp_restart(). After this, the chip's USB-Serial-JTAG bootloader takes over
# with a different USB serial → port renames (e.g. usbmodem00011 → usbmodem1201).
touch_1200() {
    local port="$1"
    echo "[devloop] 1200bps touch → $port"
    "$PY" -c "
import serial, time
s = serial.Serial('$port', 1200, timeout=1)
time.sleep(0.5)
s.close()
" 2>&1 || true
}

build() {
    if [[ $SKIP_BUILD -eq 1 ]]; then
        echo "[devloop] skipping build"
        return 0
    fi
    echo "[devloop] building $ENV"
    cd "$PROJECT" && "$PIO" run -e "$ENV" >/dev/null 2>&1
}

# Returns the JTAG bootloader port for a freshly-touched board. The chip
# enumerates as USB-Serial-JTAG with a serial derived from chip MAC, which on
# macOS produces ports like /dev/cu.usbmodem1201 or /dev/cu.usbmodem13101.
# We identify it by comparing to the pre-touch port list.
wait_for_bootloader() {
    local pre_ports="$1"
    local timeout_s="${2:-10}"
    local elapsed=0
    while [[ $elapsed -lt $timeout_s ]]; do
        for p in /dev/cu.usbmodem*; do
            [[ -e "$p" ]] || continue
            # If this port wasn't in pre_ports, it's the bootloader.
            if ! grep -q "^${p}$" <<<"$pre_ports"; then
                # Confirm esptool can connect (handshake = bootloader)
                if "$PY" -m esptool \
                       --chip esp32s3 --port "$p" chip_id >/dev/null 2>&1; then
                    echo "$p"; return 0
                fi
            fi
        done
        sleep 0.5
        elapsed=$((elapsed + 1))
    done
    return 1
}

flash_one() {
    local board="$1"
    echo "[devloop] === board $board ==="

    # 1. Identify the current CDC port (if board is running firmware).
    local pre_ports
    pre_ports=$(ls /dev/cu.usbmodem* 2>/dev/null || true)
    local cdc_port
    cdc_port=$(find_cdc_port || true)

    # 2. Trigger bootloader entry. Prefer 1200bps touch if firmware is up.
    if [[ -n "$cdc_port" ]]; then
        touch_1200 "$cdc_port"
        sleep 1
    else
        echo "[devloop] no CDC port responding — assuming already in bootloader"
    fi

    # 3. Find the JTAG bootloader port.
    local jtag_port=""
    for p in /dev/cu.usbmodem*; do
        [[ -e "$p" ]] || continue
        # Bootloader ports DON'T respond to AT; firmware ones DO.
        if ! "$PY" -c "
import serial, time
s = serial.Serial('$p', 115200, timeout=0.5)
time.sleep(0.1); s.write(b'AT\r\n'); s.flush(); time.sleep(0.2)
out = s.read_all().decode(errors='replace')
s.close()
exit(0 if 'OK' in out else 1)
" 2>/dev/null; then
            jtag_port="$p"
            break
        fi
    done
    if [[ -z "$jtag_port" ]]; then
        echo "[devloop] could not find JTAG bootloader port — manual BOOT+RESET needed" >&2
        return 1
    fi
    echo "[devloop] bootloader on $jtag_port — flashing"

    # 4. Flash with --after no_reset (avoid RTC bit re-arming download mode).
    "$PY" -m esptool \
        --chip esp32s3 --port "$jtag_port" --baud 460800 --after no_reset \
        write_flash 0x0 \
        "$PROJECT/.pio/build/$ENV/firmware.factory.bin" >/dev/null

    # 5. Power-cycle via uhubctl with a clean 5s delay (drains VBUS for sure).
    read -r loc port <<<"$(hub_for $board)"
    echo "[devloop] cycling hub $loc port $port"
    uhubctl -l "$loc" -p "$port" -a cycle -d 5 >/dev/null

    # 6. Verify AT comes back up.
    if [[ $SKIP_VERIFY -eq 0 ]]; then
        sleep 5
        local new_cdc
        new_cdc=$(find_cdc_port || true)
        if [[ -n "$new_cdc" ]]; then
            echo "[devloop] board $board OK on $new_cdc"
        else
            echo "[devloop] board $board did not come back — manual check required" >&2
            return 1
        fi
    fi
}

build
for b in $BOARDS; do
    flash_one "$b"
done
echo "[devloop] done."
