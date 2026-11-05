# Quick Start

From an unflashed board to a working link. Budget twenty minutes the first time,
most of it toolchain install.

## 1. Fit the capacitor

Before anything else. The Seeed HaLow add-on browns the board out under PA load
without extra bulk capacitance — 470–1000 µF, low ESR, across the XIAO's **5V**
and **GND** pads. Skipping this produces resets that look like firmware bugs.

## 2. Install the toolchain

```bash
pip install platformio==6.2.0
git clone https://github.com/thebentern/warthog.git
cd warthog
export UV_CONSTRAINT="$PWD/scripts/ci/penv-constraints.txt"
```

The pinned pioarduino platform needs PlatformIO 6.2: with 6.1.x its SCons 4.11.1 sits beside
6.1's 4.8.1 and the build stops with `No module named 'SCons.Tool.FortranCommon'`. The platform
also installs PlatformIO into its own environment (`penv` in the PlatformIO core dir) as
`pioarduino>=6.2.0`, which PyPI resolves past 6.2.0; `UV_CONSTRAINT` holds it at 6.2.0, so keep it
set in the shell you build from. uv reads `UV_CONSTRAINT` as a list of files separated by spaces,
so the checkout path must not contain one. `scripts/ci/check-platform-pin.sh --installed` prints
the penv's version. A penv that already holds another version, or one made by an older platform
(`pioarduino-core` 6.1.19), keeps it: delete the penv directory and the next build recreates it.

Component versions are pinned exactly. After pulling a change to `sdkconfig.defaults` or `sdkconfig.ecm`,
run `rm -f sdkconfig.warthog-*`: an existing sdkconfig keeps every value it sets itself (a line with no
`# default:` above it), the ones it took from them included.

If an existing checkout fails its first build after a HaLow component update, see
[Troubleshooting](Troubleshooting#build-fails-on-the-first-build-after-a-halow-component-update).

## 3. Choose a build

One station image serves every region the module offers (AU, JP, KR, NZ, US):

```bash
pio run -e warthog-station
```

The country is set at runtime: `AT+REGION=<CC>` ([step 6](#6-point-it-at-an-uplink)), or
pick it in the web flasher. A board with none set runs as US.

For a peer-to-peer mesh instead of a station uplink, build
`warthog-mesh-smoke` and read [Mesh Mode](Mesh-Mode) first.

> **The station build can join a mesh at runtime** — `AT+MESHEN=1`, then
> `AT+RESET`. Mesh ID, passphrase and channel are set the same way
> (`AT+MESHID=`, `AT+MESHPASS=`, `AT+MESHCHAN=`); all persist to NVS.
>
> **With mesh on it uses its region's default mesh channel** (US: ch42,
> 923.0 MHz, 2 MHz; the table is in [Mesh Mode](Mesh-Mode#building-for-mesh)).
> To match a peer on another channel:
> `AT+MESHCHAN=<chan>,<freq_hz>,<gclass>,<sclass>,<bw>`, then `AT+RESET`. The
> set must be a row of the region's table; one that is not is refused with the
> reason. `AT+MESHCFG?` reports what actually applied.

## 4. Flash

The Warthog firmware runs its console and USB network on USB-OTG (TinyUSB), so
`esptool` cannot reset the board into download mode over DTR/RTS. Put the board
in download mode by hand (a board already running Warthog can take `AT+DLMODE`
instead; see [Flashing](Flashing#reflashing-a-running-board)):

**Hold BOOT → tap RESET → release BOOT**, then:

```bash
pio run -e warthog-station -t upload
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
+USB: ip=192.168.4.1 mounted=1 tx=... drop=...
+USBNET: tx_queued=... txq=... ...
+AP: ip=192.168.5.1
+DNS: offered=1.1.1.1
+REGION: running=US stored=none (not set: US default)
OK
```

If `AT` does not answer, see [Troubleshooting](Troubleshooting).

## 6. Point it at an uplink

Set the country you operate in (`AT+REGION?` lists the offered ones; none set
runs as US), then, for station mode, give it an access point. Both persist, so
this survives reboots and does not need a rebuild:

```
AT+REGION=US
AT+HALOW=MyHaLowAP,secretpassphrase
AT+RESET
```

After it comes back, `AT+STATUS?` should show a HaLow IP.

## 7. Use it

The host gets a USB Ethernet adapter — see [Host Mode](Host-Mode). Phones and
other clients can join the `warthog` Wi-Fi AP — see [Client Mode](Client-Mode).
