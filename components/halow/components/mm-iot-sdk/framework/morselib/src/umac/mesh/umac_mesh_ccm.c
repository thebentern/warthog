/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * AES-CCM built on mbedtls's BULK primitives, for the host CCMP datapath.
 *
 * Functionally identical to hostap's aes-ccm.c -- same B_0, same AAD framing,
 * same counter blocks, verified against the same RFC 3610 vector -- but it
 * reaches the cipher differently, and on this part that is the whole story:
 *
 *     aes_encrypt(), one 16-byte ECB block        43 us
 *     16 of those, for 256 bytes                 688 us
 *     one bulk CTR call, same 256 bytes           46 us
 *
 * 43 us is ~7000 cycles at 160 MHz, which is not what the AES peripheral
 * costs; it is what acquiring and releasing it costs, paid once per call.
 * hostap drives CCM one ECB block at a time and so does mbedtls's own
 * mbedtls_ccm_encrypt_and_tag(), which measured no better. Since CCM is CTR
 * plus CBC-MAC, and both have bulk APIs that stay inside a single acquire,
 * building on those turns ~1.6 ms per frame into ~0.1 ms.
 *
 * The CBC-MAC needs no special support: CCM defines X_1 = E(B_0) and
 * X_i+1 = E(X_i XOR B_i), which is exactly CBC with a zero IV, so the final
 * ciphertext block IS the MAC. mbedtls updates the IV in place, so a long
 * payload can be chained through a small scratch buffer rather than needing
 * one as large as the frame -- the CBC output is discarded, only the IV
 * matters.
 *
 * hostap's aes-ccm.c stays in the build as the reference the host tests pin
 * (test_aes_ccm.c); this is the one the datapath calls.
 */
#include "umac_mesh_ccm.h"

#include "mbedtls/aes.h"

#include <string.h>

#define CCM_BLOCK 16u
/** Largest AAD CCMP presents: a 30-byte 4-address QoS header, plus the
 *  2-byte length prefix, rounds to two blocks. */
#define CCM_AAD_BUF (2u * CCM_BLOCK)
/** Scratch for discarded CBC output. Chunking keeps stack use bounded without
 *  giving up the bulk call -- a 1500-byte frame is 6 calls, not 94. */
#define CCM_CBC_SCRATCH 256u

/* B_0 = Flags | Nonce | l(m), per RFC 3610 / IEEE 802.11 CCMP. */
static void ccm_b0(uint8_t b[CCM_BLOCK], size_t M, size_t L, const uint8_t *nonce,
                   size_t aad_len, size_t plain_len)
{
    b[0] = (uint8_t)((aad_len ? 0x40u : 0u) | (((M - 2u) / 2u) << 3) | (L - 1u));
    memcpy(&b[1], nonce, 15u - L);
    b[CCM_BLOCK - 2] = (uint8_t)(plain_len >> 8);
    b[CCM_BLOCK - 1] = (uint8_t)(plain_len & 0xff);
}

/* A_i = Flags | Nonce | Counter i. */
static void ccm_a_block(uint8_t a[CCM_BLOCK], size_t L, const uint8_t *nonce, uint16_t counter)
{
    a[0] = (uint8_t)(L - 1u);
    memcpy(&a[1], nonce, 15u - L);
    memset(&a[CCM_BLOCK - L], 0, L);
    a[CCM_BLOCK - 2] = (uint8_t)(counter >> 8);
    a[CCM_BLOCK - 1] = (uint8_t)(counter & 0xff);
}

/* CBC-MAC a block-aligned run, chaining through @p iv. Output is discarded;
 * @p iv carries the state, which is what makes chunking sound. */
static int ccm_cbc_mac(mbedtls_aes_context *ctx, uint8_t iv[CCM_BLOCK], const uint8_t *data,
                       size_t len)
{
    uint8_t scratch[CCM_CBC_SCRATCH];

    while (len != 0u)
    {
        size_t n = (len > sizeof(scratch)) ? sizeof(scratch) : len;
        if (mbedtls_aes_crypt_cbc(ctx, MBEDTLS_AES_ENCRYPT, n, iv, data, scratch) != 0)
        {
            return -1;
        }
        data += n;
        len -= n;
    }
    return 0;
}

/* Everything both directions share: the MAC over B_0 | AAD | payload, and the
 * S_0 keystream block the MIC is masked with. On return @p x holds the MAC and
 * @p a is primed at A_1 for the payload. */
static int ccm_auth(mbedtls_aes_context *ctx, size_t M, size_t L, const uint8_t *nonce,
                    const uint8_t *aad, size_t aad_len, const uint8_t *plain, size_t plain_len,
                    uint8_t x[CCM_BLOCK], uint8_t s0[CCM_BLOCK], uint8_t a[CCM_BLOCK])
{
    uint8_t hdr[CCM_BLOCK + CCM_AAD_BUF];
    size_t hdr_len = CCM_BLOCK;

    if (aad_len > CCM_AAD_BUF - 2u)
    {
        return -1;
    }

    ccm_b0(hdr, M, L, nonce, aad_len, plain_len);
    if (aad_len != 0u)
    {
        /* B_1.. = l(a) | a | zero padding, to a whole number of blocks. */
        uint8_t *ab = &hdr[CCM_BLOCK];
        ab[0] = (uint8_t)(aad_len >> 8);
        ab[1] = (uint8_t)(aad_len & 0xff);
        memcpy(&ab[2], aad, aad_len);
        size_t used = 2u + aad_len;
        size_t padded = ((used + CCM_BLOCK - 1u) / CCM_BLOCK) * CCM_BLOCK;
        memset(&ab[used], 0, padded - used);
        hdr_len += padded;
    }

    memset(x, 0, CCM_BLOCK); /* zero IV: C_1 = E(B_0 XOR 0) = X_1 */
    if (ccm_cbc_mac(ctx, x, hdr, hdr_len) != 0)
    {
        return -1;
    }

    /* Payload: whole blocks straight from the frame, then a zero-padded tail. */
    size_t full = plain_len & ~(CCM_BLOCK - 1u);
    if (full != 0u && ccm_cbc_mac(ctx, x, plain, full) != 0)
    {
        return -1;
    }
    size_t last = plain_len - full;
    if (last != 0u)
    {
        uint8_t tail[CCM_BLOCK];
        memset(tail, 0, sizeof(tail));
        memcpy(tail, plain + full, last);
        if (ccm_cbc_mac(ctx, x, tail, CCM_BLOCK) != 0)
        {
            return -1;
        }
    }

    /* S_0 = E(A_0). Running it through CTR leaves the counter at A_1, exactly
     * where the payload keystream must start. */
    ccm_a_block(a, L, nonce, 0);
    memset(s0, 0, CCM_BLOCK);
    size_t nc_off = 0;
    uint8_t stream[CCM_BLOCK];
    if (mbedtls_aes_crypt_ctr(ctx, CCM_BLOCK, &nc_off, a, stream, s0, s0) != 0)
    {
        return -1;
    }
    return 0;
}

int warthog_ccm_ae(const uint8_t *key, const uint8_t *nonce, size_t M, const uint8_t *aad,
                   size_t aad_len, uint8_t *data, size_t data_len, uint8_t *auth)
{
    mbedtls_aes_context ctx;
    uint8_t x[CCM_BLOCK], s0[CCM_BLOCK], a[CCM_BLOCK], stream[CCM_BLOCK];
    int rc = -1;

    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_enc(&ctx, key, 128) != 0)
    {
        goto out;
    }
    /* MAC over the PLAINTEXT, so this must run before the payload is
     * encrypted -- data is enciphered in place below. */
    if (ccm_auth(&ctx, M, 2, nonce, aad, aad_len, data, data_len, x, s0, a) != 0)
    {
        goto out;
    }

    size_t nc_off = 0;
    if (data_len != 0u &&
        mbedtls_aes_crypt_ctr(&ctx, data_len, &nc_off, a, stream, data, data) != 0)
    {
        goto out;
    }
    for (size_t i = 0; i < M; i++)
    {
        auth[i] = (uint8_t)(x[i] ^ s0[i]);
    }
    rc = 0;

out:
    mbedtls_aes_free(&ctx);
    return rc;
}

int warthog_ccm_ad(const uint8_t *key, const uint8_t *nonce, size_t M, const uint8_t *aad,
                   size_t aad_len, uint8_t *data, size_t data_len, const uint8_t *auth)
{
    mbedtls_aes_context ctx;
    uint8_t x[CCM_BLOCK], s0[CCM_BLOCK], a[CCM_BLOCK], stream[CCM_BLOCK];
    int rc = -1;

    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_enc(&ctx, key, 128) != 0) /* CTR and CBC-MAC both encrypt */
    {
        goto out;
    }

    /* Decrypt first: the MAC is over the plaintext. */
    ccm_a_block(a, 2, nonce, 0);
    memset(s0, 0, CCM_BLOCK);
    size_t nc_off = 0;
    if (mbedtls_aes_crypt_ctr(&ctx, CCM_BLOCK, &nc_off, a, stream, s0, s0) != 0)
    {
        goto out;
    }
    if (data_len != 0u &&
        mbedtls_aes_crypt_ctr(&ctx, data_len, &nc_off, a, stream, data, data) != 0)
    {
        goto out;
    }

    {
        uint8_t x2[CCM_BLOCK], s0b[CCM_BLOCK], a2[CCM_BLOCK];
        if (ccm_auth(&ctx, M, 2, nonce, aad, aad_len, data, data_len, x2, s0b, a2) != 0)
        {
            goto out;
        }
        /* Constant time: a MIC check that leaks timing leaks the MIC. */
        uint8_t diff = 0;
        for (size_t i = 0; i < M; i++)
        {
            diff |= (uint8_t)(auth[i] ^ (x2[i] ^ s0b[i]));
        }
        if (diff != 0u)
        {
            goto out; /* caller must discard the frame; data is already clobbered */
        }
    }
    rc = 0;

out:
    mbedtls_aes_free(&ctx);
    return rc;
}
