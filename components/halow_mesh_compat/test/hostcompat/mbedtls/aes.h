/*
 * Host-test stand-in for mbedtls/aes.h.
 *
 * umac_mesh_ccm.c is written against mbedtls because that is what ships on the
 * target. To test that file on a host -- which is the only place its output can
 * be compared against hostap's reference CCM -- the few entry points it uses
 * are re-implemented here on top of hostap's software AES. Same primitive,
 * different packaging: if the two CCM implementations then disagree, the
 * disagreement is in umac_mesh_ccm.c and not in the block cipher.
 */
#ifndef HOSTCOMPAT_MBEDTLS_AES_H
#define HOSTCOMPAT_MBEDTLS_AES_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MBEDTLS_AES_ENCRYPT 1
#define MBEDTLS_AES_DECRYPT 0

typedef struct { void *ctx; } mbedtls_aes_context;

/* hostap's software AES (aes-internal-enc.c). */
void *aes_encrypt_init(const uint8_t *key, size_t len);
int aes_encrypt(void *ctx, const uint8_t *plain, uint8_t *crypt);
void aes_encrypt_deinit(void *ctx);

static inline void mbedtls_aes_init(mbedtls_aes_context *c) { c->ctx = NULL; }
static inline void mbedtls_aes_free(mbedtls_aes_context *c)
{
    if (c->ctx) { aes_encrypt_deinit(c->ctx); c->ctx = NULL; }
}
static inline int mbedtls_aes_setkey_enc(mbedtls_aes_context *c, const unsigned char *key,
                                         unsigned int keybits)
{
    c->ctx = aes_encrypt_init(key, keybits / 8u);
    return c->ctx ? 0 : -1;
}
static inline int mbedtls_aes_crypt_ecb(mbedtls_aes_context *c, int mode,
                                        const unsigned char in[16], unsigned char out[16])
{
    (void)mode;
    return aes_encrypt(c->ctx, in, out);
}
/* CBC-MAC use only: umac_mesh_ccm.c feeds whole blocks and keeps the IV. */
static inline int mbedtls_aes_crypt_cbc(mbedtls_aes_context *c, int mode, size_t len,
                                        unsigned char iv[16], const unsigned char *in,
                                        unsigned char *out)
{
    (void)mode;
    for (size_t off = 0; off + 16u <= len; off += 16u)
    {
        unsigned char blk[16];
        for (int i = 0; i < 16; i++) { blk[i] = (unsigned char)(iv[i] ^ in[off + i]); }
        if (aes_encrypt(c->ctx, blk, iv) != 0) { return -1; }
        if (out) { memcpy(out + off, iv, 16); }
    }
    return 0;
}
/* Counter mode, mbedtls semantics: nc_off/stream_block carry partial state. */
static inline int mbedtls_aes_crypt_ctr(mbedtls_aes_context *c, size_t len, size_t *nc_off,
                                        unsigned char nonce_counter[16],
                                        unsigned char stream_block[16],
                                        const unsigned char *in, unsigned char *out)
{
    size_t n = *nc_off;
    for (size_t i = 0; i < len; i++)
    {
        if (n == 0)
        {
            if (aes_encrypt(c->ctx, nonce_counter, stream_block) != 0) { return -1; }
            for (int j = 15; j >= 0; j--) { if (++nonce_counter[j] != 0) { break; } }
        }
        out[i] = (unsigned char)(in[i] ^ stream_block[n]);
        n = (n + 1u) & 0x0fu;
    }
    *nc_off = n;
    return 0;
}
#endif
