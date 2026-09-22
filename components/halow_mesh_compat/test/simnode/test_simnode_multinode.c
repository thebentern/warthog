/*
 * Several real stacks on a virtual air.
 *
 * Every node in this file is a COMPLETE simnode: umac_mesh.c, the forwarding
 * glue, the forwarding engine, both datapath files, the path/RMC tables and
 * the frame builders, cut at mmdrv_tx_frame() exactly as the smoke test cuts
 * it. What is new here is that there are three of them, and that the only
 * thing joining them is bytes: a frame one node handed to its chip is fed to
 * another node's receive path verbatim. No test code ever builds a PREQ, a
 * PREP, a PERR or a mesh header -- every frame asserted on was built by one
 * shipping stack and parsed by another.
 *
 * HOW THREE STACKS COEXIST. The SDK keeps its state in file-scope statics --
 * one path table, one HWMP sequence number, one peer list, one clock -- so a
 * second simnode_start() in one image would give node B node A's tables. The
 * simulator is therefore built once as a shared object and loaded three times
 * from three DISTINCT FILES (libsimnode0/1/2). The dynamic loader gives each
 * file its own .data and .bss, so each node gets private statics, while libc
 * stays shared and the whole mesh runs in one process against one virtual
 * clock. RTLD_LOCAL plus macOS's two-level namespace (-Bsymbolic on ELF)
 * keeps each node's internal calls bound to its own copy.
 *
 * This was chosen over the two alternatives because it is the cheapest thing
 * that genuinely isolates the statics: fork-per-node needs a marshalling
 * protocol and loses single-process debugging and backtraces, and
 * objcopy --prefix-symbols needs an allowlist so that libc references are not
 * prefixed too. Both would have bought the same isolation the loader already
 * gives for free.
 *
 * WHAT THIS STILL CANNOT TELL YOU. The air here is lossless, instantaneous
 * and perfectly ordered, and a frame is heard by every node in range in the
 * order it was sent. Nothing about timing, collisions, retries, reordering or
 * what a real mac80211 peer does with these bytes is modelled. A green run
 * means three shipping stacks agree with each other about the protocol, not
 * that the protocol survives a radio.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "simnode.h"
#include "umac_mesh_fwd.h"
#include "umac_mesh_hwmp.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* ---- one node = one loaded copy of the whole simulator ----------------- */

#define NODES 3
enum { A = 0, W = 1, B = 2 };

struct node {
    void *lib;
    const char *name;
    uint8_t mac[6];
    bool (*start)(const uint8_t *);
    void (*stop)(void);
    void (*gates)(bool, bool, bool, bool);
    bool (*add_peer)(const uint8_t *);
    void (*del_peer)(const uint8_t *);
    bool (*host_tx)(const uint8_t *, const uint8_t *, const uint8_t *, uint16_t);
    bool (*rx)(const uint8_t *, uint16_t, int16_t);
    void (*tick)(void);
    void (*pump)(void);
    void (*advance)(uint32_t);
    uint32_t (*now)(void);
    unsigned (*outbox_count)(void);
    const struct simnode_frame *(*outbox_get)(unsigned);
    void (*outbox_clear)(void);
    unsigned (*host_rx_count)(void);
    const struct simnode_hostrx *(*host_rx_get)(unsigned);
    void (*host_rx_clear)(void);
    unsigned (*live_allocs)(void);
    int (*render_paths)(char *, uint32_t);
};

static struct node nd[NODES];

/* Parsers, borrowed from node A's copy. These are pure byte decoders with no
 * state of their own, so which node's copy runs them does not matter -- and
 * they are the SHIPPING decoders, which is the point: the test never learns
 * a frame layout of its own. */
static uint16_t (*fw_parse_frame)(const uint8_t *, uint16_t, struct umac_mesh_rx_frame *);
static bool (*hwmp_parse_preq)(const uint8_t *, uint16_t, struct hwmp_preq *);
static bool (*hwmp_parse_prep)(const uint8_t *, uint16_t, struct hwmp_prep *);
static bool (*hwmp_parse_perr)(const uint8_t *, uint16_t, struct hwmp_perr *);
static uint8_t (*hwmp_element_id)(const uint8_t *, uint16_t);

static void *need_sym(void *lib, const char *name)
{
    void *p = dlsym(lib, name);
    if (p == NULL) { printf("FAIL dlsym(%s): %s\n", name, dlerror()); exit(1); }
    return p;
}

#define BIND(i, field, name) (*(void **)(&nd[i].field) = need_sym(nd[i].lib, name))

/** The directory the test binary itself lives in, so the per-node copies are
 *  found however the binary was invoked, not only from the test directory. */
static const char *s_exedir = ".";

static void open_nodes(void)
{
    static const char *names[NODES] = { "A", "W", "B" };
    static const uint8_t macs[NODES][6] = {
        { 0x02, 0, 0, 0, 0, 0x0a }, { 0x02, 0, 0, 0, 0, 0x0b }, { 0x02, 0, 0, 0, 0, 0x0c },
    };
    for (int i = 0; i < NODES; i++)
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/libsimnode%d" SIMNODE_SO_SUFFIX, s_exedir, i);
        nd[i].lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (nd[i].lib == NULL) { printf("FAIL dlopen(%s): %s\n", path, dlerror()); exit(1); }
        nd[i].name = names[i];
        memcpy(nd[i].mac, macs[i], 6);
        BIND(i, start, "simnode_start");
        BIND(i, stop, "simnode_stop");
        BIND(i, gates, "simnode_set_gates");
        BIND(i, add_peer, "simnode_add_peer");
        BIND(i, del_peer, "simnode_del_peer");
        BIND(i, host_tx, "simnode_host_tx");
        BIND(i, rx, "simnode_rx");
        BIND(i, tick, "simnode_tick");
        BIND(i, pump, "simnode_pump");
        BIND(i, advance, "simnode_advance_ms");
        BIND(i, now, "mmosal_get_time_ms");
        BIND(i, outbox_count, "simnode_outbox_count");
        BIND(i, outbox_get, "simnode_outbox_get");
        BIND(i, outbox_clear, "simnode_outbox_clear");
        BIND(i, host_rx_count, "simnode_host_rx_count");
        BIND(i, host_rx_get, "simnode_host_rx_get");
        BIND(i, host_rx_clear, "simnode_host_rx_clear");
        BIND(i, live_allocs, "simnode_live_allocs");
        BIND(i, render_paths, "simnode_render_paths");
    }
    *(void **)(&fw_parse_frame) = need_sym(nd[A].lib, "umac_mesh_fwd_parse_frame");
    *(void **)(&hwmp_parse_preq) = need_sym(nd[A].lib, "umac_mesh_hwmp_parse_preq");
    *(void **)(&hwmp_parse_prep) = need_sym(nd[A].lib, "umac_mesh_hwmp_parse_prep");
    *(void **)(&hwmp_parse_perr) = need_sym(nd[A].lib, "umac_mesh_hwmp_parse_perr");
    *(void **)(&hwmp_element_id) = need_sym(nd[A].lib, "umac_mesh_hwmp_element_id");
}

/* ---- the air ----------------------------------------------------------
 *
 * Lossless and instantaneous: a frame is heard by every node the topology
 * puts in range, whatever its RA. Delivering to everyone in range rather than
 * to the addressee is the faithful choice -- a radio has no address filter --
 * and it means the shipping code's own addr1 check is under test too. */

static bool in_range[NODES][NODES];

struct airframe {
    int src;
    struct simnode_frame f;
};
#define AIRLOG_MAX 128
static struct airframe airlog[AIRLOG_MAX];
static unsigned airlog_n;

static void air_reset(void)
{
    airlog_n = 0;
    for (int i = 0; i < NODES; i++) { nd[i].outbox_clear(); nd[i].host_rx_clear(); }
}

/** One hop: everything currently queued is transmitted and received. */
static unsigned air_round(void)
{
    struct airframe batch[AIRLOG_MAX];
    unsigned n = 0;
    for (int i = 0; i < NODES; i++)
    {
        unsigned c = nd[i].outbox_count();
        for (unsigned k = 0; k < c && n < AIRLOG_MAX; k++)
        {
            const struct simnode_frame *f = nd[i].outbox_get(k);
            if (f == NULL) { continue; }
            batch[n].src = i;
            batch[n].f = *f;
            n++;
        }
        nd[i].outbox_clear();
    }
    for (unsigned k = 0; k < n; k++)
    {
        if (airlog_n < AIRLOG_MAX) { airlog[airlog_n++] = batch[k]; }
        for (int j = 0; j < NODES; j++)
        {
            if (j != batch[k].src && in_range[batch[k].src][j])
            {
                nd[j].rx(batch[k].f.bytes, batch[k].f.len, -60);
            }
        }
    }
    return n;
}

/** Run the air until nothing more is queued anywhere. @returns rounds used,
 *  or 0 if it never settled -- a caller asserts on that, because a mesh that
 *  never goes quiet is a broadcast storm. */
static unsigned air_settle(void)
{
    for (unsigned r = 1; r <= 12u; r++)
    {
        if (air_round() == 0) { return r; }
    }
    return 0;
}

static void air_advance(uint32_t ms) { for (int i = 0; i < NODES; i++) { nd[i].advance(ms); } }
static void air_tick(void) { for (int i = 0; i < NODES; i++) { nd[i].tick(); } }

/* ---- reading the log --------------------------------------------------- */

static const uint8_t *fr_ra(const struct simnode_frame *f) { return f->bytes + 4; }
static const uint8_t *fr_ta(const struct simnode_frame *f) { return f->bytes + 10; }

/** The action body of a category-13 mesh path-selection frame, or NULL. */
static const uint8_t *hwmp_body(const struct simnode_frame *f, uint16_t *out_len)
{
    if (!f->is_mgmt || f->len < 27u) { return NULL; }
    if (f->bytes[24] != HWMP_CATEGORY_MESH || f->bytes[25] != HWMP_ACTION_PATH_SELECTION)
    {
        return NULL;
    }
    *out_len = (uint16_t)(f->len - 24u);
    return f->bytes + 24;
}

/** The @p nth frame in the log sent by @p src carrying HWMP element @p eid. */
static const struct simnode_frame *find_hwmp(int src, uint8_t eid, unsigned nth)
{
    unsigned seen = 0;
    for (unsigned i = 0; i < airlog_n; i++)
    {
        if (airlog[i].src != src) { continue; }
        uint16_t blen = 0;
        const uint8_t *body = hwmp_body(&airlog[i].f, &blen);
        if (body == NULL || hwmp_element_id(body, blen) != eid) { continue; }
        if (seen++ == nth) { return &airlog[i].f; }
    }
    return NULL;
}

static unsigned count_hwmp(int src, uint8_t eid)
{
    unsigned n = 0;
    while (find_hwmp(src, eid, n) != NULL) { n++; }
    return n;
}

/** The @p nth data frame in the log sent by @p src. */
static const struct simnode_frame *find_data(int src, unsigned nth)
{
    unsigned seen = 0;
    for (unsigned i = 0; i < airlog_n; i++)
    {
        if (airlog[i].src == src && !airlog[i].f.is_mgmt && seen++ == nth)
        {
            return &airlog[i].f;
        }
    }
    return NULL;
}

static unsigned count_data(int src)
{
    unsigned n = 0;
    while (find_data(src, n) != NULL) { n++; }
    return n;
}

/** The sn= field of a rendered path line, or 0 when it has none. Separate
 *  because a CHECK's message is evaluated on the failing branch too, so the
 *  condition cannot be the thing that walks the string. */
static uint32_t line_sn(const char *line)
{
    if (line == NULL) { return 0u; }
    const char *p = strstr(line, "sn=");
    return (p == NULL) ? 0u : (uint32_t)strtoul(p + 3, NULL, 10);
}

/** The path table line for @p dst in @p n's own +MESHPATH dump, or NULL. */
static const char *path_line(int n, const uint8_t *dst, char *buf, uint32_t len)
{
    char want[32];
    snprintf(want, sizeof(want), "dst=%02x%02x%02x", dst[3], dst[4], dst[5]);
    if (nd[n].render_paths(buf, len) <= 0) { return NULL; }
    return strstr(buf, want);
}

/* ---- the topology ------------------------------------------------------ */

static void topo_clear(void) { memset(in_range, 0, sizeof(in_range)); }

static void topo_link(int x, int y)
{
    in_range[x][y] = in_range[y][x] = true;
    nd[x].add_peer(nd[y].mac);
    nd[y].add_peer(nd[x].mac);
}

/**
 * Bring all three up fresh with the given gates.
 *
 * The peers have to be dropped by hand: umac_mesh_disable_mesh() returns
 * UNAVAILABLE on this firmware -- a mesh is never torn down in the field --
 * so simnode_stop() leaves the datapath's peer table standing. Left there, a
 * neighbour from the previous topology stays a valid next hop and the next
 * scenario silently never discovers anything.
 */
static void mesh_restart(bool fwd, bool bridge, bool grp_std, bool secure)
{
    for (int i = 0; i < NODES; i++)
    {
        for (int j = 0; j < NODES; j++)
        {
            if (i != j) { nd[i].del_peer(nd[j].mac); }
        }
    }
    for (int i = 0; i < NODES; i++)
    {
        nd[i].stop();
        nd[i].start(nd[i].mac);
        nd[i].gates(fwd, bridge, grp_std, secure);
    }
    topo_clear();
    air_reset();
}

/* ---- scenarios --------------------------------------------------------- */

static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static uint8_t payload[24];

/**
 * 1. HWMP discovery, end to end, between real stacks.
 *
 * A wants B, which is not its neighbour. Every frame below was built by one
 * shipping stack and parsed by another; the test supplies only the topology.
 */
static void scenario_discovery(void)
{
    printf("--- 1: A discovers B through W, with three real stacks ---\n");
    mesh_restart(/*fwd=*/true, false, false, false);
    topo_link(A, W);
    topo_link(W, B);
    air_reset();

    nd[A].host_tx(nd[B].mac, nd[A].mac, payload, sizeof(payload));
    unsigned rounds = air_settle();
    CHECK(rounds != 0, "the exchange settles (%u air rounds)", rounds);

    /* A asks. */
    CHECK(count_hwmp(A, HWMP_EID_PREQ) == 1,
          "A originates exactly one PREQ (%u)", count_hwmp(A, HWMP_EID_PREQ));
    const struct simnode_frame *preq = find_hwmp(A, HWMP_EID_PREQ, 0);
    struct hwmp_preq p0;
    memset(&p0, 0, sizeof(p0));
    bool p0_ok = false;
    if (preq != NULL)
    {
        uint16_t blen = 0;
        const uint8_t *body = hwmp_body(preq, &blen);
        p0_ok = (body != NULL) && hwmp_parse_preq(body, blen, &p0);
        CHECK(memcmp(fr_ra(preq), BCAST, 6) == 0, "and broadcasts it (RA %02x:%02x)",
              fr_ra(preq)[0], fr_ra(preq)[5]);
        CHECK(memcmp(fr_ta(preq), nd[A].mac, 6) == 0, "with A as transmitter");
    }
    CHECK(p0_ok, "the shipping PREQ parser accepts the shipping PREQ builder's bytes");
    CHECK(p0_ok && memcmp(p0.orig_addr, nd[A].mac, 6) == 0, "originator is A");
    CHECK(p0_ok && memcmp(p0.target_addr, nd[B].mac, 6) == 0, "target is B");
    CHECK(p0_ok && p0.ttl == HWMP_DEFAULT_TTL, "ttl is the default %u (got %u)",
          HWMP_DEFAULT_TTL, p0_ok ? p0.ttl : 0);
    CHECK(p0_ok && p0.hop_count == 0, "hop count starts at 0 (got %u)", p0_ok ? p0.hop_count : 0);

    /* W relays it. */
    CHECK(count_hwmp(W, HWMP_EID_PREQ) == 1,
          "W rebroadcasts it exactly once (%u)", count_hwmp(W, HWMP_EID_PREQ));
    const struct simnode_frame *relayed = find_hwmp(W, HWMP_EID_PREQ, 0);
    struct hwmp_preq p1;
    memset(&p1, 0, sizeof(p1));
    bool p1_ok = false;
    if (relayed != NULL)
    {
        uint16_t blen = 0;
        const uint8_t *body = hwmp_body(relayed, &blen);
        p1_ok = (body != NULL) && hwmp_parse_preq(body, blen, &p1);
    }
    CHECK(p1_ok, "W's relayed PREQ parses");
    CHECK(p1_ok && p0_ok && p1.preq_id == p0.preq_id && memcmp(p1.orig_addr, nd[A].mac, 6) == 0,
          "carrying A's PREQ id and originator, not W's");
    CHECK(p1_ok && p0_ok && p1.ttl == (uint8_t)(p0.ttl - 1u),
          "with ttl spent by exactly one hop (%u -> %u)", p0_ok ? p0.ttl : 0, p1_ok ? p1.ttl : 0);
    CHECK(p1_ok && p0_ok && p1.hop_count == (uint8_t)(p0.hop_count + 1u),
          "and hop count grown by one (%u -> %u)", p0_ok ? p0.hop_count : 0,
          p1_ok ? p1.hop_count : 0);
    CHECK(p1_ok && p0_ok && p1.metric > p0.metric,
          "and the path metric grown by the link (%u -> %u)",
          p0_ok ? p0.metric : 0, p1_ok ? p1.metric : 0);

    /* B answers. */
    CHECK(count_hwmp(B, HWMP_EID_PREP) == 1,
          "B answers with exactly one PREP (%u)", count_hwmp(B, HWMP_EID_PREP));
    const struct simnode_frame *prep = find_hwmp(B, HWMP_EID_PREP, 0);
    struct hwmp_prep q0;
    memset(&q0, 0, sizeof(q0));
    bool q0_ok = false;
    if (prep != NULL)
    {
        uint16_t blen = 0;
        const uint8_t *body = hwmp_body(prep, &blen);
        q0_ok = (body != NULL) && hwmp_parse_prep(body, blen, &q0);
        CHECK(memcmp(fr_ra(prep), nd[W].mac, 6) == 0,
              "sent back to W, the hop it came from, not broadcast");
    }
    CHECK(q0_ok && memcmp(q0.target_addr, nd[B].mac, 6) == 0, "the PREP answers for B");
    CHECK(q0_ok && memcmp(q0.orig_addr, nd[A].mac, 6) == 0, "and is addressed to A's request");

    /* W carries it back. */
    CHECK(count_hwmp(W, HWMP_EID_PREP) == 1,
          "W forwards the PREP exactly once (%u)", count_hwmp(W, HWMP_EID_PREP));
    const struct simnode_frame *fwd = find_hwmp(W, HWMP_EID_PREP, 0);
    struct hwmp_prep q1;
    memset(&q1, 0, sizeof(q1));
    bool q1_ok = false;
    if (fwd != NULL)
    {
        uint16_t blen = 0;
        const uint8_t *body = hwmp_body(fwd, &blen);
        q1_ok = (body != NULL) && hwmp_parse_prep(body, blen, &q1);
        CHECK(memcmp(fr_ra(fwd), nd[A].mac, 6) == 0, "unicast onward to A");
    }
    CHECK(q1_ok && q0_ok && q1.ttl == (uint8_t)(q0.ttl - 1u),
          "with ttl spent by one hop (%u -> %u)", q0_ok ? q0.ttl : 0, q1_ok ? q1.ttl : 0);
    CHECK(q1_ok && memcmp(q1.target_addr, nd[B].mac, 6) == 0,
          "still answering for B after the relay");

    /* And the tables each stack built from those bytes. */
    char buf[768];
    const char *line = path_line(A, nd[B].mac, buf, sizeof(buf));
    CHECK(line != NULL, "A now has a path to B");
    CHECK(line != NULL && strstr(line, "via=00000b") != NULL, "through W");
    CHECK(line != NULL && strstr(line, "hops=2") != NULL, "two hops away");
    CHECK(line != NULL && strstr(line, "active") != NULL, "and usable");

    char buf2[768];
    const char *wline = path_line(W, nd[B].mac, buf2, sizeof(buf2));
    CHECK(wline != NULL && strstr(wline, "hops=1") != NULL,
          "W's own path to B is one hop, so the relay knows where to send");
}

/**
 * 2. A unicast relayed A -> W -> B.
 *
 * The frame A puts on air is consumed by W's real receive path, and what W
 * emits is asserted against what A emitted: the relay must carry the mesh
 * endpoints forward and spend exactly one hop of TTL.
 */
static void scenario_relay(void)
{
    printf("--- 2: a unicast relayed A -> W -> B ---\n");
    /* The path exists from scenario 1; only the log and the host counters
     * are cleared, so what follows is the data plane alone. */
    air_reset();

    unsigned allocs_before[NODES];
    for (int i = 0; i < NODES; i++) { allocs_before[i] = nd[i].live_allocs(); }

    static const uint8_t body[16] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
    CHECK(nd[A].host_tx(nd[B].mac, nd[A].mac, body, sizeof(body)),
          "A's host sends 16 bytes to B");
    unsigned rounds = air_settle();
    CHECK(rounds != 0, "it settles (%u air rounds)", rounds);

    CHECK(count_data(A) == 1, "A emits exactly one data frame (%u)", count_data(A));
    CHECK(count_data(B) == 0, "B, the destination, relays nothing (%u)", count_data(B));
    CHECK(count_data(W) == 1, "W relays it exactly once (%u)", count_data(W));

    const struct simnode_frame *first = find_data(A, 0);
    const struct simnode_frame *hop2 = find_data(W, 0);
    struct umac_mesh_rx_frame f0, f1;
    memset(&f0, 0, sizeof(f0));
    memset(&f1, 0, sizeof(f1));
    bool f0_ok = first != NULL && fw_parse_frame(first->bytes, first->len, &f0) != 0;
    bool f1_ok = hop2 != NULL && fw_parse_frame(hop2->bytes, hop2->len, &f1) != 0;
    CHECK(f0_ok, "the shipping parser reads A's frame");
    CHECK(f1_ok, "and W's");

    CHECK(f0_ok && memcmp(f0.addr1, nd[W].mac, 6) == 0, "A addressed it to W, its next hop");
    CHECK(f0_ok && !f0.group && memcmp(f0.addr3, nd[B].mac, 6) == 0 &&
          memcmp(f0.addr4, nd[A].mac, 6) == 0,
          "four-address, mesh DA=B and mesh SA=A");

    CHECK(f1_ok && memcmp(f1.addr1, nd[B].mac, 6) == 0, "W re-addressed it to B");
    CHECK(f1_ok && memcmp(f1.addr2, nd[W].mac, 6) == 0, "with itself as transmitter");
    CHECK(f1_ok && f0_ok && memcmp(f1.addr3, f0.addr3, 6) == 0 &&
          memcmp(f1.addr4, f0.addr4, 6) == 0,
          "and carried the mesh endpoints through unchanged -- a relay must not "
          "rewrite them to itself");
    CHECK(f1_ok && f0_ok && f1.mc.ttl == (uint8_t)(f0.mc.ttl - 1u),
          "one hop of mesh TTL spent (%u -> %u)", f0_ok ? f0.mc.ttl : 0, f1_ok ? f1.mc.ttl : 0);
    CHECK(f1_ok && f0_ok && f1.mc.seq == f0.mc.seq,
          "the originator's mesh sequence number preserved (%u vs %u) -- a relay that "
          "renumbers breaks every downstream duplicate cache",
          f0_ok ? f0.mc.seq : 0, f1_ok ? f1.mc.seq : 0);

    /* What the far host actually got. */
    CHECK(nd[B].host_rx_count() == 1, "B's host receives it exactly once (%u)",
          nd[B].host_rx_count());
    const struct simnode_hostrx *got = nd[B].host_rx_get(0);
    CHECK(got != NULL && memcmp(got->da, nd[B].mac, 6) == 0, "802.3 destination is B");
    CHECK(got != NULL && memcmp(got->sa, nd[A].mac, 6) == 0,
          "802.3 source is A, not the relay W");
    CHECK(got != NULL && got->len == sizeof(body) && memcmp(got->payload, body, sizeof(body)) == 0,
          "and the payload survived the relay byte for byte (%u bytes)", got ? got->len : 0);

    CHECK(nd[W].host_rx_count() == 0,
          "W relayed it and did NOT deliver it to its own host (%u)", nd[W].host_rx_count());
    CHECK(nd[A].host_rx_count() == 0, "A did not receive its own frame (%u)",
          nd[A].host_rx_count());

    for (int i = 0; i < NODES; i++)
    {
        CHECK(nd[i].live_allocs() <= allocs_before[i],
              "%s freed every buffer it took (%u -> %u)", nd[i].name, allocs_before[i],
              nd[i].live_allocs());
    }
}

/**
 * 3. A flood that reaches everyone exactly once.
 *
 * A triangle, so the frame arrives at each of W and B twice: once from A and
 * once from the other relay. Delivering it twice is the bug this rules out,
 * and so is a rebroadcast that never dies.
 */
static void scenario_flood(void)
{
    printf("--- 3: a broadcast reaches every node exactly once, in a triangle ---\n");
    mesh_restart(/*fwd=*/true, false, /*grp_std=*/true, false);
    topo_link(A, W);
    topo_link(W, B);
    topo_link(A, B); /* the third edge: every relay is heard by every other node */
    air_reset();

    static const uint8_t body[12] = { 0xca, 0xfe, 0xba, 0xbe, 0x01, 0x02 };
    CHECK(nd[A].host_tx(BCAST, nd[A].mac, body, sizeof(body)), "A broadcasts 12 bytes");
    unsigned rounds = air_settle();
    CHECK(rounds != 0, "the flood dies out (%u air rounds) -- it is not a broadcast storm",
          rounds);

    CHECK(count_data(A) == 1, "A sends one group frame (%u)", count_data(A));
    const struct simnode_frame *g = find_data(A, 0);
    struct umac_mesh_rx_frame gf;
    memset(&gf, 0, sizeof(gf));
    bool g_ok = g != NULL && fw_parse_frame(g->bytes, g->len, &gf) != 0;
    CHECK(g_ok && gf.group, "group-addressed, in the three-address form a mac80211 peer expects");
    CHECK(g_ok && memcmp(gf.addr3, nd[A].mac, 6) == 0, "with A as the mesh source in addr3");

    CHECK(nd[W].host_rx_count() == 1, "W's host receives it exactly once (%u), though W heard "
          "it from both A and B", nd[W].host_rx_count());
    CHECK(nd[B].host_rx_count() == 1, "B's host receives it exactly once (%u), though B heard "
          "it from both A and W", nd[B].host_rx_count());
    CHECK(nd[A].host_rx_count() == 0,
          "A does not receive its own broadcast back (%u)", nd[A].host_rx_count());

    const struct simnode_hostrx *wgot = nd[W].host_rx_get(0);
    CHECK(wgot != NULL && memcmp(wgot->sa, nd[A].mac, 6) == 0 &&
          memcmp(wgot->da, BCAST, 6) == 0,
          "delivered as a broadcast from A");
    CHECK(wgot != NULL && wgot->len == sizeof(body) &&
          memcmp(wgot->payload, body, sizeof(body)) == 0,
          "with the payload intact (%u bytes)", wgot ? wgot->len : 0);

    CHECK(count_data(W) == 1, "W rebroadcasts it exactly once (%u)", count_data(W));
    CHECK(count_data(B) == 1, "B rebroadcasts it exactly once (%u)", count_data(B));

    const struct simnode_frame *wg = find_data(W, 0);
    struct umac_mesh_rx_frame wf;
    memset(&wf, 0, sizeof(wf));
    bool w_ok = wg != NULL && fw_parse_frame(wg->bytes, wg->len, &wf) != 0;
    CHECK(w_ok && g_ok && memcmp(wf.addr3, nd[A].mac, 6) == 0 && wf.mc.seq == gf.mc.seq,
          "carrying A's mesh source and sequence number, which is what lets the third "
          "node recognise the copy as a duplicate");
    CHECK(w_ok && g_ok && wf.mc.ttl == (uint8_t)(gf.mc.ttl - 1u),
          "and one hop of TTL spent (%u -> %u)", g_ok ? gf.mc.ttl : 0, w_ok ? wf.mc.ttl : 0);

    /* Re-injecting the very same bytes must still not deliver twice. */
    air_reset();
    if (g != NULL) { nd[W].rx(g->bytes, g->len, -60); }
    nd[W].pump();
    CHECK(nd[W].host_rx_count() == 0,
          "replaying the identical frame at W delivers nothing further (%u)",
          nd[W].host_rx_count());

    /* The same triangle, but a unicast. B is in range and receives every
     * octet of it -- a radio has no address filter, the firmware is the
     * filter -- and B is neither the addressee nor on the path. */
    air_reset();
    static const uint8_t direct[8] = { 0xd1, 0xd2, 0xd3, 0xd4 };
    CHECK(nd[A].host_tx(nd[W].mac, nd[A].mac, direct, sizeof(direct)),
          "A sends a unicast to its neighbour W, with B in range");
    CHECK(air_settle() != 0, "it settles");
    CHECK(count_data(A) == 1, "A sends it once (%u)", count_data(A));
    const struct simnode_frame *u = find_data(A, 0);
    CHECK(u != NULL && memcmp(fr_ra(u), nd[W].mac, 6) == 0, "addressed to W");
    CHECK(nd[W].host_rx_count() == 1, "W's host receives it (%u)", nd[W].host_rx_count());
    CHECK(nd[B].host_rx_count() == 0,
          "B overheard the whole frame and delivered nothing (%u) -- addr1 is not B",
          nd[B].host_rx_count());
    CHECK(count_data(B) == 0,
          "and relayed nothing (%u): overhearing a unicast is not a reason to forward it",
          count_data(B));
}

/**
 * 4. A lost link produces a PERR that reaches the far side.
 *
 * W loses B. A, two hops away and using W to reach B, must hear about it and
 * stop treating its path as usable.
 */
static void scenario_link_loss(void)
{
    printf("--- 4: W loses B, and A two hops away hears the PERR ---\n");
    mesh_restart(/*fwd=*/true, false, false, false);
    topo_link(A, W);
    topo_link(W, B);
    air_reset();

    nd[A].host_tx(nd[B].mac, nd[A].mac, payload, sizeof(payload));
    CHECK(air_settle() != 0, "A discovers B through W first");

    char buf[768];
    const char *before = path_line(A, nd[B].mac, buf, sizeof(buf));
    CHECK(before != NULL && strstr(before, "active") != NULL,
          "A's path to B is active before the link is lost");

    air_reset();
    /* The peering watchdog's own call: umac_datapath_mesh_del_peer() is what
     * runs when a peer stops answering, and it is what tells the relay. */
    nd[W].del_peer(nd[B].mac);
    in_range[W][B] = in_range[B][W] = false;
    nd[W].pump();
    unsigned rounds = air_settle();
    CHECK(rounds != 0, "the PERR exchange settles (%u air rounds)", rounds);

    CHECK(count_hwmp(W, HWMP_EID_PERR) >= 1,
          "W announces the loss with a PERR (%u)", count_hwmp(W, HWMP_EID_PERR));
    const struct simnode_frame *perr = find_hwmp(W, HWMP_EID_PERR, 0);
    struct hwmp_perr e0;
    memset(&e0, 0, sizeof(e0));
    bool e0_ok = false;
    if (perr != NULL)
    {
        uint16_t blen = 0;
        const uint8_t *pbody = hwmp_body(perr, &blen);
        e0_ok = (pbody != NULL) && hwmp_parse_perr(pbody, blen, &e0);
        CHECK(memcmp(fr_ta(perr), nd[W].mac, 6) == 0, "from W");
    }
    CHECK(e0_ok, "the shipping PERR parser accepts the shipping PERR builder's bytes");
    CHECK(e0_ok && memcmp(e0.dest_addr, nd[B].mac, 6) == 0,
          "naming B as the unreachable destination");
    CHECK(e0_ok && e0.reason == HWMP_REASON_MESH_PATH_ERROR_DEST_UNREACHABLE,
          "with reason 'destination unreachable' (%u)", e0_ok ? e0.reason : 0);

    char buf2[768];
    const char *after = path_line(A, nd[B].mac, buf2, sizeof(buf2));
    CHECK(after != NULL, "A still lists the destination");
    CHECK(after != NULL && strstr(after, "active") == NULL,
          "but it is no longer usable -- the PERR crossed two hops and reached A");
    CHECK(after != NULL && strstr(after, "dead") != NULL, "it is marked dead");
    CHECK(line_sn(after) > line_sn(before),
          "at a newer sequence number than the path it replaces (%u -> %u), so a stale "
          "PREP cannot resurrect it", line_sn(before), line_sn(after));

    /* A frame for B now has nowhere to go: it must be held for discovery, not
     * silently sent down the dead path. Past the shipped per-target PREQ
     * interval, so the rate limiter is not what is being measured. */
    air_reset();
    air_advance(UMAC_MESH_PREQ_MIN_INTERVAL_MS + 100u);
    nd[A].host_tx(nd[B].mac, nd[A].mac, payload, sizeof(payload));
    air_settle();
    CHECK(count_data(A) == 0, "a new frame for B is not sent down the dead path (%u)",
          count_data(A));
    CHECK(count_hwmp(A, HWMP_EID_PREQ) >= 1,
          "A asks again instead (%u PREQ)", count_hwmp(A, HWMP_EID_PREQ));
    char buf3[768];
    CHECK(nd[A].render_paths(buf3, sizeof(buf3)) > 0 && strstr(buf3, "pending=1") != NULL,
          "and the frame is held for that discovery, not dropped");
}

int main(int argc, char **argv)
{
    static char dir[512];
    if (argc > 0 && argv[0] != NULL)
    {
        snprintf(dir, sizeof(dir), "%s", argv[0]);
        char *slash = strrchr(dir, '/');
        if (slash != NULL) { *slash = '\0'; s_exedir = dir; }
    }
    printf("=== simnode multinode: three real stacks on a virtual air ===\n");
    open_nodes();
    memset(payload, 0xA5, sizeof(payload));

    for (int i = 0; i < NODES; i++)
    {
        CHECK(nd[i].start(nd[i].mac), "node %s starts", nd[i].name);
    }
    /* The isolation the whole file rests on: if the three nodes shared statics
     * this would be the first thing to fail, and every assertion after it
     * would be about one stack talking to itself. */
    nd[A].advance(7000);
    nd[W].advance(3000);
    CHECK(nd[A].now() != nd[W].now() && nd[W].now() != nd[B].now(),
          "the three nodes have private clocks (%u / %u / %u), so their statics are "
          "genuinely separate", nd[A].now(), nd[W].now(), nd[B].now());
    nd[W].advance(4000);
    CHECK(nd[A].now() == nd[W].now(), "and the test can put them back in step (%u / %u)",
          nd[A].now(), nd[W].now());

    scenario_discovery();
    scenario_relay();
    scenario_flood();
    scenario_link_loss();

    for (int i = 0; i < NODES; i++) { nd[i].stop(); }

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_simnode_multinode: all passed\n");
    return 0;
}
