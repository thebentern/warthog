/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WARTHOG_MESH_ADDR_H
#define WARTHOG_MESH_ADDR_H

/* Plain mesh addressing (batman off), one step per probe tick: DHCP for MESH_ADDR_DHCP_WAIT_MS
 * from the first ESTAB, then the static 10.77 address, beside which DHCP is asked again for
 * MESH_ADDR_ATTEMPT_MS -- at once when a peer joins, else after a doubling wait -- until a lease
 * replaces it. libc only. */

#include <stdbool.h>
#include <stdint.h>

#define MESH_ADDR_DHCP_WAIT_MS   6000
#define MESH_ADDR_ATTEMPT_MS     12000  /* beside the static address: OpenWrt's dnsmasq pings ~3 s before it offers */
#define MESH_ADDR_BIND_GRACE_MS  10000  /* an offer being taken at the deadline: lwIP re-requests ~11 s */
#define MESH_ADDR_RETRY_FIRST_MS 30000  /* static: the first attempt, doubling per attempt */
#define MESH_ADDR_RETRY_MAX_MS   600000

enum mesh_addr_act {
    MESH_ADDR_NONE,
    MESH_ADDR_START_DHCP, /* first ESTAB: netif up, DHCP client started, no address yet */
    MESH_ADDR_LEASED,     /* lwIP holds a lease */
    MESH_ADDR_STATIC,     /* stop the client; take the static address */
    MESH_ADDR_RETRY,      /* start the client beside the static address, which stays */
    MESH_ADDR_HOLD,       /* the address went mid-attempt (NAK, expiry): re-apply the static one unless lwIP bound or checks an offer */
    MESH_ADDR_RETRY_FAIL, /* stop the client; the static address stays */
};
struct mesh_addr_in {
    uint32_t elapsed_ms;   /* since the previous step */
    bool     estab;        /* a peer has reached ESTAB */
    uint8_t  peers;        /* peers now; one more is a peer that may serve DHCP */
    bool     dhcp;         /* AT+MESHDHCP=1 */
    bool     dhcp_running; /* lwIP's client is started */
    bool     leased;       /* it holds a lease (bound, renewing or rebinding) */
    bool     have_ip;      /* the netif has an address */
    bool     binding;      /* the client is requesting or ARP-checking an offer */
};
struct mesh_addr {         /* zero-initialised */
    uint8_t  phase, level, prev_peers;
    bool     joined;       /* a peer joined during the first attempt */
    bool     stretched;    /* this attempt already restarted for a join: once, so it still ends */
    uint32_t ms, retries;
};
enum mesh_addr_act mesh_addr_step(struct mesh_addr *s, const struct mesh_addr_in *in);

#endif
