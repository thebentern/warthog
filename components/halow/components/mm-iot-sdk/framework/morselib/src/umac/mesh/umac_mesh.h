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

/** True when the mesh runs SAE/AMPE, so hostap's MPM owns peering. */
/** TX a fully built mgmt frame (hostap's SAE auth) with mesh VIF metadata. */
int umac_mesh_tx_mgmt_mmpkt(struct mmpkt *frm);

/** Register a datapath peer for hostap's mesh MPM (the .sta_add path). */
enum mmwlan_status umac_mesh_add_datapath_peer(const uint8_t *peer_addr);

bool umac_mesh_sae_active(void);

/** Our mesh address, from mesh start (zero before). */
const uint8_t *umac_mesh_own_addr(void);

/** Offer a SAE candidate heard at @p rssi to hostap; a new peer is skipped at or
 *  below the RSSI floor, while held off after a failed SAE, or when the table is full. */
void umac_mesh_offer_sae_candidate(const uint8_t *addr, const uint8_t *ies, size_t ies_len,
                                   int16_t rssi);

/** hostap's SAE with @p addr failed: free its datapath slot and hold it off. */
void umac_mesh_sae_failed(const uint8_t *addr);

/** hostap's peering with @p addr restarted before ESTAB: free its slot and hold it off. */
void umac_mesh_plink_failed(const uint8_t *addr);

/** An Authentication frame from @p ta (@p body after the MAC header): a held-off
 *  neighbour's SAE Commit ends its hold-off and is offered before hostap sees it. */
void umac_mesh_note_sae_auth(const uint8_t *ta, const uint8_t *body, uint32_t len, int16_t rssi);

/** Under SAE, refuse a Mesh Peering Open (@p body from its category octet) from @p ta,
 *  which holds no slot, heard at @p rssi at or below the floor or while @p ta is held off. */
bool umac_mesh_sae_open_refused(const uint8_t *ta, const uint8_t *body, uint32_t len, int16_t rssi);

/** At or below the candidate RSSI floor (AT+MESHRSSI)? Always false with it off. */
bool umac_mesh_rssi_below_floor(int16_t rssi);

/** Open mesh, table full: is @p sa an established peer due a probe response
 *  telling it we accept it? At most once per MPM_FULL_REANNOUNCE_MS per link. */
bool umac_mesh_reannounce_due(const uint8_t *sa);

/** Open mesh: may the beacon of @p sa, a neighbour we will not open toward, be
 *  answered? At most once per MPM_FULL_REANNOUNCE_MS per address, and to at
 *  most MPM_MAX_QUIET addresses in any such period. */
bool umac_mesh_unopenable_answer_due(const uint8_t *sa);

/** Forget every peer link; the next beacon re-runs the peering. */
void umac_mesh_reset_links(void);

/** Send an HWMP PREQ to @p da, targeting @p da itself. Emitting this is what
 *  makes us reachable: a peer installs a path to a PREQ's originator. */
int umac_mesh_hwmp_send_preq(const uint8_t *da);

/** Handle a received mesh action frame (category 13). Answers a PREQ that
 *  targets us with a PREP. */
void umac_mesh_handle_hwmp(const uint8_t *body, uint16_t len, const uint8_t *ta,
                           bool is_protected, bool is_group_addressed);

/**
 * Transmit an action frame to a mesh peer, appending S1G Capabilities.
 *
 * The Morse driver on the far side validates that element on peering frames,
 * so hostap's MPM output cannot go on air without it. Used by the supplicant
 * shim's .send_action driver op.
 *
 * @returns 0 once handed to the radio; 1 when group path selection sent off the
 *          umac event loop was queued for it instead; or a negative error.
 */
int umac_mesh_tx_action(const uint8_t *da, const uint8_t *body, uint16_t body_len);

/**
 * Bring the mesh interface up: ADD_INTERFACE(type=MESH), BSS config, then
 * MESH_CONFIG(START) to begin beaconing.
 *
 * Brings up the interface, starts beaconing, and registers the wpa_supplicant
 * mesh interface so peering runs against this VIF.
 */
enum mmwlan_status umac_mesh_enable_mesh(struct umac_data *umacd,
                                         const struct mmwlan_mesh_args *args);

/**
 * Tear the mesh interface down: MESH_CONFIG(STOP), REMOVE_INTERFACE.
 */
enum mmwlan_status umac_mesh_disable_mesh(struct umac_data *umacd);

/** Count of peers in the ESTABLISHED state. */
uint8_t umac_mesh_get_peer_count(struct umac_data *umacd);

/** mmwlan_mesh_query_peer_links(): up to @p max peers into @p out, taken on the umac event
 *  loop (directly when already on it). @p count gets how many on MMWLAN_SUCCESS; on any
 *  other status no snapshot was taken, @p count is 0 and @p out is untouched. */
enum mmwlan_status umac_mesh_peer_links_snapshot(struct umac_data *umacd,
                                                 struct mmwlan_mesh_peer_link *out, uint8_t max,
                                                 uint8_t *count);


/**
 * retrieve the mesh args saved by the last successful
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
 * return the shared mesh BSSID derived from the mesh_id
 * CRC32 hash. NULL if mesh is not active. Used by the beacon constructor
 * to stamp Addr3 with the same value as the chip's RX filter, so peer
 * chips configured with the same shared BSSID will admit our beacon.
 */
const uint8_t *umac_mesh_get_shared_bssid(void);
