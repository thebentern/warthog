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
 *  (D) the mesh profile of an Open or Confirm, and a Mesh ID naming another mesh.
 */
#include "umac_mesh_ies.h"

#include <stdio.h>
#include <stdlib.h>
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

/* AT+PLINKRX? on WTHG-0272A1F8738D, 2026-11-10: an AMPE Mesh Peering Open from another Warthog,
 * in the S1G form the air carries and umac_mesh_handle_mpm() would see. Cut after the MIC
 * element; the opaque AMPE trailer follows on air. Mesh ID at 53, Mesh Configuration at 64. */
static const uint8_t REAL_SAE_OPEN_S1G[] = {
    0x0f, 0x01, 0x10, 0x00, 0xd9, 0x0f, 0x9e, 0x00, 0x40, 0x00, 0x00, 0x08,
    0x08, 0x02, 0x20, 0x00, 0xfd, 0x00, 0xfa, 0x01, 0x00, 0x01, 0x08, 0x02,
    0x04, 0x0b, 0x8c, 0x16, 0x98, 0x24, 0xb0, 0x30, 0x14, 0x01, 0x00, 0x00,
    0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00,
    0x0f, 0xac, 0x08, 0x00, 0x00, 0x72, 0x09, 0x68, 0x61, 0x6c, 0x6f, 0x77,
    0x6d, 0x65, 0x73, 0x68, 0x71, 0x07, 0x01, 0x01, 0x00, 0x01, 0x01, 0x00,
    0x01, 0x75, 0x14, 0x01, 0x00, 0x7c, 0x85, 0x93, 0xb3, 0xea, 0x18, 0xa6,
    0x88, 0x2b, 0x96, 0x67, 0x62, 0xea, 0x9d, 0xcf, 0xf6, 0xb0, 0x74, 0x8c,
    0x10, 0x70, 0x6f, 0x56, 0x65, 0x7a, 0xb0, 0xf0, 0x56, 0x70, 0x37, 0x12,
    0x80, 0x87, 0xd9, 0x9c, 0x35,
};
/* AT+PLINKTX? on the same board: hostap's Close. A Mesh ID and no Mesh Configuration. */
static const uint8_t REAL_CLOSE[] = {
    0x0f, 0x03, 0x72, 0x09, 0x68, 0x61, 0x6c, 0x6f, 0x77, 0x6d, 0x65, 0x73,
    0x68, 0x75, 0x18, 0x01, 0x00, 0x77, 0xee, 0x00, 0x00, 0x34, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00,
};
static const uint8_t HALOW[] = { 'h', 'a', 'l', 'o', 'w', 'm', 'e', 's', 'h' };

/* A prefix of @p f in a heap block of exactly that size, so ASan sees one octet too many. */
static bool prefix_matches_(const uint8_t *f, uint32_t n, bool sae)
{
    uint8_t *p = malloc(n != 0u ? n : 1u);
    if (p == NULL) { return false; }
    memcpy(p, f, n);
    const bool r = umac_mesh_ies_mpm_matches(p, n, HALOW, sizeof(HALOW), sae);
    free(p);
    return r;
}

/* @p f with octet @p i set to @p v, in a heap block of exactly @p n octets. A failed
 * allocation reads as a match, which the sweep below counts as wrong. */
static bool mutated_matches_(const uint8_t *f, uint32_t n, uint32_t i, uint8_t v, bool sae)
{
    uint8_t *p = malloc(n);
    if (p == NULL) { return true; }
    memcpy(p, f, n);
    p[i] = v;
    const bool r = umac_mesh_ies_mpm_matches(p, n, HALOW, sizeof(HALOW), sae);
    free(p);
    return r;
}

/* umac_mesh_ies_other_mesh_id() against HALOW, on @p n octets in a heap block of exactly that
 * size. */
static bool other_id_(const uint8_t *ies, uint32_t n)
{
    uint8_t *p = malloc(n != 0u ? n : 1u);
    if (p == NULL) { return false; }
    memcpy(p, ies, n);
    const bool r = umac_mesh_ies_other_mesh_id(p, n, HALOW, sizeof(HALOW));
    free(p);
    return r;
}

/* The S1G Capabilities element mesh_tx_mpm_() puts ahead of the discovery blob: element 217,
 * fifteen octets, here those of the captured Open above. */
static const uint8_t S1G[17] = {
    0xd9, 0x0f, 0x9e, 0x00, 0x40, 0x00, 0x00, 0x08, 0x08, 0x02, 0x20, 0x00, 0xfd, 0x00, 0xfa,
    0x01, 0x00,
};
/* Room for one of our own frames: the extra element is not part of UMAC_MESH_MPM_BODY_MAXLEN. */
#define OWN_MAXLEN (UMAC_MESH_MPM_BODY_MAXLEN + 32)

/* One of our own frames as on air. An Open is 57 octets: Mesh ID element at 31, Mesh
 * Configuration element at 42, ending at 51. */
static uint16_t own_mpm_(uint8_t *buf, uint8_t action, bool sae)
{
    if (action == UMAC_MESH_MPM_ACTION_OPEN)
    {
        return umac_mesh_ies_build_mpm_body(buf, OWN_MAXLEN, UMAC_MESH_MPM_ACTION_OPEN, 0x1234, 0,
                                            0, 0, HALOW, 9, sae, S1G, 17);
    }
    return umac_mesh_ies_build_mpm_body(buf, OWN_MAXLEN, action, 0x1234, 0x5678,
                                        action == UMAC_MESH_MPM_ACTION_CLOSE ? 54 : 0,
                                        action == UMAC_MESH_MPM_ACTION_CONFIRM ? 1 : 0, HALOW, 9,
                                        sae, S1G, 17);
}

/* Prefixes of @p f, each in its own heap block, that do not match exactly from @p from on. */
static int truncation_wrong_(const uint8_t *f, uint32_t n, bool sae, uint32_t from)
{
    int wrong = 0;
    for (uint32_t L = 0; L <= n; L++)
    {
        wrong += (prefix_matches_(f, L, sae) != (L >= from));
    }
    return wrong;
}

/* Every octet of @p f set to 0x00 and to 0xff: how many of those inside the Mesh ID element
 * (@p id_len octets at @p id_at, header included) or the five profile octets at @p prof_at
 * still match. A mutation equal to the octet's own value is none and is skipped (congestion
 * control is already 0, as is an open frame's authentication protocol). */
static int mutation_wrong_(const uint8_t *f, uint32_t n, bool sae, uint32_t id_at, uint32_t id_len,
                           uint32_t prof_at)
{
    int wrong = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        for (int k = 0; k < 2; k++)
        {
            const uint8_t v = k ? 0xff : 0x00;
            if (f[i] == v) { continue; }
            const bool hit = mutated_matches_(f, n, i, v, sae);
            const bool profile = (i >= id_at && i < id_at + id_len) ||
                                 (i >= prof_at && i < prof_at + 5u);
            wrong += (profile && hit);
        }
    }
    return wrong;
}

static void test_profile(void)
{
    const uint32_t real_n = (uint32_t)sizeof(REAL_SAE_OPEN_S1G);
    uint8_t own[OWN_MAXLEN], buf[OWN_MAXLEN];
    printf("\n=== (D) the mesh profile of an Open or Confirm ===\n");

    /* D1: a real Open, as received. */
    CHECK(real_n == 113u && REAL_SAE_OPEN_S1G[53] == UMAC_MESH_EID_MESH_ID &&
          REAL_SAE_OPEN_S1G[64] == UMAC_MESH_EID_MESH_CONFIG,
          "the captured Open: %u octets, Mesh ID element at 53, Mesh Configuration at 64",
          (unsigned)real_n);
    CHECK(umac_mesh_ies_mpm_matches(REAL_SAE_OPEN_S1G, real_n, HALOW, 9, true),
          "a captured AMPE Open for \"halowmesh\" matches an SAE node on that mesh");
    CHECK(!umac_mesh_ies_mpm_matches(REAL_SAE_OPEN_S1G, real_n, HALOW, 9, false),
          "and not an open node on it: its authentication protocol octet is 1");
    static const struct { const char *id; uint8_t len; const char *what; } almost[] = {
        { "halowmesi", 9, "the last octet differs" },
        { "halowmes", 8, "a prefix, one octet short" },
        { "halowmeshx", 10, "one octet longer" },
        { "HALOWMESH", 9, "another case" },
    };
    for (unsigned i = 0; i < sizeof(almost) / sizeof(almost[0]); i++)
    {
        CHECK(!umac_mesh_ies_mpm_matches(REAL_SAE_OPEN_S1G, real_n, (const uint8_t *)almost[i].id,
                                         almost[i].len, true),
              "nor the mesh \"%s\": %s", almost[i].id, almost[i].what);
    }

    /* D2: our own frames, as mesh_tx_mpm_() builds them. */
    const uint16_t own_n = own_mpm_(own, UMAC_MESH_MPM_ACTION_OPEN, false);
    CHECK(own_n == 57u && own[31] == UMAC_MESH_EID_MESH_ID && own[42] == UMAC_MESH_EID_MESH_CONFIG,
          "our own Open: %u octets, Mesh ID element at 31, Mesh Configuration at 42",
          (unsigned)own_n);
    CHECK(umac_mesh_ies_mpm_matches(own, own_n, HALOW, 9, false) &&
          !umac_mesh_ies_mpm_matches(own, own_n, HALOW, 9, true),
          "our own open Open matches an open node and not an SAE one");
    uint16_t n = own_mpm_(buf, UMAC_MESH_MPM_ACTION_CONFIRM, false);
    CHECK(n != 0 && umac_mesh_ies_mpm_matches(buf, n, HALOW, 9, false) &&
          !umac_mesh_ies_mpm_matches(buf, n, HALOW, 9, true),
          "our own open Confirm likewise");
    n = own_mpm_(buf, UMAC_MESH_MPM_ACTION_OPEN, true);
    CHECK(n != 0 && umac_mesh_ies_mpm_matches(buf, n, HALOW, 9, true) &&
          !umac_mesh_ies_mpm_matches(buf, n, HALOW, 9, false),
          "our own SAE Open matches an SAE node and not an open one");
    n = own_mpm_(buf, UMAC_MESH_MPM_ACTION_CONFIRM, true);
    CHECK(n != 0 && umac_mesh_ies_mpm_matches(buf, n, HALOW, 9, true) &&
          !umac_mesh_ies_mpm_matches(buf, n, HALOW, 9, false),
          "our own SAE Confirm likewise");

    /* D3: the five profile octets, and the two that are not part of the profile. */
    int wrong = 0;
    for (uint32_t i = 44; i <= 48; i++)
    {
        static const uint8_t add[3] = { 0x01, 0x02, 0x80 };
        for (unsigned k = 0; k < sizeof(add); k++)
        {
            memcpy(buf, own, own_n);
            buf[i] = (uint8_t)(buf[i] + add[k]);
            wrong += umac_mesh_ies_mpm_matches(buf, own_n, HALOW, 9, false);
        }
    }
    CHECK(wrong == 0,
          "path selection protocol and metric, congestion control, synchronisation method, "
          "authentication protocol: another value in any one refuses (%d accepted)", wrong);
    memcpy(buf, own, own_n);
    buf[49] = 0x06;
    buf[50] = 0x19;
    CHECK(umac_mesh_ies_mpm_matches(buf, own_n, HALOW, 9, false),
          "Formation Info 0x06 and Capability 0x19 still match: neither is part of the profile");

    /* D4: element order. Both frames are assembled, not captured: an open Open in the order
     * the Linux Morse driver puts on air (morse_mgmt_mesh_peering_mgmt_ies_order: S1G
     * Capabilities, S1G Operation, Mesh ID, Mesh Configuration, Peer Management, and no
     * Supported Rates), with the two S1G elements of the OpenMANET beacon in test_s1g_beacon.c. */
    static const uint8_t linux_open[] = {
        0x0f, 0x01, 0x00, 0x00,
        0xd9, 0x0f, 0x9e, 0x00, 0x40, 0xf8, 0x80, 0x0c, 0x00, 0x02, 0x00, 0x00, 0xfd, 0x00, 0xfa,
        0x01, 0x00,
        0xe8, 0x06, 0x03, 0x02, 0x29, 0x2a, 0xc4, 0xcc,
        0x72, 0x09, 'h', 'a', 'l', 'o', 'w', 'm', 'e', 's', 'h',
        0x71, 0x07, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x19,
        0x75, 0x04, 0x00, 0x00, 0x34, 0x12,
    };
    static const uint8_t config_first[] = {
        0x0f, 0x01, 0x00, 0x00,
        0xd9, 0x0f, 0x9e, 0x00, 0x40, 0xf8, 0x80, 0x0c, 0x00, 0x02, 0x00, 0x00, 0xfd, 0x00, 0xfa,
        0x01, 0x00,
        0xe8, 0x06, 0x03, 0x02, 0x29, 0x2a, 0xc4, 0xcc,
        0x71, 0x07, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x19,
        0x72, 0x09, 'h', 'a', 'l', 'o', 'w', 'm', 'e', 's', 'h',
        0x75, 0x04, 0x00, 0x00, 0x34, 0x12,
    };
    CHECK(umac_mesh_ies_mpm_matches(linux_open, sizeof(linux_open), HALOW, 9, false),
          "an open Open in the Linux Morse driver's element order matches");
    CHECK(umac_mesh_ies_mpm_matches(config_first, sizeof(config_first), HALOW, 9, false),
          "and so does one with the Mesh Configuration ahead of the Mesh ID: no order is assumed");

    /* D5, D6: an element missing or short. */
    static const uint8_t no_id[] = {
        0x0f, 0x01, 0x00, 0x00,
        0x71, 0x07, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01,
        0x75, 0x04, 0x00, 0x00, 0x34, 0x12,
    };
    CHECK(!umac_mesh_ies_mpm_matches(no_id, sizeof(no_id), HALOW, 9, false),
          "no Mesh ID element: refused");
    static const uint8_t no_config[] = {
        0x0f, 0x01, 0x00, 0x00,
        0x72, 0x09, 'h', 'a', 'l', 'o', 'w', 'm', 'e', 's', 'h',
        0x75, 0x04, 0x00, 0x00, 0x34, 0x12,
    };
    CHECK(!umac_mesh_ies_mpm_matches(no_config, sizeof(no_config), HALOW, 9, false),
          "no Mesh Configuration element: refused");
    static const uint8_t config6[] = {
        0x0f, 0x01, 0x00, 0x00,
        0x72, 0x09, 'h', 'a', 'l', 'o', 'w', 'm', 'e', 's', 'h',
        0x71, 0x06, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00,
        0x75, 0x04, 0x00, 0x00, 0x34, 0x12,
    };
    CHECK(!umac_mesh_ies_mpm_matches(config6, sizeof(config6), HALOW, 9, false),
          "a six-octet Mesh Configuration: refused");
    static const uint8_t config3_then_7[] = {
        0x0f, 0x01, 0x00, 0x00,
        0x72, 0x09, 'h', 'a', 'l', 'o', 'w', 'm', 'e', 's', 'h',
        0x71, 0x03, 0x01, 0x01, 0x00,
        0x71, 0x07, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01,
        0x75, 0x04, 0x00, 0x00, 0x34, 0x12,
    };
    CHECK(umac_mesh_ies_mpm_matches(config3_then_7, sizeof(config3_then_7), HALOW, 9, false),
          "a three-octet one followed by a full one: the full one decides, and matches");

    /* D7: odd Mesh ID elements. The Mesh Configuration comes first in the last two, so it
     * is the Mesh ID walk that must refuse. */
    static const uint8_t id_empty[] = {
        0x0f, 0x01, 0x00, 0x00,
        0x72, 0x00,
        0x71, 0x07, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01,
        0x75, 0x04, 0x00, 0x00, 0x34, 0x12,
    };
    CHECK(!umac_mesh_ies_mpm_matches(id_empty, sizeof(id_empty), HALOW, 9, false),
          "a zero-length (wildcard) Mesh ID: refused");
    n = 0;
    buf[n++] = 0x0f; buf[n++] = 0x01; buf[n++] = 0x00; buf[n++] = 0x00;
    buf[n++] = UMAC_MESH_EID_MESH_ID;
    buf[n++] = 33;
    memset(&buf[n], 'x', 33);
    memcpy(&buf[n], HALOW, sizeof(HALOW));
    n = (uint16_t)(n + 33);
    memcpy(&buf[n], &id_empty[6], sizeof(id_empty) - 6u);
    n = (uint16_t)(n + sizeof(id_empty) - 6u);
    CHECK(!prefix_matches_(buf, n, false),
          "a 33-octet Mesh ID wholly inside the body, beginning with ours: refused");
    static const uint8_t id_runs_past[] = {
        0x0f, 0x01, 0x00, 0x00,
        0x71, 0x07, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01,
        0x72, 0xff, 'h', 'a', 'l', 'o', 'w', 'm', 'e', 's', 'h',
    };
    CHECK(!prefix_matches_(id_runs_past, sizeof(id_runs_past), false),
          "a Mesh ID length octet of 0xff running past the end: refused");
    static const uint8_t id_cut[] = {
        0x0f, 0x01, 0x00, 0x00,
        0x71, 0x07, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01,
        0x72, 0x09, 'h', 'a', 'l', 'o',
    };
    CHECK(!prefix_matches_(id_cut, sizeof(id_cut), false),
          "a body cut in the middle of the Mesh ID: refused");

    /* D8: a Confirm's elements start after capability(2) and AID(2). Here the AID octets
     * read as a Mesh ID element header. */
    static const uint8_t confirm_aid[] = {
        0x0f, 0x02, 0x00, 0x00, 0x72, 0x09,
        0x72, 0x09, 'h', 'a', 'l', 'o', 'w', 'm', 'e', 's', 'h',
        0x71, 0x07, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01,
        0x75, 0x06, 0x00, 0x00, 0x34, 0x12, 0x78, 0x56,
    };
    CHECK(umac_mesh_ies_mpm_matches(confirm_aid, sizeof(confirm_aid), HALOW, 9, false),
          "a Confirm whose AID octets are 72 09 is judged by its real elements, from offset 6");
    static const uint8_t confirm_aid_only[] = {
        0x0f, 0x02, 0x00, 0x00, 0x72, 0x09,
        'h', 'a', 'l', 'o', 'w', 'm', 'e', 's', 'h',
        0x71, 0x07, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01,
        0x75, 0x06, 0x00, 0x00, 0x34, 0x12, 0x78, 0x56,
    };
    CHECK(!prefix_matches_(confirm_aid_only, sizeof(confirm_aid_only), false),
          "and refused when its only \"Mesh ID\" is that AID followed by the ID's octets");

    /* D9, D10: the sweeps, each copy in a heap block of its exact size (run SAN=1). */
    wrong = truncation_wrong_(own, own_n, false, 51);
    CHECK(wrong == 0,
          "truncation sweep, our own Open: a match exactly from 51 octets on, where the Mesh "
          "Configuration ends (%d wrong)", wrong);
    wrong = truncation_wrong_(REAL_SAE_OPEN_S1G, real_n, true, 73);
    CHECK(wrong == 0, "truncation sweep, the captured Open: exactly from 73 octets on (%d wrong)",
          wrong);
    wrong = mutation_wrong_(own, own_n, false, 31, 11, 44);
    CHECK(wrong == 0,
          "mutation sweep, our own Open: 0x00 or 0xff anywhere in the Mesh ID element or the "
          "five profile octets refuses (%d accepted)", wrong);
    wrong = mutation_wrong_(REAL_SAE_OPEN_S1G, real_n, true, 53, 11, 66);
    CHECK(wrong == 0, "mutation sweep, the captured Open: the same (%d accepted)", wrong);

    /* D11: arguments. */
    CHECK(!umac_mesh_ies_mpm_matches(NULL, own_n, HALOW, 9, false), "NULL body is refused");
    CHECK(!umac_mesh_ies_mpm_matches(own, own_n, NULL, 9, false), "NULL Mesh ID is refused");
    CHECK(!umac_mesh_ies_mpm_matches(own, own_n, HALOW, 0, false) &&
          !umac_mesh_ies_mpm_matches(id_empty, sizeof(id_empty), HALOW, 0, false),
          "a zero-length Mesh ID of ours is refused, against a zero-length Mesh ID element too");
    CHECK(!umac_mesh_ies_mpm_matches(own, 0, HALOW, 9, false) &&
          !umac_mesh_ies_mpm_matches(own, 1, HALOW, 9, false),
          "a body of 0 or 1 octets is refused");

    /* D12: why umac_mesh_handle_mpm() must not put a Close to this test. */
    CHECK(!umac_mesh_ies_mpm_matches(REAL_CLOSE, sizeof(REAL_CLOSE), HALOW, 9, true) &&
          !umac_mesh_ies_mpm_matches(REAL_CLOSE, sizeof(REAL_CLOSE), HALOW, 9, false),
          "hostap's captured Close does not match: it carries no Mesh Configuration");
    n = own_mpm_(buf, UMAC_MESH_MPM_ACTION_CLOSE, false);
    CHECK(n != 0 && umac_mesh_ies_mpm_matches(buf, n, HALOW, 9, false),
          "our own Close does: Warthog's carries the whole discovery blob");

    /* D13: a Mesh ID element naming another mesh. */
    printf("--- a Mesh ID naming another mesh ---\n");
    static const struct { uint8_t ies[24]; uint8_t len; bool other; const char *what; } ids[] = {
        { { 0x00, 0x00, 0x72, 0x09, 'o', 't', 'h', 'e', 'r', 'm', 'e', 's', 'h' }, 13, true,
          "a wildcard SSID, then the Mesh ID \"othermesh\": another mesh" },
        { { 0x00, 0x00, 0x72, 0x09, 'h', 'a', 'l', 'o', 'w', 'm', 'e', 's', 'h' }, 13, false,
          "a wildcard SSID, then our Mesh ID: not another mesh" },
        { { 0x72, 0x09, 'h', 'a', 'l', 'o', 'w', 'm', 'e', 's', 'i' }, 11, true,
          "\"halowmesi\": another mesh" },
        { { 0x72, 0x08, 'h', 'a', 'l', 'o', 'w', 'm', 'e', 's' }, 10, true,
          "\"halowmes\", a prefix of ours: another mesh" },
        { { 0x72, 0x0a, 'h', 'a', 'l', 'o', 'w', 'm', 'e', 's', 'h', 'x' }, 12, true,
          "\"halowmeshx\", ours and one octet more: another mesh" },
        { { 0x72, 0x00 }, 2, false, "a zero-length (wildcard) Mesh ID: not counted" },
        { { 0x00, 0x00 }, 2, false, "a wildcard SSID alone: no Mesh ID" },
        { { 0x00, 0x04, 'h', 'o', 'm', 'e' }, 6, false, "an SSID \"home\" and no Mesh ID" },
        { { 0x72, 0x20, 'o', 't' }, 4, false, "a Mesh ID length octet running past the end" },
        { { 0x00, 0x00, 0x72 }, 3, false, "cut inside the Mesh ID element's header" },
        { { 0x72, 0x09, 'h', 'a', 'l', 'o', 'w', 'm', 'e', 's', 'h',
            0x72, 0x09, 'o', 't', 'h', 'e', 'r', 'm', 'e', 's', 'h' }, 22, false,
          "our Mesh ID, then another: the first decides" },
    };
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
    {
        CHECK(other_id_(ids[i].ies, ids[i].len) == ids[i].other, "%s", ids[i].what);
    }
    CHECK(!umac_mesh_ies_other_mesh_id(NULL, 13, HALOW, 9) &&
          !umac_mesh_ies_other_mesh_id(ids[0].ies, 13, NULL, 9) &&
          !umac_mesh_ies_other_mesh_id(ids[0].ies, 13, HALOW, 0) &&
          !umac_mesh_ies_other_mesh_id(ids[0].ies, 0, HALOW, 9),
          "NULL elements, a NULL or zero-length Mesh ID of ours, and no octets at all: false");
}

int main(void)
{
    printf("=== mesh MPM parser tests ===\n");
    test_perspective();
    test_offsets();
    test_bounds();
    test_profile();
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
