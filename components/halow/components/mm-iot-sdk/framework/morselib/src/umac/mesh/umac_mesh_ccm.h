/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * AES-CCM on mbedtls bulk primitives. See umac_mesh_ccm.c for why this exists
 * alongside hostap's aes-ccm.c (same algorithm, ~15x cheaper on this part).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * Encrypt @p data in place and produce the @p M-byte MIC.
 *
 * @p nonce is 13 bytes (L=2). @p aad is authenticated, not encrypted, and may
 * be at most 30 bytes -- the largest CCMP presents, a 4-address QoS header.
 * @p auth must have room for @p M bytes.
 *
 * @returns 0 on success, negative on bad arguments or a cipher failure.
 */
int warthog_ccm_ae(const uint8_t *key, const uint8_t *nonce, size_t M, const uint8_t *aad,
                   size_t aad_len, uint8_t *data, size_t data_len, uint8_t *auth);

/**
 * Decrypt @p data in place and verify @p auth.
 *
 * On failure @p data has ALREADY been overwritten with the (garbage)
 * decryption -- CCM decrypts before it can authenticate. The caller must
 * discard the frame, never forward it.
 *
 * @returns 0 if the MIC verified, negative otherwise.
 */
int warthog_ccm_ad(const uint8_t *key, const uint8_t *nonce, size_t M, const uint8_t *aad,
                   size_t aad_len, uint8_t *data, size_t data_len, const uint8_t *auth);
