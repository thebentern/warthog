/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hang_guard_core.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

const char *const warthog_hang_names[WARTHOG_HANG_PROBES] = { "loop", "tcpip", "usb", "timer", "health" };
const char *const warthog_hang_test_names[WARTHOG_HANG_TESTS] = { "-", "loop", "health", "drv", "tcpip",
                                                                  "usb", "timer", "main", "spin" };

/* IDF's esp_task_wdt_print_triggered_tasks() caption (task_wdt.c), which a TWDT core dump's reason starts with. */
static const char k_twdt_caption[] = "Task watchdog got triggered. "
                                     "The following tasks/users did not reset the watchdog in time:";

void warthog_hang_init(struct warthog_hang_state p[WARTHOG_HANG_PROBES])
{
    memset(p, 0, sizeof(*p) * WARTHOG_HANG_PROBES);
    for (int i = 0; i < WARTHOG_HANG_PROBES; i++) {
        p[i].limit_ms = WARTHOG_HANG_LIMIT_S * 1000u;
    }
}

void warthog_hang_step(struct warthog_hang_state *p, uint32_t answers, enum warthog_hang_post r, uint32_t now_ms)
{
    if (r == WARTHOG_HANG_OFF) {
        p->on = false;
        p->seen = answers;
        p->age_ms = 0;
        return;
    }
    if (!p->on || answers != p->seen) {
        p->on = true;
        p->since_ms = now_ms;
    }
    p->seen = answers;
    if (r == WARTHOG_HANG_FULL) {
        p->full++;
    }
    p->age_ms = now_ms - p->since_ms; /* uint32 wrap */
    if (p->age_ms > p->max_ms) {
        p->max_ms = p->age_ms;
    }
}

uint32_t warthog_hang_health_limit_ms(uint32_t interval_ms)
{
    const uint64_t sum = (uint64_t)interval_ms + (uint64_t)WARTHOG_HANG_HEALTH_SLACK_S * 1000u;
    const uint32_t l = sum > UINT32_MAX ? UINT32_MAX : (uint32_t)sum;
    return l > WARTHOG_HANG_LIMIT_S * 1000u ? l : WARTHOG_HANG_LIMIT_S * 1000u;
}

uint32_t warthog_hang_gap_ms(uint32_t now_ms, uint32_t stamp_ms)
{
    return (int32_t)(now_ms - stamp_ms) <= 0 ? 0u : now_ms - stamp_ms;
}

int warthog_hang_stalled(const struct warthog_hang_state p[WARTHOG_HANG_PROBES])
{
    int best = -1;
    for (int i = 0; i < WARTHOG_HANG_PROBES; i++) {
        if (p[i].on && p[i].age_ms >= p[i].limit_ms && (best < 0 || p[i].age_ms > p[best].age_ms)) {
            best = i;
        }
    }
    return best;
}

static char *hang_put_(char *p, const char *end, const char *s)
{
    while (*s != '\0' && p < end) {
        *p++ = *s++;
    }
    return p;
}

static char *hang_put_u_(char *p, const char *end, uint32_t v)
{
    char d[10];
    int n = 0;
    do {
        d[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0u);
    while (n > 0 && p < end) {
        *p++ = d[--n];
    }
    return p;
}

/* Formatted by hand: the abort path calls nothing it need not. */
size_t warthog_hang_reason(char *buf, size_t len, int stalled, uint32_t test,
                           const struct warthog_hang_state p[WARTHOG_HANG_PROBES], uint32_t up_s)
{
    if (len == 0) {
        return 0;
    }
    char *w = buf;
    const char *end = buf + len - 1;
    if (stalled >= 0 && stalled < WARTHOG_HANG_PROBES) {
        w = hang_put_(w, end, test != WARTHOG_HANG_TEST_NONE ? "HANG test " : "HANG ");
        w = hang_put_(hang_put_(w, end, warthog_hang_names[stalled]), end, ":");
        for (int i = 0; i < WARTHOG_HANG_PROBES; i++) {
            w = hang_put_(hang_put_(hang_put_(w, end, " "), end, warthog_hang_names[i]), end, "=");
            w = p[i].on ? hang_put_u_(w, end, p[i].age_ms / 1000u) : hang_put_(w, end, "off");
        }
        w = hang_put_u_(hang_put_(w, end, " up="), end, up_s);
    }
    *w = '\0';
    return (size_t)(w - buf);
}

void warthog_hang_rec_fill(struct warthog_hang_rec *r, const struct warthog_hang_state p[WARTHOG_HANG_PROBES],
                           const uint32_t idle_ms[2], uint32_t up_s, uint32_t test)
{
    volatile struct warthog_hang_rec *v = r; /* the magic stays the last store */
    for (int i = 0; i < WARTHOG_HANG_PROBES; i++) {
        v->age_s[i] = p[i].on ? p[i].age_ms / 1000u : WARTHOG_HANG_NONE;
    }
    v->idle_ms[0] = idle_ms[0];
    v->idle_ms[1] = idle_ms[1];
    v->up_s = up_s;
    v->test = test;
    v->magic = WARTHOG_HANG_MAGIC;
}

void warthog_hang_coredump_reason(char *s)
{
    const size_t n = sizeof(k_twdt_caption) - 1u;
    char *w = s;
    const char *r = s;
    if (strncmp(s, k_twdt_caption, n) == 0) {
        w = hang_put_(w, s + n, "TASK_WDT");
        r = s + n;
    }
    while (*r != '\0') {
        if (r[0] == '\n' && r[1] == ' ' && r[2] == '-' && r[3] == ' ') {
            r += 4;
        } else if (r[0] == '\n') {
            r++;
        } else {
            *w++ = *r++;
            continue;
        }
        *w++ = ' ';
    }
    *w = '\0';
}

int warthog_hang_test_find(const char *name)
{
    if (strcasecmp(name, "off") == 0) {
        return WARTHOG_HANG_TEST_NONE;
    }
    for (int i = 1; i < WARTHOG_HANG_TESTS; i++) {
        if (strcasecmp(name, warthog_hang_test_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

int warthog_hang_test_probe(uint32_t test)
{
    switch (test) {
    case WARTHOG_HANG_TEST_LOOP: return WARTHOG_HANG_LOOP;
    case WARTHOG_HANG_TEST_TCPIP: return WARTHOG_HANG_TCPIP;
    case WARTHOG_HANG_TEST_USB: return WARTHOG_HANG_USB;
    case WARTHOG_HANG_TEST_TIMER: return WARTHOG_HANG_TIMER;
    case WARTHOG_HANG_TEST_HEALTH:
    case WARTHOG_HANG_TEST_DRV: return WARTHOG_HANG_HEALTH;
    default: return -1;
    }
}

int warthog_hang_test_reply(char *buf, size_t len, uint32_t test)
{
    switch (test) {
    case WARTHOG_HANG_TEST_LOOP:
    case WARTHOG_HANG_TEST_TCPIP:
    case WARTHOG_HANG_TEST_USB:
    case WARTHOG_HANG_TEST_TIMER:
        return snprintf(buf, len, "+HANGTEST: %s blocks at its next probe; reset=PANIC in about %u s\r\n",
                        warthog_hang_test_names[test], (unsigned)WARTHOG_HANG_LIMIT_S);
    case WARTHOG_HANG_TEST_HEALTH:
        return snprintf(buf, len, "+HANGTEST: health blocks at its next wake; reset=PANIC in up to %u s\r\n",
                        (unsigned)WARTHOG_HANG_LIMIT_S);
    case WARTHOG_HANG_TEST_DRV:
        return snprintf(buf, len, "+HANGTEST: drv blocks; the chip restart then blocks loop; reset=PANIC in up to %u s\r\n",
                        (unsigned)WARTHOG_HANG_LIMIT_S + 100u);
    case WARTHOG_HANG_TEST_MAIN:
        return snprintf(buf, len, "+HANGTEST: main blocks at its next tick; reset=TASK_WDT in about %u s\r\n",
                        (unsigned)WARTHOG_HANG_TWDT_S);
    case WARTHOG_HANG_TEST_SPIN:
        return snprintf(buf, len, "+HANGTEST: the AT task spins on CPU 0; reset=TASK_WDT in about %u s\r\n",
                        (unsigned)WARTHOG_HANG_TWDT_S);
    default:
        return snprintf(buf, len, "%s", "");
    }
}

static const char *test_name_(uint32_t test)
{
    return test < WARTHOG_HANG_TESTS ? warthog_hang_test_names[test] : "?";
}

/* A value or "off" (WARTHOG_HANG_NONE), into @p s of 11. */
static const char *num_or_off_(char s[11], uint32_t v)
{
    if (v == WARTHOG_HANG_NONE) {
        return "off";
    }
    snprintf(s, 11, "%lu", (unsigned long)v);
    return s;
}

int warthog_hang_summary_line(char *buf, size_t len, const struct warthog_hang_view *v)
{
    char tw[11], i0[11], i1[11];
    return snprintf(buf, len, "+HANG: limit_s=%u twdt_s=%s up_s=%lu idle0_max_ms=%s idle1_max_ms=%s test=%s\r\n",
                    (unsigned)WARTHOG_HANG_LIMIT_S, num_or_off_(tw, v->twdt ? WARTHOG_HANG_TWDT_S : WARTHOG_HANG_NONE),
                    (unsigned long)v->up_s,
                    num_or_off_(i0, v->idle_max_ms[0]), num_or_off_(i1, v->idle_max_ms[1]), test_name_(v->test));
}

int warthog_hang_probe_line(char *buf, size_t len, int i, const struct warthog_hang_state *p)
{
    if (i < 0 || i >= WARTHOG_HANG_PROBES) {
        return snprintf(buf, len, "%s", "");
    }
    char a[11];
    return snprintf(buf, len, "+HANG: %s age_s=%s max_s=%lu limit_s=%lu full=%lu\r\n", warthog_hang_names[i],
                    num_or_off_(a, p->on ? p->age_ms / 1000u : WARTHOG_HANG_NONE), (unsigned long)(p->max_ms / 1000u),
                    (unsigned long)(p->limit_ms / 1000u), (unsigned long)p->full);
}

int warthog_hang_prev_line(char *buf, size_t len, const char *reset, const struct warthog_hang_view *v)
{
    if (!v->prev_valid) {
        return snprintf(buf, len, "+HANG: prev none\r\n");
    }
    const struct warthog_hang_rec *r = &v->prev;
    char s[WARTHOG_HANG_PROBES][11], i0[11], i1[11];
    return snprintf(buf, len,
                    "+HANG: prev reset=%s up_s=%lu loop=%s tcpip=%s usb=%s timer=%s health=%s idle0_ms=%s "
                    "idle1_ms=%s test=%s\r\n",
                    reset, (unsigned long)r->up_s, num_or_off_(s[0], r->age_s[0]), num_or_off_(s[1], r->age_s[1]),
                    num_or_off_(s[2], r->age_s[2]), num_or_off_(s[3], r->age_s[3]), num_or_off_(s[4], r->age_s[4]),
                    num_or_off_(i0, r->idle_ms[0]), num_or_off_(i1, r->idle_ms[1]), test_name_(r->test));
}
