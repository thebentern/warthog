# Development

An ESP-IDF 5.5 project driven by PlatformIO, with a vendored Morse Micro HaLow
SDK and a host-side test suite for the parts that can be tested without a radio.

## Layout

| Path | What |
|---|---|
| `main/` | Application: USB, AP, NAT, mesh bring-up, AT console |
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

## Writing a test

The convention, and the reason for it:

- **Assert at absolute offsets, not round trips.** A round trip agrees with
  itself no matter how wrong it is. Encoding and decoding with the same wrong
  offset passes.
- **Pin the values a peer computes independently.** Anything carried in a MIC or
  a length octet fails silently on air — no log, no counter, on either side.
- **Test the refusals.** A parser that accepts a malformed frame is a bug even
  when nothing crashes.

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

Warthog's log output is physically unreachable after early boot — the app moves
the shared USB PHY to USB-OTG for the AT console. Counters read back over AT are
the only visibility into the receive path, which is why there are so many of
them. See [AT Command Reference](AT-Command-Reference).

When adding one, make sure it is incremented on the path the shipping build
actually takes. A counter that silently reads zero is worse than no counter — it
reads as a failure that is not happening.
