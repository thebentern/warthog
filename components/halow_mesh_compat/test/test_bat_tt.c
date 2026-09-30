/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Translation-table tests for the BATMAN_V engine (main/bat/bat_tt.c plus the
 * UNICAST_TVLV path in bat_utvlv.c that carries TT requests and responses).
 *
 * The expected values come from the clean-room spec (batman-spec/tt.md, membership.md,
 * packets.md) whose numbers were measured against batman-adv 2024.3:
 *  - the own OGM TT container of a minimal node, byte for byte (tt §4.5), its commit
 *    and 3 re-sends, the CRCs of tt §3.3 (0x2986c104, 0x1871a4d8, 0x149229c9);
 *  - the OGM decision procedure of tt §5.2 (apply a diff, changes-only request, full
 *    request on a jump / wrong CRC / wrong VLAN set), the CRC check of tt §5.4, the
 *    response handling of tt §6.4 and the answer to a request (tt §6.2 and the E8 bytes);
 *  - temporary entries (tt §8.3) that follow the latest originator with their creation time
 *    kept (tt §8.1 step 4), best-originator resolution (tt §9.1), originator loss (tt §8.2);
 *  - the two queries bat0 addressing makes: which routed originator a client MAC resolves to
 *    (and how long ago it was heard), and the best gateway by min(route throughput, announced
 *    download) (membership §6.3 item 5);
 *  - the design's deviations: TTVN 255 -> 0 accepted as +1, DEL|ROAM applied as DEL,
 *    ROAM-flagged full-table records not stored, requests paced at one per 3 s once a node's
 *    table is held and at every OGM (at least 500 ms apart) before, answers paced at one per
 *    requester per 500 ms, temporary rows capped at a quarter of the table;
 *  - a node whose full table cannot be taken (an answer over the 2048-byte reassembly buffer,
 *    or one that does not fit the 256 rows): the wait before asking it again doubles from 3 s
 *    to 60 s, and is 3 s again once its table is in sync; an oversized answer is charged to the
 *    owner its head piece names, never to a relay that answered on its behalf and cut it
 *    (tt §6.3, dataplane §2.4); unicast headers toward such a node carry the TTVN its OGMs
 *    announce, so a batman-adv relay does not drop them (dataplane §6.2), unless it is the next hop;
 *  - AT+BATTG? at a full 256-row table: each originator's CRC lines before its rows, paged
 *    whole through the port's 4 KiB buffer, and the lookup by client or originator MAC.
 * The second half runs the in-process simulator (bat_sim) for scenarios S3, S12, S15.
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

#define MAXTX 512
#define MAXDL 64
struct frame { uint32_t t; size_t len; uint8_t b[BAT_MAX_LINK_FRAME]; };
struct stub {
    uint32_t now;
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
static const uint8_t N1[6] = { 0x02, 0x11, 0x00, 0x00, 0x00, 0x01 };    /* neighbour */
static const uint8_t N2[6] = { 0x02, 0x22, 0x00, 0x00, 0x00, 0x01 };    /* neighbour */
static const uint8_t X[6] = { 0x02, 0x55, 0x00, 0x00, 0x00, 0x01 };     /* originator via N1 */
static const uint8_t Y[6] = { 0x02, 0x56, 0x00, 0x00, 0x00, 0x01 };     /* originator via N2 */
static const uint8_t C1[6] = { 0x06, 0x55, 0x00, 0x00, 0x00, 0x01 };    /* clients */
static const uint8_t C2[6] = { 0x06, 0x55, 0x00, 0x00, 0x00, 0x02 };
static const uint8_t C3[6] = { 0x06, 0x55, 0x00, 0x00, 0x00, 0x03 };
static const uint8_t C4[6] = { 0x06, 0x55, 0x00, 0x00, 0x00, 0x04 };

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

static size_t mk_ogm(uint8_t *f, const uint8_t *src, const uint8_t *orig, uint32_t seq,
                     const uint8_t *tvlv, uint16_t tl)
{
    link_hdr(f, BC, src);
    uint8_t *p = f + 14;
    p[0] = 0x04;
    p[1] = 0x0f;
    p[2] = 50;
    p[3] = 0;
    bat_put32(p + 4, seq);
    memcpy(p + 8, orig, 6);
    bat_put16(p + 14, tl);
    bat_put32(p + 16, 0xFFFFFFFFu);
    if (tl) {
        memcpy(p + 20, tvlv, tl);
    }
    return 34u + tl;
}

/* TT change or full-table record */
struct rec { uint8_t flags; const uint8_t *mac; uint16_t vid; };
struct vl { uint32_t crc; uint16_t vid; };

/* TT container (header included): flags, ttvn, VLAN records, change records. */
static size_t tt_tvlv(uint8_t *o, uint8_t flags, uint8_t ttvn, const struct vl *v, unsigned nv,
                      const struct rec *r, unsigned nr)
{
    size_t n = 8;
    o[0] = 0x04;
    o[1] = 0x01;
    o[4] = flags;
    o[5] = ttvn;
    bat_put16(o + 6, (uint16_t)nv);
    for (unsigned i = 0; i < nv; i++, n += 8) {
        bat_put32(o + n, v[i].crc);
        bat_put16(o + n + 4, v[i].vid);
        bat_put16(o + n + 6, 0);
    }
    for (unsigned i = 0; i < nr; i++, n += 12) {
        o[n] = r[i].flags;
        o[n + 1] = o[n + 2] = o[n + 3] = 0;
        memcpy(o + n + 4, r[i].mac, 6);
        bat_put16(o + n + 10, r[i].vid);
    }
    bat_put16(o + 2, (uint16_t)(n - 4));
    return n;
}

/* UNICAST_TVLV frame to the engine: link N1 -> W, dst/src originators, one TVLV area. */
static size_t mk_utvlv(uint8_t *f, const uint8_t *lsrc, const uint8_t *dst, const uint8_t *src,
                       const uint8_t *tvlv, size_t tl)
{
    link_hdr(f, W, lsrc);
    uint8_t *p = f + 14;
    p[0] = 0x44;
    p[1] = 0x0f;
    p[2] = 50;
    p[3] = 0;
    memcpy(p + 4, dst, 6);
    memcpy(p + 10, src, 6);
    bat_put16(p + 16, (uint16_t)tl);
    p[18] = p[19] = 0;
    memcpy(p + 20, tvlv, tl);
    return 34 + tl;
}

static uint32_t crc1(const uint8_t *mac, uint16_t vid, uint8_t flags)
{
    return bat_crc32c_tt(vid, flags, mac);
}

/* ---- engine helpers ------------------------------------------------------------ */

static struct bat *B;
static uint8_t RXBUF[4096];

static void stub_reset(void)
{
    memset(&S, 0, sizeof(S));
    S.rng = 0x2545F491u;
    S.lt_default = BAT_TPUT_UNKNOWN;
    S.tx_ret = BAT_TX_OK;
    S.now = 100000;
}

static struct bat *eng2(const uint8_t *hard, const uint8_t *soft)
{
    struct bat_config c;
    if (!B) {
        B = malloc(bat_ctx_size());
    }
    bat_config_defaults(&c);
    memcpy(c.hard_addr, hard, 6);
    memcpy(c.soft_addr, soft, 6);
    if (bat_init(B, &c, &OPS, NULL) != 0) {
        printf("FAIL bat_init refused a valid config\n");
        failures++;
    }
    return B;
}

static void fresh(void)
{
    stub_reset();
    eng2(W, WS);
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

static uint32_t SEQ = 5000;

/* One OGM from @orig via neighbour @via carrying TT container @tv. */
static void ogm_tt(const uint8_t *via, const uint8_t *orig, const uint8_t *tv, size_t tl)
{
    uint8_t f[1700];
    rx(f, mk_ogm(f, via, orig, SEQ++, tv, (uint16_t)tl));
}

static void run_to(uint32_t t)
{
    while ((int32_t)(t - S.now) > 0) {
        S.now++;
        bat_tick(B);
    }
}

static struct bat_orig *orig(const uint8_t *a)
{
    return bat_orig_find(B, a);
}

/* rows of originator @o (any state) */
static unsigned rows_of(const struct bat_orig *o)
{
    unsigned n = 0, oi = bat_orig_index(B, o);
    for (unsigned i = 0; i < BAT_TT_ROWS; i++) {
        n += B->tt.rows[i].used && B->tt.rows[i].orig == oi;
    }
    return n;
}

static const struct bat_tt_row *row(const uint8_t *mac, uint16_t vid, const uint8_t *via)
{
    struct bat_orig *o = via ? orig(via) : NULL;
    for (unsigned i = 0; i < BAT_TT_ROWS; i++) {
        const struct bat_tt_row *r = &B->tt.rows[i];
        if (r->used && memcmp(r->mac, mac, 6) == 0 && r->vid == vid &&
            (!o || r->orig == bat_orig_index(B, o))) {
            return r;
        }
    }
    return NULL;
}

/* Last UNICAST_TVLV the engine sent; NULL if none since @from. Returns the TT value;
 * LU_LEN / LU_DST describe that frame. */
static size_t LU_LEN;
static const uint8_t *LU_DST;
static const uint8_t *last_utvlv(unsigned from, const uint8_t **pkt, size_t *vlen)
{
    for (unsigned k = S.ntx; k-- > from;) {
        const uint8_t *p = S.tx[k].b + 14;
        if (S.tx[k].len >= 34 + 4 && p[0] == 0x44) {
            LU_LEN = S.tx[k].len;
            LU_DST = S.tx[k].b;
            if (pkt) {
                *pkt = p;
            }
            if (vlen) {
                *vlen = bat_get16(p + 22);
            }
            return p + 24;
        }
    }
    return NULL;
}

static unsigned count_type(unsigned from, uint8_t type)
{
    unsigned n = 0;
    for (unsigned k = from; k < S.ntx; k++) {
        n += S.tx[k].len > 14 && S.tx[k].b[14] == type;
    }
    return n;
}

static int hexeq(const uint8_t *a, const char *hex, size_t *n_out)
{
    size_t n = 0;
    for (const char *h = hex; *h;) {
        if (*h == ' ' || *h == '|') {
            h++;
            continue;
        }
        unsigned v;
        if (sscanf(h, "%2x", &v) != 1) {
            return 0;
        }
        if (a[n] != v) {
            printf("     byte %zu: have %02x want %02x\n", n, a[n], v);
            return 0;
        }
        n++;
        h += 2;
    }
    if (n_out) {
        *n_out = n;
    }
    return 1;
}

static size_t build(uint8_t *out, size_t max)
{
    B->now = S.now;
    return bat_tt_ogm_tvlv_build(B, out, max);
}

/* ---- small helpers ------------------------------------------------------------- */

static void test_helpers(void)
{
    uint8_t f[64] = { 0 };
    memcpy(f, C1, 6);
    memcpy(f + 6, C2, 6);
    f[12] = 0x08;
    CHECK(bat_frame_vid(f, 60) == 0x0000, "vid: untagged frame -> 0x0000");
    f[12] = 0x81;
    f[13] = 0x00;
    f[14] = 0xa0;
    f[15] = 0x05;
    CHECK(bat_frame_vid(f, 60) == 0x8005, "vid: 802.1Q VLAN 5 (PCP 5) -> 0x8005 (tt §1.1)");
    f[14] = 0x00;
    f[15] = 0x00;
    CHECK(bat_frame_vid(f, 60) == 0x8000, "vid: priority tag -> 0x8000, not untagged");
    f[14] = 0x3f;
    f[15] = 0xff;
    CHECK(bat_frame_vid(f, 60) == 0x8fff, "vid: VLAN 4095, DEI/PCP bits masked -> 0x8fff");
    CHECK(bat_frame_vid(f, 15) == 0x0000, "vid: tag cut short -> treated as untagged");
    CHECK(!bat_ttvn_older(0x81, 1) && bat_ttvn_older(0x82, 1) && bat_ttvn_older(0, 1) &&
          !bat_ttvn_older(1, 1) && !bat_ttvn_older(2, 1) && !bat_ttvn_older(0x80, 1),
          "ttvn serial rule (dataplane §6.1): vs 1, 0x81 not older, 0x82 and 0 older, 1/2/0x80 not");
    CHECK(!bat_ttvn_older(4, 3) && !bat_ttvn_older(130, 3) && bat_ttvn_older(2, 3) &&
          bat_ttvn_older(0, 3) && bat_ttvn_older(200, 3),
          "ttvn serial rule: dataplane §6.2 relay table (3 current, 4/130 newer, 2/0/200 older)");
}

/* ---- own table and the OGM container ---------------------------------------------- */

static const uint8_t M1[6] = { 0x02, 0x00, 0x5e, 0x10, 0x00, 0x01 };   /* tt §12 W */
static const uint8_t M99[6] = { 0x02, 0x00, 0x5e, 0x10, 0x00, 0x99 };

static void test_local_build(void)
{
    uint8_t o[512];
    size_t n = 0, w = 0;
    stub_reset();
    eng2(M1, M1);
    CHECK(B->tt.n_local == 1 && B->tt.local[0].state == BAT_TTL_NEW && B->tt.n_changes == 1 &&
          bat_tt_own_ttvn(B) == 0,
          "init: soft MAC queued as one uncommitted ADD, TTVN 0 (tt §4.4)");
    n = build(o, 492);
    CHECK(n == 28 && hexeq(o, "04 01 00 18 | 01 01 00 01 | 29 86 c1 04 00 00 00 00 | "
                              "00 00 00 00 02 00 5e 10 00 01 00 00", &w) && w == 28,
          "first own TT container = tt §4.5 example byte for byte (TTVN 1, CRC 0x2986c104, one ADD)");
    int same = 1;
    for (int i = 0; i < 3; i++) {
        uint8_t r[512];
        same &= build(r, 492) == 28 && memcmp(r, o, 28) == 0;
    }
    CHECK(same && B->tt.resend == 0, "the changeset rides in 3 more OGMs unchanged (4 in all, tt §4.5)");
    n = build(o, 492);
    CHECK(n == 16 && hexeq(o, "04 01 00 0c | 01 01 00 01 | 29 86 c1 04 00 00 00 00", NULL),
          "5th container: same TTVN and CRC, no change records (tt §4.5 'afterwards')");
    CHECK(B->tt.local[0].state == BAT_TTL_ON && bat_tt_own_ttvn(B) == 1, "soft MAC committed, TTVN 1");

    CHECK(bat_tt_local_add(B, M99, 0, 0) == 0, "hook: add a second client");
    n = build(o, 492);
    CHECK(n == 28 && hexeq(o, "04 01 00 18 | 01 02 00 01 | 18 71 a4 d8 00 00 00 00 | "
                              "00 00 00 00 02 00 5e 10 00 99 00 00", NULL),
          "second client: TTVN 2, CRC 0x1871a4d8 (tt §3.3), one ADD");
    for (int i = 0; i < 3; i++) {
        build(o, 492);
    }
    n = build(o, 492);
    CHECK(n == 16 && bat_get32(o + 8) == 0x1871a4d8u, "then steady with CRC 0x1871a4d8");
    CHECK(bat_tt_local_del(B, M99, 0) == 0 && B->tt.local[1].state == BAT_TTL_DEL,
          "hook: delete it -> pending-delete until the next commit");
    n = build(o, 492);
    CHECK(n == 28 && hexeq(o, "04 01 00 18 | 01 03 00 01 | 29 86 c1 04 00 00 00 00 | "
                              "01 00 00 00 02 00 5e 10 00 99 00 00", NULL) && B->tt.n_local == 1,
          "delete: TTVN 3, CRC back to 0x2986c104, one DEL record (flags 0x01), entry gone");
    for (int i = 0; i < 4; i++) {
        build(o, 492);
    }
    /* add + delete inside one OGM interval cancel out (tt §4.3) */
    bat_tt_local_add(B, M99, 0, 0);
    bat_tt_local_del(B, M99, 0);
    CHECK(B->tt.n_changes == 0 && B->tt.n_local == 1, "coalescing: ADD then DEL of an uncommitted entry -> nothing");
    n = build(o, 492);
    CHECK(n == 16 && o[5] == 3, "... and the next OGM keeps TTVN 3");
    /* delete + re-add of a committed entry cancel out too */
    bat_tt_local_add(B, M99, 0, 0);
    build(o, 492);
    for (int i = 0; i < 4; i++) {
        build(o, 492);
    }
    CHECK(o[5] == 4, "setup: second client committed at TTVN 4");
    bat_tt_local_del(B, M99, 0);
    bat_tt_local_add(B, M99, 0, 0);
    CHECK(B->tt.n_changes == 0 && B->tt.local[1].state == BAT_TTL_ON,
          "coalescing: DEL then ADD of a committed entry -> nothing queued, entry committed again");
    n = build(o, 492);
    CHECK(n == 16 && o[5] == 4 && bat_get32(o + 8) == 0x1871a4d8u, "... TTVN and CRC unchanged");
    /* flags: WIFI/ISOLA enter the change record and the CRC */
    CHECK(bat_tt_local_add(B, M99, 0, 0x20) == -1, "hook: changing the flags of an existing entry is refused");
    CHECK(bat_tt_local_add(B, C1, 0, 0x21) == 0, "hook: a new entry with ISOLA (+ a DEL bit, masked)");
    n = build(o, 492);
    CHECK(n == 28 && o[16] == 0x20 && bat_get32(o + 8) == (0x1871a4d8u ^ crc1(C1, 0, 0x20)),
          "ISOLA client: ADD record flags 0x20, CRC includes the flag (tt §3.1)");
}

static void test_local_misc(void)
{
    uint8_t o[512];
    size_t n;
    stub_reset();
    eng2(M1, M1);
    CHECK(bat_tt_local_add(B, M1, 0x8000, 0) == 0, "hook: soft MAC also in VID 0x8000");
    n = build(o, 492);
    CHECK(n == 4 + 4 + 16 + 24 && bat_get16(o + 6) == 2 && bat_get16(o + 12) == 0x0000 &&
          bat_get16(o + 20) == 0x8000 && bat_get32(o + 16) == crc1(M1, 0x8000, 0) &&
          bat_get16(o + 24 + 10) == 0x0000 && bat_get16(o + 36 + 10) == 0x8000,
          "two VLANs: one record each (untagged first), two ADDs");
    /* a VLAN whose last entry is deleted disappears from the list */
    for (int i = 0; i < 4; i++) {
        build(o, 492);
    }
    bat_tt_local_del(B, M1, 0x8000);
    n = build(o, 492);
    CHECK(bat_get16(o + 6) == 1 && n == 4 + 4 + 8 + 12, "empty VLAN omitted after its delete commits");
    /* oversize changeset: no change records, queue consumed, TTVN still advances */
    for (int i = 0; i < 4; i++) {
        build(o, 492);
    }
    uint8_t t0 = bat_tt_own_ttvn(B);
    bat_tt_local_add(B, M99, 0, 0);
    bat_tt_local_add(B, C1, 0, 0);
    n = build(o, 30);
    CHECK(n == 16 && o[5] == (uint8_t)(t0 + 1) && B->tt.n_changes == 0 && B->tt.resend == 3,
          "changeset larger than the room: container without records, TTVN +1, queue consumed (tt §4.5.7)");
    n = build(o, 492);
    CHECK(n == 16, "... and its re-sends carry no records either");
    /* TTVN wraps 255 -> 0 */
    for (int i = 0; i < 3; i++) {
        build(o, 492);
    }
    B->tt.ttvn = 255;
    bat_tt_local_del(B, C1, 0);
    n = build(o, 492);
    CHECK(o[5] == 0 && bat_tt_own_ttvn(B) == 0, "TTVN 255 -> 0 at the next commit (tt §4.4)");
    /* capacity */
    stub_reset();
    eng2(M1, M1);
    int ok = 1;
    for (unsigned i = 1; i < BAT_TT_LOCAL_MAX; i++) {
        uint8_t m[6] = { 0x06, 0x01, 0, 0, 0, (uint8_t)i };
        ok &= bat_tt_local_add(B, m, 0, 0) == 0;
    }
    uint8_t m[6] = { 0x06, 0x01, 0, 0, 0, 0x77 };
    CHECK(ok && bat_tt_local_add(B, m, 0, 0) == -1, "hook: local table full -> -1");
    CHECK(bat_tt_local_add(B, BC, 0, 0) == -1 && bat_tt_local_del(B, m, 0) == -1,
          "hook: group MAC refused; deleting an unknown entry -> -1");
    bat_tt_local_del(B, M1, 0);
    CHECK(B->tt.local[0].state == BAT_TTL_FREE || B->tt.n_local == BAT_TT_LOCAL_MAX - 1,
          "hook: an uncommitted entry is deleted at once");
}

/* ---- OGM reception (tt §5) ------------------------------------------------------------ */

static void setup_x(void)
{
    fresh();
    add_neigh(N1, N1, 1000);
    add_neigh(N2, N2, 1000);
}

static void test_ogm_rx(void)
{
    uint8_t tv[1600];
    size_t tl, vlen;
    const uint8_t *pkt, *v;
    unsigned t0;
    setup_x();
    /* (a) first contact: TTVN 1 with the whole table as ADDs and a matching CRC */
    struct vl v1[] = { { crc1(C1, 0, 0), 0 } };
    struct rec r1[] = { { 0, C1, 0 } };
    tl = tt_tvlv(tv, 0x01, 1, v1, 1, r1, 1);
    t0 = S.ntx;
    ogm_tt(N1, X, tv, tl);
    struct bat_orig *o = orig(X);
    CHECK(o && o->tt.known && o->tt.ttvn == 1 && rows_of(o) == 1 && row(C1, 0, X) &&
          row(C1, 0, X)->ttvn == 1 && C(BAT_C_TT_DIFF) == 1 && count_type(t0, 0x44) == 0,
          "TTVN 1 with changes on first contact: applied, S = 1, known, no request (tt §5.2 1.2)");
    /* (b) the same changeset again with the next OGM: rule 3, nothing */
    tl = tt_tvlv(tv, 0x01, 1, v1, 1, r1, 1);
    ogm_tt(N1, X, tv, tl);
    CHECK(C(BAT_C_TT_DIFF) == 1 && rows_of(o) == 1 && count_type(t0, 0x44) == 0,
          "repeated changeset at the same TTVN: ignored, no request");
    /* (c) next TTVN with one ADD */
    struct vl v2[] = { { crc1(C1, 0, 0) ^ crc1(C2, 0, 0), 0 } };
    struct rec r2[] = { { 0, C2, 0 } };
    tl = tt_tvlv(tv, 0x01, 2, v2, 1, r2, 1);
    ogm_tt(N1, X, tv, tl);
    CHECK(o->tt.ttvn == 2 && rows_of(o) == 2 && row(C2, 0, X)->ttvn == 2 && count_type(t0, 0x44) == 0,
          "TTVN S+1 with an ADD: applied, entry carries TTVN 2");
    /* (d) next TTVN without changes (the 4 carrying OGMs were missed): changes-only request */
    tl = tt_tvlv(tv, 0x01, 3, v1, 1, NULL, 0);
    ogm_tt(N1, X, tv, tl);
    v = last_utvlv(t0, &pkt, &vlen);
    CHECK(v && LU_LEN == 14 + 20 + 16 && memcmp(LU_DST, N1, 6) == 0 &&
          hexeq(pkt, "44 0f 32 00", NULL) && memcmp(pkt + 4, X, 6) == 0 && memcmp(pkt + 10, W, 6) == 0 &&
          bat_get16(pkt + 16) == 16 && bat_get16(pkt + 18) == 0 &&
          hexeq(pkt + 20, "04 01 00 0c 02 03 00 01", NULL) && bat_get32(pkt + 28) == v1[0].crc &&
          bat_get32(pkt + 32) == 0,
          "S+1 without changes: changes-only request (flags 0x02) to X via N1, VLAN records copied (tt §6.1)");
    CHECK(o->tt.req_pending && C(BAT_C_TT_REQ_TX) == 1 && o->tt.ttvn == 2,
          "request outstanding, S unchanged");
    unsigned t1 = S.ntx;
    ogm_tt(N1, X, tv, tl);
    CHECK(count_type(t1, 0x44) == 0, "second OGM within 3 s: no second request (one outstanding per originator)");
    S.now += 3000;
    ogm_tt(N1, X, tv, tl);
    CHECK(count_type(t1, 0x44) == 1 && C(BAT_C_TT_REQ_TX) == 2, "3 s later the request is repeated");
    /* (e) changes-only response from X: DEL C2 */
    struct rec rd[] = { { 0x01, C2, 0 } };
    uint8_t f[1700];
    tl = tt_tvlv(tv, 0x04, 3, v1, 1, rd, 1);
    rx(f, mk_utvlv(f, N1, W, X, tv, tl));
    CHECK(o->tt.ttvn == 3 && o->tt.known && rows_of(o) == 1 && !row(C2, 0, X) && !o->tt.req_pending &&
          C(BAT_C_TT_RESP_RX) == 1 && C(BAT_C_UT_CONSUMED) == 1,
          "changes-only response applied: C2 deleted, S = 3, request marker cleared (tt §6.4)");
    t1 = S.ntx;
    ogm_tt(N1, X, tv, tl = tt_tvlv(tv, 0x01, 3, v1, 1, NULL, 0));
    CHECK(count_type(t1, 0x44) == 0, "in sync: TTVN and CRC match, nothing sent (tt §5.2 3)");
    /* (g) wrong CRC at the same TTVN */
    struct vl vbad[] = { { 0x12345678, 0 } };
    uint32_t cf = C(BAT_C_TT_CRC_FAIL);
    ogm_tt(N1, X, tv, tl = tt_tvlv(tv, 0x01, 3, vbad, 1, NULL, 0));
    v = last_utvlv(t1, &pkt, NULL);
    CHECK(v && v[0] == 0x12 && v[1] == 3 && bat_get32(v + 4) == 0x12345678 &&
          C(BAT_C_TT_CRC_FAIL) == cf + 1,
          "same TTVN, wrong CRC: full-table request (0x12) carrying the announced CRC (tt §5.2 2)");
    /* (h) TTVN jump */
    S.now += 3001;
    t1 = S.ntx;
    ogm_tt(N1, X, tv, tl = tt_tvlv(tv, 0x01, 7, v1, 1, NULL, 0));
    v = last_utvlv(t1, NULL, NULL);
    CHECK(v && v[0] == 0x12 && v[1] == 7, "TTVN jump 3 -> 7: full-table request for 7");
    /* (i) full response: replaces everything of X; DEL and ROAM records skipped */
    struct vl vf[] = { { crc1(C1, 0, 0) ^ crc1(C4, 0, 0x10), 0 }, { crc1(C3, 0x8001, 0), 0x8001 } };
    struct rec rf[] = { { 0x00, C1, 0 }, { 0x00, C3, 0x8001 }, { 0x01, C2, 0 }, { 0x02, C2, 0x8001 },
                        { 0x10, C4, 0 } };
    tl = tt_tvlv(tv, 0x14, 7, vf, 2, rf, 5);
    rx(f, mk_utvlv(f, N1, W, X, tv, tl));
    CHECK(o->tt.ttvn == 7 && o->tt.known && rows_of(o) == 3 && row(C3, 0x8001, X) &&
          row(C4, 0, X) && (row(C4, 0, X)->flags & 0xF0) == 0x10 && !row(C2, 0, NULL) &&
          !row(C2, 0x8001, NULL) && C(BAT_C_TT_FULL) == 1,
          "full response: X's rows replaced; DEL- and ROAM-flagged records not stored; WIFI flag kept");
    unsigned n0;
    CHECK(bat_tt_orig_crc(B, bat_orig_index(B, o), 0, &n0) == vf[0].crc && n0 == 2 &&
          bat_tt_orig_crc(B, bat_orig_index(B, o), 0x8001, &n0) == vf[1].crc && n0 == 1,
          "per-(originator, VLAN) CRCs recomputed, synchronised flags included");
    t1 = S.ntx;
    ogm_tt(N1, X, tv, tl = tt_tvlv(tv, 0x01, 7, vf, 2, NULL, 0));
    CHECK(count_type(t1, 0x44) == 0, "OGM announcing both VLAN CRCs: in sync");
    S.now += 3001;
    ogm_tt(N1, X, tv, tl = tt_tvlv(tv, 0x01, 7, vf, 1, NULL, 0));
    v = last_utvlv(t1, NULL, NULL);
    CHECK(v && v[0] == 0x12, "a VLAN we hold entries in is missing from the OGM: full request (tt §5.4 2)");
    S.now += 3001;
    t1 = S.ntx;
    struct vl vx[] = { vf[0], vf[1], { 0, 0x8005 } };
    ogm_tt(N1, X, tv, tl = tt_tvlv(tv, 0x01, 7, vx, 3, NULL, 0));
    CHECK(count_type(t1, 0x44) == 1, "an announced VLAN we hold no entry of (CRC 0): full request (tt §5.4 1)");
    /* malformed containers */
    uint32_t bad = C(BAT_C_TT_BAD);
    uint8_t shortv[] = { 0x04, 0x01, 0x00, 0x03, 0x01, 0x07, 0x00 };
    ogm_tt(N1, X, shortv, sizeof(shortv));
    uint8_t manyv[] = { 0x04, 0x01, 0x00, 0x0c, 0x01, 0x07, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0 };
    ogm_tt(N1, X, manyv, sizeof(manyv));
    CHECK(C(BAT_C_TT_BAD) == bad + 2 && o->tt.ttvn == 7 && rows_of(o) == 3,
          "TT value < 4 bytes or VLAN count 0xFFFF: ignored as TT_BAD, nothing changed");
    uint8_t v2tv[64];
    tl = tt_tvlv(v2tv, 0x01, 8, v1, 1, r2, 1);
    v2tv[1] = 2;
    ogm_tt(N1, X, v2tv, tl);
    CHECK(o->tt.ttvn == 7 && rows_of(o) == 3, "TT container of version 2: not understood, ignored");
}

static void test_ogm_rx_more(void)
{
    uint8_t tv[1600];
    size_t tl;
    setup_x();
    /* mod-256: S = 255 -> TTVN 0 with changes is applied (deviation 2.4.6) */
    struct vl v1[] = { { crc1(C1, 0, 0), 0 } };
    struct rec r1[] = { { 0, C1, 0 } };
    tl = tt_tvlv(tv, 0x01, 1, v1, 1, r1, 1);
    ogm_tt(N1, X, tv, tl);
    struct bat_orig *o = orig(X);
    o->tt.ttvn = 255;
    unsigned t0 = S.ntx;
    struct vl v2[] = { { crc1(C1, 0, 0) ^ crc1(C2, 0, 0), 0 } };
    struct rec r2[] = { { 0, C2, 0 } };
    ogm_tt(N1, X, tv, tl = tt_tvlv(tv, 0x01, 0, v2, 1, r2, 1));
    CHECK(o->tt.ttvn == 0 && rows_of(o) == 2 && count_type(t0, 0x44) == 0,
          "TTVN 255 -> 0 applied as the next version, no request");
    /* DEL|ROAM applied as DEL; duplicate ADDs idempotent; zero MAC skipped */
    static const uint8_t Z[6] = { 0 };
    struct vl v3[] = { { crc1(C1, 0, 0) ^ crc1(C3, 0, 0), 0 } };
    struct rec r3[] = { { 0x03, C2, 0 }, { 0x00, C3, 0 }, { 0x00, C3, 0 }, { 0x00, Z, 0 } };
    ogm_tt(N1, X, tv, tl = tt_tvlv(tv, 0x01, 1, v3, 1, r3, 4));
    CHECK(o->tt.ttvn == 1 && rows_of(o) == 2 && !row(C2, 0, X) && row(C3, 0, X) &&
          !row(Z, 0, NULL) && count_type(t0, 0x44) == 0,
          "DEL|ROAM removes like DEL; a doubled ADD makes one row; zero MAC skipped; CRC matches");
    /* group MACs and odd VIDs are stored and enter the CRC (tt §1.1, §13 pitfalls 2, 14) */
    static const uint8_t G1[6] = { 0x33, 0x33, 0x00, 0x00, 0x00, 0x01 };
    static const uint8_t G2[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 };
    static const uint8_t BS[6] = { 0x02, 0x0b, 0x00, 0x00, 0xba, 0x70 };
    struct vl vg[] = { { 0x149229c9u, 0 }, { crc1(C4, 0x1234, 0), 0x1234 } };
    struct rec rg[] = { { 0, BS, 0 }, { 0, G1, 0 }, { 0, G2, 0 }, { 0, C4, 0x1234 } };
    tl = tt_tvlv(tv, 0x14, 9, vg, 2, rg, 4);
    uint8_t f[1700];
    rx(f, mk_utvlv(f, N1, W, X, tv, tl));
    t0 = S.ntx;
    ogm_tt(N1, X, tv, tl = tt_tvlv(tv, 0x01, 9, vg, 2, NULL, 0));
    CHECK(rows_of(o) == 4 && row(G1, 0, X) && row(C4, 0x1234, X) && count_type(t0, 0x44) == 0,
          "group MACs and VID 0x1234 stored; {B soft, 33:33::1, 01:00:5e::1} = 0x149229c9 checks (tt §3.3)");
    /* a remote announcement of our own soft MAC: stored, never withdraws ours */
    struct vl vw[] = { { crc1(WS, 0, 0), 0 } };
    struct rec rw[] = { { 0, WS, 0 } };
    ogm_tt(N2, Y, tv, tl = tt_tvlv(tv, 0x01, 1, vw, 1, rw, 1));
    CHECK(row(WS, 0, Y) && B->tt.n_local == 1 && bat_tt_is_own_client(B, WS, 0),
          "remote add of our soft MAC stored (for its CRC); our own entry stays (tt §8.1 note)");
    /* originator gone: rows removed, known cleared */
    bat_tt_orig_gone(B, o);
    CHECK(rows_of(o) == 0 && !o->tt.known && row(WS, 0, Y), "originator gone: its rows only are removed, known = 0");
    t0 = S.ntx;
    ogm_tt(N1, X, tv, tl = tt_tvlv(tv, 0x01, 9, vg, 2, NULL, 0));
    const uint8_t *v = last_utvlv(t0, NULL, NULL);
    CHECK(v && v[0] == 0x12 && v[1] == 9, "unknown table again: the next OGM (TTVN != 1) draws a full request");
    /* no route to the owner */
    struct bat_orig *z = bat_orig_get(B, (const uint8_t[6]){ 0x02, 0x99, 0, 0, 0, 1 });
    t0 = S.ntx;
    uint32_t nr = C(BAT_C_TT_REQ_NOROUTE);
    bat_tt_ogm_rx(B, z, tv + 4, tl - 4);
    CHECK(count_type(t0, 0x44) == 0 && C(BAT_C_TT_REQ_NOROUTE) == nr + 1 && !z->tt.req_pending,
          "request toward an originator without a default route: tt_req_noroute, no marker");
    /* response from an unknown source */
    uint32_t ru = C(BAT_C_TT_RESP_UNKNOWN);
    unsigned used = bat_tt_rows_used(B);
    rx(f, mk_utvlv(f, N1, W, (const uint8_t[6]){ 0x02, 0x98, 0, 0, 0, 1 }, tv,
                   tt_tvlv(tv, 0x14, 1, v1, 1, r1, 1)));
    CHECK(C(BAT_C_TT_RESP_UNKNOWN) == ru + 1 && bat_tt_rows_used(B) == used,
          "response from an unknown originator: tt_resp_unknown, ignored");
    uint32_t rr = C(BAT_C_TT_RESP_RX);
    tl = tt_tvlv(tv, 0x14, 5, v1, 1, r1, 1);
    tv[1] = 2;
    rx(f, mk_utvlv(f, N1, W, Y, tv, tl));
    CHECK(C(BAT_C_TT_RESP_RX) == rr && bat_tt_rows_used(B) == used && C(BAT_C_UT_CONSUMED) > 0,
          "UNICAST_TVLV for us with a TT container of version 2: consumed, not applied");
    /* the 5 s purge forgets a request marker older than 3 s */
    struct bat_orig *yy = orig(Y);
    yy->tt.req_pending = 1;
    yy->tt.req_ts = S.now - 3000;
    B->now = S.now;
    bat_tt_purge(B);
    CHECK(!yy->tt.req_pending, "purge: a request marker 3 s old is cleared");
}

static void test_rows_full(void)
{
    uint8_t tv[1600];
    size_t tl;
    setup_x();
    /* fill the table from Y with full responses of 120 entries each in distinct VLANs */
    uint8_t f[1700];
    static uint8_t macs[BAT_TT_ROWS + 8][6];
    for (unsigned i = 0; i < BAT_TT_ROWS + 8; i++) {
        uint8_t m[6] = { 0x06, 0x66, 0x00, 0x00, (uint8_t)(i >> 8), (uint8_t)i };
        memcpy(macs[i], m, 6);
    }
    struct rec r[120];
    /* Y first, so it is routable */
    ogm_tt(N2, Y, tv, tt_tvlv(tv, 0x01, 1, NULL, 0, NULL, 0));
    ogm_tt(N1, X, tv, tt_tvlv(tv, 0x01, 1, NULL, 0, NULL, 0));
    unsigned k = 0;
    for (int part = 0; part < 2; part++) {
        struct vl v = { 0, (uint16_t)(0x8100 + part) };
        for (unsigned i = 0; i < 120; i++, k++) {
            r[i].flags = 0;
            r[i].mac = macs[k];
            r[i].vid = v.vid;
            v.crc ^= crc1(macs[k], v.vid, 0);
        }
        tl = tt_tvlv(tv, 0x14, 1, &v, 1, r, 120);
        rx(f, mk_utvlv(f, N2, W, part ? X : Y, tv, tl));
    }
    CHECK(bat_tt_rows_used(B) == 240, "setup: 240 rows from X and Y");
    struct vl vv = { 0, 0x8200 };
    for (unsigned i = 0; i < 20; i++, k++) {
        r[i].flags = 0;
        r[i].mac = macs[k];
        r[i].vid = vv.vid;
        vv.crc ^= crc1(macs[k], vv.vid, 0);
    }
    struct bat_orig *x = orig(X);
    uint8_t s0 = x->tt.ttvn;
    tl = tt_tvlv(tv, 0x01, (uint8_t)(s0 + 1), &vv, 1, r, 20);
    unsigned t0 = S.ntx;
    ogm_tt(N1, X, tv, tl);
    CHECK(bat_tt_rows_used(B) == BAT_TT_ROWS && C(BAT_C_TT_ROWS_FULL) == 1 && !x->tt.known &&
          count_type(t0, 0x44) == 1 && last_utvlv(t0, NULL, NULL)[0] == 0x12,
          "table full mid-changeset: rest not applied, known = 0, CRC fails, full request (tt §5.3)");
}

/* ---- answering a request (tt §6.2, E8) --------------------------------------------- */

static void test_answer(void)
{
    static const uint8_t BB[6] = { 0x02, 0x0b, 0x00, 0x00, 0x00, 0x01 };
    uint8_t f[256], o[512];
    stub_reset();
    eng2(M1, M1);
    build(o, 492);   /* first commit: TTVN 1 */
    set_tput(BB, 1000);
    rx(f, mk_elp(f, BB, BB, 77));
    bat_neigh_find(B, BB)->tput_acc = 1000u << 10;
    rx(f, mk_ogm(f, BB, BB, 1234, NULL, 0));
    CHECK(bat_route_nh(B, orig(BB), BAT_TBL_DEFAULT) != NULL, "setup: B routed");
    const uint8_t req[] = { 0x04, 0x01, 0x00, 0x0c, 0x02, 0x01, 0x00, 0x01, 0x29, 0x86, 0xc1, 0x04,
                            0x00, 0x00, 0x00, 0x00 };
    link_hdr(f, M1, BB);
    size_t n = 14;
    const char *reqhex = "44 0f 32 00 | 02 00 5e 10 00 01 | 02 0b 00 00 00 01 | 00 10 | 00 00";
    for (const char *h = reqhex; *h;) {
        unsigned x;
        if (*h == ' ' || *h == '|') {
            h++;
            continue;
        }
        sscanf(h, "%2x", &x);
        f[n++] = (uint8_t)x;
        h += 2;
    }
    memcpy(f + n, req, sizeof(req));
    n += sizeof(req);
    unsigned t0 = S.ntx;
    rx(f, n);
    const uint8_t *pkt;
    CHECK(last_utvlv(t0, &pkt, NULL) && LU_LEN == 14 + 48 && memcmp(LU_DST, BB, 6) == 0 &&
          hexeq(pkt, "44 0f 32 00 | 02 0b 00 00 00 01 | 02 00 5e 10 00 01 | 00 1c | 00 00 | "
                     "04 01 00 18 | 14 01 00 01 | 29 86 c1 04 00 00 00 00 | "
                     "00 00 00 00 02 00 5e 10 00 01 00 00", NULL),
          "B's changes-only request answered with the full table, byte for byte tt §12 E8");
    CHECK(C(BAT_C_TT_REQ_RX) == 1 && C(BAT_C_TT_RESP_TX) == 1, "counters: tt_req_rx, tt_resp_tx");
    /* the answer lists committed and pending-delete entries, never uncommitted ones */
    bat_tt_local_add(B, M99, 0, 0);
    for (int i = 0; i < 5; i++) {
        build(o, 492);
    }
    bat_tt_local_del(B, M99, 0);
    bat_tt_local_add(B, C1, 0, 0);
    S.now += 600;   /* past the answer pacing (test_answer_pacing) */
    t0 = S.ntx;
    rx(f, n);
    size_t vlen;
    const uint8_t *v = last_utvlv(t0, &pkt, &vlen);
    int has99 = 0, hasc1 = 0;
    for (size_t off = 4 + 8; v && off + 12 <= vlen; off += 12) {
        has99 |= memcmp(v + off + 4, M99, 6) == 0;
        hasc1 |= memcmp(v + off + 4, C1, 6) == 0;
    }
    CHECK(v && v[0] == 0x14 && v[1] == 2 && vlen == 4 + 8 + 24 && has99 && !hasc1 &&
          bat_get32(v + 4) == 0x1871a4d8u,
          "full answer: current TTVN 2 and CRC, pending-delete entry included, uncommitted one not");
    /* an uncommitted entry in a new VLAN adds no VLAN record to the answer */
    bat_tt_local_add(B, C2, 0x8007, 0);
    S.now += 600;
    t0 = S.ntx;
    rx(f, n);
    v = last_utvlv(t0, NULL, &vlen);
    CHECK(v && bat_get16(v + 2) == 1 && vlen == 4 + 8 + 24, "full answer: a VLAN holding only uncommitted entries is not listed");
    /* a known originator without a default route gets no answer */
    struct bat_orig *nr = bat_orig_get(B, (const uint8_t[6]){ 0x02, 0x0d, 0, 0, 0, 1 });
    uint32_t rt = C(BAT_C_TT_RESP_TX);
    uint8_t h[256];
    memcpy(h, f, n);
    memcpy(h + 14 + 10, nr->addr, 6);
    t0 = S.ntx;
    rx(h, n);
    CHECK(count_type(t0, 0x44) == 0 && C(BAT_C_TT_RESP_TX) == rt && C(BAT_C_TT_REQ_RX) == 4,
          "request from a known originator we have no route to: not answered");
    /* requester we cannot route back to: dropped */
    uint8_t g[256];
    memcpy(g, f, n);
    memcpy(g + 14 + 10, (const uint8_t[6]){ 0x02, 0x0c, 0, 0, 0, 1 }, 6);
    t0 = S.ntx;
    rx(g, n);
    CHECK(count_type(t0, 0x44) == 0, "request from an unknown originator: not answered");
    /* a request for another originator is relayed, not answered (option A) */
    static const uint8_t CC[6] = { 0x02, 0x0c, 0x00, 0x00, 0x00, 0x01 };
    set_tput(CC, 1000);
    rx(g, mk_elp(g, CC, CC, 99));
    bat_neigh_find(B, CC)->tput_acc = 1000u << 10;
    rx(g, mk_ogm(g, CC, CC, 555, NULL, 0));
    memcpy(g, f, n);
    memcpy(g + 14 + 4, CC, 6);
    t0 = S.ntx;
    rx(g, n);
    CHECK(count_type(t0, 0x44) == 1 && last_utvlv(t0, &pkt, NULL) && memcmp(LU_DST, CC, 6) == 0 &&
          pkt[2] == 49 && C(BAT_C_UT_FWD) == 1,
          "request addressed to someone else: relayed toward it with TTL 49 (tt §6.3 option A)");
}

/* Answers are paced (hardening; Linux answers each request): one full table per requester per
 * 500 ms, so a UNICAST_TVLV packed with request containers, or a burst of request frames, draws
 * one answer, also when its source field names a third originator (reflection). */
static void test_answer_pacing(void)
{
    static const uint8_t VV[6] = { 0x02, 0x0e, 0x00, 0x00, 0x00, 0x01 };   /* two hops, via N1 */
    uint8_t f[1700], o[512], req[1600];
    fresh();
    build(o, 492);
    add_neigh(N1, N1, 1000);
    rx(f, mk_ogm(f, N1, N1, 1234, NULL, 0));
    rx(f, mk_ogm(f, N1, VV, 555, NULL, 0));
    CHECK(bat_route_nh(B, orig(N1), BAT_TBL_DEFAULT) && bat_route_nh(B, orig(VV), BAT_TBL_DEFAULT),
          "pacing setup: N1 and VV (behind N1) routed");
    size_t rl = 0;
    for (int k = 0; k < 195; k++, rl += 8) {
        uint8_t *c = req + rl;
        c[0] = 0x04;
        c[1] = 0x01;
        bat_put16(c + 2, 4);
        c[4] = 0x02;
        c[5] = 1;
        bat_put16(c + 6, 0);
    }
    unsigned t0 = S.ntx;
    uint32_t q0 = C(BAT_C_TT_REQ_RX), a0 = C(BAT_C_TT_RESP_TX), p0 = C(BAT_C_TT_REQ_PACED);
    size_t n = mk_utvlv(f, N1, W, N1, req, rl);
    rx(f, n);
    CHECK(n <= BAT_MAX_LINK_FRAME && count_type(t0, 0x44) == 1 && C(BAT_C_TT_REQ_RX) == q0 + 195 &&
          C(BAT_C_TT_RESP_TX) == a0 + 1 && C(BAT_C_TT_REQ_PACED) == p0 + 194,
          "one %zu-byte UNICAST_TVLV holding 195 request containers: one answer (%u sent), every request "
          "counted, 194 paced (tt_req_paced)", n, count_type(t0, 0x44));
    t0 = S.ntx;
    for (int k = 0; k < 5; k++) {
        rx(f, mk_utvlv(f, N1, W, N1, req, 8));
        S.now += 99;
    }
    CHECK(count_type(t0, 0x44) == 0, "5 single-request frames from N1 inside 500 ms of the answer: none answered");
    S.now += 5;
    rx(f, mk_utvlv(f, N1, W, N1, req, 8));
    CHECK(count_type(t0, 0x44) == 1, "500 ms after the last answer N1 is answered again");
    t0 = S.ntx;
    rx(f, mk_utvlv(f, N1, W, VV, req, rl));
    last_utvlv(t0, NULL, NULL);
    CHECK(count_type(t0, 0x44) == 1 && memcmp(LU_DST, N1, 6) == 0 && memcmp(LU_DST + 14 + 4, VV, 6) == 0,
          "195 requests naming VV as their source: one answer toward VV, paced apart from N1's");
    t0 = S.ntx;
    rx(f, mk_utvlv(f, N1, W, VV, req, rl));
    CHECK(count_type(t0, 0x44) == 0, "... and a second such packet at once: none (no reflection amplification)");
}

/* ---- temporary entries and resolution ---------------------------------------------- */

static size_t mk_bcast(uint8_t *f, const uint8_t *src, const uint8_t *orig_, uint32_t seq,
                       const uint8_t *isrc, uint16_t vlan)
{
    link_hdr(f, BC, src);
    uint8_t *p = f + 14;
    p[0] = 0x01;
    p[1] = 0x0f;
    p[2] = 49;
    p[3] = 0;
    bat_put32(p + 4, seq);
    memcpy(p + 8, orig_, 6);
    uint8_t *in = p + 14;
    memset(in, 0xff, 6);
    memcpy(in + 6, isrc, 6);
    size_t n = 12;
    if (vlan) {
        in[12] = 0x81;
        in[13] = 0x00;
        bat_put16(in + 14, vlan);
        n = 16;
    }
    in[n] = 0x08;
    in[n + 1] = 0x06;
    memset(in + n + 2, 0x11, 28);
    return 28 + n + 2 + 28;
}

static void test_temp(void)
{
    uint8_t tv[256], f[256];
    size_t tl;
    setup_x();
    struct vl v1[] = { { crc1(C1, 0, 0), 0 } };
    struct rec r1[] = { { 0, C1, 0 } };
    ogm_tt(N1, X, tv, tl = tt_tvlv(tv, 0x01, 1, v1, 1, r1, 1));
    struct bat_orig *o = orig(X);
    uint32_t t_create = S.now;
    rx(f, mk_bcast(f, N1, X, 10, C2, 0));
    const struct bat_tt_row *r = row(C2, 0, X);
    CHECK(r && (r->flags & BAT_TTR_TEMP) && r->ttvn == 1 && C(BAT_C_TT_TEMP_NEW) == 1 && S.ndl == 1,
          "BCAST delivered from X with inner source C2: temporary row via X, TTVN = S(X) (tt §8.3)");
    CHECK(bat_tt_resolve(B, C2, 0) == o, "temporary row usable for sending");
    unsigned t0 = S.ntx;
    ogm_tt(N1, X, tv, tl = tt_tvlv(tv, 0x01, 1, v1, 1, NULL, 0));
    unsigned n;
    CHECK(count_type(t0, 0x44) == 0 && bat_tt_orig_crc(B, bat_orig_index(B, o), 0, &n) == v1[0].crc,
          "temporary row excluded from X's CRC: no request");
    rx(f, mk_bcast(f, N1, X, 11, C3, 7));
    CHECK(row(C3, 0x8007, X) && (row(C3, 0x8007, X)->flags & BAT_TTR_TEMP), "VID from the inner 802.1Q tag");
    uint32_t tn = C(BAT_C_TT_TEMP_NEW);
    rx(f, mk_bcast(f, N1, X, 12, (const uint8_t[6]){ 0xba, 0xbe, 0x01, 0x02, 0x03, 0x04 }, 0));
    rx(f, mk_bcast(f, N1, X, 13, (const uint8_t[6]){ 0x01, 0x00, 0x5e, 0x01, 0x02, 0x03 }, 0));
    rx(f, mk_bcast(f, N1, X, 14, WS, 0));
    rx(f, mk_bcast(f, N1, X, 15, C1, 0));
    rx(f, mk_bcast(f, N1, X, 16, C2, 0));
    CHECK(C(BAT_C_TT_TEMP_NEW) == tn && S.ndl == 7,
          "no temporary row for a BA:BE source, a group source, our own client, or an existing (MAC, VID)");
    CHECK(!row((const uint8_t[6]){ 0xba, 0xbe, 0x01, 0x02, 0x03, 0x04 }, 0, NULL),
          "BA:BE loop-detect source never enters TT (dataplane §12.2)");
    /* a temporary row from another originator does not replace an announced one */
    rx(f, mk_elp(f, N2, N2, 5));
    ogm_tt(N2, Y, tv, tt_tvlv(tv, 0x01, 1, NULL, 0, NULL, 0));
    rx(f, mk_bcast(f, N2, Y, 20, C1, 0));
    CHECK(!row(C1, 0, Y) && row(C1, 0, X), "(MAC, VID) already announced by X: no temporary row via Y");
    /* the real announcement replaces the temporary row */
    struct vl v2[] = { { crc1(C1, 0, 0) ^ crc1(C2, 0, 0), 0 } };
    struct rec r2[] = { { 0, C2, 0 } };
    ogm_tt(N1, X, tv, tl = tt_tvlv(tv, 0x01, 2, v2, 1, r2, 1));
    r = row(C2, 0, X);
    CHECK(r && !(r->flags & BAT_TTR_TEMP) && r->ttvn == 2 && o->tt.ttvn == 2,
          "announcement of C2 replaces the temporary row (tt §8.1 step 4)");
    /* a temporary row of another originator is dropped by an announcement too */
    rx(f, mk_bcast(f, N2, Y, 21, C4, 0));
    CHECK(row(C4, 0, Y) && (row(C4, 0, Y)->flags & BAT_TTR_TEMP), "C4 learned temporarily via Y");
    struct vl v3[] = { { v2[0].crc ^ crc1(C4, 0, 0), 0 } };
    struct rec r3[] = { { 0, C4, 0 } };
    ogm_tt(N1, X, tv, tl = tt_tvlv(tv, 0x01, 3, v3, 1, r3, 1));
    CHECK(row(C4, 0, X) && !row(C4, 0, Y), "X announces C4: the temporary row via Y is dropped");
    /* expiry: 600 s after creation, checked by the 5 s purge; X and Y stay routed meanwhile */
    uint32_t es = 2000;
    struct vl vy[] = { { 0, 0 } };
    size_t tly;
    uint8_t tvy[64];
    tly = tt_tvlv(tvy, 0x01, 1, vy, 0, NULL, 0);
    tl = tt_tvlv(tv, 0x01, 3, v3, 1, NULL, 0);
    while ((int32_t)(t_create + 606000 - S.now) > 0) {
        S.now += 250;
        if (S.now - t_create >= 595000 && S.now - t_create < 595250) {
            CHECK(row(C3, 0x8007, X) != NULL, "temporary row still there at 595 s");
        }
        if (S.now % 1000 < 250) {
            rx(f, mk_elp(f, N1, N1, es));
            rx(f, mk_elp(f, N2, N2, es++));
            ogm_tt(N1, X, tv, tl);
            ogm_tt(N2, Y, tvy, tly);
        }
        bat_tick(B);
    }
    CHECK(orig(X) && bat_route_nh(B, orig(X), BAT_TBL_DEFAULT) && row(C4, 0, X), "X still routed at 606 s");
    CHECK(row(C3, 0x8007, X) == NULL, "temporary row gone by 606 s (600 s + one 5 s purge)");
}

/* tt §8.1 step 4: a temporary entry follows the latest originator that delivers the client's
 * frames and keeps its creation time, so it still expires 600 s after it was first learned
 * (batman-adv 2024.3 in the VM: re-pointed on every BCAST, gone 600 s after creation). */
static void test_temp_repoint(void)
{
    uint8_t f[256];
    setup_x();
    ogm_tt(N1, X, NULL, 0);
    ogm_tt(N2, Y, NULL, 0);
    uint32_t t_create = S.now, bx = 100, by = 100;
    rx(f, mk_bcast(f, N1, X, bx++, C1, 0));
    const struct bat_tt_row *r = row(C1, 0, X);
    CHECK(r && (r->flags & BAT_TTR_TEMP) && bat_tt_resolve(B, C1, 0) == orig(X),
          "repoint setup: C1 learned temporarily via X");
    S.now += 5000;
    rx(f, mk_bcast(f, N2, Y, by++, C1, 0));
    r = row(C1, 0, NULL);
    CHECK(row(C1, 0, Y) && !row(C1, 0, X) && bat_tt_resolve(B, C1, 0) == orig(Y) && bat_tt_rows_used(B) == 1 &&
          (r->flags & BAT_TTR_TEMP) && r->ts == t_create && r->ttvn == orig(Y)->tt.ttvn &&
          C(BAT_C_TT_TEMP_NEW) == 1,
          "a BCAST from Y with inner source C1: the temporary row moves to Y, creation time kept");
    rx(f, mk_bcast(f, N2, Y, by++, C1, 0));
    CHECK(row(C1, 0, Y) && row(C1, 0, NULL)->ts == t_create && bat_tt_rows_used(B) == 1,
          "another BCAST from Y: row unchanged");
    /* Y keeps sending C1's broadcasts; the row still ends 600 s after its creation via X */
    uint32_t es = 2000;
    bool at595 = false;
    while ((int32_t)(t_create + 606000 - S.now) > 0) {
        S.now += 250;
        if (S.now % 1000 < 250) {
            rx(f, mk_elp(f, N1, N1, es));
            rx(f, mk_elp(f, N2, N2, es++));
            ogm_tt(N1, X, NULL, 0);
            ogm_tt(N2, Y, NULL, 0);
        }
        if (S.now % 10000 < 250) {
            rx(f, mk_bcast(f, N2, Y, by++, C1, 0));
        }
        if (S.now - t_create >= 595000 && S.now - t_create < 595250) {
            at595 = row(C1, 0, Y) != NULL && bat_tt_resolve(B, C1, 0) == orig(Y);
        }
        bat_tick(B);
    }
    CHECK(at595, "C1 still resolves via Y at 595 s");
    CHECK(bat_route_nh(B, orig(Y), BAT_TBL_DEFAULT) && !row(C1, 0, NULL),
          "... and the row is gone by 606 s although Y kept delivering its frames (no refresh)");
}

/* Temporary rows (learned from data) hold at most a quarter of the table, and an announced
 * client displaces the oldest one when the table is full, so a flood of spoofed inner sources
 * never locks announced clients out (hardening; batman-adv has no fixed-size table). */
static void test_temp_cap(void)
{
    uint8_t tv[1700], f[1700];
    size_t tl;
    setup_x();
    ogm_tt(N1, X, NULL, 0);
    ogm_tt(N2, Y, NULL, 0);
    static uint8_t src[300][6];
    for (unsigned i = 0; i < 300; i++) {
        const uint8_t m[6] = { 0x06, 0x44, 0x00, 0x00, (uint8_t)(i >> 8), (uint8_t)i };
        memcpy(src[i], m, 6);
        rx(f, mk_bcast(f, N2, Y, 500 + i, src[i], 0));
        S.now += 10;
    }
    unsigned temp = 0;
    for (unsigned i = 0; i < BAT_TT_ROWS; i++) {
        temp += B->tt.rows[i].used && (B->tt.rows[i].flags & BAT_TTR_TEMP);
    }
    CHECK(temp == BAT_TT_ROWS / 4 && bat_tt_rows_used(B) == BAT_TT_ROWS / 4 && row(src[299], 0, Y) &&
          !row(src[0], 0, NULL) && C(BAT_C_TT_ROWS_FULL) == 0,
          "300 broadcasts from Y with distinct inner sources: %u temporary rows (cap %u), the newest kept",
          temp, (unsigned)(BAT_TT_ROWS / 4));
    /* X announces a client: installed at once, no request */
    struct vl v1[] = { { crc1(C1, 0, 0), 0 } };
    struct rec r1[] = { { 0, C1, 0 } };
    S.ntx = 0;
    unsigned t0 = S.ntx;
    ogm_tt(N1, X, tv, tt_tvlv(tv, 0x01, 1, v1, 1, r1, 1));
    CHECK(row(C1, 0, X) && orig(X)->tt.known && count_type(t0, 0x44) == 0 && bat_tt_resolve(B, C1, 0) == orig(X),
          "X's announced client installed next to the temporary rows, no request");
    /* announced rows fill the rest: each add past a full table displaces the oldest temporary row */
    static uint8_t macs[240][6];
    struct rec r[120];
    for (int part = 0; part < 2; part++) {
        struct vl v = { 0, (uint16_t)(0x8100 + part) };
        for (unsigned i = 0; i < 120; i++) {
            const uint8_t m[6] = { 0x06, 0x66, 0x00, 0x00, (uint8_t)part, (uint8_t)i };
            memcpy(macs[part * 120 + i], m, 6);
            r[i].flags = 0;
            r[i].mac = macs[part * 120 + i];
            r[i].vid = v.vid;
            v.crc ^= crc1(macs[part * 120 + i], v.vid, 0);
        }
        tl = tt_tvlv(tv, 0x14, 1, &v, 1, r, 120);
        rx(f, mk_utvlv(f, N1, W, part ? N1 : N2, tv, tl));
    }
    temp = 0;
    for (unsigned i = 0; i < BAT_TT_ROWS; i++) {
        temp += B->tt.rows[i].used && (B->tt.rows[i].flags & BAT_TTR_TEMP);
    }
    CHECK(bat_tt_rows_used(B) == BAT_TT_ROWS && temp == BAT_TT_ROWS / 4 - 49 && orig(N1)->tt.known &&
          orig(N2)->tt.known && C(BAT_C_TT_ROWS_FULL) == 0 && row(src[299], 0, Y) && row(C1, 0, X),
          "two 120-entry full tables past a full table: every entry stored, %u temporary rows left (oldest gone)",
          temp);
}

static void test_resolve(void)
{
    uint8_t tv[256];
    setup_x();
    set_tput(N2, 500);
    bat_neigh_find(B, N2)->tput_acc = 500u << 10;
    struct vl v1[] = { { crc1(C1, 0, 0), 0 } };
    struct rec r1[] = { { 0, C1, 0 } };
    ogm_tt(N1, X, tv, tt_tvlv(tv, 0x01, 1, v1, 1, r1, 1));
    ogm_tt(N2, Y, tv, tt_tvlv(tv, 0x01, 1, v1, 1, r1, 1));
    CHECK(row(C1, 0, X) && row(C1, 0, Y), "one client announced by two originators: two rows");
    CHECK(bat_tt_resolve(B, C1, 0) == orig(X), "resolve: the originator with the best route (1000 > 500)");
    CHECK(bat_tt_resolve(B, C1, 0x8000) == NULL && bat_tt_resolve(B, C2, 0) == NULL,
          "resolve: other VID or unknown MAC -> none (unknown unicast is dropped)");
    struct bat_orig *x = orig(X);
    for (unsigned c = 0; c < BAT_CANDS_PER_ORIG; c++) {
        x->cand[c].t[BAT_TBL_DEFAULT].valid = 0;
    }
    x->tbl[BAT_TBL_DEFAULT].router = -1;
    CHECK(bat_tt_resolve(B, C1, 0) == orig(Y), "resolve: originators without a route are skipped");
    struct vl v2[] = { { crc1(C1, 0, 0) ^ crc1(C2, 0, 0), 0 } };
    struct rec r2[] = { { 0, C2, 0 } };
    ogm_tt(N1, X, tv, tt_tvlv(tv, 0x01, 2, v2, 1, r2, 1));
    CHECK(row(C2, 0, X) && bat_tt_resolve(B, C2, 0) == orig(X), "setup: C2 announced by X only");
    x = orig(X);
    for (unsigned c = 0; c < BAT_CANDS_PER_ORIG; c++) {
        x->cand[c].t[BAT_TBL_DEFAULT].valid = 0;
    }
    x->tbl[BAT_TBL_DEFAULT].router = -1;
    CHECK(bat_tt_resolve(B, C2, 0) == NULL, "resolve: a client whose only originator lost its route -> none");
}

/* GW TVLV v1: download, upload (100 kbit/s units). */
static size_t gw_tvlv(uint8_t *o, uint32_t down, uint32_t up)
{
    o[0] = 0x01;
    o[1] = 0x01;
    bat_put16(o + 2, 8);
    bat_put32(o + 4, down);
    bat_put32(o + 8, up);
    return 12;
}

static bool zeroed(const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        if (b[i]) {
            return false;
        }
    }
    return true;
}

/* bat_client_route: what bat0 addressing asks about the lease's router (its bat0 or bridge MAC). */
static void test_client_route(void)
{
    uint8_t tv[256];
    struct bat_client_route r;
    setup_x();
    set_tput(N2, 500);
    bat_neigh_find(B, N2)->tput_acc = 500u << 10;
    memset(&r, 0xAA, sizeof(r));
    CHECK(!bat_client_route(B, C1, &r) && zeroed(&r, sizeof(r)), "client route: unknown MAC -> false, answer zeroed");
    struct vl v1[] = { { crc1(C1, 0, 0), 0 } };
    struct rec r1[] = { { 0, C1, 0 } };
    ogm_tt(N2, Y, tv, tt_tvlv(tv, 0x01, 1, v1, 1, r1, 1));
    const uint32_t t_y = S.now;
    CHECK(bat_client_route(B, C1, &r) && memcmp(r.orig, Y, 6) == 0 && r.tput == bat_route_tput(B, orig(Y)) &&
          r.tput > 0 && r.ogm_age_ms == 0, "client route: C1 announced by Y -> Y, its route throughput, OGM age 0");
    ogm_tt(N1, X, tv, tt_tvlv(tv, 0x01, 1, v1, 1, r1, 1));
    CHECK(bat_client_route(B, C1, &r) && memcmp(r.orig, X, 6) == 0 && r.tput == bat_route_tput(B, orig(X)),
          "client route: announced by X too -> X, the better route (as unicast to C1 goes)");
    CHECK(bat_client_route(B, C1, NULL), "client route: the answer may be skipped");
    S.now = t_y + 7000;
    CHECK(bat_client_route(B, C1, &r) && r.ogm_age_ms == 7000, "client route: OGM age counts from the last "
          "accepted OGM (%lu ms)", (unsigned long)r.ogm_age_ms);
    struct vl v2[] = { { crc1(C2, 0x8001, 0), 0x8001 } };
    struct rec r2[] = { { 0, C2, 0x8001 } };
    ogm_tt(N1, X, tv, tt_tvlv(tv, 0x01, 2, v2, 1, r2, 1));
    CHECK(row(C2, 0x8001, X) && !bat_client_route(B, C2, &r), "client route: only untagged rows count (C2 is on VLAN 1)");
    run_to(S.now + BAT_CAND_TIMEOUT_MS + 2000);
    CHECK(!bat_client_route(B, C1, &r) && zeroed(&r, sizeof(r)),
          "client route: both originators silent past the 200 s candidate timeout -> false");
}

/* bat_gw_best: the gateways peers announce, best by min(route throughput, download), and how many
 * there are (bat0 addressing's rise detector sees a second or replacing gateway by the count). */
static void test_gw_best(void)
{
    uint8_t tv[256];
    struct bat_gw g;
    setup_x();
    set_tput(N2, 500);
    bat_neigh_find(B, N2)->tput_acc = 500u << 10;
    memset(&g, 0xAA, sizeof(g));
    ogm_tt(N1, X, tv, 0);
    ogm_tt(N2, Y, tv, 0);
    CHECK(orig(X) && orig(Y) && bat_gw_best(B, &g) == 0 && zeroed(&g, sizeof(g)),
          "gateway: routes but no GW TVLV -> 0, answer zeroed");
    ogm_tt(N1, X, tv, gw_tvlv(tv, 100, 20));
    const uint32_t t_x = S.now;
    CHECK(bat_gw_best(B, &g) == 1 && memcmp(g.orig, X, 6) == 0 && g.down == 100 && g.up == 20 &&
          g.tput == bat_route_tput(B, orig(X)) && g.ogm_age_ms == 0, "gateway: X announces 10.0/2.0 Mbit/s -> 1, X");
    uint32_t xt = bat_route_tput(B, orig(X)), yt = bat_route_tput(B, orig(Y));
    CHECK(xt > yt && yt > 100, "setup: route to X %lu > route to Y %lu > X's 100 download", (unsigned long)xt,
          (unsigned long)yt);
    ogm_tt(N2, Y, tv, gw_tvlv(tv, 5000, 1000));
    unsigned n = bat_gw_best(B, &g);
    CHECK(n == 2 && memcmp(g.orig, Y, 6) == 0 && g.down == 5000 && g.tput == yt,
          "gateway: Y too -> 2 (%u); Y's min(route %lu, down 5000) beats X's min(route %lu, down 100)", n,
          (unsigned long)yt, (unsigned long)xt);
    CHECK(bat_gw_best(B, NULL) == 2, "gateway: the answer may be skipped, the count stays");
    ogm_tt(N2, Y, tv, gw_tvlv(tv, 50, 1000));
    CHECK(bat_gw_best(B, &g) == 2 && memcmp(g.orig, X, 6) == 0, "gateway: Y down to 50 -> X again (100 > 50), still 2");
    ogm_tt(N2, Y, tv, gw_tvlv(tv, 100, 1000));
    CHECK(bat_gw_best(B, &g) == 2 && memcmp(g.orig, X, 6) == 0, "gateway: equal metrics keep the first originator");
    ogm_tt(N1, X, tv, 0);
    CHECK(bat_gw_best(B, &g) == 1 && memcmp(g.orig, Y, 6) == 0, "gateway: X's next OGM without GW withdraws it -> 1, Y");
    S.now = t_x + 3000;
    CHECK(bat_gw_best(B, &g) == 1 && g.ogm_age_ms == S.now - orig(Y)->last_seen, "gateway: OGM age of the chosen one");
    ogm_tt(N1, X, tv, gw_tvlv(tv, 100, 20));
    CHECK(bat_gw_best(B, &g) == 2, "gateway: X announces again -> 2");
    struct bat_orig *y = orig(Y);
    for (unsigned c = 0; c < BAT_CANDS_PER_ORIG; c++) {
        y->cand[c].t[BAT_TBL_DEFAULT].valid = 0;
    }
    y->tbl[BAT_TBL_DEFAULT].router = -1;
    CHECK(bat_gw_best(B, &g) == 1 && memcmp(g.orig, X, 6) == 0, "gateway: Y loses its route -> 1, X (a gateway "
          "without a route is not counted)");
    struct bat_orig *x = orig(X);
    for (unsigned c = 0; c < BAT_CANDS_PER_ORIG; c++) {
        x->cand[c].t[BAT_TBL_DEFAULT].valid = 0;
    }
    x->tbl[BAT_TBL_DEFAULT].router = -1;
    memset(&g, 0xAA, sizeof(g));
    CHECK(bat_gw_best(B, &g) == 0 && zeroed(&g, sizeof(g)), "gateway: none with a route -> 0, answer zeroed");
    CHECK(bat_gw_best(B, NULL) == 0, "gateway: the answer may be skipped");
}

/* ---- full tables that cannot be taken: request backoff (hardening) ----------------- */

#define NCL 240
static uint8_t CL[NCL][6], CY[NCL][6];
static uint16_t FSEQ = 700;
static uint32_t REQ_AT[64];
static unsigned NREQ;

static void mk_clients(void)
{
    for (unsigned i = 0; i < NCL; i++) {
        const uint8_t m[6] = { 0x06, 0x55, 0x10, 0x00, (uint8_t)(i >> 8), (uint8_t)i };
        const uint8_t y[6] = { 0x06, 0x56, 0x10, 0x00, (uint8_t)(i >> 8), (uint8_t)i };
        memcpy(CL[i], m, 6);
        memcpy(CY[i], y, 6);
    }
}

static uint32_t crc_n(uint8_t (*cl)[6], unsigned n)
{
    uint32_t c = 0;
    for (unsigned i = 0; i < n; i++) {
        c ^= crc1(cl[i], 0, 0);
    }
    return c;
}

/* OGM of @o via @via at @ttvn: ADDs of clients [from, to), the VLAN CRC over the first @total. */
static void o_ogm(const uint8_t *via, const uint8_t *o, uint8_t (*cl)[6], uint8_t ttvn, unsigned from,
                  unsigned to, unsigned total)
{
    uint8_t tv[1600];
    struct rec r[64];
    struct vl v = { crc_n(cl, total), 0 };
    for (unsigned k = from; k < to; k++) {
        r[k - from].flags = 0;
        r[k - from].mac = cl[k];
        r[k - from].vid = 0;
    }
    ogm_tt(via, o, tv, tt_tvlv(tv, 0x01, ttvn, &v, 1, r, to - from));
}

/* Batman packet @pkt (@L bytes) to W from neighbour @via, cut like dataplane §7.2 (at most 1280 bytes a
 * piece, from the tail) by fragment originator @fo; only the pieces whose bit is set in @mask are sent
 * (bit k = fragment number k; the highest number is the head, which starts with the packet header). */
static void frag_to_w(const uint8_t *via, const uint8_t *fo, const uint8_t *pkt, size_t L, unsigned mask)
{
    uint8_t f[1700];
    size_t e = 1280 - 20, nf = (L + e - 1) / e, sz = (L + nf - 1) / nf;
    FSEQ++;
    for (size_t k = 0; k < nf; k++) {
        size_t hi = L - k * sz, lo = L > (k + 1) * sz ? L - (k + 1) * sz : 0;
        uint8_t *h = f + 14;
        if (!(mask >> k & 1)) {
            continue;
        }
        link_hdr(f, W, via);
        h[0] = 0x41;
        h[1] = 0x0f;
        h[2] = 50;
        h[3] = (uint8_t)(k << 4);
        memcpy(h + 4, W, 6);
        memcpy(h + 10, fo, 6);
        bat_put16(h + 16, FSEQ);
        bat_put16(h + 18, (uint16_t)L);
        memcpy(h + 20, pkt + lo, hi - lo);
        rx(f, 14 + 20 + (hi - lo));
    }
}

/* @o's full-table answer (its first @n clients) with source @o, cut by @fo: @o itself, or a relay that
 * answers on its behalf (tt §6.3; the fragment originator is the node that cut it, dataplane §2.4). */
static void o_full_by(const uint8_t *via, const uint8_t *fo, const uint8_t *o, uint8_t (*cl)[6], uint8_t ttvn,
                      unsigned n, unsigned mask)
{
    static uint8_t tv[4096], pkt[4096];
    static struct rec r[NCL];
    struct vl v = { crc_n(cl, n), 0 };
    for (unsigned k = 0; k < n; k++) {
        r[k].flags = 0;
        r[k].mac = cl[k];
        r[k].vid = 0;
    }
    size_t tl = tt_tvlv(tv, 0x14, ttvn, &v, 1, r, n);
    uint8_t f[1700];
    if (20 + tl <= 1500) {
        rx(f, mk_utvlv(f, via, W, o, tv, tl));
        return;
    }
    pkt[0] = 0x44;
    pkt[1] = 0x0f;
    pkt[2] = 50;
    pkt[3] = 0;
    memcpy(pkt + 4, W, 6);
    memcpy(pkt + 10, o, 6);
    bat_put16(pkt + 16, (uint16_t)tl);
    pkt[18] = pkt[19] = 0;
    memcpy(pkt + 20, tv, tl);
    frag_to_w(via, fo, pkt, 20 + tl, mask);
}

/* @o's own full-table answer (its first @n clients), every piece. */
static void o_full(const uint8_t *via, const uint8_t *o, uint8_t (*cl)[6], uint8_t ttvn, unsigned n)
{
    o_full_by(via, o, o, cl, ttvn, n, ~0u);
}

static void tick_1s(void)
{
    uint8_t f[64];
    run_to(S.now + 1000);
    S.ntx = 0;
    rx(f, mk_elp(f, N1, N1, 1000 + S.now / 1000));
    rx(f, mk_elp(f, N2, N2, 1000 + S.now / 1000));
}

/* X grows to @n clients by commits of 20, one an OGM interval, its OGMs arriving via @via; returns
 * its TTVN. */
static uint8_t grow_via(const uint8_t *via, unsigned n)
{
    uint8_t ttvn = 0;
    for (unsigned have = 0; have < n; have += 20) {
        tick_1s();
        o_ogm(via, X, CL, ++ttvn, have, have + 20, have + 20);
    }
    return ttvn;
}

static uint8_t grow_x(unsigned n)
{
    return grow_via(N1, n);
}

/* X announces @ttvn over its first @total clients every second for @secs; a new request is answered
 * at once with its first @answer_n clients (none when 0). Request times go to REQ_AT. */
static uint32_t steady(uint8_t ttvn, unsigned total, unsigned answer_n, unsigned secs)
{
    uint32_t r0 = C(BAT_C_TT_REQ_TX), seen = r0;
    for (unsigned sec = 0; sec < secs; sec++) {
        tick_1s();
        o_ogm(N1, X, CL, ttvn, 0, 0, total);
        if (C(BAT_C_TT_REQ_TX) != seen) {
            seen = C(BAT_C_TT_REQ_TX);
            if (NREQ < 64) {
                REQ_AT[NREQ++] = S.now;
            }
            if (answer_n) {
                o_full(N1, X, CL, ttvn, answer_n);
            }
        }
    }
    return C(BAT_C_TT_REQ_TX) - r0;
}

/* The gaps between the requests REQ_AT[from..] are exactly @want (ms), @n of them. */
static bool gaps(unsigned from, const uint32_t *want, unsigned n)
{
    if (NREQ < from + n + 1) {
        return false;
    }
    for (unsigned k = 0; k < n; k++) {
        if (REQ_AT[from + k + 1] - REQ_AT[from + k] != want[k]) {
            return false;
        }
    }
    return true;
}

static unsigned resolvable(uint8_t (*cl)[6], unsigned from, unsigned to)
{
    unsigned n = 0;
    for (unsigned i = from; i < to; i++) {
        n += bat_tt_resolve(B, cl[i], 0) != NULL;
    }
    return n;
}

static const uint32_t BACKOFF[] = { 6000, 12000, 24000, 48000, 60000, 60000 };

static void test_req_backoff(void)
{
    static const uint32_t PACE[] = { 3000, 3000 };
    mk_clients();
    /* A: in sync with X's 200 clients (a 2.4 KB full table) through its diffs, then one lost commit */
    setup_x();
    uint8_t s = grow_x(200);
    struct bat_orig *x = orig(X);
    CHECK(x && x->tt.known && rows_of(x) == 200 && C(BAT_C_TT_REQ_TX) == 0,
          "backoff: in sync with a 200-client X through its diffs, no request");
    tick_1s();                                           /* X's commit s + 1 lost on air */
    NREQ = 0;
    uint32_t reqs = steady((uint8_t)(s + 2), 202, 202, 120);
    CHECK(reqs == 5 && gaps(0, BACKOFF, 4) && C(BAT_C_FR_TOOBIG) > 0 && C(BAT_C_TT_REQ_STALL) == 5 &&
          x->tt.ttvn == s && resolvable(CL, 200, 202) == 0,
          "backoff: every answer over 2048 bytes (fr_toobig): 5 requests in 2 min, 6, 12, 24, 48 s apart "
          "(%u; tt_req_stall %u)", reqs, C(BAT_C_TT_REQ_STALL));
    reqs = steady((uint8_t)(s + 2), 202, 202, 180);
    CHECK(reqs == 3 && gaps(4, BACKOFF + 4, 2) && REQ_AT[7] - REQ_AT[6] == 60000,
          "backoff: then one request a minute (%u in 3 min)", reqs);
    /* X shrinks to 150 clients: the next answer fits, the table syncs, and the wait is 3 s again */
    reqs = steady((uint8_t)(s + 3), 150, 150, 61);
    CHECK(reqs == 1 && x->tt.known && x->tt.ttvn == s + 3 && rows_of(x) == 150 && resolvable(CL, 0, 150) == 150,
          "backoff: X down to 150 clients: the one request within the minute is answered whole, in sync");
    tick_1s();                                           /* s + 4 lost; the answers are lost too */
    NREQ = 0;
    uint32_t r0 = C(BAT_C_TT_REQ_TX);
    steady((uint8_t)(s + 5), 151, 0, 7);
    CHECK(C(BAT_C_TT_REQ_TX) - r0 == 3 && gaps(0, PACE, 2),
          "backoff: after the sync, a lost answer is asked for again every 3 s (%u requests)", C(BAT_C_TT_REQ_TX) - r0);

    /* A': the same lost commit with a table whose answer fits: no backoff */
    setup_x();
    s = grow_x(140);
    x = orig(X);
    tick_1s();
    reqs = steady((uint8_t)(s + 2), 142, 142, 60);
    CHECK(reqs == 1 && C(BAT_C_TT_REQ_STALL) == 0 && x->tt.known && x->tt.ttvn == s + 2 &&
          resolvable(CL, 140, 142) == 2, "backoff: a 140-client X resyncs with one request, no backoff");

    /* B: X times out, then comes back with its 200-client table unchanged */
    setup_x();
    s = grow_x(200);
    for (unsigned i = 0; i < BAT_ORIG_TIMEOUT_MS / 1000 + 5; i++) {
        tick_1s();
    }
    CHECK(orig(X) == NULL, "backoff: X purged after %u s without its OGMs", BAT_ORIG_TIMEOUT_MS / 1000);
    NREQ = 0;
    reqs = steady(s, 200, 200, 120);
    x = orig(X);
    CHECK(x && !x->tt.known && reqs == 5 && gaps(0, BACKOFF, 4) && resolvable(CL, 0, 200) == 0,
          "backoff: a relearned X with an unchanged 200-client table: 5 requests in 2 min, backing off (%u)", reqs);

    /* C: Y's 120 rows and X's 140 are more than 256: every answer arrives and does not fit */
    setup_x();
    tick_1s();
    o_ogm(N2, Y, CY, 1, 0, 0, 120);
    struct vl vy = { crc_n(CY, 120), 0 };
    static struct rec ry[NCL];
    for (unsigned k = 0; k < 120; k++) {
        ry[k].flags = 0;
        ry[k].mac = CY[k];
        ry[k].vid = 0;
    }
    static uint8_t tv[2048];
    size_t tl = tt_tvlv(tv, 0x14, 1, &vy, 1, ry, 120);
    uint8_t f[1700];
    rx(f, mk_utvlv(f, N2, W, Y, tv, tl));
    struct bat_orig *y = orig(Y);
    CHECK(y && y->tt.known && rows_of(y) == 120, "backoff: Y's 120 rows stored (%u)", y ? rows_of(y) : 0);
    s = grow_x(140);
    x = orig(X);
    uint32_t full0 = C(BAT_C_TT_FULL);
    NREQ = 0;
    reqs = steady(s, 140, 140, 120);
    CHECK(!x->tt.known && rows_of(x) == BAT_TT_ROWS - 120 && reqs == 5 && gaps(0, BACKOFF, 4) &&
          C(BAT_C_TT_FULL) - full0 == 5 && C(BAT_C_TT_REQ_STALL) == 5,
          "backoff: 120 + 140 rows > %u: each answer stored as far as it fits, 5 requests in 2 min (%u)",
          BAT_TT_ROWS, reqs);
    for (unsigned c = 0; c < BAT_CANDS_PER_ORIG; c++) {
        for (unsigned t = 0; t < BAT_NTBL; t++) {
            y->cand[c].t[t].valid = 0;
        }
    }
    bat_orig_recompute(B, y);
    CHECK(rows_of(y) == 0, "backoff: Y loses its route, its rows go");
    reqs = steady(s, 140, 140, 61);
    CHECK(reqs == 1 && x->tt.known && rows_of(x) == 140 && resolvable(CL, 0, 140) == 140,
          "backoff: X's next request, within the minute, is answered and fits: in sync");
    tick_1s();
    NREQ = 0;
    r0 = C(BAT_C_TT_REQ_TX);
    steady((uint8_t)(s + 2), 141, 0, 7);
    CHECK(C(BAT_C_TT_REQ_TX) - r0 == 3 && gaps(0, PACE, 2), "backoff: ... and asked every 3 s again (%u)",
          C(BAT_C_TT_REQ_TX) - r0);
}

/* ---- answers cut by a relay on the owner's behalf (tt §6.3) --------------------------------- */

static const uint8_t NC[2][6] = { { 0x06, 0x11, 0x00, 0x00, 0x00, 0x01 }, { 0x06, 0x11, 0x00, 0x00, 0x00, 0x02 } };

/* TT requests W sent to @dst among the frames recorded since the last tick_1s. */
static unsigned reqs_to(const uint8_t *dst)
{
    unsigned n = 0;
    for (unsigned k = 0; k < S.ntx; k++) {
        const uint8_t *p = S.tx[k].b + 14;
        n += S.tx[k].len >= 14 + 25 && p[0] == 0x44 && memcmp(p + 4, dst, 6) == 0 && p[20] == 0x04 &&
             (p[24] & 0x0F) == 0x02;
    }
    return n;
}

/* N1's own OGM: its 2-client table at TTVN 4, no change records. */
static void n1_ogm(void)
{
    uint8_t tv[64];
    struct vl v = { crc_n((uint8_t (*)[6])NC, 2), 0 };
    ogm_tt(N1, N1, tv, tt_tvlv(tv, 0x01, 4, &v, 1, NULL, 0));
}

/* W asks N1 for its own table and that answer is lost (N1 had no route back yet, tt §5.1); for 2 min
 * N1 answers W's later requests, and X (200 clients, 2.4 KB) answers too, cut by @fo. Request times
 * for X go to REQ_AT; returns when N1 was asked again (ms after the lost request, 0 = never). */
static uint32_t onbehalf_run(const uint8_t *fo)
{
    setup_x();
    tick_1s();
    n1_ogm();
    uint32_t t0 = S.now, again = 0;
    struct bat_orig *n1 = orig(N1);
    CHECK(n1 && n1->tt.req_pending && reqs_to(N1) == 1, "on behalf: W asks N1 for its own table (the answer is lost)");
    NREQ = 0;
    for (unsigned sec = 0; sec < 120; sec++) {
        tick_1s();
        n1_ogm();
        if (reqs_to(N1)) {
            again = again ? again : S.now - t0;
            o_full(N1, N1, (uint8_t (*)[6])NC, 4, 2);
        }
        o_ogm(N1, X, CL, 9, 0, 0, 200);
        if (reqs_to(X)) {
            if (NREQ < 64) {
                REQ_AT[NREQ++] = S.now;
            }
            o_full_by(N1, fo, X, CL, 9, 200, ~0u);
        }
    }
    return again;
}

/* A batman-adv relay holding X's table in sync answers W's full-table request for X itself, with source
 * X, and cuts the answer itself, so the fragment originator is the relay (dataplane §2.4, §7.2 step 5).
 * An answer too big to reassemble is charged to X, named in the head piece, never to the relay. */
static void test_req_backoff_onbehalf(void)
{
    mk_clients();
    uint32_t again = onbehalf_run(N1);
    struct bat_orig *x = orig(X), *n1 = orig(N1);
    unsigned st = C(BAT_C_TT_REQ_STALL);
    CHECK(x && NREQ == 5 && gaps(0, BACKOFF, 4) && st == 5 && x->tt.backoff > 0 && n1->tt.backoff == 0,
          "on behalf: X's 2.4 KB answers cut by N1: X's requests back off, 5 in 2 min 6, 12, 24, 48 s apart "
          "(%u; tt_req_stall +%u)", NREQ, st);
    CHECK(again == 1000 && n1->tt.known && !n1->tt.req_pending && resolvable((uint8_t (*)[6])NC, 0, 2) == 2,
          "on behalf: N1, which cut them, is asked again at its next OGM, 1 s after its lost answer, and its "
          "table syncs (asked again at +%u ms)", again);
    again = onbehalf_run(X);
    x = orig(X);
    n1 = orig(N1);
    CHECK(NREQ == 5 && gaps(0, BACKOFF, 4) && again == 1000 && n1->tt.known,
          "on behalf: the same with X cutting its own answers: X backs off, N1 asked again at +%u ms", again);

    /* only the head piece names the answer's source */
    setup_x();
    tick_1s();
    o_ogm(N1, X, CL, 9, 0, 0, 200);
    tick_1s();
    o_ogm(N1, X, CL, 9, 0, 0, 200);
    x = orig(X);
    uint32_t st0 = C(BAT_C_TT_REQ_STALL);
    uint32_t tb0 = C(BAT_C_FR_TOOBIG);
    o_full_by(N1, X, X, CL, 9, 200, 1u);
    CHECK(x && x->tt.req_pending && C(BAT_C_FR_TOOBIG) - tb0 == 1 && C(BAT_C_TT_REQ_STALL) == st0 &&
          x->tt.backoff == 0, "on behalf: a tail piece of X's oversized answer alone: fr_toobig, no stall");
    /* oversized packets from X that are no TT answer to W, their bytes 20, 21 and 24 as a TT answer's: a
     * UNICAST_4ADDR, a UNICAST_TVLV whose container is not TT, one carrying a TT request, and a TT answer
     * whose inner destination is N2 (type, container type, TT flags, inner destination) */
    static uint8_t big[2400];
    static const struct { uint8_t type, cont, flags; const uint8_t *dst; } kind[] = {
        { 0x42, 0x04, 0x14, W }, { 0x44, 0x7f, 0x14, W }, { 0x44, 0x04, 0x12, W }, { 0x44, 0x04, 0x14, N2 } };
    bool none = true;
    for (unsigned k = 0; k < 4; k++) {
        memset(big, 0x5a, sizeof(big));
        big[0] = kind[k].type;
        big[1] = 0x0f;
        big[2] = 50;
        memcpy(big + 4, kind[k].dst, 6);
        memcpy(big + 10, X, 6);
        bat_put16(big + 16, (uint16_t)(sizeof(big) - 20));
        big[20] = kind[k].cont;
        big[21] = 1;
        bat_put16(big + 22, (uint16_t)(sizeof(big) - 24));
        big[24] = kind[k].flags;
        frag_to_w(N1, X, big, sizeof(big), ~0u);
        none = none && C(BAT_C_TT_REQ_STALL) == st0 && x->tt.backoff == 0;
    }
    CHECK(none && C(BAT_C_FR_TOOBIG) - tb0 == 1 + 4 * 2, "on behalf: an oversized UNICAST_4ADDR, non-TT "
          "UNICAST_TVLV, TT request, or TT answer to another node from X, all pieces: fr_toobig, no stall");
    o_full_by(N1, X, X, CL, 9, 200, ~0u);
    CHECK(C(BAT_C_TT_REQ_STALL) - st0 == 1 && x->tt.backoff == 1,
          "on behalf: then X's whole oversized answer: one stall, the next request in 6 s (tt_req_stall +%u)",
          C(BAT_C_TT_REQ_STALL) - st0);
}

/* Join race (on air vs 2025.4, VM vs 2024.3): X's OGM reaches W before W's own first OGM reaches X, so
 * X cannot route back and drops W's request (tt §6.2). Until X's table is first held, W asks again at
 * X's next OGM (as Linux does, tt §6.1), never faster than 500 ms; once held, the 3 s guard applies. */
static void test_join_race(void)
{
    uint8_t tv[256];
    struct vl v1[] = { { crc1(C1, 0, 0), 0 } };
    setup_x();
    size_t tl = tt_tvlv(tv, 0x01, 2, v1, 1, NULL, 0);
    unsigned t0 = S.ntx;
    ogm_tt(N1, X, tv, tl);
    CHECK(count_type(t0, 0x44) == 1, "join race: X's first OGM (TTVN 2, unknown table): W asks for it");
    S.now += 300;
    ogm_tt(N1, X, tv, tl);
    CHECK(count_type(t0, 0x44) == 1, "join race: an OGM 300 ms later draws no second request (500 ms floor)");
    S.now += 700;
    ogm_tt(N1, X, tv, tl);
    CHECK(count_type(t0, 0x44) == 2,
          "join race: the answer never came: X's next OGM, 1 s after the request, draws another (%u)",
          count_type(t0, 0x44));
    uint8_t f[1700];
    struct rec r1[] = { { 0, C1, 0 } };
    tl = tt_tvlv(tv, 0x14, 2, v1, 1, r1, 1);
    rx(f, mk_utvlv(f, N1, W, X, tv, tl));
    struct bat_orig *o = orig(X);
    CHECK(o && o->tt.known && !o->tt.req_pending, "join race: X's answer is applied, table held");
    struct vl vbad[] = { { 0xdeadbeef, 0 } };
    tl = tt_tvlv(tv, 0x01, 2, vbad, 1, NULL, 0);
    t0 = S.ntx;
    S.now += 1000;
    ogm_tt(N1, X, tv, tl);
    S.now += 1000;
    ogm_tt(N1, X, tv, tl);
    S.now += 1000;
    ogm_tt(N1, X, tv, tl);
    unsigned in3 = count_type(t0, 0x44);
    S.now += 1000;
    ogm_tt(N1, X, tv, tl);
    CHECK(in3 == 1 && count_type(t0, 0x44) == 2,
          "join race: once held, a CRC mismatch is asked at once, then 3 s later, not at every OGM (%u, %u)",
          in3, count_type(t0, 0x44));
}

/* ---- TTVN in unicast headers (tt §9.1 step 5, dataplane §6.2) ----------------------------- */

/* A client frame from W's soft interface to @dst; the TTVN byte of the UNICAST it made toward @o,
 * -1 if none. */
static int uc_ttvn(const uint8_t *dst, const uint8_t *o)
{
    uint8_t in[64];
    memcpy(in, dst, 6);
    memcpy(in + 6, WS, 6);
    in[12] = 0x08;
    in[13] = 0x00;
    memset(in + 14, 0x5a, sizeof(in) - 14);
    S.ntx = 0;
    bat_tx_soft(B, in, sizeof(in));
    for (unsigned k = S.ntx; k-- > 0;) {
        const uint8_t *p = S.tx[k].b + 14;
        if (p[0] == 0x40 && memcmp(p + 4, o, 6) == 0) {
            return p[3];
        }
    }
    return -1;
}

/* A batman-adv relay whose view of the destination's TTVN is @view drops a unicast with an older one
 * when TT still names the same originator (dataplane §6.2 step 7). */
static bool relay_passes(int ttvn, uint8_t view)
{
    return ttvn >= 0 && !bat_ttvn_older((uint8_t)ttvn, view);
}

/* Unicast headers carry W's synced TTVN for the destination, as batman-adv's do. While that table cannot
 * be taken (an answer too big or not fitting, or never synced) the synced TTVN stays behind the one X
 * announces and every batman-adv relay would drop the packet, the rows W still holds included; the
 * header then carries X's announced TTVN, unless X is the next hop (X re-resolves stale packets itself). */
static void test_uc_ttvn(void)
{
    mk_clients();
    setup_x();
    uint8_t s = grow_x(200);
    int t = uc_ttvn(CL[0], X);
    CHECK(t == s && relay_passes(t, s), "uc ttvn: X in sync (TTVN %u): the header carries it (%d)", s, t);
    tick_1s();                                           /* X's commit s + 1 lost on air */
    o_ogm(N1, X, CL, (uint8_t)(s + 2), 0, 0, 202);
    struct bat_orig *x = orig(X);
    t = uc_ttvn(CL[0], X);
    CHECK(x->tt.req_pending && t == s, "uc ttvn: a missed change set, the answer awaited: the synced TTVN, "
          "as batman-adv sends (%d)", t);
    o_full(N1, X, CL, (uint8_t)(s + 2), 202);
    t = uc_ttvn(CL[0], X);
    CHECK(x->tt.ttvn == s && x->tt.backoff == 1 && row(CL[0], 0, X) && t == s + 2 && relay_passes(t, s + 2),
          "uc ttvn: the answer over 2048 bytes: to a row W still holds, X's announced TTVN %u (%d), so a "
          "batman-adv relay passes it", s + 2, t);
    for (unsigned i = 0; i < 300; i++) {
        tick_1s();
        o_ogm(N1, X, CL, (uint8_t)(s + 4), 0, 0, 204);
    }
    t = uc_ttvn(CL[0], X);
    CHECK(x->tt.ttvn == s && t == s + 4, "uc ttvn: 5 min later X at %u, still stuck at %u: the header follows "
          "X's OGMs (%d)", s + 4, s, t);
    /* W re-routes a stale packet addressed to it for X's client: the rewritten header is X's announced TTVN */
    uint8_t f[128];
    link_hdr(f, W, N2);
    uint8_t *p = f + 14;
    p[0] = 0x40;
    p[1] = 0x0f;
    p[2] = 50;
    p[3] = (uint8_t)(bat_tt_own_ttvn(B) - 1);
    memcpy(p + 4, W, 6);
    memcpy(p + 10, CL[0], 6);
    memcpy(p + 16, N2, 6);
    p[22] = 0x08;
    p[23] = 0x00;
    memset(p + 24, 0x33, 46);
    S.ntx = 0;
    uint32_t rr = C(BAT_C_UC_REROUTE);
    rx(f, 14 + 70);
    CHECK(C(BAT_C_UC_REROUTE) - rr == 1 && S.ntx == 1 && S.tx[0].b[14] == 0x40 && memcmp(S.tx[0].b + 18, X, 6) == 0 &&
          S.tx[0].b[17] == (uint8_t)(s + 4), "uc ttvn: a stale packet for X's client re-routed by W carries X's "
          "announced TTVN too (%u)", S.ntx ? S.tx[0].b[17] : 0);
    /* X shrinks to 150 clients: the answer fits, the table syncs, the synced TTVN again */
    for (unsigned i = 0; i < 61 && x->tt.ttvn != (uint8_t)(s + 5); i++) {
        tick_1s();
        o_ogm(N1, X, CL, (uint8_t)(s + 5), 0, 0, 150);
        if (reqs_to(X)) {
            o_full(N1, X, CL, (uint8_t)(s + 5), 150);
        }
    }
    tick_1s();
    o_ogm(N1, X, CL, (uint8_t)(s + 5), 0, 0, 150);
    t = uc_ttvn(CL[0], X);
    CHECK(x->tt.known && x->tt.ttvn == s + 5 && x->tt.backoff == 0 && t == s + 5,
          "uc ttvn: X down to 150 clients, in sync again: its synced TTVN (%d)", t);

    /* X is W's neighbour and its own next hop: X re-resolves a stale packet itself (dataplane §6.2 step 6) */
    setup_x();
    add_neigh(X, X, 2000);
    s = grow_via(X, 200);
    x = orig(X);
    tick_1s();
    o_ogm(X, X, CL, (uint8_t)(s + 2), 0, 0, 202);
    o_full(X, X, CL, (uint8_t)(s + 2), 202);
    t = uc_ttvn(CL[0], X);
    CHECK(x && bat_mac_eq(bat_route_nh(B, x, BAT_TBL_DEFAULT), X) && x->tt.backoff == 1 && t == s,
          "uc ttvn: the same with X the next hop: the synced TTVN %u (%d)", s, t);
    /* ... also when W hears X on a second hard interface of X's (link address not X's originator address) */
    static const uint8_t XL[6] = { 0x02, 0x55, 0x00, 0x00, 0x00, 0x02 };
    setup_x();
    add_neigh(XL, X, 2000);
    s = grow_via(XL, 200);
    x = orig(X);
    tick_1s();
    o_ogm(XL, X, CL, (uint8_t)(s + 2), 0, 0, 202);
    o_full(XL, X, CL, (uint8_t)(s + 2), 202);
    t = uc_ttvn(CL[0], X);
    CHECK(x && bat_mac_eq(bat_route_nh(B, x, BAT_TBL_DEFAULT), XL) && x->tt.backoff == 1 && t == s,
          "uc ttvn: ... and with X the next hop through its second interface: the synced TTVN %u (%d)", s, t);

    /* never synced: a temporary row learned from X's broadcast */
    setup_x();
    tick_1s();
    o_ogm(N1, X, CL, 9, 0, 0, 200);
    rx(f, mk_bcast(f, N1, X, 40, CL[5], 0));
    x = orig(X);
    t = uc_ttvn(CL[5], X);
    CHECK(x && !x->tt.known && row(CL[5], 0, X) && (row(CL[5], 0, X)->flags & BAT_TTR_TEMP) && t == 9 &&
          relay_passes(t, 9), "uc ttvn: X never synced: to a temporary row, X's announced TTVN 9, not 0 (%d)", t);
}

static void test_render(void)
{
    uint8_t tv[256], o[512];
    char buf[BAT_RENDER_BUF];
    setup_x();
    build(o, 492);
    struct vl v1[] = { { crc1(C1, 0, 0), 0 }, { crc1(C2, 0x8005, 0x10), 0x8005 } };
    struct rec r1[] = { { 0, C1, 0 }, { 0x10, C2, 0x8005 } };
    ogm_tt(N1, X, tv, tt_tvlv(tv, 0x01, 1, v1, 2, r1, 2));
    size_t n = bat_render(B, BAT_RENDER_TT_GLOBAL, buf, sizeof(buf));
    char want[1024];
    snprintf(want, sizeof(want),
             "+BATTG: rows=2/%u\r\n"
             "+BATTG: crc via=02:55:00:00:00:01 vid=-1 crc=0x%08x entries=1\r\n"
             "+BATTG: crc via=02:55:00:00:00:01 vid=5 crc=0x%08x entries=1\r\n"
             "+BATTG: 06:55:00:00:00:01 vid=-1 via=02:55:00:00:00:01 ttvn=1 flags=---\r\n"
             "+BATTG: 06:55:00:00:00:02 vid=5 via=02:55:00:00:00:01 ttvn=1 flags=W--\r\n",
             (unsigned)BAT_TT_ROWS, v1[0].crc, v1[1].crc);
    CHECK(n == strlen(want) && strcmp(buf, want) == 0, "render TT global (5.3 format)");
    if (strcmp(buf, want) != 0) {
        printf("%s---\n%s", buf, want);
    }
    n = bat_render(B, BAT_RENDER_TT_LOCAL, buf, sizeof(buf));
    snprintf(want, sizeof(want),
             "+BATTL: ttvn=1 changes=0 resend=3\r\n"
             "+BATTL: 06:77:00:00:00:01 vid=-1 flags=-- crc=0x%08x\r\n", crc1(WS, 0, 0));
    CHECK(n == strlen(want) && strcmp(buf, want) == 0, "render TT local (5.3 format)");
    if (strcmp(buf, want) != 0) {
        printf("%s---\n%s", buf, want);
    }
    /* odd VID and truncation */
    struct vl vr[] = { { crc1(C3, 0x1234, 0), 0x1234 } };
    struct rec rr[] = { { 0, C3, 0x1234 } };
    ogm_tt(N2, Y, tv, tt_tvlv(tv, 0x01, 1, vr, 1, rr, 1));
    add_neigh(N2, N2, 1000);
    ogm_tt(N2, Y, tv, tt_tvlv(tv, 0x01, 1, vr, 1, rr, 1));
    bat_render(B, BAT_RENDER_TT_GLOBAL, buf, sizeof(buf));
    CHECK(strstr(buf, "vid=raw:0x1234") != NULL, "render: out-of-spec VID shown raw");
    uint8_t bf[128];
    memcpy(bf, BC, 6);
    memcpy(bf + 6, N1, 6);
    bf[12] = 0x43;
    bf[13] = 0x05;
    memset(bf + 14, 0, 70);
    bf[14] = 0x01;
    bf[15] = 0x0f;
    bf[16] = 49;
    bat_put32(bf + 18, 77);
    memcpy(bf + 22, X, 6);
    memset(bf + 28, 0xff, 6);
    memcpy(bf + 34, C4, 6);
    bf[40] = 0x08;
    rx(bf, 90);
    bat_render(B, BAT_RENDER_TT_GLOBAL, buf, sizeof(buf));
    CHECK(strstr(buf, "+BATTG: 06:55:00:00:00:04 vid=-1 via=02:55:00:00:00:01 ttvn=1 flags=--T\r\n") != NULL,
          "render: a temporary row is flagged T");
    n = bat_render(B, BAT_RENDER_TT_GLOBAL, buf, 120);
    CHECK(n < 120 && strncmp(buf, "+BATTG: rows=4/", 15) == 0 && strstr(buf, "+BATTG: (truncated)\r\n"),
          "render: a short buffer keeps the summary and ends with the truncation marker");
    n = bat_render(B, BAT_RENDER_TT_LOCAL, buf, 40);
    CHECK(n < 40 && buf[n] == '\0', "render local: tiny buffer stays terminated");
}

/* ---- paged TT render: a full 256-row table through the 4 KiB port buffer ------------- */

static char PG[131072];             /* the concatenated chunks */
static unsigned PG_BAD;             /* chunks not NUL-terminated inside len or not ending "\r\n" */

static unsigned count_lines(const char *s, const char *prefix)
{
    unsigned n = 0;
    size_t pl = strlen(prefix);
    for (const char *l = s; *l;) {
        n += strncmp(l, prefix, pl) == 0;
        const char *e = strstr(l, "\r\n");
        if (!e) {
            break;
        }
        l = e + 2;
    }
    return n;
}

static unsigned pages(enum bat_render_kind k, const uint8_t *mac, size_t len)
{
    static char chunk[BAT_RENDER_BUF];
    uint32_t cur = 0;
    size_t used = 0;
    unsigned n = 0;
    PG[0] = '\0';
    PG_BAD = 0;
    while (cur != BAT_RENDER_DONE && n < 2000) {
        memset(chunk, 0x5a, sizeof(chunk));
        size_t w = bat_render_from(B, k, mac, &cur, chunk, len);
        n++;
        if (w >= len || chunk[w] != '\0' || strlen(chunk) != w || (w && strcmp(chunk + w - 2, "\r\n") != 0) ||
            used + w >= sizeof(PG)) {
            PG_BAD++;
            continue;
        }
        memcpy(PG + used, chunk, w + 1);
        used += w;
    }
    return cur == BAT_RENDER_DONE ? n : 0;
}

static size_t longest_line(const char *s)
{
    size_t best = 0;
    for (const char *l = s; *l;) {
        const char *e = strstr(l, "\r\n");
        size_t ll = e ? (size_t)(e + 2 - l) : strlen(l);
        best = ll > best ? ll : best;
        l += ll;
    }
    return best;
}

/* Originator k: N1 for k = 0, else 02:60:00:00:00:k; its 8 clients 06:60:00:00:k:i. */
static void pg_orig(uint8_t o[6], unsigned k)
{
    const uint8_t m[6] = { 0x02, 0x60, 0x00, 0x00, 0x00, (uint8_t)k };
    memcpy(o, k ? m : N1, 6);
}
static void pg_client(uint8_t c[6], unsigned k, unsigned i)
{
    const uint8_t m[6] = { 0x06, 0x60, 0x00, 0x00, (uint8_t)k, (uint8_t)i };
    memcpy(c, m, 6);
}

/* 32 wizard-like originators: bat0 MAC (client 0) untagged and on VLAN 1, 3 more clients in
 * each; 8 rows each fill all 256 rows. Everything arrives as OGM TT diffs through bat_rx_hard. */
static void mesh256(void)
{
    uint8_t tv[512], o[6];
    static uint8_t cl[BAT_MAX_ORIG][4][6];
    fresh();
    add_neigh(N1, N1, 1000);
    for (unsigned k = 0; k < BAT_MAX_ORIG; k++) {
        struct vl v[2] = { { 0, 0 }, { 0, 0x8001 } };
        struct rec r[8];
        for (unsigned i = 0; i < 4; i++) {
            pg_client(cl[k][i], k, i);
            for (unsigned t = 0; t < 2; t++) {
                r[t * 4 + i] = (struct rec){ 0, cl[k][i], v[t].vid };
                v[t].crc ^= crc1(cl[k][i], v[t].vid, 0);
            }
        }
        pg_orig(o, k);
        ogm_tt(N1, o, tv, tt_tvlv(tv, 0x01, 1, v, 2, r, 8));
    }
}

static void test_render_pages(void)
{
    static char full[131072];
    uint8_t o[6], c[6];
    mesh256();
    unsigned origs = 0, known = 0;
    for (unsigned i = 0; i < BAT_MAX_ORIG; i++) {
        origs += B->orig[i].used;
        known += B->orig[i].used && B->orig[i].tt.known;
    }
    CHECK(origs == 32 && known == 32 && bat_tt_rows_used(B) == BAT_TT_ROWS && C(BAT_C_TT_ROWS_FULL) == 0,
          "pages setup: 32 originators x 8 rows = all %u rows, learned from OGM diffs (%u known)",
          (unsigned)BAT_TT_ROWS, known);
    size_t fl = bat_render(B, BAT_RENDER_TT_GLOBAL, full, sizeof(full));
    CHECK(count_lines(full, "+BATTG: crc ") == 64 && count_lines(full, "+BATTG: 06:60:") == 256 &&
          fl > 3 * BAT_RENDER_BUF, "pages: the whole TT listing is %zu bytes, several port buffers", fl);

    /* one originator's CRC lines, then its rows; each originator one block */
    int grouped = 1;
    unsigned blocks = 0;
    char cur_via[18] = "", seen[BAT_MAX_ORIG][18];
    bool in_rows = false;
    for (const char *l = full; *l && grouped;) {
        const char *e = strstr(l, "\r\n");
        if (!e) {
            break;
        }
        const char *via = strstr(l, "via=");
        bool crc = strncmp(l, "+BATTG: crc ", 12) == 0, row = strncmp(l, "+BATTG: 06:", 11) == 0;
        if ((crc || row) && via && via < e) {
            if (strncmp(via + 4, cur_via, 17) != 0) {
                grouped &= crc && blocks < BAT_MAX_ORIG;
                for (unsigned b = 0; b < blocks && grouped; b++) {
                    grouped &= strncmp(seen[b], via + 4, 17) != 0;
                }
                if (grouped) {
                    memcpy(cur_via, via + 4, 17);
                    cur_via[17] = '\0';
                    memcpy(seen[blocks++], cur_via, 18);
                }
                in_rows = false;
            } else {
                grouped &= !(crc && in_rows);
                in_rows |= row;
            }
        }
        l = e + 2;
    }
    CHECK(grouped && blocks == 32,
          "render TT global: each originator's CRC lines come right before its rows, one block each (%u)", blocks);

    unsigned n = pages(BAT_RENDER_TT_GLOBAL, NULL, BAT_RENDER_BUF);
    CHECK(n >= 4 && PG_BAD == 0 && count_lines(PG, "+BATTG: rows=") == 1 && strncmp(PG, "+BATTG: rows=256/", 17) == 0 &&
          count_lines(PG, "+BATTG: crc ") == 64 && count_lines(PG, "+BATTG: 06:60:") == 256 &&
          !strstr(PG, "(truncated)"),
          "AT+BATTG? paged in %u chunks of %u bytes: one summary, all 64 CRC lines and 256 client rows", n,
          (unsigned)BAT_RENDER_BUF);
    CHECK(strcmp(PG, full) == 0, "pages: the chunks joined are the whole render byte for byte");

    pg_orig(o, 31);
    n = pages(BAT_RENDER_TT_GLOBAL, o, BAT_RENDER_BUF);
    CHECK(n == 1 && count_lines(PG, "+BATTG: ") == 11 && count_lines(PG, "+BATTG: crc via=02:60:00:00:00:1f ") == 2 &&
          count_lines(PG, "+BATTG: 06:60:00:00:1f:") == 8 && strncmp(PG, "+BATTG: rows=", 13) == 0,
          "AT+BATTG=<originator>: summary, its 2 CRC lines and its 8 rows only");
    pg_client(c, 17, 0);
    n = pages(BAT_RENDER_TT_GLOBAL, c, BAT_RENDER_BUF);
    CHECK(n == 1 && count_lines(PG, "+BATTG: ") == 5 && count_lines(PG, "+BATTG: crc via=02:60:00:00:00:11 ") == 2 &&
          strstr(PG, "+BATTG: 06:60:00:00:11:00 vid=-1 via=02:60:00:00:00:11 ") &&
          strstr(PG, "+BATTG: 06:60:00:00:11:00 vid=1 via=02:60:00:00:00:11 "),
          "AT+BATTG=<client>: summary, the claiming originator's CRC lines and that client's rows (both VLANs)");
    c[0] = 0x0e;
    n = pages(BAT_RENDER_TT_GLOBAL, c, BAT_RENDER_BUF);
    CHECK(n == 1 && count_lines(PG, "+BATTG: ") == 1, "AT+BATTG=<unknown mac>: the summary line only");

    /* chunk boundaries inside the CRC lines and inside the rows of one originator */
    size_t ll = longest_line(full);
    const size_t lens[] = { ll + BAT_RENDER_RESERVE + 1, 150, 200, 257, 333, 512, 1000, 2500 };
    int same = 1;
    for (unsigned i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        n = pages(BAT_RENDER_TT_GLOBAL, NULL, lens[i]);
        if (!n || PG_BAD || strcmp(PG, full) != 0) {
            printf("     len %zu: chunks %u bad %u\n", lens[i], n, PG_BAD);
            same = 0;
        }
    }
    CHECK(same, "pages: every chunk size from one line (%zu + %u + 1 bytes) up joins into the whole listing", ll,
          (unsigned)BAT_RENDER_RESERVE);
    n = pages(BAT_RENDER_TT_GLOBAL, NULL, ll + BAT_RENDER_RESERVE);
    size_t pl = strlen(PG);
    CHECK(n > 0 && PG_BAD == 0 && pl > 21 && strcmp(PG + pl - 21, "+BATTG: (truncated)\r\n") == 0,
          "pages: a line longer than the chunk ends the listing with (truncated)");
    size_t lf = bat_render(B, BAT_RENDER_TT_LOCAL, full, sizeof(full));
    n = pages(BAT_RENDER_TT_LOCAL, NULL, longest_line(full) + BAT_RENDER_RESERVE + 1);
    CHECK(lf > 0 && n == 2 && PG_BAD == 0 && strcmp(PG, full) == 0, "pages: TT local, one line per chunk");
}

/* ---- simulator scenarios -------------------------------------------------------------- */

static struct bat_sim *line(uint32_t seed, uint32_t tput)
{
    struct bat_sim *s = bat_sim_new(3, seed);
    bat_sim_link(s, 0, 1, true, tput);
    bat_sim_link(s, 1, 2, true, tput);
    for (unsigned i = 0; i < 3; i++) {
        bat_sim_start(s, i);
    }
    return s;
}

static struct bat_orig *sorig(struct bat_sim *s, unsigned at, unsigned who)
{
    return bat_orig_find(bat_sim_engine(s, at), bat_sim_hard(s, who));
}

static const struct bat_tt_row *srow(struct bat_sim *s, unsigned at, const uint8_t *mac, uint16_t vid,
                                     unsigned via)
{
    struct bat *b = bat_sim_engine(s, at);
    struct bat_orig *o = sorig(s, at, via);
    for (unsigned i = 0; o && i < BAT_TT_ROWS; i++) {
        const struct bat_tt_row *r = &b->tt.rows[i];
        if (r->used && r->orig == bat_orig_index(b, o) && r->vid == vid && memcmp(r->mac, mac, 6) == 0) {
            return r;
        }
    }
    return NULL;
}

/* announced CRC of @who's untagged VLAN vs @at's copy */
static bool crc_synced(struct bat_sim *s, unsigned at, unsigned who)
{
    struct bat *w = bat_sim_engine(s, who), *a = bat_sim_engine(s, at);
    uint8_t o[512];
    memcpy(o, w->tt.last_tvlv, w->tt.last_tvlv_len);
    struct bat_orig *x = sorig(s, at, who);
    if (!x || !x->tt.known || x->tt.ttvn != w->tt.ttvn) {
        return false;
    }
    unsigned n;
    for (uint16_t i = 0; i < bat_get16(o + 6); i++) {
        if (bat_tt_orig_crc(a, bat_orig_index(a, x), bat_get16(o + 12 + 8 * i), &n) != bat_get32(o + 8 + 8 * i)) {
            return false;
        }
    }
    return true;
}

static void s3(void)
{
    struct bat_sim *s = line(3, 100);
    bat_sim_run(s, 6000);
    const struct bat_tt_row *r = srow(s, 0, bat_sim_soft(s, 2), 0, 2);
    CHECK(r && !(r->flags & BAT_TTR_TEMP), "S3: A's global TT holds C's soft MAC via C");
    CHECK(crc_synced(s, 0, 2) && crc_synced(s, 2, 0) && crc_synced(s, 1, 0) && crc_synced(s, 0, 1),
          "S3: every copy's CRC and TTVN equal the owner's announcement");
    uint32_t q[3];
    for (unsigned i = 0; i < 3; i++) {
        q[i] = bat_sim_counter(s, i, BAT_C_TT_REQ_TX);
    }
    bat_sim_run(s, 20000);
    CHECK(bat_sim_counter(s, 0, BAT_C_TT_REQ_TX) == q[0] && bat_sim_counter(s, 1, BAT_C_TT_REQ_TX) == q[1] &&
          bat_sim_counter(s, 2, BAT_C_TT_REQ_TX) == q[2],
          "S3: after convergence tt_req_tx stops rising (%u %u %u)", q[0], q[1], q[2]);
    CHECK(bat_sim_counter(s, 0, BAT_C_TT_CRC_FAIL) == 0 && bat_sim_counter(s, 2, BAT_C_TT_CRC_FAIL) == 0,
          "S3: no CRC failure anywhere");
    bat_sim_free(s);
}

static void s12(void)
{
    struct bat_sim *s = line(12, 100);
    bat_sim_run(s, 8000);
    struct bat *c = bat_sim_engine(s, 2);
    uint32_t q0 = bat_sim_counter(s, 0, BAT_C_TT_REQ_TX), q1 = bat_sim_counter(s, 1, BAT_C_TT_REQ_TX);
    const uint8_t k1[6] = { 0x06, 0x5a, 0xcc, 0x00, 0x00, 0x01 };
    const uint8_t k2[6] = { 0x06, 0x5a, 0xcc, 0x00, 0x00, 0x02 };
    bat_tt_local_add(c, k1, 0, 0);
    bat_sim_run(s, 2500);
    CHECK(srow(s, 0, k1, 0, 2) && crc_synced(s, 0, 2) && bat_sim_counter(s, 0, BAT_C_TT_REQ_TX) == q0 &&
          bat_sim_counter(s, 1, BAT_C_TT_REQ_TX) == q1,
          "S12: C adds a client: A and W apply the diff from C's OGM, no request");
    bat_tt_local_add(c, k2, 0x8003, 0x10);
    bat_sim_run(s, 2500);
    bat_tt_local_del(c, k1, 0);
    bat_sim_run(s, 2500);
    CHECK(!srow(s, 0, k1, 0, 2) && srow(s, 0, k2, 0x8003, 2) && crc_synced(s, 0, 2) &&
          bat_sim_counter(s, 0, BAT_C_TT_REQ_TX) == q0,
          "S12: VLAN add and a delete follow, still no request");
    /* walk C's TTVN up to 255 and over the wrap */
    c->tt.ttvn = 254;
    struct bat_orig *ca = sorig(s, 0, 2), *cw = sorig(s, 1, 2);
    ca->tt.ttvn = 254;
    cw->tt.ttvn = 254;
    bat_tt_local_add(c, k1, 0, 0);
    bat_sim_run(s, 2500);
    CHECK(c->tt.ttvn == 255 && ca->tt.ttvn == 255 && crc_synced(s, 0, 2), "S12: 254 -> 255 applied");
    bat_tt_local_del(c, k1, 0);
    bat_sim_run(s, 2500);
    CHECK(c->tt.ttvn == 0 && ca->tt.ttvn == 0 && crc_synced(s, 0, 2) &&
          bat_sim_counter(s, 0, BAT_C_TT_REQ_TX) == q0,
          "S12: 255 -> 0 applied as +1 (mod 256), no request");
    /* a Linux peer requests the full table at the wrap: answered */
    uint32_t a0 = bat_sim_counter(s, 2, BAT_C_TT_RESP_TX);
    ca->tt.known = 0;
    ca->tt.ttvn = 200;
    bat_sim_run(s, 2500);
    CHECK(bat_sim_counter(s, 0, BAT_C_TT_REQ_TX) > q0 && bat_sim_counter(s, 2, BAT_C_TT_RESP_TX) > a0 &&
          crc_synced(s, 0, 2) && ca->tt.ttvn == 0,
          "S12: a full-table request (Linux-style at the wrap) is answered and resyncs A");
    bat_sim_free(s);
}

static void s15(void)
{
    struct bat_sim *s = line(15, 100);
    bat_sim_run(s, 6000);
    struct bat *a = bat_sim_engine(s, 0), *c = bat_sim_engine(s, 2);
    const uint8_t nc[6] = { 0x06, 0x5a, 0xdd, 0x00, 0x00, 0x09 };
    /* C bridges a new host whose first frame is a broadcast; C's table does not have it yet */
    uint8_t f[128];
    bat_sim_mk_eth(f, BC, nc, 0x0806, 46, 0x22);
    bat_bcast_tx_own(c, f, 60);   /* a bridged host's frame: bat_tx_soft would refuse its source */
    bat_sim_run(s, 10);
    const struct bat_tt_row *r = srow(s, 0, nc, 0, 2);
    CHECK(r && (r->flags & BAT_TTR_TEMP), "S15: A learned C's new host temporarily from its broadcast");
    bat_sim_soft_rx_clear(s, 2);
    size_t n = bat_sim_mk_eth(f, nc, bat_sim_soft(s, 0), 0x0800, 60, 0x33);
    CHECK(bat_sim_soft_tx(s, 0, f, n) == 0, "S15: A's unicast to the new host is sent");
    bat_sim_run(s, 10);
    CHECK(bat_sim_soft_rx_count(s, 2) == 1 && bat_sim_soft_rx_get(s, 2, 0)->len == n &&
          memcmp(bat_sim_soft_rx_get(s, 2, 0)->bytes, f, n) == 0,
          "S15: ... and delivered at C before any TT change");
    bat_tt_local_add(c, nc, 0, 0);
    bat_sim_run(s, 2500);
    r = srow(s, 0, nc, 0, 2);
    CHECK(r && !(r->flags & BAT_TTR_TEMP) && crc_synced(s, 0, 2),
          "S15: C's announcement replaces the temporary row");
    (void)a;
    bat_sim_free(s);
}

int main(void)
{
    test_helpers();
    test_local_build();
    test_local_misc();
    test_ogm_rx();
    test_ogm_rx_more();
    test_rows_full();
    test_answer();
    test_answer_pacing();
    test_temp();
    test_temp_repoint();
    test_temp_cap();
    test_resolve();
    test_client_route();
    test_gw_best();
    test_req_backoff();
    test_req_backoff_onbehalf();
    test_join_race();
    test_uc_ttvn();
    test_render();
    test_render_pages();
    s3();
    s12();
    s15();
    free(B);
    if (failures) {
        printf("test_bat_tt: %d FAILED\n", failures);
        return 1;
    }
    printf("test_bat_tt: all passed\n");
    return 0;
}
