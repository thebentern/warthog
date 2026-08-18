/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * Public mmwlan_mesh_* entry points. Compiled into morselib; the mmwlan_*
 * names are on librarymangler.py's protected list (`mmwlan*` glob), so they
 * survive un-mangled and are callable from application code.
 *
 * Skeleton: enable() validates args and delegates to umac_mesh_enable_mesh(),
 * which currently returns MMWLAN_UNAVAILABLE. The umac core start/stop dance
 * (mmwlan_ap_enable() does this via the file-static
 * umac_stop_core_if_no_interface) is deferred until umac_mesh_enable_mesh()
 * actually drives the chip — at which point the start/stop belongs inside the
 * umac event handler, not here. See docs/history/mesh-port-scope.md.
 */

/* Per-file MMLOG override to INF — see the note in umac_mesh.c. Must precede
 * any include that pulls mmlog.h. */
#define MMLOG_LEVEL_OVRD 5

#include "mmwlan.h"
#include "mmwlan_mesh.h"
#include "mmlog.h"
#include "mmosal.h"
#include <stdio.h>
#include "esp_log.h"

#include "umac_mesh.h"
#include "umac/core/umac_core.h"
#include "umac/data/umac_data.h"

static mmwlan_mesh_peer_event_cb_t s_peer_event_cb;
static void *s_peer_event_arg;

/* Runs on the umac task — same contract as umac_ap_start_evt_handler. The
 * umac interface/datapath internals expect to be touched only from this
 * context, which is why enable() queues rather than calling directly. */
static void umac_mesh_start_evt_handler(struct umac_data *umacd, const struct umac_evt *evt)
{
    enum mmwlan_status status = umac_mesh_enable_mesh(umacd, evt->args.mesh_start.args);
    if (evt->args.mesh_start.status)
    {
        *evt->args.mesh_start.status = status;
    }
    if (evt->args.mesh_start.semb)
    {
        mmosal_semb_give(evt->args.mesh_start.semb);
    }
}

enum mmwlan_status mmwlan_mesh_enable(const struct mmwlan_mesh_args *args)
{
    enum mmwlan_status status = MMWLAN_ERROR;
    struct umac_data *umacd = umac_data_get_umacd();
    struct umac_root_data *root = umac_data_get_root(umacd);
    if (root == NULL)
    {
        return MMWLAN_UNAVAILABLE;
    }

    if (!umac_mesh_validate_args(umacd, args))
    {
        return MMWLAN_INVALID_ARGUMENT;
    }

    status = umac_core_start(umacd);
    if (status != MMWLAN_SUCCESS)
    {
        return status;
    }

    /* Run the enable on the umac task and block for the result, exactly as
     * mmwlan_ap_enable() does for ap_start. */
    UMAC_QUEUE_EVT_AND_WAIT(umac_mesh_start_evt_handler, mesh_start, &status, .args = args);

    return status;
}

enum mmwlan_status mmwlan_mesh_disable(void)
{
    struct umac_data *umacd = umac_data_get_umacd();
    return umac_mesh_disable_mesh(umacd);
}

enum mmwlan_status mmwlan_mesh_register_peer_event_cb(mmwlan_mesh_peer_event_cb_t cb, void *arg)
{
    s_peer_event_cb = cb;
    s_peer_event_arg = arg;
    return MMWLAN_SUCCESS;
}

uint8_t mmwlan_mesh_get_peer_count(void)
{
    struct umac_data *umacd = umac_data_get_umacd();
    return umac_mesh_get_peer_count(umacd);
}

/* Public wrapper for the host-driven probe-request burst. */
extern int umac_mesh_tx_broadcast_probe(void);
int mmwlan_mesh_tx_broadcast_probe(void)
{
    return umac_mesh_tx_broadcast_probe();
}

/* Public wrapper for the opcode probe. */
extern int umac_mesh_probe_opcode(uint16_t opcode);
int mmwlan_mesh_probe_opcode(uint16_t opcode)
{
    return umac_mesh_probe_opcode(opcode);
}
