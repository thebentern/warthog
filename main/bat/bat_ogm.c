/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bat_internal.h"

enum { UPD_IGNORE, UPD_NEW, UPD_SAME };

static size_t agg_cap(const struct bat *b)
{
    return b->cfg.hard_mtu < BAT_OGM_MAX_OWN ? b->cfg.hard_mtu : BAT_OGM_MAX_OWN;
}

void bat_ogm_flush(struct bat *b)
{
    if (b->agg_len == 0) {
        return;
    }
    bat_link_tx(b, bat_bcast_addr, b->agg, BAT_ETH_HLEN + (size_t)b->agg_len);
    BAT_INC(b, OGM_TX_FRAMES);
    b->agg_len = 0;
}

/* Room for one record at the tail of the queue, flushing first if it would pass the cap. */
static uint8_t *ogm_queue(struct bat *b, size_t len)
{
    if (len > BAT_AGG_BUF) {
        BAT_INC(b, OGM_TOOBIG);
        return NULL;
    }
    if (b->agg_len > 0 && b->agg_len + len > agg_cap(b)) {
        bat_ogm_flush(b);
    }
    uint8_t *p = b->agg + BAT_ETH_HLEN + b->agg_len;
    b->agg_len = (uint16_t)(b->agg_len + len);
    return p;
}

static void ogm_queued(struct bat *b)
{
    if (!b->cfg.aggregate_ogm) {
        bat_ogm_flush(b);
    }
}

void bat_ogm_own(struct bat *b)
{
    uint8_t *r = b->ogm_own;
    r[BAT_OFF_TYPE] = BAT_PT_OGM2;
    r[BAT_OFF_VERSION] = BAT_COMPAT;
    r[BAT_OGM_TTL] = BAT_OGM_TTL_INIT;
    r[BAT_OGM_FLAGS] = 0;
    bat_put32(r + BAT_OGM_SEQ, b->ogm_seq);
    memcpy(r + BAT_OGM_ORIG, b->cfg.hard_addr, 6);
    bat_put32(r + BAT_OGM_TPUT, 0xFFFFFFFFu);
    size_t tl = bat_tt_ogm_tvlv_build(b, r + BAT_OGM_HLEN, BAT_OGM_MAX_OWN - BAT_OGM_HLEN);
    if (tl > BAT_OGM_MAX_OWN - BAT_OGM_HLEN) {
        tl = 0;
    }
    bat_put16(r + BAT_OGM_TVLV_LEN, (uint16_t)tl);
    b->ogm_seq++;
    bat_seq_note(b);
    if (bat_neigh_count(b) == 0) {
        BAT_INC(b, OGM_SUPP_OWN);
        return;
    }
    uint8_t *q = ogm_queue(b, BAT_OGM_HLEN + tl);
    if (!q) {
        return;
    }
    memcpy(q, r, BAT_OGM_HLEN + tl);
    BAT_INC(b, OGM_TX_OWN);
    ogm_queued(b);
}

bool bat_flood_suppressed(struct bat *b, const uint8_t orig[6], const uint8_t *from_link_src)
{
    unsigned cnt = 0;
    const struct bat_neigh *only = NULL;
    for (unsigned i = 0; i < BAT_MAX_NEIGH; i++) {
        if (b->neigh[i].used) {
            cnt++;
            only = &b->neigh[i];
        }
    }
    if (cnt == 0) {
        return true;
    }
    if (cnt > 1) {
        return false;
    }
    if (bat_mac_eq(only->orig, orig)) {
        return true;
    }
    if (from_link_src) {
        const struct bat_neigh *f = bat_neigh_find(b, from_link_src);
        if (f && bat_mac_eq(only->orig, f->orig)) {
            return true;
        }
    }
    return false;
}

static void select_router(struct bat_orig *o, unsigned ci, unsigned t)
{
    int r = o->tbl[t].router;
    if (r == (int)ci) {
        return;
    }
    if (r >= 0 && o->cand[r].used && o->cand[r].t[t].valid) {
        const struct bat_cand_tbl *R = &o->cand[r].t[t], *C = &o->cand[ci].t[t];
        int32_t gap = (int32_t)(C->seq - R->seq);
        if (gap < BAT_ROUTE_SWITCH_GAP && R->tput >= C->tput) {
            return;
        }
    }
    o->tbl[t].router = (int8_t)ci;
}

/* elp-ogm §3.5 sequence rule and store, then §3.7 router selection, for one table. */
static int table_update(struct bat *b, struct bat_orig *o, unsigned ci, unsigned t, uint32_t seq,
                        uint8_t ttl, uint32_t tput)
{
    struct bat_otbl *tb = &o->tbl[t];
    struct bat_cand *c = &o->cand[ci];
    int32_t d = (int32_t)(seq - tb->last_seq);
    bool is_new = d > 0;
    if (d <= -BAT_SEQ_WINDOW || d >= BAT_SEQ_MAX_JUMP) {
        if (tb->restart_valid && bat_age(b, tb->restart_ts) < BAT_RESTART_MS) {
            BAT_INC(b, OGM_RESTART_BLOCKED);
            return UPD_IGNORE;
        }
        tb->restart_ts = b->now;
        tb->restart_valid = 1;
        BAT_INC(b, OGM_RESTART);
        is_new = true;
    } else if (d < 0) {
        BAT_INC(b, OGM_OLD);
        return UPD_IGNORE;
    }
    c->last_seen = b->now;
    o->last_seen = b->now;
    tb->last_seq = seq;
    tb->last_ttl = ttl;
    c->t[t].tput = tput;
    c->t[t].seq = seq;
    c->t[t].ttl = ttl;
    c->t[t].valid = 1;
    if (is_new) {
        BAT_INC(b, OGM_NEW);
    } else {
        BAT_INC(b, OGM_SAME);
    }
    select_router(o, ci, t);
    return is_new ? UPD_NEW : UPD_SAME;
}

static void process_tvlv(struct bat *b, struct bat_orig *o, const uint8_t *area, size_t len)
{
    struct bat_tvlv t;
    size_t off = 0;
    bool gw = false, tt = false;
    while (bat_tvlv_next(area, len, &off, &t) == BAT_TVLV_OK) {
        if (t.version != 1) {
            continue;
        }
        if (t.type == BAT_TVLV_TT) {
            if (!tt) {                     /* only the first: a legitimate OGM2 carries one */
                tt = true;
                bat_tt_ogm_rx(b, o, t.val, t.len);
            }
        } else if (t.type == BAT_TVLV_GW) {
            uint32_t down = t.len >= BAT_TVLV_GW_LEN ? bat_get32(t.val) : 0;
            uint32_t up = t.len >= BAT_TVLV_GW_LEN ? bat_get32(t.val + 4) : 0;
            gw = down != 0 && up != 0;
            o->gw_down = gw ? down : 0;
            o->gw_up = gw ? up : 0;
        }
    }
    o->gw_valid = gw;
    if (!gw) {
        o->gw_down = o->gw_up = 0;
    }
}

static void forward(struct bat *b, struct bat_orig *o, unsigned ci, const uint8_t *rec, size_t rl)
{
    struct bat_otbl *tb = &o->tbl[BAT_TBL_IFACE];
    uint32_t seq = bat_get32(rec + BAT_OGM_SEQ);
    uint8_t ttl = rec[BAT_OGM_TTL];
    uint32_t tput = o->cand[ci].t[BAT_TBL_IFACE].tput;
    if (tb->router != (int)ci || tb->last_fwd_seq == seq) {
        return;
    }
    tb->last_fwd_seq = seq;
    if (ttl <= 1) {
        BAT_INC(b, OGM_FWD_TTL);
        return;
    }
    if (tput == 0) {
        BAT_INC(b, OGM_FWD_TPUT0);
        return;
    }
    if (rl > b->cfg.hard_mtu) {            /* dataplane §9.3: nothing unfragmented above it */
        BAT_INC(b, OGM_TOOBIG);
        return;
    }
    uint8_t *q = ogm_queue(b, rl);
    if (!q) {
        return;
    }
    memcpy(q, rec, rl);
    q[BAT_OGM_TTL] = (uint8_t)(ttl - 1);
    bat_put32(q + BAT_OGM_TPUT, tput);
    BAT_INC(b, OGM_TX_FWD);
    ogm_queued(b);
}

static uint32_t iface_tput(const struct bat *b, uint32_t p)
{
    if (b->cfg.half_duplex && p > BAT_TPUT_DEFAULT) {
        return p / 2;
    }
    return (uint32_t)((uint64_t)p * (255u - b->cfg.hop_penalty) / 255u);
}

static void ogm_record(struct bat *b, const uint8_t *rec, size_t rl, struct bat_neigh *n)
{
    const uint8_t *orig = rec + BAT_OGM_ORIG;
    uint32_t adv = bat_get32(rec + BAT_OGM_TPUT);
    BAT_INC(b, OGM_REC);
    if (bat_is_own(b, orig)) {
        BAT_INC(b, OGM_OWN);
        return;
    }
    if (adv == 0) {
        BAT_INC(b, OGM_TPUT0);
        return;
    }
    if (!n) {
        BAT_INC(b, OGM_NOT_NEIGH);
        return;
    }
    struct bat_orig *o = bat_orig_get(b, orig);
    if (!o) {
        return;
    }
    struct bat_cand *c = bat_cand_get(b, o, (unsigned)(n - b->neigh));
    if (!c) {
        return;
    }
    unsigned ci = (unsigned)(c - o->cand);
    uint32_t link = bat_neigh_tput(n);
    uint32_t p = link < adv ? link : adv;
    uint32_t seq = bat_get32(rec + BAT_OGM_SEQ);
    uint8_t ttl = rec[BAT_OGM_TTL];
    if (table_update(b, o, ci, BAT_TBL_DEFAULT, seq, ttl, p) == UPD_NEW) {
        process_tvlv(b, o, rec + BAT_OGM_HLEN, rl - BAT_OGM_HLEN);
    }
    if (!o->used || !o->cand[ci].used) {
        return;
    }
    if (bat_flood_suppressed(b, orig, n->addr)) {
        return;
    }
    if (table_update(b, o, ci, BAT_TBL_IFACE, seq, ttl, iface_tput(b, p)) != UPD_IGNORE) {
        forward(b, o, ci, rec, rl);
    }
}

void bat_ogm_rx(struct bat *b, uint8_t *frame, size_t len)
{
    struct bat_neigh *n = bat_neigh_find(b, frame + BAT_LINK_SRC);
    size_t off = BAT_ETH_HLEN;
    BAT_INC(b, OGM_RX);
    while (len - off >= BAT_OGM_HLEN) {
        const uint8_t *rec = frame + off;
        if (off > BAT_ETH_HLEN && (rec[BAT_OFF_TYPE] != BAT_PT_OGM2 ||
                                   rec[BAT_OFF_VERSION] != BAT_COMPAT)) {
            BAT_INC(b, OGM_BADREC);
            return;
        }
        size_t rl = BAT_OGM_HLEN + (size_t)bat_get16(rec + BAT_OGM_TVLV_LEN);
        if (rl > len - off) {
            BAT_INC(b, OGM_OVERRUN);
            return;
        }
        ogm_record(b, rec, rl, n);
        off += rl;
    }
}
