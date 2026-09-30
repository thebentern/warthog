/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bat_internal.h"

struct bat_orig *bat_orig_find(struct bat *b, const uint8_t addr[6])
{
    for (unsigned i = 0; i < BAT_MAX_ORIG; i++) {
        if (b->orig[i].used && bat_mac_eq(b->orig[i].addr, addr)) {
            return &b->orig[i];
        }
    }
    return NULL;
}

unsigned bat_orig_index(const struct bat *b, const struct bat_orig *o)
{
    return (unsigned)(o - b->orig);
}

struct bat_orig *bat_orig_at(struct bat *b, unsigned idx)
{
    return (idx < BAT_MAX_ORIG && b->orig[idx].used) ? &b->orig[idx] : NULL;
}

static void cand_free(struct bat *b, struct bat_cand *c)
{
    if (!c->used) {
        return;
    }
    struct bat_neigh *n = &b->neigh[c->neigh];
    if (n->used && n->refs > 0) {
        n->refs--;
    }
    memset(c, 0, sizeof(*c));
}

/* Tells TT and fragments, releases the candidates; neighbours left without one go at the purge. */
static void orig_free(struct bat *b, struct bat_orig *o)
{
    bat_tt_orig_gone(b, o);
    bat_frag_orig_gone(b, bat_orig_index(b, o));
    for (unsigned c = 0; c < BAT_CANDS_PER_ORIG; c++) {
        cand_free(b, &o->cand[c]);
    }
    memset(o, 0, sizeof(*o));
    BAT_INC(b, ORIG_PURGED);
}

static bool is_neigh_orig(const struct bat *b, const struct bat_orig *o)
{
    for (unsigned i = 0; i < BAT_MAX_NEIGH; i++) {
        if (b->neigh[i].used && bat_mac_eq(b->neigh[i].orig, o->addr)) {
            return true;
        }
    }
    return false;
}

/* Lowest default-route throughput among originators that are no neighbour's own, oldest on a tie. */
static int worst_routed(struct bat *b)
{
    int w = -1;
    for (unsigned i = 0; i < BAT_MAX_ORIG; i++) {
        const struct bat_orig *x = &b->orig[i];
        if (!x->used || is_neigh_orig(b, x)) {
            continue;
        }
        if (w < 0) {
            w = (int)i;
            continue;
        }
        uint32_t t = bat_route_tput(b, x), tw = bat_route_tput(b, &b->orig[w]);
        if (t < tw || (t == tw && bat_age(b, x->last_seen) > bat_age(b, b->orig[w].last_seen))) {
            w = (int)i;
        }
    }
    return w;
}

/* @for_neigh: a direct neighbour's originator may displace the worst-routed other one. */
static struct bat_orig *orig_get(struct bat *b, const uint8_t addr[6], bool for_neigh)
{
    struct bat_orig *o = bat_orig_find(b, addr);
    if (o) {
        return o;
    }
    int slot = -1, old = -1;
    for (unsigned i = 0; i < BAT_MAX_ORIG; i++) {
        struct bat_orig *x = &b->orig[i];
        if (!x->used) {
            slot = (int)i;
            break;
        }
        if (x->tbl[BAT_TBL_DEFAULT].router >= 0 || bat_age(b, x->last_seen) <= BAT_EVICT_AGE_MS) {
            continue;
        }
        if (old < 0 || bat_age(b, x->last_seen) > bat_age(b, b->orig[old].last_seen)) {
            old = (int)i;
        }
    }
    if (slot < 0 && old < 0 && for_neigh) {
        old = worst_routed(b);
    }
    if (slot < 0) {
        if (old < 0) {
            BAT_INC(b, ORIG_FULL);
            return NULL;
        }
        orig_free(b, &b->orig[old]);
        slot = old;
    }
    o = &b->orig[slot];
    memset(o, 0, sizeof(*o));
    memcpy(o->addr, addr, 6);
    o->used = 1;
    o->last_seen = b->now;
    for (unsigned t = 0; t < BAT_NTBL; t++) {
        o->tbl[t].router = -1;
    }
    BAT_INC(b, ORIG_NEW);
    return o;
}

struct bat_orig *bat_orig_get(struct bat *b, const uint8_t addr[6])
{
    return orig_get(b, addr, false);
}

struct bat_orig *bat_orig_get_neigh(struct bat *b, const uint8_t addr[6])
{
    return orig_get(b, addr, true);
}

struct bat_cand *bat_cand_get(struct bat *b, struct bat_orig *o, unsigned neigh_idx)
{
    int slot = -1, old = -1;
    for (unsigned i = 0; i < BAT_CANDS_PER_ORIG; i++) {
        struct bat_cand *c = &o->cand[i];
        if (c->used && c->neigh == neigh_idx) {
            return c;
        }
    }
    for (unsigned i = 0; i < BAT_CANDS_PER_ORIG; i++) {
        struct bat_cand *c = &o->cand[i];
        if (!c->used) {
            slot = (int)i;
            break;
        }
        if (o->tbl[BAT_TBL_DEFAULT].router == (int)i || o->tbl[BAT_TBL_IFACE].router == (int)i) {
            continue;
        }
        if (old < 0 || bat_age(b, c->last_seen) > bat_age(b, o->cand[old].last_seen)) {
            old = (int)i;
        }
    }
    if (slot < 0) {
        if (old < 0) {
            BAT_INC(b, CAND_FULL);
            return NULL;
        }
        cand_free(b, &o->cand[old]);
        slot = old;
    }
    struct bat_cand *c = &o->cand[slot];
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->neigh = (uint8_t)neigh_idx;
    c->last_seen = b->now;
    b->neigh[neigh_idx].refs++;
    return c;
}

const uint8_t *bat_route_nh(struct bat *b, const struct bat_orig *o, int tbl)
{
    if (!o || tbl < 0 || tbl >= BAT_NTBL) {
        return NULL;
    }
    int r = o->tbl[tbl].router;
    if (r < 0 || !o->cand[r].used || !b->neigh[o->cand[r].neigh].used) {
        return NULL;
    }
    return b->neigh[o->cand[r].neigh].addr;
}

uint32_t bat_route_tput(const struct bat *b, const struct bat_orig *o)
{
    (void)b;
    int r = o ? o->tbl[BAT_TBL_DEFAULT].router : -1;
    if (r < 0 || !o->cand[r].used) {
        return 0;
    }
    return o->cand[r].t[BAT_TBL_DEFAULT].tput;
}

void bat_orig_recompute(struct bat *b, struct bat_orig *o)
{
    bool lost = false;
    for (unsigned t = 0; t < BAT_NTBL; t++) {
        int best = -1;
        for (unsigned i = 0; i < BAT_CANDS_PER_ORIG; i++) {
            const struct bat_cand *c = &o->cand[i];
            if (!c->used || !c->t[t].valid) {
                continue;
            }
            if (best < 0 || c->t[t].tput > o->cand[best].t[t].tput) {
                best = (int)i;
            }
        }
        if (o->tbl[t].router >= 0 && best < 0) {
            BAT_INC(b, ROUTE_LOST);
            lost = true;
        }
        o->tbl[t].router = (int8_t)best;
    }
    if (lost) {
        bat_tt_orig_gone(b, o);
    }
}

void bat_orig_purge(struct bat *b)
{
    for (unsigned i = 0; i < BAT_MAX_ORIG; i++) {
        struct bat_orig *o = &b->orig[i];
        if (!o->used) {
            continue;
        }
        if (bat_age(b, o->last_seen) > BAT_ORIG_TIMEOUT_MS) {
            orig_free(b, o);
            continue;
        }
        bool del = false;
        for (unsigned c = 0; c < BAT_CANDS_PER_ORIG; c++) {
            if (o->cand[c].used && bat_age(b, o->cand[c].last_seen) > BAT_CAND_TIMEOUT_MS) {
                cand_free(b, &o->cand[c]);
                del = true;
            }
        }
        if (del) {
            bat_orig_recompute(b, o);
        }
        for (unsigned t = 0; t < BAT_NTBL; t++) {
            if (o->tbl[t].restart_valid && bat_age(b, o->tbl[t].restart_ts) > BAT_STAMP_CLEAR_MS) {
                o->tbl[t].restart_valid = 0;
            }
        }
        if (o->bcast.reset_valid && bat_age(b, o->bcast.reset_ts) > BAT_STAMP_CLEAR_MS) {
            o->bcast.reset_valid = 0;
        }
    }
    for (unsigned i = 0; i < BAT_MAX_NEIGH; i++) {
        struct bat_neigh *n = &b->neigh[i];
        if (!n->used) {
            continue;
        }
        if (n->refs == 0) {
            memset(n, 0, sizeof(*n));
            BAT_INC(b, NEIGH_PURGED);
        } else if (bat_age(b, n->last_seen) > 0x40000000u) {
            n->last_seen = b->now - 0x40000000u;   /* keep the age from aliasing after 2^31 ms */
        }
    }
    bat_frag_purge(b);
}
