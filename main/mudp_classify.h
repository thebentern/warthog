/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WARTHOG_MUDP_CLASSIFY_H
#define WARTHOG_MUDP_CLASSIFY_H

/* Which netif a multicast datagram is attributed to, for the Meshtastic
 * repeater. Split out of mudp.c so it can be tested on the host. Freestanding
 * on purpose -- no SDK, no ESP-IDF, libc only. */

#include <stdint.h>

enum { MUDP_NIF_USB = 0, MUDP_NIF_AP, MUDP_NIF_HALOW, MUDP_NIF_COUNT };

#define MUDP_FROM_SELF    (-2) /* one of our own addresses: our own repeat */
#define MUDP_FROM_UNKNOWN (-1)

struct mudp_nif {
    uint32_t ip;      /* host order; 0 = netif not up / not joined */
    uint32_t netmask; /* host order */
};

/* A datagram from @p src (host order) received on the socket bound to netif
 * @p arrived: that slot, MUDP_FROM_SELF for one of our own addresses, or
 * MUDP_FROM_UNKNOWN if @p arrived is not a joined netif. The source subnet
 * is never used -- it cannot tell a mesh sender from a local one. */
int mudp_classify(uint32_t src, int arrived, const struct mudp_nif nif[MUDP_NIF_COUNT]);

#endif
