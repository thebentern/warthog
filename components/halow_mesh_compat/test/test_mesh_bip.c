/*
 * BIP-CMAC-128 for group-addressed mesh action frames (umac_mesh_bip.c).
 *
 * An MFP peer drops a broadcast PREQ or PERR whose MMIE does not verify, and it
 * says nothing: the only symptom is a mesh that never finds a path. So the
 * shipping code is pinned three ways, none of which trusts the others:
 *  - AES-CMAC against the four RFC 4493 vectors;
 *  - AES-CMAC against hostap's own omac1_aes_128 over a spread of lengths;
 *  - the whole MMIE against hostap wlantest's bip_protect() construction
 *    (FC with Retry/PwrMgt/MoreData masked, A1..A3, body, MIC field zeroed),
 *    rebuilt here on omac1_aes_128, on the IEEE 802.11 M.9.1 frame and a PREQ.
 * Then the verifier: every field the MIC covers must break it, the fields the
 * standard masks must not, and a body that does not end in an MMIE is not one.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "umac_mesh_bip.h"

int omac1_aes_128(const uint8_t *key, const uint8_t *data, size_t data_len, uint8_t *mac);

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static int hex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (s[0] && s[1] && n < cap)
    {
        unsigned v;
        if (s[0] == ' ') { s++; continue; }
        if (sscanf(s, "%2x", &v) != 1) { return -1; }
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return (int)n;
}

static const uint8_t RFC_KEY[16] = { 0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                                     0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c };

static void t_rfc4493(void)
{
    printf("--- AES-CMAC: RFC 4493 section 4 ---\n");
    static const char *msg =
        "6bc1bee22e409f96e93d7e117393172a" "ae2d8a571e03ac9c9eb76fac45af8e51"
        "30c81c46a35ce411e5fbc1191a0a52ef" "f69f2445df4f9b17ad2b417be66c3710";
    static const struct { size_t len; const char *mac; } v[] = {
        { 0,  "bb1d6929e95937287fa37d129b756746" },
        { 16, "070a16b46b4d4144f79bdd9dd04a287c" },
        { 40, "dfa66747de9ae63030ca32611497c827" },
        { 64, "51f0bebf7e3b9d92fc49741779363cfe" },
    };
    uint8_t m[64];
    (void)hex(msg, m, sizeof(m));
    for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); i++)
    {
        uint8_t want[16], got[16] = { 0 };
        (void)hex(v[i].mac, want, sizeof(want));
        int rc = umac_mesh_aes_cmac128(RFC_KEY, m, v[i].len, got);
        CHECK(rc == 0 && memcmp(got, want, 16) == 0, "example %u, %zu-octet message", i + 1,
              v[i].len);
    }
}

static void t_vs_hostap(void)
{
    printf("--- AES-CMAC: against hostap omac1_aes_128, lengths 0..96 ---\n");
    uint8_t key[16], msg[96];
    for (unsigned i = 0; i < 16; i++) { key[i] = (uint8_t)(0x31u * i + 7u); }
    for (unsigned i = 0; i < sizeof(msg); i++) { msg[i] = (uint8_t)(i * 13u + 5u); }
    unsigned bad = 0;
    for (size_t n = 0; n <= sizeof(msg); n++)
    {
        uint8_t a[16], b[16] = { 0 };
        omac1_aes_128(key, msg, n, a);
        if (umac_mesh_aes_cmac128(key, msg, n, b) != 0 || memcmp(a, b, 16) != 0) { bad++; }
    }
    CHECK(bad == 0, "all 97 lengths agree, block-aligned and not (%u differ)", bad);
}

/* hostap wlantest bip_protect(), on omac1_aes_128: the reference construction. */
static size_t ref_protect(const uint8_t key[16], const uint8_t *frame, size_t len, uint64_t ipn,
                          uint16_t kid, uint8_t *out)
{
    memcpy(out, frame, len);
    uint8_t *p = out + len;
    *p++ = 76; *p++ = 16;
    *p++ = (uint8_t)kid; *p++ = (uint8_t)(kid >> 8);
    for (int i = 0; i < 6; i++) { *p++ = (uint8_t)(ipn >> (8 * i)); }
    memset(p, 0, 8);
    size_t plen = len + 18;
    uint8_t *buf = malloc(plen + 20 - 24);
    uint16_t fc = (uint16_t)(frame[0] | (frame[1] << 8));
    fc &= (uint16_t)~(0x0800u | 0x1000u | 0x2000u);
    buf[0] = (uint8_t)fc; buf[1] = (uint8_t)(fc >> 8);
    memcpy(buf + 2, frame + 4, 18);
    memcpy(buf + 20, out + 24, plen - 24);
    uint8_t mic[16];
    omac1_aes_128(key, buf, plen + 20 - 24, mic);
    free(buf);
    memcpy(p, mic, 8);
    return plen;
}

static const uint8_t M91_IGTK[16] = { 0x4e, 0xa9, 0x54, 0x3e, 0x09, 0xcf, 0x2b, 0x1e,
                                      0xca, 0x66, 0xff, 0xc5, 0x8b, 0xde, 0xcb, 0xcf };

static size_t ours(const uint8_t key[16], const uint8_t *frame, size_t len, uint64_t ipn,
                   uint16_t kid, uint8_t *out, size_t cap)
{
    memcpy(out, frame, len);
    size_t b = umac_mesh_bip_protect(key, kid, ipn, out, out + 24, len - 24, cap - 24);
    return b ? b + 24 : 0;
}

static void t_mmie_matches_reference(void)
{
    printf("--- MMIE: byte-identical to wlantest bip_protect ---\n");
    /* IEEE 802.11-2012 M.9.1: broadcast Deauthentication, IPN 4, key id 4. */
    static const uint8_t deauth[] = { 0xc0, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff,
                                      0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00,
                                      0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x02, 0x00 };
    uint8_t a[128], b[128];
    size_t na = ref_protect(M91_IGTK, deauth, sizeof(deauth), 4, 4, a);
    size_t nb = ours(M91_IGTK, deauth, sizeof(deauth), 4, 4, b, sizeof(b));
    CHECK(na == nb && memcmp(a, b, na) == 0, "the M.9.1 broadcast Deauthentication frame");
    CHECK(nb == sizeof(deauth) + 18u && b[26] == 76 && b[27] == 16 && b[28] == 4 && b[29] == 0 &&
              b[30] == 4 && b[31] == 0 && b[35] == 0,
          "MMIE last: 76, 16, key id 4 LE, IPN 4 LE (%zu octets)", nb);

    /* A PREQ as warthog broadcasts it, with Retry/PwrMgt/MoreData set in the header. */
    uint8_t preq[24 + 40];
    memset(preq, 0, sizeof(preq));
    preq[0] = 0xd0; preq[1] = 0x08 | 0x10 | 0x20;
    memset(preq + 4, 0xff, 6);
    for (int i = 0; i < 6; i++) { preq[10 + i] = preq[16 + i] = (uint8_t)(0x02 + i); }
    preq[22] = 0x50; preq[23] = 0x01;
    preq[24] = 13; preq[25] = 1; preq[26] = 130; preq[27] = 37;
    for (unsigned i = 28; i < sizeof(preq); i++) { preq[i] = (uint8_t)(i * 3u); }
    const uint64_t ipn = 0x0000a1b2c3d4e5ull;
    na = ref_protect(M91_IGTK, preq, sizeof(preq), ipn, 5, a);
    nb = ours(M91_IGTK, preq, sizeof(preq), ipn, 5, b, sizeof(b));
    CHECK(na == nb && memcmp(a, b, na) == 0, "a Mesh Action PREQ, 48-bit IPN, key id 5");

    CHECK(umac_mesh_bip_protect(M91_IGTK, 4, 1, preq, b + 24, 40, 40 + 17) == 0,
          "an MMIE that would not fit is refused");
}

static void t_verify(void)
{
    printf("--- verify: what the MIC covers, and what it deliberately does not ---\n");
    uint8_t f[24 + 30 + 18];
    memset(f, 0, sizeof(f));
    f[0] = 0xd0;
    memset(f + 4, 0xff, 6);
    for (int i = 0; i < 6; i++) { f[10 + i] = f[16 + i] = (uint8_t)(0x10 + i); }
    f[24] = 13; f[25] = 1;
    for (unsigned i = 26; i < 54; i++) { f[i] = (uint8_t)i; }
    size_t n = ours(M91_IGTK, f, 54, 77, 4, f, sizeof(f));
    const size_t bl = n - 24;
    CHECK(n == sizeof(f) && umac_mesh_bip_verify(M91_IGTK, f, f + 24, bl), "our own frame verifies");

    uint16_t kid = 0; uint64_t ipn = 0;
    CHECK(umac_mesh_bip_parse(f + 24, bl, &kid, &ipn) && kid == 4 && ipn == 77,
          "parse gives back key id %u and IPN %llu", kid, (unsigned long long)ipn);

    /* Each refusal below is paired with the untouched frame verifying, so a verifier
     * that refuses everything fails them too. */
    const bool good = umac_mesh_bip_verify(M91_IGTK, f, f + 24, bl);
    uint8_t k2[16];
    memcpy(k2, M91_IGTK, 16);
    k2[15] ^= 1;
    CHECK(good && !umac_mesh_bip_verify(k2, f, f + 24, bl), "another key does not verify");

    static const struct { unsigned off; uint8_t bit; const char *what; } cov[] = {
        { 0, 0x10, "subtype" }, { 1, 0x40, "the Protected bit" }, { 4, 0x01, "A1" },
        { 10, 0x80, "A2 (the transmitter)" }, { 16, 0x02, "A3" }, { 30, 0x04, "the body" },
        { 56, 0x01, "the key id" }, { 58, 0x01, "the IPN" }, { 70, 0x01, "the MIC" },
    };
    for (unsigned i = 0; i < sizeof(cov) / sizeof(cov[0]); i++)
    {
        f[cov[i].off] ^= cov[i].bit;
        bool broken = !umac_mesh_bip_verify(M91_IGTK, f, f + 24, bl);
        f[cov[i].off] ^= cov[i].bit;
        CHECK(broken && umac_mesh_bip_verify(M91_IGTK, f, f + 24, bl), "flipping %s breaks it",
              cov[i].what);
    }
    f[1] |= 0x08 | 0x10 | 0x20;
    f[2] = 0x7f; f[22] = 0x35;
    CHECK(umac_mesh_bip_verify(M91_IGTK, f, f + 24, bl),
          "Retry, PwrMgt, MoreData, duration and sequence number are outside the AAD");
    f[1] = 0; f[2] = 0; f[22] = 0;

    const bool whole = umac_mesh_bip_parse(f + 24, bl, NULL, NULL);
    CHECK(whole && !umac_mesh_bip_parse(f + 24, bl - 1, NULL, NULL) &&
              !umac_mesh_bip_verify(M91_IGTK, f, f + 24, bl - 1),
          "a body that does not END in the MMIE carries none");
    f[54] = 75;
    CHECK(whole && !umac_mesh_bip_parse(f + 24, bl, NULL, NULL),
          "a trailing element that is not 76 is none");
    f[54] = 76; f[55] = 17;
    CHECK(whole && !umac_mesh_bip_parse(f + 24, bl, NULL, NULL), "nor is a 76 whose length is not 16");
    f[55] = 16;
    CHECK(whole && !umac_mesh_bip_parse(f + 24 + bl - 17, 17, NULL, NULL),
          "nor anything shorter than an MMIE");
}

int main(void)
{
    printf("=== BIP-CMAC-128: the MMIE on group-addressed path selection ===\n");
    t_rfc4493();
    t_vs_hostap();
    t_mmie_matches_reference();
    t_verify();
    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_mesh_bip: all passed\n");
    return 0;
}
