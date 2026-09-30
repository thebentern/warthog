/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * Mesh path table and proxy (MPP) table -- what a relay consults to pick a
 * next hop, and what it learns from PREQ, PREP, PERR and Address Extension.
 *
 * The update rules are vanilla mac80211's hwmp_route_info_get(): a path is
 * replaced by a newer HWMP sequence number, or by the same sequence number
 * with a strictly better metric -- and switching to a DIFFERENT next hop on
 * equal sequence numbers needs a metric at least 10 % better, so two
 * equal-cost routes do not flap. HWMP frames are unauthenticated, so these
 * rules are also what stops a forged PREQ from steering traffic with a stale
 * number. OpenMANET's 999-0027 differs: a different next hop must be 10 %
 * better even with a newer number (the PREQ/PREP is then neither answered nor
 * forwarded), and the same next hop is taken even with a worse metric.
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
#define UMAC_MESH_PATH_MAX 32u
#endif
#ifndef UMAC_MESH_PROXY_MAX
#define UMAC_MESH_PROXY_MAX 32u
#endif
/** Ceiling on a path's lifetime. Below it the PREQ/PREP Lifetime field sets it,
 *  as in mac80211; OpenMANET 1.8.0 advertises 48828 TU (50 s). */
#define UMAC_MESH_PATH_LIFETIME_MAX_MS 60000u
/** A path is freed this long after its expiry, as mac80211's mesh_path_expire();
 *  well under 2^31 ms, past which an old expiry reads as the future. */
#define UMAC_MESH_PATH_EXPIRE_MS 600000u
/** mac80211 MESH_PATH_EXPIRE: 600 s, refreshed on every transmit and receive. */
#define UMAC_MESH_PROXY_LIFETIME_MS 600000u
/** At most this many hosts learned behind one mesh node: one peer's flood
 *  cannot unlearn everyone else's hosts. */
#define UMAC_MESH_PROXY_PER_NODE 8u
/** How long the relay that carried a host's unicast to us outranks a flood's
 *  transmitter: 5000 TU, just over vanilla mac80211's dot11MeshHWMPactivePathTimeout
 *  (5000 ms), the longest that relay's path to the host's node outlives its last
 *  frame. OpenMANET's is 50000 ms, and it extends 1-hop paths of metric <= 253
 *  on use. */
#define UMAC_MESH_LEAF_VIA_HOLD_MS 5120u

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
    uint8_t  via[6];      /* leaf: the peer its traffic last arrived through */
    uint32_t exp_ms;
    uint32_t uni_ms;      /* leaf: last unicast to us from this host; the sweep
                           * keeps it at most UMAC_MESH_LEAF_VIA_HOLD_MS behind now */
    bool     used;
    bool     leaf;        /* learned in leaf mode; via is meaningful */
    bool     uni;         /* leaf: seen in a unicast to us; never evicted */
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
 *          false when the table already held a live path as good or better, in
 *          which case nothing changed -- including expiry, so a repeated stale
 *          advertisement cannot keep a dead path alive -- or when @p dst is new
 *          and every slot holds a live path. umac_mesh_hwmp_relay tells the two
 *          apart by lookup.
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

/** Free every path, active or dead, and every proxy entry whose expiry is
 *  UMAC_MESH_PATH_EXPIRE_MS past, and pull each kept entry's uni_ms up to at
 *  most UMAC_MESH_LEAF_VIA_HOLD_MS behind @p now_ms. Run it periodically: a slot
 *  kept 2^31 ms past its expiry reads as unexpired again, and a unicast time
 *  left 2^32 ms reads as fresh. @returns how many were freed. */
uint32_t umac_mesh_path_expire(struct umac_mesh_pathtbl *t, uint32_t now_ms);

/** Learn that @p ext sits behind @p mesh_sta (from Address Extension). A live
 *  entry is never evicted for a newcomer, and a node may own at most
 *  UMAC_MESH_PROXY_PER_NODE; a host may move to another node. @returns false
 *  if refused. */
bool umac_mesh_proxy_learn(struct umac_mesh_pathtbl *t, const uint8_t *ext,
                           const uint8_t *mesh_sta, uint32_t now_ms);

/**
 * Leaf mode: learn that @p ext sits behind @p mesh_sta, reached through peer
 * @p via. @p uni: the evidence was a unicast addressed to us. A leaf entry
 * learned only from group frames may be evicted, oldest first, for a
 * newcomer (within @p mesh_sta's own entries once it is at its bound); one
 * with unicast evidence, and every relay-learned entry, never is. A host
 * re-learned behind another node loses its unicast evidence.
 */
bool umac_mesh_proxy_learn_leaf(struct umac_mesh_pathtbl *t, const uint8_t *ext,
                                const uint8_t *mesh_sta, const uint8_t *via, bool uni,
                                uint32_t now_ms);

/** The live entry for @p ext, or NULL. */
const struct umac_mesh_proxy *umac_mesh_proxy_entry(const struct umac_mesh_pathtbl *t,
                                                    const uint8_t *ext, uint32_t now_ms);

/** Refresh @p ext's entry on transmit use, as mac80211 does. */
void umac_mesh_proxy_touch(struct umac_mesh_pathtbl *t, const uint8_t *ext, uint32_t now_ms);

/** The mesh node proxying for @p ext, or NULL. */
const uint8_t *umac_mesh_proxy_lookup(const struct umac_mesh_pathtbl *t, const uint8_t *ext,
                                      uint32_t now_ms);

/** Live counts, for AT reporting. */
uint32_t umac_mesh_path_count(const struct umac_mesh_pathtbl *t, uint32_t now_ms);
uint32_t umac_mesh_proxy_count(const struct umac_mesh_pathtbl *t, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
