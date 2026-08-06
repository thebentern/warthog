/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * <linux/skbuff.h> compat — a functional linear sk_buff sufficient for the
 * dot11ah S1G<->11n frame transforms. The transforms grow/shrink the frame in
 * place (skb_put / skb_trim / skb_pull / skb_push) and stash per-frame mac80211
 * metadata in the 48-byte control buffer (cb), read back via IEEE80211_SKB_CB /
 * IEEE80211_SKB_RXCB. This is a single-buffer, non-cloned, non-fragmented
 * implementation — exactly the shape the codec needs and no more. The driver's
 * real frame container is struct mmpkt; the bridge translates at the edges.
 */
#ifndef _COMPAT_LINUX_SKBUFF_H_
#define _COMPAT_LINUX_SKBUFF_H_

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <string.h>

/* mac80211 reserves 48 bytes of control buffer for tx_info / rx_status. */
#define SKB_CB_SIZE 48

struct sk_buff {
    u8 *head;           /* start of allocated buffer */
    u8 *data;           /* start of valid data */
    u8 *tail;           /* end of valid data */
    u8 *end;            /* end of allocated buffer */
    unsigned int len;   /* bytes of valid data (tail - data) */
    __be16 protocol;
    u8 cb[SKB_CB_SIZE]; /* opaque per-frame control block */
};

/* --- length / room queries --- */
static inline unsigned int skb_headroom(const struct sk_buff *skb)
{
    return (unsigned int)(skb->data - skb->head);
}
static inline unsigned int skb_tailroom(const struct sk_buff *skb)
{
    return (unsigned int)(skb->end - skb->tail);
}
static inline bool skb_is_nonlinear(const struct sk_buff *skb) { (void)skb; return false; }

/* --- grow/shrink the valid-data window --- */
/* Append len bytes at the tail, return pointer to the start of the new area. */
static inline void *skb_put(struct sk_buff *skb, unsigned int len)
{
    u8 *old_tail = skb->tail;
    skb->tail += len;
    skb->len  += len;
    return old_tail;
}
/* Prepend len bytes at the head, return pointer to the new data start. */
static inline void *skb_push(struct sk_buff *skb, unsigned int len)
{
    skb->data -= len;
    skb->len  += len;
    return skb->data;
}
/* Remove len bytes from the head, return the new data start. */
static inline void *skb_pull(struct sk_buff *skb, unsigned int len)
{
    skb->len  -= len;
    skb->data += len;
    return skb->data;
}
/* Truncate the buffer to len bytes. */
static inline void skb_trim(struct sk_buff *skb, unsigned int len)
{
    if (skb->len > len) {
        skb->len  = len;
        skb->tail = skb->data + len;
    }
}
/* Reserve headroom on a fresh skb (moves data/tail forward). */
static inline void skb_reserve(struct sk_buff *skb, unsigned int len)
{
    skb->data += len;
    skb->tail += len;
}

/* --- header pointer helpers (linear buffer: all headers sit in data) --- */
static inline int  skb_network_offset(const struct sk_buff *skb) { (void)skb; return 0; }
static inline u8  *skb_network_header(const struct sk_buff *skb) { return skb->data; }
static inline u8  *skb_mac_header(const struct sk_buff *skb)     { return skb->data; }
static inline u8  *skb_tail_pointer(const struct sk_buff *skb)   { return skb->tail; }

/* --- allocation (host test only; firmware bridges to mmpkt) --- */
static inline void __skb_set_pointers(struct sk_buff *skb, u8 *buf, unsigned int size)
{
    skb->head = buf;
    skb->data = buf;
    skb->tail = buf;
    skb->end  = buf + size;
    skb->len  = 0;
}

#endif /* _COMPAT_LINUX_SKBUFF_H_ */
