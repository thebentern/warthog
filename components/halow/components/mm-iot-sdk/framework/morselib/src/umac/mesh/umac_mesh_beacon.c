/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * S1G beacon construction and the shared mesh discovery IEs.
 *
 * Two consumers, deliberately sharing their IE values:
 *   - umac_mesh_get_beacon(), the template the chip asks the host for
 *   - umac_mesh_build_discovery_ies(), the flat IE blob used by mesh probe
 *     responses and by every Mesh Peering (MPM) action frame
 * They must agree: a peer runs mac80211's mesh_matches_local() over the Mesh
 * ID and Mesh Configuration in whichever frame it sees first, and disagreement
 * between our beacon and our peering frames would make peering succeed or fail
 * depending on arrival order.
 *
 * Beaconing: the MM6108 firmware fires the beacon IRQ once for a mesh VIF and
 * never re-arms its TBTT (the MM8108 self-schedules; this one does not). A host
 * beacon timer (morse_beacon_start / beacon.c) re-drives this template at the
 * beacon interval, and the chip transmits each one -- AT+BCNSTAT? shows
 * served == txcomp climbing together. So the S1G beacon below IS on air, and
 * beacon-driven peers (mac80211 / OpenMANET) can discover the node from it.
 *
 * S1G short beacon layout (IEEE 802.11-2020 s9.3.4), 15-byte fixed header:
 *   frame_control(2) duration(2) sa(6) timestamp(4) change_seq(1)
 * then SSID(0, empty for mesh), Mesh ID(114), Mesh Configuration(113),
 * S1G Beacon Compatibility(213), S1G Capabilities(217), S1G Operation(232),
 * S1G Short Beacon Interval(214). Supported Rates / DS Params / ERP / HT / VHT
 * are masked for S1G -- but note the discovery IE blob DOES carry Supported
 * Rates, because mesh_matches_local() compares the basic rate set.
 */

/* Lift log level to INF so we can see beacon-init diagnostics. ERR-only would
 * blackhole the bring-up trace. Same trick used in umac_mesh.c. */
#define MMLOG_LEVEL_OVRD 5

#include "common/common.h"
#include "mmlog.h"

#include "umac_mesh_beacon.h"
#include "umac_mesh_ies.h"
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

/* umac_mesh_ies.h must stay freestanding (the host tests link it with libc
 * only), so it re-declares the element IDs instead of including dot11.h. This
 * file sees both headers, which makes it the one place the two can be pinned
 * together. If the SDK ever renumbers these, fail the build here rather than
 * on air, where the symptom is a peer silently declining to peer. */
MM_STATIC_ASSERT(UMAC_MESH_EID_MESH_ID == DOT11_IE_MESH_ID, "mesh IE id drift");
MM_STATIC_ASSERT(UMAC_MESH_EID_MESH_CONFIG == DOT11_IE_MESH_CONFIGURATION, "mesh IE id drift");
MM_STATIC_ASSERT(UMAC_MESH_IES_MESH_ID_MAXLEN == MMWLAN_MESH_ID_MAXLEN, "mesh id maxlen drift");

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
/* S1G short-beacon frame control: version 0, type EXT(3), subtype
 * S1G_BEACON(1) => low byte 0x1C, high byte 0x00.
 *
 * UNTESTED on air: this chip does not beacon in mesh mode at all (the beacon
 * IRQ fires once at startup and never again -- see AT+BCNSTAT?), so no value
 * here has ever been transmitted. 0x00 matches the in-tree hostap S1G builder,
 * which emits bss_bw=0 at both 1 and 2 MHz on the AP path that does work.
 * Note the upper octet is the BSS-BW subfield (bits 11-13), NOT an
 * optional-field presence flag -- the presence bits are 0x0100/0x0200/0x0400. */
#define MESH_S1G_BEACON_FC_LO 0x1C
#define MESH_S1G_BEACON_FC_HI 0x00

/* Mesh Configuration IE — see field layout in S1G beacon header above and
 * IEEE 802.11-2020 §9.4.2.97. 7 bytes fixed payload. */
#define MESH_CFG_IE_LEN 7
/* 0x01/0x01 is correct FOR THIS PEER. Measured, not reasoned.
 *
 * The kernel's generic constants say otherwise -- Linux include/linux/ieee80211.h
 * has IEEE80211_PATH_PROTOCOL_HWMP = 0 and IEEE80211_PATH_METRIC_AIRTIME = 0,
 * and mac80211's mesh_matches_local() rejects a candidate whose meshconf_psel /
 * meshconf_pmetric differ from ifmsh->mesh_pp_id / mesh_pm_id. That argues for
 * 0x00/0x00, and it was tried on hardware. Result:
 *
 *   0x01/0x01 -> peer creates a candidate, sends Open, reaches OPN_RCVD
 *   0x00/0x00 -> AT+PRSPSTAT? req_rx=16 rsp_tx=16 but AT+MPMSTAT? rx=0:
 *                the peer probes us repeatedly and NEVER initiates peering
 *
 * So the morse driver runs with mesh_pp_id / mesh_pm_id = 1, not the kernel
 * defaults, and 0x00 is what fails mesh_matches_local() here. Do not "correct"
 * these to 0 on the strength of the kernel headers alone -- verify against the
 * peer. Set WARTHOG_MESH_PATHSEL_ZERO to re-test 0x00/0x00. */
#ifdef WARTHOG_MESH_PATHSEL_ZERO
#define MESH_PATH_PROTO_ID  0x00
#define MESH_PATH_METRIC_ID 0x00
#else
#define MESH_PATH_PROTO_ID  0x01  /* matches this peer's mesh_pp_id */
#define MESH_PATH_METRIC_ID 0x01  /* matches this peer's mesh_pm_id */
#endif
#define MESH_CONGESTION_NONE  0x00
#define MESH_SYNC_NEIGHBOR    0x01
#define MESH_AUTH_NONE        0x00
#define MESH_AUTH_SAE         0x01
#define MESH_FORMATION_INFO   0x00

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
    /* Same builder the probe-response / MPM blob uses. Hand-serializing these
     * seven octets twice is how the beacon and the peering frames drift apart,
     * and a peer runs mesh_matches_local() over whichever it sees first. */
    uint8_t ie[2 + UMAC_MESH_CFG_IE_LEN];
    uint16_t n = umac_mesh_ies_build_mesh_config(ie, (uint16_t)sizeof(ie),
                                                 s_args.security_type == MMWLAN_SAE);
    consbuf_append(buf, ie, n);
}

/* Serialize the IEs a peer needs to accept us as a mesh candidate, into a flat
 * buffer (probe responses take a pre-built IE blob, not a consbuf).
 *
 * Deliberately emits the SAME values as the beacon path above -- 802.11s
 * mesh_matches_local() compares Mesh ID plus the path-selection protocol,
 * metric, congestion control, sync method and auth protocol fields, so a probe
 * response that disagreed with our beacon would be rejected as a candidate.
 *
 * SSID is NOT included here: frame_probe_response_build takes ssid/ssid_len
 * separately, and mesh STAs advertise a zero-length SSID.
 *
 * Returns bytes written, or 0 if mesh is inactive or the buffer is too small. */
uint16_t umac_mesh_build_discovery_ies(uint8_t *out, uint16_t out_len)
{
    if (!s_initialized)
    {
        return 0;
    }
    return umac_mesh_ies_build_discovery(out, out_len, s_args.mesh_id, s_args.mesh_id_len,
                                         s_args.security_type == MMWLAN_SAE);
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

    /* Linux dot11ah peers derive their converted (legacy) channel from the
     * 1MHz primary in this element, and mac80211's mesh_matches_local()
     * requires candidates to share that channel. Our channel config uses a
     * 2MHz-wide primary equal to the operating channel; converted, that lands
     * one legacy channel above where a Linux mesh on the same S1G channel
     * sits, so our beacons are silently ignored. Advertise the Linux
     * convention instead: 1MHz primary on the lower subchannel (index 0).
     * TX itself is unchanged -- the whole 2MHz channel is shared either way. */
    if (op_bw_mhz == 2 && pri_bw_mhz == 2 && pri_chnum == op_ch_idx && pri_chnum > 0)
    {
        pri_bw_mhz = 1;
        pri_chnum = (uint8_t)(op_ch_idx - 1);
        pri_1m_loc = 0;
    }

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
    {
        extern volatile uint8_t g_warthog_bcn_own[160];
        extern volatile uint16_t g_warthog_bcn_own_len;
        struct mmpktview *v = mmpkt_open(beacon);
        const uint8_t *d = (const uint8_t *)mmpkt_get_data_start(v);
        uint32_t l = mmpkt_get_data_length(v);
        if (l > 160) l = 160;
        memcpy((void *)g_warthog_bcn_own, d, l);
        g_warthog_bcn_own_len = (uint16_t)mmpkt_get_data_length(v);
        mmpkt_close(&v);
    }
    return beacon;
}
