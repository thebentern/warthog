/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * "morse.h" compat shim — the driver-private structures the morse_driver
 * dot11ah / mesh sources read via vif->drv_priv (cast to struct morse_vif).
 *
 * This is the warthog-side stand-in for the Linux driver's giant morse.h.
 * Only the fields the ported sources actually touch are provided; the
 * struct is GROWN INCREMENTALLY as each translation unit is ported (each
 * new unit reveals the next batch of fields it needs, validated by the
 * host compile). Field names/types mirror the upstream morse.h so the
 * vendored .c files compile unmodified.
 *
 * Layout-sensitivity note: warthog populates these structs itself from the
 * mmwlan/umac channel state before invoking a transform; they are NOT a
 * wire format, so exact upstream offsets do not matter — only field names
 * and value semantics.
 */
#ifndef _COMPAT_MORSE_H_
#define _COMPAT_MORSE_H_

#include <linux/types.h>
#include <linux/crc32.h>
#include <net/mac80211.h>

/* Driver-wide constants/macros that live in the upstream morse.h. */
#ifndef AID_LIMIT
#define AID_LIMIT  (2007)        /* max association id (mesh/AP AID bitmap) */
#endif
#ifndef KHZ_TO_HZ
#define KHZ_TO_HZ(x) ((x) * 1000)
#endif
#ifndef MHZ_TO_HZ
#define MHZ_TO_HZ(x) ((x) * 1000000)
#endif

enum morse_mac_subbands_mode {
    SUBBANDS_MODE_DISABLED = 0,
    SUBBANDS_MODE_ENABLED  = 1,
};

/* CSSID (Compressed SSID) = bitwise-NOT of the CRC32 of the SSID. Inline in the
 * upstream morse.h; the S1G beacon builder uses it to compress the mesh ID. */
static inline u32 morse_generate_cssid(const u8 *ssid, u8 len)
{
    return ~crc32(~0, ssid, len);
}

/* Pack a 6-byte MAC into the low 48 bits of a u64 (big-endian order). */
static inline u64 mac2uint64(const u8 *bssid)
{
    return ((u64)(bssid[0]) << 40) | ((u64)(bssid[1]) << 32) |
           ((u64)(bssid[2]) << 24) | ((u64)(bssid[3]) << 16) |
           ((u64)(bssid[4]) <<  8) |  (u64)(bssid[5]);
}

/* struct morse_channel_info is the dot11ah layer's own type — it is defined in
 * dot11ah/dot11ah.h (the canonical, fuller definition incl. S1G caps). The
 * dot11ah port always includes dot11ah.h before morse.h, so we must NOT
 * redefine it here. Pull it in so morse.h is also usable standalone. */
#include "dot11ah/dot11ah.h"
/* PV1 header-compression state (struct morse_pv1) is embedded by value in
 * struct morse_vif below, so the full definition must be visible here. */
#include "pv1.h"
/* Time-unit + ceiling helpers (MORSE_TU_TO_MS etc.) the driver core uses. */
#include "utils.h"

/* Driver custom config (subset the transform reads). */
struct morse_custom_configs {
    u8 sta_type;
    u8 enc_mode;
    enum morse_mac_subbands_mode enable_subbands;
    struct morse_channel_info channel_info;
    struct morse_channel_info default_bw_info;
};

/* Minimal AP-side state pointer (mesh treats itself AP-like for beaconing). */
struct morse_vif_ap {
    int num_stas;
    u16 largest_aid;   /* highest associated AID — sizes the TIM partial bitmap */
};

/* The driver-private VIF struct, reachable via vif->drv_priv. Field names and
 * types mirror the upstream morse.h; only the subset the dot11ah codec touches
 * is present. Grown incrementally as units are ported. */
struct morse_vif {
    struct ieee80211_vif *vif;
    struct morse_custom_configs *custom_configs;
    struct morse_vif_ap *ap;
    u64 epoch;
    u32 edca_param_crc;
    bool is_sta_assoc;
    bool chan_switch_in_progress;
    bool mask_ecsa_info_in_beacon;
    bool enable_pv1;
    struct morse_channel_info ecsa_channel_info;
    struct ieee80211_s1g_cap s1g_cap_ie;  /* cached S1G capabilities IE body */
    u16 s1g_bcn_change_seq;               /* beacon change sequence number */
    u32 s1g_oper_param_crc;               /* CRC of last S1G operation params */
    struct morse_pv1 pv1;                 /* PV1 header-compression state */
    /* mesh sub-state grown when the mesh units are ported. */
    void *mesh;
};

/* vif->drv_priv -> struct morse_vif accessor (matches upstream inline). */
static inline struct morse_vif *ieee80211_vif_to_morse_vif(struct ieee80211_vif *vif)
{
    return (struct morse_vif *)vif->drv_priv;
}

static inline struct ieee80211_vif *morse_vif_to_ieee80211_vif(struct morse_vif *mv)
{
    return mv->vif;
}

#endif /* _COMPAT_MORSE_H_ */
