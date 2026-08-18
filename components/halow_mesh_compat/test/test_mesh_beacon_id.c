/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Mesh ID matching over a beacon body
 * (morselib src/umac/mesh/umac_mesh_ies.c, umac_mesh_ies_beacon_has_mesh_id).
 *
 * This decides whether we answer a beaconing neighbour at all. A mac80211 or
 * OpenMANET mesh node beacons rather than probing, so warthog -- which only
 * ever answered probe requests -- could not otherwise discover one. Two ways
 * to get it wrong, both quiet: match too loosely and we peer with every mesh
 * in range, match too strictly and interop simply never happens.
 *
 * It also walks attacker-controlled length octets straight off the air, so the
 * truncation cases below are the point of the test, not padding.
 */
#include "umac_mesh_ies.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

#define FIXED 12 /* timestamp(8) + beacon interval(2) + capability(2) */

static const uint8_t MESH_ID[] = { 'o','p','e','n','m','a','n','e','t' };
#define MESH_ID_LEN ((uint8_t)sizeof(MESH_ID))

/* A beacon body: fixed fields, then Supported Rates, then Mesh ID, then Mesh
 * Configuration -- the order a real mesh beacon uses. */
static uint32_t build_beacon(uint8_t *out, const uint8_t *id, uint8_t id_len)
{
    uint32_t n = 0;
    memset(out, 0, FIXED);
    n = FIXED;
    out[n++] = UMAC_MESH_EID_SUPPORTED_RATES;
    out[n++] = 4;
    out[n++] = 0x8c; out[n++] = 0x12; out[n++] = 0x98; out[n++] = 0x24;
    if (id_len) {
        out[n++] = UMAC_MESH_EID_MESH_ID;
        out[n++] = id_len;
        memcpy(&out[n], id, id_len);
        n += id_len;
    }
    out[n++] = UMAC_MESH_EID_MESH_CONFIG;
    out[n++] = UMAC_MESH_CFG_IE_LEN;
    for (int i = 0; i < UMAC_MESH_CFG_IE_LEN; i++) out[n++] = 0;
    return n;
}

int main(void)
{
    uint8_t buf[128];
    uint32_t len = build_beacon(buf, MESH_ID, MESH_ID_LEN);

    CHECK(umac_mesh_ies_beacon_has_mesh_id(buf, len, MESH_ID, MESH_ID_LEN) == true,
          "matches our own Mesh ID");

    /* A different mesh in range must not be answered. */
    static const uint8_t OTHER[] = { 'w','a','r','t','h','o','g' };
    CHECK(umac_mesh_ies_beacon_has_mesh_id(buf, len, OTHER, (uint8_t)sizeof(OTHER)) == false,
          "rejects a different Mesh ID");

    /* Same prefix, different length -- must not match on prefix alone. */
    CHECK(umac_mesh_ies_beacon_has_mesh_id(buf, len, MESH_ID, MESH_ID_LEN - 1) == false,
          "rejects a prefix of our Mesh ID");

    /* No Mesh ID element at all (an ordinary AP beacon). */
    len = build_beacon(buf, NULL, 0);
    CHECK(umac_mesh_ies_beacon_has_mesh_id(buf, len, MESH_ID, MESH_ID_LEN) == false,
          "rejects a beacon carrying no Mesh ID element");

    /* Wildcards: a zero-length or NULL id would otherwise match everything. */
    len = build_beacon(buf, MESH_ID, MESH_ID_LEN);
    CHECK(umac_mesh_ies_beacon_has_mesh_id(buf, len, MESH_ID, 0) == false,
          "zero-length Mesh ID is not a wildcard");
    CHECK(umac_mesh_ies_beacon_has_mesh_id(buf, len, NULL, MESH_ID_LEN) == false,
          "NULL Mesh ID is not a wildcard");
    CHECK(umac_mesh_ies_beacon_has_mesh_id(NULL, len, MESH_ID, MESH_ID_LEN) == false,
          "NULL body is rejected");

    /* Truncation. Every one of these is reachable from the air. */
    CHECK(umac_mesh_ies_beacon_has_mesh_id(buf, FIXED, MESH_ID, MESH_ID_LEN) == false,
          "body with fixed fields only (no IEs)");
    CHECK(umac_mesh_ies_beacon_has_mesh_id(buf, FIXED - 1, MESH_ID, MESH_ID_LEN) == false,
          "body shorter than the fixed fields");
    CHECK(umac_mesh_ies_beacon_has_mesh_id(buf, 0, MESH_ID, MESH_ID_LEN) == false,
          "empty body");

    /* An element whose length runs past the end of the buffer must be refused,
     * not read. Under ASan a regression here is a hard failure, not a guess. */
    uint8_t evil[FIXED + 2];
    memset(evil, 0, sizeof(evil));
    evil[FIXED] = UMAC_MESH_EID_MESH_ID;
    evil[FIXED + 1] = 200; /* claims 200 bytes; 0 remain */
    CHECK(umac_mesh_ies_beacon_has_mesh_id(evil, sizeof(evil), MESH_ID, MESH_ID_LEN) == false,
          "element length past end of buffer is refused");

    /* Cut inside the Mesh ID itself. The element sits after Supported Rates
     * (2 + 4), so its last octet is at FIXED + 6 + 2 + MESH_ID_LEN - 1; one
     * byte short of that leaves a Mesh ID element claiming more than remains.
     * (Trimming the tail of the whole body would only truncate the Mesh
     * Configuration element that follows, which is a different case.) */
    len = build_beacon(buf, MESH_ID, MESH_ID_LEN);
    const uint32_t mesh_id_end = FIXED + 6 + 2 + MESH_ID_LEN;
    CHECK(umac_mesh_ies_beacon_has_mesh_id(buf, mesh_id_end - 1, MESH_ID, MESH_ID_LEN) == false,
          "truncated mid Mesh ID is refused");
    CHECK(umac_mesh_ies_beacon_has_mesh_id(buf, mesh_id_end, MESH_ID, MESH_ID_LEN) == true,
          "matches when the Mesh ID is the last complete element");

    /* A dangling single octet after a well-formed element: not enough room for
     * another element header, and must terminate the walk rather than read on. */
    CHECK(umac_mesh_ies_beacon_has_mesh_id(buf, FIXED + 1, MESH_ID, MESH_ID_LEN) == false,
          "single dangling octet terminates the walk");

    printf(failures ? "\nFAILED (%d)\n" : "\nALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
