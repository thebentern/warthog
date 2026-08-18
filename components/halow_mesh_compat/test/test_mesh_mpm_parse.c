/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Parser safety and link-id semantics for the shipping MPM code
 * (morselib src/umac/mesh/umac_mesh_ies.c).
 *
 * These parsers run on attacker-controlled RF input: every element length in a
 * received Mesh Peering frame is chosen by whoever is transmitting. So the
 * truncation and mutation sweeps below matter as much as the happy path --
 * build with SAN=1 to run them under ASan/UBSan, which is the configuration
 * that actually detects an over-read.
 *
 *  (A) link-id perspective -- the field the standard calls "llid" is, from the
 *      receiver's side, the PEER's id (i.e. our plid). Getting this backwards
 *      makes mac80211 answer CNF_IGNR, which is silent: the radio works, the
 *      peering just never completes. That failure cost a lot of bench time.
 *  (B) per-action field offsets -- OPEN has capability(2), CONFIRM has
 *      capability(2)+AID(2), CLOSE has neither. A wrong offset finds a
 *      plausible-looking value in the wrong place.
 *  (C) truncation + mutation sweeps -- no crash, no over-read, and success
 *      only when the Peer Management IE really lies inside the buffer.
 */
#include "umac_mesh_ies.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static const uint8_t MESH_ID[] = "warthog-mesh-test";
#define MESH_ID_LEN 17

/* Same hardware capture as test_mesh_ies_golden.c: llid 0xc7f8, plid 0xa0b5. */
static const uint8_t GOLDEN_CONFIRM[] = {
    0x0f, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01, 0x08,
    0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c,
    0x72, 0x11, 0x77, 0x61, 0x72, 0x74, 0x68, 0x6f,
    0x67, 0x2d, 0x6d, 0x65, 0x73, 0x68, 0x2d, 0x74,
    0x65, 0x73, 0x74, 0x71, 0x07, 0x01, 0x01, 0x00,
    0x01, 0x00, 0x00, 0x09, 0x75, 0x06, 0x00, 0x00,
    0xf8, 0xc7, 0xb5, 0xa0,
};
#define GOLDEN_LEN ((uint32_t)sizeof(GOLDEN_CONFIRM))

static void test_perspective(void)
{
    uint16_t llid = 0, plid = 0;
    printf("\n=== (A) link-id perspective ===\n");

    CHECK(umac_mesh_ies_get_peer_llid(GOLDEN_CONFIRM, GOLDEN_LEN, &llid), "llid parses");
    CHECK(llid == 0xc7f8, "frame's first link id (0x%04x) is the PEER's llid = our plid", llid);

    CHECK(umac_mesh_ies_get_peer_plid(GOLDEN_CONFIRM, GOLDEN_LEN, &plid), "plid parses");
    CHECK(plid == 0xa0b5, "frame's second link id (0x%04x) is the peer's view of OUR llid", plid);

    CHECK(llid != plid, "the two link ids are distinct (a swap bug would collapse them)");

    printf("--- build -> parse round trip ---\n");
    static const uint16_t ids[] = { 1, 0x00ff, 0xff00, 0xffff, 0x1234, 0xc7f8 };
    int bad = 0;
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        for (unsigned j = 0; j < sizeof(ids) / sizeof(ids[0]); j++) {
            uint8_t buf[UMAC_MESH_MPM_BODY_MAXLEN];
            uint16_t n = umac_mesh_ies_build_mpm_body(buf, sizeof(buf),
                                                      UMAC_MESH_MPM_ACTION_CONFIRM,
                                                      ids[i], ids[j],   0, 1, MESH_ID, MESH_ID_LEN, false, NULL, 0);
            uint16_t gl = 0, gp = 0;
            if (n == 0 || !umac_mesh_ies_get_peer_llid(buf, n, &gl) ||
                !umac_mesh_ies_get_peer_plid(buf, n, &gp) || gl != ids[i] || gp != ids[j]) {
                bad++;
            }
        }
    }
    CHECK(bad == 0, "36 llid/plid combinations survive build -> parse unchanged");
}

static void test_offsets(void)
{
    uint8_t buf[UMAC_MESH_MPM_BODY_MAXLEN];
    uint16_t v = 0;
    printf("\n=== (B) per-action field offsets ===\n");

    uint16_t n = umac_mesh_ies_build_mpm_body(buf, sizeof(buf), UMAC_MESH_MPM_ACTION_OPEN,
                                              0xbeef, 0,   0, 1, MESH_ID, MESH_ID_LEN, false, NULL, 0);
    CHECK(umac_mesh_ies_get_peer_llid(buf, n, &v) && v == 0xbeef,
          "OPEN: llid found past capability(2) only");
    CHECK(!umac_mesh_ies_get_peer_plid(buf, n, &v),
          "OPEN: no plid field (4-byte PM IE), correctly refused");

    /* CLOSE has no capability field. Hand-craft one -- the builder refuses to
     * emit CLOSE, but a peer can certainly send us one, and mac80211's reason
     * code is the single most informative thing it ever tells us. */
    uint8_t close_body[] = {
        UMAC_MESH_MPM_CATEGORY, UMAC_MESH_MPM_ACTION_CLOSE,
        UMAC_MESH_EID_MESH_ID, 4, 'm', 'e', 's', 'h',
        UMAC_MESH_EID_PEER_MGMT, 6, 0x00, 0x00, 0x11, 0x22, 0x37, 0x00,
    };
    CHECK(umac_mesh_ies_get_peer_llid(close_body, sizeof(close_body), &v) && v == 0x2211,
          "CLOSE: llid found with NO capability field skipped");
    CHECK(umac_mesh_ies_get_close_reason(close_body, sizeof(close_body), &v) && v == 0x0037,
          "CLOSE: reason code 55 (MESH_CLOSE_RCVD) read from the IE tail");

    /* An 8-byte PM IE carries llid, plid and reason. */
    uint8_t close8[] = {
        UMAC_MESH_MPM_CATEGORY, UMAC_MESH_MPM_ACTION_CLOSE,
        UMAC_MESH_EID_PEER_MGMT, 8, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44, 0x36, 0x00,
    };
    CHECK(umac_mesh_ies_get_close_reason(close8, sizeof(close8), &v) && v == 0x0036,
          "CLOSE: 8-byte PM IE still yields the reason from the last two octets");

    /* A 4-byte PM IE has no reason field. Reading "the last two octets" there
     * would hand back the llid dressed up as a reason code. */
    uint8_t close4[] = {
        UMAC_MESH_MPM_CATEGORY, UMAC_MESH_MPM_ACTION_CLOSE,
        UMAC_MESH_EID_PEER_MGMT, 4, 0x00, 0x00, 0x99, 0x88,
    };
    CHECK(!umac_mesh_ies_get_close_reason(close4, sizeof(close4), &v),
          "CLOSE: a 4-byte PM IE has no reason field and is refused (not read as llid)");
}

static void test_bounds(void)
{
    uint16_t v;
    printf("\n=== (C) truncation + mutation sweeps (attacker-controlled input) ===\n");

    int wrong = 0;
    for (uint32_t L = 0; L <= GOLDEN_LEN; L++) {
        uint8_t probe[sizeof(GOLDEN_CONFIRM)];
        memcpy(probe, GOLDEN_CONFIRM, L);
        bool got_llid = umac_mesh_ies_get_peer_llid(probe, L, &v);
        bool got_plid = umac_mesh_ies_get_peer_plid(probe, L, &v);
        (void)umac_mesh_ies_get_close_reason(probe, L, &v);
        /* The PM IE occupies bytes 44..51, so nothing may parse before 52. */
        bool should = (L >= GOLDEN_LEN);
        if (got_llid != should || got_plid != should) wrong++;
    }
    CHECK(wrong == 0, "truncation sweep: parsers succeed only once the PM IE is wholly present");

    int crashed = 0;
    for (uint32_t i = 0; i < GOLDEN_LEN; i++) {
        for (int val = 0; val < 2; val++) {
            uint8_t probe[sizeof(GOLDEN_CONFIRM)];
            memcpy(probe, GOLDEN_CONFIRM, GOLDEN_LEN);
            probe[i] = val ? 0xff : 0x00;
            (void)umac_mesh_ies_get_peer_llid(probe, GOLDEN_LEN, &v);
            (void)umac_mesh_ies_get_peer_plid(probe, GOLDEN_LEN, &v);
            (void)umac_mesh_ies_get_close_reason(probe, GOLDEN_LEN, &v);
        }
    }
    CHECK(crashed == 0, "mutation sweep: every byte set to 0x00 and 0xff, no over-read (run SAN=1)");

    /* A zero-length element must not stall the walk. */
    uint8_t zero_elem[] = {
        UMAC_MESH_MPM_CATEGORY, UMAC_MESH_MPM_ACTION_OPEN, 0, 0,
        0x30, 0,                                     /* zero-length element */
        UMAC_MESH_EID_PEER_MGMT, 4, 0x00, 0x00, 0x55, 0x66,
    };
    CHECK(umac_mesh_ies_get_peer_llid(zero_elem, sizeof(zero_elem), &v) && v == 0x6655,
          "a zero-length element does not stall the IE walk");

    /* An element claiming to run past the buffer must be refused, not read. */
    uint8_t overrun[] = {
        UMAC_MESH_MPM_CATEGORY, UMAC_MESH_MPM_ACTION_OPEN, 0, 0,
        0x30, 0xff,                                  /* claims 255 bytes */
        UMAC_MESH_EID_PEER_MGMT, 4, 0x00, 0x00, 0x55, 0x66,
    };
    CHECK(!umac_mesh_ies_get_peer_llid(overrun, sizeof(overrun), &v),
          "an element length running past the buffer is refused");

    /* A PM IE too short to hold a link id must be skipped, not read. */
    uint8_t shortpm[] = {
        UMAC_MESH_MPM_CATEGORY, UMAC_MESH_MPM_ACTION_OPEN, 0, 0,
        UMAC_MESH_EID_PEER_MGMT, 3, 0x00, 0x00, 0x55,
    };
    CHECK(!umac_mesh_ies_get_peer_llid(shortpm, sizeof(shortpm), &v),
          "a 3-byte Peer Management IE is refused, not partially read");

    CHECK(!umac_mesh_ies_get_peer_llid(NULL, 10, &v), "NULL body is refused");
    CHECK(!umac_mesh_ies_get_peer_llid(GOLDEN_CONFIRM, GOLDEN_LEN, NULL), "NULL out is refused");
    CHECK(!umac_mesh_ies_get_peer_llid(GOLDEN_CONFIRM, 1, &v), "a 1-byte body is refused");
}

int main(void)
{
    printf("=== mesh MPM parser tests ===\n");
    test_perspective();
    test_offsets();
    test_bounds();
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
