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
 *
 * Two causes sit between the channel checks and the silence ones, because each
 * proves our mesh audible: every new peering refused by the RSSI floor (and
 * none passed), and a beaconless peer heard only by probe requests naming our
 * mesh, which with 0 beacons used to read as "wrong channel". One sighting is
 * enough for either: a beaconless OpenMANET node probes about once a minute.
 *
 * Their counts come from a window the peering watchdog rolls at each report:
 * from the report before last (or the last tick with a peer) to now. So a pass
 * seen once stops masking the floor two reports later, a report read just after
 * a roll still spans a full period, and a wrapped counter still subtracts right.
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

static void expect_n(const char *what, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

/* ...plus probe requests naming our mesh, and the RSSI floor's skips and passes. */
static enum warthog_mesh_diag diag_x(unsigned peers, int pin, unsigned chan, uint32_t bcn,
                                     uint32_t probes, uint32_t skips, uint32_t passes)
{
    struct warthog_mesh_diag_in in = {
        .peers = peers, .chan_pin_status = pin, .applied_chan = chan, .beacons_heard = bcn,
        .mesh_probes = probes, .floor_skips = skips, .floor_passes = passes,
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

    /* A beaconless peer (OpenMANET mesh_beacon_less_mode=1) is heard only by
     * its probe requests: 0 beacons must not read as "wrong channel". */
    expect("probes naming our mesh, no beacons: a beaconless peer",
           diag_x(0, 0, 42, 0, 3, 0, 0), WARTHOG_MESH_DIAG_BEACONLESS_PEER);
    expect("beacons heard: probes add nothing",
           diag_x(0, 0, 42, 7, 3, 0, 0), WARTHOG_MESH_DIAG_AUDIBLE_NOT_JOINING);
    expect("a refused channel still beats the probes",
           diag_x(0, 3, 42, 0, 3, 0, 0), WARTHOG_MESH_DIAG_CHAN_REJECTED);
    expect("an unpinned radio still beats the probes",
           diag_x(0, 0, 0, 0, 3, 0, 0), WARTHOG_MESH_DIAG_CHAN_UNPINNED);

    /* Our mesh heard, but every new peering refused by the RSSI floor. */
    expect("one floor skip and no pass is the floor",
           diag_x(0, 0, 42, 50, 0, 1, 0), WARTHOG_MESH_DIAG_BELOW_FLOOR);
    expect("one probe naming our mesh, no beacons: a beaconless peer",
           diag_x(0, 0, 42, 0, 1, 0, 0), WARTHOG_MESH_DIAG_BEACONLESS_PEER);
    expect("only below the floor, with beacons",
           diag_x(0, 0, 42, 50, 0, 4, 0), WARTHOG_MESH_DIAG_BELOW_FLOOR);
    expect("only below the floor, with no beacons (a weak beaconless peer)",
           diag_x(0, 0, 42, 0, 5, 5, 0), WARTHOG_MESH_DIAG_BELOW_FLOOR);
    expect("one neighbour cleared the floor: not the floor",
           diag_x(0, 0, 42, 50, 0, 4, 1), WARTHOG_MESH_DIAG_AUDIBLE_NOT_JOINING);
    expect("passes but no skips: not the floor",
           diag_x(0, 0, 42, 0, 0, 0, 9), WARTHOG_MESH_DIAG_NOTHING_AUDIBLE);
    expect("peered beats the floor",
           diag_x(1, 0, 42, 50, 0, 4, 0), WARTHOG_MESH_DIAG_PEERED);
    expect("a refused channel beats the floor",
           diag_x(0, 3, 42, 50, 0, 4, 0), WARTHOG_MESH_DIAG_CHAN_REJECTED);
    expect("an unpinned radio beats the floor",
           diag_x(0, 0, 0, 50, 0, 4, 0), WARTHOG_MESH_DIAG_CHAN_UNPINNED);
    {
        const char *t = warthog_mesh_diag_text(WARTHOG_MESH_DIAG_BELOW_FLOOR);
        if (strstr(t, "AT+MESHRSSI") == NULL || strstr(t, "-80") == NULL) {
            printf("FAIL the below-floor text names neither the command nor OpenMANET's -80\n");
            failures++;
        } else {
            printf("ok   the below-floor text names AT+MESHRSSI and OpenMANET's -80 dBm\n");
        }
        t = warthog_mesh_diag_text(WARTHOG_MESH_DIAG_BEACONLESS_PEER);
        if (strstr(t, "beaconless") == NULL || strstr(t, "wrong channel") != NULL) {
            printf("FAIL the beaconless text does not say beaconless, or blames the channel\n");
            failures++;
        } else {
            printf("ok   the beaconless text says beaconless and does not blame the channel\n");
        }
    }

    /* The window. Totals as the counters hold them: probes, skips, passes. */
    {
        struct warthog_mesh_diag_window w = { { 0, 0, 0 }, { 0, 0, 0 } };
        struct warthog_mesh_diag_in in = { .peers = 0, .chan_pin_status = 0,
                                           .applied_chan = 42, .beacons_heard = 50 };
        struct warthog_mesh_diag_counts now = { 2, 3, 1 }; /* one scanner passed early */
        warthog_mesh_diag_window_fill(&w, &now, &in);
        expect_n("from boot: every count so far", in.mesh_probes == 2 && in.floor_skips == 3 &&
                 in.floor_passes == 1);
        expect("so the early pass masks the floor at the first report",
               warthog_mesh_diagnose(&in), WARTHOG_MESH_DIAG_AUDIBLE_NOT_JOINING);
        warthog_mesh_diag_window_roll(&w, &now);           /* report 1 */
        now.floor_skips = 5;
        warthog_mesh_diag_window_fill(&w, &now, &in);
        expect_n("just after report 1 the window still starts at boot",
                 in.floor_skips == 5 && in.floor_passes == 1 && in.mesh_probes == 2);
        warthog_mesh_diag_window_roll(&w, &now);           /* report 2 */
        now.floor_skips = 7;
        now.mesh_probes = 3;
        warthog_mesh_diag_window_fill(&w, &now, &in);
        expect_n("after report 2 it starts at report 1: the early pass is gone",
                 in.floor_skips == 4 && in.floor_passes == 0 && in.mesh_probes == 1);
        expect("and the floor is reported", warthog_mesh_diagnose(&in),
               WARTHOG_MESH_DIAG_BELOW_FLOOR);
        warthog_mesh_diag_window_reset(&w, &now);          /* a peer came up */
        now.floor_passes = 2;
        warthog_mesh_diag_window_fill(&w, &now, &in);
        expect_n("a tick with a peer starts the window again",
                 in.floor_skips == 0 && in.floor_passes == 1 && in.mesh_probes == 0);
        struct warthog_mesh_diag_counts hi = { 0xfffffffeu, 0xfffffffdu, 0xffffffffu };
        struct warthog_mesh_diag_counts lo = { 1u, 2u, 0u };
        warthog_mesh_diag_window_reset(&w, &hi);
        warthog_mesh_diag_window_fill(&w, &lo, &in);
        expect_n("a counter that wrapped still subtracts right",
                 in.mesh_probes == 3u && in.floor_skips == 5u && in.floor_passes == 1u);
        warthog_mesh_diag_window_fill(NULL, &lo, &in);
        warthog_mesh_diag_window_fill(&w, NULL, &in);
        warthog_mesh_diag_window_fill(&w, &lo, NULL);
        warthog_mesh_diag_window_roll(NULL, &lo);
        warthog_mesh_diag_window_roll(&w, NULL);
        warthog_mesh_diag_window_reset(NULL, &lo);
        warthog_mesh_diag_window_reset(&w, NULL);
        warthog_mesh_diag_window_fill(&w, &lo, &in);
        expect_n("NULL arguments change nothing",
                 in.mesh_probes == 3u && in.floor_skips == 5u && in.floor_passes == 1u);
    }

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
