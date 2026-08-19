#!/bin/sh
# Regenerate the README terminal screenshots from REAL sessions.
#
# These are captured against attached hardware rather than written by hand, so
# they cannot drift into showing output the firmware no longer produces. If you
# change what these commands print, run this with a board attached.
#
#   sh scripts/regen-screenshots.sh [board-serial-suffix]
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PY="${PIO_PYTHON:-$HOME/.platformio/penv/bin/python}"
SUFFIX="${1:-}"
CAP=$(mktemp -d)
cd "$ROOT"

echo "== build =="
{ echo '$ pio run -e warthog-us'
  pio run -e warthog-us 2>&1 | grep -E "^(RAM|Flash|Building|Linking|Successfully created|=+ \[SUCCESS\])" | head -8
} > "$CAP/build.txt"

echo "== AT console =="
"$PY" - "$SUFFIX" > "$CAP/at.txt" <<'PYEOF'
import sys, time, serial, serial.tools.list_ports as lp
sfx = sys.argv[1] if len(sys.argv) > 1 else ""
port = next((p.device for p in lp.comports()
             if p.vid == 0x303a and p.serial_number
             and (not sfx or p.serial_number.endswith(sfx))), None)
if not port:
    raise SystemExit("no warthog serial port found")
s = serial.Serial(port, 115200, timeout=2); time.sleep(0.8); s.reset_input_buffer()
for cmd in ("AT", "AT+VERSION?", "AT+STATUS?"):
    print("warthog> " + cmd)
    s.reset_input_buffer(); s.write((cmd + "\r\n").encode()); s.flush(); time.sleep(1.7)
    for l in s.read(6000).decode(errors="replace").splitlines():
        t = l.strip()
        if t and t != cmd and "umac" not in t and "probe" not in t:
            print(t)
s.close()
PYEOF

echo "== host USB interface =="
IFACE=$(for i in $(ifconfig -l | tr ' ' '\n' | grep -E '^en[0-9]+'); do
          ipconfig getifaddr "$i" 2>/dev/null | grep -q '^192\.168\.4\.' && echo "$i" && break
        done)
{ echo '$ networksetup -listallhardwareports | grep -A2 Warthog'
  networksetup -listallhardwareports | grep -A2 -i warthog | grep -A2 "$IFACE" | head -3
  echo ''
  echo "\$ ipconfig getifaddr $IFACE"; ipconfig getifaddr "$IFACE"
  echo ''
  echo '$ ping -c 4 192.168.4.1'; ping -c 4 -t 3 192.168.4.1
} > "$CAP/host.txt" 2>&1

python3 scripts/make_terminal_svg.py "$CAP/build.txt" docs/img/usb-build.svg   "Building — pio run -e warthog-us"
python3 scripts/make_terminal_svg.py "$CAP/at.txt"    docs/img/usb-console.svg "warthog — AT console over USB"
python3 scripts/make_terminal_svg.py "$CAP/host.txt"  docs/img/usb-host.svg    "macOS — USB Ethernet via CDC-NCM"
# OpenMANET captures need the Pi (PI_SSH="ssh root@<pi>") and a mesh build on
# the board; skipped when PI_SSH is unset.
if [ -n "${PI_SSH:-}" ]; then
  echo "== OpenMANET (Pi) =="
  $PI_SSH 'echo "root@OpenMANET:~# iw dev wlh0 station dump | grep -E \"^Station|plink\""; iw dev wlh0 station dump | grep -E "^Station|plink"; echo ""; echo "root@OpenMANET:~# iw dev wlh0 mpath dump"; iw dev wlh0 mpath dump; echo ""; echo "root@OpenMANET:~# ping -c 4 10.77.131.165"; ping -c 4 -W 2 10.77.131.165' > "$CAP/pi.txt" 2>&1
  python3 scripts/make_terminal_svg.py "$CAP/pi.txt" docs/img/openmanet-pi.svg "OpenMANET (Raspberry Pi) — the mesh as it sees it"
fi
rm -rf "$CAP"
echo "done"
