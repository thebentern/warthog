# Development

An ESP-IDF 5.5 project driven by PlatformIO, with a vendored Morse Micro HaLow
SDK and a host-side test suite for the parts that can be tested without a radio.

## Layout

| Path | What |
|---|---|
| `main/` | Application: USB, AP, NAT, mesh bring-up, AT console |
| `main/bat/` | Clean-room BATMAN_V engine: libc only, all state in one `struct bat`; `main/bat_port.c` runs it on the mesh |
| `components/halow/` | Vendored Morse Micro SDK and the mesh port |
| `components/halow_mesh_compat/` | S1G ↔ 11n compatibility layer and host tests |
| `docs/` | Design notes and hardware findings |
| `tools/bench/` | Multi-board flashing and bench scripts |

## Building every environment

Six device environments plus `native`. Build them all before sending a change —
it is easy to break an env you are not using, because build flags differ between
them:

```bash
for e in warthog-us warthog-eu warthog-jp warthog-kr warthog-au warthog-mesh-smoke; do
  pio run -e $e || echo "FAILED: $e"
done
```

An incremental build does not always rebuild what includes a changed morselib
header (an edit to `umac_datapath_data.h` recompiled only the edited `.c` files),
so a struct change can link objects built against two layouts. After editing any
header under `components/halow/`, build with `pio run -e <env> -t clean` first.

## Host tests

Byte-layout and pure-logic code lives in **freestanding** modules — libc only,
no SDK includes — so the tests link the real shipping source rather than a copy
of it:

```bash
make -C components/halow_mesh_compat/test
```

`make freestanding` is part of that run and fails the build if an SDK include
ever creeps into a module that is supposed to be testable on the host. That
guard is what keeps the on-air byte layouts under test.

Suites cover S1G channel mapping, information-element coding, the peer-link
table, mesh peering frames, mesh data headers, beacon identity, AES-CCM, CCMP
framing, S1G beacon parsing and HWMP path selection.

The batman engine has its own suites (`test_bat_*`, listed in `bat.mk`): codec
and CRC vectors, per-module behaviour, golden captures from batman-adv 2024.3
(`bat_golden/`), hostile input, and a multi-engine simulator (`bat_sim.c`).
`bat-freestanding` fails on any include beyond libc and `bat-nostatic` on any
writable static in an engine object, so several engines can share one process.
`bat-registered` fails on a batman test source with no `TESTS` entry, which
`make` would never build or run.
`test_bat_mode` covers the firmware port's decisions and `test_simnode_batman`
the batman frames through the real 802.11s datapath; the host-CCMP builds of
`test_simnode_keys` add them under SAE. `test_glue_guard.sh` (sections 26 on)
compiles the firmware glue no test links — `main/bat_port.c`, the batman AT
setters, the bat0 addressing in `mesh.c`, rate control's throughput, the NAPT
enable in `main/nat.c` — and runs it against FreeRTOS, ESP-IDF, lwIP and morselib
stubs; it also fails on an `AT+BATSTAT?` port counter the AT reference does not
name. `simnode-seamed` fails on a shipping `.c` a simnode source `#include`s
without listing it in `SIMNODE_SEAMED`, which would leave an edit to it unbuilt.
None of that is a board; batman mode on boards is in [Batman Mode](Batman-Mode#measured-on-air). Interop with a real batman-adv runs on a Linux VM
(`batman_vm/`, not part of `make`; usage in its `run_all.sh`). CI only compiles
its `batvm_node` with `-Werror`, so an engine API change it missed fails there,
and shellchecks its scripts; it runs no scenario.

The batman code must stay clean-room: batman-adv, batctl and alfred are
GPL-2.0-only and this firmware links GPL-3.0 code. Work from the protocol
specification and captures, and test against batman-adv only as a black box.

## Writing a test

The convention, and the reason for it:

- **Assert at absolute offsets, not round trips.** A round trip agrees with
  itself no matter how wrong it is. Encoding and decoding with the same wrong
  offset passes.
- **Pin the values a peer computes independently.** Anything carried in a MIC or
  a length octet fails silently on air — no log, no counter, on either side.
- **Test the refusals.** A parser that accepts a malformed frame is a bug even
  when nothing crashes.
- **Force an interleaving, do not hope for one.** `simnode` runs on one thread. A
  test runs the event loop's work at an exact point inside another task's call
  with a hook (`simnode_set_peer_read_hook`, `_lock_hook`, `_rx_filter_hook`,
  `_tx_alloc_hook`) and must fail with `SAN=1` before the fix, where a read of
  freed memory aborts.

`test_mesh_hwmp.c` is a worked example: it pins every field of a path request
and reply at its byte offset, because two independent readings of the
specification placed two fields two bytes off in a way that still produced a
frame Linux accepts.

## Adding a freestanding module

1. Put it in `components/halow/.../umac/mesh/`, including only `<stdint.h>`,
   `<stdbool.h>` and `<string.h>`.
2. Register it in `components/halow/components/morselib/CMakeLists.txt` —
   otherwise it compiles nowhere and the link fails with undefined references.
3. Add it to `TESTS` and the `freestanding` target in the test `Makefile`.

## The archive-extraction trap

`morselib` links as a static archive, and the linker will not extract an object
from it just to satisfy a reference coming from `main/`. This is why every
`g_warthog_*` counter is **defined in `main/at.c`** and only `extern`'d inside
morselib.

If you add a counter the other way round, it will link on one env and fail on
another. Follow the existing pattern.

Related: declarations should not be hidden behind build flags that only some
environments set. Doing so compiles fine on the env you are testing and breaks
every other one.

## Diagnostics

The USB-Serial-JTAG console goes dark after early boot — the app moves the
shared USB PHY to USB-OTG for the AT console, which mirrors INFO-and-above log
lines but drops any that arrive while the port is full or busy with another
line. Counters read back over AT are the dependable visibility into the
receive path, which is why there are so many of them. See
[AT Command Reference](AT-Command-Reference).

When adding one, make sure it is incremented on the path the shipping build
actually takes. A counter that silently reads zero is worse than no counter — it
reads as a failure that is not happening.
