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
 *
 * The mesh start's RESULT line states what the chip answered: PASS only on the chip
 * interface the build asks for (MESH on -meshvif, the boot STA one elsewhere) with
 * MESH_CONFIG(START) accepted. A fallback or a refused MESH_CONFIG used to print "mesh VIF
 * added AND MESH_CONFIG(START) accepted" too, and that line is what people grep.
 */
#include <stdio.h>
#include <string.h>

#include "mesh_diag.h"

static int failures;

#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* The RESULT line for @p in, and its verdict. */
static char s_line[512];
static enum warthog_mesh_start_verdict start_(const struct warthog_mesh_start_in *in)
{
    memset(s_line, 0, sizeof(s_line));
    return warthog_mesh_start_result(in, s_line, sizeof(s_line));
}

static void t_start_result(void)
{
    /* -meshvif, the chip took the MESH VIF and its beaconless MESH_CONFIG(START). */
    struct warthog_mesh_start_in in = { .status = 0, .built_mesh = 1, .chip_vif = 5,
                                        .meshcfg_mode = 2 };
    CHECK(start_(&in) == WARTHOG_MESH_START_PASS && strstr(s_line, "RESULT: PASS") == s_line &&
              strstr(s_line, "chip_vif=mesh(5)") && strstr(s_line, "fallback=0(add_st=0)") &&
              strstr(s_line, "mesh_config=accepted,beaconless(st=0)"),
          "start: a MESH VIF with MESH_CONFIG accepted is a PASS that says so (%s)", s_line);

    /* -meshvif, the chip refused the MESH VIF: the STA fallback carries the mesh, beaconing. */
    in.chip_vif = 1;
    in.fallback = 1;
    in.add_status = -1;
    in.meshcfg_mode = 1;
    CHECK(start_(&in) == WARTHOG_MESH_START_WARN && strstr(s_line, "RESULT: WARN") == s_line &&
              strstr(s_line, "chip_vif=sta(1)") && strstr(s_line, "fallback=1(add_st=-1)") &&
              !strstr(s_line, "PASS") && !strstr(s_line, "mesh VIF added"),
          "start: a STA fallback on a MESH-VIF build is a WARN naming the STA VIF (%s)", s_line);

    /* Every other env: the boot STA VIF is the one asked for. */
    in = (struct warthog_mesh_start_in){ .status = 0, .built_mesh = 0, .chip_vif = 1,
                                         .meshcfg_mode = 1 };
    CHECK(start_(&in) == WARTHOG_MESH_START_PASS && strstr(s_line, "chip_vif=sta(1)"),
          "start: the boot STA VIF on a STA build is a PASS (%s)", s_line);
    in.chip_vif = 5;
    CHECK(start_(&in) == WARTHOG_MESH_START_WARN, "start: a MESH VIF a STA build did not ask for is a WARN");

    /* MESH_CONFIG(START) refused, on either build: up, but not accepted. */
    in = (struct warthog_mesh_start_in){ .status = 0, .built_mesh = 1, .chip_vif = 5,
                                         .meshcfg_refused = 1, .meshcfg_status = -22,
                                         .meshcfg_mode = 2 };
    CHECK(start_(&in) == WARTHOG_MESH_START_WARN &&
              strstr(s_line, "mesh_config=REFUSED,beaconless(st=-22)") && !strstr(s_line, "accepted"),
          "start: a refused MESH_CONFIG is a WARN with its status (%s)", s_line);
    in.built_mesh = 0;
    in.chip_vif = 1;
    CHECK(start_(&in) == WARTHOG_MESH_START_WARN, "start: on a STA build too");

    /* The start failed: FAIL, with what the chip answered on the way. */
    in = (struct warthog_mesh_start_in){ .status = 3, .built_mesh = 1, .chip_vif = 0,
                                         .fallback = 1, .add_status = -110 };
    CHECK(start_(&in) == WARTHOG_MESH_START_FAIL && strstr(s_line, "RESULT: FAIL") == s_line &&
              strstr(s_line, "status=3") && strstr(s_line, "chip_vif=none(0)") &&
              strstr(s_line, "fallback=1(add_st=-110)") && strstr(s_line, "mesh_config=none"),
          "start: a failed start is a FAIL with the chip's answers (%s)", s_line);

    /* No chip VIF claimed although the enable succeeded is never a PASS. */
    in = (struct warthog_mesh_start_in){ .status = 0, .built_mesh = 0, .chip_vif = 0 };
    CHECK(start_(&in) == WARTHOG_MESH_START_WARN, "start: no chip VIF is never a PASS");

    /* The longest line fits the buffer main/mesh.c gives it, whole. */
    in = (struct warthog_mesh_start_in){ .status = -2147483647 - 1, .built_mesh = 1,
                                         .chip_vif = 4294967295u, .fallback = 4294967295u,
                                         .add_status = -2147483647 - 1,
                                         .meshcfg_refused = 4294967295u,
                                         .meshcfg_status = -2147483647 - 1,
                                         .meshcfg_mode = 4294967295u };
    (void)start_(&in);
    const size_t fail_len = strlen(s_line);
    in.status = 0;
    (void)start_(&in);
    const size_t up_len = strlen(s_line);
    CHECK(fail_len < WARTHOG_MESH_START_RESULT_LEN && up_len < WARTHOG_MESH_START_RESULT_LEN,
          "start: the longest lines fit %u bytes (%zu, %zu)", (unsigned)WARTHOG_MESH_START_RESULT_LEN,
          fail_len, up_len);
}

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

    t_start_result();

    if (failures) {
        printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_mesh_diag: all passed\n");
    return 0;
}
