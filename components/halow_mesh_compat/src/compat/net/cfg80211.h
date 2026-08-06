/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * <net/cfg80211.h> compat — the channel-definition + regulatory subset the
 * morse_driver dot11ah layer reads. Only what the port references is
 * provided; the full cfg80211 socket/netlink surface is intentionally absent
 * (we drive the chip directly, not through nl80211).
 */
#ifndef _COMPAT_NET_CFG80211_H_
#define _COMPAT_NET_CFG80211_H_

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/ieee80211.h>

/* nl80211 band + channel-width enums (subset). */
enum nl80211_band {
    NL80211_BAND_2GHZ = 0,
    NL80211_BAND_5GHZ = 1,
    NL80211_BAND_60GHZ = 2,
    NL80211_BAND_6GHZ = 3,
    NL80211_BAND_S1GHZ = 4,
    NUM_NL80211_BANDS,
};

enum nl80211_chan_width {
    NL80211_CHAN_WIDTH_20_NOHT,
    NL80211_CHAN_WIDTH_20,
    NL80211_CHAN_WIDTH_40,
    NL80211_CHAN_WIDTH_80,
    NL80211_CHAN_WIDTH_80P80,
    NL80211_CHAN_WIDTH_160,
    NL80211_CHAN_WIDTH_5,
    NL80211_CHAN_WIDTH_10,
    NL80211_CHAN_WIDTH_1,
    NL80211_CHAN_WIDTH_2,
    NL80211_CHAN_WIDTH_4,
    NL80211_CHAN_WIDTH_8,
    NL80211_CHAN_WIDTH_16,
};

enum nl80211_dfs_state {
    NL80211_DFS_USABLE,
    NL80211_DFS_UNAVAILABLE,
    NL80211_DFS_AVAILABLE,
};

/* S1G per-channel bandwidth flags. The kernel defines these in
 * enum ieee80211_channel_flags (cfg80211) for kernels >= 5.10.11; with the
 * version pinned to 6.6.0, morse's dot11ah sources take the `#else` path that
 * references the canonical kernel enum name, so it must be spelled exactly
 * `ieee80211_channel_flags`. Only the S1G-bandwidth subset the port reads is
 * provided here. */
enum ieee80211_channel_flags {
    IEEE80211_CHAN_1MHZ  = BIT(14),
    IEEE80211_CHAN_2MHZ  = BIT(15),
    IEEE80211_CHAN_4MHZ  = BIT(16),
    IEEE80211_CHAN_8MHZ  = BIT(17),
    IEEE80211_CHAN_16MHZ = BIT(18),
};

/* struct ieee80211_channel lives in cfg80211 (not <linux/ieee80211.h>). */
struct ieee80211_channel {
    enum nl80211_band band;
    u32 center_freq;
    u16 freq_offset;
    u16 hw_value;
    u32 flags;
    int max_antenna_gain;
    int max_power;
    int max_reg_power;
    bool beacon_found;
    u32 orig_flags;
    int orig_mag, orig_mpwr;
    enum nl80211_dfs_state dfs_state;
};

struct cfg80211_chan_def {
    struct ieee80211_channel *chan;
    enum nl80211_chan_width width;
    u32 center_freq1;
    u32 center_freq2;
    u16 freq1_offset;
};

/* morse aliases struct ieee80211_channel_s1g to ieee80211_channel for modern
 * kernels (see s1g_ieee80211.h `#else`). ieee80211_channel_to_khz is the
 * kernel cfg80211 helper the S1G channel map uses; provide it here. */
#ifndef ieee80211_channel_s1g
#define ieee80211_channel_s1g ieee80211_channel
#endif
static inline u32 ieee80211_channel_to_khz(const struct ieee80211_channel *chan)
{
    return MHZ_TO_KHZ(chan->center_freq) + chan->freq_offset;
}

/* Regulatory rule flags (subset). Only NL80211_RRF_AUTO_BW is referenced by the
 * S1G reg-rules table; the rest are provided for completeness/future rules. */
enum nl80211_reg_rule_flags {
    NL80211_RRF_NO_OFDM     = 1 << 0,
    NL80211_RRF_NO_CCK      = 1 << 1,
    NL80211_RRF_NO_INDOOR   = 1 << 2,
    NL80211_RRF_NO_OUTDOOR  = 1 << 3,
    NL80211_RRF_DFS         = 1 << 4,
    NL80211_RRF_PTP_ONLY    = 1 << 5,
    NL80211_RRF_PTMP_ONLY   = 1 << 6,
    NL80211_RRF_NO_IR       = 1 << 7,
    NL80211_RRF_AUTO_BW     = 1 << 11,
    NL80211_RRF_NO_HT40MINUS = 1 << 13,
    NL80211_RRF_NO_HT40PLUS = 1 << 14,
    NL80211_RRF_NO_80MHZ    = 1 << 15,
    NL80211_RRF_NO_160MHZ   = 1 << 16,
};

/* Power / gain unit conversions used by the REG_RULE() table macros. */
#ifndef DBI_TO_MBI
#define DBI_TO_MBI(gain) ((gain) * 100)
#define MBI_TO_DBI(gain) ((gain) / 100)
#define DBM_TO_MBM(eirp) ((eirp) * 100)
#define MBM_TO_DBM(eirp) ((eirp) / 100)
#endif

/* Regulatory rule structs (subset used by reg_rules.c). */
struct ieee80211_freq_range {
    u32 start_freq_khz;
    u32 end_freq_khz;
    u32 max_bandwidth_khz;
};

struct ieee80211_power_rule {
    u32 max_antenna_gain;
    u32 max_eirp;
};

struct ieee80211_reg_rule {
    struct ieee80211_freq_range freq_range;
    struct ieee80211_power_rule power_rule;
    u32 flags;
    u32 dfs_cac_ms;
};

struct ieee80211_regdomain {
    u32 n_reg_rules;
    char alpha2[2];
    u8 dfs_region;
    struct ieee80211_reg_rule reg_rules[];
};

int ieee80211_freq_khz_to_channel(u32 freq);
int ieee80211_frequency_to_channel(int freq);
u32 ieee80211_channel_to_freq_khz(int chan, enum nl80211_band band);
int ieee80211_channel_to_frequency(int chan, enum nl80211_band band);
/* morse-patched S1G-aware freq->channel (implemented in dot11ah/s1g_ieee80211.c). */
int __ieee80211_freq_khz_to_channel(u32 freq);

/* Locate the first information element with id @eid in the TLV buffer @ies
 * (length @len). Returns a pointer to the element header, or NULL. Standard
 * cfg80211 helper; implemented in compat.c. */
const u8 *cfg80211_find_ie(u8 eid, const u8 *ies, int len);

/* Map a channel definition to its global operating class. Implemented in
 * compat.c (S1G operating classes for the bands the port uses). */
int ieee80211_chandef_to_operating_class(struct cfg80211_chan_def *chandef,
                                         u8 *op_class);

/* Max spatial streams in a VHT NSS map — used to size MCS loops. */
#ifndef NL80211_VHT_NSS_MAX
#define NL80211_VHT_NSS_MAX 8
#endif

#endif /* _COMPAT_NET_CFG80211_H_ */
