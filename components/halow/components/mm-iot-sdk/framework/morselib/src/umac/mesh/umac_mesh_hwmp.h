/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * HWMP path selection elements: PREQ and PREP.
 *
 * A mac80211 mesh will not transmit a unicast data frame to a neighbour it has
 * no PATH to, and it never creates one just because a peer link reached ESTAB
 * -- MESH_PATH_ACTIVE is only ever set by hwmp_route_info_get() or
 * mesh_path_fix_nexthop(). So an 802.11s peer that answers no PREQ is
 * reachable for broadcast (which skips path resolution entirely) and
 * unreachable for everything else. That is exactly how it presented against
 * OpenMANET: peering ESTAB both ways, broadcast ARP arriving, and every ping
 * dying in the peer's discovery retries. Forcing mesh_nolearn=1 on that peer
 * made the same link run at 0% loss, which is what pins the diagnosis.
 *
 * Emitting a PREQ is enough to become reachable: hwmp_route_info_get()
 * installs a path to the ORIGINATOR of any PREQ it accepts. Answering one with
 * a PREP is what makes us reachable on demand. Both are here.
 *
 * Kept freestanding (libc only) so the host tests drive the real shipping code.
 * `make -C components/halow_mesh_compat/test freestanding` fails the build if
 * an SDK include creeps in.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HWMP_ADDR_LEN 6

/** Mesh action category and the HWMP path-selection action code. */
#define HWMP_CATEGORY_MESH 13
#define HWMP_ACTION_PATH_SELECTION 1

/** Element IDs. 130/131 -- NOT 113/114, which are Mesh Config and Mesh ID. */
#define HWMP_EID_PREQ 130
#define HWMP_EID_PREP 131

/** Element length octets. mac80211 has historically required these EXACTLY
 *  (preq_len != 37 / prep_len != 31 -> drop), so always emit them. */
#define HWMP_PREQ_ELEM_LEN 37
#define HWMP_PREP_ELEM_LEN 31

/** Full action-body sizes, from the category octet. */
#define HWMP_PREQ_BODY_LEN 41
#define HWMP_PREP_BODY_LEN 35

/** Address Extension. We never set it and we reject it on receive: an AE PREQ
 *  carries a sixth address and a different element length. */
#define HWMP_FLAG_AE 0x40

/** Per-target flags: Target Only, and Unknown Target Sequence Number. These
 *  live in their own octet, NOT in the element's top-level flags. */
#define HWMP_TGT_FLAG_TO 0x01
#define HWMP_TGT_FLAG_USN 0x04

/** Default element TTL. mac80211 refuses to forward at ttl <= 1. */
#define HWMP_DEFAULT_TTL 31

/** A parsed PREQ. Only the single-target, non-AE shape is representable --
 *  every other shape is refused rather than modelled. */
struct hwmp_preq {
    uint8_t flags;
    uint8_t hop_count;
    uint8_t ttl;
    uint32_t preq_id;
    uint8_t orig_addr[HWMP_ADDR_LEN];
    uint32_t orig_sn;
    uint32_t lifetime;
    uint32_t metric;
    uint8_t target_count;
    uint8_t target_flags;
    uint8_t target_addr[HWMP_ADDR_LEN];
    uint32_t target_sn;
};

/**
 * HWMP sequence-number comparison, modulo 2^32.
 *
 * Wrap-safe: 0 is one PAST 0xffffffff, not far behind it. Getting this
 * backwards makes a node reject every fresh path after the first wrap, so both
 * directions are pinned in the host tests.
 *
 * @returns true when @p a is strictly newer than @p b.
 */
bool hwmp_sn_gt(uint32_t a, uint32_t b);

/**
 * Build a self-originated PREQ action body.
 *
 * @param out          buffer for the body, from the category octet.
 * @param out_len      its size; must be at least HWMP_PREQ_BODY_LEN.
 * @param orig_addr    our own mesh address.
 * @param orig_sn      our HWMP sequence number, already incremented.
 * @param preq_id      our path-discovery id, already incremented.
 * @param target_addr  who we want a path to. Never the broadcast address:
 *                     that selects the proactive-root branch on the peer.
 * @param lifetime_tu  path lifetime in TUs.
 * @returns bytes written, or 0 if the arguments or the buffer are unusable.
 */
uint16_t umac_mesh_hwmp_build_preq(uint8_t *out, uint16_t out_len, const uint8_t *orig_addr,
                                   uint32_t orig_sn, uint32_t preq_id, const uint8_t *target_addr,
                                   uint32_t lifetime_tu);

/**
 * Build a PREP answering @p preq.
 *
 * NAMING TRAP, and it is the one that bites: in a PREP the "target" is the
 * node that ANSWERS (us) and the "originator" is the node that ASKED. That is
 * inverted from a PREQ. Swapping them yields a frame the peer accepts and
 * installs a path from, pointing at the wrong node.
 *
 * @param own_addr  our mesh address -- the PREP's TARGET.
 * @param own_sn    our HWMP sequence number.
 * @returns bytes written, or 0 if the arguments or the buffer are unusable.
 */
uint16_t umac_mesh_hwmp_build_prep(uint8_t *out, uint16_t out_len, const struct hwmp_preq *preq,
                                   const uint8_t *own_addr, uint32_t own_sn);

/**
 * Parse a PREQ action body.
 *
 * Refuses anything it cannot represent rather than guessing: wrong category or
 * action code, wrong element id, AE set, target_count != 1, or a body too
 * short for the fields it must read.
 *
 * @returns true on success, with @p out filled.
 */
bool umac_mesh_hwmp_parse_preq(const uint8_t *body, uint16_t len, struct hwmp_preq *out);

/**
 * Our next HWMP sequence number, given a PREQ we are about to answer.
 *
 * A PREQ carries the originator's idea of OUR sequence number, but only when
 * it has one: the Unknown Target Sequence Number flag says the field is
 * meaningless and must be ignored. warthog itself always sets USN and sends
 * target_sn 0, and so does mac80211 for a target it has never reached.
 *
 * Adopting the field regardless is not merely untidy. HWMP action frames are
 * unauthenticated, so any node in range could send a PREQ naming our address
 * with a sequence number far ahead of ours; a few of those push us past the
 * comparison's half-range and every peer then reads our replies as stale,
 * discards them, and never installs a path to us. Broadcast keeps working, so
 * the link reads as healthy while nothing unicast arrives -- and no counter on
 * either side records it.
 *
 * Pure so the policy can be tested without a radio.
 *
 * @returns the sequence number to use for our reply.
 */
uint32_t umac_mesh_hwmp_next_own_sn(uint32_t cur, const struct hwmp_preq *preq);

/** True when @p preq asks for a path to @p own_addr, i.e. we must answer. */
bool umac_mesh_hwmp_targets_us(const struct hwmp_preq *preq, const uint8_t *own_addr);

#ifdef __cplusplus
}
#endif
