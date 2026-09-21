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
                                    .link_metric = 100, .path_lifetime_ms = 5120,
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

    /* ---- garbage in --------------------------------------------------- */
    CHECK(umac_mesh_hwmp_relay(&c, preq, 3, A, &act, &why) == 0 && why == UMAC_MESH_HWMP_DROP_PARSE, "3-octet body refused");
    CHECK(umac_mesh_hwmp_relay(NULL, preq, HWMP_PREQ_BODY_LEN, A, &act, &why) == 0, "NULL ctx refused");
    preq[2] = 200;
    CHECK(umac_mesh_hwmp_relay(&c, preq, HWMP_PREQ_BODY_LEN, A, &act, &why) == 0, "unknown element refused");

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_mesh_hwmp_relay: all passed\n");
    return 0;
}
