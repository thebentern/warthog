/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * BATMAN_V wire codec: big-endian helpers, header layouts, the TVLV walker and the
 * TT CRC-32C variant, all checked against byte strings from the clean-room spec
 * (batman-spec/packets.md, tt.md, membership.md) and its captures README. Nothing
 * here is derived from batman-adv source; the vectors are frames seen on the wire.
 *
 * Every header offset in bat_codec.h is exercised by reading a captured frame with it
 * and comparing the field to the value the spec's annotation gives, so a wrong offset
 * fails here rather than as a silent interop problem.
 */
#include <stdio.h>
#include <string.h>

#include "bat.h"
#include "bat_codec.h"
#include "bat_crc32c.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static const uint8_t MAC_A[6] = { 0x02, 0x1a, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t MAC_B1[6] = { 0x02, 0x1b, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t MAC_B2[6] = { 0x02, 0x1b, 0x00, 0x00, 0x00, 0x02 };
static const uint8_t MAC_C[6] = { 0x02, 0x1c, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t MAC_D[6] = { 0x02, 0x1e, 0x00, 0x00, 0x00, 0x01 };

static void test_be(void)
{
    uint8_t b[4] = { 0 };
    bat_put16(b, 0x4305);
    CHECK(b[0] == 0x43 && b[1] == 0x05, "put16 writes big-endian");
    CHECK(bat_get16(b) == 0x4305, "get16 reads big-endian");
    bat_put32(b, 0x9c34abae);
    CHECK(b[0] == 0x9c && b[1] == 0x34 && b[2] == 0xab && b[3] == 0xae, "put32 writes big-endian");
    CHECK(bat_get32(b) == 0x9c34abaeu, "get32 reads big-endian");
    const uint8_t hi[4] = { 0xff, 0xff, 0xff, 0xff };
    CHECK(bat_get32(hi) == 0xFFFFFFFFu, "get32 of ff ff ff ff is 0xFFFFFFFF (no sign extension)");
    const uint8_t h16[2] = { 0x80, 0x01 };
    CHECK(bat_get16(h16) == 0x8001, "get16 of 80 01 is 0x8001");
}

/* packets §5.5 */
static const uint8_t ELP[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02, 0x1b, 0x00, 0x00, 0x00, 0x02, 0x43, 0x05,
    0x03, 0x0f, 0x02, 0x1b, 0x00, 0x00, 0x00, 0x01, 0x74, 0x51, 0xd7, 0x46,
    0x00, 0x00, 0x01, 0xf4, 0x00, 0x00, 0x00, 0x00,
};

/* packets §6.5: B's own OGM2 followed by A's OGM2 forwarded by B */
static const uint8_t OGM_AGG[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02, 0x1b, 0x00, 0x00, 0x00, 0x02, 0x43, 0x05,
    0x04, 0x0f, 0x32, 0x00, 0x5f, 0x06, 0x32, 0x44, 0x02, 0x1b, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x24, 0xff, 0xff, 0xff, 0xff,
    0x04, 0x01, 0x00, 0x14, 0x01, 0x02, 0x00, 0x02,
    0x0f, 0xd6, 0x30, 0xbf, 0x80, 0x00, 0x00, 0x00, 0x6e, 0x0a, 0x66, 0x60, 0x00, 0x00, 0x00, 0x00,
    0x06, 0x02, 0x00, 0x04, 0x38, 0x00, 0x00, 0x00,
    0x02, 0x01, 0x00, 0x00,
    0x04, 0x0f, 0x31, 0x00, 0x87, 0xff, 0x20, 0xa5, 0x02, 0x1a, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x44, 0x00, 0x01, 0x58, 0xab,
    0x04, 0x01, 0x00, 0x28, 0x01, 0x03, 0x00, 0x03,
    0x37, 0xc7, 0x5f, 0x13, 0x80, 0x00, 0x00, 0x00, 0x7e, 0xfb, 0x22, 0x34, 0x80, 0x01, 0x00, 0x00,
    0x2a, 0x59, 0xc2, 0x66, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x1d, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x06, 0x02, 0x00, 0x04, 0x27, 0x00, 0x00, 0x00,
    0x01, 0x01, 0x00, 0x08, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x14,
    0x02, 0x01, 0x00, 0x00,
};

/* packets §9.5 (client frame cut after the IPv4 version byte) */
static const uint8_t BCAST[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02, 0x1c, 0x00, 0x00, 0x00, 0x01, 0x43, 0x05,
    0x01, 0x0f, 0x31, 0x00, 0x00, 0x00, 0x00, 0x05, 0x02, 0x1c, 0x00, 0x00, 0x00, 0x01,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02, 0x1c, 0x00, 0x00, 0xba, 0x70, 0x08, 0x00, 0x45, 0x00,
};

/* packets §8.5 */
static const uint8_t UNICAST[] = {
    0x02, 0x1c, 0x00, 0x00, 0x00, 0x01, 0x02, 0x1b, 0x00, 0x00, 0x00, 0x02, 0x43, 0x05,
    0x40, 0x0f, 0x31, 0x02, 0x02, 0x1c, 0x00, 0x00, 0x00, 0x01,
    0x02, 0x1c, 0x00, 0x00, 0xba, 0x70, 0x02, 0x1d, 0x00, 0x00, 0x00, 0x01, 0x08, 0x00,
    0x45, 0x00, 0x00, 0x54,
};

/* packets §8.5, 4addr DHCP-to-gateway (batman bytes only) */
static const uint8_t UC4[] = {
    0x42, 0x0f, 0x32, 0x04, 0x02, 0x1a, 0x00, 0x00, 0x00, 0x01, 0x02, 0x1c, 0x00, 0x00, 0x00, 0x01,
    0x01, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02, 0x1c, 0x00, 0x00, 0xba, 0x70, 0x08, 0x00,
};

/* packets §7.2, fragment 1 (head) of a 1524-byte packet (batman bytes only) */
static const uint8_t FRAG1[] = {
    0x41, 0x0f, 0x32, 0x10, 0x02, 0x1a, 0x00, 0x00, 0x00, 0x01, 0x02, 0x1c, 0x00, 0x00, 0x00, 0x01,
    0xb7, 0x39, 0x05, 0xf4, 0x40, 0x0f, 0x32, 0x04, 0x02, 0x1a, 0x00, 0x00, 0x00, 0x01,
};

/* packets §13.2, TTL exceeded from B to C (batman bytes only) */
static const uint8_t ICMP_TTLX[] = {
    0x43, 0x0f, 0x32, 0x0b, 0x02, 0x1c, 0x00, 0x00, 0x00, 0x01, 0x02, 0x1b, 0x00, 0x00, 0x00, 0x01,
    0x28, 0x00, 0x00, 0x01,
};

/* packets §11.4, full-table TT request D -> A (batman bytes only) */
static const uint8_t UTVLV_REQ[] = {
    0x44, 0x0f, 0x32, 0x00, 0x02, 0x1a, 0x00, 0x00, 0x00, 0x01, 0x02, 0x1e, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x20, 0x00, 0x00,
    0x04, 0x01, 0x00, 0x1c, 0x12, 0x05, 0x00, 0x03,
    0x37, 0xc7, 0x5f, 0x13, 0x80, 0x00, 0x00, 0x00, 0x7e, 0xfb, 0x22, 0x34, 0x80, 0x01, 0x00, 0x00,
    0xcb, 0xc9, 0xa7, 0x5b, 0x00, 0x00, 0x00, 0x00,
};

static void test_layouts(void)
{
    const uint8_t *p = ELP + 14;
    CHECK(bat_get16(ELP + BAT_LINK_TYPE) == 0x4305 && memcmp(ELP + BAT_LINK_SRC, MAC_B2, 6) == 0,
          "link header: dst 0, src 6, ethertype 12");
    CHECK(p[BAT_OFF_TYPE] == BAT_PT_ELP && p[BAT_OFF_VERSION] == 15, "ELP: type 0x03, version 15");
    CHECK(memcmp(p + BAT_ELP_ORIG, MAC_B1, 6) == 0, "ELP: originator at 2 (primary, not the link source)");
    CHECK(bat_get32(p + BAT_ELP_SEQ) == 0x7451d746u, "ELP: seqno at 8");
    CHECK(bat_get32(p + BAT_ELP_INTERVAL) == 500, "ELP: interval at 12 (500 ms)");
    CHECK(BAT_ELP_HLEN == 16 && BAT_ELP_LEN == 20 && sizeof(ELP) - 14 == BAT_ELP_LEN,
          "ELP: 16-byte header, 20 bytes sent");

    p = OGM_AGG + 14;
    CHECK(p[BAT_OGM_TTL] == 50 && p[BAT_OGM_FLAGS] == 0, "OGM2: TTL at 2 (50), flags at 3");
    CHECK(bat_get32(p + BAT_OGM_SEQ) == 0x5f063244u, "OGM2: seqno at 4");
    CHECK(memcmp(p + BAT_OGM_ORIG, MAC_B1, 6) == 0, "OGM2: originator at 8");
    CHECK(bat_get16(p + BAT_OGM_TVLV_LEN) == 36, "OGM2: TVLV length at 14 (36)");
    CHECK(bat_get32(p + BAT_OGM_TPUT) == 0xFFFFFFFFu, "OGM2: throughput at 16 (own = max)");
    const uint8_t *q = p + BAT_OGM_HLEN + 36;
    CHECK(q - (OGM_AGG + 14) == 56, "OGM2: the second record starts at batman offset 56");
    CHECK(q[BAT_OGM_TTL] == 49 && bat_get32(q + BAT_OGM_TPUT) == 88235 &&
          memcmp(q + BAT_OGM_ORIG, MAC_A, 6) == 0 && bat_get16(q + BAT_OGM_TVLV_LEN) == 68,
          "OGM2: forwarded record TTL 49, throughput 88235, originator A, TVLV 68");
    CHECK(sizeof(OGM_AGG) == 158 && (q + BAT_OGM_HLEN + 68) == OGM_AGG + sizeof(OGM_AGG),
          "OGM2: aggregate of 20+36 and 20+68 fills the 158-byte frame exactly");

    p = BCAST + 14;
    CHECK(p[BAT_OFF_TYPE] == BAT_PT_BCAST && p[BAT_BC_TTL] == 49 && p[BAT_BC_RSVD] == 0,
          "BCAST: type 0x01, TTL at 2 (49 fresh), reserved at 3");
    CHECK(bat_get32(p + BAT_BC_SEQ) == 5 && memcmp(p + BAT_BC_ORIG, MAC_C, 6) == 0,
          "BCAST: seqno at 4, originator at 8");
    CHECK(BAT_BC_HLEN == 14 && p[BAT_BC_HLEN] == 0xff && p[BAT_BC_HLEN + 12] == 0x08,
          "BCAST: client frame at 14");

    p = UNICAST + 14;
    CHECK(p[BAT_OFF_TYPE] == BAT_PT_UNICAST && p[BAT_UC_TTL] == 49 && p[BAT_UC_TTVN] == 2,
          "UNICAST: TTL at 2 (49), TTVN at 3 (2)");
    CHECK(memcmp(p + BAT_UC_DEST, MAC_C, 6) == 0 && BAT_UC_DEST == BAT_OFF_UNI_DEST,
          "UNICAST: destination originator at 4");
    CHECK(BAT_UC_HLEN == 10 && p[BAT_UC_HLEN] == 0x02 && p[BAT_UC_HLEN + 5] == 0x70,
          "UNICAST: client frame at 10");

    CHECK(UC4[BAT_OFF_TYPE] == BAT_PT_4ADDR && UC4[BAT_UC_TTVN] == 4 &&
          memcmp(UC4 + BAT_UC_DEST, MAC_A, 6) == 0, "4ADDR: shares the UNICAST first 10 bytes");
    CHECK(memcmp(UC4 + BAT_4A_SRC, MAC_C, 6) == 0 && UC4[BAT_4A_SUBTYPE] == BAT_4A_ST_DATA &&
          UC4[BAT_4A_RSVD] == 0, "4ADDR: source at 10, subtype at 16 (1 = data), reserved at 17");
    CHECK(BAT_4A_HLEN == 18 && UC4[BAT_4A_HLEN] == 0xff, "4ADDR: client frame at 18");

    CHECK(FRAG1[BAT_OFF_TYPE] == BAT_PT_FRAG && FRAG1[BAT_FR_TTL] == 50 &&
          FRAG1[BAT_FR_NUMPRIO] >> 4 == 1 && ((FRAG1[BAT_FR_NUMPRIO] >> 1) & 7) == 0,
          "FRAG: TTL at 2, number in bits 7-4 of byte 3 (1), priority bits 3-1 (0)");
    CHECK(memcmp(FRAG1 + BAT_FR_DEST, MAC_A, 6) == 0 && memcmp(FRAG1 + BAT_FR_ORIG, MAC_C, 6) == 0,
          "FRAG: destination at 4, fragmenting originator at 10");
    CHECK(bat_get16(FRAG1 + BAT_FR_SEQ) == 0xb739 && bat_get16(FRAG1 + BAT_FR_TOTAL) == 1524,
          "FRAG: seqno at 16, total size at 18 (1524)");
    CHECK(BAT_FR_HLEN == 20 && FRAG1[BAT_FR_HLEN] == 0x40, "FRAG: data at 20 (head carries 40 0f ..)");

    CHECK(ICMP_TTLX[BAT_IC_MSGTYPE] == BAT_IC_TTL_EXC && ICMP_TTLX[BAT_IC_TTL] == 50,
          "ICMP: message type at 3 (11 = TTL exceeded), TTL 50");
    CHECK(memcmp(ICMP_TTLX + BAT_IC_DEST, MAC_C, 6) == 0 &&
          memcmp(ICMP_TTLX + BAT_IC_ORIG, MAC_B1, 6) == 0, "ICMP: destination at 4, originator at 10");
    CHECK(ICMP_TTLX[BAT_IC_UID] == 0x28 && ICMP_TTLX[BAT_IC_RRCOUNT] == 0 &&
          bat_get16(ICMP_TTLX + BAT_IC_SEQ) == 1, "ICMP: uid at 16, route-record count at 17, seqno at 18");
    CHECK(BAT_IC_HLEN == 20 && BAT_IC_RR_LEN == 20 + 6 * BAT_IC_RR_SLOTS && BAT_IC_TP_HLEN == 28,
          "ICMP: 20-byte base, 116-byte route record (16 slots), 28-byte TP header");

    CHECK(memcmp(UTVLV_REQ + BAT_UT_DEST, MAC_A, 6) == 0 &&
          memcmp(UTVLV_REQ + BAT_UT_SRC, MAC_D, 6) == 0, "UNICAST_TVLV: destination at 4, source at 10");
    CHECK(bat_get16(UTVLV_REQ + BAT_UT_TVLV_LEN) == sizeof(UTVLV_REQ) - BAT_UT_HLEN &&
          bat_get16(UTVLV_REQ + BAT_UT_ALIGN) == 0 && UTVLV_REQ[BAT_UT_RSVD] == 0,
          "UNICAST_TVLV: TVLV length at 16 covers the rest, alignment at 18 is 0");

    CHECK(bat_hdr_len(BAT_PT_ELP) == 16 && bat_hdr_len(BAT_PT_OGM2) == 20 &&
          bat_hdr_len(BAT_PT_BCAST) == 14 && bat_hdr_len(BAT_PT_UNICAST) == 10 &&
          bat_hdr_len(BAT_PT_FRAG) == 20 && bat_hdr_len(BAT_PT_4ADDR) == 18 &&
          bat_hdr_len(BAT_PT_ICMP) == 20 && bat_hdr_len(BAT_PT_UTVLV) == 20 &&
          bat_hdr_len(BAT_PT_MCAST) == 6, "fixed header lengths match packets §3.5");
    CHECK(bat_hdr_len(0x45) == 10 && bat_hdr_len(0x7F) == 10 && bat_hdr_len(0x00) == 0 &&
          bat_hdr_len(0x80) == 0 && bat_hdr_len(0x06) == 0,
          "unknown unicast-range types carry the 10-byte unicast prefix; others none");
    CHECK(BAT_HARD_MTU_DEFAULT == 1500 && BAT_MAX_LINK_FRAME - BAT_ETH_HLEN == 1586,
          "hard MTU 1500 default, 1586 largest batman packet accepted");
}

static void test_tvlv(void)
{
    struct bat_tvlv t;
    size_t off = 0;
    /* B's own OGM TVLV area from packets §6.5: TT (20), MCAST (4), DAT (0) */
    const uint8_t *area = OGM_AGG + 14 + BAT_OGM_HLEN;
    int r = bat_tvlv_next(area, 36, &off, &t);
    CHECK(r == BAT_TVLV_OK && t.type == BAT_TVLV_TT && t.version == 1 && t.len == 20 &&
          t.val == area + 4 && off == 24, "TVLV walk: TT v1 length 20, value right after the header");
    r = bat_tvlv_next(area, 36, &off, &t);
    CHECK(r == BAT_TVLV_OK && t.type == BAT_TVLV_MCAST && t.version == 2 && t.len == 4 &&
          t.val[0] == 0x38 && off == 32, "TVLV walk: MCAST v2 flags 0x38");
    r = bat_tvlv_next(area, 36, &off, &t);
    CHECK(r == BAT_TVLV_OK && t.type == BAT_TVLV_DAT && t.len == 0 && off == 36,
          "TVLV walk: zero-length DAT container is a container");
    r = bat_tvlv_next(area, 36, &off, &t);
    CHECK(r == BAT_TVLV_END && off == 36, "TVLV walk: ends exactly at the area end");

    /* A's forwarded TVLV area carries a GW container */
    area = OGM_AGG + 14 + 56 + BAT_OGM_HLEN;
    off = 0;
    int seen_gw = 0, n = 0;
    while (bat_tvlv_next(area, 68, &off, &t) == BAT_TVLV_OK) {
        n++;
        if (t.type == BAT_TVLV_GW) {
            seen_gw = t.version == 1 && t.len == 8 && bat_get32(t.val) == 100 &&
                      bat_get32(t.val + 4) == 20;
        }
    }
    CHECK(n == 4 && seen_gw && off == 68, "TVLV walk: TT, MCAST, GW (100/20), DAT in the forwarded record");

    const uint8_t over[] = { 0x04, 0x01, 0x00, 0x10, 0x01, 0x02, 0x00, 0x00 };
    off = 0;
    r = bat_tvlv_next(over, sizeof(over), &off, &t);
    CHECK(r == BAT_TVLV_OVERRUN && off == 0, "TVLV walk: a length past the area stops the walk");
    const uint8_t two_then_over[] = { 0x02, 0x01, 0x00, 0x00, 0x04, 0x01, 0x00, 0x05, 0xaa };
    off = 0;
    r = bat_tvlv_next(two_then_over, sizeof(two_then_over), &off, &t);
    int r2 = bat_tvlv_next(two_then_over, sizeof(two_then_over), &off, &t);
    CHECK(r == BAT_TVLV_OK && r2 == BAT_TVLV_OVERRUN && off == 4,
          "TVLV walk: containers before an overrun stay parsed");
    const uint8_t trailing[] = { 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
    off = 0;
    r = bat_tvlv_next(trailing, sizeof(trailing), &off, &t);
    r2 = bat_tvlv_next(trailing, sizeof(trailing), &off, &t);
    CHECK(r == BAT_TVLV_OK && r2 == BAT_TVLV_END && off == 4,
          "TVLV walk: 3 trailing bytes (less than a header) end the walk");
    off = 0;
    CHECK(bat_tvlv_next(trailing, 0, &off, &t) == BAT_TVLV_END, "TVLV walk: an empty area is END");
    off = 9;
    CHECK(bat_tvlv_next(trailing, sizeof(trailing), &off, &t) == BAT_TVLV_END,
          "TVLV walk: an offset past the area is END, not a read past it");
    const uint8_t maxlen[] = { 0x04, 0x01, 0xff, 0xff };
    off = 0;
    CHECK(bat_tvlv_next(maxlen, sizeof(maxlen), &off, &t) == BAT_TVLV_OVERRUN,
          "TVLV walk: length 0xFFFF is an overrun");

    /* The TT request of packets §11.4: TVLV area = everything after the 20-byte header */
    off = 0;
    r = bat_tvlv_next(UTVLV_REQ + 20, sizeof(UTVLV_REQ) - 20, &off, &t);
    CHECK(r == BAT_TVLV_OK && t.type == BAT_TVLV_TT && t.len == 28 && t.val[0] == 0x12 &&
          t.val[1] == 5 && bat_get16(t.val + 2) == 3, "TVLV walk: TT request flags 0x12, TTVN 5, 3 VLANs");
}

struct crc_row { uint8_t mac[6]; uint16_t vid; uint8_t flags; uint32_t crc; };

static void test_crc(void)
{
    const uint8_t chk[] = "123456789";
    CHECK(bat_crc32c(0, chk, 9) == 0x58E3FA20u, "CRC-32C init 0, no xorout: \"123456789\" -> 0x58E3FA20");
    CHECK(bat_crc32c(0xFFFFFFFFu, chk, 9) == (0xE3069283u ^ 0xFFFFFFFFu),
          "same register with init 0xFFFFFFFF gives the textbook value before its xorout");
    CHECK(bat_crc32c(0, chk, 0) == 0, "CRC over nothing is the start register");

    /* tt §3.3 per-entry vectors */
    static const struct crc_row rows[] = {
        { { 0x02, 0x0a, 0x00, 0x00, 0xba, 0x70 }, 0x0000, 0x00, 0x1e8d3b2b },
        { { 0x02, 0x0e, 0x00, 0x00, 0x00, 0x10 }, 0x0000, 0x00, 0x9ed20dd5 },
        { { 0x02, 0x0a, 0x00, 0x00, 0xba, 0x70 }, 0x8000, 0x00, 0xb8e53ec0 },
        { { 0x02, 0x0a, 0x00, 0x00, 0xba, 0x70 }, 0x8001, 0x00, 0xf1d943e7 },
        { { 0x02, 0x0e, 0x00, 0x00, 0x00, 0x20 }, 0x0000, 0x20, 0x936ac7d9 },
        { { 0x02, 0x0b, 0x00, 0x00, 0xba, 0x70 }, 0x0000, 0x00, 0x269c5487 },
        { { 0x33, 0x33, 0x00, 0x00, 0x00, 0x01 }, 0x0000, 0x00, 0x5f960a1a },
        { { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 }, 0x0000, 0x00, 0x6d987754 },
        { { 0x02, 0x00, 0x5e, 0x10, 0x00, 0x01 }, 0x0000, 0x00, 0x2986c104 },
        { { 0x02, 0x00, 0x5e, 0x10, 0x00, 0x99 }, 0x0000, 0x00, 0x31f765dc },
    };
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        const struct crc_row *e = &rows[i];
        uint32_t c = bat_crc32c_tt(e->vid, e->flags, e->mac);
        CHECK(c == e->crc, "tt §3.3 entry %02x:..:%02x:%02x vid 0x%04x flags 0x%02x -> 0x%08x (got 0x%08x)",
              e->mac[0], e->mac[4], e->mac[5], e->vid, e->flags, e->crc, c);
    }
    uint8_t s9[9] = { 0x00, 0x00, 0x20, 0x02, 0x0e, 0x00, 0x00, 0x00, 0x20 };
    CHECK(bat_crc32c(0, s9, 9) == 0x936ac7d9u, "TT entry helper feeds VID BE || flags & 0xF0 || MAC");
    const uint8_t m20[6] = { 0x02, 0x0e, 0x00, 0x00, 0x00, 0x20 };
    CHECK(bat_crc32c_tt(0x0000, 0x2F, m20) == 0x936ac7d9u,
          "only the high nibble of the client flags enters the CRC");

    /* tt §3.3 set vectors */
    const uint8_t a70[6] = { 0x02, 0x0a, 0x00, 0x00, 0xba, 0x70 };
    const uint8_t e10[6] = { 0x02, 0x0e, 0x00, 0x00, 0x00, 0x10 };
    const uint8_t b70[6] = { 0x02, 0x0b, 0x00, 0x00, 0xba, 0x70 };
    const uint8_t m1[6] = { 0x33, 0x33, 0x00, 0x00, 0x00, 0x01 };
    const uint8_t v4[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 };
    const uint8_t w1[6] = { 0x02, 0x00, 0x5e, 0x10, 0x00, 0x01 };
    const uint8_t w99[6] = { 0x02, 0x00, 0x5e, 0x10, 0x00, 0x99 };
    CHECK(bat_crc32c_tt(0, 0, a70) == 0x1e8d3b2bu, "set {02:0a:00:00:ba:70} -> 0x1e8d3b2b");
    CHECK((bat_crc32c_tt(0, 0, a70) ^ bat_crc32c_tt(0, 0, e10)) == 0x805f36feu,
          "set {..ba:70, 02:0e:00:00:00:10} -> 0x805f36fe");
    CHECK((bat_crc32c_tt(0, 0, b70) ^ bat_crc32c_tt(0, 0, m1) ^ bat_crc32c_tt(0, 0, v4)) == 0x149229c9u,
          "set {02:0b:00:00:ba:70, 33:33:00:00:00:01, 01:00:5e:00:00:01} -> 0x149229c9");
    CHECK((bat_crc32c_tt(0, 0, a70) ^ bat_crc32c_tt(0, 0x20, m20)) == 0x8de7fcf2u,
          "set {..ba:70, 02:0e:00:00:00:20 ISOLA} -> 0x8de7fcf2");
    CHECK((bat_crc32c_tt(0, 0, a70) ^ bat_crc32c_tt(0, 0, m20)) == 0xb0bc7f4fu,
          "the same set without the flag -> 0xb0bc7f4f");
    CHECK(bat_crc32c_tt(0, 0, w1) == 0x2986c104u, "set {02:00:5e:10:00:01} -> 0x2986c104");
    CHECK((bat_crc32c_tt(0, 0, w1) ^ bat_crc32c_tt(0, 0, w99)) == 0x1871a4d8u,
          "set {..00:01, ..00:99} -> 0x1871a4d8");

    /* packets §11.4 worked example */
    const uint8_t ab70[6] = { 0x02, 0x1a, 0x00, 0x00, 0xba, 0x70 };
    const uint8_t ab00[6] = { 0x02, 0x1a, 0x00, 0x00, 0xb0, 0x00 };
    CHECK(bat_crc32c_tt(0, 0, ab70) == 0x91af5af8u, "packets §11.4: 02:1a:00:00:ba:70 -> 0x91af5af8");
    CHECK(bat_crc32c_tt(0, 0, ab00) == 0x5a66fda3u, "packets §11.4: 02:1a:00:00:b0:00 -> 0x5a66fda3");
    CHECK((bat_crc32c_tt(0, 0, ab70) ^ bat_crc32c_tt(0, 0, ab00)) == 0xcbc9a75bu,
          "packets §11.4: XOR 0xcbc9a75b, as announced (and in the §11.4 request)");
    CHECK(bat_crc32c_tt(0x8000, 0, ab70) == 0x37c75f13u, "packets §11.4: VLAN 0x8000 record 0x37c75f13");
    CHECK(bat_get32(OGM_AGG + 14 + 56 + 28) == bat_crc32c_tt(0x8000, 0, ab70),
          "the forwarded OGM of §6.5 announces that same VLAN-0 CRC");

    /* membership §3.3 */
    const uint8_t mb[6] = { 0x02, 0xbb, 0xba, 0x70, 0x00, 0x02 };
    const uint8_t m6[6] = { 0x33, 0x33, 0xff, 0x70, 0x00, 0x02 };
    CHECK(bat_crc32c_tt(0x0000, 0, mb) == 0x3e8a4020u, "membership §3.3: 02:bb:ba:70:00:02 untagged -> 0x3e8a4020");
    CHECK(bat_crc32c_tt(0x8000, 0, mb) == 0x98e245cbu, "membership §3.3: same on VID 0x8000 -> 0x98e245cb");
    CHECK((bat_crc32c_tt(0, 0, mb) ^ bat_crc32c_tt(0, 0, m1) ^ bat_crc32c_tt(0, 0, m6) ^
           bat_crc32c_tt(0, 0, v4)) == 0x3ec22a85u, "membership §3.3: four-entry set -> 0x3ec22a85");

    /* captures README */
    const uint8_t bff[6] = { 0x02, 0x00, 0x00, 0x00, 0x0b, 0xff };
    CHECK(bat_crc32c_tt(0x8000, 0, bff) == 0x796a9d58u, "captures README: VLAN 0x8000 {02:00:00:00:0b:ff} -> 0x796a9d58");

    /* linearity (tt §3.1): XOR of entry CRCs == CRC of the XOR of the 9-byte strings */
    const uint8_t *macs[4] = { a70, e10, b70, m20 };
    const uint16_t vids[4] = { 0x0000, 0x8000, 0x8001, 0x0000 };
    const uint8_t flags[4] = { 0x00, 0x10, 0x20, 0x30 };
    uint8_t acc[9] = { 0 };
    uint32_t x = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t s[9] = { (uint8_t)(vids[i] >> 8), (uint8_t)vids[i], flags[i] };
        memcpy(s + 3, macs[i], 6);
        for (int k = 0; k < 9; k++) {
            acc[k] ^= s[k];
        }
        x ^= bat_crc32c_tt(vids[i], flags[i], macs[i]);
    }
    CHECK(x == bat_crc32c(0, acc, 9), "linearity: XOR of four entry CRCs = CRC of the XORed strings");
    const uint8_t z78[6] = { 0x02, 0x0e, 0x00, 0x00, 0x00, 0x78 }, z79[6] = { 0x02, 0x0e, 0x00, 0x00, 0x00, 0x79 };
    const uint8_t z7a[6] = { 0x02, 0x0e, 0x00, 0x00, 0x00, 0x7a }, z7b[6] = { 0x02, 0x0e, 0x00, 0x00, 0x00, 0x7b };
    CHECK((bat_crc32c_tt(0, 0, z78) ^ bat_crc32c_tt(0, 0, z79) ^ bat_crc32c_tt(0, 0x20, z7a) ^
           bat_crc32c_tt(0, 0x20, z7b)) == 0, "tt §3.1 zero-sum set is invisible to the CRC");
}

int main(void)
{
    test_be();
    test_layouts();
    test_tvlv();
    test_crc();
    if (failures) {
        printf("test_bat_codec: %d FAILED\n", failures);
        return 1;
    }
    printf("test_bat_codec: all passed\n");
    return 0;
}
