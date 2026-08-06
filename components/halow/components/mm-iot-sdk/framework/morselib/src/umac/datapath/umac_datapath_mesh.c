/*
 * warthog mesh-support fork — umac datapath ops for 802.11s mesh mode.
 *
 * Mirrors umac_datapath_ap.c structurally but with safe NULL/empty-peer-table
 * stubs everywhere except the RX management frame path. The chip drives
 * umac_datapath_process_tx_frame autonomously (for beacon-template TX and
 * occasional house-keeping frames); AP ops crashed mesh boards because the
 * AP TX-dequeue derefs an AP-STA struct that doesn't exist in mesh mode.
 * Our stubs return "no frame, no peer" so the autonomous TX poll exits
 * cleanly and the chip handles whatever else it needs internally.
 *
 * The RX path is the substantive part: process_rx_mgmt_frame_mesh routes
 * action frames through umac_datapath_process_rx_action_frame (which has
 * our DOT11_ACTION_CATEGORY_SELF_PROTECTED case for PLINK), and other
 * mgmt subtypes through the supplicant fan-out. Probe req / response are
 * intentionally not handled (no AP context to answer with) — mesh peer
 * discovery happens via S1G beacons + PLINK action frames instead.
 *
 * Built as part of Phase 4f-step7 (docs/mesh-port-scope.md). Not a full
 * mesh datapath — no peer table yet (mmwlan_mesh_get_peer_count stays 0),
 * no 4-address data-frame construction (that's Phase 6). Just enough to
 * pass the data->ops NULL check in umac_datapath_rx_frame_filter without
 * crashing the chip's TX poll.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 */

/* Surface INF/WRN logs in this file while we're debugging the chain. ERR
 * is always visible at morselib's default threshold, but we want to see
 * each frame routing decision. Beware: bumping OVRD pulls mm_hexdump in
 * for some unrelated logging helpers — keep diagnostic lines at ERR/WRN
 * for now. */

#include "mmdrv.h"
#include "mmlog.h"
#include "mmpkt.h"
#include "mmwlan.h"
#include "mmwlan_internal.h"
#include "umac/datapath/umac_datapath_data.h"
#include "umac/datapath/umac_datapath_private.h"
#include "umac/data/umac_data.h"
#include "umac/supplicant_shim/umac_supp_shim.h"
#include "dot11/dot11.h"
#include "dot11/dot11_utils.h"
#include "common/mac_address.h"
#include "umac/stats/umac_stats.h"

/* --- RX management frame dispatch ---------------------------------------- */

static void process_rx_mgmt_frame_mesh(struct umac_data *umacd,
                                       struct umac_sta_data *stad,
                                       struct mmpktview *rxbufview)
{
    (void)stad;  /* No mesh peer table yet; stad is always NULL here */
    const struct dot11_hdr *header = (struct dot11_hdr *)mmpkt_get_data_start(rxbufview);
    uint16_t frame_control_le = header->frame_control;
    uint16_t subtype = dot11_frame_control_get_subtype(frame_control_le);

    switch (subtype)
    {
        case DOT11_FC_SUBTYPE_ACTION:
            /* PLINK Open/Confirm/Close land here. The action-frame switch
             * in umac_datapath_process_rx_action_frame has our
             * DOT11_ACTION_CATEGORY_SELF_PROTECTED case routing to the
             * supplicant fan-out → hostap mesh_mpm_action_rx. */
            umac_datapath_process_rx_action_frame(umacd, stad, rxbufview);
            break;

        case DOT11_FC_SUBTYPE_PROBE_REQ:
            /* Mesh peers can probe each other, but we don't yet have a
             * response path (would need AP-like probe-response generation
             * with mesh IEs). Drop silently for now. */
            MMLOG_WRN("mesh: ignoring probe req (no responder yet)\n");
            break;

        case DOT11_FC_SUBTYPE_PROBE_RSP:
            /* Forward to supplicant — bss.c uses these for IE caching. */
            umac_supp_process_mgmt_frame(umacd, rxbufview);
            break;

        default:
            /* Other mgmt subtypes (auth/assoc/disassoc/deauth/etc.) — route
             * to supplicant. Anything mesh-specific that needs handling will
             * surface as a WRN here. */
            MMLOG_ERR("mesh: rx mgmt subtype=%u (fc=0x%04x) → supplicant fan-out\n",
                      subtype, frame_control_le);
            umac_supp_process_mgmt_frame(umacd, rxbufview);
            break;
    }
}

/* --- Peer-table lookups: no peers yet, return NULL safely --------------- */

static struct umac_sta_data *mesh_lookup_stad_by_peer_addr(struct umac_data *umacd,
                                                           const uint8_t *peer_addr)
{
    (void)umacd; (void)peer_addr;
    /* Phase 4f stub: no peer table yet. Returning NULL is safe; the callers
     * in umac_datapath.c check for NULL before dereferencing. */
    return NULL;
}

static struct umac_sta_data *mesh_lookup_stad_by_tx_dest_addr(struct umac_data *umacd,
                                                              const uint8_t *dest_addr)
{
    (void)umacd; (void)dest_addr;
    return NULL;
}

static struct umac_sta_data *mesh_lookup_stad_by_aid(struct umac_data *umacd, uint16_t aid)
{
    (void)umacd; (void)aid;
    return NULL;
}

/* --- STA state functions: called only when stad is non-NULL, which won't
 *     happen until we have a peer table. Safe stubs. ---------------------- */

static bool mesh_set_stad_sleep_state(struct umac_sta_data *stad, bool asleep)
{
    (void)stad; (void)asleep;
    return false;
}

static bool mesh_is_stad_tx_paused(struct umac_sta_data *stad)
{
    (void)stad;
    /* Return true (paused) if anyone ever calls this with a non-NULL stad
     * we haven't populated. Errs on the side of "don't TX", which is safer
     * than the AP version's NULL deref. */
    return true;
}

static enum mmwlan_sta_state mesh_get_sta_state(struct umac_sta_data *stad)
{
    (void)stad;
    /* Returning DISABLED is the safest default — anything that branches
     * on this value treats DISABLED as "not associated, don't send". */
    return MMWLAN_STA_DISABLED;
}

/* --- TX queue: empty-queue stubs ---------------------------------------- */

static void mesh_enqueue_tx_frame(struct umac_data *umacd,
                                  struct umac_sta_data *stad,
                                  struct mmpkt *txbuf)
{
    (void)umacd; (void)stad;
    /* No peer to send to — drop the frame. mmpkt ownership is the caller's
     * convention; mirroring umac_ap_queue_pkt which takes ownership. */
    MMLOG_WRN("mesh: enqueue TX with no peer; dropping pkt %p\n", (void *)txbuf);
    mmpkt_release(txbuf);
}

static bool mesh_dequeue_tx_frame(struct umac_data *umacd,
                                  struct umac_sta_data **stad,
                                  struct mmpkt **txbuf)
{
    (void)umacd;
    /* THIS is the function that crashed us with AP ops installed. The
     * caller (umac_datapath_process_tx_frame, line ~1985) does:
     *   has_more = data->ops->dequeue_tx_frame(umacd, &stad, &mmpkt);
     *   if (mmpkt == NULL) return false;
     *   MMOSAL_ASSERT(stad != NULL);
     * Setting *txbuf to NULL means the caller bails before the assert. */
    *stad = NULL;
    *txbuf = NULL;
    return false;
}

/* --- Header construction for outgoing data frames ----------------------- */

static void mesh_construct_80211_data_header(struct umac_sta_data *stad,
                                             const struct umac_8023_hdr *hdr_8023,
                                             struct dot11_data_hdr *data_hdr)
{
    (void)stad; (void)hdr_8023; (void)data_hdr;
    /* Phase 6 territory: mesh data frames are 4-address. With no peers and
     * no data flowing, this should not be reached. If it is, the log line
     * tells us TX-data wiring needs the full 4-addr implementation. */
    MMLOG_ERR("mesh: construct_80211_data_header called (4-addr mesh data Phase 6 — unimplemented)\n");
}

/* --- Frames allowed before "association" -------------------------------- */

/* In mesh mode there is no association in the AP/STA sense — peers come and
 * go through PLINK. The filter at umac_datapath_rx_frame_allowed_pre_association
 * always treats us as "pre-association" because no STA-mode connection
 * happens. Let through what mesh actually uses on the wire:
 *
 *   • S1G_BEACON (EXT type) — peer discovery (skipped by line 1204 check
 *     in umac_datapath.c too, but include for completeness)
 *   • PROBE_REQ / PROBE_RESP — active peer discovery
 *   • ACTION — PLINK Open/Confirm/Close + (later) mesh routing frames
 *   • AUTH — SAE; harmless for open mesh, needed for SAE mesh
 */
const uint16_t frames_allowed_pre_association_mesh_mode[] = {
    DOT11_VER_TYPE_SUBTYPE(0, MGMT, PROBE_REQ),
    DOT11_VER_TYPE_SUBTYPE(0, MGMT, PROBE_RSP),
    DOT11_VER_TYPE_SUBTYPE(0, MGMT, AUTH),
    DOT11_VER_TYPE_SUBTYPE(0, MGMT, ACTION),
    DOT11_VER_TYPE_SUBTYPE(0, EXT,  S1G_BEACON),
    UINT16_MAX,
};

/* --- Ops table ---------------------------------------------------------- */

const struct umac_datapath_ops datapath_ops_mesh = {
    .process_rx_mgmt_frame       = process_rx_mgmt_frame_mesh,
    .lookup_stad_by_peer_addr    = mesh_lookup_stad_by_peer_addr,
    .lookup_stad_by_tx_dest_addr = mesh_lookup_stad_by_tx_dest_addr,
    .lookup_stad_by_aid          = mesh_lookup_stad_by_aid,
    .set_stad_sleep_state        = mesh_set_stad_sleep_state,
    .is_stad_tx_paused           = mesh_is_stad_tx_paused,
    .enqueue_tx_frame            = mesh_enqueue_tx_frame,
    .dequeue_tx_frame            = mesh_dequeue_tx_frame,
    .construct_80211_data_header = mesh_construct_80211_data_header,
    .get_sta_state               = mesh_get_sta_state,
    .frames_allowed_pre_association = frames_allowed_pre_association_mesh_mode,
};

void umac_datapath_configure_mesh_mode(struct umac_data *umacd)
{
    struct umac_datapath_data *data = umac_data_get_datapath(umacd);
    data->ops = &datapath_ops_mesh;
    MMLOG_INF("Datapath configured for mesh mode\n");
}
