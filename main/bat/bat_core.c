/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bat_crc32c.h"
#include "bat_internal.h"

const uint8_t bat_bcast_addr[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

void bat_config_defaults(struct bat_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->hard_mtu = BAT_HARD_MTU_DEFAULT;
    cfg->elp_interval_ms = 500;
    cfg->ogm_interval_ms = 1000;
    cfg->hop_penalty = 30;
    cfg->bcast_copies = 1;
    cfg->tput_override = 0;
    cfg->aggregate_ogm = true;
    cfg->half_duplex = true;
}

size_t bat_ctx_size(void)
{
    return sizeof(struct bat);
}

uint32_t bat_rand(struct bat *b)
{
    return b->ops.rand32(b->user);
}

static uint32_t jittered(struct bat *b, uint32_t interval)
{
    return interval - BAT_JITTER_MS + bat_rand(b) % (2 * BAT_JITTER_MS);
}

static uint32_t agg_period(struct bat *b)
{
    return BAT_AGG_MIN_MS + bat_rand(b) % BAT_AGG_SPREAD_MS;
}

static void sample_now(struct bat *b)
{
    b->now = b->ops.now_ms(b->user);
}

/* The words of a struct bat_seq_keep before its CRC, in field order. */
enum { KEEP_WORDS = 6 };

static uint32_t keep_crc(const uint32_t w[KEEP_WORDS])
{
    uint8_t img[4 * KEEP_WORDS];
    for (unsigned i = 0; i < KEEP_WORDS; i++) {
        bat_put32(img + 4 * i, w[i]);
    }
    return bat_crc32c(0xFFFFFFFFu, img, sizeof(img));
}

static void addr_words(const uint8_t a[BAT_ALEN], uint32_t *hi, uint32_t *lo)
{
    *hi = bat_get32(a);
    *lo = bat_get16(a + 4);
}

static bool seq_keep_ok(const struct bat_seq_keep *k, const uint8_t hard[BAT_ALEN])
{
    const uint32_t w[KEEP_WORDS] = { k->magic, k->addr_hi, k->addr_lo, k->elp, k->ogm, k->bcast };
    uint32_t hi, lo;
    addr_words(hard, &hi, &lo);
    return w[0] == BAT_SEQ_KEEP_MAGIC && w[1] == hi && w[2] == lo && k->crc == keep_crc(w);
}

void bat_seq_note(struct bat *b)
{
    struct bat_seq_keep *k = b->cfg.seq_keep;
    if (!k) {
        return;
    }
    /* bcast_seq is incremented before use, the others after */
    uint32_t w[KEEP_WORDS] = { BAT_SEQ_KEEP_MAGIC, 0, 0, b->elp_seq, b->ogm_seq, b->bcast_seq + 1 };
    addr_words(b->cfg.hard_addr, &w[1], &w[2]);
    k->magic = w[0];
    k->addr_hi = w[1];
    k->addr_lo = w[2];
    k->elp = w[3];
    k->ogm = w[4];
    k->bcast = w[5];
    k->crc = keep_crc(w);
}

int bat_init(struct bat *b, const struct bat_config *cfg, const struct bat_ops *ops, void *user)
{
    if (!b) {
        return -1;
    }
    memset(b, 0, sizeof(*b));
    if (!cfg || !ops || !ops->tx || !ops->deliver || !ops->now_ms || !ops->rand32 ||
        !ops->link_tput) {
        return -1;
    }
    if (!bat_mac_unicast_ok(cfg->hard_addr) || !bat_mac_unicast_ok(cfg->soft_addr)) {
        return -1;
    }
    if (cfg->hard_mtu < 100 || cfg->hard_mtu > BAT_MAX_LINK_FRAME - BAT_ETH_HLEN) {
        return -1;
    }
    if (cfg->ogm_interval_ms < 40 || cfg->elp_interval_ms < 100) {
        return -1;
    }
    if (cfg->bcast_copies < 1 || cfg->bcast_copies > 3) {
        return -1;
    }
    b->cfg = *cfg;
    b->ops = *ops;
    b->user = user;
    sample_now(b);
    b->elp_seq = bat_rand(b);
    b->ogm_seq = bat_rand(b);
    b->bcast_seq = bat_rand(b);
    b->frag_seq = (uint16_t)bat_rand(b);
    if (b->frag_seq == 0) {
        b->frag_seq = 1;
    }
    /* Kept seqnos override the draws, which stay in place so every later draw is unchanged. */
    const bool kept = cfg->seq_keep && seq_keep_ok(cfg->seq_keep, cfg->hard_addr);
    if (kept) {
        b->elp_seq = cfg->seq_keep->elp + BAT_SEQ_MARGIN;
        b->ogm_seq = cfg->seq_keep->ogm + BAT_SEQ_MARGIN;
        b->bcast_seq = cfg->seq_keep->bcast + BAT_SEQ_MARGIN - 1;
    }
    bat_seq_note(b);   /* the start is kept before anything is sent */
    b->next_elp = b->now + jittered(b, b->cfg.elp_interval_ms);
    b->next_ogm = b->now + jittered(b, b->cfg.ogm_interval_ms);
    b->next_agg = b->now + agg_period(b);
    b->next_purge = b->now + BAT_PURGE_MS;
    b->next_tt_purge = b->now + BAT_TT_PURGE_MS;
    bat_tt_init(b);
    bat_data_init(b);
    return kept ? 1 : 0;
}

void bat_rx_hard(struct bat *b, uint8_t *frame, size_t len)
{
    sample_now(b);
    BAT_INC(b, RX);
    if (len < BAT_ETH_HLEN + 2) {
        BAT_INC(b, RX_SHORT);
        return;
    }
    if (len > BAT_MAX_LINK_FRAME) {
        BAT_INC(b, RX_TOOBIG);
        return;
    }
    if (bat_get16(frame + BAT_LINK_TYPE) != BAT_ETHERTYPE) {
        BAT_INC(b, RX_TYPE);
        return;
    }
    if (frame[BAT_ETH_HLEN + BAT_OFF_VERSION] != BAT_COMPAT) {
        BAT_INC(b, RX_VERSION);
        return;
    }
    if (!bat_mac_unicast_ok(frame + BAT_LINK_SRC)) {
        BAT_INC(b, RX_SRC_BAD);
        return;
    }
    if (bat_is_own(b, frame + BAT_LINK_SRC)) {
        BAT_INC(b, RX_SRC_OWN);
        return;
    }
    bat_rx_dispatch(b, frame, len, 0);
}

/* ELP, OGM2 and BCAST: fixed header present and link destination exactly broadcast. */
static bool mgmt_ok(struct bat *b, const uint8_t *frame, size_t len, size_t hlen)
{
    if (len - BAT_ETH_HLEN < hlen) {
        BAT_INC(b, RX_HDR);
        return false;
    }
    if (!bat_mac_is_bcast(frame + BAT_LINK_DST)) {
        if (frame[BAT_ETH_HLEN] == BAT_PT_ELP && !bat_mac_is_group(frame + BAT_LINK_DST)) {
            BAT_INC(b, ELP_PROBE);
        } else {
            BAT_INC(b, RX_MGMT_DST);
        }
        return false;
    }
    return true;
}

void bat_rx_dispatch(struct bat *b, uint8_t *frame, size_t len, unsigned depth)
{
    if (depth > 0) {
        if (len < BAT_ETH_HLEN + 2) {
            BAT_INC(b, RX_SHORT);
            return;
        }
        if (frame[BAT_ETH_HLEN + BAT_OFF_VERSION] != BAT_COMPAT) {
            BAT_INC(b, RX_VERSION);
            return;
        }
    }
    uint8_t pt = frame[BAT_ETH_HLEN + BAT_OFF_TYPE];
    if (pt >= BAT_PT_UNI_FIRST && pt <= BAT_PT_UNI_LAST) {
        if (depth > 0 && pt == BAT_PT_FRAG) {
            BAT_INC(b, FR_NESTED);
            return;
        }
        const uint8_t *dst = frame + BAT_LINK_DST;
        if (!bat_mac_unicast_ok(dst) || !bat_is_own(b, dst)) {
            BAT_INC(b, RX_UNI_DST);
            return;
        }
        switch (pt) {
        case BAT_PT_UNICAST:
        case BAT_PT_4ADDR:
            bat_unicast_rx(b, frame, len, depth);
            break;
        case BAT_PT_FRAG:
            bat_frag_rx(b, frame, len, depth);
            break;
        case BAT_PT_ICMP:
            bat_icmp_rx(b, frame, len);
            break;
        case BAT_PT_UTVLV:
            bat_utvlv_rx(b, frame, len);
            break;
        default:
            bat_unknown_rx(b, frame, len);
            break;
        }
        return;
    }
    if (depth > 0) {
        BAT_INC(b, RX_TYPE);
        return;
    }
    switch (pt) {
    case BAT_PT_ELP:
        if (mgmt_ok(b, frame, len, BAT_ELP_HLEN)) {
            bat_elp_rx(b, frame, len);
        }
        break;
    case BAT_PT_OGM2:
        if (mgmt_ok(b, frame, len, BAT_OGM_HLEN)) {
            bat_ogm_rx(b, frame, len);
        }
        break;
    case BAT_PT_BCAST:
        if (mgmt_ok(b, frame, len, BAT_BC_HLEN)) {
            bat_bcast_rx(b, frame, len);
        }
        break;
    default:
        BAT_INC(b, RX_TYPE);
        break;
    }
}

static bool is_loop_type(const uint8_t *frame, size_t len)
{
    uint16_t et = bat_get16(frame + BAT_LINK_TYPE);
    if (et == BAT_ETHERTYPE) {
        return true;
    }
    return et == 0x8100 && len >= BAT_ETH_HLEN + 4 && bat_get16(frame + 16) == BAT_ETHERTYPE;
}

int bat_tx_soft(struct bat *b, const uint8_t *frame, size_t len)
{
    static const uint8_t stp[6] = { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x00 };
    static const uint8_t ectp[6] = { 0xcf, 0x00, 0x00, 0x00, 0x00, 0x00 };

    sample_now(b);
    BAT_INC(b, ST_TX);
    if (len < BAT_ETH_HLEN) {
        BAT_INC(b, ST_SHORT);
        return -1;
    }
    if (is_loop_type(frame, len)) {
        BAT_INC(b, ST_LOOP);
        return -1;
    }
    if (!bat_mac_eq(frame + BAT_LINK_SRC, b->cfg.soft_addr)) {
        BAT_INC(b, ST_SRC);
        return -1;
    }
    if (bat_get16(frame + BAT_LINK_TYPE) == 0x8100) {
        BAT_INC(b, ST_VLAN);
        return -1;
    }
    if (len > BAT_MAX_LINK_FRAME - BAT_ETH_HLEN - BAT_4A_HLEN) {
        BAT_INC(b, ST_TOOBIG);
        return -1;
    }
    if (bat_mac_eq(frame, stp) || bat_mac_eq(frame, ectp)) {
        BAT_INC(b, ST_CTRL);
        return -1;
    }
    if (bat_mac_is_group(frame)) {
        BAT_INC(b, ST_BCAST);
        return bat_bcast_tx_own(b, frame, len);
    }
    return bat_data_tx_soft_unicast(b, frame, len, 0x0000);
}

static uint32_t until(const struct bat *b, uint32_t deadline)
{
    int32_t d = (int32_t)(deadline - b->now);
    return d < 1 ? 1u : (uint32_t)d;
}

bool bat_grp_held(const struct bat *b)
{
    return b->cfg.bcast_copies >= 2 && b->grp_sent && b->now - b->grp_ts < BAT_BC_COPY_MS;
}

uint32_t bat_grp_free_at(const struct bat *b)
{
    return b->grp_sent ? b->grp_ts + BAT_BC_COPY_MS : b->now;
}

/* @t, or later if a group frame sent then would be held. */
static uint32_t grp_after(const struct bat *b, uint32_t t)
{
    uint32_t f = bat_grp_free_at(b);
    return bat_grp_held(b) && (int32_t)(f - t) > 0 ? f : t;
}

uint32_t bat_tick(struct bat *b)
{
    sample_now(b);
    /* ELP and the aggregate wait out the spacing too, but go before waiting broadcast copies. */
    if (bat_due(b, b->next_elp) && !bat_grp_held(b)) {
        bat_elp_send(b);
        b->next_elp = b->now + jittered(b, b->cfg.elp_interval_ms);
    }
    if (bat_due(b, b->next_ogm)) {
        bat_ogm_own(b);
        b->next_ogm = b->now + jittered(b, b->cfg.ogm_interval_ms);
    }
    if (bat_due(b, b->next_agg) && !(b->agg_len && bat_grp_held(b))) {
        bat_ogm_flush(b);
        b->next_agg = b->now + agg_period(b);
    }
    if (bat_due(b, b->next_purge)) {
        bat_orig_purge(b);
        b->next_purge = b->now + BAT_PURGE_MS;
    }
    if (bat_due(b, b->next_tt_purge)) {
        bat_tt_purge(b);
        b->next_tt_purge = b->now + BAT_TT_PURGE_MS;
    }
    uint32_t bc_due = 0;
    bool bc = bat_bcast_copies(b, &bc_due);
    uint32_t w = 1000;
    const uint32_t dl[5] = { grp_after(b, b->next_elp), b->next_ogm,
                             b->agg_len ? grp_after(b, b->next_agg) : b->next_agg, b->next_purge,
                             b->next_tt_purge };
    for (int i = 0; i < 5; i++) {
        uint32_t u = until(b, dl[i]);
        if (u < w) {
            w = u;
        }
    }
    if (bc && until(b, bc_due) < w) {
        w = until(b, bc_due);
    }
    return w;
}

int bat_link_tx(struct bat *b, const uint8_t dst[6], uint8_t *frame, size_t len)
{
    memmove(frame + BAT_LINK_DST, dst, 6);
    memcpy(frame + BAT_LINK_SRC, b->cfg.hard_addr, 6);
    bat_put16(frame + BAT_LINK_TYPE, BAT_ETHERTYPE);
    int r = b->ops.tx(b->user, frame, len);
    switch (r) {
    case BAT_TX_OK:
        BAT_INC(b, LK_TX);
        if (bat_mac_is_group(frame + BAT_LINK_DST)) {
            sample_now(b);         /* ops->tx may have blocked: the spacing runs from the hand-off */
            b->grp_ts = b->now;
            b->grp_sent = 1;
        }
        break;
    case BAT_TX_NOPEER:
        BAT_INC(b, LK_NOPEER);
        break;
    case BAT_TX_BUSY:
        BAT_INC(b, LK_BUSY);
        break;
    default:
        BAT_INC(b, LK_FAIL);
        break;
    }
    return r;
}

bool bat_deliver_frame(struct bat *b, const uint8_t *inner, size_t len)
{
    if (len < BAT_ETH_HLEN || is_loop_type(inner, len)) {
        BAT_INC(b, DELIVER_BAD);
        return false;
    }
    BAT_INC(b, DELIVER);
    b->ops.deliver(b->user, inner, len);
    return true;
}

void bat_deliver(struct bat *b, const uint8_t *inner, size_t len)
{
    (void)bat_deliver_frame(b, inner, len);
}

unsigned bat_neigh_count(const struct bat *b)
{
    unsigned n = 0;
    for (unsigned i = 0; i < BAT_MAX_NEIGH; i++) {
        n += b->neigh[i].used ? 1u : 0u;
    }
    return n;
}

unsigned bat_route_count(const struct bat *b)
{
    unsigned n = 0;
    for (unsigned i = 0; i < BAT_MAX_ORIG; i++) {
        n += (b->orig[i].used && b->orig[i].tbl[BAT_TBL_DEFAULT].router >= 0) ? 1u : 0u;
    }
    return n;
}

bool bat_client_route(struct bat *b, const uint8_t mac[BAT_ALEN], struct bat_client_route *out)
{
    struct bat_orig *o = bat_tt_resolve(b, mac, 0);
    if (out) {
        memset(out, 0, sizeof(*out));
        if (o) {
            memcpy(out->orig, o->addr, BAT_ALEN);
            out->tput = bat_route_tput(b, o);
            out->ogm_age_ms = b->ops.now_ms(b->user) - o->last_seen;
        }
    }
    return o != NULL;
}

unsigned bat_gw_best(struct bat *b, struct bat_gw *out)
{
    const struct bat_orig *best = NULL;
    uint32_t bm = 0;
    unsigned n = 0;
    for (unsigned i = 0; i < BAT_MAX_ORIG; i++) {
        const struct bat_orig *o = &b->orig[i];
        if (!o->used || !o->gw_valid || !bat_route_nh(b, o, BAT_TBL_DEFAULT)) {
            continue;
        }
        n++;
        uint32_t t = bat_route_tput(b, o), m = t < o->gw_down ? t : o->gw_down;
        if (!best || m > bm) {
            best = o;
            bm = m;
        }
    }
    if (out) {
        memset(out, 0, sizeof(*out));
        if (best) {
            memcpy(out->orig, best->addr, BAT_ALEN);
            out->down = best->gw_down;
            out->up = best->gw_up;
            out->tput = bat_route_tput(b, best);
            out->ogm_age_ms = b->ops.now_ms(b->user) - best->last_seen;
        }
    }
    return n;
}

uint32_t bat_counter(const struct bat *b, enum bat_counter c)
{
    return (unsigned)c < BAT_C__COUNT ? b->cnt[c] : 0;
}

const char *bat_counter_name(enum bat_counter c)
{
    switch (c) {
#define BAT_X_NAME(id, name) case BAT_C_##id: return name;
    BAT_COUNTERS(BAT_X_NAME)
#undef BAT_X_NAME
    default:
        return "?";
    }
}
