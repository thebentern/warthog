/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Unit test for the ported 802.11s mesh peering FSM (mesh/mesh_plink.c).
 * Drives a peer link through the full handshake (active + passive open ->
 * established) and teardown (close -> holding -> listen), and checks the
 * received-frame -> event mapping. Side effects are captured via a recording
 * ops vtable so transitions and emitted action frames can be asserted.
 */
#include "mesh/mesh_plink.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* --- recording ops --- */
struct rec_tx { u8 action; u16 llid, plid, reason; };
struct rec {
    int tx_n;
    struct rec_tx tx[16];
    int set_timer_n, del_timer_n, estab_n, deact_n;
    u32 last_timeout;
    u16 next_llid;
};

static u16 rec_new_llid(struct mesh_plink_ctx *ctx)
{
    struct rec *r = ctx->priv;
    return ++r->next_llid;            /* non-zero, monotonically increasing */
}
static void rec_tx_frame(struct mesh_plink_ctx *ctx, struct mesh_peer *peer,
                         u8 action, u16 llid, u16 plid, u16 reason)
{
    struct rec *r = ctx->priv;
    (void)peer;
    if (r->tx_n < 16)
        r->tx[r->tx_n] = (struct rec_tx){ action, llid, plid, reason };
    r->tx_n++;
}
static void rec_set_timer(struct mesh_plink_ctx *ctx, struct mesh_peer *peer,
                          u32 ms)
{
    struct rec *r = ctx->priv; (void)peer;
    r->set_timer_n++; r->last_timeout = ms;
}
static void rec_del_timer(struct mesh_plink_ctx *ctx, struct mesh_peer *peer)
{
    struct rec *r = ctx->priv; (void)peer; r->del_timer_n++;
}
static void rec_on_estab(struct mesh_plink_ctx *ctx, struct mesh_peer *peer)
{
    struct rec *r = ctx->priv; (void)peer; r->estab_n++;
}
static void rec_on_deact(struct mesh_plink_ctx *ctx, struct mesh_peer *peer)
{
    struct rec *r = ctx->priv; (void)peer; r->deact_n++;
}

static const struct mesh_plink_ops rec_ops = {
    .new_llid       = rec_new_llid,
    .tx_frame       = rec_tx_frame,
    .set_timer      = rec_set_timer,
    .del_timer      = rec_del_timer,
    .on_established = rec_on_estab,
    .on_deactivated = rec_on_deact,
};

static void ctx_init(struct mesh_plink_ctx *ctx, struct rec *r)
{
    memset(r, 0, sizeof(*r));
    memset(ctx, 0, sizeof(*ctx));
    ctx->ops = &rec_ops;
    ctx->priv = r;
    ctx->cfg.retry_timeout_ms   = 40;
    ctx->cfg.confirm_timeout_ms = 40;
    ctx->cfg.holding_timeout_ms = 40;
    ctx->cfg.max_peer_links     = 4;
}

static u8 last_action(const struct rec *r)
{
    return r->tx_n ? r->tx[r->tx_n - 1].action : 0;
}

int main(void)
{
    static const u8 peerB[ETH_ALEN] = { 0x02,0,0,0,0,0xBB };
    struct mesh_plink_ctx ctx;
    struct rec r;
    struct mesh_peer p;

    /* ============ Test 1: active open -> ESTABLISHED ============ */
    ctx_init(&ctx, &r);
    mesh_peer_init(&p, &ctx, peerB, /*authenticated=*/true);
    CHECK(p.plink_state == MESH_PLINK_LISTEN, "fresh peer is LISTEN");

    mesh_plink_open(&p);
    CHECK(p.plink_state == MESH_PLINK_OPN_SNT, "open: LISTEN -> OPN_SNT");
    CHECK(p.llid != 0, "open: local llid assigned (%u)", p.llid);
    CHECK(r.tx_n == 1 && r.tx[0].action == WLAN_SP_MESH_PEERING_OPEN,
          "open: sent a single PEERING_OPEN");
    CHECK(r.set_timer_n == 1 && r.last_timeout == ctx.cfg.retry_timeout_ms,
          "open: armed retry timer");

    /* peer's OPEN arrives -> we confirm */
    mesh_plink_fsm(&p, MPL_OPN_ACPT);
    CHECK(p.plink_state == MESH_PLINK_OPN_RCVD, "OPN_SNT + OPN_ACPT -> OPN_RCVD");
    CHECK(last_action(&r) == WLAN_SP_MESH_PEERING_CONFIRM,
          "OPN_SNT + OPN_ACPT emits CONFIRM");

    /* peer's CONFIRM arrives -> established */
    bool changed = mesh_plink_fsm(&p, MPL_CNF_ACPT);
    CHECK(p.plink_state == MESH_PLINK_ESTAB, "OPN_RCVD + CNF_ACPT -> ESTAB");
    CHECK(changed, "establishment reports MBSS changed");
    CHECK(ctx.num_estab == 1, "established peer count == 1");
    CHECK(r.estab_n == 1, "on_established fired once");
    CHECK(r.del_timer_n >= 1, "establish cancelled the peering timer");

    /* ============ Test 2: close from ESTABLISHED ============ */
    int tx_before = r.tx_n;
    changed = mesh_plink_fsm(&p, MPL_CLS_ACPT);
    CHECK(p.plink_state == MESH_PLINK_HOLDING, "ESTAB + CLS_ACPT -> HOLDING");
    CHECK(p.reason == WLAN_REASON_MESH_CLOSE, "close reason = MESH_CLOSE");
    CHECK(ctx.num_estab == 0, "established count decremented to 0");
    CHECK(r.deact_n == 1, "on_deactivated fired");
    CHECK(r.tx_n == tx_before + 1 &&
          last_action(&r) == WLAN_SP_MESH_PEERING_CLOSE,
          "ESTAB close emits PEERING_CLOSE");

    /* holding -> close completes -> back to LISTEN */
    mesh_plink_fsm(&p, MPL_CLS_ACPT);
    CHECK(p.plink_state == MESH_PLINK_LISTEN, "HOLDING + CLS_ACPT -> LISTEN");
    CHECK(p.llid == 0 && p.plid == 0, "restart cleared llid/plid");

    /* ============ Test 3: passive open (peer initiates) ============ */
    ctx_init(&ctx, &r);
    mesh_peer_init(&p, &ctx, peerB, true);
    mesh_plink_fsm(&p, MPL_OPN_ACPT);
    CHECK(p.plink_state == MESH_PLINK_OPN_RCVD, "LISTEN + OPN_ACPT -> OPN_RCVD");
    CHECK(p.llid != 0, "passive open assigned llid");
    CHECK(r.tx_n == 2 &&
          r.tx[0].action == WLAN_SP_MESH_PEERING_OPEN &&
          r.tx[1].action == WLAN_SP_MESH_PEERING_CONFIRM,
          "passive open emits OPEN then CONFIRM");
    mesh_plink_fsm(&p, MPL_CNF_ACPT);
    CHECK(p.plink_state == MESH_PLINK_ESTAB, "passive: OPN_RCVD + CNF_ACPT -> ESTAB");

    /* ============ Test 4: received-frame -> event mapping ============ */
    ctx_init(&ctx, &r);
    mesh_peer_init(&p, &ctx, peerB, true);
    p.llid = 7;  /* pretend we already sent an Open with llid 7 */

    enum mesh_plink_event ev;
    ev = mesh_plink_get_event(&ctx, &p, WLAN_SP_MESH_PEERING_OPEN,
                              /*matches_local=*/true, /*ie_len=*/6,
                              /*llid=*/0, /*plid=*/9);
    CHECK(ev == MPL_OPN_ACPT, "OPEN matching+free -> OPN_ACPT");

    ev = mesh_plink_get_event(&ctx, &p, WLAN_SP_MESH_PEERING_OPEN,
                              /*matches_local=*/false, 6, 0, 9);
    CHECK(ev == MPL_OPN_RJCT, "OPEN non-matching -> OPN_RJCT");

    ev = mesh_plink_get_event(&ctx, &p, WLAN_SP_MESH_PEERING_CONFIRM,
                              true, 8, /*llid=*/7, /*plid=*/9);
    CHECK(ev == MPL_CNF_ACPT, "CONFIRM llid-match -> CNF_ACPT");

    ev = mesh_plink_get_event(&ctx, &p, WLAN_SP_MESH_PEERING_CONFIRM,
                              true, 8, /*llid=*/999, 9);
    CHECK(ev == MPL_CNF_IGNR, "CONFIRM llid-mismatch -> CNF_IGNR");

    p.plink_state = MESH_PLINK_ESTAB;
    ev = mesh_plink_get_event(&ctx, &p, WLAN_SP_MESH_PEERING_CLOSE,
                              true, 8, 7, 9);
    CHECK(ev == MPL_CLS_ACPT, "CLOSE while ESTAB -> CLS_ACPT");

    p.plink_state = MESH_PLINK_LISTEN;
    p.authenticated = false;
    ev = mesh_plink_get_event(&ctx, &p, WLAN_SP_MESH_PEERING_OPEN,
                              true, 6, 0, 9);
    CHECK(ev == MPL_UNDEFINED, "unauthenticated peer -> UNDEFINED");

    printf("\n%s\n", failures ? "SOME CHECKS FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}
