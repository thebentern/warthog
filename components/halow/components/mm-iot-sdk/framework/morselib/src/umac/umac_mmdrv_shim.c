/*
 *  Copyright 2022 Morse Micro
 *  SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 */

#include "common/common.h"
#include "mmlog.h"
#include "umac/ap/umac_ap.h"
#include "umac/core/umac_core.h"
#include "umac/datapath/umac_datapath.h"
#include "umac/connection/umac_connection.h"
#include "umac/offload/umac_offload.h"
#include "umac/regdb/umac_regdb.h"
#include "umac/stats/umac_stats.h"
/* Phase 4f-step13 — mesh-mode beacon template path. When mesh is active,
 * route the chip's beacon-template request to umac_mesh_get_beacon instead
 * of umac_ap_get_beacon (which asserts when no AP context exists). */
#include "umac/mesh/umac_mesh_beacon.h"

/* RX entry counters (storage in main/at.c; AT+RXCHAN?). */
extern volatile uint32_t g_warthog_shim_rx;
extern volatile uint32_t g_warthog_shim_rx_notrunning;



void mmdrv_host_process_rx_frame(struct mmpkt *rxbuf, uint16_t channel)
{
    MM_UNUSED(channel);

    MMOSAL_DEV_ASSERT(channel == 0);
    struct umac_data *umacd = umac_data_get_umacd();

    /* Phase 4f-step14 diagnostic — log every chip-delivered frame at the
     * absolute earliest point in the host stack. Earlier `rx#` logs live
     * inside umac_datapath_rx_frame which is downstream of multiple filter
     * gates. If this log fires but those don't, the chip IS delivering
     * frames but they're being dropped in the umac layer. If THIS log
     * doesn't fire either, the chip itself is filtering out peer beacons
     * before they cross the chip→host boundary — the firmware-side mesh
     * RX path doesn't admit foreign mesh BSS frames.
     *
     * Throttled the same way the beacon-template log is: first 8 calls
     * verbatim, then every 200th. ERR level so it surfaces at default
     * morselib log threshold (which is ERR-only). */
    static uint32_t s_rx_shim_count = 0;
    s_rx_shim_count++;
    if (s_rx_shim_count <= 8 || (s_rx_shim_count % 200) == 0)
    {
        MMLOG_ERR("mmdrv_host_process_rx_frame#%lu: chan=%u\n",
                  (unsigned long)s_rx_shim_count, (unsigned)channel);
    }

    g_warthog_shim_rx++;
    if (!umac_core_is_running(umacd))
    {
        g_warthog_shim_rx_notrunning++;
        mmpkt_release(rxbuf);
        return;
    }

    umac_datapath_rx_frame(umacd, rxbuf);
}

void mmdrv_host_process_tx_status(struct mmpkt *mmpkt)
{
    struct umac_data *umacd = umac_data_get_umacd();

    /* Phase 4f-step20 diagnostic — log every TX_STATUS notification from
     * the chip. We've never instrumented this. If our mesh beacon is
     * actually being TXed on-air, we should see TX_STATUS events for
     * each beacon (every 100ms at 100 TU beacon interval). If we see
     * ZERO, the chip is silently dropping our beacon template — meaning
     * our beacon construction is malformed in a way the chip rejects
     * before TX (and the peer never receives anything, which would
     * explain why our peer chip never sees an RX either). */
    static uint32_t s_tx_status_count = 0;
    s_tx_status_count++;
    if (s_tx_status_count <= 8 || (s_tx_status_count % 100) == 0)
    {
        MMLOG_ERR("mmdrv_host_process_tx_status#%lu: chip TX completion\n",
                  (unsigned long)s_tx_status_count);
    }

    if (!umac_core_is_running(umacd))
    {
        mmpkt_release(mmpkt);
        return;
    }

    umac_datapath_handle_tx_status(umacd, mmpkt);
}

void mmdrv_host_set_tx_paused(uint16_t sources_mask, bool paused)
{
    struct umac_data *umacd = umac_data_get_umacd();
    if (paused)
    {
        umac_datapath_pause(umacd, sources_mask);
    }
    else
    {
        umac_datapath_unpause(umacd, sources_mask);
    }
}

void mmdrv_host_update_tx_paused(uint16_t sources_mask, mmdrv_host_update_tx_paused_cb_t cb)
{
    struct umac_data *umacd = umac_data_get_umacd();
    umac_datapath_update_tx_paused(umacd, sources_mask, cb);
}

static void hw_restart_evt_handler(struct umac_data *umacd, const struct umac_evt *evt)
{
    MM_UNUSED(evt);

    if (umac_interface_get_vif_id(umacd, UMAC_INTERFACE_AP) != UMAC_INTERFACE_VIF_ID_INVALID)
    {
        MMLOG_ERR("Unable to recover from hardware restart with AP interface active\n");
        MMOSAL_ASSERT(false);
    }

    if (umac_interface_is_active(umacd))
    {
        const char *country_code = umac_regdb_get_country_code(umacd);
        if (country_code == NULL)
        {
            MMLOG_ERR("Channel list not set\n");
            return;
        }

        mmdrv_deinit();
        MMOSAL_ASSERT(mmdrv_init(NULL, country_code) == 0);

        umac_interface_configure_periodic_health_check(umacd);
        umac_stats_increment_hw_restart_counter(umacd);
        umac_scan_handle_hw_restarted(umacd);
        umac_connection_handle_hw_restarted(umacd);
    }

    MMLOG_DBG("Notify MMDRV that restart has completed\n");
    mmdrv_hw_restart_completed();
}

void mmdrv_host_hw_restart_required(void)
{
    struct umac_data *umacd = umac_data_get_umacd();

    struct umac_evt evt = UMAC_EVT_INIT(hw_restart_evt_handler);
    bool ok = umac_core_evt_queue_at_start(umacd, &evt);
    if (!ok)
    {

        MMLOG_ERR("Failed to queue HW_RESTARTED event.\n");
        MMOSAL_ASSERT(false);
    }
}

static void beacon_loss_evt_handler(struct umac_data *umacd, const struct umac_evt *evt)
{
    MM_UNUSED(evt);

    umac_connection_handle_beacon_loss(umacd);
}

void mmdrv_host_beacon_loss(uint32_t num_bcns)
{
    MM_UNUSED(num_bcns);
    struct umac_data *umacd = umac_data_get_umacd();

    struct umac_evt evt = UMAC_EVT_INIT(beacon_loss_evt_handler);
    bool ok = umac_core_evt_queue(umacd, &evt);
    if (!ok)
    {
        MMLOG_WRN("Failed to queue BEACON_LOSS event.\n");
    }
}

static void connection_loss_evt_handler(struct umac_data *umacd, const struct umac_evt *evt)
{
    MM_UNUSED(evt);

    MMLOG_WRN("UMAC_EVT_CONNECTION_LOSS event received with reason code %lu\n",
              evt->args.connection_loss.reason);
    umac_connection_process_disassoc_req(umacd, NULL);
}

void mmdrv_host_connection_loss(uint32_t reason)
{
    struct umac_data *umacd = umac_data_get_umacd();

    struct umac_evt evt = UMAC_EVT_INIT(connection_loss_evt_handler);
    evt.args.connection_loss.reason = reason;
    bool ok = umac_core_evt_queue(umacd, &evt);
    if (!ok)
    {
        MMLOG_WRN("Failed to queue CONNECTION_LOSS event.\n");
    }
}

static void dhcp_lease_update_evt_handler(struct umac_data *umacd, const struct umac_evt *evt)
{
    umac_offload_dhcp_lease_update(umacd, &evt->args.dhcp_offload_lease_update.lease_info);
}

void mmdrv_host_dhcp_lease_update(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns)
{
    struct umac_data *umacd = umac_data_get_umacd();

    struct umac_evt evt = UMAC_EVT_INIT(dhcp_lease_update_evt_handler);
    evt.args.dhcp_offload_lease_update.lease_info.ip4_addr = ip;
    evt.args.dhcp_offload_lease_update.lease_info.mask4_addr = mask;
    evt.args.dhcp_offload_lease_update.lease_info.gw4_addr = gw;
    evt.args.dhcp_offload_lease_update.lease_info.dns4_addr = dns;

    bool ok = umac_core_evt_queue(umacd, &evt);
    if (!ok)
    {
        MMLOG_WRN("Failed to queue DHCP_OFFLOAD_LEASE_UPDATE event.\n");
    }
}

static void hw_scan_complete_evt_handler(struct umac_data *umacd, const struct umac_evt *evt)
{
    umac_scan_hw_scan_done(umacd, evt->args.hw_scan_done.state);
}

void mmdrv_host_hw_scan_complete(enum mmwlan_scan_state state)
{
    struct umac_data *umacd = umac_data_get_umacd();

    struct umac_evt evt =
        UMAC_EVT_INIT_ARGS(hw_scan_complete_evt_handler, hw_scan_done, .state = state);
    bool ok = umac_core_evt_queue(umacd, &evt);
    if (!ok)
    {
        MMLOG_ERR("Failed to queue HW_SCAN_DONE event.\n");
    }
}

void mmdrv_host_stats_increment_datapath_driver_rx_alloc_failures(void)
{
    struct umac_data *umacd = umac_data_get_umacd();
    umac_stats_increment_datapath_driver_rx_alloc_failures(umacd);
}


void mmdrv_host_stats_increment_datapath_driver_rx_read_failures(void)
{
    struct umac_data *umacd = umac_data_get_umacd();
    umac_stats_increment_datapath_driver_rx_read_failures(umacd);
}

/* Beacon-handshake counters, reported by AT+BCNSTAT?.
 *
 * Storage is defined in main/at.c, not here: morselib links as an archive and
 * the linker will not extract an object from it merely to satisfy a reference
 * originating in main, so main must own the storage. */
extern volatile uint32_t g_warthog_bcn_req;
extern volatile uint32_t g_warthog_bcn_served;
extern volatile uint32_t g_warthog_bcn_null;
extern volatile uint32_t g_warthog_bcn_inactive;

struct mmpkt *mmdrv_host_get_beacon(void)
{
    struct umac_data *umacd = umac_data_get_umacd();
    /* Phase 4f-step11 diagnostic — log every call so we can confirm whether
     * the chip is asking the host for beacon templates while in mesh mode.
     * If we never see this line in mesh-smoke output, the chip generates
     * mesh beacons internally (without the Mesh ID IE the other side needs)
     * and providing a host-built template won't help. If we DO see it but
     * umac_ap_get_beacon returns NULL/garbage for mesh (no AP context),
     * that's the path to fix. ERR level so it surfaces at default threshold. */
    static uint32_t s_beacon_req_count = 0;
    s_beacon_req_count++;
    if (s_beacon_req_count <= 8 || (s_beacon_req_count % 200) == 0)
    {
        MMLOG_ERR("mmdrv_host_get_beacon#%lu: chip requesting beacon template\n",
                  (unsigned long)s_beacon_req_count);
    }

    /* Counters mirrored to AT+BCNSTAT? (storage in main/at.c). The MMLOG line
     * above is unreachable on this hardware once the app claims the USB PHY for
     * TinyUSB, so the log alone cannot answer whether the chip is still asking
     * for beacon templates. "beacon IRQ fires once and never again" is the
     * symptom being chased: req counts the chip's asks, served/null count what
     * the host handed back. */
    g_warthog_bcn_req = s_beacon_req_count;

    /* Phase 4f-step13 — when mesh is active, hand the chip a proper mesh
     * beacon template (with Mesh ID IE 114, Mesh Configuration IE 113, S1G
     * Capabilities + Operation IEs). Without this the chip falls back to an
     * internally-generated beacon that lacks the Mesh ID IE — and that's
     * exactly why two boards on the same mesh_id can't discover each other.
     * See umac_mesh_beacon.c for the frame layout. */
    if (umac_mesh_beacon_is_active())
    {
        struct mmpkt *bcn = umac_mesh_get_beacon(umacd);
        if (bcn != NULL)
        {
            g_warthog_bcn_served++;
        }
        else
        {
            g_warthog_bcn_null++;
        }
        return bcn;
    }
    g_warthog_bcn_inactive++;
    /* AP mode path: chip is on an AP VIF, so umac_ap_data is set up. If
     * neither mesh nor AP is active, returning NULL is safe — the chip
     * will fall back to its internal template and the (now-unused) beacon
     * channel will stay idle. */
    if (umac_data_get_ap(umacd) == NULL)
    {
        return NULL;
    }
    return umac_ap_get_beacon(umacd);
}
