/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Golden-bytes test for the 802.11s 4-address mesh data header
 * (morselib src/umac/mesh/umac_mesh_ies.c, umac_mesh_ies_build_data_hdr4).
 *
 * This is the header every IP packet wears when it crosses the HaLow mesh
 * link -- the data plane a Meshtastic UDP transport will ride on. Its layout
 * is fixed by IEEE 802.11-2020 s9.3.2.1 table 9-30 and by the receiver's
 * dot11_get_da()/dot11_get_sa_data(), which read addr3/addr4 when both
 * ToDS and FromDS are set. Get any field wrong and the receiver reconstructs
 * the wrong 802.3 endpoints, silently.
 */
#include "umac_mesh_ies.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static const uint8_t RA[6] = { 0xa8,0xdd,0x9f,0x4d,0xc7,0xf8 }; /* a real board's HaLow MAC */
static const uint8_t TA[6] = { 0x3c,0x1a,0xcc,0x4c,0x81,0x9d };
static const uint8_t DA[6] = { 0x01,0x00,0x5e,0x00,0x00,0xfb }; /* mDNS multicast */
static const uint8_t SA[6] = { 0xde,0xad,0xbe,0xef,0x00,0x01 };

int main(void)
{
    uint8_t h[UMAC_MESH_DATA_HDR4_LEN];
    printf("=== 4-address mesh data header ===\n");

    uint16_t n = umac_mesh_ies_build_data_hdr4(h, RA, TA, DA, SA);
    CHECK(n == UMAC_MESH_DATA_HDR4_LEN, "header is %u bytes (want 30)", n);

    /* Frame control, little-endian on the wire:
     *   type=2 (DATA) <<2 = 0x08, subtype=8 (QoS Data) <<4 = 0x80  -> low byte 0x88
     *   ToDS 0x0100 | FromDS 0x0200                                 -> high byte 0x03 */
    CHECK(h[0] == 0x88, "FC low byte 0x88 = type DATA, subtype QoS Data (got 0x%02x)", h[0]);
    CHECK(h[1] == 0x03, "FC high byte 0x03 = ToDS|FromDS -- what makes it a 4-address frame (got 0x%02x)", h[1]);
    CHECK(h[2] == 0 && h[3] == 0, "duration = 0");
    CHECK(memcmp(&h[4],  RA, 6) == 0, "addr1 = RA (next hop peer)");
    CHECK(memcmp(&h[10], TA, 6) == 0, "addr2 = TA (us)");
    CHECK(memcmp(&h[16], DA, 6) == 0, "addr3 = DA (final destination)");
    CHECK(h[22] == 0 && h[23] == 0, "sequence control = 0 (datapath stamps it)");
    CHECK(memcmp(&h[24], SA, 6) == 0, "addr4 = SA (original source)");

    /* Multicast DA must survive into addr3 unchanged -- ARP, mDNS and
     * Meshtastic's UDP multicast all depend on this. */
    CHECK(h[16] == 0x01, "multicast bit of DA preserved in addr3");

    /* Endpoint recovery, as the receiver does it: with ToDS+FromDS,
     * DA = addr3 and SA = addr4. Both must round-trip. */
    CHECK(memcmp(&h[16], DA, 6) == 0 && memcmp(&h[24], SA, 6) == 0,
          "receiver recovers the exact 802.3 DA/SA from addr3/addr4");

    /* Single-hop case: RA==DA and TA==SA, and all four must STILL be written. */
    uint8_t s[UMAC_MESH_DATA_HDR4_LEN];
    umac_mesh_ies_build_data_hdr4(s, RA, TA, RA, TA);
    CHECK(memcmp(&s[4], &s[16], 6) == 0 && memcmp(&s[10], &s[24], 6) == 0,
          "single hop: addr1==addr3 and addr2==addr4, all four populated");

    CHECK(umac_mesh_ies_build_data_hdr4(h, NULL, TA, DA, SA) == 0, "NULL RA is refused");
    CHECK(umac_mesh_ies_build_data_hdr4(NULL, RA, TA, DA, SA) == 0, "NULL out is refused");

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
