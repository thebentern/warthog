/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 */
#include "umac_mesh_pathtbl.h"
#include "umac_mesh_hwmp.h" /* hwmp_sn_gt */

#include <string.h>

static bool past_(uint32_t now_ms, uint32_t t_ms)
{
    return (int32_t)(now_ms - t_ms) >= 0;
}

static bool eq_(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

void umac_mesh_pathtbl_init(struct umac_mesh_pathtbl *t)
{
    if (t != NULL)
    {
        memset(t, 0, sizeof(*t));
    }
}

static struct umac_mesh_path *find_(struct umac_mesh_pathtbl *t, const uint8_t *dst)
{
    for (uint32_t i = 0; i < UMAC_MESH_PATH_MAX; i++)
    {
        if (t->p[i].used && eq_(t->p[i].dst, dst))
        {
            return &t->p[i];
        }
    }
    return NULL;
}

/* A free slot, else an expired one, else the one expiring soonest. */
static struct umac_mesh_path *alloc_(struct umac_mesh_pathtbl *t, uint32_t now_ms)
{
    struct umac_mesh_path *victim = NULL;
    for (uint32_t i = 0; i < UMAC_MESH_PATH_MAX; i++)
    {
        struct umac_mesh_path *e = &t->p[i];
        if (!e->used)
        {
            return e;
        }
        if (past_(now_ms, e->exp_ms))
        {
            return e;
        }
        if (victim == NULL || (int32_t)(e->exp_ms - victim->exp_ms) < 0)
        {
            victim = e;
        }
    }
    return victim;
}

const struct umac_mesh_path *umac_mesh_path_lookup(const struct umac_mesh_pathtbl *t,
                                                   const uint8_t *dst, uint32_t now_ms)
{
    if (t == NULL || dst == NULL)
    {
        return NULL;
    }
    for (uint32_t i = 0; i < UMAC_MESH_PATH_MAX; i++)
    {
        const struct umac_mesh_path *e = &t->p[i];
        if (e->used && (e->flags & UMAC_MESH_PATH_ACTIVE) && !past_(now_ms, e->exp_ms) &&
            eq_(e->dst, dst))
        {
            return e;
        }
    }
    return NULL;
}

bool umac_mesh_path_update(struct umac_mesh_pathtbl *t, const uint8_t *dst,
                           const uint8_t *next_hop, uint32_t sn, uint32_t metric,
                           uint8_t hop_count, uint32_t lifetime_ms, uint32_t now_ms)
{
    if (t == NULL || dst == NULL || next_hop == NULL)
    {
        return false;
    }
    struct umac_mesh_path *e = find_(t, dst);
    /* mac80211 hwmp_route_info_get: only an ACTIVE path with a valid sn can
     * reject new information -- not fresh if ours is newer, or equal and this
     * is no better. A path a PERR deactivated takes the next advertisement.
     * A different next hop must beat ours by ~10 % (metric * 10 / 9 >= ours)
     * or the route flaps between equals. */
    if (e != NULL && (e->flags & UMAC_MESH_PATH_ACTIVE) &&
        (e->flags & UMAC_MESH_PATH_SN_VALID) && !past_(now_ms, e->exp_ms))
    {
        if (hwmp_sn_gt(e->sn, sn))
        {
            return false;
        }
        if (e->sn == sn)
        {
            uint32_t cmp = eq_(e->next_hop, next_hop) ? metric
                                                      : (uint32_t)(((uint64_t)metric * 10u) / 9u);
            if (cmp >= e->metric)
            {
                return false;
            }
        }
    }
    if (e == NULL)
    {
        e = alloc_(t, now_ms);
        if (e == NULL)
        {
            return false;
        }
        memset(e, 0, sizeof(*e));
        memcpy(e->dst, dst, 6);
        e->used = true;
    }
    memcpy(e->next_hop, next_hop, 6);
    e->sn = sn;
    e->metric = metric;
    e->hop_count = hop_count;
    e->flags = UMAC_MESH_PATH_ACTIVE | UMAC_MESH_PATH_SN_VALID;
    /* Only ever extend: a shorter lifetime in a later frame does not cut a
     * path off early. */
    uint32_t exp = now_ms + lifetime_ms;
    if (!e->used || past_(now_ms, e->exp_ms) || (int32_t)(exp - e->exp_ms) > 0)
    {
        e->exp_ms = exp;
    }
    return true;
}

bool umac_mesh_path_invalidate(struct umac_mesh_pathtbl *t, const uint8_t *dst,
                               uint32_t sn, const uint8_t *from, uint32_t now_ms)
{
    if (t == NULL || dst == NULL)
    {
        return false;
    }
    struct umac_mesh_path *e = find_(t, dst);
    if (e == NULL || !(e->flags & UMAC_MESH_PATH_ACTIVE) || past_(now_ms, e->exp_ms))
    {
        return false;
    }
    if (from != NULL && !eq_(e->next_hop, from))
    {
        return false; /* not from our next hop for this destination */
    }
    /* mac80211 mesh_path_error rx: act when we hold no valid sn, the PERR's
     * is newer, or it is 0 (unknown). An OLDER sn is a stale error and is
     * ignored, so a delayed PERR cannot kill a path that was since rebuilt. */
    if ((e->flags & UMAC_MESH_PATH_SN_VALID) && sn != 0u && !hwmp_sn_gt(sn, e->sn))
    {
        return false;
    }
    e->flags &= (uint8_t)~UMAC_MESH_PATH_ACTIVE;
    if (sn != 0u)
    {
        e->sn = sn;
        e->flags |= UMAC_MESH_PATH_SN_VALID;
    }
    return true;
}

uint32_t umac_mesh_path_lose_next_hop(struct umac_mesh_pathtbl *t, const uint8_t *next_hop)
{
    if (t == NULL || next_hop == NULL)
    {
        return 0;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < UMAC_MESH_PATH_MAX; i++)
    {
        struct umac_mesh_path *e = &t->p[i];
        if (e->used && (e->flags & UMAC_MESH_PATH_ACTIVE) && eq_(e->next_hop, next_hop))
        {
            e->flags &= (uint8_t)~UMAC_MESH_PATH_ACTIVE;
            n++;
        }
    }
    return n;
}

void umac_mesh_proxy_learn(struct umac_mesh_pathtbl *t, const uint8_t *ext,
                           const uint8_t *mesh_sta, uint32_t now_ms)
{
    if (t == NULL || ext == NULL || mesh_sta == NULL)
    {
        return;
    }
    struct umac_mesh_proxy *slot = NULL, *victim = NULL;
    for (uint32_t i = 0; i < UMAC_MESH_PROXY_MAX; i++)
    {
        struct umac_mesh_proxy *x = &t->x[i];
        if (x->used && eq_(x->ext, ext))
        {
            slot = x; /* re-learn: a host may move behind another node */
            break;
        }
        if (!x->used || past_(now_ms, x->exp_ms))
        {
            if (slot == NULL)
            {
                slot = x;
            }
        }
        else if (victim == NULL || (int32_t)(x->exp_ms - victim->exp_ms) < 0)
        {
            victim = x;
        }
    }
    if (slot == NULL)
    {
        slot = victim;
    }
    if (slot == NULL)
    {
        return;
    }
    memcpy(slot->ext, ext, 6);
    memcpy(slot->mesh_sta, mesh_sta, 6);
    slot->exp_ms = now_ms + UMAC_MESH_PROXY_LIFETIME_MS;
    slot->used = true;
}

const uint8_t *umac_mesh_proxy_lookup(const struct umac_mesh_pathtbl *t, const uint8_t *ext,
                                      uint32_t now_ms)
{
    if (t == NULL || ext == NULL)
    {
        return NULL;
    }
    for (uint32_t i = 0; i < UMAC_MESH_PROXY_MAX; i++)
    {
        const struct umac_mesh_proxy *x = &t->x[i];
        if (x->used && !past_(now_ms, x->exp_ms) && eq_(x->ext, ext))
        {
            return x->mesh_sta;
        }
    }
    return NULL;
}

uint32_t umac_mesh_path_count(const struct umac_mesh_pathtbl *t, uint32_t now_ms)
{
    uint32_t n = 0;
    if (t == NULL) { return 0; }
    for (uint32_t i = 0; i < UMAC_MESH_PATH_MAX; i++)
    {
        const struct umac_mesh_path *e = &t->p[i];
        if (e->used && (e->flags & UMAC_MESH_PATH_ACTIVE) && !past_(now_ms, e->exp_ms)) { n++; }
    }
    return n;
}

uint32_t umac_mesh_proxy_count(const struct umac_mesh_pathtbl *t, uint32_t now_ms)
{
    uint32_t n = 0;
    if (t == NULL) { return 0; }
    for (uint32_t i = 0; i < UMAC_MESH_PROXY_MAX; i++)
    {
        if (t->x[i].used && !past_(now_ms, t->x[i].exp_ms)) { n++; }
    }
    return n;
}
