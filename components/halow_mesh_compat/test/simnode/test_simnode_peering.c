/*
 * Peering, the MPM state machine, and what losing a peer does to forwarding.
 *
 * Everything here is driven through simnode_rx(), i.e. through the real RX
 * path: umac_datapath_rx_frame -> the mesh filter -> process_rx_mgmt_frame_mesh
 * -> umac_mesh_handle_mpm(). The frames the test injects are built by the
 * SHIPPING builder (umac_mesh_ies_build_mpm_body) and the frames the firmware
 * emits are read back with the SHIPPING parsers
 * (umac_mesh_ies_get_peer_llid/plid/close_reason), so a hand-rolled layout in
 * the test cannot agree with a hand-rolled layout in the firmware while both
 * are wrong about the air.
 *
 * SCOPE, and what is deliberately absent:
 *
 *   - This is warthog's OWN MPM responder, the one that runs on an OPEN mesh.
 *     SAE and AMPE live in the hostap supplicant, which the simulator stubs
 *     (umac_supp_*), so no SAE peering is driven here and none is faked. A
 *     SAE-mode assertion would be asserting on a stub.
 *   - mmosal_get_time_ms() is the simulator's virtual clock, so the 30 s
 *     peering timeout is crossed by advancing it, not by waiting.
 *   - mpm_expire_stale_() runs from umac_mesh_maybe_initiate_mpm() on every
 *     received probe request (prod_() below), and from the service tick --
 *     which only POSTS it to the umac event loop, where the RX path that
 *     dereferences the stations it frees also runs. simnode_tick() pumps that
 *     loop; section 5c calls the bare tick to show the post is all it does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "simnode.h"
#include "umac_mesh_ies.h"
#include "umac_mesh_hwmp.h"
#include "umac_mesh_plink_tbl.h"

/* Reset hooks: the shipping "forget every peer" entry points. There is no
 * mesh teardown on this port (umac_mesh_disable_mesh returns UNAVAILABLE), so
 * scenarios are separated with the same two calls AT+MESHRELINK uses. */
void umac_mesh_reset_links(void);
void umac_datapath_mesh_del_peer(const uint8_t *peer_addr);
uint8_t umac_datapath_mesh_peer_count(void);

/* The AT counters umac_mesh.c publishes (storage in the generated
 * warthog_globals.c, mirroring main/at.c). */
extern volatile uint32_t g_warthog_mpm_rx, g_warthog_mpm_open_tx, g_warthog_mpm_conf_tx;
extern volatile uint32_t g_warthog_mpm_conf_rx, g_warthog_mpm_close_rx, g_warthog_mpm_close_tx;
extern volatile uint32_t g_warthog_mpm_parse_fail, g_warthog_mpm_estab;
extern volatile uint32_t g_warthog_mpm_no_slot, g_warthog_mpm_expired;
extern volatile uint32_t g_warthog_mpm_close_reason;
extern volatile char g_warthog_mpm_links[256];
extern volatile uint32_t g_warthog_mpm_foreign, g_warthog_prq_other;
/* True when we hold no MPM link for @p sa (it refreshes one we do hold). */
bool umac_mesh_note_s1g_beacon(const uint8_t *sa);

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static const uint8_t W[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 }; /* us */
static const uint8_t A[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0a };
static const uint8_t B[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0b };
static const uint8_t C[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0c };
static const uint8_t D[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0d };
static const uint8_t E[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0e };
static const uint8_t P[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x50 }; /* prodder */
static const uint8_t R1[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0xa1 }; /* remote dst */
static const uint8_t R2[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0xa2 };

/* The Mesh ID simnode_start() brings the node up with. A peering frame whose
 * Mesh ID does not match is a different mesh. */
static const uint8_t MESHID[] = { 's', 'i', 'm', 'n', 'o', 'd', 'e' };

#define DOT11_MGMT_HDR_LEN 24u
#define FC_ACTION 0xd0u /* subtype 13 (Action), type 0 (Mgmt), version 0 */
#define FC_PROBE_REQ 0x40u /* subtype 4 (Probe Request) */

/* ---- injection helpers ------------------------------------------------- */

/** Wrap @p body in a PV0 management header from @p sa to us and receive it. */
static bool rx_mgmt(uint8_t fc0, const uint8_t *sa, const uint8_t *body, uint16_t body_len)
{
    uint8_t f[512];
    if ((uint32_t)body_len + DOT11_MGMT_HDR_LEN > sizeof(f)) { return false; }
    memset(f, 0, DOT11_MGMT_HDR_LEN);
    f[0] = fc0;
    memcpy(f + 4, W, 6);   /* addr1 = RA = us */
    memcpy(f + 10, sa, 6); /* addr2 = TA = the peer */
    memcpy(f + 16, sa, 6); /* addr3 = BSSID; in a mesh that is the sender */
    if (body != NULL && body_len != 0) { memcpy(f + DOT11_MGMT_HDR_LEN, body, body_len); }
    return simnode_rx(f, (uint16_t)(DOT11_MGMT_HDR_LEN + body_len), -60);
}

/** Receive an MPM frame from @p sa, built by the shipping builder. */
static bool rx_mpm(const uint8_t *sa, uint8_t action, uint16_t llid, uint16_t plid,
                   uint16_t reason, uint16_t aid)
{
    uint8_t body[UMAC_MESH_MPM_BODY_MAXLEN];
    uint16_t n = umac_mesh_ies_build_mpm_body(body, sizeof(body), action, llid, plid, reason, aid,
                                              MESHID, sizeof(MESHID), /*sae=*/false, NULL, 0);
    if (n == 0) { return false; }
    return rx_mgmt(FC_ACTION, sa, body, n);
}

/** Receive a category-13 HWMP body from @p sa. */
static bool rx_hwmp(const uint8_t *sa, const uint8_t *body, uint16_t n)
{
    return rx_mgmt(FC_ACTION, sa, body, n);
}

/**
 * Receive a probe request from @p sa.
 *
 * This is the firmware's own driver for the peering watchdog: the mesh RX
 * dispatch answers a probe request and then calls
 * umac_mesh_maybe_initiate_mpm(), whose first act is mpm_expire_stale_().
 */
static bool prod_(const uint8_t *sa)
{
    /* SSID element carrying the Mesh ID, which is what a mesh probe carries. */
    uint8_t ies[2 + sizeof(MESHID)];
    ies[0] = 0; /* SSID */
    ies[1] = (uint8_t)sizeof(MESHID);
    memcpy(ies + 2, MESHID, sizeof(MESHID));
    return rx_mgmt(FC_PROBE_REQ, sa, ies, (uint16_t)sizeof(ies));
}

/* ---- outbox inspection ------------------------------------------------- */

/** The @p nth category-15 frame in the outbox carrying @p action, or NULL. */
static const struct simnode_frame *mpm_nth(uint8_t action, unsigned nth)
{
    unsigned seen = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (f == NULL || f->len <= DOT11_MGMT_HDR_LEN + 1u) { continue; }
        const uint8_t *b = f->bytes + DOT11_MGMT_HDR_LEN;
        if (b[0] != UMAC_MESH_MPM_CATEGORY || b[1] != action) { continue; }
        if (seen++ == nth) { return f; }
    }
    return NULL;
}

static unsigned mpm_count(uint8_t action)
{
    unsigned n = 0;
    while (mpm_nth(action, n) != NULL) { n++; }
    return n;
}

/** Index of the @p nth category-15 frame with @p action within the outbox, or
 *  -1. Used to assert the ORDER two frames were handed to the chip in. */
static int mpm_pos(uint8_t action, unsigned nth)
{
    unsigned seen = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (f == NULL || f->len <= DOT11_MGMT_HDR_LEN + 1u) { continue; }
        const uint8_t *b = f->bytes + DOT11_MGMT_HDR_LEN;
        if (b[0] != UMAC_MESH_MPM_CATEGORY || b[1] != action) { continue; }
        if (seen++ == nth) { return (int)i; }
    }
    return -1;
}

/** Link ids of an MPM frame we sent. llid is ours, plid our view of theirs. */
static bool mpm_ids(const struct simnode_frame *f, uint16_t *llid, uint16_t *plid)
{
    if (f == NULL) { return false; }
    const uint8_t *b = f->bytes + DOT11_MGMT_HDR_LEN;
    uint32_t n = f->len - DOT11_MGMT_HDR_LEN;
    uint16_t a = 0, p = 0;
    bool ok = umac_mesh_ies_get_peer_llid(b, n, &a);
    if (llid != NULL) { *llid = a; }
    if (plid != NULL)
    {
        (void)umac_mesh_ies_get_peer_plid(b, n, &p);
        *plid = p;
    }
    return ok;
}

/** Copy the single +MESHPATH line beginning at @p at into @p out. Comparing
 *  substring POSITIONS inside a multi-line dump would let "active" from the
 *  next path satisfy a claim about this one. */
static const char *path_line(const char *dump, const char *key, char *out, size_t out_len)
{
    out[0] = '\0';
    const char *at = (dump != NULL && key != NULL) ? strstr(dump, key) : NULL;
    if (at == NULL) { return NULL; }
    const char *end = strstr(at, "\r\n");
    size_t n = (end != NULL) ? (size_t)(end - at) : strlen(at);
    if (n >= out_len) { n = out_len - 1u; }
    memcpy(out, at, n);
    out[n] = '\0';
    return out;
}

/** Destination address of a frame we sent (addr1). */
static const uint8_t *frame_da(const struct simnode_frame *f)
{
    return f->bytes + 4;
}

/** A broadcast category-13 frame carrying element @p eid, or NULL. */
static const struct simnode_frame *hwmp_bcast(uint8_t eid)
{
    static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (f == NULL || f->len <= DOT11_MGMT_HDR_LEN + 2u) { continue; }
        const uint8_t *b = f->bytes + DOT11_MGMT_HDR_LEN;
        if (b[0] != HWMP_CATEGORY_MESH || b[2] != eid) { continue; }
        if (memcmp(frame_da(f), bcast, 6) != 0) { continue; }
        return f;
    }
    return NULL;
}

/* ---- scenario setup ---------------------------------------------------- */

static void reset_node(void)
{
    umac_datapath_mesh_del_peer(NULL); /* every datapath peer */
    umac_mesh_reset_links();           /* every MPM link */
    simnode_outbox_clear();
}

/** Run the handshake the way a peer does: their Open, then their Confirm
 *  echoing the llid we answered with. Returns our llid for the link. */
static uint16_t peer_up(const uint8_t *peer, uint16_t their_llid)
{
    simnode_outbox_clear();
    if (!rx_mpm(peer, UMAC_MESH_MPM_ACTION_OPEN, their_llid, 0, 0, 0)) { return 0; }
    uint16_t ours = 0;
    if (!mpm_ids(mpm_nth(UMAC_MESH_MPM_ACTION_CONFIRM, 0), &ours, NULL)) { return 0; }
    simnode_outbox_clear();
    if (!rx_mpm(peer, UMAC_MESH_MPM_ACTION_CONFIRM, their_llid, ours, 0, 1)) { return 0; }
    return ours;
}

/** Install a path to @p dst through @p via by receiving a PREQ that @p via
 *  transmitted on @p dst's behalf. This is the real route-info rule: a node
 *  installs a path to the ORIGINATOR of any PREQ it accepts, next-hop being
 *  the transmitter. */
static bool install_path_via(const uint8_t *via, const uint8_t *dst, uint32_t sn)
{
    uint8_t body[HWMP_PREQ_BODY_LEN];
    uint16_t n = umac_mesh_hwmp_build_preq(body, sizeof(body), dst, sn, sn, W, 4882u);
    if (n == 0) { return false; }
    return rx_hwmp(via, body, n);
}

/* ---- the mesh profile (sections 14 to 21) ------------------------------ */

#define FC_PROBE_RSP 0x50u
static const uint8_t OTHERID[] = { 'o', 't', 'h', 'e', 'r', 'm', 'e', 's', 'h' };

/* A neighbour no earlier section used: its tail is in no stale AT+MPMPEERS? string. */
static void xaddr_(uint8_t out[6], uint8_t id)
{
    static const uint8_t base[6] = { 0x02, 0x00, 0x00, 0x00, 0x02, 0x00 };
    memcpy(out, base, 6);
    out[5] = id;
}

/** As rx_mpm, for the mesh @p id and the security @p sae its sender claims. */
static bool rx_mpm_as(const uint8_t *sa, uint8_t action, uint16_t llid, uint16_t plid,
                      uint16_t aid, const uint8_t *id, uint8_t id_len, bool sae)
{
    uint8_t body[UMAC_MESH_MPM_BODY_MAXLEN];
    uint16_t n = umac_mesh_ies_build_mpm_body(body, sizeof(body), action, llid, plid, 0, aid,
                                              id, id_len, sae, NULL, 0);
    return n != 0 && rx_mgmt(FC_ACTION, sa, body, n);
}

/** An Open from @p sa: @p ies, then a Peer Management element carrying @p llid. */
static bool rx_open_ies(const uint8_t *sa, const uint8_t *ies, uint16_t ies_len, uint16_t llid)
{
    uint8_t b[160];
    uint16_t n = 0;
    if ((uint32_t)ies_len + 10u > sizeof(b)) { return false; }
    b[n++] = UMAC_MESH_MPM_CATEGORY;
    b[n++] = UMAC_MESH_MPM_ACTION_OPEN;
    b[n++] = 0;
    b[n++] = 0;
    if (ies_len != 0) { memcpy(b + n, ies, ies_len); n = (uint16_t)(n + ies_len); }
    b[n++] = UMAC_MESH_EID_PEER_MGMT;
    b[n++] = 4;
    b[n++] = 0;
    b[n++] = 0;
    b[n++] = (uint8_t)llid;
    b[n++] = (uint8_t)(llid >> 8);
    return rx_mgmt(FC_ACTION, sa, b, n);
}

/** Frames in the outbox with frame control octet @p fc0 addressed to @p da. */
static unsigned count_fc_to(uint8_t fc0, const uint8_t *da)
{
    unsigned n = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        n += (f != NULL && f->len >= DOT11_MGMT_HDR_LEN && f->bytes[0] == fc0 &&
              memcmp(frame_da(f), da, 6) == 0);
    }
    return n;
}

/** The refusal of an Open from @p sa that cited @p llid: nothing but one Close(54) to it,
 *  llid 0, plid its llid, and no link. Clear the outbox before the Open. */
static bool refused_54_(const uint8_t *sa, uint16_t llid)
{
    const struct simnode_frame *cl = mpm_nth(UMAC_MESH_MPM_ACTION_CLOSE, 0);
    uint16_t reason = 0, l = 0xffff, p = 0;
    if (cl == NULL || mpm_count(UMAC_MESH_MPM_ACTION_CLOSE) != 1 ||
        mpm_count(UMAC_MESH_MPM_ACTION_OPEN) != 0 || mpm_count(UMAC_MESH_MPM_ACTION_CONFIRM) != 0 ||
        memcmp(frame_da(cl), sa, 6) != 0)
    {
        return false;
    }
    const uint8_t *b = cl->bytes + DOT11_MGMT_HDR_LEN;
    const uint32_t n = cl->len - DOT11_MGMT_HDR_LEN;
    return umac_mesh_ies_get_close_reason(b, n, &reason) && reason == 54u && /* the literal */
           mpm_ids(cl, &l, &p) && l == 0 && p == llid && umac_mesh_note_s1g_beacon(sa);
}

/* ======================================================================== */

int main(void)
{
    printf("=== simnode peering: MPM, the peering watchdog, and peer loss ===\n");

    CHECK(simnode_start(W), "node starts on an OPEN mesh");
    /* Forwarding on: peer loss only touches the path table when the engine
     * owns it (umac_datapath_mesh_del_peer gates on AT+MESHFWD/MESHBRIDGE). */
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/false);

    /* ---- 1. An Open from an unknown neighbour starts the handshake ------ */
    reset_node();
    {
        uint32_t rx0 = g_warthog_mpm_rx, open0 = g_warthog_mpm_open_tx;
        CHECK(rx_mpm(A, UMAC_MESH_MPM_ACTION_OPEN, 0x1111, 0, 0, 0),
              "an MPM Open from A is accepted by the real RX path");
        CHECK(g_warthog_mpm_rx == rx0 + 1, "it reached umac_mesh_handle_mpm (mpm_rx %u -> %u)",
              (unsigned)rx0, (unsigned)g_warthog_mpm_rx);

        CHECK(mpm_count(UMAC_MESH_MPM_ACTION_OPEN) == 1,
              "we answer with exactly one Open (got %u)", mpm_count(UMAC_MESH_MPM_ACTION_OPEN));
        CHECK(mpm_count(UMAC_MESH_MPM_ACTION_CONFIRM) == 1,
              "and exactly one Confirm (got %u)", mpm_count(UMAC_MESH_MPM_ACTION_CONFIRM));
        CHECK(g_warthog_mpm_open_tx == open0 + 1, "the Open is counted for AT+MPMSTAT?");

        int po = mpm_pos(UMAC_MESH_MPM_ACTION_OPEN, 0);
        int pc = mpm_pos(UMAC_MESH_MPM_ACTION_CONFIRM, 0);
        CHECK(po >= 0 && pc >= 0 && po < pc,
              "Open goes to the chip BEFORE the Confirm (positions %d, %d)", po, pc);

        const struct simnode_frame *op = mpm_nth(UMAC_MESH_MPM_ACTION_OPEN, 0);
        const struct simnode_frame *cf = mpm_nth(UMAC_MESH_MPM_ACTION_CONFIRM, 0);
        CHECK(op != NULL && memcmp(frame_da(op), A, 6) == 0, "the Open is addressed to A");
        CHECK(cf != NULL && memcmp(frame_da(cf), A, 6) == 0, "the Confirm is addressed to A");

        uint16_t o_llid = 0, c_llid = 0, c_plid = 0;
        CHECK(mpm_ids(op, &o_llid, NULL), "our Open carries a Peer Management IE");
        CHECK(mpm_ids(cf, &c_llid, &c_plid), "our Confirm carries a Peer Management IE");
        CHECK(o_llid != 0, "our Open carries a non-zero llid (got 0x%04x)", o_llid);
        CHECK(o_llid == c_llid, "Open and Confirm cite the SAME llid (0x%04x vs 0x%04x)",
              o_llid, c_llid);
        CHECK(c_plid == 0x1111,
              "the Confirm echoes THEIR llid as plid (got 0x%04x, want 0x1111)", c_plid);

        /* AID 0 is the group key; a peer assigning per-link AIDs refuses it,
         * which is what left a mac80211 peer stuck at OPN_RCVD. The builder
         * puts AID at body[4..5] of a Confirm. */
        uint16_t aid = 0;
        if (cf != NULL && cf->len > DOT11_MGMT_HDR_LEN + 5u)
        {
            const uint8_t *b = cf->bytes + DOT11_MGMT_HDR_LEN;
            aid = (uint16_t)(b[4] | ((uint16_t)b[5] << 8));
        }
        CHECK(aid != 0, "the Confirm carries a non-zero AID (got %u)", aid);

        CHECK(g_warthog_mpm_estab == 0,
              "the link is NOT established yet -- they have not confirmed (estab=%u)",
              (unsigned)g_warthog_mpm_estab);
        CHECK(umac_datapath_mesh_peer_count() == 0,
              "and no datapath peer exists yet (count=%u)", umac_datapath_mesh_peer_count());
    }

    /* ---- 2. An Open we cannot read a link id from is answered with silence */
    reset_node();
    {
        uint32_t pf0 = g_warthog_mpm_parse_fail;
        /* Category + action + capability only: no Peer Management IE at all.
         * Sent from C, an address no earlier scenario used, because the
         * published AT+MPMPEERS? string is only rewritten when mpm_publish_
         * runs -- so "C is absent from it" is a statement about this frame
         * having published nothing, which is exactly the claim under test. */
        uint8_t runt[4] = { UMAC_MESH_MPM_CATEGORY, UMAC_MESH_MPM_ACTION_OPEN, 0, 0 };
        CHECK(rx_mgmt(FC_ACTION, C, runt, sizeof(runt)), "a truncated Open is delivered");
        CHECK(g_warthog_mpm_parse_fail == pf0 + 1,
              "the link-id parse failure is counted (%u -> %u)",
              (unsigned)pf0, (unsigned)g_warthog_mpm_parse_fail);
        CHECK(mpm_count(UMAC_MESH_MPM_ACTION_CONFIRM) == 0,
              "we send NO Confirm: plid=0 would be answered CNF_IGNR (got %u)",
              mpm_count(UMAC_MESH_MPM_ACTION_CONFIRM));
        CHECK(mpm_count(UMAC_MESH_MPM_ACTION_OPEN) == 0,
              "and no Open either (got %u)", mpm_count(UMAC_MESH_MPM_ACTION_OPEN));
        CHECK(strstr((const char *)g_warthog_mpm_links, "00000c") == NULL,
              "and no link was created for C: %s", (const char *)g_warthog_mpm_links);
    }

    /* ---- 3. Their Confirm echoing our llid establishes the link --------- */
    reset_node();
    {
        simnode_outbox_clear();
        rx_mpm(A, UMAC_MESH_MPM_ACTION_OPEN, 0x2222, 0, 0, 0);
        uint16_t ours = 0;
        CHECK(mpm_ids(mpm_nth(UMAC_MESH_MPM_ACTION_CONFIRM, 0), &ours, NULL) && ours != 0,
              "we answered A's Open with llid 0x%04x", ours);

        simnode_outbox_clear();
        uint32_t cr0 = g_warthog_mpm_conf_rx;
        CHECK(rx_mpm(A, UMAC_MESH_MPM_ACTION_CONFIRM, 0x2222, ours, 0, 1),
              "A confirms, echoing our llid");
        CHECK(g_warthog_mpm_conf_rx == cr0 + 1, "the Confirm is counted");
        CHECK(g_warthog_mpm_estab == 1, "the link is ESTAB (estab=%u)",
              (unsigned)g_warthog_mpm_estab);
        CHECK(umac_datapath_mesh_peer_count() == 1,
              "and the peer was handed to the data plane (count=%u)",
              umac_datapath_mesh_peer_count());
        CHECK(mpm_count(UMAC_MESH_MPM_ACTION_CONFIRM) == 0,
              "we quiesce: no Confirm answers a Confirm once ESTAB (got %u)",
              mpm_count(UMAC_MESH_MPM_ACTION_CONFIRM));
        CHECK(strstr((const char *)g_warthog_mpm_links, "estab=1") != NULL,
              "AT+MPMPEERS? shows the link up: %s", (const char *)g_warthog_mpm_links);
    }

    /* ---- 4. A Confirm echoing the WRONG llid does not establish --------- */
    reset_node();
    {
        rx_mpm(B, UMAC_MESH_MPM_ACTION_OPEN, 0x3333, 0, 0, 0);
        uint16_t ours = 0;
        mpm_ids(mpm_nth(UMAC_MESH_MPM_ACTION_CONFIRM, 0), &ours, NULL);
        simnode_outbox_clear();

        uint16_t wrong = (uint16_t)(ours ^ 0xa5a5u);
        CHECK(wrong != ours, "the echoed llid under test really is wrong (0x%04x)", wrong);
        rx_mpm(B, UMAC_MESH_MPM_ACTION_CONFIRM, 0x3333, wrong, 0, 1);
        CHECK(g_warthog_mpm_estab == 0,
              "a Confirm citing an llid that is not ours does NOT establish (estab=%u)",
              (unsigned)g_warthog_mpm_estab);
        CHECK(umac_datapath_mesh_peer_count() == 0,
              "no datapath peer is created (count=%u)", umac_datapath_mesh_peer_count());
        CHECK(mpm_count(UMAC_MESH_MPM_ACTION_CONFIRM) == 1,
              "and we retransmit our Confirm while the link is still coming up (got %u)",
              mpm_count(UMAC_MESH_MPM_ACTION_CONFIRM));
    }

    /* ---- 5. An Open arriving on an ESTAB link gets a Confirm ALONE ------ */
    reset_node();
    {
        uint16_t ours = peer_up(A, 0x4444);
        CHECK(ours != 0 && g_warthog_mpm_estab == 1, "A is established (our llid 0x%04x)", ours);

        simnode_outbox_clear();
        rx_mpm(A, UMAC_MESH_MPM_ACTION_OPEN, 0x4444, 0, 0, 0);
        CHECK(mpm_count(UMAC_MESH_MPM_ACTION_OPEN) == 0,
              "answering an ESTAB peer's Open with our own Open would loop forever -- "
              "we send none (got %u)", mpm_count(UMAC_MESH_MPM_ACTION_OPEN));
        CHECK(mpm_count(UMAC_MESH_MPM_ACTION_CONFIRM) == 1,
              "we answer with the Confirm alone (got %u)",
              mpm_count(UMAC_MESH_MPM_ACTION_CONFIRM));
        CHECK(umac_datapath_mesh_peer_count() == 1,
              "the established link is undisturbed (count=%u)",
              umac_datapath_mesh_peer_count());
        uint16_t c_llid = 0;
        mpm_ids(mpm_nth(UMAC_MESH_MPM_ACTION_CONFIRM, 0), &c_llid, NULL);
        CHECK(c_llid == ours, "and it still cites the link's own llid (0x%04x vs 0x%04x)",
              c_llid, ours);
    }

    /* ---- 5a. A Close republishes what AT+MPMPEERS? prints -------------
     *
     * Every other teardown calls mpm_publish_. The RX Close path did not, so
     * a dead link kept reading estab=1 while AT+MPMSTAT? said 0 -- the two
     * commands contradicted each other after any Close. */
    reset_node();
    {
        uint16_t ours = peer_up(A, 0x8888);
        CHECK(ours != 0 && umac_datapath_mesh_peer_count() == 1, "A is established");
        CHECK(strstr((const char *)g_warthog_mpm_links, "estab=1") != NULL,
              "AT+MPMPEERS? shows the link established: %s", (const char *)g_warthog_mpm_links);

        rx_mpm(A, UMAC_MESH_MPM_ACTION_CLOSE, 0x8888, ours, 0, 0);
        CHECK(umac_datapath_mesh_peer_count() == 0,
              "the Close tears the peer down (count=%u)", umac_datapath_mesh_peer_count());
        CHECK(strstr((const char *)g_warthog_mpm_links, "estab=1") == NULL,
              "and AT+MPMPEERS? no longer claims it is established: %s",
              (const char *)g_warthog_mpm_links);
    }

    /* ---- 5b. The watchdog runs on OUR clock, with no peer traffic ------
     *
     * The peer going silent is exactly what the watchdog exists for, so it
     * must not depend on a neighbour transmitting. It used to: its only
     * caller ran from a received probe request or beacon, which is the
     * PEER's clock, and a two-node mesh whose partner died held the dead
     * stad, its chip registration and its key slot forever. */
    reset_node();
    {
        uint16_t ours = peer_up(A, 0x7777);
        CHECK(ours != 0 && umac_datapath_mesh_peer_count() == 1, "A is established");
        uint32_t expired_before = g_warthog_mpm_expired;

        /* No frame from anyone: only our own 2 s service tick, past the 30 s
         * peer timeout. */
        for (unsigned i = 0; i < 20u; i++)
        {
            simnode_advance_ms(2000);
            simnode_tick();
        }
        CHECK(umac_datapath_mesh_peer_count() == 0,
              "40 s of silence and only our own tick expires the dead peer (count=%u)",
              umac_datapath_mesh_peer_count());
        CHECK(g_warthog_mpm_expired > expired_before,
              "and it is counted as an expiry (%lu -> %lu)",
              (unsigned long)expired_before, (unsigned long)g_warthog_mpm_expired);
    }

    /* ---- 5c. The tick frees no station itself ---------------------------
     *
     * The tick runs on the mesh-probe task, but stations are dereferenced by
     * the RX path on the umac event loop, so freeing one from the tick is a
     * cross-task use-after-free. The tick must only post the work: nothing is
     * torn down until the event loop runs, for expiry and for AT+MESHRELINK. */
    reset_node();
    {
        extern volatile uint32_t g_warthog_mesh_repeer_req;
        void umac_mesh_service_tick(void);

        uint16_t ours = peer_up(A, 0x7a7a);
        CHECK(ours != 0 && umac_datapath_mesh_peer_count() == 1, "A is established");
        uint32_t expired_before = g_warthog_mpm_expired;
        simnode_advance_ms(40000);
        umac_mesh_service_tick();
        CHECK(umac_datapath_mesh_peer_count() == 1 && g_warthog_mpm_expired == expired_before,
              "40 s of silence: the bare tick leaves A's station alone (count=%u)",
              umac_datapath_mesh_peer_count());
        CHECK(simnode_evt_pending() == 1u, "it posted one event to the loop (%u)", simnode_evt_pending());
        umac_mesh_service_tick();
        CHECK(simnode_evt_pending() == 1u, "a second tick while it is queued posts no more (%u)",
              simnode_evt_pending());
        simnode_pump();
        CHECK(umac_datapath_mesh_peer_count() == 0 && g_warthog_mpm_expired > expired_before,
              "the event loop runs it and A expires (count=%u)", umac_datapath_mesh_peer_count());
        CHECK(strstr((const char *)g_warthog_mpm_links, "00000a") == NULL,
              "and its link is gone from AT+MPMPEERS? (%s)", (const char *)g_warthog_mpm_links);

        CHECK(peer_up(B, 0x7b7b) != 0 && umac_datapath_mesh_peer_count() == 1, "B is established");
        g_warthog_mesh_repeer_req = 1;
        umac_mesh_service_tick();
        CHECK(umac_datapath_mesh_peer_count() == 1 && g_warthog_mesh_repeer_req == 1,
              "AT+MESHRELINK: the bare tick leaves B's station alone");
        simnode_pump();
        CHECK(umac_datapath_mesh_peer_count() == 0 && g_warthog_mesh_repeer_req == 0,
              "the event loop tears B down (count=%u)", umac_datapath_mesh_peer_count());
        umac_mesh_service_tick();
        CHECK(simnode_evt_pending() == 1u, "once the event has run, the next tick posts again (%u)",
              simnode_evt_pending());
        simnode_pump();

        /* A full event pool refuses the post; the tick must not wait on it forever. */
        CHECK(simnode_evt_fill() > 0u, "event queue filled");
        umac_mesh_service_tick();
        simnode_pump();
        umac_mesh_service_tick();
        CHECK(simnode_evt_pending() == 1u, "a tick refused by a full queue is posted by the next (%u)",
              simnode_evt_pending());
        simnode_pump();
    }

    /* ---- 6. An Open with a NEW llid means the peer restarted ------------ */
    reset_node();
    {
        uint16_t ours = peer_up(A, 0x5555);
        CHECK(ours != 0 && umac_datapath_mesh_peer_count() == 1, "A is established");

        simnode_outbox_clear();
        rx_mpm(A, UMAC_MESH_MPM_ACTION_OPEN, 0x6666, 0, 0, 0); /* A rebooted */
        CHECK(umac_datapath_mesh_peer_count() == 0,
              "the stale datapath peer is torn down (count=%u)",
              umac_datapath_mesh_peer_count());
        CHECK(g_warthog_mpm_estab == 0, "and the link leaves ESTAB (estab=%u)",
              (unsigned)g_warthog_mpm_estab);
        CHECK(mpm_count(UMAC_MESH_MPM_ACTION_OPEN) == 1,
              "the full handshake runs again: an Open goes out (got %u)",
              mpm_count(UMAC_MESH_MPM_ACTION_OPEN));
        uint16_t c_plid = 0;
        mpm_ids(mpm_nth(UMAC_MESH_MPM_ACTION_CONFIRM, 0), NULL, &c_plid);
        CHECK(c_plid == 0x6666, "and the Confirm cites the NEW plid (got 0x%04x)", c_plid);

        /* It comes back up on the new id. */
        uint16_t again = 0;
        mpm_ids(mpm_nth(UMAC_MESH_MPM_ACTION_CONFIRM, 0), &again, NULL);
        rx_mpm(A, UMAC_MESH_MPM_ACTION_CONFIRM, 0x6666, again, 0, 1);
        CHECK(umac_datapath_mesh_peer_count() == 1,
              "the restarted peer re-establishes (count=%u)", umac_datapath_mesh_peer_count());
    }

    /* ---- 7. Close tears the link down and re-arms peering --------------- */
    reset_node();
    {
        uint16_t ours = peer_up(A, 0x7777);
        CHECK(ours != 0 && umac_datapath_mesh_peer_count() == 1, "A is established");

        simnode_outbox_clear();
        uint32_t cr0 = g_warthog_mpm_close_rx;
        rx_mpm(A, UMAC_MESH_MPM_ACTION_CLOSE, 0x7777, ours, 52, 0);
        CHECK(g_warthog_mpm_close_rx == cr0 + 1, "the Close is counted");
        CHECK(g_warthog_mpm_close_reason == 52,
              "the peer's reason code is recorded for AT+MPMSTAT? (got %u)",
              (unsigned)g_warthog_mpm_close_reason);
        CHECK(umac_datapath_mesh_peer_count() == 0,
              "the datapath peer is gone (count=%u)", umac_datapath_mesh_peer_count());
        CHECK(g_warthog_mpm_estab == 0, "and estab is cleared (estab=%u)",
              (unsigned)g_warthog_mpm_estab);
        /* NOT asserted here: that AT+MPMPEERS? stops showing the link. The
         * Close path releases the table entry but never re-renders
         * g_warthog_mpm_links, so the operator-visible string keeps the dead
         * link at estab=1 while AT+MPMSTAT? reports estab=0. Reported rather
         * than pinned -- asserting the stale string would bless the defect. */

        /* Latching ESTAB through a Close is what wedged peering until reboot:
         * maybe_initiate_mpm early-returns on it and nothing else reopens. */
        simnode_outbox_clear();
        rx_mpm(A, UMAC_MESH_MPM_ACTION_OPEN, 0x7778, 0, 0, 0);
        uint16_t fresh = 0;
        CHECK(mpm_ids(mpm_nth(UMAC_MESH_MPM_ACTION_CONFIRM, 0), &fresh, NULL),
              "peering is re-armed: A's next Open is answered");
        CHECK(fresh != 0 && fresh != ours,
              "with a FRESH llid, not the dead link's (0x%04x vs 0x%04x)", fresh, ours);
    }

    /* ---- 8. A neighbour too many is refused with Close, not silence ----- */
    reset_node();
    {
        const uint8_t *full[4] = { A, B, C, D };
        for (int i = 0; i < 4; i++)
        {
            rx_mpm(full[i], UMAC_MESH_MPM_ACTION_OPEN, (uint16_t)(0x8000 + i), 0, 0, 0);
        }
        CHECK(mpm_count(UMAC_MESH_MPM_ACTION_CONFIRM) == 4,
              "MPM_MAX_LINKS (%u) neighbours are all answered (got %u)",
              (unsigned)MPM_MAX_LINKS, mpm_count(UMAC_MESH_MPM_ACTION_CONFIRM));

        /* Every link must carry its OWN id pair: one shared pair only ever
         * describes two nodes, and that bug took down a three-board mesh. */
        uint16_t ids[4] = { 0, 0, 0, 0 };
        bool distinct = true, nonzero = true;
        for (unsigned i = 0; i < 4; i++)
        {
            mpm_ids(mpm_nth(UMAC_MESH_MPM_ACTION_CONFIRM, i), &ids[i], NULL);
            if (ids[i] == 0) { nonzero = false; }
            for (unsigned j = 0; j < i; j++) { if (ids[i] == ids[j]) { distinct = false; } }
        }
        CHECK(nonzero, "every link minted a non-zero llid (%04x %04x %04x %04x)",
              ids[0], ids[1], ids[2], ids[3]);
        CHECK(distinct, "and each neighbour got a DISTINCT one (%04x %04x %04x %04x)",
              ids[0], ids[1], ids[2], ids[3]);

        simnode_outbox_clear();
        uint32_t ns0 = g_warthog_mpm_no_slot, ct0 = g_warthog_mpm_close_tx;
        rx_mpm(E, UMAC_MESH_MPM_ACTION_OPEN, 0x9999, 0, 0, 0);
        CHECK(g_warthog_mpm_no_slot == ns0 + 1,
              "the fifth neighbour is counted as a refusal (%u -> %u)",
              (unsigned)ns0, (unsigned)g_warthog_mpm_no_slot);
        CHECK(g_warthog_mpm_close_tx == ct0 + 1, "and answered with a Close, not silence");
        const struct simnode_frame *cl = mpm_nth(UMAC_MESH_MPM_ACTION_CLOSE, 0);
        CHECK(cl != NULL && memcmp(frame_da(cl), E, 6) == 0, "the Close is addressed to E");
        CHECK(mpm_count(UMAC_MESH_MPM_ACTION_CONFIRM) == 0,
              "and no Confirm was sent to a peer we have no slot for (got %u)",
              mpm_count(UMAC_MESH_MPM_ACTION_CONFIRM));
        if (cl != NULL)
        {
            uint16_t reason = 0, plid = 0;
            const uint8_t *b = cl->bytes + DOT11_MGMT_HDR_LEN;
            uint32_t n = cl->len - DOT11_MGMT_HDR_LEN;
            /* The LITERAL, not UMAC_MESH_REASON_MAX_PEERS: asserting against
             * the constant follows it wherever it goes, which is how it sat
             * at 52 (MESH-PEERING-CANCELLED) unnoticed. Linux reads 53 as
             * MESH-MAX-PEERS and backs off; 52 tells it to retry at once. */
            CHECK(umac_mesh_ies_get_close_reason(b, n, &reason) && reason == 53u,
                  "it states why: reason 53, MESH-MAX-PEERS on the wire (got %u)", reason);
            mpm_ids(cl, NULL, &plid);
            CHECK(plid == 0x9999,
                  "and cites the requester's own link id so it can match it (got 0x%04x)", plid);
        }
    }

    /* ---- 9. Unanswered Opens give up, Close, and start over ------------- */
    reset_node();
    {
        /* A probe request from A is what drives maybe_initiate_mpm; A never
         * answers, so the retry counter climbs. */
        for (int i = 0; i < 8; i++) { prod_(A); }
        CHECK(mpm_count(UMAC_MESH_MPM_ACTION_OPEN) == 8,
              "eight unanswered probes produce eight Opens (got %u)",
              mpm_count(UMAC_MESH_MPM_ACTION_OPEN));
        uint16_t first = 0;
        mpm_ids(mpm_nth(UMAC_MESH_MPM_ACTION_OPEN, 0), &first, NULL);
        CHECK(mpm_count(UMAC_MESH_MPM_ACTION_CLOSE) == 0,
              "and no Close yet (got %u)", mpm_count(UMAC_MESH_MPM_ACTION_CLOSE));

        simnode_outbox_clear();
        prod_(A); /* the ninth */
        const struct simnode_frame *cl = mpm_nth(UMAC_MESH_MPM_ACTION_CLOSE, 0);
        CHECK(cl != NULL, "the ninth gives up and sends Close so the peer drops its half");
        CHECK(mpm_count(UMAC_MESH_MPM_ACTION_OPEN) == 0,
              "no further Open on that attempt (got %u)", mpm_count(UMAC_MESH_MPM_ACTION_OPEN));
        if (cl != NULL)
        {
            uint16_t plid = 0;
            mpm_ids(cl, NULL, &plid);
            CHECK(plid == first,
                  "the Close cites the llid the peer recorded for us (0x%04x vs 0x%04x)",
                  plid, first);
        }

        simnode_outbox_clear();
        prod_(A); /* first sighting again */
        uint16_t after = 0;
        CHECK(mpm_ids(mpm_nth(UMAC_MESH_MPM_ACTION_OPEN, 0), &after, NULL),
              "the link was forgotten, so the next sighting re-opens");
        CHECK(after != 0 && after != first,
              "with a fresh llid (0x%04x vs 0x%04x)", after, first);
    }

    /* ---- 10. The peering watchdog drops a peer that went silent --------- */
    reset_node();
    {
        uint16_t ours = peer_up(A, 0xaaaa);
        CHECK(ours != 0 && umac_datapath_mesh_peer_count() == 1, "A is established");

        uint32_t exp0 = g_warthog_mpm_expired;
        simnode_advance_ms(29000);
        prod_(P); /* runs mpm_expire_stale_ at now = t+29 s */
        CHECK(umac_datapath_mesh_peer_count() == 1,
              "at 29 s A is still inside the 30 s timeout (count=%u)",
              umac_datapath_mesh_peer_count());
        CHECK(strstr((const char *)g_warthog_mpm_links, "estab=1") != NULL,
              "and still ESTAB: %s", (const char *)g_warthog_mpm_links);

        simnode_advance_ms(2000);
        prod_(P); /* now = t+31 s */
        CHECK(g_warthog_mpm_expired == exp0 + 1,
              "past 30 s the watchdog expires it (%u -> %u)",
              (unsigned)exp0, (unsigned)g_warthog_mpm_expired);
        CHECK(umac_datapath_mesh_peer_count() == 0,
              "and tears down its datapath peer -- nothing else notices a board "
              "that lost power (count=%u)", umac_datapath_mesh_peer_count());
        CHECK(g_warthog_mpm_estab == 0, "estab drops to %u", (unsigned)g_warthog_mpm_estab);
        CHECK(strstr((const char *)g_warthog_mpm_links, "00000a") == NULL,
              "A is out of the table: %s", (const char *)g_warthog_mpm_links);
        CHECK(strstr((const char *)g_warthog_mpm_links, "000050") != NULL,
              "while the neighbour we just heard from is not: %s",
              (const char *)g_warthog_mpm_links);
    }

    /* ---- 11. Hearing from a peer refreshes its liveness ----------------- */
    reset_node();
    {
        CHECK(peer_up(A, 0xbbbb) != 0 && umac_datapath_mesh_peer_count() == 1,
              "A is established");
        simnode_advance_ms(20000);
        prod_(P);
        CHECK(umac_datapath_mesh_peer_count() == 1, "A survives 20 s of silence");

        /* One frame from A, and nothing else. */
        rx_mpm(A, UMAC_MESH_MPM_ACTION_OPEN, 0xbbbb, 0, 0, 0);
        simnode_advance_ms(20000);
        prod_(P); /* 40 s since peering, but only 20 s since we heard from A */
        CHECK(umac_datapath_mesh_peer_count() == 1,
              "a frame from A reset its timer: at 40 s total it is still up (count=%u)",
              umac_datapath_mesh_peer_count());
        CHECK(g_warthog_mpm_estab == 1, "and still ESTAB (estab=%u)",
              (unsigned)g_warthog_mpm_estab);

        simnode_advance_ms(31000);
        prod_(P);
        CHECK(umac_datapath_mesh_peer_count() == 0,
              "31 s after that last frame it does expire (count=%u)",
              umac_datapath_mesh_peer_count());
    }

    /* ---- 12. Losing a peer kills the paths that ran through it ---------- */
    reset_node();
    {
        CHECK(peer_up(A, 0xcccc) != 0, "A is established");
        CHECK(peer_up(B, 0xdddd) != 0 && umac_datapath_mesh_peer_count() == 2,
              "and so is B (count=%u)", umac_datapath_mesh_peer_count());

        simnode_outbox_clear();
        CHECK(install_path_via(A, R1, 10), "a PREQ from A on R1's behalf is accepted");
        CHECK(install_path_via(B, R2, 20), "and one from B on R2's behalf");

        char paths[1024];
        simnode_render_paths(paths, sizeof(paths));
        CHECK(strstr(paths, "dst=0000a1 via=00000a") != NULL,
              "the table holds R1 via A:\n%s", paths);
        CHECK(strstr(paths, "dst=0000a2 via=00000b") != NULL, "and R2 via B");

        simnode_outbox_clear();
        rx_mpm(A, UMAC_MESH_MPM_ACTION_CLOSE, 0xcccc, 0, 52, 0); /* A leaves */
        CHECK(umac_datapath_mesh_peer_count() == 1,
              "A is gone from the data plane, B is not (count=%u)",
              umac_datapath_mesh_peer_count());

        simnode_render_paths(paths, sizeof(paths));
        char line[256];
        const char *r1 = path_line(paths, "dst=0000a1", line, sizeof(line));
        CHECK(r1 != NULL && strstr(r1, "dead") != NULL,
              "the path through A is marked dead:\n%s", paths);
        char line2[256];
        const char *r2 = path_line(paths, "dst=0000a2 via=00000b", line2, sizeof(line2));
        CHECK(r2 != NULL && strstr(r2, "active") != NULL,
              "while the path through B is untouched: %s", line2);

        const struct simnode_frame *perr = hwmp_bcast(HWMP_EID_PERR);
        CHECK(perr != NULL, "and the loss is announced with a broadcast PERR");
        if (perr != NULL)
        {
            struct hwmp_perr pe;
            memset(&pe, 0, sizeof(pe));
            bool ok = umac_mesh_hwmp_parse_perr(perr->bytes + DOT11_MGMT_HDR_LEN,
                                                (uint16_t)(perr->len - DOT11_MGMT_HDR_LEN), &pe);
            CHECK(ok, "the firmware's own PERR parser accepts what the firmware built");
            CHECK(ok && memcmp(pe.dest_addr, R1, 6) == 0,
                  "it names the destination that became unreachable");
            CHECK(ok && pe.dest_sn == 11u,
                  "at the destination's sn PLUS ONE, or every holder rejects it as "
                  "not newer (got %lu, want 11)", (unsigned long)pe.dest_sn);
            CHECK(ok && pe.reason == HWMP_REASON_MESH_PATH_ERROR_DEST_UNREACHABLE,
                  "with reason DEST_UNREACHABLE (got %u)", pe.reason);
        }
    }

    /* ---- 14. An Open naming another Mesh ID creates nothing ---------------
     *
     * 802.11s peers only inside one mesh profile. This Open used to be answered
     * with our Open and Confirm and given a link, and the Confirm that followed
     * a datapath peer: two open meshes with different Mesh IDs on one channel
     * merged. Sections 14 to 21 each use addresses no other section does. */
    printf("--- 14. an Open naming another Mesh ID is refused with Close(54) ---\n");
    reset_node();
    {
        uint8_t x[6];
        xaddr_(x, 0x14);
        const uint32_t rx0 = g_warthog_mpm_rx, fo0 = g_warthog_mpm_foreign;
        const uint32_t ct0 = g_warthog_mpm_close_tx, ns0 = g_warthog_mpm_no_slot;
        simnode_outbox_clear();
        CHECK(rx_mpm_as(x, UMAC_MESH_MPM_ACTION_OPEN, 0x1111, 0, 0, OTHERID, sizeof(OTHERID), false),
              "an Open naming the mesh \"othermesh\" is delivered");
        CHECK(g_warthog_mpm_rx == rx0 + 1 && g_warthog_mpm_foreign == fo0 + 1,
              "it is counted as received and as foreign (rx +%u, foreign +%u)",
              (unsigned)(g_warthog_mpm_rx - rx0), (unsigned)(g_warthog_mpm_foreign - fo0));
        CHECK(refused_54_(x, 0x1111),
              "and answered with one Close(54) citing its link id: no Open, no Confirm, no link");
        CHECK(g_warthog_mpm_close_tx == ct0 + 1 && g_warthog_mpm_no_slot == ns0,
              "the Close is counted in close_tx, and no_slot is left alone (+%u, %u -> %u)",
              (unsigned)(g_warthog_mpm_close_tx - ct0), (unsigned)ns0,
              (unsigned)g_warthog_mpm_no_slot);
        CHECK(umac_datapath_mesh_peer_count() == 0, "no datapath peer (count=%u)",
              umac_datapath_mesh_peer_count());
        simnode_outbox_clear();
        rx_mpm(x, UMAC_MESH_MPM_ACTION_OPEN, 0x1112, 0, 0, 0);
        CHECK(mpm_count(UMAC_MESH_MPM_ACTION_OPEN) == 1 &&
              mpm_count(UMAC_MESH_MPM_ACTION_CONFIRM) == 1,
              "the same sender on our Mesh ID is then answered with one Open and one Confirm: "
              "the refusal left nothing behind (%u, %u)",
              mpm_count(UMAC_MESH_MPM_ACTION_OPEN), mpm_count(UMAC_MESH_MPM_ACTION_CONFIRM));
    }

    /* ---- 15. Our Mesh ID with another authentication protocol ------------ */
    printf("--- 15. our Mesh ID with the SAE profile is another mesh profile ---\n");
    reset_node();
    {
        uint8_t x[6];
        xaddr_(x, 0x15);
        const uint32_t fo0 = g_warthog_mpm_foreign;
        simnode_outbox_clear();
        rx_mpm_as(x, UMAC_MESH_MPM_ACTION_OPEN, 0x1511, 0, 0, MESHID, sizeof(MESHID), true);
        CHECK(g_warthog_mpm_foreign == fo0 + 1,
              "an Open naming our Mesh ID and authentication protocol 1 is counted as foreign (+%u)",
              (unsigned)(g_warthog_mpm_foreign - fo0));
        CHECK(refused_54_(x, 0x1511), "and refused with one Close(54), no link");
    }

    /* ---- 16. An Open with a mesh element missing or malformed ------------ */
    printf("--- 16. an Open with a mesh element missing or malformed is refused ---\n");
    reset_node();
    {
        static const uint8_t CFG[9] = { UMAC_MESH_EID_MESH_CONFIG, 0x07, 0x01, 0x01, 0x00,
                                        0x01, 0x00, 0x00, 0x01 };
        static const char *const what[4] = {
            "(a) a Mesh Configuration and no Mesh ID",
            "(b) our Mesh ID and no Mesh Configuration",
            "(c) a 33-octet Mesh ID beginning with ours, then the Mesh Configuration",
            "(d) a zero-length Mesh ID, then the Mesh Configuration",
        };
        uint8_t ies[4][48];
        uint16_t len[4];
        memcpy(ies[0], CFG, sizeof(CFG));
        len[0] = (uint16_t)sizeof(CFG);
        ies[1][0] = UMAC_MESH_EID_MESH_ID;
        ies[1][1] = (uint8_t)sizeof(MESHID);
        memcpy(&ies[1][2], MESHID, sizeof(MESHID));
        len[1] = (uint16_t)(2u + sizeof(MESHID));
        ies[2][0] = UMAC_MESH_EID_MESH_ID;
        ies[2][1] = 33;
        memset(&ies[2][2], 'x', 33);
        memcpy(&ies[2][2], MESHID, sizeof(MESHID));
        memcpy(&ies[2][35], CFG, sizeof(CFG));
        len[2] = (uint16_t)(35u + sizeof(CFG));
        ies[3][0] = UMAC_MESH_EID_MESH_ID;
        ies[3][1] = 0;
        memcpy(&ies[3][2], CFG, sizeof(CFG));
        len[3] = (uint16_t)(2u + sizeof(CFG));
        for (unsigned i = 0; i < 4; i++)
        {
            uint8_t x[6];
            const uint16_t llid = (uint16_t)(0x1610 + i);
            xaddr_(x, (uint8_t)(0x60 + i));
            const uint32_t fo0 = g_warthog_mpm_foreign;
            simnode_outbox_clear();
            rx_open_ies(x, ies[i], len[i], llid);
            CHECK(g_warthog_mpm_foreign == fo0 + 1 && refused_54_(x, llid),
                  "%s: counted as foreign (+%u) and refused with one Close(54), no link", what[i],
                  (unsigned)(g_warthog_mpm_foreign - fo0));
        }

        /* (e) ends inside the Mesh ID element and has no Peer Management element. */
        static const uint8_t cut[9] = { UMAC_MESH_MPM_CATEGORY, UMAC_MESH_MPM_ACTION_OPEN, 0, 0,
                                        UMAC_MESH_EID_MESH_ID, 0x07, 's', 'i', 'm' };
        uint8_t x[6];
        xaddr_(x, 0x64);
        const uint32_t fo0 = g_warthog_mpm_foreign, pf0 = g_warthog_mpm_parse_fail;
        simnode_outbox_clear();
        rx_mgmt(FC_ACTION, x, cut, (uint16_t)sizeof(cut));
        CHECK(g_warthog_mpm_foreign == fo0 + 1 && g_warthog_mpm_parse_fail == pf0 + 1,
              "(e) an Open cut inside its Mesh ID element: counted as foreign and as a parse "
              "failure (+%u, +%u)", (unsigned)(g_warthog_mpm_foreign - fo0),
              (unsigned)(g_warthog_mpm_parse_fail - pf0));
        CHECK(simnode_outbox_count() == 0 && umac_mesh_note_s1g_beacon(x),
              "and answered with no frame at all, there being no link id to cite; no link "
              "(%u frames)", simnode_outbox_count());
    }

    /* ---- 17. A Confirm naming another mesh ------------------------------- */
    printf("--- 17. a Confirm naming another mesh neither creates nor establishes a link ---\n");
    reset_node();
    {
        uint8_t x[6];
        xaddr_(x, 0x17);
        uint32_t fo0 = g_warthog_mpm_foreign;
        simnode_outbox_clear();
        rx_mpm_as(x, UMAC_MESH_MPM_ACTION_CONFIRM, 0x1711, 0x2222, 1, OTHERID, sizeof(OTHERID), false);
        CHECK(g_warthog_mpm_foreign == fo0 + 1 && simnode_outbox_count() == 0,
              "(a) a stranger's Confirm naming another mesh is counted and answered with nothing "
              "(+%u, %u frames)", (unsigned)(g_warthog_mpm_foreign - fo0), simnode_outbox_count());
        CHECK(umac_mesh_note_s1g_beacon(x) && umac_datapath_mesh_peer_count() == 0,
              "it made no link, as an unsolicited Confirm used to, and no datapath peer (count=%u)",
              umac_datapath_mesh_peer_count());

        simnode_outbox_clear();
        prod_(A);
        uint16_t ours = 0;
        CHECK(mpm_ids(mpm_nth(UMAC_MESH_MPM_ACTION_OPEN, 0), &ours, NULL) && ours != 0,
              "(b) we open toward A with llid 0x%04x", ours);
        fo0 = g_warthog_mpm_foreign;
        simnode_outbox_clear();
        rx_mpm_as(A, UMAC_MESH_MPM_ACTION_CONFIRM, 0x1722, ours, 1, OTHERID, sizeof(OTHERID), false);
        CHECK(g_warthog_mpm_foreign == fo0 + 1 && simnode_outbox_count() == 0,
              "a Confirm from A echoing that llid but naming another mesh is counted and answered "
              "with nothing (+%u, %u frames)", (unsigned)(g_warthog_mpm_foreign - fo0),
              simnode_outbox_count());
        CHECK(g_warthog_mpm_estab == 0 && umac_datapath_mesh_peer_count() == 0,
              "and does not establish the link (estab=%u, count=%u)",
              (unsigned)g_warthog_mpm_estab, umac_datapath_mesh_peer_count());
        simnode_outbox_clear();
        rx_mpm(A, UMAC_MESH_MPM_ACTION_OPEN, 0x1722, 0, 0, 0);
        uint16_t again = 0;
        CHECK(mpm_ids(mpm_nth(UMAC_MESH_MPM_ACTION_CONFIRM, 0), &again, NULL) && again == ours,
              "A's Open on our mesh is answered with a Confirm carrying the same llid: the link "
              "was not disturbed (0x%04x vs 0x%04x)", again, ours);
        rx_mpm(A, UMAC_MESH_MPM_ACTION_CONFIRM, 0x1722, ours, 0, 1);
        CHECK(umac_datapath_mesh_peer_count() == 1,
              "and A's Confirm on our mesh establishes it (count=%u)",
              umac_datapath_mesh_peer_count());
    }

    /* ---- 18. Foreign Opens take no link and evict nobody ----------------- */
    printf("--- 18. foreign Opens take none of the four links and evict nobody ---\n");
    reset_node();
    {
        const uint32_t ns0 = g_warthog_mpm_no_slot;
        unsigned refused = 0;
        for (unsigned i = 0; i < 5; i++)
        {
            uint8_t x[6];
            const uint16_t llid = (uint16_t)(0x1810 + i);
            xaddr_(x, (uint8_t)(0x80 + i));
            simnode_outbox_clear();
            rx_mpm_as(x, UMAC_MESH_MPM_ACTION_OPEN, llid, 0, 0, OTHERID, sizeof(OTHERID), false);
            refused += refused_54_(x, llid);
        }
        CHECK(refused == 5 && g_warthog_mpm_no_slot == ns0,
              "(a) five strangers' foreign Opens are each refused with Close(54), and none is "
              "counted in no_slot (%u refused, no_slot %u -> %u)", refused, (unsigned)ns0,
              (unsigned)g_warthog_mpm_no_slot);
        CHECK(peer_up(A, 0x18a0) != 0 && peer_up(B, 0x18b0) != 0 && peer_up(C, 0x18c0) != 0 &&
              peer_up(D, 0x18d0) != 0 && umac_datapath_mesh_peer_count() == 4,
              "and all four links are left for real peers: A, B, C and D reach ESTAB (count=%u)",
              umac_datapath_mesh_peer_count());
    }
    reset_node();
    {
        CHECK(peer_up(A, 0x18a1) != 0 && peer_up(B, 0x18b1) != 0 && peer_up(C, 0x18c1) != 0,
              "(b) A, B and C are established");
        prod_(P);
        CHECK(strstr((const char *)g_warthog_mpm_links, "000050") != NULL,
              "P's probe request drew our Open: it holds the fourth link, unanswered: %s",
              (const char *)g_warthog_mpm_links);
        simnode_outbox_clear();
        rx_mpm_as(E, UMAC_MESH_MPM_ACTION_OPEN, 0x18e1, 0, 0, OTHERID, sizeof(OTHERID), false);
        unsigned to_p = 0;
        for (unsigned i = 0; mpm_nth(UMAC_MESH_MPM_ACTION_CLOSE, i) != NULL; i++)
        {
            to_p += (memcmp(frame_da(mpm_nth(UMAC_MESH_MPM_ACTION_CLOSE, i)), P, 6) == 0);
        }
        CHECK(to_p == 0 && refused_54_(E, 0x18e1),
              "a foreign Open from E: one Close(54) to E, and no Close to P (%u)", to_p);
        prod_(P);
        char entry[64] = "";
        const char *at = strstr((const char *)g_warthog_mpm_links, "000050");
        if (at != NULL)
        {
            size_t n = strcspn(at, ";");
            if (n >= sizeof(entry)) { n = sizeof(entry) - 1u; }
            memcpy(entry, at, n);
            entry[n] = '\0';
        }
        CHECK(strstr(entry, "opens=2") != NULL,
              "P keeps its link, and its next probe request draws our second Open on it: %s",
              (const char *)g_warthog_mpm_links);
    }

    /* ---- 19. A foreign Open from a peer's own address --------------------- */
    printf("--- 19. a foreign Open from a peer's address neither ends nor refreshes its link ---\n");
    reset_node();
    {
        CHECK(peer_up(A, 0x1911) != 0 && umac_datapath_mesh_peer_count() == 1, "A is established");
        simnode_advance_ms(20000);
        const uint32_t fo0 = g_warthog_mpm_foreign, ex0 = g_warthog_mpm_expired;
        simnode_outbox_clear();
        rx_mpm_as(A, UMAC_MESH_MPM_ACTION_OPEN, 0x1999, 0, 0, OTHERID, sizeof(OTHERID), false);
        CHECK(g_warthog_mpm_foreign == fo0 + 1 && umac_datapath_mesh_peer_count() == 1,
              "20 s on, an Open from A's address with a new llid and another Mesh ID is counted "
              "and the link stays up (+%u, count=%u)", (unsigned)(g_warthog_mpm_foreign - fo0),
              umac_datapath_mesh_peer_count());
        CHECK(simnode_outbox_count() == 0,
              "a sender we hold a link with is sent nothing: a peer in ESTAB takes any Close as "
              "the end of the link (%u frames)", simnode_outbox_count());
        simnode_advance_ms(11000);
        simnode_tick();
        CHECK(umac_datapath_mesh_peer_count() == 0 && g_warthog_mpm_expired == ex0 + 1,
              "nor was it a sign of life: A expires 31 s after its last matching frame "
              "(count=%u, expired +%u)", umac_datapath_mesh_peer_count(),
              (unsigned)(g_warthog_mpm_expired - ex0));
    }

    /* ---- 20. A stranger's Close(53) --------------------------------------- */
    printf("--- 20. a stranger's Close(53) takes no hold-off entry ---\n");
    reset_node();
    {
        const uint32_t cr0 = g_warthog_mpm_close_rx;
        for (unsigned i = 0; i < 4; i++)
        {
            uint8_t x[6];
            xaddr_(x, (uint8_t)(0xa0 + i));
            rx_mpm(x, UMAC_MESH_MPM_ACTION_CLOSE, 0x9000, 0, 53, 0);
        }
        CHECK(g_warthog_mpm_close_rx == cr0 + 4 && g_warthog_mpm_close_reason == 53,
              "four strangers each send Close(53): each is counted and the reason recorded "
              "(close_rx +%u, reason %u)", (unsigned)(g_warthog_mpm_close_rx - cr0),
              (unsigned)g_warthog_mpm_close_reason);
        simnode_outbox_clear();
        prod_(E);
        CHECK(mpm_count(UMAC_MESH_MPM_ACTION_OPEN) == 1,
              "and nobody is held off by them: E's probe request still draws our Open (%u); "
              "with the four hold-off entries taken it drew none",
              mpm_count(UMAC_MESH_MPM_ACTION_OPEN));
    }

    /* ---- 21. Probe requests that do not name our mesh --------------------
     *
     * simnode_rx() copies each frame into a packet of its exact length, so the
     * cut ones end where the firmware's walks must stop. The node is reset for
     * each: an Open drawn by one must not fill the link table for the next. */
    printf("--- 21. a probe request that does not name our mesh is answered and draws no Open ---\n");
    reset_node();
    {
        static const struct { uint8_t ies[13]; uint8_t len; uint32_t other; const char *what; } pr[] = {
            { { 0x00, 0x00 }, 2, 0, "(a) a wildcard probe request, a scanner's" },
            { { 0x00, 0x00, 0x72, 0x09, 'o', 't', 'h', 'e', 'r', 'm', 'e', 's', 'h' }, 13, 1,
              "(b) one naming the mesh \"othermesh\"" },
            { { 0x00, 0x00, 0x72, 0x20, 'o', 't' }, 6, 0,
              "(c) one whose Mesh ID length runs past the end" },
            { { 0x00, 0x00, 0x72 }, 3, 0, "(d) one cut inside an element header" },
            { { 0x72, 0x07, 's', 'i', 'm', 'n', 'o', 'd' }, 8, 0,
              "(e) one naming our mesh, cut one octet short" },
        };
        for (unsigned i = 0; i < sizeof(pr) / sizeof(pr[0]); i++)
        {
            uint8_t x[6];
            xaddr_(x, (uint8_t)(0xb0 + i));
            const uint32_t po0 = g_warthog_prq_other;
            reset_node();
            rx_mgmt(FC_PROBE_REQ, x, pr[i].ies, pr[i].len);
            CHECK(count_fc_to(FC_PROBE_RSP, x) == 1 && mpm_count(UMAC_MESH_MPM_ACTION_OPEN) == 0 &&
                  umac_mesh_note_s1g_beacon(x),
                  "%s: one probe response, no Open, no link (%u, %u)", pr[i].what,
                  count_fc_to(FC_PROBE_RSP, x), mpm_count(UMAC_MESH_MPM_ACTION_OPEN));
            CHECK(g_warthog_prq_other == po0 + pr[i].other,
                  "    and prq_other counts it %u time(s) (+%u)", (unsigned)pr[i].other,
                  (unsigned)(g_warthog_prq_other - po0));
        }

        CHECK(peer_up(A, 0x2111) != 0 && umac_datapath_mesh_peer_count() == 1, "A is established");
        simnode_advance_ms(20000);
        simnode_outbox_clear();
        rx_mgmt(FC_PROBE_REQ, A, pr[0].ies, pr[0].len);
        rx_mgmt(FC_PROBE_REQ, A, pr[1].ies, pr[1].len);
        CHECK(count_fc_to(FC_PROBE_RSP, A) == 2 && umac_datapath_mesh_peer_count() == 1,
              "20 s on, (a) and (b) from A's own address are each answered (%u)",
              count_fc_to(FC_PROBE_RSP, A));
        simnode_advance_ms(11000);
        simnode_tick();
        CHECK(umac_datapath_mesh_peer_count() == 0,
              "and did not refresh A's link: it expires 31 s after its last matching frame "
              "(count=%u)", umac_datapath_mesh_peer_count());
    }
    printf("--- 13. nothing fell through into code the simulator does not model ---\n");

    /* ---- 13. Nothing fell through into code the simulator does not model  */
    CHECK(simnode_sta_mode_hits() == 0,
          "peering never entered STA-mode connection code");
    CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 0,
          "and never reached hostap's SAE peering, which is stubbed here");

    simnode_stop();

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_simnode_peering: all passed\n");
    return 0;
}
