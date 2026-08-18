/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * S1G Beacon parsing, against a frame captured off the air
 * (morselib src/umac/mesh/umac_mesh_ies.c).
 *
 * This is the frame warthog must act on to discover a mac80211/OpenMANET mesh
 * peer, and getting it wrong is silent in a specific and costly way: an S1G
 * Beacon uses a COMPRESSED header with no addr1 and no addr3, so the ordinary
 * dot11_get_ta() reads offset 10 -- the timestamp -- and yields a plausible
 * looking but different "peer address" on every single beacon. That is exactly
 * what warthog did, reporting last_ta values like 48:c3:a4:09:00:d5 that
 * changed each second while the real sender never appeared.
 *
 * The bytes below are a verbatim capture from an OpenMANET node (Raspberry Pi
 * 4 + Morse MM6108) beaconing on 923 MHz, taken with the driver's monitor
 * interface. Keeping the real frame means the offsets are pinned to something
 * a peer actually transmits rather than to a reading of the standard.
 */
#include "umac_mesh_ies.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* Captured 2026-08-17, 923 MHz, from e4:5f:01:28:bf:74 (mesh id "halowmesh"). */
static const uint8_t S1G_BEACON[] = {
    0x1c,0x00,                                     /* FC: type 3, subtype 1   */
    0x00,0x00,                                     /* duration                */
    0xe4,0x5f,0x01,0x28,0xbf,0x74,                 /* SA                      */
    0x48,0x63,0xfb,0x12,                           /* timestamp               */
    0x00,                                          /* change sequence         */
    0xd5,0x08,0x00,0x00,0xe8,0x03,0x00,0x00,0x00,0x00,
    0x05,0x02,0x00,0x01,                           /* TIM                     */
    0xd9,0x0f,0x9e,0x00,0x40,0xf8,0x80,0x0c,0x00,0x02,0x00,0x00,0xfd,0x00,0xfa,0x01,0x00,
    0xe8,0x06,0x03,0x02,0x29,0x2a,0xc4,0xcc,
    0xd6,0x02,0xe8,0x03,
    0x00,0x00,
    0x72,0x09,'h','a','l','o','w','m','e','s','h', /* Mesh ID (114)           */
    0x71,0x07,0x01,0x01,0x00,0x01,0x00,0x00,0x19,  /* Mesh Configuration (113)*/
};
static const uint8_t EXPECT_SA[6] = { 0xe4,0x5f,0x01,0x28,0xbf,0x74 };
static const uint8_t MESH_ID[] = { 'h','a','l','o','w','m','e','s','h' };

int main(void)
{
    uint8_t sa[6];

    CHECK(umac_mesh_ies_is_s1g_beacon(S1G_BEACON, sizeof(S1G_BEACON)),
          "captured frame is recognised as an S1G Beacon");
    CHECK(umac_mesh_ies_s1g_beacon_sa(S1G_BEACON, sizeof(S1G_BEACON), sa) &&
          memcmp(sa, EXPECT_SA, 6) == 0,
          "source address is read from offset 4, not offset 10");

    /* The timestamp is what a naive reader picks up, and it changes constantly. */
    CHECK(memcmp(&S1G_BEACON[10], EXPECT_SA, 6) != 0,
          "offset 10 is NOT the address (it is the timestamp)");

    CHECK(umac_mesh_ies_s1g_beacon_has_mesh_id(S1G_BEACON, sizeof(S1G_BEACON),
                                               MESH_ID, sizeof(MESH_ID)),
          "Mesh ID 'halowmesh' is found by walking from offset 15");

    static const uint8_t OTHER[] = { 'w','a','r','t','h','o','g' };
    CHECK(!umac_mesh_ies_s1g_beacon_has_mesh_id(S1G_BEACON, sizeof(S1G_BEACON),
                                                OTHER, sizeof(OTHER)),
          "a different mesh in range is not matched");
    CHECK(!umac_mesh_ies_s1g_beacon_has_mesh_id(S1G_BEACON, sizeof(S1G_BEACON),
                                                MESH_ID, sizeof(MESH_ID) - 1),
          "a prefix of the Mesh ID is not matched");

    /* An ordinary management beacon must not be taken for an S1G one. */
    static const uint8_t LEGACY[] = { 0x80, 0x00, 0,0, 0xff,0xff,0xff,0xff,0xff,0xff };
    CHECK(!umac_mesh_ies_is_s1g_beacon(LEGACY, sizeof(LEGACY)),
          "a legacy beacon (type 0, subtype 8) is not an S1G beacon");
    /* Nor a probe request, which shares the channel and arrives constantly. */
    static const uint8_t PROBE[] = { 0x40, 0x00, 0,0, 0xff,0xff,0xff,0xff,0xff,0xff };
    CHECK(!umac_mesh_ies_is_s1g_beacon(PROBE, sizeof(PROBE)), "a probe request is rejected");

    /* Truncation: every one of these is reachable from the air. */
    CHECK(!umac_mesh_ies_is_s1g_beacon(S1G_BEACON, 1), "1-byte frame cannot be classified");
    CHECK(!umac_mesh_ies_s1g_beacon_sa(S1G_BEACON, 9, sa), "SA needs 10 bytes present");
    CHECK(umac_mesh_ies_s1g_beacon_sa(S1G_BEACON, 10, sa) && memcmp(sa, EXPECT_SA, 6) == 0,
          "SA is readable from exactly 10 bytes");
    CHECK(!umac_mesh_ies_s1g_beacon_has_mesh_id(S1G_BEACON, 14, MESH_ID, sizeof(MESH_ID)),
          "a frame ending before the IEs has no Mesh ID");
    /* Cut inside the Mesh ID element itself. */
    CHECK(!umac_mesh_ies_s1g_beacon_has_mesh_id(S1G_BEACON, sizeof(S1G_BEACON) - 12,
                                                MESH_ID, sizeof(MESH_ID)),
          "a Mesh ID element truncated by the frame end is refused");

    /* An element claiming more than remains must be refused, not read. */
    uint8_t evil[UMAC_MESH_S1G_BEACON_IE_OFFSET + 2];
    memset(evil, 0, sizeof(evil));
    evil[0] = 0x1c; evil[1] = 0x00;
    evil[UMAC_MESH_S1G_BEACON_IE_OFFSET] = 114;
    evil[UMAC_MESH_S1G_BEACON_IE_OFFSET + 1] = 200;
    CHECK(!umac_mesh_ies_s1g_beacon_has_mesh_id(evil, sizeof(evil), MESH_ID, sizeof(MESH_ID)),
          "element length past the end of the buffer is refused");

    CHECK(!umac_mesh_ies_is_s1g_beacon(NULL, 32), "NULL frame is rejected");
    CHECK(!umac_mesh_ies_s1g_beacon_sa(S1G_BEACON, sizeof(S1G_BEACON), NULL),
          "NULL output is rejected");

    /* ---- optional header fields shift every element ------------------
     *
     * The S1G Beacon's compressed header is not a fixed size. Next TBTT (3),
     * Compressed SSID (4) and ANO (1) each appear only when their
     * frame-control bit is set, in that order -- exactly what the ported
     * driver's morse_dot11_find_s1g_beacon_ies() skips. Assuming the shortest
     * form walks into the middle of a field: the Mesh ID is simply not found,
     * the peer is never discovered, and nothing logs or counts it. It looks
     * identical to the peer being out of range.
     *
     * The captured fixture has no optional fields set, so that shape was the
     * only one the offset ever worked for. Rebuild it with each combination
     * and require the walk to still land. */
    {
        static const struct { uint16_t bit; uint32_t size; const char *name; } OPT[] = {
            { 0x0100u, 3u, "Next TBTT" },
            { 0x0200u, 4u, "Compressed SSID" },
            { 0x0400u, 1u, "ANO" },
        };
        int all_ok = 1, sa_ok = 1, off_ok = 1;

        for (unsigned mask = 0; mask < 8u; mask++)
        {
            uint8_t v[sizeof(S1G_BEACON) + 8];
            uint16_t fc = (uint16_t)(S1G_BEACON[0] | ((uint16_t)S1G_BEACON[1] << 8));
            uint32_t pad = 0;
            for (unsigned b = 0; b < 3u; b++)
            {
                if (mask & (1u << b)) { fc |= OPT[b].bit; pad += OPT[b].size; }
            }
            /* fixed header, then the announced optional octets, then the
             * element tail verbatim. */
            memcpy(v, S1G_BEACON, UMAC_MESH_S1G_BEACON_IE_OFFSET);
            v[0] = (uint8_t)(fc & 0xff);
            v[1] = (uint8_t)(fc >> 8);
            memset(v + UMAC_MESH_S1G_BEACON_IE_OFFSET, 0xcc, pad);
            memcpy(v + UMAC_MESH_S1G_BEACON_IE_OFFSET + pad,
                   S1G_BEACON + UMAC_MESH_S1G_BEACON_IE_OFFSET,
                   sizeof(S1G_BEACON) - UMAC_MESH_S1G_BEACON_IE_OFFSET);
            uint32_t vlen = (uint32_t)(sizeof(S1G_BEACON) + pad);

            if (umac_mesh_ies_s1g_beacon_ie_offset(fc) !=
                UMAC_MESH_S1G_BEACON_IE_OFFSET + pad) { off_ok = 0; }
            if (!umac_mesh_ies_s1g_beacon_has_mesh_id(v, vlen, MESH_ID, (uint8_t)sizeof(MESH_ID)))
            {
                all_ok = 0;
            }
            /* SA precedes the optional fields, so it must be unaffected. */
            uint8_t sa[6];
            if (!umac_mesh_ies_s1g_beacon_sa(v, vlen, sa) || memcmp(sa, EXPECT_SA, 6) != 0)
            {
                sa_ok = 0;
            }
            /* A frame that ends before the elements even begin must be
             * refused, so the bound is checked against the REAL offset rather
             * than the shortest-header constant. With the optional fields
             * present, a length that was valid for the short header is not. */
            if (umac_mesh_ies_s1g_beacon_has_mesh_id(v, UMAC_MESH_S1G_BEACON_IE_OFFSET + pad - 1u,
                                                     MESH_ID, (uint8_t)sizeof(MESH_ID)))
            {
                all_ok = 0;
            }
        }
        CHECK(off_ok, "IE offset follows the Next TBTT / Compressed SSID / ANO bits");
        CHECK(all_ok, "Mesh ID is found for all 8 optional-field combinations");
        CHECK(sa_ok, "source address is unaffected by the optional fields");
    }

    printf(failures ? "\nFAILED (%d)\n" : "\nALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
