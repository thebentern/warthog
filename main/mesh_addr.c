/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mesh_addr.h"

enum { P_IDLE, P_DHCP, P_LEASED, P_HELD, P_RETRY };

/* MESH_ADDR_RETRY_FIRST_MS doubled @p n times, at most MESH_ADDR_RETRY_MAX_MS. */
static uint32_t retry_wait(uint8_t n)
{
    uint32_t w = MESH_ADDR_RETRY_FIRST_MS;
    for (uint8_t i = 0; i < n && w < MESH_ADDR_RETRY_MAX_MS; i++) {
        w *= 2;
    }
    return w < MESH_ADDR_RETRY_MAX_MS ? w : MESH_ADDR_RETRY_MAX_MS;
}

/* An attempt of @p len still runs: its time is left, or an offer is being taken just past it. */
static bool attempt_on(const struct mesh_addr *s, const struct mesh_addr_in *in, uint32_t len)
{
    return in->dhcp_running && (s->ms < len || (in->binding && s->ms < len + MESH_ADDR_BIND_GRACE_MS));
}

static enum mesh_addr_act to_leased(struct mesh_addr *s)
{
    s->phase = P_LEASED;
    s->level = 0;
    s->joined = false;
    return MESH_ADDR_LEASED;
}

enum mesh_addr_act mesh_addr_step(struct mesh_addr *s, const struct mesh_addr_in *in)
{
    const bool rise = in->peers > s->prev_peers;
    s->prev_peers = in->peers;
    if (rise) {
        s->level = 0;
        s->joined |= s->phase == P_DHCP; /* the first window is not stretched; the retry after it is at once */
    }
    switch (s->phase) {
    case P_IDLE:
        if (!in->estab) {
            return MESH_ADDR_NONE;
        }
        s->ms = 0;
        s->phase = in->dhcp ? P_DHCP : P_HELD;
        return in->dhcp ? MESH_ADDR_START_DHCP : MESH_ADDR_STATIC;
    case P_DHCP:
        if (in->leased) {
            return to_leased(s);
        }
        s->ms += in->elapsed_ms;
        if (attempt_on(s, in, MESH_ADDR_DHCP_WAIT_MS)) {
            return MESH_ADDR_NONE;
        }
        s->ms = 0;
        s->phase = P_HELD;
        return MESH_ADDR_STATIC;
    case P_LEASED:
        if (in->leased) {
            return MESH_ADDR_NONE;
        }
        s->ms = 0; /* lwIP dropped it (NAK, expiry) and is asking again: the static address meanwhile */
        s->stretched = false;
        s->phase = P_RETRY;
        return MESH_ADDR_HOLD;
    case P_HELD:
        s->ms = s->ms > UINT32_MAX - in->elapsed_ms ? UINT32_MAX : s->ms + in->elapsed_ms; /* no peer for weeks */
        if (!in->dhcp || in->peers == 0) {
            return MESH_ADDR_NONE;
        }
        if (!rise && !s->joined) {
            if (s->ms < retry_wait(s->level)) {
                return MESH_ADDR_NONE;
            }
            if (s->level < 255) {
                s->level++;
            }
        }
        s->joined = false;
        s->retries++;
        s->ms = 0;
        s->stretched = false;
        s->phase = P_RETRY;
        return MESH_ADDR_RETRY;
    case P_RETRY:
        if (in->leased) {
            return to_leased(s);
        }
        /* A peer joined mid-attempt: a whole one for it -- but ONCE, else a
         * flapping peer holds the attempt open forever and it never ends. */
        if (rise && !s->stretched) {
            s->ms = in->elapsed_ms;
            s->stretched = true;
        } else {
            s->ms += in->elapsed_ms;
        }
        if (attempt_on(s, in, MESH_ADDR_ATTEMPT_MS)) {
            return in->have_ip ? MESH_ADDR_NONE : MESH_ADDR_HOLD;
        }
        s->ms = 0;
        s->phase = P_HELD;
        return MESH_ADDR_RETRY_FAIL;
    default:
        return MESH_ADDR_NONE;
    }
}
