/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bat_mode.h"

#include <stdio.h>
#include <string.h>

#include "bat.h"

_Static_assert(BAT_MODE_RX_MAX <= BAT_MAX_LINK_FRAME, "an RX slot holds every frame the hook keeps");

enum bat_mode_reason bat_mode_check(uint8_t batman, uint8_t mesh_en, uint8_t fwd, uint8_t bridge,
                                    bool sae_build, bool host_ccmp_build)
{
    if (!batman) {
        return BAT_MODE_OFF;
    }
    if (!mesh_en) {
        return BAT_MODE_MESH_OFF;
    }
    /* Under SAE every peer's ELP/OGM/BCAST is a group frame only host CCMP opens. */
    if (sae_build && !host_ccmp_build) {
        return BAT_MODE_NO_GROUP_RX;
    }
    if (fwd) {
        return BAT_MODE_FWD;
    }
    if (bridge) {
        return BAT_MODE_BRIDGE;
    }
    return BAT_MODE_OK;
}

const char *bat_mode_reason_text(enum bat_mode_reason r)
{
    switch (r) {
    case BAT_MODE_OK:          return "ok";
    case BAT_MODE_OFF:         return "off";
    case BAT_MODE_MESH_OFF:    return "mesh-off";
    case BAT_MODE_FWD:         return "fwd";
    case BAT_MODE_BRIDGE:      return "bridge";
    case BAT_MODE_NO_GROUP_RX: return "sae-no-host-ccmp";
    case BAT_MODE_NOMEM:       return "nomem";
    case BAT_MODE_INIT_FAIL:   return "init-failed";
    case BAT_MODE_MESH_FAILED: return "mesh-failed";
    default:                   return "?";
    }
}

enum bat_mode_rx bat_mode_rx_classify(const uint8_t *frame, size_t len, const uint8_t *ta)
{
    if (frame == NULL || len < 14) {
        return BAT_MODE_RX_SHORT;
    }
    if (len > BAT_MODE_RX_MAX) {
        return BAT_MODE_RX_TOOBIG;
    }
    if (frame[12] != 0x43 || frame[13] != 0x05) {
        return BAT_MODE_RX_NONBAT;
    }
    /* batman links are single-hop: a frame another 802.11s node relayed is not one. */
    if (ta != NULL && memcmp(frame + 6, ta, 6) != 0) {
        return BAT_MODE_RX_RELAYED;
    }
    /* A unicast ELP is a Linux neighbour's throughput probe; the engine would only drop it. */
    if (len >= 15 && frame[14] == 0x03 && (frame[0] & 0x01) == 0) {
        return BAT_MODE_RX_PROBE;
    }
    return BAT_MODE_RX_BATMAN;
}

const uint8_t *bat_mode_tx_ra(const uint8_t *frame)
{
    return (frame[0] & 0x01) ? NULL : frame;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') {
        return (c | 0x20) - 'a' + 10;
    }
    return -1;
}

bool bat_mode_parse_mac(const char *s, uint8_t out[6])
{
    uint8_t m[6];
    if (s == NULL) {
        return false;
    }
    for (unsigned i = 0; i < 6; i++, s += 3) {
        const int hi = hex_digit(s[0]), lo = hi < 0 ? -1 : hex_digit(s[1]);
        if (lo < 0 || s[2] != (i < 5 ? ':' : '\0')) {
            return false;
        }
        m[i] = (uint8_t)(hi << 4 | lo);
    }
    memcpy(out, m, 6);
    return true;
}

void bat_mode_soft_mac(const uint8_t factory[6], const uint8_t mesh[6], uint8_t out[6])
{
    memmove(out, factory, 6);
    out[0] = 0x06; /* unicast, locally administered */
    if (mesh != NULL && memcmp(out, mesh, 6) == 0) {
        out[0] = 0x02;
    }
}

void bat_mode_port_config(struct bat_config *cfg, const uint8_t mesh[6], const uint8_t factory[6],
                          uint32_t tput_override, bool grp_std, uint8_t copies_override)
{
    memcpy(cfg->hard_addr, mesh, 6);
    bat_mode_soft_mac(factory, mesh, cfg->soft_addr);
    cfg->tput_override = tput_override;
    /* Replicas are ACKed per peer; standard group frames are not. */
    cfg->bcast_copies = copies_override ? copies_override : (grp_std ? 3 : 1);
}

void bat_mode_static_ip(const uint8_t soft[6], unsigned attempt, uint8_t ip[4])
{
    /* A batch's factory MACs step by 4 (one per ESP32 interface): drop those two bits. */
    const uint32_t id = ((uint32_t)soft[3] << 16 | (uint32_t)soft[4] << 8 | soft[5]) >> 2;
    ip[0] = 10;
    ip[1] = 41;
    ip[2] = 253;
    ip[3] = (uint8_t)(2u + (id + attempt) % 252u);
}

void bat_mode_static_gw(uint8_t gw[4])
{
    gw[0] = 10;
    gw[1] = 41;
    gw[2] = 0;
    gw[3] = 1;
}

enum bat_mode_router bat_mode_router_state(bool have_router, bool mac_known, int answer, uint32_t ogm_age_ms)
{
    if (!have_router) {
        return BAT_MODE_ROUTER_NONE;
    }
    if (!mac_known || answer < 0) {
        return BAT_MODE_ROUTER_UNKNOWN;
    }
    /* batman keeps a silent originator's route 200 s; its OGMs stop at once. */
    return (answer > 0 && ogm_age_ms <= BAT_MODE_ROUTER_OGM_MS) ? BAT_MODE_ROUTER_OK : BAT_MODE_ROUTER_LOST;
}

const char *bat_mode_router_text(enum bat_mode_router r)
{
    switch (r) {
    case BAT_MODE_ROUTER_NONE:    return "none";
    case BAT_MODE_ROUTER_OK:      return "ok";
    case BAT_MODE_ROUTER_LOST:    return "lost";
    case BAT_MODE_ROUTER_UNKNOWN: return "unknown";
    default:                      return "?";
    }
}

enum { BAT0_P_IDLE, BAT0_P_DHCP, BAT0_P_LEASED, BAT0_P_PROBE, BAT0_P_HELD, BAT0_P_RETRY };

/* @p first doubled @p n times, at most BAT_MODE_RETRY_MAX_MS. */
static uint32_t doubled(uint32_t first, uint8_t n)
{
    uint32_t w = first;
    for (uint8_t i = 0; i < n && w < BAT_MODE_RETRY_MAX_MS; i++) {
        w *= 2;
    }
    return w < BAT_MODE_RETRY_MAX_MS ? w : BAT_MODE_RETRY_MAX_MS;
}

/* The wait before the next attempt while an address is held. */
static uint32_t retry_wait(uint8_t level)
{
    return doubled(BAT_MODE_RETRY_FIRST_MS, level);
}

/* An attempt still runs: its time is left, or an offer is being taken just past it. */
static bool attempt_on(const struct bat_mode_bat0 *s, const struct bat_mode_bat0_in *in)
{
    return in->dhcp_running && (s->ms < BAT_MODE_DHCP_WAIT_MS ||
                                (in->binding && s->ms < BAT_MODE_DHCP_WAIT_MS + BAT_MODE_BIND_GRACE_MS));
}

static enum bat_mode_bat0_act to_leased(struct bat_mode_bat0 *s)
{
    s->phase = BAT0_P_LEASED;
    s->lost_ms = 0;
    s->level = 0;
    s->held_lease = false;
    return BAT0_LEASED;
}

enum bat_mode_bat0_act bat_mode_bat0_step(struct bat_mode_bat0 *s, const struct bat_mode_bat0_in *in)
{
    /* One gateway more (a second, or one replacing a gate whose route lingers), or a route after none,
     * may mean a DHCP server that was not there; the best one changing does not. */
    const bool rise = in->gws > s->prev_gws || (in->routes != 0 && !s->prev_routes);
    s->prev_gws = in->gws;
    s->prev_routes = in->routes != 0;
    if (rise) {
        s->level = 0; /* also mid-attempt: the wait after it is the first one */
        s->lost_level = 0;
    }
    switch (s->phase) {
    case BAT0_P_IDLE:
        if (in->routes == 0) {
            return BAT0_NONE; /* nobody to reach, and no gate can route an OFFER back yet */
        }
        s->attempt = 0;
        s->ticks = 0;
        s->ms = 0;
        s->phase = in->dhcp ? BAT0_P_DHCP : BAT0_P_PROBE;
        return in->dhcp ? BAT0_START_DHCP : BAT0_PROBE;
    case BAT0_P_DHCP:
        if (in->leased) {
            return to_leased(s);
        }
        s->ms += in->elapsed_ms;
        if (attempt_on(s, in)) {
            return BAT0_NONE;
        }
        s->attempt = 0; /* a lease lost later probes every candidate again */
        s->ticks = 0;
        s->phase = BAT0_P_PROBE;
        return BAT0_PROBE;
    case BAT0_P_LEASED:
        if (!in->leased) {
            s->phase = BAT0_P_DHCP; /* lwIP dropped it (NAK, expiry) and is asking again */
            s->ms = 0;
            return BAT0_NONE;
        }
        if (!in->dhcp || in->router == BAT_MODE_ROUTER_NONE || in->router == BAT_MODE_ROUTER_OK) {
            s->lost_ms = 0;
            s->lost_level = 0;
            return BAT0_NONE;
        }
        s->lost_ms += in->elapsed_ms;
        /* Doubling per restart until a check passes: the same server may hand the same router back. */
        if (s->lost_ms < doubled(BAT_MODE_ROUTER_LOSS_MS, s->lost_level)) {
            return BAT0_NONE;
        }
        if (s->lost_level < 255) {
            s->lost_level++;
        }
        s->restarts++;
        s->held_lease = true;
        s->ms = 0;
        s->phase = BAT0_P_RETRY;
        return BAT0_ROUTER_LOST;
    case BAT0_P_PROBE:
        if (in->taken) {
            s->ticks = 0;
            if (++s->attempt >= BAT_MODE_STATIC_TRIES) {
                break; /* every probed candidate is held: take the next unprobed */
            }
            return BAT0_PROBE;
        }
        if (++s->ticks < BAT_MODE_PROBE_TICKS) {
            return BAT0_PROBE;
        }
        break;
    case BAT0_P_HELD:
        if (!in->dhcp) {
            return BAT0_NONE;
        }
        s->ms = s->ms > UINT32_MAX - in->elapsed_ms ? UINT32_MAX : s->ms + in->elapsed_ms; /* no route for weeks */
        if (in->routes == 0 || s->ms < retry_wait(s->level)) {
            return BAT0_NONE;
        }
        if (s->level < 255) {
            s->level++;
        }
        s->retries++;
        s->ms = 0;
        s->phase = BAT0_P_RETRY;
        return BAT0_RETRY;
    case BAT0_P_RETRY:
        if (in->leased) {
            return to_leased(s);
        }
        s->ms += in->elapsed_ms;
        if (attempt_on(s, in)) {
            return in->have_ip ? BAT0_NONE : BAT0_HOLD;
        }
        s->ms = 0;
        s->phase = BAT0_P_HELD;
        return BAT0_RETRY_FAIL;
    default:
        return BAT0_NONE;
    }
    s->phase = BAT0_P_HELD; /* from the probe: candidate s->attempt */
    s->ms = 0;
    s->held_lease = false;
    return BAT0_STATIC;
}

bool bat_mode_bat0_probing(const struct bat_mode_bat0 *s)
{
    return s->phase == BAT0_P_PROBE;
}

bool bat_mode_bat0_leased(const struct bat_mode_bat0 *s)
{
    return s->phase == BAT0_P_LEASED;
}

int bat_mode_bat0_line(const struct bat_mode_bat0_view *v, char *buf, size_t len)
{
    static const char *const addr[] = { "idle", "dhcp", "leased", "probing", "static", "retry" };
    const struct bat_mode_bat0 *s = v->s;
    const char *a = s->phase < sizeof(addr) / sizeof(addr[0]) ? addr[s->phase] : "?";
    if (s->phase == BAT0_P_HELD && s->held_lease) {
        a = "held";
    }
    char router[48], retry[16], gw[48];
    int n = snprintf(router, sizeof(router), "%u.%u.%u.%u", v->router_ip[0], v->router_ip[1], v->router_ip[2],
                     v->router_ip[3]);
    const bool lost = v->router == BAT_MODE_ROUTER_LOST || v->router == BAT_MODE_ROUTER_UNKNOWN;
    if (s->phase == BAT0_P_LEASED && n > 0 && (size_t)n < sizeof(router)) {
        snprintf(router + n, sizeof(router) - (size_t)n, lost ? "(%s %lus)" : "(%s)",
                 bat_mode_router_text((enum bat_mode_router)v->router), (unsigned long)(s->lost_ms / 1000));
    }
    /* Held: the next attempt; leased with a failing router: the restart it counts down to. */
    const uint32_t w = s->phase == BAT0_P_LEASED ? doubled(BAT_MODE_ROUTER_LOSS_MS, s->lost_level)
                                                 : retry_wait(s->level);
    const uint32_t spent = s->phase == BAT0_P_LEASED ? s->lost_ms : s->ms;
    if (v->dhcp && (s->phase == BAT0_P_HELD || (s->phase == BAT0_P_LEASED && lost))) {
        snprintf(retry, sizeof(retry), "%lus", (unsigned long)((spent < w ? w - spent : 0) / 1000));
    } else {
        snprintf(retry, sizeof(retry), "-");
    }
    if (v->gw) {
        snprintf(gw, sizeof(gw), "%02x:%02x:%02x:%02x:%02x:%02x(%lu.%lu/%lu.%lu)", v->gw_orig[0], v->gw_orig[1],
                 v->gw_orig[2], v->gw_orig[3], v->gw_orig[4], v->gw_orig[5], (unsigned long)(v->gw_down / 10),
                 (unsigned long)(v->gw_down % 10), (unsigned long)(v->gw_up / 10), (unsigned long)(v->gw_up % 10));
    } else {
        snprintf(gw, sizeof(gw), "none");
    }
    return snprintf(buf, len, "+MESHBATMAN: bat0 addr=%s ip=%u.%u.%u.%u router=%s retry_in=%s retries=%lu "
                    "restarts=%lu gw=%s\r\n", a, v->ip[0], v->ip[1], v->ip[2], v->ip[3], router, retry,
                    (unsigned long)s->retries, (unsigned long)s->restarts, gw);
}

uint32_t bat_mode_kbps_to_units(bool present, bool estab, bool rc_valid, uint32_t kbps)
{
    if (!present || !estab) {
        return 0;
    }
    if (!rc_valid || kbps == 0) {
        return 0xFFFFFFFFu;
    }
    /* 0 means "no station" to the engine, so a live peer is never below 1. */
    return kbps < 100u ? 1u : kbps / 100u;
}

uint32_t bat_mode_link_tput(struct bat_mode_links *c, uint32_t now, const uint8_t addr[6],
                            bat_mode_links_query_t query, void *ctx)
{
    if (!c->valid || now - c->ts >= BAT_MODE_LINKS_MS) {
        uint8_t n = 0;
        if (query(ctx, c->l, BAT_MODE_LINKS_MAX, &n)) {
            c->n = n < BAT_MODE_LINKS_MAX ? n : BAT_MODE_LINKS_MAX;
            c->ts = now;
            c->valid = true;
        } else {
            /* A failed query is not "no station" (elp-ogm 2.5); the next lookup retries. */
            c->fails++;
            if (!c->valid || now - c->ts >= BAT_MODE_LINKS_STALE_MS) {
                return 0xFFFFFFFFu;
            }
        }
    }
    for (uint8_t i = 0; i < c->n; i++) {
        if (memcmp(c->l[i].addr, addr, 6) == 0) {
            return bat_mode_kbps_to_units(true, c->l[i].estab != 0, c->l[i].rc_valid != 0,
                                          c->l[i].expected_tput_kbps);
        }
    }
    return bat_mode_kbps_to_units(false, false, false, 0);
}
