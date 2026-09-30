/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * The 802.11s peer-link table: one MPM entry per neighbour.
 *
 * Split out of umac_mesh.c and kept freestanding -- <stdint.h>, <stdbool.h>
 * and <string.h> only -- so the host tests in components/halow_mesh_compat/test
 * exercise the real shipping code rather than a copy of it. Three of the bugs
 * that broke a three-board mesh lived in exactly this logic (one global link-id
 * pair, a peer that never expired, a slot reused for a rejoining node), and
 * none of them needed hardware to find once the table was testable.
 *
 * Deliberately holds no policy: no timers, no radio, no key handling, no
 * knowledge of the datapath. Expiry reports which addresses went away and lets
 * the caller tear them down, which is what keeps this file dependency-free.
 *
 * Keep it freestanding. `make -C components/halow_mesh_compat/test freestanding`
 * fails the build if an SDK include creeps back in.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** MAC address length, mirrored so this file needs no SDK header. */
#define MPM_ADDR_LEN 6

/**
 * Peers we can hold links to at once.
 *
 * Must not exceed UMAC_DATAPATH_MESH_MAX_PEERS -- every established link
 * allocates a station there; umac_mesh.c static-asserts it. An Open with no
 * room, even after mpm_table_make_room(), is answered Close(MESH_MAX_PEERS)
 * and counted in @c no_slot.
 */
#ifndef MPM_MAX_LINKS
#define MPM_MAX_LINKS 4
#endif

/** One peering FSM: 802.11s runs one of these per neighbour, each with its
 *  own link-id pair. A single shared pair only ever describes two nodes. */
struct mpm_link {
    uint8_t addr[MPM_ADDR_LEN];
    uint16_t llid;          /**< ours; the peer echoes it back in its Confirm */
    uint16_t plid;          /**< theirs, from the Open/Confirm we received */
    uint32_t last_heard_ms; /**< refreshed by any frame from this peer */
    uint16_t opens;         /**< Opens sent since this link last changed state */
    uint32_t reannounce_ms; /**< last "we accept you" sent while full; 0 = never */
    bool used;
    bool estab;
};

/** Neighbours we answer without a link. With every entry live, a newcomer
 *  waits for one to lapse: none is evicted. */
#ifndef MPM_MAX_QUIET
#define MPM_MAX_QUIET 8
#endif

/** SAE neighbours held off after a failed handshake. More than the peer table
 *  holds, so a table's worth of failures is remembered with room to spare. */
#ifndef MPM_MAX_SAE_HELD
#define MPM_MAX_SAE_HELD 8
#endif

/** A per-address time stamp: a Close(MESH_MAX_PEERS) refusal, or a beacon
 *  answered from a neighbour we hold no link to. */
struct mpm_stamp {
    uint8_t addr[MPM_ADDR_LEN];
    uint32_t since_ms;
    bool used;
};

struct mpm_table {
    struct mpm_link links[MPM_MAX_LINKS];
    struct mpm_stamp refused[MPM_MAX_LINKS];
    struct mpm_stamp refused_over; /**< last refusal no entry could hold; addr unused */
    struct mpm_stamp quiet[MPM_MAX_QUIET];
    struct mpm_stamp sae_held[MPM_MAX_SAE_HELD];
    uint32_t no_slot; /**< Opens refused because the table was full */
    uint32_t expired; /**< links dropped for inactivity */
};

/** Clear the table. Safe to call repeatedly. */
void mpm_table_init(struct mpm_table *t);

/** Find the link for @p addr, or NULL. */
struct mpm_link *mpm_table_find(struct mpm_table *t, const uint8_t *addr);

/**
 * Find the link for @p addr, creating it if there is room.
 *
 * A new entry takes @p llid and is stamped with @p now_ms. An EXISTING entry
 * keeps the llid it already had: re-minting mid-handshake is what makes a peer
 * answer CNF_IGNR, so the caller may pass a fresh llid on every call without
 * disturbing a link in progress.
 *
 * @returns the link, or NULL when the table is full (and bumps @c no_slot).
 */
struct mpm_link *mpm_table_get_or_create(struct mpm_table *t, const uint8_t *addr, uint16_t llid,
                                         uint32_t now_ms);

/** Drop a link. @p l may be NULL. */
void mpm_table_release(struct mpm_table *t, struct mpm_link *l);

/**
 * Make room for an Open from @p addr, which holds no link: with none free, release
 * the unanswered link (not ESTAB, plid 0) that sent the most Opens, copying its
 * address and llid out for the caller's Close (either output may be NULL).
 * @returns true if a link was released.
 */
bool mpm_table_make_room(struct mpm_table *t, const uint8_t *addr,
                         uint8_t out_addr[MPM_ADDR_LEN], uint16_t *out_llid);

/** Number of links that have reached ESTAB. */
uint8_t mpm_table_estab_count(const struct mpm_table *t);

/**
 * Drop links unheard from for longer than @p timeout_ms.
 *
 * Nothing else notices a peer going away: MPM Close only arrives if the peer
 * leaves politely, and a board that loses power never sends one. Entries with
 * @c last_heard_ms of 0 are never expired (not yet stamped).
 *
 * Expired addresses are copied into @p out_addrs so the caller can tear down
 * the matching datapath peers; that indirection is what keeps this file free
 * of SDK dependencies. Entries beyond @p max_out are still expired, just not
 * reported -- size @p out_addrs at MPM_MAX_LINKS to see them all.
 *
 * @returns how many addresses were written to @p out_addrs.
 */
int mpm_table_expire(struct mpm_table *t, uint32_t now_ms, uint32_t timeout_ms,
                     uint8_t out_addrs[][MPM_ADDR_LEN], int max_out);

/**
 * Hold off opening toward @p addr for @p holdoff_ms from @p now_ms: it answered
 * our Open with Close(MESH_MAX_PEERS). Nothing is evicted: with every entry
 * live for another address, every address without an entry is held off instead.
 */
void mpm_table_refuse(struct mpm_table *t, const uint8_t *addr, uint32_t now_ms,
                      uint32_t holdoff_ms);

/** Drop @p addr's hold-off: its own Open shows it has room again. */
void mpm_table_unrefuse(struct mpm_table *t, const uint8_t *addr);

/**
 * Is @p addr held off at @p now_ms? By its own entry, or for any address while
 * every entry is live or within @p holdoff_ms of a refusal none could hold.
 * Unsigned elapsed time, so a wrapped clock is safe; lapsed entries are freed.
 */
bool mpm_table_refused(struct mpm_table *t, const uint8_t *addr, uint32_t now_ms,
                       uint32_t holdoff_ms);

/**
 * May a beacon from @p addr, a neighbour we hold no link to, be answered (and
 * restamped) at @p now_ms? Only if unanswered for @p period_ms and an entry is
 * free: none is evicted, so at most MPM_MAX_QUIET answers fall in any period.
 */
bool mpm_table_quiet_due(struct mpm_table *t, const uint8_t *addr, uint32_t now_ms,
                         uint32_t period_ms);

/**
 * Hold @p addr off for @p hold_ms from @p now_ms: SAE with it failed. With every
 * entry live for another address, the oldest is replaced: holding off everyone
 * instead would keep a working neighbour out.
 */
void mpm_table_sae_hold(struct mpm_table *t, const uint8_t *addr, uint32_t now_ms,
                        uint32_t hold_ms);

/** Is @p addr held off at @p now_ms? Unsigned elapsed time; lapsed entries are freed. */
bool mpm_table_sae_held(struct mpm_table *t, const uint8_t *addr, uint32_t now_ms,
                        uint32_t hold_ms);

/** Drop @p addr's SAE hold-off: its own Commit shows it knows us and has room. */
void mpm_table_sae_release(struct mpm_table *t, const uint8_t *addr);

/**
 * Render the table as "aabbcc llid=N plid=N estab=N; " per link, for
 * AT+MPMPEERS?. Always NUL-terminates; writes "(none) " when empty.
 */
void mpm_table_render(const struct mpm_table *t, char *out, uint32_t out_len);

#ifdef __cplusplus
}
#endif
