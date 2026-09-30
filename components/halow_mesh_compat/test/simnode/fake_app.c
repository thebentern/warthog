/*
 * The two app-side entry points morselib calls back into, plus umac_core's
 * timeout registration (recorded, never fired) and event queue (run by
 * simnode_pump).
 *
 * On the firmware the entry points live in main/at.c and main/mesh.c, which
 * the harness does not link: at.c is the AT command surface and drags in USB,
 * NVS and the ESP-IDF netif stack. The storage those files provide is mirrored
 * by the generated warthog_globals.c; these are the functions. The core pair
 * stands in for morselib's umac_core, whose event loop is a task.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The real headers, so the compiler checks these against the declarations the
 * firmware itself sees. A fake whose signature has drifted still links; only
 * the header catches it. */
#include "umac/core/umac_core.h"
#include "umac/data/umac_data.h"

void warthog_mesh_rssi_note(const uint8_t *ta, int16_t rssi, int8_t noise, uint8_t bw_mhz);

/* main/at.c: per-peer RSSI/noise telemetry, fed from the datapath's RX path.
 * Recorded here so a test can assert the datapath reported a peer at all. */
static struct { uint8_t ta[6]; int16_t rssi; int8_t noise; uint8_t bw; unsigned n; } s_rssi[8];
static unsigned s_rssi_calls;

void warthog_mesh_rssi_note(const uint8_t *ta, int16_t rssi, int8_t noise, uint8_t bw_mhz)
{
    s_rssi_calls++;
    if (ta == NULL) { return; }
    for (unsigned i = 0; i < 8; i++)
    {
        if (s_rssi[i].n != 0 && memcmp(s_rssi[i].ta, ta, 6) == 0)
        {
            s_rssi[i].rssi = rssi; s_rssi[i].noise = noise; s_rssi[i].bw = bw_mhz; s_rssi[i].n++;
            return;
        }
    }
    for (unsigned i = 0; i < 8; i++)
    {
        if (s_rssi[i].n == 0)
        {
            memcpy(s_rssi[i].ta, ta, 6);
            s_rssi[i].rssi = rssi; s_rssi[i].noise = noise; s_rssi[i].bw = bw_mhz; s_rssi[i].n = 1;
            return;
        }
    }
}

unsigned simnode_rssi_calls(void) { return s_rssi_calls; }

bool simnode_rssi_for(const uint8_t *ta, int16_t *rssi)
{
    for (unsigned i = 0; i < 8; i++)
    {
        if (s_rssi[i].n != 0 && memcmp(s_rssi[i].ta, ta, 6) == 0)
        {
            if (rssi != NULL) { *rssi = s_rssi[i].rssi; }
            return true;
        }
    }
    return false;
}

/* umac_core's timeout queue. A registered timeout fires only when a test calls
 * simnode_run_timeouts(), at the virtual time it was due: suites that never
 * call it see every timeout recorded and none fired, as before. */
uint32_t mmosal_get_time_ms(void);

#define SIMNODE_TIMEOUTS_MAX 16u
static struct { uint32_t due; umac_core_timeout_handler_t h; void *a1, *a2; bool used; } s_to[SIMNODE_TIMEOUTS_MAX];
static unsigned s_timeouts;
static bool s_fail_next_to;

/* The next registration fails, as it does when the core's timeout pool is empty. */
void simnode_fail_next_timeout(void) { s_fail_next_to = true; }

bool umac_core_register_timeout(struct umac_data *umacd, uint32_t delta_ms,
                                umac_core_timeout_handler_t handler, void *arg1, void *arg2)
{
    (void)umacd;
    if (s_fail_next_to) { s_fail_next_to = false; return false; }
    s_timeouts++;
    for (unsigned i = 0; i < SIMNODE_TIMEOUTS_MAX; i++)
    {
        if (!s_to[i].used)
        {
            s_to[i].due = mmosal_get_time_ms() + delta_ms;
            s_to[i].h = handler; s_to[i].a1 = arg1; s_to[i].a2 = arg2; s_to[i].used = true;
            return true;
        }
    }
    return false; /* the pool is exhausted, as the firmware's can be */
}

/* umac_timeoutq_deplete_timeout_protected: pull one matching timeout earlier, never later. */
int umac_core_deplete_timeout(struct umac_data *umacd, uint32_t delta_ms,
                              umac_core_timeout_handler_t handler, void *arg1, void *arg2)
{
    (void)umacd;
    for (unsigned i = 0; i < SIMNODE_TIMEOUTS_MAX; i++)
    {
        if (s_to[i].used && s_to[i].h == handler && s_to[i].a1 == arg1 && s_to[i].a2 == arg2)
        {
            uint32_t due = mmosal_get_time_ms() + delta_ms;
            if ((int32_t)(due - s_to[i].due) < 0) { s_to[i].due = due; return 1; }
            return 0;
        }
    }
    return -1;
}

unsigned simnode_timeouts_registered(void) { return s_timeouts; }

unsigned simnode_timeouts_pending(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < SIMNODE_TIMEOUTS_MAX; i++) { n += s_to[i].used ? 1u : 0u; }
    return n;
}

/* The earliest pending timeout's due time; false when none is pending. */
bool simnode_timeout_next_due(uint32_t *due)
{
    int best = -1;
    for (unsigned i = 0; i < SIMNODE_TIMEOUTS_MAX; i++)
    {
        if (s_to[i].used && (best < 0 || (int32_t)(s_to[i].due - s_to[best].due) < 0)) { best = (int)i; }
    }
    if (best >= 0 && due != NULL) { *due = s_to[best].due; }
    return best >= 0;
}

/* Which task is running: the firmware's umac event loop runs the datapath, queued
 * events, core timeouts and hostap; a test's own calls stand for the other tasks. */
static unsigned s_on_loop;
void simnode_loop_enter(void) { s_on_loop++; }
void simnode_loop_leave(void) { s_on_loop -= (s_on_loop != 0u) ? 1u : 0u; }

bool umac_core_evtloop_is_active(struct umac_data *umacd)
{
    (void)umacd;
    return s_on_loop != 0u;
}

/* Fire every timeout due by now, earliest first, including any a handler registers. */
unsigned simnode_fire_timeouts(void)
{
    unsigned fired = 0;
    for (unsigned guard = 0; guard < 256u; guard++)
    {
        int best = -1;
        for (unsigned i = 0; i < SIMNODE_TIMEOUTS_MAX; i++)
        {
            if (s_to[i].used && (int32_t)(mmosal_get_time_ms() - s_to[i].due) >= 0 &&
                (best < 0 || (int32_t)(s_to[i].due - s_to[best].due) < 0))
            {
                best = (int)i;
            }
        }
        if (best < 0) { break; }
        umac_core_timeout_handler_t h = s_to[best].h;
        void *a1 = s_to[best].a1, *a2 = s_to[best].a2;
        s_to[best].used = false; /* dequeued before it runs, as the core loop does */
        simnode_loop_enter();
        h(a1, a2);
        simnode_loop_leave();
        fired++;
    }
    return fired;
}

/* umac_core's event queue. Posting copies the event, as the firmware's pool
 * does; simnode_pump() runs the handlers, standing in for the event loop. */
#define SIMNODE_EVTQ_MAX 8u
static struct umac_evt s_evtq[SIMNODE_EVTQ_MAX];
static unsigned s_evtq_head, s_evtq_n;

bool umac_core_evt_queue(struct umac_data *umacd, const struct umac_evt *evt)
{
    (void)umacd;
    if (evt == NULL || s_evtq_n >= SIMNODE_EVTQ_MAX) { return false; }
    s_evtq[(s_evtq_head + s_evtq_n) % SIMNODE_EVTQ_MAX] = *evt;
    s_evtq_n++;
    return true;
}

/* Run the oldest queued event, as the loop does; false when none was queued. */
bool simnode_evt_dispatch_one(struct umac_data *umacd)
{
    if (s_evtq_n == 0u) { return false; }
    struct umac_evt evt = s_evtq[s_evtq_head];
    s_evtq_head = (s_evtq_head + 1u) % SIMNODE_EVTQ_MAX;
    s_evtq_n--;
    evt.handler(umacd, &evt);
    return true;
}

unsigned simnode_evt_pending(void) { return s_evtq_n; }

/* Throw every queued event away unrun, as a core restart does. */
void simnode_evt_discard(void) { s_evtq_n = 0; }

static void simnode_evt_noop_(struct umac_data *umacd, const struct umac_evt *evt)
{
    (void)umacd;
    (void)evt;
}

/* Fill the queue with no-op events, so the next post fails as a full pool does. */
unsigned simnode_evt_fill(void)
{
    const struct umac_evt noop = UMAC_EVT_INIT(simnode_evt_noop_);
    unsigned n = 0;
    while (umac_core_evt_queue(NULL, &noop)) { n++; }
    return n;
}
