/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Tests for the MPM frame bridge (mesh/mesh_mpm_frame.c) and its integration
 * with the peering FSM (mesh/mesh_plink.c):
 *
 *  (A) Wire round-trip: build OPEN/CONFIRM/CLOSE frames and parse them back,
 *      asserting the on-the-wire layout and the receiver-perspective llid/plid
 *      swap.
 *  (B) Two-node handshake through *serialized* frames: two FSMs whose tx_frame
 *      op builds real action-frame bytes; a shared "medium" parses each frame
 *      and delivers it to the other node's mesh_plink_rx. Driving one node's
 *      open() carries both nodes to ESTABLISHED — the FSM + frame ser/deser
 *      working together end-to-end, with no radio and no struct morse.
 */
#include "mesh/mesh_mpm_frame.h"
#include "mesh/mesh_plink.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static const u8 ADDR_A[ETH_ALEN] = { 0x02,0,0,0,0,0xAA };
static const u8 ADDR_B[ETH_ALEN] = { 0x02,0,0,0,0,0xBB };
static const u8 MESH_ID[] = { 'm','e','s','h','0' };

/* ===================== (A) wire round-trip ===================== */
static void test_roundtrip(void)
{
    u8 frame[MPM_FRAME_MAX];
    struct mpm_frame pf;
    size_t n;

    /* OPEN: our_llid = 0x1234, no plid, no reason. */
    n = mpm_build_frame(frame, sizeof(frame), ADDR_B, ADDR_A,
                        WLAN_SP_MESH_PEERING_OPEN, 0x1234, 0, 0, 0,
                        MESH_ID, sizeof(MESH_ID));
    CHECK(n > 0, "build OPEN returns a frame (%zu bytes)", n);
    CHECK(frame[24] == WLAN_CATEGORY_SELF_PROTECTED, "OPEN category = SELF_PROTECTED");
    CHECK(frame[25] == WLAN_SP_MESH_PEERING_OPEN, "OPEN action_code correct");
    CHECK(mpm_parse_frame(frame, n, &pf) && pf.valid, "parse OPEN ok");
    CHECK(pf.action == WLAN_SP_MESH_PEERING_OPEN, "parsed action = OPEN");
    CHECK(pf.peer_mgmt_ie_len == 4, "OPEN peering IE len = 4");
    CHECK(pf.plid == 0x1234, "OPEN: sender llid -> receiver plid (0x1234)");
    CHECK(pf.llid == 0, "OPEN: no echoed llid");
    CHECK(memcmp(pf.sa, ADDR_A, ETH_ALEN) == 0, "OPEN source = A");
    CHECK(pf.mesh_id_len == sizeof(MESH_ID) &&
          memcmp(pf.mesh_id, MESH_ID, sizeof(MESH_ID)) == 0, "OPEN carries Mesh ID");

    /* CONFIRM: our_llid = 0xBEEF, our_plid = 0x1234, aid = 7. */
    n = mpm_build_frame(frame, sizeof(frame), ADDR_B, ADDR_A,
                        WLAN_SP_MESH_PEERING_CONFIRM, 0xBEEF, 0x1234, 0, 7,
                        MESH_ID, sizeof(MESH_ID));
    CHECK(mpm_parse_frame(frame, n, &pf) && pf.valid, "parse CONFIRM ok");
    CHECK(pf.peer_mgmt_ie_len == 6, "CONFIRM peering IE len = 6");
    CHECK(pf.plid == 0xBEEF, "CONFIRM: sender llid -> receiver plid");
    CHECK(pf.llid == 0x1234, "CONFIRM: sender plid -> receiver llid (echo)");
    CHECK(pf.aid == 7, "CONFIRM carries AID");

    /* CLOSE: our_llid = 0xBEEF, our_plid = 0x1234, reason = MESH_CLOSE. */
    n = mpm_build_frame(frame, sizeof(frame), ADDR_B, ADDR_A,
                        WLAN_SP_MESH_PEERING_CLOSE, 0xBEEF, 0x1234,
                        WLAN_REASON_MESH_CLOSE, 0, MESH_ID, sizeof(MESH_ID));
    CHECK(mpm_parse_frame(frame, n, &pf) && pf.valid, "parse CLOSE ok");
    CHECK(pf.peer_mgmt_ie_len == 8, "CLOSE-with-plid peering IE len = 8");
    CHECK(pf.plid == 0xBEEF && pf.llid == 0x1234, "CLOSE llid/plid swapped");
    CHECK(pf.reason == WLAN_REASON_MESH_CLOSE, "CLOSE carries reason");

    /* A garbage buffer must be rejected. */
    u8 junk[40]; memset(junk, 0xA5, sizeof(junk));
    CHECK(!mpm_parse_frame(junk, sizeof(junk), &pf), "garbage frame rejected");
}

/* ===================== (B) two-node handshake ===================== */
struct node {
    const u8 *addr;
    const u8 *mesh_id;
    u8 mesh_id_len;
    u16 aid;
    u16 next_llid;
    struct mesh_plink_ctx ctx;
    struct mesh_peer peer;   /* this node's view of the other node */
    struct node *other;
};

/* shared "medium": a FIFO of frames in flight */
struct inflight { struct node *dest; u8 buf[MPM_FRAME_MAX]; size_t len; };
static struct inflight medium[32];
static int medium_head, medium_tail;

static u16 node_new_llid(struct mesh_plink_ctx *ctx)
{
    struct node *n = ctx->priv;
    return ++n->next_llid;
}
static void node_tx(struct mesh_plink_ctx *ctx, struct mesh_peer *peer,
                    u8 action, u16 llid, u16 plid, u16 reason)
{
    struct node *n = ctx->priv;
    struct inflight *f = &medium[medium_tail++ % 32];
    f->dest = n->other;
    f->len = mpm_build_frame(f->buf, sizeof(f->buf), peer->addr, n->addr,
                             action, llid, plid, reason, n->aid,
                             n->mesh_id, n->mesh_id_len);
}
static const struct mesh_plink_ops node_ops = {
    .new_llid = node_new_llid,
    .tx_frame = node_tx,
    /* timers / established hooks unused in this convergence test */
};

static void node_init(struct node *n, const u8 *addr, struct node *other)
{
    memset(n, 0, sizeof(*n));
    n->addr = addr;
    n->mesh_id = MESH_ID;
    n->mesh_id_len = sizeof(MESH_ID);
    n->other = other;
    n->ctx.ops = &node_ops;
    n->ctx.priv = n;
    n->ctx.cfg.retry_timeout_ms = 40;
    n->ctx.cfg.confirm_timeout_ms = 40;
    n->ctx.cfg.holding_timeout_ms = 40;
    n->ctx.cfg.max_peer_links = 4;
    mesh_peer_init(&n->peer, &n->ctx, other->addr, /*authenticated=*/true);
}

/* deliver one queued frame to its destination node's FSM */
static void medium_pump_one(void)
{
    struct inflight *f = &medium[medium_head++ % 32];
    struct mpm_frame pf;
    bool matches;

    if (!mpm_parse_frame(f->buf, f->len, &pf))
        return;
    matches = (pf.mesh_id_len == f->dest->mesh_id_len) &&
              (memcmp(pf.mesh_id, f->dest->mesh_id, pf.mesh_id_len) == 0);
    mesh_plink_rx(&f->dest->peer, pf.action, matches,
                  pf.peer_mgmt_ie_len, pf.llid, pf.plid);
}

static void test_handshake(void)
{
    struct node A, B;
    int pumped = 0;

    /* B must exist before A's Open arrives; node_init cross-links them. */
    static struct node SA, SB;
    node_init(&SA, ADDR_A, &SB);
    node_init(&SB, ADDR_B, &SA);
    A = SA; B = SB;
    /* fix self-referential pointers after the struct copies */
    A.other = &B; B.other = &A;
    A.ctx.priv = &A; B.ctx.priv = &B;
    A.peer.ctx = &A.ctx; B.peer.ctx = &B.ctx;
    medium_head = medium_tail = 0;

    CHECK(A.peer.plink_state == MESH_PLINK_LISTEN &&
          B.peer.plink_state == MESH_PLINK_LISTEN, "both nodes start LISTEN");

    /* A actively opens to B; pump the medium to completion. */
    mesh_plink_open(&A.peer);
    while (medium_head != medium_tail && pumped < 64) {
        medium_pump_one();
        pumped++;
    }

    CHECK(A.peer.plink_state == MESH_PLINK_ESTAB, "node A reached ESTABLISHED");
    CHECK(B.peer.plink_state == MESH_PLINK_ESTAB, "node B reached ESTABLISHED");
    CHECK(A.peer.llid != 0 && A.peer.plid == B.peer.llid,
          "A's plid matches B's llid (link ids cross-bound)");
    CHECK(B.peer.plid == A.peer.llid, "B's plid matches A's llid");
    CHECK(A.ctx.num_estab == 1 && B.ctx.num_estab == 1,
          "each node counts one established peer");

    /* Now A closes the link. The close handshake is asymmetric: A -> HOLDING
     * (sends Close); B receives it -> HOLDING (sends Close back); A receives
     * B's Close while in HOLDING -> fsm_restart -> LISTEN. B then waits in
     * HOLDING for its holding timer (a timer callback, not modelled here). */
    mesh_plink_fsm(&A.peer, MPL_CLS_ACPT);   /* local close request */
    pumped = 0;
    while (medium_head != medium_tail && pumped < 64) {
        medium_pump_one();
        pumped++;
    }
    CHECK(A.peer.plink_state == MESH_PLINK_LISTEN,
          "A completed close handshake -> LISTEN");
    CHECK(B.peer.plink_state == MESH_PLINK_HOLDING,
          "B tore down on Close (awaiting holding timer)");
    CHECK(A.ctx.num_estab == 0 && B.ctx.num_estab == 0,
          "both established counts dropped to 0 after teardown");
}

int main(void)
{
    printf("--- wire round-trip ---\n");
    test_roundtrip();
    printf("\n--- two-node handshake through serialized frames ---\n");
    test_handshake();
    printf("\n%s\n", failures ? "SOME CHECKS FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}
