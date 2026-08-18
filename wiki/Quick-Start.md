# Quick Start

From an unflashed board to a working link. Budget twenty minutes the first time,
most of it toolchain install.

## 1. Fit the capacitor

Before anything else. The Seeed HaLow add-on browns the board out under PA load
without extra bulk capacitance — 470–1000 µF, low ESR, across the XIAO's **5V**
and **GND** pads. Skipping this produces resets that look like firmware bugs.

## 2. Install the toolchain

```bash
pip install platformio
git clone https://github.com/thebentern/warthog.git
cd warthog
```

## 3. Choose a build

Pick the env for your region — this sets the radio's regulatory domain and is
baked into the image:

```bash
pio run -e warthog-us      # 902–928 MHz
pio run -e warthog-eu      # 863–868 MHz
pio run -e warthog-jp      # 916.5–927.5 MHz
pio run -e warthog-kr      # 917.5–923.5 MHz
pio run -e warthog-au
```

For a peer-to-peer mesh instead of a station uplink, build
`warthog-mesh-smoke` and read [Mesh Mode](Mesh-Mode) first.

## 4. Flash

The HaLow add-on uses USB-OTG, so the usual auto-reset does not work. Put the
board in download mode by hand:

**Hold BOOT → tap RESET → release BOOT**, then:

```bash
pio run -e warthog-us -t upload
```

Tap **RESET** when it finishes. Full detail and the multi-board flasher in
[Flashing](Flashing).

## 5. Confirm it booted

A CDC-ACM console appears as `/dev/cu.usbmodem*` (macOS) or `/dev/ttyACM*`
(Linux). Any terminal at 115200 8-N-1:

```
AT
OK

AT+VERSION?
+VERSION: warthog 0.1.0-dev
OK

AT+STATUS?
+HALOW: ip=... gw=...
+USB: ip=192.168.4.1 mounted=1
+AP: ip=192.168.5.1
OK
```

If `AT` does not answer, see [Troubleshooting](Troubleshooting).

## 6. Point it at an uplink

For station mode, give it an access point. Credentials persist in NVS, so this
survives reboots and does not need a rebuild:

```
AT+HALOW=MyHaLowAP,secretpassphrase
AT+RESET
```

After it comes back, `AT+STATUS?` should show a HaLow IP.

## 7. Use it

The host gets a USB Ethernet adapter — see [Host Mode](Host-Mode). Phones and
other clients can join the `warthog` Wi-Fi AP — see [Client Mode](Client-Mode).
