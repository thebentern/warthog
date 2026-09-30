/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Hostile input for the batman engine. Run it under SAN=1 (ASan + UBSan): every frame is
 * handed to bat_rx_hard in a heap buffer of exactly its length, so any read or write past
 * the frame is reported.
 *
 *  1. A converged engine (configured as node C, fed B's frames from capture 02, plus a
 *     second neighbour so it has interface-table routes and relays) receives every prefix
 *     (0..len bytes) of every frame of every committed golden capture, once as captured
 *     and once re-addressed to the engine's hard address.
 *  2. 100 000 seeded random mutations of golden frames (bit flips, byte sets in the
 *     header region, truncation and extension past 1600 bytes, re-addressing).
 *  3. Crafted frames, each with the exact counters it must move and, where the frame
 *     must not touch state, a check that tables and slots are unchanged.
 *  4. Two engines each routing an originator through the other: the TTL ends the loop.
 * After 1 and 2 the TT and fragment invariants are checked, and every render of the fuzzed
 * tables pages whole (bat_render_from) and filters by MAC inside its listing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bat_crc32c.h"
#include "bat_internal.h"
#include "bat_pcap.h"
#include "bat_sim.h"

#ifndef BAT_GOLDEN_DIR
#define BAT_GOLDEN_DIR "bat_golden/pcap"
#endif

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* ---- stub ops (records only the last TX, so the fuzz loops stay cheap) --------------- */

static struct {
    uint32_t now, rng, tput;
    unsigned ntx, ndl;
    size_t last_len;
    uint8_t last[2200];
} S;

static int st_tx(void *u, const uint8_t *f, size_t len)
{
    (void)u;
    S.ntx++;
    S.last_len = len;
    if (len <= sizeof(S.last)) {
        memcpy(S.last, f, len);
    }
    return BAT_TX_OK;
}
static void st_deliver(void *u, const uint8_t *f, size_t len)
{
    (void)u;
    (void)f;
    (void)len;
    S.ndl++;
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
    (void)a;
    return S.tput;
}
static const struct bat_ops OPS = { st_tx, st_deliver, st_now, st_rand, st_tput };

static struct bat *B;
static uint8_t HC[6] = { 0x02, 0x00, 0x00, 0x00, 0x0c, 0x01 };    /* engine = C */
static uint8_t SC[6] = { 0x02, 0x00, 0x00, 0x00, 0x0c, 0xff };
static uint8_t NB[6] = { 0x02, 0x00, 0x00, 0x00, 0x0b, 0x02 };    /* B's link toward C */
static uint8_t OB[6] = { 0x02, 0x00, 0x00, 0x00, 0x0b, 0x01 };    /* B's originator */
static uint8_t OA[6] = { 0x02, 0x00, 0x00, 0x00, 0x0a, 0x01 };    /* A */
static uint8_t NF[6] = { 0x02, 0x00, 0x00, 0x00, 0x0f, 0x01 };    /* extra neighbour F */
static const uint8_t BC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* Exact-size heap copy, so ASan sees any access beyond @len. */
static void feed(const uint8_t *f, size_t len)
{
    uint8_t *b = malloc(len ? len : 1);
    if (len) {
        memcpy(b, f, len);
    }
    bat_rx_hard(B, b, len);
    free(b);
}

static void advance_to(uint32_t t)
{
    while ((int32_t)(t - S.now) > 0) {
        S.now++;
        bat_tick(B);
    }
}

/* ---- golden frames ------------------------------------------------------------------- */

#define MAXF 3000
static struct { size_t len; uint8_t *b; } FR[MAXF];
static unsigned NFR;

static const char *const CAPS[] = {
    "01-startup-AB__B-b-a.pcap", "01-startup-AB__B-b-c.pcap", "02-join-C__B-b-a.pcap",
    "02-join-C__B-b-c.pcap", "03-steady__B-b-a.pcap", "03-steady__B-b-c.pcap",
    "05-unicast-ping__B-b-a.pcap", "05-unicast-ping__B-b-c.pcap", "07-tt-diff-add-client__B-b-a.pcap",
    "07-tt-diff-add-client__B-b-c.pcap", "08-tt-missed-diff-nonfull-request__B-b-a.pcap",
    "08-tt-missed-diff-nonfull-request__B-b-c.pcap", "09-roam-client-C-to-A__B-b-a.pcap",
    "09-roam-client-C-to-A__B-b-c.pcap", "10-arp-broadcast__B-b-a.pcap", "10-arp-broadcast__B-b-c.pcap",
    "13-dhcp-via-gateway__B-b-a.pcap", "13-dhcp-via-gateway__B-b-c.pcap", "14a-frag-mtu1500__B-b-a.pcap",
    "14a-frag-mtu1500__B-b-c.pcap", "14b-frag-mtu600-AB__B-b-a.pcap", "14b-frag-mtu600-AB__B-b-c.pcap",
    "15-batctl-icmp__B-b-a.pcap", "15-batctl-icmp__B-b-c.pcap", "17-gw-tvlv-change__B-b-a.pcap",
    "19a-wifi-hwsim-startup__W1-mesh0.pcap", "19b-wifi-hwsim-data__W1-mesh0.pcap",
    "20-wifi-3node-line__W2-mesh0.pcap", "21-unicast-unknown-dst__B-b-a.pcap",
    "21-unicast-unknown-dst__B-b-c.pcap",
};

static void load(void)
{
    for (unsigned i = 0; i < sizeof(CAPS) / sizeof(CAPS[0]); i++) {
        char path[512];
        struct bat_pcap p;
        struct bat_pcap_rec r;
        snprintf(path, sizeof(path), "%s/%s", BAT_GOLDEN_DIR, CAPS[i]);
        if (bat_pcap_open(&p, path) != 0) {
            printf("FAIL cannot open %s\n", path);
            failures++;
            continue;
        }
        while (bat_pcap_next(&p, &r) == 1 && NFR < MAXF) {
            FR[NFR].len = r.caplen;
            FR[NFR].b = malloc(r.caplen);
            memcpy(FR[NFR].b, r.data, r.caplen);
            NFR++;
        }
        bat_pcap_close(&p);
    }
}

/* C after capture 02, plus neighbour F carrying A and B, so interface routes exist. */
static void converge(void)
{
    struct bat_config c;
    bat_config_defaults(&c);
    memcpy(c.hard_addr, HC, 6);
    memcpy(c.soft_addr, SC, 6);
    c.half_duplex = false;
    if (!B) {
        B = malloc(bat_ctx_size());
    }
    memset(&S, 0, sizeof(S));
    S.rng = 0xC0FFEEu;
    S.tput = 100000;
    S.now = 1000;
    bat_init(B, &c, &OPS, NULL);
    char path[512];
    struct bat_pcap p;
    struct bat_pcap_rec r;
    snprintf(path, sizeof(path), "%s/02-join-C__B-b-c.pcap", BAT_GOLDEN_DIR);
    if (bat_pcap_open(&p, path) != 0) {
        printf("FAIL cannot open %s\n", path);
        failures++;
        return;
    }
    uint64_t t0 = 0;
    while (bat_pcap_next(&p, &r) == 1) {
        t0 = t0 ? t0 : r.t_us;
        if (memcmp(r.data + 6, NB, 6) == 0) {
            advance_to(1000 + (uint32_t)((r.t_us - t0) / 1000u));
            feed(r.data, r.caplen);
            bat_tick(B);
        }
    }
    bat_pcap_close(&p);
    uint8_t f[128];
    memset(f, 0, sizeof(f));
    memcpy(f, BC, 6);
    memcpy(f + 6, NF, 6);
    f[12] = 0x43;
    f[13] = 0x05;
    f[14] = 0x03;
    f[15] = 0x0f;
    memcpy(f + 16, NF, 6);
    bat_put32(f + 22, 1);
    bat_put32(f + 26, 500);
    feed(f, 34);
    bat_neigh_find(B, NF)->tput_acc = 50000ull << 10;
    /* one OGM aggregate via F: F itself, A and B with fresh seqnos */
    size_t n = 14;
    const uint8_t *who[3] = { NF, OA, OB };
    for (int i = 0; i < 3; i++) {
        uint8_t *q = f + n;
        memset(q, 0, 20);
        q[0] = 0x04;
        q[1] = 0x0f;
        q[2] = 49;
        bat_put32(q + 4, i == 0 ? 7 : i == 1 ? 2770758995u : 623099240u);
        memcpy(q + 8, who[i], 6);
        bat_put32(q + 16, 40000);
        n += 20;
    }
    feed(f, n);
    advance_to(S.now + 1500);
}

/* ---- state fingerprint for "nothing changed" checks ---------------------------------------- */

static uint32_t fingerprint(void)
{
    uint32_t h = bat_crc32c(0, (const uint8_t *)B->neigh, sizeof(B->neigh));
    for (unsigned i = 0; i < BAT_MAX_ORIG; i++) {
        const struct bat_orig *o = &B->orig[i];
        h = bat_crc32c(h, (const uint8_t *)o, sizeof(*o));
    }
    h = bat_crc32c(h, (const uint8_t *)&B->tt, sizeof(B->tt));
    for (unsigned i = 0; i < BAT_FRAG_SLOTS; i++) {
        const struct bat_frag_slot *s = &B->data.slot[i];
        h = bat_crc32c(h, (const uint8_t *)s, offsetof(struct bat_frag_slot, buf));
    }
    return h;
}

static bool invariants(void)
{
    unsigned n = 0;
    bool ok = true;
    for (unsigned i = 0; i < BAT_TT_ROWS; i++) {
        const struct bat_tt_row *r = &B->tt.rows[i];
        if (r->used) {
            n++;
            ok &= r->orig < BAT_MAX_ORIG && B->orig[r->orig].used;
        }
    }
    ok &= n == B->tt.nrows && B->tt.n_local <= BAT_TT_LOCAL_MAX && B->tt.n_changes <= BAT_TT_CHANGES_MAX &&
          B->tt.last_tvlv_len <= sizeof(B->tt.last_tvlv);
    for (unsigned i = 0; i < BAT_FRAG_SLOTS; i++) {
        const struct bat_frag_slot *s = &B->data.slot[i];
        if (s->used) {
            ok &= s->have <= s->total && s->total <= BAT_FRAG_BUF && s->orig < BAT_MAX_ORIG &&
                  B->orig[s->orig].used;
        }
    }
    return ok;
}

/* ---- 1: prefixes -------------------------------------------------------------------------- */

static void test_prefixes(void)
{
    unsigned long calls = 0;
    for (unsigned i = 0; i < NFR; i++) {
        uint8_t g[2048];
        size_t len = FR[i].len < sizeof(g) ? FR[i].len : sizeof(g);
        for (int pass = 0; pass < 2; pass++) {
            memcpy(g, FR[i].b, len);
            if (pass == 1 && !(g[0] & 1)) {
                memcpy(g, HC, 6);
            }
            for (size_t l = 0; l <= len; l++) {
                feed(g, l);
                calls++;
            }
        }
        if (i % 64 == 0) {
            S.now += 50;
            bat_tick(B);
        }
    }
    CHECK(invariants() && bat_counter(B, BAT_C_RX) >= calls,
          "prefixes: %lu truncations of %u golden frames (as captured and re-addressed) survived, invariants hold",
          calls, NFR);
}

/* ---- 2: random mutations ------------------------------------------------------------------- */

static uint32_t XS = 0x1234abcdu;
static uint32_t xr(void)
{
    XS ^= XS << 13;
    XS ^= XS >> 17;
    XS ^= XS << 5;
    return XS;
}

static void test_mutations(void)
{
    static uint8_t g[1800];
    unsigned long n = 0;
    uint32_t rx0 = bat_counter(B, BAT_C_RX);
    for (unsigned it = 0; it < 100000; it++) {
        const unsigned k = xr() % NFR;
        size_t len = FR[k].len < 1700 ? FR[k].len : 1700;
        memcpy(g, FR[k].b, len);
        unsigned ops = 1 + xr() % 4;
        for (unsigned o = 0; o < ops; o++) {
            switch (xr() % 6) {
            case 0:     /* bit flip anywhere */
                if (len) {
                    g[xr() % len] ^= (uint8_t)(1u << (xr() % 8));
                }
                break;
            case 1:     /* byte set in the headers */
            case 2: {
                size_t lim = len < 64 ? len : 64;
                if (lim > 14) {
                    g[14 + xr() % (lim - 14)] = (uint8_t)xr();
                }
                break;
            }
            case 3:     /* truncate */
                len = len ? xr() % (len + 1) : 0;
                break;
            case 4: {   /* extend with junk, sometimes past the 1600-byte limit */
                size_t add = xr() % 120;
                if (len + add > sizeof(g)) {
                    add = sizeof(g) - len;
                }
                for (size_t q = 0; q < add; q++) {
                    g[len + q] = (uint8_t)xr();
                }
                len += add;
                break;
            }
            default:    /* re-address to us, sometimes from a neighbour */
                if (len >= 12) {
                    memcpy(g, HC, 6);
                    if (xr() & 1) {
                        memcpy(g + 6, (xr() & 1) ? NB : NF, 6);
                    }
                }
                break;
            }
        }
        feed(g, len);
        n++;
        if (it % 500 == 0) {
            S.now += 20;
            bat_tick(B);
        }
    }
    CHECK(invariants() && bat_counter(B, BAT_C_RX) - rx0 == n,
          "mutations: %lu seeded mutated frames survived, invariants hold", n);
}

/* The fuzzed tables through every render: each listing, paged in exact-size heap chunks
 * (4096 bytes, and the smallest that holds its longest line), joins into the whole render;
 * filtered by each originator's and each client's MAC it stays inside that listing. */
static char RW[1 << 17], RP[1 << 17];
static bool page_all(enum bat_render_kind k, const uint8_t *mac, size_t len)
{
    uint32_t cur = 0;
    size_t used = 0;
    RP[0] = '\0';
    for (unsigned n = 0; cur != BAT_RENDER_DONE; n++) {
        char *c = malloc(len);
        size_t w = bat_render_from(B, k, mac, &cur, c, len);
        bool ok = n < 4000 && w < len && strlen(c) == w && used + w < sizeof(RP);
        if (ok) {
            memcpy(RP + used, c, w + 1);
            used += w;
        }
        free(c);
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool render_lines_within(const char *part, const char *whole)
{
    for (const char *l = part; *l;) {
        const char *e = strstr(l, "\r\n");
        char line[512];
        size_t ll = e ? (size_t)(e + 2 - l) : strlen(l);
        if (ll >= sizeof(line)) {
            return false;
        }
        memcpy(line, l, ll);
        line[ll] = '\0';
        if (!strstr(whole, line)) {
            return false;
        }
        l += ll;
    }
    return true;
}

static bool has_line(const char *s, const char *tag, const uint8_t *m)
{
    char want[48];
    snprintf(want, sizeof(want), "%s%02x:%02x:%02x:%02x:%02x:%02x ", tag, m[0], m[1], m[2], m[3], m[4], m[5]);
    return strstr(s, want) != NULL;
}

static void test_render_fuzzed(void)
{
    const enum bat_render_kind ks[] = { BAT_RENDER_NEIGH, BAT_RENDER_ORIG, BAT_RENDER_TT_GLOBAL,
                                        BAT_RENDER_TT_LOCAL, BAT_RENDER_STAT };
    bool whole = true, filtered = true;
    unsigned lookups = 0;
    for (unsigned i = 0; i < sizeof(ks) / sizeof(ks[0]); i++) {
        size_t fl = bat_render(B, ks[i], RW, sizeof(RW)), longest = 0, cur = 0;
        for (size_t p = 0; p < fl; p++) {
            cur++;
            if (RW[p] == '\n') {
                /* an ORIG entry is an originator line plus its candidate lines */
                const bool cand = ks[i] == BAT_RENDER_ORIG && p + 12 < fl && strncmp(RW + p + 1, "+BATO:  via", 11) == 0;
                longest = cur > longest ? cur : longest;
                cur = cand ? cur : 0;
            }
        }
        const size_t lens[] = { BAT_RENDER_BUF, longest + BAT_RENDER_RESERVE + 1 };
        for (unsigned j = 0; j < 2; j++) {
            if (!page_all(ks[i], NULL, lens[j]) || strcmp(RP, RW) != 0) {
                printf("     kind %d at %zu bytes: paged listing differs\n", (int)ks[i], lens[j]);
                whole = false;
            }
        }
    }
    bat_render(B, BAT_RENDER_ORIG, RW, sizeof(RW));
    for (unsigned oi = 0; oi < BAT_MAX_ORIG; oi++) {
        if (B->orig[oi].used) {
            filtered &= page_all(BAT_RENDER_ORIG, B->orig[oi].addr, BAT_RENDER_BUF) && render_lines_within(RP, RW) &&
                        has_line(RP, "+BATO: ", B->orig[oi].addr);
            lookups++;
        }
    }
    bat_render(B, BAT_RENDER_TT_GLOBAL, RW, sizeof(RW));
    for (unsigned r = 0; r < BAT_TT_ROWS; r++) {
        if (B->tt.rows[r].used) {
            filtered &= page_all(BAT_RENDER_TT_GLOBAL, B->tt.rows[r].mac, 200) && render_lines_within(RP, RW) &&
                        (!bat_orig_at(B, B->tt.rows[r].orig) || has_line(RP, "+BATTG: ", B->tt.rows[r].mac));
            lookups++;
        }
    }
    CHECK(whole, "renders of the fuzzed tables: every kind pages whole at 4096 bytes and at its longest entry");
    CHECK(filtered && lookups > 0, "renders of the fuzzed tables: %u MAC lookups page inside their listing", lookups);
}

/* ---- 3: crafted --------------------------------------------------------------------------- */

static uint32_t SNAP[BAT_C__COUNT];
static void snap(void)
{
    for (int i = 0; i < BAT_C__COUNT; i++) {
        SNAP[i] = bat_counter(B, (enum bat_counter)i);
    }
}
/* every listed counter moved by exactly one, every other one did not move */
static int only(const enum bat_counter *w, unsigned n)
{
    for (int i = 0; i < BAT_C__COUNT; i++) {
        uint32_t d = bat_counter(B, (enum bat_counter)i) - SNAP[i];
        bool listed = false;
        for (unsigned k = 0; k < n; k++) {
            listed |= w[k] == (enum bat_counter)i;
        }
        if (listed ? d != 1 : d != 0) {
            printf("     counter %s moved by %u\n", bat_counter_name((enum bat_counter)i), d);
            return 0;
        }
    }
    return 1;
}
#define ONLY(...) only((const enum bat_counter[]){ __VA_ARGS__ }, \
                       sizeof((const enum bat_counter[]){ __VA_ARGS__ }) / sizeof(enum bat_counter))

static size_t lh(uint8_t *f, const uint8_t *dst, const uint8_t *src)
{
    memcpy(f, dst, 6);
    memcpy(f + 6, src, 6);
    f[12] = 0x43;
    f[13] = 0x05;
    return 14;
}

static void x_expect(const char *what, const uint8_t *f, size_t len, bool keep_state,
                     const enum bat_counter *w, unsigned n)
{
    uint32_t fp = fingerprint();
    snap();
    feed(f, len);
    bool c = only(w, n);
    bool st = !keep_state || fingerprint() == fp;
    CHECK(c && st, "crafted: %s%s", what, keep_state ? " (state unchanged)" : "");
}
#define EXPECT(what, f, len, keep, ...) \
    x_expect(what, f, len, keep, (const enum bat_counter[]){ __VA_ARGS__ }, \
             sizeof((const enum bat_counter[]){ __VA_ARGS__ }) / sizeof(enum bat_counter))

static uint32_t SEQ_A = 2770759100u;

static size_t ogm_a(uint8_t *f, const uint8_t *tvlv, uint16_t tl, uint16_t tl_field)
{
    lh(f, BC, NB);
    uint8_t *q = f + 14;
    memset(q, 0, 20);
    q[0] = 0x04;
    q[1] = 0x0f;
    q[2] = 49;
    bat_put32(q + 4, SEQ_A++);
    memcpy(q + 8, OA, 6);
    bat_put16(q + 14, tl_field);
    bat_put32(q + 16, 88235);
    memcpy(q + 20, tvlv, tl);
    return 34u + tl;
}

static size_t frag(uint8_t *f, const uint8_t *dst, const uint8_t *fo, uint16_t seq, unsigned num, uint8_t ttl,
                   uint16_t total, const uint8_t *data, size_t dl)
{
    lh(f, HC, NB);
    uint8_t *q = f + 14;
    q[0] = 0x41;
    q[1] = 0x0f;
    q[2] = ttl;
    q[3] = (uint8_t)(num << 4);
    memcpy(q + 4, dst, 6);
    memcpy(q + 10, fo, 6);
    bat_put16(q + 16, seq);
    bat_put16(q + 18, total);
    memcpy(q + 20, data, dl);
    return 34 + dl;
}

static void test_crafted(void)
{
    uint8_t f[1700], d[1600];
    size_t n;
    converge();
    struct bat_orig *oa = bat_orig_find(B, OA);
    CHECK(oa && bat_route_nh(B, oa, BAT_TBL_IFACE) && bat_neigh_count(B) == 2 && oa->tt.known,
          "setup: converged C with two neighbours and interface routes");
    /* gate */
    n = lh(f, BC, HC);
    memset(f + 14, 0, 20);
    f[14] = 0x03;
    f[15] = 0x0f;
    EXPECT("own hard MAC as link source -> rx_src_own", f, 34, true, BAT_C_RX, BAT_C_RX_SRC_OWN);
    memcpy(f + 6, BC, 6);
    EXPECT("group link source -> rx_src_bad", f, 34, true, BAT_C_RX, BAT_C_RX_SRC_BAD);
    memset(f + 6, 0, 6);
    EXPECT("zero link source -> rx_src_bad", f, 34, true, BAT_C_RX, BAT_C_RX_SRC_BAD);
    memcpy(f + 6, NB, 6);
    f[15] = 14;
    EXPECT("version 14 -> rx_version", f, 34, true, BAT_C_RX, BAT_C_RX_VERSION);
    f[15] = 15;
    f[14] = 0x80;
    EXPECT("type 0x80 -> rx_type", f, 34, true, BAT_C_RX, BAT_C_RX_TYPE);
    f[14] = 0x02;
    EXPECT("type 0x02 (CODED) -> rx_type", f, 34, true, BAT_C_RX, BAT_C_RX_TYPE);
    EXPECT("15-byte frame -> rx_short", f, 15, true, BAT_C_RX, BAT_C_RX_SHORT);
    EXPECT("1601-byte frame -> rx_toobig", f, 1601, true, BAT_C_RX, BAT_C_RX_TOOBIG);
    f[12] = 0x08;
    EXPECT("ethertype 0x0800 -> rx_type", f, 34, true, BAT_C_RX, BAT_C_RX_TYPE);
    f[12] = 0x43;
    /* own originator inside */
    f[14] = 0x03;
    memcpy(f + 16, HC, 6);
    EXPECT("ELP carrying our originator -> elp_own_orig", f, 34, true, BAT_C_RX, BAT_C_ELP_OWN_ORIG);
    lh(f, BC, NB);
    memset(f + 14, 0, 20);
    f[14] = 0x04;
    f[15] = 0x0f;
    f[16] = 49;
    memcpy(f + 22, HC, 6);
    bat_put32(f + 30, 1000);
    EXPECT("OGM2 carrying our originator -> ogm_own", f, 34, true, BAT_C_RX, BAT_C_OGM_RX, BAT_C_OGM_REC,
           BAT_C_OGM_OWN);
    lh(f, BC, NB);
    memset(f + 14, 0, 60);
    f[14] = 0x01;
    f[15] = 0x0f;
    f[16] = 49;
    memcpy(f + 22, HC, 6);
    EXPECT("BCAST carrying our originator -> bc_own", f, 14 + 14 + 40, true, BAT_C_RX, BAT_C_BC_RX, BAT_C_BC_OWN);
    memcpy(f + 22, OA, 6);
    f[16] = 1;
    EXPECT("BCAST TTL 1 -> bc_ttl", f, 68, true, BAT_C_RX, BAT_C_BC_RX, BAT_C_BC_TTL);
    f[16] = 0;
    EXPECT("BCAST TTL 0 -> bc_ttl", f, 68, true, BAT_C_RX, BAT_C_BC_RX, BAT_C_BC_TTL);
    memset(d, 0x5a, 100);
    n = frag(f, HC, HC, 1, 0, 50, 100, d, 50);
    EXPECT("fragment with our own address as fragment originator -> fr_unknown", f, n, true, BAT_C_RX,
           BAT_C_FR_RX, BAT_C_FR_UNKNOWN);
    /* TVLV lengths */
    lh(f, HC, NB);
    memset(f + 14, 0, 40);
    f[14] = 0x44;
    f[15] = 0x0f;
    f[16] = 50;
    memcpy(f + 18, HC, 6);
    memcpy(f + 24, OA, 6);
    bat_put16(f + 30, 21);
    EXPECT("UNICAST_TVLV with TVLV length > frame -> ut_len", f, 14 + 40, true, BAT_C_RX, BAT_C_UT_RX, BAT_C_UT_LEN);
    uint8_t tv[64];
    n = ogm_a(f, tv, 0, 30);
    EXPECT("OGM2 whose tvlv_len overruns the frame -> ogm_overrun", f, n, true, BAT_C_RX, BAT_C_OGM_RX,
           BAT_C_OGM_OVERRUN);
    const uint8_t many[] = { 0x04, 0x01, 0x00, 0x0c, 0x01, 0x02, 0xff, 0xff, 1, 2, 3, 4, 0, 0, 0, 0 };
    uint32_t fpt = bat_crc32c(0, (const uint8_t *)&B->tt, sizeof(B->tt));
    struct bat_tt_orig t0 = oa->tt;
    n = ogm_a(f, many, sizeof(many), sizeof(many));
    snap();
    feed(f, n);
    CHECK(bat_counter(B, BAT_C_TT_BAD) == SNAP[BAT_C_TT_BAD] + 1 && bat_counter(B, BAT_C_TT_REQ_TX) == SNAP[BAT_C_TT_REQ_TX] &&
          fpt == bat_crc32c(0, (const uint8_t *)&B->tt, sizeof(B->tt)) && memcmp(&t0, &oa->tt, sizeof(t0)) == 0,
          "crafted: TT VLAN count 0xFFFF in an OGM -> tt_bad, TT untouched, no request");
    const uint8_t shorttt[] = { 0x04, 0x01, 0x00, 0x03, 0x01, 0x02, 0x00 };
    n = ogm_a(f, shorttt, sizeof(shorttt), sizeof(shorttt));
    snap();
    feed(f, n);
    CHECK(bat_counter(B, BAT_C_TT_BAD) == SNAP[BAT_C_TT_BAD] + 1 &&
          fpt == bat_crc32c(0, (const uint8_t *)&B->tt, sizeof(B->tt)), "crafted: TT value of 3 bytes -> tt_bad");
    /* A's current announcement plus 11 stray bytes: the partial record is ignored */
    uint8_t part[64];
    unsigned nn;
    part[0] = 0x04;
    part[1] = 0x01;
    part[4] = 0x01;
    part[5] = oa->tt.ttvn;
    bat_put16(part + 6, 2);
    bat_put32(part + 8, bat_tt_orig_crc(B, bat_orig_index(B, oa), 0x8000, &nn));
    bat_put16(part + 12, 0x8000);
    bat_put16(part + 14, 0);
    bat_put32(part + 16, bat_tt_orig_crc(B, bat_orig_index(B, oa), 0x0000, &nn));
    bat_put16(part + 20, 0);
    bat_put16(part + 22, 0);
    memset(part + 24, 0xee, 11);
    bat_put16(part + 2, 4 + 16 + 11);
    n = ogm_a(f, part, 35, 35);
    snap();
    feed(f, n);
    CHECK(bat_counter(B, BAT_C_TT_BAD) == SNAP[BAT_C_TT_BAD] && bat_counter(B, BAT_C_TT_REQ_TX) == SNAP[BAT_C_TT_REQ_TX] &&
          fpt == bat_crc32c(0, (const uint8_t *)&B->tt, sizeof(B->tt)),
          "crafted: a partial change record after A's in-sync announcement is ignored (no request, no change)");
    /* CPU bound of the CRC check: every announced VLAN record costs a pass over A's rows, so a
     * VLAN list that repeats a VID (194 fit in a frame) is refused, and only the first TT
     * container of an OGM2 record is read (a legitimate OGM2 carries one) */
    static uint8_t dup[1600];
    memcpy(dup, part, 24);
    uint16_t ndup = 2;
    while (4 + 4 + 8 * (ndup + 1) + 20 + 14 <= 1600) {
        memcpy(dup + 8 + 8 * ndup, part + 16, 8);
        ndup++;
    }
    bat_put16(dup + 6, ndup);
    bat_put16(dup + 2, (uint16_t)(4 + 8 * ndup));
    size_t dl = 8 + 8 * (size_t)ndup;
    oa->tt.req_pending = 0;
    t0 = oa->tt;
    n = ogm_a(f, dup, (uint16_t)dl, (uint16_t)dl);
    snap();
    feed(f, n);
    CHECK(bat_counter(B, BAT_C_TT_BAD) == SNAP[BAT_C_TT_BAD] + 1 && bat_counter(B, BAT_C_TT_REQ_TX) == SNAP[BAT_C_TT_REQ_TX] &&
          fpt == bat_crc32c(0, (const uint8_t *)&B->tt, sizeof(B->tt)) && memcmp(&t0, &oa->tt, sizeof(t0)) == 0,
          "crafted: A's in-sync announcement listing VID 0 %u times (%zu-byte frame) -> tt_bad, no request, "
          "TT untouched", ndup - 1u, n);
    uint8_t two[64];
    memcpy(two, part, 24);
    bat_put16(two + 2, 4 + 16);
    memcpy(two + 24, part, 8);
    bat_put16(two + 24 + 2, 4);
    two[24 + 5] = (uint8_t)(oa->tt.ttvn + 3);
    bat_put16(two + 24 + 6, 0);
    n = ogm_a(f, two, 32, 32);
    snap();
    feed(f, n);
    CHECK(bat_counter(B, BAT_C_TT_REQ_TX) == SNAP[BAT_C_TT_REQ_TX] && bat_counter(B, BAT_C_TT_BAD) == SNAP[BAT_C_TT_BAD] &&
          fpt == bat_crc32c(0, (const uint8_t *)&B->tt, sizeof(B->tt)) && memcmp(&t0, &oa->tt, sizeof(t0)) == 0,
          "crafted: a second TT container in the same OGM2 record (a TTVN jump) is not read: no request");
    /* fragments */
    n = frag(f, HC, OA, 100, 0, 50, 0, d, 50);
    EXPECT("fragment total 0 -> fr_bad", f, n, true, BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_BAD);
    n = frag(f, HC, OA, 101, 0, 50, 2049, d, 50);
    EXPECT("fragment total 2049 -> fr_toobig", f, n, true, BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_TOOBIG);
    n = frag(f, HC, OA, 102, 0, 50, 100, d, 0);
    EXPECT("fragment with a zero-length payload -> fr_bad", f, n, true, BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_BAD);
    n = frag(f, HC, (const uint8_t[6]){ 0x02, 0x99, 0, 0, 0, 1 }, 103, 0, 50, 100, d, 50);
    EXPECT("fragment from an unknown originator -> fr_unknown", f, n, true, BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_UNKNOWN);
    n = frag(f, HC, OA, 104, 0, 50, 100, d, 50);
    EXPECT("first half of a fragmented packet held", f, n, false, BAT_C_RX, BAT_C_FR_RX);
    EXPECT("the same fragment number again -> fr_dup", f, n, true, BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_DUP);
    n = frag(f, HC, OA, 104, 1, 50, 101, d, 50);
    EXPECT("second half with a different total -> fr_bad, slot discarded", f, n, false, BAT_C_RX, BAT_C_FR_RX,
           BAT_C_FR_BAD);
    n = frag(f, HC, OA, 105, 0, 50, 100, d, 60);
    EXPECT("slot for a 100-byte packet", f, n, false, BAT_C_RX, BAT_C_FR_RX);
    n = frag(f, HC, OA, 105, 1, 50, 100, d, 60);
    EXPECT("data beyond the total -> fr_bad, slot discarded", f, n, false, BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_BAD);
    uint8_t in[64] = { 0x41, 0x0f, 0x32, 0x00 };
    memcpy(in + 4, HC, 6);
    memcpy(in + 10, OA, 6);
    n = frag(f, HC, OA, 106, 0, 50, 40, in, 40);
    EXPECT("fragment inside a fragment -> fr_nested", f, n, false, BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_DONE,
           BAT_C_FR_NESTED);
    uint8_t bc[64] = { 0x01, 0x0f, 0x31, 0x00, 0, 0, 0, 1 };
    memcpy(bc + 8, OA, 6);
    n = frag(f, HC, OA, 107, 0, 50, 60, bc, 60);
    EXPECT("reassembled BCAST -> rx_type", f, n, false, BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_DONE, BAT_C_RX_TYPE);
    uint8_t un[64] = { 0x45, 0x0f, 0x32, 0x00 };
    memcpy(un + 4, HC, 6);
    n = frag(f, HC, OA, 108, 0, 50, 40, un, 40);
    EXPECT("reassembled 0x45 for us -> unk_self", f, n, false, BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_DONE, BAT_C_UNK_SELF);
    n = frag(f, OB, OA, 109, 0, 1, 1000, d, 50);
    EXPECT("transit fragment TTL 1 -> fr_ttl", f, n, true, BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_TTL);
    f[16] = 0;
    EXPECT("transit fragment TTL 0 -> fr_ttl (no wrap to 255)", f, n, true, BAT_C_RX, BAT_C_FR_RX, BAT_C_FR_TTL);
    /* TTL 0/1 on unicast relay and ICMP */
    lh(f, HC, NB);
    memset(f + 14, 0, 60);
    f[14] = 0x40;
    f[15] = 0x0f;
    f[16] = 1;
    memcpy(f + 18, OA, 6);
    EXPECT("unicast relay TTL 1 -> uc_ttl", f, 14 + 10 + 40, true, BAT_C_RX, BAT_C_UC_RX, BAT_C_UC_TTL);
    f[16] = 0;
    EXPECT("unicast relay TTL 0 -> uc_ttl", f, 14 + 10 + 40, true, BAT_C_RX, BAT_C_UC_RX, BAT_C_UC_TTL);
    f[14] = 0x43;
    f[17] = 0;
    memcpy(f + 24, OB, 6);
    EXPECT("ICMP echo reply in transit, TTL 0 -> uc_ttl", f, 34, true, BAT_C_RX, BAT_C_IC_RX, BAT_C_UC_TTL);
    f[17] = 8;
    EXPECT("ICMP echo request in transit, TTL 0 -> TTL exceeded to its source", f, 34, false, BAT_C_RX,
           BAT_C_IC_RX, BAT_C_IC_TTLX, BAT_C_LK_TX);
    CHECK(S.last[17] == 11 && memcmp(S.last + 18, OB, 6) == 0 && memcmp(S.last + 24, HC, 6) == 0 &&
          S.last[16] == 50, "crafted: ... type 11, back to B, from C, TTL 50");
}

/* ---- 4: a routing loop ---------------------------------------------------------------------- */

static void test_loop(void)
{
    /* a triangle, so every node has two neighbours and interface-table routes */
    struct bat_sim *s = bat_sim_new(3, 99);
    bat_sim_link(s, 0, 1, true, 100);
    bat_sim_link(s, 1, 2, true, 100);
    bat_sim_link(s, 0, 2, true, 100);
    for (unsigned i = 0; i < 3; i++) {
        bat_sim_start(s, i);
    }
    bat_sim_run(s, 3000);
    const uint8_t Z[6] = { 0x02, 0x7a, 0, 0, 0, 1 };
    /* node 0 hears Z's OGM from node 1 and node 1 from node 0: each routes Z through the other */
    for (unsigned i = 0; i < 2; i++) {
        uint8_t f[64] = { 0 };
        memset(f, 0xff, 6);
        memcpy(f + 6, bat_sim_hard(s, 1 - i), 6);
        f[12] = 0x43;
        f[13] = 0x05;
        f[14] = 0x04;
        f[15] = 0x0f;
        f[16] = 40;
        bat_put32(f + 18, 55);
        memcpy(f + 22, Z, 6);
        bat_put32(f + 30, 50);
        bat_sim_inject(s, i, f, 34);
    }
    bat_sim_run(s, 300);
    struct bat *a = bat_sim_engine(s, 0), *b = bat_sim_engine(s, 1);
    struct bat_orig *za = bat_orig_find(a, Z), *zb = bat_orig_find(b, Z);
    const uint8_t *na = bat_route_nh(a, za, BAT_TBL_DEFAULT), *nb = bat_route_nh(b, zb, BAT_TBL_DEFAULT);
    const uint8_t *ia = bat_route_nh(a, za, BAT_TBL_IFACE), *ib = bat_route_nh(b, zb, BAT_TBL_IFACE);
    CHECK(na && nb && ia && ib && memcmp(na, bat_sim_hard(s, 1), 6) == 0 && memcmp(nb, bat_sim_hard(s, 0), 6) == 0 &&
          memcmp(ia, bat_sim_hard(s, 1), 6) == 0 && memcmp(ib, bat_sim_hard(s, 0), 6) == 0,
          "loop setup: node 0 routes Z via node 1 and node 1 via node 0 (both tables)");
    uint32_t fw0 = 0, ifw0 = 0, ttl0 = 0, ttlx0 = 0, uns0 = bat_sim_counter(s, 0, BAT_C_IC_UNKNOWN_SRC);
    for (unsigned i = 0; i < 3; i++) {
        fw0 += bat_sim_counter(s, i, BAT_C_UC_FWD);
        ifw0 += bat_sim_counter(s, i, BAT_C_IC_FWD);
        ttl0 += bat_sim_counter(s, i, BAT_C_UC_TTL);
        ttlx0 += bat_sim_counter(s, i, BAT_C_IC_TTLX);
    }
    uint8_t *p = a->txbuf + 14;
    memset(p, 0, 80);
    p[0] = 0x40;
    p[1] = 0x0f;
    p[2] = 50;
    memcpy(p + 4, Z, 6);
    bat_sim_mk_eth(p + 10, (const uint8_t[6]){ 0x06, 0x7a, 0, 0, 0, 1 }, bat_sim_soft(s, 0), 0x0800, 40, 1);
    a->now = bat_sim_now(s);
    bat_send_to_orig(a, za, a->txbuf, 14 + 10 + 54, BAT_TBL_DEFAULT);
    bat_icmp_send_echo(a, Z, 1, 1, 20);
    bat_sim_run(s, 300);
    uint32_t fw = 0, ifw = 0, ttl = 0, ttlx = 0;
    for (unsigned i = 0; i < 3; i++) {
        fw += bat_sim_counter(s, i, BAT_C_UC_FWD);
        ifw += bat_sim_counter(s, i, BAT_C_IC_FWD);
        ttl += bat_sim_counter(s, i, BAT_C_UC_TTL);
        ttlx += bat_sim_counter(s, i, BAT_C_IC_TTLX);
    }
    CHECK(fw - fw0 == 49 && ttl - ttl0 == 1,
          "loop: the unicast is relayed 49 times (TTL 50 -> 1), then dropped (uc_ttl) (%u, %u)", fw - fw0, ttl - ttl0);
    CHECK(ifw - ifw0 == 49 && ttlx == ttlx0 && bat_sim_counter(s, 0, BAT_C_IC_UNKNOWN_SRC) == uns0 + 1,
          "loop: an echo request (TTL 50) is relayed 49 times; its TTL-1 copy is back at the requester, "
          "whose own address is no originator there: dropped (ic_unknown_src)");
    /* started with TTL 49, it expires at node 1, which answers the requester */
    p = a->txbuf + 14;
    memset(p, 0, 20);
    p[0] = 0x43;
    p[1] = 0x0f;
    p[2] = 49;
    p[3] = 8;
    memcpy(p + 4, Z, 6);
    memcpy(p + 10, bat_sim_hard(s, 0), 6);
    a->now = bat_sim_now(s);
    bat_send_to_orig(a, za, a->txbuf, 34, BAT_TBL_DEFAULT);
    bat_sim_run(s, 300);
    uint32_t ttlx2 = 0;
    for (unsigned i = 0; i < 3; i++) {
        ttlx2 += bat_sim_counter(s, i, BAT_C_IC_TTLX);
    }
    CHECK(ttlx2 == ttlx0 + 1 && bat_sim_counter(s, 1, BAT_C_IC_TTLX) >= 1,
          "loop: an echo request with TTL 49 expires at node 1, which sends TTL exceeded");
    bat_sim_free(s);
}

int main(void)
{
    clock_t c0 = clock();
    load();
    CHECK(NFR == 2493, "loaded %u golden frames", NFR);
    converge();
    CHECK(bat_route_count(B) >= 2 && bat_tt_rows_used(B) == 10, "setup: engine converged on capture 02 (%u routes)",
          bat_route_count(B));
    test_prefixes();
    test_mutations();
    test_render_fuzzed();
    test_crafted();
    test_loop();
    double secs = (double)(clock() - c0) / CLOCKS_PER_SEC;
    CHECK(secs < 20.0, "hostile run took %.1f s of CPU (budget 20 s)", secs);
    for (unsigned i = 0; i < NFR; i++) {
        free(FR[i].b);
    }
    free(B);
    if (failures) {
        printf("test_bat_hostile: %d FAILED\n", failures);
        return 1;
    }
    printf("test_bat_hostile: all passed\n");
    return 0;
}
