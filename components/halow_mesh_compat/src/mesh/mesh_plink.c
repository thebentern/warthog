/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 802.11s mesh peering (MPM) finite state machine — Warthog softmac port.
 *
 * Ported from the Linux kernel net/mac80211/mesh_plink.c (v6.6). The state
 * transitions are reproduced exactly; the only changes are mechanical:
 *   - sta->mesh->*            ->  peer->*
 *   - sdata->u.mesh.mshcfg.*  ->  peer->ctx->cfg.*
 *   - mesh_plink_frame_tx()   ->  ops->tx_frame()
 *   - mesh_plink_timer_set/mod_plink_timer/del_timer -> ops->set_timer/del_timer
 *   - mesh_get_new_llid()     ->  ops->new_llid()
 *   - the MBSS bookkeeping in establish/deactivate (estab count, HT prot, slot
 *     time, power-save) collapses to ops->on_established/on_deactivated plus a
 *     boolean "changed" return, since those touch mac80211 BSS state the
 *     warthog bridge owns rather than the FSM.
 *
 * Locking: the kernel takes sta->mesh->plink_lock around the switch. The port
 * runs the MPM on a single FreeRTOS task, so no lock is needed here.
 */
#include "mesh/mesh_plink.h"

#include <string.h>

static const char *const mplstates[] = {
    [MESH_PLINK_LISTEN]   = "LISTEN",
    [MESH_PLINK_OPN_SNT]  = "OPN-SNT",
    [MESH_PLINK_OPN_RCVD] = "OPN-RCVD",
    [MESH_PLINK_CNF_RCVD] = "CNF-RCVD",
    [MESH_PLINK_ESTAB]    = "ESTAB",
    [MESH_PLINK_HOLDING]  = "HOLDING",
    [MESH_PLINK_BLOCKED]  = "BLOCKED",
};

static const char *const mplevents[] = {
    [MPL_UNDEFINED] = "NONE",
    [MPL_OPN_ACPT]  = "OPN_ACPT",
    [MPL_OPN_RJCT]  = "OPN_RJCT",
    [MPL_OPN_IGNR]  = "OPN_IGNR",
    [MPL_CNF_ACPT]  = "CNF_ACPT",
    [MPL_CNF_RJCT]  = "CNF_RJCT",
    [MPL_CNF_IGNR]  = "CNF_IGNR",
    [MPL_CLS_ACPT]  = "CLS_ACPT",
    [MPL_CLS_IGNR]  = "CLS_IGNR",
};

const char *mesh_plink_state_name(enum mesh_plink_state s)
{
    return (s <= MESH_PLINK_BLOCKED) ? mplstates[s] : "?";
}
const char *mesh_plink_event_name(enum mesh_plink_event e)
{
    return (e <= MPL_CLS_IGNR) ? mplevents[e] : "?";
}

/* --- small helpers that route to the ops vtable --- */
static u16 plink_new_llid(struct mesh_plink_ctx *ctx)
{
    return ctx->ops && ctx->ops->new_llid ? ctx->ops->new_llid(ctx) : 0;
}
static void plink_tx(struct mesh_plink_ctx *ctx, struct mesh_peer *peer,
                     u8 action, u16 llid, u16 plid, u16 reason)
{
    if (ctx->ops && ctx->ops->tx_frame)
        ctx->ops->tx_frame(ctx, peer, action, llid, plid, reason);
}
static void plink_set_timer(struct mesh_plink_ctx *ctx, struct mesh_peer *peer,
                            u32 ms)
{
    if (ctx->ops && ctx->ops->set_timer)
        ctx->ops->set_timer(ctx, peer, ms);
}
static void plink_del_timer(struct mesh_plink_ctx *ctx, struct mesh_peer *peer)
{
    if (ctx->ops && ctx->ops->del_timer)
        ctx->ops->del_timer(ctx, peer);
}

void mesh_peer_init(struct mesh_peer *peer, struct mesh_plink_ctx *ctx,
                    const u8 *addr, bool authenticated)
{
    memset(peer, 0, sizeof(*peer));
    peer->ctx = ctx;
    peer->plink_state = MESH_PLINK_LISTEN;
    peer->authenticated = authenticated;
    if (addr)
        memcpy(peer->addr, addr, ETH_ALEN);
}

u16 mesh_plink_free_count(const struct mesh_plink_ctx *ctx)
{
    if (ctx->cfg.max_peer_links <= ctx->num_estab)
        return 0;
    return ctx->cfg.max_peer_links - ctx->num_estab;
}

/* mesh_plink_fsm_restart - reset a peer-link FSM back to LISTEN. */
static void mesh_plink_fsm_restart(struct mesh_peer *peer)
{
    peer->plink_state = MESH_PLINK_LISTEN;
    peer->llid = peer->plid = peer->reason = 0;
    peer->plink_retries = 0;
}

void mesh_plink_open(struct mesh_peer *peer)
{
    struct mesh_plink_ctx *ctx = peer->ctx;

    if (!peer->authenticated)
        return;
    peer->llid = plink_new_llid(ctx);
    if (peer->plink_state != MESH_PLINK_LISTEN &&
        peer->plink_state != MESH_PLINK_BLOCKED)
        return;
    peer->plink_state = MESH_PLINK_OPN_SNT;
    plink_set_timer(ctx, peer, ctx->cfg.retry_timeout_ms);
    plink_tx(ctx, peer, WLAN_SP_MESH_PEERING_OPEN, peer->llid, 0, 0);
}

void mesh_plink_block(struct mesh_peer *peer)
{
    struct mesh_plink_ctx *ctx = peer->ctx;

    if (peer->plink_state == MESH_PLINK_ESTAB && ctx->num_estab)
        ctx->num_estab--;
    peer->plink_state = MESH_PLINK_BLOCKED;
    if (ctx->ops && ctx->ops->on_deactivated)
        ctx->ops->on_deactivated(ctx, peer);
}

/* mesh_plink_close - move to HOLDING with the appropriate reason. */
static void mesh_plink_close(struct mesh_peer *peer, enum mesh_plink_event event)
{
    struct mesh_plink_ctx *ctx = peer->ctx;
    u16 reason = (event == MPL_CLS_ACPT) ? WLAN_REASON_MESH_CLOSE
                                         : WLAN_REASON_MESH_CONFIG;

    peer->reason = reason;
    peer->plink_state = MESH_PLINK_HOLDING;
    plink_set_timer(ctx, peer, ctx->cfg.holding_timeout_ms);
}

/* mesh_plink_establish - link reached ESTAB; do the MBSS bookkeeping. */
static bool mesh_plink_establish(struct mesh_peer *peer)
{
    struct mesh_plink_ctx *ctx = peer->ctx;

    plink_del_timer(ctx, peer);
    peer->plink_state = MESH_PLINK_ESTAB;
    ctx->num_estab++;
    if (ctx->ops && ctx->ops->on_established)
        ctx->ops->on_established(ctx, peer);
    return true; /* BSS state changed (estab count / HT prot / slot time) */
}

static bool mesh_plink_deactivate(struct mesh_peer *peer)
{
    struct mesh_plink_ctx *ctx = peer->ctx;
    bool changed = false;

    if (peer->plink_state == MESH_PLINK_ESTAB && ctx->num_estab) {
        ctx->num_estab--;
        changed = true;
    }
    if (ctx->ops && ctx->ops->on_deactivated)
        ctx->ops->on_deactivated(ctx, peer);
    return changed;
}

bool mesh_plink_fsm(struct mesh_peer *peer, enum mesh_plink_event event)
{
    struct mesh_plink_ctx *ctx = peer->ctx;
    u8 action = 0;
    bool changed = false;

    switch (peer->plink_state) {
    case MESH_PLINK_LISTEN:
        switch (event) {
        case MPL_CLS_ACPT:
            mesh_plink_fsm_restart(peer);
            break;
        case MPL_OPN_ACPT:
            peer->plink_state = MESH_PLINK_OPN_RCVD;
            peer->llid = plink_new_llid(ctx);
            plink_set_timer(ctx, peer, ctx->cfg.retry_timeout_ms);
            action = WLAN_SP_MESH_PEERING_OPEN;
            break;
        default:
            break;
        }
        break;
    case MESH_PLINK_OPN_SNT:
        switch (event) {
        case MPL_OPN_RJCT:
        case MPL_CNF_RJCT:
        case MPL_CLS_ACPT:
            mesh_plink_close(peer, event);
            action = WLAN_SP_MESH_PEERING_CLOSE;
            break;
        case MPL_OPN_ACPT:
            /* retry timer is left untouched */
            peer->plink_state = MESH_PLINK_OPN_RCVD;
            action = WLAN_SP_MESH_PEERING_CONFIRM;
            break;
        case MPL_CNF_ACPT:
            peer->plink_state = MESH_PLINK_CNF_RCVD;
            plink_set_timer(ctx, peer, ctx->cfg.confirm_timeout_ms);
            break;
        default:
            break;
        }
        break;
    case MESH_PLINK_OPN_RCVD:
        switch (event) {
        case MPL_OPN_RJCT:
        case MPL_CNF_RJCT:
        case MPL_CLS_ACPT:
            mesh_plink_close(peer, event);
            action = WLAN_SP_MESH_PEERING_CLOSE;
            break;
        case MPL_OPN_ACPT:
            action = WLAN_SP_MESH_PEERING_CONFIRM;
            break;
        case MPL_CNF_ACPT:
            changed |= mesh_plink_establish(peer);
            break;
        default:
            break;
        }
        break;
    case MESH_PLINK_CNF_RCVD:
        switch (event) {
        case MPL_OPN_RJCT:
        case MPL_CNF_RJCT:
        case MPL_CLS_ACPT:
            mesh_plink_close(peer, event);
            action = WLAN_SP_MESH_PEERING_CLOSE;
            break;
        case MPL_OPN_ACPT:
            changed |= mesh_plink_establish(peer);
            action = WLAN_SP_MESH_PEERING_CONFIRM;
            break;
        default:
            break;
        }
        break;
    case MESH_PLINK_ESTAB:
        switch (event) {
        case MPL_CLS_ACPT:
            changed |= mesh_plink_deactivate(peer);
            mesh_plink_close(peer, event);
            action = WLAN_SP_MESH_PEERING_CLOSE;
            break;
        case MPL_OPN_ACPT:
            action = WLAN_SP_MESH_PEERING_CONFIRM;
            break;
        default:
            break;
        }
        break;
    case MESH_PLINK_HOLDING:
        switch (event) {
        case MPL_CLS_ACPT:
            plink_del_timer(ctx, peer);
            mesh_plink_fsm_restart(peer);
            break;
        case MPL_OPN_ACPT:
        case MPL_CNF_ACPT:
        case MPL_OPN_RJCT:
        case MPL_CNF_RJCT:
            action = WLAN_SP_MESH_PEERING_CLOSE;
            break;
        default:
            break;
        }
        break;
    default:
        /* PLINK_BLOCKED is administrative; no event-driven transitions. */
        break;
    }

    if (action) {
        plink_tx(ctx, peer, action, peer->llid, peer->plid, peer->reason);
        /* also send confirm in the open case */
        if (action == WLAN_SP_MESH_PEERING_OPEN)
            plink_tx(ctx, peer, WLAN_SP_MESH_PEERING_CONFIRM,
                     peer->llid, peer->plid, 0);
    }
    return changed;
}

enum mesh_plink_event mesh_plink_get_event(struct mesh_plink_ctx *ctx,
                                           struct mesh_peer *peer, u8 ftype,
                                           bool matches_local, u8 ie_len,
                                           u16 llid, u16 plid)
{
    enum mesh_plink_event event = MPL_UNDEFINED;

    /* CLOSE always "matches"; others require a matching mesh profile. */
    if (ftype != WLAN_SP_MESH_PEERING_CLOSE && !matches_local) {
        /* deny open request from a non-matching peer */
        if (ftype == WLAN_SP_MESH_PEERING_OPEN)
            return MPL_OPN_RJCT;
    }

    if (!peer->authenticated)
        return MPL_UNDEFINED;
    if (peer->plink_state == MESH_PLINK_BLOCKED)
        return MPL_UNDEFINED;

    switch (ftype) {
    case WLAN_SP_MESH_PEERING_OPEN:
        if (!matches_local)
            event = MPL_OPN_RJCT;
        else if (!mesh_plink_free_count(ctx) ||
                 (peer->plid && peer->plid != plid))
            event = MPL_OPN_IGNR;
        else
            event = MPL_OPN_ACPT;
        break;
    case WLAN_SP_MESH_PEERING_CONFIRM:
        if (!matches_local)
            event = MPL_CNF_RJCT;
        else if (!mesh_plink_free_count(ctx) ||
                 peer->llid != llid ||
                 (peer->plid && peer->plid != plid))
            event = MPL_CNF_IGNR;
        else
            event = MPL_CNF_ACPT;
        break;
    case WLAN_SP_MESH_PEERING_CLOSE:
        if (peer->plink_state == MESH_PLINK_ESTAB)
            /* Don't check llid/plid: only one plink per peer is supported,
             * so accepting Close avoids a livelock when the peer restarts. */
            event = MPL_CLS_ACPT;
        else if (peer->plid != plid)
            event = MPL_CLS_IGNR;
        else if (ie_len == 8 && peer->llid != llid)
            event = MPL_CLS_IGNR;
        else
            event = MPL_CLS_ACPT;
        break;
    default:
        break;
    }
    return event;
}

void mesh_plink_rx(struct mesh_peer *peer, u8 ftype, bool matches_local,
                   u8 ie_len, u16 llid, u16 plid)
{
    enum mesh_plink_event event;

    /* learn the peer's link id from Open/Confirm before stepping the FSM */
    if ((ftype == WLAN_SP_MESH_PEERING_OPEN ||
         ftype == WLAN_SP_MESH_PEERING_CONFIRM) && !peer->plid)
        peer->plid = plid;

    event = mesh_plink_get_event(peer->ctx, peer, ftype, matches_local,
                                 ie_len, llid, plid);
    if (event != MPL_UNDEFINED)
        mesh_plink_fsm(peer, event);
}
