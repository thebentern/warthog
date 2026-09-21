/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 */
#include "umac_mesh_hwmp_relay.h"

#include <string.h>

static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static bool eq_(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

static void none_(struct umac_mesh_hwmp_action *out, enum umac_mesh_hwmp_drop *why,
                  enum umac_mesh_hwmp_drop reason)
{
    if (out != NULL) { out->kind = UMAC_MESH_HWMP_NONE; out->body_len = 0; }
    if (why != NULL) { *why = reason; }
}

static void preq_(const struct umac_mesh_hwmp_ctx *c, const struct hwmp_preq *q,
                  const uint8_t *ta, struct umac_mesh_hwmp_action *out,
                  enum umac_mesh_hwmp_drop *why)
{
    if (eq_(q->orig_addr, c->own_addr))
    {
        none_(out, why, UMAC_MESH_HWMP_DROP_OWN);
        return;
    }
    /* Path to the ORIGINATOR is via whoever handed us the frame. Not fresh
     * means we have already seen this request as good or better: neither
     * answer nor forward, which is the duplicate suppression. */
    bool fresh = umac_mesh_path_update(c->tbl, q->orig_addr, ta, q->orig_sn,
                                       q->metric + c->link_metric, (uint8_t)(q->hop_count + 1u),
                                       c->path_lifetime_ms, c->now_ms);
    if (!fresh)
    {
        none_(out, why, UMAC_MESH_HWMP_DROP_NOT_FRESH);
        return;
    }
    if (umac_mesh_hwmp_targets_us(q, c->own_addr))
    {
        *c->own_sn = umac_mesh_hwmp_next_own_sn(*c->own_sn, q);
        uint16_t n = umac_mesh_hwmp_build_prep(out->body, sizeof(out->body), q, c->own_addr,
                                               *c->own_sn);
        if (n == 0) { none_(out, why, UMAC_MESH_HWMP_DROP_PARSE); return; }
        out->kind = UMAC_MESH_HWMP_SEND_PREP;
        out->body_len = n;
        memcpy(out->to, ta, 6);
        if (why != NULL) { *why = UMAC_MESH_HWMP_DROP_NONE; }
        return;
    }
    if (!c->forwarding)
    {
        none_(out, why, UMAC_MESH_HWMP_DROP_NO_FWD);
        return;
    }
    /* The relay transform refuses ttl <= 1 itself. Metric goes out with our
     * hop added, so the next node accumulates the whole path cost. */
    struct hwmp_preq fwd = *q;
    fwd.metric += c->link_metric;
    uint16_t n = umac_mesh_hwmp_build_preq_fwd(out->body, sizeof(out->body), &fwd, 0);
    if (n == 0)
    {
        none_(out, why, UMAC_MESH_HWMP_DROP_TTL);
        return;
    }
    out->kind = UMAC_MESH_HWMP_REBROADCAST_PREQ;
    out->body_len = n;
    memcpy(out->to, BCAST, 6);
    if (why != NULL) { *why = UMAC_MESH_HWMP_DROP_NONE; }
}

static void prep_(const struct umac_mesh_hwmp_ctx *c, const struct hwmp_prep *p,
                  const uint8_t *ta, struct umac_mesh_hwmp_action *out,
                  enum umac_mesh_hwmp_drop *why)
{
    if (eq_(p->target_addr, c->own_addr))
    {
        none_(out, why, UMAC_MESH_HWMP_DROP_OWN);
        return;
    }
    /* Path to the TARGET (the answerer) is via the transmitter. */
    bool fresh = umac_mesh_path_update(c->tbl, p->target_addr, ta, p->target_sn,
                                       p->metric + c->link_metric, (uint8_t)(p->hop_count + 1u),
                                       c->path_lifetime_ms, c->now_ms);
    if (eq_(p->orig_addr, c->own_addr))
    {
        /* Our own request answered; the path is installed, nothing to send. */
        none_(out, why, fresh ? UMAC_MESH_HWMP_DROP_NONE : UMAC_MESH_HWMP_DROP_NOT_FRESH);
        return;
    }
    if (!fresh)
    {
        none_(out, why, UMAC_MESH_HWMP_DROP_NOT_FRESH);
        return;
    }
    if (!c->forwarding)
    {
        none_(out, why, UMAC_MESH_HWMP_DROP_NO_FWD);
        return;
    }
    /* Carry it back toward the originator along the path its PREQ built. */
    const struct umac_mesh_path *back = umac_mesh_path_lookup(c->tbl, p->orig_addr, c->now_ms);
    if (back == NULL)
    {
        none_(out, why, UMAC_MESH_HWMP_DROP_NO_PATH);
        return;
    }
    struct hwmp_prep fwd = *p;
    fwd.metric += c->link_metric;
    uint16_t n = umac_mesh_hwmp_build_prep_fwd(out->body, sizeof(out->body), &fwd, 0);
    if (n == 0)
    {
        none_(out, why, UMAC_MESH_HWMP_DROP_TTL);
        return;
    }
    out->kind = UMAC_MESH_HWMP_FORWARD_PREP;
    out->body_len = n;
    memcpy(out->to, back->next_hop, 6);
    if (why != NULL) { *why = UMAC_MESH_HWMP_DROP_NONE; }
}

static void perr_(const struct umac_mesh_hwmp_ctx *c, const struct hwmp_perr *e,
                  const uint8_t *ta, struct umac_mesh_hwmp_action *out,
                  enum umac_mesh_hwmp_drop *why)
{
    /* Only a PERR from our next hop for that destination counts, and only if
     * it deactivated something do we pass it on -- else two relays echo it. */
    bool changed = umac_mesh_path_invalidate(c->tbl, e->dest_addr, e->dest_sn, ta, c->now_ms);
    if (!changed)
    {
        none_(out, why, UMAC_MESH_HWMP_DROP_UNCHANGED);
        return;
    }
    if (!c->forwarding)
    {
        none_(out, why, UMAC_MESH_HWMP_DROP_NO_FWD);
        return;
    }
    if (e->ttl <= 1u)
    {
        none_(out, why, UMAC_MESH_HWMP_DROP_TTL);
        return;
    }
    uint16_t n = umac_mesh_hwmp_build_perr(out->body, sizeof(out->body), (uint8_t)(e->ttl - 1u),
                                           e->dest_addr, e->dest_sn, e->reason);
    if (n == 0) { none_(out, why, UMAC_MESH_HWMP_DROP_PARSE); return; }
    out->kind = UMAC_MESH_HWMP_FORWARD_PERR;
    out->body_len = n;
    memcpy(out->to, BCAST, 6);
    if (why != NULL) { *why = UMAC_MESH_HWMP_DROP_NONE; }
}

uint8_t umac_mesh_hwmp_relay(const struct umac_mesh_hwmp_ctx *c, const uint8_t *body,
                             uint16_t len, const uint8_t *ta,
                             struct umac_mesh_hwmp_action *out,
                             enum umac_mesh_hwmp_drop *why)
{
    if (c == NULL || c->own_addr == NULL || c->tbl == NULL || c->own_sn == NULL ||
        body == NULL || ta == NULL || out == NULL)
    {
        none_(out, why, UMAC_MESH_HWMP_DROP_PARSE);
        return 0;
    }
    uint8_t eid = umac_mesh_hwmp_element_id(body, len);
    switch (eid)
    {
        case HWMP_EID_PREQ:
        {
            struct hwmp_preq q;
            if (!umac_mesh_hwmp_parse_preq(body, len, &q))
            {
                none_(out, why, UMAC_MESH_HWMP_DROP_PARSE);
                return 0;
            }
            preq_(c, &q, ta, out, why);
            return eid;
        }
        case HWMP_EID_PREP:
        {
            struct hwmp_prep p;
            if (!umac_mesh_hwmp_parse_prep(body, len, &p))
            {
                none_(out, why, UMAC_MESH_HWMP_DROP_PARSE);
                return 0;
            }
            prep_(c, &p, ta, out, why);
            return eid;
        }
        case HWMP_EID_PERR:
        {
            struct hwmp_perr e;
            if (!umac_mesh_hwmp_parse_perr(body, len, &e))
            {
                none_(out, why, UMAC_MESH_HWMP_DROP_PARSE);
                return 0;
            }
            perr_(c, &e, ta, out, why);
            return eid;
        }
        default:
            none_(out, why, UMAC_MESH_HWMP_DROP_PARSE);
            return 0;
    }
}

uint32_t umac_mesh_hwmp_lose_neighbour(const struct umac_mesh_hwmp_ctx *c,
                                       const uint8_t *neighbour,
                                       struct umac_mesh_hwmp_action *out, uint32_t max)
{
    if (c == NULL || c->tbl == NULL || neighbour == NULL)
    {
        return 0;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < UMAC_MESH_PATH_MAX; i++)
    {
        struct umac_mesh_path *e = &c->tbl->p[i];
        if (!e->used || !(e->flags & UMAC_MESH_PATH_ACTIVE) || !eq_(e->next_hop, neighbour))
        {
            continue;
        }
        e->flags &= (uint8_t)~UMAC_MESH_PATH_ACTIVE;
        if (c->forwarding && out != NULL && n < max)
        {
            struct umac_mesh_hwmp_action *a = &out[n];
            uint16_t l = umac_mesh_hwmp_build_perr(a->body, sizeof(a->body), HWMP_DEFAULT_TTL,
                                                   e->dst, e->sn,
                                                   HWMP_REASON_MESH_PATH_ERROR_DEST_UNREACHABLE);
            if (l != 0)
            {
                a->kind = UMAC_MESH_HWMP_FORWARD_PERR;
                a->body_len = l;
                memcpy(a->to, BCAST, 6);
                n++;
            }
        }
    }
    return n;
}
