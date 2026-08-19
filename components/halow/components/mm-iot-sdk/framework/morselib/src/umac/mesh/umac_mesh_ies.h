/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * Pure 802.11s mesh byte layout: the discovery IE blob, Mesh Peering (MPM)
 * action-frame bodies, and the Peer Management IE parsers.
 *
 * Deliberately free of every SDK dependency -- <stdint.h>, <stdbool.h> and
 * <string.h> only. That is what lets the host tests in
 * components/halow_mesh_compat/test/ link the REAL shipping code instead of a
 * reimplementation of it. These bytes were derived from hardware (a mac80211
 * peer accepted the resulting Open), so a silent regression here breaks peering
 * with no visible symptom; the tests pin them against a captured frame.
 *
 * Keep this file freestanding. `make -C components/halow_mesh_compat/test
 * freestanding` fails the build if an SDK include creeps back in.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Longest Mesh ID we build for; matches MMWLAN_MESH_ID_MAXLEN. */
#define UMAC_MESH_IES_MESH_ID_MAXLEN 32

/* Element IDs (IEEE 802.11-2020). Mirrored from dot11.h, which is not
 * includable here; umac_mesh_beacon.c static-asserts they agree. */
#define UMAC_MESH_EID_SUPPORTED_RATES 1
#define UMAC_MESH_EID_MESH_CONFIG 113
#define UMAC_MESH_EID_MESH_ID 114
#define UMAC_MESH_EID_PEER_MGMT 117

/* Mesh Peering action frames: category 15 (SELF_PROTECTED) + action code. */
#define UMAC_MESH_MPM_CATEGORY 15
#define UMAC_MESH_MPM_ACTION_OPEN 1
#define UMAC_MESH_MPM_ACTION_CONFIRM 2
#define UMAC_MESH_MPM_ACTION_CLOSE 3

/** Mesh Configuration IE payload length (IEEE 802.11-2020 s9.4.2.97). */
#define UMAC_MESH_CFG_IE_LEN 7

/**
 * Worst-case discovery blob: Supported Rates (2+8) + Mesh ID (2+32)
 * + Mesh Configuration (2+7). Size every caller's buffer from this -- an
 * undersized buffer makes the builder return 0, which silently kills the only
 * working discovery path.
 */
#define UMAC_MESH_DISCOVERY_IES_MAXLEN (2 + 8 + 2 + UMAC_MESH_IES_MESH_ID_MAXLEN + 2 + UMAC_MESH_CFG_IE_LEN)

/**
 * Worst-case MPM body: category, action, capability(2), AID(2), the discovery
 * IEs, then the Peer Management IE (2 + 6).
 */
#define UMAC_MESH_MPM_BODY_MAXLEN (6 + UMAC_MESH_DISCOVERY_IES_MAXLEN + 8)

/**
 * Build the IEs a peer needs to accept us as a mesh candidate: Supported Rates,
 * Mesh ID, Mesh Configuration -- in that order.
 *
 * All three are load-bearing. mac80211's mesh_matches_local() compares the Mesh
 * ID, the Mesh Configuration fields AND the basic rate set; omitting Supported
 * Rates parses as basic_rates == 0 and the candidate is refused.
 *
 * SSID is not included: callers that need one pass it separately, and mesh STAs
 * advertise a zero-length SSID.
 *
 * @returns bytes written, or 0 if the arguments are invalid or @p out is too
 *          small. On failure @p out is left untouched.
 */
uint16_t umac_mesh_ies_build_discovery(uint8_t *out, uint16_t out_len, const uint8_t *mesh_id,
                                       uint8_t mesh_id_len, bool sae);

/**
 * Build the Mesh Configuration IE alone (element ID + length + 7 payload
 * octets), so the beacon path and the discovery blob cannot drift apart.
 */
/** The rate set we advertise. S1G beacons carry no Supported Rates element,
 *  but hostap's MPM refuses a peer without one; this is what we substitute. */
const uint8_t *umac_mesh_ies_supported_rates(uint8_t *len_out);

uint16_t umac_mesh_ies_build_mesh_config(uint8_t *out, uint16_t out_len, bool sae);

/**
 * Build a Mesh Peering action-frame body (everything after the 802.11 header).
 *
 * Link-id perspective: @p llid is OUR id, @p plid is our view of the peer's id
 * (the value the peer sent us as ITS llid). OPEN carries only llid and omits
 * the AID field; CONFIRM carries both plus AID; CLOSE carries both plus a
 * reason code and, unlike the other two, no capability or AID fields at all.
 *
 * @p reason is used only by CLOSE (802.11 reason codes 52-60; note Linux's
 * numbering: 52 MESH-PEER-CANCELED, 53 MESH-MAX-PEERS, 56 MESH-MAX-RETRIES,
 * 57 MESH-CONFIRM-TIMEOUT); pass 0 otherwise.
 *
 * @p aid is used only by CONFIRM and MUST be non-zero -- AID 0 is reserved for
 * the group key, and a peer assigning per-link AIDs will refuse it.
 *
 * @p extra_ies (may be NULL) is copied in ahead of the discovery elements, for
 * elements this freestanding file cannot build -- chiefly S1G Capabilities,
 * which a Morse peer requires on peering frames.
 *
 * @returns bytes written, or 0 on invalid action / bad arguments / small buffer.
 */
uint16_t umac_mesh_ies_build_mpm_body(uint8_t *out, uint16_t out_len, uint8_t action,
                                      uint16_t llid, uint16_t plid, uint16_t reason, uint16_t aid,
                                      const uint8_t *mesh_id, uint8_t mesh_id_len, bool sae,
                                      const uint8_t *extra_ies, uint16_t extra_ies_len);

/** 802.11 reason code: no room for another peering. */
#define UMAC_MESH_REASON_MAX_PEERS 52

/**
 * Read the sender's link id (first Peer Management IE link-id field).
 *
 * From the receiver's perspective this is OUR plid -- the frame's llid is the
 * peer's id. Getting this backwards produces a Confirm the peer answers with
 * CNF_IGNR, which is silent.
 */
bool umac_mesh_ies_get_peer_llid(const uint8_t *body, uint32_t len, uint16_t *out_llid);

/**
 * Read the second link-id field of a CONFIRM: the sender's view of OUR llid.
 * Used to decide the link is established.
 */
bool umac_mesh_ies_get_peer_plid(const uint8_t *body, uint32_t len, uint16_t *out_plid);

/**
 * Read the reason code from a CLOSE. mac80211 states why it refused a peering
 * here (802.11 reason codes 52-60), which is far more informative than guessing
 * at which field it disliked.
 */
bool umac_mesh_ies_get_close_reason(const uint8_t *body, uint32_t len, uint16_t *out_reason);

/**
 * Does this beacon/probe-response body advertise the given Mesh ID?
 *
 * @p body starts at the fixed fields (timestamp 8, beacon interval 2,
 * capability 2), so IEs begin at offset 12; a body shorter than that has no
 * IEs and returns false. Scans for the Mesh ID element (114) and compares.
 *
 * Needed to act on a peer's BEACON. warthog discovers only by answering probe
 * requests, which works between warthog boards because they all probe every
 * 2 s -- but a mac80211/OpenMANET mesh node beacons (beacon_int 1000 TU) and
 * does not necessarily probe, so without this it is never discovered at all.
 *
 * Zero-length or NULL mesh_id returns false: a wildcard match would make us
 * answer every mesh in range.
 */
bool umac_mesh_ies_beacon_has_mesh_id(const uint8_t *body, uint32_t len, const uint8_t *mesh_id,
                                      uint8_t mesh_id_len);

/* ---- 4-address mesh data header ----------------------------------------- */

/** Length of an 802.11 data MAC header carrying all four addresses, no QoS. */
#define UMAC_MESH_DATA_HDR4_LEN 30

/**
 * Write the 30-byte MAC header of an 802.11s mesh data frame (IEEE 802.11-2020
 * s9.3.2.1, table 9-30): FC with ToDS+FromDS, type DATA, subtype QoS Data;
 * duration 0; addr1=RA (next hop), addr2=TA (us), addr3=DA, addr4=SA; sequence
 * control 0 (the datapath stamps the real one).
 *
 * For a single hop RA==DA and TA==SA, but all four are still written -- that is
 * what lets the receiver's dot11_get_da()/dot11_get_sa_data() recover the 802.3
 * endpoints, and what a forwarding node needs to relay without losing them. No
 * Mesh Control field: that is required only for multi-hop and would need the RX
 * side to strip it.
 *
 * @returns UMAC_MESH_DATA_HDR4_LEN, or 0 if any pointer is NULL.
 */
uint16_t umac_mesh_ies_build_data_hdr4(uint8_t out[UMAC_MESH_DATA_HDR4_LEN], const uint8_t *ra,
                                       const uint8_t *ta, const uint8_t *da, const uint8_t *sa);

/** Length of a 3-address 802.11 data MAC header (no addr4). */
#define UMAC_MESH_DATA_HDR3_LEN 24

/**
 * Write the 24-byte header of a GROUP-addressed 802.11s mesh data frame:
 * FC = QoS Data, FromDS only; addr1 = the group DA (broadcast/multicast),
 * addr2 = TA (us), addr3 = SA. This is how mac80211 and the reference port
 * send mesh broadcast/multicast, and it is REQUIRED on this chip: the MM6108
 * firmware filters 4-address data on addr3 == own MAC, so a group DA tunneled
 * as a 4-address unicast (addr3 = 01:00:5e:..) is ACKed and then discarded.
 * Measured: unicast ICMP delivered, multicast to 239.0.0.69 ACKed but never
 * reached the peer's host, until this shape was used.
 *
 * @returns UMAC_MESH_DATA_HDR3_LEN, or 0 if any pointer is NULL.
 */
uint16_t umac_mesh_ies_build_data_hdr3_group(uint8_t out[UMAC_MESH_DATA_HDR3_LEN],
                                             const uint8_t *da, const uint8_t *ta,
                                             const uint8_t *sa);

#ifdef __cplusplus
}
#endif

/* ---- S1G Beacon (802.11ah) ---------------------------------------------- */

/**
 * Offset of the first information element in an S1G Beacon.
 *
 * S1G Beacons use a COMPRESSED header, not the ordinary management layout:
 * frame control (2), duration (2), SA (6), timestamp (4), change sequence (1).
 * There is no addr1 and no addr3, so the usual dot11_get_ta() -- which reads
 * offset 10 -- lands on the timestamp and returns a different "address" every
 * beacon. Captured from a mac80211/OpenMANET peer on 923 MHz:
 *
 *   1c00 0000 e45f 0128 bf74 4863 fb12 00 ...
 *    FC   dur  \___ SA = e4:5f:01:28:bf:74 ___/ timestamp
 *
 * The optional Next TBTT / Compressed SSID / ANO fields are announced by bits
 * in the frame control's upper octet; this offset assumes none are present,
 * which is what the peer sends (upper octet 0x00). Verified against a real
 * capture in test_s1g_beacon.c.
 */
/** Where the elements start in an S1G Beacon when NO optional field is present.
 *  Three optional fields may sit between the fixed header and the elements,
 *  each announced by a frame-control bit -- use
 *  umac_mesh_ies_s1g_beacon_ie_offset() rather than this constant. */
#define UMAC_MESH_S1G_BEACON_IE_OFFSET 15u

/** Frame-control bits announcing the optional S1G Beacon fields, and their
 *  sizes. Mirrors morse_dot11_find_s1g_beacon_ies() in the ported driver
 *  (components/halow_mesh_compat/src/dot11ah/ie.c). */
#define UMAC_MESH_S1G_FC_NEXT_TBTT 0x0100u
#define UMAC_MESH_S1G_FC_COMPRESS_SSID 0x0200u
#define UMAC_MESH_S1G_FC_ANO 0x0400u
#define UMAC_MESH_S1G_NEXT_TBTT_SIZE 3u
#define UMAC_MESH_S1G_COMPRESS_SSID_SIZE 4u
#define UMAC_MESH_S1G_ANO_SIZE 1u

/**
 * Offset of the first element in an S1G Beacon, given its frame control.
 *
 * The compressed header is not a fixed size: Next TBTT (3), Compressed SSID
 * (4) and ANO (1) each appear only when their frame-control bit is set, in
 * that order. Assuming the shortest form is not a parse error -- it walks into
 * the middle of a field and simply fails to find anything, so a beaconing peer
 * is never discovered and no counter or log says why.
 */
uint32_t umac_mesh_ies_s1g_beacon_ie_offset(uint16_t frame_control);
/** Offset of the source address in an S1G Beacon. */
#define UMAC_MESH_S1G_BEACON_SA_OFFSET 4u
/** Frame control type/subtype identifying an S1G Beacon: type 3, subtype 1. */
#define UMAC_MESH_S1G_BEACON_TYPE 3u
#define UMAC_MESH_S1G_BEACON_SUBTYPE 1u

/** Is @p frame an S1G Beacon (type 3 / subtype 1)? @p len must cover the FC. */
bool umac_mesh_ies_is_s1g_beacon(const uint8_t *frame, uint32_t len);

/**
 * Read the source address of an S1G Beacon into @p sa.
 *
 * @returns false if the frame is too short or not an S1G Beacon.
 */
bool umac_mesh_ies_s1g_beacon_sa(const uint8_t *frame, uint32_t len, uint8_t sa[6]);

/**
 * Does this S1G Beacon advertise @p mesh_id?
 *
 * Same element walk as umac_mesh_ies_beacon_has_mesh_id(), but from the S1G
 * beacon's own IE offset. Every length octet comes off the air, so each step is
 * bounds-checked.
 */
bool umac_mesh_ies_s1g_beacon_has_mesh_id(const uint8_t *frame, uint32_t len,
                                          const uint8_t *mesh_id, uint8_t mesh_id_len);
