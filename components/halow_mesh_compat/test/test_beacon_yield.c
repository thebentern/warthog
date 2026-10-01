/*
 * When the host beacon timer yields to the chip (driver/beacon/beacon.h,
 * morse_beacon_host_tick_yields), compiled straight out of the SDK.
 *
 * The MM6108 raises its beacon IRQ once for a STA-type chip VIF and never re-arms the TBTT,
 * so a host timer drives every mesh beacon. On a MESH-type VIF (WARTHOG_MESH_CHIP_VIF_MESH)
 * the chip may schedule its own TBTT, as a Linux mesh point's does, and a host tick on top
 * would put a second beacon in every interval. The rule: a tick yields when the chip raised
 * an IRQ since the last tick, once it has raised two since the start (the kickoff and one
 * TBTT of its own). A chip that stops gets the host timer back at the next tick.
 */
#include <stdio.h>
#include <string.h>

#include "beacon.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* Feed a run of chip IRQ counts, one per host tick; returns the ticks that yielded as a
 * string of y/. */
static const char *run_(const unsigned *irqs, unsigned n, char *out)
{
    uint32_t seen = 0;
    for (unsigned i = 0; i < n; i++)
    {
        out[i] = morse_beacon_host_tick_yields(irqs[i], &seen) ? 'y' : '.';
    }
    out[n] = '\0';
    return out;
}

int main(void)
{
    char got[32];

    /* STA chip VIF: the one kickoff IRQ, then silence. The host beacons every tick. */
    static const unsigned sta[] = { 0, 1, 1, 1, 1, 1 };
    CHECK(strcmp(run_(sta, 6, got), "......") == 0,
          "a chip that raised only its kickoff IRQ never takes a host tick (%s)", got);

    /* MESH chip VIF scheduling its own TBTT: one IRQ per interval after the kickoff. */
    static const unsigned mesh[] = { 1, 2, 3, 4, 5, 6 };
    CHECK(strcmp(run_(mesh, 6, got), ".yyyyy") == 0,
          "a self-scheduling chip takes every tick from its second IRQ on (%s)", got);

    /* Phase drift: an interval with no IRQ gets a host beacon, one with two yields once. */
    static const unsigned drift[] = { 1, 2, 2, 4, 5, 5, 6 };
    CHECK(strcmp(run_(drift, 7, got), ".y.yy.y") == 0,
          "an interval the chip missed is beaconed by the host (%s)", got);

    /* The chip stops mid-run: the host timer is back at the next tick. */
    static const unsigned stop[] = { 1, 2, 3, 3, 3 };
    CHECK(strcmp(run_(stop, 5, got), ".yy..") == 0,
          "a chip that stops scheduling hands the beacon back (%s)", got);

    /* The first tick after a restart (counts reset to 0) is the host's. */
    uint32_t seen = 7;
    CHECK(!morse_beacon_host_tick_yields(0, &seen) && seen == 0,
          "a restart's zero count yields nothing and resets what the tick saw");

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_beacon_yield: all passed\n");
    return 0;
}
