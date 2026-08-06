#!/usr/bin/env python3
"""Wrap a Linux Morse Micro firmware ELF in MMFW container format.

Output is suitable for the morselib SDK's firmware_mbin.c loader.

The MMFW format (from morselib/include/mbin.h):
  TLV: type=0x8000 (MAGIC), len=4, data='MMFW' (0x57464d4d)
  TLV: type=0x0001 (BCF_ADDR), len=4, data=BCF load address
  Per LOAD-able section:
    TLV: type=0x8001 (FW_SEGMENT), len=4+N
         data: load_addr(u32 LE) + N raw bytes of section data
    (segments capped at 32KB; longer sections split into multiple TLVs)
  TLV: type=0x8f00 (EOF), len=0

The SDK MMFW splits its segments at 32KB boundaries. We mirror that.

Usage:
  elf_to_mmfw.py <input.elf> <output.mbin> [--bcf-addr=0x8011fa80]
"""
import struct, sys, subprocess, os

if len(sys.argv) < 3:
    print(__doc__)
    sys.exit(1)

input_elf = sys.argv[1]
output_mbin = sys.argv[2]
bcf_addr_arg = next((a for a in sys.argv[3:] if a.startswith("--bcf-addr=")), "--bcf-addr=0x8011fa80")
bcf_addr = int(bcf_addr_arg.split("=")[1], 0)

# Find the RISC-V objcopy
import glob
objcopy_candidates = sorted(glob.glob(os.path.expanduser("~/.platformio/packages/toolchain-riscv32-esp*/bin/riscv32-esp-elf-objcopy")))
if not objcopy_candidates:
    print("Error: riscv32-esp-elf-objcopy not found")
    sys.exit(1)
RISCV_OBJCOPY = objcopy_candidates[-1]

# Section order to match SDK MMFW layout exactly. Load addresses from
# nm output on the Linux ELF.
SECTIONS_LAYOUT = [
    # (section_name, base_addr, max_chunk_size)
    (".host_imem",   0x00100000, 32768),
    (".host_dmem",   0x80100000, 32768),
    (".mac_imem",    0x00120000, 32768),
    (".mac_rodmem",  0x00150000, 32768),
    (".mac_dmem",    0x80200000, 32768),
    (".uphy_imem",   0x001f0000, 32768),
    (".uphy_dmem",   0x80300000, 32768),
    (".lphy_imem",   0x00158000, 32768),
    (".lphy_dmem",   0x80400000, 32768),
]

# Extract each section to a binary file
import tempfile
tmpdir = tempfile.mkdtemp(prefix="mmfw_")

print(f"Extracting from {input_elf} via {RISCV_OBJCOPY}")

segments = []  # list of (load_addr, bytes)

for sec_name, base_addr, max_chunk in SECTIONS_LAYOUT:
    out_file = os.path.join(tmpdir, sec_name.lstrip('.') + ".bin")
    result = subprocess.run(
        [RISCV_OBJCOPY, "-O", "binary", f"--only-section={sec_name}", input_elf, out_file],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        print(f"  warn: failed to extract {sec_name}: {result.stderr.strip()}")
        continue
    if not os.path.exists(out_file) or os.path.getsize(out_file) == 0:
        print(f"  skip: {sec_name} (empty or absent)")
        continue
    with open(out_file, "rb") as f:
        section_data = f.read()
    print(f"  {sec_name:15} addr=0x{base_addr:08x}  size={len(section_data):>7} bytes")
    # Split into 32KB chunks
    offset = 0
    while offset < len(section_data):
        chunk = section_data[offset:offset+max_chunk]
        seg_addr = base_addr + offset
        segments.append((seg_addr, chunk))
        offset += len(chunk)

# Now build the MMFW container
print(f"\nBuilding MMFW container at {output_mbin}")
print(f"BCF load address: 0x{bcf_addr:08x}")
print(f"Total segments: {len(segments)}")

out = bytearray()
# TLV: MAGIC
out += struct.pack("<HH", 0x8000, 4)
out += struct.pack("<I", 0x57464d4d)
# TLV: BCF_ADDR
out += struct.pack("<HH", 0x0001, 4)
out += struct.pack("<I", bcf_addr)
# Per segment
for seg_addr, seg_data in segments:
    payload_len = 4 + len(seg_data)  # load_addr + data
    if payload_len > 0xffff:
        print(f"  ERROR: segment at 0x{seg_addr:08x} too large for u16 length field: {payload_len}")
        sys.exit(1)
    out += struct.pack("<HH", 0x8001, payload_len)
    out += struct.pack("<I", seg_addr)
    out += seg_data
# TLV: EOF
out += struct.pack("<HH", 0x8f00, 0)

with open(output_mbin, "wb") as f:
    f.write(out)

print(f"\nWrote {len(out)} bytes to {output_mbin}")
print(f"  vs reference SDK mm6108.mbin (~399KB)")

# Cleanup
import shutil
shutil.rmtree(tmpdir)
