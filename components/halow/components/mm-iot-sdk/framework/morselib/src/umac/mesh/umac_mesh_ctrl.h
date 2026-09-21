/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * 802.11s Mesh Control field (IEEE 802.11-2020 s9.2.4.7.3), the layout
 * mac80211 emits as struct ieee80211s_hdr: flags, TTL, LE32 sequence number,
 * then 0, 6 or 12 octets of Address Extension. It sits between QoS Control
 * and the body -- inside the encrypted region -- and is what a relay reads to
 * decide TTL, duplicate suppression and proxied endpoints.
 *
 * Freestanding (libc only) so the host tests and the mesh simulator drive the
 * shipping code.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Address Extension modes, flags bits 0-1. 3 is reserved and refused. */
#define UMAC_MESH_CTRL_AE_NONE 0u
/** One extension address: the proxied SA of a group-addressed frame. */
#define UMAC_MESH_CTRL_AE_A4   1u
/** Two extension addresses: proxied DA then proxied SA, individually addressed. */
#define UMAC_MESH_CTRL_AE_A5A6 2u
#define UMAC_MESH_CTRL_AE_MASK 0x03u

/** Mesh Power Save Level and RSPI, flags bits 2 and 3. Carried, not acted on. */
#define UMAC_MESH_CTRL_FLAG_PS_LEVEL 0x04u
#define UMAC_MESH_CTRL_FLAG_RSPI     0x08u

#define UMAC_MESH_CTRL_LEN_MIN 6u
#define UMAC_MESH_CTRL_LEN_MAX 18u

/** mac80211's dot11MeshTTL default; it refuses to forward at ttl <= 1. */
#define UMAC_MESH_CTRL_TTL_DEFAULT 31u

struct umac_mesh_ctrl {
    uint8_t  flags;      /* whole octet; ae is derived from it */
    uint8_t  ttl;
    uint32_t seq;
    /* eaddr1 is the only address for AE 1 (proxied SA); for AE 2 it is the
     * proxied DA and eaddr2 the proxied SA -- mac80211's naming. */
    uint8_t  eaddr1[6];
    uint8_t  eaddr2[6];
};

/** Address Extension mode of @p mc (flags & mask). */
static inline uint8_t umac_mesh_ctrl_ae(const struct umac_mesh_ctrl *mc)
{
    return (uint8_t)(mc->flags & UMAC_MESH_CTRL_AE_MASK);
}

/** Encoded length for an AE mode: 6, 12 or 18. 0 for the reserved mode. */
uint16_t umac_mesh_ctrl_len(uint8_t ae);

/**
 * Serialise @p mc. Only the addresses the AE mode calls for are written.
 * @returns bytes written, or 0 if @p out is too small or the mode is reserved.
 */
uint16_t umac_mesh_ctrl_build(uint8_t *out, uint16_t out_len, const struct umac_mesh_ctrl *mc);

/**
 * Parse a Mesh Control field from the start of @p in. Every length is
 * attacker-controlled: refuses a buffer shorter than the mode requires, and
 * refuses the reserved AE mode rather than guessing a length for it.
 * @returns true with @p out filled and @p consumed set to the field length.
 */
bool umac_mesh_ctrl_parse(const uint8_t *in, uint16_t in_len, struct umac_mesh_ctrl *out,
                          uint16_t *consumed);

#ifdef __cplusplus
}
#endif
