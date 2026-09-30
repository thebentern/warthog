/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bat_internal.h"

uint32_t bat_neigh_tput(const struct bat_neigh *n)
{
    return (uint32_t)(n->tput_acc >> 10);
}

struct bat_neigh *bat_neigh_find(struct bat *b, const uint8_t hard[6])
{
    for (unsigned i = 0; i < BAT_MAX_NEIGH; i++) {
        if (b->neigh[i].used && bat_mac_eq(b->neigh[i].addr, hard)) {
            return &b->neigh[i];
        }
    }
    return NULL;
}

/* Drops every candidate learned through neighbour @ni, then recomputes the routers it held. */
static void neigh_evict(struct bat *b, unsigned ni)
{
    for (unsigned i = 0; i < BAT_MAX_ORIG; i++) {
        struct bat_orig *o = &b->orig[i];
        bool hit = false;
        if (!o->used) {
            continue;
        }
        for (unsigned c = 0; c < BAT_CANDS_PER_ORIG; c++) {
            if (o->cand[c].used && o->cand[c].neigh == ni) {
                o->cand[c].used = 0;
                hit = true;
            }
        }
        if (hit) {
            bat_orig_recompute(b, o);
        }
    }
    memset(&b->neigh[ni], 0, sizeof(b->neigh[ni]));
    BAT_INC(b, NEIGH_PURGED);
}

/* bat_neigh_get would find or admit @hard (a free slot, or one silent for over 30 s). */
static bool neigh_room(struct bat *b, const uint8_t hard[6])
{
    if (bat_neigh_find(b, hard)) {
        return true;
    }
    for (unsigned i = 0; i < BAT_MAX_NEIGH; i++) {
        if (!b->neigh[i].used || bat_age(b, b->neigh[i].last_seen) > BAT_EVICT_AGE_MS) {
            return true;
        }
    }
    return false;
}

struct bat_neigh *bat_neigh_get(struct bat *b, const uint8_t hard[6], const uint8_t orig[6])
{
    struct bat_neigh *n = bat_neigh_find(b, hard);
    if (n) {
        return n;
    }
    int slot = -1, old = -1;
    for (unsigned i = 0; i < BAT_MAX_NEIGH; i++) {
        if (!b->neigh[i].used) {
            slot = (int)i;
            break;
        }
        if (old < 0 || bat_age(b, b->neigh[i].last_seen) > bat_age(b, b->neigh[old].last_seen)) {
            old = (int)i;
        }
    }
    if (slot < 0) {
        if (old < 0 || bat_age(b, b->neigh[old].last_seen) <= BAT_EVICT_AGE_MS) {
            BAT_INC(b, NEIGH_FULL);
            return NULL;
        }
        neigh_evict(b, (unsigned)old);
        slot = old;
    }
    n = &b->neigh[slot];
    memset(n, 0, sizeof(*n));
    memcpy(n->addr, hard, 6);
    memcpy(n->orig, orig, 6);
    n->used = 1;
    n->last_seen = b->now;
    BAT_INC(b, NEIGH_NEW);
    return n;
}

static uint32_t sample(struct bat *b, const struct bat_neigh *n)
{
    if (b->cfg.tput_override) {
        return b->cfg.tput_override;
    }
    uint32_t r = b->ops.link_tput(b->user, n->addr);
    return r == BAT_TPUT_UNKNOWN ? BAT_TPUT_DEFAULT : r;
}

void bat_elp_send(struct bat *b)
{
    uint8_t *f = b->txbuf, *p = f + BAT_ETH_HLEN;
    p[BAT_OFF_TYPE] = BAT_PT_ELP;
    p[BAT_OFF_VERSION] = BAT_COMPAT;
    memcpy(p + BAT_ELP_ORIG, b->cfg.hard_addr, 6);
    bat_put32(p + BAT_ELP_SEQ, b->elp_seq);
    bat_put32(p + BAT_ELP_INTERVAL, b->cfg.elp_interval_ms);
    bat_put32(p + BAT_ELP_HLEN, 0);
    bat_link_tx(b, bat_bcast_addr, f, BAT_ETH_HLEN + BAT_ELP_LEN);
    b->elp_seq++;
    bat_seq_note(b);
    BAT_INC(b, ELP_TX);
    for (unsigned i = 0; i < BAT_MAX_NEIGH; i++) {
        struct bat_neigh *n = &b->neigh[i];
        if (!n->used) {
            continue;
        }
        uint64_t s = (uint64_t)sample(b, n) << 10;
        n->tput_acc = n->tput_acc == 0 ? s : (7 * n->tput_acc + s) / 8;
    }
}

void bat_elp_rx(struct bat *b, uint8_t *frame, size_t len)
{
    const uint8_t *p = frame + BAT_ETH_HLEN;
    const uint8_t *orig = p + BAT_ELP_ORIG;
    (void)len;
    if (bat_is_own(b, orig)) {
        BAT_INC(b, ELP_OWN_ORIG);
        return;
    }
    /* A direct neighbour outranks a distant originator, unless it cannot be admitted anyway. */
    struct bat_orig *o = neigh_room(b, frame + BAT_LINK_SRC) ? bat_orig_get_neigh(b, orig) :
                                                               bat_orig_get(b, orig);
    if (!o) {
        return;
    }
    struct bat_neigh *n = bat_neigh_get(b, frame + BAT_LINK_SRC, orig);
    if (!n) {
        return;
    }
    struct bat_cand *c = bat_cand_get(b, o, (unsigned)(n - b->neigh));
    uint32_t seq = bat_get32(p + BAT_ELP_SEQ);
    int32_t d = (int32_t)(seq - n->elp_seq);
    if (d > -BAT_SEQ_WINDOW && d <= 0) {
        BAT_INC(b, ELP_DUP);
        return;
    }
    n->last_seen = b->now;
    if (c) {
        c->last_seen = b->now;
    }
    n->elp_seq = seq;
    n->elp_interval = bat_get32(p + BAT_ELP_INTERVAL);
    BAT_INC(b, ELP_RX);
}
