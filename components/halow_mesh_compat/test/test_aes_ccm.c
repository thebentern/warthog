/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * AES-CCM, the cipher a host-software CCMP datapath runs on
 * (hostap src/crypto/aes-ccm.c).
 *
 * Checked against the RFC 3610 vectors, and then in place -- which is the
 * case that matters here and the case upstream got wrong. aes_ccm_encr()
 * wrote the keystream into the output buffer and then XORed the input into
 * it, so calling it with out == in destroyed the plaintext before reading it
 * and produced all zeros, under a MIC computed over the real plaintext. A
 * packet encrypted in its own buffer would have gone out as zeros and
 * verified as authentic at the far end. Nothing about that failure is loud.
 *
 * The tail block was also written a full 16 bytes wide for a partial block,
 * overrunning by up to 15 bytes; callers hid it by over-allocating. Both are
 * fixed, and both are pinned below.
 */
#include "includes.h"
#include "common.h"
#include "aes.h"
#include "aes_wrap.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

int aes_ccm_ae(const u8 *key, size_t key_len, const u8 *nonce, size_t M,
               const u8 *plain, size_t plain_len, const u8 *aad, size_t aad_len,
               u8 *crypt, u8 *auth);
int aes_ccm_ad(const u8 *key, size_t key_len, const u8 *nonce, size_t M,
               const u8 *crypt, size_t crypt_len, const u8 *aad, size_t aad_len,
               const u8 *auth, u8 *plain);

/* RFC 3610 packet vector #1: 16-byte key, 13-byte nonce, M=8, 8 bytes of AAD,
 * 23 bytes of payload. */
static const u8 K[16] = { 0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,
                          0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF };
static const u8 N[13] = { 0x00,0x00,0x00,0x03,0x02,0x01,0x00,0xA0,
                          0xA1,0xA2,0xA3,0xA4,0xA5 };
static const u8 AAD[8] = { 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07 };
static const u8 PT[23] = { 0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
                           0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
                           0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E };
static const u8 CT[23] = { 0x58,0x8C,0x97,0x9A,0x61,0xC6,0x63,0xD2,
                           0xF0,0x66,0xD0,0xC2,0xC0,0xF9,0x89,0x80,
                           0x6D,0x5F,0x6B,0x61,0xDA,0xC3,0x84 };
static const u8 MIC[8] = { 0x17,0xE8,0xD1,0x2C,0xFD,0xF9,0x26,0xE0 };

int main(void)
{
    u8 ct[64], mic[8], pt[64];

    /* Baseline: separate buffers, the way upstream is exercised. */
    memset(ct, 0, sizeof(ct));
    CHECK(aes_ccm_ae(K, sizeof(K), N, 8, PT, sizeof(PT), AAD, sizeof(AAD), ct, mic) == 0,
          "aes_ccm_ae returns success");
    CHECK(memcmp(ct, CT, sizeof(CT)) == 0, "ciphertext matches RFC 3610 vector #1");
    CHECK(memcmp(mic, MIC, sizeof(MIC)) == 0, "MIC matches RFC 3610 vector #1");

    CHECK(aes_ccm_ad(K, sizeof(K), N, 8, CT, sizeof(CT), AAD, sizeof(AAD), MIC, pt) == 0,
          "aes_ccm_ad accepts the vector");
    CHECK(memcmp(pt, PT, sizeof(PT)) == 0, "decrypt recovers the plaintext");

    /* A corrupted MIC must be rejected. */
    u8 bad[8]; memcpy(bad, MIC, 8); bad[0] ^= 0x01;
    CHECK(aes_ccm_ad(K, sizeof(K), N, 8, CT, sizeof(CT), AAD, sizeof(AAD), bad, pt) != 0,
          "a flipped MIC bit is rejected");
    /* ...and so must a corrupted ciphertext. */
    u8 badct[23]; memcpy(badct, CT, 23); badct[5] ^= 0x80;
    CHECK(aes_ccm_ad(K, sizeof(K), N, 8, badct, 23, AAD, sizeof(AAD), MIC, pt) != 0,
          "a flipped ciphertext bit is rejected");
    /* AAD is authenticated, not encrypted -- tampering with it must fail. */
    u8 badaad[8]; memcpy(badaad, AAD, 8); badaad[3] ^= 0x10;
    CHECK(aes_ccm_ad(K, sizeof(K), N, 8, CT, sizeof(CT), badaad, sizeof(badaad), MIC, pt) != 0,
          "a flipped AAD bit is rejected");

    /* IN PLACE -- the case a packet datapath actually uses, and the one that
     * silently produced zeros before the fix. */
    u8 buf[64];
    memcpy(buf, PT, sizeof(PT));
    CHECK(aes_ccm_ae(K, sizeof(K), N, 8, buf, sizeof(PT), AAD, sizeof(AAD), buf, mic) == 0,
          "in-place encrypt returns success");
    CHECK(memcmp(buf, CT, sizeof(CT)) == 0, "IN-PLACE encrypt matches the vector");
    CHECK(memcmp(mic, MIC, sizeof(MIC)) == 0, "in-place encrypt produces the right MIC");

    memcpy(buf, CT, sizeof(CT));
    CHECK(aes_ccm_ad(K, sizeof(K), N, 8, buf, sizeof(CT), AAD, sizeof(AAD), MIC, buf) == 0,
          "in-place decrypt accepts");
    CHECK(memcmp(buf, PT, sizeof(PT)) == 0, "IN-PLACE decrypt recovers the plaintext");

    /* Exact-length tail. 23 bytes is 1 full block + a 7-byte remainder, so a
     * tail that writes 16 bytes scribbles 9 past the end. Fence the buffer. */
    u8 fenced[23 + 16];
    memset(fenced, 0xA5, sizeof(fenced));
    memcpy(fenced, PT, sizeof(PT));
    CHECK(aes_ccm_ae(K, sizeof(K), N, 8, fenced, sizeof(PT), AAD, sizeof(AAD), fenced, mic) == 0,
          "encrypt with a fenced buffer returns success");
    int fence_ok = 1;
    for (size_t i = sizeof(PT); i < sizeof(fenced); i++) {
        if (fenced[i] != 0xA5) { fence_ok = 0; break; }
    }
    CHECK(fence_ok, "partial tail block does not write past the payload");

    /* Payload sizes around the block boundary, in place, round-tripped. A
     * mesh datapath sees all of these -- an ARP replica is short, a full
     * MeshPacket is not. */
    for (size_t n = 1; n <= 33; n++) {
        u8 a[64], b[64], m[8];
        for (size_t i = 0; i < n; i++) a[i] = (u8)(i * 7 + 1);
        memcpy(b, a, n);
        if (aes_ccm_ae(K, sizeof(K), N, 8, b, n, AAD, sizeof(AAD), b, m) != 0 ||
            aes_ccm_ad(K, sizeof(K), N, 8, b, n, AAD, sizeof(AAD), m, b) != 0 ||
            memcmp(a, b, n) != 0) {
            CHECK(0, "in-place round trip at %zu bytes", n);
            break;
        }
        if (n == 33) CHECK(1, "in-place round trip, every length 1..33 bytes");
    }

    /* Zero-length payload: a frame that is all header and MIC is legal. */
    CHECK(aes_ccm_ae(K, sizeof(K), N, 8, PT, 0, AAD, sizeof(AAD), ct, mic) == 0,
          "zero-length payload encrypts");
    CHECK(aes_ccm_ad(K, sizeof(K), N, 8, ct, 0, AAD, sizeof(AAD), mic, pt) == 0,
          "zero-length payload round-trips");

    printf(failures ? "\nFAILED (%d)\n" : "\nALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
