/* SPDX-License-Identifier: GPL-2.0-or-later
 * <linux/if_ether.h> compat — Ethernet constants the 802.11 headers use. */
#ifndef _COMPAT_LINUX_IF_ETHER_H_
#define _COMPAT_LINUX_IF_ETHER_H_
#include <linux/types.h>
#define ETH_ALEN 6
#define ETH_HLEN 14
#define ETH_P_PAE 0x888E
#define ETH_P_TDLS 0x890D
struct ethhdr { u8 h_dest[ETH_ALEN]; u8 h_source[ETH_ALEN]; __be16 h_proto; } __packed;
#endif
