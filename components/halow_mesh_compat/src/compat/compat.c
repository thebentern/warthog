/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Out-of-line implementations for the kernel-compat layer: CRC32 (IEEE
 * 802.11 reflected poly) and the cfg80211 channel<->frequency helpers that
 * the morse_driver dot11ah sources call.
 */
#include <linux/types.h>
#include <linux/crc32.h>
#include <linux/ieee80211.h>
#include <net/cfg80211.h>

/* IEEE 802.11 / Ethernet CRC32, reflected polynomial 0xEDB88320, matching
 * the Linux crc32_le(seed, buf, len) contract used for FCS and CSSID. */
u32 crc32(u32 seed, const void *data, size_t len)
{
    const u8 *p = (const u8 *)data;
    u32 crc = seed;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1) + 1));
    }
    return crc;
}

/* nl80211 band values mirrored from cfg80211.h. */
int ieee80211_channel_to_frequency(int chan, enum nl80211_band band)
{
    /* S1G band: morse maps these region-specifically elsewhere; for the
     * non-S1G bands use the standard 802.11 Annex-E channelization. */
    if (band == NL80211_BAND_2GHZ) {
        if (chan == 14)
            return 2484;
        if (chan < 14)
            return 2407 + chan * 5;
        return 0;
    }
    if (band == NL80211_BAND_5GHZ)
        return 5000 + chan * 5;
    if (band == NL80211_BAND_6GHZ)
        return 5950 + chan * 5;
    return 0;
}

int ieee80211_frequency_to_channel(int freq)
{
    if (freq == 2484)
        return 14;
    if (freq < 2484)
        return (freq - 2407) / 5;
    if (freq >= 4910 && freq <= 4980)
        return (freq - 4000) / 5;
    if (freq < 5945)
        return (freq - 5000) / 5;
    if (freq == 5935)
        return 2;
    if (freq <= 45000)
        return (freq - 5950) / 5;
    return 0;
}

int ieee80211_freq_khz_to_channel(u32 freq)
{
    return __ieee80211_freq_khz_to_channel(freq);
}

/* Walk the IE TLV buffer and return the first element whose id == eid, or NULL.
 * Bounds-checked: stops if a declared element length would run past the end.
 * Mirrors the semantics of the kernel cfg80211_find_ie(). */
const u8 *cfg80211_find_ie(u8 eid, const u8 *ies, int len)
{
    const u8 *end = ies + len;

    while (ies + 2 <= end) {
        u8 id = ies[0];
        u8 elen = ies[1];

        if (ies + 2 + elen > end)
            break;
        if (id == eid)
            return ies;
        ies += 2 + elen;
    }
    return NULL;
}

/* Map a channel definition to a global operating class. The S1G<->11n RX
 * transform calls this only to annotate a 5 GHz Supported-Operating-Classes
 * element; the warthog datapath does not consume that annotation, so a
 * not-found result (non-zero) is the safe, side-effect-free stub. A full
 * operating-class table can replace this when 5 GHz cross-band advertisement
 * is needed. */
int ieee80211_chandef_to_operating_class(struct cfg80211_chan_def *chandef,
                                         u8 *op_class)
{
    (void)chandef;
    if (op_class)
        *op_class = 0;
    return 0;
}

/* Monotonic tick counter backing get_jiffies_64(). Firmware drives this from
 * the FreeRTOS tick hook; the host test leaves it at zero. */
volatile u64 __compat_jiffies_64;
