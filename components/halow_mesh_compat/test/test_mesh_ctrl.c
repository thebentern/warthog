/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Mesh Control field byte layout (morselib src/umac/mesh/umac_mesh_ctrl.c).
 *
 * Pinned at absolute offsets against mac80211's struct ieee80211s_hdr, because
 * a round trip agrees with itself no matter how wrong it is: a big-endian
 * sequence number, or AE addresses swapped, survives build->parse and is only
 * caught by a peer that then forwards to the wrong host. The sequence number is
 * what duplicate suppression keys on, so an endianness slip there would make a
 * relay drop or loop the wrong frames.
 */
#include "umac_mesh_ctrl.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static const uint8_t DA[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
static const uint8_t SA[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };

int main(void)
{
    uint8_t buf[UMAC_MESH_CTRL_LEN_MAX + 4];
    struct umac_mesh_ctrl mc, back;
    uint16_t n, used;

    /* ---- lengths ---------------------------------------------------- */
    CHECK(umac_mesh_ctrl_len(0) == 6,  "AE 0 is 6 octets");
    CHECK(umac_mesh_ctrl_len(1) == 12, "AE 1 is 12 octets");
    CHECK(umac_mesh_ctrl_len(2) == 18, "AE 2 is 18 octets");
    CHECK(umac_mesh_ctrl_len(3) == 0,  "AE 3 is reserved and has no length");
    CHECK(umac_mesh_ctrl_len(0xf1) == 12, "only the low two bits select the mode");

    /* ---- AE 0: what warthog emits today ------------------------------ */
    memset(&mc, 0, sizeof(mc));
    mc.flags = 0; mc.ttl = 31; mc.seq = 0x04030201u;
    n = umac_mesh_ctrl_build(buf, sizeof(buf), &mc);
    CHECK(n == 6, "AE 0 builds 6 octets (got %u)", n);
    CHECK(buf[0] == 0x00, "flags at +0");
    CHECK(buf[1] == 31,   "ttl at +1");
    CHECK(buf[2] == 0x01 && buf[3] == 0x02 && buf[4] == 0x03 && buf[5] == 0x04,
          "seq is little-endian at +2..+5 (%02x %02x %02x %02x)", buf[2], buf[3], buf[4], buf[5]);

    /* ---- AE 2: individually addressed, proxied both ends ------------- */
    memset(&mc, 0, sizeof(mc));
    mc.flags = UMAC_MESH_CTRL_AE_A5A6 | UMAC_MESH_CTRL_FLAG_PS_LEVEL;
    mc.ttl = 7; mc.seq = 0xdeadbeefu;
    memcpy(mc.eaddr1, DA, 6); memcpy(mc.eaddr2, SA, 6);
    n = umac_mesh_ctrl_build(buf, sizeof(buf), &mc);
    CHECK(n == 18, "AE 2 builds 18 octets (got %u)", n);
    CHECK(buf[0] == 0x06, "flags carry AE 2 and PS level untouched (%02x)", buf[0]);
    CHECK(memcmp(&buf[6], DA, 6) == 0,  "AE 2: proxied DA at +6");
    CHECK(memcmp(&buf[12], SA, 6) == 0, "AE 2: proxied SA at +12");
    CHECK(buf[2] == 0xef && buf[5] == 0xde, "seq LE for a high-bit value");

    CHECK(umac_mesh_ctrl_parse(buf, n, &back, &used), "AE 2 parses");
    CHECK(used == 18, "AE 2 consumes 18 (got %u)", used);
    CHECK(back.seq == 0xdeadbeefu && back.ttl == 7, "AE 2 seq/ttl round trip");
    CHECK(umac_mesh_ctrl_ae(&back) == 2, "AE 2 mode recovered");
    CHECK(memcmp(back.eaddr1, DA, 6) == 0 && memcmp(back.eaddr2, SA, 6) == 0,
          "AE 2 addresses recovered in order");

    /* ---- AE 1: group frame from a proxy, one address ----------------- */
    memset(&mc, 0, sizeof(mc));
    mc.flags = UMAC_MESH_CTRL_AE_A4; mc.ttl = 3; mc.seq = 9;
    memcpy(mc.eaddr1, SA, 6);
    memcpy(mc.eaddr2, DA, 6); /* must NOT be written for AE 1 */
    memset(buf, 0xa5, sizeof(buf));
    n = umac_mesh_ctrl_build(buf, sizeof(buf), &mc);
    CHECK(n == 12, "AE 1 builds 12 octets (got %u)", n);
    CHECK(memcmp(&buf[6], SA, 6) == 0, "AE 1: the one address at +6");
    CHECK(buf[12] == 0xa5, "AE 1 writes nothing past +12");

    /* ---- parse against mac80211-shaped bytes, not our own builder ---- */
    {
        const uint8_t wire[12] = { 0x01, 0x1e, 0x2a, 0x00, 0x00, 0x00,
                                   0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
        CHECK(umac_mesh_ctrl_parse(wire, sizeof(wire), &back, &used), "mac80211 AE 1 wire parses");
        CHECK(back.ttl == 30 && back.seq == 42, "ttl 30 seq 42 from wire");
        CHECK(memcmp(back.eaddr1, SA, 6) == 0, "wire eaddr1");
        CHECK(used == 12, "wire consumed 12");
    }

    /* ---- refusals: every length is attacker-controlled --------------- */
    {
        uint8_t w[18] = { 0x02, 5, 1, 0, 0, 0 };
        CHECK(!umac_mesh_ctrl_parse(w, 17, &back, &used), "AE 2 with 17 octets refused");
        CHECK(!umac_mesh_ctrl_parse(w, 12, &back, &used), "AE 2 with 12 octets refused");
        w[0] = 0x01;
        CHECK(!umac_mesh_ctrl_parse(w, 11, &back, &used), "AE 1 with 11 octets refused");
        CHECK(umac_mesh_ctrl_parse(w, 12, &back, &used),  "AE 1 with 12 octets accepted");
        w[0] = 0x03;
        CHECK(!umac_mesh_ctrl_parse(w, 18, &back, &used), "reserved AE 3 refused even with room");
        w[0] = 0x00;
        CHECK(!umac_mesh_ctrl_parse(w, 5, &back, &used),  "5 octets refused");
        CHECK(!umac_mesh_ctrl_parse(NULL, 18, &back, &used), "NULL input refused");
        memset(&mc, 0, sizeof(mc)); mc.flags = 0x03;
        CHECK(umac_mesh_ctrl_build(buf, sizeof(buf), &mc) == 0, "reserved AE 3 cannot be built");
        mc.flags = 0x02;
        CHECK(umac_mesh_ctrl_build(buf, 17, &mc) == 0, "AE 2 into 17 octets refused");
        CHECK(umac_mesh_ctrl_build(buf, 18, &mc) == 18, "AE 2 into exactly 18 accepted");
    }

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_mesh_ctrl: all passed\n");
    return 0;
}
