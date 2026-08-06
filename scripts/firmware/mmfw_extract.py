#!/usr/bin/env python3
"""MMFW container extractor.

Format (from mbin.h):
  magic   (uint32 LE)  = 0x57464d4d ("MMFW")
  TLVs in sequence:
    type  (uint16 LE)
    length (uint16 LE)
    data (length bytes)

Field types (from mbin.h):
  0x0001  FW_TLV_BCF_ADDR
  0x8000  MAGIC
  0x8001  FW_SEGMENT          (data: load_addr(u32 LE) + raw bytes)
  0x8002  FW_SEGMENT_DEFLATED (data: load_addr(u32 LE) + chunk_size(u16 LE) + zlib_hdr(2 bytes) + deflate stream)
  0x8f00  EOF
  0x8f01  EOF_WITH_SIGNATURE
"""
import sys, struct, zlib, os

if len(sys.argv) < 3:
    print("Usage: mmfw_extract.py <input.mbin> <output_dir>")
    sys.exit(1)

inpath = sys.argv[1]
outdir = sys.argv[2]
os.makedirs(outdir, exist_ok=True)

with open(inpath, 'rb') as f:
    data = f.read()

if len(data) < 8:
    print(f"too short: {len(data)} bytes")
    sys.exit(1)

# The file starts with TLVs. First TLV is type=0x8000 (MAGIC) with data 'MMFW'.
first_tlv_type, first_tlv_len = struct.unpack('<HH', data[:4])
if first_tlv_type != 0x8000 or first_tlv_len != 4:
    print(f"Unexpected first TLV: type=0x{first_tlv_type:04x} len={first_tlv_len}")
    sys.exit(1)
magic_data = struct.unpack('<I', data[4:8])[0]
print(f"First TLV: type=MAGIC, data=0x{magic_data:08x} ({data[4:8].decode('ascii', errors='replace')})")
if magic_data != 0x57464d4d:
    print(f"Not MMFW signature!")
    sys.exit(1)

off = 8
segments = []
segment_index = 0
while off + 4 <= len(data):
    tlv_type, tlv_len = struct.unpack('<HH', data[off:off+4])
    off += 4
    payload = data[off:off+tlv_len]
    off += tlv_len

    if tlv_type == 0x8001:  # FW_SEGMENT
        if len(payload) < 4:
            print(f"  [seg {segment_index}] short FW_SEGMENT")
            continue
        load_addr = struct.unpack('<I', payload[:4])[0]
        seg_data = payload[4:]
        outname = os.path.join(outdir, f"seg{segment_index:02d}_0x{load_addr:08x}_raw.bin")
        with open(outname, 'wb') as f:
            f.write(seg_data)
        print(f"  [seg {segment_index}] FW_SEGMENT load_addr=0x{load_addr:08x} len={len(seg_data)} -> {outname}")
        segments.append((segment_index, 'raw', load_addr, len(seg_data), outname))
        segment_index += 1

    elif tlv_type == 0x8002:  # FW_SEGMENT_DEFLATED
        if len(payload) < 8:
            print(f"  [seg {segment_index}] short FW_SEGMENT_DEFLATED")
            continue
        load_addr, chunk_size = struct.unpack('<IH', payload[:6])
        # zlib_header(2) + deflate stream
        # Try decompression starting at offset 6
        try:
            # Use raw deflate + manual zlib wrap
            d = zlib.decompressobj()
            decompressed = d.decompress(payload[6:])
            decompressed += d.flush()
        except zlib.error as e:
            print(f"  [seg {segment_index}] DEFLATED zlib error: {e}")
            # Save raw deflated for analysis
            outname = os.path.join(outdir, f"seg{segment_index:02d}_0x{load_addr:08x}_DEFLATED_raw.bin")
            with open(outname, 'wb') as f:
                f.write(payload[6:])
            segments.append((segment_index, 'def-failed', load_addr, len(payload[6:]), outname))
            segment_index += 1
            continue
        outname = os.path.join(outdir, f"seg{segment_index:02d}_0x{load_addr:08x}.bin")
        with open(outname, 'wb') as f:
            f.write(decompressed)
        print(f"  [seg {segment_index}] DEFLATED load_addr=0x{load_addr:08x} compressed={len(payload[6:])} -> {len(decompressed)} bytes ({outname})")
        segments.append((segment_index, 'inflated', load_addr, len(decompressed), outname))
        segment_index += 1

    elif tlv_type in (0x8f00, 0x8f01):
        print(f"  [tlv] EOF (type=0x{tlv_type:04x})")
        break

    else:
        print(f"  [tlv] unknown type=0x{tlv_type:04x} len={tlv_len} (skipping)")

print(f"\nTotal segments: {len(segments)}")
print(f"\nLayout:")
for s in segments:
    print(f"  seg{s[0]:02d}  load=0x{s[2]:08x}  size={s[3]:>8}  {s[1]}  {s[4]}")
