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
  elf_to_mmfw.py [--bcf-addr ADDR] [--objcopy PATH] <input.elf> <output.mbin>

objcopy is --objcopy, else $OBJCOPY, else the newest riscv32-esp-elf-objcopy under
$PLATFORMIO_CORE_DIR (default $HOME/.platformio)/packages/toolchain-riscv32-esp*/bin; warthog
builds install none (README.md says where to get one). Exits non-zero, writing nothing, when objcopy
fails on a section or no segment is extracted.
"""
import argparse, glob, os, shutil, struct, subprocess, sys

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("input_elf")
ap.add_argument("output_mbin")
ap.add_argument("--bcf-addr", type=lambda v: int(v, 0), default=0x8011fa80)
ap.add_argument("--objcopy")
args = ap.parse_args()
input_elf, output_mbin, bcf_addr = args.input_elf, args.output_mbin, args.bcf_addr
if os.path.realpath(output_mbin) == os.path.realpath(input_elf) or \
        (os.path.exists(output_mbin) and os.path.exists(input_elf) and os.path.samefile(output_mbin, input_elf)):
    sys.exit(f"error: the output {output_mbin} is the input")

# Find the RISC-V objcopy: --objcopy, else $OBJCOPY, else the newest in the PlatformIO core.
RISCV_OBJCOPY = args.objcopy or os.environ.get("OBJCOPY")
if not RISCV_OBJCOPY:
    core = os.path.abspath(os.path.expanduser(os.environ.get("PLATFORMIO_CORE_DIR") or "~/.platformio"))
    objcopy_candidates = glob.glob(os.path.join(core, "packages/toolchain-riscv32-esp*/bin/riscv32-esp-elf-objcopy"))
    RISCV_OBJCOPY = max(objcopy_candidates, key=os.path.getmtime, default=None)
RISCV_OBJCOPY = RISCV_OBJCOPY and shutil.which(os.path.expanduser(RISCV_OBJCOPY))
if not RISCV_OBJCOPY:
    sys.exit("error: riscv32-esp-elf-objcopy not found (warthog builds install no RISC-V toolchain): "
             "pass --objcopy PATH or set OBJCOPY; scripts/firmware/README.md says where to get one")

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
        # An absent section exits 0 with an empty file, so this is a real error (wrong objcopy, bad ELF).
        shutil.rmtree(tmpdir)
        sys.exit(f"error: {RISCV_OBJCOPY} failed on {sec_name} (rc={result.returncode}): {result.stderr.strip()}")
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

shutil.rmtree(tmpdir)
if not segments:
    sys.exit(f"error: no segment extracted from {input_elf}; {output_mbin} not written")

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
