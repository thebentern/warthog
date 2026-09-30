/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * HWMP relay decisions (morselib src/umac/mesh/umac_mesh_hwmp_relay.c).
 *
 * Three nodes in a line, A - W - B, with W the warthog under test. What is
 * pinned: a PREQ from A for B is rebroadcast by W with the hop cost added and
 * the path back to A installed via A; the same PREQ arriving again is neither
 * answered nor forwarded (duplicate suppression, and the reason a forged stale
 * PREQ changes nothing); B's PREP comes back through W to A along the path the
 * PREQ built; a PREQ for W itself is answered, never forwarded; nothing is
 * forwarded with forwarding off; TTL dies at the relay; a PERR from the wrong
 * side is ignored and a PERR that changed nothing is not echoed.
 *
 * Path lifetime is the PREQ/PREP Lifetime field, as in mac80211's
 * hwmp_route_info_get(): TU to ms rounded down (48828 TU is 49999 ms, 1074
 * TU 1099 ms), for the PREQ originator and the PREP target alike, only ever
 * extended, and capped at UMAC_MESH_PATH_LIFETIME_MAX_MS -- including a TU
 * count whose ms value does not fit 32 bits (0x00400000 TU * 1024 is 2^32).
 * A full table refuses a new destination as TABLE_FULL -- a PREQ's
 * originator, a relayed PREP's target, the target of the PREP answering our
 * own PREQ -- which a duplicate (NOT_FRESH, including a repeat of that last
 * PREP) never is, and a slot that lapses is reusable.
 */
#include "umac_mesh_hwmp_relay.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static const uint8_t A[6] = { 0x02, 0, 0, 0, 0, 0xaa };
static const uint8_t W[6] = { 0x02, 0, 0, 0, 0, 0x77 };
static const uint8_t B[6] = { 0x02, 0, 0, 0, 0, 0xbb };
static const uint8_t Z[6] = { 0x02, 0, 0, 0, 0, 0x99 }; /* a node not on any path */
static const uint8_t BC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static struct umac_mesh_pathtbl T;
static uint32_t own_sn = 40;

static struct umac_mesh_hwmp_ctx ctx(bool fwd, uint32_t now)
{
    struct umac_mesh_hwmp_ctx c = { .own_addr = W, .tbl = &T, .forwarding = fwd,
                                    .link_metric = 100,
                                    .max_lifetime_ms = UMAC_MESH_PATH_LIFETIME_MAX_MS,
                                    .now_ms = now, .own_sn = &own_sn };
    return c;
}

int main(void)
{
    struct umac_mesh_hwmp_action act; enum umac_mesh_hwmp_drop why;
    uint8_t preq[64], prep[64], perr[64];
    uint32_t now = 1000;
    umac_mesh_pathtbl_init(&T);

    /* ---- A asks for B; W relays --------------------------------------- */
    umac_mesh_hwmp_build_preq(preq, sizeof(preq), A, 500, 1, B, 4882);
    struct umac_mesh_hwmp_ctx c = ctx(true, now);
    uint8_t eid = umac_mesh_hwmp_relay(&c, preq, HWMP_PREQ_BODY_LEN, A, &act, &why);
    CHECK(eid == HWMP_EID_PREQ, "PREQ recognised");
    CHECK(act.kind == UMAC_MESH_HWMP_REBROADCAST_PREQ, "PREQ for B is rebroadcast");
    CHECK(memcmp(act.to, BC, 6) == 0, "to broadcast");
    {
        struct hwmp_preq q;
        CHECK(umac_mesh_hwmp_parse_preq(act.body, act.body_len, &q), "rebroadcast parses");
        CHECK(q.hop_count == 1 && q.ttl == HWMP_DEFAULT_TTL - 1, "hop+1 ttl-1 (hop %u ttl %u)", q.hop_count, q.ttl);
        CHECK(q.metric == 100, "metric carries W's link cost (got %u)", (unsigned)q.metric);
        CHECK(memcmp(q.orig_addr, A, 6) == 0 && q.orig_sn == 500 && q.preq_id == 1, "originator fields verbatim");
        CHECK(memcmp(q.target_addr, B, 6) == 0, "target verbatim");
    }
    const struct umac_mesh_path *p = umac_mesh_path_lookup(&T, A, now);
    CHECK(p != NULL && memcmp(p->next_hop, A, 6) == 0 && p->sn == 500 && p->metric == 100,
          "path to A installed via A, sn 500, metric 100");

    /* ---- the same PREQ again: duplicate suppression -------------------- */
    eid = umac_mesh_hwmp_relay(&c, preq, HWMP_PREQ_BODY_LEN, A, &act, &why);
    CHECK(act.kind == UMAC_MESH_HWMP_NONE && why == UMAC_MESH_HWMP_DROP_NOT_FRESH,
          "the same PREQ again is not forwarded (why=%d)", (int)why);
    /* Via another neighbour with a worse metric: also not fresh. */
    {
        struct hwmp_preq q; uint8_t via[64];
        umac_mesh_hwmp_parse_preq(preq, HWMP_PREQ_BODY_LEN, &q);
        q.hop_count = 3; q.ttl = 20; q.metric = 5000;
        umac_mesh_hwmp_build_preq_fwd(via, sizeof(via), &q, 0);
        umac_mesh_hwmp_relay(&c, via, HWMP_PREQ_BODY_LEN, Z, &act, &why);
        CHECK(act.kind == UMAC_MESH_HWMP_NONE && why == UMAC_MESH_HWMP_DROP_NOT_FRESH,
              "the same request via a worse route is not forwarded either");
        p = umac_mesh_path_lookup(&T, A, now);
        CHECK(memcmp(p->next_hop, A, 6) == 0, "and the path to A still points at A");
    }
    /* A forged PREQ claiming to be A with a STALE sn changes nothing. */
    {
        uint8_t forged[64];
        umac_mesh_hwmp_build_preq(forged, sizeof(forged), A, 499, 9, B, 4882);
        umac_mesh_hwmp_relay(&c, forged, HWMP_PREQ_BODY_LEN, Z, &act, &why);
        CHECK(act.kind == UMAC_MESH_HWMP_NONE && why == UMAC_MESH_HWMP_DROP_NOT_FRESH, "forged stale PREQ is dropped");
        p = umac_mesh_path_lookup(&T, A, now);
        CHECK(memcmp(p->next_hop, A, 6) == 0, "path to A unchanged by the forgery");
    }

    /* ---- B answers; W carries the PREP back to A ----------------------- */
    {
        struct hwmp_preq q;
        umac_mesh_hwmp_parse_preq(act.body[0] ? preq : preq, HWMP_PREQ_BODY_LEN, &q); /* the original */
        /* B builds the PREP from the (relayed) PREQ; sn 700 is B's own. */
        umac_mesh_hwmp_build_prep(prep, sizeof(prep), &q, B, 700);
    }
    eid = umac_mesh_hwmp_relay(&c, prep, HWMP_PREP_BODY_LEN, B, &act, &why);
    CHECK(eid == HWMP_EID_PREP, "PREP recognised");
    CHECK(act.kind == UMAC_MESH_HWMP_FORWARD_PREP, "PREP for A is forwarded (why=%d)", (int)why);
    CHECK(memcmp(act.to, A, 6) == 0, "to A -- the next hop toward the originator");
    {
        struct hwmp_prep pp;
        CHECK(umac_mesh_hwmp_parse_prep(act.body, act.body_len, &pp), "forwarded PREP parses");
        CHECK(pp.hop_count == 1 && pp.metric == 100, "PREP hop+1, metric+link");
        CHECK(memcmp(pp.target_addr, B, 6) == 0 && pp.target_sn == 700, "target B verbatim");
        CHECK(memcmp(pp.orig_addr, A, 6) == 0 && pp.orig_sn == 500, "originator A verbatim");
    }
    p = umac_mesh_path_lookup(&T, B, now);
    CHECK(p != NULL && memcmp(p->next_hop, B, 6) == 0 && p->sn == 700, "path to B installed via B from the PREP");
    /* W now has both directions: A via A, B via B. That is the relay. */
    CHECK(umac_mesh_path_count(&T, now) == 2, "two paths: the relay is complete");

    /* ---- a PREP whose originator we have no path to is dropped -------- */
    {
        uint8_t q2[64], p2[64]; struct hwmp_preq qq;
        umac_mesh_hwmp_build_preq(q2, sizeof(q2), Z, 1, 1, B, 4882);
        umac_mesh_hwmp_parse_preq(q2, HWMP_PREQ_BODY_LEN, &qq);
        umac_mesh_hwmp_build_prep(p2, sizeof(p2), &qq, B, 701);
        umac_mesh_hwmp_relay(&c, p2, HWMP_PREP_BODY_LEN, B, &act, &why);
        CHECK(act.kind == UMAC_MESH_HWMP_NONE && why == UMAC_MESH_HWMP_DROP_NO_PATH,
              "PREP toward an unknown originator has nowhere to go (why=%d)", (int)why);
    }

    /* ---- a PREQ for W itself: answered, never forwarded ---------------- */
    {
        uint8_t q3[64];
        umac_mesh_hwmp_build_preq(q3, sizeof(q3), A, 501, 2, W, 4882);
        uint32_t sn_before = own_sn;
        umac_mesh_hwmp_relay(&c, q3, HWMP_PREQ_BODY_LEN, A, &act, &why);
        CHECK(act.kind == UMAC_MESH_HWMP_SEND_PREP, "PREQ for us is answered with a PREP");
        CHECK(memcmp(act.to, A, 6) == 0, "to the transmitter");
        CHECK(own_sn == sn_before + 1, "our sn advanced (USN set, so just +1)");
        struct hwmp_prep pp;
        umac_mesh_hwmp_parse_prep(act.body, act.body_len, &pp);
        CHECK(memcmp(pp.target_addr, W, 6) == 0 && pp.target_sn == own_sn, "PREP target is us at our sn");
        /* With forwarding OFF the answer still happens: answering is what a leaf does. */
        struct umac_mesh_hwmp_ctx leaf = ctx(false, now);
        uint8_t q4[64];
        umac_mesh_hwmp_build_preq(q4, sizeof(q4), A, 502, 3, W, 4882);
        umac_mesh_hwmp_relay(&leaf, q4, HWMP_PREQ_BODY_LEN, A, &act, &why);
        CHECK(act.kind == UMAC_MESH_HWMP_SEND_PREP, "a leaf still answers a PREQ for itself");
    }

    /* ---- forwarding off: nothing for third parties --------------------- */
    {
        struct umac_mesh_hwmp_ctx leaf = ctx(false, now);
        uint8_t q5[64];
        umac_mesh_hwmp_build_preq(q5, sizeof(q5), A, 503, 4, B, 4882);
        umac_mesh_hwmp_relay(&leaf, q5, HWMP_PREQ_BODY_LEN, A, &act, &why);
        CHECK(act.kind == UMAC_MESH_HWMP_NONE && why == UMAC_MESH_HWMP_DROP_NO_FWD, "leaf does not forward a PREQ for B");
        p = umac_mesh_path_lookup(&T, A, now);
        CHECK(p != NULL && p->sn == 503, "but it still learned the path to A from it");
    }

    /* ---- TTL dies at the relay ----------------------------------------- */
    {
        struct hwmp_preq q; uint8_t low[64];
        umac_mesh_hwmp_build_preq(low, sizeof(low), A, 600, 5, B, 4882);
        umac_mesh_hwmp_parse_preq(low, HWMP_PREQ_BODY_LEN, &q);
        q.ttl = 1;
        umac_mesh_hwmp_build_preq_fwd(low, sizeof(low), &q, 0); /* refuses; build by hand below */
        low[6] = 1; /* force ttl 1 into a fresh PREQ */
        umac_mesh_hwmp_build_preq(low, sizeof(low), A, 600, 5, B, 4882); low[6] = 1;
        umac_mesh_hwmp_relay(&c, low, HWMP_PREQ_BODY_LEN, A, &act, &why);
        CHECK(act.kind == UMAC_MESH_HWMP_NONE && why == UMAC_MESH_HWMP_DROP_TTL, "PREQ at ttl 1 is not forwarded (why=%d)", (int)why);
        p = umac_mesh_path_lookup(&T, A, now);
        CHECK(p != NULL && p->sn == 600, "yet the path to A was still learned from it");
    }

    /* ---- PERR ---------------------------------------------------------- */
    umac_mesh_pathtbl_init(&T);
    umac_mesh_path_update(&T, B, B, 700, 100, 1, 5120, now); /* B via B */
    umac_mesh_hwmp_build_perr(perr, sizeof(perr), 31, B, 701, HWMP_REASON_MESH_PATH_ERROR_DEST_UNREACHABLE);
    umac_mesh_hwmp_relay(&c, perr, HWMP_PERR_BODY_LEN, Z, &act, &why);
    CHECK(act.kind == UMAC_MESH_HWMP_NONE && why == UMAC_MESH_HWMP_DROP_UNCHANGED, "PERR for B from Z (not our next hop) is ignored");
    CHECK(umac_mesh_path_lookup(&T, B, now) != NULL, "path to B survives a third party's PERR");
    eid = umac_mesh_hwmp_relay(&c, perr, HWMP_PERR_BODY_LEN, B, &act, &why);
    CHECK(eid == HWMP_EID_PERR && act.kind == UMAC_MESH_HWMP_FORWARD_PERR, "PERR from B (our next hop) is acted on and forwarded");
    CHECK(memcmp(act.to, BC, 6) == 0, "forwarded PERR goes to broadcast");
    {
        struct hwmp_perr e;
        umac_mesh_hwmp_parse_perr(act.body, act.body_len, &e);
        CHECK(e.ttl == 30 && e.dest_sn == 701 && memcmp(e.dest_addr, B, 6) == 0, "forwarded PERR: ttl-1, dest and sn verbatim");
    }
    CHECK(umac_mesh_path_lookup(&T, B, now) == NULL, "path to B is gone");
    umac_mesh_hwmp_relay(&c, perr, HWMP_PERR_BODY_LEN, B, &act, &why);
    CHECK(act.kind == UMAC_MESH_HWMP_NONE && why == UMAC_MESH_HWMP_DROP_UNCHANGED, "the same PERR again changes nothing and is NOT re-forwarded");
    {
        struct umac_mesh_hwmp_ctx leaf = ctx(false, now);
        umac_mesh_path_update(&T, B, B, 800, 100, 1, 5120, now);
        umac_mesh_hwmp_build_perr(perr, sizeof(perr), 31, B, 801, 63);
        umac_mesh_hwmp_relay(&leaf, perr, HWMP_PERR_BODY_LEN, B, &act, &why);
        CHECK(act.kind == UMAC_MESH_HWMP_NONE && why == UMAC_MESH_HWMP_DROP_NO_FWD, "a leaf applies the PERR but forwards nothing");
        CHECK(umac_mesh_path_lookup(&T, B, now) == NULL, "and its path is gone");
    }

    /* ---- losing a neighbour ---------------------------------------------- */
    umac_mesh_pathtbl_init(&T);
    umac_mesh_path_update(&T, A, A, 1, 1, 1, 5120, now);
    umac_mesh_path_update(&T, B, B, 2, 1, 1, 5120, now);
    umac_mesh_path_update(&T, Z, B, 3, 2, 2, 5120, now); /* Z reached via B */
    {
        struct umac_mesh_hwmp_action acts[4];
        uint32_t n = umac_mesh_hwmp_lose_neighbour(&c, B, acts, 4);
        CHECK(n == 2, "losing B yields two PERRs, for B and for Z-via-B (got %u)", (unsigned)n);
        struct hwmp_perr e0, e1;
        umac_mesh_hwmp_parse_perr(acts[0].body, acts[0].body_len, &e0);
        umac_mesh_hwmp_parse_perr(acts[1].body, acts[1].body_len, &e1);
        CHECK((memcmp(e0.dest_addr, B, 6) == 0 && memcmp(e1.dest_addr, Z, 6) == 0) ||
              (memcmp(e0.dest_addr, Z, 6) == 0 && memcmp(e1.dest_addr, B, 6) == 0), "PERRs name B and Z");
        /* B was at sn 2, Z at 3: the PERRs must say 3 and 4, or a neighbour
         * holding the same numbers rejects them as stale. */
        uint32_t snB = memcmp(e0.dest_addr, B, 6) == 0 ? e0.dest_sn : e1.dest_sn;
        uint32_t snZ = memcmp(e0.dest_addr, Z, 6) == 0 ? e0.dest_sn : e1.dest_sn;
        CHECK(snB == 3 && snZ == 4, "PERRs carry sn + 1 (B %u, Z %u)", (unsigned)snB, (unsigned)snZ);
        CHECK(umac_mesh_path_lookup(&T, A, now) != NULL, "path via A untouched");
        CHECK(umac_mesh_path_count(&T, now) == 1, "one path left");
        struct umac_mesh_hwmp_ctx leaf = ctx(false, now);
        umac_mesh_path_update(&T, B, B, 4, 1, 1, 5120, now);
        CHECK(umac_mesh_hwmp_lose_neighbour(&leaf, B, acts, 4) == 0, "a leaf drops the path but emits no PERR");
        CHECK(umac_mesh_path_lookup(&T, B, now) == NULL, "and the path is dropped");
    }

    /* ---- the element's Lifetime is the path's lifetime ------------------ */
    {
        static const uint8_t L1[6] = { 0x02, 0, 0, 0, 0, 0x31 };
        static const uint8_t L2[6] = { 0x02, 0, 0, 0, 0, 0x32 };
        static const uint8_t X[6]  = { 0x02, 0, 0, 0, 0, 0x33 };
        static const uint8_t F[6]  = { 0x02, 0, 0, 0, 0, 0x34 };
        static const uint8_t V[6]  = { 0x02, 0, 0, 0, 0, 0x35 };
        static const uint8_t N[6]  = { 0x02, 0, 0, 0, 0, 0x36 };
        umac_mesh_pathtbl_init(&T);
        const uint32_t t0 = 70000;
        struct umac_mesh_hwmp_ctx lc = ctx(true, t0);
        umac_mesh_hwmp_build_preq(preq, sizeof(preq), L1, 900, 7, L2, 48828u);
        umac_mesh_hwmp_relay(&lc, preq, HWMP_PREQ_BODY_LEN, L1, &act, &why);
        const struct umac_mesh_path *pl = umac_mesh_path_lookup(&T, L1, t0);
        CHECK(pl != NULL && pl->exp_ms == t0 + 49999u,
              "PREQ at 48828 TU: the path to its originator lives 49999 ms (got %ld)",
              pl != NULL ? (long)(pl->exp_ms - t0) : -1L);
        struct hwmp_preq q;
        umac_mesh_hwmp_parse_preq(preq, HWMP_PREQ_BODY_LEN, &q);
        umac_mesh_hwmp_build_prep(prep, sizeof(prep), &q, L2, 60);
        lc.now_ms = t0 + 20;
        umac_mesh_hwmp_relay(&lc, prep, HWMP_PREP_BODY_LEN, X, &act, &why);
        struct hwmp_prep r;
        bool fwd_ok = act.kind == UMAC_MESH_HWMP_FORWARD_PREP &&
                      umac_mesh_hwmp_parse_prep(act.body, act.body_len, &r) && r.lifetime == 48828u;
        pl = umac_mesh_path_lookup(&T, L2, t0 + 20);
        CHECK(fwd_ok && pl != NULL && pl->exp_ms == t0 + 20 + 49999u &&
                  memcmp(pl->next_hop, X, 6) == 0,
              "PREP at 48828 TU: carried back unchanged, and the path to its target via X "
              "lives 49999 ms (got %ld)",
              pl != NULL ? (long)(pl->exp_ms - t0 - 20) : -1L);
        /* mesh11sd's 1100 ms mobility setting: shorter than any local default. */
        umac_mesh_hwmp_build_preq(preq, sizeof(preq), F, 5, 8, L2, 1074u);
        lc.now_ms = t0 + 100;
        umac_mesh_hwmp_relay(&lc, preq, HWMP_PREQ_BODY_LEN, F, &act, &why);
        pl = umac_mesh_path_lookup(&T, F, t0 + 100);
        CHECK(pl != NULL && pl->exp_ms == t0 + 100 + 1099u,
              "1074 TU gives 1099 ms, not a local constant (got %ld)",
              pl != NULL ? (long)(pl->exp_ms - t0 - 100) : -1L);
        /* HWMP is unauthenticated and live paths are never evicted: a forged
         * lifetime must not pin a slot for weeks. 59000 TU is under the cap in
         * TU but 60416 ms; 0x00400000 TU wraps to 0 ms in 32-bit arithmetic. */
        const uint32_t big[3] = { 59000u, 0x00400000u, 0xffffffffu };
        const uint8_t *who[3] = { Z, V, N };
        for (unsigned k = 0; k < 3u; k++)
        {
            umac_mesh_hwmp_build_preq(preq, sizeof(preq), who[k], 5, 9 + k, L2, big[k]);
            lc.now_ms = t0 + 200 + k;
            umac_mesh_hwmp_relay(&lc, preq, HWMP_PREQ_BODY_LEN, who[k], &act, &why);
            pl = umac_mesh_path_lookup(&T, who[k], lc.now_ms);
            CHECK(pl != NULL && pl->exp_ms == lc.now_ms + UMAC_MESH_PATH_LIFETIME_MAX_MS,
                  "lifetime %lu TU is capped at %u ms (got %ld)", (unsigned long)big[k],
                  UMAC_MESH_PATH_LIFETIME_MAX_MS,
                  pl != NULL ? (long)(pl->exp_ms - lc.now_ms) : -1L);
        }
        /* Only ever extended: a later, shorter advertisement does not cut it. */
        umac_mesh_hwmp_build_preq(preq, sizeof(preq), L1, 901, 10, L2, 1074u);
        lc.now_ms = t0 + 300;
        umac_mesh_hwmp_relay(&lc, preq, HWMP_PREQ_BODY_LEN, L1, &act, &why);
        pl = umac_mesh_path_lookup(&T, L1, t0 + 300);
        CHECK(pl != NULL && pl->sn == 901 && pl->exp_ms == t0 + 49999u,
              "a fresher PREQ with a shorter lifetime does not shorten the path");
        _Static_assert(UMAC_MESH_PATH_LIFETIME_MAX_MS == 60000u,
                       "the cap is 60 s: above OpenMANET 1.8.0's 48828 TU, and no higher");
    }

    /* ---- a full table ---------------------------------------------------- */
    /* Live paths are never evicted, so UMAC_MESH_PATH_MAX live originators
     * lock out the next one -- even a PREQ for us. The reason must say so. */
    {
        static const uint8_t NEWO[6] = { 0x02, 0, 0, 0, 0x71, 0x01 };
        static const uint8_t NT[6]   = { 0x02, 0, 0, 0, 0x71, 0x02 };
        umac_mesh_pathtbl_init(&T);
        const uint32_t t1 = 200000;
        struct umac_mesh_hwmp_ctx fc = ctx(true, t1);
        bool all = true;
        for (uint32_t i = 0; i < UMAC_MESH_PATH_MAX; i++)
        {
            const uint8_t o[6] = { 0x02, 0, 0, 0, 0x70, (uint8_t)i };
            umac_mesh_hwmp_build_preq(preq, sizeof(preq), o, 1, i, B, 48828u);
            umac_mesh_hwmp_relay(&fc, preq, HWMP_PREQ_BODY_LEN, A, &act, &why);
            all = all && act.kind == UMAC_MESH_HWMP_REBROADCAST_PREQ;
        }
        CHECK(all && umac_mesh_path_count(&T, t1) == UMAC_MESH_PATH_MAX,
              "%u originators fill the table", (unsigned)UMAC_MESH_PATH_MAX);
        uint32_t sn_before = own_sn;
        umac_mesh_hwmp_build_preq(preq, sizeof(preq), NEWO, 1, 50, W, 48828u);
        umac_mesh_hwmp_relay(&fc, preq, HWMP_PREQ_BODY_LEN, A, &act, &why);
        CHECK(act.kind == UMAC_MESH_HWMP_NONE && why == UMAC_MESH_HWMP_DROP_TABLE_FULL &&
                  own_sn == sn_before,
              "one more originator asking for us: unanswered, TABLE_FULL (why=%d)", (int)why);
        const uint8_t o0[6] = { 0x02, 0, 0, 0, 0x70, 0 };
        umac_mesh_hwmp_build_preq(preq, sizeof(preq), o0, 1, 0, B, 48828u);
        umac_mesh_hwmp_relay(&fc, preq, HWMP_PREQ_BODY_LEN, A, &act, &why);
        CHECK(act.kind == UMAC_MESH_HWMP_NONE && why == UMAC_MESH_HWMP_DROP_NOT_FRESH,
              "a known originator's repeat is still NOT_FRESH (why=%d)", (int)why);
        /* The PREP answering our own PREQ installs nothing either. */
        struct hwmp_preq q;
        umac_mesh_hwmp_build_preq(preq, sizeof(preq), W, 60, 51, NT, 48828u);
        umac_mesh_hwmp_parse_preq(preq, HWMP_PREQ_BODY_LEN, &q);
        umac_mesh_hwmp_build_prep(prep, sizeof(prep), &q, NT, 3);
        umac_mesh_hwmp_relay(&fc, prep, HWMP_PREP_BODY_LEN, B, &act, &why);
        CHECK(why == UMAC_MESH_HWMP_DROP_TABLE_FULL && umac_mesh_path_lookup(&T, NT, t1) == NULL,
              "the PREP for our own request: TABLE_FULL, no path (why=%d)", (int)why);
        /* A PREP we would relay back to a live originator, naming a new
         * target: refused for the same reason, and nothing forwarded. */
        {
            static const uint8_t NT3[6] = { 0x02, 0, 0, 0, 0x71, 0x03 };
            const uint8_t o3[6] = { 0x02, 0, 0, 0, 0x70, 3 };
            struct hwmp_preq q3;
            umac_mesh_hwmp_build_preq(preq, sizeof(preq), o3, 1, 3, NT3, 48828u);
            umac_mesh_hwmp_parse_preq(preq, HWMP_PREQ_BODY_LEN, &q3);
            umac_mesh_hwmp_build_prep(prep, sizeof(prep), &q3, NT3, 9);
            umac_mesh_hwmp_relay(&fc, prep, HWMP_PREP_BODY_LEN, B, &act, &why);
            CHECK(act.kind == UMAC_MESH_HWMP_NONE && why == UMAC_MESH_HWMP_DROP_TABLE_FULL &&
                      umac_mesh_path_lookup(&T, NT3, t1) == NULL,
                  "a relayed PREP naming a new target: TABLE_FULL, not forwarded (why=%d)", (int)why);
        }
        fc.now_ms = t1 + 49999u;
        umac_mesh_hwmp_build_preq(preq, sizeof(preq), NEWO, 2, 52, W, 48828u);
        umac_mesh_hwmp_relay(&fc, preq, HWMP_PREQ_BODY_LEN, A, &act, &why);
        CHECK(act.kind == UMAC_MESH_HWMP_SEND_PREP,
              "once the originators' 49999 ms have passed, it is answered (why=%d)", (int)why);
    }

    /* ---- a repeated answer to our own PREQ is a duplicate ---------------- */
    /* Heard via two relays or retransmitted, the second copy names a target
     * we now hold a live path to: NOT_FRESH, never a full table. */
    {
        static const uint8_t NT4[6] = { 0x02, 0, 0, 0, 0x71, 0x04 };
        umac_mesh_pathtbl_init(&T);
        struct umac_mesh_hwmp_ctx dc = ctx(true, 300000);
        struct hwmp_preq q4;
        umac_mesh_hwmp_build_preq(preq, sizeof(preq), W, 70, 61, NT4, 4882u);
        umac_mesh_hwmp_parse_preq(preq, HWMP_PREQ_BODY_LEN, &q4);
        umac_mesh_hwmp_build_prep(prep, sizeof(prep), &q4, NT4, 5);
        umac_mesh_hwmp_relay(&dc, prep, HWMP_PREP_BODY_LEN, B, &act, &why);
        CHECK(why == UMAC_MESH_HWMP_DROP_NONE && umac_mesh_path_lookup(&T, NT4, 300000) != NULL,
              "the PREP answering our own PREQ installs the path (why=%d)", (int)why);
        umac_mesh_hwmp_relay(&dc, prep, HWMP_PREP_BODY_LEN, B, &act, &why);
        CHECK(act.kind == UMAC_MESH_HWMP_NONE && why == UMAC_MESH_HWMP_DROP_NOT_FRESH,
              "the same PREP again: NOT_FRESH, not TABLE_FULL (why=%d)", (int)why);
    }

    /* ---- garbage in --------------------------------------------------- */
    CHECK(umac_mesh_hwmp_relay(&c, preq, 3, A, &act, &why) == 0 && why == UMAC_MESH_HWMP_DROP_PARSE, "3-octet body refused");
    CHECK(umac_mesh_hwmp_relay(NULL, preq, HWMP_PREQ_BODY_LEN, A, &act, &why) == 0, "NULL ctx refused");
    preq[2] = 200;
    CHECK(umac_mesh_hwmp_relay(&c, preq, HWMP_PREQ_BODY_LEN, A, &act, &why) == 0, "unknown element refused");

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_mesh_hwmp_relay: all passed\n");
    return 0;
}
