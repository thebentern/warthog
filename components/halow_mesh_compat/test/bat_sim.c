/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * In-process multi-engine simulator for the BATMAN_V engine; see bat_sim.h.
 *
 * Time only moves in bat_sim_run(). Each 1 ms step first delivers every queued link
 * frame that is due (FIFO; each delivery is followed by that node's bat_tick, as the
 * firmware's engine task does), then ticks every node whose last bat_tick asked to run
 * now. Frames a node sends while handling a step are due one step later, so an engine
 * is never re-entered from inside its own ops->tx.
 */
#include "bat_sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIM_RX_MAX   4096u     /* soft frames kept per node */
#define SIM_CAP_MAX  20000u    /* captured link frames kept */

struct sim_node {
    struct bat_sim *s;
    unsigned idx;
    struct bat *b;
    struct bat_config cfg;
    bool started, stopped;
    uint32_t rng;
    uint32_t (*rfn)(void *);
    void *rarg;
    uint32_t next_tick;
    uint8_t hard[6], soft[6];
    struct bat_sim_frame *rx;
    unsigned nrx, caprx;
};

struct sim_q {
    unsigned from, to;
    uint32_t t;
    size_t len;
    bool done;
    uint8_t bytes[BAT_MAX_LINK_FRAME];
};

struct bat_sim {
    unsigned n;
    uint32_t now;
    uint32_t loss_rng;
    struct sim_node node[BAT_SIM_MAX_NODES];
    bool up[BAT_SIM_MAX_NODES][BAT_SIM_MAX_NODES];
    uint32_t tput[BAT_SIM_MAX_NODES][BAT_SIM_MAX_NODES];
    uint32_t loss[BAT_SIM_MAX_NODES][BAT_SIM_MAX_NODES];
    struct sim_q *q;
    unsigned qn, qcap;
    bool capture;
    struct bat_sim_frame *cap;
    unsigned ncap, capcap;
};

static uint32_t xorshift(uint32_t *x)
{
    uint32_t v = *x;
    v ^= v << 13;
    v ^= v >> 17;
    v ^= v << 5;
    *x = v;
    return v;
}

static void *xrealloc(void *p, size_t n)
{
    void *r = realloc(p, n);
    if (!r) {
        fprintf(stderr, "bat_sim: out of memory\n");
        abort();
    }
    return r;
}

int bat_sim_node_of(const struct bat_sim *s, const uint8_t hard[6])
{
    for (unsigned i = 0; i < s->n; i++) {
        if (memcmp(s->node[i].hard, hard, 6) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool lost(struct bat_sim *s, unsigned from, unsigned to)
{
    uint32_t pm = s->loss[from][to];
    return pm && xorshift(&s->loss_rng) % 1000u < pm;
}

static void enqueue(struct bat_sim *s, unsigned from, unsigned to, const uint8_t *frame, size_t len)
{
    if (s->qn == s->qcap) {
        s->qcap = s->qcap ? 2 * s->qcap : 64;
        s->q = xrealloc(s->q, s->qcap * sizeof(*s->q));
    }
    struct sim_q *e = &s->q[s->qn++];
    e->from = from;
    e->to = to;
    e->t = s->now + 1;
    e->len = len;
    e->done = false;
    memcpy(e->bytes, frame, len);
}

static void record(struct bat_sim_frame **arr, unsigned *n, unsigned *cap, unsigned max,
                   unsigned from, unsigned to, uint32_t t, const uint8_t *frame, size_t len)
{
    if (*n >= max) {
        return;
    }
    if (*n == *cap) {
        *cap = *cap ? 2 * *cap : 16;
        *arr = xrealloc(*arr, *cap * sizeof(**arr));
    }
    struct bat_sim_frame *f = &(*arr)[(*n)++];
    f->from = from;
    f->to = to;
    f->t = t;
    f->len = len > sizeof(f->bytes) ? sizeof(f->bytes) : len;
    memcpy(f->bytes, frame, f->len);
}

static int op_tx(void *user, const uint8_t *frame, size_t len)
{
    struct sim_node *nd = user;
    struct bat_sim *s = nd->s;
    unsigned i = nd->idx;
    if (len < 14 || len > BAT_MAX_LINK_FRAME) {
        return BAT_TX_FAIL;
    }
    bool group = frame[0] & 1;
    int j = group ? -1 : bat_sim_node_of(s, frame);
    if (s->capture) {
        unsigned to = group ? BAT_SIM_BCAST : (j < 0 ? BAT_SIM_NONE : (unsigned)j);
        record(&s->cap, &s->ncap, &s->capcap, SIM_CAP_MAX, i, to, s->now, frame, len);
    }
    if (group) {
        bool any = false;
        for (unsigned k = 0; k < s->n; k++) {
            if (k == i || !s->up[i][k]) {
                continue;
            }
            any = true;
            if (!lost(s, i, k)) {
                enqueue(s, i, k, frame, len);
            }
        }
        return any ? BAT_TX_OK : BAT_TX_NOPEER;
    }
    if (j < 0 || (unsigned)j == i || !s->up[i][j]) {
        return BAT_TX_NOPEER;
    }
    if (!lost(s, i, (unsigned)j)) {
        enqueue(s, i, (unsigned)j, frame, len);
    }
    return BAT_TX_OK;
}

static void op_deliver(void *user, const uint8_t *frame, size_t len)
{
    struct sim_node *nd = user;
    record(&nd->rx, &nd->nrx, &nd->caprx, SIM_RX_MAX, nd->idx, nd->idx, nd->s->now, frame, len);
}

static uint32_t op_now(void *user)
{
    return ((struct sim_node *)user)->s->now;
}

static uint32_t op_rand(void *user)
{
    struct sim_node *nd = user;
    return nd->rfn ? nd->rfn(nd->rarg) : xorshift(&nd->rng);
}

static uint32_t op_link_tput(void *user, const uint8_t hard[6])
{
    struct sim_node *nd = user;
    struct bat_sim *s = nd->s;
    int j = bat_sim_node_of(s, hard);
    if (j < 0 || !s->up[nd->idx][j]) {
        return 0;
    }
    return s->tput[nd->idx][j];
}

static const struct bat_ops sim_ops = {
    .tx = op_tx, .deliver = op_deliver, .now_ms = op_now, .rand32 = op_rand,
    .link_tput = op_link_tput,
};

struct bat_sim *bat_sim_new(unsigned nodes, uint32_t seed)
{
    if (nodes == 0 || nodes > BAT_SIM_MAX_NODES) {
        return NULL;
    }
    struct bat_sim *s = calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->n = nodes;
    s->loss_rng = seed * 2654435761u + 0x9e3779b9u;
    if (s->loss_rng == 0) {
        s->loss_rng = 1;
    }
    for (unsigned i = 0; i < nodes; i++) {
        struct sim_node *nd = &s->node[i];
        nd->s = s;
        nd->idx = i;
        nd->b = calloc(1, bat_ctx_size());
        if (!nd->b) {
            bat_sim_free(s);
            return NULL;
        }
        nd->rng = (seed + 1u) * 2246822519u + (i + 1u) * 3266489917u;
        if (nd->rng == 0) {
            nd->rng = 0x1234567u;
        }
        const uint8_t hard[6] = { 0x02, 0x5a, 0x00, 0x00, 0x00, (uint8_t)(i + 1) };
        const uint8_t soft[6] = { 0x06, 0x5a, 0x00, 0x00, 0x00, (uint8_t)(i + 1) };
        memcpy(nd->hard, hard, 6);
        memcpy(nd->soft, soft, 6);
        bat_config_defaults(&nd->cfg);
        memcpy(nd->cfg.hard_addr, hard, 6);
        memcpy(nd->cfg.soft_addr, soft, 6);
    }
    return s;
}

void bat_sim_free(struct bat_sim *s)
{
    if (!s) {
        return;
    }
    for (unsigned i = 0; i < s->n; i++) {
        free(s->node[i].b);
        free(s->node[i].rx);
    }
    free(s->q);
    free(s->cap);
    free(s);
}

struct bat_config *bat_sim_cfg(struct bat_sim *s, unsigned i)
{
    return &s->node[i].cfg;
}

void bat_sim_start(struct bat_sim *s, unsigned i)
{
    struct sim_node *nd = &s->node[i];
    if (bat_init(nd->b, &nd->cfg, &sim_ops, nd) < 0) {
        fprintf(stderr, "bat_sim: bat_init failed for node %u\n", i);
        abort();
    }
    nd->started = true;
    nd->stopped = false;
    nd->next_tick = s->now + bat_tick(nd->b);
}

void bat_sim_restart(struct bat_sim *s, unsigned i)
{
    bat_sim_start(s, i);
}

void bat_sim_stop(struct bat_sim *s, unsigned i)
{
    s->node[i].stopped = true;
}

void bat_sim_link_dir(struct bat_sim *s, unsigned from, unsigned to, bool up, uint32_t tput_units)
{
    s->up[from][to] = up;
    s->tput[from][to] = tput_units;
}

void bat_sim_link(struct bat_sim *s, unsigned a, unsigned b, bool up, uint32_t tput_units)
{
    bat_sim_link_dir(s, a, b, up, tput_units);
    bat_sim_link_dir(s, b, a, up, tput_units);
}

void bat_sim_loss(struct bat_sim *s, unsigned from, unsigned to, uint32_t permille)
{
    s->loss[from][to] = permille;
}

void bat_sim_set_rand(struct bat_sim *s, unsigned i, uint32_t (*fn)(void *), void *arg)
{
    s->node[i].rfn = fn;
    s->node[i].rarg = arg;
}

static bool alive(const struct sim_node *nd)
{
    return nd->started && !nd->stopped;
}

static void tick(struct bat_sim *s, struct sim_node *nd)
{
    nd->next_tick = s->now + bat_tick(nd->b);
}

static void rx_one(struct bat_sim *s, unsigned to, const uint8_t *frame, size_t len)
{
    uint8_t buf[BAT_MAX_LINK_FRAME + 64];
    struct sim_node *nd = &s->node[to];
    if (!alive(nd) || len > sizeof(buf)) {
        return;
    }
    memcpy(buf, frame, len);
    bat_rx_hard(nd->b, buf, len);
    tick(s, nd);
}

void bat_sim_run(struct bat_sim *s, uint32_t ms)
{
    while (ms--) {
        s->now++;
        unsigned n = s->qn;
        for (unsigned k = 0; k < n; k++) {
            if (s->q[k].done || (int32_t)(s->now - s->q[k].t) < 0) {
                continue;
            }
            s->q[k].done = true;
            struct sim_q e = s->q[k];            /* the queue may grow while delivering */
            rx_one(s, e.to, e.bytes, e.len);
        }
        unsigned w = 0;
        for (unsigned k = 0; k < s->qn; k++) {
            if (!s->q[k].done) {
                if (w != k) {
                    s->q[w] = s->q[k];
                }
                w++;
            }
        }
        s->qn = w;
        for (unsigned i = 0; i < s->n; i++) {
            struct sim_node *nd = &s->node[i];
            if (alive(nd) && (int32_t)(s->now - nd->next_tick) >= 0) {
                tick(s, nd);
            }
        }
    }
}

uint32_t bat_sim_now(const struct bat_sim *s)
{
    return s->now;
}

void bat_sim_set_now(struct bat_sim *s, uint32_t now)
{
    s->now = now;
}

struct bat *bat_sim_engine(struct bat_sim *s, unsigned i)
{
    return s->node[i].b;
}

const uint8_t *bat_sim_hard(const struct bat_sim *s, unsigned i)
{
    return s->node[i].hard;
}

const uint8_t *bat_sim_soft(const struct bat_sim *s, unsigned i)
{
    return s->node[i].soft;
}

int bat_sim_soft_tx(struct bat_sim *s, unsigned i, const uint8_t *frame, size_t len)
{
    struct sim_node *nd = &s->node[i];
    if (!alive(nd)) {
        return -1;
    }
    int r = bat_tx_soft(nd->b, frame, len);
    tick(s, nd);
    return r;
}

unsigned bat_sim_soft_rx_count(const struct bat_sim *s, unsigned i)
{
    return s->node[i].nrx;
}

const struct bat_sim_frame *bat_sim_soft_rx_get(const struct bat_sim *s, unsigned i, unsigned k)
{
    return k < s->node[i].nrx ? &s->node[i].rx[k] : NULL;
}

void bat_sim_soft_rx_clear(struct bat_sim *s, unsigned i)
{
    s->node[i].nrx = 0;
}

void bat_sim_inject(struct bat_sim *s, unsigned to, const uint8_t *link_frame, size_t len)
{
    rx_one(s, to, link_frame, len);
}

void bat_sim_capture(struct bat_sim *s, bool on)
{
    s->capture = on;
}

void bat_sim_capture_clear(struct bat_sim *s)
{
    s->ncap = 0;
}

unsigned bat_sim_captured(const struct bat_sim *s)
{
    return s->ncap;
}

const struct bat_sim_frame *bat_sim_captured_get(const struct bat_sim *s, unsigned k)
{
    return k < s->ncap ? &s->cap[k] : NULL;
}

uint32_t bat_sim_counter(struct bat_sim *s, unsigned i, enum bat_counter c)
{
    return s->node[i].started ? bat_counter(s->node[i].b, c) : 0;
}

size_t bat_sim_mk_eth(uint8_t *out, const uint8_t dst[6], const uint8_t src[6], uint16_t type,
                      size_t payload_len, uint8_t fill)
{
    memcpy(out, dst, 6);
    memcpy(out + 6, src, 6);
    out[12] = (uint8_t)(type >> 8);
    out[13] = (uint8_t)type;
    memset(out + 14, fill, payload_len);
    return 14 + payload_len;
}
