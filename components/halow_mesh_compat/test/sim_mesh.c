/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Multi-node mesh simulator driving the SHIPPING forwarding code.
 *
 * N node states -- each its own path table, multicast cache, HWMP counters --
 * joined by an adjacency matrix, with a single queue standing in for the air.
 * A frame popped from the queue is offered to every node adjacent to its
 * transmitter; each such node runs the same umac_mesh_hwmp_relay() and
 * umac_mesh_fwd_rx() the firmware runs, and whatever those decide is queued
 * again with that node as transmitter. Nothing here models radio; what it
 * proves is that the DECISIONS compose into a mesh: paths form through a
 * relay, unicast crosses it exactly once, a group frame reaches every node
 * exactly once and never storms, proxied hosts on opposite sides reach each
 * other with their real addresses, a lost link is announced, and a node with
 * forwarding off relays nothing.
 *
 * A queue that will not drain is itself a failure: the run cap is the
 * broadcast-storm detector.
 */
#include "umac_mesh_fwd.h"
#include "umac_mesh_hwmp_relay.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

#define NMAX 6
#define QMAX 512
#define RUN_CAP 4000
#define LOGMAX 128

struct node {
    uint8_t addr[6];
    struct umac_mesh_pathtbl tbl;
    struct umac_mesh_rmc rmc;
    uint32_t own_sn, preq_id, mesh_seq;
    bool forwarding;
    /* what reached this node's host side */
    struct { uint8_t da[6], sa[6]; uint32_t seq; bool group; } log[LOGMAX];
    int nlog;
    uint32_t tx, fwd_data, fwd_preq, fwd_prep, prep_sent, perr_sent, preq_sent, dup, ttl_drop;
};

enum fkind { F_DATA, F_ACTION };
struct frame {
    enum fkind kind;
    int from;                 /* node index of the transmitter */
    uint8_t ta[6];
    uint8_t ra[6];
    /* data */
    struct umac_mesh_rx_frame d;
    uint32_t payload;         /* an id, so a delivery can be matched to its send */
    /* action */
    uint8_t body[HWMP_PREQ_BODY_LEN];
    uint16_t body_len;
};

static struct {
    struct node n[NMAX];
    int nn;
    bool adj[NMAX][NMAX];
    struct frame q[QMAX];
    int qh, qt;
    uint32_t now;
    uint32_t steps;
    bool overflow;
} S;

static const uint8_t BC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static bool eq(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 6) == 0; }

static int idx_of(const uint8_t *addr)
{
    for (int i = 0; i < S.nn; i++) if (eq(S.n[i].addr, addr)) return i;
    return -1;
}

static bool is_peer_cb(const uint8_t *addr, void *arg)
{
    int me = (int)(intptr_t)arg;
    int j = idx_of(addr);
    return j >= 0 && S.adj[me][j];
}

static void sim_reset(int nn, bool forwarding_all)
{
    memset(&S, 0, sizeof(S));
    S.nn = nn;
    S.now = 100000;
    for (int i = 0; i < nn; i++) {
        struct node *n = &S.n[i];
        uint8_t a[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, (uint8_t)(0xa0 + i) };
        memcpy(n->addr, a, 6);
        umac_mesh_pathtbl_init(&n->tbl);
        umac_mesh_rmc_init(&n->rmc);
        n->own_sn = 10 * (uint32_t)(i + 1);
        n->forwarding = forwarding_all;
    }
}

static void link(int a, int b) { S.adj[a][b] = S.adj[b][a] = true; }
static void unlink_(int a, int b) { S.adj[a][b] = S.adj[b][a] = false; }

static void push(const struct frame *f)
{
    if (S.qt - S.qh >= QMAX) { S.overflow = true; return; } /* a storm; run() reports it */
    S.q[S.qt++ % QMAX] = *f;
    S.n[f->from].tx++;
}

static struct umac_mesh_fwd_ctx fctx(int i)
{
    struct umac_mesh_fwd_ctx c = { .own_addr = S.n[i].addr, .tbl = &S.n[i].tbl, .rmc = &S.n[i].rmc,
                                   .forwarding = S.n[i].forwarding, .element_ttl = 31, .now_ms = S.now,
                                   .is_peer = is_peer_cb, .is_peer_arg = (void *)(intptr_t)i };
    return c;
}

static struct umac_mesh_hwmp_ctx hctx(int i)
{
    struct umac_mesh_hwmp_ctx c = { .own_addr = S.n[i].addr, .tbl = &S.n[i].tbl,
                                    .forwarding = S.n[i].forwarding, .link_metric = 100,
                                    .path_lifetime_ms = 5120, .now_ms = S.now, .own_sn = &S.n[i].own_sn };
    return c;
}

static void queue_action(int from, const uint8_t *ra, const uint8_t *body, uint16_t len)
{
    struct frame f; memset(&f, 0, sizeof(f));
    f.kind = F_ACTION; f.from = from; memcpy(f.ta, S.n[from].addr, 6); memcpy(f.ra, ra, 6);
    memcpy(f.body, body, len); f.body_len = len;
    push(&f);
}

/* One node receives one frame and acts on it. */
static void receive(int me, const struct frame *f)
{
    struct node *n = &S.n[me];
    if (f->kind == F_ACTION) {
        struct umac_mesh_hwmp_ctx c = hctx(me);
        struct umac_mesh_hwmp_action a; enum umac_mesh_hwmp_drop why;
        umac_mesh_hwmp_relay(&c, f->body, f->body_len, f->ta, &a, &why);
        switch (a.kind) {
        case UMAC_MESH_HWMP_SEND_PREP:       n->prep_sent++; queue_action(me, a.to, a.body, a.body_len); break;
        case UMAC_MESH_HWMP_REBROADCAST_PREQ:n->fwd_preq++;  queue_action(me, a.to, a.body, a.body_len); break;
        case UMAC_MESH_HWMP_FORWARD_PREP:    n->fwd_prep++;  queue_action(me, a.to, a.body, a.body_len); break;
        case UMAC_MESH_HWMP_FORWARD_PERR:    n->perr_sent++; queue_action(me, a.to, a.body, a.body_len); break;
        default: break;
        }
        return;
    }
    struct umac_mesh_fwd_ctx c = fctx(me);
    struct umac_mesh_fwd_rx_result r;
    umac_mesh_fwd_rx(&c, &f->d, &r);
    if (r.drop == UMAC_MESH_FWD_DROP_DUP) n->dup++;
    if (r.drop == UMAC_MESH_FWD_DROP_TTL) n->ttl_drop++;
    if (r.verdict == UMAC_MESH_FWD_DELIVER || r.verdict == UMAC_MESH_FWD_DELIVER_AND_FORWARD) {
        if (n->nlog < LOGMAX) {
            memcpy(n->log[n->nlog].da, r.deliver_da, 6); memcpy(n->log[n->nlog].sa, r.deliver_sa, 6);
            n->log[n->nlog].seq = f->payload; n->log[n->nlog].group = f->d.group; n->nlog++;
        }
    }
    if (r.verdict == UMAC_MESH_FWD_FORWARD || r.verdict == UMAC_MESH_FWD_DELIVER_AND_FORWARD) {
        struct frame g = *f;
        g.from = me; memcpy(g.ta, n->addr, 6); memcpy(g.ra, r.fwd_ra, 6);
        memcpy(g.d.addr1, r.fwd_ra, 6); memcpy(g.d.addr2, n->addr, 6);
        g.d.mc = r.fwd_mc;
        n->fwd_data++;
        push(&g);
    }
    if (r.send_perr) { n->perr_sent++; queue_action(me, r.perr_to, r.perr_body, r.perr_len); }
}

/* Drain the air. Returns false on a storm: the run cap hit or the queue overflowed. */
static bool run(void)
{
    uint32_t steps = 0;
    while (S.qh < S.qt) {
        struct frame f = S.q[S.qh++ % QMAX];
        if (++steps > RUN_CAP || S.overflow) return false;
        S.steps++;
        for (int j = 0; j < S.nn; j++) {
            if (j == f.from || !S.adj[f.from][j]) continue;
            if (!eq(f.ra, BC) && !eq(f.ra, S.n[j].addr)) continue; /* unicast: only the RA hears it usefully */
            receive(j, &f);
        }
    }
    return true;
}

/* Node i originates a PREQ for target. */
static void discover(int i, const uint8_t *target)
{
    uint8_t body[HWMP_PREQ_BODY_LEN];
    uint16_t n = umac_mesh_hwmp_build_preq(body, sizeof(body), S.n[i].addr, ++S.n[i].own_sn,
                                           ++S.n[i].preq_id, target, 4882);
    S.n[i].preq_sent++;
    queue_action(i, BC, body, n);
}

/* Node i sends an 802.3 frame (da, sa). Returns false if it had no route. */
static bool send(int i, const uint8_t *da, const uint8_t *sa, uint32_t payload)
{
    struct umac_mesh_fwd_ctx c = fctx(i);
    struct umac_mesh_fwd_tx_result t;
    umac_mesh_fwd_tx(&c, da, sa, S.n[i].mesh_seq++, &t);
    if (!t.ok) return false;
    struct frame f; memset(&f, 0, sizeof(f));
    f.kind = F_DATA; f.from = i; memcpy(f.ta, S.n[i].addr, 6); memcpy(f.ra, t.ra, 6);
    f.payload = payload;
    f.d.group = (t.shape == UMAC_MESH_TX_GROUP_3ADDR);
    memcpy(f.d.addr1, t.ra, 6); memcpy(f.d.addr2, S.n[i].addr, 6);
    memcpy(f.d.addr3, t.addr3, 6); memcpy(f.d.addr4, t.addr4, 6);
    f.d.mc = t.mc;
    push(&f);
    return true;
}

static int delivered(int i, uint32_t payload, const uint8_t *da, const uint8_t *sa)
{
    int k = 0;
    for (int j = 0; j < S.n[i].nlog; j++)
        if (S.n[i].log[j].seq == payload && (!da || eq(S.n[i].log[j].da, da)) && (!sa || eq(S.n[i].log[j].sa, sa))) k++;
    return k;
}

static const struct umac_mesh_path *path(int i, int to)
{
    return umac_mesh_path_lookup(&S.n[i].tbl, S.n[to].addr, S.now);
}

int main(void)
{
    /* ================= 1. Line A - W - B: paths form through W ============ */
    printf("--- scenario 1: A - W - B unicast relay ---\n");
    sim_reset(3, true); link(0, 1); link(1, 2);
    #define A 0
    #define Wn 1
    #define B 2
    discover(A, S.n[B].addr);
    CHECK(run(), "discovery drains (no storm), %u steps", S.steps);
    CHECK(S.n[Wn].fwd_preq == 1, "W rebroadcast the PREQ once (got %u)", S.n[Wn].fwd_preq);
    CHECK(S.n[B].prep_sent == 1, "B answered with one PREP");
    CHECK(S.n[Wn].fwd_prep == 1, "W carried the PREP back once");
    const struct umac_mesh_path *p;
    p = path(A, B); CHECK(p && eq(p->next_hop, S.n[Wn].addr) && p->hop_count == 2, "A: path to B via W, 2 hops");
    p = path(Wn, A); CHECK(p && eq(p->next_hop, S.n[A].addr), "W: path to A via A");
    p = path(Wn, B); CHECK(p && eq(p->next_hop, S.n[B].addr), "W: path to B via B");
    p = path(B, A); CHECK(p && eq(p->next_hop, S.n[Wn].addr) && p->hop_count == 2, "B: path to A via W, 2 hops");
    CHECK(p && p->metric == 200, "B's metric to A is two hops' worth (got %u)", (unsigned)p->metric);

    CHECK(send(A, S.n[B].addr, S.n[A].addr, 1001), "A can send to B");
    CHECK(run(), "unicast drains");
    CHECK(delivered(B, 1001, S.n[B].addr, S.n[A].addr) == 1, "B received A's frame exactly once, addressed B<-A");
    CHECK(delivered(Wn, 1001, NULL, NULL) == 0, "W did not deliver it to its own host");
    CHECK(S.n[Wn].fwd_data == 1, "W forwarded exactly one data frame");
    CHECK(send(B, S.n[A].addr, S.n[B].addr, 1002) && run() && delivered(A, 1002, S.n[A].addr, S.n[B].addr) == 1,
          "and B's reply reached A exactly once");

    /* ================= 2. Group flood along a 4-node line ================= */
    printf("--- scenario 2: group flood A - W1 - W2 - B ---\n");
    sim_reset(4, true); link(0, 1); link(1, 2); link(2, 3);
    CHECK(send(0, BC, S.n[0].addr, 2001), "A broadcasts");
    CHECK(run(), "flood drains, %u steps", S.steps);
    for (int i = 1; i < 4; i++)
        CHECK(delivered(i, 2001, BC, S.n[0].addr) == 1, "node %d got the broadcast exactly once", i);
    CHECK(delivered(0, 2001, NULL, NULL) == 0, "A did not get its own broadcast back");
    CHECK(S.n[1].fwd_data == 1 && S.n[2].fwd_data == 1, "each relay rebroadcast once");
    CHECK(S.n[3].fwd_data == 1, "B (the end) also rebroadcasts once -- it cannot know it is the end");
    CHECK(S.n[1].dup >= 1, "W1 saw its own frame come back via W2 and dropped it as duplicate/own");

    /* ================= 3. Triangle: RMC stops the storm =================== */
    printf("--- scenario 3: triangle A - W - B with A - B ---\n");
    sim_reset(3, true); link(0, 1); link(1, 2); link(0, 2);
    CHECK(send(0, BC, S.n[0].addr, 3001), "A broadcasts into a triangle");
    CHECK(run(), "triangle drains (RMC stops the storm), %u steps", S.steps);
    CHECK(delivered(1, 3001, NULL, NULL) == 1 && delivered(2, 3001, NULL, NULL) == 1, "W and B each delivered exactly once");
    CHECK(S.n[1].fwd_data == 1 && S.n[2].fwd_data == 1, "each rebroadcast exactly once, not per copy heard");
    CHECK(S.n[1].dup + S.n[2].dup >= 2, "the second copies were caught by the caches");

    /* ================= 4. Hosts behind A and B talk across W ============== */
    printf("--- scenario 4: proxied host-to-host across a relay ---\n");
    sim_reset(3, true); link(0, 1); link(1, 2);
    const uint8_t HA[6] = { 0x00, 0xe0, 0x4f, 0, 0, 0xa1 }, HB[6] = { 0x00, 0xe0, 0x4f, 0, 0, 0xb1 };
    discover(A, S.n[B].addr); run();
    /* HA behind A broadcasts (ARP-like): everyone learns HA sits behind A. */
    CHECK(send(A, BC, HA, 4001) && run(), "HA broadcasts through A");
    CHECK(delivered(B, 4001, BC, HA) == 1, "B delivered it as from HA, the real source");
    CHECK(umac_mesh_proxy_lookup(&S.n[B].tbl, HA, S.now) && eq(umac_mesh_proxy_lookup(&S.n[B].tbl, HA, S.now), S.n[A].addr),
          "B learned HA is behind A");
    CHECK(umac_mesh_proxy_lookup(&S.n[Wn].tbl, HA, S.now) != NULL, "W learned it too");
    /* HB replies unicast to HA: B shapes AE 2 toward A, W forwards, A delivers HB->HA. */
    CHECK(send(B, HA, HB, 4002), "HB can send to HA (B knows the proxy)");
    CHECK(run(), "drains");
    CHECK(delivered(A, 4002, HA, HB) == 1, "A delivered HB's frame to HA with the real endpoints, exactly once");
    CHECK(S.n[Wn].fwd_data == 2, "W forwarded both (the broadcast and the unicast)");
    CHECK(umac_mesh_proxy_lookup(&S.n[A].tbl, HB, S.now) && eq(umac_mesh_proxy_lookup(&S.n[A].tbl, HB, S.now), S.n[B].addr),
          "A learned HB is behind B from the AE 2 frame");
    CHECK(send(A, HB, HA, 4003) && run() && delivered(B, 4003, HB, HA) == 1, "and HA -> HB works the other way");

    /* ================= 5. Link loss: PERR announces it ==================== */
    printf("--- scenario 5: W loses B ---\n");
    sim_reset(3, true); link(0, 1); link(1, 2);
    discover(A, S.n[B].addr); run();
    CHECK(path(A, B) != NULL, "A has a path to B");
    unlink_(1, 2);
    {
        struct umac_mesh_hwmp_ctx c = hctx(Wn);
        struct umac_mesh_hwmp_action acts[4];
        uint32_t k = umac_mesh_hwmp_lose_neighbour(&c, S.n[B].addr, acts, 4);
        CHECK(k == 1, "W emits one PERR for B");
        for (uint32_t i = 0; i < k; i++) { S.n[Wn].perr_sent++; queue_action(Wn, acts[i].to, acts[i].body, acts[i].body_len); }
    }
    CHECK(run(), "PERR drains");
    CHECK(path(A, B) == NULL, "A's path to B is invalidated by W's PERR");
    CHECK(!send(A, S.n[B].addr, S.n[A].addr, 5001), "A now has no route to B and would ask for a PREQ");
    /* A forwards the PERR once -- it cannot know nothing lies beyond it -- and
     * W, whose path to B was already dead, does NOT echo A's copy: a PERR that
     * changed nothing is not forwarded, which is what ends it. */
    CHECK(S.n[A].perr_sent == 1, "A forwarded the PERR once (got %u)", S.n[A].perr_sent);
    CHECK(S.n[Wn].perr_sent == 1, "W sent only its own PERR and did not echo A's (got %u)", S.n[Wn].perr_sent);

    /* ================= 6. Leaf: forwarding off, nothing crosses ============ */
    printf("--- scenario 6: W is a leaf ---\n");
    sim_reset(3, false); link(0, 1); link(1, 2);
    discover(A, S.n[B].addr); run();
    CHECK(S.n[Wn].fwd_preq == 0, "leaf W did not rebroadcast the PREQ");
    CHECK(path(A, B) == NULL, "A never got a path to B");
    CHECK(path(Wn, A) != NULL, "W still learned the path to A");
    CHECK(!send(A, S.n[B].addr, S.n[A].addr, 6001), "A cannot send to B through a leaf");
    CHECK(send(A, BC, S.n[A].addr, 6002) && run() && delivered(Wn, 6002, NULL, NULL) == 1 && delivered(B, 6002, NULL, NULL) == 0,
          "A's broadcast reaches W and stops there");

    /* ================= 7. TTL dies at the second relay ==================== */
    printf("--- scenario 7: TTL ---\n");
    sim_reset(4, true); link(0, 1); link(1, 2); link(2, 3);
    {
        /* Originate a broadcast with ttl 2 by hand. */
        struct frame f; memset(&f, 0, sizeof(f));
        f.kind = F_DATA; f.from = 0; memcpy(f.ta, S.n[0].addr, 6); memcpy(f.ra, BC, 6); f.payload = 7001;
        f.d.group = true; memcpy(f.d.addr1, BC, 6); memcpy(f.d.addr2, S.n[0].addr, 6); memcpy(f.d.addr3, S.n[0].addr, 6);
        f.d.mc.ttl = 2; f.d.mc.seq = 1;
        push(&f);
    }
    CHECK(run(), "drains");
    CHECK(delivered(1, 7001, NULL, NULL) == 1, "W1 (ttl 2) delivered");
    CHECK(delivered(2, 7001, NULL, NULL) == 1, "W2 (ttl 1) delivered");
    CHECK(delivered(3, 7001, NULL, NULL) == 0, "B never saw it: W2 did not forward at ttl 1");
    CHECK(S.n[2].ttl_drop == 1, "W2 counted the TTL stop");

    /* ================= 8. Storm detector actually detects ================= */
    printf("--- scenario 8: the cap catches a storm (self-check) ---\n");
    sim_reset(3, true); link(0, 1); link(1, 2); link(0, 2);
    {
        /* A group frame from a ghost source (so the OWN check cannot save
         * anyone), and every receive is given a fresh seq so the cache never
         * matches. Without duplicate suppression a triangle must storm. */
        struct frame f; memset(&f, 0, sizeof(f));
        f.kind = F_DATA; f.from = 0; memcpy(f.ta, S.n[0].addr, 6); memcpy(f.ra, BC, 6); f.payload = 8001;
        f.d.group = true; memcpy(f.d.addr1, BC, 6); memcpy(f.d.addr2, S.n[0].addr, 6);
        const uint8_t ghost[6] = { 0x02, 0xde, 0xad, 0, 0, 1 }; memcpy(f.d.addr3, ghost, 6);
        f.d.mc.ttl = 200; f.d.mc.seq = 1;
        push(&f);
        uint32_t steps = 0; bool stormed = false;
        while (S.qh < S.qt) {
            struct frame g = S.q[S.qh++ % QMAX];
            if (++steps > RUN_CAP || S.overflow) { stormed = true; break; }
            for (int j = 0; j < S.nn; j++) {
                if (j == g.from || !S.adj[g.from][j]) continue;
                g.d.mc.seq = steps * 10 + (uint32_t)j; /* defeat the cache */
                receive(j, &g);
            }
        }
        CHECK(stormed, "with the cache defeated the triangle storms and the detector trips (steps %u, overflow %d)", steps, (int)S.overflow);
        CHECK(S.n[1].fwd_data > 50 || S.n[2].fwd_data > 50, "and the relays really were re-forwarding (W %u, B %u)", S.n[1].fwd_data, S.n[2].fwd_data);
    }

    printf("\n%s\n", failures ? "SIMULATION FAILED" : "SIMULATION PASSED");
    return failures ? 1 : 0;
}
