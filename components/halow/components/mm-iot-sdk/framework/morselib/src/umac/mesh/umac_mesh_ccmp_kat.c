/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * On-target known-answer test and benchmark for AES-CCM.
 *
 * A host test already pins hostap's aes-ccm.c against RFC 3610, but it links
 * hostap's SOFTWARE AES. On target, aes_encrypt() goes through the mbedtls
 * shim instead -- the same shim whose aes_encrypt_init() was building a
 * DECRYPTION key schedule until we fixed it, which would have made every
 * result silently wrong. That fix has never executed on hardware. This runs
 * the same vector through the path that actually ships.
 *
 * It also times the cipher, because host software CCMP is only viable if the
 * ESP32-S3 can afford it: mbedtls here may use the DMA AES port, where every
 * 16-byte block takes a shared mutex and a GDMA transaction. CCMP costs
 * 4 + 2*ceil(payload/16) block operations per frame, so the per-call cost
 * decides whether this approach is practical at all.
 *
 * Results are published into counters owned by main/at.c (AT+CCMPKAT?) --
 * morselib is a static archive and the linker will not extract an object to
 * satisfy a call from main.
 */
#include "umac_mesh_ccmp_kat.h"

/* Via the supplicant shim's private header, which is morselib's established
 * way in to hostap's crypto declarations (bip.c reaches omac1_aes_128 the same
 * way). Including hostap's aes.h directly from a morselib TU does not work --
 * it needs hostap's own type prelude, and pulling that in rewrites aes_encrypt
 * to its mangled name. */
#include "umac/supplicant_shim/umac_supp_shim_private.h"
#include "mmosal.h"
#include "mbedtls/ccm.h"
#include "mbedtls/aes.h"
#include "umac/mesh/umac_mesh_ccm.h"

#include <string.h>

/* Published to main/at.c. */
extern volatile uint32_t g_warthog_ccmp_kat_ok;
extern volatile uint32_t g_warthog_ccmp_kat_ran;
extern volatile uint32_t g_warthog_ccmp_kat_fail_stage;
extern volatile uint32_t g_warthog_aes_ns_per_block;
extern volatile uint32_t g_warthog_ccmp_us_per_frame;
extern volatile uint32_t g_warthog_mbedtls_us_per_frame;
extern volatile uint32_t g_warthog_aes_setup_ns, g_warthog_aes_ecb_ns;
extern volatile uint32_t g_warthog_aes_ctr_us;
extern volatile uint32_t g_warthog_bulk_ccm_us;

/* RFC 3610 packet vector #1. */
static const uint8_t K[16] = { 0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,
                               0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF };
static const uint8_t N[13] = { 0x00,0x00,0x00,0x03,0x02,0x01,0x00,0xA0,
                               0xA1,0xA2,0xA3,0xA4,0xA5 };
static const uint8_t AAD[8] = { 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07 };
static const uint8_t PT[23] = { 0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
                                0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
                                0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E };
static const uint8_t CT[23] = { 0x58,0x8C,0x97,0x9A,0x61,0xC6,0x63,0xD2,
                                0xF0,0x66,0xD0,0xC2,0xC0,0xF9,0x89,0x80,
                                0x6D,0x5F,0x6B,0x61,0xDA,0xC3,0x84 };
static const uint8_t MIC[8] = { 0x17,0xE8,0xD1,0x2C,0xFD,0xF9,0x26,0xE0 };

/* A mesh frame's worth of payload, for the timing pass: an encrypted
 * MeshPacket is around this size. */
#define KAT_BENCH_PAYLOAD 256
#define KAT_BENCH_ITERS 64

void umac_mesh_ccmp_kat_run(void)
{
    uint8_t buf[sizeof(PT) + 16];
    uint8_t mic[8];
    uint32_t stage = 0;

    g_warthog_ccmp_kat_ran++;

    /* 1: the vector, separate buffers. */
    stage = 1;
    memset(buf, 0, sizeof(buf));
    if (aes_ccm_ae(K, sizeof(K), N, 8, PT, sizeof(PT), AAD, sizeof(AAD), buf, mic) != 0 ||
        memcmp(buf, CT, sizeof(CT)) != 0 || memcmp(mic, MIC, sizeof(MIC)) != 0)
    {
        goto fail;
    }

    /* 2: decrypt back. */
    stage = 2;
    if (aes_ccm_ad(K, sizeof(K), N, 8, CT, sizeof(CT), AAD, sizeof(AAD), MIC, buf) != 0 ||
        memcmp(buf, PT, sizeof(PT)) != 0)
    {
        goto fail;
    }

    /* 3: IN PLACE -- what a packet datapath does, and what was broken. */
    stage = 3;
    memcpy(buf, PT, sizeof(PT));
    if (aes_ccm_ae(K, sizeof(K), N, 8, buf, sizeof(PT), AAD, sizeof(AAD), buf, mic) != 0 ||
        memcmp(buf, CT, sizeof(CT)) != 0 || memcmp(mic, MIC, sizeof(MIC)) != 0)
    {
        goto fail;
    }
    if (aes_ccm_ad(K, sizeof(K), N, 8, buf, sizeof(CT), AAD, sizeof(AAD), MIC, buf) != 0 ||
        memcmp(buf, PT, sizeof(PT)) != 0)
    {
        goto fail;
    }

    /* 4: a tampered MIC must be refused, or "decryption works" means nothing. */
    stage = 4;
    {
        uint8_t bad[8];
        memcpy(bad, MIC, sizeof(bad));
        bad[0] ^= 0x01;
        if (aes_ccm_ad(K, sizeof(K), N, 8, CT, sizeof(CT), AAD, sizeof(AAD), bad, buf) == 0)
        {
            goto fail;
        }
    }

    g_warthog_ccmp_kat_ok = 1;
    g_warthog_ccmp_kat_fail_stage = 0;

    /* Timing. One encrypt of a realistic payload, repeated; report per-frame
     * microseconds and the implied per-AES-block nanoseconds. */
    {
        static uint8_t bench[KAT_BENCH_PAYLOAD + 16];
        uint8_t bmic[8];
        uint32_t t0 = mmosal_get_time_ms();
        for (int i = 0; i < KAT_BENCH_ITERS; i++)
        {
            (void)aes_ccm_ae(K, sizeof(K), N, 8, bench, KAT_BENCH_PAYLOAD, AAD, sizeof(AAD),
                             bench, bmic);
        }
        uint32_t ms = mmosal_get_time_ms() - t0;
        uint32_t us_per_frame = (ms * 1000u) / KAT_BENCH_ITERS;
        g_warthog_ccmp_us_per_frame = us_per_frame;
        /* CCMP block cost: 1 (B_0) + 1 (AAD) + ceil(P/16) for the MIC pass,
         * + 1 (S_0) + ceil(P/16) for the keystream. */
        uint32_t blocks = 3u + 2u * ((KAT_BENCH_PAYLOAD + 15u) / 16u);
        g_warthog_aes_ns_per_block = blocks ? (us_per_frame * 1000u) / blocks : 0;
    }

    /* 5: the BULK implementation must agree with the reference, byte for byte,
     * on the same vector -- including in place, and including refusing a
     * tampered MIC. An optimisation that produces different ciphertext is not
     * an optimisation. */
    stage = 5;
    {
        uint8_t b[sizeof(PT) + 16];
        uint8_t bmic[8];
        memcpy(b, PT, sizeof(PT));
        if (warthog_ccm_ae(K, N, 8, AAD, sizeof(AAD), b, sizeof(PT), bmic) != 0 ||
            memcmp(b, CT, sizeof(CT)) != 0 || memcmp(bmic, MIC, sizeof(MIC)) != 0)
        {
            goto fail;
        }
        if (warthog_ccm_ad(K, N, 8, AAD, sizeof(AAD), b, sizeof(CT), MIC) != 0 ||
            memcmp(b, PT, sizeof(PT)) != 0)
        {
            goto fail;
        }
        uint8_t badmic[8];
        memcpy(badmic, MIC, sizeof(badmic));
        badmic[7] ^= 0x80;
        memcpy(b, CT, sizeof(CT));
        if (warthog_ccm_ad(K, N, 8, AAD, sizeof(AAD), b, sizeof(CT), badmic) == 0)
        {
            goto fail;
        }
    }

    /* Split fixed cost from per-block cost.
     *
     * aes_ccm_ae() runs aes_encrypt_init() -- a malloc plus an mbedtls key
     * schedule -- and aes_encrypt_deinit() on EVERY call, so a per-frame
     * figure cannot say whether the cost is the cipher or the setup. Time a
     * bare context init/deinit, and separately a run of ECB blocks on an
     * already-initialised context. If setup dominates, caching one context
     * per key is the fix and it is a small change; if the blocks dominate,
     * the cipher itself is the ceiling. */
    {
        uint8_t blk[16] = { 0 };
        uint32_t t0 = mmosal_get_time_ms();
        for (int i = 0; i < 200; i++)
        {
            void *a = aes_encrypt_init(K, 16);
            if (a) { aes_encrypt_deinit(a); }
        }
        g_warthog_aes_setup_ns = ((mmosal_get_time_ms() - t0) * 1000000u) / 200u;

        void *aes = aes_encrypt_init(K, 16);
        if (aes)
        {
            t0 = mmosal_get_time_ms();
            for (int i = 0; i < 2000; i++)
            {
                (void)aes_encrypt(aes, blk, blk);
            }
            g_warthog_aes_ecb_ns = ((mmosal_get_time_ms() - t0) * 1000000u) / 2000u;
            aes_encrypt_deinit(aes);
        }
    }

    /* Bulk CTR over a whole frame, in ONE call.
     *
     * 50 us for a single 16-byte ECB block is ~8000 cycles at 160 MHz, which
     * is not what the AES peripheral costs -- it is what acquiring and
     * releasing it costs, paid once per call. CCM is CTR plus CBC-MAC, and
     * mbedtls's CTR does the entire buffer inside one acquire. If this is
     * dramatically cheaper than 16 separate ECB calls, then a CCM built on
     * bulk CTR + bulk CBC-MAC is the implementation to write, and per-block
     * ECB is simply the wrong API to have reached for. */
    {
        /* And time the bulk CCM end to end, which is the number that decides
         * whether the datapath can afford host crypto. */
        {
            static uint8_t wbuf[KAT_BENCH_PAYLOAD];
            uint8_t wmic[8];
            uint32_t t0 = mmosal_get_time_ms();
            for (int i = 0; i < KAT_BENCH_ITERS; i++)
            {
                (void)warthog_ccm_ae(K, N, 8, AAD, sizeof(AAD), wbuf, sizeof(wbuf), wmic);
            }
            g_warthog_bulk_ccm_us = ((mmosal_get_time_ms() - t0) * 1000u) / KAT_BENCH_ITERS;
        }

        static uint8_t ctrbuf[KAT_BENCH_PAYLOAD];
        uint8_t nonce_counter[16] = { 0 };
        uint8_t stream_block[16] = { 0 };
        size_t nc_off = 0;
        mbedtls_aes_context actx;
        mbedtls_aes_init(&actx);
        if (mbedtls_aes_setkey_enc(&actx, K, 128) == 0)
        {
            uint32_t t0 = mmosal_get_time_ms();
            for (int i = 0; i < KAT_BENCH_ITERS; i++)
            {
                nc_off = 0;
                memset(nonce_counter, 0, sizeof(nonce_counter));
                (void)mbedtls_aes_crypt_ctr(&actx, sizeof(ctrbuf), &nc_off, nonce_counter,
                                            stream_block, ctrbuf, ctrbuf);
            }
            g_warthog_aes_ctr_us = ((mmosal_get_time_ms() - t0) * 1000u) / KAT_BENCH_ITERS;
        }
        mbedtls_aes_free(&actx);
    }

    /* Same work through mbedtls's own CCM API, for comparison.
     *
     * hostap drives the cipher one 16-byte ECB block at a time, and on this
     * part each of those calls goes through the AES peripheral's DMA path --
     * a shared mutex and a GDMA transaction per block, whose setup cost
     * swamps the 16 bytes of actual work. mbedtls_ccm_encrypt_and_tag() does
     * the whole frame in one call and can amortise that. If it is markedly
     * faster, the datapath should use it and hostap's aes-ccm.c stays as the
     * reference implementation the host tests pin. */
    {
        static uint8_t bench2[KAT_BENCH_PAYLOAD + 16];
        uint8_t tag[8];
        mbedtls_ccm_context ctx;
        mbedtls_ccm_init(&ctx);
        if (mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, K, 128) == 0)
        {
            uint32_t t0 = mmosal_get_time_ms();
            for (int i = 0; i < KAT_BENCH_ITERS; i++)
            {
                (void)mbedtls_ccm_encrypt_and_tag(&ctx, KAT_BENCH_PAYLOAD, N, sizeof(N), AAD,
                                                  sizeof(AAD), bench2, bench2, tag, sizeof(tag));
            }
            uint32_t ms = mmosal_get_time_ms() - t0;
            g_warthog_mbedtls_us_per_frame = (ms * 1000u) / KAT_BENCH_ITERS;
        }
        mbedtls_ccm_free(&ctx);
    }
    return;

fail:
    g_warthog_ccmp_kat_ok = 0;
    g_warthog_ccmp_kat_fail_stage = stage;
}
