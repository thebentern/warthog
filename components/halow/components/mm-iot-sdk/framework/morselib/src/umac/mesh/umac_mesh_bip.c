/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * BIP-CMAC-128 as mac80211 frames it: MIC = first 8 octets of AES-CMAC(IGTK,
 * FC(Retry/PwrMgt/MoreData masked) || A1..A3 || body, MMIE MIC zeroed).
 * libc plus mbedtls/aes.h only, so the host tests run the shipping code.
 */
#include "umac_mesh_bip.h"

#include "mbedtls/aes.h"

#include <string.h>

#define BLK 16u
#define BIP_AAD_LEN 20u
#define BIP_MIC_LEN 8u

/* One message as up to two segments, with a run of @p b treated as zero. */
struct cmac_msg
{
    const uint8_t *a;
    size_t alen;
    const uint8_t *b;
    size_t blen;
    size_t zero_from;
    size_t zero_len;
};

static uint8_t msg_byte_(const struct cmac_msg *m, size_t i)
{
    if (i < m->alen)
    {
        return m->a[i];
    }
    i -= m->alen;
    if (i >= m->zero_from && i - m->zero_from < m->zero_len)
    {
        return 0;
    }
    return m->b[i];
}

/* RFC 4493 subkey: shift left one bit, XOR Rb into the last octet on carry out. */
static void cmac_dbl_(const uint8_t in[BLK], uint8_t out[BLK])
{
    uint8_t carry = 0;
    for (int i = BLK - 1; i >= 0; i--)
    {
        uint8_t b = in[i];
        out[i] = (uint8_t)((b << 1) | carry);
        carry = (uint8_t)(b >> 7);
    }
    if (carry)
    {
        out[BLK - 1] ^= 0x87u;
    }
}

static int cmac_(const uint8_t key[16], const struct cmac_msg *m, uint8_t mac[BLK])
{
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    int rc = -1;
    uint8_t l[BLK] = { 0 }, k1[BLK], k2[BLK], x[BLK] = { 0 }, blk[BLK];

    if (mbedtls_aes_setkey_enc(&ctx, key, 128) != 0 ||
        mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, l, l) != 0)
    {
        goto out;
    }
    cmac_dbl_(l, k1);
    cmac_dbl_(k1, k2);

    const size_t total = m->alen + m->blen;
    size_t n = (total + BLK - 1u) / BLK;
    const bool whole = (n != 0u) && (total % BLK == 0u);
    if (n == 0u)
    {
        n = 1u;
    }
    for (size_t i = 0; i + 1u < n; i++)
    {
        for (size_t j = 0; j < BLK; j++)
        {
            blk[j] = (uint8_t)(x[j] ^ msg_byte_(m, i * BLK + j));
        }
        if (mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, blk, x) != 0)
        {
            goto out;
        }
    }
    for (size_t j = 0; j < BLK; j++)
    {
        size_t idx = (n - 1u) * BLK + j;
        uint8_t v = idx < total ? msg_byte_(m, idx) : (idx == total ? 0x80u : 0u);
        blk[j] = (uint8_t)(x[j] ^ v ^ (whole ? k1[j] : k2[j]));
    }
    if (mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, blk, mac) != 0)
    {
        goto out;
    }
    rc = 0;
out:
    mbedtls_aes_free(&ctx);
    return rc;
}

int umac_mesh_aes_cmac128(const uint8_t key[16], const uint8_t *msg, size_t len, uint8_t mac[16])
{
    if (key == NULL || mac == NULL || (msg == NULL && len != 0u))
    {
        return -1;
    }
    struct cmac_msg m = { .a = msg, .alen = len, .b = NULL, .blen = 0, .zero_from = 0,
                          .zero_len = 0 };
    return cmac_(key, &m, mac);
}

static void bip_aad_(const uint8_t *hdr, uint8_t aad[BIP_AAD_LEN])
{
    aad[0] = hdr[0];
    aad[1] = (uint8_t)(hdr[1] & ~0x38u); /* Retry 0x08, PwrMgt 0x10, MoreData 0x20 */
    memcpy(&aad[2], hdr + 4, 18u);        /* A1, A2, A3 */
}

/* MIC over hdr + body, whose last UMAC_MESH_MMIE_LEN octets are the MMIE. */
static int bip_mic_(const uint8_t key[16], const uint8_t *hdr, const uint8_t *body, size_t len,
                    uint8_t mic[BIP_MIC_LEN])
{
    uint8_t aad[BIP_AAD_LEN], full[BLK];
    bip_aad_(hdr, aad);
    struct cmac_msg m = { .a = aad, .alen = sizeof(aad), .b = body, .blen = len,
                          .zero_from = len - BIP_MIC_LEN, .zero_len = BIP_MIC_LEN };
    if (cmac_(key, &m, full) != 0)
    {
        return -1;
    }
    memcpy(mic, full, BIP_MIC_LEN);
    return 0;
}

size_t umac_mesh_bip_protect(const uint8_t key[16], uint16_t key_id, uint64_t ipn,
                             const uint8_t hdr[UMAC_MESH_BIP_HDR_LEN], uint8_t *body,
                             size_t body_len, size_t cap)
{
    if (key == NULL || hdr == NULL || body == NULL || body_len > cap ||
        cap - body_len < UMAC_MESH_MMIE_LEN)
    {
        return 0;
    }
    uint8_t *e = body + body_len;
    e[0] = UMAC_MESH_MMIE_EID;
    e[1] = UMAC_MESH_MMIE_LEN - 2u;
    e[2] = (uint8_t)key_id;
    e[3] = (uint8_t)(key_id >> 8);
    for (int i = 0; i < 6; i++)
    {
        e[4 + i] = (uint8_t)(ipn >> (8 * i)); /* IPN little-endian, as mac80211 writes it */
    }
    memset(&e[10], 0, BIP_MIC_LEN);
    size_t n = body_len + UMAC_MESH_MMIE_LEN;
    if (bip_mic_(key, hdr, body, n, &e[10]) != 0)
    {
        return 0;
    }
    return n;
}

bool umac_mesh_bip_parse(const uint8_t *body, size_t len, uint16_t *key_id, uint64_t *ipn)
{
    if (body == NULL || len < UMAC_MESH_MMIE_LEN)
    {
        return false;
    }
    const uint8_t *e = body + len - UMAC_MESH_MMIE_LEN;
    if (e[0] != UMAC_MESH_MMIE_EID || e[1] != UMAC_MESH_MMIE_LEN - 2u)
    {
        return false;
    }
    if (key_id != NULL)
    {
        *key_id = (uint16_t)(e[2] | ((uint16_t)e[3] << 8));
    }
    if (ipn != NULL)
    {
        uint64_t v = 0;
        for (int i = 5; i >= 0; i--)
        {
            v = (v << 8) | e[4 + i];
        }
        *ipn = v;
    }
    return true;
}

bool umac_mesh_bip_verify(const uint8_t key[16], const uint8_t hdr[UMAC_MESH_BIP_HDR_LEN],
                          const uint8_t *body, size_t len)
{
    if (key == NULL || hdr == NULL || !umac_mesh_bip_parse(body, len, NULL, NULL))
    {
        return false;
    }
    uint8_t mic[BIP_MIC_LEN];
    if (bip_mic_(key, hdr, body, len, mic) != 0)
    {
        return false;
    }
    /* Constant time: the MIC compared is attacker-supplied. */
    uint8_t diff = 0;
    for (unsigned i = 0; i < BIP_MIC_LEN; i++)
    {
        diff |= (uint8_t)(mic[i] ^ body[len - BIP_MIC_LEN + i]);
    }
    return diff == 0u;
}
