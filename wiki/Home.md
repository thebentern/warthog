# Warthog

Open-source firmware that turns an ESP32-S3 plus a Wi-Fi HaLow module into a
long-range network bridge you can read, modify and reflash.

Sub-GHz 802.11ah gives kilometre-class range at low power. Warthog puts three
usable surfaces on top of it and an AT console in front of it, so you can
reconfigure a node over a serial port instead of a rebuild.

## Pick your path

| You want to… | Start here |
|---|---|
| Get a board running for the first time | **[Quick Start](Quick-Start)** |
| Give a laptop a long-range uplink over USB | **[Host Mode](Host-Mode)** |
| Connect a phone or tablet | **[Client Mode](Client-Mode)** — not USB, see why there |
| Let phones and IoT clients share that uplink | **[Client Mode](Client-Mode)** |
| Build a peer-to-peer network with no infrastructure | **[Mesh Mode](Mesh-Mode)** |
| Talk to OpenMANET or vanilla OpenWrt | **[OpenMANET Interop](OpenMANET-Interop)** |
| Look up a command | **[AT Command Reference](AT-Command-Reference)** |
| Work out why something is not passing traffic | **[Troubleshooting](Troubleshooting)** |
| Build, test, contribute | **[Development](Development)** |

## The three surfaces

Warthog is not one device role. A node runs an **uplink** — either a HaLow
station joining an access point, or an 802.11s mesh peer — and presents
**downstream surfaces** to whatever is plugged into or associated with it.

```
        ┌──────── downstream ────────┐        ┌──── uplink ────┐

  laptop ──USB──▶ CDC-ECM ┐
                          ├─▶ NAPT ─▶ HaLow ─▶  AP  (station mode)
  phone  ──WiFi─▶ 2.4 AP  ┘                or  mesh (802.11s peers)
```

Both downstream surfaces are live at once and share the uplink. The two uplink
modes are mutually exclusive: a node is a station **or** a mesh peer, chosen at
build time.

## Hardware

- [Seeed Studio XIAO ESP32-S3](https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html)
- [Seeed Studio Wi-Fi HaLow module for XIAO](https://www.seeedstudio.com/Wi-Fi-HaLow-Module-for-Seeed-Studio-XIAO-p-6262.html) (Quectel FGH100M-H / Morse Micro MM6108)
- 900 MHz antenna, I-PEX → SMA

> **This combination needs a hardware modification before it will run.** The
> add-on feeds the radio's PA from USB VBUS through only 20 µF, and the turn-on
> inrush browns out the ESP32-S3. Add a 470–1000 µF low-ESR capacitor across the
> XIAO's 5V and GND pads. This is not optional and the failure looks like random
> resets.

## Licence

GPL-3.0. Built on ESP-IDF 5.5, TinyUSB and `morsemicro/halow`.
