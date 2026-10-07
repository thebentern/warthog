/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The hang guard's logic: main/hang_guard_core.c, compiled straight out of main/. A tick is
 * warthog_hang_step for each probe at now_ms, as main/hang_guard.c runs it once a second.
 *  (1) answered every tick: never stalled, age 0, max 0;
 *  (2) loop, tcpip, usb, timer each unanswered alone: stalled at 180 s, not at 179, the reason exact; answered
 *      then, age 0 and max_ms 180 s, which the next unanswered tick keeps;
 *  (3) the health limit: its interval plus 90 s, at least 180 s, clamped; a 150 s interval stalls at 240 s;
 *  (4) a refused post (FULL) ages as unanswered and is counted per tick; one with an answer since (tcpip's
 *      lwIP-timer beat while the mailbox stays full) does not age, and is counted;
 *  (5) an OFF probe never ages or stalls, starts again at 0, and is "off" in the reason;
 *  (6) a late tick: an answer since the last restarts at 0, none counts the whole gap;
 *  (7) two probes over their limits: the one waiting longest is named, on a tie the lower index;
 *  (8) ages across the 2^32 ms wrap;
 *  (9) an idle task's gap: 0 for a stamp at or after now (the other CPU stamped after main read now);
 *  (10) the reason: the test marker, the longest within WARTHOG_HANG_REASON_MAX, a cut prefix;
 *  (11) a core dump's reason on one line: IDF's task watchdog caption becomes TASK_WDT;
 *  (12) AT+HANGTEST's names, the probe each needs, its reply lines;
 *  (13) the .noinit record: its fields, an OFF probe as WARTHOG_HANG_NONE, the magic written last;
 *  (14) AT+HANG?'s lines, exact, and the longest within at.c's line[192].
 */
#include "hang_guard_core.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

_Static_assert(sizeof(struct warthog_hang_rec) == 40u, "the .noinit record is 40 bytes");
_Static_assert(WARTHOG_HANG_LIMIT_S == 180u && WARTHOG_HANG_TWDT_S == 60u && WARTHOG_HANG_HEALTH_SLACK_S == 90u,
               "the limits the AT reference documents");

static struct warthog_hang_state p[WARTHOG_HANG_PROBES];
static uint32_t answers[WARTHOG_HANG_PROBES];

/* One tick at @p t seconds: every probe answered but those in @p silent (a bit each), which post @p r. */
static void tick_(uint32_t t, unsigned silent, enum warthog_hang_post r)
{
    for (int i = 0; i < WARTHOG_HANG_PROBES; i++) {
        if (!(silent & (1u << i))) {
            answers[i]++;
            warthog_hang_step(&p[i], answers[i], WARTHOG_HANG_POSTED, t * 1000u);
        } else {
            warthog_hang_step(&p[i], answers[i], r, t * 1000u);
        }
    }
}

static void reset_(void)
{
    warthog_hang_init(p);
    memset(answers, 0, sizeof(answers));
}

static void t_answered(void)
{
    reset_();
    bool init_ok = true;
    for (int i = 0; i < WARTHOG_HANG_PROBES; i++) {
        init_ok = init_ok && !p[i].on && p[i].limit_ms == 180000u && p[i].age_ms == 0 && p[i].full == 0;
    }
    CHECK(init_ok, "(1) init: every probe off, its limit 180000 ms");
    bool never = true, age0 = true;
    for (uint32_t t = 0; t < 10000u; t++) {
        tick_(t, 0, WARTHOG_HANG_POSTED);
        never = never && warthog_hang_stalled(p) == -1;
        for (int i = 0; i < WARTHOG_HANG_PROBES; i++) {
            age0 = age0 && p[i].on && p[i].age_ms == 0;
        }
    }
    bool max0 = true;
    for (int i = 0; i < WARTHOG_HANG_PROBES; i++) {
        max0 = max0 && p[i].max_ms == 0;
    }
    CHECK(never && age0 && max0, "(1) answered every tick for 10000 ticks: never stalled, age 0, max 0");
}

static void t_unanswered(void)
{
    static const char *const want[4] = {
        "HANG loop: loop=180 tcpip=0 usb=0 timer=0 health=0 up=200",
        "HANG tcpip: loop=0 tcpip=180 usb=0 timer=0 health=0 up=200",
        "HANG usb: loop=0 tcpip=0 usb=180 timer=0 health=0 up=200",
        "HANG timer: loop=0 tcpip=0 usb=0 timer=180 health=0 up=200",
    };
    for (int x = WARTHOG_HANG_LOOP; x <= WARTHOG_HANG_TIMER; x++) {
        reset_();
        bool quiet = true;
        for (uint32_t t = 0; t < 180u; t++) {
            tick_(t, 1u << x, WARTHOG_HANG_POSTED);
            quiet = quiet && warthog_hang_stalled(p) == -1;
        }
        tick_(180u, 1u << x, WARTHOG_HANG_POSTED);
        const int s = warthog_hang_stalled(p);
        char buf[WARTHOG_HANG_REASON_MAX];
        const size_t n = warthog_hang_reason(buf, sizeof(buf), s, WARTHOG_HANG_TEST_NONE, p, 200u);
        CHECK(quiet && s == x && n == strlen(want[x]) && strcmp(buf, want[x]) == 0,
              "(2) %s unanswered alone: not stalled through tick 179, stalled at 180: \"%s\"", warthog_hang_names[x], buf);
        tick_(181u, 0, WARTHOG_HANG_POSTED);
        const bool answered = p[x].age_ms == 0 && p[x].max_ms == 180000u;
        tick_(182u, 1u << x, WARTHOG_HANG_POSTED);
        CHECK(answered && p[x].age_ms == 1000u && p[x].max_ms == 180000u,
              "(2) %s answered at 181: age 0, max_ms 180000, kept at the next wait (max_ms %lu)", warthog_hang_names[x],
              (unsigned long)p[x].max_ms);
    }
}

static void t_health_limit(void)
{
    CHECK(warthog_hang_health_limit_ms(90000u) == 180000u && warthog_hang_health_limit_ms(150000u) == 240000u &&
              warthog_hang_health_limit_ms(0) == 180000u && warthog_hang_health_limit_ms(UINT32_MAX) == UINT32_MAX &&
              warthog_hang_health_limit_ms(UINT32_MAX - 89999u) == UINT32_MAX,
          "(3) health limit: 90 s -> 180 s, 150 s -> 240 s, 0 -> 180 s, clamped at UINT32_MAX");
    reset_();
    const uint32_t last_checked = 61234u;
    bool at180 = true;
    int s = -1;
    for (uint32_t t = 0; t <= 240u; t++) {
        for (int i = WARTHOG_HANG_LOOP; i < WARTHOG_HANG_HEALTH; i++) {
            warthog_hang_step(&p[i], ++answers[i], WARTHOG_HANG_POSTED, t * 1000u);
        }
        p[WARTHOG_HANG_HEALTH].limit_ms = warthog_hang_health_limit_ms(150000u);
        warthog_hang_step(&p[WARTHOG_HANG_HEALTH], last_checked, WARTHOG_HANG_POSTED, t * 1000u);
        s = warthog_hang_stalled(p);
        if (t < 240u) {
            at180 = at180 && s == -1;
        }
    }
    CHECK(at180 && s == WARTHOG_HANG_HEALTH, "(3) a 150 s health interval: not stalled at 180 s, stalled at 240 s");
}

static void t_full(void)
{
    reset_();
    bool quiet = true;
    for (uint32_t t = 0; t < 180u; t++) {
        tick_(t, 1u << WARTHOG_HANG_USB, WARTHOG_HANG_FULL);
        quiet = quiet && warthog_hang_stalled(p) == -1;
    }
    tick_(180u, 1u << WARTHOG_HANG_USB, WARTHOG_HANG_FULL);
    CHECK(quiet && warthog_hang_stalled(p) == WARTHOG_HANG_USB && p[WARTHOG_HANG_USB].age_ms == 180000u &&
              p[WARTHOG_HANG_USB].full == 181u,
          "(4) a post refused every tick ages as unanswered and counts each tick (full %lu)",
          (unsigned long)p[WARTHOG_HANG_USB].full);

    reset_();
    bool young = true;
    for (uint32_t t = 0; t < 10000u; t++) {
        answers[WARTHOG_HANG_TCPIP]++;
        tick_(t, 1u << WARTHOG_HANG_TCPIP, WARTHOG_HANG_FULL);
        young = young && warthog_hang_stalled(p) == -1 && p[WARTHOG_HANG_TCPIP].age_ms == 0;
    }
    CHECK(young && p[WARTHOG_HANG_TCPIP].full == 10000u && p[WARTHOG_HANG_TCPIP].max_ms == 0,
          "(4) refused every tick but answered each (tcpip's beat, the mailbox full): age 0 for 10000 ticks, full %lu",
          (unsigned long)p[WARTHOG_HANG_TCPIP].full);
}

static void t_off(void)
{
    reset_();
    bool never = true, age0 = true;
    for (uint32_t t = 0; t < 10000u; t++) {
        tick_(t, 1u << WARTHOG_HANG_LOOP, WARTHOG_HANG_OFF);
        never = never && warthog_hang_stalled(p) == -1;
        age0 = age0 && !p[WARTHOG_HANG_LOOP].on && p[WARTHOG_HANG_LOOP].age_ms == 0;
    }
    CHECK(never && age0, "(5) 10000 OFF ticks: never stalled, age 0, off");
    tick_(10000u, 1u << WARTHOG_HANG_LOOP, WARTHOG_HANG_POSTED);
    const bool back0 = p[WARTHOG_HANG_LOOP].on && p[WARTHOG_HANG_LOOP].age_ms == 0;
    tick_(10001u, 1u << WARTHOG_HANG_LOOP, WARTHOG_HANG_POSTED);
    CHECK(back0 && p[WARTHOG_HANG_LOOP].age_ms == 1000u, "(5) OFF then POSTED, the same answer count: starts again at 0");

    reset_();
    for (uint32_t t = 0; t <= 180u; t++) {
        tick_(t, 1u << WARTHOG_HANG_LOOP, WARTHOG_HANG_POSTED);
        warthog_hang_step(&p[WARTHOG_HANG_TCPIP], answers[WARTHOG_HANG_TCPIP], WARTHOG_HANG_OFF, t * 1000u);
    }
    char buf[WARTHOG_HANG_REASON_MAX];
    (void)warthog_hang_reason(buf, sizeof(buf), warthog_hang_stalled(p), WARTHOG_HANG_TEST_NONE, p, 181u);
    CHECK(strcmp(buf, "HANG loop: loop=180 tcpip=off usb=0 timer=0 health=0 up=181") == 0,
          "(5) an OFF probe is \"off\" in the reason: \"%s\"", buf);
}

static void t_late(void)
{
    reset_();
    tick_(0, 0, WARTHOG_HANG_POSTED);
    tick_(170u, 0, WARTHOG_HANG_POSTED);
    bool age0 = warthog_hang_stalled(p) == -1;
    for (int i = 0; i < WARTHOG_HANG_PROBES; i++) {
        age0 = age0 && p[i].age_ms == 0;
    }
    CHECK(age0, "(6) no tick for 170 s, then one with an answer since: age 0");
    tick_(370u, 1u << WARTHOG_HANG_TIMER, WARTHOG_HANG_POSTED);
    CHECK(warthog_hang_stalled(p) == WARTHOG_HANG_TIMER && p[WARTHOG_HANG_TIMER].age_ms == 200000u,
          "(6) no tick for 200 s and no answer since: the gap counts, stalled");
}

static void t_two(void)
{
    reset_();
    for (int i = 0; i < WARTHOG_HANG_PROBES; i++) {
        p[i].on = true;
    }
    p[WARTHOG_HANG_LOOP].age_ms = 190000u;
    p[WARTHOG_HANG_USB].age_ms = 200000u;
    p[WARTHOG_HANG_HEALTH].age_ms = 230000u;
    p[WARTHOG_HANG_HEALTH].limit_ms = 240000u;
    CHECK(warthog_hang_stalled(p) == WARTHOG_HANG_USB,
          "(7) loop 190 s and usb 200 s over 180, health 230 s under its 240: usb, the oldest over its limit");
    p[WARTHOG_HANG_TCPIP].age_ms = 200000u;
    CHECK(warthog_hang_stalled(p) == WARTHOG_HANG_TCPIP, "(7) tcpip and usb both 200 s: tcpip, the lower index");
    p[WARTHOG_HANG_TCPIP].on = false;
    CHECK(warthog_hang_stalled(p) == WARTHOG_HANG_USB, "(7) an OFF probe's stale age is never picked");
}

static void t_wrap(void)
{
    struct warthog_hang_state s[WARTHOG_HANG_PROBES];
    warthog_hang_init(s);
    const uint32_t since = UINT32_MAX - 500u;
    warthog_hang_step(&s[0], 9u, WARTHOG_HANG_POSTED, since);
    warthog_hang_step(&s[0], 9u, WARTHOG_HANG_POSTED, since + 179999u);
    const bool before = warthog_hang_stalled(s) == -1 && s[0].age_ms == 179999u;
    warthog_hang_step(&s[0], 9u, WARTHOG_HANG_POSTED, since + 180000u);
    CHECK(before && warthog_hang_stalled(s) == 0 && s[0].age_ms == 180000u,
          "(8) since 2^32 - 501 ms, the clock wrapped: not stalled at 179999 ms, stalled at 180000 ms");
}

static void t_gap(void)
{
    CHECK(warthog_hang_gap_ms(1000u, 1001u) == 0 && warthog_hang_gap_ms(1000u, 1000u) == 0 &&
              warthog_hang_gap_ms(5000u, 1000u) == 4000u && warthog_hang_gap_ms(100u, UINT32_MAX - 99u) == 200u,
          "(9) idle gap: a stamp after now or at it 0, else the difference, across the wrap too");
}

static void t_reason(void)
{
    reset_();
    for (uint32_t t = 0; t <= 180u; t++) {
        tick_(t, 1u << WARTHOG_HANG_LOOP, WARTHOG_HANG_POSTED);
    }
    char buf[WARTHOG_HANG_REASON_MAX + 8];
    (void)warthog_hang_reason(buf, sizeof(buf), WARTHOG_HANG_LOOP, WARTHOG_HANG_TEST_LOOP, p, 7u);
    CHECK(strcmp(buf, "HANG test loop: loop=180 tcpip=0 usb=0 timer=0 health=0 up=7") == 0,
          "(10) with an AT+HANGTEST block set: \"%s\"", buf);

    struct warthog_hang_state w[WARTHOG_HANG_PROBES];
    warthog_hang_init(w);
    for (int i = 0; i < WARTHOG_HANG_PROBES; i++) {
        w[i].on = true;
        w[i].age_ms = UINT32_MAX;
    }
    const size_t n = warthog_hang_reason(buf, sizeof(buf), WARTHOG_HANG_HEALTH, WARTHOG_HANG_TEST_HEALTH, w, UINT32_MAX);
    CHECK(n == 99u && strlen(buf) == 99u && n + 1u <= WARTHOG_HANG_REASON_MAX &&
              strcmp(buf, "HANG test health: loop=4294967 tcpip=4294967 usb=4294967 timer=4294967 health=4294967 "
                          "up=4294967295") == 0,
          "(10) the longest reason is %u characters and its NUL fits WARTHOG_HANG_REASON_MAX (%u)", (unsigned)n,
          (unsigned)WARTHOG_HANG_REASON_MAX);

    char full[WARTHOG_HANG_REASON_MAX];
    (void)warthog_hang_reason(full, sizeof(full), WARTHOG_HANG_HEALTH, WARTHOG_HANG_TEST_HEALTH, w, UINT32_MAX);
    memset(buf, 0x5a, sizeof(buf));
    const size_t c = warthog_hang_reason(buf, 20u, WARTHOG_HANG_HEALTH, WARTHOG_HANG_TEST_HEALTH, w, UINT32_MAX);
    CHECK(c == 19u && buf[19] == '\0' && strncmp(buf, full, 19u) == 0 && (unsigned char)buf[20] == 0x5au,
          "(10) cut at len 20: a NUL-terminated 19-character prefix, nothing past buf[19]");

    memset(buf, 0x5a, sizeof(buf));
    CHECK(warthog_hang_reason(buf, sizeof(buf), -1, WARTHOG_HANG_TEST_NONE, w, 1u) == 0 && buf[0] == '\0' &&
              warthog_hang_reason(buf, sizeof(buf), WARTHOG_HANG_PROBES, WARTHOG_HANG_TEST_NONE, w, 1u) == 0 &&
              buf[0] == '\0',
          "(10) no stalled probe: \"\", 0");
    buf[0] = 'x';
    CHECK(warthog_hang_reason(buf, 0, WARTHOG_HANG_LOOP, WARTHOG_HANG_TEST_NONE, w, 1u) == 0 && buf[0] == 'x',
          "(10) len 0 writes nothing");
}

static void t_coredump(void)
{
    char s[256];
    strcpy(s, "Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:"
              "\n - main (CPU 0)\n - IDLE0 (CPU 0)");
    warthog_hang_coredump_reason(s);
    CHECK(strcmp(s, "TASK_WDT main (CPU 0) IDLE0 (CPU 0)") == 0, "(11) the task watchdog's caption: \"%s\"", s);
    static const char *const same[] = {
        "HANG loop: loop=180 tcpip=0 usb=0 timer=0 health=0 up=200",
        "MMOSAL_ASSERT pc=0x1 fileid=0x2 line=3",
        "abort() was called",
    };
    for (unsigned i = 0; i < sizeof(same) / sizeof(same[0]); i++) {
        strcpy(s, same[i]);
        warthog_hang_coredump_reason(s);
        CHECK(strcmp(s, same[i]) == 0, "(11) unchanged: \"%s\"", s);
    }
    strcpy(s, "a\nb");
    warthog_hang_coredump_reason(s);
    CHECK(strcmp(s, "a b") == 0, "(11) any other newline becomes a space: \"%s\"", s);
    strcpy(s, "Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:");
    warthog_hang_coredump_reason(s);
    CHECK(strcmp(s, "TASK_WDT") == 0, "(11) the caption alone (a reason cut at its buffer): \"%s\"", s);
}

static void t_tests(void)
{
    static const char *const upper[WARTHOG_HANG_TESTS] = { "-", "LOOP", "HEALTH", "DRV", "TCPIP", "USB", "TIMER", "MAIN",
                                                          "SPIN" };
    static const char *const mixed[WARTHOG_HANG_TESTS] = { "-", "Loop", "hEaLtH", "Drv", "tcpIP", "uSb", "TiMeR", "Main",
                                                          "sPiN" };
    bool found = true;
    for (int i = 1; i < WARTHOG_HANG_TESTS; i++) {
        found = found && warthog_hang_test_find(warthog_hang_test_names[i]) == i &&
                warthog_hang_test_find(upper[i]) == i && warthog_hang_test_find(mixed[i]) == i;
    }
    CHECK(found, "(12) each of the eight names, in any case, finds its id");
    CHECK(warthog_hang_test_find("off") == WARTHOG_HANG_TEST_NONE && warthog_hang_test_find("OFF") == WARTHOG_HANG_TEST_NONE &&
              warthog_hang_test_find("x") == -1 && warthog_hang_test_find("-") == -1 && warthog_hang_test_find("") == -1,
          "(12) off is NONE; x, - and \"\" are -1");
    CHECK(warthog_hang_test_probe(WARTHOG_HANG_TEST_LOOP) == WARTHOG_HANG_LOOP &&
              warthog_hang_test_probe(WARTHOG_HANG_TEST_TCPIP) == WARTHOG_HANG_TCPIP &&
              warthog_hang_test_probe(WARTHOG_HANG_TEST_USB) == WARTHOG_HANG_USB &&
              warthog_hang_test_probe(WARTHOG_HANG_TEST_TIMER) == WARTHOG_HANG_TIMER &&
              warthog_hang_test_probe(WARTHOG_HANG_TEST_HEALTH) == WARTHOG_HANG_HEALTH &&
              warthog_hang_test_probe(WARTHOG_HANG_TEST_DRV) == WARTHOG_HANG_HEALTH &&
              warthog_hang_test_probe(WARTHOG_HANG_TEST_MAIN) == -1 && warthog_hang_test_probe(WARTHOG_HANG_TEST_SPIN) == -1 &&
              warthog_hang_test_probe(WARTHOG_HANG_TEST_NONE) == -1 && warthog_hang_test_probe(WARTHOG_HANG_TESTS) == -1,
          "(12) the probe each test needs: its own; health and drv health; main, spin and none none");
    static const char *const reply[WARTHOG_HANG_TESTS] = {
        "",
        "+HANGTEST: loop blocks at its next probe; reset=PANIC in about 180 s\r\n",
        "+HANGTEST: health blocks at its next wake; reset=PANIC in up to 180 s\r\n",
        "+HANGTEST: drv blocks; the chip restart then blocks loop; reset=PANIC in up to 280 s\r\n",
        "+HANGTEST: tcpip blocks at its next probe; reset=PANIC in about 180 s\r\n",
        "+HANGTEST: usb blocks at its next probe; reset=PANIC in about 180 s\r\n",
        "+HANGTEST: timer blocks at its next probe; reset=PANIC in about 180 s\r\n",
        "+HANGTEST: main blocks at its next tick; reset=TASK_WDT in about 60 s\r\n",
        "+HANGTEST: the AT task spins on CPU 0; reset=TASK_WDT in about 60 s\r\n",
    };
    for (uint32_t i = 1; i < WARTHOG_HANG_TESTS; i++) {
        char buf[256];
        const int n = warthog_hang_test_reply(buf, sizeof(buf), i);
        CHECK(strcmp(buf, reply[i]) == 0 && n + 1 <= 128, "(12) %s's reply, exact and within at.c's 128: %.*s",
              warthog_hang_test_names[i], n - 2, buf);
    }
}

static void t_rec(void)
{
    struct warthog_hang_state s[WARTHOG_HANG_PROBES];
    warthog_hang_init(s);
    for (int i = 0; i < WARTHOG_HANG_PROBES; i++) {
        s[i].on = true;
        s[i].age_ms = (uint32_t)i * 1000u + 999u;
    }
    s[WARTHOG_HANG_USB].on = false;
    struct warthog_hang_rec r;
    memset(&r, 0xee, sizeof(r));
    const uint32_t idle[2] = { 12u, WARTHOG_HANG_NONE };
    warthog_hang_rec_fill(&r, s, idle, 321u, WARTHOG_HANG_TEST_DRV);
    CHECK(r.magic == WARTHOG_HANG_MAGIC && r.up_s == 321u && r.age_s[0] == 0 && r.age_s[1] == 1u &&
              r.age_s[2] == WARTHOG_HANG_NONE && r.age_s[3] == 3u && r.age_s[4] == 4u && r.idle_ms[0] == 12u &&
              r.idle_ms[1] == WARTHOG_HANG_NONE && r.test == WARTHOG_HANG_TEST_DRV,
          "(13) the record: whole-second ages, an OFF probe WARTHOG_HANG_NONE, the idle gaps, up_s, test, the magic");
    /* idle_ms read through the record's own magic: it holds the old magic only if the magic is stored after. */
    memset(&r, 0, sizeof(r));
    r.magic = 0x11111111u;
    warthog_hang_rec_fill(&r, s, &r.magic, 5u, WARTHOG_HANG_TEST_NONE);
    CHECK(r.idle_ms[0] == 0x11111111u && r.magic == WARTHOG_HANG_MAGIC,
          "(13) the magic is written after every field (idle0 read %#lx)", (unsigned long)r.idle_ms[0]);
}

/* An AT line without its CRLF, for the log. */
#define LINE_(b) (int)strcspn((b), "\r"), (b)

static void t_lines(void)
{
    struct warthog_hang_view v;
    memset(&v, 0, sizeof(v));
    char buf[256];
    v.twdt = true;
    v.up_s = 1234u;
    v.idle_max_ms[0] = 12u;
    v.idle_max_ms[1] = 34u;
    v.test = WARTHOG_HANG_TEST_LOOP;
    warthog_hang_summary_line(buf, sizeof(buf), &v);
    CHECK(strcmp(buf, "+HANG: limit_s=180 twdt_s=60 up_s=1234 idle0_max_ms=12 idle1_max_ms=34 test=loop\r\n") == 0,
          "(14) summary, the task watchdog on: %.*s", LINE_(buf));
    v.twdt = false;
    v.up_s = 5u;
    v.idle_max_ms[0] = WARTHOG_HANG_NONE;
    v.idle_max_ms[1] = 7u;
    v.test = WARTHOG_HANG_TEST_NONE;
    warthog_hang_summary_line(buf, sizeof(buf), &v);
    CHECK(strcmp(buf, "+HANG: limit_s=180 twdt_s=off up_s=5 idle0_max_ms=off idle1_max_ms=7 test=-\r\n") == 0,
          "(14) summary, the task watchdog off, an idle hook off, no test: %.*s", LINE_(buf));

    struct warthog_hang_state s = { .on = true, .age_ms = 12345u, .max_ms = 180999u, .limit_ms = 180000u, .full = 3u };
    warthog_hang_probe_line(buf, sizeof(buf), WARTHOG_HANG_LOOP, &s);
    CHECK(strcmp(buf, "+HANG: loop age_s=12 max_s=180 limit_s=180 full=3\r\n") == 0, "(14) a probe on: %.*s", LINE_(buf));
    s = (struct warthog_hang_state){ .on = false, .age_ms = 0, .max_ms = 5000u, .limit_ms = 240000u, .full = 0 };
    warthog_hang_probe_line(buf, sizeof(buf), WARTHOG_HANG_HEALTH, &s);
    CHECK(strcmp(buf, "+HANG: health age_s=off max_s=5 limit_s=240 full=0\r\n") == 0, "(14) a probe off: %.*s", LINE_(buf));

    warthog_hang_prev_line(buf, sizeof(buf), "PANIC", &v);
    CHECK(strcmp(buf, "+HANG: prev none\r\n") == 0, "(14) no previous record: %.*s", LINE_(buf));
    v.prev_valid = true;
    v.prev = (struct warthog_hang_rec){ .magic = WARTHOG_HANG_MAGIC, .up_s = 200u,
                                        .age_s = { 180u, 0, 1u, WARTHOG_HANG_NONE, 90u },
                                        .idle_ms = { 12u, WARTHOG_HANG_NONE }, .test = WARTHOG_HANG_TEST_LOOP };
    warthog_hang_prev_line(buf, sizeof(buf), "PANIC", &v);
    CHECK(strcmp(buf, "+HANG: prev reset=PANIC up_s=200 loop=180 tcpip=0 usb=1 timer=off health=90 idle0_ms=12 "
                      "idle1_ms=off test=loop\r\n") == 0,
          "(14) the previous boot's last tick: %.*s", LINE_(buf));
    v.prev.test = 99u;
    warthog_hang_prev_line(buf, sizeof(buf), "PANIC", &v);
    CHECK(strstr(buf, " test=?\r\n") != NULL, "(14) a test id out of range prints as ?");

    v.prev = (struct warthog_hang_rec){ .magic = WARTHOG_HANG_MAGIC, .up_s = UINT32_MAX - 1u,
                                        .age_s = { UINT32_MAX - 1u, UINT32_MAX - 1u, UINT32_MAX - 1u, UINT32_MAX - 1u,
                                                   UINT32_MAX - 1u },
                                        .idle_ms = { UINT32_MAX - 1u, UINT32_MAX - 1u }, .test = WARTHOG_HANG_TEST_HEALTH };
    const int n = warthog_hang_prev_line(buf, sizeof(buf), "DEEPSLEEP", &v);
    CHECK(n + 1 < 192, "(14) the longest line, prev, fits at.c's line[192] (%d)", n + 1);
}

int main(void)
{
    printf("=== hang guard logic (main/hang_guard_core.c) ===\n");
    t_answered();
    t_unanswered();
    t_health_limit();
    t_full();
    t_off();
    t_late();
    t_two();
    t_wrap();
    t_gap();
    t_reason();
    t_coredump();
    t_tests();
    t_rec();
    t_lines();
    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_hang_guard: all passed\n");
    return 0;
}
