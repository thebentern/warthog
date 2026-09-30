/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * HWMP relay: what a node does with a received PREQ, PREP or PERR. The rules
 * are mac80211's hwmp_preq/prep/perr_frame_process(). This TU DECIDES and
 * fills in frames; it sends nothing, so the same code runs under the host
 * simulator and in the firmware.
 *
 * Duplicate suppression for PREQs falls out of the path table: the same
 * request arriving again (via another neighbour, or replayed) carries the same
 * originator sequence number and a metric no better, so the path update is
 * not fresh, and a PREQ that was not fresh is neither answered nor forwarded.
 * That is also why a forged PREQ with a stale number changes nothing.
 */
#pragma once

#include "umac_mesh_hwmp.h"
#include "umac_mesh_pathtbl.h"

#ifdef __cplusplus
extern "C" {
#endif

enum umac_mesh_hwmp_action_kind {
    UMAC_MESH_HWMP_NONE = 0,
    /** Unicast a PREP to @c to (the transmitter of the PREQ). */
    UMAC_MESH_HWMP_SEND_PREP,
    /** Broadcast the rebuilt PREQ. */
    UMAC_MESH_HWMP_REBROADCAST_PREQ,
    /** Unicast the rebuilt PREP to @c to (our next hop toward its originator). */
    UMAC_MESH_HWMP_FORWARD_PREP,
    /** Broadcast the rebuilt PERR. */
    UMAC_MESH_HWMP_FORWARD_PERR,
};

struct umac_mesh_hwmp_action {
    enum umac_mesh_hwmp_action_kind kind;
    uint8_t  to[6];
    uint8_t  body[HWMP_PREQ_BODY_LEN]; /* the largest of the three */
    uint16_t body_len;
};

/** Why nothing was done; for counters and the simulator's assertions. */
enum umac_mesh_hwmp_drop {
    UMAC_MESH_HWMP_DROP_NONE = 0,
    UMAC_MESH_HWMP_DROP_PARSE,
    UMAC_MESH_HWMP_DROP_OWN,        /* our own frame came back */
    UMAC_MESH_HWMP_DROP_NOT_FRESH,  /* duplicate or stale */
    UMAC_MESH_HWMP_DROP_TTL,
    UMAC_MESH_HWMP_DROP_NO_FWD,     /* forwarding disabled */
    UMAC_MESH_HWMP_DROP_NO_PATH,    /* PREP: no route back to its originator */
    UMAC_MESH_HWMP_DROP_UNCHANGED,  /* PERR: we held no such path */
    UMAC_MESH_HWMP_DROP_TABLE_FULL, /* no free path slot for a new destination */
};

struct umac_mesh_hwmp_ctx {
    const uint8_t *own_addr;
    struct umac_mesh_pathtbl *tbl;
    bool     forwarding;      /* AT+MESHFWD */
    uint32_t link_metric;     /* cost of the hop the frame arrived over */
    uint32_t max_lifetime_ms; /* ceiling on the element's Lifetime; <= UINT32_MAX / 1024 */
    uint32_t now_ms;
    /* Our HWMP sequence number; advanced by the PREP reply rule. */
    uint32_t *own_sn;
};

/**
 * Process one category-13 action body received from @p ta.
 *
 * @param out   receives up to one action to carry out. NONE when nothing is
 *              to be sent -- the path table may still have been updated.
 * @param why   receives the reason when @p out is NONE.
 * @returns the element id handled (PREQ/PREP/PERR), or 0 if unparseable.
 */
uint8_t umac_mesh_hwmp_relay(const struct umac_mesh_hwmp_ctx *c, const uint8_t *body,
                             uint16_t len, const uint8_t *ta,
                             struct umac_mesh_hwmp_action *out,
                             enum umac_mesh_hwmp_drop *why);

/**
 * A neighbour is gone: drop every path through it and, if forwarding, build
 * one PERR per lost destination (up to @p max) for broadcast.
 * @returns the number of PERRs written.
 */
uint32_t umac_mesh_hwmp_lose_neighbour(const struct umac_mesh_hwmp_ctx *c,
                                       const uint8_t *neighbour,
                                       struct umac_mesh_hwmp_action *out, uint32_t max);

#ifdef __cplusplus
}
#endif
