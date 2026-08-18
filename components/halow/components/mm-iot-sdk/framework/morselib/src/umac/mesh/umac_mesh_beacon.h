/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * mesh beacon constructor.
 *
 * The MM6108 chip's mesh mode asks the host for a beacon template via
 * mmdrv_host_get_beacon() exactly once during MESH_CONFIG(START) processing
 * (verified on hardware 2026-05-25). If we return NULL the chip falls back to
 * an internally-generated beacon that lacks the Mesh ID IE — which is why
 * peer discovery fails between two correctly-configured mesh STAs.
 *
 * This module provides the template the chip needs:
 *   - umac_mesh_beacon_init()        — stash mesh args + own_addr once at startup
 *   - umac_mesh_get_beacon()         — build and return an mmpkt with the beacon
 *
 * The chip only requests the beacon once for mesh, so we build it fresh each
 * call but the returned mmpkt is suitable for the chip to retransmit at the
 * configured beacon interval until mesh is disabled or the chip resets.
 */

#pragma once

#include "common/common.h"
#include "umac/data/umac_data.h"
#include "mmpkt.h"
#include "mmwlan_mesh.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the mesh beacon constructor.
 *
 * Called from umac_mesh_enable_mesh() after the chip has accepted the mesh VIF
 * and we've fetched our MAC address. Stashes the args + own_addr so the beacon
 * builder can read them on every call.
 *
 * @param args      mesh args (mesh_id + length, security_type, beacon_interval)
 * @param own_addr  the 6-byte MAC the chip uses for this mesh VIF
 */
void umac_mesh_beacon_init(const struct mmwlan_mesh_args *args, const uint8_t own_addr[6]);

/**
 * Build a complete 802.11 management beacon frame for the mesh BSS.
 *
 * Returns an mmpkt suitable for handoff to the chip via the polled-callback
 * beacon path (see mmdrv_host_get_beacon -> umac_mmdrv_shim.c). Caller takes
 * ownership of the mmpkt.
 *
 * Frame layout (per IEEE 802.11-2020 §9.4):
 *   MAC header (24 B): broadcast addr1, own_addr addr2/3 (= BSSID)
 *   Fixed fields (12 B): timestamp=0, beacon_interval, capability_info
 *   IEs:
 *     SSID (0)              — empty (mesh STAs identify by Mesh ID IE)
 *     Mesh ID (114)         — the mesh_id bytes
 *     Mesh Configuration (113, 7 B) — path/metric/sync/auth ids + caps
 *     S1G Capabilities (217)
 *     S1G Operation (232)
 *
 * @return  newly-allocated mmpkt, or NULL on alloc failure
 */
struct mmpkt *umac_mesh_get_beacon(struct umac_data *umacd);

/**
 * Returns true if the mesh beacon module has been initialized — i.e. mesh
 * is currently up and the chip should be served mesh beacons instead of the
 * (asserting) AP-mode get_beacon path.
 */
bool umac_mesh_beacon_is_active(void);

/**
 * Serialize the Mesh ID (114) and Mesh Configuration (113) IEs into a flat
 * buffer, using the same values the beacon advertises. Used to build mesh
 * probe responses, which take a pre-built IE blob rather than a consbuf.
 *
 * SSID is not included -- frame_probe_response_build takes it separately and
 * mesh STAs advertise a zero-length SSID.
 *
 * @param out     destination buffer
 * @param out_len size of @p out
 * @returns bytes written, or 0 if mesh is inactive or @p out is too small.
 */
uint16_t umac_mesh_build_discovery_ies(uint8_t *out, uint16_t out_len);

/** Worst-case size of umac_mesh_build_discovery_ies() output:
 *  Supported Rates (2+8) + Mesh ID (2+MMWLAN_MESH_ID_MAXLEN) + Mesh Config (2+7).
 *  Size every caller's buffer from this -- an under-sized buffer makes the
 *  builder return 0, which silently kills the only working discovery path. */
#define UMAC_MESH_DISCOVERY_IES_MAXLEN (2 + 8 + 2 + MMWLAN_MESH_ID_MAXLEN + 2 + 7)

/** Worst-case Mesh Peering action-frame body: category, action, capability(2),
 *  AID(2), the discovery IEs, then the Peer Management IE (2 + 6). */
#define UMAC_MESH_MPM_BODY_MAXLEN (6 + UMAC_MESH_DISCOVERY_IES_MAXLEN + 8)

#ifdef __cplusplus
}
#endif
