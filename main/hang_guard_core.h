/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WARTHOG_HANG_GUARD_CORE_H
#define WARTHOG_HANG_GUARD_CORE_H

/* The hang guard's logic (main/hang_guard.c): probe ages, the stall pick, the abort reason and the
 * AT+HANG? lines. Freestanding so the host tests build it: libc only. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WARTHOG_HANG_TICK_MS 1000u
#define WARTHOG_HANG_LIMIT_S 180u          /* a probe unanswered this long aborts */
#define WARTHOG_HANG_HEALTH_SLACK_S 90u    /* health: its interval plus this, at least the limit */
#define WARTHOG_HANG_TWDT_S 60u            /* CONFIG_ESP_TASK_WDT_TIMEOUT_S */
#define WARTHOG_HANG_REASON_MAX 128u
#define WARTHOG_HANG_MAGIC 0x48414e47u     /* "HANG": the .noinit record holds a tick */
#define WARTHOG_HANG_NONE UINT32_MAX       /* off, or not measured */

enum warthog_hang_probe { WARTHOG_HANG_LOOP, WARTHOG_HANG_TCPIP, WARTHOG_HANG_USB,
                          WARTHOG_HANG_TIMER, WARTHOG_HANG_HEALTH, WARTHOG_HANG_PROBES };
enum warthog_hang_post { WARTHOG_HANG_OFF, WARTHOG_HANG_POSTED, WARTHOG_HANG_FULL };
enum warthog_hang_test { WARTHOG_HANG_TEST_NONE, WARTHOG_HANG_TEST_LOOP, WARTHOG_HANG_TEST_HEALTH,
                         WARTHOG_HANG_TEST_DRV, WARTHOG_HANG_TEST_TCPIP, WARTHOG_HANG_TEST_USB,
                         WARTHOG_HANG_TEST_TIMER, WARTHOG_HANG_TEST_MAIN, WARTHOG_HANG_TEST_SPIN,
                         WARTHOG_HANG_TESTS };

struct warthog_hang_state {
    bool on;
    uint32_t seen;      /* answer count (health: the task's wakes) at the last tick */
    uint32_t since_ms;  /* start of the wait now running */
    uint32_t age_ms;    /* at the last tick; 0 when off */
    uint32_t max_ms;    /* longest since the guard started */
    uint32_t full;      /* ticks whose post was refused */
    uint32_t limit_ms;
};
struct warthog_hang_rec {                 /* .noinit: the last tick, read at the next boot */
    uint32_t magic;
    uint32_t up_s;
    uint32_t age_s[WARTHOG_HANG_PROBES];  /* WARTHOG_HANG_NONE: off */
    uint32_t idle_ms[2];                  /* each CPU's idle gap then; WARTHOG_HANG_NONE: not measured */
    uint32_t test;                        /* enum warthog_hang_test */
};                                        /* 40 bytes */
struct warthog_hang_view {
    bool twdt;
    uint32_t up_s, test;
    uint32_t idle_max_ms[2];              /* WARTHOG_HANG_NONE: not measured */
    struct warthog_hang_state probe[WARTHOG_HANG_PROBES];
    bool prev_valid;
    struct warthog_hang_rec prev;
};

extern const char *const warthog_hang_names[WARTHOG_HANG_PROBES];
extern const char *const warthog_hang_test_names[WARTHOG_HANG_TESTS];

/* Zeroes the states; each limit WARTHOG_HANG_LIMIT_S. */
void warthog_hang_init(struct warthog_hang_state p[WARTHOG_HANG_PROBES]);
/* One tick of one probe: @p answers its count before this tick's post, @p r that post. */
void warthog_hang_step(struct warthog_hang_state *p, uint32_t answers, enum warthog_hang_post r, uint32_t now_ms);
/* The health probe's limit: its interval plus the slack, at least WARTHOG_HANG_LIMIT_S. */
uint32_t warthog_hang_health_limit_ms(uint32_t interval_ms);
/* An idle task's gap at @p now_ms since its stamp; 0 for a stamp at or after now. */
uint32_t warthog_hang_gap_ms(uint32_t now_ms, uint32_t stamp_ms);
/* The probe on and at or over its limit that has waited longest (ties: the lower index); -1 if none. */
int warthog_hang_stalled(const struct warthog_hang_state p[WARTHOG_HANG_PROBES]);
/* The abort reason, formatted without libc; always NUL-terminated within @p len. @returns chars written. */
size_t warthog_hang_reason(char *buf, size_t len, int stalled, uint32_t test,
                           const struct warthog_hang_state p[WARTHOG_HANG_PROBES], uint32_t up_s);
/* The .noinit record of this tick; its magic written last. */
void warthog_hang_rec_fill(struct warthog_hang_rec *r, const struct warthog_hang_state p[WARTHOG_HANG_PROBES],
                           const uint32_t idle_ms[2], uint32_t up_s, uint32_t test);
/* A core dump's reason on one line, in place: IDF's task watchdog caption becomes "TASK_WDT". */
void warthog_hang_coredump_reason(char *s);
/* AT+HANGTEST's argument, any case: a test id, WARTHOG_HANG_TEST_NONE for "off", -1 for anything else. */
int warthog_hang_test_find(const char *name);
/* The probe @p test needs watched, or -1. */
int warthog_hang_test_probe(uint32_t test);
/* AT+HANGTEST's reply line for @p test. @returns as snprintf. */
int warthog_hang_test_reply(char *buf, size_t len, uint32_t test);
/* AT+HANG? lines: the summary, one probe, the previous boot's last tick. @returns as snprintf. */
int warthog_hang_summary_line(char *buf, size_t len, const struct warthog_hang_view *v);
int warthog_hang_probe_line(char *buf, size_t len, int i, const struct warthog_hang_state *p);
int warthog_hang_prev_line(char *buf, size_t len, const char *reset, const struct warthog_hang_view *v);

#endif
