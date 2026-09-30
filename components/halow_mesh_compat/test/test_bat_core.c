/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * White-box tests of the BATMAN_V engine core (main/bat: bat_core, bat_elp, bat_orig,
 * bat_ogm, bat_bcast, bat_render) driven through a scripted ops stub: the test owns the
 * clock, every random number, each neighbour's link throughput and the TX result, and
 * records every link frame and every delivered client frame.
 *
 * The expected numbers are the spec's (batman-spec/elp-ogm.md, dataplane.md,
 * packets.md, membership.md), which were measured against batman-adv 2024.3 in a VM:
 *  - ELP seqno window (elp-ogm §2.3) including the first-seqno-0 quirk;
 *  - ELP, OGM and BCAST counters carried across a reset that keeps power (membership §7.4.3):
 *    resumed 256 ahead from an intact record only, with the draws and timers unchanged;
 *  - the link EWMA series 100000 -> 200 (§2.5) and its rising stall at 999;
 *  - all rows of the penalty table (§3.5), the P = 10 / 11 / 16 inversion and the
 *    low-rate hop horizon (§7.6);
 *  - the OGM sequence rule with 30 s restart protection, router switching (gap 5,
 *    strictly better, ties keep), the §3.9 suppression table and forwarding rules;
 *  - the aggregate walk (no 512-byte receive cap), the 512-byte send cap, and no forwarded
 *    record above the hard MTU;
 *  - candidate 200 s / originator 400 s expiry;
 *  - the broadcast duplicate window of dataplane §5.2 exactly as verified there, and further
 *    broadcast copies 5 ms apart (§5.3); with 2 or 3 copies every group frame 5 ms after the
 *    one before (hardening: on air a group frame right behind another was mostly lost), first
 *    copies before repeats, ELP and the OGM aggregate held too, a bounded copy queue, the 5 ms
 *    counted from the hand-off when ops->tx blocks (the port's flow-control wait);
 *  - a new direct neighbour admitted into a full originator table (hardening);
 *  - the Linux unicast ELP probe from capture 19a (must leave no state);
 *  - the renders, and AT+BATO? paged through the port's 4 KiB buffer at 32 originators with
 *    4 candidates each (whole, one summary, originators never split) and its MAC lookup.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bat_crc32c.h"
#include "bat_internal.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* ---- scripted ops stub ------------------------------------------------------- */

#define MAXTX 1024
#define MAXDL 64
struct frame { uint32_t t; size_t len; uint8_t b[BAT_MAX_LINK_FRAME]; };
struct stub {
    uint32_t now;
    uint32_t script[32];
    unsigned nscript, spos, rand_calls;
    uint32_t rng;
    struct { uint8_t a[6]; uint32_t v; } lt[16];
    unsigned nlt;
    uint32_t lt_default;
    int tx_ret;
    unsigned ntx, ndl;
    struct frame tx[MAXTX];
    struct frame dl[MAXDL];
};
static struct stub S;

static int st_tx(void *u, const uint8_t *f, size_t len)
{
    (void)u;
    if (S.ntx < MAXTX) {
        S.tx[S.ntx].t = S.now;
        S.tx[S.ntx].len = len;
        memcpy(S.tx[S.ntx].b, f, len);
        S.ntx++;
    }
    return S.tx_ret;
}
static void st_deliver(void *u, const uint8_t *f, size_t len)
{
    (void)u;
    if (S.ndl < MAXDL) {
        S.dl[S.ndl].t = S.now;
        S.dl[S.ndl].len = len;
        memcpy(S.dl[S.ndl].b, f, len);
        S.ndl++;
    }
}
static uint32_t st_now(void *u)
{
    (void)u;
    return S.now;
}
static uint32_t st_rand(void *u)
{
    (void)u;
    S.rand_calls++;
    if (S.spos < S.nscript) {
        return S.script[S.spos++];
    }
    S.rng ^= S.rng << 13;
    S.rng ^= S.rng >> 17;
    S.rng ^= S.rng << 5;
    return S.rng;
}
static uint32_t st_tput(void *u, const uint8_t a[6])
{
    (void)u;
    for (unsigned i = 0; i < S.nlt; i++) {
        if (memcmp(S.lt[i].a, a, 6) == 0) {
            return S.lt[i].v;
        }
    }
    return S.lt_default;
}
static const struct bat_ops OPS = { st_tx, st_deliver, st_now, st_rand, st_tput };

static void set_tput(const uint8_t a[6], uint32_t v)
{
    for (unsigned i = 0; i < S.nlt; i++) {
        if (memcmp(S.lt[i].a, a, 6) == 0) {
            S.lt[i].v = v;
            return;
        }
    }
    memcpy(S.lt[S.nlt].a, a, 6);
    S.lt[S.nlt++].v = v;
}

/* ---- addresses and frame builders ---------------------------------------------- */

static const uint8_t W[6] = { 0x02, 0x77, 0x00, 0x00, 0x00, 0x01 };     /* the engine */
static const uint8_t WS[6] = { 0x06, 0x77, 0x00, 0x00, 0x00, 0x01 };    /* its soft MAC */
static const uint8_t BC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static void mac(uint8_t out[6], uint8_t a, uint8_t b)
{
    const uint8_t m[6] = { 0x02, a, 0x00, 0x00, 0x00, b };
    memcpy(out, m, 6);
}

static size_t link_hdr(uint8_t *f, const uint8_t *dst, const uint8_t *src)
{
    memcpy(f, dst, 6);
    memcpy(f + 6, src, 6);
    f[12] = 0x43;
    f[13] = 0x05;
    return 14;
}

static size_t mk_elp(uint8_t *f, const uint8_t *src, const uint8_t *orig, uint32_t seq)
{
    link_hdr(f, BC, src);
    uint8_t *p = f + 14;
    memset(p, 0, 20);
    p[0] = 0x03;
    p[1] = 0x0f;
    memcpy(p + 2, orig, 6);
    bat_put32(p + 8, seq);
    bat_put32(p + 12, 500);
    return 34;
}

static size_t ogm_rec(uint8_t *p, const uint8_t *orig, uint32_t seq, uint8_t ttl, uint32_t tput,
                      const uint8_t *tvlv, uint16_t tl)
{
    p[0] = 0x04;
    p[1] = 0x0f;
    p[2] = ttl;
    p[3] = 0;
    bat_put32(p + 4, seq);
    memcpy(p + 8, orig, 6);
    bat_put16(p + 14, tl);
    bat_put32(p + 16, tput);
    if (tl) {
        if (tvlv) {
            memcpy(p + 20, tvlv, tl);
        } else {
            memset(p + 20, 0, tl);
        }
    }
    return 20u + tl;
}

static size_t mk_ogm(uint8_t *f, const uint8_t *src, const uint8_t *orig, uint32_t seq, uint8_t ttl,
                     uint32_t tput)
{
    link_hdr(f, BC, src);
    return 14 + ogm_rec(f + 14, orig, seq, ttl, tput, NULL, 0);
}

/* Client Ethernet frame, IPv4 ethertype, payload bytes counting up. */
static size_t eth(uint8_t *out, const uint8_t *dst, const uint8_t *src, size_t pl)
{
    memcpy(out, dst, 6);
    memcpy(out + 6, src, 6);
    out[12] = 0x08;
    out[13] = 0x00;
    for (size_t i = 0; i < pl; i++) {
        out[14 + i] = (uint8_t)(i * 7 + 1);
    }
    return 14 + pl;
}

static size_t mk_bcast(uint8_t *f, const uint8_t *src, const uint8_t *orig, uint32_t seq, uint8_t ttl,
                       size_t inner)
{
    link_hdr(f, BC, src);
    uint8_t *p = f + 14;
    p[0] = 0x01;
    p[1] = 0x0f;
    p[2] = ttl;
    p[3] = 0;
    bat_put32(p + 4, seq);
    memcpy(p + 8, orig, 6);
    uint8_t *in = p + 14;
    memset(in, 0xff, 6);
    memcpy(in + 6, orig, 6);
    in[6] = 0x06;
    in[12] = 0x08;
    in[13] = 0x00;
    for (size_t i = 14; i < inner; i++) {
        in[i] = (uint8_t)(seq + i);
    }
    return 28 + inner;
}

/* ---- engine helpers ------------------------------------------------------------ */

static struct bat *B;
static uint8_t RXBUF[BAT_MAX_LINK_FRAME + 64];

static void stub_reset(void)
{
    memset(&S, 0, sizeof(S));
    S.rng = 0x2545F491u;
    S.lt_default = BAT_TPUT_UNKNOWN;
    S.tx_ret = BAT_TX_OK;
    S.now = 100000;
}

static void cfg_default(struct bat_config *c)
{
    bat_config_defaults(c);
    memcpy(c->hard_addr, W, 6);
    memcpy(c->soft_addr, WS, 6);
}

static struct bat *eng(const struct bat_config *c)
{
    if (!B) {
        B = malloc(bat_ctx_size());
    }
    struct bat_config d;
    if (!c) {
        cfg_default(&d);
        c = &d;
    }
    if (bat_init(B, c, &OPS, NULL) != 0) {
        printf("FAIL bat_init refused a valid config\n");
        failures++;
    }
    return B;
}

static void fresh(void)
{
    stub_reset();
    eng(NULL);
}

static void rx(const uint8_t *f, size_t len)
{
    memcpy(RXBUF, f, len);
    bat_rx_hard(B, RXBUF, len);
}

static uint32_t C(enum bat_counter c)
{
    return bat_counter(B, c);
}

/* Counter snapshot and diff: every counter except those listed must be unchanged. */
static uint32_t SNAP[BAT_C__COUNT];
static void snap(void)
{
    for (int i = 0; i < BAT_C__COUNT; i++) {
        SNAP[i] = bat_counter(B, (enum bat_counter)i);
    }
}
static int only_changed(const enum bat_counter *which, unsigned n)
{
    for (int i = 0; i < BAT_C__COUNT; i++) {
        uint32_t d = bat_counter(B, (enum bat_counter)i) - SNAP[i];
        bool listed = false;
        for (unsigned k = 0; k < n; k++) {
            listed |= which[k] == (enum bat_counter)i;
        }
        if (listed ? d != 1 : d != 0) {
            printf("     counter %s moved by %u\n", bat_counter_name((enum bat_counter)i), d);
            return 0;
        }
    }
    return 1;
}
#define ONLY(...) only_changed((const enum bat_counter[]){ __VA_ARGS__ }, \
                               sizeof((const enum bat_counter[]){ __VA_ARGS__ }) / sizeof(enum bat_counter))

/* One own ELP now: one throughput sample per neighbour. */
static void sample(void)
{
    B->now = S.now;
    bat_elp_send(B);
}

/* Neighbour at @src with ELP originator @orig and link throughput @link (sampled once). */
static void add_neigh(const uint8_t *src, const uint8_t *orig, uint32_t link)
{
    uint8_t f[64];
    set_tput(src, link);
    rx(f, mk_elp(f, src, orig, 1000));
    struct bat_neigh *n = bat_neigh_find(B, src);
    if (n && n->tput_acc == 0) {
        n->tput_acc = (uint64_t)link << 10;
    }
}

static void run_to(uint32_t t)
{
    while ((int32_t)(t - S.now) > 0) {
        S.now++;
        bat_tick(B);
    }
}

static unsigned orig_count(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < BAT_MAX_ORIG; i++) {
        n += B->orig[i].used;
    }
    return n;
}

static struct bat_cand *cand_via(struct bat_orig *o, const uint8_t *nb)
{
    struct bat_neigh *n = bat_neigh_find(B, nb);
    if (!o || !n) {
        return NULL;
    }
    for (unsigned i = 0; i < BAT_CANDS_PER_ORIG; i++) {
        if (o->cand[i].used && o->cand[i].neigh == (unsigned)(n - B->neigh)) {
            return &o->cand[i];
        }
    }
    return NULL;
}

/* index of the next OGM2 frame in the TX log at or after *k, or -1 */
static int next_ogm_tx(unsigned *k)
{
    while (*k < S.ntx) {
        unsigned i = (*k)++;
        if (S.tx[i].len > 14 && S.tx[i].b[14] == 0x04) {
            return (int)i;
        }
    }
    return -1;
}

/* ---- tests ------------------------------------------------------------------- */

static void test_init(void)
{
    struct bat_config c;
    stub_reset();
    if (!B) {
        B = malloc(bat_ctx_size());
    }
    cfg_default(&c);
    CHECK(bat_init(B, &c, &OPS, NULL) == 0, "init: default config accepted");
    CHECK(c.hard_mtu == 1500 && c.elp_interval_ms == 500 && c.ogm_interval_ms == 1000 &&
          c.hop_penalty == 30 && c.bcast_copies == 1 && c.tput_override == 0 && c.aggregate_ogm &&
          c.half_duplex, "config defaults: 1500 / 500 / 1000 / 30 / 1 copy / auto / aggregate / half duplex");
    struct bat_config z;
    bat_config_defaults(&z);
    static const uint8_t zero[6];
    CHECK(memcmp(z.hard_addr, zero, 6) == 0 && memcmp(z.soft_addr, zero, 6) == 0,
          "config defaults zero both addresses");
    memset(c.hard_addr, 0, 6);
    CHECK(bat_init(B, &c, &OPS, NULL) == -1, "init: zero hard address refused");
    cfg_default(&c); c.soft_addr[0] = 0x07;
    CHECK(bat_init(B, &c, &OPS, NULL) == -1, "init: group soft address refused");
    cfg_default(&c); c.hard_addr[0] = 0x03;
    CHECK(bat_init(B, &c, &OPS, NULL) == -1, "init: group hard address refused");
    cfg_default(&c); memset(c.soft_addr, 0, 6);
    CHECK(bat_init(B, &c, &OPS, NULL) == -1, "init: zero soft address refused");
    cfg_default(&c); c.hard_mtu = 99;
    CHECK(bat_init(B, &c, &OPS, NULL) == -1, "init: hard MTU 99 refused");
    c.hard_mtu = 1587;
    CHECK(bat_init(B, &c, &OPS, NULL) == -1, "init: hard MTU 1587 refused");
    c.hard_mtu = 100;
    CHECK(bat_init(B, &c, &OPS, NULL) == 0, "init: hard MTU 100 accepted");
    c.hard_mtu = 1586;
    CHECK(bat_init(B, &c, &OPS, NULL) == 0, "init: hard MTU 1586 accepted");
    cfg_default(&c); c.ogm_interval_ms = 39;
    CHECK(bat_init(B, &c, &OPS, NULL) == -1, "init: OGM interval 39 ms refused");
    c.ogm_interval_ms = 40;
    CHECK(bat_init(B, &c, &OPS, NULL) == 0, "init: OGM interval 40 ms accepted");
    cfg_default(&c); c.elp_interval_ms = 99;
    CHECK(bat_init(B, &c, &OPS, NULL) == -1, "init: ELP interval 99 ms refused");
    cfg_default(&c); c.bcast_copies = 0;
    CHECK(bat_init(B, &c, &OPS, NULL) == -1, "init: 0 broadcast copies refused");
    c.bcast_copies = 4;
    CHECK(bat_init(B, &c, &OPS, NULL) == -1, "init: 4 broadcast copies refused");
    c.bcast_copies = 3;
    CHECK(bat_init(B, &c, &OPS, NULL) == 0, "init: 3 broadcast copies accepted");
    cfg_default(&c); memcpy(c.soft_addr, c.hard_addr, 6);
    CHECK(bat_init(B, &c, &OPS, NULL) == 0, "init: soft MAC equal to the hard MAC accepted");
    cfg_default(&c);
    struct bat_ops o = OPS;
    o.link_tput = NULL;
    CHECK(bat_init(B, &c, &o, NULL) == -1, "init: missing op refused");
    CHECK(bat_ctx_size() == sizeof(struct bat), "ctx size is the whole engine");
}

static void test_rand_order(void)
{
    stub_reset();
    S.now = 5000;
    const uint32_t sc[] = { 1, 2, 3, 0x10000, 5, 6, 7, 3 };
    memcpy(S.script, sc, sizeof(sc));
    S.nscript = 8;
    eng(NULL);
    CHECK(B->elp_seq == 1 && B->ogm_seq == 2 && B->bcast_seq == 3,
          "rand order: (1) ELP, (2) OGM, (3) BCAST seqno");
    CHECK(B->frag_seq == 1, "rand order: (4) fragment seqno from the low 16 bits, 0 becomes 1");
    CHECK(B->next_elp == 5000 + 480 + 5 && B->next_ogm == 5000 + 980 + 6 && B->next_agg == 5000 + 90 + 7,
          "rand order: (5) ELP, (6) OGM, (7) aggregation jitter");
    CHECK(S.rand_calls == 7, "bat_init draws exactly 7 random numbers (%u)", S.rand_calls);
    CHECK(B->next_purge == 6000 && B->next_tt_purge == 10000, "purge in 1 s, TT purge in 5 s, no jitter");
    S.now = 5097;
    uint32_t w = bat_tick(B);
    CHECK(S.rand_calls == 8 && B->next_agg == 5097 + 93, "one random number per timer re-arm (aggregation)");
    CHECK(w == 93, "bat_tick returns the time to the nearest deadline (%u)", w);
    S.now = 5485;
    S.ntx = 0;
    unsigned want = 9 + ((int32_t)(S.now - B->next_agg) >= 0);
    w = bat_tick(B);
    CHECK(S.ntx == 1 && S.rand_calls == want, "ELP timer: one ELP, one re-arm per due timer");
    CHECK(w >= 1 && w <= 1000, "bat_tick return clamped to 1..1000 (%u)", w);
    S.now = 5485 + 5000;
    w = bat_tick(B);
    CHECK(w >= 1, "an overdue timer still gives a wait of at least 1 ms (%u)", w);
}

/* One own ELP, OGM and broadcast now (W needs a neighbour for the OGM); the seqnos they carried, ~0u
 * for one not sent. */
static void send_each(uint32_t *e, uint32_t *o, uint32_t *bc)
{
    uint8_t f[128];
    unsigned t0 = S.ntx;
    B->now = S.now;
    bat_elp_send(B);
    bat_ogm_own(B);
    bat_ogm_flush(B);
    (void)bat_tx_soft(B, f, eth(f, BC, WS, 50));
    *e = *o = *bc = ~0u;
    for (unsigned k = t0; k < S.ntx; k++) {
        const uint8_t *p = S.tx[k].b + 14;
        uint32_t *d = p[0] == 0x03 ? e : p[0] == 0x04 ? o : p[0] == 0x01 ? bc : NULL;
        if (d && *d == ~0u) {
            *d = bat_get32(p + (p[0] == 0x03 ? 8 : 4));
        }
    }
}

/* bat_init with the scripted draws 1, 2, 3, 0x10000, 5, 6, 7 (or @sc) and record @k; 1 = resumed. */
static int init_keep(struct bat_seq_keep *k, const uint8_t *hard, const uint32_t *sc)
{
    static const uint32_t def[] = { 1, 2, 3, 0x10000, 5, 6, 7 };
    struct bat_config c;
    stub_reset();
    memcpy(S.script, sc ? sc : def, sizeof(def));
    S.nscript = 7;
    cfg_default(&c);
    memcpy(c.hard_addr, hard, 6);
    c.seq_keep = k;
    return bat_init(B, &c, &OPS, NULL);
}

/* membership §7.4.3: a record that outlives the engine (the port's RTC no-init memory) carries the
 * ELP, OGM and BCAST counters across a reset that keeps power, 256 ahead; anything else in it
 * (power-up garbage, another node's, a flipped bit) draws them as before, and the draws, their
 * order and the timers never change, so bat_sim and the golden replays stay deterministic. */
static void test_seq_keep(void)
{
    struct bat_seq_keep k, good, before;
    uint8_t n1[6], other[6];
    uint32_t e, o, bc;
    mac(n1, 0x11, 1);
    mac(other, 0x77, 2);
    fresh();
    memset(&k, 0, sizeof(k));
    int r = init_keep(&k, W, NULL);
    const uint32_t e0 = B->next_elp, o0 = B->next_ogm, a0 = B->next_agg;
    CHECK(r == 0 && B->elp_seq == 1 && B->ogm_seq == 2 && B->bcast_seq == 3 && B->frag_seq == 1 &&
          S.rand_calls == 7, "seq keep: a zeroed record (power-up): the seqnos drawn as without one (%d)", r);
    CHECK(k.magic == BAT_SEQ_KEEP_MAGIC && k.addr_hi == 0x02770000u && k.addr_lo == 0x0001u && k.elp == 1 &&
          k.ogm == 2 && k.bcast == 4, "seq keep: ... and written into it at once as the next each sends (1, 2, 4)");
    add_neigh(n1, n1, 100);
    send_each(&e, &o, &bc);
    CHECK(e == 1 && o == 2 && bc == 4 && k.elp == 2 && k.ogm == 3 && k.bcast == 5,
          "seq keep: one ELP, OGM and broadcast sent (%u %u %u): the record follows (%u %u %u)", e, o, bc, k.elp,
          k.ogm, k.bcast);

    good = k;
    r = init_keep(&k, W, NULL);
    CHECK(r == 1 && B->elp_seq == 2 + BAT_SEQ_MARGIN && B->ogm_seq == 3 + BAT_SEQ_MARGIN &&
          B->bcast_seq + 1 == 5 + BAT_SEQ_MARGIN, "seq keep: a reset keeping it: resumed 256 past it (%d)", r);
    CHECK(S.rand_calls == 7 && B->next_elp == e0 && B->next_ogm == o0 && B->next_agg == a0 && B->frag_seq == 1,
          "seq keep: ... with the same 7 draws and timers as a random start");
    add_neigh(n1, n1, 100);
    send_each(&e, &o, &bc);
    CHECK(e == 258 && o == 259 && bc == 261 && k.elp == 259 && k.ogm == 260 && k.bcast == 262,
          "seq keep: its first ELP, OGM and broadcast carry %u %u %u", e, o, bc);
    before = k;
    r = init_keep(&k, W, NULL);
    int r2 = init_keep(&k, W, NULL);
    CHECK(r == 1 && r2 == 1 && B->elp_seq == before.elp + 2 * BAT_SEQ_MARGIN &&
          B->ogm_seq == before.ogm + 2 * BAT_SEQ_MARGIN && B->bcast_seq + 1 == before.bcast + 2 * BAT_SEQ_MARGIN,
          "seq keep: two resets with nothing sent between: 512 ahead (a start is kept before anything is sent)");

    struct bat_config c;
    cfg_default(&c);
    c.hard_mtu = 99;
    c.seq_keep = &k;
    before = k;
    CHECK(bat_init(B, &c, &OPS, NULL) == -1 && memcmp(&k, &before, sizeof(k)) == 0,
          "seq keep: a refused config leaves the record as it was");

    k = good;
    r = init_keep(&k, other, NULL);
    r2 = init_keep(&k, other, NULL);
    CHECK(r == 0 && r2 == 1 && B->elp_seq == 1 + BAT_SEQ_MARGIN,
          "seq keep: another node's record: random seqnos, and the record is now this node's (%d, %d)", r, r2);
    unsigned bad = 0;
    for (size_t i = 0; i < sizeof(k) * 8; i++) {
        k = good;
        ((uint8_t *)&k)[i / 8] ^= (uint8_t)(1u << (i % 8));
        r = init_keep(&k, W, NULL);
        bad += r != 0 || B->elp_seq != 1 || B->ogm_seq != 2 || B->bcast_seq != 3;
    }
    CHECK(bad == 0, "seq keep: any one of its %zu bits flipped: random seqnos (%u resumed)", sizeof(k) * 8, bad);
    memset(&k, 0xff, sizeof(k));
    r = init_keep(&k, W, NULL);
    CHECK(r == 0 && B->elp_seq == 1, "seq keep: all ones: random seqnos");
    for (size_t i = 0; i < sizeof(k); i++) {
        ((uint8_t *)&k)[i] = (uint8_t)(i * 37 + 11);
    }
    memcpy(&k, &good, 4);
    r = init_keep(&k, W, NULL);
    CHECK(r == 0 && B->elp_seq == 1, "seq keep: garbage after a valid magic: random seqnos");

    static const uint32_t hi[] = { 0xFFFFFF80u, 0xFFFFFF90u, 0xFFFFFFA0u, 0x10000, 5, 6, 7 };
    memset(&k, 0, sizeof(k));
    init_keep(&k, W, hi);
    r = init_keep(&k, W, NULL);
    CHECK(r == 1 && B->elp_seq == 0x80 && B->ogm_seq == 0x90 && B->bcast_seq + 1 == 0xA1,
          "seq keep: resumed across 2^32 (%08x %08x %08x)", B->elp_seq, B->ogm_seq, B->bcast_seq + 1);
    r = init_keep(NULL, W, NULL);
    CHECK(r == 0 && B->elp_seq == 1, "seq keep: none (NULL): random seqnos, 0");
}

static void test_timers(void)
{
    fresh();
    uint8_t n1[6];
    mac(n1, 0x11, 1);
    add_neigh(n1, n1, 100);
    uint32_t pe = B->next_elp, po = B->next_ogm, pa = B->next_agg;
    int bad_elp = 0, bad_ogm = 0, bad_agg = 0, bad_ret = 0, elps = 0, ogms = 0;
    uint32_t minr = ~0u, maxr = 0;
    for (int i = 0; i < 60000; i++) {
        S.now++;
        uint32_t r = bat_tick(B);
        const uint32_t dl[5] = { B->next_elp, B->next_ogm, B->next_agg, B->next_purge, B->next_tt_purge };
        uint32_t m = ~0u;
        for (int k = 0; k < 5; k++) {
            uint32_t d = dl[k] - S.now;
            m = d < m ? d : m;
        }
        if (r != (m > 1000 ? 1000 : (m < 1 ? 1 : m))) {
            bad_ret++;
        }
        if (B->next_elp != pe) {
            uint32_t d = B->next_elp - S.now;
            bad_elp += d < 480 || d > 519;
            minr = d < minr ? d : minr;
            maxr = d > maxr ? d : maxr;
            pe = B->next_elp;
            elps++;
        }
        if (B->next_ogm != po) {
            uint32_t d = B->next_ogm - S.now;
            bad_ogm += d < 980 || d > 1019;
            po = B->next_ogm;
            ogms++;
        }
        if (B->next_agg != pa) {
            uint32_t d = B->next_agg - S.now;
            bad_agg += d < 90 || d > 109;
            pa = B->next_agg;
        }
    }
    CHECK(bad_elp == 0 && elps > 110, "ELP re-arm in [480, 519] ms (%d ELPs, %u..%u)", elps, minr, maxr);
    CHECK(bad_ogm == 0 && ogms > 55, "OGM re-arm in [980, 1019] ms (%d OGMs)", ogms);
    CHECK(bad_agg == 0, "aggregation flush re-arm in [90, 109] ms");
    CHECK(bad_ret == 0, "bat_tick always returns min(deadline - now) clamped to 1..1000");
}

static void test_gate(void)
{
    uint8_t f[1700], n1[6];
    mac(n1, 0x11, 1);
    fresh();
    add_neigh(n1, n1, 100);
    size_t len = mk_elp(f, n1, n1, 5000);

    snap(); rx(f, 15);
    CHECK(ONLY(BAT_C_RX, BAT_C_RX_SHORT), "gate: 15-byte frame -> rx_short");
    memset(f + 34, 0, 1700 - 34);
    snap(); rx(f, 1601);
    CHECK(ONLY(BAT_C_RX, BAT_C_RX_TOOBIG), "gate: 1601-byte frame -> rx_toobig");
    f[12] = 0x08; f[13] = 0x00;
    snap(); rx(f, len);
    CHECK(ONLY(BAT_C_RX, BAT_C_RX_TYPE), "gate: ethertype 0x0800 -> rx_type");
    len = mk_elp(f, n1, n1, 5001);
    f[15] = 14;
    snap(); rx(f, len);
    CHECK(ONLY(BAT_C_RX, BAT_C_RX_VERSION), "gate: version 14 -> rx_version");
    len = mk_elp(f, n1, n1, 5002);
    f[6] = 0x03;
    snap(); rx(f, len);
    CHECK(ONLY(BAT_C_RX, BAT_C_RX_SRC_BAD), "gate: group link source -> rx_src_bad");
    memset(f + 6, 0, 6);
    snap(); rx(f, len);
    CHECK(ONLY(BAT_C_RX, BAT_C_RX_SRC_BAD), "gate: zero link source -> rx_src_bad");
    len = mk_elp(f, W, n1, 5003);
    snap(); rx(f, len);
    CHECK(ONLY(BAT_C_RX, BAT_C_RX_SRC_OWN), "gate: own hard address as link source -> rx_src_own");
    len = mk_elp(f, n1, n1, 5004);
    f[0] = 0x01; f[1] = 0x00; f[2] = 0x5e;
    snap(); rx(f, len);
    CHECK(ONLY(BAT_C_RX, BAT_C_RX_MGMT_DST), "gate: ELP to a multicast address -> rx_mgmt_dst");
    len = mk_ogm(f, n1, n1, 77, 50, 100);
    memcpy(f, W, 6);
    snap(); rx(f, len);
    CHECK(ONLY(BAT_C_RX, BAT_C_RX_MGMT_DST), "gate: unicast OGM2 -> rx_mgmt_dst (packets §4.1)");
    len = mk_bcast(f, n1, n1, 5, 49, 20);
    memcpy(f, W, 6);
    snap(); rx(f, len);
    CHECK(ONLY(BAT_C_RX, BAT_C_RX_MGMT_DST), "gate: unicast BCAST -> rx_mgmt_dst");
    len = mk_elp(f, n1, n1, 5005);
    snap(); rx(f, 14 + 15);
    CHECK(ONLY(BAT_C_RX, BAT_C_RX_HDR), "gate: 15-byte ELP -> rx_hdr");
    len = mk_ogm(f, n1, n1, 78, 50, 100);
    snap(); rx(f, 14 + 19);
    CHECK(ONLY(BAT_C_RX, BAT_C_RX_HDR), "gate: 19-byte OGM2 -> rx_hdr");
    len = mk_bcast(f, n1, n1, 6, 49, 20);
    snap(); rx(f, 14 + 13);
    CHECK(ONLY(BAT_C_RX, BAT_C_RX_HDR), "gate: 13-byte BCAST -> rx_hdr");
    const uint8_t types[] = { 0x00, 0x02, 0x05, 0x06, 0x3f, 0x80, 0xff };
    for (unsigned i = 0; i < sizeof(types); i++) {
        len = mk_elp(f, n1, n1, 5006);
        f[14] = types[i];
        snap(); rx(f, len);
        CHECK(ONLY(BAT_C_RX, BAT_C_RX_TYPE), "gate: type 0x%02x has no handler -> rx_type", types[i]);
    }
    uint8_t other[6];
    mac(other, 0x99, 9);
    link_hdr(f, other, n1);
    memset(f + 14, 0, 40);
    f[14] = 0x40; f[15] = 0x0f; f[16] = 50;
    snap(); rx(f, 54);
    CHECK(ONLY(BAT_C_RX, BAT_C_RX_UNI_DST), "gate: UNICAST to a foreign MAC -> rx_uni_dst");
    link_hdr(f, BC, n1);
    f[14] = 0x45;
    snap(); rx(f, 54);
    CHECK(ONLY(BAT_C_RX, BAT_C_RX_UNI_DST), "gate: unicast-range type to broadcast -> rx_uni_dst");
    link_hdr(f, other, n1);
    f[14] = 0x7f;
    snap(); rx(f, 54);
    CHECK(ONLY(BAT_C_RX, BAT_C_RX_UNI_DST), "gate: type 0x7f to a foreign MAC -> rx_uni_dst");
}

static void test_probe_19a(void)
{
    /* Frame #3 of 19a-wifi-hwsim-startup__W1-mesh0.pcap: W2's unicast ELP probe to W1,
     * 200 bytes of batman payload, seqno 0, interval 0, zero padded. */
    uint8_t f[214] = { 0x02, 0x00, 0x00, 0x00, 0x1a, 0x01, 0x02, 0x00, 0x00, 0x00, 0x2a, 0x01,
                       0x43, 0x05, 0x03, 0x0f, 0x02, 0x00, 0x00, 0x00, 0x2a, 0x01 };
    struct bat_config c;
    stub_reset();
    cfg_default(&c);
    const uint8_t w1[6] = { 0x02, 0x00, 0x00, 0x00, 0x1a, 0x01 };
    memcpy(c.hard_addr, w1, 6);
    eng(&c);
    snap();
    rx(f, sizeof(f));
    CHECK(ONLY(BAT_C_RX, BAT_C_ELP_PROBE), "19a probe: counts only elp_probe");
    CHECK(bat_neigh_count(B) == 0 && orig_count() == 0, "19a probe: no neighbour, no originator");
}

static void test_elp_window(void)
{
    uint8_t f[64], n1[6], n2[6];
    mac(n1, 0x11, 1);
    mac(n2, 0x22, 1);
    fresh();
    snap(); rx(f, mk_elp(f, n1, n1, 0));
    CHECK(ONLY(BAT_C_RX, BAT_C_ELP_DUP, BAT_C_NEIGH_NEW, BAT_C_ORIG_NEW),
          "ELP: first seqno 0 is not accepted (d = 0)");
    struct bat_orig *o = bat_orig_find(B, n1);
    CHECK(bat_neigh_count(B) == 1 && o && cand_via(o, n1), "ELP: but the neighbour, originator and candidate exist");
    snap(); rx(f, mk_elp(f, n2, n2, 0xFFFFFFC1u));
    CHECK(ONLY(BAT_C_RX, BAT_C_ELP_DUP, BAT_C_NEIGH_NEW, BAT_C_ORIG_NEW), "ELP: first seqno 0xFFFFFFC1 not accepted");
    snap(); rx(f, mk_elp(f, n2, n2, 0xFFFFFFFFu));
    CHECK(ONLY(BAT_C_RX, BAT_C_ELP_DUP), "ELP: 0xFFFFFFFF still not accepted");
    snap(); rx(f, mk_elp(f, n2, n2, 0xFFFFFFC0u));
    CHECK(ONLY(BAT_C_RX, BAT_C_ELP_RX), "ELP: 0xFFFFFFC0 (d = -64) accepted as a restart");
    struct bat_neigh *n = bat_neigh_find(B, n1);
    rx(f, mk_elp(f, n1, n1, 100));
    CHECK(n->elp_seq == 100 && C(BAT_C_ELP_RX) == 2, "ELP: 100 accepted");
    snap(); rx(f, mk_elp(f, n1, n1, 100));
    CHECK(ONLY(BAT_C_RX, BAT_C_ELP_DUP), "ELP: 100 again is a duplicate");
    snap(); rx(f, mk_elp(f, n1, n1, 37));
    CHECK(ONLY(BAT_C_RX, BAT_C_ELP_DUP), "ELP: 37 (d = -63) is stale");
    S.now += 10;
    snap(); rx(f, mk_elp(f, n1, n1, 36));
    CHECK(ONLY(BAT_C_RX, BAT_C_ELP_RX) && n->elp_seq == 36 && n->last_seen == S.now,
          "ELP: 36 (d = -64) is a restart and refreshes last-seen");
    snap(); rx(f, mk_elp(f, n1, n1, 37));
    CHECK(ONLY(BAT_C_RX, BAT_C_ELP_RX), "ELP: 37 after the restart is new");
    CHECK(n->elp_interval == 500, "ELP: advertised interval stored");
    snap(); rx(f, mk_elp(f, n1, W, 38));
    CHECK(ONLY(BAT_C_RX, BAT_C_ELP_OWN_ORIG), "ELP: our own originator in the ELP -> elp_own_orig");
    uint8_t n3[6];
    mac(n3, 0x33, 2);
    uint8_t o3[6];
    mac(o3, 0x33, 1);
    rx(f, mk_elp(f, n3, o3, 5));
    n = bat_neigh_find(B, n3);
    CHECK(n && memcmp(n->orig, o3, 6) == 0 && bat_orig_find(B, o3) && !bat_orig_find(B, n3),
          "ELP: neighbour keyed by link source, originator by the ELP field (multi-interface sender)");
    rx(f, mk_elp(f, n3, n2, 6));
    CHECK(memcmp(n->orig, o3, 6) == 0, "ELP: the neighbour's originator is fixed at creation");
    struct bat_orig *oo = bat_orig_find(B, o3);
    uint32_t seen = oo->last_seen;
    S.now += 5000;
    rx(f, mk_elp(f, n3, o3, 7));
    CHECK(oo->last_seen == seen && cand_via(oo, n3)->last_seen == S.now,
          "ELP: refreshes the candidate, never the originator's last-seen");
}

static void test_ewma(void)
{
    uint8_t n1[6], f[64];
    mac(n1, 0x11, 1);
    fresh();
    set_tput(n1, 100000);
    rx(f, mk_elp(f, n1, n1, 1000));
    struct bat_neigh *n = bat_neigh_find(B, n1);
    CHECK(bat_neigh_tput(n) == 0, "EWMA: a new neighbour reads 0 until our next ELP");
    sample();
    CHECK(bat_neigh_tput(n) == 100000, "EWMA: first sample adopted (100000)");
    set_tput(n1, 200);
    const uint32_t exp[5] = { 87525, 76609, 67058, 58700, 51388 };
    int ok = 1;
    for (int i = 0; i < 5; i++) {
        sample();
        ok &= bat_neigh_tput(n) == exp[i];
    }
    CHECK(ok, "EWMA: 100000 -> 200 reads 87525, 76609, 67058, 58700, 51388 (elp-ogm §2.5)");
    for (int i = 5; i < 86; i++) {
        sample();
    }
    uint32_t at86 = bat_neigh_tput(n);
    sample();
    CHECK(at86 == 201 && bat_neigh_tput(n) == 200, "EWMA: reaches exactly 200 at sample 87 (86 reads %u)", at86);
    fresh();
    set_tput(n1, 10);
    rx(f, mk_elp(f, n1, n1, 1000));
    n = bat_neigh_find(B, n1);
    sample();
    set_tput(n1, 1000);
    uint32_t mx = 0;
    for (int i = 0; i < 300; i++) {
        sample();
        mx = bat_neigh_tput(n) > mx ? bat_neigh_tput(n) : mx;
    }
    CHECK(bat_neigh_tput(n) == 999 && mx == 999, "EWMA: rising toward 1000 stalls at 999");

    fresh();
    set_tput(n1, BAT_TPUT_UNKNOWN);
    rx(f, mk_elp(f, n1, n1, 1000));
    n = bat_neigh_find(B, n1);
    sample();
    CHECK(bat_neigh_tput(n) == 10, "link_tput BAT_TPUT_UNKNOWN samples 10 (1 Mbit/s)");
    set_tput(n1, 0);
    sample();
    CHECK(bat_neigh_tput(n) == 8, "link_tput 0 (no station) samples 0: 10 decays to 8");
    uint8_t n2[6];
    mac(n2, 0x22, 1);
    rx(f, mk_elp(f, n2, n2, 1000));
    set_tput(n2, 0);
    sample();
    CHECK(bat_neigh_tput(bat_neigh_find(B, n2)) == 0, "link_tput 0 on a new neighbour keeps it at 0");
    struct bat_config c;
    stub_reset();
    cfg_default(&c);
    c.tput_override = 555;
    eng(&c);
    set_tput(n1, 1000);
    rx(f, mk_elp(f, n1, n1, 1000));
    set_tput(n2, BAT_TPUT_UNKNOWN);
    rx(f, mk_elp(f, n2, n2, 1000));
    sample();
    CHECK(bat_neigh_tput(bat_neigh_find(B, n1)) == 555 && bat_neigh_tput(bat_neigh_find(B, n2)) == 555,
          "a nonzero tput_override wins over the station estimate and over unknown");
}

/* Two neighbours so the interface table is processed; returns the originator X's record. */
static struct bat_orig *penalty_case(bool half, uint32_t link, uint32_t adv, uint32_t seq)
{
    struct bat_config c;
    uint8_t n1[6], n2[6], x[6], f[128];
    mac(n1, 0x11, 1);
    mac(n2, 0x22, 1);
    mac(x, 0x55, 1);
    stub_reset();
    cfg_default(&c);
    c.half_duplex = half;
    eng(&c);
    add_neigh(n1, n1, link);
    add_neigh(n2, n2, link);
    rx(f, mk_ogm(f, n1, x, seq, 50, adv));
    return bat_orig_find(B, x);
}

static uint32_t fwd_tput_of(const uint8_t *orig)
{
    bat_ogm_flush(B);
    for (unsigned k = 0; k < S.ntx; k++) {
        const struct frame *t = &S.tx[k];
        size_t off = 14;
        while (t->b[14] == 0x04 && off + 20 <= t->len) {
            const uint8_t *r = t->b + off;
            if (memcmp(r + 8, orig, 6) == 0 && memcmp(orig, W, 6) != 0) {
                return bat_get32(r + 16);
            }
            off += 20 + bat_get16(r + 14);
        }
    }
    return 0xDEADBEEF;
}

static void test_penalty(void)
{
    uint8_t x[6], n1[6];
    mac(x, 0x55, 1);
    mac(n1, 0x11, 1);
    struct { bool half; uint32_t link, adv, def, ifc; const char *what; } rows[] = {
        { false, 100000, 0xFFFFFFFFu, 100000, 88235, "wired 10 Gbit/s straight from the originator" },
        { false, 100000, 88235, 88235, 77854, "wired, received as a forward carrying 88235" },
        { true, 1000, 0xFFFFFFFFu, 1000, 500, "WiFi link 1000 straight from the originator (halved)" },
        { true, 1000, 500, 500, 250, "WiFi link 1000 received carrying 500" },
        { true, 10, 0xFFFFFFFFu, 10, 8, "WiFi link exactly 10: hop penalty, not halved" },
        { true, 11, 0xFFFFFFFFu, 11, 5, "WiFi link 11: halved" },
        { true, 799, 0xFFFFFFFFu, 799, 399, "WiFi P1 = 799 (row 7 after its per-interface penalty) -> 399" },
    };
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        struct bat_orig *o = penalty_case(rows[i].half, rows[i].link, rows[i].adv, 1000);
        struct bat_cand *c = cand_via(o, n1);
        CHECK(c && c->t[BAT_TBL_DEFAULT].tput == rows[i].def && c->t[BAT_TBL_IFACE].tput == rows[i].ifc &&
              fwd_tput_of(x) == rows[i].ifc,
              "penalty §3.5: %s -> default %u, interface %u, forwarded %u", rows[i].what, rows[i].def,
              rows[i].ifc, rows[i].ifc);
    }
    /* elp-ogm §7.6 inversion */
    const uint32_t p[3] = { 10, 11, 16 }, want[3] = { 8, 5, 8 };
    for (int i = 0; i < 3; i++) {
        penalty_case(true, 100000, p[i], 2000);
        CHECK(fwd_tput_of(x) == want[i], "inversion §7.6: P = %u forwards %u", p[i], want[i]);
    }
    /* hop horizon: feed each forwarded value back in as the next hop's advertisement */
    const struct { uint32_t start; unsigned hops; } hz[] = { { 60, 10 }, { 10, 9 }, { 3, 3 }, { 1, 1 } };
    for (int i = 0; i < 4; i++) {
        uint32_t v = hz[i].start;
        unsigned hops = 0;
        char seqbuf[128];
        int w = 0;
        while (v != 0 && hops < 20) {
            penalty_case(true, 100000, v, 3000 + hops);
            v = fwd_tput_of(x);
            if (v == 0xDEADBEEF) {
                v = 0;   /* not forwarded: carried 0 */
            }
            hops++;
            w += snprintf(seqbuf + w, sizeof(seqbuf) - (size_t)w, "%s%u", hops > 1 ? "," : "", v);
        }
        CHECK(hops == hz[i].hops, "horizon §7.6: start %u reaches 0 after %u hops (%s)", hz[i].start,
              hz[i].hops, seqbuf);
    }
}

static void test_ogm_seq(void)
{
    uint8_t n1[6], x[6], f[128];
    mac(n1, 0x11, 1);
    mac(x, 0x55, 1);
    fresh();
    add_neigh(n1, n1, 1000);
    snap(); rx(f, mk_ogm(f, n1, x, 1000, 50, 0xFFFFFFFFu));
    CHECK(ONLY(BAT_C_RX, BAT_C_OGM_RX, BAT_C_OGM_REC, BAT_C_ORIG_NEW, BAT_C_OGM_NEW),
          "OGM seq: first seqno 1000 is simply newer (no restart), one table (single neighbour)");
    struct bat_orig *o = bat_orig_find(B, x);
    CHECK(o && o->tbl[BAT_TBL_DEFAULT].last_seq == 1000 && o->tbl[BAT_TBL_DEFAULT].router >= 0,
          "OGM seq: stored and routed");
    snap(); rx(f, mk_ogm(f, n1, x, 1000, 50, 0xFFFFFFFFu));
    CHECK(ONLY(BAT_C_RX, BAT_C_OGM_RX, BAT_C_OGM_REC, BAT_C_OGM_SAME), "OGM seq: same seqno -> same");
    snap(); rx(f, mk_ogm(f, n1, x, 999, 50, 0xFFFFFFFFu));
    CHECK(ONLY(BAT_C_RX, BAT_C_OGM_RX, BAT_C_OGM_REC, BAT_C_OGM_OLD), "OGM seq: 999 -> old");
    snap(); rx(f, mk_ogm(f, n1, x, 1000 - 63, 50, 0xFFFFFFFFu));
    CHECK(ONLY(BAT_C_RX, BAT_C_OGM_RX, BAT_C_OGM_REC, BAT_C_OGM_OLD), "OGM seq: d = -63 -> old");
    uint32_t t0 = S.now;
    snap(); rx(f, mk_ogm(f, n1, x, 1000 - 64, 50, 0xFFFFFFFFu));
    CHECK(ONLY(BAT_C_RX, BAT_C_OGM_RX, BAT_C_OGM_REC, BAT_C_OGM_RESTART, BAT_C_OGM_NEW),
          "OGM seq: d = -64 -> restart, accepted as new");
    S.now = t0 + 10000;
    snap(); rx(f, mk_ogm(f, n1, x, 500000, 50, 0xFFFFFFFFu));
    CHECK(ONLY(BAT_C_RX, BAT_C_OGM_RX, BAT_C_OGM_REC, BAT_C_OGM_RESTART_BLOCKED),
          "OGM seq: a second restart 10 s later is blocked");
    uint32_t seen = o->last_seen;
    CHECK(seen == t0 && o->tbl[BAT_TBL_DEFAULT].last_seq == 936, "OGM seq: blocked record changes nothing");
    S.now = t0 + 29999;
    snap(); rx(f, mk_ogm(f, n1, x, 500000, 50, 0xFFFFFFFFu));
    CHECK(ONLY(BAT_C_RX, BAT_C_OGM_RX, BAT_C_OGM_REC, BAT_C_OGM_RESTART_BLOCKED),
          "OGM seq: still blocked 29.999 s after the first restart");
    S.now = t0 + 30001;
    snap(); rx(f, mk_ogm(f, n1, x, 500000, 50, 0xFFFFFFFFu));
    CHECK(ONLY(BAT_C_RX, BAT_C_OGM_RX, BAT_C_OGM_REC, BAT_C_OGM_RESTART, BAT_C_OGM_NEW),
          "OGM seq: accepted again 30.001 s after it");
    S.now += 100;
    snap(); rx(f, mk_ogm(f, n1, x, 500000 + 65535, 50, 0xFFFFFFFFu));
    CHECK(ONLY(BAT_C_RX, BAT_C_OGM_RX, BAT_C_OGM_REC, BAT_C_OGM_NEW), "OGM seq: +65535 is newer");
    snap(); rx(f, mk_ogm(f, n1, x, 500000 + 65535 + 65536, 50, 0xFFFFFFFFu));
    CHECK(ONLY(BAT_C_RX, BAT_C_OGM_RX, BAT_C_OGM_REC, BAT_C_OGM_RESTART_BLOCKED),
          "OGM seq: +65536 is a restart (blocked, the last one was 0.1 s ago)");

    uint8_t y[6];
    mac(y, 0x56, 1);
    snap(); rx(f, mk_ogm(f, n1, y, 0, 50, 0xFFFFFFFFu));
    CHECK(ONLY(BAT_C_RX, BAT_C_OGM_RX, BAT_C_OGM_REC, BAT_C_ORIG_NEW, BAT_C_OGM_SAME),
          "OGM seq: a first seqno of 0 is 'same' (TVLVs not processed)");
    mac(y, 0x57, 1);
    snap(); rx(f, mk_ogm(f, n1, y, 0xFFFFFFC1u, 50, 0xFFFFFFFFu));
    CHECK(ONLY(BAT_C_RX, BAT_C_OGM_RX, BAT_C_OGM_REC, BAT_C_ORIG_NEW, BAT_C_OGM_OLD) &&
          bat_orig_find(B, y)->tbl[BAT_TBL_DEFAULT].router < 0,
          "OGM seq: a first seqno of 0xFFFFFFC1 is dropped as old");
    mac(y, 0x58, 1);
    snap(); rx(f, mk_ogm(f, n1, y, 65536, 50, 0xFFFFFFFFu));
    CHECK(ONLY(BAT_C_RX, BAT_C_OGM_RX, BAT_C_OGM_REC, BAT_C_ORIG_NEW, BAT_C_OGM_RESTART, BAT_C_OGM_NEW),
          "OGM seq: a first seqno of 65536 counts as a restart and is accepted");

    snap(); rx(f, mk_ogm(f, n1, W, 1, 50, 0xFFFFFFFFu));
    CHECK(ONLY(BAT_C_RX, BAT_C_OGM_RX, BAT_C_OGM_REC, BAT_C_OGM_OWN), "OGM: our own originator ignored");
    mac(y, 0x59, 1);
    snap(); rx(f, mk_ogm(f, n1, y, 1, 50, 0));
    CHECK(ONLY(BAT_C_RX, BAT_C_OGM_RX, BAT_C_OGM_REC, BAT_C_OGM_TPUT0) && !bat_orig_find(B, y),
          "OGM: throughput 0 ignored, no state created");
}

static void test_elp_first(void)
{
    uint8_t n1[6], x[6], f[128];
    mac(n1, 0x11, 1);
    mac(x, 0x55, 1);
    fresh();
    snap(); rx(f, mk_ogm(f, n1, n1, 1000, 50, 0xFFFFFFFFu));
    rx(f, mk_ogm(f, n1, x, 1000, 50, 0xFFFFFFFFu));
    CHECK(C(BAT_C_OGM_NOT_NEIGH) == 2 && orig_count() == 0 && bat_neigh_count(B) == 0,
          "ELP first: OGM2 from a link source that sent no ELP creates no originator and no candidate");
}

static void test_router(void)
{
    uint8_t n1[6], n2[6], x[6], f[128];
    mac(n1, 0x11, 1);
    mac(n2, 0x22, 1);
    mac(x, 0x55, 1);
    fresh();
    add_neigh(n1, n1, 1000);
    add_neigh(n2, n2, 1000);
    rx(f, mk_ogm(f, n1, x, 100, 50, 500));
    struct bat_orig *o = bat_orig_find(B, x);
    int c1 = (int)(cand_via(o, n1) - o->cand);
    CHECK(o->tbl[BAT_TBL_DEFAULT].router == c1, "router: the first candidate becomes router");
    rx(f, mk_ogm(f, n2, x, 100, 50, 500));
    int c2 = (int)(cand_via(o, n2) - o->cand);
    CHECK(o->tbl[BAT_TBL_DEFAULT].router == c1, "router: a tie keeps the incumbent");
    rx(f, mk_ogm(f, n2, x, 101, 50, 600));
    CHECK(o->tbl[BAT_TBL_DEFAULT].router == c2, "router: strictly better switches");
    rx(f, mk_ogm(f, n1, x, 102, 50, 500));
    rx(f, mk_ogm(f, n1, x, 105, 50, 500));
    CHECK(o->tbl[BAT_TBL_DEFAULT].router == c2, "router: worse and only 4 seqnos ahead keeps the router");
    rx(f, mk_ogm(f, n1, x, 106, 50, 500));
    CHECK(o->tbl[BAT_TBL_DEFAULT].router == c1, "router: 5 seqnos ahead of a stale router switches");
    CHECK(memcmp(bat_route_nh(B, o, BAT_TBL_DEFAULT), n1, 6) == 0 && bat_route_tput(B, o) == 500,
          "route lookup: next hop and default throughput of the router");

    /* default vs interface table apart (elp-ogm §7.6 inversion) */
    fresh();
    add_neigh(n1, n1, 10);
    add_neigh(n2, n2, 11);
    rx(f, mk_ogm(f, n1, x, 300, 50, 0xFFFFFFFFu));
    rx(f, mk_ogm(f, n2, x, 300, 50, 0xFFFFFFFFu));
    o = bat_orig_find(B, x);
    CHECK(memcmp(bat_route_nh(B, o, BAT_TBL_DEFAULT), n2, 6) == 0 &&
          memcmp(bat_route_nh(B, o, BAT_TBL_IFACE), n1, 6) == 0,
          "own traffic (default table) via the P = 11 link, relayed traffic (interface table) via P = 10");
    CHECK(bat_route_tput(B, o) == 11 && cand_via(o, n1)->t[BAT_TBL_IFACE].tput == 8 &&
          cand_via(o, n2)->t[BAT_TBL_IFACE].tput == 5, "... with default 11 and interface values 8 vs 5");
}

static void test_suppression(void)
{
    uint8_t n1[6], n2[6], x[6], o3[6], f[128];
    mac(n1, 0x11, 1);
    mac(n2, 0x22, 1);
    mac(x, 0x55, 1);
    mac(o3, 0x33, 1);
    /* row 1: no neighbour */
    fresh();
    S.now = B->next_ogm;
    uint32_t seq = B->ogm_seq;
    bat_tick(B);
    CHECK(C(BAT_C_OGM_SUPP_OWN) == 1 && B->ogm_seq == seq + 1 && B->agg_len == 0,
          "§3.9 no neighbour: own OGM suppressed, seqno still advances");
    CHECK(bat_flood_suppressed(B, x, NULL), "§3.9 no neighbour: received OGM suppressed");
    /* rows 2-4: exactly one neighbour */
    add_neigh(n1, n1, 1000);
    S.now = B->next_ogm;
    bat_tick(B);
    CHECK(C(BAT_C_OGM_TX_OWN) == 1, "§3.9 one neighbour: own OGM sent");
    CHECK(bat_flood_suppressed(B, n1, n1), "§3.9 one neighbour = the originator: suppressed");
    CHECK(bat_flood_suppressed(B, n1, NULL) && bat_flood_suppressed(B, n1, o3),
          "§3.9 one neighbour = the originator, whoever delivered it: suppressed");
    CHECK(bat_flood_suppressed(B, x, n1), "§3.9 one neighbour = the one who sent it: suppressed");
    CHECK(!bat_flood_suppressed(B, x, o3), "§3.9 one neighbour, anything else: processed");
    rx(f, mk_ogm(f, n1, x, 10, 50, 0xFFFFFFFFu));
    struct bat_orig *o = bat_orig_find(B, x);
    CHECK(o->tbl[BAT_TBL_IFACE].router < 0 && !cand_via(o, n1)->t[BAT_TBL_IFACE].valid &&
          o->tbl[BAT_TBL_IFACE].last_seq == 0, "§3.9 leaf: no interface-table state at all");
    CHECK(C(BAT_C_OGM_TX_FWD) == 0, "§3.9 leaf: forwards nothing");
    /* row 5: two neighbours */
    add_neigh(n2, n2, 1000);
    CHECK(!bat_flood_suppressed(B, n1, n1) && !bat_flood_suppressed(B, x, n2),
          "§3.9 two neighbours: processed, may forward");
    rx(f, mk_ogm(f, n1, x, 11, 50, 0xFFFFFFFFu));
    CHECK(o->tbl[BAT_TBL_IFACE].router >= 0 && C(BAT_C_OGM_TX_FWD) == 1, "§3.9 two neighbours: forwarded");
}

static void test_forward(void)
{
    uint8_t n1[6], n2[6], x[6], f[256];
    mac(n1, 0x11, 1);
    mac(n2, 0x22, 1);
    mac(x, 0x55, 1);
    fresh();
    add_neigh(n1, n1, 1000);
    add_neigh(n2, n2, 1000);
    const uint8_t tv[] = { 0x09, 0x01, 0x00, 0x04, 0xde, 0xad, 0xbe, 0xef,
                           0x04, 0x01, 0x00, 0x04, 0x01, 0x07, 0x00, 0x00 };
    link_hdr(f, BC, n1);
    size_t len = 14 + ogm_rec(f + 14, x, 7000, 50, 0xFFFFFFFFu, tv, sizeof(tv));
    f[14 + 3] = 0x5a;   /* flags: receivers ignore them, forwarders copy them */
    S.ntx = 0;
    rx(f, len);
    bat_ogm_flush(B);
    unsigned k = 0;
    int i = next_ogm_tx(&k);
    const uint8_t *r = i >= 0 ? S.tx[i].b + 14 : NULL;
    CHECK(i >= 0 && S.tx[i].len == len && memcmp(S.tx[i].b, BC, 6) == 0 && memcmp(S.tx[i].b + 6, W, 6) == 0,
          "forward: one broadcast OGM frame from our hard address");
    CHECK(r && r[2] == 49 && bat_get32(r + 16) == 500, "forward: TTL - 1 and the interface-table throughput");
    uint8_t exp[256];
    memcpy(exp, f + 14, len - 14);
    exp[2] = 49;
    bat_put32(exp + 16, 500);
    CHECK(r && memcmp(r, exp, len - 14) == 0, "forward: every other byte copied, flags and TVLVs included");
    S.ntx = 0;
    rx(f, len);
    bat_ogm_flush(B);
    CHECK(C(BAT_C_OGM_TX_FWD) == 1 && S.ntx == 0, "forward: once per seqno (same copy again not forwarded)");
    rx(f, mk_ogm(f, n2, x, 7000, 50, 0xFFFFFFFFu));
    CHECK(C(BAT_C_OGM_TX_FWD) == 1, "forward: a copy from a non-router neighbour is never forwarded");
    set_tput(n2, 400);
    bat_neigh_find(B, n2)->tput_acc = 400 << 10;
    S.ntx = 0;
    rx(f, mk_ogm(f, n2, x, 7001, 50, 0xFFFFFFFFu));
    bat_ogm_flush(B);
    CHECK(C(BAT_C_OGM_TX_FWD) == 1 && S.ntx == 0,
          "forward: a new seqno first delivered by a worse non-router neighbour is not forwarded");
    rx(f, mk_ogm(f, n1, x, 7001, 50, 0xFFFFFFFFu));
    CHECK(C(BAT_C_OGM_TX_FWD) == 2 && fwd_tput_of(x) == 500,
          "forward: the router's later copy of that seqno is (with the router's value, 500)");
    rx(f, mk_ogm(f, n1, x, 7002, 1, 0xFFFFFFFFu));
    CHECK(C(BAT_C_OGM_TX_FWD) == 2 && C(BAT_C_OGM_FWD_TTL) == 1, "forward: TTL 1 not forwarded");
    struct bat_orig *o = bat_orig_find(B, x);
    CHECK(o->tbl[BAT_TBL_IFACE].last_fwd_seq == 7002, "forward: last forwarded seqno set even when refused");
    rx(f, mk_ogm(f, n1, x, 7003, 50, 1));
    CHECK(C(BAT_C_OGM_TX_FWD) == 2 && C(BAT_C_OGM_FWD_TPUT0) == 1,
          "forward: a record whose forwarded throughput is 0 is not forwarded (deviation 2.4.2)");
    rx(f, mk_ogm(f, n1, x, 7004, 2, 0xFFFFFFFFu));
    CHECK(C(BAT_C_OGM_TX_FWD) == 3, "forward: TTL 2 forwarded (with TTL 1)");
}

static void test_tvlv_once(void)
{
    uint8_t n1[6], n2[6], x[6], f[256];
    mac(n1, 0x11, 1);
    mac(n2, 0x22, 1);
    mac(x, 0x55, 1);
    fresh();
    add_neigh(n1, n1, 1000);
    add_neigh(n2, n2, 1000);
    /* TT diff (ttvn 1, one VLAN, one ADD) and GW 100/20 */
    uint8_t cl[6] = { 0x06, 0x55, 0x00, 0x00, 0x00, 0x01 };
    uint8_t tv[64];
    size_t tl = 0;
    const uint8_t tth[] = { 0x04, 0x01, 0x00, 0x18, 0x01, 0x01, 0x00, 0x01 };
    memcpy(tv, tth, 8);
    tl = 8;
    bat_put32(tv + tl, bat_crc32c_tt(0, 0, cl));
    tl += 4;
    memset(tv + tl, 0, 4);
    tl += 4;
    memset(tv + tl, 0, 12);
    memcpy(tv + tl + 4, cl, 6);
    tl += 12;
    const uint8_t gw[] = { 0x01, 0x01, 0x00, 0x08, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x14 };
    memcpy(tv + tl, gw, sizeof(gw));
    tl += sizeof(gw);
    link_hdr(f, BC, n1);
    size_t len = 14 + ogm_rec(f + 14, x, 4000, 50, 0xFFFFFFFFu, tv, (uint16_t)tl);
    rx(f, len);
    struct bat_orig *o = bat_orig_find(B, x);
    CHECK(o->gw_valid && o->gw_down == 100 && o->gw_up == 20, "TVLV: GW 100/20 parsed on a new seqno");
    struct bat_tt before = B->tt;
    uint32_t tt_cnt[BAT_C__COUNT];
    for (int i = BAT_C_UT_RX; i <= BAT_C_TT_BAD; i++) {
        tt_cnt[i] = C((enum bat_counter)i);
    }
    struct bat_tt_orig ot = o->tt;
    /* the same seqno through the other neighbour, without the GW container */
    link_hdr(f, BC, n2);
    len = 14 + ogm_rec(f + 14, x, 4000, 50, 0xFFFFFFFFu, tv, (uint16_t)(tl - sizeof(gw)));
    rx(f, len);
    int same = 1;
    for (int i = BAT_C_UT_RX; i <= BAT_C_TT_BAD; i++) {
        same &= tt_cnt[i] == C((enum bat_counter)i);
    }
    CHECK(cand_via(o, n2) && cand_via(o, n2)->t[BAT_TBL_DEFAULT].seq == 4000,
          "TVLV once: the second copy updates its candidate");
    CHECK(o->gw_valid == 1, "TVLV once: the second copy is not parsed (GW absence not applied)");
    CHECK(same && memcmp(&before, &B->tt, sizeof(before)) == 0 && memcmp(&ot, &o->tt, sizeof(ot)) == 0,
          "TVLV once: every TT counter and row untouched by the second copy");
    link_hdr(f, BC, n1);
    len = 14 + ogm_rec(f + 14, x, 4001, 50, 0xFFFFFFFFu, tv, (uint16_t)(tl - sizeof(gw)));
    rx(f, len);
    CHECK(o->gw_valid == 0 && o->gw_down == 0, "TVLV: an OGM without GW withdraws the gateway");
    const uint8_t gw0[] = { 0x01, 0x01, 0x00, 0x08, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x00 };
    const uint8_t gws[] = { 0x01, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x64 };
    const uint8_t gw2[] = { 0x01, 0x02, 0x00, 0x08, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x14 };
    rx(f, 14 + ogm_rec(f + 14, x, 4002, 50, 0xFFFFFFFFu, gw, sizeof(gw)));
    int was = o->gw_valid;
    rx(f, 14 + ogm_rec(f + 14, x, 4003, 50, 0xFFFFFFFFu, gw0, sizeof(gw0)));
    CHECK(was && o->gw_valid == 0, "TVLV: GW with upload 0 is not a gateway (packets §11.2)");
    rx(f, 14 + ogm_rec(f + 14, x, 4004, 50, 0xFFFFFFFFu, gws, sizeof(gws)));
    CHECK(o->gw_valid == 0, "TVLV: GW shorter than 8 bytes is not a gateway");
    rx(f, 14 + ogm_rec(f + 14, x, 4005, 50, 0xFFFFFFFFu, gw2, sizeof(gw2)));
    CHECK(o->gw_valid == 0, "TVLV: GW version 2 is not understood (skipped, counts as absent)");
}

static void test_ogm_walk(void)
{
    uint8_t n1[6], x[6], y[6], z[6];
    static uint8_t f[1700];
    mac(n1, 0x11, 1);
    mac(x, 0x55, 1);
    mac(y, 0x56, 1);
    mac(z, 0x57, 1);
    fresh();
    add_neigh(n1, n1, 1000);
    size_t len = link_hdr(f, BC, n1);
    len += ogm_rec(f + len, x, 1, 50, 0xFFFFFFFFu, NULL, 8);
    len += ogm_rec(f + len, y, 1, 50, 0xFFFFFFFFu, NULL, 0);
    snap(); rx(f, len);
    CHECK(C(BAT_C_OGM_REC) - SNAP[BAT_C_OGM_REC] == 2 && bat_orig_find(B, x) && bat_orig_find(B, y) &&
          C(BAT_C_OGM_RX) - SNAP[BAT_C_OGM_RX] == 1, "walk: two records in one frame, both processed");

    len = link_hdr(f, BC, n1);
    len += ogm_rec(f + len, x, 2, 50, 0xFFFFFFFFu, NULL, 0);
    size_t second = len;
    len += ogm_rec(f + len, z, 2, 50, 0xFFFFFFFFu, NULL, 40);
    bat_put16(f + second + 14, 41);
    snap(); rx(f, len);
    CHECK(C(BAT_C_OGM_OVERRUN) - SNAP[BAT_C_OGM_OVERRUN] == 1 && !bat_orig_find(B, z) &&
          bat_orig_find(B, x)->tbl[0].last_seq == 2, "walk: a record ending past the frame stops the walk, the first stays processed");

    len = link_hdr(f, BC, n1);
    len += ogm_rec(f + len, z, 3, 50, 0xFFFFFFFFu, NULL, 580);
    snap(); rx(f, len);
    CHECK(len == 14 + 600 && bat_orig_find(B, z) && bat_orig_find(B, z)->tbl[0].router >= 0,
          "walk: a 600-byte single record is processed (no 512-byte receive cap)");

    uint8_t v[6];
    mac(v, 0x60, 1);
    len = link_hdr(f, BC, n1);
    len += ogm_rec(f + len, x, 4, 50, 0xFFFFFFFFu, NULL, 0);
    size_t bad = len;
    len += ogm_rec(f + len, y, 4, 50, 0xFFFFFFFFu, NULL, 0);
    len += ogm_rec(f + len, v, 4, 50, 0xFFFFFFFFu, NULL, 0);
    f[bad] = 0x99;
    snap(); rx(f, len);
    CHECK(C(BAT_C_OGM_BADREC) - SNAP[BAT_C_OGM_BADREC] == 1 && !bat_orig_find(B, v) &&
          bat_orig_find(B, y)->tbl[0].last_seq == 1, "walk: a bad second record stops the walk");
    f[bad] = 0x04;
    f[bad + 1] = 0x0e;
    snap(); rx(f, len);
    CHECK(C(BAT_C_OGM_BADREC) - SNAP[BAT_C_OGM_BADREC] == 1, "walk: so does a bad version in a later record");

    len = link_hdr(f, BC, n1);
    len += ogm_rec(f + len, x, 6, 50, 0xFFFFFFFFu, NULL, 0);
    memset(f + len, 0, 19);
    len += 19;
    snap(); rx(f, len);
    CHECK(ONLY(BAT_C_RX, BAT_C_OGM_RX, BAT_C_OGM_REC, BAT_C_OGM_NEW),
          "walk: 19 trailing bytes are ignored silently");
    len = link_hdr(f, BC, n1);
    len += ogm_rec(f + len, x, 7, 50, 0xFFFFFFFFu, NULL, 0);
    memset(f + len, 0, 20);
    f[len] = 0x04;
    f[len + 1] = 0x0f;
    len += 20;
    snap(); rx(f, len);
    CHECK(C(BAT_C_OGM_TPUT0) - SNAP[BAT_C_OGM_TPUT0] == 1,
          "walk: 20 bytes of zero-padded record parse as throughput 0 and are dropped");
}

static void test_own_ogm(void)
{
    uint8_t n1[6];
    mac(n1, 0x11, 1);
    fresh();
    S.now = B->next_ogm;
    uint32_t s0 = B->ogm_seq;
    bat_tick(B);
    add_neigh(n1, n1, 1000);
    S.ntx = 0;
    run_to(S.now + 1200);
    unsigned k = 0;
    int i = next_ogm_tx(&k);
    const uint8_t *r = i >= 0 ? S.tx[i].b + 14 : NULL;
    uint16_t tl = r ? bat_get16(r + 14) : 0;
    CHECK(r && r[0] == 0x04 && r[1] == 0x0f && r[2] == 50 && r[3] == 0 && bat_get32(r + 4) == s0 + 1 &&
          memcmp(r + 8, W, 6) == 0 && bat_get32(r + 16) == 0xFFFFFFFFu,
          "own OGM: 04 0f 32 00, seqno after the suppressed one, our originator, throughput max");
    CHECK(r && S.tx[i].len == 14u + 20u + tl && 20u + tl <= 512 && memcmp(S.tx[i].b, BC, 6) == 0 &&
          memcmp(S.tx[i].b + 6, W, 6) == 0, "own OGM: one record of %u bytes <= 512, broadcast from our hard address",
          20u + tl);
    CHECK(S.tx[i].t > B->next_ogm - 2000, "own OGM: leaves on the next aggregation flush");
}

static void test_aggregation(void)
{
    uint8_t n1[6], n2[6];
    static uint8_t f[1700];
    mac(n1, 0x11, 1);
    mac(n2, 0x22, 1);
    struct { uint16_t mtu; bool agg; const char *what; } cases[] = {
        { 1500, true, "cap 512" }, { 300, true, "hard MTU 300: cap 300" }, { 1500, false, "aggregation off" },
    };
    for (unsigned cse = 0; cse < 3; cse++) {
        struct bat_config c;
        stub_reset();
        cfg_default(&c);
        c.hard_mtu = cases[cse].mtu;
        c.aggregate_ogm = cases[cse].agg;
        eng(&c);
        B->next_ogm = S.now + 100000;   /* keep own OGMs out of this measurement */
        add_neigh(n1, n1, 1000);
        add_neigh(n2, n2, 1000);
        size_t len = link_hdr(f, BC, n1);
        for (int i = 0; i < 10; i++) {
            uint8_t x[6];
            mac(x, 0x70, (uint8_t)i);
            len += ogm_rec(f + len, x, 100, 50, 0xFFFFFFFFu, NULL, 100);
        }
        S.ntx = 0;
        rx(f, len);
        uint8_t big[6];
        mac(big, 0x71, 1);
        len = link_hdr(f, BC, n1);
        len += ogm_rec(f + len, big, 100, 50, 0xFFFFFFFFu, NULL, 580);
        rx(f, len);
        run_to(S.now + 300);
        unsigned frames = 0, recs = 0, over = 0, lone_big = 0, k = 0;
        size_t cap = cases[cse].mtu < 512 ? cases[cse].mtu : 512;
        int i;
        while ((i = next_ogm_tx(&k)) >= 0) {
            size_t pl = S.tx[i].len - 14, off = 0;
            unsigned n = 0;
            while (off + 20 <= pl) {
                off += 20 + bat_get16(S.tx[i].b + 14 + off + 14);
                n++;
            }
            frames++;
            recs += n;
            if (pl > cap) {
                if (n == 1) {
                    lone_big++;
                } else {
                    over++;
                }
            }
        }
        unsigned per = (unsigned)(cap / 120);
        bool fits = cases[cse].mtu >= 600;
        unsigned want = (cases[cse].agg ? (10 + per - 1) / per : 10) + (fits ? 1 : 0);
        if (fits) {
            CHECK(recs == 11 && over == 0 && lone_big == 1 && frames == want && C(BAT_C_OGM_TOOBIG) == 0,
                  "aggregation (%s): 11 forwarded records in %u frames (want %u), none over the cap but the lone "
                  "600-byte record", cases[cse].what, frames, want);
        } else {
            CHECK(recs == 10 && over == 0 && lone_big == 0 && frames == want && C(BAT_C_OGM_TOOBIG) == 1,
                  "aggregation (%s): 10 forwarded records in %u frames (want %u); the 600-byte record is over the "
                  "hard MTU: dropped (ogm_toobig), never sent above it", cases[cse].what, frames, want);
        }
    }
    /* a record above the hard MTU is never forwarded (dataplane §9.3: nothing unfragmented above it) */
    struct bat_config c;
    stub_reset();
    cfg_default(&c);
    eng(&c);
    add_neigh(n1, n1, 1000);
    add_neigh(n2, n2, 1000);
    uint8_t z[6];
    mac(z, 0x72, 1);
    const uint16_t sizes[3] = { 1500, 1504, 1536 };
    for (int i = 0; i < 3; i++) {
        size_t len = link_hdr(f, BC, n1);
        len += ogm_rec(f + len, z, (uint32_t)(200 + i), 50, 0xFFFFFFFFu, NULL, (uint16_t)(sizes[i] - 20));
        S.ntx = 0;
        snap();
        rx(f, len);
        run_to(S.now + 150);
        unsigned k = 0, big = 0;
        int j;
        while ((j = next_ogm_tx(&k)) >= 0) {
            big += S.tx[j].len - 14 > 1500;
        }
        bool fwd = C(BAT_C_OGM_TX_FWD) - SNAP[BAT_C_OGM_TX_FWD] == 1;
        CHECK(big == 0 && (sizes[i] <= 1500 ? fwd : !fwd && C(BAT_C_OGM_TOOBIG) - SNAP[BAT_C_OGM_TOOBIG] == 1),
              "forwarding a %u-byte OGM2 record at hard MTU 1500: %s", sizes[i],
              sizes[i] <= 1500 ? "sent alone" : "dropped (ogm_toobig), nothing above the MTU");
    }
}

static void test_purge(void)
{
    uint8_t n1[6], x[6], f[128];
    mac(n1, 0x11, 1);
    mac(x, 0x55, 1);
    fresh();
    uint32_t t0 = S.now;
    add_neigh(n1, n1, 1000);
    rx(f, mk_ogm(f, n1, x, 10, 50, 0xFFFFFFFFu));
    struct bat_orig *o = bat_orig_find(B, x);
    o->tt.known = 1;
    o->tt.ttvn = 5;
    run_to(t0 + 199000);
    CHECK(bat_route_count(B) == 1 && bat_neigh_count(B) == 1, "purge: route and neighbour still there at 199 s");
    run_to(t0 + 201000);
    CHECK(bat_route_count(B) == 0 && C(BAT_C_ROUTE_LOST) == 1, "purge: candidates gone by 201 s, route lost");
    CHECK(bat_neigh_count(B) == 0 && C(BAT_C_NEIGH_PURGED) == 1, "purge: neighbour without candidates freed");
    CHECK(bat_orig_find(B, x) == o && o->tt.known == 0, "purge: originator kept, TT told to drop its entries");
    run_to(t0 + 399000);
    CHECK(bat_orig_find(B, x) != NULL, "purge: originator still there at 399 s");
    run_to(t0 + 401000);
    CHECK(bat_orig_find(B, x) == NULL && bat_orig_find(B, n1) == NULL && C(BAT_C_ORIG_PURGED) == 2,
          "purge: both originators gone by 401 s");

    /* an ELP-refreshed neighbour keeps its candidate; its originator still ages out */
    fresh();
    t0 = S.now;
    add_neigh(n1, n1, 1000);
    uint32_t seq = 2000;
    while ((int32_t)(S.now - (t0 + 450000)) < 0) {
        run_to(S.now + 500);
        rx(f, mk_elp(f, n1, n1, seq++));
    }
    CHECK(bat_neigh_count(B) == 1 && C(BAT_C_ORIG_PURGED) == 1 && bat_orig_find(B, n1) &&
          C(BAT_C_ORIG_NEW) == 2, "purge: an originator seen only by ELP is recreated after 400 s (elp-ogm §4.1)");

    /* recompute after a purge: best stored value wins, ties go to the lowest index */
    {
        uint8_t n2[6], n3[6];
        mac(n2, 0x22, 1);
        mac(n3, 0x33, 1);
        fresh();
        t0 = S.now;
        add_neigh(n1, n1, 1000);
        add_neigh(n2, n2, 1000);
        add_neigh(n3, n3, 1000);
        rx(f, mk_ogm(f, n1, x, 100, 50, 300));
        rx(f, mk_ogm(f, n2, x, 100, 50, 300));
        rx(f, mk_ogm(f, n3, x, 101, 50, 900));
        o = bat_orig_find(B, x);
        int c1 = (int)(cand_via(o, n1) - o->cand), c2 = (int)(cand_via(o, n2) - o->cand);
        int c3 = (int)(cand_via(o, n3) - o->cand);
        rx(f, mk_ogm(f, n1, x, 106, 50, 300));
        CHECK(o->tbl[0].router == c1, "recompute setup: router via n1 (5 seqnos ahead of n3)");
        /* keep n1 and n2 fresh, let n3's candidate expire */
        for (uint32_t k = 0; k < 205; k++) {
            run_to(S.now + 1000);
            rx(f, mk_elp(f, n1, n1, 2000 + k));
            rx(f, mk_elp(f, n2, n2, 2000 + k));
            cand_via(o, n1)->last_seen = S.now;
            cand_via(o, n2)->last_seen = S.now;
        }
        CHECK(!cand_via(o, n3) || !o->cand[c3].used, "recompute: n3's candidate expired");
        CHECK(o->tbl[0].router == (c1 < c2 ? c1 : c2),
              "recompute: equal stored values (300) go to the lowest candidate index");
    }
    /* restart timestamps are forgotten after 60 s */
    fresh();
    add_neigh(n1, n1, 1000);
    rx(f, mk_ogm(f, n1, x, 1000000, 50, 0xFFFFFFFFu));
    o = bat_orig_find(B, x);
    CHECK(o->tbl[0].restart_valid, "restart stamp set by a first seqno >= 65536");
    uint8_t a2[6];
    mac(a2, 0x0a, 2);
    add_neigh(a2, a2, 1000);
    rx(f, mk_bcast(f, a2, a2, 900000, 49, 30));
    struct bat_orig *oa = bat_orig_find(B, a2);
    CHECK(oa->bcast.reset_valid && oa->bcast.last == 900000, "BCAST reset stamp set by a restart-sized jump");
    run_to(S.now + 59000);
    CHECK(o->tbl[0].restart_valid && oa->bcast.reset_valid, "restart stamps kept at 59 s");
    run_to(S.now + 2500);
    CHECK(!o->tbl[0].restart_valid && !oa->bcast.reset_valid,
          "restart stamps (OGM tables and BCAST window) cleared once older than 60 s");
}

static uint32_t o_bwin_last(const uint8_t *orig)
{
    struct bat_orig *o = bat_orig_find(B, orig);
    return o ? o->bcast.last : 0;
}

static void test_bcast_window(void)
{
    uint8_t a[6], c[6], f[256];
    mac(a, 0x0a, 1);
    mac(c, 0x0c, 1);
    fresh();
    add_neigh(a, a, 1000);
    add_neigh(c, c, 1000);
    struct { uint32_t seq; uint8_t ttl; int deliver; int flood_ttl; enum bat_counter cnt; const char *what; } rows[] = {
        { 1000, 49, 1, 48, BAT_C_BC_DELIVER, "1000 accepted" },
        { 1000, 49, 0, -1, BAT_C_BC_DUP, "1000 again: duplicate" },
        { 990, 49, 1, 48, BAT_C_BC_DELIVER, "990 accepted (inside the window)" },
        { 900, 49, 1, 48, BAT_C_BC_DELIVER, "900 accepted (restart, window reset)" },
        { 1001, 2, 1, 1, BAT_C_BC_DELIVER, "1001 with TTL 2 accepted and re-flooded with TTL 1" },
        { 1002, 1, 0, -1, BAT_C_BC_TTL, "1002 with TTL 1 dropped" },
        { 200000, 49, 0, -1, BAT_C_BC_RESTART_BLOCKED, "200000 dropped (restart within 30 s)" },
        { 1003, 49, 1, 48, BAT_C_BC_DELIVER, "1003 accepted" },
    };
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        S.ntx = 0;
        S.ndl = 0;
        uint32_t before = C(rows[i].cnt);
        size_t len = mk_bcast(f, a, a, rows[i].seq, rows[i].ttl, 40);
        rx(f, len);
        int ok = C(rows[i].cnt) == before + 1 && (int)S.ndl == rows[i].deliver;
        if (rows[i].flood_ttl >= 0) {
            ok &= S.ntx == 1 && S.tx[0].b[16] == rows[i].flood_ttl && memcmp(S.tx[0].b, BC, 6) == 0 &&
                  memcmp(S.tx[0].b + 6, W, 6) == 0 && memcmp(S.tx[0].b + 14 + 4, f + 14 + 4, len - 18) == 0;
            ok &= S.ndl == 1 && S.dl[0].len == 40 && memcmp(S.dl[0].b, f + 28, 40) == 0;
        } else {
            ok &= S.ntx == 0;
        }
        CHECK(ok, "BCAST window (dataplane §5.2): %s", rows[i].what);
        S.now += 100;
    }
    S.now += 30000;
    rx(f, mk_bcast(f, a, a, 200000, 49, 40));
    CHECK(C(BAT_C_BC_DELIVER) == 6, "BCAST window: the jump is accepted once 30 s have passed");
    uint32_t d = C(BAT_C_BC_DELIVER), dups = C(BAT_C_BC_DUP);
    for (uint32_t s = 200001; s <= 200070; s++) {
        rx(f, mk_bcast(f, a, a, s, 49, 40));
    }
    rx(f, mk_bcast(f, a, a, 200070 - 1, 49, 40));
    rx(f, mk_bcast(f, a, a, 200070 - 63, 49, 40));
    CHECK(C(BAT_C_BC_DELIVER) == d + 70 && C(BAT_C_BC_DUP) == dups + 2,
          "BCAST window: 70 in a row, then newest-1 and newest-63 are duplicates (64-bit history)");
    uint32_t blocked = C(BAT_C_BC_RESTART_BLOCKED);
    rx(f, mk_bcast(f, a, a, 200070 - 64, 49, 40));
    CHECK(C(BAT_C_BC_DELIVER) == d + 70 && C(BAT_C_BC_RESTART_BLOCKED) == blocked + 1,
          "BCAST window: newest-64 is a restart, blocked within 30 s of the last one");
    S.now += 30001;
    rx(f, mk_bcast(f, a, a, 200070 - 64, 49, 40));
    rx(f, mk_bcast(f, a, a, 200070 - 63, 49, 40));
    CHECK(C(BAT_C_BC_DELIVER) == d + 72 && o_bwin_last(a) == 200070 - 63,
          "BCAST window: after 30 s the restart is accepted and the window reset to it");

    uint8_t z[6];
    mac(z, 0x7e, 1);
    S.ntx = 0;
    S.ndl = 0;
    snap(); rx(f, mk_bcast(f, a, z, 1, 49, 40));
    CHECK(ONLY(BAT_C_RX, BAT_C_BC_RX, BAT_C_BC_UNKNOWN) && S.ntx == 0 && S.ndl == 0,
          "BCAST from an originator never seen: bc_unknown, not re-flooded, not delivered");
    snap(); rx(f, mk_bcast(f, a, W, 1, 49, 40));
    CHECK(ONLY(BAT_C_RX, BAT_C_BC_RX, BAT_C_BC_OWN), "BCAST carrying our own originator -> bc_own");
    snap(); rx(f, mk_bcast(f, a, a, 5000, 0, 40));
    CHECK(ONLY(BAT_C_RX, BAT_C_BC_RX, BAT_C_BC_TTL), "BCAST with TTL 0 -> bc_ttl");
    S.ndl = 0;
    size_t len = mk_bcast(f, c, c, 5001, 49, 40);
    f[28 + 12] = 0x43;
    f[28 + 13] = 0x05;
    snap(); rx(f, len);
    CHECK(S.ndl == 0 && C(BAT_C_DELIVER_BAD) - SNAP[BAT_C_DELIVER_BAD] == 1 &&
          C(BAT_C_BC_FWD) - SNAP[BAT_C_BC_FWD] == 1, "BCAST carrying 0x4305 inside: re-flooded, not delivered");
    len = mk_bcast(f, c, c, 5002, 49, 10);
    S.ndl = 0;
    rx(f, len);
    CHECK(S.ndl == 0 && C(BAT_C_DELIVER_BAD) - SNAP[BAT_C_DELIVER_BAD] == 2,
          "BCAST whose client frame is under 14 bytes: not delivered");
}

static void test_bcast_flood(void)
{
    uint8_t a[6], b2[6], x[6], f[1700];
    mac(a, 0x0a, 1);
    mac(b2, 0x0b, 1);
    mac(x, 0x55, 1);
    uint8_t inner[64];
    size_t il = eth(inner, BC, WS, 40);
    fresh();
    S.ntx = 0;
    snap();
    CHECK(bat_tx_soft(B, inner, il) == 0 && ONLY(BAT_C_ST_TX, BAT_C_ST_BCAST, BAT_C_BC_SUPP) && S.ntx == 0,
          "flood: own broadcast with no neighbour is suppressed");
    add_neigh(a, a, 1000);
    S.ntx = 0;
    uint32_t seq = B->bcast_seq;
    snap();
    CHECK(bat_tx_soft(B, inner, il) == 0 && ONLY(BAT_C_ST_TX, BAT_C_ST_BCAST, BAT_C_BC_TX_OWN, BAT_C_LK_TX),
          "flood: own broadcast with one neighbour is sent once");
    const uint8_t *p = S.tx[0].b + 14;
    CHECK(S.ntx == 1 && S.tx[0].len == 28 + il && p[0] == 0x01 && p[1] == 0x0f && p[2] == 0x31 && p[3] == 0 &&
          bat_get32(p + 4) == seq + 1 && memcmp(p + 8, W, 6) == 0 && memcmp(p + 14, inner, il) == 0,
          "own BCAST header byte-exact: 01 0f 31 00 <seq BE32> <hard>, client frame after it");
    S.ntx = 0;
    rx(f, mk_bcast(f, a, a, 10, 49, 40));
    CHECK(S.ntx == 0 && C(BAT_C_BC_SUPP) == 2 && C(BAT_C_BC_DELIVER) == 1,
          "flood: only neighbour is the BCAST originator: delivered, not re-flooded");
    /* one neighbour b2 (orig b2) relaying x's broadcast; x known through b2's OGM */
    fresh();
    add_neigh(b2, b2, 1000);
    rx(f, mk_ogm(f, b2, x, 5, 50, 0xFFFFFFFFu));
    S.ntx = 0;
    rx(f, mk_bcast(f, b2, x, 10, 48, 40));
    CHECK(S.ntx == 0 && C(BAT_C_BC_SUPP) == 1 && C(BAT_C_BC_DELIVER) == 1,
          "flood: only neighbour is the one it came from: delivered, not re-flooded");
    add_neigh(a, a, 1000);
    rx(f, mk_bcast(f, b2, x, 11, 48, 40));
    CHECK(S.ntx == 1 && C(BAT_C_BC_FWD) == 1, "flood: two neighbours: re-flooded once");
    struct bat_config c;
    stub_reset();
    cfg_default(&c);
    c.bcast_copies = 3;
    eng(&c);
    add_neigh(a, a, 1000);
    add_neigh(b2, b2, 1000);
    B->next_elp = B->next_ogm = S.now + 100000;   /* only broadcasts on the link below */
    S.ntx = 0;
    uint32_t t1 = S.now;
    rx(f, mk_bcast(f, a, a, 10, 49, 40));
    CHECK(S.ntx == 1 && S.tx[0].t == t1 && C(BAT_C_BC_FWD) == 1 && bat_tick(B) <= 5,
          "flood: bcast_copies 3: the first copy goes out at once, the engine asks to run within 5 ms");
    run_to(t1 + 4);
    CHECK(S.ntx == 1, "flood: ... no second copy within 4 ms");
    run_to(t1 + 30);
    CHECK(S.ntx == 3 && S.tx[1].t == t1 + 5 && S.tx[2].t == t1 + 10 && S.tx[0].len == S.tx[2].len &&
          memcmp(S.tx[0].b, S.tx[1].b, S.tx[0].len) == 0 && memcmp(S.tx[0].b, S.tx[2].b, S.tx[0].len) == 0,
          "flood: copies 2 and 3 follow 5 ms apart, byte-identical (dataplane §5.3: same seqno and TTL)");
    S.ntx = 0;
    il = eth(inner, BC, WS, 40);
    t1 = S.now;
    bat_tx_soft(B, inner, il);
    run_to(t1 + 30);
    CHECK(S.ntx == 3 && S.tx[0].t == t1 && S.tx[1].t == t1 + 5 && S.tx[2].t == t1 + 10,
          "flood: own broadcast also sent bcast_copies times, 5 ms apart");
    /* a late tick sends one copy per tick, never two back to back */
    S.ntx = 0;
    t1 = S.now;
    bat_tx_soft(B, inner, il);
    S.now = t1 + 40;
    bat_tick(B);
    CHECK(S.ntx == 2, "flood: a tick 40 ms late sends only the next copy");
    run_to(t1 + 44);
    CHECK(S.ntx == 2, "flood: ... the last one waits 5 ms more");
    run_to(t1 + 45);
    CHECK(S.ntx == 3 && S.tx[2].t == t1 + 45, "flood: ... then goes");
    /* sizes */
    fresh();
    add_neigh(a, a, 1000);
    add_neigh(b2, b2, 1000);
    static uint8_t big[1600];
    il = eth(big, BC, WS, 1472);
    S.ntx = 0;
    CHECK(bat_tx_soft(B, big, il) == 0 && S.ntx == 1 && S.tx[0].len == 1514,
          "own BCAST of a 1486-byte client frame fits a 1500 hard MTU");
    il = eth(big, BC, WS, 1473);
    snap();
    CHECK(bat_tx_soft(B, big, il) == -1 && ONLY(BAT_C_ST_TX, BAT_C_ST_BCAST, BAT_C_BC_TOOBIG),
          "own BCAST one byte over the hard MTU: bc_toobig (never fragmented)");
    size_t len = mk_bcast(f, a, a, 77, 49, 1487);
    S.ntx = 0;
    S.ndl = 0;
    snap(); rx(f, len);
    CHECK(S.ntx == 0 && S.ndl == 1 && C(BAT_C_BC_TOOBIG) - SNAP[BAT_C_BC_TOOBIG] == 1,
          "relayed BCAST over the hard MTU: delivered, not re-flooded");
}

/* Smallest gap between consecutive group frames in S.tx[from..], -1 with fewer than two. */
static int grp_gap(unsigned from)
{
    int best = -1;
    bool have = false;
    uint32_t last = 0;
    for (unsigned k = from; k < S.ntx; k++) {
        if (!(S.tx[k].b[0] & 1)) {
            continue;
        }
        if (have && (best < 0 || (int)(S.tx[k].t - last) < best)) {
            best = (int)(S.tx[k].t - last);
        }
        last = S.tx[k].t;
        have = true;
    }
    return best;
}

static uint8_t tx_type(unsigned k)
{
    return S.tx[k].b[14];
}

static uint32_t tx_bseq(unsigned k)
{
    return bat_get32(S.tx[k].b + 14 + 4);
}

/* Copies of the broadcast with seqno @seq in S.tx. */
static unsigned bcopies(uint32_t seq)
{
    unsigned n = 0;
    for (unsigned k = 0; k < S.ntx; k++) {
        n += tx_type(k) == 0x01 && tx_bseq(k) == seq;
    }
    return n;
}

/* On air (MM6108, against batman-adv 2025.4) a group frame sent < 1 ms after the sender's previous
 * one mostly never arrived, so two broadcasts queued in one ms lost the second's every copy. With 2
 * or 3 copies (standard group frames) every copy, first or further, own or relayed, leaves at least
 * BAT_BC_COPY_MS after the engine's previous group frame; first copies go before repeats; the tick's
 * ELP and OGM aggregate wait for the same spacing; the queue is BAT_BC_COPY_SLOTS broadcasts. */
static void test_bcast_pacing(void)
{
    uint8_t a[6], b2[6], f[1700], i1[128], i2[128];
    mac(a, 0x0a, 1);
    mac(b2, 0x0b, 1);
    const uint8_t mc[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x45 };
    size_t l1 = eth(i1, BC, WS, 40), l2 = eth(i2, mc, WS, 50);
    struct bat_config c;
    stub_reset();
    cfg_default(&c);
    c.bcast_copies = 3;
    eng(&c);
    add_neigh(a, a, 1000);
    add_neigh(b2, b2, 1000);
    B->next_elp = B->next_ogm = S.now + 100000;

    S.ntx = 0;
    uint32_t t1 = S.now, seq = B->bcast_seq;
    CHECK(bat_tx_soft(B, i1, l1) == 0 && bat_tx_soft(B, i2, l2) == 0 && S.ntx == 1 && tx_bseq(0) == seq + 1 &&
          bat_tick(B) == BAT_BC_COPY_MS, "pace: two own broadcasts in one ms: only the first's first copy leaves "
          "at once, the engine asks to run in 5 ms");
    run_to(t1 + 60);
    bool order = S.ntx == 6;
    for (unsigned k = 0; order && k < 6; k++) {
        order = tx_type(k) == 0x01 && S.tx[k].t == t1 + 5 * k && tx_bseq(k) == seq + 1 + (k & 1);
    }
    CHECK(order && grp_gap(0) == 5, "pace: A1 B1 A2 B2 A3 B3, 5 ms apart: the second broadcast's first copy "
          "goes before the first's repeats (%u frames, smallest gap %d ms)", S.ntx, grp_gap(0));
    CHECK(S.tx[1].len == 28 + l2 && memcmp(S.tx[1].b, S.tx[3].b, S.tx[1].len) == 0 &&
          memcmp(S.tx[1].b, S.tx[5].b, S.tx[1].len) == 0, "pace: the second broadcast's 3 copies are byte-identical");

    /* one arriving while others wait never jumps the queue, and an ELP due goes before them all */
    S.ntx = 0;
    t1 = S.now;
    seq = B->bcast_seq;
    bat_tx_soft(B, i1, l1);
    bat_tx_soft(B, i1, l1);
    S.now = t1 + 5;
    B->next_elp = t1 + 5;
    bat_tx_soft(B, i2, l2);
    CHECK(S.ntx == 1, "pace: a third broadcast arriving when the spacing allows, with another still unsent, "
          "does not leave at once");
    bat_tick(B);                                  /* as the port does after each event */
    run_to(t1 + 60);
    order = S.ntx == 10 && tx_type(1) == 0x03 && S.tx[1].t == t1 + 5 && grp_gap(0) == 5;
    for (unsigned k = 0; order && k < 9; k++) {
        order = tx_type(k + (k > 0)) == 0x01 && tx_bseq(k + (k > 0)) == seq + 1 + k % 3;
    }
    CHECK(order, "pace: the ELP first, then B1 C1 A2 B2 C2 A3 B3 C3, 5 ms apart");
    B->next_elp = S.now + 100000;

    /* a relayed broadcast and an own one in the same ms */
    S.ntx = 0;
    t1 = S.now;
    seq = B->bcast_seq;
    rx(f, mk_bcast(f, a, a, 20, 49, 40));
    CHECK(bat_tx_soft(B, i1, l1) == 0 && S.ntx == 1 && tx_bseq(0) == 20, "pace: a relayed then an own broadcast "
          "in one ms: the relayed first copy at once, the own one waits");
    run_to(t1 + 60);
    CHECK(S.ntx == 6 && grp_gap(0) == 5 && bcopies(20) == 3 && bcopies(seq + 1) == 3 && tx_bseq(1) == seq + 1 &&
          S.tx[1].t == t1 + 5, "pace: relayed and own, 3 copies each, every frame 5 ms after the one before");

    /* the tick's ELP waits for the spacing and goes before the next copy */
    S.ntx = 0;
    t1 = S.now;
    bat_tx_soft(B, i1, l1);
    B->next_elp = t1 + 2;
    run_to(t1 + 30);
    CHECK(S.ntx == 4 && tx_type(0) == 0x01 && tx_type(1) == 0x03 && S.tx[1].t == t1 + 5 && tx_type(2) == 0x01 &&
          S.tx[2].t == t1 + 10 && S.tx[3].t == t1 + 15 && grp_gap(0) == 5,
          "pace: an ELP due 2 ms after a copy leaves 5 ms after it, the next copy 5 ms after the ELP");
    /* a broadcast waits for the spacing behind an ELP */
    B->next_elp = S.now + 1;
    run_to(S.now + 1);
    S.ntx = 0;
    t1 = S.now;
    S.now = t1 + 2;
    bat_tx_soft(B, i1, l1);
    CHECK(S.ntx == 0, "pace: a broadcast 2 ms after an ELP does not leave at once");
    run_to(t1 + 30);
    CHECK(S.ntx == 3 && S.tx[0].t == t1 + 5 && S.tx[1].t == t1 + 10 && S.tx[2].t == t1 + 15,
          "pace: ... its copies leave 5, 10 and 15 ms after the ELP");
    /* the OGM aggregate waits too */
    B->next_elp = S.now + 100000;
    S.ntx = 0;
    t1 = S.now;
    bat_tx_soft(B, i1, l1);
    B->next_ogm = t1 + 1;
    B->next_agg = t1 + 2;
    run_to(t1 + 30);
    CHECK(S.ntx == 4 && tx_type(1) == 0x04 && S.tx[1].t == t1 + 5 && S.tx[2].t == t1 + 10 && grp_gap(0) == 5,
          "pace: the OGM aggregate due 2 ms after a copy leaves 5 ms after it, the next copy 5 ms later");
    B->next_ogm = B->next_agg = S.now + 100000;

    /* the queue: BAT_BC_COPY_SLOTS broadcasts. One more takes the place of the repeats of one already
     * sent; with every slot's broadcast still unsent, a further one is dropped. */
    run_to(S.now + 50);
    S.ntx = 0;
    S.ndl = 0;
    t1 = S.now;
    seq = B->bcast_seq;
    snap();
    int ret[BAT_BC_COPY_SLOTS + 2];
    for (unsigned i = 0; i < BAT_BC_COPY_SLOTS + 2; i++) {
        ret[i] = bat_tx_soft(B, i1, l1);
    }
    bool rets = true;
    for (unsigned i = 0; i < BAT_BC_COPY_SLOTS + 1; i++) {
        rets = rets && ret[i] == 0;
    }
    CHECK(rets && ret[BAT_BC_COPY_SLOTS + 1] == -1 && S.ntx == 1 &&
          C(BAT_C_BC_COPY_DROP) - SNAP[BAT_C_BC_COPY_DROP] == 2 &&
          C(BAT_C_BC_QUEUE_FULL) - SNAP[BAT_C_BC_QUEUE_FULL] == 1 &&
          C(BAT_C_BC_TX_OWN) - SNAP[BAT_C_BC_TX_OWN] == BAT_BC_COPY_SLOTS + 1,
          "pace: %u broadcasts in one ms: the first once (its 2 repeats dropped, bc_copy_drop), %u queued, the last "
          "dropped (bc_queue_full, -1)", BAT_BC_COPY_SLOTS + 2, BAT_BC_COPY_SLOTS);
    run_to(t1 + 5 * (3 * BAT_BC_COPY_SLOTS + 4));
    bool firsts = S.ntx == 1 + 3 * BAT_BC_COPY_SLOTS && bcopies(seq + 1) == 1 &&
                  bcopies(seq + BAT_BC_COPY_SLOTS + 2) == 0;
    for (unsigned i = 0; firsts && i < BAT_BC_COPY_SLOTS; i++) {
        firsts = tx_bseq(1 + i) == seq + 2 + i && bcopies(seq + 2 + i) == 3;
    }
    CHECK(firsts && grp_gap(0) == 5, "pace: ... then each queued one's first copy, then the repeats, 3 copies each, "
          "5 ms apart (%u frames, smallest gap %d ms)", S.ntx, grp_gap(0));
    /* a relayed broadcast the full queue cannot take is still delivered here */
    S.ntx = 0;
    for (unsigned i = 0; i < BAT_BC_COPY_SLOTS + 1; i++) {
        bat_tx_soft(B, i1, l1);
    }
    snap();
    S.ndl = 0;
    rx(f, mk_bcast(f, a, a, 30, 49, 40));
    CHECK(S.ndl == 1 && C(BAT_C_BC_QUEUE_FULL) - SNAP[BAT_C_BC_QUEUE_FULL] == 1 &&
          C(BAT_C_BC_FWD) == SNAP[BAT_C_BC_FWD] && C(BAT_C_BC_DELIVER) - SNAP[BAT_C_BC_DELIVER] == 1,
          "pace: a relayed broadcast meeting a full queue: not re-flooded (bc_queue_full), delivered");
    run_to(S.now + 200);

    /* one copy (per-peer replicas, ACKed): nothing is paced */
    stub_reset();
    cfg_default(&c);
    eng(&c);
    add_neigh(a, a, 1000);
    add_neigh(b2, b2, 1000);
    B->next_ogm = S.now + 100000;
    B->next_elp = S.now + 1;
    S.ntx = 0;
    t1 = S.now;
    bat_tx_soft(B, i1, l1);
    bat_tx_soft(B, i2, l2);
    run_to(t1 + 1);
    CHECK(S.ntx == 3 && S.tx[0].t == t1 && S.tx[1].t == t1 && tx_type(2) == 0x03 && S.tx[2].t == t1 + 1,
          "pace: with one copy both broadcasts leave at once and the ELP 1 ms later (replicas are ACKed per peer)");
}

/* ---- pacing when a hand-off blocks ------------------------------------------------ */

/* The port's ops->tx first waits in mmwlan_tx_wait_until_ready while the datapath is paused (flow
 * control), then hands the frame over. Here group frame number BLK_AT (from 1) is handed over BLK_MS
 * after its ops->tx call began (S.tx[].t is the hand-off), and one event for the engine task queues
 * meanwhile: BLK_KIND 0 = a neighbour's ELP, 1 = an own broadcast from lwIP, 2 = none. */
static unsigned BLK_AT, BLK_MS, BLK_GRP, BLK_EV, BLK_KIND;
static uint32_t BLK_ELPSEQ = 3000;
static uint8_t BLK_NB[6];

static int blk_tx(void *u, const uint8_t *f, size_t len)
{
    if ((f[0] & 1) && ++BLK_GRP == BLK_AT && BLK_MS) {
        S.now += BLK_MS;
        BLK_EV += BLK_KIND < 2;
    }
    return st_tx(u, f, len);
}

/* bat_port_task: a queued event is handled at once, otherwise the engine's wait passes; then bat_tick. */
static void port_run(uint32_t end, uint32_t wait)
{
    uint8_t f[128], in[64];
    while ((int32_t)(end - S.now) > 0) {
        if (BLK_EV) {
            BLK_EV--;
            if (BLK_KIND == 0) {
                rx(f, mk_elp(f, BLK_NB, BLK_NB, BLK_ELPSEQ++));
            } else {
                bat_tx_soft(B, in, eth(in, BC, WS, 40));
            }
        } else {
            S.now += wait;
        }
        wait = bat_tick(B);
    }
}

/* 3 copies, two neighbours, timers far off; ops->tx blocks on group frame @at for @ms. */
static void blk_setup(unsigned at, unsigned ms, unsigned kind)
{
    struct bat_config c;
    uint8_t b2[6];
    stub_reset();
    cfg_default(&c);
    c.bcast_copies = 3;
    eng(&c);
    B->ops.tx = blk_tx;
    mac(BLK_NB, 0x0a, 1);
    mac(b2, 0x0b, 1);
    add_neigh(BLK_NB, BLK_NB, 1000);
    add_neigh(b2, b2, 1000);
    B->next_elp = B->next_ogm = S.now + 100000;
    S.ntx = 0;
    BLK_GRP = BLK_EV = 0;
    BLK_AT = at;
    BLK_MS = ms;
    BLK_KIND = kind;
}

/* The spacing counts from when ops->tx accepted the previous group frame, not from when the engine
 * call that sent it began: an event handled right after a blocked hand-off (the port takes a queued
 * event at once, then ticks) must still wait BAT_BC_COPY_MS. */
static void test_bcast_pacing_blocked(void)
{
    uint8_t in[64];
    size_t il = eth(in, BC, WS, 40);
    /* two own broadcasts in one ms, one of the first three hand-offs blocked, a neighbour's ELP queued */
    for (unsigned ms = 2; ms <= 8; ms += 2) {
        int worst = 99;
        bool all = true;
        for (unsigned at = 1; at <= 3; at++) {
            blk_setup(at, ms, 0);
            bat_tx_soft(B, in, il);
            bat_tick(B);
            bat_tx_soft(B, in, il);
            port_run(S.now + 80, bat_tick(B));
            int g = grp_gap(0);
            worst = g < worst ? g : worst;
            all = all && S.ntx == 6;
        }
        CHECK(all && worst >= BAT_BC_COPY_MS, "pace, blocked: a hand-off %u ms late with an ELP arriving meanwhile: "
              "every group frame still %d ms after the one before (smallest gap %d ms)", ms, BAT_BC_COPY_MS, worst);
    }
    /* an own broadcast queued while the last copy blocks */
    blk_setup(6, 6, 1);
    bat_tx_soft(B, in, il);
    bat_tick(B);
    bat_tx_soft(B, in, il);
    port_run(S.now + 120, bat_tick(B));
    CHECK(S.ntx == 9 && grp_gap(0) >= BAT_BC_COPY_MS, "pace, blocked: a broadcast queued while the last copy "
          "blocks 6 ms waits 5 ms after that hand-off (%u frames, smallest gap %d ms)", S.ntx, grp_gap(0));
    /* the tick's ELP blocks 30 ms, an ARP broadcast queues meanwhile */
    blk_setup(1, 30, 1);
    uint32_t t1 = S.now;
    B->next_elp = t1 + 1;
    port_run(t1 + 100, 1);
    CHECK(S.ntx == 4 && tx_type(0) == 0x03 && S.tx[0].t == t1 + 31 && tx_type(1) == 0x01 &&
          S.tx[1].t == S.tx[0].t + BAT_BC_COPY_MS && grp_gap(0) == BAT_BC_COPY_MS,
          "pace, blocked: an ELP handed over 30 ms late, a broadcast queued meanwhile leaves 5 ms after it "
          "(ELP at +%u, copy at +%u)", S.tx[0].t - t1, S.ntx > 1 ? S.tx[1].t - t1 : 0);
    /* an own broadcast's first copy blocks 30 ms; the port's tick right after must not send copy 2 */
    blk_setup(1, 30, 0);
    t1 = S.now;
    bat_tx_soft(B, in, il);
    port_run(t1 + 100, bat_tick(B));
    CHECK(S.ntx == 3 && S.tx[0].t == t1 + 30 && S.tx[1].t == t1 + 35 && S.tx[2].t == t1 + 40,
          "pace, blocked: a first copy handed over 30 ms late, the repeats 5 and 10 ms after it "
          "(%u frames, smallest gap %d ms)", S.ntx, grp_gap(0));
    /* the tick's ELP blocks with copies waiting and nothing queues: the engine asks to run again 5 ms after
     * the hand-off, no later */
    blk_setup(2, 30, 2);
    t1 = S.now;
    bat_tx_soft(B, in, il);
    B->next_elp = t1 + 5;
    port_run(t1 + 100, bat_tick(B));
    CHECK(S.ntx == 4 && tx_type(1) == 0x03 && S.tx[1].t == t1 + 35 && S.tx[2].t == t1 + 40 &&
          S.tx[3].t == t1 + 45, "pace, blocked: copies waiting behind an ELP handed over 30 ms late leave 5 and "
          "10 ms after it (at +%u, +%u)", S.ntx > 2 ? S.tx[2].t - t1 : 0, S.ntx > 3 ? S.tx[3].t - t1 : 0);

    /* 2^31 ms and more without an accepted group frame never holds the next one */
    struct bat_config c;
    stub_reset();
    cfg_default(&c);
    c.bcast_copies = 3;
    eng(&c);
    B->next_elp = S.now;
    B->next_ogm = S.now + 100000;
    bat_tick(B);
    S.tx_ret = BAT_TX_BUSY;
    S.now += 1000;
    bat_tick(B);
    S.now += 0x80000010u;
    S.tx_ret = BAT_TX_OK;
    B->next_elp = S.now;
    S.ntx = 0;
    bat_tick(B);
    CHECK(S.ntx == 1 && tx_type(0) == 0x03, "pace: an ELP due 2^31 + 16 ms after the last accepted group frame "
          "leaves at once (%u frames)", S.ntx);
}

static void test_soft_tx(void)
{
    uint8_t f[1700], a[6];
    mac(a, 0x0a, 1);
    fresh();
    add_neigh(a, a, 1000);
    size_t l = eth(f, BC, WS, 40);
    snap();
    CHECK(bat_tx_soft(B, f, 13) == -1 && ONLY(BAT_C_ST_TX, BAT_C_ST_SHORT), "soft TX: 13 bytes -> st_short");
    f[12] = 0x43; f[13] = 0x05;
    snap();
    CHECK(bat_tx_soft(B, f, l) == -1 && ONLY(BAT_C_ST_TX, BAT_C_ST_LOOP), "soft TX: ethertype 0x4305 -> st_loop");
    f[12] = 0x81; f[13] = 0x00; f[16] = 0x43; f[17] = 0x05;
    snap();
    CHECK(bat_tx_soft(B, f, l) == -1 && ONLY(BAT_C_ST_TX, BAT_C_ST_LOOP), "soft TX: 802.1Q + 0x4305 -> st_loop");
    f[16] = 0x08; f[17] = 0x00;
    snap();
    CHECK(bat_tx_soft(B, f, l) == -1 && ONLY(BAT_C_ST_TX, BAT_C_ST_VLAN), "soft TX: 802.1Q-tagged -> st_vlan");
    l = eth(f, BC, a, 40);
    snap();
    CHECK(bat_tx_soft(B, f, l) == -1 && ONLY(BAT_C_ST_TX, BAT_C_ST_SRC), "soft TX: source not our soft MAC -> st_src");
    const uint8_t stp[6] = { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x00 }, ectp[6] = { 0xcf, 0, 0, 0, 0, 0 };
    l = eth(f, stp, WS, 40);
    snap();
    CHECK(bat_tx_soft(B, f, l) == -1 && ONLY(BAT_C_ST_TX, BAT_C_ST_CTRL), "soft TX: STP destination -> st_ctrl");
    l = eth(f, ectp, WS, 40);
    snap();
    CHECK(bat_tx_soft(B, f, l) == -1 && ONLY(BAT_C_ST_TX, BAT_C_ST_CTRL), "soft TX: ECTP destination -> st_ctrl");
    l = eth(f, BC, WS, 1569 - 14);
    snap();
    CHECK(bat_tx_soft(B, f, l) == -1 && ONLY(BAT_C_ST_TX, BAT_C_ST_TOOBIG), "soft TX: 1569 bytes -> st_toobig");
    const uint8_t mc[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x45 };
    l = eth(f, mc, WS, 60);
    snap();
    CHECK(bat_tx_soft(B, f, l) == 0 && ONLY(BAT_C_ST_TX, BAT_C_ST_BCAST, BAT_C_BC_TX_OWN, BAT_C_LK_TX),
          "soft TX: IPv4 multicast (239.0.0.69's MAC) floods as BCAST");
}

static void test_link_tx(void)
{
    uint8_t a[6];
    mac(a, 0x0a, 1);
    fresh();
    const int rets[4] = { BAT_TX_OK, BAT_TX_NOPEER, BAT_TX_BUSY, BAT_TX_FAIL };
    const enum bat_counter cs[4] = { BAT_C_LK_TX, BAT_C_LK_NOPEER, BAT_C_LK_BUSY, BAT_C_LK_FAIL };
    for (int i = 0; i < 4; i++) {
        S.tx_ret = rets[i];
        S.ntx = 0;
        S.now = B->next_elp;
        snap();
        bat_tick(B);
        CHECK(S.ntx == 1 && C(cs[i]) - SNAP[cs[i]] == 1 && C(BAT_C_ELP_TX) - SNAP[BAT_C_ELP_TX] == 1,
              "link TX result %d counted as %s (an ELP goes out even with no neighbour)", rets[i],
              bat_counter_name(cs[i]));
    }
    const uint8_t *p = S.tx[0].b + 14;
    CHECK(S.tx[0].len == 34 && memcmp(S.tx[0].b, BC, 6) == 0 && memcmp(S.tx[0].b + 6, W, 6) == 0 &&
          S.tx[0].b[12] == 0x43 && S.tx[0].b[13] == 0x05 && p[0] == 3 && p[1] == 15 &&
          memcmp(p + 2, W, 6) == 0 && bat_get32(p + 12) == 500 && bat_get32(p + 16) == 0,
          "own ELP: 20-byte payload 03 0f <hard> <seq> 000001f4 00000000 to broadcast");
    uint32_t s1 = bat_get32(p + 8);
    S.now = B->next_elp;
    S.ntx = 0;
    bat_tick(B);
    CHECK(bat_get32(S.tx[0].b + 14 + 8) == s1 + 1, "own ELP: seqno +1 per ELP");
}

static void test_elp_run7(void)
{
    /* membership §3.5 worked ELP (run7): accepted by batman-adv 2024.3 as-is */
    const uint8_t want[34] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02, 0xbb, 0x00, 0x00, 0x00, 0x02,
                               0x43, 0x05, 0x03, 0x0f, 0x02, 0xbb, 0x00, 0x00, 0x00, 0x02, 0x58, 0xe0,
                               0x2c, 0xa2, 0x00, 0x00, 0x01, 0xf4, 0x00, 0x00, 0x00, 0x00 };
    struct bat_config c;
    stub_reset();
    S.script[0] = 0x58e02ca2u;
    S.nscript = 1;
    cfg_default(&c);
    const uint8_t h[6] = { 0x02, 0xbb, 0x00, 0x00, 0x00, 0x02 }, sm[6] = { 0x02, 0xbb, 0xba, 0x70, 0x00, 0x02 };
    memcpy(c.hard_addr, h, 6);
    memcpy(c.soft_addr, sm, 6);
    eng(&c);
    S.now = B->next_elp;
    bat_tick(B);
    CHECK(S.ntx == 1 && S.tx[0].len == sizeof(want) && memcmp(S.tx[0].b, want, sizeof(want)) == 0,
          "own ELP equals the run7 frame of membership §3.5 byte for byte");
}

static void test_capacity(void)
{
    uint8_t f[1700];
    /* neighbours */
    fresh();
    uint32_t t0 = S.now;
    uint8_t nb[9][6];
    for (int i = 0; i < 9; i++) {
        mac(nb[i], 0x40, (uint8_t)(i + 1));
    }
    for (int i = 0; i < 8; i++) {
        add_neigh(nb[i], nb[i], 1000);
    }
    uint8_t x[6];
    mac(x, 0x55, 1);
    rx(f, mk_ogm(f, nb[0], x, 10, 50, 0xFFFFFFFFu));
    S.now = t0 + 10000;
    snap(); rx(f, mk_elp(f, nb[8], nb[8], 5));
    CHECK(bat_neigh_count(B) == 8 && C(BAT_C_NEIGH_FULL) == 1 && !bat_neigh_find(B, nb[8]),
          "capacity: 9th neighbour refused while all 8 are fresh (neigh_full)");
    S.now = t0 + 35000;
    for (int i = 1; i < 8; i++) {
        rx(f, mk_elp(f, nb[i], nb[i], 2000));
    }
    rx(f, mk_elp(f, nb[8], nb[8], 6));
    struct bat_orig *o = bat_orig_find(B, x);
    CHECK(bat_neigh_find(B, nb[8]) && !bat_neigh_find(B, nb[0]) && C(BAT_C_NEIGH_PURGED) == 1,
          "capacity: the neighbour silent for > 30 s is evicted for the 9th");
    CHECK(o && o->tbl[0].router < 0 && o->tbl[1].router < 0 && C(BAT_C_ROUTE_LOST) == 2,
          "capacity: its candidates go and both routes through it (default, interface) are lost");

    /* originators */
    fresh();
    t0 = S.now;
    for (int i = 0; i < 8; i++) {
        add_neigh(nb[i], nb[i], 1000);   /* 8 originators with no route */
    }
    for (int i = 0; i < 24; i++) {
        uint8_t y[6];
        mac(y, 0x60, (uint8_t)i);
        rx(f, mk_ogm(f, nb[0], y, 10, 50, 0xFFFFFFFFu));
    }
    CHECK(orig_count() == 32, "capacity: 32 originators");
    uint8_t y33[6];
    mac(y33, 0x61, 1);
    S.now = t0 + 29000;
    snap(); rx(f, mk_ogm(f, nb[0], y33, 10, 50, 0xFFFFFFFFu));
    CHECK(C(BAT_C_ORIG_FULL) == 1 && !bat_orig_find(B, y33),
          "capacity: the 33rd refused while every route-less originator is 30 s old or younger");
    S.now = t0 + 31000;
    for (int i = 0; i < 24; i++) {
        uint8_t y[6];
        mac(y, 0x60, (uint8_t)i);
        rx(f, mk_ogm(f, nb[0], y, 11, 50, 0xFFFFFFFFu));
    }
    rx(f, mk_ogm(f, nb[0], y33, 10, 50, 0xFFFFFFFFu));
    CHECK(bat_orig_find(B, y33) && !bat_orig_find(B, nb[0]) && C(BAT_C_ORIG_PURGED) == 1,
          "capacity: the oldest route-less originator older than 30 s is evicted");
    CHECK(bat_neigh_find(B, nb[0]) != NULL, "capacity: its neighbour stays (other candidates use it)");

    /* a new direct neighbour while all 32 originators are routed (a mesh of more than 32 nodes,
     * or one peer announcing 31 more): it displaces the worst-routed originator that is no
     * neighbour's own, whose next OGM is then refused (hardening; Linux has no fixed table) */
    fresh();
    add_neigh(nb[0], nb[0], 1000);
    rx(f, mk_ogm(f, nb[0], nb[0], 10, 50, 0xFFFFFFFFu));
    for (int i = 0; i < 31; i++) {
        uint8_t y[6];
        mac(y, 0x62, (uint8_t)i);
        rx(f, mk_ogm(f, nb[0], y, 10, 50, (uint32_t)(i == 7 ? 150 : 200 + i)));
    }
    CHECK(orig_count() == 32 && bat_route_count(B) == 32, "capacity: 32 routed originators through one neighbour");
    uint8_t yw[6];
    mac(yw, 0x62, 7);
    S.now += 1000;
    snap();
    add_neigh(nb[1], nb[1], 1000);
    CHECK(bat_neigh_find(B, nb[1]) && bat_orig_find(B, nb[1]) && !bat_orig_find(B, yw) && orig_count() == 32 &&
          C(BAT_C_ORIG_PURGED) - SNAP[BAT_C_ORIG_PURGED] == 1 && C(BAT_C_ORIG_FULL) == SNAP[BAT_C_ORIG_FULL],
          "capacity: a new direct neighbour is admitted; the worst-routed other originator (150) makes room");
    rx(f, mk_ogm(f, nb[1], nb[1], 20, 50, 0xFFFFFFFFu));
    CHECK(bat_route_nh(B, bat_orig_find(B, nb[1]), BAT_TBL_DEFAULT) != NULL && bat_route_count(B) == 32,
          "capacity: ... and its own OGM routes it");
    snap();
    rx(f, mk_ogm(f, nb[0], yw, 11, 50, 150));
    CHECK(!bat_orig_find(B, yw) && C(BAT_C_ORIG_FULL) - SNAP[BAT_C_ORIG_FULL] == 1,
          "capacity: the displaced originator's next OGM is refused (orig_full): no flapping");
    /* neighbours' own originators are never displaced; nor is anything when the neighbour
     * table has no room for the newcomer anyway */
    for (int i = 2; i < 8; i++) {
        add_neigh(nb[i], nb[i], 1000);
        rx(f, mk_ogm(f, nb[i], nb[i], 30, 50, 0xFFFFFFFFu));
    }
    unsigned nown = 0;
    for (int i = 0; i < 8; i++) {
        nown += bat_orig_find(B, nb[i]) != NULL;
    }
    CHECK(bat_neigh_count(B) == 8 && nown == 8 && orig_count() == 32 && bat_route_count(B) == 32,
          "capacity: 8 neighbours, their originators kept, 24 others left, all 32 routed");
    snap();
    rx(f, mk_elp(f, nb[8], nb[8], 5));
    CHECK(!bat_neigh_find(B, nb[8]) && !bat_orig_find(B, nb[8]) && orig_count() == 32 &&
          C(BAT_C_ORIG_PURGED) == SNAP[BAT_C_ORIG_PURGED],
          "capacity: a 9th neighbour while all 8 are fresh displaces no originator");

    /* candidates */
    fresh();
    for (int i = 0; i < 5; i++) {
        add_neigh(nb[i], nb[i], 1000);
    }
    for (int i = 0; i < 4; i++) {
        S.now += 100;
        rx(f, mk_ogm(f, nb[i], x, 50, 50, (uint32_t)(100 + i)));
    }
    o = bat_orig_find(B, x);
    unsigned used = 0;
    for (int i = 0; i < BAT_CANDS_PER_ORIG; i++) {
        used += o->cand[i].used;
    }
    CHECK(used == 4 && memcmp(bat_route_nh(B, o, 0), nb[3], 6) == 0, "capacity: 4 candidates, router the best");
    uint8_t r1 = bat_neigh_find(B, nb[1])->refs;
    S.now += 100;
    rx(f, mk_ogm(f, nb[4], x, 50, 50, 50));
    CHECK(cand_via(o, nb[4]) && !cand_via(o, nb[0]) && cand_via(o, nb[1]) && cand_via(o, nb[3]),
          "capacity: a 5th candidate evicts the oldest one that routes nothing");
    CHECK(bat_neigh_find(B, nb[1])->refs == r1 && bat_neigh_find(B, nb[0])->refs == 1,
          "capacity: the evicted candidate's neighbour reference is released");
}

static int contains(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

static void test_render(void)
{
    uint8_t n1[6], n2[6], x[6], f[128];
    static char buf[BAT_RENDER_BUF];
    mac(n1, 0x11, 1);
    mac(n2, 0x22, 1);
    mac(x, 0x55, 1);
    int names_ok = 1;
    for (int i = 0; i < BAT_C__COUNT; i++) {
        const char *n = bat_counter_name((enum bat_counter)i);
        names_ok &= n && n[0] && strcmp(n, "?") != 0;
        for (int k = 0; k < i; k++) {
            names_ok &= strcmp(n, bat_counter_name((enum bat_counter)k)) != 0;
        }
    }
    CHECK(names_ok, "every counter has a unique name (%d counters)", BAT_C__COUNT);
    CHECK(strcmp(bat_counter_name(BAT_C_OGM_RESTART_BLOCKED), "ogm_restart_blocked") == 0 &&
          strcmp(bat_counter_name(BAT_C_RX), "rx") == 0 && strcmp(bat_counter_name(BAT_C_DELIVER), "deliver") == 0,
          "counter names are the lowercase ids");
    CHECK(strcmp(bat_counter_name(BAT_C__COUNT), "?") == 0 && bat_counter(B, BAT_C__COUNT) == 0,
          "out-of-range counter reads 0 and is named ?");

    uint8_t n3[6];
    mac(n3, 0x33, 1);
    fresh();
    add_neigh(n1, n1, 10);
    add_neigh(n2, n2, 11);
    add_neigh(n3, n3, 1234);
    rx(f, mk_ogm(f, n1, x, 300, 50, 0xFFFFFFFFu));
    rx(f, mk_ogm(f, n2, x, 300, 50, 0xFFFFFFFFu));
    S.now += 2500;
    size_t n = bat_render(B, BAT_RENDER_NEIGH, buf, sizeof(buf));
    CHECK(n == strlen(buf) && strncmp(buf, "+BATN: count=3/8\r\n", 18) == 0 &&
          contains(buf, "+BATN: 02:33:00:00:00:01 orig=02:33:00:00:00:01 seen=2500ms tput=123.4 interval=500 cands=1\r\n") &&
          contains(buf, "+BATN: 02:11:00:00:00:01 orig=02:11:00:00:00:01 seen=2500ms tput=1.0 interval=500 cands=2\r\n"),
          "render NEIGH: count line first, one line per neighbour");
    n = bat_render(B, BAT_RENDER_ORIG, buf, sizeof(buf));
    CHECK(strncmp(buf, "+BATO: self=02:77:00:00:00:01 soft=06:77:00:00:00:01 ogmseq=", 60) == 0 &&
          contains(buf, " routes=1 count=4/32\r\n"), "render ORIG: summary line first");
    CHECK(contains(buf, "+BATO: 02:55:00:00:00:01 seen=2500ms nh=02:22:00:00:00:01 tput=1.1 iftput=0.8 ttvn=- tt=0 gw=-\r\n"),
          "render ORIG: originator line with default and interface router values");
    CHECK(contains(buf, "+BATO:  via 02:11:00:00:00:01 tput=1.0 iftput=0.8 seq=300 -J\r\n") &&
          contains(buf, "+BATO:  via 02:22:00:00:00:01 tput=1.1 iftput=0.5 seq=300 *-\r\n"),
          "render ORIG: candidate lines mark the default (*) and interface (J) routers");
    CHECK(contains(buf, "+BATO: 02:33:00:00:00:01 seen=2500ms nh=- tput=- iftput=- ttvn=- tt=0 gw=-\r\n"),
          "render ORIG: an originator known only by ELP shows no route");
    n = bat_render(B, BAT_RENDER_STAT, buf, sizeof(buf));
    int all = contains(buf, "+BATSTAT: self=02:77:00:00:00:01 soft=06:77:00:00:00:01 neigh=3/8 orig=4/32 routes=1 tt=");
    for (int i = 0; i < BAT_C__COUNT; i++) {
        char k[64];
        snprintf(k, sizeof(k), " %s=%u", bat_counter_name((enum bat_counter)i), bat_counter(B, (enum bat_counter)i));
        all &= contains(buf, k);
    }
    CHECK(all && contains(buf, "\r\n+BATSTAT: rx rx=") && contains(buf, "\r\n+BATSTAT: lk lk_tx=") &&
          contains(buf, "\r\n+BATSTAT: ogm ogm_rx=") && n < BAT_RENDER_BUF - 32 && n == strlen(buf),
          "render STAT: summary, then every counter as name=value in its group line (%zu bytes)", n);
    int lines = 0;
    for (const char *q = buf; (q = strstr(q, "\r\n")); q += 2) {
        lines++;
    }
    CHECK(lines == 11, "render STAT: summary + 10 group lines (%d)", lines);
    char small[200];
    n = bat_render(B, BAT_RENDER_STAT, small, sizeof(small));
    CHECK(n == strlen(small) && n < sizeof(small) && contains(small, "+BATSTAT: self=") &&
          contains(small, "+BATSTAT: (truncated)\r\n") && !contains(small, "+BATSTAT: rx rx="),
          "render STAT truncated: summary kept, marker last, never past the buffer");
    {
        size_t full = bat_render(B, BAT_RENDER_STAT, buf, sizeof(buf));
        const char *e1 = strstr(buf, "\r\n"), *e2 = e1 ? strstr(e1 + 2, "\r\n") : NULL;
        size_t l1 = e1 ? (size_t)(e1 + 2 - buf) : 0, l2 = e2 ? (size_t)(e2 + 2 - buf) - l1 : 0;
        char line1[512], cut[1024];
        memcpy(line1, buf, l1);
        line1[l1] = 0;
        size_t m = bat_render(B, BAT_RENDER_STAT, cut, l1 + l2 + 10);
        char want[600];
        snprintf(want, sizeof(want), "%s+BATSTAT: (truncated)\r\n", line1);
        CHECK(full > l1 + l2 && strcmp(cut, want) == 0 && m == strlen(want),
              "render: a line that would pass len - 32 is dropped and the marker follows the last whole line");
    }
    char tiny[20];
    n = bat_render(B, BAT_RENDER_ORIG, tiny, sizeof(tiny));
    CHECK(n == strlen(tiny) && n < sizeof(tiny), "render into a 20-byte buffer stays inside it");
    CHECK(bat_render(B, BAT_RENDER_NEIGH, buf, 0) == 0, "render with len 0 writes nothing");
}

/* ---- paged render: whole tables at OpenMANET size through the 4 KiB port buffer ------ */

static char PG[65536];              /* the concatenated chunks */
static unsigned PG_BAD;             /* chunks that broke the chunk rules */

static unsigned count_lines(const char *s, const char *prefix)
{
    unsigned n = 0;
    size_t pl = strlen(prefix);
    for (const char *l = s; *l;) {
        if (strncmp(l, prefix, pl) == 0) {
            n++;
        }
        const char *e = strstr(l, "\r\n");
        if (!e) {
            break;
        }
        l = e + 2;
    }
    return n;
}

/* bat_render_from from cursor 0 until BAT_RENDER_DONE into @len-byte chunks, joined in PG.
 * Every chunk must be NUL-terminated inside @len, end on "\r\n" and (ORIG) never open with a
 * candidate line; PG_BAD counts those that do not. Returns the chunks used, 0 if more than 200. */
static unsigned pages(enum bat_render_kind k, const uint8_t *mac, size_t len)
{
    static char chunk[BAT_RENDER_BUF];
    uint32_t cur = 0;
    size_t used = 0;
    unsigned n = 0;
    PG[0] = '\0';
    PG_BAD = 0;
    while (cur != BAT_RENDER_DONE && n < 200) {
        memset(chunk, 0x5a, sizeof(chunk));
        size_t w = bat_render_from(B, k, mac, &cur, chunk, len);
        n++;
        if (w >= len || chunk[w] != '\0' || strlen(chunk) != w || (w && strcmp(chunk + w - 2, "\r\n") != 0) ||
            strncmp(chunk, "+BATO:  via", 11) == 0 || used + w >= sizeof(PG)) {
            PG_BAD++;
            continue;
        }
        memcpy(PG + used, chunk, w + 1);
        used += w;
    }
    return cur == BAT_RENDER_DONE ? n : 0;
}

/* Longest entry of a whole render: one line, or (ORIG) an originator line with its candidates. */
static size_t longest_entry(const char *s, bool with_cands)
{
    size_t best = 0, cur = 0;
    for (const char *l = s; *l;) {
        const char *e = strstr(l, "\r\n");
        size_t ll = e ? (size_t)(e + 2 - l) : strlen(l);
        cur = (with_cands && strncmp(l, "+BATO:  via", 11) == 0) ? cur + ll : ll;
        best = cur > best ? cur : best;
        l += ll;
    }
    return best;
}

/* 32 originators (4 of them the neighbours) each heard through all 4 neighbours, every OGM
 * carrying a TT diff with one client, sequence numbers and throughputs at full width. */
static void mesh32(uint8_t nb[4][6])
{
    uint8_t f[256], tv[64];
    fresh();
    for (unsigned j = 0; j < 4; j++) {
        mac(nb[j], 0xA0, (uint8_t)j);
        add_neigh(nb[j], nb[j], 4000000000u);
    }
    for (unsigned k = 0; k < BAT_MAX_ORIG; k++) {
        uint8_t o[6], cl[6] = { 0x06, 0xA0, 0x00, 0x00, 0x00, (uint8_t)k };
        mac(o, 0xA0, (uint8_t)k);
        tv[0] = 0x04;                     /* TT v1: diff, ttvn 1, one untagged VLAN, one add */
        tv[1] = 0x01;
        bat_put16(tv + 2, 4 + 8 + 12);
        tv[4] = 0x01;
        tv[5] = 1;
        bat_put16(tv + 6, 1);
        bat_put32(tv + 8, bat_crc32c_tt(0, 0, cl));
        bat_put16(tv + 12, 0);
        bat_put16(tv + 14, 0);
        memset(tv + 16, 0, 4);
        memcpy(tv + 20, cl, 6);
        bat_put16(tv + 26, 0);
        for (unsigned j = 0; j < 4; j++) {
            link_hdr(f, BC, nb[j]);
            size_t l = ogm_rec(f + 14, o, 4000000000u + k, j == k ? 50 : 49, 4000000000u, tv, 28);
            rx(f, 14 + l);
        }
    }
}

static void test_render_pages(void)
{
    static char full[65536];
    uint8_t nb[4][6];
    mesh32(nb);
    unsigned origs = orig_count(), cands = 0, known = 0;
    for (unsigned i = 0; i < BAT_MAX_ORIG; i++) {
        for (unsigned c = 0; c < BAT_CANDS_PER_ORIG; c++) {
            cands += B->orig[i].cand[c].used;
        }
        known += B->orig[i].used && B->orig[i].tt.known;
    }
    CHECK(origs == 32 && cands == 128 && known == 32 && bat_tt_rows_used(B) == 32,
          "pages setup: 32 originators x 4 candidates with TT, all through bat_rx_hard (%u, %u, %u)", origs,
          cands, known);
    size_t fl = bat_render(B, BAT_RENDER_ORIG, full, sizeof(full));
    CHECK(count_lines(full, "+BATO: 02:a0:") == 32 && count_lines(full, "+BATO:  via ") == 128 &&
          !contains(full, "(truncated)") && fl > BAT_RENDER_BUF,
          "pages: the whole ORIG listing (%zu bytes) is longer than the port's %u-byte buffer", fl,
          (unsigned)BAT_RENDER_BUF);
    char *bufsmall = malloc(BAT_RENDER_BUF);
    size_t one = bat_render(B, BAT_RENDER_ORIG, bufsmall, BAT_RENDER_BUF);
    unsigned ol = count_lines(bufsmall, "+BATO: 02:a0:"), cl = count_lines(bufsmall, "+BATO:  via ");
    CHECK(contains(bufsmall, "+BATO: (truncated)\r\n") && ol < 32 && cl == 4 * ol && one < BAT_RENDER_BUF,
          "bat_render into %u bytes: %u originators, each with all 4 candidates, then (truncated)",
          (unsigned)BAT_RENDER_BUF, ol);
    free(bufsmall);

    unsigned n = pages(BAT_RENDER_ORIG, NULL, BAT_RENDER_BUF);
    CHECK(n >= 2 && PG_BAD == 0 && count_lines(PG, "+BATO: self=") == 1 && strncmp(PG, "+BATO: self=", 12) == 0 &&
          count_lines(PG, "+BATO: 02:a0:") == 32 && count_lines(PG, "+BATO:  via ") == 128 &&
          !contains(PG, "(truncated)") && !contains(PG, "(more)"),
          "AT+BATO? paged in %u chunks of %u bytes: one summary, all 32 originators and 128 candidates", n,
          (unsigned)BAT_RENDER_BUF);
    CHECK(strcmp(PG, full) == 0, "pages: the chunks joined are the whole render byte for byte");
    int once = 1;
    for (unsigned k = 0; k < 32; k++) {
        char want[64];
        snprintf(want, sizeof(want), "+BATO: 02:a0:00:00:00:%02x seen=", k);
        once &= count_lines(PG, want) == 1;
    }
    CHECK(once, "pages: every originator exactly once");

    /* the node in the last slot, looked up directly */
    const struct bat_orig *last = NULL;
    for (unsigned i = 0; i < BAT_MAX_ORIG; i++) {
        last = B->orig[i].used ? &B->orig[i] : last;
    }
    char want[160];
    snprintf(want, sizeof(want), "+BATO: 02:a0:00:00:00:%02x seen=", last->addr[5]);
    n = pages(BAT_RENDER_ORIG, last->addr, BAT_RENDER_BUF);
    const char *ln = strstr(PG, want);
    CHECK(n == 1 && PG_BAD == 0 && strncmp(PG, "+BATO: self=", 12) == 0 && ln && strstr(ln, " ttvn=1 tt=1 ") &&
          count_lines(PG, "+BATO: ") == 6 && count_lines(PG, "+BATO:  via ") == 4,
          "AT+BATO=<mac of the last slot>: summary, that originator with ttvn=1, its 4 candidates, nothing else");
    uint8_t nobody[6];
    mac(nobody, 0xEE, 1);
    n = pages(BAT_RENDER_ORIG, nobody, BAT_RENDER_BUF);
    CHECK(n == 1 && count_lines(PG, "+BATO: ") == 1 && strncmp(PG, "+BATO: self=", 12) == 0,
          "AT+BATO=<unknown mac>: the summary line only");

    /* buffer sizes down to one entry: still the whole listing */
    size_t lu = longest_entry(full, true);
    CHECK(lu > 300 && lu < 600, "pages: longest ORIG entry (originator + 4 candidates) is %zu bytes", lu);
    int same = 1;
    const size_t lens[] = { lu + BAT_RENDER_RESERVE + 1, 700, 1024, 1500, 2048, 3000 };
    for (unsigned i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        n = pages(BAT_RENDER_ORIG, NULL, lens[i]);
        if (!n || PG_BAD || strcmp(PG, full) != 0) {
            printf("     len %zu: chunks %u bad %u\n", lens[i], n, PG_BAD);
            same = 0;
        }
    }
    CHECK(same, "pages: any buffer that holds the longest entry (%zu + %u + 1 bytes and up) gives the whole listing",
          lu, (unsigned)BAT_RENDER_RESERVE);
    n = pages(BAT_RENDER_ORIG, NULL, lu + BAT_RENDER_RESERVE);
    size_t pl = strlen(PG);
    CHECK(n > 0 && PG_BAD == 0 && pl > 20 && strcmp(PG + pl - 20, "+BATO: (truncated)\r\n") == 0 &&
          count_lines(PG, "+BATO: 02:a0:") < 32,
          "pages: an entry longer than the chunk ends the listing with (truncated) instead of looping");
    n = pages(BAT_RENDER_ORIG, NULL, 40);
    CHECK(n > 0 && n <= 2 && PG_BAD == 0, "pages: a 40-byte buffer terminates (%u chunks)", n);

    /* the other kinds page the same way */
    const enum bat_render_kind ks[] = { BAT_RENDER_NEIGH, BAT_RENDER_STAT, BAT_RENDER_TT_LOCAL };
    same = 1;
    for (unsigned i = 0; i < 3; i++) {
        size_t f1 = bat_render(B, ks[i], full, sizeof(full));
        size_t ll = longest_entry(full, false);
        n = pages(ks[i], NULL, ll + BAT_RENDER_RESERVE + 1);
        if (!n || PG_BAD || strcmp(PG, full) != 0 || (f1 > ll && n < 2)) {
            printf("     kind %d: chunks %u bad %u\n", (int)ks[i], n, PG_BAD);
            same = 0;
        }
    }
    CHECK(same, "pages: NEIGH, STAT and TT_LOCAL chunked one line at a time join into the whole render");
    uint32_t cur = BAT_RENDER_DONE;
    full[0] = 'x';
    CHECK(bat_render_from(B, BAT_RENDER_ORIG, NULL, &cur, full, 100) == 0 && full[0] == '\0' &&
          cur == BAT_RENDER_DONE, "pages: a finished cursor renders nothing");
    cur = 0;
    CHECK(bat_render_from(B, BAT_RENDER_ORIG, NULL, &cur, full, 0) == 0 && cur == BAT_RENDER_DONE,
          "pages: len 0 writes nothing and finishes");
}

int main(void)
{
    test_init();
    test_rand_order();
    test_seq_keep();
    test_timers();
    test_gate();
    test_probe_19a();
    test_elp_window();
    test_ewma();
    test_penalty();
    test_ogm_seq();
    test_elp_first();
    test_router();
    test_suppression();
    test_forward();
    test_tvlv_once();
    test_ogm_walk();
    test_own_ogm();
    test_aggregation();
    test_purge();
    test_bcast_window();
    test_bcast_flood();
    test_bcast_pacing();
    test_bcast_pacing_blocked();
    test_soft_tx();
    test_link_tx();
    test_elp_run7();
    test_capacity();
    test_render();
    test_render_pages();
    free(B);
    if (failures) {
        printf("test_bat_core: %d FAILED\n", failures);
        return 1;
    }
    printf("test_bat_core: all passed\n");
    return 0;
}
