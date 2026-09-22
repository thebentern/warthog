/*
 * The RTOS, replaced. One of the two things the simulator fakes; the other is
 * the chip (fake_chip.c). Everything between them is the real firmware.
 *
 * Time is VIRTUAL. mmosal_get_time_ms() returns a counter the test advances
 * with simnode_advance_ms(); nothing here ever sleeps or reads the wall clock.
 * That is what makes a run reproducible, and it is what lets a test step over
 * a 600 s proxy lifetime without waiting for it.
 *
 * Mutexes are real pthread mutexes rather than no-ops: the firmware's locking
 * is load-bearing (one lock guards the path tables, the HWMP sequence number
 * and the discovery gate), so a test built on no-op locks would report a
 * deadlock-free design that has never been locked.
 */
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The real header, so a drifted signature breaks the build instead of
 * silently linking. mmosal.h declares the mutex and semaphore types as
 * opaque, which is why the definitions below are ours to make. */
#include "mmosal.h"
#include "mmhal.h"

/* A crash inside the firmware is a finding. Unbuffered output plus a
 * backtrace means it can be read from the test log alone. */
static void simnode_fault_(int sig)
{
    fflush(stdout);
    fprintf(stderr, "FAIL firmware faulted inside the simulator (signal %d)\n", sig);
    void *bt[24];
    backtrace_symbols_fd(bt, backtrace(bt, 24), 2);
    _exit(139);
}

__attribute__((constructor)) static void simnode_rtos_init_(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGSEGV, simnode_fault_);
    signal(SIGBUS, simnode_fault_);
}

/* ---- virtual clock ---------------------------------------------------- */

static uint32_t s_now_ms = 100000u; /* not zero: wrap-safe compares want headroom */

uint32_t mmosal_get_time_ms(void) { return s_now_ms; }
void simnode_set_time_ms(uint32_t t) { s_now_ms = t; }
void simnode_advance_ms(uint32_t d) { s_now_ms += d; }

/* ---- allocation ------------------------------------------------------- */

static unsigned s_live_allocs;

void *mmosal_malloc_(size_t size) { void *p = malloc(size); if (p) { s_live_allocs++; } return p; }
void *mmosal_calloc(size_t n, size_t size) { void *p = calloc(n, size); if (p) { s_live_allocs++; } return p; }
void mmosal_free(void *p) { if (p) { s_live_allocs--; free(p); } }
/** Live allocations, so a test can assert the firmware leaks no packets. */
unsigned simnode_live_allocs(void) { return s_live_allocs; }

/* ---- mutexes ---------------------------------------------------------- */

struct mmosal_mutex { pthread_mutex_t m; };

struct mmosal_mutex *mmosal_mutex_create(const char *name)
{
    (void)name;
    struct mmosal_mutex *mu = calloc(1, sizeof(*mu));
    if (mu != NULL) { pthread_mutex_init(&mu->m, NULL); }
    return mu;
}

void mmosal_mutex_delete(struct mmosal_mutex *mu)
{
    if (mu != NULL) { pthread_mutex_destroy(&mu->m); free(mu); }
}

bool mmosal_mutex_get(struct mmosal_mutex *mu, uint32_t timeout_ms)
{
    (void)timeout_ms; /* the harness never blocks: a wait here would be a bug */
    if (mu == NULL) { return false; }
    pthread_mutex_lock(&mu->m);
    return true;
}

bool mmosal_mutex_release(struct mmosal_mutex *mu)
{
    if (mu == NULL) { return false; }
    pthread_mutex_unlock(&mu->m);
    return true;
}

/* ---- binary semaphores ------------------------------------------------ */

struct mmosal_semb { unsigned count; };

struct mmosal_semb *mmosal_semb_create(const char *name)
{
    (void)name;
    return calloc(1, sizeof(struct mmosal_semb));
}

void mmosal_semb_delete(struct mmosal_semb *s) { free(s); }
bool mmosal_semb_give(struct mmosal_semb *s) { if (s) { s->count = 1; } return true; }

bool mmosal_semb_wait(struct mmosal_semb *s, uint32_t timeout_ms)
{
    (void)timeout_ms;
    /* Never blocks. The simulator is single-stepped by the test, so a wait
     * that could block would be a deadlock, not a delay. */
    if (s == NULL || s->count == 0u) { return false; }
    s->count = 0u;
    return true;
}

int mmosal_printf(const char *fmt, ...)
{
    (void)fmt;
    return 0; /* the firmware's chatter is not the test's output */
}

const char *mmosal_task_name(void) { return "simnode"; }

/* Critical sections nest in the firmware; a counter keeps that honest. */
static unsigned s_crit;
void mmosal_task_enter_critical(void) { s_crit++; }
void mmosal_task_exit_critical(void) { if (s_crit) { s_crit--; } }
unsigned simnode_in_critical(void) { return s_crit; }

void mmosal_impl_assert(void)
{
    /* A firmware assertion inside the harness is a finding, not noise: print
     * where it came from so it can be read without a debugger. */
    fflush(stdout);
    fprintf(stderr, "FAIL firmware assertion fired inside the simulator\n");
    void *bt[24];
    int n = backtrace(bt, 24);
    backtrace_symbols_fd(bt, n, 2);
    abort();
}

void mmosal_log_failure_info(const struct mmosal_failure_info *info) { (void)info; }

uint32_t mmhal_random_u32(uint32_t min, uint32_t max)
{
    /* Deterministic: a reproducible run matters more than entropy, and the
     * mesh uses this only for jitter and identifiers. */
    static uint32_t s = 0x12345678u;
    s = s * 1664525u + 1013904223u;
    return (max <= min) ? min : (min + (s % (max - min + 1u)));
}
