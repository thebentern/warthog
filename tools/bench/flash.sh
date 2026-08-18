# flash7: hub-safe serial flasher that also handles boards enumerating >1 CDC port.
PY="${PIO_PYTHON:-$HOME/.platformio/penv/bin/python}"
# Derive the repo root from this script rather than hardcoding one checkout.
cd "$(cd "$(dirname "$0")/../.." && pwd)"
# Which build to flash. Defaults to the mesh bench image.
ENV_NAME="${WARTHOG_ENV:-warthog-mesh-smoke}"
first=1
for spec in "$@"; do
  set -- $spec; SER=$1; HUB=$2; PORT=$3
  [ $first = 0 ] && { echo "  (settling 20s -- hub protection)"; sleep 20; }
  first=0
  # DLMODE on whichever CDC port of this board actually answers AT
  $PY - "$SER" <<'PYEOF'
import sys, time, serial, serial.tools.list_ports as lp
ser=sys.argv[1]
for p in [q.device for q in lp.comports() if q.vid==0x303a and q.serial_number==ser]:
    try:
        s=serial.Serial(p,115200,timeout=1); time.sleep(0.35); s.reset_input_buffer()
        s.write(b'AT\r\n'); s.flush(); time.sleep(0.7)
        if 'OK' in s.read(200).decode(errors='replace'):
            s.write(b'AT+DLMODE\r\n'); s.flush(); time.sleep(1.2); s.close()
            print("  DLMODE ->", p); break
        s.close()
    except Exception: pass
PYEOF
  for i in $(seq 1 12); do sleep 2; uhubctl -l "$HUB" 2>/dev/null | grep "Port $PORT:" | grep -q "303a:0009" && break; done
  ROM=$($PY -c "
import serial.tools.list_ports as lp
for p in lp.comports():
    if p.vid==0x303a and p.pid==0x0009: print(p.device); break")
  if [ -z "$ROM" ]; then echo "$SER: not in ROM"; continue; fi
  $PY -m esptool --chip esp32s3 --port "$ROM" --baud 921600 --before no-reset --after hard-reset \
    write_flash 0x0 ".pio/build/$ENV_NAME/firmware.factory.bin" 2>&1 | grep -q "Hash of data verified" \
    && echo "$SER: flashed OK" || { echo "$SER: flash FAILED"; continue; }
  uhubctl -l "$HUB" -p "$PORT" -a cycle -d 5 >/dev/null 2>&1
done
