# Warthog

```
           .                   .
           .        :      ----::
           .        :...........:-:.-..:-
           .            :.--.:-::
           .            :..-....:
           .              ...:  :::
           .            .:...:-..:::.-------:--:.--::-.
           .           -...-.:.:...             ::..:......::..
    .:..:::-         :..-.-...-...:.         :. .-:...:::::::::-::...::.
    .:.::::--..:................:.:          :..............:.:-------:: .
     .-::::-..:-........::::..:----.:     ::...----:....-..::--:.......-::
        -----:---------:..:..:.......--:------.......::..:.---.........:.:
        :.....:---:----..:..:..::..................-...-:-...:---:-:.....--
       ...---.::---::.---:..-::...---------------....--.:..-.::-----.-...--.
        .  -::.......::--:.-..:-:...:..........:...-:.-..--:-.......:.--:.-
          ::-.........-::...-:......-..........-:...::...::-.........-:.
          :.-.........:.:      :-------------------.     :.:............
          :..:........:.:                                ..:.........-.:
           ..:-.....-:::                                  :.:-.....-:.:
             :::.:.:.-                                      ::..:.:.-
```

> **Warthog** — open-source firmware turning an ESP32-S3 + Wi-Fi HaLow module into a long-range USB Ethernet and 2.4 GHz Wi-Fi AP gateway you can hack.

Plug it into a laptop and the host gets a USB Ethernet adapter. Join the side-car 2.4 GHz Wi-Fi AP from a phone or IoT client and you share the same uplink. Both surfaces are NAT'd out a sub-GHz 802.11ah HaLow link with kilometer-class range. A CDC-ACM console exposes an AT-command surface for runtime reconfig without reflashing.

## Features

- USB **CDC-NCM** network adapter for the host (in-box on macOS, Linux, Windows 10+ and iOS/iPadOS; a CDC-ECM build, `warthog-us-ecm`, for hosts without NCM)
- Secondary **2.4 GHz Wi-Fi AP** for clients that can't (or shouldn't) be wired
- **HaLow STA uplink** via Morse Micro MM6108 / Quectel FGH100M-H — kilometer range at low power
- **lwIP NAPT** bridges both downstream surfaces out HaLow
- USB **CDC-ACM** console with mirrored ESP-IDF logs and an **AT command** surface for runtime SSID/PSK changes
- NVS-persisted credentials — no reflashing to retarget the uplink
- Per-region release builds (US / EU / JP / KR / AU), BCF + country code baked in at compile time
- Built on **ESP-IDF 5.5 + TinyUSB + `morsemicro/halow`**

## Documentation

Full guides live in the [wiki](../../wiki) — quick start, flashing, each
operating mode, the AT reference and troubleshooting. Deep technical notes are
versioned alongside the code in [`docs/`](docs/).

## Operating modes

Warthog is not one device role. A node runs an **uplink** and presents
**downstream surfaces** to whatever is plugged into or associated with it.

```
        ┌──────── downstream ────────┐        ┌──── uplink ────┐

  laptop ──USB──▶ CDC-NCM ┐
                          ├─▶ NAPT ─▶ HaLow ─▶  AP  (station mode)
  phone  ──WiFi─▶ 2.4 AP  ┘                or  mesh (802.11s peers)
```

| Mode | What it does | Chosen |
|---|---|---|
| **Host** | Gives the machine it is plugged into a USB Ethernet adapter (`192.168.4.1/24`) | always on |
| **Client** | 2.4 GHz AP so phones and IoT clients share the uplink (`192.168.5.1/24`) | always on |
| **Station uplink** | Joins an existing HaLow access point | default builds |
| **Mesh uplink** | 802.11s peer-to-peer, no infrastructure | `AT+MESHEN=1` on any build; `warthog-mesh-sae` for SAE/AMPE (`warthog-mesh-sae-swccmp` against OpenMANET) |

Both downstream surfaces are live at once. The two uplink modes are mutually
exclusive; which one runs is a runtime setting (`AT+MESHEN`), as are the mesh
ID, passphrase and channel. Whether the build speaks SAE/AMPE at all is still
chosen at build time.

### Warthog is a mesh leaf, not a relay

> **Unless `AT+MESHFWD=1`.** Forwarding mode relays path selection and data
> for other nodes and advertises the capability; it is implemented,
> host-tested and simulated, and has not been on a radio. See
> [Mesh Mode](wiki/Mesh-Mode.md#forwarding). This section describes the
> default.

A Warthog joins a mesh and talks to its peers. It does **not** forward frames
between two other nodes, so it cannot extend a mesh's reach — a Warthog placed
between two nodes that cannot hear each other does not connect them.

In the default leaf mode this is switched off, not merely unverified: the RX
data path delivers to the local host or drops, and a unicast whose mesh
destination is another node is dropped (`AT+RXCHAN?` reason 93, or 94 without
Mesh Control); the relay decision (`umac_mesh_fwd_rx`), the only code that
reads and decrements the Mesh Control TTL, runs only under `AT+MESHFWD=1` or
`AT+MESHBRIDGE=1`; a leaf neither relays a PREQ nor starts path discovery
(its only PREQs are the per-peer keepalive); and the multicast repeater
refuses to re-send a datagram out the interface it arrived on (`main/mudp.c`).

A leaf does learn hosts behind other mesh nodes (the LAN of a bridged
OpenMANET node) from Address Extension frames a peer carried (under SAE, a
keyed peer), and records that peer as the host's relay. A reply to such a host
is addressed to its node with Address Extension mode 2 and handed to the node
itself when it is a peer, else to the relay; the leaf sends no PREQ for it. If
the relay holds no path to that node, a vanilla mac80211 relay drops the reply
with a PERR, so first contact from a far host on an idle mesh (its ARP arrives
as a flood before its node has sent a PREQ) can fail. A warthog relay, like
OpenMANET's patched mac80211 with forwarding on (per its source), holds the
reply and discovers the node itself, giving up after 6.8 s; simulated, not
measured. Broadcasts keep the leaf shape, one plain replica per peer (or one
standard group frame with `AT+MESHGRP=1`), and a unicast to an unknown address
goes to the first peer.

If you need a relay — an airborne node extending coverage, for instance —
that is `AT+MESHFWD=1` (802.11s HWMP forwarding), which has not been on a
radio. See [Mesh Mode](wiki/Mesh-Mode.md#forwarding).

### Warthog routes, it does not bridge

> **Unless `AT+MESHBRIDGE=1`.** Bridge mode puts USB, the Wi-Fi AP and the
> mesh on one L2 segment and turns NAT off; it exists precisely to remove the
> limitations below, and it is compiled but not yet measured on air. See
> [Mesh Mode](wiki/Mesh-Mode.md#bridge-mode). Everything in this section
> describes the default, NAT mode.

Every surface is its own IP subnet and Warthog NAPTs between them
(`main/nat.c`). The tethered host is **not** on the same layer-2 segment as
anything upstream, and that has consequences worth knowing before you design
around it:

- **Link-local and broadcast discovery does not cross Warthog.** mDNS/DNS-SD,
  SSDP, NetBIOS and anything else that finds peers by broadcasting on the
  local segment will not see devices on the other side.
- **Multicast does not cross either, with one exception.** A small
  application-layer repeater (`main/mudp.c`) forwards exactly one group —
  Meshtastic's `239.0.0.69:4403` — between the USB, AP and HaLow netifs.
  Nothing else is repeated.
- **ATAK / CoT multicast discovery on `239.2.3.1:6969` will not work**, and
  adding it to the repeater would not fix it. Every Warthog NATs its tethered
  host to the same addresses — `WARTHOG_USB_GW_IP` is the compile-time constant
  `192.168.4.1` on every unit — so two hosts on opposite sides of a mesh are
  both `192.168.4.x`. A CoT event carries the sender's address in its payload,
  so repeating the group would deliver a contact whose address, on the
  receiver's side, is the receiver's own subnet. That is worse than not
  finding each other: it is finding each other wrongly. The same applies to
  mDNS/SD, whose A records would advertise unroutable addresses.

  Point-to-point CoT to a known, routable address still works; discovery does
  not. What fixes it is one L2 segment with unique host addresses — bridging
  rather than NAT — which is what `AT+MESHBRIDGE=1` now provides (compiled,
  not yet measured on air), not a bigger repeater.
- **Inbound connections need explicit forwarding.** Upstream devices cannot
  reach the tethered host by address, because it is behind NAT.
- **IP fragments cross NAT whole.** lwIP's NAPT reads ports from every packet
  and a fragment after the first has none, so Warthog reassembles a fragmented
  datagram before NAPT and lwIP cuts it again for the outgoing MTU
  (`main/nat_frag.c`); at most 10 fragments a datagram, IPv4 only
  ([Troubleshooting](wiki/Troubleshooting.md#large-packets-from-a-node-at-mtu-1460-go-unanswered-ip-fragments)).

If your application depends on layer-2 adjacency or broadcast discovery, this
is the limitation to design around — use known addresses, a server both sides
reach, or an overlay (see [Security](#security)).

## USB setup, end to end

Three commands from a fresh checkout to a working link. The screenshots below
are real sessions against an attached board, regenerated by
`scripts/regen-screenshots.sh`.

### 1. Build

```bash
pio run -e warthog-us
```

![Building warthog](docs/img/usb-build.svg)

### 2. Flash

**No toolchain?** Use the [web flasher](https://thebentern.github.io/warthog/) —
Chrome, Edge or Opera, no install. It also has a Configure tab that drives the
same AT commands from the browser.

The HaLow add-on drives USB-OTG, so `esptool` cannot pull the board into
download mode over DTR/RTS. Do it by hand — **hold BOOT, tap RESET, release
BOOT** — then:

```bash
pio run -e warthog-us -t upload
```

Tap **RESET** when it finishes.

### 3. Talk to it

A CDC-ACM console appears as `/dev/cu.usbmodem*` (macOS) or `/dev/ttyACM*`
(Linux). Any terminal at 115200 8-N-1:

![AT console over USB](docs/img/usb-console.svg)

`AT+STATUS?` reports every surface at once: the HaLow uplink, the USB network,
the Wi-Fi AP, and the DNS handed to downstream clients.

### 4. Use the network

The same cable also presents a **USB Ethernet adapter**. The host picks up a
DHCP lease from the board and can route out over HaLow:

![macOS USB Ethernet via CDC-NCM](docs/img/usb-host.svg)

That is a stock macOS driver binding a stock USB class — nothing to install.

## Connecting phones and tablets

The USB surface presents **CDC-NCM**, which every current host binds with an
in-box driver — macOS, Linux, Windows 10+ and iOS/iPadOS. The class matters far
more than the cable:

| Client | Over USB | Over the Wi-Fi AP |
|---|---|---|
| macOS laptop | ✅ verified — DHCP lease, 300/300 at 1400 B, ~1.5 ms | ✅ |
| Linux laptop | ✅ expected — `cdc_ncm` in-tree since 2.6.38 (not re-verified) | ✅ |
| Windows laptop | ✅ expected — `usbncm.sys` in box on Windows 10+ (untested) | ✅ |
| iPhone / iPad | ✅ expected — Apple's in-box NCM driver, no dext or MFi (untested here) | ✅ |
| Android phone/tablet | ⚠️ untested — needs USB host mode; also has to power the board | ✅ recommended |

### Why an iPad works over USB now

warthog used to present CDC-ECM, which macOS and Linux bind and iOS does not —
so a cable to an iPad did nothing. It now presents **CDC-NCM**, the class Apple
binds with its own driver: no dext, no MFi, and it works in airplane mode
because the link is not a radio.

Two things that made this work, both easy to get wrong:

- **TinyUSB must resolve to >= 0.21.0**, pinned in `main/idf_component.yml`.
  `esp_tinyusb` alone only asks for `>= 0.17.0~2`, so the resolve was landing
  on 0.19.0 by luck; below 0.21.0 the NCM control requests are unreliable.
- **The configuration descriptor is built by `esp_tinyusb`, not by hand.** A
  hand-rolled composite serves ECM fine and is rejected outright for NCM, in a
  way that looks like dead hardware: the device still answers its device and
  string descriptors, so it appears in a hub listing with the right name while
  the host never creates a device for it.

Two caveats that remain:

1. **The phone has to be the USB host** — OTG mode, a suitable adapter, and the
   phone supplies the power. The HaLow PA needs a bulk capacitor even from a
   laptop port ([`docs/power-notes.md`](docs/power-notes.md)); a phone port is a
   worse supply.
2. **Two warthogs on one host collide.** Both vend `192.168.4.1/24`, so a host
   with two attached configures only one and the other falls back to a
   link-local address.

If you meet a host that binds ECM and not NCM, `warthog-us-ecm` builds CDC-ECM
instead. esp_tinyusb's default configuration descriptor has no ECM function, so
that build supplies the CDC-ACM + CDC-ECM descriptor the board shipped with
before NCM. Built in CI, which checks the ELF holds that descriptor; not
measured on a host.

## How Warthog compares

There are off-the-shelf HaLow USB adapters that just work — Heltec HT-HD01 V2, Alfa Network AHUS-1, Vantron's MM8108 dongles. If you need a HaLow uplink for a laptop today and don't want to think about firmware, **buy one of those.** They're vendor-supported and require zero assembly.

Warthog is for a different audience: people who want to *own* the firmware on a HaLow gateway and ship custom behavior on top.

| | Commercial dongles (Heltec / Alfa / Vantron) | Warthog |
|---|---|---|
| Firmware | Closed, vendor binary | **Open source** (GPL-3.0); ESP-IDF + TinyUSB + `morsemicro/halow` |
| Hardware | Pre-built USB stick | XIAO ESP32-S3 + Seeed HaLow add-on (~$30 BOM) — needs a [bulk-cap mod](docs/power-notes.md) on this specific board |
| Host surface | USB Ethernet only | USB CDC-NCM **+** CDC-ACM console **+** 2.4 GHz Wi-Fi AP |
| Runtime config | Vendor app or fixed defaults | **AT commands** over the CDC port (`AT+HALOW=ssid,psk`), NVS-persisted |
| Secondary clients | Need their own USB host | Join the Wi-Fi AP — no USB required |
| Region story | Vendor-locked, opaque | Per-region build artifacts; BCF + country code explicit at compile time |
| Customization | Whatever the vendor shipped | Drop in additional USB classes, telemetry over CDC, MQTT bridge, web UI — it's just an ESP-IDF project |
| Docs | Datasheet | The gotchas captured: power delivery, NAPT inversion, USB-OTG handoff (see `docs/`) |

**Short version:** want a HaLow USB adapter today? Buy a dongle. Want a hackable HaLow + USB + Wi-Fi AP gateway with an AT control plane on hardware you understand? Build Warthog.

## Hardware

Tested target:

- [Seeed Studio XIAO ESP32-S3](https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html)
- [Seeed Studio Wi-Fi HaLow module for XIAO](https://www.seeedstudio.com/Wi-Fi-HaLow-Module-for-Seeed-Studio-XIAO-p-6262.html) (Quectel FGH100M-H, MM6108)
- External 900 MHz antenna via I-PEX → SMA pigtail

> ⚙️  **Required mod for this combo:** the Seeed add-on routes the FGH100M-H's PA supply (`VDD_FEM`) straight to USB VBUS with only 20 µF of bulk capacitance. The radio's turn-on inrush collapses the rail under PA load and the ESP32-S3 `POWERON`-resets. Add a **470–1000 µF / ≥ 10 V** low-ESR cap across the XIAO's 5V ↔ GND pads before flying. Full diagnosis + waveform reasoning in [`docs/power-notes.md`](docs/power-notes.md).

## Releases

Tagged releases ship per-region binary bundles on the [GitHub releases page](../../releases). Each bundle contains:

| File | Purpose |
|------|---------|
| `warthog-vX.Y.Z-REGION.factory.bin` | Single-shot flash: bootloader + partitions + app, write to offset `0x0` |
| `warthog-vX.Y.Z-REGION.bin` | App-only image (e.g. for OTA), write to `0x10000` |
| `warthog-vX.Y.Z-REGION.elf` | Debug symbols (for `addr2line` against a panic backtrace) |
| `warthog-vX.Y.Z-REGION-{bootloader,partitions}.bin` | Components if you want to flash piecewise |
| `flash.sh` | POSIX shell flasher (auto-detects port, wraps `esptool`) |
| `SHA256SUMS.txt` | Covers every asset in the release |

Hold the XIAO's **BOOT** button, tap **RESET**, release BOOT to enter download mode, then:

```bash
./flash.sh warthog-v0.1.0-us.factory.bin
```

Tap RESET on the XIAO when it finishes.

## Build & flash

Build for your region:

```bash
pio run -e warthog-us       # United States (902–928 MHz)
pio run -e warthog-eu       # Europe (863–868 MHz)
pio run -e warthog-jp       # Japan (916.5–927.5 MHz)
pio run -e warthog-kr       # Korea (917.5–923.5 MHz)
pio run -e warthog-au       # Australia
pio run -e warthog-mesh-smoke   # 802.11s mesh (see Mesh mode below)
```

Flash (XIAO + HaLow uses USB-OTG, so the auto-reset path is gone — hold **BOOT**, tap **RESET**, then run):

```bash
pio run -e warthog-us -t upload
pio device monitor -e warthog-us
```

A board already running Warthog needs no buttons: send `AT+DLMODE` on its
console, then flash the ROM port it re-enumerates as (`303a:0009`) and leave
with a watchdog reset, which boots the new image
([Flashing](wiki/Flashing.md#reflashing-a-running-board)):

```bash
python -m esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX --baud 921600 \
  --before no-reset --after watchdog-reset write-flash 0x0 .pio/build/warthog-us/firmware.factory.bin
```

Configure HaLow credentials at build time:

```bash
pio run -e warthog-us \
  -DHALOW_SSID=\"MyHaLowAP\" -DHALOW_PASSPHRASE=\"secret\"
```

Other build-time defaults you can override the same way (all live in `[warthog_base]` in `platformio.ini`):

| Macro | Default | What it controls |
|---|---|---|
| `WARTHOG_AP_SSID` | `warthog` | 2.4 GHz AP SSID |
| `WARTHOG_AP_PSK` | `warthog-default` | 2.4 GHz AP WPA2 passphrase |
| `WARTHOG_USB_GW_IP` / `_NETMASK` | `192.168.4.1/24` | USB network subnet (host gets `.2+`) |
| `WARTHOG_AP_GW_IP` / `_NETMASK` | `192.168.5.1/24` | Wi-Fi AP subnet |
| `WARTHOG_DOWNSTREAM_DNS` | `1.1.1.1` | DNS handed to USB + AP clients via DHCP option 6. See [Troubleshooting](#troubleshooting) — getting this wrong is what makes `ping 8.8.8.8` work while `curl example.com` hangs. Override with `AT+DNS=` at runtime instead of reflashing. |

These are compile-time defaults only; everything in NVS (HaLow creds, AP creds, DNS — see AT commands below) wins over them at boot.

## Runtime configuration (AT commands)

The CDC-ACM port (`/dev/cu.usbmodemXXXX` on macOS) accepts AT commands so you don't have to reflash to change credentials. Connect with any serial terminal at 115200 8-N-1 (baud is ignored by CDC):

```
AT                          → OK
AT+VERSION?                 → +VERSION: warthog 0.1.0-dev / OK
AT+STATUS?                  → +HALOW: ip=... / +USB: ip=... mounted=1
                              +AP: ip=... / +DNS: offered=1.1.1.1 / OK
AT+HALOW=MyAP,secret        → store HaLow creds in NVS; AT+RESET to apply
AT+HALOW?                   → +HALOW: ssid="MyAP" (psk hidden) / OK
AT+WIFIAP=warthog,xyz,11    → change AP SSID/PSK/channel
AT+WIFIAP?                  → +WIFIAP: ssid="warthog" chan=6 (psk hidden) / OK
AT+MESHEN=1                 → join a mesh instead of a HaLow AP; AT+RESET to apply
AT+MESHID=halowmesh         → mesh ID, must match every peer exactly
AT+MESHPASS=secret          → SAE passphrase (SAE builds); length-only readback
AT+MESHCHAN=42,923000000,69,2,2
                            → pin the S1G channel as a set; region builds ship unpinned
AT+MESHCFG?                 → everything a peer matches on, plus what Warthog won't do
AT+DNS=8.8.8.8              → set the DNS handed to USB + AP clients via DHCP
AT+DNS?                     → +DNS: 1.1.1.1 / OK
AT+RESET                    → reboot
AT+ERASE                    → wipe NVS warthog namespace
```

`AT+DNS=` validates the input via `inet_pton`; malformed addresses (`AT+DNS=garbage`, `AT+DNS=1.2.3`, `AT+DNS=0.0.0.0`) come back as `+ERR: not a valid IPv4 address` / `ERROR` without touching NVS. To revert to the build-time default (`WARTHOG_DOWNSTREAM_DNS`), run `AT+ERASE` — that wipes the whole namespace including HaLow / AP creds, so you'll re-enter those too.

Credentials and the custom DNS live in NVS namespace `warthog`. On boot, `halow.c`, `wifi_ap.c`, and `usb_net.c` read NVS first and fall back to the `-DHALOW_SSID=...` / `-DWARTHOG_AP_SSID=...` / `-DWARTHOG_DOWNSTREAM_DNS=...` build flags only if the NVS entry is absent.

## Mesh mode (802.11s)

Instead of associating to an access point, a node can join an 802.11s mesh over
HaLow. Every node is a peer — nothing to elect, nothing to associate to, and a
node that loses power takes only its own links with it.

```bash
pio run -e warthog-mesh-sae -t upload      # encrypted: SAE auth + AMPE per-link keys
pio run -e warthog-mesh-smoke -t upload    # open peering; AT+MESHSEC=0 for a peer set to encryption='none'
```

The encrypted build runs real 802.11s security — SAE authentication
(Dragonfly, group 19; a peer's commit in group 20 or 21 is also accepted) and
AMPE key exchange, which gives every link its own pairwise key and every node
its own group key (what the radio can hold is under [Status](#status)). All
nodes share one passphrase: `AT+MESHPASS=` at runtime, defaulting to the
build's `WARTHOG_MESH_PASSPHRASE` (`warthog-mesh`) until one is set. Peering,
keying and addressing are automatic.

A node first asks for a DHCP lease on the mesh; a peer whose mesh interface is
bridged to a LAN with a DHCP server can answer it. With no offer within
6 s it addresses itself statically from its own MAC — `10.77.<mac[4]>.<mac[5]>/16`
— so `3c:1a:cc:4c:83:a5` is `10.77.131.165`. `AT+MESHDHCP=0` skips the DHCP
attempt. Batman mode differs: the address comes up on the first batman route,
with a 45 s DHCP wait, an ARP-probed `10.41.253.x/16` fallback beside which DHCP
keeps being asked, and a new lease when the old one's router leaves
([Batman Mode](wiki/Batman-Mode.md)).

```
AT+MPMPEERS?                     peers, handshake state, AMPE key counters
AT+SAERX?                        SAE/peering state machine (encrypted build)
AT+MESHSEC=0                     open data plane (open build, unencrypted peers)
AT+MPING=10.77.199.248,4         confirm data
```

### Interoperating with OpenMANET / OpenWrt

Warthog meshes with Linux `mac80211` peers. Verified against OpenMANET 1.8.0 on
a Raspberry Pi 4 with a Seeed HaLow HAT, meshing with two Warthog nodes at once —
0–3% loss, 8–19 ms round trip, with the peer configured by hand for an open
mesh (`encryption='none'`).

What the OpenMANET side sees once a warthog has joined — established peer
links, resolved paths at hop count 1, and answered pings:

![OpenMANET view of the mesh](docs/img/openmanet-pi.svg)

**The use case** is hanging phones and EUDs off your mesh: a warthog
joins as a peer and presents a Wi-Fi AP and a USB Ethernet adapter on the other
side. That story, end to end, is in the wiki:
[OpenMANET Gateway](../../wiki/OpenMANET-Gateway).

Two things on that hand-configured peer stopped the measured build dead, each
with no error message: its mesh interface, converted from the stock HaLow
access point, stayed in that AP's `br-lan`, and unbridging it drops it out of
the `lan` firewall zone. Both are covered, with the diagnostic signature of
each, in [`docs/mesh-openmanet.md`](docs/mesh-openmanet.md). A node set up by
OpenMANET's LuCI mesh wizard instead runs SAE (mesh ID `openmanet`, passphrase
`changeme123` unless changed) and makes its mesh interface a batman-adv
(BATMAN_V) member of `bat0`. In plain mesh mode a Warthog peers with such a node
but, per source (not measured), has no IP path into `bat0`. `AT+MESHBATMAN=1`
makes it a BATMAN_V member instead ([Batman Mode](wiki/Batman-Mode.md)); against
the wizard's SAE mesh that needs the host-CCMP build `warthog-mesh-sae-swccmp`,
because `warthog-mesh-sae` refuses it. Measured on air on 2026-09-29/30 against
two OpenMANET 1.8.0 Pis (batman-adv 2025.4) whose `bat0` was set up by hand, one
hop apart: batman tables both ways, a DHCP lease from a Pi, pings, and
Meshtastic's group into a Pi's LAN; not against a wizard node or over more than
one hop. A Linux node's unicast above its RTS threshold (1000 on both bench
Pis) reaches a Warthog from every node only on
`warthog-mesh-sae-swccmp-meshvif`, which runs the mesh on a MESH chip interface,
or with the node set to CTS-to-self or RTS off; on other builds only from the
peer the chip registered last
([OpenMANET Interop](wiki/OpenMANET-Interop.md#frames-over-about-1000-bytes-from-a-linux-node)). Current builds try DHCP first and learn the hosts behind a bridged
peer from Address Extension; neither has been on a radio against a bridged
node.

## Troubleshooting

### "`ping 8.8.8.8` works but `curl example.com` hangs"

A DHCP-DNS gap: ICMP to a numeric IP returns within ~90 ms (HaLow RTT), but name resolution stalls.

**Cause.** The Warthog DHCP server has to hand the host a DNS resolver via DHCP option 6, *and* that resolver has to be reachable through the NAT chain. Two ways that breaks:

1. **No DNS option** — the host gets only IP + gateway, falls back to its primary-interface resolver (usually a `192.168.x.1` or `10.x.x.1` from Wi-Fi), and tries to query that LAN IP through the Warthog USB tunnel. The LAN IP isn't reachable through HaLow's NAT, so DNS just times out.
2. **DNS option present but unroutable** — the upstream HaLow AP's DHCP lease hands the ESP32 a *LAN-side* resolver (e.g. on a HaLowLink 2, `192.168.12.1`). Re-advertising that address to downstream USB clients makes the host NAPT its DNS queries through HaLow toward `192.168.12.1` — which the upstream AP only answers for clients on its own subnet, not for NAT'd traffic. Same hang.

**Fix.** Warthog hands out a *publicly reachable* resolver instead. Default is **`1.1.1.1`** (Cloudflare) — works through any NAT chain, no LAN dependency. Override at runtime to use a different public resolver or a private one reachable from the upstream network:

```
AT+DNS=8.8.8.8
AT+RESET
```

…or at build time, e.g. for a fleet that ships pointed at a Pi-hole on the upstream LAN:

```bash
pio run -e warthog-us -DWARTHOG_DOWNSTREAM_DNS=\"10.0.0.53\"
```

**Verify on macOS** (after reflash + fresh DHCP lease):

```bash
ipconfig getpacket en10 | grep domain_name_server
# Expected: domain_name_server (ip_mult): {1.1.1.1}
```

If the firmware advertises the right DNS but the host still uses the old one, force a new lease: `sudo ifconfig en10 down && sudo ifconfig en10 up`, or unplug and replug the cable. macOS caches the original lease for hours otherwise.

**Implementation note.** Advertising DHCP option 6 requires *two* ESP-IDF calls per netif (USB and Wi-Fi AP), not one:

```c
dhcps_offer_t offer_dns = OFFER_DNS;
esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET,
                       ESP_NETIF_DOMAIN_NAME_SERVER,
                       &offer_dns, sizeof(offer_dns));   /* enable option 6 emission */
esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns); /* set the IP to advertise */
```

`esp_netif_set_dns_info` alone is not sufficient: it stores the address in the dhcps struct, but lwip's dhcpserver only includes option 6 in offers when the `dhcps_dns` enable bit is set, which defaults to `0x00`. Without the `dhcps_option(OFFER_DNS)` call the address is silently never advertised. See `main/usb_net.c` and `main/wifi_ap.c`.

### Debug logging

Verbose runtime diagnostics are gated behind a single build flag so production logs stay quiet. Enable for a debug build:

```bash
PLATFORMIO_BUILD_FLAGS="-DWARTHOG_DEBUG=1" pio run -e warthog-us -t upload
```

That switches on:

- `warthog.nat: tick: halow_ip=… usb.napt=1 ap.napt=1 default=…` every 30 s — confirms the NAPT supervisor is enforcing state.
- `warthog.nat: HaLow STA IP changed: X -> Y (NAPT table now stale; downstream clients will see drops)` — fires on STA re-association with a new IP, the usual cause of downstream clients losing internet after a few minutes.
- New diagnostic blocks should be gated behind the same `#if WARTHOG_DEBUG`, keeping a single knob.

### When in doubt

The CDC console (`/dev/cu.usbmodemXXXX`) carries all ESP-IDF logs after USB-OTG takes over, and `AT+STATUS?` gives a one-shot view of every netif's IP plus the currently-offered DNS. Known failure modes and their diagnostic signatures are documented in `docs/` (`power-notes.md`, `napt-notes.md`).

## Status

| Phase | Description | Status |
|-------|-------------|--------|
| 0 | Repo skeleton + PlatformIO/IDF build | ✅ |
| 1 | HaLow STA bring-up | ✅ SAE association + DHCP lease verified against a HaLowLink 2 |
| 2 | USB net device (CDC-ECM, macOS/Linux) | ✅ DHCP + ping verified on macOS |
| 3 | 2.4 GHz Wi-Fi AP | ✅ SSID `warthog` visible |
| 4 | lwIP NAPT bridge | ✅ end-to-end internet verified (host → USB → HaLow → upstream → 8.8.8.8) |
| 5 | Polish (LEDs, AT, NVS) | ✅ partial — LED state machine, AT commands, NVS persistence shipped; CDC-NCM replaced ECM as the USB class. Windows RNDIS and a web UI deferred. |
| 6 | 802.11s mesh over HaLow | ✅ peering, data plane and HWMP path selection; 3-node warthog mesh verified |
| 7 | OpenMANET / OpenWrt interop | ✅ unencrypted mesh: 0–3% loss, 8–19 ms against OpenMANET 1.8.0. SAE/AMPE peering verified cross-vendor; its data plane on `warthog-mesh-sae-swccmp` only, in batman mode (2026-09-30) — see [`docs/mesh-openmanet.md`](docs/mesh-openmanet.md) |
| 8 | 802.11s forwarding + L2 bridge | 🧪 implemented, host-tested and simulated (`sim_mesh`: relay, flood, proxy, link loss, TTL); **no forwarded or bridged frame has been on a radio** — see [Mesh Mode](wiki/Mesh-Mode.md#forwarding) |
| 9 | BATMAN_V member (`AT+MESHBATMAN=1`) | ✅ one hop from two OpenMANET 1.8.0 Pis (batman-adv 2025.4), `warthog-mesh-sae-swccmp`, 2026-09-29/30: tables, gateway, DHCP, pings, Meshtastic into the Pi's LAN; no relaying measured — see [Batman Mode](wiki/Batman-Mode.md#measured-on-air) |

SAE/AMPE is implemented: the `warthog-mesh-sae` build derives a per-link MTK
per peer, and peering interoperates with an SAE-configured OpenMANET node (the
verified peer was configured by hand). One limit applies: with chip crypto the
**encrypted** data plane is warthog-to-warthog only, because every 802.11s peer
generates its own group key and Warthog puts only its own TX group key in the
chip: on this chip firmware a second group-key install on the mesh interface
(the same key at another AID) broke group decryption (measured; the Linux order,
our own key at AID 0 first, is untested). So a peer's group-addressed frames are
not decrypted in hardware; each peer's group key is kept on the host, where only
host software CCMP (`warthog-mesh-sae-swccmp`) can use it. That build carried
encrypted traffic with OpenMANET 1.8.0 on air on 2026-09-29/30, in batman
mode. That measurement was on the STA chip interface every other build runs the
mesh on. `warthog-mesh-sae-meshvif` keeps the keys in the chip on a MESH chip
interface and installs each peer's group key at that peer's AID, as Linux does,
so the chip can open a peer's group frames itself; measured on air on 2026-10-01 (before that change it peered with the Pis but opened none of their group frames). See
[OpenMANET Interop](wiki/OpenMANET-Interop.md#group-frames-in-the-chip-warthog-mesh-sae-meshvif).

Mesh is no longer confined to the capability builds: `AT+MESHEN=1` enables it
on any image, region envs included, and the mesh ID, passphrase and channel are
runtime settings. The mesh envs remain useful because they pin a channel and
fix the identity at build time.

Implemented but not measured on air: 802.11s forwarding (`AT+MESHFWD=1` — path
selection relayed, unicast and group data relayed, proxied endpoints learned,
link loss announced; host-tested and simulated, see [Mesh Mode](wiki/Mesh-Mode.md#forwarding)).

Implemented but not measured on air, likewise: L2 bridge mode (`AT+MESHBRIDGE=1`
— USB, Wi-Fi AP and mesh as one segment, NAT off; builds on every env with the
lwIP bridge compiled in, has not carried a packet).

BATMAN_V member mode (`AT+MESHBATMAN=1` — a clean-room batman-adv compat-15
member of an OpenMANET `bat0`) is measured on air one hop from OpenMANET 1.8.0
(phase 9); relaying for other nodes, a wizard-configured node and the
remaining items in [Batman Mode](wiki/Batman-Mode.md#not-yet-measured) are not.

Not implemented: per-transmitter group keys in the chip (the host keeps one
per peer, above), multicast across the mesh on a leaf, batman-adv's distributed
ARP table, multicast optimisation, gateway mode and bridge loop avoidance,
Windows RNDIS, and a web UI.

### What is measured, and what is not

The ✅ above means "observed on hardware", and it is worth being precise about
when, because a large amount of the current tree has not been on a radio.

**Measured on hardware.** Phases 0–5 as described. A 3-node warthog 802.11s
mesh, and cross-vendor SAE/AMPE peering to an OpenMANET node, on 2026-09-20.
The unencrypted data-plane figures in phase 7, against OpenMANET **1.8.0**. On
2026-09-21, an OpenMANET **24.10** (`r28739-d9340319c6`) baseline: the peer's
mesh configuration, the absence of a batman fabric on an un-wizarded node, and
proxied endpoints crossing the mesh on air. On 2026-09-29 and 2026-09-30,
`warthog-mesh-sae-swccmp` against two OpenMANET 1.8.0 Pis on an SAE mesh
(`ieee80211w=2`), in batman mode: host CCMP opening the Pis' group frames and
unicast; the Pis' protected group PREQs taken and answered, so they held an
`ACTIVE` path to each Warthog (after the fix in
[Management frame protection](#management-frame-protection-80211w)); batman neighbours,
originators and translation tables both ways; the Warthogs choosing the gateway
Pi; a DHCP lease from a Pi;
pings; Meshtastic's group into a Pi's LAN; heap and stack in batman mode. Also
measured then: on the STA chip interface a Linux node's unicast above its RTS
threshold (1000 on both Pis) reaches a Warthog only from the peer the chip
registered last, which `warthog-mesh-sae-swccmp-meshvif` fixes, and the chip
hands the host unicast data addressed to other stations
([Batman Mode](wiki/Batman-Mode.md#measured-on-air)).

**Measured on 2026-10-03.** Under a mesh, `AT+CHIPRESTART` reloads the chip
and puts the mesh back in about 1.1 s with USB and the peer links up, 12 of 12
in a soak (both `-meshvif` builds, [Mesh Mode](wiki/Mesh-Mode.md#chip-restarts));
`AT+STACKS?` reads at least 2356 bytes free in the `health` task during a
restart and 4192-4388 in ESP-IDF's `wifi` task; an assert
(`AT+ASSERTTEST=at`, `=loop`) reboots the board once in about 6 s with its
record and core dump ([Troubleshooting](wiki/Troubleshooting.md#the-board-drops-off-usb));
host TX fragmentation (`AT+HOSTFRAG`) with chip keys delivers frames cut in 2
to an OpenMANET node and to another Warthog; chip firmware 1.17.6 loses frames
cut in 3 with chip keys and re-encapsulates every fragment after the first with
host CCMP ([OpenMANET Interop](wiki/OpenMANET-Interop.md#what-chip-firmware-1176-does-with-fragments)),
which is why builds cut in at most 2, and not at all with host CCMP. Those
fragments were read with the capture rings (`AT+TXCAP` on the sender, `AT+RXCAP`
on the receiver, each frame the same on both but Duration), and the Block Ack
sessions with `AT+AMPDU?` (`orig`, `addba_tx`). At 1 MHz MCS0 (`AT+TXRATE=0,1`)
a Pi's 1000- and 1472-byte pings are answered 8/8 with chip keys
(`warthog-mesh-sae-meshvif`) and `AT+HOSTFRAG=auto`, the default there (the
session ended by a DELBA, `ba_end`), and 0/8 with `=0`, whose replies the chip
cut in 2 under the Warthog's Block Ack session; with host CCMP
(`-swccmp-meshvif`) 8/8 with `AT+SEALFIT=1`, the default, and 0/8 with `=0`.
Over a two-hour soak, an OpenMANET Pi at MTU 1460 (its `bat0`'s) sent 1452- and
1472-byte pings as two IP fragments and none was answered (0/5 in each of 24
rounds, both boards): ESP-IDF's lwIP drops IP fragments addressed to it by
default. Builds from then on reassemble them
([Troubleshooting](wiki/Troubleshooting.md#large-packets-from-a-node-at-mtu-1460-go-unanswered-ip-fragments)).
With such a build, the same day, both boards: the Pi at MTU 1460 and at 1500
pinged the Warthog with up to 14392 bytes (10 fragments), 5/5; a Mac on the
Warthog's USB pinged the Pi through NAT with 100, 1472, 1473, 2000 and 6000
bytes, 3/3 each (one 6000-byte run 2/3), `AT+MTU?` `ip_reass` rising both ways
and `ip_reass_drop` 0. With the USB link's rewrite (one owner of the network
class, NCM's receive renewed per datagram) the Mac's pings to the board of 100
to 6000 bytes went 3/3 with the link up throughout, where before 1473 bytes went
1/3, 2000 bytes 0/3 and then nothing until a reset; bursts of 100 × 1400 and
40 × 6000 bytes lost nothing, and `+USBNET` read `rx` above `rx_xfer` (the host
packing blocks), `drop_full` 0, `tx_stall_ms` 0
([Troubleshooting](wiki/Troubleshooting.md#usb-networking-stops-after-large-pings)).

**Not measured.** Everything else added from 2026-09-21 on is compiled, reviewed
and where possible host-tested, but has not run on a radio: receive-side Address
Extension against a real bridged peer; leaf-mode learning of hosts behind any
mesh node (with the relay that carried them) and the Address Extension mode 2
replies to them; DHCP-first netif bring-up; runtime channel configuration on a
region build; per-peer RSSI/SNR/bandwidth; the `fwdcand` forwarding-feasibility
counter; the peering watchdog's output (its cause-selection is unit-tested on
the host; its log lines have never fired on hardware); standard group frames
(`AT+MESHGRP=1`); our own group key going into the chip with the first SAE peer;
the re-install of each surviving link's own AMPE key when an SAE peer is
removed; a peer's advertised group-key RSC used as its replay floor; the
nonzero own-MGTK PN base and its re-install (`-DWARTHOG_MESH_MGTK_PN_BASE`,
set only on the swccmp bench builds, because it assumes the chip honours an
installed group key's PN; a relay's or bridge's group path selection on those
builds depends on it); a chip restart the chip starts itself (a failed health
check or a bus error) and one in client mode; `AT+ASSERTTEST=crit` and
`=hang`, the boot watchdog and its two USB retries, safe mode and the chip held
in reset there and at the hang stop; the 2-fragment rule with chip keys other
than at 1 MHz MCS0 alone (a frame moved to rates where 2 fragments are enough,
`clamp`; with `AT+HOSTFRAG=0` too, for frames the chip cuts), a chip-sealed
frame sent whole under the Warthog's Block Ack session with `AT+HOSTFRAG=0`, or
before the DELBA that ended one for a cut is through (`seal_ba`), `AT+SEALFIT`
with host CCMP other than at 1 MHz MCS0 alone, group frames (`AT+MESHGRP=1`)
sent only at a rate that carries them whole and whether the chip cuts them
otherwise, the link budget those faster rates cost (derived: about 3 dB at 1 MHz
MCS1, 5 dB at MCS2), the RTS/CTS choice of a first rate the rule puts in, the TX
pool reserve of 5, the chip's rate for an A-MPDU whose frames' chains differ,
host TX fragmentation's Block Ack wait (a cut frame held until the DELBA's TX
status and 20 ms more, ADDBA held off 15 s, `delba_noack`), `AT+AMPDU=0`,
`AT+TIDPARAMS` (measured only as making no difference to 3-fragment loss; on by
default on the chip-key builds, whether the chip aggregates whole frames on it
is not), the `warthog_led` task's stack at 3072 bytes; bat0's 100 ms wait for
a transmit slot (host-tested); the drop of whole TCP, UDP and ICMP packets
shorter than their header ahead of NAPT, which stock NAPT reads and rewrites
past their end (`AT+MTU?` `ip_short_drop`; host-tested on IDF's own lwIP, under
ASan); the CDC-ECM build (`warthog-us-ecm`) on a host;
peer-capacity signalling at 4 peers (the accepting bit, Close(53), the
re-announce and the SAE offer gate); PREQ/PREP lifetimes up to 60 s and the
600 s sweep of lapsed paths and proxy entries; the Meshtastic
repeater's one-socket-per-interface receive and send (the repeater was measured
on air before that change); AT replies longer than the 512-byte USB FIFO, now
sent in pieces under a port lock; `AT+MESHPMF=1`; a relay's or bridge's group
path selection sent as group-addressed privacy under our MGTK (receiving a
node's is measured);
Block Ack frames to a peer that runs MFP (an OpenMANET wizard node) sent protected, so A-MPDU sessions can
form with it where every ADDBA was dropped before; batman mode beyond one hop
and against a wizard-configured node; the receive filter's drop of unicast
data addressed to other stations (`not_ours`), and whether the chip hands up
unicast management frames addressed to them too (`mgmt_nours`, counted, not
dropped); and the whole of 802.11s
forwarding and bridge mode — every forwarding decision is host-tested and a
multi-node simulator drives the shipping code through relay, flood, proxy,
link-loss and TTL scenarios, but no forwarded frame has been on a radio, and
whether the chip delivers third-party frames to the host at all is the
`fwdcand` question above. Host software CCMP's refusal of a unicast keyed with
a group key (`AT+SWCCMP?` `grpkey=`) is host-tested only.

**What the unmeasured receive-side work does to the measured path.** A
previous revision of this paragraph claimed warthog never emits a Mesh Control
field, so its own frames could never enter the new receive code. That was
wrong: every mesh data frame warthog transmits carries a 6-byte Mesh Control
(`umac_datapath.c`, "Mesh Control Present" — measured, the peer's MM6108 drops
4-address data without it). So warthog-to-warthog frames DO take the
`mesh_ctrl_present` branch. What that branch does to them is the pre-existing
6-byte strip plus three additive counters; the Address Extension capture only
fires when the sender set AE flags, which a warthog does only when forwarding,
bridging, or replying to a host learned behind another node — never to another
warthog in the default NAT and leaf mode. The 3-node result from 2026-09-20 was
measured with that strip already in place. The exposure is therefore the
counters and, on foreign frames, the AE parse and the host learning it feeds,
not the strip.

If you are deciding whether to trust this for something that matters, the
not-measured paragraph is the honest answer.

## Security

**Treat the mesh as an untrusted transport and protect traffic above it.** That
is not a placeholder caveat; it follows from how 802.11s works and from what
this firmware currently ships.

### 802.11s SAE is hop-by-hop, never end-to-end

SAE authenticates a *link*. Each mesh hop decrypts a frame and re-encrypts it
for the next hop, so every node a packet traverses sees it in the clear. A
mesh is a group of peers that all hold the same passphrase, which means:

- **No per-node identity.** Possession of the passphrase is the whole
  credential. Any device holding it is a full member.
- **No revocation.** Removing a node means changing the passphrase on every
  other node. On Warthog that is `AT+MESHPASS=` plus a reboot per node — no
  reflash — but it is still every node, and there is no way to exclude one
  device without re-keying all the others.
- **Any member can decrypt anything traversing it.** A compromised or captured
  node reads all traffic routed through it, and can inject.

This is a property of 802.11s, not a Warthog limitation. OpenMANET nodes on the
same mesh are in exactly the same position.

### What each mode actually gives you

| Mode | Peering | Data plane | Use |
|---|---|---|---|
| Non-SAE mesh, default (`AT+MESHSEC=1`) | open, unauthenticated | CCMP under a **public constant** | warthog-to-warthog only; not link security |
| `AT+MESHSEC=0` | open, unauthenticated | cleartext | interop with an OpenMANET node set to `encryption='none'` |
| `warthog-mesh-sae` | SAE (Dragonfly) | CCMP under a per-link AMPE MTK | the only mode with real link security |

The `AT+MESHSEC=1` key, the default on every image built without SAE (the
region builds and `warthog-mesh-smoke`), is `00 11 22 33 … ff` — a counting
sequence compiled into every Warthog image. It is not a secret and anyone with
the firmware has it. It also cannot interoperate: a peer that derives real keys
or sends cleartext can neither read those frames nor be read by them.

### The default SAE passphrase is in the binary

The passphrase is a runtime setting — `AT+MESHPASS=<pass>` stores it in NVS and
it takes effect on the next boot. `AT+MESHPASS?` reports its length and never
its value.

What is in the binary is the *default*: `WARTHOG_MESH_PASSPHRASE`, which is
`warthog-mesh` unless the build overrides it. A node that has never been given
a passphrase is therefore on a passphrase that anyone holding the image knows.
Set one before deploying, and treat a published or shared image as a published
default. The build-time override still exists if you would rather ship images
that are safe before they are configured:

```bash
pio run -e warthog-mesh-sae --build-flag='-UWARTHOG_MESH_PASSPHRASE' \
                            --build-flag='-DWARTHOG_MESH_PASSPHRASE=\"your-passphrase\"'
```

### Management frame protection (802.11w)

MFP is off by default: the SAE build's mesh join advertises no management frame
protection (`AT+MESHPMF=0`). An OpenMANET peer running `ieee80211w=2` still
peers with it.

Unicast path selection (HWMP PREQ, PREP, PERR) follows each peer's MFP, as
mac80211 does. A keyed SAE peer runs MFP once its AMPE has delivered an IGTK
(hostap sends one exactly when the peer runs `ieee80211w` 1 or 2; 2 is the
OpenMANET wizard default), once it sends us protected unicast path selection, or,
with `AT+MESHPMF=1`, always. The RSN element of a peering Open is not used: nothing
authenticates it. Toward such a peer unicast path selection goes out
CCMP-protected under that link's key, by the same route as its data: the chip
(`HW_ENC`), or host CCMP on the swccmp builds; and from it, it must arrive
protected.

Group path selection is group-addressed privacy (802.11 Table 9-47; mac80211's
`ieee80211_is_group_privacy_action`), whatever either side's MFP: on an SAE mesh
mac80211 sends every group PREQ and PERR CCMP-protected under its own MGTK, with
the Protected bit and no MMIE, and from a peer that runs MFP it drops a group one
in the clear or carrying a BIP MMIE. Warthog does the same, and refuses one in the
clear from a peer without MFP too (below). Its own group path
selection (a relay's broadcast PREQs and PERRs, a bridge's broadcast PREQs) goes
out Protected under its own MGTK, sealed by the chip as its group data is, one
frame for every peer; before hostap delivers that MGTK, or while its install in
the chip has failed (retried with the next peer), it goes in the clear. A
peer's group path selection is taken only if it arrived Protected, host CCMP
(or, on `warthog-mesh-sae-meshvif`, the chip holding that MGTK
at the peer's AID) opened it under that peer's MGTK (from its AMPE), and its PN is above that key's
management replay counter, whose floor is the RSC the AMPE carried. Answering it
is the only way a 1.8.0 node, which sends Warthog unicast only over an HWMP path,
gets that path. In the clear it is refused from every established peer, MFP or
not. That is stricter than mac80211, which takes it in the clear from a peer
without MFP, and loses nothing against it: every established SAE peer's AMPE
delivered its MGTK, and mac80211 and Warthog both protect group path selection
with it. Taken in the clear, anyone on the channel could send it in the name of a
peer without MFP and have a relay re-send it under Warthog's MGTK, which every MFP
node opens. It is also refused with an MMIE (18 or 26 octets, as mac80211 finds
one), under another key id, or opened by the chip under anything but the
sender's own MGTK at its AID. On every build but `warthog-mesh-sae-meshvif` the chip's only group key is Warthog's own MGTK, which every
peer holds, so such a frame could be forged in the sender's name by any of them. The chip cannot open a peer's group
path selection (except on `warthog-mesh-sae-meshvif`), so only
the swccmp builds with host CCMP on can take it, as for
group data, whether it comes from a Linux node or a Warthog relay or bridge; on
`warthog-mesh-sae` a wizard node never gets a path to Warthog. The same holds
between Warthogs: on `warthog-mesh-sae`, `-nochipkey`, and a swccmp build with
host CCMP off, a relay or bridge takes no group PREQ or PERR from another Warthog,
at either `AT+MESHPMF` setting, where before this change they were taken in the
clear (`0`) or with a BIP MMIE (`1`). Updating every Warthog does not bring that
back: relay discovery between Warthogs under SAE needs a swccmp build with host
CCMP on (`-swccmp-on`, or `AT+SWCCMP=1` after each boot). A Warthog image from
before this change refuses a Protected group frame and sends group path selection
in the clear or with an MMIE, both of which this one refuses: update every Warthog
on an SAE mesh together.

Under SAE, a protected unicast management frame is accepted only under the
link's pairwise key id; one under a group key, which every mesh member holds, is
refused. So is a unicast data frame the chip decrypted under any key id but the
link's pairwise one (`AT+RXCHAN?` reason 96); the `-nochipkey` and `-swccmp`
builds, whose chip holds no pairwise key, refuse every unicast data frame the
chip decrypted. Path selection from a station whose link AMPE has not keyed
(one the supplicant added before SAE finished) is refused, as mac80211 takes it
only from an established peer.

Group path selection is built and sent on the umac event loop: from another
task (the network stack) it is queued for it, up to four frames, and dropped if
the queue or the loop's event queue is full. A leaf sends only unicast path
selection (its PREPs); a relay (`AT+MESHFWD=1`) broadcasts PREQs and PERRs, a
bridge (`AT+MESHBRIDGE=1`) PREQs. None needs `AT+MESHPMF=1` against an
OpenMANET mesh. An open mesh and a non-SAE keyed mesh protect nothing more;
their group path selection is queued the same way.

`AT+MESHPMF=1` (stored; applies after `AT+RESET`) sets PMF to *required* in the
mesh join (`umac/supplicant_shim/supplicant_core_mesh.c`): the RSN capabilities
ask for MFP, hostap's `mesh_rsn` generates an IGTK and sends it in every AMPE
Open (nothing of Warthog's carries an MMIE), and every keyed peer is treated as
running MFP. A peer that runs MFP off still accepts our protected unicast path
selection (mac80211 decrypts it with the link key).

`AT+MESHFWDSTAT?` counts it: `hwmp prot/unprotected/unestab/gp/mmie/nommie`
for path selection received, `hwmp tx prot/gp/plain` for sent, `qdrop` for
group frames dropped instead of queued and `qfail` for queued ones the event loop
could not send, `mgmt prot chip/host/nodec` for
protected management frames opened by the chip (unicast only: a group one it opens
counts as `mgmt gp own` instead), by host CCMP, or by neither (a group one counts
in `mgmt gp nodec` as well),
`grpkey` for protected unicast ones refused under a group key, `mgmt gp
nodec/own/key/replay` for protected group ones refused, and `igtk` for
peer IGTKs installed.

**Measured broken, fixed from source, re-measured.** On 2026-09-29, against
OpenMANET 1.8.0 nodes with `ieee80211w=2`, Warthog refused every group PREQ the
nodes sent (then counted `bipfail`, though host CCMP had opened each one), so no
node held a path to it and no unicast from a node arrived. The group rules above
come from mac80211's source (`net/mac80211/rx.c`, `tx.c`, `wpa.c`) and the host
tests. On 2026-09-30 (`warthog-mesh-sae-swccmp`, batman mode, `AT+MESHPMF=0`)
the nodes' `iw dev wlh0 mpath dump` listed both Warthogs `ACTIVE` at hop count
1, `hwmp gp` counted their group PREQs taken, and their unicast arrived. The
group path selection a Warthog relay or bridge sends rests on two chip
properties that are unmeasured. One is whether the chip encrypts a group management frame under
Warthog's MGTK at all. The other applies to `-swccmp` and `-swccmp-on`, which
install that MGTK at a nonzero TX PN and advertise one below it as its Key RSC: a
node, or another Warthog, takes our group PREQs and PERRs only if the chip starts
the key at that PN. If it does not, every one is dropped as a replay, and each
re-install (`mgtk_reinst`, now after every AMPE Open that follows a relay's or
bridge's group path selection, not only with `AT+MESHGRP=1`) reuses PNs under the
same key. The node's `iw dev wlh0 mpath dump` showing Warthog `ACTIVE` settles
neither: that path comes from the node's own PREQ and Warthog's unicast PREP, and
`hwmp tx gp` counts frames sent, not taken. [OpenMANET
Interop](wiki/OpenMANET-Interop.md#management-frame-protection-peering-does-not-need-it-path-selection-does)
has the check that does. Every other build installs our MGTK at PN 0 and
advertises RSC 0, so a peer that joins or re-peers can be fed each of our earlier
group frames under that key once, until the MGTK changes. `AT+MESHPMF=1` has
never been run on air.

### Short packets through NAT

ESP-IDF's NAPT reads a TCP, UDP or ICMP header without checking the packet holds
one, and on a session match rewrites its port and checksum past the packet's
end. Any mesh member or tethered host can send such a packet, also to 0.0.0.0
while the Warthog has lost its HaLow lease, since NAPT sessions outlive it.
Warthog's lwIP input hook drops them first, with UDP packets too short for their
destination port, which lwIP's DHCP check reads (`AT+MTU?` `ip_short_drop`;
[`docs/napt-notes.md`](docs/napt-notes.md#short-packets)). Host-tested under
ASan; not measured on a board.

### For anything that actually needs confidentiality

Run an end-to-end tunnel over the bridged segment and let the mesh be plumbing:

- **WireGuard** between the endpoints that matter — per-peer keys, real
  identity, revocation by removing a key.
- **Reticulum** if you want end-to-end encryption plus its own routing over a
  transport you do not trust.

Either gives you the per-node identity and revocation that 802.11s SAE
structurally cannot.

## Licensing

Warthog's own code is **GPL-3.0-or-later** (see [`LICENSE`](LICENSE)).

It vendors the Morse Micro IoT SDK under `components/halow/`, which is **not**
all GPL and is not warthog's to relicense. What is in the tree:

| Component | Licence | Notes |
|---|---|---|
| Warthog firmware (`main/`, mesh port, tests) | GPL-3.0-or-later | This project |
| Morse Micro SDK sources | `GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial` | Dual; distributed here under the GPL branch |
| Third-party SDK components | Apache-2.0, MIT, BSD-3-Clause, GPL-2.0-or-later, Zlib | Per-file SPDX headers |
| HaLow firmware and board-config blobs (`*.mbin`) | `LicenseRef-MorseMicroBDL` | Binary Distribution Licence |

Full licence texts are in
[`components/halow/components/mm-iot-sdk/LICENSES/`](components/halow/components/mm-iot-sdk/LICENSES/).

The firmware blobs are redistributed **complete and unmodified**, which is what
the Morse Micro Binary Distribution Licence permits, and solely for use with
hardware containing a Morse Micro HaLow chip. If you fork this repository, keep
them unmodified and keep the `LICENSES/` directory with them. Do not assume the
GPL applies to the blobs — it does not.

## Regulatory

The radio's regulatory domain is fixed at build time by the region env you
choose (`warthog-us`, `-eu`, `-jp`, `-kr`, `-au`), which selects both the board
config and the channel list. **Build the env for the jurisdiction you are
operating in.** Transmitting on another region's channel plan is very likely
illegal where you are, and sub-GHz spectrum differs sharply between regions.

Warthog is an experimental project. You are responsible for operating it within
your local rules, including duty-cycle and transmit-power limits.

