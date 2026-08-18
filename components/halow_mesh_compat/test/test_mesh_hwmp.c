/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * HWMP PREQ/PREP byte layout (morselib src/umac/mesh/umac_mesh_hwmp.c).
 *
 * These offsets are the whole feature. A PREQ whose element length octet still
 * says 37 but whose target address is two bytes short is accepted by mac80211
 * and installs a path to a MAC that does not exist -- there is no error, no
 * log, and no counter anywhere on either side. Two independent reviews of the
 * spec this was written from placed target_sn at +35 (overlapping the target
 * address) and the PREP originator at +23 (overlapping the metric); both
 * mistakes sum to a body two bytes short while every length octet still reads
 * correct. That is precisely the class of bug that only a golden test catches,
 * so every field is pinned at an absolute offset here rather than checked as a
 * round trip -- a round trip agrees with itself no matter how wrong it is.
 */
#include "umac_mesh_hwmp.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static const uint8_t US[6]  = { 0x3c, 0x1a, 0xcc, 0x4c, 0x83, 0xa5 }; /* a warthog */
static const uint8_t PI[6]  = { 0xe4, 0x5f, 0x01, 0x28, 0xbf, 0x74 }; /* the OpenMANET node */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(void)
{
    uint8_t buf[64];

    /* ---- sequence numbers ------------------------------------------- */
    /* Unambiguous anchors first, so an implementation that is inverted
     * cannot pass on the wrap case alone. */
    CHECK(hwmp_sn_gt(2, 1), "2 is newer than 1");
    CHECK(!hwmp_sn_gt(1, 2), "1 is not newer than 2");
    CHECK(!hwmp_sn_gt(7, 7), "a sequence number is not newer than itself");
    /* Wrap: 0 is one PAST 0xffffffff, not four billion behind it. Backwards
     * here means rejecting every fresh path after the first wrap. */
    CHECK(hwmp_sn_gt(0, 0xffffffffu), "0 is newer than 0xffffffff (wrap)");
    CHECK(!hwmp_sn_gt(0xffffffffu, 0), "0xffffffff is not newer than 0 (wrap)");

    /* ---- PREQ build -------------------------------------------------- */
    memset(buf, 0xa5, sizeof(buf));
    uint16_t n = umac_mesh_hwmp_build_preq(buf, sizeof(buf), US, 0x11223344u, 0x55667788u, PI,
                                           4882u);
    CHECK(n == 41, "PREQ action body is 41 bytes");
    CHECK(buf[0] == 13, "category is 13 (mesh action)");
    CHECK(buf[1] == 1, "action code is 1 (HWMP path selection)");
    CHECK(buf[2] == 130, "element id is 130 (PREQ), not 114 which is Mesh ID");
    CHECK(buf[3] == 37, "element length octet is exactly 37");
    CHECK(buf[4] == 0x00, "top-level flags clear: no gate, no proactive PREP, no AE");
    CHECK(buf[5] == 0, "hop count starts at 0; the receiver adds one");
    CHECK(buf[6] == 31, "element TTL is 31");
    CHECK(rd32(&buf[7]) == 0x55667788u, "path discovery id at +7, little-endian");
    CHECK(memcmp(&buf[11], US, 6) == 0, "originator address at +11");
    CHECK(rd32(&buf[17]) == 0x11223344u, "originator sequence number at +17");
    CHECK(rd32(&buf[21]) == 4882u, "lifetime at +21");
    CHECK(rd32(&buf[25]) == 0, "metric at +25 starts at zero");
    CHECK(buf[29] == 1, "target count at +29 is exactly 1");
    CHECK(buf[30] == 0x05, "per-target flags at +30 are TO|USN, a separate octet from +4");
    /* The two that were wrong in the spec: the target address must occupy
     * +31..+36 and the sequence number must start at +37, not +35. */
    CHECK(memcmp(&buf[31], PI, 6) == 0, "target address occupies +31..+36 intact");
    CHECK(buf[36] == PI[5], "the last octet of the target address is NOT overwritten");
    CHECK(rd32(&buf[37]) == 0, "target sequence number at +37, clear of the address");
    CHECK(buf[41] == 0xa5, "nothing is written past the 41st byte");

    /* A short buffer must be refused, not partially filled. */
    memset(buf, 0xa5, sizeof(buf));
    CHECK(umac_mesh_hwmp_build_preq(buf, 40, US, 1, 1, PI, 100) == 0 && buf[0] == 0xa5,
          "a buffer one byte short is refused and left untouched");
    CHECK(umac_mesh_hwmp_build_preq(NULL, 64, US, 1, 1, PI, 100) == 0, "NULL output is refused");
    CHECK(umac_mesh_hwmp_build_preq(buf, sizeof(buf), US, 1, 1, NULL, 100) == 0,
          "a NULL target is refused");

    /* ---- PREQ parse round trip --------------------------------------- */
    struct hwmp_preq p;
    umac_mesh_hwmp_build_preq(buf, sizeof(buf), PI, 0xdeadbeefu, 0x01020304u, US, 1000u);
    CHECK(umac_mesh_hwmp_parse_preq(buf, 41, &p), "a PREQ we built parses");
    CHECK(memcmp(p.orig_addr, PI, 6) == 0 && p.orig_sn == 0xdeadbeefu,
          "originator address and sequence number survive the round trip");
    CHECK(memcmp(p.target_addr, US, 6) == 0, "target address survives the round trip");
    CHECK(p.preq_id == 0x01020304u && p.lifetime == 1000u, "discovery id and lifetime survive");
    CHECK(p.target_count == 1 && p.target_flags == 0x05, "target count and flags survive");
    CHECK(umac_mesh_hwmp_targets_us(&p, US), "a PREQ for our address targets us");
    CHECK(!umac_mesh_hwmp_targets_us(&p, PI), "a PREQ for someone else does not");

    /* ---- PREQ parse rejection ---------------------------------------- */
    {
        uint8_t v[64];
        umac_mesh_hwmp_build_preq(v, sizeof(v), PI, 1, 1, US, 1000);
        CHECK(!umac_mesh_hwmp_parse_preq(v, 40, &p), "a body one byte short is refused");
        CHECK(!umac_mesh_hwmp_parse_preq(NULL, 41, &p), "NULL body is refused");
        CHECK(!umac_mesh_hwmp_parse_preq(v, 41, NULL), "NULL output is refused");

        uint8_t w[64]; memcpy(w, v, sizeof(w));
        w[0] = 15; CHECK(!umac_mesh_hwmp_parse_preq(w, 41, &p), "a non-mesh category is refused");
        memcpy(w, v, sizeof(w));
        w[1] = 2;  CHECK(!umac_mesh_hwmp_parse_preq(w, 41, &p), "a non-HWMP action code is refused");
        memcpy(w, v, sizeof(w));
        w[2] = 131; CHECK(!umac_mesh_hwmp_parse_preq(w, 41, &p), "a PREP element is not a PREQ");
        /* AE changes the layout; refuse rather than misread a sixth address. */
        memcpy(w, v, sizeof(w));
        w[4] |= 0x40; CHECK(!umac_mesh_hwmp_parse_preq(w, 41, &p),
                            "the Address Extension bit is refused, not misparsed");
        memcpy(w, v, sizeof(w));
        w[29] = 2; CHECK(!umac_mesh_hwmp_parse_preq(w, 41, &p),
                         "more than one target is refused");
        /* A length octet larger than the buffer must not be trusted. */
        memcpy(w, v, sizeof(w));
        w[3] = 200; CHECK(!umac_mesh_hwmp_parse_preq(w, 41, &p),
                          "an element length past the end of the buffer is refused");
    }

    /* ---- PREP build --------------------------------------------------- */
    /* The Pi asks us for a path to ourselves. */
    umac_mesh_hwmp_build_preq(buf, sizeof(buf), PI, 0x0a0b0c0du, 0x99u, US, 4882u);
    umac_mesh_hwmp_parse_preq(buf, 41, &p);

    uint8_t prep[64];
    memset(prep, 0x5a, sizeof(prep));
    uint16_t m = umac_mesh_hwmp_build_prep(prep, sizeof(prep), &p, US, 0x77u);
    CHECK(m == 35, "PREP action body is 35 bytes");
    CHECK(prep[0] == 13 && prep[1] == 1, "PREP is also category 13, action 1");
    CHECK(prep[2] == 131, "element id is 131 (PREP)");
    CHECK(prep[3] == 31, "element length octet is exactly 31");
    CHECK(prep[4] == 0x00 && prep[5] == 0 && prep[6] == 31, "flags, hop count, TTL");
    /* The inversion that produces a valid frame pointing at the wrong node:
     * in a PREP the TARGET is us, the answerer, and the ORIGINATOR is the
     * node that asked. */
    CHECK(memcmp(&prep[7], US, 6) == 0, "PREP target at +7 is US, the node answering");
    CHECK(rd32(&prep[13]) == 0x77u, "our own sequence number at +13");
    CHECK(rd32(&prep[17]) == 4882u, "the PREQ's lifetime is echoed at +17");
    CHECK(rd32(&prep[21]) == 0, "metric at +21");
    /* The second spec error: originator at +25, not +23 overlapping the metric. */
    CHECK(memcmp(&prep[25], PI, 6) == 0, "PREP originator at +25 is the node that ASKED");
    CHECK(rd32(&prep[21]) == 0 && prep[24] == 0, "the metric field is not clipped by the address");
    CHECK(rd32(&prep[31]) == 0x0a0b0c0du, "the asker's sequence number is echoed at +31");
    CHECK(prep[35] == 0x5a, "nothing is written past the 35th byte");
    CHECK(memcmp(&prep[7], &prep[25], 6) != 0, "target and originator are not the same address");

    memset(prep, 0x5a, sizeof(prep));
    CHECK(umac_mesh_hwmp_build_prep(prep, 34, &p, US, 1) == 0 && prep[0] == 0x5a,
          "a PREP buffer one byte short is refused and left untouched");
    CHECK(umac_mesh_hwmp_build_prep(prep, sizeof(prep), NULL, US, 1) == 0,
          "a NULL PREQ is refused");

    /* ---- sequence-number adoption policy ----------------------------
     *
     * A PREQ carries the originator's idea of OUR sequence number, but the
     * Unknown Target Sequence Number flag says that field is meaningless.
     * These frames are unauthenticated, so a node that adopts the value
     * regardless can be walked forward by anyone in range: a few PREQs naming
     * our address with a far-ahead number push us past the comparison's
     * half-range, every peer then reads our replies as stale, and unicast dies
     * while broadcast keeps working. No counter on either side records it. */
    {
        struct hwmp_preq q;
        memset(&q, 0, sizeof(q));
        memcpy(q.orig_addr, PI, 6);
        memcpy(q.target_addr, US, 6);

        /* USN set: the field must be ignored no matter how large. */
        q.target_flags = 0x05; /* TO | USN, what we and mac80211 send */
        q.target_sn = 0x40000000u;
        CHECK(umac_mesh_hwmp_next_own_sn(100u, &q) == 101u,
              "with USN set, a far-ahead target SN is IGNORED");
        q.target_sn = 0xfffffff0u;
        CHECK(umac_mesh_hwmp_next_own_sn(5u, &q) == 6u,
              "with USN set, even a near-wrap target SN is ignored");

        /* A sustained attack must not move us more than one per frame. */
        uint32_t sn = 1000u;
        for (unsigned i = 0; i < 64u; i++)
        {
            q.target_sn = 0x10000000u * (i + 1u);
            sn = umac_mesh_hwmp_next_own_sn(sn, &q);
        }
        CHECK(sn == 1064u, "64 crafted PREQs advance us by exactly 64, not into the half-range");

        /* USN clear: the peer does have a value, so honour a newer one. */
        q.target_flags = 0x01; /* TO only */
        q.target_sn = 500u;
        CHECK(umac_mesh_hwmp_next_own_sn(100u, &q) == 501u,
              "with USN clear, a newer target SN is adopted then incremented");
        q.target_sn = 50u;
        CHECK(umac_mesh_hwmp_next_own_sn(100u, &q) == 101u,
              "with USN clear, an older target SN is not adopted");
        q.target_sn = 100u;
        CHECK(umac_mesh_hwmp_next_own_sn(100u, &q) == 101u,
              "with USN clear, an equal target SN is not adopted");

        /* The counter is modulo 2^32; the comparison is built for that. */
        q.target_flags = 0x05;
        CHECK(umac_mesh_hwmp_next_own_sn(0xffffffffu, &q) == 0u,
              "our sequence number wraps to 0, not to 1");

        CHECK(umac_mesh_hwmp_next_own_sn(7u, NULL) == 8u, "a NULL PREQ just increments");
    }

    /* Near-wrap anchors for the comparison itself, which the policy rests on. */
    CHECK(hwmp_sn_gt(0xffffffffu, 0xfffffffeu), "0xffffffff is newer than 0xfffffffe");
    CHECK(hwmp_sn_gt(1u, 0xffffffffu), "1 is newer than 0xffffffff (two past wrap)");
    CHECK(!hwmp_sn_gt(0xfffffffeu, 1u), "0xfffffffe is not newer than 1");

    printf(failures ? "\nFAILED (%d)\n" : "\nALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
