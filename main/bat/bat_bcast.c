/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bat_internal.h"

enum { WIN_ACCEPT, WIN_DUP, WIN_BLOCKED };

/* dataplane §5.2 step 6: 64-entry history below the newest seqno, 30 s restart protection. */
static int window(struct bat *b, struct bat_bwin *w, uint32_t s)
{
    int32_t d = (int32_t)(w->last - s);
    if (d >= 0 && d < BAT_SEQ_WINDOW && (w->bits >> d) & 1u) {
        return WIN_DUP;
    }
    int32_t g = (int32_t)(s - w->last);
    if (g <= -BAT_SEQ_WINDOW || g >= BAT_SEQ_MAX_JUMP) {
        if (w->reset_valid && bat_age(b, w->reset_ts) < BAT_RESTART_MS) {
            return WIN_BLOCKED;
        }
        w->reset_ts = b->now;
        w->reset_valid = 1;
    }
    if (g > -BAT_SEQ_WINDOW && g <= 0) {
        w->bits |= (uint64_t)1 << (unsigned)(-g);
    } else if (g > 0 && g < BAT_SEQ_WINDOW) {
        w->bits = (w->bits << (unsigned)g) | 1u;
        w->last = s;
    } else {
        w->bits = 1;
        w->last = s;
    }
    return WIN_ACCEPT;
}

static bool older(const struct bat_bc_copy *x, const struct bat_bc_copy *y)
{
    return (int32_t)(x->ord - y->ord) < 0;
}

/* The next copy to send: the broadcast with the most copies left, the oldest on a tie. */
static struct bat_bc_copy *next_copy(struct bat *b)
{
    struct bat_bc_copy *n = NULL;
    for (unsigned i = 0; i < BAT_BC_COPY_SLOTS; i++) {
        struct bat_bc_copy *c = &b->bc_copy[i];
        if (c->left && (!n || c->left > n->left || (c->left == n->left && older(c, n)))) {
            n = c;
        }
    }
    return n;
}

/* A free slot, else that of the broadcast sent most (oldest on a tie), whose other copies are given
 * up; NULL when no waiting broadcast has been sent yet. */
static struct bat_bc_copy *slot_get(struct bat *b)
{
    struct bat_bc_copy *v = NULL;
    for (unsigned i = 0; i < BAT_BC_COPY_SLOTS; i++) {
        struct bat_bc_copy *c = &b->bc_copy[i];
        if (c->left == 0) {
            return c;
        }
        if (c->left < b->cfg.bcast_copies &&
            (!v || c->left < v->left || (c->left == v->left && older(c, v)))) {
            v = c;
        }
    }
    if (v) {
        b->cnt[BAT_C_BC_COPY_DROP] += v->left;
        v->left = 0;
    }
    return v;
}

static void send_copy(struct bat *b, struct bat_bc_copy *c)
{
    bat_link_tx(b, bat_bcast_addr, c->frame, c->len);
    c->left--;
}

/* dataplane §5.3. With 2 or 3 copies (unACKed group frames: on the bench HaLow link one sent right
 * behind another was mostly lost) every copy waits in a slot and bat_tick sends one at a time, each
 * BAT_BC_COPY_MS after our previous group frame; with nothing waiting the first goes at once.
 * false = dropped, the slots all holding broadcasts not yet sent. */
static bool copies(struct bat *b, uint8_t *frame, size_t len)
{
    if (b->cfg.bcast_copies < 2) {
        bat_link_tx(b, bat_bcast_addr, frame, len);
        return true;
    }
    const bool idle = next_copy(b) == NULL;
    struct bat_bc_copy *c = slot_get(b);
    if (!c) {
        BAT_INC(b, BC_QUEUE_FULL);
        return false;
    }
    memcpy(c->frame, frame, len);
    c->len = (uint16_t)len;
    c->left = b->cfg.bcast_copies;
    c->ord = b->bc_ord++;
    if (idle && !bat_grp_held(b)) {
        send_copy(b, c);
    }
    return true;
}

bool bat_bcast_copies(struct bat *b, uint32_t *due)
{
    struct bat_bc_copy *c = next_copy(b);
    if (c && !bat_grp_held(b)) {
        send_copy(b, c);
        c = next_copy(b);
    }
    if (!c) {
        return false;
    }
    *due = bat_grp_free_at(b);
    return true;
}

/* frame holds a complete BCAST behind room for the link header; @from = arriving link source.
 * false = dropped at a full copy queue (a suppressed or oversized one counts as handled). */
static bool flood(struct bat *b, uint8_t *frame, size_t len, const uint8_t *from, bool own)
{
    const uint8_t *orig = frame + BAT_ETH_HLEN + BAT_BC_ORIG;
    if (bat_flood_suppressed(b, orig, from)) {
        BAT_INC(b, BC_SUPP);
        return true;
    }
    if (len - BAT_ETH_HLEN > b->cfg.hard_mtu) {
        BAT_INC(b, BC_TOOBIG);
        return true;
    }
    if (!copies(b, frame, len)) {
        return false;
    }
    if (own) {
        BAT_INC(b, BC_TX_OWN);
    } else {
        BAT_INC(b, BC_FWD);
    }
    return true;
}

int bat_bcast_tx_own(struct bat *b, const uint8_t *inner, size_t len)
{
    if (len + BAT_BC_HLEN > b->cfg.hard_mtu) {
        BAT_INC(b, BC_TOOBIG);
        return -1;
    }
    uint8_t *f = b->txbuf, *p = f + BAT_ETH_HLEN;
    b->bcast_seq++;
    bat_seq_note(b);
    p[BAT_OFF_TYPE] = BAT_PT_BCAST;
    p[BAT_OFF_VERSION] = BAT_COMPAT;
    p[BAT_BC_TTL] = BAT_BC_TTL_INIT;
    p[BAT_BC_RSVD] = 0;
    bat_put32(p + BAT_BC_SEQ, b->bcast_seq);
    memcpy(p + BAT_BC_ORIG, b->cfg.hard_addr, 6);
    memcpy(p + BAT_BC_HLEN, inner, len);
    return flood(b, f, BAT_ETH_HLEN + BAT_BC_HLEN + len, NULL, true) ? 0 : -1;
}

void bat_bcast_rx(struct bat *b, uint8_t *frame, size_t len)
{
    uint8_t *p = frame + BAT_ETH_HLEN;
    uint8_t from[6];
    memcpy(from, frame + BAT_LINK_SRC, 6);
    BAT_INC(b, BC_RX);
    if (bat_is_own(b, p + BAT_BC_ORIG)) {
        BAT_INC(b, BC_OWN);
        return;
    }
    if (p[BAT_BC_TTL] < 2) {
        BAT_INC(b, BC_TTL);
        return;
    }
    p[BAT_BC_TTL]--;
    struct bat_orig *o = bat_orig_find(b, p + BAT_BC_ORIG);
    if (!o) {
        BAT_INC(b, BC_UNKNOWN);
        return;
    }
    switch (window(b, &o->bcast, bat_get32(p + BAT_BC_SEQ))) {
    case WIN_DUP:
        BAT_INC(b, BC_DUP);
        return;
    case WIN_BLOCKED:
        BAT_INC(b, BC_RESTART_BLOCKED);
        return;
    default:
        break;
    }
    (void)flood(b, frame, len, from, false);
    const uint8_t *inner = p + BAT_BC_HLEN;
    size_t ilen = len - BAT_ETH_HLEN - BAT_BC_HLEN;
    if (bat_deliver_frame(b, inner, ilen)) {
        BAT_INC(b, BC_DELIVER);
        bat_tt_learn_temp(b, o, inner, ilen);
    }
}
