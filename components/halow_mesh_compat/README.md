# halow_mesh_compat

Linux GPL morse_driver compatibility layer for Warthog — a parallel
mesh-capable code path that vendors selected portions of the upstream
Linux driver and provides a thin shim mapping Linux kernel APIs to
ESP-IDF / FreeRTOS equivalents.

## Status: scaffold

This component is **scaffolded but not implemented**. It contains the
directory layout, license file, build files, and shim header — but no
actual ported code yet.

The port is gated on **Morse Micro providing a mesh-capable MMFW firmware
for the MM6108** (see `../../docs/history/morse-support-inquiry.md` and
`../../docs/history/mesh-driver-port-plan.md`). Without that firmware, no
amount of host-driver code makes the chip's mesh RX path open.

When the firmware prerequisite is met, the port proceeds per
`../../docs/history/mesh-driver-port-plan.md`:

1. Vendor `dot11ah/` from `the Linux morse driver` (or equivalent upstream
   checkout) into `src/dot11ah/`
2. Vendor selected portions of `mesh.c`, `mesh.h`, `command.c`, and
   `mac.c` mesh-relevant chunks into `src/mesh/`
3. Implement Linux kernel API shims in `src/shim/`
4. Wire into existing `umac_mesh.c` via the `halow_mesh_*` public API
   in `include/halow_mesh.h`

## License

This component will contain code derived from `MorseMicro/morse_driver`
which is licensed **GPL-2.0-or-later**. Vendored files retain their
original license. The combined Warthog firmware that links this
component is therefore GPL-2.0+ in the mesh build configuration.

Warthog dual-licenses: the STA-only build remains under the project's
permissive license; the mesh-enabled build (gated by
`-DWARTHOG_MESH_COMPAT=ON`) is GPL-2.0+.

See `LICENSE` in this directory.

## Why exists

After 33 documented iterations of host-side experimentation against the
fullmac MMFW chip firmware (-step1 through -step33), we
established that the chip's mesh-mode RX filter cannot be opened via any
host-side chip command in the documented opcode range. The Linux driver
succeeds at mesh because it uses a different firmware variant
(softmac/thin-LMAC ELF format) that has open RX by default.

To match the Linux driver's success on the embedded port, we'd need either:

- Morse to provide a mesh-capable MMFW (chip-side mesh state machine
  completed), or
- Morse to provide an MMFW-wrapped version of the public Linux
  `mm6108-tlm.bin`, or
- Discovery of an undocumented chip command that opens RX (we probed
  every opcode in 0x0000-0x00FF; none worked)

This component is the host-driver scaffold ready for path 1 or 2.
