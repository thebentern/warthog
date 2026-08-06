/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * Phase 4f-step23 — S1G beacon constructor for mesh.
 *
 * ============================================================================
 * THE KEY INSIGHT BEHIND STEP 23
 * ============================================================================
 *
 * After 21+ iterations against the chip's mesh path with zero RX activity, the
 * one consistent observation was: chip accepts every command, our beacon
 * template gets handed in once, peer chip never sees a single frame.
 *
 * Deep dive into the upstream Linux morse_driver `dot11ah` subsystem (cloned
 * to /tmp/morse_driver, see docs/morse-support-inquiry.md) surfaced the
 * missing piece:
 *
 *   The Linux driver TRANSFORMS regular 802.11 beacons into S1G beacons
 *   BEFORE handing them to the chip.
 *
 *   See `morse_dot11ah_beacon_to_s1g` in /tmp/morse_driver/dot11ah/
 *   tx_11n_to_s1g.c:819–956 — for every beacon mac80211 produces, dot11ah
 *   rewrites the MAC header (regular 0x80 → S1G-Ext 0x1C), strips
 *   non-S1G IEs (Supp Rates, DS Params, ERP, HT cap/op, …), and inserts
 *   the S1G-specific IEs (Beacon Compatibility, Capabilities, Operation,
 *   Short Beacon Interval). The chip only ever sees S1G beacons.
 *
 * AP mode works in our embedded port because hostap (compiled with
 * CONFIG_IEEE80211AH) builds the S1G beacon directly via the `ext_head`
 * branch in `ieee802_11_build_ap_params` (see beacon.c:2298–2326 in the
 * embedded hostap), so the chip gets a pre-formed S1G beacon and doesn't
 * need a dot11ah transform.
 *
 * Mesh mode in morselib (which doesn't have a host-side mesh hostap path)
 * was previously handing the chip a regular 802.11 beacon (FC 0x80 0x00)
 * and the chip's mesh firmware was silently rejecting it — explaining
 * every symptom we've seen:
 *   - mmdrv_host_get_beacon fires once (chip asks for template)
 *   - then silence: no TX_STATUS, no peer RX, no chip events
 *
 * ============================================================================
 * S1G BEACON LAYOUT (per IEEE 802.11-2020 §9.3.4)
 * ============================================================================
 *
 * Fixed header (15 bytes, vs 36 for regular 802.11 beacons):
 *   frame_control (2B)  = 0x001C (FTYPE_EXT=3 | STYPE_S1G_BEACON=1)
 *                                 -> wire LE bytes: 0x1C 0x00
 *   duration      (2B)  = 0x0000
 *   sa            (6B)  = source/BSSID of the mesh STA
 *   timestamp     (4B)  = 0 (chip fills on TX — 4 octets, NOT 8 like regular)
 *   change_seq    (1B)  = 0 (incrementing seqnum when beacon IEs change)
 *
 * IEs (in the order our embedded morselib expects, mirroring what hostap
 * tail-builds for an S1G AP beacon):
 *   SSID (0)                      — empty for mesh, peers ID via Mesh ID IE
 *   Mesh ID (114)                 — the mesh_id bytes
 *   Mesh Configuration (113, 7B)  — path/metric/sync/auth + caps
 *   S1G Beacon Compatibility (213, 8B) — cap_info, beacon_int, tsf_msb (chip)
 *   S1G Capabilities (217, 15B)   — via ie_s1g_capabilities_build
 *   S1G Operation (232, 6B)       — channel width/op_class/prim_chan/freq
 *   S1G Short Beacon Interval (214, 2B) — short_beacon_int
 *
 * NOT included (per S1G beacon spec — see morse_dot11ah_mask_ies in
 * /tmp/morse_driver/dot11ah/ie.c:502):
 *   - Supported Rates (1) — not used in S1G PHY
 *   - DS Params (3)       — irrelevant for sub-GHz
 *   - ERP Info (42), HT Cap/Op, VHT Cap/Op — all 11n/ac, masked for S1G
 *
 * If this finally unlocks mesh RX, we know the embedded port needs to live
 * with a parallel dot11ah-equivalent for any other mgmt frames mesh sends
 * (peering open/confirm/close action frames, etc.) — those also need to be
 * S1G-formatted before reaching the chip.
 * ============================================================================
 */

/* Lift log level to INF so we can see beacon-init diagnostics. ERR-only would
 * blackhole the bring-up trace. Same trick used in umac_mesh.c. */
#define MMLOG_LEVEL_OVRD 5

#include "common/common.h"
#include "mmlog.h"

#include "umac_mesh_beacon.h"
#include "umac/frames/frames_common.h"
#include "umac/ies/ies_common.h"
#include "umac/ies/s1g_capabilities.h"
#include "umac/ies/s1g_operation.h"
#include "umac/interface/umac_interface.h"  /* current_s1g_operation accessor */
#include "umac/rc/umac_rc.h"
#include "dot11/dot11.h"
#include "dot11/dot11_ies.h"
#include "mmdrv.h"

#include <string.h>

/* Mesh state — only one mesh VIF can exist on the MM6108. */
static bool s_initialized = false;
static struct mmwlan_mesh_args s_args;
static uint8_t s_own_addr[6];
/* Beacon change sequence — increments when IEs change. For now we never
 * change IEs during a mesh session, so it stays 0. */
static uint8_t s_change_seq = 0;

/* ---------------------------------------------------------------------------
 * Frame Control for S1G beacon
 * ---------------------------------------------------------------------------
 * FC bits (LE):
 *   protocol version = 0   (bits 0-1)
 *   type = EXT = 3         (bits 2-3) -> 0x0C
 *   subtype = S1G_BCN = 1  (bits 4-7) -> 0x10
 * Total = 0x1C, wire-order: low byte 0x1C, high byte 0x00.
 *
 * Compare to a regular 802.11 beacon (type=MGMT=0, subtype=BEACON=8 → 0x80
 * 0x00), which the embedded port was incorrectly using before this patch. */
#define MESH_S1G_BEACON_FC_LO 0x1C
#define MESH_S1G_BEACON_FC_HI 0x00

/* Mesh Configuration IE — see field layout in S1G beacon header above and
 * IEEE 802.11-2020 §9.4.2.97. 7 bytes fixed payload. */
#define MESH_CFG_IE_LEN 7
#define MESH_PATH_PROTO_NONE  0x00
#define MESH_PATH_METRIC_NONE 0x00
#define MESH_CONGESTION_NONE  0x00
#define MESH_SYNC_NEIGHBOR    0x01
#define MESH_AUTH_NONE        0x00
#define MESH_AUTH_SAE         0x01
#define MESH_FORMATION_INFO   0x00
/* Mesh Capability: bits 0 (accepting peerings) + 3 (forwarding) = 0x09 */
#define MESH_CAPABILITY       0x09

void umac_mesh_beacon_init(const struct mmwlan_mesh_args *args, const uint8_t own_addr[6])
{
    if (args == NULL || own_addr == NULL)
    {
        MMLOG_ERR("umac_mesh_beacon_init: NULL args or own_addr\n");
        return;
    }
    memcpy(&s_args, args, sizeof(s_args));
    memcpy(s_own_addr, own_addr, 6);
    s_change_seq = 0;
    s_initialized = true;
    MMLOG_INF("mesh beacon: init OK (S1G fmt) mesh_id_len=%u "
              "own_addr=%02x:%02x:%02x:%02x:%02x:%02x\n",
              args->mesh_id_len,
              own_addr[0], own_addr[1], own_addr[2],
              own_addr[3], own_addr[4], own_addr[5]);
}

bool umac_mesh_beacon_is_active(void)
{
    return s_initialized;
}

/* ---------------------------------------------------------------------------
 * Per-IE builders. Each appends one complete IE (header + payload) to buf.
 * --------------------------------------------------------------------------- */

/* SSID IE (0). Empty payload for mesh — peers ID via Mesh ID IE. */
static void append_ssid_ie_empty_(struct consbuf *buf)
{
    uint8_t ie[2] = { DOT11_IE_SSID, 0 };
    consbuf_append(buf, ie, sizeof(ie));
}

/* Mesh ID IE (114). Payload = mesh_id bytes (length = s_args.mesh_id_len). */
static void append_mesh_id_ie_(struct consbuf *buf)
{
    uint8_t hdr[2] = { DOT11_IE_MESH_ID, s_args.mesh_id_len };
    consbuf_append(buf, hdr, sizeof(hdr));
    consbuf_append(buf, s_args.mesh_id, s_args.mesh_id_len);
}

/* Mesh Configuration IE (113, 7B). See header comment for field meaning. */
static void append_mesh_config_ie_(struct consbuf *buf)
{
    uint8_t ie[2 + MESH_CFG_IE_LEN] = {
        DOT11_IE_MESH_CONFIGURATION, MESH_CFG_IE_LEN,
        MESH_PATH_PROTO_NONE,
        MESH_PATH_METRIC_NONE,
        MESH_CONGESTION_NONE,
        MESH_SYNC_NEIGHBOR,
        (s_args.security_type == MMWLAN_SAE) ? MESH_AUTH_SAE : MESH_AUTH_NONE,
        MESH_FORMATION_INFO,
        MESH_CAPABILITY,
    };
    consbuf_append(buf, ie, sizeof(ie));
}

/* S1G Beacon Compatibility IE (213, 8B payload).
 * compat_info  = capability info (16-bit). Mesh STAs are neither ESS nor
 *                IBSS — only Privacy bit (4) gets set if security is SAE.
 * beacon_int   = beacon interval in TUs.
 * tsf_completion = 4-octet MSB of TSF, chip fills on TX (0 here is OK).
 *
 * Layout matches hostap's hostapd_eid_s1g_beacon_compat in
 * src/hostap/src/ap/ieee802_11_s1g.c:113 and Linux dot11ah's
 * morse_dot11ah_insert_s1g_compatibility (tx_11n_to_s1g.c:58). */
static void append_s1g_bcn_compat_ie_(struct consbuf *buf)
{
    uint16_t cap_info = (s_args.security_type == MMWLAN_SAE) ? 0x0010 : 0x0000;
    uint16_t bcn_int = s_args.beacon_interval_tu ? s_args.beacon_interval_tu : 100;

    uint8_t ie[2 + 8] = {
        DOT11_IE_S1G_BEACON_COMPATIBILITY, 8,
        (uint8_t)(cap_info & 0xff), (uint8_t)(cap_info >> 8),
        (uint8_t)(bcn_int  & 0xff), (uint8_t)(bcn_int  >> 8),
        0, 0, 0, 0,  /* tsf_completion — chip fills */
    };
    consbuf_append(buf, ie, sizeof(ie));
}

/* S1G Operation IE (232, 6B payload). Derived from the chip's current
 * channel config (already set by umac_interface_set_channel_from_regdb
 * before mesh beaconing starts — see umac_mesh.c).
 *
 * Byte layout (per dot11_ie_s1g_operation in dot11/dot11_ies.h:143):
 *   channel_width        — bitfield: bit0=pri 1MHz, bits1-4=(op_bw-1),
 *                                    bit5=pri 1MHz loc, bit7=no-MCS10
 *   operating_class      — global operating class
 *   primary_channel_number
 *   channel_center_freq  — operating channel number
 *   basic_s1g_mcs_nss_set[2] = {0xC4, 0xCC}  (MCS 4 for 1SS, per Linux default) */
static void append_s1g_operation_ie_(struct umac_data *umacd, struct consbuf *buf)
{
    const struct ie_s1g_operation *cur =
        umac_interface_get_current_s1g_operation_info(umacd);

    uint8_t op_bw_mhz   = cur ? cur->operation_channel_width_mhz : 2;
    uint8_t pri_bw_mhz  = cur ? cur->primary_channel_width_mhz   : 2;
    uint8_t pri_1m_loc  = cur ? cur->primary_1mhz_channel_loc    : 0;
    uint8_t op_class    = cur ? cur->operating_class             : 0;
    uint8_t pri_chnum   = cur ? cur->primary_channel_number      : 0;
    uint8_t op_ch_idx   = cur ? cur->operating_channel_index     : 0;

    /* Build channel_width byte:
     *   bit 0: pri chan width — 0 if pri=2MHz, 1 if pri=1MHz
     *   bits 1-4: op chan width = op_bw_mhz - 1  (1MHz->0, 2MHz->1, 4MHz->3,
     *                                              8MHz->7, 16MHz->15)
     *   bit 5: pri 1MHz channel location
     *   bit 7: 0 (we don't recommend no-MCS10) */
    uint8_t ch_width = 0;
    if (pri_bw_mhz == 1)
    {
        ch_width |= DOT11_MASK_S1G_OP_CHAN_WIDTH_PRI_CHAN_WIDTH;
    }
    uint8_t op_bw_field = (op_bw_mhz > 0) ? (uint8_t)(op_bw_mhz - 1) : 0;
    ch_width |= (uint8_t)((op_bw_field << DOT11_SHIFT_S1G_OP_CHAN_WIDTH_OP_CHAN_WIDTH)
                          & DOT11_MASK_S1G_OP_CHAN_WIDTH_OP_CHAN_WIDTH);
    if (pri_1m_loc)
    {
        ch_width |= DOT11_MASK_S1G_OP_CHAN_WIDTH_PRI_CHAN_LOC;
    }

    uint8_t ie[2 + 6] = {
        DOT11_IE_S1G_OPERATION, 6,
        ch_width,
        op_class,
        pri_chnum,
        op_ch_idx,
        /* basic_s1g_mcs_nss_set: 1SS MCS 4 baseline.
         * Matches Linux dot11ah default in tx_11n_to_s1g.c:177
         *   u8 s1g_mcs_and_nss_set[] = {0xCC, 0xC4};
         * but stored on the wire as LSB-first: {0xC4, 0xCC}.
         * Wire byte layout: see hostapd_eid_s1g_oper. */
        0xC4, 0xCC,
    };
    consbuf_append(buf, ie, sizeof(ie));
}

/* S1G Short Beacon Interval IE (214, 2B). */
static void append_s1g_short_bcn_int_ie_(struct consbuf *buf)
{
    uint16_t bcn_int = s_args.beacon_interval_tu ? s_args.beacon_interval_tu : 100;
    uint8_t ie[2 + 2] = {
        DOT11_IE_SHORT_BCN_INT, 2,
        (uint8_t)(bcn_int & 0xff), (uint8_t)(bcn_int >> 8),
    };
    consbuf_append(buf, ie, sizeof(ie));
}

/* ---------------------------------------------------------------------------
 * Top-level beacon builder — called twice by build_mgmt_frame (size pass,
 * then fill pass). consbuf_append in size-pass-mode only tracks offset.
 * --------------------------------------------------------------------------- */
static void umac_mesh_build_beacon_(struct umac_data *umacd, struct consbuf *buf, void *params)
{
    MM_UNUSED(params);

    /* --- S1G beacon fixed header (15 bytes) ---
     *   frame_control (2)   = 0x001C  (FTYPE_EXT | STYPE_S1G_BEACON)
     *   duration      (2)   = 0
     *   sa            (6)   = own_addr (mesh STA MAC)
     *   timestamp     (4)   = 0  (chip fills on TX — only 4 octets, not 8)
     *   change_seq    (1)   = monotonic seqnum, bumps when IEs change */
    uint8_t header[2 + 2 + 6 + 4 + 1] = {
        MESH_S1G_BEACON_FC_LO, MESH_S1G_BEACON_FC_HI,  /* frame_control */
        0x00, 0x00,                                     /* duration */
        0, 0, 0, 0, 0, 0,                               /* sa[6] — fill below */
        0, 0, 0, 0,                                     /* timestamp[4] */
        0,                                              /* change_seq */
    };
    memcpy(&header[4], s_own_addr, 6);
    header[14] = s_change_seq;
    consbuf_append(buf, header, sizeof(header));

    /* --- IEs --- */
    append_ssid_ie_empty_(buf);
    append_mesh_id_ie_(buf);
    append_mesh_config_ie_(buf);
    append_s1g_bcn_compat_ie_(buf);
    /* S1G Capabilities IE (217) — embedded morselib provides a builder that
     * pulls from the umac's runtime capability state (set during driver init).
     * Use the _ap variant since we're acting as a beacon source. */
    ie_s1g_capabilities_build_ap(umacd, buf);
    append_s1g_operation_ie_(umacd, buf);
    append_s1g_short_bcn_int_ie_(buf);
}

struct mmpkt *umac_mesh_get_beacon(struct umac_data *umacd)
{
    if (!s_initialized)
    {
        MMLOG_WRN("mesh beacon: get_beacon called before init\n");
        return NULL;
    }

    struct mmpkt *beacon = build_mgmt_frame(umacd, umac_mesh_build_beacon_, NULL);
    if (beacon == NULL)
    {
        MMLOG_ERR("mesh beacon: alloc failed\n");
        return NULL;
    }

    /* Tag TX metadata so the chip routes this to the beacon TX path. Same
     * fields umac_ap_get_beacon sets. vif_id=0 since mesh is single-VIF. */
    struct mmdrv_tx_metadata *tx_metadata = mmdrv_get_tx_metadata(beacon);
    tx_metadata->flags = MMDRV_TX_FLAG_IMMEDIATE_REPORT;
    tx_metadata->tid = MMWLAN_MAX_QOS_TID;
    tx_metadata->vif_id = 0;

    /* Populate the rate-control table for the mgmt TX path. Without this the
     * chip doesn't know what S1G-MCS rate to use for this beacon. */
    umac_rc_init_rate_table_mgmt(umacd, &tx_metadata->rc_data, false);

    MMLOG_INF("mesh beacon: S1G fmt built (FC 0x%02x%02x) — len-tagged for chip TX\n",
              MESH_S1G_BEACON_FC_HI, MESH_S1G_BEACON_FC_LO);
    return beacon;
}
