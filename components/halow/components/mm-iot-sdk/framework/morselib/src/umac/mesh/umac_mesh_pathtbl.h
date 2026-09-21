/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * Mesh path table and proxy (MPP) table -- what a relay consults to pick a
 * next hop, and what it learns from PREQ, PREP, PERR and Address Extension.
 *
 * The update rules are mac80211's hwmp_route_info_get(): a path is replaced
 * by a newer HWMP sequence number, or by the same sequence number with a
 * strictly better metric -- and switching to a DIFFERENT next hop on equal
 * sequence numbers needs a metric at least 10 % better, so two equal-cost
 * routes do not flap. HWMP frames are unauthenticated, so these rules are
 * also what stops a forged PREQ from steering traffic with a stale number.
 *
 * Static, bounded, freestanding (libc only).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef UMAC_MESH_PATH_MAX
#define UMAC_MESH_PATH_MAX 16u
#endif
#ifndef UMAC_MESH_PROXY_MAX
#define UMAC_MESH_PROXY_MAX 32u
#endif
/** mac80211 dot11MeshHWMPactivePathTimeout: 5000 TU, ~5.1 s. */
#define UMAC_MESH_PATH_LIFETIME_MS 5120u
/** mac80211 MESH_PATH_EXPIRE for proxy entries. */
#define UMAC_MESH_PROXY_LIFETIME_MS 60000u

#define UMAC_MESH_PATH_ACTIVE   0x01u
#define UMAC_MESH_PATH_SN_VALID 0x02u

struct umac_mesh_path {
    uint8_t  dst[6];
    uint8_t  next_hop[6];
    uint32_t sn;
    uint32_t metric;
    uint32_t exp_ms;
    uint8_t  hop_count;
    uint8_t  flags;
    bool     used;
};

struct umac_mesh_proxy {
    uint8_t  ext[6];      /* the host behind a mesh node */
    uint8_t  mesh_sta[6]; /* the mesh node proxying for it */
    uint32_t exp_ms;
    bool     used;
};

struct umac_mesh_pathtbl {
    struct umac_mesh_path  p[UMAC_MESH_PATH_MAX];
    struct umac_mesh_proxy x[UMAC_MESH_PROXY_MAX];
};

void umac_mesh_pathtbl_init(struct umac_mesh_pathtbl *t);

/** The active, unexpired path to @p dst, or NULL. */
const struct umac_mesh_path *umac_mesh_path_lookup(const struct umac_mesh_pathtbl *t,
                                                   const uint8_t *dst, uint32_t now_ms);

/**
 * Offer path information learned from a PREQ (dst = originator) or a PREP
 * (dst = target), reached via @p next_hop (the frame's transmitter).
 *
 * @returns true when the information was FRESH and the path now reflects it;
 *          false when the table already held something as good or better, in
 *          which case nothing changed -- including expiry, so a repeated stale
 *          advertisement cannot keep a dead path alive.
 */
bool umac_mesh_path_update(struct umac_mesh_pathtbl *t, const uint8_t *dst,
                           const uint8_t *next_hop, uint32_t sn, uint32_t metric,
                           uint8_t hop_count, uint32_t lifetime_ms, uint32_t now_ms);

/**
 * Apply a PERR naming @p dst with @p sn, received from @p from. Deactivates
 * the path only if it is active, @p from IS its next hop (mac80211: a node
 * not on our path cannot knock it out; NULL skips the check for a local
 * loss), and the PERR is not older than what we hold (sn newer, sn 0, or ours
 * has no valid sn). @returns true when a path was deactivated -- the signal
 * to forward the PERR onward; a PERR that changed nothing must not be
 * forwarded, or two relays echo it forever.
 */
bool umac_mesh_path_invalidate(struct umac_mesh_pathtbl *t, const uint8_t *dst,
                               uint32_t sn, const uint8_t *from, uint32_t now_ms);

/** Drop every active path whose next hop is @p next_hop (a peer went away).
 *  @returns how many were dropped. */
uint32_t umac_mesh_path_lose_next_hop(struct umac_mesh_pathtbl *t, const uint8_t *next_hop);

/** Learn that @p ext sits behind @p mesh_sta (from Address Extension). */
void umac_mesh_proxy_learn(struct umac_mesh_pathtbl *t, const uint8_t *ext,
                           const uint8_t *mesh_sta, uint32_t now_ms);

/** The mesh node proxying for @p ext, or NULL. */
const uint8_t *umac_mesh_proxy_lookup(const struct umac_mesh_pathtbl *t, const uint8_t *ext,
                                      uint32_t now_ms);

/** Live counts, for AT reporting. */
uint32_t umac_mesh_path_count(const struct umac_mesh_pathtbl *t, uint32_t now_ms);
uint32_t umac_mesh_proxy_count(const struct umac_mesh_pathtbl *t, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
