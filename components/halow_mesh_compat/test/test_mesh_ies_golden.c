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
 *  (C) the capacity octets of the Mesh Configuration: with no table reader
 *      the golden frames above hold (0 peerings, accepting); with one,
 *      Formation Info is min(peerings, 63) << 1 as mac80211 writes it
 *      (net/mac80211/mesh.c, v6.6) and a full table clears capability bit 0
 *      and nothing else; set_accepting rewrites that one bit in a built blob
 *      and refuses a blob with no (or a truncated) Mesh Configuration
 *  (D) peer_openable: a neighbour's Mesh Configuration that clears its
 *      accepting bit or names another auth protocol is not opened toward; an
 *      absent or truncated element does not block (the peer can still Close)
 *  (E) probe-response IEs: under SAE, hostap's RSN element verbatim between
 *      Supported Rates and Mesh ID; on an open mesh, or with no usable RSN
 *      bytes, exactly the discovery blob -- a mac80211 peer drops a beacon or
 *      probe response whose RSN presence disagrees with its own mesh security
 *  (F) probe-request IEs: a zero-length SSID then the Mesh ID element, the only
 *      shape mac80211 (mesh.c, ieee80211_mesh_rx_probe_req) answers
 *  (G) the RSN hand-over check: an RSN element first, lengths inside the
 *      buffer, at most UMAC_MESH_IES_RSN_MAXLEN
 *  Both: a Mesh Configuration shorter than its 7 octets is skipped, never
 *  read or written past. The blob sits in an exactly-sized heap buffer, so
 *  `make SAN=1` turns an overrun into an ASan abort, not a silent pass.
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

static bool s_cap_accepting;
static uint8_t s_cap_peerings;
static void fake_capacity(struct umac_mesh_ies_capacity *out)
{
    out->accepting = s_cap_accepting;
    out->peerings = s_cap_peerings;
}

static void test_capacity_octets(void)
{
    printf("\n=== (C) capacity octets: Formation Info and the accepting bit ===\n");
    uint8_t cfg[2 + UMAC_MESH_CFG_IE_LEN];

    CHECK(umac_mesh_ies_capacity_fn == NULL, "no table reader is installed by default");
    CHECK(umac_mesh_ies_build_mesh_config(cfg, sizeof(cfg), false) == 9 && cfg[7] == 0x00 &&
          cfg[8] == 0x01, "without one: 0 peerings, accepting -- the golden frames above");

    umac_mesh_ies_capacity_fn = fake_capacity;
    umac_mesh_ies_cap_forwarding = 1;
    s_cap_accepting = false;
    s_cap_peerings = 4;
    static const uint8_t FULL4[] = { 0x71, 0x07, 0x01, 0x01, 0x00, 0x01, 0x00, 0x08, 0x08 };
    CHECK(umac_mesh_ies_build_mesh_config(cfg, sizeof(cfg), false) == sizeof(FULL4) &&
          memcmp(cfg, FULL4, sizeof(FULL4)) == 0,
          "4 peers, full, forwarding: Formation 0x08, capability 0x08 (got %02x %02x)",
          cfg[7], cfg[8]);
    s_cap_accepting = true;
    s_cap_peerings = 3;
    (void)umac_mesh_ies_build_mesh_config(cfg, sizeof(cfg), false);
    CHECK(cfg[7] == 0x06 && cfg[8] == 0x09, "3 peers, room: 0x06 / 0x09 (got %02x %02x)", cfg[7], cfg[8]);
    s_cap_peerings = 63;
    (void)umac_mesh_ies_build_mesh_config(cfg, sizeof(cfg), false);
    CHECK(cfg[7] == 0x7e, "63 peerings: 0x7e (got %02x)", cfg[7]);
    s_cap_peerings = 200;
    (void)umac_mesh_ies_build_mesh_config(cfg, sizeof(cfg), false);
    CHECK(cfg[7] == 0x7e, "capped at 63, never into the Connected-to-AS bit (got %02x)", cfg[7]);

    /* set_accepting on a built discovery blob, as the probe response uses it. */
    uint8_t on[UMAC_MESH_DISCOVERY_IES_MAXLEN], off[UMAC_MESH_DISCOVERY_IES_MAXLEN];
    s_cap_peerings = 4;
    s_cap_accepting = true;
    uint16_t non = umac_mesh_ies_build_discovery(on, sizeof(on), MESH_ID, MESH_ID_LEN, false);
    s_cap_accepting = false;
    uint16_t noff = umac_mesh_ies_build_discovery(off, sizeof(off), MESH_ID, MESH_ID_LEN, false);
    umac_mesh_ies_capacity_fn = NULL;
    umac_mesh_ies_cap_forwarding = 0;
    CHECK(non == noff && non > 0 && memcmp(on, off, non - 1u) == 0 && on[non - 1u] == 0x09 &&
          off[noff - 1u] == 0x08, "full changes the capability octet's bit 0 only");
    CHECK(umac_mesh_ies_set_accepting(off, noff, true) && memcmp(on, off, non) == 0,
          "set_accepting(true) restores the accepting blob byte for byte");
    CHECK(umac_mesh_ies_set_accepting(off, noff, false) && off[noff - 1u] == 0x08 &&
          memcmp(on, off, non - 1u) == 0, "set_accepting(false) clears it again, nothing else");
    CHECK(!umac_mesh_ies_set_accepting(off, (uint16_t)(noff - 9u), true),
          "no Mesh Configuration in the blob: false");
    uint8_t trunc[] = { 0x72, 0x09, 'x', 0x71, 0x07 };
    uint8_t trunc0[sizeof(trunc)];
    memcpy(trunc0, trunc, sizeof(trunc));
    CHECK(!umac_mesh_ies_set_accepting(trunc, sizeof(trunc), true) &&
          memcmp(trunc, trunc0, sizeof(trunc)) == 0, "truncated element: false, nothing written");
    CHECK(!umac_mesh_ies_set_accepting(NULL, 0, true), "NULL blob: false");

    /* A 3-octet Mesh Configuration ends the blob: its capability octet would be 3 past the end. */
    static const uint8_t shortcfg[] = { 0x72, 0x02, 's', 'n', 0x71, 0x03, 0x01, 0x01, 0x00 };
    uint8_t *h = malloc(sizeof(shortcfg));
    if (h != NULL)
    {
        memcpy(h, shortcfg, sizeof(shortcfg));
        CHECK(!umac_mesh_ies_set_accepting(h, sizeof(shortcfg), false) &&
              memcmp(h, shortcfg, sizeof(shortcfg)) == 0, "short Mesh Configuration: false, nothing written");
        free(h);
    }
    /* A short one, then a full one: only the full one's capability octet moves. */
    uint8_t two[] = { 0x71, 0x03, 0x01, 0x01, 0x00, 0x71, 0x07, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01 };
    CHECK(umac_mesh_ies_set_accepting(two, sizeof(two), false) && two[13] == 0x00 && two[4] == 0x00 &&
          two[2] == 0x01, "short element skipped: the full one after it is rewritten");
}

static void test_peer_openable(void)
{
    printf("\n=== (D) peer_openable: the neighbour's own Mesh Configuration ===\n");
    uint8_t c[] = { 0x71, 0x07, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01 };
    CHECK(umac_mesh_ies_peer_openable(c, sizeof(c), false), "open, accepting: openable");
    c[8] = 0x08;
    CHECK(!umac_mesh_ies_peer_openable(c, sizeof(c), false), "forwarding but not accepting: not");
    c[8] = 0x00;
    CHECK(!umac_mesh_ies_peer_openable(c, sizeof(c), false), "not accepting: not");
    c[8] = 0x19;
    c[6] = 0x01;
    CHECK(!umac_mesh_ies_peer_openable(c, sizeof(c), false), "SAE neighbour, we are open: not");
    CHECK(umac_mesh_ies_peer_openable(c, sizeof(c), true), "SAE neighbour, we are SAE: openable");
    c[6] = 0x00;
    CHECK(!umac_mesh_ies_peer_openable(c, sizeof(c), true), "open neighbour, we are SAE: not");

    uint8_t blob[UMAC_MESH_DISCOVERY_IES_MAXLEN];
    uint16_t n = umac_mesh_ies_build_discovery(blob, sizeof(blob), MESH_ID, MESH_ID_LEN, false);
    blob[n - 1u] = 0x00;
    CHECK(!umac_mesh_ies_peer_openable(blob, n, false), "found after Supported Rates and Mesh ID");
    CHECK(umac_mesh_ies_peer_openable(blob, (uint16_t)(n - 9u), false),
          "no Mesh Configuration: openable (its Close still refuses us)");
    uint8_t trunc[] = { 0x72, 0x09, 'x' };
    CHECK(umac_mesh_ies_peer_openable(trunc, sizeof(trunc), false), "truncated: openable");
    CHECK(umac_mesh_ies_peer_openable(NULL, 0, false), "NULL: openable");

    static const uint8_t shortcfg[] = { 0x72, 0x02, 's', 'n', 0x71, 0x03, 0x01, 0x01, 0x00 };
    uint8_t *h = malloc(sizeof(shortcfg));
    if (h != NULL)
    {
        memcpy(h, shortcfg, sizeof(shortcfg));
        CHECK(umac_mesh_ies_peer_openable(h, sizeof(shortcfg), false),
              "short Mesh Configuration at the end: treated as absent, not read past");
        free(h);
    }
    uint8_t two[] = { 0x71, 0x03, 0x01, 0x01, 0x00, 0x71, 0x07, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01 };
    CHECK(umac_mesh_ies_peer_openable(two, sizeof(two), false),
          "short element skipped: the full one after it is read (open, accepting)");
}

/* hostap's wpa_write_rsn_ie() output for warthog's mesh config (mesh_rsn.c):
 * RSN v1, group CCMP-128, one pairwise CCMP-128, one AKM SAE 00-0F-AC:8,
 * capabilities 0 (AT+MESHPMF=0). */
static const uint8_t HOSTAP_MESH_RSN[] = {
    0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f,
    0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x08, 0x00, 0x00,
};

/* The SAE probe response's IEs for "warthog-mesh-test": rates, RSN, Mesh ID, config. */
static const uint8_t GOLDEN_PRSP_SAE[] = {
    0x01, 0x08, 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c,
    0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f,
    0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x08, 0x00, 0x00,
    0x72, 0x11, 0x77, 0x61, 0x72, 0x74, 0x68, 0x6f, 0x67, 0x2d, 0x6d, 0x65,
    0x73, 0x68, 0x2d, 0x74, 0x65, 0x73, 0x74,
    0x71, 0x07, 0x01, 0x01, 0x00, 0x01, 0x01, 0x00, 0x01,
};

/* The probe request's IEs for "warthog-mesh-test": SSID(0), then Mesh ID. */
static const uint8_t GOLDEN_PREQ[] = {
    0x00, 0x00,
    0x72, 0x11, 0x77, 0x61, 0x72, 0x74, 0x68, 0x6f, 0x67, 0x2d, 0x6d, 0x65,
    0x73, 0x68, 0x2d, 0x74, 0x65, 0x73, 0x74,
};

/* Does an element walk over [p, p+n) meet element @p eid? */
static bool has_eid(const uint8_t *p, uint16_t n, uint8_t eid)
{
    uint16_t off = 0;
    while (off + 2u <= n && off + 2u + p[off + 1] <= n)
    {
        if (p[off] == eid) { return true; }
        off = (uint16_t)(off + 2u + p[off + 1]);
    }
    return false;
}

static void test_probe_resp_ies(void)
{
    printf("\n=== (E) probe-response IEs: RSN under SAE only ===\n");
    uint8_t buf[UMAC_MESH_PROBE_RESP_IES_MAXLEN];
    uint8_t ref[UMAC_MESH_DISCOVERY_IES_MAXLEN];

    uint16_t n = umac_mesh_ies_build_probe_resp(buf, sizeof(buf), MESH_ID, MESH_ID_LEN, true,
                                                HOSTAP_MESH_RSN, sizeof(HOSTAP_MESH_RSN));
    if (n == sizeof(GOLDEN_PRSP_SAE) && memcmp(buf, GOLDEN_PRSP_SAE, n) != 0) {
        hexdiff(buf, GOLDEN_PRSP_SAE, n);
    }
    CHECK(n == sizeof(GOLDEN_PRSP_SAE) && memcmp(buf, GOLDEN_PRSP_SAE, n) == 0,
          "SAE: rates, hostap's RSN verbatim, Mesh ID, Mesh Config (auth 1) -- %u bytes", n);
    CHECK(umac_mesh_ies_set_accepting(buf, n, false) && buf[n - 1u] == 0x00 &&
          !umac_mesh_ies_peer_openable(buf, n, true),
          "SAE: the Mesh Configuration is still found past the RSN element");

    uint16_t rn = umac_mesh_ies_build_discovery(ref, sizeof(ref), MESH_ID, MESH_ID_LEN, false);
    n = umac_mesh_ies_build_probe_resp(buf, sizeof(buf), MESH_ID, MESH_ID_LEN, false,
                                       HOSTAP_MESH_RSN, sizeof(HOSTAP_MESH_RSN));
    CHECK(n == rn && n > 0 && memcmp(buf, ref, n) == 0 && !has_eid(buf, n, UMAC_MESH_EID_RSN),
          "open: RSN bytes supplied, none emitted -- the discovery blob byte for byte");

    rn = umac_mesh_ies_build_discovery(ref, sizeof(ref), MESH_ID, MESH_ID_LEN, true);
    n = umac_mesh_ies_build_probe_resp(buf, sizeof(buf), MESH_ID, MESH_ID_LEN, true, NULL, 0);
    CHECK(n == rn && n > 0 && memcmp(buf, ref, n) == 0,
          "SAE before hostap's RSN exists: the SAE discovery blob, no RSN element");
    uint8_t bad[sizeof(HOSTAP_MESH_RSN)];
    memcpy(bad, HOSTAP_MESH_RSN, sizeof(bad));
    bad[1] = 0x30; /* runs past the buffer */
    n = umac_mesh_ies_build_probe_resp(buf, sizeof(buf), MESH_ID, MESH_ID_LEN, true, bad, sizeof(bad));
    CHECK(n == rn && memcmp(buf, ref, n) == 0, "SAE with a truncated RSN element: left out, not sent");

    printf("--- sizing sweep ---\n");
    uint8_t maxid[UMAC_MESH_IES_MESH_ID_MAXLEN];
    memset(maxid, 'z', sizeof(maxid));
    uint8_t maxrsn[UMAC_MESH_IES_RSN_MAXLEN];
    memset(maxrsn, 0xee, sizeof(maxrsn));
    maxrsn[0] = UMAC_MESH_EID_RSN;
    maxrsn[1] = (uint8_t)(sizeof(maxrsn) - 2u);
    n = umac_mesh_ies_build_probe_resp(buf, sizeof(buf), maxid, sizeof(maxid), true, maxrsn,
                                       sizeof(maxrsn));
    CHECK(n == UMAC_MESH_PROBE_RESP_IES_MAXLEN,
          "the longest Mesh ID and RSN fill exactly UMAC_MESH_PROBE_RESP_IES_MAXLEN (%u)",
          (unsigned)UMAC_MESH_PROBE_RESP_IES_MAXLEN);
    int wrong_rc = 0, leaked = 0;
    for (uint16_t cap = 0; cap < sizeof(GOLDEN_PRSP_SAE); cap++) {
        uint8_t probe[sizeof(GOLDEN_PRSP_SAE)];
        memset(probe, 0xA5, sizeof(probe));
        if (umac_mesh_ies_build_probe_resp(probe, cap, MESH_ID, MESH_ID_LEN, true, HOSTAP_MESH_RSN,
                                           sizeof(HOSTAP_MESH_RSN)) != 0) wrong_rc++;
        for (uint16_t i = 0; i < sizeof(probe); i++) {
            if (probe[i] != 0xA5) { leaked = 1; break; }
        }
    }
    CHECK(wrong_rc == 0 && leaked == 0,
          "every out_len below %u returns 0 and writes nothing", (unsigned)sizeof(GOLDEN_PRSP_SAE));
    CHECK(umac_mesh_ies_build_probe_resp(NULL, 64, MESH_ID, MESH_ID_LEN, true, NULL, 0) == 0 &&
          umac_mesh_ies_build_probe_resp(buf, sizeof(buf), NULL, MESH_ID_LEN, true, NULL, 0) == 0 &&
          umac_mesh_ies_build_probe_resp(buf, sizeof(buf), MESH_ID, 0, true, NULL, 0) == 0 &&
          umac_mesh_ies_build_probe_resp(buf, sizeof(buf), maxid,
                                         UMAC_MESH_IES_MESH_ID_MAXLEN + 1, true, NULL, 0) == 0,
          "NULL buffer, NULL / empty / over-long Mesh ID: refused");
}

static void test_probe_req_ies(void)
{
    printf("\n=== (F) probe-request IEs: wildcard SSID, then Mesh ID ===\n");
    uint8_t buf[UMAC_MESH_PROBE_REQ_IES_MAXLEN];
    uint16_t n = umac_mesh_ies_build_probe_req(buf, sizeof(buf), MESH_ID, MESH_ID_LEN);
    CHECK(n == sizeof(GOLDEN_PREQ) && memcmp(buf, GOLDEN_PREQ, n) == 0,
          "SSID(0) then Mesh ID \"warthog-mesh-test\" -- %u bytes", n);

    uint8_t maxid[UMAC_MESH_IES_MESH_ID_MAXLEN];
    memset(maxid, 'z', sizeof(maxid));
    CHECK(umac_mesh_ies_build_probe_req(buf, sizeof(buf), maxid, sizeof(maxid)) ==
          UMAC_MESH_PROBE_REQ_IES_MAXLEN, "a 32-byte Mesh ID fills UMAC_MESH_PROBE_REQ_IES_MAXLEN");
    int wrong_rc = 0, leaked = 0;
    for (uint16_t cap = 0; cap < sizeof(GOLDEN_PREQ); cap++) {
        uint8_t probe[sizeof(GOLDEN_PREQ)];
        memset(probe, 0xA5, sizeof(probe));
        if (umac_mesh_ies_build_probe_req(probe, cap, MESH_ID, MESH_ID_LEN) != 0) wrong_rc++;
        for (uint16_t i = 0; i < sizeof(probe); i++) {
            if (probe[i] != 0xA5) { leaked = 1; break; }
        }
    }
    CHECK(wrong_rc == 0 && leaked == 0,
          "every out_len below %u returns 0 and writes nothing", (unsigned)sizeof(GOLDEN_PREQ));
    CHECK(umac_mesh_ies_build_probe_req(NULL, 64, MESH_ID, MESH_ID_LEN) == 0 &&
          umac_mesh_ies_build_probe_req(buf, sizeof(buf), NULL, MESH_ID_LEN) == 0 &&
          umac_mesh_ies_build_probe_req(buf, sizeof(buf), MESH_ID, 0) == 0 &&
          umac_mesh_ies_build_probe_req(buf, sizeof(buf), maxid, UMAC_MESH_IES_MESH_ID_MAXLEN + 1) == 0,
          "NULL buffer, NULL / empty / over-long Mesh ID: refused");
    uint8_t roomy[2 * UMAC_MESH_PROBE_REQ_IES_MAXLEN];
    uint8_t longid[UMAC_MESH_IES_MESH_ID_MAXLEN + 1];
    memset(longid, 'z', sizeof(longid));
    CHECK(umac_mesh_ies_build_probe_req(roomy, sizeof(roomy), longid, sizeof(longid)) == 0,
          "a 33-byte Mesh ID is refused even when the buffer has room for it");
}

static void test_rsn_valid(void)
{
    printf("\n=== (G) which bytes count as our RSN element ===\n");
    CHECK(umac_mesh_ies_rsn_valid(HOSTAP_MESH_RSN, sizeof(HOSTAP_MESH_RSN)),
          "hostap's mesh RSN element: accepted");
    uint8_t two[sizeof(HOSTAP_MESH_RSN) + 3];
    memcpy(two, HOSTAP_MESH_RSN, sizeof(HOSTAP_MESH_RSN));
    two[sizeof(HOSTAP_MESH_RSN)] = 0xf4; /* RSNX, as hostap appends when it has capabilities */
    two[sizeof(HOSTAP_MESH_RSN) + 1] = 0x01;
    two[sizeof(HOSTAP_MESH_RSN) + 2] = 0x20;
    CHECK(umac_mesh_ies_rsn_valid(two, sizeof(two)), "RSN followed by RSNX: accepted whole");

    uint8_t b[UMAC_MESH_IES_RSN_MAXLEN + 1];
    memset(b, 0, sizeof(b));
    b[0] = UMAC_MESH_EID_RSN;
    b[1] = (uint8_t)(sizeof(b) - 2u);
    CHECK(!umac_mesh_ies_rsn_valid(b, sizeof(b)), "longer than UMAC_MESH_IES_RSN_MAXLEN: refused");
    CHECK(!umac_mesh_ies_rsn_valid(NULL, sizeof(HOSTAP_MESH_RSN)), "NULL: refused");
    CHECK(!umac_mesh_ies_rsn_valid(HOSTAP_MESH_RSN, 0), "empty: refused");
    memcpy(b, HOSTAP_MESH_RSN, sizeof(HOSTAP_MESH_RSN));
    b[0] = 0x31;
    CHECK(!umac_mesh_ies_rsn_valid(b, sizeof(HOSTAP_MESH_RSN)), "first element not RSN (49): refused");
    memcpy(b, HOSTAP_MESH_RSN, sizeof(HOSTAP_MESH_RSN));
    CHECK(!umac_mesh_ies_rsn_valid(b, sizeof(HOSTAP_MESH_RSN) - 1u), "one octet short: refused");
    CHECK(!umac_mesh_ies_rsn_valid(b, sizeof(HOSTAP_MESH_RSN) + 1u), "a stray trailing octet: refused");
    static const uint8_t tiny[] = { 0x30, 0x01, 0x01 };
    static const uint8_t empty_rsn[] = { 0x30, 0x00 };
    CHECK(!umac_mesh_ies_rsn_valid(tiny, sizeof(tiny)) && !umac_mesh_ies_rsn_valid(empty_rsn, 2),
          "an RSN element too short for its version: refused");
    /* 802.11 makes every RSNE field after Version optional; hostap's parser takes this one. */
    static const uint8_t version_only[] = { 0x30, 0x02, 0x01, 0x00 };
    CHECK(umac_mesh_ies_rsn_valid(version_only, sizeof(version_only)),
          "a version-only RSN element: accepted");
    /* One octet on the heap: reading a length octet past it is an ASan abort under SAN=1. */
    uint8_t *lone = malloc(1);
    if (lone != NULL)
    {
        lone[0] = UMAC_MESH_EID_RSN;
        CHECK(!umac_mesh_ies_rsn_valid(lone, 1), "a lone element id: refused, its length never read");
        free(lone);
    }
}

int main(void)
{
    printf("=== mesh IE / MPM golden-byte tests ===\n");
    test_discovery_ies();
    test_mpm_bodies();
    test_extra_ies();
    test_capacity_octets();
    test_peer_openable();
    test_probe_resp_ies();
    test_probe_req_ies();
    test_rsn_valid();
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
