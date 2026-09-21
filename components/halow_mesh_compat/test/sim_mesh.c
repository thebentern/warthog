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
    uint32_t tx, fwd_data, fwd_preq, fwd_prep, prep_sent, perr_sent, preq_sent, dup, ttl_drop, nodec;
};

enum fkind { F_DATA, F_ACTION };
struct frame {
    enum fkind kind;
    int from;                 /* node index of the transmitter */
    uint8_t ta[6];
    uint8_t ra[6];
    /* data: the on-air header bytes -- MAC, QoS Control, Mesh Control -- built
     * by the same umac_mesh_fwd_tx_header()/umac_mesh_ctrl_build() the
     * firmware uses, and parsed back with umac_mesh_fwd_parse_frame(). */
    uint8_t bytes[64];
    uint16_t len;
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
    bool grp_std;             /* AT+MESHGRP=1: standard 3-address group frames */
    /* Range is not peering: an attacker can be in range without a link. The
     * firmware takes data and path selection only from ESTAB peers. */
    bool estab[NMAX][NMAX];
    /* Key domain. Under SAE each node has its own group key and the chip
     * latches ONE peer's (the measured MM6108 limit); host CCMP lifts it. A
     * standard group frame is keyed by its TRANSMITTER; a per-peer replica
     * is pairwise and always decrypts. */
    bool secure;
    bool host_ccmp[NMAX];
    int  slot[NMAX];          /* peer index whose group key the chip holds, -1 none */
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
    return j >= 0 && S.estab[me][j];
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
        S.slot[i] = -1;
    }
}

static void link(int a, int b) { S.adj[a][b] = S.adj[b][a] = true; S.estab[a][b] = S.estab[b][a] = true; }
static void unlink_(int a, int b) { S.adj[a][b] = S.adj[b][a] = false; S.estab[a][b] = S.estab[b][a] = false; }
static void range_only(int a, int b) { S.adj[a][b] = S.adj[b][a] = true; S.estab[a][b] = S.estab[b][a] = false; }
static void latch(int node, int peer) { S.slot[node] = peer; }

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

/* Put one data frame on the air exactly as the firmware would shape it. */
static void emit_data(int from, const uint8_t *ra, const uint8_t *dst8023, const uint8_t *src8023,
                      bool sidecar, const uint8_t *mesh_da, const uint8_t *mesh_sa,
                      const struct umac_mesh_ctrl *mc, uint32_t payload)
{
    struct frame f; memset(&f, 0, sizeof(f));
    f.kind = F_DATA; f.from = from; memcpy(f.ta, S.n[from].addr, 6); memcpy(f.ra, ra, 6);
    f.payload = payload;
    struct umac_mesh_tx_hdr_in in = { .ra = ra, .own = S.n[from].addr, .dst8023 = dst8023,
                                      .src8023 = src8023, .sidecar_valid = sidecar,
                                      .mesh_da = mesh_da, .mesh_sa = mesh_sa, .grp_std = S.grp_std };
    uint16_t n = umac_mesh_fwd_tx_header(&in, f.bytes);
    if (n == 0) { printf("FAIL emit: header refused\n"); failures++; return; }
    f.bytes[n] = 0x00; f.bytes[n + 1] = 0x01; n += 2; /* QoS: tid 0, Mesh Control Present */
    uint16_t m = umac_mesh_ctrl_build(&f.bytes[n], (uint16_t)(sizeof(f.bytes) - n), mc);
    if (m == 0) { printf("FAIL emit: mesh control refused\n"); failures++; return; }
    f.len = (uint16_t)(n + m);
    push(&f);
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
    /* The glue takes path selection only from ESTAB peers; the datapath has no
     * stad for anyone else, so their data never reaches the engine either. */
    if (!S.estab[f->from][me]) return;
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
    /* Bytes -> frame, as the firmware's receive path does it. */
    struct umac_mesh_rx_frame pf;
    if (umac_mesh_fwd_parse_frame(f->bytes, f->len, &pf) == 0) { printf("FAIL parse at node %d\n", me); failures++; return; }
    if (!pf.group && !eq(pf.addr1, n->addr)) return; /* the chip does not deliver unicast for others */
    /* Key domain: a 3-address group frame is keyed by its transmitter. */
    if (pf.group && S.secure && !S.host_ccmp[me] && S.slot[me] != f->from) { n->nodec++; return; }
    (void)umac_mesh_fwd_normalise_replica(&pf);
    struct umac_mesh_fwd_ctx c = fctx(me);
    struct umac_mesh_fwd_rx_result r;
    umac_mesh_fwd_rx(&c, &pf, &r);
    if (r.drop == UMAC_MESH_FWD_DROP_DUP) n->dup++;
    if (r.drop == UMAC_MESH_FWD_DROP_TTL) n->ttl_drop++;
    if (r.verdict == UMAC_MESH_FWD_DELIVER || r.verdict == UMAC_MESH_FWD_DELIVER_AND_FORWARD) {
        if (n->nlog < LOGMAX) {
            memcpy(n->log[n->nlog].da, r.deliver_da, 6); memcpy(n->log[n->nlog].sa, r.deliver_sa, 6);
            n->log[n->nlog].seq = f->payload; n->log[n->nlog].group = pf.group; n->nlog++;
        }
    }
    if (r.verdict == UMAC_MESH_FWD_FORWARD || r.verdict == UMAC_MESH_FWD_DELIVER_AND_FORWARD) {
        bool group = (r.fwd_ra[0] & 0x01) != 0;
        if (!group) {
            emit_data(me, r.fwd_ra, r.mesh_da, r.mesh_sa, true, r.mesh_da, r.mesh_sa, &r.fwd_mc, f->payload);
            n->fwd_data++;
        } else if (S.grp_std) {
            /* One standard broadcast with the native Mesh Control. */
            emit_data(me, r.fwd_ra, r.mesh_da, r.mesh_sa, true, r.mesh_da, r.mesh_sa, &r.fwd_mc, f->payload);
            n->fwd_data++;
        } else {
            /* One unicast replica per neighbour except the sender, AE 2 carrying
             * the group and the real source -- exactly glue_forward(). */
            const uint8_t *src = umac_mesh_ctrl_ae(&r.fwd_mc) == UMAC_MESH_CTRL_AE_A4 ? r.fwd_mc.eaddr1 : r.mesh_sa;
            struct umac_mesh_ctrl rep; umac_mesh_fwd_replica_ctrl(&r.fwd_mc, r.mesh_da, src, &rep);
            int sent = 0;
            for (int j = 0; j < S.nn; j++) {
                if (j == me || !S.adj[me][j] || eq(S.n[j].addr, f->ta)) continue;
                emit_data(me, S.n[j].addr, r.mesh_da, r.mesh_sa, true, r.mesh_da, r.mesh_sa, &rep, f->payload);
                sent++;
            }
            if (sent) n->fwd_data++;
        }
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
    if (t.shape == UMAC_MESH_TX_GROUP_3ADDR) {
        if (S.grp_std) {
            emit_data(i, da, da, sa, true, da, S.n[i].addr, &t.mc, payload);
        } else {
            /* tx_classify: the replica marker; the builder: one copy per peer. */
            struct umac_mesh_ctrl rep; umac_mesh_fwd_replica_ctrl(&t.mc, da, sa, &rep);
            for (int j = 0; j < S.nn; j++) {
                if (j == i || !S.adj[i][j]) continue;
                emit_data(i, S.n[j].addr, da, sa, true, da, S.n[i].addr, &rep, payload);
            }
        }
        return true;
    }
    bool ae = umac_mesh_ctrl_ae(&t.mc) != UMAC_MESH_CTRL_AE_NONE;
    emit_data(i, t.ra, da, sa, ae, t.addr3, S.n[i].addr, &t.mc, payload);
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
    CHECK(S.n[3].fwd_data == 0, "B (the end) has no neighbour but the sender, so it emits nothing -- as the firmware would");
    /* With per-peer replicas the sender is excluded, so on a line nothing echoes
     * back and the cache is never even consulted; the triangle and the standard
     * broadcasts are where it earns its keep. */
    CHECK(S.n[1].dup == 0, "no echo reaches W1 on a line: sender exclusion did its job (dup %u)", S.n[1].dup);

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
        /* Originate a broadcast with ttl 2: one replica per neighbour, AE 2. */
        struct umac_mesh_ctrl nat = { .flags = 0, .ttl = 2, .seq = 1 }, rep;
        umac_mesh_fwd_replica_ctrl(&nat, BC, S.n[0].addr, &rep);
        emit_data(0, S.n[1].addr, BC, S.n[0].addr, true, BC, S.n[0].addr, &rep, 7001);
    }
    CHECK(run(), "drains");
    CHECK(delivered(1, 7001, NULL, NULL) == 1, "W1 (ttl 2) delivered");
    CHECK(delivered(2, 7001, NULL, NULL) == 1, "W2 (ttl 1) delivered");
    CHECK(delivered(3, 7001, NULL, NULL) == 0, "B never saw it: W2 did not forward at ttl 1");
    CHECK(S.n[2].ttl_drop == 1, "W2 counted the TTL stop");

    /* ================= 8. Storm detector actually detects ================= */
    printf("--- scenario 8: the cap catches a storm (self-check) ---\n");
    /* On a TRIANGLE, sender exclusion turns a defeated-cache flood into a
     * single chain that TTL bounds -- the cache is not the only safety. So the
     * self-check needs a topology exclusion cannot linearise: a 4-node full
     * mesh, where every relay still fans out to two. */
    sim_reset(4, true); for (int a = 0; a < 4; a++) for (int b = a + 1; b < 4; b++) link(a, b);
    {
        /* A group frame from a ghost source (so the OWN check cannot save
         * anyone), and every receive is given a fresh seq so the cache never
         * matches. Without duplicate suppression a triangle must storm. */
        const uint8_t ghost[6] = { 0x02, 0xde, 0xad, 0, 0, 1 };
        struct umac_mesh_ctrl nat = { .flags = 0, .ttl = 200, .seq = 1 }, rep;
        umac_mesh_fwd_replica_ctrl(&nat, BC, ghost, &rep);
        for (int j = 1; j < 4; j++) emit_data(0, S.n[j].addr, BC, ghost, true, BC, ghost, &rep, 8001);
        uint32_t steps = 0; bool stormed = false;
        while (S.qh < S.qt) {
            struct frame g = S.q[S.qh++ % QMAX];
            if (++steps > RUN_CAP || S.overflow) { stormed = true; break; }
            /* defeat the cache: rewrite the Mesh Control seq in the bytes (LE32 at MAC+2+2) */
            uint16_t fc = (uint16_t)(g.bytes[0] | (g.bytes[1] << 8));
            uint16_t off = (uint16_t)((((fc & 0x0300u) == 0x0300u) ? 30 : 24) + 2 + 2);
            uint32_t sq = steps * 10;
            g.bytes[off] = (uint8_t)sq; g.bytes[off+1] = (uint8_t)(sq >> 8); g.bytes[off+2] = (uint8_t)(sq >> 16); g.bytes[off+3] = (uint8_t)(sq >> 24);
            for (int j = 0; j < S.nn; j++) {
                if (j == g.from || !S.adj[g.from][j]) continue;
                receive(j, &g);
            }
        }
        CHECK(stormed, "with the cache defeated the full mesh storms and the detector trips (steps %u, overflow %d)", steps, (int)S.overflow);
        CHECK(S.n[1].fwd_data > 50 || S.n[2].fwd_data > 50, "and the relays really were re-forwarding (%u, %u)", S.n[1].fwd_data, S.n[2].fwd_data);
    }
    {
        /* And the positive: on a triangle the same sabotage is bounded by
         * exclusion and TTL alone -- it drains without the cache. */
        sim_reset(3, true); link(0, 1); link(1, 2); link(0, 2);
        const uint8_t ghost[6] = { 0x02, 0xde, 0xad, 0, 0, 2 };
        struct umac_mesh_ctrl nat = { .flags = 0, .ttl = 200, .seq = 1 }, rep;
        umac_mesh_fwd_replica_ctrl(&nat, BC, ghost, &rep);
        for (int j = 1; j < 3; j++) emit_data(0, S.n[j].addr, BC, ghost, true, BC, ghost, &rep, 8002);
        uint32_t steps = 0; bool stormed = false;
        while (S.qh < S.qt) {
            struct frame g = S.q[S.qh++ % QMAX];
            if (++steps > RUN_CAP || S.overflow) { stormed = true; break; }
            uint16_t fc = (uint16_t)(g.bytes[0] | (g.bytes[1] << 8));
            uint16_t off = (uint16_t)((((fc & 0x0300u) == 0x0300u) ? 30 : 24) + 2 + 2);
            uint32_t sq = steps * 10;
            g.bytes[off] = (uint8_t)sq; g.bytes[off+1] = (uint8_t)(sq >> 8); g.bytes[off+2] = (uint8_t)(sq >> 16); g.bytes[off+3] = (uint8_t)(sq >> 24);
            for (int j = 0; j < S.nn; j++) { if (j == g.from || !S.adj[g.from][j]) continue; receive(j, &g); }
        }
        CHECK(!stormed && steps < 600, "triangle with the cache defeated still drains: sender exclusion + TTL bound it (%u steps)", steps);
    }

    /* ================= 9. grp_std: standard 3-address broadcasts ========= */
    printf("--- scenario 9: group flood as standard 3-address frames (AT+MESHGRP=1) ---\n");
    sim_reset(4, true); link(0, 1); link(1, 2); link(2, 3); S.grp_std = true;
    CHECK(send(0, BC, S.n[0].addr, 9001), "A broadcasts one 3-address frame");
    CHECK(run(), "drains, %u steps", S.steps);
    for (int i = 1; i < 4; i++)
        CHECK(delivered(i, 9001, BC, S.n[0].addr) == 1, "node %d got it exactly once", i);
    CHECK(S.n[1].fwd_data == 1 && S.n[2].fwd_data == 1 && S.n[3].fwd_data == 1,
          "every node rebroadcasts once -- a standard frame goes to everyone, the sender drops its echo");
    CHECK(S.n[1].dup >= 1 && S.n[2].dup >= 1, "the echoes were caught by the caches");
    CHECK(delivered(0, 9001, NULL, NULL) == 0, "A never delivered its own broadcast");
    /* NOTE: keys are not modelled here; under SAE a standard group frame decrypts
     * only where the receiver holds the sender's group key -- see scenario 10. */

    /* ================= 10. Key domain: what SAE does to group frames ====== */
    printf("--- scenario 10: standard group frames under SAE with one chip group-key slot ---\n");
    /* Line A - W - B, chip crypto, W latched A and B latched W: A's flood works by luck of latch order. */
    sim_reset(3, true); link(0, 1); link(1, 2); S.grp_std = true; S.secure = true; latch(1, 0); latch(2, 1);
    CHECK(send(0, BC, S.n[0].addr, 10001) && run(), "A floods a line");
    CHECK(delivered(1, 10001, NULL, NULL) == 1 && delivered(2, 10001, NULL, NULL) == 1, "reaches W and B: each chip happened to hold the transmitter's key");
    /* Star: W peers A and B, W latched A. B's flood cannot be decrypted at W. */
    sim_reset(3, true); link(0, 1); link(1, 2); S.grp_std = true; S.secure = true; latch(1, 0); latch(0, 1); latch(2, 1);
    CHECK(send(2, BC, S.n[2].addr, 10002) && run(), "B floods");
    CHECK(S.n[1].nodec == 1 && delivered(1, 10002, NULL, NULL) == 0, "W cannot decrypt B's group frame: its one slot holds A's key -- the measured failure");
    CHECK(delivered(0, 10002, NULL, NULL) == 0, "so A never gets it");
    /* Host CCMP on W lifts the limit. */
    sim_reset(3, true); link(0, 1); link(1, 2); S.grp_std = true; S.secure = true; latch(1, 0); latch(0, 1); latch(2, 1); S.host_ccmp[1] = true;
    CHECK(send(2, BC, S.n[2].addr, 10003) && run(), "B floods again");
    CHECK(S.n[1].nodec == 0 && delivered(1, 10003, NULL, NULL) == 1 && delivered(0, 10003, NULL, NULL) == 1, "with host CCMP on W it decrypts and relays to A");
    /* Replicate mode never hits it: replicas are pairwise. */
    sim_reset(3, true); link(0, 1); link(1, 2); S.grp_std = false; S.secure = true; latch(1, 0); latch(0, 1); latch(2, 1);
    CHECK(send(2, BC, S.n[2].addr, 10004) && run(), "B floods in replicate mode");
    CHECK(S.n[1].nodec == 0 && delivered(1, 10004, NULL, NULL) == 1 && delivered(0, 10004, NULL, NULL) == 1, "per-peer replicas are pairwise: decrypt everywhere regardless of the slot");

    /* ================= 11. Forged PREQ from a node in range but not peered = */
    printf("--- scenario 11: forged high-SN PREQ from a non-peer ---\n");
    sim_reset(4, true); link(0, 1); link(1, 2); range_only(3, 1); /* X (3) hears W, no link */
    discover(A, S.n[B].addr); run();
    const struct umac_mesh_path *pa = path(Wn, A);
    CHECK(pa != NULL && eq(pa->next_hop, S.n[A].addr), "W has a real path to A via A");
    uint32_t sn_before = pa->sn;
    {
        /* X claims to be A with a huge sequence number, asking for B. */
        uint8_t forged[HWMP_PREQ_BODY_LEN];
        uint16_t nb = umac_mesh_hwmp_build_preq(forged, sizeof(forged), S.n[A].addr, 0x7fffffffu, 77, S.n[B].addr, 4882);
        queue_action(3, BC, forged, nb);
        run();
    }
    pa = path(Wn, A);
    CHECK(pa != NULL && eq(pa->next_hop, S.n[A].addr) && pa->sn == sn_before, "W's path to A is untouched: the forgery came from a non-peer and was never processed");
    CHECK(S.n[Wn].fwd_preq == 1, "W rebroadcast only the genuine PREQ (got %u)", S.n[Wn].fwd_preq);

    /* ================= 12. Bystander PERR ================================= */
    printf("--- scenario 12: PERR from a peer that is not our next hop ---\n");
    sim_reset(3, true); link(0, 1); link(1, 2); link(0, 2); /* triangle */
    discover(A, S.n[B].addr); run();
    const struct umac_mesh_path *pb = path(A, B);
    CHECK(pb != NULL && eq(pb->next_hop, S.n[B].addr), "A reaches B directly (metric wins over the relay)");
    {
        uint8_t perr[HWMP_PERR_BODY_LEN];
        uint16_t nb = umac_mesh_hwmp_build_perr(perr, sizeof(perr), 31, S.n[B].addr, pb->sn + 5, HWMP_REASON_MESH_PATH_ERROR_DEST_UNREACHABLE);
        queue_action(Wn, BC, perr, nb); /* W is a peer of A, but not A's next hop for B */
        run();
    }
    pb = path(A, B);
    CHECK(pb != NULL && eq(pb->next_hop, S.n[B].addr), "A's path to B survives a PERR from W, which is not on it");
    CHECK(S.n[A].perr_sent == 0, "and A did not forward the bystander's PERR");

    /* ================= 13. Two relays: PERR propagates two hops, rediscovery recovers = */
    printf("--- scenario 13: A - W1 - W2 - B, link loss two hops away ---\n");
    sim_reset(4, true); link(0, 1); link(1, 2); link(2, 3);
    discover(0, S.n[3].addr); run();
    const struct umac_mesh_path *p0 = path(0, 3);
    CHECK(p0 != NULL && eq(p0->next_hop, S.n[1].addr) && p0->hop_count == 3, "A reaches B via W1 in 3 hops");
    CHECK(path(1, 3) != NULL && eq(path(1, 3)->next_hop, S.n[2].addr), "W1 reaches B via W2");
    uint32_t old_sn = p0->sn;
    unlink_(2, 3);
    {
        struct umac_mesh_hwmp_ctx c = hctx(2); struct umac_mesh_hwmp_action acts[4];
        uint32_t k = umac_mesh_hwmp_lose_neighbour(&c, S.n[3].addr, acts, 4);
        CHECK(k == 1, "W2 announces B");
        for (uint32_t i = 0; i < k; i++) { S.n[2].perr_sent++; queue_action(2, acts[i].to, acts[i].body, acts[i].body_len); }
        run();
    }
    CHECK(path(2, 3) == NULL && path(1, 3) == NULL && path(0, 3) == NULL, "the PERR reached two hops: W2, W1 and A all dropped their paths to B");
    CHECK(S.n[1].perr_sent == 1 && S.n[0].perr_sent == 1, "W1 and A each forwarded it once");
    CHECK(!send(0, S.n[3].addr, S.n[0].addr, 13001), "A has no route to B");
    link(2, 3);
    discover(0, S.n[3].addr); run();
    p0 = path(0, 3);
    CHECK(p0 != NULL && eq(p0->next_hop, S.n[1].addr), "rediscovery rebuilt the path");
    CHECK(hwmp_sn_gt(p0->sn, old_sn), "at a newer sequence number (%u > %u)", (unsigned)p0->sn, (unsigned)old_sn);
    CHECK(send(0, S.n[3].addr, S.n[0].addr, 13002) && run() && delivered(3, 13002, NULL, NULL) == 1, "and traffic flows again, exactly once");

    /* ================= 14. Ring under a burst ============================== */
    printf("--- scenario 14: ring A - W - B - C - A, 30 broadcasts ---\n");
    sim_reset(4, true); link(0, 1); link(1, 2); link(2, 3); link(3, 0);
    for (uint32_t k = 0; k < 30; k++) CHECK(send(0, BC, S.n[0].addr, 14000 + k) || true, "(send %u)", (unsigned)k);
    CHECK(run(), "30 floods on a ring drain (%u steps)", S.steps);
    {
        int bad = 0;
        for (int i = 1; i < 4; i++) for (uint32_t k = 0; k < 30; k++) if (delivered(i, 14000 + k, NULL, NULL) != 1) bad++;
        CHECK(bad == 0, "every node delivered every one of the 30 exactly once (%d misses)", bad);
        CHECK(delivered(0, 14000, NULL, NULL) == 0 && delivered(0, 14029, NULL, NULL) == 0, "A never delivered its own");
    }

    /* ================= 15. A poisoned loop is bounded by TTL =============== */
    printf("--- scenario 15: two relays pointing at each other ---\n");
    sim_reset(3, true); link(0, 1); link(1, 2);
    const uint8_t Cx[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0xcc }; /* a destination that does not exist */
    umac_mesh_path_update(&S.n[0].tbl, Cx, S.n[1].addr, 1, 100, 1, 60000, S.now); /* A: C via W */
    umac_mesh_path_update(&S.n[1].tbl, Cx, S.n[2].addr, 1, 100, 1, 60000, S.now); /* W: C via B */
    umac_mesh_path_update(&S.n[2].tbl, Cx, S.n[1].addr, 1, 100, 1, 60000, S.now); /* B: C via W -- the loop */
    CHECK(send(0, Cx, S.n[0].addr, 15001), "A sends to C");
    CHECK(run(), "the loop drains (%u steps)", S.steps);
    CHECK(S.steps <= 40, "bounded by TTL 31: %u steps, not a storm", S.steps);
    CHECK(S.n[1].fwd_data + S.n[2].fwd_data >= 28 && S.n[1].fwd_data + S.n[2].fwd_data <= 31, "W and B ping-ponged it until TTL ran out (%u + %u)", S.n[1].fwd_data, S.n[2].fwd_data);
    CHECK(delivered(1, 15001, NULL, NULL) == 0 && delivered(2, 15001, NULL, NULL) == 0, "nobody delivered a frame for C");
    CHECK(S.n[1].ttl_drop + S.n[2].ttl_drop == 1, "exactly one relay stopped it on TTL");

    printf("\n%s\n", failures ? "SIMULATION FAILED" : "SIMULATION PASSED");
    return failures ? 1 : 0;
}
