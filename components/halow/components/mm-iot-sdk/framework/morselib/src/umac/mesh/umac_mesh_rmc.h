/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * Recent Multicast Cache (IEEE 802.11-2020 s10.35.4 / mac80211 mesh_rmc).
 *
 * A relay that rebroadcasts group frames must remember what it has already
 * seen, keyed on the mesh source and the Mesh Control sequence number, or two
 * relays in range of each other rebroadcast every frame to each other until
 * TTL runs out. This is the memory. It is bounded and static, and it is what
 * makes forwarding safe to turn on.
 *
 * Freestanding (libc only) so the host tests and the simulator drive it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** mac80211 uses 256 buckets; a HaLow mesh has a handful of sources. */
#ifndef UMAC_MESH_RMC_BUCKETS
#define UMAC_MESH_RMC_BUCKETS 64u
#endif
/** Entries per bucket before the oldest is evicted (mac80211: 4). */
#ifndef UMAC_MESH_RMC_QUEUE
#define UMAC_MESH_RMC_QUEUE 4u
#endif
/** How long a (source, seq) pair is remembered (mac80211: 30 s). */
#ifndef UMAC_MESH_RMC_TIMEOUT_MS
#define UMAC_MESH_RMC_TIMEOUT_MS 30000u
#endif

struct umac_mesh_rmc_entry {
    uint32_t seq;
    uint32_t exp_ms;   /* absolute, wrap-safe against now_ms */
    uint8_t  sa[6];
    bool     used;
};

struct umac_mesh_rmc {
    struct umac_mesh_rmc_entry e[UMAC_MESH_RMC_BUCKETS][UMAC_MESH_RMC_QUEUE];
};

void umac_mesh_rmc_init(struct umac_mesh_rmc *rmc);

/**
 * Record (@p sa, @p seq) and say whether it was already there.
 *
 * @returns true when this is a DUPLICATE -- the caller must drop it. False
 *          means it is new and has now been remembered until the timeout.
 *          NULL arguments read as "not a duplicate" so a broken caller fails
 *          open to delivery rather than silently eating traffic.
 */
bool umac_mesh_rmc_check(struct umac_mesh_rmc *rmc, const uint8_t *sa, uint32_t seq,
                         uint32_t now_ms);

/** Live (unexpired) entries, for AT reporting. */
uint32_t umac_mesh_rmc_count(const struct umac_mesh_rmc *rmc, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
