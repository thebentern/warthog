/*
 * umac_mesh_fwd_glue.c, end to end, on the real stack.
 *
 * The glue is the one mesh translation unit no freestanding test can link: it
 * needs mmosal, mmpkt, mmdrv and the datapath, so until the simulator existed
 * its behaviour was checked structurally by test_glue_guard.sh and not at all
 * by execution. Everything here runs the SHIPPING file -- held frames, the
 * discovery retry, the bounded pending queue, the PERR rate gate, the
 * forwarding queue cap, the peer-loss PERR burst -- and asserts wherever
 * possible on the bytes the firmware handed to the chip, parsed back with the
 * firmware's own parsers.
 *
 * What it cannot tell you: nothing below mmdrv_tx_frame() exists here. A green
 * run means the glue's own logic is consistent, not that the mesh works on air.
 *
 * Two mechanics worth knowing before reading the phases:
 *
 *   - simnode_set_gates() calls umac_mesh_fwd_glue_init(), which re-initialises
 *     the path table, the RMC, both rate gates, the protection latch and the
 *     pending queue. Phases that change a gate therefore start from an empty
 *     table on purpose, and every assertion is on a DELTA of the AT+MESHFWDSTAT
 *     counters rather than an absolute, because those counters are never reset.
 *   - the PREQ gate has a global floor (UMAC_MESH_PREQ_GLOBAL_MIN_MS, 50 ms) as
 *     well as a per-target interval, so one flush emits at most one PREQ no
 *     matter how many targets are waiting. That is asserted, not worked around.
 */
#include <stdio.h>
#include <string.h>

#include "simnode.h"

#include "mmpkt.h"
#include "mmwlan.h"
#include "mmdrv.h"
#include "umac/data/umac_data.h"
#include "umac/datapath/umac_datapath.h"
#include "umac/datapath/umac_datapath_private.h"
#include "umac/mesh/umac_mesh_fwd.h"
#include "umac/mesh/umac_mesh_fwd_glue.h"
#include "umac/mesh/umac_mesh_ctrl.h"
#include "umac/mesh/umac_mesh_hwmp.h"
#include "umac/mesh/umac_mesh_ies.h"

static int failures;
static int checks;
#define CHECK(cond, ...) do { \
    checks++; \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* ---- the counters AT+MESHFWDSTAT? prints -------------------------------- */
extern volatile uint32_t g_warthog_fwd_uni, g_warthog_fwd_grp, g_warthog_fwd_nomem;
extern volatile uint32_t g_warthog_fwd_drop_own, g_warthog_fwd_drop_dup, g_warthog_fwd_drop_ttl;
extern volatile uint32_t g_warthog_fwd_drop_nopath, g_warthog_fwd_drop_nofwd,
    g_warthog_fwd_drop_bad;
extern volatile uint32_t g_warthog_fwd_drop_full;
extern volatile uint32_t g_warthog_fwd_perr_tx, g_warthog_fwd_perr_suppressed,
    g_warthog_fwd_preq_tx;
extern volatile uint32_t g_warthog_hwmp_relay_preq, g_warthog_hwmp_relay_prep,
    g_warthog_hwmp_relay_perr;
extern volatile uint32_t g_warthog_fwd_pend_tx, g_warthog_fwd_pend_drop;
extern volatile uint32_t g_warthog_hwmp_prot, g_warthog_hwmp_unprotected;
extern volatile uint32_t g_warthog_hwmp_mmie, g_warthog_hwmp_nommie;
/* The gates themselves, which is what AT+MESHFWD etc. write. */
extern volatile uint32_t g_warthog_mesh_fwd, g_warthog_mesh_bridge;

/* A snapshot of every one of them, so each phase asserts on its own delta. */
struct stat_snap {
    uint32_t uni, grp, nomem;
    uint32_t d_own, d_dup, d_ttl, d_nopath, d_nofwd, d_bad, d_full;
    uint32_t perr_tx, perr_supp, preq_tx;
    uint32_t r_preq, r_prep, r_perr;
    uint32_t pend_tx, pend_drop;
    uint32_t prot, unprot, mmie, nommie;
};

static void snap(struct stat_snap *s)
{
    s->uni = g_warthog_fwd_uni; s->grp = g_warthog_fwd_grp; s->nomem = g_warthog_fwd_nomem;
    s->d_own = g_warthog_fwd_drop_own; s->d_dup = g_warthog_fwd_drop_dup;
    s->d_ttl = g_warthog_fwd_drop_ttl; s->d_nopath = g_warthog_fwd_drop_nopath;
    s->d_nofwd = g_warthog_fwd_drop_nofwd; s->d_bad = g_warthog_fwd_drop_bad;
    s->d_full = g_warthog_fwd_drop_full;
    s->perr_tx = g_warthog_fwd_perr_tx; s->perr_supp = g_warthog_fwd_perr_suppressed;
    s->preq_tx = g_warthog_fwd_preq_tx;
    s->r_preq = g_warthog_hwmp_relay_preq; s->r_prep = g_warthog_hwmp_relay_prep;
    s->r_perr = g_warthog_hwmp_relay_perr;
    s->pend_tx = g_warthog_fwd_pend_tx; s->pend_drop = g_warthog_fwd_pend_drop;
    s->prot = g_warthog_hwmp_prot; s->unprot = g_warthog_hwmp_unprotected;
    s->mmie = g_warthog_hwmp_mmie; s->nommie = g_warthog_hwmp_nommie;
}

/* ---- addresses ---------------------------------------------------------- */
static const uint8_t W[6]  = { 0x02, 0, 0, 0, 0, 0x01 }; /* us */
static const uint8_t A[6]  = { 0x02, 0, 0, 0, 0, 0x0a }; /* peer */
static const uint8_t B[6]  = { 0x02, 0, 0, 0, 0, 0x0b }; /* peer */
static const uint8_t C[6]  = { 0x02, 0, 0, 0, 0, 0x0c }; /* peer, added and lost */
static const uint8_t R[6]  = { 0x02, 0, 0, 0, 0, 0x21 }; /* two hops away, via A */
static const uint8_t S[6]  = { 0x02, 0, 0, 0, 0, 0x22 }; /* two hops away, via B */
static const uint8_t Z[6]  = { 0x02, 0, 0, 0, 0, 0x23 }; /* never routable */
static const uint8_t X[6]  = { 0x02, 0, 0, 0, 0, 0x31 }; /* a far originator */
static const uint8_t R2[6] = { 0x02, 0, 0, 0, 0, 0x32 };
static const uint8_t P1[6] = { 0x02, 0, 0, 0, 0, 0x41 }; /* reached via C */
static const uint8_t P2[6] = { 0x02, 0, 0, 0, 0, 0x42 };
static const uint8_t Q1[6] = { 0x02, 0, 0, 0, 0, 0x51 }; /* pending-queue targets */
static const uint8_t Q2[6] = { 0x02, 0, 0, 0, 0, 0x52 };
static const uint8_t Q3[6] = { 0x02, 0, 0, 0, 0, 0x53 };
static const uint8_t H[6]  = { 0x02, 0, 0, 0, 0, 0x61 }; /* a host behind us */
static const uint8_t NOPEER[6] = { 0x02, 0, 0, 0, 0, 0x7f };
static const uint8_t GRP[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x69 };

static const char *mac_(const uint8_t *m)
{
    static char b[4][20];
    static int k;
    k = (k + 1) & 3;
    snprintf(b[k], sizeof(b[k]), "%02x:%02x", m[4], m[5]);
    return b[k];
}

/* ---- building the frames a peer's radio would hand up ------------------- */

struct meshdata {
    const uint8_t *ra;       /* addr1: the next hop, or the group DA */
    const uint8_t *ta;       /* addr2 */
    const uint8_t *a3;       /* mesh DA (4-address) or mesh SA (group) */
    const uint8_t *a4;       /* mesh SA; NULL selects the 3-address group form */
    uint8_t  ttl;
    uint32_t seq;
    uint8_t  ae;
    const uint8_t *eaddr1, *eaddr2;
    uint16_t payload_len;
};

/* The 802.11 sequence control, stepped per frame so the reorder and defrag
 * paths see distinct frames rather than retransmissions of one. */
static uint16_t s_sn11;

static uint16_t build_mesh_data(uint8_t *out, uint16_t cap, const struct meshdata *d)
{
    uint16_t n;
    if (d->a4 != NULL)
    {
        n = umac_mesh_ies_build_data_hdr4(out, d->ra, d->ta, d->a3, d->a4);
    }
    else
    {
        n = umac_mesh_ies_build_data_hdr3_group(out, d->ra, d->ta, d->a3);
    }
    if (n == 0 || cap < n + 32u) { return 0; }
    s_sn11 = (uint16_t)(s_sn11 + 1u);
    uint16_t sc = (uint16_t)(s_sn11 << 4);
    out[22] = (uint8_t)(sc & 0xffu);
    out[23] = (uint8_t)(sc >> 8);
    /* QoS Control: TID 0, Mesh Control Present (bit 8). */
    out[n++] = 0x00;
    out[n++] = 0x01;
    struct umac_mesh_ctrl mc;
    memset(&mc, 0, sizeof(mc));
    mc.flags = d->ae;
    mc.ttl = d->ttl;
    mc.seq = d->seq;
    if (d->eaddr1 != NULL) { memcpy(mc.eaddr1, d->eaddr1, 6); }
    if (d->eaddr2 != NULL) { memcpy(mc.eaddr2, d->eaddr2, 6); }
    uint16_t mcn = umac_mesh_ctrl_build(out + n, (uint16_t)(cap - n), &mc);
    if (mcn == 0) { return 0; }
    n = (uint16_t)(n + mcn);
    static const uint8_t snap8[8] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00 };
    memcpy(out + n, snap8, sizeof(snap8));
    n = (uint16_t)(n + sizeof(snap8));
    for (uint16_t i = 0; i < d->payload_len && n < cap; i++) { out[n++] = (uint8_t)(0xa0 + i); }
    return n;
}

/** Inject one mesh data frame and run the event loop, as a received frame does. */
static bool rx_mesh(const struct meshdata *d)
{
    uint8_t f[320];
    uint16_t n = build_mesh_data(f, sizeof(f), d);
    if (n == 0) { return false; }
    return simnode_rx(f, n, -60);
}

/* ---- HWMP bodies -------------------------------------------------------- */

/** A PREP announcing @p target (the answerer) to @p orig (the asker). */
static uint16_t mk_prep(uint8_t *body, uint16_t cap, const uint8_t *target, uint32_t target_sn,
                        const uint8_t *orig, uint32_t orig_sn)
{
    struct hwmp_preq q;
    memset(&q, 0, sizeof(q));
    q.flags = 0;
    q.hop_count = 0;
    q.ttl = HWMP_DEFAULT_TTL;
    q.preq_id = 1;
    memcpy(q.orig_addr, orig, 6);
    q.orig_sn = orig_sn;
    q.lifetime = 5000;
    q.metric = 0;
    q.target_count = 1;
    q.target_flags = HWMP_TGT_FLAG_TO;
    memcpy(q.target_addr, target, 6);
    q.target_sn = 0;
    return umac_mesh_hwmp_build_prep(body, cap, &q, target, target_sn);
}

/** Install an active path to @p dst via peer @p via, the way a PREP answering
 *  our own PREQ does. Returns the sequence number the path now holds. */
static uint32_t install_path(const uint8_t *dst, const uint8_t *via, uint32_t sn)
{
    uint8_t body[HWMP_PREP_BODY_LEN];
    uint32_t own_sn = 1;
    uint16_t n = mk_prep(body, sizeof(body), dst, sn, W, 1);
    if (n != 0) { umac_mesh_fwd_glue_hwmp_rx(body, n, via, &own_sn, false, false); }
    return sn;
}

/* ---- reading the outbox ------------------------------------------------- */

/** The action body of a management frame: past the 24-byte PV0 MAC header. */
static const uint8_t *act_body(const struct simnode_frame *f, uint16_t *len_out)
{
    if (f == NULL || !f->is_mgmt || f->len <= 24u) { return NULL; }
    if (len_out != NULL) { *len_out = (uint16_t)(f->len - 24u); }
    return f->bytes + 24;
}

/** The i-th outbox frame carrying HWMP element @p eid, or NULL. */
static const struct simnode_frame *find_hwmp(uint8_t eid, unsigned nth)
{
    unsigned seen = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        uint16_t bl = 0;
        const uint8_t *b = act_body(f, &bl);
        if (b == NULL) { continue; }
        if (umac_mesh_hwmp_element_id(b, bl) != eid) { continue; }
        if (seen++ == nth) { return f; }
    }
    return NULL;
}

static unsigned count_hwmp(uint8_t eid)
{
    unsigned n = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        uint16_t bl = 0;
        const uint8_t *b = act_body(simnode_outbox_get(i), &bl);
        if (b != NULL && umac_mesh_hwmp_element_id(b, bl) == eid) { n++; }
    }
    return n;
}

static unsigned count_data(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (f != NULL && !f->is_mgmt) { n++; }
    }
    return n;
}

static const struct simnode_frame *nth_data(unsigned nth)
{
    unsigned seen = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (f != NULL && !f->is_mgmt && seen++ == nth) { return f; }
    }
    return NULL;
}

/** Parse an outbox data frame with the SHIPPING parser. */
static bool parse_data(const struct simnode_frame *f, struct umac_mesh_rx_frame *out)
{
    return f != NULL && !f->is_mgmt && umac_mesh_fwd_parse_frame(f->bytes, f->len, out) != 0;
}

/** The management frame's addr1. */
static const uint8_t *mgmt_da(const struct simnode_frame *f)
{
    static const uint8_t zero[6] = { 0 };
    return (f != NULL && f->len >= 10u) ? f->bytes + 4 : zero;
}

/** pending=N as AT+MESHPATH? reports it. */
static long render_pending(void)
{
    char buf[2048];
    int n = umac_mesh_fwd_glue_render(buf, sizeof(buf));
    if (n <= 0) { return -1; }
    buf[(unsigned)n < sizeof(buf) ? (unsigned)n : sizeof(buf) - 1] = '\0';
    const char *p = strstr(buf, "pending=");
    return (p != NULL) ? strtol(p + 8, NULL, 10) : -1;
}

static long render_paths_count(void)
{
    char buf[2048];
    int n = umac_mesh_fwd_glue_render(buf, sizeof(buf));
    if (n <= 0) { return -1; }
    buf[(unsigned)n < sizeof(buf) ? (unsigned)n : sizeof(buf) - 1] = '\0';
    const char *p = strstr(buf, "paths=");
    return (p != NULL) ? strtol(p + 6, NULL, 10) : -1;
}

/* ---- an 802.3 frame, for the direct tx_pending calls -------------------- */

static struct mmpkt *mk_8023(const uint8_t *da, const uint8_t *sa, uint16_t payload_len)
{
    struct mmpkt *p = umac_datapath_alloc_mmpkt_for_qos_data_tx(
        (uint32_t)payload_len + sizeof(struct umac_8023_hdr), MMDRV_PKT_CLASS_DATA_TID0);
    if (p == NULL) { return NULL; }
    struct umac_8023_hdr h;
    memcpy(h.dest_addr, da, 6);
    memcpy(h.src_addr, sa, 6);
    h.ethertype_be = htobe16(0x0800);
    struct mmpktview *v = mmpkt_open(p);
    mmpkt_append_data(v, (const uint8_t *)&h, sizeof(h));
    static const uint8_t pad[64] = { 0 };
    mmpkt_append_data(v, pad, payload_len <= sizeof(pad) ? payload_len : sizeof(pad));
    mmpkt_close(&v);
    return p;
}

/* ========================================================================= */

int main(void)
{
    printf("=== umac_mesh_fwd_glue.c on the real stack ===\n");

    struct stat_snap s0;
    struct umac_mesh_rx_frame pf;
    uint8_t body[128];
    uint32_t own_sn = 7;

    CHECK(simnode_start(W), "node up");
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/false);
    CHECK(simnode_add_peer(A) && simnode_add_peer(B), "peers %s and %s established",
          mac_(A), mac_(B));

    /* The first service tick runs the one-shot CCMP known-answer test, which
     * allocates; get it out of the way before the leak baseline is taken. */
    simnode_tick();
    simnode_outbox_clear();
    const unsigned base_allocs = simnode_live_allocs();
    printf("     leak baseline: %u live allocations\n", base_allocs);

    struct umac_data *umacd = umac_data_get_umacd();
    CHECK(umacd != NULL, "the datapath handle the glue is driven with exists");

    /* ---------------------------------------------------------------------
     * 1. tx_pending: HELD.
     *
     * The TX entry found no STA for a unicast destination it has no path to.
     * The glue must take the buffer rather than let the first frame of every
     * flow die, and the lookup that got there must have put a PREQ on air.
     * ------------------------------------------------------------------- */
    printf("\n--- 1. a frame for an undiscovered destination is HELD ---\n");
    snap(&s0);
    static const uint8_t payload[32] = { 0xde, 0xad, 0xbe, 0xef };
    CHECK(simnode_host_tx(R, W, payload, sizeof(payload)),
          "the TX entry accepted a frame for %s, which has no path", mac_(R));
    CHECK(render_pending() == 1, "it is held for discovery (pending=%ld)", render_pending());
    CHECK(count_data() == 0, "and nothing went out as data (%u data frames)", count_data());
    CHECK(g_warthog_fwd_preq_tx - s0.preq_tx == 1, "exactly one PREQ was originated (%u)",
          g_warthog_fwd_preq_tx - s0.preq_tx);
    CHECK(count_hwmp(HWMP_EID_PREQ) == 1, "and exactly one PREQ reached the chip (%u)",
          count_hwmp(HWMP_EID_PREQ));
    {
        const struct simnode_frame *f = find_hwmp(HWMP_EID_PREQ, 0);
        uint16_t bl = 0;
        const uint8_t *b = act_body(f, &bl);
        struct hwmp_preq q;
        bool ok = b != NULL && umac_mesh_hwmp_parse_preq(b, bl, &q);
        CHECK(ok, "the firmware's own parser accepts the PREQ it built");
        CHECK(ok && memcmp(q.target_addr, R, 6) == 0, "it asks for %s", mac_(R));
        CHECK(ok && memcmp(q.orig_addr, W, 6) == 0, "on our own behalf");
        CHECK(f != NULL && mgmt_da(f)[0] == 0xff, "and it is broadcast, not sent to the target");
    }
    CHECK(simnode_live_allocs() > base_allocs,
          "the held buffer is still allocated (%u vs %u at rest)",
          simnode_live_allocs(), base_allocs);

    /* ---------------------------------------------------------------------
     * 2. the flush driven by a RECEIVED PATH-SELECTION FRAME.
     *
     * umac_mesh_fwd_glue_hwmp_rx() ends in flush_pending_(). The PREP installs
     * the path and the held frame must go out on the same call.
     * ------------------------------------------------------------------- */
    printf("\n--- 2. the PREP arrives: flush_pending_ from the receive path ---\n");
    snap(&s0);
    simnode_outbox_clear();
    {
        uint16_t n = mk_prep(body, sizeof(body), R, 5, W, 1);
        CHECK(n == HWMP_PREP_BODY_LEN, "a PREP for %s via %s built (%u bytes)", mac_(R), mac_(A), n);
        umac_mesh_fwd_glue_hwmp_rx(body, n, A, &own_sn, false, false);
    }
    simnode_pump();
    CHECK(render_paths_count() == 1, "the path table now holds one path (%ld)",
          render_paths_count());
    CHECK(g_warthog_fwd_pend_tx - s0.pend_tx == 1, "the held frame was released to the wire (%u)",
          g_warthog_fwd_pend_tx - s0.pend_tx);
    CHECK(g_warthog_fwd_pend_drop - s0.pend_drop == 0, "and not dropped (%u)",
          g_warthog_fwd_pend_drop - s0.pend_drop);
    CHECK(render_pending() == 0, "nothing is still held (pending=%ld)", render_pending());
    CHECK(count_data() == 1, "exactly one data frame reached the chip (%u)", count_data());
    {
        bool ok = parse_data(nth_data(0), &pf);
        CHECK(ok, "the firmware's parser accepts the frame it built");
        CHECK(ok && memcmp(pf.addr1, A, 6) == 0, "addr1 is the next hop %s", mac_(A));
        CHECK(ok && memcmp(pf.addr2, W, 6) == 0, "addr2 is us");
        CHECK(ok && memcmp(pf.addr3, R, 6) == 0, "addr3 is the mesh destination %s", mac_(R));
        CHECK(ok && memcmp(pf.addr4, W, 6) == 0, "addr4 is us, the mesh source");
    }
    CHECK(simnode_live_allocs() == base_allocs, "no buffer leaked (%u vs %u)",
          simnode_live_allocs(), base_allocs);

    /* ---------------------------------------------------------------------
     * 3. tx_pending: SENT IMMEDIATELY.
     *
     * The other branch: the PREP beat the caller here, so the path exists by
     * the time tx_pending runs and the frame must be sent, not held and not
     * dropped. Only reachable by calling the glue directly -- through the
     * datapath the lookup would have found the path first.
     * ------------------------------------------------------------------- */
    printf("\n--- 3. the path appeared late: tx_pending SENDS rather than holding ---\n");
    snap(&s0);
    simnode_outbox_clear();
    {
        struct mmpkt *pkt = mk_8023(R, W, 24);
        CHECK(pkt != NULL, "an 802.3 frame for %s allocated", mac_(R));
        bool taken = pkt != NULL && umac_mesh_fwd_glue_tx_pending(umacd, pkt, R);
        CHECK(taken, "tx_pending took ownership");
        simnode_pump();
        CHECK(g_warthog_fwd_pend_tx - s0.pend_tx == 1, "counted as a pending send (%u)",
              g_warthog_fwd_pend_tx - s0.pend_tx);
        CHECK(render_pending() == 0, "and it was NOT parked (pending=%ld)", render_pending());
        CHECK(count_data() == 1, "one data frame reached the chip (%u)", count_data());
        bool ok = parse_data(nth_data(0), &pf);
        CHECK(ok && memcmp(pf.addr1, A, 6) == 0, "addressed to the path's next hop %s", mac_(A));
    }
    CHECK(simnode_live_allocs() == base_allocs, "no buffer leaked (%u vs %u)",
          simnode_live_allocs(), base_allocs);

    /* ---------------------------------------------------------------------
     * 4. tx_pending: DROPPED.
     *
     * send_now_ resolves the next hop to a peer record a second time, and the
     * comment above it says that record can go away underneath. Reproduced
     * deterministically: the path survives while the peer record does not,
     * because del_peer only announces the loss to the glue while a forwarding
     * gate is set. tx_pending must then hand the frame BACK (false) so the
     * caller releases it -- not hold it, not leak it.
     * ------------------------------------------------------------------- */
    printf("\n--- 4. the next hop is gone: tx_pending refuses the frame ---\n");
    snap(&s0);
    simnode_outbox_clear();
    {
        /* The AT+MESHFWD / AT+MESHBRIDGE gates themselves, written directly:
         * simnode_set_gates() would re-init the path table and destroy the
         * very path this case needs. */
        g_warthog_mesh_fwd = 0;
        g_warthog_mesh_bridge = 0;
        simnode_del_peer(A);
        g_warthog_mesh_fwd = 1;
        CHECK(render_paths_count() == 1, "the path to %s outlived the peer record (paths=%ld)",
              mac_(R), render_paths_count());

        struct mmpkt *pkt = mk_8023(R, W, 24);
        CHECK(pkt != NULL, "an 802.3 frame for %s allocated", mac_(R));
        bool taken = pkt != NULL && umac_mesh_fwd_glue_tx_pending(umacd, pkt, R);
        CHECK(!taken, "tx_pending refused it, so the caller frees it");
        CHECK(render_pending() == 0, "it was not parked either (pending=%ld)", render_pending());
        CHECK(g_warthog_fwd_pend_tx - s0.pend_tx == 0, "and not counted as sent (%u)",
              g_warthog_fwd_pend_tx - s0.pend_tx);
        simnode_pump();
        CHECK(count_data() == 0, "nothing reached the chip (%u data frames)", count_data());
        if (!taken && pkt != NULL) { mmpkt_release(pkt); } /* what the TX entry does */
    }
    CHECK(simnode_add_peer(A), "peer %s re-established", mac_(A));
    CHECK(simnode_live_allocs() == base_allocs, "no buffer leaked (%u vs %u)",
          simnode_live_allocs(), base_allocs);

    /* ---------------------------------------------------------------------
     * 5. the PERR rate gate.
     *
     * A relay asked to forward to a destination it cannot route answers with
     * one PERR -- and, under a stream of such frames, ONE per interval, not
     * one per frame. Both halves are counters the AT command reports.
     * ------------------------------------------------------------------- */
    printf("\n--- 5. unroutable relay: one PERR, then the rate gate ---\n");
    snap(&s0);
    simnode_outbox_clear();
    {
        struct meshdata d = { .ra = W, .ta = A, .a3 = Z, .a4 = A, .ttl = 31, .seq = 1000,
                              .payload_len = 16 };
        CHECK(rx_mesh(&d), "a frame from %s for unroutable %s received", mac_(A), mac_(Z));
        CHECK(g_warthog_fwd_drop_nopath - s0.d_nopath == 1, "dropped as no-path (%u)",
              g_warthog_fwd_drop_nopath - s0.d_nopath);
        CHECK(g_warthog_fwd_perr_tx - s0.perr_tx == 1, "one PERR was transmitted (%u)",
              g_warthog_fwd_perr_tx - s0.perr_tx);
        CHECK(count_hwmp(HWMP_EID_PERR) == 1, "and exactly one reached the chip (%u)",
              count_hwmp(HWMP_EID_PERR));
        const struct simnode_frame *f = find_hwmp(HWMP_EID_PERR, 0);
        uint16_t bl = 0;
        const uint8_t *b = act_body(f, &bl);
        struct hwmp_perr e;
        bool ok = b != NULL && umac_mesh_hwmp_parse_perr(b, bl, &e);
        CHECK(ok, "the firmware's own parser accepts the PERR it built");
        CHECK(ok && memcmp(e.dest_addr, Z, 6) == 0, "it names %s unreachable", mac_(Z));
        CHECK(ok && e.reason == HWMP_REASON_MESH_PATH_ERROR_NO_FORWARDING,
              "with reason 62, as mac80211 does (%u)", ok ? e.reason : 0);
        CHECK(f != NULL && memcmp(mgmt_da(f), A, 6) == 0,
              "sent back to the transmitter %s, not broadcast", mac_(A));

        /* A second frame for the same destination, immediately. */
        struct stat_snap s1;
        snap(&s1);
        simnode_outbox_clear();
        d.seq = 1001;
        CHECK(rx_mesh(&d), "a second frame for %s arrives right behind it", mac_(Z));
        CHECK(g_warthog_fwd_drop_nopath - s1.d_nopath == 1, "also dropped as no-path (%u)",
              g_warthog_fwd_drop_nopath - s1.d_nopath);
        CHECK(g_warthog_fwd_perr_suppressed - s1.perr_supp == 1,
              "but the PERR is rate-limited away (%u)",
              g_warthog_fwd_perr_suppressed - s1.perr_supp);
        CHECK(g_warthog_fwd_perr_tx - s1.perr_tx == 0, "none transmitted (%u)",
              g_warthog_fwd_perr_tx - s1.perr_tx);
        CHECK(count_hwmp(HWMP_EID_PERR) == 0, "and none reached the chip (%u)",
              count_hwmp(HWMP_EID_PERR));

        /* 200 ms later: past the gate's 50 ms global floor, but still inside the
         * 500 ms per-target interval. This is what separates the two limits --
         * without it, a per-target interval of 1 ms would pass every assertion
         * above, because those two frames arrive at the same instant. */
        snap(&s1);
        simnode_outbox_clear();
        simnode_advance_ms(200u);
        d.seq = 1002;
        CHECK(rx_mesh(&d), "a third arrives 200 ms later");
        CHECK(g_warthog_fwd_perr_suppressed - s1.perr_supp == 1,
              "still suppressed: the per-target interval is the longer limit (%u)",
              g_warthog_fwd_perr_suppressed - s1.perr_supp);
        CHECK(g_warthog_fwd_perr_tx - s1.perr_tx == 0, "none transmitted (%u)",
              g_warthog_fwd_perr_tx - s1.perr_tx);

        /* Past the per-target interval, the gate opens again. */
        snap(&s1);
        simnode_outbox_clear();
        simnode_advance_ms(400u); /* 600 ms since the first, past the 500 ms interval */
        d.seq = 1003;
        CHECK(rx_mesh(&d), "a fourth arrives 600 ms after the first");
        CHECK(g_warthog_fwd_perr_tx - s1.perr_tx == 1, "the gate has re-opened (%u)",
              g_warthog_fwd_perr_tx - s1.perr_tx);
        CHECK(count_hwmp(HWMP_EID_PERR) == 1, "one more PERR on air (%u)",
              count_hwmp(HWMP_EID_PERR));
    }
    CHECK(simnode_live_allocs() == base_allocs, "no buffer leaked (%u vs %u)",
          simnode_live_allocs(), base_allocs);

    /* ---------------------------------------------------------------------
     * 6. forwarding a unicast relay, and the per-next-hop queue cap.
     *
     * The relay shares the TX pool with our own traffic and with peering, so
     * a next hop already holding UMAC_MESH_FWD_QUEUE_CAP frames must refuse
     * the next one. TX is paused (the datapath's own scan pause) so the queue
     * builds up exactly as it would behind a busy link.
     * ------------------------------------------------------------------- */
    printf("\n--- 6. relaying unicast, and the queue cap at the next hop ---\n");
    (void)install_path(S, B, 11);
    snap(&s0);
    simnode_outbox_clear();
    umac_datapath_pause(umacd, UMAC_DATAPATH_PAUSE_SOURCE_SCAN);
    {
        struct meshdata d = { .ra = W, .ta = A, .a3 = S, .a4 = A, .ttl = 31, .payload_len = 24 };
        for (uint32_t i = 0; i < 8u; i++)
        {
            d.seq = 2000u + i;
            if (!rx_mesh(&d)) { break; }
        }
        CHECK(g_warthog_fwd_uni - s0.uni == 8, "eight relayed frames were queued to %s (%u)",
              mac_(B), g_warthog_fwd_uni - s0.uni);
        CHECK(g_warthog_fwd_drop_full - s0.d_full == 0, "none refused yet (%u)",
              g_warthog_fwd_drop_full - s0.d_full);
        CHECK(count_data() == 0, "and none sent while TX is paused (%u)", count_data());

        d.seq = 2008u;
        CHECK(rx_mesh(&d), "a ninth frame arrives for the same next hop");
        CHECK(g_warthog_fwd_drop_full - s0.d_full == 1, "it is dropped: queue full (%u)",
              g_warthog_fwd_drop_full - s0.d_full);
        CHECK(g_warthog_fwd_uni - s0.uni == 8, "and not counted as forwarded (%u)",
              g_warthog_fwd_uni - s0.uni);
    }
    umac_datapath_unpause(umacd, UMAC_DATAPATH_PAUSE_SOURCE_SCAN);
    simnode_pump();
    CHECK(count_data() == 8, "exactly eight relayed frames reached the chip (%u)", count_data());
    {
        bool ok = parse_data(nth_data(0), &pf);
        CHECK(ok && memcmp(pf.addr1, B, 6) == 0, "addressed to the next hop %s", mac_(B));
        CHECK(ok && memcmp(pf.addr2, W, 6) == 0, "transmitted by us");
        CHECK(ok && memcmp(pf.addr3, S, 6) == 0, "mesh destination carried forward: %s", mac_(S));
        CHECK(ok && memcmp(pf.addr4, A, 6) == 0,
              "mesh source carried forward: %s, not rewritten to us", mac_(A));
        CHECK(ok && pf.mc.ttl == 30, "and the TTL was decremented (%u)", ok ? pf.mc.ttl : 0);
        CHECK(ok && pf.mc.seq == 2000u, "with the originator's sequence number intact (%lu)",
              ok ? (unsigned long)pf.mc.seq : 0ul);
    }
    CHECK(simnode_live_allocs() == base_allocs, "no buffer leaked (%u vs %u)",
          simnode_live_allocs(), base_allocs);

    /* ---------------------------------------------------------------------
     * 6b. the relay cannot get a buffer.
     *
     * umac_mesh_fwd_glue_forward() is called directly here with one allocation
     * armed to fail: through simnode_rx() the arming would land on the receive
     * buffer instead, and the branch would never run. What must hold is that
     * the failure is counted and nothing is queued or leaked.
     * ------------------------------------------------------------------- */
    printf("\n--- 6b. no buffer for the relayed copy ---\n");
    {
        extern void simnode_fail_next_alloc(void);
        struct stat_snap s1;
        snap(&s1);
        simnode_outbox_clear();

        struct meshdata d = { .ra = W, .ta = A, .a3 = S, .a4 = A, .ttl = 31, .seq = 2100,
                              .payload_len = 24 };
        uint8_t raw[320];
        uint16_t rawn = build_mesh_data(raw, sizeof(raw), &d);
        CHECK(rawn > UMAC_MESH_DATA_HDR4_LEN, "a relay frame built (%u bytes)", rawn);
        const struct dot11_data_hdr *dh = (const struct dot11_data_hdr *)raw;

        /* The body the relay would copy, allocated BEFORE the failure is armed. */
        struct mmpkt *bodypkt = mk_8023(S, A, 24);
        CHECK(bodypkt != NULL, "a body buffer for it");
        struct mmpktview *bodyview = (bodypkt != NULL) ? mmpkt_open(bodypkt) : NULL;

        struct umac_mesh_fwd_rx_result r;
        memset(&r, 0, sizeof(r));
        r.verdict = UMAC_MESH_FWD_FORWARD;
        memcpy(r.fwd_ra, B, 6);
        memcpy(r.mesh_da, S, 6);
        memcpy(r.mesh_sa, A, 6);
        r.fwd_mc.ttl = 30;
        r.fwd_mc.seq = 2100;

        unsigned before = simnode_live_allocs();
        if (bodyview != NULL)
        {
            simnode_fail_next_alloc();
            umac_mesh_fwd_glue_forward(umacd, bodyview, 0x0800, &dh->base, dh, &r);
        }
        CHECK(g_warthog_fwd_nomem - s1.nomem == 1, "the failure is counted as nomem (%u)",
              g_warthog_fwd_nomem - s1.nomem);
        CHECK(g_warthog_fwd_uni - s1.uni == 0, "and not counted as forwarded (%u)",
              g_warthog_fwd_uni - s1.uni);
        CHECK(simnode_live_allocs() == before, "nothing was allocated (%u vs %u)",
              simnode_live_allocs(), before);
        simnode_pump();
        CHECK(count_data() == 0, "and nothing reached the chip (%u)", count_data());
        if (bodyview != NULL) { mmpkt_close(&bodyview); }
        if (bodypkt != NULL) { mmpkt_release(bodypkt); }
    }
    CHECK(simnode_live_allocs() == base_allocs, "no buffer leaked (%u vs %u)",
          simnode_live_allocs(), base_allocs);

    /* ---------------------------------------------------------------------
     * 7. forwarding a group frame, and the duplicate cache.
     * ------------------------------------------------------------------- */
    printf("\n--- 7. relaying a group frame, then refusing its duplicate ---\n");
    snap(&s0);
    simnode_outbox_clear();
    {
        struct meshdata d = { .ra = GRP, .ta = A, .a3 = A, .a4 = NULL, .ttl = 31, .seq = 3000,
                              .payload_len = 24 };
        CHECK(rx_mesh(&d), "a group frame from %s received", mac_(A));
        simnode_pump();
        CHECK(g_warthog_fwd_grp - s0.grp == 1, "it was relayed as a group frame (%u)",
              g_warthog_fwd_grp - s0.grp);
        CHECK(count_data() >= 1, "at least one replica reached the chip (%u)", count_data());
        bool ok = parse_data(nth_data(0), &pf);
        CHECK(ok, "the replica parses");
        CHECK(ok && memcmp(pf.addr1, A, 6) != 0, "and does not go back to the sender %s", mac_(A));
        CHECK(ok && umac_mesh_ctrl_ae(&pf.mc) == UMAC_MESH_CTRL_AE_A5A6,
              "it carries AE 2, the group-replica form (ae=%u)",
              ok ? umac_mesh_ctrl_ae(&pf.mc) : 0xff);
        CHECK(ok && memcmp(pf.mc.eaddr1, GRP, 6) == 0, "with the group DA in the extension");
        CHECK(ok && memcmp(pf.mc.eaddr2, A, 6) == 0, "and the real source %s", mac_(A));
        CHECK(ok && pf.mc.ttl == 30, "TTL decremented (%u)", ok ? pf.mc.ttl : 0);

        struct stat_snap s1;
        snap(&s1);
        simnode_outbox_clear();
        CHECK(rx_mesh(&d), "the same group frame arrives again");
        simnode_pump();
        CHECK(g_warthog_fwd_drop_dup - s1.d_dup == 1, "the duplicate cache refuses it (%u)",
              g_warthog_fwd_drop_dup - s1.d_dup);
        CHECK(g_warthog_fwd_grp - s1.grp == 0, "nothing was relayed a second time (%u)",
              g_warthog_fwd_grp - s1.grp);
        CHECK(count_data() == 0, "and nothing reached the chip (%u)", count_data());
    }
    CHECK(simnode_live_allocs() == base_allocs, "no buffer leaked (%u vs %u)",
          simnode_live_allocs(), base_allocs);

    /* ---------------------------------------------------------------------
     * 8. the remaining drop counters: own, ttl, not-for-us, non-peer HWMP.
     * ------------------------------------------------------------------- */
    printf("\n--- 8. the drop counters AT+MESHFWDSTAT reports ---\n");
    {
        struct stat_snap s1;
        snap(&s1);
        simnode_outbox_clear();
        /* Our own frame coming back. This one is handed to the glue DIRECTLY:
         * the datapath's own filter (umac_datapath_rx_frame_filter, "Bcast
         * frame which AP relayed for us") already discards any frame whose
         * 802.11 SA is our address, one layer before the engine sees it, so
         * through simnode_rx() the counter could never move and an assertion
         * on it could never fail. */
        struct meshdata own = { .ra = W, .ta = A, .a3 = S, .a4 = W, .ttl = 31, .seq = 4000,
                                .payload_len = 16 };
        uint8_t ownf[320];
        uint16_t ownn = build_mesh_data(ownf, sizeof(ownf), &own);
        CHECK(ownn > UMAC_MESH_DATA_HDR4_LEN, "a frame whose mesh source is US built (%u)", ownn);
        {
            const struct dot11_data_hdr *dh = (const struct dot11_data_hdr *)ownf;
            struct umac_mesh_ctrl mc;
            uint16_t used = 0;
            bool parsed = umac_mesh_ctrl_parse(ownf + UMAC_MESH_DATA_HDR4_LEN + 2u,
                                               (uint16_t)(ownn - UMAC_MESH_DATA_HDR4_LEN - 2u),
                                               &mc, &used);
            CHECK(parsed, "its Mesh Control parses");
            struct umac_mesh_fwd_rx_result res;
            memset(&res, 0, sizeof(res));
            if (parsed) { umac_mesh_fwd_glue_rx(umacd, NULL, &dh->base, dh, &mc, &res); }
            CHECK(parsed && res.verdict == UMAC_MESH_FWD_DROP, "the engine drops it (verdict=%d)",
                  parsed ? (int)res.verdict : -1);
            CHECK(g_warthog_fwd_drop_own - s1.d_own == 1, "counted as drop own (%u)",
                  g_warthog_fwd_drop_own - s1.d_own);
        }

        snap(&s1);
        struct meshdata ttl = { .ra = W, .ta = A, .a3 = S, .a4 = A, .ttl = 1, .seq = 4001,
                                .payload_len = 16 };
        CHECK(rx_mesh(&ttl), "a relay frame arrives with ttl 1");
        CHECK(g_warthog_fwd_drop_ttl - s1.d_ttl == 1, "counted as drop ttl (%u)",
              g_warthog_fwd_drop_ttl - s1.d_ttl);
        CHECK(g_warthog_fwd_uni - s1.uni == 0, "and was not relayed (%u)",
              g_warthog_fwd_uni - s1.uni);

        snap(&s1);
        struct meshdata other = { .ra = B, .ta = A, .a3 = S, .a4 = A, .ttl = 31, .seq = 4002,
                                  .payload_len = 16 };
        CHECK(rx_mesh(&other), "a unicast whose RA is not us is delivered up by the chip");
        CHECK(g_warthog_fwd_drop_bad - s1.d_bad == 1, "counted as drop bad (%u)",
              g_warthog_fwd_drop_bad - s1.d_bad);

        snap(&s1);
        simnode_outbox_clear();
        uint16_t n = mk_prep(body, sizeof(body), R2, 3, W, 1);
        umac_mesh_fwd_glue_hwmp_rx(body, n, NOPEER, &own_sn, false, false);
        CHECK(g_warthog_fwd_drop_bad - s1.d_bad == 1,
              "path selection from a non-peer is refused and counted (%u)",
              g_warthog_fwd_drop_bad - s1.d_bad);
        CHECK(simnode_outbox_count() == 0, "nothing was sent in reply (%u frames)",
              simnode_outbox_count());
    }
    CHECK(simnode_live_allocs() == base_allocs, "no buffer leaked (%u vs %u)",
          simnode_live_allocs(), base_allocs);

    /* ---------------------------------------------------------------------
     * 9. the three relay counters.
     * ------------------------------------------------------------------- */
    printf("\n--- 9. relaying path selection: PREQ, PREP, PERR ---\n");
    {
        struct stat_snap s1;
        snap(&s1);
        simnode_outbox_clear();
        uint16_t n = umac_mesh_hwmp_build_preq(body, sizeof(body), X, 7, 3, Q1, 5000);
        CHECK(n == HWMP_PREQ_BODY_LEN, "a PREQ from %s for %s built (%u)", mac_(X), mac_(Q1), n);
        umac_mesh_fwd_glue_hwmp_rx(body, n, A, &own_sn, false, false);
        CHECK(g_warthog_hwmp_relay_preq - s1.r_preq == 1, "we rebroadcast it (%u)",
              g_warthog_hwmp_relay_preq - s1.r_preq);
        {
            const struct simnode_frame *f = find_hwmp(HWMP_EID_PREQ, 0);
            uint16_t bl = 0;
            const uint8_t *b = act_body(f, &bl);
            struct hwmp_preq q;
            bool ok = b != NULL && umac_mesh_hwmp_parse_preq(b, bl, &q);
            CHECK(ok, "the rebroadcast PREQ parses");
            CHECK(ok && memcmp(q.orig_addr, X, 6) == 0, "originator preserved");
            CHECK(ok && q.ttl == HWMP_DEFAULT_TTL - 1, "TTL decremented (%u)", ok ? q.ttl : 0);
            CHECK(ok && q.hop_count == 1, "hop count incremented (%u)", ok ? q.hop_count : 0);
            CHECK(f != NULL && mgmt_da(f)[0] == 0xff, "and it goes out broadcast");
        }

        snap(&s1);
        simnode_outbox_clear();
        n = mk_prep(body, sizeof(body), R2, 9, X, 7);
        umac_mesh_fwd_glue_hwmp_rx(body, n, B, &own_sn, false, false);
        CHECK(g_warthog_hwmp_relay_prep - s1.r_prep == 1,
              "a PREP for someone else is carried back (%u)",
              g_warthog_hwmp_relay_prep - s1.r_prep);
        {
            const struct simnode_frame *f = find_hwmp(HWMP_EID_PREP, 0);
            CHECK(f != NULL && memcmp(mgmt_da(f), A, 6) == 0,
                  "toward the originator's next hop %s", mac_(A));
        }

        snap(&s1);
        simnode_outbox_clear();
        n = umac_mesh_hwmp_build_perr(body, sizeof(body), HWMP_DEFAULT_TTL, R2, 10,
                                      HWMP_REASON_MESH_PATH_ERROR_DEST_UNREACHABLE);
        umac_mesh_fwd_glue_hwmp_rx(body, n, B, &own_sn, false, false);
        CHECK(g_warthog_hwmp_relay_perr - s1.r_perr == 1,
              "a PERR that invalidated a path is forwarded (%u)",
              g_warthog_hwmp_relay_perr - s1.r_perr);
        {
            const struct simnode_frame *f = find_hwmp(HWMP_EID_PERR, 0);
            CHECK(f != NULL && mgmt_da(f)[0] == 0xff, "broadcast onward");
        }

        snap(&s1);
        simnode_outbox_clear();
        umac_mesh_fwd_glue_hwmp_rx(body, n, B, &own_sn, false, false);
        CHECK(g_warthog_hwmp_relay_perr - s1.r_perr == 0,
              "the same PERR again changes nothing and is not echoed (%u)",
              g_warthog_hwmp_relay_perr - s1.r_perr);
    }
    CHECK(simnode_live_allocs() == base_allocs, "no buffer leaked (%u vs %u)",
          simnode_live_allocs(), base_allocs);

    /* ---------------------------------------------------------------------
     * 10. peer_lost and its PERR burst.
     * ------------------------------------------------------------------- */
    printf("\n--- 10. a peer is lost: every path through it is announced dead ---\n");
    {
        CHECK(simnode_add_peer(C), "peer %s established", mac_(C));
        (void)install_path(P1, C, 21);
        (void)install_path(P2, C, 22);
        struct stat_snap s1;
        snap(&s1);
        simnode_outbox_clear();
        simnode_del_peer(C);
        CHECK(g_warthog_fwd_perr_tx - s1.perr_tx == 2, "two PERRs were transmitted (%u)",
              g_warthog_fwd_perr_tx - s1.perr_tx);
        CHECK(count_hwmp(HWMP_EID_PERR) == 2, "and two reached the chip (%u)",
              count_hwmp(HWMP_EID_PERR));
        bool saw_p1 = false, saw_p2 = false, all_bcast = true, all_bumped = true;
        for (unsigned i = 0; i < 2; i++)
        {
            const struct simnode_frame *f = find_hwmp(HWMP_EID_PERR, i);
            uint16_t bl = 0;
            const uint8_t *b = act_body(f, &bl);
            struct hwmp_perr e;
            if (b == NULL || !umac_mesh_hwmp_parse_perr(b, bl, &e)) { all_bcast = false; continue; }
            if (mgmt_da(f)[0] != 0xff) { all_bcast = false; }
            if (memcmp(e.dest_addr, P1, 6) == 0) { saw_p1 = true; if (e.dest_sn != 22u) { all_bumped = false; } }
            else if (memcmp(e.dest_addr, P2, 6) == 0) { saw_p2 = true; if (e.dest_sn != 23u) { all_bumped = false; } }
        }
        CHECK(saw_p1 && saw_p2, "one names %s and one names %s", mac_(P1), mac_(P2));
        CHECK(all_bcast, "both are broadcast");
        CHECK(all_bumped,
              "each announces the destination at sn+1, or every holder would refuse it");
        /* The PERR gate is NOT consulted on this path: a lost neighbour is a
         * bounded burst, not a stream, and suppressing it would leave stale
         * paths across the mesh. */
        CHECK(g_warthog_fwd_perr_suppressed - s1.perr_supp == 0,
              "the rate gate did not swallow any of them (%u)",
              g_warthog_fwd_perr_suppressed - s1.perr_supp);
    }
    CHECK(simnode_live_allocs() == base_allocs, "no buffer leaked (%u vs %u)",
          simnode_live_allocs(), base_allocs);

    /* ---------------------------------------------------------------------
     * 11. the pending queue: bounds, eviction, the PREQ retry, the 3 s lapse.
     *
     * The buffers held here come out of the same TX pool as our own traffic
     * and peering, so the bounds are the point: an unanswered discovery must
     * not be able to park the pool.
     * ------------------------------------------------------------------- */
    printf("\n--- 11. the pending queue is bounded, retried, and expires ---\n");
    {
        struct stat_snap s1;
        snap(&s1);
        simnode_outbox_clear();
        struct mmpkt *p;

        p = mk_8023(Q1, W, 16);
        CHECK(p != NULL && umac_mesh_fwd_glue_tx_pending(umacd, p, Q1), "one frame held for %s",
              mac_(Q1));
        CHECK(render_pending() == 1, "pending=%ld", render_pending());
        p = mk_8023(Q1, W, 16);
        CHECK(p != NULL && umac_mesh_fwd_glue_tx_pending(umacd, p, Q1), "a second for %s",
              mac_(Q1));
        CHECK(render_pending() == 2, "pending=%ld", render_pending());
        CHECK(g_warthog_fwd_pend_drop - s1.pend_drop == 0, "nothing evicted yet (%u)",
              g_warthog_fwd_pend_drop - s1.pend_drop);

        p = mk_8023(Q1, W, 16);
        CHECK(p != NULL && umac_mesh_fwd_glue_tx_pending(umacd, p, Q1), "a third for %s",
              mac_(Q1));
        CHECK(render_pending() == 2,
              "the per-target bound holds the queue at %u (pending=%ld)",
              UMAC_MESH_PENDING_PER_TARGET, render_pending());
        CHECK(g_warthog_fwd_pend_drop - s1.pend_drop == 1,
              "and the oldest for that target was dropped, not leaked (%u)",
              g_warthog_fwd_pend_drop - s1.pend_drop);

        p = mk_8023(Q2, W, 16);
        CHECK(p != NULL && umac_mesh_fwd_glue_tx_pending(umacd, p, Q2), "one for %s", mac_(Q2));
        p = mk_8023(Q2, W, 16);
        CHECK(p != NULL && umac_mesh_fwd_glue_tx_pending(umacd, p, Q2), "a second for %s",
              mac_(Q2));
        CHECK(render_pending() == 4, "the queue is full (pending=%ld)", render_pending());

        struct stat_snap s2;
        snap(&s2);
        p = mk_8023(Q3, W, 16);
        CHECK(p != NULL && umac_mesh_fwd_glue_tx_pending(umacd, p, Q3),
              "a frame for a third target %s", mac_(Q3));
        CHECK(render_pending() == 4, "the total bound still holds at 4 (pending=%ld)",
              render_pending());
        CHECK(g_warthog_fwd_pend_drop - s2.pend_drop == 1,
              "the oldest frame overall was evicted (%u)",
              g_warthog_fwd_pend_drop - s2.pend_drop);
        CHECK(simnode_live_allocs() == base_allocs + 4u,
              "exactly four buffers are parked (%u live vs %u at rest)", simnode_live_allocs(),
              base_allocs);

        /* The retry. The first PREQ for a held target is a single unacked
         * broadcast; the tick has to re-ask or a lost PREQ costs the frame. */
        snap(&s2);
        simnode_outbox_clear();
        simnode_advance_ms(600u); /* past the 500 ms per-target interval */
        simnode_tick();
        CHECK(g_warthog_fwd_preq_tx - s2.preq_tx == 1,
              "the tick re-asks -- once, because the gate's global floor bounds the burst (%u)",
              g_warthog_fwd_preq_tx - s2.preq_tx);
        CHECK(count_hwmp(HWMP_EID_PREQ) == 1, "one PREQ reached the chip (%u)",
              count_hwmp(HWMP_EID_PREQ));
        {
            const struct simnode_frame *f = find_hwmp(HWMP_EID_PREQ, 0);
            uint16_t bl = 0;
            const uint8_t *b = act_body(f, &bl);
            struct hwmp_preq q;
            bool ok = b != NULL && umac_mesh_hwmp_parse_preq(b, bl, &q);
            CHECK(ok && (memcmp(q.target_addr, Q1, 6) == 0 || memcmp(q.target_addr, Q2, 6) == 0 ||
                         memcmp(q.target_addr, Q3, 6) == 0),
                  "and it asks for one of the waiting targets");
            CHECK(ok && memcmp(q.orig_addr, W, 6) == 0, "on our own behalf");
        }
        CHECK(render_pending() == 4, "nothing was released by the retry (pending=%ld)",
              render_pending());

        snap(&s2);
        simnode_outbox_clear();
        simnode_tick();
        CHECK(g_warthog_fwd_preq_tx - s2.preq_tx == 0,
              "an immediate second tick re-asks nothing: the gate is shut (%u)",
              g_warthog_fwd_preq_tx - s2.preq_tx);

        /* The lapse. Every held frame must give its buffer back on time even
         * though no PREP ever came -- and NOT before, or a PREP that arrives
         * inside the window finds nothing left to send.
         *
         * The two waits are wall values, deliberately not UMAC_MESH_PENDING_MS:
         * a test written in terms of the constant follows it wherever it moves
         * and cannot catch it changing. 2.0 s in (one service tick short of the
         * limit) the frames must still be there; 3.2 s in they must all be gone.
         * Everything was pushed at the same instant, and 600 ms of that budget
         * was already spent on the retry above. */
        snap(&s2);
        simnode_outbox_clear();
        unsigned held = (unsigned)render_pending();
        simnode_advance_ms(1400u); /* 2.0 s since the pushes */
        simnode_tick();
        CHECK(render_pending() == (long)held,
              "2 s in, nothing has lapsed yet (pending=%ld of %u)", render_pending(), held);
        CHECK(g_warthog_fwd_pend_drop - s2.pend_drop == 0, "and none was dropped (%u)",
              g_warthog_fwd_pend_drop - s2.pend_drop);

        snap(&s2);
        simnode_outbox_clear();
        simnode_advance_ms(1200u); /* 3.2 s since the pushes */
        simnode_tick();
        CHECK(render_pending() == 0, "3.2 s in, every held frame has lapsed (pending=%ld)",
              render_pending());
        CHECK(g_warthog_fwd_pend_drop - s2.pend_drop == held,
              "all %u were counted as dropped (%u)", held,
              g_warthog_fwd_pend_drop - s2.pend_drop);
        CHECK(g_warthog_fwd_pend_tx - s2.pend_tx == 0, "and none as sent (%u)",
              g_warthog_fwd_pend_tx - s2.pend_tx);
        CHECK(g_warthog_fwd_preq_tx - s2.preq_tx == 0,
              "a lapsed target is not re-asked for (%u PREQs)",
              g_warthog_fwd_preq_tx - s2.preq_tx);
        CHECK(simnode_live_allocs() == base_allocs,
              "and every parked buffer came back (%u vs %u)", simnode_live_allocs(), base_allocs);
    }

    /* ---------------------------------------------------------------------
     * 12. tx_classify: the sidecar for a host behind us, and one sequence
     *     number per frame.
     * ------------------------------------------------------------------- */
    printf("\n--- 12. a bridged host's frame is proxied with AE 2 ---\n");
    {
        simnode_outbox_clear();
        CHECK(simnode_host_tx(A, H, payload, sizeof(payload)),
              "a frame from host %s behind us, to peer %s", mac_(H), mac_(A));
        CHECK(count_data() == 1, "one data frame reached the chip (%u)", count_data());
        bool ok = parse_data(nth_data(0), &pf);
        CHECK(ok && umac_mesh_ctrl_ae(&pf.mc) == UMAC_MESH_CTRL_AE_A5A6,
              "it carries AE 2 (ae=%u)", ok ? umac_mesh_ctrl_ae(&pf.mc) : 0xff);
        CHECK(ok && memcmp(pf.mc.eaddr1, A, 6) == 0, "proxied destination is %s", mac_(A));
        CHECK(ok && memcmp(pf.mc.eaddr2, H, 6) == 0, "proxied source is the host %s", mac_(H));
        uint32_t first = ok ? pf.mc.seq : 0;

        simnode_outbox_clear();
        CHECK(simnode_host_tx(A, H, payload, sizeof(payload)), "a second frame from the same host");
        ok = parse_data(nth_data(0), &pf);
        CHECK(ok && pf.mc.seq != first,
              "gets its own Mesh Control sequence number (%lu then %lu)", (unsigned long)first,
              ok ? (unsigned long)pf.mc.seq : 0ul);
    }
    CHECK(simnode_live_allocs() == base_allocs, "no buffer leaked (%u vs %u)",
          simnode_live_allocs(), base_allocs);

    /* ---------------------------------------------------------------------
     * 13. forwarding off, bridge on: the leaf case.
     *
     * simnode_set_gates() re-initialises the glue's tables, so this phase
     * starts from an empty path table by design.
     * ------------------------------------------------------------------- */
    printf("\n--- 13. bridge-only: a relay frame is refused, with no PERR ---\n");
    simnode_set_gates(/*fwd=*/false, /*bridge=*/true, /*grp_std=*/false, /*secure=*/false);
    {
        struct stat_snap s1;
        snap(&s1);
        simnode_outbox_clear();
        struct meshdata d = { .ra = W, .ta = A, .a3 = S, .a4 = A, .ttl = 31, .seq = 5000,
                              .payload_len = 16 };
        CHECK(rx_mesh(&d), "a frame for %s arrives while forwarding is off", mac_(S));
        CHECK(g_warthog_fwd_drop_nofwd - s1.d_nofwd == 1, "counted as drop nofwd (%u)",
              g_warthog_fwd_drop_nofwd - s1.d_nofwd);
        CHECK(g_warthog_fwd_uni - s1.uni == 0, "nothing was relayed (%u)",
              g_warthog_fwd_uni - s1.uni);
        CHECK(g_warthog_fwd_perr_tx - s1.perr_tx == 0,
              "and a leaf sends no PERR -- it never claimed to route (%u)",
              g_warthog_fwd_perr_tx - s1.perr_tx);
        CHECK(simnode_outbox_count() == 0, "nothing reached the chip (%u)",
              simnode_outbox_count());
    }

    /* ---------------------------------------------------------------------
     * 14. the protection latch.
     * ------------------------------------------------------------------- */
    printf("\n--- 14. trust on first protected path-selection frame ---\n");
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/false);
    {
        struct stat_snap s1;
        snap(&s1);
        simnode_outbox_clear();
        uint16_t n = umac_mesh_hwmp_build_preq(body, sizeof(body), X, 31, 9, Q1, 5000);
        umac_mesh_fwd_glue_hwmp_rx(body, n, B, &own_sn, /*is_protected=*/true, false);
        CHECK(g_warthog_hwmp_prot - s1.prot == 1, "a protected frame is counted (%u)",
              g_warthog_hwmp_prot - s1.prot);
        CHECK(g_warthog_hwmp_relay_preq - s1.r_preq == 1, "and processed (%u)",
              g_warthog_hwmp_relay_preq - s1.r_preq);

        snap(&s1);
        simnode_outbox_clear();
        n = umac_mesh_hwmp_build_preq(body, sizeof(body), X, 32, 10, Q2, 5000);
        umac_mesh_fwd_glue_hwmp_rx(body, n, B, &own_sn, /*is_protected=*/false, false);
        CHECK(g_warthog_hwmp_unprotected - s1.unprot == 1,
              "a plaintext frame from that peer is now refused (%u)",
              g_warthog_hwmp_unprotected - s1.unprot);
        CHECK(g_warthog_hwmp_relay_preq - s1.r_preq == 0, "and not processed (%u)",
              g_warthog_hwmp_relay_preq - s1.r_preq);
        CHECK(simnode_outbox_count() == 0, "nothing was sent (%u)", simnode_outbox_count());

        /* A peer that has never protected anything is still trusted, which is
         * what keeps today's unprotected mesh working. */
        snap(&s1);
        umac_mesh_fwd_glue_hwmp_rx(body, n, A, &own_sn, /*is_protected=*/false, false);
        CHECK(g_warthog_hwmp_unprotected - s1.unprot == 0,
              "a peer that never protected anything is still accepted (%u)",
              g_warthog_hwmp_unprotected - s1.unprot);
    }

    /* ---------------------------------------------------------------------
     * 15. the group-addressed MMIE census.
     *
     * Nothing is gated on it yet: the point of the two counters is to say what
     * the chip actually hands up before anything depends on it.
     * ------------------------------------------------------------------- */
    printf("\n--- 15. group-addressed path selection: MMIE present or not ---\n");
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/true);
    {
        struct stat_snap s1;
        snap(&s1);
        uint16_t n = umac_mesh_hwmp_build_perr(body, sizeof(body), HWMP_DEFAULT_TTL, Q3, 1,
                                               HWMP_REASON_MESH_PATH_ERROR_DEST_UNREACHABLE);
        umac_mesh_fwd_glue_hwmp_rx(body, n, A, &own_sn, false, /*is_group_addressed=*/true);
        CHECK(g_warthog_hwmp_nommie - s1.nommie == 1, "a bare body is counted as no MMIE (%u)",
              g_warthog_hwmp_nommie - s1.nommie);
        CHECK(g_warthog_hwmp_mmie - s1.mmie == 0, "and not as one (%u)",
              g_warthog_hwmp_mmie - s1.mmie);

        snap(&s1);
        body[n++] = 76u; /* MMIE element id */
        body[n++] = 16u; /* its length */
        for (unsigned i = 0; i < 16u; i++) { body[n++] = (uint8_t)i; }
        umac_mesh_fwd_glue_hwmp_rx(body, n, A, &own_sn, false, /*is_group_addressed=*/true);
        CHECK(g_warthog_hwmp_mmie - s1.mmie == 1, "a trailing MMIE is recognised (%u)",
              g_warthog_hwmp_mmie - s1.mmie);
        CHECK(g_warthog_hwmp_nommie - s1.nommie == 0, "and not counted as absent (%u)",
              g_warthog_hwmp_nommie - s1.nommie);
    }
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/false);

    CHECK(simnode_live_allocs() == base_allocs,
          "at the end, live allocations are back to the baseline (%u vs %u)",
          simnode_live_allocs(), base_allocs);

    simnode_stop();

    printf("\n%d checks\n", checks);
    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_simnode_glue: all passed\n");
    return 0;
}
