/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Several BATMAN_V engines in one process (bat_sim.c), checked against the topology
 * behaviour in batman-spec/elp-ogm.md §7 and dataplane.md §5:
 *
 *  S1  pair: neighbour and originator both ways within 3 s, default throughput = the
 *      link, interface tables empty (a single-neighbour node suppresses them), no
 *      forwarding.
 *  S2  line A - W - C (A and C out of range): A routes C via W at min(100, 100 / 2)
 *      = 50; only W forwards.
 *  S5  S2 plus a broadcast from A: delivered once at W and once at C, never back at
 *      A; W re-floods it once, C (whose only neighbour sent it) not at all.
 *  S7  S2 plus a restart of W: A and C take W's new sequence numbers at once; a
 *      second restart within 30 s is ignored (ogm_restart_blocked) until 30 s after
 *      the first, then accepted again.
 *  S7b S7 with W's counters kept across both restarts (membership §7.4.3), 5 s after W's
 *      first broadcast and 10 s apart: resumed 256 ahead, A and C take W's OGMs and
 *      broadcasts at once, with no restart path and nothing blocked.
 *  S8 S2 with every sequence counter starting at 0xFFFFFFB0 and a broadcast per
 *      second for 100 s, so ELP, OGM and BCAST counters all wrap: exactly one OGM
 *      restart per (originator, processed table) at first contact, the ELP and BCAST
 *      restart paths (d = -80), and no stale / duplicate drops afterwards.
 *  S9  S2 then the W - C link cut: A keeps its route to C for 200 s (not before
 *      199 s), loses it by 201 s; meanwhile W's relay toward C fails with lk_nopeer.
 *  S10 diamond A - {B, C} - D with a 100 path and a 40 path: D uses B (50 vs 20);
 *      after B's links drop to 20, D switches to C only once B's forwarded value
 *      (half of its EWMA toward A) falls below C's 20, which the EWMA reaches at its
 *      11th sample (90, 81, 73, 66, 61, 55, 51, 47, 44, 41, 38).
 *  S11 4-node full mesh: N^2 = 16 OGM records per OGM interval, every aggregate at
 *      or below 512 bytes.
 *  S12 S2 with 3 broadcast copies (standard group frames) on every node, and each second
 *      A queuing two broadcasts and C one in the same ms: over 20 s, across the 2^32 ms
 *      wrap, no node sends a group frame (ELP, OGM, own or relayed BCAST copy) less than
 *      5 ms after its previous one, and every broadcast is delivered once at each other node.
 *  S12b A (1 copy) sends 6 broadcasts in one ms through W (3 copies): W delivers all 6 but its
 *      copy queue takes 5 (1 sent, 4 waiting), so C, beyond W, gets the first 5.
 *  S13 A joins a running B (and a running line B - C) at 20 phases across one OGM period:
 *      when A's first TT request reaches a node that cannot route back to A yet and is
 *      dropped (tt §6.2), A asks again at the next OGM, so every join holds the peer's
 *      table within 1.2 s of the route, with at most 2 requests.
 *  Plus S2 run across the 2^32 ms wrap of the engines' clock: timers, ages and
 *  expiry use unsigned differences, so nothing expires, blocks or stalls at the wrap.
 *
 * Also: two engines' state is independent (nothing static in the engine).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bat_internal.h"
#include "bat_sim.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static const uint8_t BC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static struct bat_orig *orig(struct bat_sim *s, unsigned at, unsigned of)
{
    return bat_orig_find(bat_sim_engine(s, at), bat_sim_hard(s, of));
}

static const uint8_t *nh(struct bat_sim *s, unsigned at, unsigned of, int tbl)
{
    struct bat_orig *o = orig(s, at, of);
    return o ? bat_route_nh(bat_sim_engine(s, at), o, tbl) : NULL;
}

static bool routes_via(struct bat_sim *s, unsigned at, unsigned of, unsigned via)
{
    const uint8_t *h = nh(s, at, of, BAT_TBL_DEFAULT);
    return h && memcmp(h, bat_sim_hard(s, via), 6) == 0;
}

static uint32_t rtput(struct bat_sim *s, unsigned at, unsigned of)
{
    return bat_route_tput(bat_sim_engine(s, at), orig(s, at, of));
}

static uint32_t cnt(struct bat_sim *s, unsigned i, enum bat_counter c)
{
    return bat_sim_counter(s, i, c);
}

static bool iface_empty(struct bat_sim *s, unsigned at)
{
    struct bat *b = bat_sim_engine(s, at);
    for (unsigned i = 0; i < BAT_MAX_ORIG; i++) {
        const struct bat_orig *o = &b->orig[i];
        if (!o->used) {
            continue;
        }
        if (o->tbl[BAT_TBL_IFACE].router >= 0 || o->tbl[BAT_TBL_IFACE].last_seq != 0) {
            return false;
        }
        for (unsigned c = 0; c < BAT_CANDS_PER_ORIG; c++) {
            if (o->cand[c].used && o->cand[c].t[BAT_TBL_IFACE].valid) {
                return false;
            }
        }
    }
    return true;
}

/* A broadcast client frame from node i's soft MAC with a recognisable payload. */
static size_t bframe(struct bat_sim *s, unsigned i, uint8_t *f, uint8_t tag)
{
    size_t n = bat_sim_mk_eth(f, BC, bat_sim_soft(s, i), 0x0800, 60, tag);
    f[14] = 0x45;
    f[15] = tag;
    return n;
}

/* line A(0) - W(1) - C(2) */
static struct bat_sim *line(uint32_t seed, uint32_t tput)
{
    struct bat_sim *s = bat_sim_new(3, seed);
    bat_sim_link(s, 0, 1, true, tput);
    bat_sim_link(s, 1, 2, true, tput);
    return s;
}

static void start_all(struct bat_sim *s, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        bat_sim_start(s, i);
    }
}

static void s1(void)
{
    struct bat_sim *s = bat_sim_new(2, 1);
    bat_sim_link(s, 0, 1, true, 100);
    start_all(s, 2);
    bat_sim_run(s, 3000);
    CHECK(bat_neigh_count(bat_sim_engine(s, 0)) == 1 && bat_neigh_count(bat_sim_engine(s, 1)) == 1,
          "S1: each node has the other as ELP neighbour within 3 s");
    CHECK(routes_via(s, 0, 1, 1) && routes_via(s, 1, 0, 0), "S1: each routes the other directly");
    CHECK(rtput(s, 0, 1) == 100 && rtput(s, 1, 0) == 100, "S1: default throughput 100 both ways");
    CHECK(iface_empty(s, 0) && iface_empty(s, 1), "S1: interface tables empty on both (single neighbour)");
    CHECK(cnt(s, 0, BAT_C_OGM_TX_FWD) == 0 && cnt(s, 1, BAT_C_OGM_TX_FWD) == 0, "S1: nothing forwarded");
    CHECK(cnt(s, 0, BAT_C_OGM_TX_OWN) >= 2 && cnt(s, 0, BAT_C_ELP_TX) >= 5, "S1: ELP every ~0.5 s, OGM every ~1 s");
    bat_sim_free(s);
}

static void s2(void)
{
    struct bat_sim *s = line(2, 100);
    start_all(s, 3);
    bat_sim_run(s, 5000);
    CHECK(routes_via(s, 0, 2, 1) && rtput(s, 0, 2) == 50, "S2: A routes C via W at 50 = min(100, 100 / 2) (%u)",
          rtput(s, 0, 2));
    CHECK(routes_via(s, 2, 0, 1) && rtput(s, 2, 0) == 50, "S2: C routes A via W at 50");
    CHECK(routes_via(s, 0, 1, 1) && rtput(s, 0, 1) == 100, "S2: A routes W directly at 100");
    CHECK(routes_via(s, 1, 0, 0) && routes_via(s, 1, 2, 2), "S2: W routes both ends directly");
    struct bat_orig *wa = orig(s, 1, 0), *wc = orig(s, 1, 2);
    CHECK(wa && wc && wa->tbl[BAT_TBL_IFACE].last_fwd_seq != 0 && wc->tbl[BAT_TBL_IFACE].last_fwd_seq != 0 &&
          cnt(s, 1, BAT_C_OGM_TX_FWD) >= 6, "S2: W forwards both A's and C's OGMs (%u)", cnt(s, 1, BAT_C_OGM_TX_FWD));
    CHECK(cnt(s, 0, BAT_C_OGM_TX_FWD) == 0 && cnt(s, 2, BAT_C_OGM_TX_FWD) == 0, "S2: A and C forward nothing");
    CHECK(iface_empty(s, 0) && iface_empty(s, 2) && !iface_empty(s, 1), "S2: only W keeps interface tables");
    bat_sim_free(s);
}

static void s5(void)
{
    uint8_t f[128];
    struct bat_sim *s = line(5, 100);
    start_all(s, 3);
    bat_sim_run(s, 5000);
    size_t n = bframe(s, 0, f, 0x55);
    CHECK(bat_sim_soft_tx(s, 0, f, n) == 0, "S5: A's broadcast accepted by its engine");
    bat_sim_run(s, 100);
    const struct bat_sim_frame *w = bat_sim_soft_rx_get(s, 1, 0), *c = bat_sim_soft_rx_get(s, 2, 0);
    CHECK(bat_sim_soft_rx_count(s, 1) == 1 && w && w->len == n && memcmp(w->bytes, f, n) == 0,
          "S5: delivered once at W, byte-identical");
    CHECK(bat_sim_soft_rx_count(s, 2) == 1 && c && c->len == n && memcmp(c->bytes, f, n) == 0,
          "S5: delivered once at C, byte-identical");
    CHECK(bat_sim_soft_rx_count(s, 0) == 0 && cnt(s, 0, BAT_C_BC_OWN) == 1,
          "S5: never delivered back at A (W's re-flood reaches A and is dropped as its own)");
    CHECK(cnt(s, 1, BAT_C_BC_FWD) == 1 && cnt(s, 2, BAT_C_BC_FWD) == 0 && cnt(s, 2, BAT_C_BC_SUPP) == 1,
          "S5: W re-floods once, C suppresses (its only neighbour sent it)");
    bat_sim_free(s);
}

static void s7(void)
{
    struct bat_sim *s = line(7, 100);
    start_all(s, 3);
    /* First contact is itself a restart (a random first seqno is >= 65536 away from the
     * stored 0) and starts the 30 s protection, so restart W only after that has passed. */
    bat_sim_run(s, 40000);
    uint32_t ra = cnt(s, 0, BAT_C_OGM_RESTART), rc = cnt(s, 2, BAT_C_OGM_RESTART);
    uint32_t ba = cnt(s, 0, BAT_C_OGM_RESTART_BLOCKED), bc = cnt(s, 2, BAT_C_OGM_RESTART_BLOCKED);
    uint32_t t1 = bat_sim_now(s);
    bat_sim_restart(s, 1);
    uint32_t tr = 0, trc = 0;
    while (bat_sim_now(s) - t1 < 3000) {
        bat_sim_run(s, 1);
        if (!tr && cnt(s, 0, BAT_C_OGM_RESTART) > ra) {
            tr = bat_sim_now(s);
        }
        if (!trc && cnt(s, 2, BAT_C_OGM_RESTART) > rc) {
            trc = bat_sim_now(s);
        }
    }
    CHECK(tr && tr - t1 <= 2000 && cnt(s, 0, BAT_C_OGM_RESTART) == ra + 1,
          "S7: A accepts W's new OGM sequence within 2 s of the restart (%u ms)", tr ? tr - t1 : 0);
    CHECK(trc && trc - t1 <= 2000 && cnt(s, 2, BAT_C_OGM_RESTART) == rc + 1,
          "S7: so does C (%u ms)", trc ? trc - t1 : 0);
    CHECK(cnt(s, 0, BAT_C_OGM_RESTART_BLOCKED) == ba && cnt(s, 2, BAT_C_OGM_RESTART_BLOCKED) == bc,
          "S7: nothing blocked: the last restart was more than 30 s ago");
    CHECK(routes_via(s, 0, 2, 1) && routes_via(s, 2, 0, 1) && routes_via(s, 0, 1, 1) && rtput(s, 0, 2) == 50 &&
          bat_sim_now(s) - orig(s, 0, 1)->last_seen < 1200, "S7: A and C route through W within 3 s");
    bat_sim_run(s, t1 + 10000 - bat_sim_now(s));
    bat_sim_restart(s, 1);
    uint32_t first = tr < trc ? tr : trc, last = tr > trc ? tr : trc;
    bat_sim_run(s, first + 29900 - bat_sim_now(s));
    struct bat_orig *aw = orig(s, 0, 1), *cw = orig(s, 2, 1);
    uint32_t age = bat_sim_now(s) - aw->last_seen, agec = bat_sim_now(s) - cw->last_seen;
    CHECK(cnt(s, 0, BAT_C_OGM_RESTART_BLOCKED) >= ba + 15 && age > 15000,
          "S7: a second restart 10 s later is ignored by A (%u blocked, W's originator age %u ms)",
          cnt(s, 0, BAT_C_OGM_RESTART_BLOCKED) - ba, age);
    CHECK(cnt(s, 2, BAT_C_OGM_RESTART_BLOCKED) >= bc + 15 && agec > 15000, "S7: and by C (age %u ms)", agec);
    CHECK(routes_via(s, 0, 2, 1) && rtput(s, 0, 2) == 50, "S7: A keeps routing C through W meanwhile");
    bat_sim_run(s, last + 31200 - bat_sim_now(s));
    age = bat_sim_now(s) - aw->last_seen;
    agec = bat_sim_now(s) - cw->last_seen;
    CHECK(age < 1200 && agec < 1200 && cnt(s, 0, BAT_C_OGM_RESTART) == ra + 2,
          "S7: accepted again once 30 s have passed since the first restart (ages %u, %u ms)", age, agec);
    CHECK(last + 31200 - t1 <= 32500, "S7: ... by %u ms after the first restart", last + 31200 - t1);
    bat_sim_free(s);
}

/* ms from @t0 until node @i received a soft frame tagged @tag (0: never). */
static uint32_t got_at(struct bat_sim *s, unsigned i, uint8_t tag, uint32_t t0)
{
    for (unsigned k = 0; k < bat_sim_soft_rx_count(s, i); k++) {
        const struct bat_sim_frame *f = bat_sim_soft_rx_get(s, i, k);
        if (f->len > 15 && f->bytes[15] == tag) {
            return f->t - t0;
        }
    }
    return 0;
}

/* S7 with W's counters kept across both restarts (the port's RTC record, membership §7.4.3). W's
 * first broadcast at 35 s starts A's and C's 30 s broadcast protection, so random seqnos at 40 s and
 * 50 s would both be held; kept ones resume 256 ahead and never take a restart path. */
static void s7b(void)
{
    uint8_t f[128];
    struct bat_seq_keep keep;
    memset(&keep, 0xa5, sizeof(keep));  /* power-up garbage */
    struct bat_sim *s = line(7, 100);
    bat_sim_cfg(s, 1)->seq_keep = &keep;
    start_all(s, 3);
    bat_sim_run(s, 35000);
    bat_sim_soft_tx(s, 1, f, bframe(s, 1, f, 0x70));
    bat_sim_run(s, 5000);
    CHECK(got_at(s, 0, 0x70, 0) && got_at(s, 2, 0x70, 0), "S7b: W's broadcast at 35 s reaches A and C");
    uint32_t ra = cnt(s, 0, BAT_C_OGM_RESTART), rc = cnt(s, 2, BAT_C_OGM_RESTART);
    for (unsigned n = 0; n < 2; n++) {
        const uint32_t next = bat_sim_engine(s, 1)->ogm_seq, t = bat_sim_now(s);
        bat_sim_restart(s, 1);
        CHECK(bat_sim_engine(s, 1)->ogm_seq == next + BAT_SEQ_MARGIN, "S7b: restart %u: W's OGM seqno resumes "
              "256 past the next it would have sent", n + 1);
        for (unsigned k = 0; k < 30; k++) {
            bat_sim_soft_tx(s, 1, f, bframe(s, 1, f, (uint8_t)(0x80 + 0x20 * n + k)));
            bat_sim_run(s, 100);
        }
        uint32_t da = 0, dc = 0;
        for (unsigned k = 0; k < 30 && !(da && dc); k++) {
            da = da ? da : got_at(s, 0, (uint8_t)(0x80 + 0x20 * n + k), t);
            dc = dc ? dc : got_at(s, 2, (uint8_t)(0x80 + 0x20 * n + k), t);
        }
        CHECK(da && dc && da <= 1500 && dc <= 1500,
              "S7b: restart %u (%s): W's broadcasts reach A and C within 1.5 s (%u, %u ms)", n + 1,
              n ? "10 s after the first" : "5 s after its first broadcast", da, dc);
        bat_sim_run(s, t + 10000 - bat_sim_now(s));
    }
    CHECK(cnt(s, 0, BAT_C_OGM_RESTART) == ra && cnt(s, 2, BAT_C_OGM_RESTART) == rc &&
          cnt(s, 0, BAT_C_OGM_RESTART_BLOCKED) == 0 && cnt(s, 2, BAT_C_OGM_RESTART_BLOCKED) == 0 &&
          cnt(s, 0, BAT_C_BC_RESTART_BLOCKED) == 0 && cnt(s, 2, BAT_C_BC_RESTART_BLOCKED) == 0,
          "S7b: A and C took no OGM restart and blocked nothing for either restart");
    uint32_t age = bat_sim_now(s) - orig(s, 0, 1)->last_seen;
    CHECK(routes_via(s, 0, 2, 1) && rtput(s, 0, 2) == 50 && age < 1200, "S7b: A routes C through W (W heard %u ms "
          "ago)", age);
    bat_sim_free(s);
}

struct s8rand { unsigned calls; uint32_t x; };
static uint32_t s8_rand(void *arg)
{
    struct s8rand *r = arg;
    if (r->calls++ < 3) {
        return 0xFFFFFFB0u;   /* ELP, OGM and BCAST seqnos (bat_init draw order) */
    }
    r->x ^= r->x << 13;
    r->x ^= r->x >> 17;
    r->x ^= r->x << 5;
    return r->x;
}

static void s8(void)
{
    uint8_t f[128];
    struct s8rand r[3] = { { 0, 11 }, { 0, 22 }, { 0, 33 } };
    struct bat_sim *s = line(8, 100);
    for (unsigned i = 0; i < 3; i++) {
        bat_sim_set_rand(s, i, s8_rand, &r[i]);
    }
    start_all(s, 3);
    CHECK(bat_sim_engine(s, 0)->elp_seq == 0xFFFFFFB0u && bat_sim_engine(s, 1)->ogm_seq == 0xFFFFFFB0u &&
          bat_sim_engine(s, 2)->bcast_seq == 0xFFFFFFB0u, "S8: all counters start at 0xFFFFFFB0");
    bat_sim_run(s, 5000);
    unsigned sent = 0;
    bat_sim_capture(s, true);
    while (bat_sim_now(s) < 100000) {
        size_t n = bframe(s, 0, f, (uint8_t)sent);
        bat_sim_soft_tx(s, 0, f, n);
        sent++;
        bat_sim_run(s, 1000);
    }
    bat_sim_capture(s, false);
    struct bat *a = bat_sim_engine(s, 0), *c = bat_sim_engine(s, 2);
    CHECK(a->elp_seq < 0x1000 && a->ogm_seq < 0x1000 && a->bcast_seq < 0x1000,
          "S8: A's ELP, OGM and BCAST counters wrapped past 0 (%08x %08x %08x)", a->elp_seq, a->ogm_seq,
          a->bcast_seq);
    CHECK(cnt(s, 0, BAT_C_OGM_RESTART) == 2 && cnt(s, 2, BAT_C_OGM_RESTART) == 2,
          "S8: A and C: exactly one OGM restart per originator (default table only, leaf)");
    CHECK(cnt(s, 1, BAT_C_OGM_RESTART) == 4, "S8: W: exactly one per originator and table (2 x 2) (%u)",
          cnt(s, 1, BAT_C_OGM_RESTART));
    int clean = 1;
    for (unsigned i = 0; i < 3; i++) {
        clean &= cnt(s, i, BAT_C_OGM_OLD) == 0 && cnt(s, i, BAT_C_ELP_DUP) == 0 && cnt(s, i, BAT_C_BC_DUP) == 0 &&
                 cnt(s, i, BAT_C_OGM_RESTART_BLOCKED) == 0 && cnt(s, i, BAT_C_BC_RESTART_BLOCKED) == 0;
    }
    CHECK(clean, "S8: no ogm_old, elp_dup, bc_dup or blocked restart anywhere across the wrap");
    CHECK(cnt(s, 1, BAT_C_ELP_RX) >= 2 * 190 && cnt(s, 0, BAT_C_ELP_RX) >= 190,
          "S8: first ELPs taken through the restart path (d = -80), every later one accepted");
    CHECK(bat_sim_soft_rx_count(s, 1) == sent && bat_sim_soft_rx_count(s, 2) == sent,
          "S8: every one of A's %u broadcasts delivered at W and C, across the BCAST wrap", sent);
    CHECK(routes_via(s, 0, 2, 1) && routes_via(s, 2, 0, 1), "S8: routes intact after the wrap");
    /* the test's own duplicate: replay one of W's re-floods to C */
    const struct bat_sim_frame *dup = NULL;
    for (unsigned k = 0; k < bat_sim_captured(s); k++) {
        const struct bat_sim_frame *fr = bat_sim_captured_get(s, k);
        if (fr->from == 1 && fr->bytes[14] == 0x01) {
            dup = fr;
        }
    }
    uint32_t d0 = cnt(s, 2, BAT_C_BC_DUP);
    if (dup) {
        bat_sim_inject(s, 2, dup->bytes, dup->len);
    }
    CHECK(dup && cnt(s, 2, BAT_C_BC_DUP) == d0 + 1 && bat_sim_soft_rx_count(s, 2) == sent,
          "S8: a replayed re-flood is the only duplicate, and it is dropped");
    (void)c;
    bat_sim_free(s);
}

static void s9(void)
{
    uint8_t f[128];
    struct bat_sim *s = line(9, 100);
    start_all(s, 3);
    bat_sim_run(s, 8000);
    CHECK(routes_via(s, 0, 2, 1), "S9: A routes C via W before the cut");
    uint32_t tc = bat_sim_now(s);
    bat_sim_link(s, 1, 2, false, 0);
    bat_sim_run(s, 10000);
    uint32_t nop = cnt(s, 1, BAT_C_LK_NOPEER);
    size_t n = bat_sim_mk_eth(f, bat_sim_soft(s, 2), bat_sim_soft(s, 0), 0x0800, 60, 0x99);
    bat_sim_soft_tx(s, 0, f, n);
    bat_sim_run(s, 10);
    CHECK(cnt(s, 1, BAT_C_UC_FWD) >= 1 && cnt(s, 1, BAT_C_LK_NOPEER) > nop && bat_sim_soft_rx_count(s, 2) == 0,
          "S9: A's unicast to C still leaves A, W's relay toward C fails with lk_nopeer");
    bat_sim_run(s, tc + 199000 - bat_sim_now(s));
    CHECK(routes_via(s, 0, 2, 1), "S9: A still routes C at 199 s after the cut");
    bat_sim_run(s, tc + 201500 - bat_sim_now(s));
    struct bat_orig *ac = orig(s, 0, 2);
    struct bat *a = bat_sim_engine(s, 0);
    unsigned rows = 0;
    for (unsigned i = 0; ac && i < BAT_TT_ROWS; i++) {
        rows += a->tt.rows[i].used && a->tt.rows[i].orig == bat_orig_index(a, ac);
    }
    CHECK(!nh(s, 0, 2, BAT_TBL_DEFAULT) && rows == 0 && cnt(s, 0, BAT_C_ROUTE_LOST) >= 1,
          "S9: by 201.5 s A's route to C and its TT rows are gone");
    CHECK(routes_via(s, 0, 1, 1), "S9: A's route to W survives");
    bat_sim_free(s);
}

static void s10(void)
{
    enum { A, B, C, D };
    struct bat_sim *s = bat_sim_new(4, 10);
    bat_sim_link(s, A, B, true, 100);
    bat_sim_link(s, B, D, true, 100);
    bat_sim_link(s, A, C, true, 40);
    bat_sim_link(s, C, D, true, 40);
    start_all(s, 4);
    bat_sim_run(s, 8000);
    CHECK(routes_via(s, D, A, B) && rtput(s, D, A) == 50, "S10: D reaches A via B at 50 (%u)", rtput(s, D, A));
    struct bat_orig *dc = orig(s, D, A);
    struct bat_cand *viac = NULL;
    for (unsigned i = 0; dc && i < BAT_CANDS_PER_ORIG; i++) {
        const uint8_t *h = bat_sim_engine(s, D)->neigh[dc->cand[i].neigh].addr;
        if (dc->cand[i].used && memcmp(h, bat_sim_hard(s, C), 6) == 0) {
            viac = &dc->cand[i];
        }
    }
    CHECK(viac && viac->t[BAT_TBL_DEFAULT].tput == 20, "S10: D's alternative via C stores 20 = min(40, 40 / 2)");
    uint32_t t0 = bat_sim_now(s);
    bat_sim_link(s, A, B, true, 20);
    bat_sim_link(s, B, D, true, 20);
    struct bat_neigh *ba = bat_neigh_find(bat_sim_engine(s, B), bat_sim_hard(s, A));
    uint32_t prev = bat_neigh_tput(ba), samples = 0, samples_at_switch = 0, tsw = 0;
    char series[160];
    int w = 0;
    while (bat_sim_now(s) - t0 < 12000) {
        bat_sim_run(s, 1);
        uint32_t v = bat_neigh_tput(ba);
        if (v != prev) {
            samples++;
            if (samples <= 11) {
                w += snprintf(series + w, sizeof(series) - (size_t)w, "%s%u", samples > 1 ? "," : "", v);
            }
            prev = v;
        }
        if (!tsw && routes_via(s, D, A, C)) {
            tsw = bat_sim_now(s);
            samples_at_switch = samples;
        }
    }
    CHECK(strcmp(series, "90,81,73,66,61,55,51,47,44,41,38") == 0, "S10: B's EWMA toward A: %s", series);
    CHECK(tsw && samples_at_switch >= 11, "S10: D does not switch before B's 11th sample (switched at sample %u)",
          samples_at_switch);
    CHECK(tsw && tsw - t0 <= 10 * 1020 + 110, "S10: D switches to C within 10 OGM intervals of the degrade (%u ms)",
          tsw - t0);
    CHECK(routes_via(s, D, A, C) && rtput(s, D, A) == 20, "S10: D then reaches A via C at 20");
    bat_sim_free(s);
}

static void s11(void)
{
    struct bat_sim *s = bat_sim_new(4, 11);
    for (unsigned i = 0; i < 4; i++) {
        for (unsigned j = i + 1; j < 4; j++) {
            bat_sim_link(s, i, j, true, 100);
        }
    }
    start_all(s, 4);
    bat_sim_run(s, 6000);
    int direct = 1;
    for (unsigned i = 0; i < 4; i++) {
        for (unsigned j = 0; j < 4; j++) {
            if (i != j) {
                direct &= routes_via(s, i, j, j) && rtput(s, i, j) == 100;
            }
        }
    }
    CHECK(direct, "S11: every node routes every other directly at 100");
    bat_sim_capture(s, true);
    bat_sim_run(s, 20000);
    unsigned recs = 0, own = 0, frames = 0, over = 0;
    size_t maxpl = 0;
    for (unsigned k = 0; k < bat_sim_captured(s); k++) {
        const struct bat_sim_frame *fr = bat_sim_captured_get(s, k);
        if (fr->len <= 14 || fr->bytes[14] != 0x04) {
            continue;
        }
        size_t pl = fr->len - 14, off = 0;
        frames++;
        maxpl = pl > maxpl ? pl : maxpl;
        over += pl > 512;
        while (off + 20 <= pl) {
            const uint8_t *r = fr->bytes + 14 + off;
            own += r[2] == 50;
            recs++;
            off += 20 + bat_get16(r + 14);
        }
    }
    double per = recs / 20.0;
    CHECK(per >= 15.0 && per <= 17.0 && own >= 76 && own <= 84,
          "S11: %.1f OGM records per interval (N^2 = 16), %u own over 20 s", per, own);
    CHECK(over == 0, "S11: every aggregate at or below 512 bytes (largest %zu, %u frames)", maxpl, frames);
    CHECK(frames < recs, "S11: records are aggregated (%u frames for %u records)", frames, recs);
    int fwd = 1;
    for (unsigned i = 0; i < 4; i++) {
        fwd &= cnt(s, i, BAT_C_OGM_TX_FWD) > 0 && cnt(s, i, BAT_C_OGM_FWD_TTL) == 0;
    }
    CHECK(fwd, "S11: every node forwards (each has 3 neighbours)");
    bat_sim_free(s);
}

/* Smallest gap, in ms, between consecutive group frames node @i sent in the capture; -1: under two. */
static int grp_gap(struct bat_sim *s, unsigned i, unsigned *frames)
{
    int best = -1;
    bool have = false;
    uint32_t last = 0;
    *frames = 0;
    for (unsigned k = 0; k < bat_sim_captured(s); k++) {
        const struct bat_sim_frame *f = bat_sim_captured_get(s, k);
        if (f->from != i || f->to != BAT_SIM_BCAST) {
            continue;
        }
        (*frames)++;
        if (have && (best < 0 || (int)(f->t - last) < best)) {
            best = (int)(f->t - last);
        }
        last = f->t;
        have = true;
    }
    return best;
}

/* Soft frames node @i received whose payload tag byte is @tag. */
static unsigned got(struct bat_sim *s, unsigned i, uint8_t tag)
{
    unsigned n = 0;
    for (unsigned k = 0; k < bat_sim_soft_rx_count(s, i); k++) {
        const struct bat_sim_frame *f = bat_sim_soft_rx_get(s, i, k);
        n += f->len > 15 && f->bytes[15] == tag;
    }
    return n;
}

static void s12(void)
{
    uint8_t f[128];
    struct bat_sim *s = line(12, 100);
    for (unsigned i = 0; i < 3; i++) {
        bat_sim_cfg(s, i)->bcast_copies = 3;
    }
    bat_sim_set_now(s, 0xFFFFFFFFu - 15000u);
    start_all(s, 3);
    bat_sim_run(s, 5000);
    bat_sim_capture(s, true);
    bool ok = true;
    for (unsigned sec = 0; sec < 20; sec++) {
        size_t n = bframe(s, 0, f, (uint8_t)(0x10 + sec));
        ok = ok && bat_sim_soft_tx(s, 0, f, n) == 0;
        n = bframe(s, 0, f, (uint8_t)(0x40 + sec));
        ok = ok && bat_sim_soft_tx(s, 0, f, n) == 0;
        n = bframe(s, 2, f, (uint8_t)(0x70 + sec));
        ok = ok && bat_sim_soft_tx(s, 2, f, n) == 0;
        bat_sim_run(s, 1000);
    }
    CHECK(ok && bat_sim_now(s) < 20000, "S12: 20 times, A queues two broadcasts and C one in the same ms, the "
          "clock crossing 2^32 ms on the way");
    for (unsigned i = 0; i < 3; i++) {
        unsigned frames;
        int g = grp_gap(s, i, &frames);
        CHECK(frames > 60 && g >= 5, "S12: node %u: %u group frames, never two within 5 ms (smallest gap %d ms)", i,
              frames, g);
    }
    bool once = true;
    for (unsigned sec = 0; sec < 20; sec++) {
        once = once && got(s, 1, (uint8_t)(0x10 + sec)) == 1 && got(s, 1, (uint8_t)(0x40 + sec)) == 1 &&
               got(s, 1, (uint8_t)(0x70 + sec)) == 1 && got(s, 2, (uint8_t)(0x10 + sec)) == 1 &&
               got(s, 2, (uint8_t)(0x40 + sec)) == 1 && got(s, 0, (uint8_t)(0x70 + sec)) == 1;
    }
    CHECK(once && bat_sim_soft_rx_count(s, 0) == 20 && bat_sim_soft_rx_count(s, 1) == 60 &&
          bat_sim_soft_rx_count(s, 2) == 40, "S12: every broadcast delivered exactly once at each other node");
    CHECK(cnt(s, 1, BAT_C_BC_FWD) == 60 && cnt(s, 1, BAT_C_BC_COPY_DROP) == 0 && cnt(s, 1, BAT_C_BC_QUEUE_FULL) == 0 &&
          cnt(s, 0, BAT_C_BC_COPY_DROP) == 0, "S12: W relays all 60 with every copy, nothing dropped at a queue");
    bat_sim_free(s);
}

/* A relayed burst over the copy queue: A (1 copy) sends 6 broadcasts in one ms, all reaching W (3 copies)
 * together. W delivers every one; it takes 5 (1 sent, 4 waiting), so the 6th is delivered at W but not
 * re-flooded, and C, beyond W, gets 5. */
static void s12b(void)
{
    uint8_t f[128];
    struct bat_sim *s = line(12, 100);
    bat_sim_cfg(s, 1)->bcast_copies = 3;
    bat_sim_cfg(s, 2)->bcast_copies = 3;
    start_all(s, 3);
    bat_sim_run(s, 5000);
    for (unsigned k = 0; k < 6; k++) {
        bat_sim_soft_tx(s, 0, f, bframe(s, 0, f, (uint8_t)(0x20 + k)));
    }
    bat_sim_run(s, 500);
    unsigned w = 0, c = 0;
    for (unsigned k = 0; k < 6; k++) {
        w += got(s, 1, (uint8_t)(0x20 + k)) == 1;
        c += (k < 5) == (got(s, 2, (uint8_t)(0x20 + k)) == 1);
    }
    CHECK(w == 6 && c == 6 && cnt(s, 1, BAT_C_BC_FWD) == 5 && cnt(s, 1, BAT_C_BC_QUEUE_FULL) == 1,
          "S12b: 6 relayed broadcasts in one ms: W delivers 6, re-floods the first 5 (bc_queue_full %u), C gets "
          "those 5", cnt(s, 1, BAT_C_BC_QUEUE_FULL));
    bat_sim_free(s);
}

/* Node 0 joins the running line 1 [- 2] @off ms into its 7th second; @p is the peer measured. Returns
 * ms from 0's first route to @p until it holds @p's table (-1: not within 10 s); *reqs = its TT
 * requests to @p. */
static int join_sync(uint32_t seed, unsigned n, unsigned p, uint32_t off, unsigned *reqs)
{
    struct bat_sim *s = bat_sim_new(n, seed);
    for (unsigned i = 0; i + 1 < n; i++) {
        bat_sim_link(s, i, i + 1, true, 100);
    }
    for (unsigned i = 1; i < n; i++) {
        bat_sim_start(s, i);
    }
    bat_sim_run(s, 6000 + off);
    bat_sim_capture(s, true);
    bat_sim_start(s, 0);
    struct bat *a = bat_sim_engine(s, 0);
    uint32_t t_route = 0;
    int d = -1;
    for (uint32_t k = 0; k < 10000 && d < 0; k++) {
        bat_sim_run(s, 1);
        struct bat_orig *o = orig(s, 0, p);
        if (o && !t_route && bat_route_nh(a, o, BAT_TBL_DEFAULT)) {
            t_route = bat_sim_now(s);
        }
        if (o && t_route && o->tt.known) {
            d = (int)(bat_sim_now(s) - t_route);
        }
    }
    *reqs = 0;
    for (unsigned k = 0; k < bat_sim_captured(s); k++) {
        const struct bat_sim_frame *f = bat_sim_captured_get(s, k);
        const uint8_t *u = f->bytes + BAT_ETH_HLEN;
        *reqs += f->from == 0 && f->len >= BAT_ETH_HLEN + BAT_UT_HLEN + BAT_TVLV_HLEN + 1 &&
                 u[BAT_OFF_TYPE] == BAT_PT_UTVLV && memcmp(u + BAT_UT_DEST, bat_sim_hard(s, p), 6) == 0 &&
                 (u[BAT_UT_HLEN + BAT_TVLV_HLEN] & 0x0F) == BAT_TT_REQUEST;
    }
    bat_sim_free(s);
    return d;
}

static void s13(void)
{
    for (unsigned hops = 1; hops <= 2; hops++) {
        unsigned slow = 0, many = 0, maxr = 0;
        int worst = 0;
        for (unsigned k = 0; k < 20; k++) {
            unsigned r;
            int d = join_sync(1300 + 20 * hops + k, hops + 1, hops, 50 * k, &r);
            slow += d < 0 || d > 1200;
            many += r > 2;
            worst = d < 0 || (worst >= 0 && d > worst) ? d : worst;
            maxr = r > maxr ? r : maxr;
        }
        CHECK(slow == 0 && many == 0,
              "S13: %s, 20 phases over one OGM period: every join holds %s's table within 1.2 s of the "
              "route, at most 2 requests (%u slow, worst %d ms, most requests %u)", hops == 1 ? "A joins B" :
              "A joins the line B - C", hops == 1 ? "B" : "C", slow, worst, maxr);
    }
}

static void clock_wrap(void)
{
    uint8_t f[128];
    struct bat_sim *s = line(12, 100);
    bat_sim_set_now(s, 0xFFFFFFFFu - 20000u);
    start_all(s, 3);
    bat_sim_run(s, 60000);
    CHECK(bat_sim_now(s) < 50000, "clock wrap: virtual time crossed 2^32 ms (now %u)", bat_sim_now(s));
    CHECK(routes_via(s, 0, 2, 1) && rtput(s, 0, 2) == 50 && routes_via(s, 2, 0, 1),
          "clock wrap: S2 routes converge and hold across the wrap");
    int quiet = 1;
    for (unsigned i = 0; i < 3; i++) {
        quiet &= cnt(s, i, BAT_C_ORIG_PURGED) == 0 && cnt(s, i, BAT_C_ROUTE_LOST) == 0 &&
                 cnt(s, i, BAT_C_NEIGH_PURGED) == 0 && cnt(s, i, BAT_C_OGM_RESTART_BLOCKED) == 0;
    }
    CHECK(quiet, "clock wrap: nothing purged, lost or blocked");
    CHECK(cnt(s, 1, BAT_C_ELP_TX) >= 115 && cnt(s, 1, BAT_C_ELP_TX) <= 125 && cnt(s, 1, BAT_C_OGM_TX_OWN) >= 58,
          "clock wrap: timers keep their period (%u ELPs, %u OGMs in 60 s)", cnt(s, 1, BAT_C_ELP_TX),
          cnt(s, 1, BAT_C_OGM_TX_OWN));
    size_t n = bframe(s, 0, f, 0x77);
    bat_sim_soft_tx(s, 0, f, n);
    bat_sim_run(s, 50);
    CHECK(bat_sim_soft_rx_count(s, 2) == 1, "clock wrap: a broadcast still crosses the line");
    bat_sim_link(s, 1, 2, false, 0);
    bat_sim_run(s, 202000);
    CHECK(!nh(s, 0, 2, BAT_TBL_DEFAULT) && routes_via(s, 0, 1, 1), "clock wrap: expiry still works afterwards");
    bat_sim_free(s);
}

static void independence(void)
{
    struct bat_sim *s = bat_sim_new(2, 3);
    bat_sim_link(s, 0, 1, true, 100);
    bat_sim_start(s, 0);
    bat_sim_run(s, 2000);
    CHECK(cnt(s, 0, BAT_C_ELP_TX) >= 3 && bat_sim_counter(s, 1, BAT_C_ELP_RX) == 0 &&
          bat_neigh_count(bat_sim_engine(s, 0)) == 0, "two engines in one process: an unstarted one sees nothing");
    bat_sim_start(s, 1);
    bat_sim_run(s, 3000);
    CHECK(cnt(s, 1, BAT_C_ELP_RX) >= 4 && routes_via(s, 1, 0, 0) && routes_via(s, 0, 1, 1),
          "two engines in one process: each keeps its own state");
    bat_sim_stop(s, 1);
    uint32_t e = cnt(s, 1, BAT_C_ELP_TX);
    bat_sim_run(s, 2000);
    CHECK(cnt(s, 1, BAT_C_ELP_TX) == e, "a stopped node is not ticked");
    bat_sim_free(s);
}

int main(void)
{
    independence();
    s1();
    s2();
    s5();
    s7();
    s7b();
    s8();
    s9();
    s10();
    s11();
    s12();
    s12b();
    s13();
    clock_wrap();
    if (failures) {
        printf("test_bat_sim: %d FAILED\n", failures);
        return 1;
    }
    printf("test_bat_sim: all passed\n");
    return 0;
}
