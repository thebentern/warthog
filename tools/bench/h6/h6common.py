"""Shared by the H6 USB tests: find a board's console port and network interface by its USB serial.

Port names change whenever the hub re-enumerates, so nothing here takes one. Never call flush() on these
ports: on a board whose USB has stopped it blocks for ever. Every port is opened with a write timeout.
"""
import os
import re
import subprocess
import sys
import time

import serial
import serial.tools.list_ports as lp

HERE = os.path.dirname(os.path.abspath(__file__))


def tool(name):
    """A host tool built by `make` in this directory."""
    p = os.path.join(HERE, name)
    if not os.path.exists(p):
        sys.exit(f"{p} is not built: run `make -C {HERE}` (needs libusb-1.0)")
    return p


def port(usb_serial):
    for p in lp.comports():
        if p.serial_number == usb_serial:
            return p.device
    raise RuntimeError(f"no serial port with USB serial {usb_serial}")


def name(usb_serial):
    return usb_serial[-4:]


def iface(usb_serial):
    """The host network interface of the board's USB network function. Its MAC is the 12 hex digits of
    the USB serial (WTHG-<mac>). macOS (networksetup) and Linux (/sys/class/net)."""
    mac = usb_serial.split("-")[-1].lower()
    mac = ":".join(mac[i:i + 2] for i in range(0, 12, 2))
    if sys.platform == "darwin":
        out = subprocess.run(["networksetup", "-listallhardwareports"], capture_output=True, text=True).stdout
        dev = None
        for line in out.splitlines():
            if line.startswith("Device:"):
                dev = line.split()[1]
            elif line.startswith("Ethernet Address:") and line.split()[-1].lower() == mac:
                return dev
    else:
        for dev in os.listdir("/sys/class/net"):
            try:
                with open(f"/sys/class/net/{dev}/address") as f:
                    if f.read().strip().lower() == mac:
                        return dev
            except OSError:
                pass
    raise RuntimeError(f"no network interface with MAC {mac}")


def at(usb_serial, cmds, wait=1.5):
    """One console session: open, send each command, read to OK or ERROR, close. Returns {cmd: [lines]}
    with only the '+...', OK and ERROR lines. A command that gets neither OK nor ERROR has no such line."""
    res = {}
    s = serial.Serial(port(usb_serial), 115200, timeout=0.2, write_timeout=2.0)
    try:
        time.sleep(0.25)
        s.reset_input_buffer()
        for c in cmds:
            s.write((c + "\r\n").encode())
            buf = b""
            end = time.time() + wait
            while time.time() < end:
                d = s.read(4096)
                if d:
                    buf += d
                    lines = [l.strip() for l in buf.replace(b"\r", b"").split(b"\n")]
                    if b"OK" in lines or b"ERROR" in lines:
                        break
            lines = [l for l in buf.decode(errors="replace").replace("\r", "").split("\n") if l.strip()]
            res[c] = [l for l in lines if l.startswith("+") or l in ("OK", "ERROR")]
    finally:
        s.close()
    return res


def field(lines, prefix, key):
    for l in lines:
        if l.startswith(prefix):
            m = re.search(r"\b" + re.escape(key) + r"=(\S+)", l)
            if m:
                return m.group(1)
    return None


def usb_answers(usb_serial):
    """Does the board still answer a control request on endpoint 0 (usbprobe's GET_STATUS)?"""
    out = subprocess.run([tool("usbprobe")], capture_output=True, text=True, timeout=20).stdout
    m = re.search(r"serial=" + re.escape(usb_serial) + r" get_status_rc=(-?\d+)", out)
    return bool(m and int(m.group(1)) >= 0)
