/*
 * umac_mesh_fwd_glue.c, end to end, on the real stack.
 *
 * The glue is the one mesh translation unit no freestanding test can link: it
 * needs mmosal, mmpkt, mmdrv and the datapath, so until the simulator existed
 * its behaviour was checked structurally by test_glue_guard.sh and not at all
 * by execution. Everything here runs the SHIPPING file -- held frames, the
 * discovery retry, the bounded pending queue, the relay's own hold and PREQ
 * ladder, the forwarding queue cap, the peer-loss PERR burst, the lifetime
 * ceiling, the table-full count and the tick's path sweep -- and asserts wherever
 * possible on the bytes the firmware handed to the chip, parsed back with the
 * firmware's own parsers.
 *
 * What it cannot tell you: nothing below mmdrv_tx_frame() exists here. A green
 * run means the glue's own logic is consistent, not that the mesh works on air.
 *
 * Two mechanics worth knowing before reading the phases:
 *
 *   - simnode_set_gates() changes the gates only. The path table, the RMC,
 *     both rate gates, the protection latch and the pending queue carry over
 *     from phase to phase, as on the device, where a gate change needs a
 *     reboot. Every assertion is on a DELTA of the AT+MESHFWDSTAT counters
 *     rather than an absolute, because those counters are never reset.
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
#include "umac/mesh/umac_mesh.h"
#include "umac/mesh/umac_mesh_hwmp_relay.h"

/* UMAC_MESH_FWD_QUEUE_CAP, private to the glue: phase 6 pins the value. */
#define UMAC_MESH_FWD_QUEUE_CAP_TEST 8u

/* main/at.c's AT+MESHPATH? buffer. */
#define AT_MESHPATH_LEN 4096u

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
extern volatile uint32_t g_warthog_fwd_drop_full, g_warthog_fwd_drop_tblfull;
extern volatile uint32_t g_warthog_fwd_perr_tx, g_warthog_fwd_preq_tx;
extern volatile uint32_t g_warthog_fwd_hold, g_warthog_fwd_hold_tx, g_warthog_fwd_hold_drop;
extern volatile uint32_t g_warthog_hwmp_rann_rx, g_warthog_hwmp_perr_rx, g_warthog_hwmp_parse_fail;
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
    uint32_t d_own, d_dup, d_ttl, d_nopath, d_nofwd, d_bad, d_full, d_tblfull;
    uint32_t perr_tx, preq_tx;
    uint32_t hold, hold_tx, hold_drop;
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
    s->d_full = g_warthog_fwd_drop_full; s->d_tblfull = g_warthog_fwd_drop_tblfull;
    s->perr_tx = g_warthog_fwd_perr_tx;
    s->preq_tx = g_warthog_fwd_preq_tx;
    s->hold = g_warthog_fwd_hold; s->hold_tx = g_warthog_fwd_hold_tx;
    s->hold_drop = g_warthog_fwd_hold_drop;
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
static const uint8_t T1[6] = { 0x02, 0, 0, 0, 0, 0x24 }; /* relay-hold targets */
static const uint8_t T2[6] = { 0x02, 0, 0, 0, 0, 0x25 };
static const uint8_t T3[6] = { 0x02, 0, 0, 0, 0, 0x26 };
static const uint8_t T4[6] = { 0x02, 0, 0, 0, 0, 0x27 };
static const uint8_t T5[6] = { 0x02, 0, 0, 0, 0, 0x28 };
static const uint8_t V[6]  = { 0x02, 0, 0, 0, 0, 0x29 }; /* two hops away, via B */
static const uint8_t O1[6] = { 0x02, 0, 0, 0, 0, 0x55 }; /* our own held targets */
static const uint8_t O2[6] = { 0x02, 0, 0, 0, 0, 0x56 };
static const uint8_t O3[6] = { 0x02, 0, 0, 0, 0, 0x57 };
static const uint8_t O4[6] = { 0x02, 0, 0, 0, 0, 0x58 };
static const uint8_t O5[6] = { 0x02, 0, 0, 0, 0, 0x59 };
static const uint8_t HD[6] = { 0x02, 0, 0, 0, 0, 0x62 }; /* hosts a relayed AE 2 frame names */
static const uint8_t HS[6] = { 0x02, 0, 0, 0, 0, 0x63 };
static const uint8_t NOPEER[6] = { 0x02, 0, 0, 0, 0, 0x7f };
static const uint8_t GRP[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x69 };
static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t L1[6] = { 0x02, 0, 0, 0, 0, 0x81 }; /* relay-ladder targets, phases 23+ */
static const uint8_t L2[6] = { 0x02, 0, 0, 0, 0, 0x82 };
static const uint8_t L3[6] = { 0x02, 0, 0, 0, 0, 0x83 };
static const uint8_t L4[6] = { 0x02, 0, 0, 0, 0, 0x84 };
static const uint8_t L5[6] = { 0x02, 0, 0, 0, 0, 0x85 };
static const uint8_t L6[6] = { 0x02, 0, 0, 0, 0, 0x86 };
static const uint8_t L7[6] = { 0x02, 0, 0, 0, 0, 0x87 };
static const uint8_t LP[6] = { 0x02, 0, 0, 0, 0, 0x88 }; /* becomes a peer mid-discovery */
static const uint8_t OQ1[6] = { 0x02, 0, 0, 0, 0, 0x89 }; /* our own discoveries, phase 30 */
static const uint8_t OQ2[6] = { 0x02, 0, 0, 0, 0, 0x8a };
static const uint8_t OQ3[6] = { 0x02, 0, 0, 0, 0, 0x8b };

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

/* The receive path hands path selection to the glue on the umac event loop, which
 * sends what it relays at once; from any other task it would be queued for the loop. */
void simnode_loop_enter(void);
void simnode_loop_leave(void);
static void hwmp_rx_(const uint8_t *body, uint16_t len, const uint8_t *ta, uint32_t *own_sn,
                     bool is_protected, bool is_group_addressed)
{
    simnode_loop_enter();
    umac_mesh_fwd_glue_hwmp_rx(body, len, ta, own_sn, is_protected, is_group_addressed);
    simnode_loop_leave();
}

/** Install an active path to @p dst via peer @p via, the way a PREP answering
 *  our own PREQ does. Returns the sequence number the path now holds. */
static uint32_t install_path(const uint8_t *dst, const uint8_t *via, uint32_t sn)
{
    uint8_t body[HWMP_PREP_BODY_LEN];
    uint32_t own_sn = 1;
    uint16_t n = mk_prep(body, sizeof(body), dst, sn, W, 1);
    if (n != 0) { hwmp_rx_(body, n, via, &own_sn, false, false); }
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

/** relay_held=N: relayed frames held for discovery. */
static long render_relay_held(void)
{
    char buf[2048];
    int n = umac_mesh_fwd_glue_render(buf, sizeof(buf));
    if (n <= 0) { return -1; }
    buf[(unsigned)n < sizeof(buf) ? (unsigned)n : sizeof(buf) - 1] = '\0';
    const char *p = strstr(buf, "relay_held=");
    return (p != NULL) ? strtol(p + 11, NULL, 10) : -1;
}

/** PREQs in the outbox that we originated for @p target. */
static unsigned preq_for(const uint8_t *target)
{
    unsigned n = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        uint16_t bl = 0;
        const uint8_t *b = act_body(simnode_outbox_get(i), &bl);
        struct hwmp_preq q;
        if (b != NULL && umac_mesh_hwmp_element_id(b, bl) == HWMP_EID_PREQ &&
            umac_mesh_hwmp_parse_preq(b, bl, &q) && memcmp(q.target_addr, target, 6) == 0 &&
            memcmp(q.orig_addr, W, 6) == 0)
        {
            n++;
        }
    }
    return n;
}

/** The Lifetime of the last PREQ in the outbox we originated for @p target, or 0. */
static uint32_t preq_lifetime(const uint8_t *target)
{
    uint32_t lt = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        uint16_t bl = 0;
        const uint8_t *b = act_body(simnode_outbox_get(i), &bl);
        struct hwmp_preq q;
        if (b != NULL && umac_mesh_hwmp_element_id(b, bl) == HWMP_EID_PREQ &&
            umac_mesh_hwmp_parse_preq(b, bl, &q) && memcmp(q.target_addr, target, 6) == 0 &&
            memcmp(q.orig_addr, W, 6) == 0)
        {
            lt = q.lifetime;
        }
    }
    return lt;
}

/** Run virtual time forward to @p abs_ms, firing core timeouts as they fall due. */
static void run_until(uint32_t abs_ms)
{
    int32_t d = (int32_t)(abs_ms - mmosal_get_time_ms());
    if (d > 0) { simnode_advance_run((uint32_t)d); }
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

/* Every slot AT+MESHPATH? lists, active or dead, in a buffer too big to cut:
 * the number of dst= lines, and whether @p dst is one of them. */
static unsigned render_slots(const uint8_t *dst, bool *listed)
{
    static char buf[16384];
    int n = umac_mesh_fwd_glue_render(buf, sizeof(buf));
    buf[(n > 0 && (unsigned)n < sizeof(buf)) ? (unsigned)n : 0] = '\0';
    char want[32];
    snprintf(want, sizeof(want), "dst=%02x%02x%02x ", dst[3], dst[4], dst[5]);
    unsigned k = 0;
    for (const char *q = buf; (q = strstr(q, "+MESHPATH: dst=")) != NULL; q++) { k++; }
    *listed = strstr(buf, want) != NULL;
    return k;
}

/* One AT+MESHPATH? render into @p len bytes of a larger, patterned buffer.
 * ok: NUL-terminated inside @p len, nothing written past it, whole lines only,
 * and either every entry of the full tables listed or the marker last. */
#define RENDER_MARK "+MESHPATH: (truncated)\r\n"
struct render_view {
    const char *text;
    int w;
    unsigned paths, hosts, pinned;
    bool term, whole, marked, all, ok;
};

static struct render_view render_into(uint32_t len)
{
    static char big[AT_MESHPATH_LEN + 64u];
    const size_t mlen = sizeof(RENDER_MARK) - 1u;
    struct render_view v;
    memset(&v, 0, sizeof(v));
    memset(big, 0x5a, sizeof(big));
    v.text = big;
    v.w = umac_mesh_fwd_glue_render(big, len);
    bool untouched = true;
    for (uint32_t i = len; i < sizeof(big); i++) { untouched = untouched && big[i] == 0x5a; }
    v.term = untouched && v.w > 0 && (uint32_t)v.w < len && big[v.w] == '\0' && strlen(big) == (size_t)v.w;
    if (!v.term) { return v; }
    unsigned lines = 0, heads = 0;
    for (const char *q = big; (q = strstr(q, "\r\n")) != NULL; q += 2) { lines++; }
    for (const char *q = big; (q = strstr(q, "+MESHPATH: ")) != NULL; q++) { heads++; }
    for (const char *q = big; (q = strstr(q, "+MESHPATH: dst=")) != NULL; q++) { v.paths++; }
    for (const char *q = big; (q = strstr(q, "+MESHPATH: host=")) != NULL; q++) { v.hosts++; }
    for (const char *q = big; (q = strstr(q, " relay=00000a uni\r\n")) != NULL; q++) { v.pinned++; }
    v.whole = lines == heads && big[v.w - 1] == '\n' && strncmp(big, "+MESHPATH: ", 11) == 0;
    v.marked = (size_t)v.w >= mlen && strcmp(big + v.w - mlen, RENDER_MARK) == 0;
    v.all = v.paths == UMAC_MESH_PATH_MAX && v.hosts == UMAC_MESH_PROXY_MAX &&
            v.pinned == UMAC_MESH_PROXY_MAX;
    v.ok = v.whole && (v.all != v.marked);
    return v;
}

/* Armed as the TX allocation hook: that one allocation fails, as an empty pool's does. */
extern void simnode_fail_next_alloc(void);
static void fail_tx_alloc_(void) { simnode_fail_next_alloc(); }

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
        CHECK(ok && q.lifetime == 4882u, "with vanilla's 4882 TU lifetime, our own refresh window's (%lu)",
              ok ? (unsigned long)q.lifetime : 0ul);
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
        hwmp_rx_(body, n, A, &own_sn, false, false);
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
     * 5. a relay with no path HOLDS the frame and discovers the node itself.
     *
     * OpenMANET's mac80211 (999-0027) queues a relayed unicast whose mesh DA it
     * holds no path to, originates its own PREQ with a retry ladder (0, 0.4,
     * 1.2, 2.8, 4.8 s), sends the queue on the PREP and gives up silently at
     * 6.8 s. None of its configurations sends a no-route PERR. The ladder runs
     * off a umac core timeout, fired here by simnode_advance_run() at the
     * virtual millisecond it falls due.
     * ------------------------------------------------------------------- */
    printf("\n--- 5. unroutable relay frame: held, PREQ ladder, no PERR, given up at 6.8 s ---\n");
    run_until(mmosal_get_time_ms() + 1000u); /* clear of phase 1's PREQ and the gate's floor */
    snap(&s0);
    simnode_outbox_clear();
    {
        const uint32_t t0 = mmosal_get_time_ms();
        struct meshdata d = { .ra = W, .ta = A, .a3 = Z, .a4 = A, .ttl = 31, .seq = 1000,
                              .payload_len = 16 };
        CHECK(rx_mesh(&d), "a frame from %s for unroutable %s received", mac_(A), mac_(Z));
        CHECK(g_warthog_fwd_hold - s0.hold == 1 && render_relay_held() == 1 && count_data() == 0,
              "it is HELD for discovery, not sent (hold +%u, relay_held=%ld, %u data)",
              g_warthog_fwd_hold - s0.hold, render_relay_held(), count_data());
        CHECK(simnode_live_allocs() == base_allocs + 1u && g_warthog_fwd_drop_nopath - s0.d_nopath == 0,
              "one buffer is parked for it and nothing counted as a no-path drop (%u vs %u, nopath +%u)",
              simnode_live_allocs(), base_allocs, g_warthog_fwd_drop_nopath - s0.d_nopath);
        CHECK(count_hwmp(HWMP_EID_PERR) == 0 && g_warthog_fwd_perr_tx - s0.perr_tx == 0,
              "no PERR goes back to %s: OpenMANET's relay sends none (%u on air)", mac_(A),
              count_hwmp(HWMP_EID_PERR));
        {
            const struct simnode_frame *f = find_hwmp(HWMP_EID_PREQ, 0);
            uint16_t bl = 0;
            const uint8_t *b = act_body(f, &bl);
            struct hwmp_preq q;
            bool ok = b != NULL && umac_mesh_hwmp_parse_preq(b, bl, &q);
            CHECK(preq_for(Z) == 1 && ok && memcmp(q.target_addr, Z, 6) == 0 &&
                      memcmp(q.orig_addr, W, 6) == 0 && mgmt_da(f)[0] == 0xff &&
                      (q.target_flags & HWMP_TGT_FLAG_TO) != 0,
                  "the relay asks for %s itself at once: one PREQ, ours, broadcast, Target Only (%u)",
                  mac_(Z), preq_for(Z));
            CHECK(ok && q.lifetime == 48828u,
                  "with OpenMANET's 48828 TU lifetime, so the path its PREP installs outlives 5 s (%lu)",
                  ok ? (unsigned long)q.lifetime : 0ul);
        }

        static const uint32_t at[] = { 400u, 1200u, 2800u, 4800u };
        for (unsigned k = 0; k < 4u; k++)
        {
            simnode_outbox_clear();
            run_until(t0 + at[k] - 1u);
            unsigned early = preq_for(Z);
            run_until(t0 + at[k]);
            CHECK(early == 0 && preq_for(Z) == 1 && count_hwmp(HWMP_EID_PERR) == 0,
                  "PREQ %u for %s at exactly +%u ms, none 1 ms before (%u then %u)", k + 2u,
                  mac_(Z), (unsigned)at[k], early, preq_for(Z));
        }
        simnode_outbox_clear();
        run_until(t0 + 6799u);
        CHECK(preq_for(Z) == 0 && render_relay_held() == 1 && simnode_live_allocs() == base_allocs + 1u &&
                  g_warthog_fwd_hold_drop - s0.hold_drop == 0,
              "no sixth PREQ, and still held 1 ms before the give-up (%u PREQ, relay_held=%ld)",
              preq_for(Z), render_relay_held());
        run_until(t0 + 6800u);
        CHECK(g_warthog_fwd_hold_drop - s0.hold_drop == 1 && render_relay_held() == 0 &&
                  simnode_live_allocs() == base_allocs,
              "given up at +6800 ms: dropped and its buffer released (hold_drop +%u, %u vs %u live)",
              g_warthog_fwd_hold_drop - s0.hold_drop, simnode_live_allocs(), base_allocs);
        CHECK(count_hwmp(HWMP_EID_PERR) == 0 && g_warthog_fwd_hold_tx - s0.hold_tx == 0 &&
                  simnode_timeouts_pending() == 0 && g_warthog_fwd_hold - s0.hold == 1,
              "silently, with nothing sent and no timer left armed (%u PERR, %u timeouts)",
              count_hwmp(HWMP_EID_PERR), simnode_timeouts_pending());
    }

    /* ---------------------------------------------------------------------
     * 5b. the PREP arrives: the held copy leaves as the relay copy it is.
     *
     * Frames that arrive while a discovery runs join it (no second PREQ) and
     * leave in arrival order, carrying the ORIGINATOR's mesh SA, sequence
     * number and Address Extension, and one hop of TTL -- never reshaped as
     * a frame of ours.
     * ------------------------------------------------------------------- */
    printf("\n--- 5b. held relay frames go out on the PREP, as relayed, in order ---\n");
    run_until(mmosal_get_time_ms() + 1000u);
    snap(&s0);
    simnode_outbox_clear();
    {
        const uint32_t t0 = mmosal_get_time_ms();
        struct meshdata d = { .ra = W, .ta = A, .a3 = T1, .a4 = X, .ttl = 20, .seq = 7001,
                              .ae = UMAC_MESH_CTRL_AE_A5A6, .eaddr1 = HD, .eaddr2 = HS,
                              .payload_len = 16 };
        CHECK(rx_mesh(&d), "a frame from %s (mesh SA %s) for %s, AE 2", mac_(A), mac_(X), mac_(T1));
        run_until(t0 + 100u);
        d.seq = 7002;
        CHECK(rx_mesh(&d), "a second 100 ms later");
        d.seq = 7003;
        CHECK(rx_mesh(&d), "and a third, past the per-target bound");
        CHECK(g_warthog_fwd_hold - s0.hold == 3 && g_warthog_fwd_hold_drop - s0.hold_drop == 1 &&
                  render_relay_held() == 2 && preq_for(T1) == 1,
              "the later frames join the running discovery: 2 held, the oldest evicted, still one PREQ "
              "(hold +%u, drop +%u, relay_held=%ld, %u PREQ)", g_warthog_fwd_hold - s0.hold,
              g_warthog_fwd_hold_drop - s0.hold_drop, render_relay_held(), preq_for(T1));

        simnode_outbox_clear();
        (void)install_path(T1, B, 40);
        simnode_pump();
        CHECK(g_warthog_fwd_hold_tx - s0.hold_tx == 2 && render_relay_held() == 0 && count_data() == 2,
              "the PREP releases both to the wire (hold_tx +%u, relay_held=%ld, %u data)",
              g_warthog_fwd_hold_tx - s0.hold_tx, render_relay_held(), count_data());
        bool ok0 = parse_data(nth_data(0), &pf);
        CHECK(ok0 && memcmp(pf.addr1, B, 6) == 0 && memcmp(pf.addr2, W, 6) == 0 &&
                  memcmp(pf.addr3, T1, 6) == 0 && memcmp(pf.addr4, X, 6) == 0,
              "addressed RA %s, TA us, mesh DA %s, mesh SA %s -- the originator, not us", mac_(B),
              mac_(T1), mac_(X));
        CHECK(ok0 && pf.mc.ttl == 19 && umac_mesh_ctrl_ae(&pf.mc) == UMAC_MESH_CTRL_AE_A5A6 &&
                  memcmp(pf.mc.eaddr1, HD, 6) == 0 && memcmp(pf.mc.eaddr2, HS, 6) == 0,
              "one hop of TTL spent (%u) and AE 2 (%s, %s) carried unchanged", ok0 ? pf.mc.ttl : 0,
              mac_(HD), mac_(HS));
        struct umac_mesh_rx_frame pf2;
        bool ok1 = parse_data(nth_data(1), &pf2);
        CHECK(ok0 && ok1 && pf.mc.seq == 7002u && pf2.mc.seq == 7003u,
              "in arrival order with the originator's sequence numbers (%lu, %lu)",
              ok0 ? (unsigned long)pf.mc.seq : 0ul, ok1 ? (unsigned long)pf2.mc.seq : 0ul);
        CHECK(simnode_live_allocs() == base_allocs && g_warthog_fwd_hold - s0.hold == 3,
              "no buffer leaked (%u vs %u)", simnode_live_allocs(), base_allocs);
        simnode_outbox_clear();
        run_until(t0 + 2000u);
        CHECK(preq_for(T1) == 0 && simnode_timeouts_pending() == 0 && g_warthog_fwd_hold_tx - s0.hold_tx == 2,
              "and the ladder stops: no re-ask after the PREP, no timer left (%u PREQ)", preq_for(T1));
    }

    /* ---------------------------------------------------------------------
     * 5c. the relay's own cap, beside ours.
     *
     * Relayed frames are TX-pool buffers: UMAC_MESH_PENDING_RELAY_MAX of them
     * at most, and they can neither evict nor be evicted by our own held
     * frames. Targets that fall due at one instant are asked one per gate
     * floor, not skipped.
     * ------------------------------------------------------------------- */
    printf("\n--- 5c. the relay cap is its own: relayed frames never displace ours ---\n");
    run_until(mmosal_get_time_ms() + 1000u);
    snap(&s0);
    simnode_outbox_clear();
    {
        const uint32_t t0 = mmosal_get_time_ms();
        const uint8_t *own[4] = { O1, O2, O3, O4 };
        bool took = true;
        for (unsigned i = 0; i < 4u; i++)
        {
            struct mmpkt *p = mk_8023(own[i], W, 16);
            took &= p != NULL && umac_mesh_fwd_glue_tx_pending(umacd, p, own[i]);
        }
        CHECK(took && render_pending() == 4, "four of our own frames held (pending=%ld)", render_pending());
        struct meshdata d = { .ra = W, .ta = A, .a4 = A, .ttl = 31, .payload_len = 16 };
        const uint8_t *rt[4] = { T2, T2, T3, T3 };
        for (unsigned i = 0; i < 4u; i++) { d.a3 = rt[i]; d.seq = 7100u + i; (void)rx_mesh(&d); }
        CHECK(render_relay_held() == 4 && render_pending() == 4 &&
                  g_warthog_fwd_pend_drop - s0.pend_drop == 0 && g_warthog_fwd_hold_drop - s0.hold_drop == 0 &&
                  simnode_live_allocs() == base_allocs + 8u,
              "four relayed frames held beside them, none evicted (relay_held=%ld pending=%ld, %u live)",
              render_relay_held(), render_pending(), simnode_live_allocs() - base_allocs);
        d.a3 = T4; d.seq = 7104;
        (void)rx_mesh(&d);
        CHECK(render_relay_held() == (long)UMAC_MESH_PENDING_RELAY_MAX && render_pending() == 4 &&
                  g_warthog_fwd_hold_drop - s0.hold_drop == 1 && g_warthog_fwd_pend_drop - s0.pend_drop == 0 &&
                  simnode_live_allocs() == base_allocs + 8u,
              "a fifth relayed frame evicts the oldest RELAYED one, not ours (hold_drop +%u pend_drop +%u)",
              g_warthog_fwd_hold_drop - s0.hold_drop, g_warthog_fwd_pend_drop - s0.pend_drop);
        struct mmpkt *p5 = mk_8023(O5, W, 16);
        CHECK(p5 != NULL && umac_mesh_fwd_glue_tx_pending(umacd, p5, O5) && render_pending() == 4 &&
                  render_relay_held() == (long)UMAC_MESH_PENDING_RELAY_MAX &&
                  g_warthog_fwd_pend_drop - s0.pend_drop == 1 && g_warthog_fwd_hold_drop - s0.hold_drop == 1,
              "and a fifth of ours evicts our oldest, never a relayed one (pend_drop +%u hold_drop +%u)",
              g_warthog_fwd_pend_drop - s0.pend_drop, g_warthog_fwd_hold_drop - s0.hold_drop);
        unsigned at0 = preq_for(T2) + preq_for(T3) + preq_for(T4);
        run_until(t0 + 49u);
        unsigned at49 = preq_for(T2) + preq_for(T3) + preq_for(T4);
        run_until(t0 + 100u);
        CHECK(at0 == 1 && at49 == 1 && preq_for(T2) == 1 && preq_for(T3) == 1 && preq_for(T4) == 1,
              "three relayed targets due at once are asked one per 50 ms floor, none skipped "
              "(%u at 0, %u at 49 ms, then %u/%u/%u)", at0, at49, preq_for(T2), preq_for(T3), preq_for(T4));
        run_until(t0 + 7000u);
        CHECK(render_relay_held() == 0 && render_pending() == 0 && simnode_live_allocs() == base_allocs &&
                  g_warthog_fwd_hold_drop - s0.hold_drop == 1u + UMAC_MESH_PENDING_RELAY_MAX,
              "past the give-up every parked buffer is back, ours included (%u vs %u)",
              simnode_live_allocs(), base_allocs);
    }

    /* ---------------------------------------------------------------------
     * 5d. the TX pool's flow control refuses the hold.
     *
     * OpenMANET's patch 900 drops a forward while the AC queue is stopped;
     * here, while the TX pool's own pause source (PKTMEM) is set, no copy is
     * taken and no PREQ goes out. Any other pause source (a scan, standby)
     * says nothing about the pool and does not refuse the hold.
     * ------------------------------------------------------------------- */
    printf("\n--- 5d. a paused TX pool refuses the hold; a scan pause does not ---\n");
    run_until(mmosal_get_time_ms() + 1000u);
    snap(&s0);
    simnode_outbox_clear();
    {
        umac_datapath_pause(umacd, MMDRV_PAUSE_SOURCE_MASK_PKTMEM);
        struct meshdata d = { .ra = W, .ta = A, .a3 = T5, .a4 = A, .ttl = 31, .seq = 7200,
                              .payload_len = 16 };
        CHECK(rx_mesh(&d), "a frame for unroutable %s while the pool is paused", mac_(T5));
        CHECK(g_warthog_fwd_hold_drop - s0.hold_drop == 1 && g_warthog_fwd_hold - s0.hold == 0 &&
                  render_relay_held() == 0 && preq_for(T5) == 0 && simnode_live_allocs() == base_allocs,
              "refused: counted as a hold drop, no copy, no PREQ (hold_drop +%u, %u PREQ, %u vs %u live)",
              g_warthog_fwd_hold_drop - s0.hold_drop, preq_for(T5), simnode_live_allocs(), base_allocs);
        umac_datapath_unpause(umacd, MMDRV_PAUSE_SOURCE_MASK_PKTMEM);
        simnode_pump();
        d.seq = 7201;
        CHECK(rx_mesh(&d) && g_warthog_fwd_hold - s0.hold == 1 && render_relay_held() == 1 &&
                  preq_for(T5) == 1,
              "unpaused, the next one is held and asked for (hold +%u, %u PREQ)",
              g_warthog_fwd_hold - s0.hold, preq_for(T5));
        run_until(mmosal_get_time_ms() + 7000u);
        CHECK(render_relay_held() == 0 && simnode_live_allocs() == base_allocs,
              "and given back at its give-up (%u vs %u)", simnode_live_allocs(), base_allocs);
        snap(&s0);
        simnode_outbox_clear();
        umac_datapath_pause(umacd, UMAC_DATAPATH_PAUSE_SOURCE_SCAN);
        d.seq = 7202;
        (void)rx_mesh(&d);
        CHECK(g_warthog_fwd_hold - s0.hold == 1 && g_warthog_fwd_hold_drop - s0.hold_drop == 0 &&
                  render_relay_held() == 1 && preq_for(T5) == 1,
              "a scan pause is not the pool's: that frame is held and asked for (hold +%u, drop +%u, %u PREQ)",
              g_warthog_fwd_hold - s0.hold, g_warthog_fwd_hold_drop - s0.hold_drop, preq_for(T5));
        umac_datapath_unpause(umacd, UMAC_DATAPATH_PAUSE_SOURCE_SCAN);
        simnode_pump();
        run_until(mmosal_get_time_ms() + 7000u);
        CHECK(render_relay_held() == 0 && simnode_live_allocs() == base_allocs,
              "and given back at its give-up (%u vs %u)", simnode_live_allocs(), base_allocs);
    }

    /* ---------------------------------------------------------------------
     * 5e. no core timeout to be had: the 2 s service tick carries the ladder.
     * ------------------------------------------------------------------- */
    printf("\n--- 5e. the ladder's timeout cannot be registered: the tick stands in ---\n");
    run_until(mmosal_get_time_ms() + 1000u);
    snap(&s0);
    simnode_outbox_clear();
    {
        const uint32_t t0 = mmosal_get_time_ms();
        simnode_fail_next_timeout();
        struct meshdata d = { .ra = W, .ta = A, .a3 = T4, .a4 = A, .ttl = 31, .seq = 7300,
                              .payload_len = 16 };
        CHECK(rx_mesh(&d) && render_relay_held() == 1 && preq_for(T4) == 1 &&
                  simnode_timeouts_pending() == 0,
              "held and asked for once, with no timeout armed (relay_held=%ld, %u PREQ, %u timeouts)",
              render_relay_held(), preq_for(T4), simnode_timeouts_pending());
        simnode_set_time_ms(t0 + 2000u);
        simnode_tick();
        CHECK(preq_for(T4) == 2 && render_relay_held() == 1,
              "the service tick re-asks the overdue step (%u PREQ)", preq_for(T4));
        simnode_set_time_ms(t0 + 8000u);
        simnode_tick();
        CHECK(render_relay_held() == 0 && g_warthog_fwd_hold_drop - s0.hold_drop == 1 &&
                  simnode_live_allocs() == base_allocs,
              "and gives it up past 6.8 s (hold_drop +%u, %u vs %u live)",
              g_warthog_fwd_hold_drop - s0.hold_drop, simnode_live_allocs(), base_allocs);
    }
    CHECK(simnode_live_allocs() == base_allocs, "no buffer leaked (%u vs %u)",
          simnode_live_allocs(), base_allocs);

    /* ---------------------------------------------------------------------
     * 5f. a released copy obeys the next hop's queue cap, as a forward does.
     * ------------------------------------------------------------------- */
    printf("\n--- 5f. held relay copies released onto a full next hop are dropped ---\n");
    run_until(mmosal_get_time_ms() + 1000u);
    snap(&s0);
    simnode_outbox_clear();
    {
        struct meshdata d = { .ra = W, .ta = A, .a3 = T2, .a4 = A, .ttl = 31, .seq = 7400,
                              .payload_len = 16 };
        (void)rx_mesh(&d);
        d.seq = 7401;
        (void)rx_mesh(&d);
        CHECK(render_relay_held() == 2, "two frames for %s held (relay_held=%ld)", mac_(T2),
              render_relay_held());
        umac_datapath_pause(umacd, UMAC_DATAPATH_PAUSE_SOURCE_SCAN);
        (void)install_path(V, B, 60);
        struct meshdata f = { .ra = W, .ta = A, .a3 = V, .a4 = A, .ttl = 31, .payload_len = 16 };
        for (uint32_t i = 0; i < UMAC_MESH_FWD_QUEUE_CAP_TEST; i++) { f.seq = 7410u + i; (void)rx_mesh(&f); }
        simnode_outbox_clear();
        (void)install_path(T2, B, 61);
        CHECK(g_warthog_fwd_hold_drop - s0.hold_drop == 2 && g_warthog_fwd_hold_tx - s0.hold_tx == 0 &&
                  render_relay_held() == 0,
              "the PREP finds %s's queue full: both dropped, not queued past the cap (drop +%u, tx +%u)",
              mac_(B), g_warthog_fwd_hold_drop - s0.hold_drop, g_warthog_fwd_hold_tx - s0.hold_tx);
        umac_datapath_unpause(umacd, UMAC_DATAPATH_PAUSE_SOURCE_SCAN);
        simnode_pump();
        CHECK(count_data() == UMAC_MESH_FWD_QUEUE_CAP_TEST && simnode_live_allocs() == base_allocs,
              "unpaused, only the %u queued forwards go out (%u) and nothing leaks",
              UMAC_MESH_FWD_QUEUE_CAP_TEST, count_data());
        run_until(mmosal_get_time_ms() + 500u); /* the ladder's timeout fires on an empty store */
    }

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

        /* A unicast whose RA is not us: the receive filter drops it first (not_ours), so
         * the engine's own RA check is handed the frame directly, as the own-frame case. */
        extern volatile uint32_t g_warthog_filt_hist[10];
        snap(&s1);
        const uint32_t no0 = g_warthog_filt_hist[9];
        struct meshdata other = { .ra = B, .ta = A, .a3 = S, .a4 = A, .ttl = 31, .seq = 4002,
                                  .payload_len = 16 };
        CHECK(rx_mesh(&other), "a unicast whose RA is not us is delivered up by the chip");
        CHECK(g_warthog_filt_hist[9] - no0 == 1 && g_warthog_fwd_drop_bad == s1.d_bad,
              "and dropped by the receive filter as not_ours (%u), before the engine (%u)",
              (unsigned)(g_warthog_filt_hist[9] - no0), g_warthog_fwd_drop_bad - s1.d_bad);
        {
            uint8_t of[320];
            uint16_t on = build_mesh_data(of, sizeof(of), &other);
            const struct dot11_data_hdr *dh = (const struct dot11_data_hdr *)of;
            struct umac_mesh_ctrl mc;
            uint16_t used = 0;
            bool parsed = on > UMAC_MESH_DATA_HDR4_LEN + 2u &&
                          umac_mesh_ctrl_parse(of + UMAC_MESH_DATA_HDR4_LEN + 2u,
                                               (uint16_t)(on - UMAC_MESH_DATA_HDR4_LEN - 2u),
                                               &mc, &used);
            struct umac_mesh_fwd_rx_result res;
            memset(&res, 0, sizeof(res));
            if (parsed) { umac_mesh_fwd_glue_rx(umacd, NULL, &dh->base, dh, &mc, &res); }
            CHECK(parsed && res.verdict == UMAC_MESH_FWD_DROP &&
                      g_warthog_fwd_drop_bad - s1.d_bad == 1,
                  "the engine, handed it directly, drops it too, counted as drop bad (%u)",
                  g_warthog_fwd_drop_bad - s1.d_bad);
        }

        snap(&s1);
        simnode_outbox_clear();
        uint16_t n = mk_prep(body, sizeof(body), R2, 3, W, 1);
        hwmp_rx_(body, n, NOPEER, &own_sn, false, false);
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
        hwmp_rx_(body, n, A, &own_sn, false, false);
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
        hwmp_rx_(body, n, B, &own_sn, false, false);
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
        hwmp_rx_(body, n, B, &own_sn, false, false);
        CHECK(g_warthog_hwmp_relay_perr - s1.r_perr == 1,
              "a PERR that invalidated a path is forwarded (%u)",
              g_warthog_hwmp_relay_perr - s1.r_perr);
        {
            const struct simnode_frame *f = find_hwmp(HWMP_EID_PERR, 0);
            CHECK(f != NULL && mgmt_da(f)[0] == 0xff, "broadcast onward");
        }

        snap(&s1);
        simnode_outbox_clear();
        hwmp_rx_(body, n, B, &own_sn, false, false);
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
     * ------------------------------------------------------------------- */
    printf("\n--- 13. bridge-only: a relay frame is refused, with no PERR and no hold ---\n");
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
        /* A destination with no path at all: a leaf still neither holds nor discovers. */
        snap(&s1);
        const unsigned to0 = simnode_timeouts_registered();
        d.a3 = T5;
        d.seq = 5001;
        CHECK(rx_mesh(&d), "a frame for unroutable %s arrives while forwarding is off", mac_(T5));
        CHECK(g_warthog_fwd_drop_nofwd - s1.d_nofwd == 1 && g_warthog_fwd_hold - s1.hold == 0 &&
                  g_warthog_fwd_hold_drop - s1.hold_drop == 0 && render_relay_held() == 0 &&
                  simnode_outbox_count() == 0 && simnode_timeouts_registered() == to0,
              "dropped as nofwd: never held, no PREQ, no timer (hold +%u, %u out)",
              g_warthog_fwd_hold - s1.hold, simnode_outbox_count());

        /* Handed a HOLD anyway (the engine never gives a leaf one), the glue refuses it. */
        snap(&s1);
        struct meshdata hd = { .ra = W, .ta = A, .a3 = T5, .a4 = A, .ttl = 31, .seq = 5002,
                               .payload_len = 16 };
        uint8_t raw[320];
        uint16_t rawn = build_mesh_data(raw, sizeof(raw), &hd);
        const struct dot11_data_hdr *dh = (const struct dot11_data_hdr *)raw;
        struct mmpkt *bodypkt = mk_8023(T5, A, 16);
        struct mmpktview *bodyview = (bodypkt != NULL) ? mmpkt_open(bodypkt) : NULL;
        struct umac_mesh_fwd_rx_result r;
        memset(&r, 0, sizeof(r));
        r.verdict = UMAC_MESH_FWD_HOLD;
        r.drop = UMAC_MESH_FWD_DROP_NO_PATH;
        memcpy(r.mesh_da, T5, 6);
        memcpy(r.mesh_sa, A, 6);
        r.fwd_mc.ttl = 30;
        r.fwd_mc.seq = 5002;
        unsigned before = simnode_live_allocs();
        if (bodyview != NULL && rawn != 0) { umac_mesh_fwd_glue_forward(umacd, bodyview, 0x0800, &dh->base, dh, &r); }
        simnode_pump();
        CHECK(g_warthog_fwd_hold - s1.hold == 0 && g_warthog_fwd_hold_drop - s1.hold_drop == 1 &&
                  render_relay_held() == 0 && simnode_live_allocs() == before && simnode_outbox_count() == 0,
              "a HOLD handed to a leaf's glue is refused: no copy, no PREQ (hold_drop +%u, %u out)",
              g_warthog_fwd_hold_drop - s1.hold_drop, simnode_outbox_count());
        if (bodyview != NULL) { mmpkt_close(&bodyview); }
        if (bodypkt != NULL) { mmpkt_release(bodypkt); }
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
        hwmp_rx_(body, n, B, &own_sn, /*is_protected=*/true, false);
        CHECK(g_warthog_hwmp_prot - s1.prot == 1, "a protected frame is counted (%u)",
              g_warthog_hwmp_prot - s1.prot);
        CHECK(g_warthog_hwmp_relay_preq - s1.r_preq == 1, "and processed (%u)",
              g_warthog_hwmp_relay_preq - s1.r_preq);

        snap(&s1);
        simnode_outbox_clear();
        n = umac_mesh_hwmp_build_preq(body, sizeof(body), X, 32, 10, Q2, 5000);
        hwmp_rx_(body, n, B, &own_sn, /*is_protected=*/false, false);
        CHECK(g_warthog_hwmp_unprotected - s1.unprot == 1,
              "a plaintext frame from that peer is now refused (%u)",
              g_warthog_hwmp_unprotected - s1.unprot);
        CHECK(g_warthog_hwmp_relay_preq - s1.r_preq == 0, "and not processed (%u)",
              g_warthog_hwmp_relay_preq - s1.r_preq);
        CHECK(simnode_outbox_count() == 0, "nothing was sent (%u)", simnode_outbox_count());

        /* A peer that has never protected anything is still trusted, which is
         * what keeps today's unprotected mesh working. */
        snap(&s1);
        hwmp_rx_(body, n, A, &own_sn, /*is_protected=*/false, false);
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
        hwmp_rx_(body, n, A, &own_sn, false, /*is_group_addressed=*/true);
        CHECK(g_warthog_hwmp_nommie - s1.nommie == 1, "a bare body is counted as no MMIE (%u)",
              g_warthog_hwmp_nommie - s1.nommie);
        CHECK(g_warthog_hwmp_mmie - s1.mmie == 0, "and not as one (%u)",
              g_warthog_hwmp_mmie - s1.mmie);

        snap(&s1);
        body[n++] = 76u; /* MMIE element id */
        body[n++] = 16u; /* its length */
        for (unsigned i = 0; i < 16u; i++) { body[n++] = (uint8_t)i; }
        hwmp_rx_(body, n, A, &own_sn, false, /*is_group_addressed=*/true);
        CHECK(g_warthog_hwmp_mmie - s1.mmie == 1, "a trailing MMIE is recognised (%u)",
              g_warthog_hwmp_mmie - s1.mmie);
        CHECK(g_warthog_hwmp_nommie - s1.nommie == 0, "and not counted as absent (%u)",
              g_warthog_hwmp_nommie - s1.nommie);
    }
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/false);

    /* ---------------------------------------------------------------------
     * 16. the relay keeps a path as long as its originator advertised.
     *
     * mac80211 sets a path's expiry from the PREQ/PREP Lifetime field
     * (hwmp_route_info_get). OpenMANET 1.8.0 advertises 48828 TU (50 s) and
     * refreshes only 10 s before expiry, so its flow uses a path through us
     * for ~40 s. X, far behind A, talks to S, far behind B. A relay that puts
     * its own 5.12 s on both paths drops that flow as no-path and PERRs the
     * sender every 5 s. Both directions: X->S rides the path B's PREP built,
     * the reply S->X the path X's PREQ built.
     *
     * Self-contained: A has never protected path selection, so plaintext from
     * it passes the latch; B's PREP goes protected, which passes whether or
     * not an earlier phase latched B.
     * ------------------------------------------------------------------- */
    printf("\n--- 16. a 50 s advertised lifetime is honoured by the relay ---\n");
    {
        struct stat_snap s1;
        const uint32_t t0 = mmosal_get_time_ms();
        uint16_t n = umac_mesh_hwmp_build_preq(body, sizeof(body), X, 900, 21, S, 48828u);
        hwmp_rx_(body, n, A, &own_sn, /*is_protected=*/false, false);
        struct hwmp_preq q;
        uint8_t pb[HWMP_PREP_BODY_LEN];
        uint16_t pn = umac_mesh_hwmp_parse_preq(body, n, &q)
                          ? umac_mesh_hwmp_build_prep(pb, sizeof(pb), &q, S, 60)
                          : 0;
        CHECK(pn == HWMP_PREP_BODY_LEN, "%s answers %s's 48828 TU PREQ (%u)", mac_(S), mac_(X), pn);
        hwmp_rx_(pb, pn, B, &own_sn, /*is_protected=*/true, false);
        simnode_pump();
        uint8_t nh[6];
        CHECK(umac_mesh_fwd_glue_next_hop(X, nh) && memcmp(nh, A, 6) == 0,
              "the PREQ installed %s via %s", mac_(X), mac_(A));
        CHECK(umac_mesh_fwd_glue_next_hop(S, nh) && memcmp(nh, B, 6) == 0,
              "the PREP installed %s via %s", mac_(S), mac_(B));

        const uint32_t at[] = { 10000u, 45000u, 49500u };
        for (unsigned k = 0; k < 3u; k++)
        {
            simnode_set_time_ms(t0 + at[k]);
            snap(&s1);
            simnode_outbox_clear();
            struct meshdata d = { .ra = W, .ta = A, .a3 = S, .a4 = X, .ttl = 31,
                                  .seq = 6000u + k, .payload_len = 16 };
            CHECK(rx_mesh(&d), "t+%u ms: a frame from %s for %s arrives via %s",
                  (unsigned)at[k], mac_(X), mac_(S), mac_(A));
            CHECK(g_warthog_fwd_uni - s1.uni == 1 && g_warthog_fwd_drop_nopath - s1.d_nopath == 0,
                  "t+%u ms: relayed, not dropped as no-path (uni %u nopath %u)", (unsigned)at[k],
                  g_warthog_fwd_uni - s1.uni, g_warthog_fwd_drop_nopath - s1.d_nopath);
            CHECK(g_warthog_fwd_perr_tx - s1.perr_tx == 0, "t+%u ms: and no PERR sent back (%u)",
                  (unsigned)at[k], g_warthog_fwd_perr_tx - s1.perr_tx);
            bool ok = parse_data(nth_data(0), &pf);
            CHECK(ok && memcmp(pf.addr1, B, 6) == 0, "t+%u ms: to the next hop %s",
                  (unsigned)at[k], mac_(B));

            snap(&s1);
            simnode_outbox_clear();
            struct meshdata r = { .ra = W, .ta = B, .a3 = X, .a4 = S, .ttl = 31,
                                  .seq = 6100u + k, .payload_len = 16 };
            CHECK(rx_mesh(&r), "t+%u ms: the reply from %s for %s arrives via %s",
                  (unsigned)at[k], mac_(S), mac_(X), mac_(B));
            CHECK(g_warthog_fwd_uni - s1.uni == 1 && g_warthog_fwd_drop_nopath - s1.d_nopath == 0,
                  "t+%u ms: the reply is relayed on the PREQ-built path (uni %u nopath %u)",
                  (unsigned)at[k], g_warthog_fwd_uni - s1.uni,
                  g_warthog_fwd_drop_nopath - s1.d_nopath);
            ok = parse_data(nth_data(0), &pf);
            CHECK(ok && memcmp(pf.addr1, A, 6) == 0, "t+%u ms: to the next hop %s",
                  (unsigned)at[k], mac_(A));
        }

        /* 48828 TU is 49999 ms: past it both paths are gone, not unbounded. */
        simnode_set_time_ms(t0 + 50500u);
        snap(&s1);
        simnode_outbox_clear();
        struct meshdata late = { .ra = W, .ta = A, .a3 = S, .a4 = X, .ttl = 31, .seq = 6010,
                                 .payload_len = 16 };
        CHECK(rx_mesh(&late), "t+50500 ms: one more frame for %s, past the advertised lifetime",
              mac_(S));
        CHECK(g_warthog_fwd_hold - s1.hold == 1 && g_warthog_fwd_uni - s1.uni == 0,
              "the PREP-built path lapsed when the originator said it would: held, not relayed (hold +%u)",
              g_warthog_fwd_hold - s1.hold);
        snap(&s1);
        struct meshdata rlate = { .ra = W, .ta = B, .a3 = X, .a4 = S, .ttl = 31, .seq = 6110,
                                  .payload_len = 16 };
        CHECK(rx_mesh(&rlate), "t+50500 ms: one more reply for %s", mac_(X));
        CHECK(g_warthog_fwd_hold - s1.hold == 1 && g_warthog_fwd_uni - s1.uni == 0,
              "and so did the PREQ-built one (hold +%u)", g_warthog_fwd_hold - s1.hold);
        /* Nobody answers: both give their buffers back at the relay's give-up. */
        snap(&s1);
        run_until(t0 + 50500u + 6800u);
        CHECK(g_warthog_fwd_hold_drop - s1.hold_drop == 2 && render_relay_held() == 0,
              "unanswered, both are dropped 6.8 s later (hold_drop +%u)",
              g_warthog_fwd_hold_drop - s1.hold_drop);

        /* The ceiling the glue hands the relay: 60 s, however long the
         * originator asks for. 0x00400000 TU is over 71 minutes. */
        static const uint8_t OL[6] = { 0x02, 0, 0, 0, 0x72, 0x01 };
        const uint32_t t1 = mmosal_get_time_ms();
        n = umac_mesh_hwmp_build_preq(body, sizeof(body), OL, 7, 300, Q2, 0x00400000u);
        hwmp_rx_(body, n, A, &own_sn, /*is_protected=*/false, false);
        simnode_set_time_ms(t1 + 59999u);
        CHECK(umac_mesh_fwd_glue_next_hop(OL, nh) && memcmp(nh, A, 6) == 0,
              "a PREQ at 0x00400000 TU: its originator %s is reachable at +59999 ms", mac_(OL));
        simnode_set_time_ms(t1 + 60000u);
        CHECK(!umac_mesh_fwd_glue_next_hop(OL, nh), "and not at +60000 ms: capped at 60 s");
    }
    CHECK(simnode_live_allocs() == base_allocs, "no buffer leaked (%u vs %u)",
          simnode_live_allocs(), base_allocs);

    /* ---------------------------------------------------------------------
     * 17. a full path table is counted, and expiry empties it.
     *
     * A live path is never evicted (HWMP is unauthenticated), and it lives as
     * long as its originator says: 50 s for OpenMANET 1.8.0. So
     * UMAC_MESH_PATH_MAX distinct originators heard inside that window fill
     * the table, and a PREQ from one more -- even one asking for us -- is
     * neither answered nor relayed until a slot lapses. tblfull is what tells
     * that apart from ordinary duplicate suppression.
     * ------------------------------------------------------------------- */
    printf("\n--- 17. a full path table: counted, and cleared by expiry ---\n");
    {
        struct stat_snap s1;
        snap(&s1);
        simnode_outbox_clear();
        for (unsigned i = 0; i < UMAC_MESH_PATH_MAX; i++)
        {
            const uint8_t o[6] = { 0x02, 0, 0, 0, 0x70, (uint8_t)i };
            uint16_t n = umac_mesh_hwmp_build_preq(body, sizeof(body), o, 1, 100u + i, Q1, 48828u);
            hwmp_rx_(body, n, A, &own_sn, false, false);
        }
        CHECK(render_paths_count() == (long)UMAC_MESH_PATH_MAX,
              "%u originators fill the table (paths=%ld)", (unsigned)UMAC_MESH_PATH_MAX,
              render_paths_count());
        CHECK(g_warthog_fwd_drop_tblfull - s1.d_tblfull == 0, "with no refusal yet (%u)",
              g_warthog_fwd_drop_tblfull - s1.d_tblfull);

        static const uint8_t NEWO[6] = { 0x02, 0, 0, 0, 0x71, 0x01 };
        simnode_outbox_clear();
        uint32_t sn_before = own_sn;
        uint16_t n = umac_mesh_hwmp_build_preq(body, sizeof(body), NEWO, 1, 200, W, 48828u);
        hwmp_rx_(body, n, A, &own_sn, false, false);
        CHECK(g_warthog_fwd_drop_tblfull - s1.d_tblfull == 1,
              "one more originator, asking for us, is refused and counted (tblfull %u)",
              g_warthog_fwd_drop_tblfull - s1.d_tblfull);
        CHECK(count_hwmp(HWMP_EID_PREP) == 0 && own_sn == sn_before,
              "and gets no PREP (%u)", count_hwmp(HWMP_EID_PREP));

        /* The first originator's PREQ again is a duplicate, not a full table. */
        const uint8_t o0[6] = { 0x02, 0, 0, 0, 0x70, 0 };
        n = umac_mesh_hwmp_build_preq(body, sizeof(body), o0, 1, 100, Q1, 48828u);
        hwmp_rx_(body, n, A, &own_sn, false, false);
        CHECK(g_warthog_fwd_drop_tblfull - s1.d_tblfull == 1,
              "a duplicate from a known originator is not counted as tblfull (%u)",
              g_warthog_fwd_drop_tblfull - s1.d_tblfull);

        /* A PREP we would carry back to a live originator, naming a target we
         * hold no path for: the same refusal, and nothing relayed. B latched
         * in phase 14, so its path selection goes protected. */
        static const uint8_t NT3[6] = { 0x02, 0, 0, 0, 0x71, 0x03 };
        const uint8_t o5[6] = { 0x02, 0, 0, 0, 0x70, 5 };
        uint8_t pb[HWMP_PREP_BODY_LEN];
        snap(&s1);
        simnode_outbox_clear();
        uint16_t pn = mk_prep(pb, sizeof(pb), NT3, 9, o5, 105);
        hwmp_rx_(pb, pn, B, &own_sn, /*is_protected=*/true, false);
        CHECK(g_warthog_fwd_drop_tblfull - s1.d_tblfull == 1 &&
                  g_warthog_hwmp_relay_prep - s1.r_prep == 0 && count_hwmp(HWMP_EID_PREP) == 0,
              "a relayed PREP naming a new target is refused and counted (tblfull %u, relayed %u)",
              g_warthog_fwd_drop_tblfull - s1.d_tblfull, g_warthog_hwmp_relay_prep - s1.r_prep);

        snap(&s1);
        simnode_advance_ms(UMAC_MESH_PATH_LIFETIME_MAX_MS);
        simnode_outbox_clear();
        n = umac_mesh_hwmp_build_preq(body, sizeof(body), NEWO, 2, 201, W, 48828u);
        hwmp_rx_(body, n, A, &own_sn, false, false);
        const struct simnode_frame *f = find_hwmp(HWMP_EID_PREP, 0);
        CHECK(f != NULL && memcmp(mgmt_da(f), A, 6) == 0,
              "once the table has lapsed, the same originator is answered via %s", mac_(A));
        CHECK(g_warthog_fwd_drop_tblfull - s1.d_tblfull == 0, "with no further refusal (%u)",
              g_warthog_fwd_drop_tblfull - s1.d_tblfull);

        /* The answer to our own PREQ, heard twice (two relays, or a retry):
         * the second is a duplicate of a live path, not a full table. */
        static const uint8_t NT2[6] = { 0x02, 0, 0, 0, 0x71, 0x02 };
        uint8_t nh[6];
        snap(&s1);
        pn = mk_prep(pb, sizeof(pb), NT2, 5, W, 1);
        hwmp_rx_(pb, pn, B, &own_sn, /*is_protected=*/true, false);
        hwmp_rx_(pb, pn, B, &own_sn, /*is_protected=*/true, false);
        CHECK(umac_mesh_fwd_glue_next_hop(NT2, nh) && memcmp(nh, B, 6) == 0,
              "our own discovery's answer installs %s via %s", mac_(NT2), mac_(B));
        CHECK(g_warthog_fwd_drop_tblfull - s1.d_tblfull == 0,
              "and its repeat is not counted as tblfull (%u)", g_warthog_fwd_drop_tblfull - s1.d_tblfull);
    }
    CHECK(simnode_live_allocs() == base_allocs, "no buffer leaked (%u vs %u)",
          simnode_live_allocs(), base_allocs);

    /* ---------------------------------------------------------------------
     * 18. a lost peer's PERR burst is 8 wide.
     *
     * A path can outlive its next hop by up to 50 s, so a lost neighbour
     * often carries several. Each one not announced costs an upstream sender
     * a frame before our NO_FORWARDING PERR reaches it. The batch is an array
     * on the umac event loop's stack (24 KiB), which is why it is 8, not the table.
     * ------------------------------------------------------------------- */
    printf("\n--- 18. a lost peer with 10 paths: 8 PERRs, and all 10 paths dead ---\n");
    {
        CHECK(simnode_add_peer(C), "peer %s established again", mac_(C));
        const long before = render_paths_count();
        for (unsigned i = 0; i < 10u; i++)
        {
            const uint8_t d[6] = { 0x02, 0, 0, 0, 0x44, (uint8_t)i };
            (void)install_path(d, C, 40u + i);
        }
        CHECK(render_paths_count() == before + 10, "ten paths via %s (paths=%ld)", mac_(C),
              render_paths_count());
        struct stat_snap s1;
        snap(&s1);
        simnode_outbox_clear();
        simnode_del_peer(C);
        CHECK(g_warthog_fwd_perr_tx - s1.perr_tx == 8, "eight PERRs were transmitted (%u)",
              g_warthog_fwd_perr_tx - s1.perr_tx);
        CHECK(count_hwmp(HWMP_EID_PERR) == 8, "and eight reached the chip (%u)",
              count_hwmp(HWMP_EID_PERR));
        CHECK(render_paths_count() == before, "every path through %s is dead (paths=%ld)", mac_(C),
              render_paths_count());
    }

    /* ---------------------------------------------------------------------
     * 19. AT+MESHPATH? capacity.
     *
     * main/at.c renders into AT_MESHPATH_LEN bytes. A full path table plus a
     * full proxy table of the longest proxy lines (leaf entries: relay= and
     * uni) must either all appear, or end with "+MESHPATH: (truncated)" --
     * never be cut silently or mid-line, never written past the buffer, at
     * any buffer size. Into 1400 bytes (too small for them) the marker must
     * be the last line. Paths are installed directly,
     * so a leaf's table can hold both here. The tables start empty: nothing is
     * held, so re-initialising the glue drops no buffer.
     * ------------------------------------------------------------------- */
    printf("\n--- 19. AT+MESHPATH? lists every entry, or says it was cut ---\n");
    simnode_set_gates(/*fwd=*/false, /*bridge=*/false, /*grp_std=*/false, /*secure=*/false);
    CHECK(render_pending() == 0, "nothing is held before the tables are reset");
    umac_mesh_fwd_glue_init();
    {
        for (uint32_t i = 0; i < UMAC_MESH_PATH_MAX; i++)
        {
            uint8_t dst[6] = { 0x02, 0, 0, 0, 0x7e, (uint8_t)i };
            (void)install_path(dst, (i & 1u) ? A : B, 4000000000u + i);
        }
        for (uint32_t k = 0; k < UMAC_MESH_PROXY_MAX; k++)
        {
            uint8_t node[6] = { 0x02, 0, 0, 0, 0x6d, (uint8_t)(k / UMAC_MESH_PROXY_PER_NODE) };
            uint8_t host[6] = { 0x02, 0, 0, 0, 0x6e, (uint8_t)k };
            struct meshdata d = { .ra = W, .ta = A, .a3 = W, .a4 = node, .ttl = 30,
                                  .seq = 9000u + k, .ae = UMAC_MESH_CTRL_AE_A5A6,
                                  .eaddr1 = W, .eaddr2 = host, .payload_len = 8 };
            (void)rx_mesh(&d);
        }
        struct render_view v = render_into(AT_MESHPATH_LEN);
        printf("     into %u bytes: %d written, %u path and %u host lines%s\n", AT_MESHPATH_LEN, v.w,
               v.paths, v.hosts, v.marked ? ", then the marker" : "");
        char want[64];
        snprintf(want, sizeof(want), "paths=%u proxies=%u ", (unsigned)UMAC_MESH_PATH_MAX,
                 (unsigned)UMAC_MESH_PROXY_MAX);
        CHECK(v.term && strstr(v.text, want) != NULL, "the summary line counts both full tables (%s)", want);
        CHECK(v.term && v.whole, "%u bytes: NUL-terminated inside the buffer, whole lines only",
              AT_MESHPATH_LEN);
        CHECK(v.ok, "%u bytes: every entry is listed, or the listing ends with the marker",
              AT_MESHPATH_LEN);
        v = render_into(1400u);
        printf("     into 1400 bytes: %d written, %u path and %u host lines%s\n", v.w, v.paths, v.hosts,
               v.marked ? ", then the marker" : "");
        CHECK(v.ok && v.marked && v.hosts < UMAC_MESH_PROXY_MAX,
              "1400 bytes: too small for them all, so the last line is the marker");
        /* Every size, so the room kept for the marker is exercised at each line boundary. */
        unsigned bad = 0, first_bad = 0;
        for (uint32_t len = sizeof(RENDER_MARK); len <= AT_MESHPATH_LEN; len++)
        {
            if (!render_into(len).ok && bad++ == 0) { first_bad = len; }
        }
        CHECK(bad == 0, "every buffer size from %u to %u bytes: whole lines, nothing past the end, "
              "and the marker exactly when cut (%u bad, first at %u)",
              (unsigned)sizeof(RENDER_MARK), AT_MESHPATH_LEN, bad, first_bad);
    }
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/false);
    simnode_host_rx_clear();

    /* ---------------------------------------------------------------------
     * 20. the service tick frees a path 600 s past its expiry.
     *
     * Every expiry compare is signed 32-bit, so a slot still held 2^31 ms
     * (24.9 days) after its expiry reads as unexpired: a lapsed ACTIVE path
     * comes back, and a dead one re-learned then keeps its old expiry and
     * never lapses -- enough of those lock the table. O1's path simply
     * lapses; O2's is killed by a PERR first. Both must still be listed one
     * ms before the bound and gone at it (as must phase 19's 32), and after
     * a jump past 2^31 ms O1 stays gone and a re-learned O2 lapses on time.
     * Driven through simnode_tick(), which is the probe task's service tick.
     * ------------------------------------------------------------------- */
    printf("\n--- 20. lapsed and dead paths are freed before their expiry can wrap ---\n");
    {
        static const uint8_t O1[6] = { 0x02, 0, 0, 0, 0x75, 0x01 };
        static const uint8_t O2[6] = { 0x02, 0, 0, 0, 0x75, 0x02 };
        uint8_t nh[6];
        bool l1 = false, l2 = false;
        simnode_advance_ms(6000u); /* phase 19's 5120 ms paths lapse, freeing slots */
        const uint32_t t0 = mmosal_get_time_ms();
        uint16_t n = umac_mesh_hwmp_build_preq(body, sizeof(body), O1, 10, 400, Q3, 4882u);
        hwmp_rx_(body, n, A, &own_sn, /*is_protected=*/false, false);
        n = umac_mesh_hwmp_build_preq(body, sizeof(body), O2, 10, 401, Q3, 4882u);
        hwmp_rx_(body, n, A, &own_sn, /*is_protected=*/false, false);
        n = umac_mesh_hwmp_build_perr(body, sizeof(body), HWMP_DEFAULT_TTL, O2, 11,
                                      HWMP_REASON_MESH_PATH_ERROR_DEST_UNREACHABLE);
        hwmp_rx_(body, n, A, &own_sn, /*is_protected=*/false, false);
        CHECK(umac_mesh_fwd_glue_next_hop(O1, nh) && !umac_mesh_fwd_glue_next_hop(O2, nh),
              "%s has a path via %s; %s's was killed by a PERR", mac_(O1), mac_(A), mac_(O2));

        /* 4882 TU is 4999 ms; the bound runs from there. */
        const uint32_t edge = t0 + 4999u + 600000u;
        simnode_set_time_ms(edge - 1u);
        simnode_tick();
        unsigned k = render_slots(O1, &l1);
        (void)render_slots(O2, &l2);
        CHECK(l1 && l2, "1 ms before 600 s past their expiry, both are still listed (%s %s)",
              l1 ? "yes" : "no", l2 ? "yes" : "no");
        simnode_set_time_ms(edge);
        simnode_tick();
        k = render_slots(O1, &l1);
        (void)render_slots(O2, &l2);
        CHECK(!l1 && !l2 && k == 0, "at 600 s, they and every other lapsed path are freed (%u listed)", k);

        const uint32_t wrap = t0 + 4999u + 0x80000000u + 10u;
        simnode_set_time_ms(wrap);
        CHECK(!umac_mesh_fwd_glue_next_hop(O1, nh),
              "2^31 ms after its expiry, %s's lapsed path does not come back", mac_(O1));
        n = umac_mesh_hwmp_build_preq(body, sizeof(body), O2, 12, 402, Q3, 4882u);
        hwmp_rx_(body, n, A, &own_sn, /*is_protected=*/false, false);
        CHECK(umac_mesh_fwd_glue_next_hop(O2, nh) && memcmp(nh, A, 6) == 0,
              "%s re-learned then has a path via %s", mac_(O2), mac_(A));
        simnode_set_time_ms(wrap + 4998u);
        CHECK(umac_mesh_fwd_glue_next_hop(O2, nh), "still at +4998 ms");
        simnode_set_time_ms(wrap + 4999u);
        CHECK(!umac_mesh_fwd_glue_next_hop(O2, nh), "and lapsed at +4999 ms, its own 4882 TU");
        simnode_set_time_ms(wrap + 86400000u);
        CHECK(!umac_mesh_fwd_glue_next_hop(O2, nh), "and still lapsed a day later");
    }

    /* ---------------------------------------------------------------------
     * 21. a mesh restart gives back frames held for discovery.
     *
     * umac_mesh_fwd_glue_init() runs on every mesh enable. It used to zero the
     * pending store, orphaning the TX-pool buffers still held for a PREQ.
     * ------------------------------------------------------------------- */
    printf("\n--- 21. re-initialising the glue releases frames held for discovery ---\n");
    {
        static const uint8_t R9[6] = { 0x02, 0, 0, 0, 0x79, 0x09 };
        struct stat_snap s1;
        simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/false);
        snap(&s1);
        CHECK(simnode_host_tx(R9, W, payload, sizeof(payload)), "a frame for undiscovered %s", mac_(R9));
        CHECK(render_pending() == 1 && simnode_live_allocs() > base_allocs,
              "is held for discovery (pending=%ld, %u live vs %u)", render_pending(),
              simnode_live_allocs(), base_allocs);
        umac_mesh_fwd_glue_init();
        CHECK(render_pending() == 0, "the restart empties the store (pending=%ld)", render_pending());
        CHECK(g_warthog_fwd_pend_drop - s1.pend_drop == 1, "counted as a pending drop (%u)",
              g_warthog_fwd_pend_drop - s1.pend_drop);
        CHECK(simnode_live_allocs() == base_allocs, "and the buffer went back (%u vs %u)",
              simnode_live_allocs(), base_allocs);

        /* A relayed hold too; the ladder's timeout left over from before the
         * restart finds nothing to ask for when it fires. */
        run_until(mmosal_get_time_ms() + 1000u);
        snap(&s1);
        simnode_outbox_clear();
        struct meshdata d = { .ra = W, .ta = A, .a3 = T3, .a4 = A, .ttl = 31, .seq = 9100,
                              .payload_len = 16 };
        CHECK(rx_mesh(&d) && render_relay_held() == 1 && simnode_timeouts_pending() == 1,
              "a relayed frame for %s is held, its ladder armed (relay_held=%ld, %u timeouts)",
              mac_(T3), render_relay_held(), simnode_timeouts_pending());
        umac_mesh_fwd_glue_init();
        CHECK(render_relay_held() == 0 && g_warthog_fwd_hold_drop - s1.hold_drop == 1 &&
                  simnode_live_allocs() == base_allocs,
              "the restart gives it back as a hold drop (hold_drop +%u, %u vs %u live)",
              g_warthog_fwd_hold_drop - s1.hold_drop, simnode_live_allocs(), base_allocs);
        simnode_outbox_clear();
        run_until(mmosal_get_time_ms() + 1000u);
        CHECK(preq_for(T3) == 0 && simnode_timeouts_pending() == 0 && g_warthog_fwd_hold - s1.hold == 1,
              "the stale timeout fires, asks nothing and does not re-arm (%u PREQ, %u timeouts)",
              preq_for(T3), simnode_timeouts_pending());
    }

    /* ---------------------------------------------------------------------
     * 22. RANN and PERR are counted as what they are, not as parse failures.
     *
     * umac_mesh_handle_hwmp() used to try every non-PREP body as a PREQ, so a
     * gate's RANN or a relay's PERR read as parse_fail on AT+HWMPSTAT?.
     * ------------------------------------------------------------------- */
    printf("\n--- 22. AT+HWMPSTAT counts RANN and PERR, not parse failures ---\n");
    {
        uint8_t rann[25] = { HWMP_CATEGORY_MESH, HWMP_ACTION_PATH_SELECTION, 126, 21, 0x80, 0, 31 };
        memcpy(rann + 7, X, 6);
        rann[13] = 5;
        uint8_t perr[HWMP_PERR_BODY_LEN];
        uint16_t pn = umac_mesh_hwmp_build_perr(perr, sizeof(perr), HWMP_DEFAULT_TTL, Q3, 3,
                                                HWMP_REASON_MESH_PATH_ERROR_DEST_UNREACHABLE);
        const uint32_t pf0 = g_warthog_hwmp_parse_fail, rann0 = g_warthog_hwmp_rann_rx,
                       perr0 = g_warthog_hwmp_perr_rx;
        simnode_set_gates(/*fwd=*/false, /*bridge=*/false, /*grp_std=*/false, /*secure=*/false);
        simnode_outbox_clear();
        umac_mesh_handle_hwmp(rann, sizeof(rann), A, false, true);
        umac_mesh_handle_hwmp(perr, pn, A, false, true);
        CHECK(g_warthog_hwmp_rann_rx - rann0 == 1 && g_warthog_hwmp_perr_rx - perr0 == 1 &&
                  g_warthog_hwmp_parse_fail == pf0 && simnode_outbox_count() == 0,
              "a leaf counts one RANN and one PERR, no parse failure, sends nothing (+%u +%u, pf +%u)",
              g_warthog_hwmp_rann_rx - rann0, g_warthog_hwmp_perr_rx - perr0,
              g_warthog_hwmp_parse_fail - pf0);
        simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/false);
        umac_mesh_handle_hwmp(rann, sizeof(rann), A, false, true);
        CHECK(g_warthog_hwmp_rann_rx - rann0 == 2 && g_warthog_hwmp_parse_fail == pf0 &&
                  simnode_outbox_count() == 0,
              "a relay counts the RANN too, and does not relay it (+%u, %u out)",
              g_warthog_hwmp_rann_rx - rann0, simnode_outbox_count());
        umac_mesh_handle_hwmp(perr, pn, A, false, true);
        CHECK(g_warthog_hwmp_perr_rx - perr0 == 2 && g_warthog_hwmp_parse_fail == pf0,
              "and the PERR (perr_rx +%u, pf +%u)", g_warthog_hwmp_perr_rx - perr0,
              g_warthog_hwmp_parse_fail - pf0);
    }

    /* ---------------------------------------------------------------------
     * 23. every relayed discovery gets its five PREQs.
     *
     * A ladder step is taken only by a PREQ for its target that went out once
     * the step was due. Three targets held at one instant are asked one per
     * 50 ms floor, so the second and third start late; each later step then
     * waits out the per-target interval from its previous send instead of being
     * counted as done without a frame.
     * ------------------------------------------------------------------- */
    printf("\n--- 23. held at one instant: five PREQs each, steps spaced from the previous send ---\n");
    run_until(mmosal_get_time_ms() + 10000u);
    snap(&s0);
    simnode_outbox_clear();
    {
        struct meshdata d = { .ra = W, .ta = A, .a4 = A, .ttl = 31, .payload_len = 16 };
        const uint32_t t0 = mmosal_get_time_ms();
        d.a3 = L1; d.seq = 9101; (void)rx_mesh(&d);
        d.a3 = L2; d.seq = 9102; (void)rx_mesh(&d);
        d.a3 = L3; d.seq = 9103; (void)rx_mesh(&d);
        run_until(t0 + 100u);
        CHECK(preq_for(L1) == 1 && preq_for(L2) == 1 && preq_for(L3) == 1,
              "first PREQs at 0, 50 and 100 ms, one per floor (%u/%u/%u)", preq_for(L1), preq_for(L2),
              preq_for(L3));
        run_until(t0 + 449u);
        const unsigned l2_449 = preq_for(L2);
        run_until(t0 + 450u);
        CHECK(preq_for(L1) == 2 && l2_449 == 1 && preq_for(L2) == 2,
              "%s's second waits 400 ms from its first: +450, not +400 (%u at +449, %u at +450)",
              mac_(L2), l2_449, preq_for(L2));
        run_until(t0 + 6799u);
        CHECK(preq_for(L1) == 5 && preq_for(L2) == 5 && preq_for(L3) == 5 && render_relay_held() == 3,
              "five PREQs for each by 6.8 s, none skipped (%u/%u/%u)", preq_for(L1), preq_for(L2),
              preq_for(L3));
        run_until(t0 + 7200u);
        CHECK(render_relay_held() == 0 && g_warthog_fwd_hold_drop - s0.hold_drop == 3 &&
                  simnode_live_allocs() == base_allocs,
              "all three given up, buffers back (%u vs %u)", simnode_live_allocs(), base_allocs);
    }

    /* ---------------------------------------------------------------------
     * 24. a PREQ the radio never sends does not take its step.
     *
     * The gate records an allowed PREQ before it is built; only one that
     * reached the radio counts for the ladder.
     * ------------------------------------------------------------------- */
    printf("\n--- 24. a relay PREQ whose buffer cannot be had is retried, not counted ---\n");
    run_until(mmosal_get_time_ms() + 10000u);
    snap(&s0);
    simnode_outbox_clear();
    {
        struct meshdata d = { .ra = W, .ta = A, .a3 = L4, .a4 = A, .ttl = 31, .seq = 9201,
                              .payload_len = 16 };
        const uint32_t t0 = mmosal_get_time_ms();
        (void)rx_mesh(&d);
        run_until(t0 + 399u);
        simnode_set_tx_alloc_hook(fail_tx_alloc_);
        run_until(t0 + 400u);
        CHECK(preq_for(L4) == 1 && g_warthog_fwd_preq_tx - s0.preq_tx == 1,
              "the second PREQ gets no buffer at +400: nothing on air (%u PREQ)", preq_for(L4));
        const unsigned reg400 = simnode_timeouts_registered();
        run_until(t0 + 799u);
        const unsigned at799 = preq_for(L4), polls = simnode_timeouts_registered() - reg400;
        run_until(t0 + 800u);
        CHECK(at799 == 1 && preq_for(L4) == 2,
              "retried once the gate's interval allows, at +800 (%u then %u)", at799, preq_for(L4));
        CHECK(polls <= 400u / UMAC_MESH_PREQ_GLOBAL_MIN_MS,
              "waiting, the ladder's timeout fires once per %u ms floor, not every ms (%u in 399 ms)",
              (unsigned)UMAC_MESH_PREQ_GLOBAL_MIN_MS, polls);
        run_until(t0 + 6799u);
        CHECK(preq_for(L4) == 5 && g_warthog_fwd_preq_tx - s0.preq_tx == 5,
              "five PREQs on air by 6.8 s, the failed one not among them (%u)", preq_for(L4));
        run_until(t0 + 7200u);
        CHECK(render_relay_held() == 0 && simnode_live_allocs() == base_allocs,
              "given up, buffers back (%u vs %u)", simnode_live_allocs(), base_allocs);
    }

    /* ---------------------------------------------------------------------
     * 25. a held frame whose mesh DA becomes a peer is sent straight to it.
     * ------------------------------------------------------------------- */
    printf("\n--- 25. the destination peers mid-discovery: released to it ---\n");
    run_until(mmosal_get_time_ms() + 10000u);
    snap(&s0);
    simnode_outbox_clear();
    {
        struct meshdata d = { .ra = W, .ta = A, .a3 = LP, .a4 = A, .ttl = 31, .seq = 9301,
                              .payload_len = 16 };
        const uint32_t t0 = mmosal_get_time_ms();
        CHECK(rx_mesh(&d) && render_relay_held() == 1, "held for %s (relay_held=%ld)", mac_(LP),
              render_relay_held());
        CHECK(simnode_add_peer(LP), "%s becomes a peer", mac_(LP));
        simnode_outbox_clear();
        run_until(t0 + 400u);
        const bool ok = parse_data(nth_data(0), &pf);
        CHECK(g_warthog_fwd_hold_tx - s0.hold_tx == 1 && render_relay_held() == 0 && ok &&
                  memcmp(pf.addr1, LP, 6) == 0 && memcmp(pf.addr3, LP, 6) == 0 &&
                  memcmp(pf.addr4, A, 6) == 0 && pf.mc.ttl == 30,
              "released to %s at the next step, mesh SA %s kept, one hop of TTL (hold_tx +%u)", mac_(LP),
              mac_(A), g_warthog_fwd_hold_tx - s0.hold_tx);
        simnode_del_peer(LP);
        run_until(t0 + 7200u);
        CHECK(simnode_live_allocs() == base_allocs, "nothing leaked (%u vs %u)", simnode_live_allocs(),
              base_allocs);
    }

    /* ---------------------------------------------------------------------
     * 26. one core timeout carries every ladder.
     *
     * The umac core's timeout pool is shared with block-ack, defragmentation
     * and connection timers: each hold pulls the one pending timeout earlier
     * rather than registering another.
     * ------------------------------------------------------------------- */
    printf("\n--- 26. twelve holds, one ladder timeout ---\n");
    run_until(mmosal_get_time_ms() + 10000u);
    simnode_outbox_clear();
    {
        struct meshdata d = { .ra = W, .ta = A, .a4 = A, .ttl = 31, .payload_len = 16 };
        const uint32_t t0 = mmosal_get_time_ms();
        const unsigned to0 = simnode_timeouts_pending();
        unsigned most = 0;
        for (unsigned i = 0; i < 12u; i++)
        {
            d.a3 = (i & 1u) ? L6 : L5; d.seq = 9400u + i; (void)rx_mesh(&d);
            unsigned n = simnode_timeouts_pending() - to0;
            if (n > most) { most = n; }
            run_until(mmosal_get_time_ms() + 30u);
        }
        CHECK(most == 1u && simnode_timeouts_pending() - to0 == 1u,
              "never more than one pending (at most +%u, +%u now)", most, simnode_timeouts_pending() - to0);
        run_until(t0 + 7500u);
        CHECK(simnode_timeouts_pending() == to0 && render_relay_held() == 0 &&
                  simnode_live_allocs() == base_allocs,
              "and none left after the give-up (%u pending)", simnode_timeouts_pending());
    }

    /* ---------------------------------------------------------------------
     * 27. a PREP handled at the instant a step falls due releases first.
     *
     * The flush releases before it asks, so no PREQ goes out for a target
     * whose path has just been installed.
     * ------------------------------------------------------------------- */
    printf("\n--- 27. a PREP on a due step: released, no PREQ after the path ---\n");
    run_until(mmosal_get_time_ms() + 10000u);
    snap(&s0);
    simnode_outbox_clear();
    {
        struct meshdata d = { .ra = W, .ta = A, .a3 = L7, .a4 = A, .ttl = 31, .seq = 9501,
                              .payload_len = 16 };
        const uint32_t t0 = mmosal_get_time_ms();
        (void)rx_mesh(&d);
        run_until(t0 + 399u);
        simnode_set_time_ms(t0 + 400u); /* the step is due; the PREP is handled first */
        (void)install_path(L7, B, 70);
        simnode_pump();
        (void)simnode_run_timeouts();
        CHECK(preq_for(L7) == 1 && g_warthog_fwd_hold_tx - s0.hold_tx == 1 && render_relay_held() == 0,
              "one PREQ in all, the frame released (%u PREQ, hold_tx +%u)", preq_for(L7),
              g_warthog_fwd_hold_tx - s0.hold_tx);
        run_until(t0 + 7200u);
        CHECK(preq_for(L7) == 1 && simnode_live_allocs() == base_allocs, "and no re-ask after it (%u)",
              preq_for(L7));
    }

    /* ---------------------------------------------------------------------
     * 28. a frame that arrives at the give-up instant starts a new discovery.
     *
     * Receive and the ladder's timeout share the event loop in no fixed order:
     * a frame handled first must not join the discovery that is just ending.
     * ------------------------------------------------------------------- */
    printf("\n--- 28. a frame at the give-up instant is asked for afresh ---\n");
    run_until(mmosal_get_time_ms() + 10000u);
    snap(&s0);
    simnode_outbox_clear();
    {
        struct meshdata d = { .ra = W, .ta = A, .a3 = L3, .a4 = A, .ttl = 31, .seq = 9601,
                              .payload_len = 16 };
        const uint32_t t0 = mmosal_get_time_ms();
        (void)rx_mesh(&d);
        run_until(t0 + 6799u);
        simnode_outbox_clear();
        simnode_set_time_ms(t0 + 6800u); /* received before the give-up timeout runs */
        d.seq = 9602;
        (void)rx_mesh(&d);
        (void)simnode_run_timeouts();
        CHECK(render_relay_held() == 1 && preq_for(L3) == 1 && g_warthog_fwd_hold_drop - s0.hold_drop == 1,
              "the old one given up, the new one held and asked for (relay_held=%ld, %u PREQ, drop +%u)",
              render_relay_held(), preq_for(L3), g_warthog_fwd_hold_drop - s0.hold_drop);
        run_until(t0 + 6800u + 7200u);
        CHECK(render_relay_held() == 0 && simnode_live_allocs() == base_allocs, "and given up in turn");
    }

    /* ---------------------------------------------------------------------
     * 29. a unicast whose mesh DA is a group address is never discovered.
     *
     * mac80211's mesh_path_add refuses a multicast destination, and a
     * broadcast Target Only PREQ reads as a root announcement that even
     * forwarding-off nodes re-flood.
     * ------------------------------------------------------------------- */
    printf("\n--- 29. mesh DA a group address: dropped as no-path, no PREQ ---\n");
    run_until(mmosal_get_time_ms() + 10000u);
    snap(&s0);
    simnode_outbox_clear();
    {
        extern volatile uint32_t g_warthog_rxdrop_reason;
        struct meshdata d = { .ra = W, .ta = A, .a3 = GRP, .a4 = A, .ttl = 31, .seq = 9701,
                              .payload_len = 16 };
        (void)rx_mesh(&d);
        const uint32_t why = g_warthog_rxdrop_reason;
        d.a3 = BCAST; d.seq = 9702;
        (void)rx_mesh(&d);
        run_until(mmosal_get_time_ms() + 7200u);
        CHECK(preq_for(GRP) == 0 && preq_for(BCAST) == 0 && g_warthog_fwd_hold - s0.hold == 0 &&
                  render_relay_held() == 0 && g_warthog_fwd_drop_nopath - s0.d_nopath == 2 &&
                  why == 106u && g_warthog_rxdrop_reason == 106u && count_data() == 0,
              "multicast and broadcast mesh DA: no PREQ, nothing held, two no-path drops, rxdrop 106 "
              "(%u/%u PREQ, hold +%u, nopath +%u, reason %lu)", preq_for(GRP), preq_for(BCAST),
              g_warthog_fwd_hold - s0.hold, g_warthog_fwd_drop_nopath - s0.d_nopath,
              (unsigned long)g_warthog_rxdrop_reason);
    }

    /* ---------------------------------------------------------------------
     * 30. every route our own discoveries take carries vanilla's 4882 TU.
     *
     * Only the relay ladder asks with 48828 TU; our own re-ask on the tick, a
     * refresh of a path we send on and the TX classifier keep 4882 TU.
     * ------------------------------------------------------------------- */
    printf("\n--- 30. our own PREQs, by every route, keep 4882 TU ---\n");
    run_until(mmosal_get_time_ms() + 10000u);
    simnode_outbox_clear();
    {
        struct mmpkt *p = mk_8023(OQ1, W, 16);
        CHECK(p != NULL && umac_mesh_fwd_glue_tx_pending(umacd, p, OQ1) && preq_for(OQ1) == 0,
              "our frame for %s is held, no PREQ yet", mac_(OQ1));
        simnode_tick();
        CHECK(preq_for(OQ1) == 1 && preq_lifetime(OQ1) == 4882u,
              "the tick's re-ask: %lu TU", (unsigned long)preq_lifetime(OQ1));

        run_until(mmosal_get_time_ms() + 1000u);
        (void)install_path(OQ2, B, 81);
        run_until(mmosal_get_time_ms() + 4200u);
        uint8_t nh[6];
        const bool via = umac_mesh_fwd_glue_next_hop(OQ2, nh);
        simnode_pump(); /* the netif task queues its PREQ; the event loop sends it */
        CHECK(via && preq_for(OQ2) == 1 && preq_lifetime(OQ2) == 4882u,
              "a refresh of a path we send on, under 1 s left: %lu TU", (unsigned long)preq_lifetime(OQ2));

        run_until(mmosal_get_time_ms() + 1000u);
        struct mmpkt *q = mk_8023(OQ3, W, 16);
        if (q != NULL)
        {
            umac_mesh_fwd_glue_tx_classify(q, OQ3, W);
            mmpkt_release(q);
        }
        simnode_pump();
        CHECK(preq_for(OQ3) == 1 && preq_lifetime(OQ3) == 4882u, "the TX classifier's: %lu TU",
              (unsigned long)preq_lifetime(OQ3));
        run_until(mmosal_get_time_ms() + 4000u);
        simnode_tick();
        CHECK(render_pending() == 0 && simnode_live_allocs() == base_allocs, "our held frame expired, nothing leaked");
    }

    /* ---------------------------------------------------------------------
     * 31. path selection another task sends goes out from the event loop.
     *
     * Off the loop every group HWMP frame is queued for it, AT+MESHPMF=0 included:
     * the send's frame and the driver below it overflow lwIP's 3.5 KB tcpip task.
     * Queued is not sent: preq_tx and the gate's record of the last PREQ (what a
     * relay ladder step is taken by) wait for the loop to send it, and a send the
     * loop cannot make is counted qfail. The 2 s tick runs its re-asks on the loop,
     * so the probe task queues nothing.
     * ------------------------------------------------------------------- */
    printf("\n--- 31. path selection from other tasks: queued, counted once sent ---\n");
    run_until(mmosal_get_time_ms() + 10000u);
    simnode_tick();
    run_until(mmosal_get_time_ms() + 10000u);
    snap(&s0);
    simnode_outbox_clear();
    {
        static const uint8_t N1[6] = { 0x02, 0, 0, 0, 0, 0x91 };
        static const uint8_t N2[6] = { 0x02, 0, 0, 0, 0, 0x92 };
        static const uint8_t N3[6] = { 0x02, 0, 0, 0, 0, 0x93 };
        extern volatile uint32_t g_warthog_hwmp_tx_qdrop, g_warthog_hwmp_tx_qfail;
        void umac_mesh_service_tick(void);
        simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/false);
        const uint32_t qd0 = g_warthog_hwmp_tx_qdrop, qf0 = g_warthog_hwmp_tx_qfail;

        CHECK(simnode_host_tx_nopump(N1, W, payload, sizeof(payload)) && preq_for(N1) == 0 &&
                  simnode_evt_pending() == 1u && g_warthog_fwd_preq_tx == s0.preq_tx,
              "a frame for undiscovered %s from the netif task: its PREQ is queued, not sent or counted",
              mac_(N1));
        simnode_set_tx_alloc_hook(fail_tx_alloc_);
        simnode_pump();
        simnode_set_tx_alloc_hook(NULL);
        CHECK(preq_for(N1) == 0 && g_warthog_hwmp_tx_qfail - qf0 == 1 && g_warthog_fwd_preq_tx == s0.preq_tx &&
                  g_warthog_hwmp_tx_qdrop == qd0,
              "the loop gets no buffer for it: nothing on air, counted qfail, not preq_tx (qfail +%u, preq_tx +%u)",
              g_warthog_hwmp_tx_qfail - qf0, g_warthog_fwd_preq_tx - s0.preq_tx);
        simnode_advance_ms(UMAC_MESH_PREQ_MIN_INTERVAL_MS);
        CHECK(simnode_host_tx_nopump(N1, W, payload, sizeof(payload)) && preq_for(N1) == 0, "asked again at +%u",
              (unsigned)UMAC_MESH_PREQ_MIN_INTERVAL_MS);
        simnode_pump();
        CHECK(preq_for(N1) == 1 && g_warthog_fwd_preq_tx - s0.preq_tx == 1 && g_warthog_hwmp_tx_qfail - qf0 == 1,
              "the loop sends it: on air and counted, once (preq_tx +%u)", g_warthog_fwd_preq_tx - s0.preq_tx);

        /* The relay holds a frame for N2 and asks at t0; our own frame for N2 asks at +400,
         * when the ladder's second step falls due. Once the loop has sent it, it takes the step. */
        run_until(mmosal_get_time_ms() + 1000u);
        simnode_outbox_clear();
        struct meshdata d = { .ra = W, .ta = A, .a3 = N2, .a4 = A, .ttl = 31, .seq = 9701,
                              .payload_len = 16 };
        const uint32_t t0 = mmosal_get_time_ms();
        CHECK(rx_mesh(&d) && render_relay_held() == 1 && preq_for(N2) == 1,
              "a relayed frame for %s is held and asked for at once", mac_(N2));
        simnode_advance_ms(UMAC_MESH_RELAY_DISC_FIRST_MS);
        (void)simnode_host_tx_nopump(N2, W, payload, sizeof(payload));
        simnode_pump();
        CHECK(preq_for(N2) == 2, "our own frame for %s asks at +%u, from the netif task via the loop", mac_(N2),
              (unsigned)UMAC_MESH_RELAY_DISC_FIRST_MS);
        run_until(t0 + 1199u);
        CHECK(preq_for(N2) == 2,
              "that PREQ took the ladder's second step: no relay PREQ again at +800 (%u by +1199)", preq_for(N2));
        run_until(t0 + 1200u);
        CHECK(preq_for(N2) == 3, "the third step asks at +1200 (%u)", preq_for(N2));

        /* The probe task's tick only posts: the re-ask is built and sent on the loop. */
        run_until(mmosal_get_time_ms() + 1000u);
        CHECK(simnode_host_tx(N3, W, payload, sizeof(payload)) && preq_for(N3) == 1,
              "a frame for %s is held, one PREQ out", mac_(N3));
        simnode_advance_ms(600u);
        const uint32_t tx0 = g_warthog_fwd_preq_tx;
        umac_mesh_service_tick();
        const unsigned posted = simnode_evt_pending();
        const unsigned early = preq_for(N3);
        simnode_pump();
        CHECK(posted == 1u && early == 1u && preq_for(N3) == 2 && g_warthog_fwd_preq_tx - tx0 >= 1u &&
                  g_warthog_hwmp_tx_qdrop == qd0,
              "the tick posts one event and its re-ask goes out from the loop (%u posted, %u then %u PREQ)",
              posted, early, preq_for(N3));

        run_until(mmosal_get_time_ms() + 8000u);
        simnode_tick();
        CHECK(render_pending() == 0 && render_relay_held() == 0 && simnode_live_allocs() == base_allocs,
              "every held frame lapsed, nothing leaked (%u vs %u)", simnode_live_allocs(), base_allocs);
    }

    /* ---------------------------------------------------------------------
     * 32. a PREQ queued off the loop goes out with a current own SN.
     *
     * The netif task queues it (drawing an SN); the loop may send something
     * newer first -- here a PREP answering A's PREQ for us. A peer drops an
     * older own SN as stale, so the drain must restamp it.
     * ------------------------------------------------------------------- */
    printf("\n--- 32. a queued PREQ is not overtaken by a newer own SN ---\n");
    run_until(mmosal_get_time_ms() + 10000u);
    simnode_tick();
    run_until(mmosal_get_time_ms() + 10000u);
    simnode_outbox_clear();
    {
        static const uint8_t N4[6] = { 0x02, 0, 0, 0, 0, 0x94 };
        extern uint32_t *umac_mesh_hwmp_own_sn_ptr(void);
        uint32_t *psn = umac_mesh_hwmp_own_sn_ptr();
        simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/false);
        const uint32_t sn0 = *psn;
        const uint32_t ptx0 = g_warthog_fwd_preq_tx;
        CHECK(simnode_host_tx_nopump(N4, W, payload, sizeof(payload)) && preq_for(N4) == 0 &&
                  simnode_evt_pending() == 1u, "netif task: PREQ for %s queued (own sn %u -> %u)", mac_(N4), sn0, *psn);
        /* The loop runs the datapath before events: A's PREQ for us is answered first. */
        uint8_t b[64];
        uint16_t n = umac_mesh_hwmp_build_preq(b, sizeof(b), A, 500, 77, W, 5000);
        hwmp_rx_(b, n, A, psn, false, false);
        simnode_pump();
        const struct simnode_frame *fp = find_hwmp(HWMP_EID_PREP, 0);
        const struct simnode_frame *fq = find_hwmp(HWMP_EID_PREQ, 0);
        int ip = -1, iq = -1;
        for (unsigned i = 0; i < simnode_outbox_count(); i++)
        {
            if (simnode_outbox_get(i) == fp) ip = (int)i;
            if (simnode_outbox_get(i) == fq) iq = (int)i;
        }
        uint16_t bl = 0; const uint8_t *bb = NULL;
        struct hwmp_prep pr; struct hwmp_preq pq;
        memset(&pr, 0, sizeof(pr)); memset(&pq, 0, sizeof(pq));
        bool okp = fp && (bb = act_body(fp, &bl)) && umac_mesh_hwmp_parse_prep(bb, bl, &pr);
        bool okq = fq && (bb = act_body(fq, &bl)) && umac_mesh_hwmp_parse_preq(bb, bl, &pq);
        printf("     PREP out #%d target_sn=%u ; PREQ out #%d orig_sn=%u target=%s ; preq_tx +%u\n", ip,
               pr.target_sn, iq, pq.orig_sn, mac_(pq.target_addr), g_warthog_fwd_preq_tx - ptx0);
        /* A as a warthog relay: learns W from the PREP, then receives the PREQ. */
        static struct umac_mesh_pathtbl at;
        umac_mesh_pathtbl_init(&at);
        uint32_t asn = 500;
        struct umac_mesh_hwmp_ctx ac = { .own_addr = A, .tbl = &at, .forwarding = true, .link_metric = 4096,
                                         .max_lifetime_ms = UMAC_MESH_PATH_LIFETIME_MAX_MS,
                                         .now_ms = mmosal_get_time_ms(), .own_sn = &asn };
        struct umac_mesh_hwmp_action act; enum umac_mesh_hwmp_drop why;
        bb = act_body(fp, &bl);
        (void)umac_mesh_hwmp_relay(&ac, bb, bl, W, &act, &why);
        printf("     A on the PREP: kind=%d why=%d\n", act.kind, why);
        bb = act_body(fq, &bl);
        (void)umac_mesh_hwmp_relay(&ac, bb, bl, W, &act, &why);
        printf("     A on the PREQ: kind=%d why=%d (REBROADCAST=%d NOT_FRESH=%d)\n", act.kind, why,
               UMAC_MESH_HWMP_REBROADCAST_PREQ, UMAC_MESH_HWMP_DROP_NOT_FRESH);
        CHECK(!(ip < iq && okp && okq && pr.target_sn > pq.orig_sn && act.kind != UMAC_MESH_HWMP_REBROADCAST_PREQ),
              "the queued PREQ does not reach A behind a newer own SN, which A would drop");
        run_until(mmosal_get_time_ms() + 8000u);
        simnode_tick();
    }

    /* ---------------------------------------------------------------------
     * 33. the service event, queued ahead of the drain, re-asks a held target
     * with a newer own SN; the queued PREQ must still go out after it, newer.
     * ------------------------------------------------------------------- */
    printf("\n--- 33. the tick's re-ask ahead of the drain ---\n");
    run_until(mmosal_get_time_ms() + 10000u);
    simnode_tick();
    run_until(mmosal_get_time_ms() + 10000u);
    simnode_outbox_clear();
    {
        static const uint8_t D1[6] = { 0x02, 0, 0, 0, 0, 0x95 };
        static const uint8_t D2[6] = { 0x02, 0, 0, 0, 0, 0x96 };
        void umac_mesh_service_tick(void);
        CHECK(simnode_host_tx(D1, W, payload, sizeof(payload)) && preq_for(D1) == 1, "a frame for %s held, PREQ out", mac_(D1));
        simnode_advance_ms(500u);
        simnode_outbox_clear();
        umac_mesh_service_tick();                    /* probe task posts the service event */
        (void)simnode_host_tx_nopump(D2, W, payload, sizeof(payload)); /* netif task queues D2's PREQ */
        simnode_advance_ms(60u);                     /* the loop was busy for 60 ms */
        simnode_pump();
        struct hwmp_preq q0, q1; uint16_t bl = 0; const uint8_t *bb;
        memset(&q0, 0, sizeof(q0)); memset(&q1, 0, sizeof(q1));
        const struct simnode_frame *f0 = find_hwmp(HWMP_EID_PREQ, 0), *f1 = find_hwmp(HWMP_EID_PREQ, 1);
        bool ok0 = f0 && (bb = act_body(f0, &bl)) && umac_mesh_hwmp_parse_preq(bb, bl, &q0);
        bool ok1 = f1 && (bb = act_body(f1, &bl)) && umac_mesh_hwmp_parse_preq(bb, bl, &q1);
        printf("     on air: #0 target=%s orig_sn=%u ; #1 target=%s orig_sn=%u\n",
               ok0 ? mac_(q0.target_addr) : "-", q0.orig_sn, ok1 ? mac_(q1.target_addr) : "-", q1.orig_sn);
        static struct umac_mesh_pathtbl at;
        umac_mesh_pathtbl_init(&at);
        uint32_t asn = 900;
        struct umac_mesh_hwmp_ctx ac = { .own_addr = A, .tbl = &at, .forwarding = true, .link_metric = 4096,
                                         .max_lifetime_ms = UMAC_MESH_PATH_LIFETIME_MAX_MS,
                                         .now_ms = mmosal_get_time_ms(), .own_sn = &asn };
        struct umac_mesh_hwmp_action a0, a1; enum umac_mesh_hwmp_drop w0, w1;
        bb = act_body(f0, &bl); (void)umac_mesh_hwmp_relay(&ac, bb, bl, W, &a0, &w0);
        bb = act_body(f1, &bl); (void)umac_mesh_hwmp_relay(&ac, bb, bl, W, &a1, &w1);
        printf("     relay A: #0 kind=%d why=%d ; #1 kind=%d why=%d\n", a0.kind, w0, a1.kind, w1);
        CHECK(!(ok0 && ok1 && q0.orig_sn > q1.orig_sn && a1.kind != UMAC_MESH_HWMP_REBROADCAST_PREQ),
              "D2's queued PREQ does not go out behind the tick's newer one");
        run_until(mmosal_get_time_ms() + 8000u);
        simnode_tick();
    }

    CHECK(simnode_live_allocs() == base_allocs,
          "at the end, live allocations are back to the baseline (%u vs %u)",
          simnode_live_allocs(), base_allocs);

    simnode_stop();

    printf("\n%d checks\n", checks);
    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_simnode_glue: all passed\n");
    return 0;
}
