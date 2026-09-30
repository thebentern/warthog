/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Golden-vector tests: the batman engine against frames captured from a real
 * batman-adv 2024.3 BATMAN_V mesh (bat_golden/pcap/, copied from the clean-room spec's
 * captures; packet data only) and the byte strings printed in the spec.
 *
 *  1. Every frame of every committed capture decodes with the engine's codec: fixed
 *     headers, OGM2 aggregates, TVLV areas, TT containers. Per-capture frame, OGM2-frame,
 *     aggregate and OGM2-record counts equal the capture sidecars' frame indexes.
 *  2. For every full-table TT response and every TTVN-1 change list, the per-VLAN CRC
 *     recomputed with the engine's CRC equals the announced VLAN record (44 checks in the
 *     committed subset of the spec's 66/66).
 *  3. Scenario 02 (C joins A-B): B's frames from 02-join-C__B-b-c.pcap replayed into an
 *     engine configured as C end with C's routes (A via B's second interface at 88235,
 *     B at 100000) and exactly C's global TT from state/02-join-C.txt; the engine's TT
 *     requests equal C's captured requests byte for byte.
 *  4. Byte-exact own frames: the first own OGM and ELP of membership §3.5 (run7), the
 *     tt §12 E8 full-table answer, the reassembly and re-cutting of capture 14a's
 *     fragments, and C's echo replies to capture 15's requests (replayed after 3).
 *  5. Fragment arithmetic of dataplane §7.2.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ---- stub ops -------------------------------------------------------------------- */

#define MAXTX 4096
struct frame { uint32_t t; size_t len; uint8_t b[BAT_MAX_LINK_FRAME]; };
static struct {
    uint32_t now;
    uint32_t script[16];
    unsigned nscript, spos;
    uint32_t rng, tput;
    unsigned ntx, ndl;
    struct frame tx[MAXTX];
    struct frame dl[64];
} S;

static int st_tx(void *u, const uint8_t *f, size_t len)
{
    (void)u;
    if (S.ntx < MAXTX) {
        S.tx[S.ntx].t = S.now;
        S.tx[S.ntx].len = len;
        memcpy(S.tx[S.ntx].b, f, len);
        S.ntx++;
    }
    return BAT_TX_OK;
}
static void st_deliver(void *u, const uint8_t *f, size_t len)
{
    (void)u;
    if (S.ndl < 64 && len <= sizeof(S.dl[0].b)) {
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
    (void)a;
    return S.tput;
}
static const struct bat_ops OPS = { st_tx, st_deliver, st_now, st_rand, st_tput };

static struct bat *B;

static void start(const char *hard, const char *soft, bool half_duplex, uint16_t mtu)
{
    struct bat_config c;
    bat_config_defaults(&c);
    sscanf(hard, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &c.hard_addr[0], &c.hard_addr[1], &c.hard_addr[2],
           &c.hard_addr[3], &c.hard_addr[4], &c.hard_addr[5]);
    sscanf(soft, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &c.soft_addr[0], &c.soft_addr[1], &c.soft_addr[2],
           &c.soft_addr[3], &c.soft_addr[4], &c.soft_addr[5]);
    c.half_duplex = half_duplex;
    c.hard_mtu = mtu;
    if (!B) {
        B = malloc(bat_ctx_size());
    }
    S.ntx = S.ndl = 0;
    S.spos = 0;
    if (bat_init(B, &c, &OPS, NULL) != 0) {
        printf("FAIL bat_init\n");
        failures++;
    }
}

static void advance_to(uint32_t t)
{
    while ((int32_t)(t - S.now) > 0) {
        S.now++;
        bat_tick(B);
    }
}

static void rx(const uint8_t *f, size_t len)
{
    static uint8_t buf[4096];
    memcpy(buf, f, len);
    bat_rx_hard(B, buf, len);
    bat_tick(B);
}

static void mac(uint8_t *o, const char *s)
{
    sscanf(s, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &o[0], &o[1], &o[2], &o[3], &o[4], &o[5]);
}

static size_t hex(uint8_t *o, const char *h)
{
    size_t n = 0;
    while (*h) {
        unsigned v;
        if (*h == ' ' || *h == '|' || *h == '\n') {
            h++;
            continue;
        }
        if (sscanf(h, "%2x", &v) != 1) {
            break;
        }
        o[n++] = (uint8_t)v;
        h += 2;
    }
    return n;
}

static int open_cap(struct bat_pcap *p, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", BAT_GOLDEN_DIR, name);
    if (bat_pcap_open(p, path) != 0) {
        printf("FAIL cannot open %s\n", path);
        failures++;
        return -1;
    }
    return 0;
}

/* ---- 1 and 2: decode every frame ----------------------------------------------------- */

static const struct {
    const char *name;
    unsigned frames, ogm_frames, aggregates, ogm_records;
} CAPS[] = {
    { "01-startup-AB__B-b-a.pcap", 85, 28, 0, 28 },
    { "01-startup-AB__B-b-c.pcap", 28, 0, 0, 0 },
    { "02-join-C__B-b-a.pcap", 117, 49, 0, 49 },
    { "02-join-C__B-b-c.pcap", 103, 34, 11, 45 },
    { "03-steady__B-b-a.pcap", 76, 33, 0, 33 },
    { "03-steady__B-b-c.pcap", 69, 25, 8, 33 },
    { "05-unicast-ping__B-b-a.pcap", 27, 9, 0, 9 },
    { "05-unicast-ping__B-b-c.pcap", 25, 8, 2, 10 },
    { "07-tt-diff-add-client__B-b-a.pcap", 99, 39, 0, 39 },
    { "07-tt-diff-add-client__B-b-c.pcap", 87, 28, 11, 39 },
    { "08-tt-missed-diff-nonfull-request__B-b-a.pcap", 102, 42, 0, 42 },
    { "08-tt-missed-diff-nonfull-request__B-b-c.pcap", 91, 34, 8, 42 },
    { "09-roam-client-C-to-A__B-b-a.pcap", 66, 27, 0, 27 },
    { "09-roam-client-C-to-A__B-b-c.pcap", 63, 24, 3, 27 },
    { "10-arp-broadcast__B-b-a.pcap", 63, 23, 1, 24 },
    { "10-arp-broadcast__B-b-c.pcap", 64, 23, 1, 24 },
    { "13-dhcp-via-gateway__B-b-a.pcap", 57, 18, 0, 18 },
    { "13-dhcp-via-gateway__B-b-c.pcap", 50, 18, 0, 18 },
    { "14a-frag-mtu1500__B-b-a.pcap", 33, 8, 1, 9 },
    { "14a-frag-mtu1500__B-b-c.pcap", 35, 9, 0, 9 },
    { "14b-frag-mtu600-AB__B-b-a.pcap", 67, 21, 0, 21 },
    { "14b-frag-mtu600-AB__B-b-c.pcap", 61, 19, 2, 21 },
    { "15-batctl-icmp__B-b-a.pcap", 77, 25, 0, 25 },
    { "15-batctl-icmp__B-b-c.pcap", 60, 18, 3, 21 },
    { "17-gw-tvlv-change__B-b-a.pcap", 70, 30, 0, 30 },
    { "19a-wifi-hwsim-startup__W1-mesh0.pcap", 261, 38, 0, 38 },
    { "19b-wifi-hwsim-data__W1-mesh0.pcap", 216, 28, 0, 28 },
    { "20-wifi-3node-line__W2-mesh0.pcap", 283, 34, 10, 50 },
    { "21-unicast-unknown-dst__B-b-a.pcap", 29, 12, 0, 12 },
    { "21-unicast-unknown-dst__B-b-c.pcap", 29, 12, 0, 12 },
};
#define NCAPS (sizeof(CAPS) / sizeof(CAPS[0]))

struct stats {
    unsigned frames, other, errors, ogm_frames, aggregates, ogm_records, tvlvs, tt, crc_ok, crc_bad;
    unsigned type[256];
};

static void err(struct stats *st, const char *cap, unsigned k, const char *why)
{
    if (st->errors++ < 5) {
        printf("     %s #%u: %s\n", cap, k, why);
    }
}

/* CRC of the change records of one VID; every record must be an ADD. */
static uint32_t crc_of_changes(const uint8_t *ch, size_t n, uint16_t vid, bool *adds_only)
{
    uint32_t crc = 0;
    for (size_t k = 0; k < n; k++) {
        const uint8_t *c = ch + 12 * k;
        if (bat_get16(c + 10) != vid) {
            continue;
        }
        *adds_only &= (c[0] & 0x03) == 0;
        crc ^= bat_crc32c_tt(vid, c[0], c + 4);
    }
    return crc;
}

/* TT v1 value. @kind_full: the container is a full table (response 0x14) or a TTVN-1 list. */
static void tt_value(struct stats *st, const char *cap, unsigned k, const uint8_t *v, size_t len, bool ogm)
{
    st->tt++;
    if (len < 4 || len < 4 + 8u * bat_get16(v + 2) || (len - 4 - 8u * bat_get16(v + 2)) % 12 != 0) {
        err(st, cap, k, "TT container length");
        return;
    }
    uint16_t nv = bat_get16(v + 2);
    size_t nch = (len - 4 - 8u * nv) / 12;
    bool full = ogm ? (v[1] == 1 && nch > 0) : (v[0] == 0x14);
    if (!full) {
        return;
    }
    bool adds = true;
    for (uint16_t i = 0; i < nv; i++) {
        uint16_t vid = bat_get16(v + 4 + 8 * i + 4);
        if (crc_of_changes(v + 4 + 8u * nv, nch, vid, &adds) == bat_get32(v + 4 + 8 * i)) {
            st->crc_ok++;
        } else {
            st->crc_bad++;
            err(st, cap, k, "TT CRC mismatch");
        }
    }
    if (!adds) {
        err(st, cap, k, "full table / TTVN-1 list carries a DEL or ROAM record");
    }
}

static void tvlv_area(struct stats *st, const char *cap, unsigned k, const uint8_t *a, size_t len, bool ogm)
{
    struct bat_tvlv t;
    size_t off = 0;
    int r;
    while ((r = bat_tvlv_next(a, len, &off, &t)) == BAT_TVLV_OK) {
        st->tvlvs++;
        if (t.type == BAT_TVLV_TT && t.version == 1) {
            tt_value(st, cap, k, t.val, t.len, ogm);
        }
    }
    if (r == BAT_TVLV_OVERRUN || off != len) {
        err(st, cap, k, "TVLV area does not end on a container boundary");
    }
}

static void decode(struct stats *st, const char *cap, unsigned k, const uint8_t *f, size_t len)
{
    if (len < 14) {
        err(st, cap, k, "short frame");
        return;
    }
    if (bat_get16(f + 12) != BAT_ETHERTYPE) {
        st->other++;
        return;
    }
    const uint8_t *p = f + 14;
    size_t pl = len - 14;
    if (pl < 2 || p[1] != BAT_COMPAT) {
        err(st, cap, k, "short or wrong version");
        return;
    }
    st->type[p[0]]++;
    size_t hl = bat_hdr_len(p[0]);
    if (hl == 0 || pl < hl) {
        err(st, cap, k, "unknown type or short header");
        return;
    }
    switch (p[0]) {
    case BAT_PT_OGM2: {
        size_t off = 0;
        unsigned n = 0;
        while (off < pl) {
            const uint8_t *r = p + off;
            if (pl - off < BAT_OGM_HLEN || r[0] != BAT_PT_OGM2 || r[1] != BAT_COMPAT ||
                BAT_OGM_HLEN + (size_t)bat_get16(r + BAT_OGM_TVLV_LEN) > pl - off) {
                err(st, cap, k, "OGM2 record walk");
                return;
            }
            tvlv_area(st, cap, k, r + BAT_OGM_HLEN, bat_get16(r + BAT_OGM_TVLV_LEN), true);
            off += BAT_OGM_HLEN + bat_get16(r + BAT_OGM_TVLV_LEN);
            n++;
        }
        st->ogm_frames++;
        st->ogm_records += n;
        st->aggregates += n > 1;
        break;
    }
    case BAT_PT_UTVLV:
        if (bat_get16(p + BAT_UT_TVLV_LEN) != pl - BAT_UT_HLEN) {
            err(st, cap, k, "UNICAST_TVLV length");
            return;
        }
        tvlv_area(st, cap, k, p + BAT_UT_HLEN, pl - BAT_UT_HLEN, false);
        break;
    case BAT_PT_MCAST: {
        size_t tl = bat_get16(p + 4);
        if (tl > pl - BAT_MC_HLEN || pl - BAT_MC_HLEN - tl < 14) {
            err(st, cap, k, "MCAST length");
            return;
        }
        tvlv_area(st, cap, k, p + BAT_MC_HLEN, tl, false);
        break;
    }
    case BAT_PT_BCAST:
    case BAT_PT_UNICAST:
    case BAT_PT_4ADDR:
        if (pl < hl + 14) {
            err(st, cap, k, "inner frame shorter than an Ethernet header");
        }
        break;
    case BAT_PT_FRAG:
        if (pl - BAT_FR_HLEN > bat_get16(p + BAT_FR_TOTAL) || pl == BAT_FR_HLEN) {
            err(st, cap, k, "fragment larger than its total");
        }
        break;
    default:
        break;
    }
}

static void test_decode(void)
{
    struct stats all;
    memset(&all, 0, sizeof(all));
    unsigned counts_ok = 0;
    for (unsigned i = 0; i < NCAPS; i++) {
        struct bat_pcap p;
        struct bat_pcap_rec r;
        struct stats st;
        memset(&st, 0, sizeof(st));
        if (open_cap(&p, CAPS[i].name) != 0) {
            continue;
        }
        int rc;
        unsigned k = 0;
        while ((rc = bat_pcap_next(&p, &r)) == 1) {
            k++;
            st.frames++;
            if (r.caplen != r.origlen) {
                err(&st, CAPS[i].name, k, "truncated capture");
            }
            decode(&st, CAPS[i].name, k, r.data, r.caplen);
        }
        if (rc < 0 || p.linktype != 1) {
            err(&st, CAPS[i].name, k, "pcap structure / link type");
        }
        bat_pcap_close(&p);
        bool ok = st.frames == CAPS[i].frames && st.ogm_frames == CAPS[i].ogm_frames &&
                  st.aggregates == CAPS[i].aggregates && st.ogm_records == CAPS[i].ogm_records;
        if (!ok) {
            printf("     %s: frames %u/%u ogm %u/%u agg %u/%u records %u/%u\n", CAPS[i].name, st.frames,
                   CAPS[i].frames, st.ogm_frames, CAPS[i].ogm_frames, st.aggregates, CAPS[i].aggregates,
                   st.ogm_records, CAPS[i].ogm_records);
        }
        counts_ok += ok;
        all.frames += st.frames;
        all.other += st.other;
        all.errors += st.errors;
        all.tvlvs += st.tvlvs;
        all.tt += st.tt;
        all.crc_ok += st.crc_ok;
        all.crc_bad += st.crc_bad;
        for (int t = 0; t < 256; t++) {
            all.type[t] += st.type[t];
        }
    }
    unsigned want_frames = 0;
    for (unsigned i = 0; i < NCAPS; i++) {
        want_frames += CAPS[i].frames;
    }
    CHECK(all.errors == 0 && all.frames == want_frames,
          "decode: all %u frames of %u captures parse with the engine codec (%u non-batman, %u TVLVs, %u TT)",
          all.frames, (unsigned)NCAPS, all.other, all.tvlvs, all.tt);
    CHECK(counts_ok == NCAPS, "decode: per-capture frame / OGM2 frame / aggregate / record counts = sidecar indexes");
    CHECK(all.type[0x03] && all.type[0x04] && all.type[0x01] && all.type[0x40] && all.type[0x41] &&
          all.type[0x42] && all.type[0x43] && all.type[0x44] && all.type[0x05],
          "decode: the committed captures exercise ELP, OGM2, BCAST, MCAST, UNICAST, FRAG, 4ADDR, ICMP, UTVLV");
    CHECK(all.crc_ok == 44 && all.crc_bad == 0,
          "TT: every full table and TTVN-1 change list re-checks against its VLAN CRCs (%u/%u)", all.crc_ok,
          all.crc_ok + all.crc_bad);
}

/* ---- 3: replay scenario 02 into an engine configured as C --------------------------------- */

struct tg_row { uint8_t mac[6], via[6]; uint16_t vid; uint32_t crc; unsigned ttvn, via_ttvn; bool temp; };

static unsigned read_transglobal(const char *node, struct tg_row *rows, unsigned max)
{
    char path[512], line[512];
    snprintf(path, sizeof(path), "%s/../state/02-join-C.txt", BAT_GOLDEN_DIR);
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    char want[64];
    snprintf(want, sizeof(want), "===== node %s =====", node);
    int in_node = 0, in_tg = 0;
    unsigned n = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "=====", 5) == 0) {
            in_node = strncmp(line, want, strlen(want)) == 0;
            in_tg = 0;
            continue;
        }
        if (!in_node) {
            continue;
        }
        if (strncmp(line, "---", 3) == 0) {
            in_tg = strncmp(line, "--- transglobal", 15) == 0;
            continue;
        }
        char m[32], via[32], flags[32];
        int vid;
        unsigned t1, t2, crc;
        if (in_tg && n < max && strlen(line) > 3 &&
            sscanf(line + 3, "%31s %d %31s ( %u) %31s ( %u) (0x%x)", m, &vid, flags, &t1, via, &t2, &crc) == 7) {
            struct tg_row *r = &rows[n++];
            mac(r->mac, m);
            mac(r->via, via);
            r->vid = vid < 0 ? 0x0000 : (uint16_t)(0x8000 | vid);
            r->crc = crc;
            r->ttvn = t1;
            r->via_ttvn = t2;
            r->temp = strchr(flags, 'T') != NULL;
        }
    }
    fclose(f);
    return n;
}

static uint32_t T_END;   /* engine time at the end of the replay */

/* Replays frames whose link source is @src; engine time = @base + capture time offset (ms). */
static uint32_t replay(const char *cap, const uint8_t *src, uint32_t base, struct bat_pcap *pout)
{
    struct bat_pcap p;
    struct bat_pcap_rec r;
    if (open_cap(&p, cap) != 0) {
        return base;
    }
    uint64_t t0 = 0;
    bool first = true;
    uint32_t t = base;
    while (bat_pcap_next(&p, &r) == 1) {
        if (first) {
            t0 = r.t_us;
            first = false;
        }
        t = base + (uint32_t)((r.t_us - t0) / 1000u);
        if (r.caplen < 14 || memcmp(r.data + 6, src, 6) != 0) {
            continue;
        }
        advance_to(t);
        rx(r.data, r.caplen);
    }
    if (pout) {
        *pout = p;
    } else {
        bat_pcap_close(&p);
    }
    return t;
}

static const struct frame *find_tx(unsigned from, uint8_t type, const uint8_t *dst_orig)
{
    for (unsigned k = from; k < S.ntx; k++) {
        if (S.tx[k].len > 20 && S.tx[k].b[14] == type && (!dst_orig || memcmp(S.tx[k].b + 18, dst_orig, 6) == 0)) {
            return &S.tx[k];
        }
    }
    return NULL;
}

/* Frame #@idx (1-based) of capture @cap. */
static size_t cap_frame(const char *cap, unsigned idx, uint8_t *out)
{
    struct bat_pcap p;
    struct bat_pcap_rec r;
    size_t n = 0;
    if (open_cap(&p, cap) != 0) {
        return 0;
    }
    for (unsigned k = 1; bat_pcap_next(&p, &r) == 1; k++) {
        if (k == idx) {
            memcpy(out, r.data, r.caplen);
            n = r.caplen;
            break;
        }
    }
    bat_pcap_close(&p);
    return n;
}

static void test_replay02(void)
{
    uint8_t bb2[6], a[6], bo[6], cc[6];
    mac(bb2, "02:00:00:00:0b:02");
    mac(bo, "02:00:00:00:0b:01");
    mac(a, "02:00:00:00:0a:01");
    mac(cc, "02:00:00:00:0c:01");
    memset(&S, 0, sizeof(S));
    S.rng = 0x9e3779b9u;
    S.tput = 100000;
    S.now = 1000;
    start("02:00:00:00:0c:01", "02:00:00:00:0c:ff", false, 1500);
    T_END = replay("02-join-C__B-b-c.pcap", bb2, 1000, NULL) + 200;
    advance_to(T_END);
    struct bat_orig *oa = bat_orig_find(B, a), *ob = bat_orig_find(B, bo);
    struct bat_neigh *n = bat_neigh_find(B, bb2);
    CHECK(n && memcmp(n->orig, bo, 6) == 0 && bat_neigh_count(B) == 1,
          "replay 02: one neighbour, link address 0b:02, ELP originator 0b:01 (address != originator)");
    const uint8_t *nha = bat_route_nh(B, oa, BAT_TBL_DEFAULT), *nhb = bat_route_nh(B, ob, BAT_TBL_DEFAULT);
    CHECK(oa && ob && nha && nhb && memcmp(nha, bb2, 6) == 0 && memcmp(nhb, bb2, 6) == 0 &&
          bat_route_tput(B, oa) == 88235 && bat_route_tput(B, ob) == 100000,
          "replay 02: C's originators = state file: B 10000.0 and A 8823.5, both via 02:00:00:00:0b:02");
    struct tg_row want[32];
    unsigned nw = read_transglobal("C", want, 32);
    unsigned match = 0;
    for (unsigned i = 0; i < nw; i++) {
        struct bat_orig *o = bat_orig_find(B, want[i].via);
        for (unsigned k = 0; o && k < BAT_TT_ROWS; k++) {
            const struct bat_tt_row *r = &B->tt.rows[k];
            unsigned cnt;
            if (r->used && r->orig == bat_orig_index(B, o) && r->vid == want[i].vid &&
                memcmp(r->mac, want[i].mac, 6) == 0 && r->ttvn == want[i].ttvn && o->tt.ttvn == want[i].via_ttvn &&
                !!(r->flags & BAT_TTR_TEMP) == want[i].temp &&
                bat_tt_orig_crc(B, r->orig, r->vid, &cnt) == want[i].crc) {
                match++;
                break;
            }
        }
    }
    CHECK(nw == 10 && match == nw && bat_tt_rows_used(B) == nw,
          "replay 02: global TT = C's transglobal row for row (client, VID, via, TTVNs, CRC, no T): %u/%u, %u rows",
          match, nw, bat_tt_rows_used(B));
    CHECK(oa->tt.known && ob->tt.known && bat_counter(B, BAT_C_TT_FULL) == 2 &&
          bat_counter(B, BAT_C_TT_CRC_FAIL) == 0,
          "replay 02: both full tables applied (tt_full 2), every later OGM checks (tt_crc_fail 0)");
    /* our first requests equal C's captured ones (sent one OGM earlier: deviation 2.4.1); the captured
     * answers come after C's requests, so each is asked again at the next OGM first: 4, as C's own counter
     * read (tt §5.1) */
    const struct frame *qb = find_tx(0, 0x44, bo), *qa = find_tx(0, 0x44, a);
    uint8_t cb[128], ca[128];
    size_t lb = cap_frame("02-join-C__B-b-c.pcap", 18, cb), la = cap_frame("02-join-C__B-b-c.pcap", 20, ca);
    CHECK(qb && qa && qb->len == lb && memcmp(qb->b, cb, lb) == 0 && qa->len == la && memcmp(qa->b, ca, la) == 0 &&
          bat_counter(B, BAT_C_TT_REQ_TX) == 4,
          "replay 02: the engine's first two full-table requests equal C's frames #18 and #20 byte for byte "
          "(tt_req_tx %u)", bat_counter(B, BAT_C_TT_REQ_TX));
    char buf[BAT_RENDER_BUF];
    bat_render(B, BAT_RENDER_ORIG, buf, sizeof(buf));
    CHECK(strstr(buf, "+BATO: 02:00:00:00:0a:01") && strstr(buf, "nh=02:00:00:00:0b:02 tput=8823.5") &&
          strstr(buf, "tput=10000.0"), "replay 02: render shows 8823.5 / 10000.0 like batctl o");
    (void)cc;
}

/* 4(iv): capture 15's echo requests to C, replayed onto the engine left by the replay above.
 * Pass 1 pairs each B->C echo request with the next C->B echo reply in the capture. */
#define MAXQ 16
static uint8_t Q15[MAXQ][256];
static size_t Q15L[MAXQ];

static void test_icmp15(void)
{
    uint8_t bb2[6], c1[6];
    mac(bb2, "02:00:00:00:0b:02");
    mac(c1, "02:00:00:00:0c:01");
    struct bat_pcap p;
    struct bat_pcap_rec r;
    if (open_cap(&p, "15-batctl-icmp__B-b-c.pcap") != 0) {
        return;
    }
    unsigned nq = 0, nr = 0;
    while (bat_pcap_next(&p, &r) == 1) {
        const uint8_t *q = r.data + 14;
        if (r.caplen < 34 || q[0] != 0x43 || r.caplen > sizeof(Q15[0])) {
            continue;
        }
        if (memcmp(r.data + 6, bb2, 6) == 0 && q[3] == 8 && memcmp(q + 4, c1, 6) == 0) {
            nq++;
        } else if (memcmp(r.data + 6, c1, 6) == 0 && q[3] == 0 && nr < nq && nr < MAXQ) {
            memcpy(Q15[nr], r.data, r.caplen);
            Q15L[nr++] = r.caplen;
        }
    }
    bat_pcap_rewind(&p);
    uint64_t t0 = 0;
    bool first = true;
    unsigned k = 0, same = 0, rr = 0;
    while (bat_pcap_next(&p, &r) == 1) {
        if (first) {
            t0 = r.t_us;
            first = false;
        }
        const uint8_t *q = r.data + 14;
        if (r.caplen < 14 || memcmp(r.data + 6, bb2, 6) != 0) {
            continue;
        }
        advance_to(T_END + 500 + (uint32_t)((r.t_us - t0) / 1000u));
        bool req = r.caplen >= 34 && q[0] == 0x43 && q[3] == 8 && memcmp(q + 4, c1, 6) == 0;
        unsigned from = S.ntx;
        rx(r.data, r.caplen);
        if (req && k < nr) {
            const struct frame *got = find_tx(from, 0x43, NULL);
            same += got && got->len == Q15L[k] && memcmp(got->b, Q15[k], Q15L[k]) == 0;
            rr += got && Q15L[k] == 14 + 116 && got->b[31] == 3;
            k++;
        }
        if (S.ntx > MAXTX - 64) {
            S.ntx = 0;
        }
    }
    bat_pcap_close(&p);
    CHECK(nq == 7 && nr == 7 && k == 7 && same == 7 && rr == 1,
          "capture 15: each of the %u echo requests to C answered with C's captured reply bytes (%u/%u), "
          "record route included", nq, same, nr);
}

/* ---- 4(i): run7's first own OGM and ELP -------------------------------------------------- */

static void test_own_frames(void)
{
    uint8_t want[128], f[64];
    memset(&S, 0, sizeof(S));
    S.rng = 1;
    S.tput = 100;
    S.now = 5000;
    uint32_t sc[] = { 0x58e02ca2u, 0x4f293be6u, 7, 9, 0, 0, 0 };
    memcpy(S.script, sc, sizeof(sc));
    S.nscript = 7;
    start("02:bb:00:00:00:02", "02:bb:ba:70:00:02", true, 1500);
    /* a neighbour, so the first own OGM (and its change record) is sent, not suppressed */
    uint8_t nb[6];
    mac(nb, "02:bb:00:00:00:01");
    size_t n = hex(f, "ff ff ff ff ff ff 02 bb 00 00 00 01 43 05 03 0f 02 bb 00 00 00 01 00 00 00 07 00 00 01 f4 00 00 00 00");
    advance_to(5100);
    rx(f, n);
    advance_to(6200);
    const struct frame *elp = find_tx(0, 0x03, NULL), *ogm = find_tx(0, 0x04, NULL);
    size_t we = hex(want, "ff ff ff ff ff ff 02 bb 00 00 00 02 43 05 | 03 0f 02 bb 00 00 00 02 58 e0 2c a2 00 00 01 f4 00 00 00 00");
    CHECK(elp && elp->len == we && memcmp(elp->b, want, we) == 0, "first own ELP = run7's bytes (membership §3.5)");
    size_t wo = hex(want, "ff ff ff ff ff ff 02 bb 00 00 00 02 43 05 | 04 0f 32 00 | 4f 29 3b e6 | 02 bb 00 00 00 02 |"
                          "00 1c | ff ff ff ff | 04 01 00 18 | 01 01 00 01 | 3e 8a 40 20 00 00 00 00 |"
                          "00 00 00 00 02 bb ba 70 00 02 00 00");
    CHECK(wo == 62 && ogm && ogm->len == wo && memcmp(ogm->b, want, wo) == 0,
          "first own OGM = the 62-byte frame of membership §3.5 (TTVN 1, CRC 0x3e8a4020, one ADD)");
}

/* ---- 4(ii): tt §12 E8 ------------------------------------------------------------------- */

static void test_e8(void)
{
    uint8_t f[256], want[256];
    memset(&S, 0, sizeof(S));
    S.rng = 77;
    S.tput = 100;
    S.now = 1000;
    start("02:00:5e:10:00:01", "02:00:5e:10:00:01", true, 1500);
    size_t n = hex(f, "ff ff ff ff ff ff 02 0b 00 00 00 01 43 05 03 0f 02 0b 00 00 00 01 00 00 00 05 00 00 01 f4 00 00 00 00");
    rx(f, n);
    advance_to(1600);
    n = hex(f, "ff ff ff ff ff ff 02 0b 00 00 00 01 43 05 04 0f 32 00 00 00 10 00 02 0b 00 00 00 01 00 00 ff ff ff ff");
    rx(f, n);
    advance_to(2200);
    CHECK(bat_tt_own_ttvn(B) == 1 && bat_route_nh(B, bat_orig_find(B, f + 6), BAT_TBL_DEFAULT),
          "E8 setup: own TTVN 1, B routed");
    n = hex(f, "02 00 5e 10 00 01 02 0b 00 00 00 01 43 05 |"
               "44 0f 32 00 | 02 00 5e 10 00 01 | 02 0b 00 00 00 01 | 00 10 | 00 00 | 04 01 00 0c | 02 01 00 01 |"
               "29 86 c1 04 00 00 00 00");
    unsigned t0 = S.ntx;
    rx(f, n);
    size_t wl = hex(want, "02 0b 00 00 00 01 02 00 5e 10 00 01 43 05 |"
                          "44 0f 32 00 | 02 0b 00 00 00 01 | 02 00 5e 10 00 01 | 00 1c | 00 00 | 04 01 00 18 |"
                          "14 01 00 01 | 29 86 c1 04 00 00 00 00 | 00 00 00 00 02 00 5e 10 00 01 00 00");
    const struct frame *a = find_tx(t0, 0x44, NULL);
    CHECK(a && a->len == wl && memcmp(a->b, want, wl) == 0, "answer to B's request = tt §12 E8 full table, byte for byte");
}

/* ---- 4(iii): capture 14a's fragments ------------------------------------------------------ */

static void test_frag14a(void)
{
    uint8_t f15[1600], f16[1600], g[1600], a[6], c[6], bo[6];
    mac(a, "02:00:00:00:0a:01");
    mac(c, "02:00:00:00:0c:01");
    mac(bo, "02:00:00:00:0b:01");
    size_t l15 = cap_frame("14a-frag-mtu1500__B-b-a.pcap", 15, f15);
    size_t l16 = cap_frame("14a-frag-mtu1500__B-b-a.pcap", 16, f16);
    CHECK(l15 == 796 && l16 == 796 && f15[14] == 0x41 && f16[14] == 0x41 && bat_get16(f15 + 30) == 40664,
          "capture 14a #15/#16: two 796-byte fragments, seq 40664");
    /* reassembly: an engine as C, the fragments handed to it as if A were its neighbour */
    memset(&S, 0, sizeof(S));
    S.rng = 5;
    S.tput = 100;
    S.now = 1000;
    start("02:00:00:00:0c:01", "02:00:00:00:0c:ff", false, 1500);
    size_t n = hex(g, "ff ff ff ff ff ff 02 00 00 00 0a 01 43 05 03 0f 02 00 00 00 0a 01 00 00 00 05 00 00 01 f4 00 00 00 00");
    rx(g, n);
    n = hex(g, "ff ff ff ff ff ff 02 00 00 00 0a 01 43 05 04 0f 32 00 00 00 00 09 02 00 00 00 0a 01 00 00 ff ff ff ff");
    rx(g, n);
    memcpy(f15, c, 6);
    memcpy(f16, c, 6);
    rx(f15, l15);
    rx(f16, l16);
    const uint8_t *pk = B->data.asm_frame + 14;
    uint8_t head[10];
    hex(head, "40 0f 32 05 02 00 00 00 0c 01");
    CHECK(bat_counter(B, BAT_C_FR_DONE) == 1 && S.ndl == 1 && S.dl[0].len == 1514 && memcmp(pk, head, 10) == 0 &&
          memcmp(S.dl[0].b, pk + 10, 1514) == 0,
          "reassembled: 1524-byte packet starting 40 0f 32 05 <C> (TTVN 5), its 1514-byte ping delivered");
    uint8_t pkt[14 + 1524];
    memcpy(pkt + 14, pk, 1524);
    /* re-cut: an engine as A with MTU 1500 and a route to C via B (0b:01) */
    memset(&S, 0, sizeof(S));
    S.rng = 6;
    S.tput = 100;
    S.now = 1000;
    start("02:00:00:00:0a:01", "02:00:00:00:0a:ff", false, 1500);
    n = hex(g, "ff ff ff ff ff ff 02 00 00 00 0b 01 43 05 03 0f 02 00 00 00 0b 01 00 00 00 05 00 00 01 f4 00 00 00 00");
    rx(g, n);
    n = hex(g, "ff ff ff ff ff ff 02 00 00 00 0b 01 43 05 04 0f 31 00 00 00 00 09 02 00 00 00 0c 01 00 00 00 01 58 ab");
    rx(g, n);
    struct bat_orig *oc = bat_orig_find(B, c);
    B->frag_seq = 40663;
    S.ntx = 0;
    int rc = bat_send_to_orig(B, oc, pkt, sizeof(pkt), BAT_TBL_DEFAULT);
    memcpy(f15, bo, 6);
    memcpy(f16, bo, 6);
    CHECK(rc == BAT_TX_OK && S.ntx == 2 && S.tx[0].len == 796 && memcmp(S.tx[0].b, f15, 796) == 0 &&
          S.tx[1].len == 796 && memcmp(S.tx[1].b, f16, 796) == 0,
          "re-cut with seq 40664, originator A, MTU 1500: both captured fragments reproduced byte for byte");
}

/* ---- 5: fragment arithmetic ---------------------------------------------------------------- */

static void test_arith(void)
{
    static const struct { uint16_t mtu; size_t L; unsigned n, s; } v[] = {
        { 700, 1452, 3, 484 }, { 1500, 1524, 2, 762 }, { 600, 1524, 3, 508 }, { 1500, 1532, 2, 766 },
        { 100, 1280, 16, 80 }, { 100, 1281, 0, 0 },
    };
    uint8_t d[6], pkt[14 + 1600] = { 0 };
    mac(d, "02:00:00:00:0c:01");
    for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        memset(&S, 0, sizeof(S));
        S.rng = 3;
        start("02:00:00:00:0a:01", "02:00:00:00:0a:ff", true, v[i].mtu);
        pkt[14] = 0x40;
        int rc = bat_frag_tx(B, d, d, pkt, 14 + v[i].L);
        bool ok = v[i].n ? rc == BAT_TX_OK && S.ntx == v[i].n : rc == BAT_SEND_DROP && S.ntx == 0;
        size_t sum = 0;
        for (unsigned k = 0; ok && k < S.ntx; k++) {
            size_t pl = S.tx[k].len - 34;
            ok &= pl <= v[i].s && (k + 1 == S.ntx || pl == v[i].s) && S.tx[k].len <= 14u + (v[i].mtu < 1280 ? v[i].mtu : 1280);
            sum += pl;
        }
        ok &= !v[i].n || sum == v[i].L;
        CHECK(ok, "arithmetic: %zu bytes over MTU %u -> %u x %u%s", v[i].L, v[i].mtu, v[i].n, v[i].s,
              v[i].n ? "" : " (refused: more than 16)");
    }
}

int main(void)
{
    test_decode();
    test_replay02();
    test_icmp15();
    test_own_frames();
    test_e8();
    test_frag14a();
    test_arith();
    free(B);
    if (failures) {
        printf("test_bat_golden: %d FAILED\n", failures);
        return 1;
    }
    printf("test_bat_golden: all passed\n");
    return 0;
}
