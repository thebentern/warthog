/*
 * Cross-check warthog_ccm_ae/ad against hostap's aes_ccm_ae/ad.
 *
 * The shipping mesh data path uses warthog_ccm_*, and until now nothing on any
 * build validated it except a target-side KAT that ran behind four stages of
 * the reference implementation. The two have DIFFERENT argument orders --
 * hostap takes (key, key_len, nonce, M, plain, plain_len, aad, aad_len, ...)
 * while ours takes (key, nonce, M, aad, aad_len, data, data_len, ...) -- so a
 * transposed call compiles cleanly and silently produces wrong ciphertext.
 * Comparing outputs byte for byte across a spread of lengths is the cheapest
 * thing that catches that, and it runs without hardware. warthog_ccm_mic, the
 * read-only MIC the chip-opened group frame check uses, must match the same MIC.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "umac_mesh_ccm.h"

int aes_ccm_ae(const uint8_t *key, size_t key_len, const uint8_t *nonce, size_t M,
               const uint8_t *plain, size_t plain_len, const uint8_t *aad, size_t aad_len,
               uint8_t *crypt, uint8_t *auth);
int aes_ccm_ad(const uint8_t *key, size_t key_len, const uint8_t *nonce, size_t M,
               const uint8_t *crypt, size_t crypt_len, const uint8_t *aad, size_t aad_len,
               const uint8_t *auth, uint8_t *plain);

static int fails;
static void check(int cond, const char *what, size_t len)
{
    if (!cond) { printf("  FAIL: %s (len=%zu)\n", what, len); fails++; }
}

int main(void)
{
    /* CCMP-128: 16-byte key, 13-byte nonce, 8-byte MIC -- the mesh case. */
    static const uint8_t key[16] = { 0xc9,0x7c,0x1f,0x67,0xce,0x37,0x11,0x85,
                                     0x51,0x4a,0x8a,0x19,0xf2,0xbd,0xd5,0x2f };
    static const uint8_t nonce[13] = { 0x00,0x50,0x30,0xf1,0x84,0x44,0x08,
                                       0xb5,0x03,0x97,0x76,0xe7,0x0c };
    static const uint8_t aad[22] = { 0x08,0x40,0x0f,0xd2,0xe1,0x28,0xa5,0x7c,
                                     0x50,0x30,0xf1,0x84,0x44,0x08,0xab,0xae,
                                     0xa5,0xb8,0xfc,0xba,0x00,0x00 };
    const size_t lens[] = { 0, 1, 15, 16, 17, 63, 64, 256, 1500 };

    printf("warthog_ccm vs hostap aes_ccm, %zu lengths\n", sizeof(lens)/sizeof(lens[0]));
    for (size_t i = 0; i < sizeof(lens)/sizeof(lens[0]); i++)
    {
        size_t n = lens[i];
        uint8_t *pt = malloc(n ? n : 1), *a = malloc(n ? n : 1), *b = malloc(n ? n : 1);
        uint8_t mic_a[8], mic_b[8];
        for (size_t j = 0; j < n; j++) { pt[j] = (uint8_t)(j * 7u + 3u); }

        /* reference */
        if (aes_ccm_ae(key, sizeof(key), nonce, 8, pt, n, aad, sizeof(aad), a, mic_a) != 0)
        { printf("  FAIL: hostap encrypt (len=%zu)\n", n); fails++; }

        /* ours -- encrypts in place, note the different argument order */
        memcpy(b, pt, n);
        if (warthog_ccm_ae(key, nonce, 8, aad, sizeof(aad), b, n, mic_b) != 0)
        { printf("  FAIL: warthog encrypt (len=%zu)\n", n); fails++; }

        check(memcmp(a, b, n) == 0, "ciphertext differs", n);
        check(memcmp(mic_a, mic_b, 8) == 0, "MIC differs", n);

        /* warthog_ccm_mic: the same MIC from the plaintext, which it must leave alone. */
        {
            uint8_t mic_c[8];
            memcpy(b, pt, n);
            check(warthog_ccm_mic(key, nonce, 8, aad, sizeof(aad), b, n, mic_c) == 0 &&
                      memcmp(mic_c, mic_a, 8) == 0, "warthog_ccm_mic MIC differs", n);
            check(memcmp(b, pt, n) == 0, "warthog_ccm_mic changed the plaintext", n);
        }

        /* ours must decrypt the reference's ciphertext */
        memcpy(b, a, n);
        check(warthog_ccm_ad(key, nonce, 8, aad, sizeof(aad), b, n, mic_a) == 0,
              "warthog rejected a valid frame", n);
        check(memcmp(b, pt, n) == 0, "roundtrip plaintext differs", n);

        /* and must reject a tampered MIC */
        uint8_t bad[8]; memcpy(bad, mic_a, 8); bad[7] ^= 0x80u;
        memcpy(b, a, n);
        check(warthog_ccm_ad(key, nonce, 8, aad, sizeof(aad), b, n, bad) != 0,
              "warthog accepted a forged MIC", n);

        /* a flipped AAD byte must also fail -- this is the property the mesh
         * depends on, since the AAD is the 802.11 header both ends derive
         * independently. */
        if (n)
        {
            uint8_t aad2[sizeof(aad)]; memcpy(aad2, aad, sizeof(aad)); aad2[1] ^= 0x01u;
            memcpy(b, a, n);
            check(warthog_ccm_ad(key, nonce, 8, aad2, sizeof(aad2), b, n, mic_a) != 0,
                  "warthog accepted a frame whose AAD changed", n);
        }
        free(pt); free(a); free(b);
    }
    printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
