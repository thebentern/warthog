/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * Internal umac mesh-mode interface. Mirrors umac/ap/umac_ap.h. Compiled into
 * morselib (added to morselib/CMakeLists.txt SRCS) so it can reach umac
 * internals; symbols here are mangled by librarymangler.py and only callable
 * from within morselib.
 */

#pragma once

#include "mmwlan.h"
#include "mmwlan_internal.h"
#include "mmwlan_mesh.h"
#include "mmpkt.h"
#include "mmdrv.h"

/* struct umac_data is the umac core state object, fully defined in
 * umac/data/umac_data.h. Forward-declare here so the prototypes below all
 * refer to the same (file-scope) incomplete type rather than each minting a
 * prototype-scoped one; callers that need the full definition include
 * umac/data/umac_data.h themselves. */
struct umac_data;

/**
 * Validate mmwlan_mesh_args. Logs the specific failure. Does not touch the
 * chip or umac state.
 *
 * @return true if args are usable.
 */
bool umac_mesh_validate_args(struct umac_data *umacd, const struct mmwlan_mesh_args *args);

/**
 * Bring the mesh interface up: ADD_INTERFACE(type=MESH), BSS config, then
 * MESH_CONFIG(START) to begin beaconing.
 *
 * Skeleton: currently logs and returns MMWLAN_UNAVAILABLE. The chip command
 * sequence is the next implementation milestone (Phase 2-3 of the scope doc).
 */
enum mmwlan_status umac_mesh_enable_mesh(struct umac_data *umacd,
                                         const struct mmwlan_mesh_args *args);

/**
 * Tear the mesh interface down: MESH_CONFIG(STOP), REMOVE_INTERFACE.
 */
enum mmwlan_status umac_mesh_disable_mesh(struct umac_data *umacd);

/** Count of peers in the ESTABLISHED state. */
uint8_t umac_mesh_get_peer_count(struct umac_data *umacd);

/**
 * Phase 4d — retrieve the mesh args saved by the last successful
 * umac_mesh_enable_mesh() call. Used by wpa_config_read_mesh() to seed the
 * wpa_ssid (mesh_id, security_type) when the supplicant interface comes up.
 * Returns NULL if mesh isn't currently enabled.
 *
 * Mirrors umac_ap_get_args() but stored file-static in umac_mesh.c since the
 * MM6108 only supports one VIF — there's never more than one active mesh
 * args instance.
 */
const struct mmwlan_mesh_args *umac_mesh_get_args(void);

/**
 * Phase 4f-step32 — return the shared mesh BSSID derived from the mesh_id
 * CRC32 hash. NULL if mesh is not active. Used by the beacon constructor
 * to stamp Addr3 with the same value as the chip's RX filter, so peer
 * chips configured with the same shared BSSID will admit our beacon.
 */
const uint8_t *umac_mesh_get_shared_bssid(void);
