# Firmware container tooling

Tools for inspecting the MMFW container format used by the HaLow firmware
images, written while working out why mesh receive behaved differently between
the SDK firmware and the Linux driver's.

The prepared `.mbin` images this directory once held have been removed. They
were Morse Micro firmware re-wrapped into a different container, and the Morse
Micro Binary Distribution Licence permits redistribution only of the firmware
"complete, unmodified, and as provided by Morse Micro". Rebuild them locally
from your own copy of the vendor firmware if you need them; do not redistribute
the result.

## What's here

| File | Purpose |
|---|---|
| `mmfw_extract.py` | Parse MMFW TLV container, decompress and write each chip-memory segment as raw bin |
| `elf_to_mmfw.py` | Take a Linux Morse Micro firmware ELF and re-wrap as MMFW (loadable by morselib SDK) |

## Why these exist

Phase 4i extracted the SDK's `mm6108.mbin` (MMFW TLV with raw segments),
mapped its memory layout to the Linux ELF firmware sections, and found
that both formats load to **identical chip memory addresses**:

| Region | Load Addr | SDK | Linux softmac | Linux thin-LMAC |
|---|---|---:|---:|---:|
| host_imem | 0x00100000 | 97684 | 97726 | 97738 |
| host_dmem | 0x80100000 | 8012 | 8012 | 8028 |
| mac_imem  | 0x00120000 | 154836 | 155064 | 146856 |
| mac_rodmem | 0x00150000 | 4168 | 4166 | 4178 |
| mac_dmem  | 0x80200000 | 21408 | 21432 | 22752 |
| uphy_imem | 0x001f0000 | 64668 | 64744 | 64744 |
| uphy_dmem | 0x80300000 | 14648 | 14648 | 14648 |
| lphy_imem | 0x00158000 | 28552 | 28610 | 28610 |
| lphy_dmem | 0x80400000 | 5344 | 5344 | 5344 |

**Critical finding:** `mac_rodmem` strings are byte-for-byte identical
across all three variants (same task names `mesh_tbtt`, `mesh_delayed_start`,
`beacon`, `con_loss`, `ap_ps_rem_awake`, `ps_wup`, `Qkkbal`, `Tmr Svc`).
The MAC firmware is the same code with different builds (Feb 2026 vs Apr
2026).

`elf_to_mmfw.py` repackages an ELF into the SDK's MMFW format. Images built
this way were verified on hardware (2026-05-26): both the softmac and thin-LMAC
v1.17.9 builds boot on the MM6108 in a XIAO + Seeed HaLow setup, execute the
standard morselib init path, accept the full mesh command sequence, and
transmit probe requests on air.

Chip RX behaviour for foreign-BSSID mesh frames was identical across all three
firmware variants, which is what ruled the firmware out as the cause of the
mesh receive problem at the time.

> **Superseded.** That investigation concluded the chip was locked into a
> restrictive mode by morselib's init sequence. It was not. Mesh receive works
> on the stock SDK firmware: the peer frames were arriving and being discarded
> further up, and the fixes were in address handling and element parsing rather
> than anywhere near the firmware image. Swapping firmware is not necessary and
> is kept here only as a record. See [`../../docs/mesh-openmanet.md`](../../docs/mesh-openmanet.md).

## How to use

### Swap to Linux firmware for testing
```sh
SDK_FW=components/halow/components/mm-iot-sdk/framework/morsefirmware/mm6108.mbin
cp "$SDK_FW" "$SDK_FW.orig_sdk_1_17_6"
cp your-locally-built-mm6108_softmac.mbin "$SDK_FW"
pio run -e warthog-mesh-smoke -t upload
```

### Restore original SDK firmware
```sh
SDK_FW=components/halow/components/mm-iot-sdk/framework/morsefirmware/mm6108.mbin
cp "$SDK_FW.orig_sdk_1_17_6" "$SDK_FW"
pio run -e warthog-mesh-smoke -t upload
```

### Regenerate the MMFW from a newer Linux ELF
```sh
# Download new firmware
curl -O https://raw.githubusercontent.com/MorseMicro/morse-firmware/main/firmware/mm6108.bin
# Convert
~/.platformio/penv/bin/python scripts/firmware/elf_to_mmfw.py mm6108.bin mm6108.mbin
```

### Decompose an MMFW for analysis
```sh
~/.platformio/penv/bin/python scripts/firmware/mmfw_extract.py mm6108.mbin /tmp/mm6108_segments
ls /tmp/mm6108_segments
```

## Format reference

MMFW TLV container (from `morselib/include/mbin.h`):
- `0x8000` MAGIC, len=4, data = `MMFW` (`0x57464d4d`)
- `0x0001` BCF_ADDR, len=4, data = BCF load address (e.g. `0x8011fa80`)
- `0x8001` FW_SEGMENT, len=4+N, data = `load_addr(u32 LE) + raw bytes`
  (capped at 32KB per TLV; split larger sections into multiple TLVs)
- `0x8002` FW_SEGMENT_DEFLATED, same as 0x8001 but data is `load_addr +
  chunk_size(u16) + zlib_hdr(2) + deflate stream` (not used by SDK)
- `0x8f00` EOF, len=0
