#!/usr/bin/env python3
"""Trigger ROM download mode on Warthog board via CDC line-coding (1200 bps)
or via AT+DLMODE command. Then close so esptool can take the port.

Usage:
  dl_trigger.py <port>           # tries AT+DLMODE then 1200bps fallback
  dl_trigger.py <port> --1200    # just toggles baud to 1200
"""
import sys
import time
import serial

def main():
    port = sys.argv[1]
    mode_1200 = "--1200" in sys.argv

    # Always try the 1200bps watchdog first — it's deterministic.
    try:
        s = serial.Serial(port, baudrate=1200, timeout=0.5)
        time.sleep(0.5)
        s.close()
        print(f"[{port}] sent 1200bps baud — chip should be in download mode")
    except Exception as e:
        print(f"[{port}] 1200bps toggle failed: {e}", file=sys.stderr)
        if not mode_1200:
            # Fallback: AT+DLMODE
            try:
                s = serial.Serial(port, baudrate=115200, timeout=1)
                s.write(b"AT+DLMODE\r\n")
                s.flush()
                time.sleep(0.5)
                s.close()
                print(f"[{port}] sent AT+DLMODE")
            except Exception as e2:
                print(f"[{port}] AT+DLMODE failed: {e2}", file=sys.stderr)
                return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
