/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * BIP-CMAC-128 (IEEE 802.11-2020 12.5.4) and its MMIE. Group path selection does not use
 * it: mac80211 protects group Mesh Action frames with its MGTK (group-addressed privacy) and
 * drops one carrying an MMIE from an MFP peer, so the datapath only parses one, to refuse it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** MMIE: element id, length, Key ID (2), IPN (6), MIC (8). Always the last element. */
#define UMAC_MESH_MMIE_EID 76u
#define UMAC_MESH_MMIE_LEN 18u
/** The MMIE of BIP-CMAC-256 and BIP-GMAC: the same, with a 16-octet MIC. */
#define UMAC_MESH_MMIE16_LEN 26u
/** The only key ids an IGTK may carry. */
#define UMAC_MESH_IGTK_ID_MIN 4u
#define UMAC_MESH_IGTK_ID_MAX 5u
/** The PV0 management header BIP authenticates: FC, duration, A1, A2, A3, SC. */
#define UMAC_MESH_BIP_HDR_LEN 24u

/** AES-128-CMAC (RFC 4493) of @p msg. Returns 0, or negative on a cipher failure. */
int umac_mesh_aes_cmac128(const uint8_t key[16], const uint8_t *msg, size_t len, uint8_t mac[16]);

/**
 * Write the MMIE for @p key_id / @p ipn at @p body + @p body_len and fill its MIC.
 * @p hdr is the frame's 24-byte header; @p body is everything after it.
 * Returns the new body length, or 0 if it does not fit in @p cap.
 */
size_t umac_mesh_bip_protect(const uint8_t key[16], uint16_t key_id, uint64_t ipn,
                             const uint8_t hdr[UMAC_MESH_BIP_HDR_LEN], uint8_t *body,
                             size_t body_len, size_t cap);

/** Key ID and IPN of the MMIE that ends @p body; false when it does not end in one. */
bool umac_mesh_bip_parse(const uint8_t *body, size_t len, uint16_t *key_id, uint64_t *ipn);

/** True when @p body ends in an MMIE of either length, as mac80211's
 *  ieee80211_get_mmie_keyidx finds one. */
bool umac_mesh_bip_has_mmie(const uint8_t *body, size_t len);

/** True when the MMIE ending @p body carries a MIC that verifies under @p key.
 *  The IPN replay check is the caller's, and only after this returns true. */
bool umac_mesh_bip_verify(const uint8_t key[16], const uint8_t hdr[UMAC_MESH_BIP_HDR_LEN],
                          const uint8_t *body, size_t len);
