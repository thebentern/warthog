/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * <net/mac80211.h> compat for the softmac mesh port.
 *
 * mac80211 is the Linux softmac framework. We are NOT linking it; instead
 * this header provides the data-structure surface the morse_driver dot11ah
 * and mesh sources read/write (vif, sta, hw, tx_info, rx_status), so those
 * sources compile. The actual frame plumbing is provided by the warthog
 * bridge over morselib's transport, not by mac80211. Grown incrementally.
 */
#ifndef _COMPAT_NET_MAC80211_H_
#define _COMPAT_NET_MAC80211_H_

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/printk.h>
#include <linux/skbuff.h>
#include <linux/ieee80211.h>
#include <net/cfg80211.h>

/* Interface types (subset; mesh is the one we care about). */
enum nl80211_iftype {
    NL80211_IFTYPE_UNSPECIFIED = 0,
    NL80211_IFTYPE_ADHOC,
    NL80211_IFTYPE_STATION,
    NL80211_IFTYPE_AP,
    NL80211_IFTYPE_AP_VLAN,
    NL80211_IFTYPE_WDS,
    NL80211_IFTYPE_MONITOR,
    NL80211_IFTYPE_MESH_POINT,
    NUM_NL80211_IFTYPES,
};

/* Per-BSS config the dot11ah transform reads (subset). */
struct ieee80211_bss_conf {
    u8 bssid[ETH_ALEN];
    u16 beacon_int;
    u8 dtim_period;
    bool enable_beacon;
    bool assoc;
};

/* Virtual interface. drv_priv aliases struct morse_vif (warthog-side). */
struct ieee80211_vif {
    enum nl80211_iftype type;
    u8 addr[ETH_ALEN];
    struct ieee80211_bss_conf bss_conf;
    bool p2p;
    /* trailing flexible driver-private area, like mac80211. */
    u8 drv_priv[] __aligned(sizeof(void *));
};

static inline bool ieee80211_vif_is_mesh(const struct ieee80211_vif *vif)
{
    return vif->type == NL80211_IFTYPE_MESH_POINT;
}

/* Station entry (subset). */
struct ieee80211_sta {
    u8 addr[ETH_ALEN];
    u16 aid;
    u8 drv_priv[] __aligned(sizeof(void *));
};

/* RX status metadata attached to received frames (subset). */
struct ieee80211_rx_status {
    u64 mactime;
    u32 device_timestamp;
    u32 flag;
    u16 freq;
    u8  band;
    s8  signal;
    u8  chains;
    s8  chain_signal[4];
    u16 rate_idx;
    u16 enc_flags;
};

/* TX info (subset). */
struct ieee80211_tx_info {
    u32 flags;
    u8  band;
    u8  hw_queue;
    void *rate_driver_data[2];
};

/* HW descriptor (subset). */
struct ieee80211_hw {
    void *priv;
    struct {
        int idx;
    } conf;
};

/* The per-frame control block overlays the 48-byte skb->cb. mac80211 stores
 * tx_info there on the TX path and rx_status on the RX path. */
#define IEEE80211_SKB_CB(skb)   ((struct ieee80211_tx_info *)((skb)->cb))
#define IEEE80211_SKB_RXCB(skb) ((struct ieee80211_rx_status *)((skb)->cb))

#endif /* _COMPAT_NET_MAC80211_H_ */
