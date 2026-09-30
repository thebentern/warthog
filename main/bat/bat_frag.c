/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bat_internal.h"

/* dataplane §3.2 with the real 802.1Q PCP; the client frame sits behind a 0x40 or 0x42 header. */
uint8_t bat_frag_prio(const uint8_t *pkt, size_t len)
{
    size_t off = pkt[BAT_OFF_TYPE] == BAT_PT_UNICAST ? BAT_UC_HLEN :
                 pkt[BAT_OFF_TYPE] == BAT_PT_4ADDR ? BAT_4A_HLEN : 0;
    if (off == 0 || len < off + BAT_ETH_HLEN + 2) {
        return 0;
    }
    const uint8_t *e = pkt + off;
    switch (bat_get16(e + 12)) {
    case 0x8100:
        return (uint8_t)(e[14] >> 5);
    case 0x0800:
        return (uint8_t)(e[15] >> 5);
    case 0x86DD:
        return (uint8_t)((e[14] & 0x0F) >> 1);
    default:
        return 0;
    }
}

/* dataplane §7.2: equal pieces of at most min(MTU, 1280) bytes, cut from the tail. */
int bat_frag_tx(struct bat *b, const uint8_t nh[6], const uint8_t dest[6], const uint8_t *frame,
                size_t len)
{
    const uint8_t *pkt = frame + BAT_ETH_HLEN;
    size_t L = len - BAT_ETH_HLEN;
    size_t mtu = b->cfg.hard_mtu < BAT_FR_MAX_FRAME ? b->cfg.hard_mtu : BAT_FR_MAX_FRAME;
    size_t e = mtu - BAT_FR_HLEN;
    size_t n = (L + e - 1) / e;
    if (n > BAT_FR_MAX_N || L > 0xFFFF) {
        BAT_INC(b, FR_TX_TOOMANY);
        return BAT_SEND_DROP;
    }
    size_t s = (L + n - 1) / n;
    uint8_t prio = bat_frag_prio(pkt, L);
    if (++b->frag_seq == 0) {
        b->frag_seq = 1;
    }
    uint8_t d[6], hop[6];
    memcpy(d, dest, 6);
    memcpy(hop, nh, 6);
    int worst = BAT_TX_OK;
    for (size_t k = 0; k < n; k++) {
        size_t hi = L - k * s, lo = L > (k + 1) * s ? L - (k + 1) * s : 0;
        uint8_t *f = b->data.fragbuf, *h = f + BAT_ETH_HLEN;
        h[BAT_OFF_TYPE] = BAT_PT_FRAG;
        h[BAT_OFF_VERSION] = BAT_COMPAT;
        h[BAT_FR_TTL] = BAT_UC_TTL_INIT;
        h[BAT_FR_NUMPRIO] = (uint8_t)(k << 4 | (unsigned)prio << 1);
        memcpy(h + BAT_FR_DEST, d, 6);
        memcpy(h + BAT_FR_ORIG, b->cfg.hard_addr, 6);
        bat_put16(h + BAT_FR_SEQ, b->frag_seq);
        bat_put16(h + BAT_FR_TOTAL, (uint16_t)L);
        memcpy(h + BAT_FR_HLEN, pkt + lo, hi - lo);
        BAT_INC(b, FR_TX);
        int r = bat_link_tx(b, hop, f, BAT_ETH_HLEN + BAT_FR_HLEN + (hi - lo));
        if (r != BAT_TX_OK && worst == BAT_TX_OK) {
            worst = r;
        }
    }
    return worst;
}

static void slot_free(struct bat_frag_slot *s)
{
    s->used = 0;
    s->have = 0;
    memset(s->piece, 0, sizeof(s->piece));
}

/* Slot of (originator, seqno), else a free one, else the least recently updated one of the
 * same originator, else another originator's idle for BAT_FRAG_STEAL_MS; NULL = drop it. */
static struct bat_frag_slot *slot_get(struct bat *b, unsigned oi, uint16_t seq, bool *fresh)
{
    struct bat_frag_slot *free_s = NULL, *own = NULL, *other = NULL;
    for (unsigned i = 0; i < BAT_FRAG_SLOTS; i++) {
        struct bat_frag_slot *s = &b->data.slot[i];
        if (s->used && s->orig == oi && s->seq == seq) {
            *fresh = false;
            return s;
        }
        struct bat_frag_slot **old = s->orig == oi ? &own : &other;
        if (!s->used) {
            free_s = free_s ? free_s : s;
        } else if (!*old || bat_age(b, s->ts) > bat_age(b, (*old)->ts)) {
            *old = s;
        }
    }
    if (!free_s) {
        free_s = own ? own : (other && bat_age(b, other->ts) > BAT_FRAG_STEAL_MS) ? other : NULL;
        if (!free_s) {
            BAT_INC(b, FR_FULL);       /* another originator's live reassembly is never evicted */
            return NULL;
        }
        BAT_INC(b, FR_EVICT);
        slot_free(free_s);
    }
    *fresh = true;
    return free_s;
}

static void reassemble(struct bat *b, struct bat_frag_slot *s)
{
    uint8_t *out = b->data.asm_frame + BAT_ETH_HLEN;
    size_t pos = 0;
    for (unsigned k = BAT_FRAG_MAXN; k-- > 0;) {
        const struct bat_frag_piece *pc = &s->piece[k];
        if (pc->used) {
            memcpy(out + pos, s->buf + pc->off, pc->len);
            pos += pc->len;
        }
    }
    memcpy(b->data.asm_frame, s->link_hdr, BAT_ETH_HLEN);
    slot_free(s);
    BAT_INC(b, FR_DONE);
    bat_rx_dispatch(b, b->data.asm_frame, BAT_ETH_HLEN + pos, 1);
}

/* dataplane §7.3-7.5; transit fragments are forwarded unchanged (deviation 2.4.4). */
void bat_frag_rx(struct bat *b, uint8_t *frame, size_t len, unsigned depth)
{
    uint8_t *p = frame + BAT_ETH_HLEN;
    size_t pl = len - BAT_ETH_HLEN;
    (void)depth;
    if (pl < BAT_FR_HLEN) {
        BAT_INC(b, RX_HDR);
        return;
    }
    BAT_INC(b, FR_RX);
    struct bat_orig *fo = bat_orig_find(b, p + BAT_FR_ORIG);
    if (!fo) {
        BAT_INC(b, FR_UNKNOWN);
        return;
    }
    if (!bat_is_own(b, p + BAT_FR_DEST)) {
        if (p[BAT_FR_TTL] < 2) {
            BAT_INC(b, FR_TTL);
            return;
        }
        if (pl > b->cfg.hard_mtu) {
            BAT_INC(b, FR_BAD);
            return;
        }
        struct bat_orig *d = bat_orig_find(b, p + BAT_FR_DEST);
        const uint8_t *nh = bat_route_nh(b, d, BAT_TBL_IFACE);
        if (!nh) {
            BAT_INC(b, FR_NOROUTE);
            return;
        }
        uint8_t hop[6];
        memcpy(hop, nh, 6);
        p[BAT_FR_TTL]--;
        BAT_INC(b, FR_FWD);
        bat_link_tx(b, hop, frame, len);
        return;
    }
    size_t dl = pl - BAT_FR_HLEN;
    uint16_t seq = bat_get16(p + BAT_FR_SEQ), total = bat_get16(p + BAT_FR_TOTAL);
    unsigned num = p[BAT_FR_NUMPRIO] >> 4;
    if (total > BAT_FRAG_BUF) {
        BAT_INC(b, FR_TOOBIG);
        bat_tt_answer_toobig(b, p + BAT_FR_HLEN, dl);
        return;
    }
    if (dl == 0 || total == 0 || dl > total) {
        BAT_INC(b, FR_BAD);
        return;
    }
    bool fresh;
    struct bat_frag_slot *s = slot_get(b, bat_orig_index(b, fo), seq, &fresh);
    if (!s) {
        return;
    }
    if (fresh) {
        s->used = 1;
        s->orig = (uint8_t)bat_orig_index(b, fo);
        s->seq = seq;
        s->total = total;
        s->have = 0;
        memcpy(s->link_hdr, frame, BAT_ETH_HLEN);
    } else if (s->total != total) {
        BAT_INC(b, FR_BAD);
        slot_free(s);
        return;
    } else if (s->piece[num].used) {
        BAT_INC(b, FR_DUP);
        return;
    } else if (s->have + dl > total) {
        BAT_INC(b, FR_BAD);
        slot_free(s);
        return;
    }
    struct bat_frag_piece *pc = &s->piece[num];
    pc->num = (uint8_t)num;
    pc->used = 1;
    pc->off = s->have;
    pc->len = (uint16_t)dl;
    memcpy(s->buf + s->have, p + BAT_FR_HLEN, dl);
    s->have = (uint16_t)(s->have + dl);
    s->ts = b->now;
    if (s->have == s->total) {
        reassemble(b, s);
    }
}

void bat_frag_purge(struct bat *b)
{
    for (unsigned i = 0; i < BAT_FRAG_SLOTS; i++) {
        struct bat_frag_slot *s = &b->data.slot[i];
        if (s->used && bat_age(b, s->ts) > BAT_FRAG_TIMEOUT_MS) {
            slot_free(s);
            BAT_INC(b, FR_TIMEOUT);
        }
    }
}

void bat_frag_orig_gone(struct bat *b, unsigned orig_idx)
{
    for (unsigned i = 0; i < BAT_FRAG_SLOTS; i++) {
        struct bat_frag_slot *s = &b->data.slot[i];
        if (s->used && s->orig == orig_idx) {
            slot_free(s);
        }
    }
}
