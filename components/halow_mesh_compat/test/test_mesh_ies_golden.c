/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Golden-bytes regression for the shipping mesh IE / MPM byte layout
 * (morselib src/umac/mesh/umac_mesh_ies.c -- the REAL firmware code, not a
 * reimplementation; that file is freestanding so it links here directly).
 *
 * The fixture is a frame captured from hardware via AT+MPMDUMP? during a
 * three-board run in which a real mac80211 peer accepted our Open and the
 * peering reached estab=1. Every byte below was paid for on the bench, and a
 * regression here breaks peering with NO visible symptom -- the radio still
 * transmits, the peer just silently declines. Hence exact memcmp.
 *
 *  (A) discovery IE blob: exact bytes, IE order, the two measured
 *      path-selection octets, the basic-rate subset, and the sizing sweep that
 *      pins the buffer bug (an undersized buffer returns 0, which silently
 *      kills the only working discovery path)
 *  (B) MPM CONFIRM/OPEN bodies: exact bytes plus field-level checks so a
 *      failure names the field rather than just "52 bytes differ"
 */
#include "umac_mesh_ies.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static const uint8_t MESH_ID[] = "warthog-mesh-test";  /* 17 bytes, no NUL */
#define MESH_ID_LEN 17

/* AT+MPMDUMP? on board WTHG-021BF681BA51, llid 0xc7f8 / plid 0xa0b5.
 * 52 bytes, category 0x0f action 0x02 (CONFIRM).
 *
 * The original capture was taken with the peer stuck in OPN_RCVD, and carried
 * AID 0 -- which turned out to be the reason it was stuck. AID 0 is reserved
 * for the group key, and a peer that assigns per-link AIDs refuses it; the
 * mac80211 side answered with Close reason 52 (MESH-PEER-CANCELED) while every
 * other field verified correct on air. Byte 4 is now 0x01, and this frame is
 * what a peer accepts rather than what one rejected.
 *
 * The Mesh Capability octet was later changed from 0x09 to 0x01 on purpose,
 * clearing the Forwarding bit -- warthog has no path table and drops any path
 * request that does not target it, so claiming to forward invites a peer to
 * blackhole traffic through us. This reference frame moved with that change;
 * it is a deliberate difference from the original capture, not drift. */
static const uint8_t GOLDEN_CONFIRM[] = {
    0x0f, 0x02, 0x00, 0x00, 0x01, 0x00, 0x01, 0x08,
    0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c,
    0x72, 0x11, 0x77, 0x61, 0x72, 0x74, 0x68, 0x6f,
    0x67, 0x2d, 0x6d, 0x65, 0x73, 0x68, 0x2d, 0x74,
    0x65, 0x73, 0x74, 0x71, 0x07, 0x01, 0x01, 0x00,
    0x01, 0x00, 0x00, 0x01, 0x75, 0x06, 0x00, 0x00,
    0xf8, 0xc7, 0xb5, 0xa0,
};
#define GOLDEN_LEN ((uint16_t)sizeof(GOLDEN_CONFIRM))

/* The discovery blob is bytes 6..43 of the Confirm: rates, Mesh ID, config. */
#define DISCOVERY_OFF 6
#define DISCOVERY_LEN 38

static void hexdiff(const uint8_t *got, const uint8_t *want, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++) {
        if (got[i] != want[i]) {
            printf("     first difference at byte %u: got 0x%02x want 0x%02x\n",
                   (unsigned)i, got[i], want[i]);
            return;
        }
    }
}

static void test_discovery_ies(void)
{
    uint8_t buf[UMAC_MESH_DISCOVERY_IES_MAXLEN];
    printf("\n=== (A) discovery IE blob ===\n");

    uint16_t n = umac_mesh_ies_build_discovery(buf, sizeof(buf), MESH_ID, MESH_ID_LEN, false);
    CHECK(n == DISCOVERY_LEN, "discovery blob is %u bytes (want %u)", n, DISCOVERY_LEN);
    if (n == DISCOVERY_LEN && memcmp(buf, GOLDEN_CONFIRM + DISCOVERY_OFF, DISCOVERY_LEN) != 0) {
        hexdiff(buf, GOLDEN_CONFIRM + DISCOVERY_OFF, DISCOVERY_LEN);
    }
    CHECK(n == DISCOVERY_LEN && memcmp(buf, GOLDEN_CONFIRM + DISCOVERY_OFF, DISCOVERY_LEN) == 0,
          "discovery blob matches the captured frame byte for byte");

    /* IE order, asserted by walking so a reorder fails readably. */
    CHECK(buf[0] == UMAC_MESH_EID_SUPPORTED_RATES && buf[1] == 8, "IE[0] = Supported Rates, len 8");
    CHECK(buf[10] == UMAC_MESH_EID_MESH_ID && buf[11] == MESH_ID_LEN, "IE[1] = Mesh ID, len 17");
    CHECK(memcmp(&buf[12], MESH_ID, MESH_ID_LEN) == 0, "Mesh ID payload is \"warthog-mesh-test\"");
    CHECK(buf[29] == UMAC_MESH_EID_MESH_CONFIG && buf[30] == UMAC_MESH_CFG_IE_LEN,
          "IE[2] = Mesh Configuration, len 7");

    /* The two octets that cost the most to get right. Values are MEASURED:
     * 0x00 makes the peer probe forever and never initiate peering. */
    CHECK(buf[31] == 0x01, "mesh config path-selection PROTOCOL is 0x01 (NOT the header's 0x00)");
    CHECK(buf[32] == 0x01, "mesh config path-selection METRIC is 0x01 (NOT the header's 0x00)");
    CHECK(buf[33] == 0x00, "congestion control = 0");
    CHECK(buf[34] == 0x01, "sync method = neighbour offset");
    CHECK(buf[35] == 0x00, "auth protocol = open");
    /* Mesh Capability. Bit 0 (accepting peerings) must be set or no peer will
     * try to peer with us. Bit 3 (forwarding) must NOT be set: we have no path
     * table and drop any path request that does not target us, so advertising
     * it invites a peer to route through us and blackhole the traffic. That
     * failure only appears once a third node joins and cannot hear the others
     * directly, which is exactly the case nobody tests on a bench of two. */
    CHECK(buf[37] == 0x01, "capability = accepting peerings, and nothing else");
    CHECK((buf[37] & 0x01) != 0, "accepting-peerings bit is set");
    CHECK((buf[37] & 0x08) == 0, "forwarding bit is CLEAR -- we do not forward");

    /* mesh_matches_local() compares the BASIC subset specifically. */
    int basic = 0;
    for (int i = 0; i < 8; i++) {
        if (buf[2 + i] & 0x80) basic++;
    }
    CHECK(basic == 3, "exactly 3 rates carry the basic bit");
    CHECK(buf[2] == 0x8c && buf[4] == 0x98 && buf[6] == 0xb0,
          "the basic rates are 6, 12 and 24 Mbps");

    /* SAE differs from open at exactly one octet (the auth protocol). */
    uint8_t sae[UMAC_MESH_DISCOVERY_IES_MAXLEN];
    uint16_t sn = umac_mesh_ies_build_discovery(sae, sizeof(sae), MESH_ID, MESH_ID_LEN, true);
    int diffs = 0, at = -1;
    for (uint16_t i = 0; i < sn && i < n; i++) {
        if (sae[i] != buf[i]) { diffs++; at = i; }
    }
    CHECK(sn == n && diffs == 1 && at == 35, "sae=true differs at exactly one byte (auth proto)");

    /* Beacon path and probe/MPM path must emit identical Mesh Config bytes. */
    uint8_t cfg[2 + UMAC_MESH_CFG_IE_LEN];
    uint16_t cn = umac_mesh_ies_build_mesh_config(cfg, sizeof(cfg), false);
    CHECK(cn == sizeof(cfg) && memcmp(cfg, &buf[29], cn) == 0,
          "beacon's Mesh Config IE == the one in the discovery blob");

    printf("--- sizing sweep (pins the undersized-buffer bug) ---\n");
    uint8_t big[UMAC_MESH_DISCOVERY_IES_MAXLEN];
    uint8_t maxid[UMAC_MESH_IES_MESH_ID_MAXLEN];
    memset(maxid, 'z', sizeof(maxid));
    uint16_t bn = umac_mesh_ies_build_discovery(big, sizeof(big), maxid, sizeof(maxid), false);
    CHECK(bn == UMAC_MESH_DISCOVERY_IES_MAXLEN,
          "a 32-byte mesh id fills exactly UMAC_MESH_DISCOVERY_IES_MAXLEN (%u)",
          (unsigned)UMAC_MESH_DISCOVERY_IES_MAXLEN);

    int leaked = 0, wrong_rc = 0;
    for (uint16_t cap = 0; cap < UMAC_MESH_DISCOVERY_IES_MAXLEN; cap++) {
        uint8_t probe[UMAC_MESH_DISCOVERY_IES_MAXLEN];
        memset(probe, 0xA5, sizeof(probe));
        if (umac_mesh_ies_build_discovery(probe, cap, maxid, sizeof(maxid), false) != 0) wrong_rc++;
        for (uint16_t i = 0; i < sizeof(probe); i++) {
            if (probe[i] != 0xA5) { leaked = 1; break; }
        }
    }
    CHECK(wrong_rc == 0, "every out_len below the required size returns 0");
    CHECK(leaked == 0, "a failing build never writes to the caller's buffer");

    CHECK(umac_mesh_ies_build_discovery(buf, sizeof(buf), MESH_ID, 0, false) == 0,
          "mesh_id_len == 0 is rejected");
    CHECK(umac_mesh_ies_build_discovery(buf, sizeof(buf), NULL, MESH_ID_LEN, false) == 0,
          "NULL mesh_id is rejected");
    CHECK(umac_mesh_ies_build_discovery(buf, sizeof(buf), maxid,
                                        UMAC_MESH_IES_MESH_ID_MAXLEN + 1, false) == 0,
          "an over-long mesh_id is rejected");
}

static void test_mpm_bodies(void)
{
    uint8_t buf[UMAC_MESH_MPM_BODY_MAXLEN];
    printf("\n=== (B) MPM action-frame bodies ===\n");

    uint16_t n = umac_mesh_ies_build_mpm_body(buf, sizeof(buf), UMAC_MESH_MPM_ACTION_CONFIRM,
                                              0xc7f8, 0xa0b5,   0, 1, MESH_ID, MESH_ID_LEN, false, NULL, 0);
    CHECK(n == GOLDEN_LEN, "CONFIRM body is %u bytes (want %u)", n, GOLDEN_LEN);
    if (n == GOLDEN_LEN && memcmp(buf, GOLDEN_CONFIRM, GOLDEN_LEN) != 0) {
        hexdiff(buf, GOLDEN_CONFIRM, GOLDEN_LEN);
    }
    CHECK(n == GOLDEN_LEN && memcmp(buf, GOLDEN_CONFIRM, GOLDEN_LEN) == 0,
          "CONFIRM matches the hardware-captured frame byte for byte");

    /* Field-level checks on the same buffer, so a failure names the field. */
    CHECK(buf[0] == UMAC_MESH_MPM_CATEGORY, "category = 15 (SELF_PROTECTED)");
    CHECK(buf[1] == UMAC_MESH_MPM_ACTION_CONFIRM, "action = 2 (CONFIRM)");
    CHECK(buf[2] == 0 && buf[3] == 0, "capability info = 0");
    /* Non-zero is the requirement, not the exact value: AID 0 is the group
     * key and gets the peering cancelled. */
    CHECK(buf[4] != 0 || buf[5] != 0, "AID is NON-ZERO");
    CHECK(buf[4] == 1 && buf[5] == 0, "AID = 1, little-endian");
    CHECK(buf[44] == UMAC_MESH_EID_PEER_MGMT && buf[45] == 6, "Peer Management IE, len 6");
    CHECK(buf[46] == 0 && buf[47] == 0, "peering protocol identifier = 0");
    CHECK(buf[48] == 0xf8 && buf[49] == 0xc7, "llid 0xc7f8 little-endian");
    CHECK(buf[50] == 0xb5 && buf[51] == 0xa0, "plid 0xa0b5 little-endian");

    /* Byte-order guard: swapping the ids must change the bytes. Without this a
     * symmetric endianness bug would pass the memcmp above. */
    uint8_t swapped[UMAC_MESH_MPM_BODY_MAXLEN];
    umac_mesh_ies_build_mpm_body(swapped, sizeof(swapped), UMAC_MESH_MPM_ACTION_CONFIRM,
                                 0xa0b5, 0xc7f8,   0, 1, MESH_ID, MESH_ID_LEN, false, NULL, 0);
    CHECK(memcmp(swapped, GOLDEN_CONFIRM, GOLDEN_LEN) != 0,
          "swapping llid/plid produces different bytes");

    /* OPEN: structural only -- no hardware capture exists for it. */
    uint16_t on = umac_mesh_ies_build_mpm_body(buf, sizeof(buf), UMAC_MESH_MPM_ACTION_OPEN,
                                               0xc7f8, 0,   0, 1, MESH_ID, MESH_ID_LEN, false, NULL, 0);
    CHECK(on == GOLDEN_LEN - 4, "OPEN body is 4 bytes shorter than CONFIRM (no AID, no plid)");
    CHECK(buf[1] == UMAC_MESH_MPM_ACTION_OPEN, "action = 1 (OPEN)");
    CHECK(buf[on - 6] == UMAC_MESH_EID_PEER_MGMT && buf[on - 5] == 4,
          "OPEN's Peer Management IE has length 4 (no plid field)");
    CHECK(buf[on - 2] == 0xf8 && buf[on - 1] == 0xc7, "OPEN carries llid, little-endian");

    /* CLOSE: no capability and no AID -- the fixed part is category+action
     * only -- and its Peer Management IE carries llid, plid AND a reason.
     * find_peer_mgmt_ie_() offsets its walk on exactly this shape, so a change
     * here silently breaks reason parsing at the far end. */
    uint16_t cn = umac_mesh_ies_build_mpm_body(buf, sizeof(buf), UMAC_MESH_MPM_ACTION_CLOSE,
                                               0x1122, 0x3344, UMAC_MESH_REASON_MAX_PEERS,
                                                0, MESH_ID, MESH_ID_LEN, false, NULL, 0);
    CHECK(cn > 0, "CLOSE builds");
    CHECK(buf[0] == UMAC_MESH_MPM_CATEGORY && buf[1] == UMAC_MESH_MPM_ACTION_CLOSE,
          "CLOSE starts with category + action");
    CHECK(buf[2] == UMAC_MESH_EID_SUPPORTED_RATES,
          "CLOSE has no capability or AID -- IEs begin immediately");
    /* IE header (id + len) sits 2 bytes ahead of the 8-byte payload. */
    CHECK(buf[cn - 10] == UMAC_MESH_EID_PEER_MGMT && buf[cn - 9] == 8,
          "CLOSE Peer Management IE is 8 bytes");
    CHECK(buf[cn - 6] == 0x22 && buf[cn - 5] == 0x11, "CLOSE carries llid, little-endian");
    CHECK(buf[cn - 4] == 0x44 && buf[cn - 3] == 0x33, "CLOSE carries plid, little-endian");
    CHECK(buf[cn - 2] == UMAC_MESH_REASON_MAX_PEERS && buf[cn - 1] == 0,
          "CLOSE reason code is last, little-endian");
    uint16_t got_reason = 0;
    CHECK(umac_mesh_ies_get_close_reason(buf, cn, &got_reason) &&
          got_reason == UMAC_MESH_REASON_MAX_PEERS,
          "our own CLOSE round-trips through the reason parser");
    CHECK(umac_mesh_ies_build_mpm_body(buf, sizeof(buf), 99, 1, 2,   0, 1, MESH_ID, MESH_ID_LEN,
                                       false, NULL, 0) == 0, "an unknown action code is refused");

    int wrong_rc = 0, leaked = 0;
    for (uint16_t cap = 0; cap < GOLDEN_LEN; cap++) {
        uint8_t probe[UMAC_MESH_MPM_BODY_MAXLEN];
        memset(probe, 0xA5, sizeof(probe));
        if (umac_mesh_ies_build_mpm_body(probe, cap, UMAC_MESH_MPM_ACTION_CONFIRM, 1, 2,
                                           0, 1, MESH_ID, MESH_ID_LEN, false, NULL, 0) != 0) wrong_rc++;
        for (uint16_t i = 0; i < sizeof(probe); i++) {
            if (probe[i] != 0xA5) { leaked = 1; break; }
        }
    }
    CHECK(wrong_rc == 0, "every undersized out_len returns 0");
    CHECK(leaked == 0, "a failing MPM build never writes to the caller's buffer");
}

/* Caller-supplied elements.
 *
 * Every peering frame warthog actually transmits carries an S1G Capabilities
 * element passed as extra_ies -- it is the difference between the far side's
 * Morse driver accepting the frame and logging "S1G capabilities mismatch" and
 * dropping it. Every other test in this file passes NULL, so the shape that
 * goes on air was the one shape not under test. */
static void test_extra_ies(void)
{
    /* A plausible S1G Capabilities element: id 217, 15 octets of payload. */
    uint8_t extra[17];
    extra[0] = 0xd9; extra[1] = 0x0f;
    for (int i = 0; i < 15; i++) { extra[2 + i] = (uint8_t)(0xa0 + i); }

    uint8_t b[256];
    uint16_t n = umac_mesh_ies_build_mpm_body(b, sizeof(b), 2, 0x1234, 0x5678, 0, 2,
                                              (const uint8_t *)"halowmesh", 9, false,
                                              extra, (uint16_t)sizeof(extra));
    CHECK(n > 0, "a body with caller-supplied elements builds");
    /* They go before the discovery blob, where a mac80211 peer puts its own
     * S1G elements. A CONFIRM's fixed part is category, action, capability
     * (2 octets) and AID (2), so the caller's elements start at 6. */
    CHECK(memcmp(&b[6], extra, sizeof(extra)) == 0,
          "caller elements are copied verbatim, immediately after the fixed part");
    CHECK(b[6 + sizeof(extra)] == 0x01, "Supported Rates follows the caller's elements");

    /* The real regression guard: the Peer Management IE must still be findable
     * past an element the parser does not understand. */
    uint16_t llid = 0, plid = 0;
    CHECK(umac_mesh_ies_get_peer_llid(b, n, &llid) && llid == 0x1234,
          "llid is still recovered with an unknown element in the way");
    CHECK(umac_mesh_ies_get_peer_plid(b, n, &plid) && plid == 0x5678,
          "plid is still recovered with an unknown element in the way");

    /* A length that wraps the size computation must be refused, not memcpy'd
     * into the caller's stack. As a uint16_t the sum wrapped small, the guard
     * passed, and ~64KB went into the caller's buffer with every length in
     * sight still looking sane. */
    uint8_t fenced[128];
    memset(fenced, 0x5a, sizeof(fenced));
    CHECK(umac_mesh_ies_build_mpm_body(fenced, sizeof(fenced), 2, 1, 2, 0, 1,
                                       (const uint8_t *)"halowmesh", 9, false,
                                       extra, 0xfff0u) == 0,
          "an extra_ies length that wraps the size computation is refused");
    int fence_ok = 1;
    for (size_t i = 0; i < sizeof(fenced); i++) { if (fenced[i] != 0x5a) { fence_ok = 0; break; } }
    CHECK(fence_ok, "...and nothing was written to the caller's buffer");
}

int main(void)
{
    printf("=== mesh IE / MPM golden-byte tests ===\n");
    test_discovery_ies();
    test_mpm_bodies();
    test_extra_ies();
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
