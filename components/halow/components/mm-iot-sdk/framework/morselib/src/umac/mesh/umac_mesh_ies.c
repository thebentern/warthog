/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * Pure 802.11s mesh byte layout. See umac_mesh_ies.h. Freestanding by design --
 * do not add SDK includes here, or the host tests stop covering the shipping
 * code (enforced by the `freestanding` target in the test Makefile).
 */

#include "umac_mesh_ies.h"

#include <string.h>

/* Mesh Configuration IE payload values.
 *
 * Path selection protocol / metric 0x01/0x01 are the 802.11 HWMP and airtime
 * IDs (Linux IEEE80211_PATH_PROTOCOL_HWMP = IEEE80211_PATH_METRIC_AIRTIME = 1).
 * mesh_matches_local() rejects other values: with 0x00/0x00 the peer never initiated. */
#define MESH_PATH_PROTO_ID 0x01  /* HWMP */
#define MESH_PATH_METRIC_ID 0x01 /* Airtime */
#define MESH_CONGESTION_NONE 0x00
#define MESH_SYNC_NEIGHBOR 0x01
#define MESH_AUTH_NONE 0x00
#define MESH_AUTH_SAE 0x01
/* Formation Info carries Number of Peerings in bits 1..6, capped at 63 as
 * mac80211 does; Connected to Gate/AS stay 0. */
#define MESH_FORMATION_PEERINGS_MAX 63u
/* Accepting Additional Mesh Peerings (bit 0) while the peer table has room,
 * plus Forwarding (bit 3) only when the relay is on. Advertising forwarding
 * invites a mac80211 peer to route through us; with the relay off that is a
 * blackhole, so the bit follows the runtime gate exactly. */
#define MESH_CAPABILITY_ACCEPTING 0x01
#define MESH_CAPABILITY_FORWARDING 0x08
uint8_t umac_mesh_ies_cap_forwarding = 0;
void (*umac_mesh_ies_capacity_fn)(struct umac_mesh_ies_capacity *out) = NULL;

/* Supported Rates, mandatory 5 GHz OFDM set. dot11ah presents S1G as 5 GHz, so
 * this is the set a peer expects. Units are 500 kbps; the MSB marks a rate
 * BASIC. 6/12/24 Mbps are the band's mandatory basic rates, and it is precisely
 * this basic subset that mesh_matches_local() compares. */
static const uint8_t k_supported_rates[] = {
    0x8C, /*  6 Mbps basic */
    0x12, /*  9 Mbps       */
    0x98, /* 12 Mbps basic */
    0x24, /* 18 Mbps       */
    0xB0, /* 24 Mbps basic */
    0x48, /* 36 Mbps       */
    0x60, /* 48 Mbps       */
    0x6C, /* 54 Mbps       */
};

const uint8_t *umac_mesh_ies_supported_rates(uint8_t *len_out)
{
    if (len_out != NULL)
    {
        *len_out = (uint8_t)sizeof(k_supported_rates);
    }
    return k_supported_rates;
}

uint16_t umac_mesh_ies_build_mesh_config(uint8_t *out, uint16_t out_len, bool sae)
{
    if (out == NULL || out_len < 2 + UMAC_MESH_CFG_IE_LEN)
    {
        return 0;
    }

    uint16_t n = 0;
    out[n++] = UMAC_MESH_EID_MESH_CONFIG;
    out[n++] = UMAC_MESH_CFG_IE_LEN;
    out[n++] = MESH_PATH_PROTO_ID;
    out[n++] = MESH_PATH_METRIC_ID;
    out[n++] = MESH_CONGESTION_NONE;
    out[n++] = MESH_SYNC_NEIGHBOR;
    out[n++] = sae ? MESH_AUTH_SAE : MESH_AUTH_NONE;

    struct umac_mesh_ies_capacity cap = { .accepting = true, .peerings = 0 };
    if (umac_mesh_ies_capacity_fn != NULL)
    {
        umac_mesh_ies_capacity_fn(&cap);
    }
    uint8_t peerings = (cap.peerings > MESH_FORMATION_PEERINGS_MAX) ? MESH_FORMATION_PEERINGS_MAX
                                                                    : cap.peerings;
    out[n++] = (uint8_t)(peerings << 1);
    out[n++] = (uint8_t)((cap.accepting ? MESH_CAPABILITY_ACCEPTING : 0u) |
                         (umac_mesh_ies_cap_forwarding ? MESH_CAPABILITY_FORWARDING : 0u));
    return n;
}

uint16_t umac_mesh_ies_build_discovery(uint8_t *out, uint16_t out_len, const uint8_t *mesh_id,
                                       uint8_t mesh_id_len, bool sae)
{
    if (out == NULL || mesh_id == NULL || mesh_id_len == 0 ||
        mesh_id_len > UMAC_MESH_IES_MESH_ID_MAXLEN)
    {
        return 0;
    }

    /* Check the whole blob up front so a partial write can never be handed to
     * the caller as a valid IE sequence. */
    uint16_t need = (uint16_t)(2 + sizeof(k_supported_rates) + 2 + mesh_id_len + 2 +
                               UMAC_MESH_CFG_IE_LEN);
    if (need > out_len)
    {
        return 0;
    }

    uint16_t n = 0;
    out[n++] = UMAC_MESH_EID_SUPPORTED_RATES;
    out[n++] = (uint8_t)sizeof(k_supported_rates);
    memcpy(&out[n], k_supported_rates, sizeof(k_supported_rates));
    n = (uint16_t)(n + sizeof(k_supported_rates));

    out[n++] = UMAC_MESH_EID_MESH_ID;
    out[n++] = mesh_id_len;
    memcpy(&out[n], mesh_id, mesh_id_len);
    n = (uint16_t)(n + mesh_id_len);

    n = (uint16_t)(n + umac_mesh_ies_build_mesh_config(&out[n], (uint16_t)(out_len - n), sae));
    return n;
}

bool umac_mesh_ies_rsn_valid(const uint8_t *rsn, uint16_t len)
{
    /* At least the 2-octet Version: 802.11 makes every later RSNE field optional. */
    if (rsn == NULL || len < 2u || len > UMAC_MESH_IES_RSN_MAXLEN || rsn[0] != UMAC_MESH_EID_RSN ||
        rsn[1] < 2u)
    {
        return false;
    }
    uint32_t off = 0;
    while (off < len)
    {
        if (off + 2u > len || off + 2u + rsn[off + 1] > len)
        {
            return false;
        }
        off += 2u + rsn[off + 1];
    }
    return true;
}

uint16_t umac_mesh_ies_build_probe_resp(uint8_t *out, uint16_t out_len, const uint8_t *mesh_id,
                                        uint8_t mesh_id_len, bool sae, const uint8_t *rsn,
                                        uint16_t rsn_len)
{
    if (out == NULL || mesh_id == NULL || mesh_id_len == 0 ||
        mesh_id_len > UMAC_MESH_IES_MESH_ID_MAXLEN)
    {
        return 0;
    }
    const uint16_t rsn_n = (sae && umac_mesh_ies_rsn_valid(rsn, rsn_len)) ? rsn_len : 0u;
    const uint32_t need = 2u + sizeof(k_supported_rates) + rsn_n + 2u + mesh_id_len + 2u +
                          UMAC_MESH_CFG_IE_LEN;
    if (need > out_len)
    {
        return 0;
    }

    uint16_t n = umac_mesh_ies_build_discovery(out, out_len, mesh_id, mesh_id_len, sae);
    if (n != 0u && rsn_n != 0u)
    {
        /* The standard probe-response order: Supported Rates, RSN, ..., Mesh ID. */
        const uint16_t at = (uint16_t)(2u + sizeof(k_supported_rates));
        memmove(&out[at + rsn_n], &out[at], (size_t)(n - at));
        memcpy(&out[at], rsn, rsn_n);
        n = (uint16_t)(n + rsn_n);
    }
    return n;
}

uint16_t umac_mesh_ies_build_probe_req(uint8_t *out, uint16_t out_len, const uint8_t *mesh_id,
                                       uint8_t mesh_id_len)
{
    if (out == NULL || mesh_id == NULL || mesh_id_len == 0 ||
        mesh_id_len > UMAC_MESH_IES_MESH_ID_MAXLEN || 4u + (uint32_t)mesh_id_len > out_len)
    {
        return 0;
    }
    out[0] = UMAC_MESH_EID_SSID;
    out[1] = 0; /* wildcard: a mesh STA identifies its MBSS by Mesh ID */
    out[2] = UMAC_MESH_EID_MESH_ID;
    out[3] = mesh_id_len;
    memcpy(&out[4], mesh_id, mesh_id_len);
    return (uint16_t)(4u + mesh_id_len);
}

uint16_t umac_mesh_ies_build_mpm_body(uint8_t *out, uint16_t out_len, uint8_t action,
                                      uint16_t llid, uint16_t plid, uint16_t reason, uint16_t aid,
                                      const uint8_t *mesh_id, uint8_t mesh_id_len, bool sae,
                                      const uint8_t *extra_ies, uint16_t extra_ies_len)
{
    if (out == NULL || action < UMAC_MESH_MPM_ACTION_OPEN || action > UMAC_MESH_MPM_ACTION_CLOSE)
    {
        return 0;
    }

    bool is_confirm = (action == UMAC_MESH_MPM_ACTION_CONFIRM);
    bool is_close = (action == UMAC_MESH_MPM_ACTION_CLOSE);
    /* proto(2) + llid(2) [+ plid(2)] [+ reason(2)]. Close always carries a
     * reason; it carries plid too, which is what lets the peer match the Close
     * to the link it holds rather than tearing down the wrong one. */
    uint8_t pm_ie_len = is_close ? 8 : (is_confirm ? 6 : 4);
    /* cat, action, then capability(2) for Open, capability(2)+AID(2) for
     * Confirm, and neither for Close -- find_peer_mgmt_ie_() assumes exactly
     * this and offsets its walk accordingly. */
    uint16_t fixed = (uint16_t)(is_close ? 2 : (2 + 2 + (is_confirm ? 2 : 0)));

    uint16_t ies_len = umac_mesh_ies_build_discovery(NULL, 0, mesh_id, mesh_id_len, sae);
    (void)ies_len; /* validated below against the real buffer */

    /* Upper bound before touching the buffer.
     *
     * Computed in 32 bits and compared before any narrowing. As a uint16_t the
     * sum wrapped: a large extra_ies_len made `need` come out small, the guard
     * passed, and the memcpy below then wrote past the caller's buffer while
     * every length in sight still looked sane. extra_ies is currently fed from
     * a bounded 64-byte element builder, so this was not reachable -- but it
     * is a caller-supplied length, and the guard should not depend on the
     * caller being careful. */
    uint32_t need = (uint32_t)fixed + 2u + sizeof(k_supported_rates) + 2u + mesh_id_len + 2u +
                    UMAC_MESH_CFG_IE_LEN + 2u + pm_ie_len + extra_ies_len;
    if (mesh_id == NULL || mesh_id_len == 0 || mesh_id_len > UMAC_MESH_IES_MESH_ID_MAXLEN ||
        need > (uint32_t)out_len)
    {
        return 0;
    }

    uint16_t n = 0;
    out[n++] = UMAC_MESH_MPM_CATEGORY;
    out[n++] = action;
    if (!is_close)
    {
        out[n++] = 0; /* capability info, LSB */
        out[n++] = 0;
        if (is_confirm)
        {
            /* AID must be NON-ZERO. AID 0 is reserved for the group key, and a
             * peer that assigns AIDs per link (wpa_supplicant does, via
             * hostapd_get_aid) will not accept 0 for a station. Sending 0 here
             * is what left a mac80211 peer stuck at OPN_RCVD and then
             * cancelling the peering with Close reason 52
             * (MESH-PEER-CANCELED), with every other field of the handshake
             * verified correct on air. */
            out[n++] = (uint8_t)(aid & 0xff);
            out[n++] = (uint8_t)(aid >> 8);
        }
    }

    /* Caller-supplied elements go BEFORE the discovery blob, matching where a
     * mac80211/S1G peer puts its S1G Capabilities and S1G Operation elements.
     * Used to carry S1G Capabilities: without it the Morse driver on the far
     * side logs "S1G capabilities mismatch" for our peering frames and they
     * never reach wpa_supplicant. */
    if (extra_ies != NULL && extra_ies_len != 0u)
    {
        memcpy(&out[n], extra_ies, extra_ies_len);
        n = (uint16_t)(n + extra_ies_len);
    }

    uint16_t got = umac_mesh_ies_build_discovery(&out[n], (uint16_t)(out_len - n), mesh_id,
                                                 mesh_id_len, sae);
    if (got == 0)
    {
        return 0;
    }
    n = (uint16_t)(n + got);

    out[n++] = UMAC_MESH_EID_PEER_MGMT;
    out[n++] = pm_ie_len;
    out[n++] = 0; /* peering protocol identifier = 0 (management protocol) */
    out[n++] = 0;
    out[n++] = (uint8_t)(llid & 0xff);
    out[n++] = (uint8_t)(llid >> 8);
    if (is_confirm || is_close)
    {
        out[n++] = (uint8_t)(plid & 0xff);
        out[n++] = (uint8_t)(plid >> 8);
    }
    if (is_close)
    {
        out[n++] = (uint8_t)(reason & 0xff);
        out[n++] = (uint8_t)(reason >> 8);
    }
    return n;
}

/* Walk the IE sequence and hand back the Peer Management IE.
 *
 * @p off must already be past the action-specific fixed fields, which differ:
 * OPEN has capability(2), CONFIRM has capability(2)+AID(2), CLOSE has neither.
 * Every length is attacker-controlled, so each step is bounded against @p len. */
static const uint8_t *find_peer_mgmt_ie_(const uint8_t *body, uint32_t len, uint32_t off,
                                         uint8_t *out_ilen)
{
    while (off + 2 <= len)
    {
        uint8_t id = body[off];
        uint8_t ilen = body[off + 1];
        if ((uint32_t)off + 2 + ilen > len)
        {
            return NULL; /* truncated element -- refuse rather than over-read */
        }
        if (id == UMAC_MESH_EID_PEER_MGMT)
        {
            *out_ilen = ilen;
            return &body[off + 2];
        }
        off += (uint32_t)2 + ilen;
    }
    return NULL;
}

/* Byte offset of the first IE, per action code. */
static uint32_t mpm_ie_offset_(uint8_t action)
{
    if (action == UMAC_MESH_MPM_ACTION_CONFIRM)
    {
        return 6; /* category, action, capability(2), AID(2) */
    }
    if (action == UMAC_MESH_MPM_ACTION_CLOSE)
    {
        return 2; /* category, action -- no capability field */
    }
    return 4; /* OPEN: category, action, capability(2) */
}

bool umac_mesh_ies_get_peer_llid(const uint8_t *body, uint32_t len, uint16_t *out_llid)
{
    if (body == NULL || out_llid == NULL || len < 2)
    {
        return false;
    }
    uint8_t ilen = 0;
    const uint8_t *pm = find_peer_mgmt_ie_(body, len, mpm_ie_offset_(body[1]), &ilen);
    if (pm == NULL || ilen < 4)
    {
        return false;
    }
    *out_llid = (uint16_t)(pm[2] | (pm[3] << 8));
    return true;
}

bool umac_mesh_ies_get_peer_plid(const uint8_t *body, uint32_t len, uint16_t *out_plid)
{
    if (body == NULL || out_plid == NULL || len < 2)
    {
        return false;
    }
    uint8_t ilen = 0;
    const uint8_t *pm = find_peer_mgmt_ie_(body, len, mpm_ie_offset_(body[1]), &ilen);
    if (pm == NULL || ilen < 6)
    {
        return false;
    }
    *out_plid = (uint16_t)(pm[4] | (pm[5] << 8));
    return true;
}

bool umac_mesh_ies_get_close_reason(const uint8_t *body, uint32_t len, uint16_t *out_reason)
{
    if (body == NULL || out_reason == NULL || len < 2)
    {
        return false;
    }
    uint8_t ilen = 0;
    const uint8_t *pm = find_peer_mgmt_ie_(body, len, mpm_ie_offset_(body[1]), &ilen);
    /* Reason is the last two octets. A 4-byte PM IE has no reason field, and
     * reading "the last two" there would return the llid as a reason code. */
    if (pm == NULL || ilen < 6)
    {
        return false;
    }
    *out_reason = (uint16_t)(pm[ilen - 2] | (pm[ilen - 1] << 8));
    return true;
}

/* Frame-control bits for a 4-address QoS data frame. Kept as literals rather
 * than dot11.h constants so this TU stays freestanding; the datapath
 * static-asserts they agree with the SDK's definitions. */
#define FC_TYPE_DATA 2
#define FC_SUBTYPE_QOS_DATA 8
#define FC_TO_DS 0x0100
#define FC_FROM_DS 0x0200

uint16_t umac_mesh_ies_build_data_hdr4(uint8_t out[UMAC_MESH_DATA_HDR4_LEN], const uint8_t *ra,
                                       const uint8_t *ta, const uint8_t *da, const uint8_t *sa)
{
    if (out == NULL || ra == NULL || ta == NULL || da == NULL || sa == NULL)
    {
        return 0;
    }
    uint16_t fc = (uint16_t)((FC_TYPE_DATA << 2) | (FC_SUBTYPE_QOS_DATA << 4) | FC_TO_DS |
                             FC_FROM_DS);
    out[0] = (uint8_t)(fc & 0xff);
    out[1] = (uint8_t)(fc >> 8);
    out[2] = 0; /* duration */
    out[3] = 0;
    memcpy(&out[4], ra, 6);
    memcpy(&out[10], ta, 6);
    memcpy(&out[16], da, 6);
    out[22] = 0; /* sequence control -- datapath fills it */
    out[23] = 0;
    memcpy(&out[24], sa, 6);
    return UMAC_MESH_DATA_HDR4_LEN;
}

uint16_t umac_mesh_ies_build_data_hdr3_group(uint8_t out[UMAC_MESH_DATA_HDR3_LEN],
                                             const uint8_t *da, const uint8_t *ta,
                                             const uint8_t *sa)
{
    if (out == NULL || da == NULL || ta == NULL || sa == NULL)
    {
        return 0;
    }
    uint16_t fc = (uint16_t)((FC_TYPE_DATA << 2) | (FC_SUBTYPE_QOS_DATA << 4) | FC_FROM_DS);
    out[0] = (uint8_t)(fc & 0xff);
    out[1] = (uint8_t)(fc >> 8);
    out[2] = 0; /* duration */
    out[3] = 0;
    memcpy(&out[4], da, 6);  /* addr1 = group DA */
    memcpy(&out[10], ta, 6); /* addr2 = TA (us)  */
    memcpy(&out[16], sa, 6); /* addr3 = SA       */
    out[22] = 0; /* sequence control -- datapath fills it */
    out[23] = 0;
    return UMAC_MESH_DATA_HDR3_LEN;
}

/** Fixed fields ahead of the IEs in a beacon / probe-response body. */
#define UMAC_MESH_BEACON_FIXED_LEN 12

bool umac_mesh_ies_beacon_has_mesh_id(const uint8_t *body, uint32_t len, const uint8_t *mesh_id,
                                      uint8_t mesh_id_len)
{
    if (body == NULL || mesh_id == NULL || mesh_id_len == 0 ||
        len < (uint32_t)UMAC_MESH_BEACON_FIXED_LEN)
    {
        return false;
    }

    uint32_t i = UMAC_MESH_BEACON_FIXED_LEN;
    while (i + 2u <= len)
    {
        uint8_t eid = body[i];
        uint8_t elen = body[i + 1];
        if (i + 2u + (uint32_t)elen > len)
        {
            return false; /* truncated element -- do not read past the buffer */
        }
        if (eid == UMAC_MESH_EID_MESH_ID)
        {
            return elen == mesh_id_len && memcmp(&body[i + 2], mesh_id, mesh_id_len) == 0;
        }
        i += 2u + (uint32_t)elen;
    }
    return false;
}

/* Walk elements from @p off looking for the Mesh ID. Shared by the legacy and
 * S1G beacon paths; every length octet is attacker-controlled. */
static bool mesh_id_in_elements_(const uint8_t *buf, uint32_t len, uint32_t off,
                                 const uint8_t *mesh_id, uint8_t mesh_id_len)
{
    while (off + 2u <= len)
    {
        uint8_t eid = buf[off];
        uint8_t elen = buf[off + 1];
        if (off + 2u + (uint32_t)elen > len)
        {
            return false; /* truncated element -- do not read past the buffer */
        }
        if (eid == UMAC_MESH_EID_MESH_ID)
        {
            return elen == mesh_id_len && memcmp(&buf[off + 2], mesh_id, mesh_id_len) == 0;
        }
        off += 2u + (uint32_t)elen;
    }
    return false;
}

bool umac_mesh_ies_is_s1g_beacon(const uint8_t *frame, uint32_t len)
{
    if (frame == NULL || len < 2u)
    {
        return false;
    }
    uint16_t fc = (uint16_t)(frame[0] | ((uint16_t)frame[1] << 8));
    return (((fc >> 2) & 3u) == UMAC_MESH_S1G_BEACON_TYPE) &&
           (((fc >> 4) & 0xfu) == UMAC_MESH_S1G_BEACON_SUBTYPE);
}

bool umac_mesh_ies_s1g_beacon_sa(const uint8_t *frame, uint32_t len, uint8_t sa[6])
{
    if (sa == NULL || !umac_mesh_ies_is_s1g_beacon(frame, len) ||
        len < UMAC_MESH_S1G_BEACON_SA_OFFSET + 6u)
    {
        return false;
    }
    memcpy(sa, &frame[UMAC_MESH_S1G_BEACON_SA_OFFSET], 6);
    return true;
}

uint32_t umac_mesh_ies_s1g_beacon_ie_offset(uint16_t frame_control)
{
    uint32_t off = UMAC_MESH_S1G_BEACON_IE_OFFSET;
    if ((frame_control & UMAC_MESH_S1G_FC_NEXT_TBTT) != 0u)
    {
        off += UMAC_MESH_S1G_NEXT_TBTT_SIZE;
    }
    if ((frame_control & UMAC_MESH_S1G_FC_COMPRESS_SSID) != 0u)
    {
        off += UMAC_MESH_S1G_COMPRESS_SSID_SIZE;
    }
    if ((frame_control & UMAC_MESH_S1G_FC_ANO) != 0u)
    {
        off += UMAC_MESH_S1G_ANO_SIZE;
    }
    return off;
}

bool umac_mesh_ies_s1g_beacon_has_mesh_id(const uint8_t *frame, uint32_t len,
                                          const uint8_t *mesh_id, uint8_t mesh_id_len)
{
    if (mesh_id == NULL || mesh_id_len == 0u || !umac_mesh_ies_is_s1g_beacon(frame, len))
    {
        return false;
    }
    /* The optional fields shift every element. A peer that announces Next TBTT
     * -- which mac80211 commonly does -- puts the Mesh ID three octets further
     * on than the shortest header implies. */
    uint32_t off = umac_mesh_ies_s1g_beacon_ie_offset(
        (uint16_t)(frame[0] | ((uint16_t)frame[1] << 8)));
    if (len < off)
    {
        return false;
    }
    return mesh_id_in_elements_(frame, len, off, mesh_id, mesh_id_len);
}

/* The Mesh Configuration payload in an element walk; NULL when it is absent
 * or a length octet runs past the buffer first. */
static const uint8_t *mesh_config_in_(const uint8_t *ies, uint32_t len)
{
    uint32_t off = 0;
    while (ies != NULL && off + 2u <= len)
    {
        uint8_t eid = ies[off];
        uint8_t elen = ies[off + 1];
        if (off + 2u + (uint32_t)elen > len)
        {
            return NULL;
        }
        if (eid == UMAC_MESH_EID_MESH_CONFIG && elen >= UMAC_MESH_CFG_IE_LEN)
        {
            return &ies[off + 2u];
        }
        off += 2u + (uint32_t)elen;
    }
    return NULL;
}

bool umac_mesh_ies_set_accepting(uint8_t *ies, uint16_t len, bool accepting)
{
    uint8_t *cfg = (uint8_t *)mesh_config_in_(ies, len);
    if (cfg == NULL)
    {
        return false;
    }
    cfg[6] = (uint8_t)(accepting ? (cfg[6] | MESH_CAPABILITY_ACCEPTING)
                                 : (cfg[6] & (uint8_t)~MESH_CAPABILITY_ACCEPTING));
    return true;
}

bool umac_mesh_ies_peer_openable(const uint8_t *ies, uint32_t len, bool sae)
{
    const uint8_t *cfg = mesh_config_in_(ies, len);
    if (cfg == NULL)
    {
        return true; /* absent or unparseable: the peer's Close still refuses us */
    }
    return cfg[4] == (sae ? MESH_AUTH_SAE : MESH_AUTH_NONE) &&
           (cfg[6] & MESH_CAPABILITY_ACCEPTING) != 0u;
}
