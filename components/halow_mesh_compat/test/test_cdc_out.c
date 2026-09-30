/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * AT output into the CDC TX FIFO: main/cdc_out.c, compiled straight out of main/.
 * The fake FIFO keeps MIN(n, room) of 512 B per call, as esp_tinyusb's
 * tinyusb_cdcacm_write_queue does; the fake host reads only while the writer waits.
 * Another task (the log mirror, +MCAST, +MPING) is modelled by a no-wait write
 * made from inside the waiting writer's sleep, which is when it runs on the device.
 */
#include "cdc_out.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

#define FIFO 512u

/* A host that stops reading may hold the AT task this long, and no longer. */
_Static_assert(CDC_OUT_IDLE_MS == 500u && CDC_OUT_POLL_MS == 10u, "cdc_out waits 10 ms at a time, 500 ms at most");

static uint8_t fifo[FIFO];
static size_t fifo_n;            /* bytes waiting in the device FIFO */
static uint8_t rx[8192];
static size_t rx_n;              /* bytes the host has read, in order */
static size_t drain_per_wait;    /* host reads this much per drain period of waiting */
static uint32_t drain_period_ms; /* ...every this many ms the writer sleeps */
static uint32_t since_drain_ms, waited_ms, waits, queues;
static bool is_connected;

/* The port lock. other_holds: another task is mid-line, so every attempt fails
 * (for a waiting writer, the port's bounded wait ran out). */
static bool lock_held, other_holds;
static unsigned lock_tries, lock_waits, lock_taken, unlocks, bad_unlocks, leaks;

/* Runs once, inside the next wait: another task writing while the writer sleeps. */
static void (*during_wait)(void);

static void host_read(size_t k)
{
    if (k > fifo_n) { k = fifo_n; }
    memcpy(rx + rx_n, fifo, k);
    rx_n += k;
    memmove(fifo, fifo + k, fifo_n - k);
    fifo_n -= k;
}

static size_t fake_queue(const uint8_t *p, size_t n)
{
    size_t room = FIFO - fifo_n, k = n < room ? n : room;
    memcpy(fifo + fifo_n, p, k);
    fifo_n += k;
    queues++;
    return k;
}

static size_t fake_room(void) { return FIFO - fifo_n; }

static bool fake_connected(void) { return is_connected; }

static void fake_wait(uint32_t ms)
{
    waits++;
    waited_ms += ms;
    since_drain_ms += ms;
    if (drain_period_ms != 0 && since_drain_ms >= drain_period_ms) {
        since_drain_ms = 0;
        host_read(drain_per_wait);
    }
    if (during_wait) {
        void (*f)(void) = during_wait;
        during_wait = NULL;
        f();
    }
}

static bool fake_lock(bool wait)
{
    if (wait) { lock_waits++; } else { lock_tries++; }
    if (lock_held || other_holds) { return false; }
    lock_held = true;
    lock_taken++;
    return true;
}

static void fake_unlock(void)
{
    if (!lock_held) { bad_unlocks++; }
    lock_held = false;
    unlocks++;
}

static struct cdc_out o = { .queue = fake_queue, .room = fake_room, .connected = fake_connected,
                            .wait_ms = fake_wait, .lock = fake_lock, .unlock = fake_unlock };

static void reset(size_t per_wait, uint32_t period_ms)
{
    if (lock_held) { leaks++; }
    fifo_n = rx_n = 0;
    drain_per_wait = per_wait;
    drain_period_ms = period_ms;
    since_drain_ms = waited_ms = waits = queues = 0;
    is_connected = true;
    o.stalled = false;
    lock_held = other_holds = false;
    lock_tries = lock_waits = 0;
    during_wait = NULL;
}

static void host_read_all(void) { host_read(fifo_n); }

/* The other task's no-wait write, and what it cost that task. */
static const char *other_line;
static size_t other_ret;
static unsigned other_calls, other_waits, other_lock_waits;

static void other_task_writes(void)
{
    uint32_t w0 = waits;
    unsigned lw0 = lock_waits;
    other_ret = cdc_out_write_nowait(&o, other_line);
    other_calls++;
    other_waits += waits - w0;
    other_lock_waits += lock_waits - lw0;
}

static size_t log_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t n = cdc_out_vprintf_nowait(&o, fmt, ap);
    va_end(ap);
    return n;
}

int main(void)
{
    static char reply[1025];
    for (int i = 0; i < 1024; i++) { reply[i] = (char)('a' + i % 26); }
    reply[1024] = '\0';
    static char expect[4096];

    /* A 1 KB reply and its OK, to a host reading 64 B per 10 ms. */
    reset(64, CDC_OUT_POLL_MS);
    size_t a = cdc_out_write(&o, reply), b = cdc_out_write(&o, "OK\r\n");
    host_read_all();
    snprintf(expect, sizeof(expect), "%sOK\r\n", reply);
    CHECK(a == 1024 && b == 4, "a 1 KB reply and its OK are queued in full (%zu + %zu)", a, b);
    CHECK(rx_n == 1028 && memcmp(rx, expect, 1028) == 0,
          "the host reads the 1 KB reply and then OK, whole and in order (%zu bytes)", rx_n);
    CHECK(waits > 0 && !o.stalled, "the writer waited for room (%u waits) and is not stalled", (unsigned)waits);
    CHECK(lock_waits == 2 && lock_tries == 0 && !lock_held,
          "each reply takes the port lock as a writer that may wait, and gives it back (%u waiting, %u tries)",
          lock_waits, lock_tries);

    /* The same with 500 B of log already in the FIFO and a 480 B reply (AT+MESHFWDSTAT? size). */
    reset(64, CDC_OUT_POLL_MS);
    static char logl[501], fwdstat[481];
    memset(logl, 'L', 500); logl[500] = '\0';
    memset(fwdstat, 'S', 478); memcpy(fwdstat + 478, "\r\n", 3);
    (void)fake_queue((const uint8_t *)logl, 500);
    a = cdc_out_write(&o, fwdstat);
    b = cdc_out_write(&o, "OK\r\n");
    host_read_all();
    snprintf(expect, sizeof(expect), "%s%sOK\r\n", logl, fwdstat);
    CHECK(a == 480 && b == 4 && rx_n == 984 && memcmp(rx, expect, 984) == 0,
          "behind 500 B of log, a 480 B reply and its OK arrive whole (%zu bytes)", rx_n);

    /* A log line printed while an 800 B +MPMPEERS line waits for room: the USB side
     * has drained the FIFO, so without the lock the line would land inside the reply. */
    reset(512, CDC_OUT_POLL_MS);
    static char peers[1024];
    int pw = snprintf(peers, sizeof(peers), "+MPMPEERS: self=0a0b0c ");
    for (int i = 0; i < 60; i++) { pw += snprintf(peers + pw, sizeof(peers) - pw, "k=4294967295 "); }
    snprintf(peers + pw, sizeof(peers) - pw, "\r\n");
    other_line = "I (123456) wifi:station: aa:bb:cc:dd:ee:ff join, AID=1\n";
    other_calls = other_waits = other_lock_waits = 0;
    during_wait = other_task_writes;
    a = cdc_out_write(&o, peers);
    b = cdc_out_write(&o, "OK\r\n");
    host_read_all();
    snprintf(expect, sizeof(expect), "%sOK\r\n", peers);
    CHECK(other_calls == 1 && a == strlen(peers) && b == 4,
          "a log line was printed while the %zu B reply waited, and the reply still went in whole", a);
    CHECK(rx_n == strlen(expect) && memcmp(rx, expect, rx_n) == 0,
          "the host reads the reply and its OK with nothing spliced in (%zu bytes, want %zu)",
          rx_n, strlen(expect));
    CHECK(other_ret == 0 && other_waits == 0 && other_lock_waits == 0,
          "the log line is dropped whole, at once: %zu bytes queued, %u waits, %u waiting lock calls",
          other_ret, other_waits, other_lock_waits);

    /* Between two replies the lock is free: another task's line goes in whole, in order. */
    reset(64, CDC_OUT_POLL_MS);
    a = cdc_out_write(&o, "+VERSION: 1\r\n");
    other_calls = 0;
    other_line = "+MCAST: rx 20 bytes from 10.41.0.2 (#1)\r\n";
    other_task_writes();
    b = cdc_out_write(&o, "OK\r\n");
    host_read_all();
    snprintf(expect, sizeof(expect), "+VERSION: 1\r\n%sOK\r\n", other_line);
    CHECK(other_ret == strlen(other_line) && rx_n == strlen(expect) && memcmp(rx, expect, rx_n) == 0,
          "a line from another task between two replies arrives whole, between them");

    /* No-wait lines go in whole or not at all: a cut line would glue the next reply onto it. */
    reset(64, CDC_OUT_POLL_MS);
    (void)fake_queue((const uint8_t *)logl, 500);
    a = cdc_out_write_nowait(&o, "+MCAST: rx 20 bytes\r\n");
    CHECK(a == 0 && fifo_n == 500, "with 12 B of room a 21 B no-wait line is dropped whole (%zu queued, FIFO %zu)",
          a, fifo_n);
    a = cdc_out_write_nowait(&o, "+MPING: 12\r\n");
    CHECK(a == 12 && fifo_n == FIFO, "a 12 B line fits the last 12 B exactly (%zu queued)", a);
    reset(64, CDC_OUT_POLL_MS);
    a = cdc_out_write_nowait(&o, reply);
    b = cdc_out_write_nowait(&o, "OK\r\n");
    host_read_all();
    CHECK(a == 0 && b == 4 && rx_n == 4 && memcmp(rx, "OK\r\n", 4) == 0,
          "a no-wait line longer than the FIFO is dropped whole and the next line still goes in (%zu, %zu)", a, b);
    CHECK(waits == 0 && lock_waits == 0 && lock_tries == 2,
          "the no-wait writer never waits: %u waits, %u waiting lock calls, %u tries",
          (unsigned)waits, lock_waits, lock_tries);

    /* Another task holds the lock: neither writer writes, neither releases its lock. */
    reset(64, CDC_OUT_POLL_MS);
    other_holds = true;
    unsigned u0 = unlocks;
    a = cdc_out_write_nowait(&o, "+MPING: seq=1 timeout\r\n");
    CHECK(a == 0 && queues == 0 && waits == 0 && lock_waits == 0 && lock_tries == 1,
          "while another writer holds the port a no-wait line is dropped after one try, no wait");
    a = cdc_out_write(&o, "+VERSION: 1\r\n");
    CHECK(a == 0 && queues == 0 && lock_waits == 1 && unlocks == u0,
          "a reply whose lock wait runs out queues nothing and does not release the holder's lock");
    other_holds = false;

    /* A host that holds the port open and never reads. */
    reset(0, 0);
    a = cdc_out_write(&o, reply);
    CHECK(a == FIFO && waited_ms == CDC_OUT_IDLE_MS && o.stalled,
          "a host that never reads: %zu bytes queued, then give up after %u ms (want %u), stalled",
          a, (unsigned)waited_ms, (unsigned)CDC_OUT_IDLE_MS);
    uint32_t w0 = waits;
    b = cdc_out_write(&o, "OK\r\n");
    CHECK(b == 0 && waits == w0 && o.stalled, "while stalled the next write does not wait (%u waits)",
          (unsigned)(waits - w0));
    CHECK(!lock_held, "a writer that gives up gives the lock back");
    host_read(100);
    b = cdc_out_write(&o, "OK\r\n");
    CHECK(b == 4 && !o.stalled, "once the host reads again, a write goes in and clears the stall");
    drain_per_wait = 64;
    drain_period_ms = CDC_OUT_POLL_MS;
    a = cdc_out_write(&o, reply);
    CHECK(a == 1024 && !o.stalled, "and a long reply waits for room again (%zu bytes)", a);

    /* A slow but live reader: one byte per 490 ms is progress, so the writer waits it out. */
    reset(1, CDC_OUT_IDLE_MS - CDC_OUT_POLL_MS);
    static char r520[521];
    memset(r520, 'x', 520); r520[520] = '\0';
    a = cdc_out_write(&o, r520);
    CHECK(a == 520 && !o.stalled, "a host reading 1 B per %u ms still gets all 520 bytes (%zu, %u ms waited)",
          (unsigned)(CDC_OUT_IDLE_MS - CDC_OUT_POLL_MS), a, (unsigned)waited_ms);

    /* The log mirror: one formatted line, whole or dropped, cut lines still ended. */
    reset(64, CDC_OUT_POLL_MS);
    const char *want_log = "I (4242) warthog.at: AT parser on CDC ACM 0\n";
    a = log_line("I (%d) %s: %s\n", 4242, "warthog.at", "AT parser on CDC ACM 0");
    host_read_all();
    CHECK(a == strlen(want_log) && rx_n == a && memcmp(rx, want_log, a) == 0,
          "a log line is mirrored whole (%zu bytes)", a);
    static char pad[600];
    memset(pad, 'p', sizeof(pad) - 1);
    const size_t lens[] = { CDC_OUT_LINE_MAX - 1, CDC_OUT_LINE_MAX, 400 };
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        reset(64, CDC_OUT_POLL_MS);
        pad[lens[i] - 1] = '\n';
        pad[lens[i]] = '\0';
        a = log_line("%s", pad);
        host_read_all();
        size_t want = lens[i] < CDC_OUT_LINE_MAX ? lens[i] : CDC_OUT_LINE_MAX - 1;
        CHECK(a == want && rx_n == want && rx[want - 1] == '\n' && memcmp(rx, pad, want - 1) == 0,
              "a %zu B log line is mirrored as %zu B ending in a newline (%zu queued)", lens[i], want, a);
        pad[lens[i] - 1] = 'p';
        pad[lens[i]] = 'p';
    }
    reset(64, CDC_OUT_POLL_MS);
    other_holds = true;
    a = log_line("I (%d) x: y\n", 1);
    CHECK(a == 0 && waits == 0 && lock_waits == 0 && lock_tries == 1,
          "a log line while a reply holds the port: dropped after one try, no wait");
    other_holds = false;
    is_connected = false;
    a = log_line("I (%d) x: y\n", 1);
    CHECK(a == 0 && lock_tries == 1 && queues == 0, "with the port closed a log line is dropped without touching the lock");

    /* Nothing to wait for. */
    reset(64, CDC_OUT_POLL_MS);
    a = cdc_out_write(&o, "+VERSION: 1\r\n");
    CHECK(a == 13 && waits == 0, "a reply that fits goes in without a wait");
    reset(64, CDC_OUT_POLL_MS);
    is_connected = false;
    a = cdc_out_write(&o, reply);
    CHECK(a == 0 && waits == 0 && queues == 0 && cdc_out_write_nowait(&o, "x") == 0,
          "with the port closed nothing is queued and nothing waits");
    is_connected = true;
    (void)fake_queue((const uint8_t *)reply, FIFO);
    uint32_t q0 = queues;
    CHECK(cdc_out_write(&o, "") == 0 && cdc_out_write(&o, NULL) == 0 && cdc_out_write(NULL, "x") == 0 &&
          cdc_out_write_nowait(&o, "") == 0 && cdc_out_write_nowait(&o, NULL) == 0 && waits == 0 && queues == q0,
          "empty or NULL text: nothing queued, no wait, even with the FIFO full");

    /* The no-wait writer runs on other tasks: it must leave the AT task's flag alone. */
    reset(64, CDC_OUT_POLL_MS);
    o.stalled = true;
    a = cdc_out_write_nowait(&o, "+MCAST: rx\r\n");
    CHECK(a == 12 && o.stalled, "the no-wait writer queues and leaves stalled as it found it");

    reset(64, CDC_OUT_POLL_MS);
    CHECK(leaks == 0 && bad_unlocks == 0 && lock_taken == unlocks,
          "every lock taken was given back once (%u taken, %u released, %u left held, %u stray)",
          lock_taken, unlocks, leaks, bad_unlocks);

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_cdc_out: all passed\n");
    return 0;
}
