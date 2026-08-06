/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * Public API for 802.11s mesh mode on the MM6108. Warthog fork addition —
 * the upstream morsemicro/halow 2.10.4-esp32-1 component exposes STA + AP
 * only (enum mmwlan_vif has no MESH member). This header + the umac/mesh/
 * sources add a mesh STA mode.
 *
 * Licensed GPL-3.0 to match the morselib sources it links against; the
 * upstream component's Apache-2.0 covers only its CMake/wrapper glue.
 *
 * Status: skeleton. mmwlan_mesh_enable() validates args and returns
 * MMWLAN_UNAVAILABLE until the umac/mesh state machine + chip MESH_CONFIG
 * command path are implemented. See docs/mesh-port-scope.md.
 */

#pragma once

#include "mmwlan.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MMWLAN_MESH_ID_MAXLEN 32

/** Peer link state per IEEE 802.11-2020 section 14.3 (mesh peering / PLINK). */
enum mmwlan_mesh_peer_state {
    MMWLAN_MESH_PEER_IDLE = 0,
    MMWLAN_MESH_PEER_OPN_SNT,
    MMWLAN_MESH_PEER_OPN_RCVD,
    MMWLAN_MESH_PEER_CNF_RCVD,
    MMWLAN_MESH_PEER_ESTAB,
    MMWLAN_MESH_PEER_HOLDING,
};

/** Peer status delivered through the peer-event callback. */
struct mmwlan_mesh_peer_status {
    uint8_t addr[MMWLAN_MAC_ADDR_LEN];
    enum mmwlan_mesh_peer_state state;
    int8_t rssi;
};

/** Peer-event callback. Fires on every PLINK state transition. */
typedef void (*mmwlan_mesh_peer_event_cb_t)(const struct mmwlan_mesh_peer_status *peer,
                                            void *arg);

/**
 * Arguments to mmwlan_mesh_enable(). Initialize with MMWLAN_MESH_ARGS_INIT to
 * pick up sensible defaults and forward-compat zeroing.
 */
struct mmwlan_mesh_args {
    /** Mesh ID. Required. */
    uint8_t mesh_id[MMWLAN_MESH_ID_MAXLEN];
    /** Length of mesh_id, no null terminator. 1..MMWLAN_MESH_ID_MAXLEN. */
    uint8_t mesh_id_len;

    /** MMWLAN_OPEN (open mesh) or MMWLAN_SAE (mesh SAE). */
    enum mmwlan_security_type security_type;

    /** SAE passphrase. Required when security_type == MMWLAN_SAE. */
    uint8_t passphrase[64];
    uint8_t passphrase_len;

    /** Max simultaneous peers. 0 selects the library default. */
    uint8_t max_peers;

    /** Beacon interval in TUs (1 TU = 1024 us). 0 selects the default (100). */
    uint16_t beacon_interval_tu;
};

/** Default initializer for mmwlan_mesh_args. */
#define MMWLAN_MESH_ARGS_INIT { {0}, 0, MMWLAN_OPEN, {0}, 0, 0, 0 }

/**
 * Bring up a mesh STA. The chip starts beaconing on the channel configured by
 * mmwlan_set_channel_list(), advertising the mesh ID. Discovered peers trigger
 * automatic PLINK peering.
 *
 * @param args  Mesh configuration. mesh_id / mesh_id_len required.
 * @return MMWLAN_SUCCESS on success, else an error code. Currently always
 *         MMWLAN_UNAVAILABLE — implementation in progress.
 */
enum mmwlan_status mmwlan_mesh_enable(const struct mmwlan_mesh_args *args);

/** Tear down the mesh STA: all peers to HOLDING, stop beaconing. */
enum mmwlan_status mmwlan_mesh_disable(void);

/** Register (or NULL to clear) the peer state-change callback. */
enum mmwlan_status mmwlan_mesh_register_peer_event_cb(mmwlan_mesh_peer_event_cb_t cb, void *arg);

/** Count of peers currently in MMWLAN_MESH_PEER_ESTAB. */
uint8_t mmwlan_mesh_get_peer_count(void);

/**
 * Phase 4f-step30 diagnostic — fire a broadcast probe request via the chip's
 * mgmt TX path on the active mesh VIF. The probe carries the mesh_id as SSID
 * (mimicking Linux's `mesh_beaconless_mode` discovery frames). Caller invokes
 * periodically (e.g. every 2s) to maintain on-air mesh-discovery traffic
 * regardless of whether the chip's internal beacon timer fires for mesh.
 *
 * Returns 0 on success, negative on error.
 */
int mmwlan_mesh_tx_broadcast_probe(void);

/**
 * Phase 4f-step33 diagnostic — probe a single chip opcode. Sends a minimal
 * REQ with the given message_id (empty payload, vif_id = active mesh VIF)
 * via mmdrv_execute_command, bypassing the morselib opcode wrappers.
 *
 * Used to enumerate the chip's command surface and find undocumented
 * opcodes that might affect mesh RX behavior. Returns 0 if chip accepts,
 * negative chip return code otherwise.
 */
int mmwlan_mesh_probe_opcode(uint16_t opcode);

#ifdef __cplusplus
}
#endif
