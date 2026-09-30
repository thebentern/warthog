/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Data-plane tests for the BATMAN_V engine: UNICAST / UNICAST_4ADDR (bat_unicast.c),
 * unknown unicast-range types, UNICAST_FRAG (bat_frag.c), ICMP (bat_icmp.c) and the
 * UNICAST_TVLV relay rule (bat_utvlv.c).
 *
 * Expected behaviour is the clean-room spec's (batman-spec/dataplane.md, packets.md),
 * measured against batman-adv 2024.3:
 *  - no TTL check at the final destination; TTL < 2 dropped at relays; TTVN serial
 *    boundary (destination TTVN 1: 0x81 accepted, 0x82 and 0x00 stale);
 *  - 0x45..0x7F relayed with TTL - 1 and byte 3 untouched, dropped when addressed to us;
 *  - fragments cut from the tail in equal pieces (1452/700 -> 3 x 484, 1524/1500 ->
 *    2 x 762, priority 5 from TOS 0xA0 -> byte 3 0x0a/0x1a/0x2a), more than 16 refused,
 *    reassembled from the highest number down, forwarded unchanged when in transit;
 *  - reassembly slots shared by all originators, but another originator's slot updated
 *    within 1 s is never evicted: a newcomer is dropped (fr_full) instead;
 *  - ICMP echo reply, record route, TTL exceeded (dataplane §8, packets §13.2);
 *  - the design's local choices: relays never drop stale TTVNs (2.4.3), transit
 *    fragments with TTL < 2 dropped (2.4.4), every UNICAST_TVLV for someone else relayed
 *    (2.4.5), the own fragment seqno skips 0 (2.4.7) and seqno 0 is reassembled.
 * The simulator (bat_sim) runs scenarios S4, S6, S13 and S14.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bat_crc32c.h"
#include "bat_internal.h"
#include "bat_sim.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* ---- scripted ops stub ------------------------------------------------------- */

#define MAXTX 256
#define MAXDL 64
struct frame { uint32_t t; size_t len; uint8_t b[2200]; };
struct stub {
    uint32_t now, rng;
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
    if (S.ntx < MAXTX && len <= sizeof(S.tx[0].b)) {
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
    if (S.ndl < MAXDL && len <= sizeof(S.dl[0].b)) {
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

/* ---- addresses and builders ------------------------------------------------------- */

static const uint8_t W[6] = { 0x02, 0x77, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t WS[6] = { 0x06, 0x77, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t BC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t N1[6] = { 0x02, 0x11, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t N2[6] = { 0x02, 0x22, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t X[6] = { 0x02, 0x55, 0x00, 0x00, 0x00, 0x01 };    /* via N1 */
static const uint8_t Y[6] = { 0x02, 0x56, 0x00, 0x00, 0x00, 0x01 };    /* via N2 */
static const uint8_t Q[6] = { 0x02, 0x57, 0x00, 0x00, 0x00, 0x01 };    /* never seen */
static const uint8_t CX[6] = { 0x06, 0x55, 0x00, 0x00, 0x00, 0x01 };   /* X's client */
static const uint8_t CY[6] = { 0x06, 0x56, 0x00, 0x00, 0x00, 0x01 };   /* Y's client */
static const uint8_t CU[6] = { 0x06, 0x58, 0x00, 0x00, 0x00, 0x01 };   /* nobody's */

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

static size_t link_hdr(uint8_t *f, const uint8_t *dst, const uint8_t *src)
{
    memcpy(f, dst, 6);
    memcpy(f + 6, src, 6);
    f[12] = 0x43;
    f[13] = 0x05;
    return 14;
}

/* Client frame dst/src/type with @pl payload bytes of a counting pattern. */
static size_t eth(uint8_t *o, const uint8_t *dst, const uint8_t *src, uint16_t type, size_t pl)
{
    memcpy(o, dst, 6);
    memcpy(o + 6, src, 6);
    bat_put16(o + 12, type);
    for (size_t i = 0; i < pl; i++) {
        o[14 + i] = (uint8_t)(i * 13 + 5);
    }
    return 14 + pl;
}

/* UNICAST (type 0x40) or 4ADDR (0x42, src4/subtype) from link @ls to W. */
static size_t mk_uc(uint8_t *f, const uint8_t *ls, uint8_t type, uint8_t ttl, uint8_t ttvn,
                    const uint8_t *dest, const uint8_t *src4, uint8_t subtype, const uint8_t *inner,
                    size_t il)
{
    link_hdr(f, W, ls);
    uint8_t *p = f + 14;
    p[0] = type;
    p[1] = 0x0f;
    p[2] = ttl;
    p[3] = ttvn;
    memcpy(p + 4, dest, 6);
    size_t h = 10;
    if (type == 0x42) {
        memcpy(p + 10, src4, 6);
        p[16] = subtype;
        p[17] = 0;
        h = 18;
    }
    memcpy(p + h, inner, il);
    return 14 + h + il;
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

/* OGM from @orig via @src announcing TTVN @ttvn with one client @cl (untagged). */
static size_t mk_ogm_tt(uint8_t *f, const uint8_t *src, const uint8_t *orig, uint32_t seq, uint8_t ttvn,
                        const uint8_t *cl)
{
    link_hdr(f, BC, src);
    uint8_t *p = f + 14;
    p[0] = 0x04;
    p[1] = 0x0f;
    p[2] = 50;
    p[3] = 0;
    bat_put32(p + 4, seq);
    memcpy(p + 8, orig, 6);
    bat_put32(p + 16, 0xFFFFFFFFu);
    uint8_t *t = p + 20;
    size_t tl = 0;
    if (cl) {
        const uint8_t hdr[] = { 0x04, 0x01, 0x00, 0x18, 0x01, ttvn, 0x00, 0x01 };
        memcpy(t, hdr, 8);
        bat_put32(t + 8, bat_crc32c_tt(0, 0, cl));
        memset(t + 12, 0, 4 + 12);
        memcpy(t + 16 + 4, cl, 6);
        tl = 28;
    }
    bat_put16(p + 14, (uint16_t)tl);
    return 34 + tl;
}

/* ---- engine helpers ------------------------------------------------------------ */

static struct bat *B;
static uint8_t RXBUF[4096];
static uint32_t SEQ = 100;

static void stub_reset(void)
{
    memset(&S, 0, sizeof(S));
    S.rng = 0x1234567u;
    S.lt_default = BAT_TPUT_UNKNOWN;
    S.tx_ret = BAT_TX_OK;
    S.now = 50000;
}

static void eng(uint16_t mtu)
{
    struct bat_config c;
    if (!B) {
        B = malloc(bat_ctx_size());
    }
    bat_config_defaults(&c);
    memcpy(c.hard_addr, W, 6);
    memcpy(c.soft_addr, WS, 6);
    c.hard_mtu = mtu;
    if (bat_init(B, &c, &OPS, NULL) != 0) {
        printf("FAIL bat_init refused a valid config\n");
        failures++;
    }
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

static void neigh(const uint8_t *n, uint32_t tput)
{
    uint8_t f[64];
    set_tput(n, tput);
    rx(f, mk_elp(f, n, n, 1000));
    bat_neigh_find(B, n)->tput_acc = (uint64_t)tput << 10;
}

/* W with neighbours N1, N2; X (client CX, TTVN 1) via N1, Y (client CY, TTVN 1) via N2. */
static void setup(uint16_t mtu)
{
    uint8_t f[128];
    stub_reset();
    eng(mtu);
    neigh(N1, 1000);
    neigh(N2, 1000);
    rx(f, mk_ogm_tt(f, N1, X, SEQ++, 1, CX));
    rx(f, mk_ogm_tt(f, N2, Y, SEQ++, 1, CY));
    B->tt.ttvn = 1;   /* W's own TTVN, as after its first commit */
    S.ntx = 0;
}

static const struct frame *last_tx(void)
{
    static const struct frame none;
    return S.ntx ? &S.tx[S.ntx - 1] : &none;
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
        if (listed ? d == 0 : d != 0) {
            printf("     counter %s moved by %u\n", bat_counter_name((enum bat_counter)i), d);
            return 0;
        }
    }
    return 1;
}
#define ONLY(...) only_changed((const enum bat_counter[]){ __VA_ARGS__ }, \
                               sizeof((const enum bat_counter[]){ __VA_ARGS__ }) / sizeof(enum bat_counter))

/* ---- UNICAST / 4ADDR at the destination -------------------------------------------- */

static void test_uc_dest(void)
{
    uint8_t in[256], f[512];
    size_t il, n;
    setup(1500);
    il = eth(in, WS, CX, 0x0800, 60);
    snap();
    rx(f, n = mk_uc(f, N1, 0x40, 50, 1, W, NULL, 0, in, il));
    CHECK(S.ndl == 1 && S.dl[0].len == il && memcmp(S.dl[0].b, in, il) == 0 &&
          ONLY(BAT_C_RX, BAT_C_UC_RX, BAT_C_UC_DELIVER, BAT_C_DELIVER),
          "UNICAST for us, current TTVN: the inner frame is delivered byte for byte");
    rx(f, mk_uc(f, N1, 0x40, 1, 1, W, NULL, 0, in, il));
    CHECK(S.ndl == 2, "TTL 1 at the destination is still delivered (dataplane §4.1 3.4)");
    rx(f, mk_uc(f, N1, 0x40, 0, 0x99, W, NULL, 0, in, il));
    CHECK(S.ndl == 3, "our own client is delivered whatever the TTVN (TTL 0, TTVN 0x99)");
    il = eth(in, BC, CX, 0x0800, 60);
    rx(f, mk_uc(f, N1, 0x40, 50, 0x00, W, NULL, 0, in, il));
    CHECK(S.ndl == 4, "group inner destination (steered DHCP reply) with a stale TTVN: delivered, no TTVN check");
    /* TTVN boundary against our TTVN 1, inner destination not ours and unknown to TT */
    il = eth(in, CU, CX, 0x0800, 60);
    rx(f, mk_uc(f, N1, 0x40, 50, 0x81, W, NULL, 0, in, il));
    CHECK(S.ndl == 5, "TTVN 0x81 vs 1 (difference 128) is not older: delivered");
    snap();
    rx(f, mk_uc(f, N1, 0x40, 50, 0x82, W, NULL, 0, in, il));
    rx(f, mk_uc(f, N1, 0x40, 50, 0x00, W, NULL, 0, in, il));
    CHECK(S.ndl == 5 && C(BAT_C_UC_STALE_DROP) - SNAP[BAT_C_UC_STALE_DROP] == 2 && S.ntx == 0,
          "TTVN 0x82 and 0x00 are stale; the client is nobody's: dropped (dataplane §6.2 7)");
    /* stale for a client TT maps elsewhere: re-routed toward X with S(X) */
    il = eth(in, CX, CY, 0x0800, 60);
    rx(f, mk_uc(f, N2, 0x40, 50, 0x00, W, NULL, 0, in, il));
    const struct frame *t = last_tx();
    CHECK(S.ndl == 5 && t && memcmp(t->b, N1, 6) == 0 && t->b[14] == 0x40 && t->b[16] == 49 &&
          t->b[17] == bat_orig_find(B, X)->tt.ttvn && memcmp(t->b + 18, X, 6) == 0 &&
          memcmp(t->b + 24, in, il) == 0 && C(BAT_C_UC_REROUTE) == 1,
          "stale TTVN, client announced by X: bytes 3..9 rewritten to (S(X), X), relayed with TTL 49");
    /* a client of ours that is pending delete is not ours any more for a stale packet */
    const uint8_t K[6] = { 0x06, 0x77, 0x00, 0x00, 0x00, 0x09 };
    bat_tt_local_add(B, K, 0, 0);
    B->tt.local[B->tt.n_local - 1].state = BAT_TTL_ON;
    B->tt.n_changes = 0;
    bat_tt_local_del(B, K, 0);
    il = eth(in, K, CX, 0x0800, 60);
    snap();
    rx(f, mk_uc(f, N1, 0x40, 50, 0x00, W, NULL, 0, in, il));
    CHECK(S.ndl == 5 && ONLY(BAT_C_RX, BAT_C_UC_RX, BAT_C_UC_STALE_DROP),
          "stale packet for our pending-delete client: dropped (tt §9.2 4, first bullet excludes it)");
    rx(f, mk_uc(f, N1, 0x40, 50, 1, W, NULL, 0, in, il));
    CHECK(S.ndl == 6, "... with a current TTVN it is delivered");
    S.ndl = 5;
    /* newer TTVN is accepted */
    il = eth(in, WS, CX, 0x0800, 60);
    rx(f, mk_uc(f, N1, 0x40, 50, 7, W, NULL, 0, in, il));
    CHECK(S.ndl == 6, "newer TTVN accepted");
    /* 4ADDR subtypes */
    snap();
    rx(f, mk_uc(f, N1, 0x42, 50, 1, W, X, 1, in, il));
    CHECK(S.ndl == 7 && memcmp(S.dl[6].b, in, il) == 0 &&
          ONLY(BAT_C_RX, BAT_C_UC_RX, BAT_C_UC_DELIVER, BAT_C_DELIVER),
          "4ADDR subtype 1 (data) delivered; source's client already known: no temporary row");
    uint8_t in2[128];
    const uint8_t H[6] = { 0x06, 0x55, 0x00, 0x00, 0x00, 0x77 };
    size_t il2 = eth(in2, BC, H, 0x0800, 50);
    rx(f, mk_uc(f, N1, 0x42, 50, 1, W, X, 1, in2, il2));
    const struct bat_tt_row *r = NULL;
    for (unsigned i = 0; i < BAT_TT_ROWS; i++) {
        if (B->tt.rows[i].used && memcmp(B->tt.rows[i].mac, H, 6) == 0) {
            r = &B->tt.rows[i];
        }
    }
    CHECK(S.ndl == 8 && r && (r->flags & BAT_TTR_TEMP) && r->orig == bat_orig_index(B, bat_orig_find(B, X)),
          "4ADDR subtype 1 with a new inner source: temporary row via the 4addr source (tt §8.3)");
    rx(f, mk_uc(f, N1, 0x42, 50, 1, W, Y, 1, (const uint8_t *)in2, il2));
    CHECK(S.ndl == 9 && C(BAT_C_TT_TEMP_NEW) == 1, "... a second source does not add another row");
    snap();
    rx(f, mk_uc(f, N1, 0x42, 50, 1, W, X, 2, in, il));
    rx(f, mk_uc(f, N1, 0x42, 50, 1, W, X, 3, in, il));
    CHECK(S.ndl == 9 && C(BAT_C_UC_4A_DAT) - SNAP[BAT_C_UC_4A_DAT] == 2,
          "4ADDR DAT get/put (2/3) for us: dropped (we are not a DHT candidate)");
    rx(f, mk_uc(f, N1, 0x42, 50, 1, W, X, 4, in, il));
    rx(f, mk_uc(f, N1, 0x42, 50, 1, W, X, 9, in, il));
    rx(f, mk_uc(f, N1, 0x42, 50, 1, W, Q, 1, in, il));
    CHECK(S.ndl == 12, "4ADDR subtype 4, unknown subtype 9, and an unknown source: delivered as data");
    const uint8_t H2[6] = { 0x06, 0x55, 0x00, 0x00, 0x00, 0x78 };
    il2 = eth(in2, BC, H2, 0x0800, 50);
    uint32_t tn = C(BAT_C_TT_TEMP_NEW);
    rx(f, mk_uc(f, N1, 0x42, 50, 1, W, X, 4, in2, il2));
    rx(f, mk_uc(f, N1, 0x42, 50, 1, W, X, 9, in2, il2));
    rx(f, mk_uc(f, N1, 0x40, 50, 1, W, NULL, 0, in2, il2));
    CHECK(S.ndl == 15 && C(BAT_C_TT_TEMP_NEW) == tn,
          "no temporary row from 4ADDR subtypes other than 1, nor from plain UNICAST (tt §8.3)");
    /* malformed */
    snap();
    rx(f, n = mk_uc(f, N1, 0x40, 50, 1, W, NULL, 0, in, 13));
    CHECK(S.ndl == 15 && ONLY(BAT_C_RX, BAT_C_UC_RX, BAT_C_UC_INNER_BAD), "inner frame < 14 bytes: uc_inner_bad");
    snap();
    rx(f, 14 + 9);
    CHECK(ONLY(BAT_C_RX, BAT_C_RX_HDR), "UNICAST header cut to 9 bytes: rx_hdr");
    snap();
    rx(f, mk_uc(f, N1, 0x42, 50, 1, W, X, 1, in, 0) - 1);
    CHECK(ONLY(BAT_C_RX, BAT_C_RX_HDR), "4ADDR header cut to 17 bytes: rx_hdr");
    uint8_t loop[64];
    size_t ll = eth(loop, WS, CX, 0x4305, 30);
    snap();
    rx(f, mk_uc(f, N1, 0x40, 50, 1, W, NULL, 0, loop, ll));
    CHECK(S.ndl == 15 && ONLY(BAT_C_RX, BAT_C_UC_RX, BAT_C_DELIVER_BAD),
          "batman-in-batman inner frame: not delivered (deliver_bad), not uc_deliver");
}

/* ---- relaying ------------------------------------------------------------------------ */

static void test_relay(void)
{
    uint8_t in[256], f[2048];
    size_t il;
    setup(1500);
    il = eth(in, CX, CY, 0x0800, 100);
    snap();
    rx(f, mk_uc(f, N2, 0x40, 2, 0x00, X, NULL, 0, in, il));
    const struct frame *t = last_tx();
    CHECK(t && t->len == 14 + 10 + il && memcmp(t->b, N1, 6) == 0 && memcmp(t->b + 6, W, 6) == 0 &&
          t->b[16] == 1 && t->b[17] == 0x00 && memcmp(t->b + 18, X, 6) == 0 &&
          ONLY(BAT_C_RX, BAT_C_UC_RX, BAT_C_UC_FWD, BAT_C_LK_TX),
          "relay: TTL 2 -> 1 toward X via N1, stale TTVN 0 left alone (deviation 2.4.3)");
    snap();
    rx(f, mk_uc(f, N2, 0x40, 1, 1, X, NULL, 0, in, il));
    CHECK(ONLY(BAT_C_RX, BAT_C_UC_RX, BAT_C_UC_TTL) && S.ntx == 1, "relay: TTL 1 dropped (uc_ttl)");
    snap();
    rx(f, mk_uc(f, N2, 0x40, 50, 1, Q, NULL, 0, in, il));
    CHECK(ONLY(BAT_C_RX, BAT_C_UC_RX, BAT_C_UC_NOROUTE) && S.ntx == 1, "relay: unknown destination dropped");
    rx(f, mk_uc(f, N2, 0x42, 50, 1, X, Y, 2, in, il));
    t = last_tx();
    CHECK(S.ntx == 2 && t->b[14] == 0x42 && t->b[16] == 49 && memcmp(t->b + 24, Y, 6) == 0 &&
          t->b[30] == 2, "relay: 4ADDR DAT traffic between peers relayed untouched but for TTL");
    /* the interface table decides a relay's next hop, the default table own traffic */
    struct bat_orig *x = bat_orig_find(B, X);
    uint8_t g[128];
    rx(g, mk_ogm_tt(g, N2, X, SEQ - 2, 1, CX));   /* same seqno via N2: second candidate */
    int c1 = -1, c2 = -1;
    for (int c = 0; c < BAT_CANDS_PER_ORIG; c++) {
        if (x->cand[c].used && memcmp(B->neigh[x->cand[c].neigh].addr, N1, 6) == 0) c1 = c;
        if (x->cand[c].used && memcmp(B->neigh[x->cand[c].neigh].addr, N2, 6) == 0) c2 = c;
    }
    if (c1 >= 0 && c2 >= 0) {
        x->tbl[BAT_TBL_DEFAULT].router = (int8_t)c1;
        x->tbl[BAT_TBL_IFACE].router = (int8_t)c2;
        x->cand[c2].t[BAT_TBL_IFACE].valid = 1;
    }
    rx(f, mk_uc(f, N1, 0x40, 50, 1, X, NULL, 0, in, il));
    CHECK(c1 >= 0 && c2 >= 0 && memcmp(last_tx()->b, N2, 6) == 0, "relay follows the interface-table router (N2)");
    uint8_t sf[256];
    size_t sl = eth(sf, CX, WS, 0x0800, 40);
    bat_tx_soft(B, sf, sl);
    CHECK(memcmp(last_tx()->b, N1, 6) == 0, "own traffic follows the default-table router (N1)");
    /* a relayed packet larger than the hard MTU is fragmented by us */
    setup(600);
    il = eth(in, CX, CY, 0x0800, 200);
    uint8_t big[1600];
    size_t bl = eth(big, CX, CY, 0x0800, 1000);
    S.ntx = 0;
    rx(f, mk_uc(f, N2, 0x40, 50, 1, X, NULL, 0, big, bl));
    CHECK(S.ntx == 2 && S.tx[0].b[14] == 0x41 && memcmp(S.tx[0].b + 24, W, 6) == 0 &&
          bat_get16(S.tx[0].b + 32) == 10 + bl && S.tx[1].b[14 + 20] == 0x40 && S.tx[1].b[14 + 22] == 49,
          "relay over a 600-byte hard MTU: fragmented by us (fragment originator = W, inner TTL 49)");
    (void)il;
}

/* ---- unknown unicast-range types ------------------------------------------------------ */

static void test_unknown(void)
{
    uint8_t f[256];
    setup(1500);
    link_hdr(f, W, N2);
    uint8_t *p = f + 14;
    p[0] = 0x45;
    p[1] = 0x0f;
    p[2] = 50;
    p[3] = 0xab;
    memcpy(p + 4, X, 6);
    memset(p + 10, 0x5a, 40);
    snap();
    rx(f, 64);
    const struct frame *t = last_tx();
    CHECK(t && t->len == 64 && memcmp(t->b, N1, 6) == 0 && t->b[16] == 49 && t->b[17] == 0xab &&
          memcmp(t->b + 18, f + 18, 46) == 0 && ONLY(BAT_C_RX, BAT_C_UNK_FWD, BAT_C_LK_TX),
          "type 0x45 not for us: relayed with TTL 49, byte 3 untouched (packets §3.4)");
    p[0] = 0x7f;
    p[2] = 2;
    rx(f, 64);
    CHECK(S.ntx == 2 && last_tx()->b[16] == 1, "type 0x7F TTL 2: relayed with TTL 1");
    p[2] = 1;
    snap();
    rx(f, 64);
    CHECK(S.ntx == 2 && ONLY(BAT_C_RX, BAT_C_UC_TTL), "type 0x7F TTL 1: dropped");
    memcpy(p + 4, W, 6);
    p[2] = 50;
    snap();
    rx(f, 64);
    CHECK(S.ntx == 2 && ONLY(BAT_C_RX, BAT_C_UNK_SELF), "type 0x45 addressed to us: dropped (unk_self)");
    snap();
    rx(f, 14 + 9);
    CHECK(ONLY(BAT_C_RX, BAT_C_RX_HDR), "type 0x7F with 9 bytes: rx_hdr");
}

/* ---- fragments: sending --------------------------------------------------------------- */

static unsigned frags(unsigned from, const struct frame **out, unsigned max)
{
    unsigned n = 0;
    for (unsigned k = from; k < S.ntx && n < max; k++) {
        if (S.tx[k].b[14] == 0x41) {
            out[n++] = &S.tx[k];
        }
    }
    return n;
}

static void test_frag_tx(void)
{
    uint8_t in[2048];
    const struct frame *fr[20];
    size_t il;
    /* 1452-byte packet (10 + 14 + 1428) over MTU 700: 3 x 484 */
    setup(700);
    il = eth(in, CX, WS, 0x0800, 1428);
    in[14] = 0x45;
    in[15] = 0x00;
    uint16_t s0 = B->frag_seq;
    bat_tx_soft(B, in, il);
    unsigned n = frags(0, fr, 20);
    int ok = n == 3;
    for (unsigned k = 0; ok && k < 3; k++) {
        const uint8_t *h = fr[k]->b + 14;
        ok &= fr[k]->len == 14 + 20 + 484 && h[3] == (k << 4) && h[2] == 50 && memcmp(h + 4, X, 6) == 0 &&
              memcmp(h + 10, W, 6) == 0 && bat_get16(h + 16) == (uint16_t)(s0 + 1) &&
              bat_get16(h + 18) == 1452 && memcmp(fr[k]->b, N1, 6) == 0;
    }
    CHECK(ok, "1452 bytes over MTU 700: 3 fragments of 20 + 484, numbers 0/1/2, total 0x05ac (dataplane §7.2)");
    CHECK(ok && fr[2]->b[34] == 0x40 && fr[2]->b[37] == 1 && memcmp(fr[2]->b + 38, X, 6) == 0 &&
          memcmp(fr[0]->b + 34, in + il - 484, 484) == 0 && memcmp(fr[1]->b + 34, in + il - 968, 484) == 0,
          "cut from the tail: fragment 0 = last 484 bytes, the highest one starts with the UNICAST header");
    CHECK(C(BAT_C_FR_TX) == 3, "fr_tx counts fragments");
    in[15] = 0xa0;
    S.ntx = 0;
    bat_tx_soft(B, in, il);
    n = frags(0, fr, 20);
    CHECK(n == 3 && fr[0]->b[17] == 0x0a && fr[1]->b[17] == 0x1a && fr[2]->b[17] == 0x2a,
          "IPv4 TOS 0xA0 -> priority 5: byte 3 = 0x0a / 0x1a / 0x2a (dataplane §7.2 example)");
    /* 1524 (1500-byte IP) over MTU 1500: 2 x 762 */
    setup(1500);
    il = eth(in, CX, WS, 0x0800, 1500);
    bat_tx_soft(B, in, il);
    n = frags(0, fr, 20);
    CHECK(n == 2 && fr[0]->len == 14 + 20 + 762 && fr[1]->len == 14 + 20 + 762 &&
          bat_get16(fr[0]->b + 32) == 1524, "1524 bytes over MTU 1500: 2 x 762 (Linux default case)");
    il = eth(in, CX, WS, 0x0800, 1476);
    S.ntx = 0;
    bat_tx_soft(B, in, il);
    CHECK(S.ntx == 1 && S.tx[0].len == 14 + 1500 && S.tx[0].b[14] == 0x40, "exactly 1500 batman bytes: not fragmented");
    /* priorities: real PCP; IPv6 traffic class */
    uint8_t pk[64] = { 0x40, 0x0f, 0x32, 0x00 };
    uint8_t *e = pk + 10;
    bat_put16(e + 12, 0x8100);
    e[14] = 0x60;
    e[15] = 0x05;
    CHECK(bat_frag_prio(pk, 40) == 3, "priority: 802.1Q PCP 3 read from the real tag");
    bat_put16(e + 12, 0x86dd);
    e[14] = 0x6b;
    e[15] = 0x80;
    CHECK(bat_frag_prio(pk, 40) == 5, "priority: IPv6 traffic class 0xb8 -> 5");
    bat_put16(e + 12, 0x0806);
    CHECK(bat_frag_prio(pk, 40) == 0, "priority: ARP -> 0");
    pk[0] = 0x44;
    CHECK(bat_frag_prio(pk, 40) == 0, "priority: non-data packet -> 0");
    /* more than 16 fragments refused */
    setup(100);
    il = eth(in, CX, WS, 0x0800, 1380);
    snap();
    CHECK(bat_tx_soft(B, in, il) == -1 && S.ntx == 0 &&
          ONLY(BAT_C_ST_TX, BAT_C_ST_UNICAST, BAT_C_FR_TX_TOOMANY),
          "MTU 100: 1404 bytes would need 18 fragments of 80: refused (fr_tx_toomany), nothing sent");
    il = eth(in, CX, WS, 0x0800, 1256);
    S.ntx = 0;
    bat_tx_soft(B, in, il);
    CHECK(frags(0, fr, 20) == 16 && S.ntx == 16, "MTU 100: 1280 bytes = 16 fragments of 80: sent");
    /* a packet larger than any soft frame: pieces stay within 1280 bytes (fragbuf) */
    setup(1500);
    static uint8_t huge[14 + 2600];
    memset(huge, 0x3c, sizeof(huge));
    huge[14] = 0x44;
    S.ntx = 0;
    CHECK(bat_frag_tx(B, N1, X, huge, sizeof(huge)) == BAT_TX_OK && S.ntx == 3 && S.tx[0].len == 14 + 20 + 867 &&
          S.tx[2].len == 14 + 20 + 866, "2600 bytes over MTU 1500: 3 pieces of <= 1260 (the 1280-byte cap), not 2 of 1300");
    /* own fragment seqno skips 0 across the 16-bit wrap */
    setup(700);
    B->frag_seq = 0xfffe;
    il = eth(in, CX, WS, 0x0800, 1000);
    bat_tx_soft(B, in, il);
    bat_tx_soft(B, in, il);
    n = frags(0, fr, 20);
    CHECK(n == 4 && bat_get16(fr[0]->b + 30) == 0xffff && bat_get16(fr[2]->b + 30) == 0x0001,
          "fragment seqno 0xffff is followed by 0x0001, never 0 (dataplane §7.5 SHOULD)");
}

/* ---- fragments: receiving --------------------------------------------------------------- */

/* Fragment @num of packet @pkt (length L, piece size s) from originator @fo to @dest. */
static size_t mk_frag(uint8_t *f, const uint8_t *ls, const uint8_t *dest, const uint8_t *fo, uint16_t seq,
                      unsigned num, uint8_t ttl, const uint8_t *pkt, size_t L, size_t s, uint16_t total)
{
    link_hdr(f, W, ls);
    uint8_t *h = f + 14;
    h[0] = 0x41;
    h[1] = 0x0f;
    h[2] = ttl;
    h[3] = (uint8_t)(num << 4);
    memcpy(h + 4, dest, 6);
    memcpy(h + 10, fo, 6);
    bat_put16(h + 16, seq);
    bat_put16(h + 18, total);
    size_t hi = L - num * s, lo = L > (num + 1) * s ? L - (num + 1) * s : 0;
    memcpy(h + 20, pkt + lo, hi - lo);
    return 34 + hi - lo;
}

static size_t mk_pkt(uint8_t *pkt, uint8_t ttvn, size_t pl)
{
    uint8_t in[2048];
    size_t il = eth(in, WS, CX, 0x0800, pl);
    pkt[0] = 0x40;
    pkt[1] = 0x0f;
    pkt[2] = 50;
    pkt[3] = ttvn;
    memcpy(pkt + 4, W, 6);
    memcpy(pkt + 10, in, il);
    return 10 + il;
}

static void test_frag_rx(void)
{
    uint8_t pkt[2100], f[1600];
    size_t L;
    setup(1500);
    L = mk_pkt(pkt, 1, 1500);
    rx(f, mk_frag(f, N1, W, X, 7, 0, 50, pkt, L, 762, (uint16_t)L));
    CHECK(S.ndl == 0 && C(BAT_C_FR_RX) == 1, "first of two fragments: held");
    rx(f, mk_frag(f, N1, W, X, 7, 1, 50, pkt, L, 762, (uint16_t)L));
    CHECK(S.ndl == 1 && S.dl[0].len == L - 10 && memcmp(S.dl[0].b, pkt + 10, L - 10) == 0 &&
          C(BAT_C_FR_DONE) == 1 && C(BAT_C_UC_DELIVER) == 1,
          "second fragment completes it: reassembled 1524-byte UNICAST delivered once");
    rx(f, mk_frag(f, N1, W, X, 8, 1, 50, pkt, L, 762, (uint16_t)L));
    rx(f, mk_frag(f, N1, W, X, 8, 0, 50, pkt, L, 762, (uint16_t)L));
    CHECK(S.ndl == 2 && memcmp(S.dl[1].b, pkt + 10, L - 10) == 0, "fragments arriving in reverse order");
    for (unsigned k = 0; k < 3; k++) {
        rx(f, mk_frag(f, N1, W, X, 0, k, 50, pkt, L, 508, (uint16_t)L));
    }
    CHECK(S.ndl == 3 && C(BAT_C_FR_DONE) == 3, "3 fragments with seqno 0: reassembled (no seqno-0 quirk)");
    /* duplicates, mismatches, overflow */
    snap();
    rx(f, mk_frag(f, N1, W, X, 9, 0, 50, pkt, L, 762, (uint16_t)L));
    rx(f, mk_frag(f, N1, W, X, 9, 0, 50, pkt, L, 762, (uint16_t)L));
    CHECK(ONLY(BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_DUP) && C(BAT_C_FR_DUP) - SNAP[BAT_C_FR_DUP] == 1,
          "the same fragment number twice: fr_dup");
    rx(f, mk_frag(f, N1, W, X, 9, 1, 50, pkt, L, 762, (uint16_t)L + 1));
    unsigned held = 0;
    for (unsigned i = 0; i < BAT_FRAG_SLOTS; i++) {
        held += B->data.slot[i].used;
    }
    CHECK(C(BAT_C_FR_BAD) == 1 && held == 0, "a total size that disagrees with the slot: slot discarded (fr_bad)");
    rx(f, mk_frag(f, N1, W, X, 9, 1, 50, pkt, L, 762, (uint16_t)L));
    CHECK(S.ndl == 3, "... so the rest of that packet never completes");
    rx(f, mk_frag(f, N1, W, X, 10, 0, 50, pkt, L, 762, 800));
    rx(f, mk_frag(f, N1, W, X, 10, 1, 50, pkt, L, 762, 800));
    CHECK(C(BAT_C_FR_BAD) == 2 && S.ndl == 3, "data beyond the announced total: slot discarded");
    snap();
    rx(f, mk_frag(f, N1, W, X, 11, 0, 50, pkt, L, 762, 0));
    rx(f, mk_frag(f, N1, W, X, 12, 0, 50, pkt, L, 762, 2049));
    CHECK(C(BAT_C_FR_BAD) - SNAP[BAT_C_FR_BAD] == 1 && C(BAT_C_FR_TOOBIG) - SNAP[BAT_C_FR_TOOBIG] == 1,
          "total 0: fr_bad; total 2049 (> 2048 buffer): fr_toobig");
    uint8_t z[64];
    size_t zl = mk_frag(z, N1, W, X, 13, 0, 50, pkt, 0, 1, 100);
    snap();
    rx(z, zl);
    CHECK(zl == 34 && ONLY(BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_BAD), "zero-length fragment payload: fr_bad");
    snap();
    rx(f, mk_frag(f, N1, W, Q, 14, 0, 50, pkt, L, 762, (uint16_t)L));
    CHECK(ONLY(BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_UNKNOWN), "fragment from an unknown originator: fr_unknown");
    snap();
    rx(f, 14 + 19);
    CHECK(ONLY(BAT_C_RX, BAT_C_RX_HDR), "fragment header cut to 19 bytes: rx_hdr");
    /* slots: a newcomer takes a free slot, else the least recently updated slot of its own
     * originator, else another originator's slot idle for more than 1 s, else it is dropped
     * (dataplane §7.5 allows a cap; the reference keeps slots per source originator) */
    setup(1500);
    for (uint16_t q = 1; q <= BAT_FRAG_SLOTS; q++) {
        rx(f, mk_frag(f, N1, W, X, q, 0, 50, pkt, L, 762, (uint16_t)L));
        S.now += 10;
    }
    snap();
    rx(f, mk_frag(f, N2, W, Y, 50, 0, 50, pkt, L, 762, (uint16_t)L));
    CHECK(ONLY(BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_FULL),
          "every slot busy with X's reassemblies updated within 1 s: Y's new one is dropped (fr_full), none evicted");
    snap();
    rx(f, mk_frag(f, N1, W, X, 20, 0, 50, pkt, L, 762, (uint16_t)L));
    CHECK(ONLY(BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_EVICT), "a new reassembly of X evicts X's least recently updated slot");
    rx(f, mk_frag(f, N1, W, X, 2, 1, 50, pkt, L, 762, (uint16_t)L));
    CHECK(S.ndl == 1, "... the others still complete");
    rx(f, mk_frag(f, N1, W, X, 1, 1, 50, pkt, L, 762, (uint16_t)L));
    CHECK(S.ndl == 1, "... the evicted packet (seq 1) no longer completes");
    S.now += 1001;
    snap();
    rx(f, mk_frag(f, N2, W, Y, 51, 0, 50, pkt, L, 762, (uint16_t)L));
    CHECK(ONLY(BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_EVICT), "after 1 s idle another originator's slot may be taken");
    /* first fragments claiming another originator between the halves of a live packet */
    setup(1500);
    rx(f, mk_frag(f, N1, W, X, 60, 0, 50, pkt, L, 762, (uint16_t)L));
    uint8_t junk[8] = { 0 };
    for (uint16_t q = 0; q < 2 * BAT_FRAG_SLOTS; q++) {
        rx(f, mk_frag(f, N2, W, Y, (uint16_t)(9000 + q), 0, 50, junk, 8, 8, 2000));
    }
    rx(f, mk_frag(f, N1, W, X, 60, 1, 50, pkt, L, 762, (uint16_t)L));
    CHECK(S.ndl == 1 && memcmp(S.dl[0].b, pkt + 10, L - 10) == 0,
          "%u bogus first fragments claiming Y between the halves of X's packet: X's packet still completes",
          2 * BAT_FRAG_SLOTS);
    /* timeout, originator gone */
    setup(1500);
    for (uint16_t q = 1; q <= BAT_FRAG_SLOTS; q++) {
        rx(f, mk_frag(f, q == 1 ? N2 : N1, W, q == 1 ? Y : X, q, 0, 50, pkt, L, 762, (uint16_t)L));
    }
    uint32_t t0 = S.now;
    B->now = t0 + 10000;
    bat_frag_purge(B);
    CHECK(C(BAT_C_FR_TIMEOUT) == 0, "slot idle exactly 10 s: kept");
    B->now = t0 + 10001;
    bat_frag_purge(B);
    unsigned used = 0;
    for (unsigned i = 0; i < BAT_FRAG_SLOTS; i++) {
        used += B->data.slot[i].used;
    }
    CHECK(C(BAT_C_FR_TIMEOUT) == BAT_FRAG_SLOTS && used == 0, "slots idle more than 10 s are discarded (fr_timeout)");
    rx(f, mk_frag(f, N2, W, Y, 30, 0, 50, pkt, L, 762, (uint16_t)L));
    bat_frag_orig_gone(B, bat_orig_index(B, bat_orig_find(B, Y)));
    used = 0;
    for (unsigned i = 0; i < BAT_FRAG_SLOTS; i++) {
        used += B->data.slot[i].used;
    }
    CHECK(used == 0, "originator gone: its reassembly slots are freed");
    /* what a reassembled packet may be */
    uint8_t nest[64] = { 0x41, 0x0f, 0x32, 0x00 };
    memcpy(nest + 4, W, 6);
    memcpy(nest + 10, X, 6);
    snap();
    rx(f, mk_frag(f, N1, W, X, 40, 0, 50, nest, 40, 40, 40));
    CHECK(ONLY(BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_DONE, BAT_C_FR_NESTED), "reassembled fragment: fr_nested");
    uint8_t bc[64] = { 0x01, 0x0f, 0x31, 0x00, 0, 0, 0, 9 };
    memcpy(bc + 8, X, 6);
    snap();
    unsigned nd0 = S.ndl, nt0 = S.ntx;
    rx(f, mk_frag(f, N1, W, X, 41, 0, 50, bc, 60, 60, 60));
    CHECK(ONLY(BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_DONE, BAT_C_RX_TYPE) && S.ndl == nd0 && S.ntx == nt0,
          "reassembled BCAST: rx_type, neither delivered nor flooded");
    uint8_t un[64] = { 0x45, 0x0f, 0x32, 0x00 };
    memcpy(un + 4, W, 6);
    snap();
    rx(f, mk_frag(f, N1, W, X, 42, 0, 50, un, 50, 50, 50));
    CHECK(ONLY(BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_DONE, BAT_C_UNK_SELF), "reassembled 0x45 for us: unk_self");
    memcpy(un + 4, Y, 6);
    S.ntx = 0;
    rx(f, mk_frag(f, N1, W, X, 43, 0, 50, un, 50, 50, 50));
    CHECK(S.ntx == 1 && memcmp(S.tx[0].b, N2, 6) == 0 && S.tx[0].b[14] == 0x45 && S.tx[0].b[16] == 49,
          "reassembled 0x45 for Y: relayed whole with TTL 49");
    uint8_t ver[64] = { 0x40, 0x0e, 0x32, 0x00 };
    snap();
    rx(f, mk_frag(f, N1, W, X, 44, 0, 50, ver, 40, 40, 40));
    CHECK(ONLY(BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_DONE, BAT_C_RX_VERSION), "reassembled version 14: rx_version");
    /* transit fragments */
    setup(1500);
    L = mk_pkt(pkt, 1, 1500);
    memcpy(pkt + 4, Y, 6);
    size_t fl = mk_frag(f, N1, Y, X, 50, 0, 50, pkt, L, 762, (uint16_t)L);
    snap();
    rx(f, fl);
    const struct frame *t = last_tx();
    CHECK(t && t->len == fl && memcmp(t->b, N2, 6) == 0 && t->b[16] == 49 && memcmp(t->b + 17, f + 17, fl - 17) == 0 &&
          ONLY(BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_FWD, BAT_C_LK_TX),
          "transit fragment: forwarded unchanged but TTL 50 -> 49, toward Y via N2 (fr_fwd)");
    f[16] = 1;
    snap();
    rx(f, fl);
    CHECK(ONLY(BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_TTL), "transit fragment with TTL 1: dropped (deviation 2.4.4)");
    f[16] = 50;
    memcpy(f + 18, Q, 6);
    snap();
    rx(f, fl);
    CHECK(ONLY(BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_NOROUTE), "transit fragment toward an unknown destination: fr_noroute");
    setup(600);
    memcpy(f + 18, Y, 6);
    snap();
    rx(f, fl);
    CHECK(ONLY(BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_BAD), "transit fragment larger than our hard MTU (600): fr_bad");
    snap();
    rx(f, mk_frag(f, N1, Y, Q, 51, 0, 50, pkt, L, 762, (uint16_t)L));
    CHECK(ONLY(BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_UNKNOWN), "transit fragment from an unknown fragment originator: fr_unknown");
}

/* ---- ICMP ---------------------------------------------------------------------------- */

static size_t mk_icmp(uint8_t *f, const uint8_t *ls, uint8_t ttl, uint8_t mt, const uint8_t *dst,
                      const uint8_t *src, uint8_t uid, uint16_t seq, size_t len, uint8_t rrc)
{
    link_hdr(f, W, ls);
    uint8_t *p = f + 14;
    memset(p, 0, len);
    p[0] = 0x43;
    p[1] = 0x0f;
    p[2] = ttl;
    p[3] = mt;
    memcpy(p + 4, dst, 6);
    memcpy(p + 10, src, 6);
    p[16] = uid;
    p[17] = rrc;
    bat_put16(p + 18, seq);
    for (unsigned k = 0; k < rrc && 20 + 6 * k + 6 <= len; k++) {
        memcpy(p + 20 + 6 * k, (const uint8_t[6]){ 0x02, 0xee, 0, 0, 0, (uint8_t)k }, 6);
    }
    return 14 + len;
}

static void test_icmp(void)
{
    uint8_t f[2048];
    setup(1500);
    snap();
    rx(f, mk_icmp(f, N1, 49, 8, W, X, 0x61, 1, 20, 0));
    const struct frame *t = last_tx();
    CHECK(t && t->len == 34 && memcmp(t->b, N1, 6) == 0 && t->b[14] == 0x43 && t->b[15] == 0x0f &&
          t->b[16] == 50 && t->b[17] == 0 && memcmp(t->b + 18, X, 6) == 0 && memcmp(t->b + 24, W, 6) == 0 &&
          t->b[30] == 0x61 && t->b[31] == 0 && bat_get16(t->b + 32) == 1 &&
          ONLY(BAT_C_RX, BAT_C_IC_RX, BAT_C_IC_REPLY, BAT_C_LK_TX),
          "echo request for us: reply type 0, TTL 50, dst = requester, src = us, uid/seq kept");
    rx(f, mk_icmp(f, N1, 1, 8, W, X, 0x62, 2, 20, 0));
    CHECK(S.ntx == 2 && last_tx()->b[16] == 50, "echo request arriving with TTL 1 at the destination: answered");
    snap();
    rx(f, mk_icmp(f, N1, 49, 8, W, Q, 0x63, 3, 20, 0));
    CHECK(ONLY(BAT_C_RX, BAT_C_IC_RX, BAT_C_IC_UNKNOWN_SRC) && S.ntx == 2,
          "echo request from an unknown originator: dropped");
    /* record route */
    rx(f, mk_icmp(f, N1, 49, 8, W, X, 0x64, 4, 116, 2));
    t = last_tx();
    CHECK(t && t->len == 14 + 116 && t->b[31] == 3 && memcmp(t->b + 34 + 12, W, 6) == 0 &&
          memcmp(t->b + 34, (const uint8_t[6]){ 0x02, 0xee, 0, 0, 0, 0 }, 6) == 0,
          "record route: our hard address (the link destination) written into slot 2, count 3");
    snap();
    rx(f, mk_icmp(f, N1, 49, 8, W, X, 0x65, 5, 116, 16));
    CHECK(ONLY(BAT_C_RX, BAT_C_IC_RX, BAT_C_IC_RR_FULL), "record route with 16 slots used: dropped");
    rx(f, mk_icmp(f, N1, 49, 0, Y, X, 0x66, 6, 116, 15));
    t = last_tx();
    CHECK(t && t->b[31] == 16 && memcmp(t->b + 34 + 90, W, 6) == 0 && memcmp(t->b, N2, 6) == 0 && t->b[16] == 48,
          "echo reply in transit: last slot filled (count 16), relayed with TTL - 1");
    uint8_t rrk[256];
    size_t rl = mk_icmp(rrk, N1, 49, 11, Y, X, 0x67, 7, 116, 3);
    rx(rrk, rl);
    t = last_tx();
    CHECK(t && t->b[16] == 48 && t->b[31] == 3 && t->b[17] == 11,
          "116-byte TTL-exceeded in transit: relayed without a record-route entry (echo only)");
    /* not for us */
    snap();
    rx(f, mk_icmp(f, N1, 1, 8, Y, X, 0x2e, 1, 20, 0));
    t = last_tx();
    CHECK(t && t->len == 34 && memcmp(t->b, N1, 6) == 0 && t->b[16] == 50 && t->b[17] == 11 &&
          memcmp(t->b + 18, X, 6) == 0 && memcmp(t->b + 24, W, 6) == 0 && t->b[30] == 0x2e &&
          ONLY(BAT_C_RX, BAT_C_IC_RX, BAT_C_IC_TTLX, BAT_C_LK_TX),
          "TTL 1 echo request for Y: TTL exceeded (type 11, TTL 50) back to X (dataplane §8.1 4)");
    snap();
    rx(f, mk_icmp(f, N1, 1, 0, Y, X, 0x2e, 1, 20, 0));
    rx(f, mk_icmp(f, N1, 1, 8, Y, Q, 0x2e, 1, 20, 0));
    CHECK(ONLY(BAT_C_RX, BAT_C_IC_RX, BAT_C_UC_TTL, BAT_C_IC_UNKNOWN_SRC),
          "TTL 1 echo reply: dropped; TTL 1 request from an unknown source: dropped");
    snap();
    rx(f, mk_icmp(f, N1, 9, 8, Y, X, 0x2f, 1, 20, 0));
    t = last_tx();
    CHECK(t && memcmp(t->b, N2, 6) == 0 && t->b[16] == 8 && t->b[17] == 8 &&
          ONLY(BAT_C_RX, BAT_C_IC_RX, BAT_C_IC_FWD, BAT_C_LK_TX), "echo request for Y: relayed with TTL 8");
    snap();
    rx(f, mk_icmp(f, N1, 9, 8, Q, X, 0x2f, 1, 20, 0));
    CHECK(ONLY(BAT_C_RX, BAT_C_IC_RX, BAT_C_UC_NOROUTE), "ICMP toward an unknown originator: dropped");
    snap();
    rx(f, mk_icmp(f, N1, 50, 15, W, X, 1, 1, 28, 0));
    rx(f, mk_icmp(f, N1, 50, 3, W, X, 1, 1, 20, 0));
    rx(f, mk_icmp(f, N1, 50, 0, W, X, 1, 1, 20, 0));
    CHECK(ONLY(BAT_C_RX, BAT_C_IC_RX, BAT_C_IC_TP, BAT_C_IC_OTHER) && C(BAT_C_IC_OTHER) - SNAP[BAT_C_IC_OTHER] == 2,
          "throughput meter for us: ic_tp; unreachable and echo reply for us: ic_other");
    snap();
    rx(f, 14 + 19);
    CHECK(ONLY(BAT_C_RX, BAT_C_RX_HDR), "ICMP cut to 19 bytes: rx_hdr");
    /* a large echo request is answered in fragments */
    setup(600);
    S.ntx = 0;
    rx(f, mk_icmp(f, N1, 49, 8, W, X, 0x70, 9, 1000, 0));
    CHECK(S.ntx == 2 && S.tx[0].b[14] == 0x41 && bat_get16(S.tx[0].b + 32) == 1000 && S.tx[1].b[34] == 0x43,
          "1000-byte echo request over MTU 600: the reply leaves in 2 fragments");
    /* the ping hook */
    setup(1500);
    CHECK(bat_icmp_send_echo(B, X, 7, 0x1234, 20) == 0 && last_tx()->len == 34 &&
          memcmp(last_tx()->b + 14, "\x43\x0f\x32\x08", 4) == 0 && memcmp(last_tx()->b + 18, X, 6) == 0 &&
          memcmp(last_tx()->b + 24, W, 6) == 0 && last_tx()->b[30] == 7 && last_tx()->b[31] == 0 &&
          bat_get16(last_tx()->b + 32) == 0x1234,
          "hook: 20-byte echo request 43 0f 32 08 | X | W | uid | 0 | seq");
    CHECK(bat_icmp_send_echo(B, X, 7, 1, 116) == 0 && last_tx()->b[31] == 1 && memcmp(last_tx()->b + 34, W, 6) == 0,
          "hook: 116-byte request pre-filled like batctl (count 1, slot 0 = us)");
    CHECK(bat_icmp_send_echo(B, Q, 7, 1, 20) == -1 && bat_icmp_send_echo(B, X, 7, 1, 19) == -1,
          "hook: unknown destination or a short length refused");
}

/* ---- UNICAST_TVLV relay and consumption --------------------------------------------- */

static void test_utvlv(void)
{
    uint8_t f[256];
    setup(1500);
    link_hdr(f, W, N1);
    uint8_t *p = f + 14;
    memset(p, 0, 40);
    p[0] = 0x44;
    p[1] = 0x0f;
    p[2] = 50;
    memcpy(p + 4, Y, 6);
    memcpy(p + 10, X, 6);
    const uint8_t unk[] = { 0x09, 0x01, 0x00, 0x04, 1, 2, 3, 4 };
    memcpy(p + 20, unk, sizeof(unk));
    bat_put16(p + 16, sizeof(unk));
    snap();
    rx(f, 14 + 20 + sizeof(unk));
    const struct frame *t = last_tx();
    CHECK(t && memcmp(t->b, N2, 6) == 0 && t->b[16] == 49 && memcmp(t->b + 17, f + 17, 20 + sizeof(unk) - 3) == 0 &&
          ONLY(BAT_C_RX, BAT_C_UT_RX, BAT_C_UT_FWD, BAT_C_LK_TX),
          "UNICAST_TVLV for Y with an unknown container: relayed uninspected (deviation 2.4.5)");
    bat_put16(p + 16, 0);
    rx(f, 14 + 20);
    CHECK(S.ntx == 2, "... and with an empty TVLV area");
    bat_put16(p + 16, sizeof(unk) + 1);
    snap();
    rx(f, 14 + 20 + sizeof(unk));
    CHECK(ONLY(BAT_C_RX, BAT_C_UT_RX, BAT_C_UT_LEN) && S.ntx == 2, "TVLV length beyond the packet: ut_len drop");
    /* for us: ROAM counted, unknown ignored, bad TT kinds counted */
    memcpy(p + 4, W, 6);
    const uint8_t mix[] = { 0x09, 0x01, 0x00, 0x00,
                            0x05, 0x01, 0x00, 0x08, 0x06, 0x55, 0, 0, 0, 9, 0, 0,
                            0x04, 0x01, 0x00, 0x04, 0x01, 0x03, 0x00, 0x00,
                            0x04, 0x01, 0x00, 0x02, 0x02, 0x03 };
    memcpy(p + 20, mix, sizeof(mix));
    bat_put16(p + 16, sizeof(mix));
    snap();
    rx(f, 14 + 20 + sizeof(mix));
    CHECK(ONLY(BAT_C_RX, BAT_C_UT_RX, BAT_C_UT_CONSUMED, BAT_C_TT_ROAM_RX, BAT_C_TT_BAD) &&
          C(BAT_C_TT_BAD) - SNAP[BAT_C_TT_BAD] == 2 && S.ntx == 2,
          "for us: unknown skipped, ROAM counted and ignored, TT kind 0x01 and a 2-byte TT container are tt_bad");
    snap();
    rx(f, 14 + 19);
    CHECK(ONLY(BAT_C_RX, BAT_C_RX_HDR), "UNICAST_TVLV cut to 19 bytes: rx_hdr");
    setup(1500);
    link_hdr(f, W, N1);
    memset(p, 0, 40);
    p[0] = 0x44;
    p[1] = 0x0f;
    p[2] = 1;
    memcpy(p + 4, Y, 6);
    memcpy(p + 10, X, 6);
    snap();
    rx(f, 14 + 20);
    CHECK(ONLY(BAT_C_RX, BAT_C_UT_RX, BAT_C_UC_TTL) && S.ntx == 0, "UNICAST_TVLV relay with TTL 1: dropped");
}

/* ---- soft-interface unicast ---------------------------------------------------------- */

static void test_soft_tx(void)
{
    uint8_t in[256];
    size_t il;
    setup(1500);
    struct bat_orig *x = bat_orig_find(B, X);
    x->tt.ttvn = 0x37;
    il = eth(in, CX, WS, 0x0800, 80);
    snap();
    CHECK(bat_tx_soft(B, in, il) == 0, "soft unicast to X's client: accepted");
    const struct frame *t = last_tx();
    CHECK(t && t->len == 14 + 10 + il && memcmp(t->b, N1, 6) == 0 && memcmp(t->b + 6, W, 6) == 0 &&
          bat_get16(t->b + 12) == 0x4305 && memcmp(t->b + 14, "\x40\x0f\x32\x37", 4) == 0 &&
          memcmp(t->b + 18, X, 6) == 0 && memcmp(t->b + 24, in, il) == 0 &&
          ONLY(BAT_C_ST_TX, BAT_C_ST_UNICAST, BAT_C_LK_TX),
          "UNICAST 40 0f 32 <S(X)> <X> + frame, to X's next hop N1 (dataplane §3.1)");
    il = eth(in, CU, WS, 0x0800, 80);
    snap();
    CHECK(bat_tx_soft(B, in, il) == -1 && ONLY(BAT_C_ST_TX, BAT_C_ST_NOROUTE),
          "unicast to a MAC nobody announces: dropped, never flooded (capture 21)");
    S.tx_ret = BAT_TX_NOPEER;
    il = eth(in, CY, WS, 0x0800, 80);
    snap();
    CHECK(bat_tx_soft(B, in, il) == 0 && ONLY(BAT_C_ST_TX, BAT_C_ST_UNICAST, BAT_C_LK_NOPEER),
          "the station vanished under us: lk_nopeer counted, the frame counts as handed on");
}

/* ---- simulator scenarios ------------------------------------------------------------ */

static struct bat_sim *line(uint32_t seed, uint16_t mtu_a)
{
    struct bat_sim *s = bat_sim_new(3, seed);
    bat_sim_link(s, 0, 1, true, 100);
    bat_sim_link(s, 1, 2, true, 100);
    bat_sim_cfg(s, 0)->hard_mtu = mtu_a;
    for (unsigned i = 0; i < 3; i++) {
        bat_sim_start(s, i);
    }
    bat_sim_run(s, 8000);
    return s;
}

static uint32_t cnt(struct bat_sim *s, unsigned i, enum bat_counter c)
{
    return bat_sim_counter(s, i, c);
}

static void s4(void)
{
    uint8_t f[256];
    struct bat_sim *s = line(4, 1500);
    struct bat *a = bat_sim_engine(s, 0), *c = bat_sim_engine(s, 2);
    bat_sim_capture(s, true);
    size_t n = bat_sim_mk_eth(f, bat_sim_soft(s, 2), bat_sim_soft(s, 0), 0x0800, 100, 0x5c);
    uint32_t fw = cnt(s, 1, BAT_C_UC_FWD);
    CHECK(bat_sim_soft_tx(s, 0, f, n) == 0, "S4: A sends a unicast soft frame to C's soft MAC");
    bat_sim_run(s, 5);
    const struct bat_sim_frame *first = NULL;
    for (unsigned k = 0; k < bat_sim_captured(s); k++) {
        const struct bat_sim_frame *fr = bat_sim_captured_get(s, k);
        if (fr->from == 0 && fr->bytes[14] == 0x40) {
            first = fr;
        }
    }
    struct bat_orig *ac = bat_orig_find(a, bat_sim_hard(s, 2));
    CHECK(bat_sim_soft_rx_count(s, 2) == 1 && bat_sim_soft_rx_get(s, 2, 0)->len == n &&
          memcmp(bat_sim_soft_rx_get(s, 2, 0)->bytes, f, n) == 0,
          "S4: delivered once at C, byte-identical");
    CHECK(cnt(s, 1, BAT_C_UC_FWD) == fw + 1 && first && first->to == 1 && first->bytes[17] == ac->tt.ttvn &&
          ac->tt.ttvn == c->tt.ttvn && first->bytes[16] == 50,
          "S4: via W (uc_fwd), TTVN = A's S for C (= C's TTVN %u), TTL 50 from A", c->tt.ttvn);
    bat_sim_free(s);
}

static void s6(void)
{
    uint8_t f[1600];
    struct bat_sim *s = line(6, 600);
    bat_sim_capture(s, true);
    size_t n = bat_sim_mk_eth(f, bat_sim_soft(s, 2), bat_sim_soft(s, 0), 0x0800, 1386, 0x6d);
    bat_sim_soft_tx(s, 0, f, n);
    bat_sim_run(s, 5);
    unsigned fa = 0, fw = 0;
    for (unsigned k = 0; k < bat_sim_captured(s); k++) {
        const struct bat_sim_frame *fr = bat_sim_captured_get(s, k);
        if (fr->bytes[14] == 0x41) {
            fa += fr->from == 0 && fr->len <= 14 + 600;
            fw += fr->from == 1;
        }
    }
    CHECK(fa == 3 && cnt(s, 1, BAT_C_FR_FWD) == 3 && fw == 3 && cnt(s, 1, BAT_C_FR_DONE) == 0,
          "S6: A (MTU 600) sends 1410 bytes as 3 fragments; W forwards them unchanged (fr_fwd)");
    CHECK(cnt(s, 2, BAT_C_FR_DONE) == 1 && bat_sim_soft_rx_count(s, 2) == 1 &&
          bat_sim_soft_rx_get(s, 2, 0)->len == n && memcmp(bat_sim_soft_rx_get(s, 2, 0)->bytes, f, n) == 0,
          "S6: C reassembles and delivers the frame once");
    bat_sim_free(s);
}

/* Node @from builds a UNICAST toward originator @to with @ttvn and inner destination @idst. */
static void uc_send(struct bat_sim *s, unsigned from, unsigned to, uint8_t ttvn, const uint8_t *idst)
{
    struct bat *a = bat_sim_engine(s, from);
    struct bat_orig *ac = bat_orig_find(a, bat_sim_hard(s, to));
    uint8_t *p = a->txbuf + 14;
    p[0] = 0x40;
    p[1] = 0x0f;
    p[2] = 50;
    p[3] = ttvn;
    memcpy(p + 4, ac->addr, 6);
    size_t n = bat_sim_mk_eth(p + 10, idst, bat_sim_soft(s, from), 0x0800, 50, 0x13);
    bat_send_to_orig(a, ac, a->txbuf, 14 + 10 + n, BAT_TBL_DEFAULT);
    bat_sim_run(s, 5);
}

static void s13(void)
{
    enum { A, Wn, Cn, D };
    struct bat_sim *s = bat_sim_new(4, 13);
    bat_sim_link(s, A, Wn, true, 100);
    bat_sim_link(s, Wn, Cn, true, 100);
    bat_sim_link(s, Cn, D, true, 100);
    for (unsigned i = 0; i < 4; i++) {
        bat_sim_start(s, i);
    }
    struct bat *c = bat_sim_engine(s, Cn), *d = bat_sim_engine(s, D);
    const uint8_t k[6] = { 0x06, 0x5a, 0xee, 0x00, 0x00, 0x01 };
    const uint8_t u[6] = { 0x06, 0x5a, 0xee, 0x00, 0x00, 0x02 };
    bat_tt_local_add(d, k, 0, 0);          /* D serves client k */
    bat_sim_run(s, 10000);
    uint8_t stale = (uint8_t)(c->tt.ttvn - 1);
    uint32_t sd = cnt(s, Wn, BAT_C_UC_STALE_DROP), fw = cnt(s, Wn, BAT_C_UC_FWD);
    uc_send(s, A, Cn, stale, bat_sim_soft(s, Cn));
    CHECK(cnt(s, Wn, BAT_C_UC_FWD) == fw + 1 && cnt(s, Wn, BAT_C_UC_STALE_DROP) == sd,
          "S13: stale TTVN at relay W: forwarded, no check (deviation 2.4.3)");
    CHECK(bat_sim_soft_rx_count(s, Cn) == 1, "S13: ... and at C, whose own client it is: delivered");
    uint32_t rr = cnt(s, Cn, BAT_C_UC_REROUTE);
    uc_send(s, A, Cn, stale, k);
    CHECK(cnt(s, Cn, BAT_C_UC_REROUTE) == rr + 1 && bat_sim_soft_rx_count(s, D) == 1 &&
          bat_sim_soft_rx_count(s, Cn) == 1 && cnt(s, D, BAT_C_UC_DELIVER) == 1,
          "S13: stale at C for D's client: C re-routes it to D (S(D)), which delivers it");
    uint32_t dr = cnt(s, Cn, BAT_C_UC_STALE_DROP);
    uc_send(s, A, Cn, stale, u);
    CHECK(cnt(s, Cn, BAT_C_UC_STALE_DROP) == dr + 1 && bat_sim_soft_rx_count(s, Cn) == 1,
          "S13: stale at C for a client nobody announces: dropped");
    uc_send(s, A, Cn, c->tt.ttvn, u);
    CHECK(bat_sim_soft_rx_count(s, Cn) == 2, "S13: current TTVN, unknown inner destination: delivered at C");
    /* a leaf has no interface-table route, so it cannot pass a re-routed packet on */
    uint32_t nr = cnt(s, D, BAT_C_UC_NOROUTE), dd = cnt(s, D, BAT_C_UC_REROUTE);
    uc_send(s, Cn, D, (uint8_t)(d->tt.ttvn - 1), bat_sim_soft(s, A));
    CHECK(cnt(s, D, BAT_C_UC_REROUTE) == dd + 1 && cnt(s, D, BAT_C_UC_NOROUTE) == nr + 1 &&
          bat_sim_soft_rx_count(s, A) == 0,
          "S13: stale at leaf D for A's client: re-routed, then dropped (uc_noroute, no interface route)");
    bat_sim_free(s);
}

static const struct bat_sim_frame *find_icmp(struct bat_sim *s, unsigned from, unsigned to, uint8_t mt,
                                             unsigned skip)
{
    for (unsigned k = 0; k < bat_sim_captured(s); k++) {
        const struct bat_sim_frame *fr = bat_sim_captured_get(s, k);
        if (fr->from == from && fr->to == to && fr->bytes[14] == 0x43 && fr->bytes[17] == mt && skip-- == 0) {
            return fr;
        }
    }
    return NULL;
}

static void s14(void)
{
    struct bat_sim *s = line(14, 1500);
    struct bat *a = bat_sim_engine(s, 0);
    const uint8_t *A = bat_sim_hard(s, 0), *Wm = bat_sim_hard(s, 1), *Cm = bat_sim_hard(s, 2);
    bat_sim_capture(s, true);
    uint32_t oth = cnt(s, 0, BAT_C_IC_OTHER);
    CHECK(bat_icmp_send_echo(a, Cm, 0x61, 1, 20) == 0, "S14: A pings C");
    bat_sim_run(s, 5);
    const struct bat_sim_frame *rep = find_icmp(s, 1, 0, 0, 0);
    CHECK(rep && cnt(s, 2, BAT_C_IC_REPLY) == 1 && cnt(s, 0, BAT_C_IC_OTHER) == oth + 1 &&
          memcmp(rep->bytes + 18, A, 6) == 0 && memcmp(rep->bytes + 24, Cm, 6) == 0 && rep->bytes[16] == 49,
          "S14: C's echo reply reaches A through W (TTL 49 on the last hop)");
    /* traceroute probe with TTL 1 */
    uint8_t *p = a->txbuf + 14;
    memset(p, 0, 20);
    p[0] = 0x43;
    p[1] = 0x0f;
    p[2] = 1;
    p[3] = 8;
    memcpy(p + 4, Cm, 6);
    memcpy(p + 10, A, 6);
    p[16] = 0x2e;
    bat_send_to_orig(a, bat_orig_find(a, Cm), a->txbuf, 34, BAT_TBL_DEFAULT);
    bat_sim_run(s, 5);
    const struct bat_sim_frame *tx = find_icmp(s, 1, 0, 11, 0);
    CHECK(tx && tx->bytes[16] == 50 && memcmp(tx->bytes + 18, A, 6) == 0 && memcmp(tx->bytes + 24, Wm, 6) == 0 &&
          tx->bytes[30] == 0x2e && cnt(s, 2, BAT_C_IC_RX) == 1,
          "S14: TTL 1 probe: TTL exceeded from W (src W, TTL 50), C never sees it");
    /* record route: A, W, C, W, A */
    bat_icmp_send_echo(a, Cm, 0xb6, 1, 116);
    bat_sim_run(s, 5);
    const struct bat_sim_frame *r1 = find_icmp(s, 0, 1, 8, 2), *r2 = find_icmp(s, 1, 2, 8, 1),
                               *r3 = find_icmp(s, 2, 1, 0, 1), *r4 = find_icmp(s, 1, 0, 0, 1);
    CHECK(r1 && r1->len == 130 && r1->bytes[31] == 1 && memcmp(r1->bytes + 34, A, 6) == 0,
          "S14: record route leaves A with count 1, slot 0 = A (batctl pre-fill)");
    CHECK(r2 && r2->bytes[31] == 2 && memcmp(r2->bytes + 40, Wm, 6) == 0 && r3 && r3->bytes[31] == 3 &&
          memcmp(r3->bytes + 46, Cm, 6) == 0 && r4 && r4->bytes[31] == 4 && memcmp(r4->bytes + 52, Wm, 6) == 0,
          "S14: W, C, W append themselves (counts 2, 3, 4)");
    uint8_t buf[256] = { 0 };
    if (r4) {
        memcpy(buf, r4->bytes, r4->len);
        bat_rx_hard(a, buf, r4->len);
    }
    CHECK(buf[31] == 5 && memcmp(buf + 58, A, 6) == 0, "S14: and the requester on arrival: A, W, C, W, A (count 5)");
    bat_sim_free(s);
}

int main(void)
{
    test_uc_dest();
    test_relay();
    test_unknown();
    test_frag_tx();
    test_frag_rx();
    test_icmp();
    test_utvlv();
    test_soft_tx();
    s4();
    s6();
    s13();
    s14();
    free(B);
    if (failures) {
        printf("test_bat_data: %d FAILED\n", failures);
        return 1;
    }
    printf("test_bat_data: all passed\n");
    return 0;
}
