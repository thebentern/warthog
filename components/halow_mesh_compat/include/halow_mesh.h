/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * halow_mesh — public API exposed by halow_mesh_compat.
 *
 * The compat layer drives the MM6108 chip via the same chip-command
 * sequence the Linux GPL morse_driver uses for mesh, plus the dot11ah
 * S1G frame transform. It depends on a mesh-capable MMFW firmware
 * (Morse Micro to provide).
 *
 * This header is the integration surface for umac_mesh.c — when the port
 * is complete, umac_mesh_enable_mesh() will route via these functions
 * instead of the direct mmdrv_* calls in its current path.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the mesh compat layer. Allocates internal state. Returns
 * 0 on success, negative on error. Idempotent.
 */
int halow_mesh_init(void);

/**
 * Bring up the chip-side mesh interface. Sends the full Linux mesh
 * command sequence (ADD_INTERFACE → GET_CAPABILITIES → SET_CHANNEL →
 * QoS×4 → BSS_BEACON_CONFIG → MESH_CONFIG → BSSID_SET → BSS_CONFIG),
 * installs the dot11ah RX/TX transform hooks, and starts the chip's
 * beacon timer.
 *
 * @param mesh_id        UTF-8 mesh network identifier (1..32 bytes)
 * @param mesh_id_len    length of mesh_id
 * @param channel        S1G channel number (e.g. 1 for US 1MHz)
 * @param bw_mhz         operating bandwidth (1, 2, 4, 8 MHz)
 * @return 0 on success, negative on error
 */
int halow_mesh_enable(const uint8_t *mesh_id, size_t mesh_id_len,
                      uint8_t channel, uint8_t bw_mhz);

/**
 * Tear down the mesh interface: stop beaconing, remove the chip VIF,
 * release internal state.
 */
int halow_mesh_disable(void);

/**
 * Number of peers currently in ESTAB state.
 */
uint8_t halow_mesh_get_peer_count(void);

/**
 * Submit a broadcast probe request via the chip's mgmt TX path.
 * Used by the host-side periodic probe loop ("mesh_beaconless mode").
 * @return 0 on success, negative on error.
 */
int halow_mesh_tx_broadcast_probe(void);

/**
 * Hook called by the morselib chip→host RX shim for frames tagged
 * with the mesh VIF id. Runs the dot11ah S1G → 11n transform, parses
 * the IEs, and routes to mesh_mpm for PLINK / HWMP handling.
 *
 * Returns 0 if the frame was consumed, negative if it should fall
 * through to the regular umac RX path.
 */
int halow_mesh_rx_frame(const uint8_t *frame, size_t frame_len,
                        int8_t rssi_dbm, uint16_t freq_100khz);

#ifdef __cplusplus
}
#endif
