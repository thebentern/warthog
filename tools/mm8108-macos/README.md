# mm8108-macos

Native macOS userspace control of the Morse Micro **MM8108 Wi-Fi HaLow (802.11ah)
USB adapter** (USB ID `325b:8100`), via libusb. No kernel extension, no root, no
Linux VM.

This exists because driving the adapter through QEMU USB passthrough was too
unstable to measure anything with — `morse_usb_reg32_read failed -110`, `-19
ENODEV`, and occasional kernel panics.

## Status

Verified on hardware (Apple Silicon macOS, MM8108B2, firmware
`rel_mm8108_2_0_0_2026_Apr_21`):

| capability | status |
| --- | --- |
| Claim the interface with libusb, unprivileged | works |
| 32-bit register read/write | works, 10/10 stable |
| Block memory read/write | works |
| Decode the firmware host table | works |
| **`GET_VERSION` (0x0002) command round trip over the YAPS mailbox** | **works, 20/20 stable** |
| Full 802.11s mesh peer | **not possible** — see "Limits" |

## Build

```sh
brew install libusb
make
```

## Use

```sh
./mm8108 info          # chip ID -> "MM8108B2"
./mm8108 hosttable     # host table: magic, API semver, ext table pointer
./mm8108 version       # API semver + firmware boot banner
./mm8108 getversion    # real GET_VERSION command over the YAPS mailbox
./mm8108 readreg 0x2d20
./mm8108 writereg 0x3c58 0x3
./mm8108 readmem 0x0011fd60 128
./mm8108 dumpraw 0x00100000 0x20000 | strings
./mm8108 irq 4         # read the 8-byte YAPS status doorbell
./mm8108 reset         # non-destructive USB bridge reset
```

`-v` traces every 12-byte transport header and the YAPS discovery/framing.

Sample:

```
$ ./mm8108 getversion
GET_VERSION (0x0002) response
  host_id  0x4b30 (sent 0x4b30)
  status   0
  length   28
  version  rel_mm8108_2_0_0_2026_Apr_21
```

## How it works

### 1. The USB layer is a memory bus, not a command protocol

The single most important fact about this device: the USB interface exposes **raw
access to the chip's address space**, nothing more. Everything the Linux driver
does — firmware load, the command mailbox, the datapath — is built on four
primitives: `reg32_read`, `reg32_write`, `dm_read`, `dm_write`.

Reverse-engineered from the GPL driver's `usb.c` (`struct morse_usb_command`,
`morse_usb_mem_read`, `morse_usb_mem_write`). Every transaction is two steps:

1. Write a 12-byte little-endian header to bulk OUT `0x02`:

   | offset | field | meaning |
   | --- | --- | --- |
   | 0 | `__le32 dir` | `0x00` write, `0x80` read, `0x02` non-destructive reset |
   | 4 | `__le32 address` | chip address for the following bulk transfer |
   | 8 | `__le32 length` | byte count of the following bulk transfer |

2. Move `length` bytes: read from bulk IN `0x82`, write to bulk OUT `0x02`.

The device has one vendor-specific interface (class 255 / subclass 255 /
protocol 41) with exactly three endpoints: bulk OUT `0x02` (512), bulk IN `0x82`
(512), interrupt IN `0x81` (8 bytes, "YAPS STAT"). Max transfer 16 KB.

### 2. macOS leaves the device unclaimed

`ioreg` shows only `AppleUSBHostCompositeDevice` on the device node, and the
`IOUSBHostInterface@0` nub has **no driver attached**. Because the interface is
vendor-specific, no macOS class driver matches it, so `libusb_claim_interface()`
succeeds with no kext detach and no elevated privileges.

### 3. Commands ride the YAPS mailbox

MM8108 carries `morse_cmd` request/response over "YAPS", which is built entirely
out of the memory primitives above — there is **no doorbell register**. Three
addresses, all discovered at runtime:

```
reg32_read(0x2d40)              -> host_table pointer (SW manifest pointer)
host_table[0]                   -> magic, must be 0xdeadbeef
host_table[+0x18]               -> extended host table
  {__le32 length; u8 mac[6];} then TLVs from offset 10,
  each {__le16 tag; __le16 length}, length INCLUDING the header
  tag 3 = YAPS table -> ysl_addr, yds_addr, status_regs_addr,
                        yaps_reserved_page_size
```

On the observed device: `ysl = yds = 0x00170000` (the same address is both the
pop and push port), `status = 0x00178000`, `reserved_page_size = 256`.

Each packet is a 4-byte delimiter plus payload:

| bits | field |
| --- | --- |
| 0-13 | `pkt_size + yaps_reserved_page_size` |
| 14-16 | pool id (to-chip TX=0 **CMD=1** BEACON=2 MGMT=3; from-chip RX=4 **CMD_RESP=5** TX_STATUS=6 AUX=7) |
| 17-18 | padding to a 4-byte boundary |
| 19 | IRQ — **this bit is the doorbell** |
| 20-24 | reserved, zero |
| 25-31 | CRC-7 (Linux `crc7_be`, poly `0x89`) over bits 0-24, `>> 1` |

The payload is a 40-byte `morse_buff_skb_header` (`sync=0xAA`, `channel=0xFE`,
`len`, `offset`, zeroed checksums, 32-byte union) followed by the 12-byte
`morse_cmd_header`. So a bare `GET_VERSION` is a 56-byte write to `yds_addr`.

To read the reply: poll the 72-byte status block (18 × `u32`) until word 17
(`lock`) is zero, then take word 14 (`fc_rx_bytes_in_queue`) and `dm_read` that
many bytes from `ysl_addr`. The response payload is
`morse_cmd_header(12) + __le32 status + __sle32 length + version[]`.

### Gotcha: acknowledge the interrupt latch

`INT1_STS` (`0x3c50`) bits 0-1 latch when YAPS has traffic, and the chip stops
signalling until they are cleared by writing them to `INT1_CLR` (`0x3c58`).
If a host disappears mid-flight (killing a VM, say), the latch is left set and
every subsequent command appears to time out. The tool now acknowledges before
and after each command round trip, which is what `morse_hw_irq_handle()` does.
That took the success rate from intermittent to 20/20.

## Firmware persistence

Firmware lives in chip RAM and **survives the host going away**. After killing
the QEMU VM that had loaded it, `0x2d40` still pointed at a valid host table with
the `0xdeadbeef` magic, and commands were serviced normally.

- Driver unbind / rebind, or a non-destructive reset: firmware survives. The
  driver deliberately writes `AON_RESET_USB_VALUE` to keep the USB power domain
  alive across a digital reset.
- Physical unplug or power cycle: RAM is lost, `0x2d40` reads back zero, and a
  full ELF download is required.

This tool does **not** implement firmware download. If `hosttable` reports a null
pointer, load firmware from Linux first (or implement the ELF `PT_LOAD` walk —
the driver's `firmware.c` does it purely through `dm_write` plus a boot-address
register write, so it is portable in principle).

## Limits — how far this can actually go

**Control plane: yes. Data plane: no.**

This tool can do anything expressible as a `morse_cmd`: query and set chip
configuration, read telemetry and statistics, change channel and TX power, and
in principle drive `SET_MESH_CONFIG` (`0xA018`). That is genuinely useful for
bring-up, measurement, and regression testing without a VM in the loop.

It cannot make macOS a **802.11s mesh peer**. The reason is structural, not
effort: the MM8108 is a *softmac* part. The chip does the PHY and low MAC, but
802.11s — beaconing, peer link management (the MPM state machine), path
selection (HWMP), the encrypted mesh datapath, and the netdev that carries IP —
all live in Linux `mac80211` plus `dot11ah`, which shims S1G into 802.11n for
`mac80211`'s benefit. Reimplementing that in macOS userspace is reimplementing
`mac80211`, and even then there is no way to present a real network interface to
the macOS stack without a DriverKit/kext network driver.

So the honest split:

- **Native macOS**: chip control, register and memory access, firmware version
  and telemetry, command round trips, scripted test automation. Reliable —
  20/20 versus a QEMU passthrough that died roughly 14 times in one session.
- **Still needs Linux**: an actual mesh peer that forwards traffic.

For mesh work the better answer is a real Linux host (a Raspberry Pi or similar
with the adapter plugged directly in), not a VM on this Mac. Vendor support is
Linux-only: Morse Micro ships `morse_driver`, `morse_cli`, and OpenWrt packages,
and has no macOS driver or SDK.

## Notes on `morse_cli`

Morse Micro's `morse_cli` (morsectrl) is open source and has a pluggable
transport registry including driverless transports (`ftdi_spi`, `uart_slip`).
Porting it to macOS and adding a libusb transport looks attractive but **does not
get you commands**: its only reg/mem-based command mechanism is "memcmd", which
reads `memcmd_cmd_addr` from the host table at `+0x10` and refuses outright when
it is zero ("not supported for production firmware"). On this device both memcmd
fields read `0x00000000` — production firmware is built without that hook, and no
host-side action changes it. The kernel driver never uses those fields either.
YAPS, implemented here, is the path that actually works.

Its `usb.h` does contain the same constants derived here independently
(`MORSE_BULK_OUT_EP 2`, `MORSE_CMD_SIZE 12`, `MORSE_CMD_RESET 0x2`), which is a
useful cross-check, but it only implements a bulk reset, not a transport.

## References

All offsets here were derived from the GPL-licensed Morse Micro Linux driver
(`morse_driver`): `usb.c` (USB transport), `mm8108.c` (register map),
`hw.h` (host table), `firmware.c` (host table discovery, firmware load),
`yaps_hw.c` / `yaps.c` (mailbox, delimiter, status block),
`skb_header.h` (packet header), `morse_commands.h` (command IDs and structs).
