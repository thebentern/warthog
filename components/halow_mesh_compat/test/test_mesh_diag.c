/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Which cause the peering watchdog reports.
 *
 * This is the branch nobody can exercise without a radio and a second node to
 * mismatch against, so it is tested here instead. What matters is not just
 * that each cause can be produced, but the ORDER: a refused channel makes the
 * applied channel and the beacon count meaningless, and an unpinned radio
 * makes a beacon count mean "audible somewhere", not "audible on our channel".
 * Reporting a later cause when an earlier one holds sends the operator after
 * the wrong thing, which is exactly the failure this watchdog exists to stop.
 */
#include <stdio.h>
#include <string.h>

#include "mesh_diag.h"

static int failures;

static void expect(const char *what, enum warthog_mesh_diag got,
                   enum warthog_mesh_diag want)
{
    if (got != want) {
        printf("FAIL %s: got %d want %d\n", what, (int)got, (int)want);
        failures++;
    } else {
        printf("ok   %s\n", what);
    }
}

static enum warthog_mesh_diag diag(unsigned peers, int pin, unsigned chan, uint32_t bcn)
{
    struct warthog_mesh_diag_in in = {
        .peers = peers, .chan_pin_status = pin,
        .applied_chan = chan, .beacons_heard = bcn,
    };
    return warthog_mesh_diagnose(&in);
}

int main(void)
{
    /* The four causes, each in isolation. */
    expect("peered",              diag(1, 0, 42, 0),   WARTHOG_MESH_DIAG_PEERED);
    expect("channel refused",     diag(0, 3, 42, 100), WARTHOG_MESH_DIAG_CHAN_REJECTED);
    expect("channel unpinned",    diag(0, 0, 0,  100), WARTHOG_MESH_DIAG_CHAN_UNPINNED);
    expect("nothing audible",     diag(0, 0, 42, 0),   WARTHOG_MESH_DIAG_NOTHING_AUDIBLE);
    expect("audible, not joining", diag(0, 0, 42, 1),  WARTHOG_MESH_DIAG_AUDIBLE_NOT_JOINING);

    /* Precedence. Each earlier cause must win over every later one. */
    expect("peered beats a refused channel",  diag(2, 3, 0, 0),
           WARTHOG_MESH_DIAG_PEERED);
    expect("refused beats unpinned",          diag(0, 3, 0, 0),
           WARTHOG_MESH_DIAG_CHAN_REJECTED);
    expect("refused beats a beacon count",    diag(0, 3, 42, 5000),
           WARTHOG_MESH_DIAG_CHAN_REJECTED);
    expect("unpinned beats a beacon count",   diag(0, 0, 0, 5000),
           WARTHOG_MESH_DIAG_CHAN_UNPINNED);
    expect("unpinned beats silence",          diag(0, 0, 0, 0),
           WARTHOG_MESH_DIAG_CHAN_UNPINNED);

    /* A negative status is still a failure -- mmwlan status codes are an enum
     * and nothing promises they stay positive. */
    expect("negative pin status is refused",  diag(0, -1, 42, 9),
           WARTHOG_MESH_DIAG_CHAN_REJECTED);

    /* One peer is enough; a relay is not required to be peered with everyone. */
    expect("a single peer counts",            diag(1, 0, 42, 0),
           WARTHOG_MESH_DIAG_PEERED);

    /* Every cause must name itself. An empty or missing string here would
     * leave the operator with a log line that says nothing. */
    for (int d = WARTHOG_MESH_DIAG_PEERED; d <= WARTHOG_MESH_DIAG_AUDIBLE_NOT_JOINING; d++) {
        const char *t = warthog_mesh_diag_text((enum warthog_mesh_diag)d);
        if (t == NULL || t[0] == '\0' || strcmp(t, "unknown") == 0) {
            printf("FAIL cause %d has no text\n", d);
            failures++;
        }
    }
    printf("ok   every cause has text\n");

    /* Out-of-range and NULL must not crash or assert a false all-clear. */
    if (strcmp(warthog_mesh_diag_text((enum warthog_mesh_diag)99), "unknown") != 0) {
        printf("FAIL out-of-range cause is not reported as unknown\n");
        failures++;
    } else {
        printf("ok   out-of-range cause reads 'unknown'\n");
    }
    expect("NULL input does not report a fault", warthog_mesh_diagnose(NULL),
           WARTHOG_MESH_DIAG_PEERED);

    if (failures) {
        printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_mesh_diag: all passed\n");
    return 0;
}
