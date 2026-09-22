/*
 * The two app-side entry points morselib calls back into, plus the one core
 * timeout registration the simulator does not drive.
 *
 * On the firmware these live in main/at.c and main/mesh.c, which the harness
 * does not link: at.c is the AT command surface and drags in USB, NVS and the
 * ESP-IDF netif stack. The storage those files provide is mirrored by the
 * generated warthog_globals.c; these are the functions.
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

/* umac_core's timeout wheel. The simulator drives time itself through the
 * service tick, so a registered timeout is recorded and never fires; a test
 * that needs one calls the tick instead. */
static unsigned s_timeouts;

bool umac_core_register_timeout(struct umac_data *umacd, uint32_t delta_ms,
                                umac_core_timeout_handler_t handler, void *arg1, void *arg2)
{
    (void)umacd; (void)delta_ms; (void)handler; (void)arg1; (void)arg2;
    s_timeouts++;
    return true;
}

unsigned simnode_timeouts_registered(void) { return s_timeouts; }
