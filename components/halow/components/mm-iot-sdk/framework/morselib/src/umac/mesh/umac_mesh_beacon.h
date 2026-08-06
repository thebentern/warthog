/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * Phase 4f-step13 — mesh beacon constructor.
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

#ifdef __cplusplus
}
#endif
