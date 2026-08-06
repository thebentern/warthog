/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 802.11s MPM frame serialization / deserialization — Warthog softmac port.
 * See mesh_mpm_frame.h for the design rationale and the link-id perspective
 * note. Ported from kernel net/mac80211/mesh_plink.c v6.6 (mesh_plink_frame_tx
 * frame layout + mesh_rx_plink_frame / mesh_process_plink_frame parsing).
 */
#include "mesh/mesh_mpm_frame.h"

#include <linux/ieee80211.h>      /* WLAN_*, IEEE80211_{F,S}TYPE_* */
#include <linux/etherdevice.h>    /* ether_addr_copy, is_multicast_ether_addr */
#include <asm/unaligned.h>        /* get/put_unaligned_le16 */
#include <string.h>

/* 802.11 management header is 24 bytes; then category(1) + action_code(1). */
#define MGMT_HDR_LEN      24
#define MPM_FIXED_LEN     (MGMT_HDR_LEN + 2)  /* through action_code */

/* Offsets within the management header. */
#define OFF_FRAME_CONTROL 0
#define OFF_DURATION      2
#define OFF_DA            4
#define OFF_SA            10
#define OFF_BSSID         16
#define OFF_SEQ           22
#define OFF_CATEGORY      24
#define OFF_ACTION_CODE   25

static u8 *put_ie(u8 *p, u8 eid, u8 len, const u8 *body)
{
    *p++ = eid;
    *p++ = len;
    if (len)
        memcpy(p, body, len);
    return p + len;
}

size_t mpm_build_frame(u8 *buf, size_t cap,
                       const u8 *da, const u8 *sa,
                       u8 action, u16 our_llid, u16 our_plid,
                       u16 reason, u16 aid,
                       const u8 *mesh_id, u8 mesh_id_len)
{
    u8 *p = buf;
    u8 ie_len = 4;            /* peering_proto(2) + llid(2) */
    bool include_plid = false;

    if (cap < MPM_FRAME_MAX || mesh_id_len > 32)
        return 0;
    if (action != WLAN_SP_MESH_PEERING_OPEN &&
        action != WLAN_SP_MESH_PEERING_CONFIRM &&
        action != WLAN_SP_MESH_PEERING_CLOSE)
        return 0;

    /* --- management header --- */
    memset(p, 0, MGMT_HDR_LEN);
    put_unaligned_le16(IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ACTION,
                       p + OFF_FRAME_CONTROL);
    memcpy(p + OFF_DA,    da, ETH_ALEN);
    memcpy(p + OFF_SA,    sa, ETH_ALEN);
    memcpy(p + OFF_BSSID, sa, ETH_ALEN);   /* mesh: BSSID = our own addr */
    p[OFF_CATEGORY]    = WLAN_CATEGORY_SELF_PROTECTED;
    p[OFF_ACTION_CODE] = action;
    p += MPM_FIXED_LEN;

    /* --- fixed fields + Mesh ID, for non-Close --- */
    if (action != WLAN_SP_MESH_PEERING_CLOSE) {
        put_unaligned_le16(0, p); p += 2;          /* capability info */
        if (action == WLAN_SP_MESH_PEERING_CONFIRM) {
            put_unaligned_le16(aid, p); p += 2;    /* AID */
        }
        p = put_ie(p, WLAN_EID_MESH_ID, mesh_id_len, mesh_id);
    } else {
        p = put_ie(p, WLAN_EID_MESH_ID, mesh_id_len, mesh_id);
    }

    /* --- Peer Management IE length per action (matches the kernel) --- */
    switch (action) {
    case WLAN_SP_MESH_PEERING_OPEN:
        break;                                     /* proto + llid */
    case WLAN_SP_MESH_PEERING_CONFIRM:
        ie_len += 2; include_plid = true;          /* + plid */
        break;
    case WLAN_SP_MESH_PEERING_CLOSE:
        if (our_plid) { ie_len += 2; include_plid = true; }
        ie_len += 2;                               /* + reason */
        break;
    }

    *p++ = WLAN_EID_PEER_MGMT;
    *p++ = ie_len;
    put_unaligned_le16(0, p); p += 2;              /* peering protocol = 0 */
    put_unaligned_le16(our_llid, p); p += 2;
    if (include_plid) { put_unaligned_le16(our_plid, p); p += 2; }
    if (action == WLAN_SP_MESH_PEERING_CLOSE) {
        put_unaligned_le16(reason, p); p += 2;
    }

    return (size_t)(p - buf);
}

bool mpm_parse_frame(const u8 *buf, size_t len, struct mpm_frame *out)
{
    const u8 *ies, *end, *peering = NULL;
    u8 peering_len = 0;
    u16 fc;

    memset(out, 0, sizeof(*out));
    if (len < MPM_FIXED_LEN)
        return false;

    /* Must be a management Action frame. */
    fc = get_unaligned_le16(buf + OFF_FRAME_CONTROL);
    if ((fc & (0x000c /*type*/ | 0x00f0 /*subtype*/)) !=
        (IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ACTION))
        return false;
    if (buf[OFF_CATEGORY] != WLAN_CATEGORY_SELF_PROTECTED)
        return false;

    out->action = buf[OFF_ACTION_CODE];
    if (out->action != WLAN_SP_MESH_PEERING_OPEN &&
        out->action != WLAN_SP_MESH_PEERING_CONFIRM &&
        out->action != WLAN_SP_MESH_PEERING_CLOSE)
        return false;
    memcpy(out->da, buf + OFF_DA, ETH_ALEN);
    memcpy(out->sa, buf + OFF_SA, ETH_ALEN);

    /* Skip the fixed fields the build side emitted, then walk the IEs:
     *   OPEN    -> capability(2)
     *   CONFIRM -> capability(2) + AID(2)
     *   CLOSE   -> (none)
     */
    ies = buf + MPM_FIXED_LEN;
    end = buf + len;
    if (out->action == WLAN_SP_MESH_PEERING_OPEN) {
        if (ies + 2 > end) return false;
        ies += 2;
    } else if (out->action == WLAN_SP_MESH_PEERING_CONFIRM) {
        if (ies + 4 > end) return false;
        out->aid = get_unaligned_le16(ies + 2);
        ies += 4;
    }

    /* Walk the IE TLVs (bounds-checked). */
    while (ies + 2 <= end) {
        u8 eid = ies[0], elen = ies[1];
        if (ies + 2 + elen > end)
            return false;            /* malformed */
        if (eid == WLAN_EID_MESH_ID) {
            out->mesh_id = ies + 2;
            out->mesh_id_len = elen;
        } else if (eid == WLAN_EID_PEER_MGMT) {
            peering = ies + 2;
            peering_len = elen;
        }
        ies += 2 + elen;
    }

    if (!peering)
        return false;
    /* Validate the Peer Management IE length for the action (kernel rule). */
    if ((out->action == WLAN_SP_MESH_PEERING_OPEN    && peering_len != 4) ||
        (out->action == WLAN_SP_MESH_PEERING_CONFIRM && peering_len != 6) ||
        (out->action == WLAN_SP_MESH_PEERING_CLOSE   &&
         peering_len != 6 && peering_len != 8))
        return false;
    out->peer_mgmt_ie_len = peering_len;

    /* peering = [proto(2)][llid(2)][plid(2)?][reason(2)?]. The frame's llid is
     * THIS host's plid; the frame's plid is our llid echoed back. */
    out->plid = get_unaligned_le16(peering + 2);   /* sender's llid -> our plid */
    if (out->action == WLAN_SP_MESH_PEERING_CONFIRM ||
        (out->action == WLAN_SP_MESH_PEERING_CLOSE && peering_len == 8))
        out->llid = get_unaligned_le16(peering + 4); /* sender's plid -> our llid */
    if (out->action == WLAN_SP_MESH_PEERING_CLOSE)
        out->reason = get_unaligned_le16(peering + peering_len - 2);

    out->valid = true;
    return true;
}
