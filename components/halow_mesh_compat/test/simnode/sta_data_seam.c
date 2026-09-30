/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * umac_sta_data.c, the shipping file, compiled here with one seam.
 *
 * A task other than the event loop finds a peer by loading a record pointer out
 * of the mesh peer table and reading the record's address through
 * umac_sta_data_matches_peer_addr. On the firmware the event loop can run
 * del_peer between those two steps; single-threaded, the simulator can only
 * put it there with a hook at the read itself. Nothing else changes: with no
 * hook armed every call goes straight to the real function.
 *
 * umac_ba.c, the shipping Block Ack code, is compiled here too, unchanged: the
 * Makefile's simnode source list does not name it yet.
 */
#define umac_sta_data_matches_peer_addr simnode_sta_data_matches_peer_addr_real
#include "umac/data/umac_sta_data.c"
#undef umac_sta_data_matches_peer_addr

#include "umac/core/umac_core.h"

bool umac_sta_data_matches_peer_addr(struct umac_sta_data *stad, const uint8_t *addr);

static void (*s_peer_read_hook)(void);

void simnode_set_peer_read_hook(void (*cb)(void))
{
    s_peer_read_hook = cb;
}

bool umac_sta_data_matches_peer_addr(struct umac_sta_data *stad, const uint8_t *addr)
{
    void (*cb)(void) = s_peer_read_hook;
    if (cb != NULL && !umac_core_evtloop_is_active(NULL))
    {
        s_peer_read_hook = NULL;
        cb(); /* the pointer is loaded; the record is read below */
    }
    return simnode_sta_data_matches_peer_addr_real(stad, addr);
}

#include "umac/ba/umac_ba.c"
