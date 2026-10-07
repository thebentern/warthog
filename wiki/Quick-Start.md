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

> **A region env can join a mesh at runtime** — `AT+MESHEN=1`, then
> `AT+RESET`. Mesh ID, passphrase and channel are set the same way
> (`AT+MESHID=`, `AT+MESHPASS=`, `AT+MESHCHAN=`); all persist to NVS.
>
> **A region build ships with no channel pinned** — it carries the whole
> country list, so its operating channel is neither chosen nor observable, and
> meeting a mesh on it is luck. Pin one to match your peer:
> `AT+MESHCHAN=<chan>,<freq_hz>,<gclass>,<sclass>,<bw>`, then `AT+RESET`. The
> channel must exist in the country's regulatory table; one that does not is
> refused and logged rather than forced. `AT+MESHCFG?` reports what actually
> applied. See [Mesh Mode](Mesh-Mode).

## 4. Flash

The Warthog firmware runs its console and USB network on USB-OTG (TinyUSB), so
`esptool` cannot reset the board into download mode over DTR/RTS. Put the board
in download mode by hand (a board already running Warthog can take `AT+DLMODE`
instead; see [Flashing](Flashing#reflashing-a-running-board)):

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
+VERSION: warthog 7f6599f
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
