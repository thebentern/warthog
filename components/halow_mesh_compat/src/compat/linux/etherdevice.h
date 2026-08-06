/* SPDX-License-Identifier: GPL-2.0-or-later
 * <linux/etherdevice.h> compat — MAC-address helpers. */
#ifndef _COMPAT_LINUX_ETHERDEVICE_H_
#define _COMPAT_LINUX_ETHERDEVICE_H_
#include <linux/types.h>
#include <linux/if_ether.h>
#include <string.h>
static inline bool is_zero_ether_addr(const u8 *a)
{ return !(a[0]|a[1]|a[2]|a[3]|a[4]|a[5]); }
static inline bool is_multicast_ether_addr(const u8 *a) { return a[0] & 0x01; }
static inline bool is_broadcast_ether_addr(const u8 *a)
{ return (a[0]&a[1]&a[2]&a[3]&a[4]&a[5]) == 0xff; }
static inline bool is_unicast_ether_addr(const u8 *a) { return !is_multicast_ether_addr(a); }
static inline void eth_broadcast_addr(u8 *a) { memset(a, 0xff, ETH_ALEN); }
static inline void eth_zero_addr(u8 *a) { memset(a, 0, ETH_ALEN); }
static inline void ether_addr_copy(u8 *dst, const u8 *src) { memcpy(dst, src, ETH_ALEN); }
static inline bool ether_addr_equal(const u8 *a, const u8 *b)
{ return memcmp(a, b, ETH_ALEN) == 0; }
/* Same comparison without the alignment assumption (kernel keeps both). */
static inline bool ether_addr_equal_unaligned(const u8 *a, const u8 *b)
{ return memcmp(a, b, ETH_ALEN) == 0; }
#endif
