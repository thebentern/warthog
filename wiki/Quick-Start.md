# Quick Start

From an unflashed board to a working link. Budget twenty minutes the first time,
most of it toolchain install.

## 1. Prepare the board

- **Seeed XIAO ESP32-S3 with the Seeed HaLow add-on.** Fit a 470–1000 µF
  low-ESR capacitor across the XIAO's **5V** and **GND** pads. The add-on can
  otherwise brown out during radio startup. Connect a 900 MHz HaLow antenna.
- **Heltec RadioCore RC32-68.** Connect a HaLow antenna for your board's band
  (915 MHz or 868 MHz). [Heltec specifications](https://heltec.org/project/rc32/)

Use a USB data cable for either board.

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

Component versions are pinned exactly. After pulling a change to
`sdkconfig.defaults` or a board overlay, remove the affected generated
`sdkconfig.warthog-*` files before rebuilding. Generated configs can keep
values from earlier defaults.

If an existing checkout fails its first build after a HaLow component update, see
[Troubleshooting](Troubleshooting#build-fails-on-the-first-build-after-a-halow-component-update).

## 3. Choose a build

Use the station environment for your board. Each environment defines its
supported regions and the default for a board with no region stored.

| Board | Station environment | Regions | Default |
|---|---|---|---|
| Seeed XIAO ESP32-S3 + HaLow add-on | `warthog-station` | AU, JP, KR, NZ, US | US |
| Heltec RadioCore RC32-68, 915 MHz (ZC-390-1) | `warthog-heltec-rc32-68` | AU, CA, JP, KR, NZ, US | US |
| Heltec RadioCore RC32-68, 868 MHz (ZC-389-1) | `warthog-heltec-rc32-68-868` | EU | EU |

Set the region at runtime with `AT+REGION=<CC>`. [Step 6](#6-point-it-at-an-uplink)

For an open mesh, set `AT+MESHEN=1` and `AT+MESHSEC=0`, match your peers' mesh
ID and channel, then run `AT+RESET`. The dedicated `warthog-mesh-*` builds are
for the XIAO. [Mesh Mode](Mesh-Mode)

## 4. Flash

The Warthog firmware runs its console and USB network on USB-OTG (TinyUSB), so
`esptool` cannot reset the board into download mode over DTR/RTS. Use the board
buttons or send `AT+DLMODE` on a board already running Warthog.
[Reflashing a running board](Flashing#reflashing-a-running-board)

- **XIAO** — hold **BOOT**, tap **RESET**, then release **BOOT**.
- **RadioCore** — hold **USER**, tap **R**, then release **USER**.

Set `warthog_env` to your board's environment from the table, then build and
upload.

```bash
warthog_env=warthog-heltec-rc32-68  # Example for the 915 MHz RadioCore
pio run -e "$warthog_env" -t upload
```

Tap **RESET** on the XIAO or **R** on the RadioCore when it finishes.
[Flashing](Flashing)

## 5. Confirm it booted

A CDC-ACM console appears as `/dev/cu.usbmodem*` (macOS) or `/dev/ttyACM*`
(Linux). Open a terminal at 115200 8-N-1. This example shows a board using
the US default.

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

Use `AT+REGION?` to see what the firmware offers. Replace `US` below with the
region where you operate. In station mode, enter the HaLow AP credentials.

```
AT+REGION=US
AT+HALOW=MyHaLowAP,secretpassphrase
AT+RESET
```

After it comes back, `AT+STATUS?` should show a HaLow IP.

## 7. Use it

The host gets a USB Ethernet adapter — see [Host Mode](Host-Mode). Phones and
other clients can join the `warthog` Wi-Fi AP — see [Client Mode](Client-Mode).
