/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * CCMP framing: header, AAD and nonce
 * (morselib src/umac/mesh/umac_mesh_ccmp_hdr.c).
 *
 * The receiver computes all of this independently and compares it by way of a
 * MIC, so a single wrong masked bit does not corrupt one field -- it makes
 * every frame fail authentication, at both ends, with nothing to see but a
 * counter. There is no partial failure to debug from, which is exactly why
 * these bytes belong in golden tests rather than in a hardware bisect.
 *
 * The frames below are the shape warthog actually sends: 4-address AND QoS,
 * so both optional parts of the AAD are covered. The masking rules are the
 * interesting part -- fields that legitimately change in flight (Retry,
 * PwrMgt, MoreData, sequence number, most of the QoS control) must NOT affect
 * the AAD, or a retransmission stops verifying.
 */
#include "umac_mesh_ccmp_hdr.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* A real mesh frame, captured from the bench: 4-address protected QoS Data,
 * ToDS|FromDS, Mesh Control present. */
static uint8_t MESH_HDR[32] = {
    0x88, 0x43,                                     /* FC: QoS Data, ToDS|FromDS, Protected */
    0xd0, 0x02,                                     /* duration */
    0xa8, 0xdd, 0x9f, 0x4d, 0xc7, 0xf8,             /* A1 = RA */
    0x3c, 0x1a, 0xcc, 0x4c, 0x83, 0xa5,             /* A2 = TA */
    0xa8, 0xdd, 0x9f, 0x4d, 0xc7, 0xf8,             /* A3 = DA */
    0x30, 0x00,                                     /* seq ctrl: seq 3, frag 0 */
    0x3c, 0x1a, 0xcc, 0x4c, 0x83, 0xa5,             /* A4 = SA */
    0x01, 0x00,                                     /* QoS: TID 1, Mesh Control present */
};

int main(void)
{
    uint8_t aad[UMAC_CCMP_AAD_MAXLEN], aad2[UMAC_CCMP_AAD_MAXLEN];
    uint8_t nonce[13], nonce2[13];
    uint8_t hdr[UMAC_CCMP_HDR_LEN], pn[6], key_id;

    /* ---- shape ------------------------------------------------------- */
    CHECK(umac_ccmp_is_4addr(MESH_HDR), "4-address frame detected");
    CHECK(umac_ccmp_is_qos(MESH_HDR), "QoS data frame detected");
    CHECK(umac_ccmp_hdr_len(MESH_HDR) == 32, "4-addr QoS header is 32 bytes");
    {
        uint8_t plain[24] = { 0x08, 0x00 }; /* 3-address, non-QoS data */
        CHECK(!umac_ccmp_is_4addr(plain) && !umac_ccmp_is_qos(plain),
              "plain 3-address data is neither 4-addr nor QoS");
        CHECK(umac_ccmp_hdr_len(plain) == 24, "3-addr non-QoS header is 24 bytes");
    }

    /* ---- CCMP header round trip --------------------------------------- */
    static const uint8_t PN[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    umac_ccmp_write_header(hdr, PN, 2);
    /* The standard's split layout: PN0 PN1 rsvd KeyID|ExtIV PN2..PN5. */
    CHECK(hdr[0] == 0x06 && hdr[1] == 0x05, "header carries PN5,PN4 first");
    CHECK(hdr[2] == 0x00, "reserved octet is zero");
    CHECK((hdr[3] & 0x20) != 0, "Ext IV bit is set");
    CHECK(((hdr[3] >> 6) & 3) == 2, "key id is in the top two bits");
    CHECK(hdr[4] == 0x04 && hdr[5] == 0x03 && hdr[6] == 0x02 && hdr[7] == 0x01,
          "header carries PN3..PN0 last");
    memset(pn, 0, sizeof(pn));
    key_id = 0xff;
    CHECK(umac_ccmp_parse_header(hdr, pn, &key_id), "header parses");
    CHECK(memcmp(pn, PN, sizeof(PN)) == 0 && key_id == 2, "PN and key id round-trip");

    /* A frame without Ext IV is not CCMP and must be refused, not misread. */
    hdr[3] &= (uint8_t)~0x20;
    CHECK(!umac_ccmp_parse_header(hdr, pn, &key_id), "Ext IV clear is rejected");

    /* ---- AAD ---------------------------------------------------------- */
    uint32_t n = umac_ccmp_build_aad(MESH_HDR, aad);
    CHECK(n == 30, "4-addr QoS AAD is 30 bytes");
    CHECK(aad[0] == 0x88 && aad[1] == 0x43, "AAD frame control keeps type/subtype and Protected");
    CHECK(memcmp(&aad[2], &MESH_HDR[4], 18) == 0, "AAD carries A1, A2, A3");
    CHECK(aad[20] == 0x00 && aad[21] == 0x00, "sequence number is masked, fragment kept");
    CHECK(memcmp(&aad[22], &MESH_HDR[24], 6) == 0, "AAD carries A4");
    CHECK(aad[28] == 0x01 && aad[29] == 0x00, "AAD keeps only the TID from QoS control");

    /* Fields that change in flight must not change the AAD -- otherwise a
     * retransmission fails to authenticate and the link looks lossy. */
    {
        uint8_t v[32];
        memcpy(v, MESH_HDR, sizeof(v));
        v[1] |= 0x08;               /* Retry */
        v[1] |= 0x10;               /* PwrMgt */
        v[1] |= 0x20;               /* MoreData */
        /* Sequence number only: it lives in the top 12 bits of the seq-ctrl
         * field, so the low nibble (the fragment number) must stay 0 -- that
         * part IS authenticated, and clobbering it here would be testing the
         * wrong thing. */
        v[22] = 0xf0; v[23] = 0xff;
        v[31] |= 0x60;              /* other QoS control bits */
        CHECK(umac_ccmp_build_aad(v, aad2) == n && memcmp(aad, aad2, n) == 0,
              "Retry/PwrMgt/MoreData, seq and QoS policy bits do not change the AAD");
    }
    /* The fragment number, by contrast, IS authenticated. */
    {
        uint8_t v[32];
        memcpy(v, MESH_HDR, sizeof(v));
        v[22] = 0x01; /* frag 1 */
        CHECK(umac_ccmp_build_aad(v, aad2) == n && memcmp(aad, aad2, n) != 0,
              "the fragment number IS authenticated");
    }
    /* An address change must change the AAD, or frames could be redirected. */
    {
        uint8_t v[32];
        memcpy(v, MESH_HDR, sizeof(v));
        v[10] ^= 0x01; /* A2 */
        CHECK(memcmp(aad, aad2, n) != 0 || umac_ccmp_build_aad(v, aad2) == n,
              "AAD builds for a modified address");
        umac_ccmp_build_aad(v, aad2);
        CHECK(memcmp(aad, aad2, n) != 0, "changing A2 changes the AAD");
    }
    /* The receiver clears Protected before rebuilding; both must agree. */
    {
        uint8_t v[32];
        memcpy(v, MESH_HDR, sizeof(v));
        v[1] &= (uint8_t)~0x40; /* Protected clear, as on the RX side */
        CHECK(umac_ccmp_build_aad(v, aad2) == n && memcmp(aad, aad2, n) == 0,
              "AAD is identical whether or not Protected was set on input");
    }
    /* A 3-address non-QoS frame gives the short AAD. */
    {
        uint8_t v[24];
        memset(v, 0, sizeof(v));
        v[0] = 0x08; /* data, 3-address, no QoS */
        CHECK(umac_ccmp_build_aad(v, aad2) == 22, "3-addr non-QoS AAD is 22 bytes");
    }

    /* ---- nonce -------------------------------------------------------- */
    umac_ccmp_build_nonce(MESH_HDR, PN, nonce);
    CHECK(nonce[0] == 0x01, "nonce priority octet is the TID");
    CHECK(memcmp(&nonce[1], &MESH_HDR[10], 6) == 0, "nonce carries A2, the transmitter");
    CHECK(nonce[7] == 0x01 && nonce[12] == 0x06, "nonce carries the PN big-endian");
    /* Two frames from one sender must never share a nonce; the PN is what
     * guarantees that, so it must actually reach the nonce. */
    {
        static const uint8_t PN2[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x07 };
        umac_ccmp_build_nonce(MESH_HDR, PN2, nonce2);
        CHECK(memcmp(nonce, nonce2, sizeof(nonce)) != 0, "a different PN gives a different nonce");
    }
    /* A management frame is tagged, so a data and a management frame with the
     * same PN still differ. */
    {
        uint8_t v[24];
        memset(v, 0, sizeof(v));
        v[0] = 0x00; /* management */
        umac_ccmp_build_nonce(v, PN, nonce2);
        CHECK((nonce2[0] & 0x10) != 0, "management frames set the nonce management bit");
    }

    /* ---- the other two header shapes --------------------------------
     *
     * umac_mesh_ccmp_hdr.h promises AAD lengths of 22, 24, 28 or 30. Only 30
     * (4-addr QoS) and 22 (3-addr non-QoS) were exercised, which left the
     * 3-address QoS shape untested -- and that is the GROUP header this
     * firmware actually puts on air: a mesh broadcast is ToDS=0/FromDS=1 with
     * a QoS control, exactly what an OpenMANET peer's ARP arrives as. It also
     * has the QoS control at a different offset (24, not 30), which both the
     * AAD's TID octet and the nonce's priority octet depend on. Reading that
     * from the wrong place does not fail loudly; it makes every group frame
     * fail authentication with nothing but a counter to show for it. */
    {
        /* 3-address QoS data, FromDS -- a mesh group frame. */
        uint8_t g[26] = {
            0x88, 0x02,                         /* QoS Data, FromDS, 3-address */
            0x00, 0x00,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* A1 = broadcast */
            0xe4, 0x5f, 0x01, 0x28, 0xbf, 0x74, /* A2 = TA */
            0xe4, 0x5f, 0x01, 0x28, 0xbf, 0x74, /* A3 */
            0x20, 0x3b,                         /* seq ctrl */
            0x06, 0x01,                         /* QoS control at 24: TID 6 */
        };
        CHECK(!umac_ccmp_is_4addr(g), "a mesh group frame is 3-address");
        CHECK(umac_ccmp_is_qos(g), "...and still QoS");
        CHECK(umac_ccmp_hdr_len(g) == 26, "3-addr QoS header is 26 bytes");

        uint8_t a3[UMAC_CCMP_AAD_MAXLEN];
        CHECK(umac_ccmp_build_aad(g, a3) == 24, "3-addr QoS AAD is 24 bytes");
        CHECK(a3[22] == 0x06 && a3[23] == 0x00,
              "the TID comes from the QoS control at offset 24, not 30");
        CHECK(memcmp(&a3[2], &g[4], 18) == 0, "AAD carries A1, A2, A3");
        CHECK(a3[20] == 0x00 && a3[21] == 0x00, "sequence number masked, fragment kept");

        uint8_t nz[13];
        static const uint8_t PN3[6] = { 1, 2, 3, 4, 5, 6 };
        umac_ccmp_build_nonce(g, PN3, nz);
        CHECK(nz[0] == 0x06, "nonce priority is the TID, read from the 3-address offset");
        CHECK(memcmp(&nz[1], &g[10], 6) == 0, "nonce carries A2 on a 3-address frame");

        /* 4-address non-QoS: the remaining promised length. */
        uint8_t q[30];
        memset(q, 0, sizeof(q));
        q[0] = 0x08; q[1] = 0x03; /* Data, ToDS|FromDS, no QoS */
        CHECK(umac_ccmp_is_4addr(q) && !umac_ccmp_is_qos(q), "4-address non-QoS is recognised");
        CHECK(umac_ccmp_hdr_len(q) == 30, "4-addr non-QoS header is 30 bytes");
        CHECK(umac_ccmp_build_aad(q, a3) == 28, "4-addr non-QoS AAD is 28 bytes");
    }

    printf(failures ? "\nFAILED (%d)\n" : "\nALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
