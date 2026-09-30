/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 */
#include "umac_mesh_fwd.h"

#include <string.h>

static bool eq_(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 6) == 0; }
static bool is_group_(const uint8_t *a) { return (a[0] & 0x01u) != 0u; }

static void drop_(struct umac_mesh_fwd_rx_result *r, enum umac_mesh_fwd_drop why)
{
    r->verdict = UMAC_MESH_FWD_DROP;
    r->drop = why;
}

/* A proxied address that is us, a neighbour, or a node we hold a path to is
 * not a host behind anyone; learning it would let one AE frame redirect
 * unicast for a mesh node. mac80211 consults the MPP table only when no
 * mesh path exists, which is the same rule from the other side. */
static bool proxy_ok_(const struct umac_mesh_fwd_ctx *c, const uint8_t *ext)
{
    if (eq_(ext, c->own_addr) || is_group_(ext))
    {
        return false;
    }
    if (c->is_peer != NULL && c->is_peer(ext, c->is_peer_arg))
    {
        return false;
    }
    return umac_mesh_path_lookup(c->tbl, ext, c->now_ms) == NULL;
}

bool umac_mesh_fwd_leaf_learn(const struct umac_mesh_fwd_ctx *c, const struct umac_mesh_rx_frame *f)
{
    /* mac80211 drops ttl 0 before it learns anything from the frame. */
    if (c == NULL || f == NULL || c->tbl == NULL || c->own_addr == NULL || c->is_peer == NULL ||
        f->mc.ttl == 0u)
    {
        return false;
    }
    uint8_t ae = umac_mesh_ctrl_ae(&f->mc);
    const uint8_t *ext, *mesh_sa;
    bool uni, flood;
    /* The shapes umac_mesh_fwd_rx accepts: AE 1 on a group frame, AE 2 on a
     * unicast. A warthog replica is AE 2 with a group e1: a flood, so not uni. */
    if (f->group)
    {
        if (ae != UMAC_MESH_CTRL_AE_A4) { return false; }
        ext = f->mc.eaddr1;
        mesh_sa = f->addr3;
        uni = false;
        flood = true;
    }
    else
    {
        if (ae != UMAC_MESH_CTRL_AE_A5A6) { return false; }
        ext = f->mc.eaddr2;
        mesh_sa = f->addr4;
        uni = eq_(f->mc.eaddr1, c->own_addr);
        flood = is_group_(f->mc.eaddr1);
    }
    /* A node is never a host behind itself: mac80211 sends to it plain. */
    if (!c->is_peer(f->addr2, c->is_peer_arg) || is_group_(mesh_sa) || eq_(mesh_sa, c->own_addr) ||
        eq_(ext, mesh_sa) || !proxy_ok_(c, ext))
    {
        return false;
    }
    /* A flood's second copy, through another relay, must not rewrite via:
     * mac80211 drops it in the RMC before learning, so the first copy wins. */
    if (flood && c->rmc != NULL && umac_mesh_rmc_check(c->rmc, mesh_sa, f->mc.seq, c->now_ms))
    {
        return false;
    }
    /* A relay that carried this host's unicast to us held a path to its node; a
     * flood's transmitter need not. Keep the former while that path can be live.
     * The age is unsigned (2^31 ms is not fresh); the sweep keeps it below 2^32. */
    uint8_t via[6];
    memcpy(via, f->addr2, 6);
    const struct umac_mesh_proxy *x = umac_mesh_proxy_entry(c->tbl, ext, c->now_ms);
    if (!uni && x != NULL && x->leaf && x->uni && eq_(x->mesh_sta, mesh_sa) &&
        (uint32_t)(c->now_ms - x->uni_ms) < UMAC_MESH_LEAF_VIA_HOLD_MS &&
        c->is_peer(x->via, c->is_peer_arg))
    {
        memcpy(via, x->via, 6);
    }
    return umac_mesh_proxy_learn_leaf(c->tbl, ext, mesh_sa, via, uni, c->now_ms);
}

bool umac_mesh_fwd_leaf_proxied(const struct umac_mesh_fwd_ctx *c, const uint8_t *da)
{
    if (c == NULL || c->tbl == NULL || c->is_peer == NULL || da == NULL || is_group_(da) ||
        c->is_peer(da, c->is_peer_arg))
    {
        return false;
    }
    const struct umac_mesh_proxy *x = umac_mesh_proxy_entry(c->tbl, da, c->now_ms);
    return x != NULL && x->leaf;
}

bool umac_mesh_fwd_proxy_via_peer(const struct umac_mesh_fwd_ctx *c, const uint8_t *da,
                                  uint8_t out[6])
{
    if (out == NULL || !umac_mesh_fwd_leaf_proxied(c, da))
    {
        return false;
    }
    struct umac_mesh_fwd_tx_result t;
    umac_mesh_fwd_tx(c, da, c->own_addr, 0, &t);
    if (!t.ok || t.shape != UMAC_MESH_TX_UNICAST_4ADDR)
    {
        return false;
    }
    memcpy(out, t.ra, 6);
    return true;
}

void umac_mesh_fwd_rx(const struct umac_mesh_fwd_ctx *c, const struct umac_mesh_rx_frame *f,
                      struct umac_mesh_fwd_rx_result *r)
{
    if (r == NULL) { return; }
    memset(r, 0, sizeof(*r));
    if (c == NULL || f == NULL || c->own_addr == NULL || c->tbl == NULL)
    {
        drop_(r, UMAC_MESH_FWD_DROP_BAD_AE);
        return;
    }
    uint8_t ae = umac_mesh_ctrl_ae(&f->mc);
    const uint8_t *mesh_sa = f->group ? f->addr3 : f->addr4;
    memcpy(r->mesh_sa, mesh_sa, 6);
    memcpy(r->mesh_da, f->group ? f->addr1 : f->addr3, 6);

    if (eq_(mesh_sa, c->own_addr))
    {
        drop_(r, UMAC_MESH_FWD_DROP_OWN);
        return;
    }
    /* mac80211: a Mesh Control ttl of 0 is dropped outright, group or not. */
    if (f->mc.ttl == 0u)
    {
        drop_(r, UMAC_MESH_FWD_DROP_TTL0);
        return;
    }

    if (f->group)
    {
        /* AE 2 has no meaning on a group frame. */
        if (ae == UMAC_MESH_CTRL_AE_A5A6)
        {
            drop_(r, UMAC_MESH_FWD_DROP_BAD_AE);
            return;
        }
        if (c->rmc != NULL && umac_mesh_rmc_check(c->rmc, mesh_sa, f->mc.seq, c->now_ms))
        {
            drop_(r, UMAC_MESH_FWD_DROP_DUP);
            return;
        }
        /* The proxied source sits behind the mesh SA. */
        if (ae == UMAC_MESH_CTRL_AE_A4 && proxy_ok_(c, f->mc.eaddr1))
        {
            (void)umac_mesh_proxy_learn(c->tbl, f->mc.eaddr1, mesh_sa, c->now_ms);
        }
        memcpy(r->deliver_da, f->addr1, 6);
        memcpy(r->deliver_sa, ae == UMAC_MESH_CTRL_AE_A4 ? f->mc.eaddr1 : mesh_sa, 6);
        /* mac80211: deliver locally regardless; forward only if enabled and
         * ttl > 1. The rebroadcast keeps SA, seq and AE so downstream RMCs see
         * the same identity. */
        if (c->forwarding && f->mc.ttl > 1u)
        {
            r->verdict = UMAC_MESH_FWD_DELIVER_AND_FORWARD;
            memcpy(r->fwd_ra, f->addr1, 6);
            r->fwd_mc = f->mc;
            r->fwd_mc.ttl = (uint8_t)(f->mc.ttl - 1u);
        }
        else
        {
            r->verdict = UMAC_MESH_FWD_DELIVER;
            r->drop = c->forwarding ? UMAC_MESH_FWD_DROP_TTL : UMAC_MESH_FWD_DROP_NO_FWD;
        }
        return;
    }

    /* Individually addressed. */
    if (!eq_(f->addr1, c->own_addr))
    {
        drop_(r, UMAC_MESH_FWD_DROP_NOT_FOR_US);
        return;
    }
    if (ae == UMAC_MESH_CTRL_AE_A4)
    {
        drop_(r, UMAC_MESH_FWD_DROP_BAD_AE); /* AE 1 is the group form */
        return;
    }
    const uint8_t *mesh_da = f->addr3;
    if (ae == UMAC_MESH_CTRL_AE_A5A6 && proxy_ok_(c, f->mc.eaddr2))
    {
        (void)umac_mesh_proxy_learn(c->tbl, f->mc.eaddr2, mesh_sa, c->now_ms);
    }
    if (eq_(mesh_da, c->own_addr))
    {
        r->verdict = UMAC_MESH_FWD_DELIVER;
        memcpy(r->deliver_da, ae == UMAC_MESH_CTRL_AE_A5A6 ? f->mc.eaddr1 : mesh_da, 6);
        memcpy(r->deliver_sa, ae == UMAC_MESH_CTRL_AE_A5A6 ? f->mc.eaddr2 : mesh_sa, 6);
        return;
    }
    /* For someone else: this is the relay case. */
    if (!c->forwarding)
    {
        drop_(r, UMAC_MESH_FWD_DROP_NO_FWD);
        return;
    }
    if (f->mc.ttl <= 1u)
    {
        drop_(r, UMAC_MESH_FWD_DROP_TTL);
        return;
    }
    const struct umac_mesh_path *p = umac_mesh_path_lookup(c->tbl, mesh_da, c->now_ms);
    const uint8_t *next = NULL;
    if (p != NULL)
    {
        next = p->next_hop;
    }
    else if (c->is_peer != NULL && c->is_peer(mesh_da, c->is_peer_arg))
    {
        next = mesh_da;
    }
    r->fwd_mc = f->mc;
    r->fwd_mc.ttl = (uint8_t)(f->mc.ttl - 1u);
    if (next == NULL)
    {
        if (is_group_(mesh_da))
        {
            drop_(r, UMAC_MESH_FWD_DROP_NO_PATH); /* mac80211 never discovers a group address */
            return;
        }
        /* OpenMANET's mac80211 (999-0027): queue it and discover; no PERR. */
        r->verdict = UMAC_MESH_FWD_HOLD;
        r->drop = UMAC_MESH_FWD_DROP_NO_PATH;
        return;
    }
    r->verdict = UMAC_MESH_FWD_FORWARD;
    memcpy(r->fwd_ra, next, 6);
}

void umac_mesh_fwd_tx(const struct umac_mesh_fwd_ctx *c, const uint8_t *da, const uint8_t *sa,
                      uint32_t seq, struct umac_mesh_fwd_tx_result *r)
{
    if (r == NULL) { return; }
    memset(r, 0, sizeof(*r));
    if (c == NULL || da == NULL || sa == NULL || c->own_addr == NULL || c->tbl == NULL)
    {
        return;
    }
    bool proxied_sa = !eq_(sa, c->own_addr);
    r->mc.ttl = c->element_ttl;
    r->mc.seq = seq;

    if (is_group_(da))
    {
        /* 3-address: addr1 = group DA, addr3 = mesh SA (us). A host behind us
         * is expressed as AE 1 carrying its address. */
        r->shape = UMAC_MESH_TX_GROUP_3ADDR;
        memcpy(r->ra, da, 6);
        memcpy(r->addr3, c->own_addr, 6);
        if (proxied_sa)
        {
            r->mc.flags = UMAC_MESH_CTRL_AE_A4;
            memcpy(r->mc.eaddr1, sa, 6);
        }
        r->ok = true;
        return;
    }

    /* Individually addressed. mac80211 precedence: a destination that is a
     * mesh node -- a neighbour, or one we hold a path to -- is addressed as
     * such; only otherwise is it a host behind some node, in which case the
     * mesh DA is that node and both ends ride in AE 2. */
    const struct umac_mesh_path *p = umac_mesh_path_lookup(c->tbl, da, c->now_ms);
    bool direct = (c->is_peer != NULL && c->is_peer(da, c->is_peer_arg));
    const uint8_t *proxy = (p == NULL && !direct) ? umac_mesh_proxy_lookup(c->tbl, da, c->now_ms) : NULL;
    const uint8_t *mesh_da = proxy != NULL ? proxy : da;
    bool ae2 = (proxy != NULL) || proxied_sa;

    r->shape = UMAC_MESH_TX_UNICAST_4ADDR;
    memcpy(r->addr3, mesh_da, 6);
    memcpy(r->addr4, c->own_addr, 6);
    if (ae2)
    {
        r->mc.flags = UMAC_MESH_CTRL_AE_A5A6;
        memcpy(r->mc.eaddr1, da, 6);
        memcpy(r->mc.eaddr2, sa, 6);
    }
    if (proxy != NULL)
    {
        umac_mesh_proxy_touch(c->tbl, da, c->now_ms);
        p = umac_mesh_path_lookup(c->tbl, mesh_da, c->now_ms);
        direct = (c->is_peer != NULL && c->is_peer(mesh_da, c->is_peer_arg));
    }
    if (p != NULL)
    {
        memcpy(r->ra, p->next_hop, 6);
        r->ok = true;
        /* Refresh before it lapses, so a flow never waits on rediscovery. */
        if ((int32_t)(p->exp_ms - c->now_ms) < (int32_t)UMAC_MESH_PATH_REFRESH_MS)
        {
            r->refresh = true;
            memcpy(r->path_target, mesh_da, 6);
        }
        return;
    }
    if (direct)
    {
        memcpy(r->ra, mesh_da, 6);
        r->ok = true;
        return;
    }
    /* Leaf: no path selection; the peer that carried this host's traffic to us. */
    if (proxy != NULL)
    {
        const struct umac_mesh_proxy *x = umac_mesh_proxy_entry(c->tbl, da, c->now_ms);
        if (x != NULL && x->leaf && c->is_peer != NULL && c->is_peer(x->via, c->is_peer_arg))
        {
            memcpy(r->ra, x->via, 6);
            r->ok = true;
            return;
        }
    }
    r->ok = false;
    r->need_path = true;
    memcpy(r->path_target, mesh_da, 6);
}

uint16_t umac_mesh_fwd_originate_preq(uint8_t *body, uint16_t body_len, const uint8_t *own,
                                      uint32_t *own_sn, uint32_t *preq_id, const uint8_t *target,
                                      uint32_t lifetime_tu, uint8_t ra_out[6])
{
    if (body == NULL || own == NULL || own_sn == NULL || preq_id == NULL || target == NULL ||
        ra_out == NULL)
    {
        return 0;
    }
    uint16_t n = umac_mesh_hwmp_build_preq(body, body_len, own, ++(*own_sn), ++(*preq_id), target,
                                           lifetime_tu);
    if (n != 0)
    {
        memset(ra_out, 0xff, 6);
    }
    return n;
}

bool umac_mesh_fwd_normalise_replica(struct umac_mesh_rx_frame *f)
{
    if (f == NULL || f->group || umac_mesh_ctrl_ae(&f->mc) != UMAC_MESH_CTRL_AE_A5A6 ||
        !is_group_(f->mc.eaddr1))
    {
        return false;
    }
    uint8_t src[6];
    memcpy(src, f->mc.eaddr2, 6);
    f->group = true;
    memcpy(f->addr1, f->mc.eaddr1, 6);
    memcpy(f->addr3, f->addr4, 6);
    f->mc.flags = (uint8_t)((f->mc.flags & ~UMAC_MESH_CTRL_AE_MASK) | UMAC_MESH_CTRL_AE_A4);
    memcpy(f->mc.eaddr1, src, 6);
    memset(f->mc.eaddr2, 0, 6);
    return true;
}

uint16_t umac_mesh_fwd_tx_header(const struct umac_mesh_tx_hdr_in *in,
                                 uint8_t out[UMAC_MESH_DATA_HDR4_LEN])
{
    if (in == NULL || out == NULL || in->ra == NULL || in->own == NULL ||
        in->dst8023 == NULL || in->src8023 == NULL)
    {
        return 0;
    }
    if (in->grp_std && is_group_(in->dst8023))
    {
        /* addr3 is the MESH SOURCE, which a relay must keep: mac80211's
         * forwarder changes only TA and TTL, and every cache downstream is
         * keyed on it. Only a frame we originate has us there. */
        const uint8_t *msa = (in->sidecar_valid && in->mesh_sa != NULL) ? in->mesh_sa : in->own;
        return umac_mesh_ies_build_data_hdr3_group(out, in->dst8023, in->own, msa);
    }
    const uint8_t *da = is_group_(in->dst8023) ? in->ra : in->dst8023;
    const uint8_t *sa = in->src8023;
    if (in->sidecar_valid && in->mesh_da != NULL && in->mesh_sa != NULL)
    {
        sa = in->mesh_sa;
        if (!is_group_(in->mesh_da))
        {
            da = in->mesh_da;
        }
    }
    return umac_mesh_ies_build_data_hdr4(out, in->ra, in->own, da, sa);
}

void umac_mesh_fwd_replica_ctrl(const struct umac_mesh_ctrl *native, const uint8_t *group_da,
                                const uint8_t *src, struct umac_mesh_ctrl *out)
{
    if (native == NULL || group_da == NULL || src == NULL || out == NULL)
    {
        return;
    }
    *out = *native;
    out->flags = (uint8_t)((native->flags & ~UMAC_MESH_CTRL_AE_MASK) | UMAC_MESH_CTRL_AE_A5A6);
    memcpy(out->eaddr1, group_da, 6);
    memcpy(out->eaddr2, src, 6);
}

uint16_t umac_mesh_fwd_parse_frame(const uint8_t *hdr, uint16_t len, struct umac_mesh_rx_frame *f)
{
    if (hdr == NULL || f == NULL || len < 24u + 2u + UMAC_MESH_CTRL_LEN_MIN)
    {
        return 0;
    }
    uint16_t fc = (uint16_t)(hdr[0] | (hdr[1] << 8));
    if (((fc >> 2) & 0x3u) != 2u || ((fc >> 4) & 0xfu) != 8u)
    {
        return 0; /* not QoS data */
    }
    bool to_ds = (fc & 0x0100u) != 0u, from_ds = (fc & 0x0200u) != 0u;
    uint16_t mac_len;
    memset(f, 0, sizeof(*f));
    if (to_ds && from_ds)
    {
        mac_len = 30u;
        f->group = false;
        memcpy(f->addr4, &hdr[24], 6);
    }
    else if (from_ds)
    {
        mac_len = 24u;
        f->group = true;
    }
    else
    {
        return 0; /* neither mesh shape */
    }
    memcpy(f->addr1, &hdr[4], 6);
    memcpy(f->addr2, &hdr[10], 6);
    memcpy(f->addr3, &hdr[16], 6);
    if (len < mac_len + 2u + UMAC_MESH_CTRL_LEN_MIN)
    {
        return 0;
    }
    uint16_t qos = (uint16_t)(hdr[mac_len] | (hdr[mac_len + 1] << 8));
    if ((qos & 0x0100u) == 0u)
    {
        return 0; /* no Mesh Control: not a mesh data frame */
    }
    uint16_t used = 0;
    if (!umac_mesh_ctrl_parse(&hdr[mac_len + 2], (uint16_t)(len - mac_len - 2u), &f->mc, &used))
    {
        return 0;
    }
    /* A 3-address frame whose addr1 is not a group address is not a mesh
     * group frame; the 4-address form is how unicast travels. */
    if (f->group && !is_group_(f->addr1))
    {
        return 0;
    }
    return (uint16_t)(mac_len + 2u + used);
}

void umac_mesh_preq_gate_init(struct umac_mesh_preq_gate *g)
{
    if (g != NULL)
    {
        memset(g, 0, sizeof(*g));
    }
}

bool umac_mesh_preq_gate_allow(struct umac_mesh_preq_gate *g, const uint8_t *target,
                               uint32_t now_ms)
{
    if (g == NULL || target == NULL)
    {
        return false;
    }
    if (g->any_used && (uint32_t)(now_ms - g->any_ms) < UMAC_MESH_PREQ_GLOBAL_MIN_MS)
    {
        return false;
    }
    uint32_t slot = UMAC_MESH_PREQ_TARGETS, oldest = 0;
    for (uint32_t i = 0; i < UMAC_MESH_PREQ_TARGETS; i++)
    {
        if (g->t[i].used && memcmp(g->t[i].target, target, 6) == 0)
        {
            if ((uint32_t)(now_ms - g->t[i].last_ms) < UMAC_MESH_PREQ_MIN_INTERVAL_MS)
            {
                return false;
            }
            slot = i;
            break;
        }
        if (!g->t[i].used)
        {
            if (slot == UMAC_MESH_PREQ_TARGETS) { slot = i; }
        }
        else if ((int32_t)(g->t[i].last_ms - g->t[oldest].last_ms) < 0 || !g->t[oldest].used)
        {
            oldest = i;
        }
    }
    if (slot == UMAC_MESH_PREQ_TARGETS)
    {
        slot = oldest;
    }
    if (!g->t[slot].used || memcmp(g->t[slot].target, target, 6) != 0)
    {
        g->t[slot].sent = false; /* a reused slot must not lend its old target's send */
    }
    memcpy(g->t[slot].target, target, 6);
    g->t[slot].last_ms = now_ms;
    g->t[slot].used = true;
    g->any_ms = now_ms;
    g->any_used = true;
    return true;
}

static int gate_find_(const struct umac_mesh_preq_gate *g, const uint8_t *target)
{
    for (uint32_t i = 0; g != NULL && target != NULL && i < UMAC_MESH_PREQ_TARGETS; i++)
    {
        if (g->t[i].used && memcmp(g->t[i].target, target, 6) == 0) { return (int)i; }
    }
    return -1;
}

void umac_mesh_preq_gate_sent(struct umac_mesh_preq_gate *g, const uint8_t *target, uint32_t sent_ms)
{
    int i = gate_find_(g, target);
    if (i >= 0)
    {
        g->t[i].sent_ms = sent_ms;
        g->t[i].sent = true;
    }
}

bool umac_mesh_preq_gate_last(const struct umac_mesh_preq_gate *g, const uint8_t *target,
                              uint32_t *sent_ms)
{
    int i = gate_find_(g, target);
    if (i < 0 || !g->t[i].sent || sent_ms == NULL)
    {
        return false;
    }
    *sent_ms = g->t[i].sent_ms;
    return true;
}

/* ---- protection latch --------------------------------------------------- */

void umac_mesh_prot_latch_init(struct umac_mesh_prot_latch *l)
{
    if (l != NULL) { memset(l, 0, sizeof(*l)); }
}

static int latch_find_(const struct umac_mesh_prot_latch *l, const uint8_t *ta)
{
    for (uint32_t i = 0; i < UMAC_MESH_PROT_LATCH_MAX; i++)
    {
        if (l->e[i].used && eq_(l->e[i].ta, ta)) { return (int)i; }
    }
    return -1;
}

bool umac_mesh_prot_latch_check(struct umac_mesh_prot_latch *l, const uint8_t *ta, bool is_protected)
{
    if (l == NULL || ta == NULL) { return true; }
    int i = latch_find_(l, ta);
    if (!is_protected)
    {
        return i < 0; /* plaintext is fine until this peer has shown it protects */
    }
    if (i < 0)
    {
        uint32_t v = UMAC_MESH_PROT_LATCH_MAX;
        for (uint32_t k = 0; k < UMAC_MESH_PROT_LATCH_MAX; k++)
        {
            if (!l->e[k].used) { v = k; break; }
        }
        if (v == UMAC_MESH_PROT_LATCH_MAX)
        {
            v = l->next;
            l->next = (l->next + 1u) % UMAC_MESH_PROT_LATCH_MAX;
        }
        memcpy(l->e[v].ta, ta, 6);
        l->e[v].used = true;
    }
    return true;
}

void umac_mesh_prot_latch_forget(struct umac_mesh_prot_latch *l, const uint8_t *ta)
{
    if (l == NULL || ta == NULL) { return; }
    int i = latch_find_(l, ta);
    if (i >= 0) { l->e[i].used = false; }
}

/* ---- frames held for discovery ----------------------------------------- */

void umac_mesh_pending_init(struct umac_mesh_pending *p)
{
    if (p != NULL) { memset(p, 0, sizeof(*p)); }
}

static bool pend_past_(uint32_t now, uint32_t exp) { return (int32_t)(now - exp) >= 0; }

static bool pend_live_(const struct umac_mesh_pending *p, uint32_t i, uint32_t now)
{
    return p->e[i].used && !pend_past_(now, p->e[i].exp_ms);
}

uint32_t umac_mesh_relay_ask_ms(uint32_t k)
{
    uint32_t t = 0, d = UMAC_MESH_RELAY_DISC_FIRST_MS;
    for (uint32_t i = 0; i < k && i <= UMAC_MESH_RELAY_PREQ_RETRIES; i++)
    {
        t += d;
        d = (d * 2u > UMAC_MESH_RELAY_DISC_CAP_MS) ? UMAC_MESH_RELAY_DISC_CAP_MS : d * 2u;
    }
    return t;
}

/* Own frames live in [0, PENDING_MAX), relayed ones after: each kind reuses and
 * evicts only its own slots. */
static void *pend_put_(struct umac_mesh_pending *p, uint32_t lo, uint32_t hi, const uint8_t *target,
                       void *handle, uint32_t now_ms, uint32_t *slot)
{
    uint32_t same = 0;
    int oldest_same = -1, freeslot = -1, oldest = -1;
    for (uint32_t i = lo; i < hi; i++)
    {
        if (!pend_live_(p, i, now_ms))
        {
            if (freeslot < 0) { freeslot = (int)i; }
            continue;
        }
        if (oldest < 0 || (int32_t)(p->e[i].order - p->e[oldest].order) < 0) { oldest = (int)i; }
        if (eq_(p->e[i].target, target))
        {
            same++;
            if (oldest_same < 0 || (int32_t)(p->e[i].order - p->e[oldest_same].order) < 0) { oldest_same = (int)i; }
        }
    }
    int v;
    if (same >= UMAC_MESH_PENDING_PER_TARGET) { v = oldest_same; }
    else if (freeslot >= 0) { v = freeslot; }
    else { v = oldest; }
    void *evicted = p->e[v].used ? p->e[v].handle : NULL;
    memcpy(p->e[v].target, target, 6);
    p->e[v].handle = handle;
    p->e[v].order = ++p->order;
    p->e[v].used = true;
    *slot = (uint32_t)v;
    return evicted;
}

void *umac_mesh_pending_push(struct umac_mesh_pending *p, const uint8_t *target, void *handle, uint32_t now_ms)
{
    if (p == NULL || target == NULL || handle == NULL) { return handle; }
    uint32_t v;
    void *evicted = pend_put_(p, 0, UMAC_MESH_PENDING_MAX, target, handle, now_ms, &v);
    p->e[v].exp_ms = now_ms + UMAC_MESH_PENDING_MS;
    p->e[v].start_ms = now_ms;
    p->e[v].asks = 0;
    p->e[v].relayed = false;
    return evicted;
}

void *umac_mesh_pending_push_relayed(struct umac_mesh_pending *p, const uint8_t *target,
                                     void *handle, uint32_t now_ms)
{
    if (p == NULL || target == NULL || handle == NULL) { return handle; }
    /* mac80211 queues onto a path already resolving: same ladder, same give-up. */
    uint32_t start = now_ms, asked = now_ms;
    uint8_t asks = 0;
    for (uint32_t i = UMAC_MESH_PENDING_MAX; i < UMAC_MESH_PENDING_SLOTS; i++)
    {
        if (pend_live_(p, i, now_ms) && eq_(p->e[i].target, target))
        {
            start = p->e[i].start_ms;
            asks = p->e[i].asks;
            asked = p->e[i].asked_ms;
            break;
        }
    }
    uint32_t v;
    void *evicted = pend_put_(p, UMAC_MESH_PENDING_MAX, UMAC_MESH_PENDING_SLOTS, target, handle,
                              now_ms, &v);
    p->e[v].start_ms = start;
    p->e[v].asks = asks;
    p->e[v].asked_ms = asked;
    p->e[v].exp_ms = start + umac_mesh_relay_ask_ms(UMAC_MESH_RELAY_PREQ_RETRIES + 1u);
    p->e[v].relayed = true;
    return evicted;
}

uint32_t umac_mesh_pending_take(struct umac_mesh_pending *p, const struct umac_mesh_fwd_ctx *c,
                                struct umac_mesh_pending_out *out, uint32_t max)
{
    if (p == NULL || c == NULL || out == NULL) { return 0; }
    /* Oldest first, as mac80211 sends a path's queue: slot reuse must not reorder a flow. */
    uint32_t idx[UMAC_MESH_PENDING_SLOTS], m = 0;
    for (uint32_t i = 0; i < UMAC_MESH_PENDING_SLOTS; i++)
    {
        if (!p->e[i].used) { continue; }
        uint32_t k = m++;
        while (k > 0 && (int32_t)(p->e[i].order - p->e[idx[k - 1]].order) < 0) { idx[k] = idx[k - 1]; k--; }
        idx[k] = i;
    }
    uint32_t n = 0;
    for (uint32_t j = 0; j < m && n < max; j++)
    {
        uint32_t i = idx[j];
        const uint8_t *ra = NULL;
        struct umac_mesh_fwd_tx_result t;
        if (pend_past_(c->now_ms, p->e[i].exp_ms))
        {
            /* expired: handed back with ok=false */
        }
        else if (p->e[i].relayed)
        {
            /* The copy already carries its mesh DA, SA, AE and TTL: it needs a next hop only. */
            const struct umac_mesh_path *pa =
                (c->tbl != NULL) ? umac_mesh_path_lookup(c->tbl, p->e[i].target, c->now_ms) : NULL;
            if (pa != NULL) { ra = pa->next_hop; }
            else if (c->is_peer != NULL && c->is_peer(p->e[i].target, c->is_peer_arg)) { ra = p->e[i].target; }
            else { continue; }
        }
        else
        {
            umac_mesh_fwd_tx(c, p->e[i].target, c->own_addr, 0, &t);
            if (!t.ok || t.shape != UMAC_MESH_TX_UNICAST_4ADDR) { continue; }
            ra = t.ra;
        }
        out[n].handle = p->e[i].handle;
        memcpy(out[n].target, p->e[i].target, 6);
        out[n].ok = (ra != NULL);
        if (ra != NULL) { memcpy(out[n].ra, ra, 6); }
        out[n].relayed = p->e[i].relayed;
        n++;
        p->e[i].used = false;
    }
    return n;
}

uint32_t umac_mesh_pending_targets(const struct umac_mesh_pending *p, uint32_t now_ms,
                                   uint8_t (*out)[6], uint32_t max)
{
    uint32_t n = 0;
    if (p == NULL || out == NULL) { return 0; }
    for (uint32_t i = 0; i < UMAC_MESH_PENDING_SLOTS && n < max; i++)
    {
        if (!pend_live_(p, i, now_ms) || p->e[i].relayed) { continue; }
        bool dup = false;
        for (uint32_t k = 0; k < n; k++)
        {
            if (eq_(out[k], p->e[i].target)) { dup = true; break; }
        }
        if (!dup) { memcpy(out[n++], p->e[i].target, 6); }
    }
    return n;
}

uint32_t umac_mesh_pending_ask_due(const struct umac_mesh_pending *p, uint32_t now_ms,
                                   uint8_t (*out)[6], uint32_t max)
{
    uint32_t n = 0;
    if (p == NULL || out == NULL) { return 0; }
    for (uint32_t i = 0; i < UMAC_MESH_PENDING_SLOTS && n < max; i++)
    {
        if (!pend_live_(p, i, now_ms) || !p->e[i].relayed || p->e[i].asks > UMAC_MESH_RELAY_PREQ_RETRIES ||
            (uint32_t)(now_ms - p->e[i].start_ms) < umac_mesh_relay_ask_ms(p->e[i].asks))
        {
            continue;
        }
        bool dup = false;
        for (uint32_t k = 0; k < n; k++)
        {
            if (eq_(out[k], p->e[i].target)) { dup = true; break; }
        }
        if (!dup) { memcpy(out[n++], p->e[i].target, 6); }
    }
    return n;
}

void umac_mesh_pending_asked(struct umac_mesh_pending *p, const uint8_t *target, uint32_t sent_ms)
{
    if (p == NULL || target == NULL) { return; }
    for (uint32_t i = 0; i < UMAC_MESH_PENDING_SLOTS; i++)
    {
        /* A step takes a PREQ sent at or after it fell due, and each PREQ one step. */
        uint32_t due = p->e[i].start_ms + umac_mesh_relay_ask_ms(p->e[i].asks);
        if (pend_live_(p, i, sent_ms) && p->e[i].relayed && eq_(p->e[i].target, target) &&
            p->e[i].asks <= UMAC_MESH_RELAY_PREQ_RETRIES && (int32_t)(sent_ms - due) >= 0 &&
            (p->e[i].asks == 0 || (int32_t)(sent_ms - p->e[i].asked_ms) > 0))
        {
            p->e[i].asks++;
            p->e[i].asked_ms = sent_ms;
        }
    }
}

bool umac_mesh_pending_next_ms(const struct umac_mesh_pending *p, uint32_t now_ms, uint32_t *delta_ms)
{
    bool any = false;
    uint32_t best = UINT32_MAX;
    if (p == NULL) { return false; }
    for (uint32_t i = 0; i < UMAC_MESH_PENDING_SLOTS; i++)
    {
        if (!p->e[i].used || !p->e[i].relayed) { continue; }
        any = true;
        uint32_t d = pend_past_(now_ms, p->e[i].exp_ms) ? 0u : p->e[i].exp_ms - now_ms;
        if (d != 0u && p->e[i].asks <= UMAC_MESH_RELAY_PREQ_RETRIES)
        {
            uint32_t due = p->e[i].start_ms + umac_mesh_relay_ask_ms(p->e[i].asks);
            uint32_t dd = ((int32_t)(due - now_ms) > 0) ? due - now_ms : 0u;
            if (dd < d) { d = dd; }
        }
        if (d < best) { best = d; }
    }
    if (any && delta_ms != NULL) { *delta_ms = best; }
    return any;
}

uint32_t umac_mesh_pending_count(const struct umac_mesh_pending *p)
{
    uint32_t n = 0;
    if (p == NULL) { return 0; }
    for (uint32_t i = 0; i < UMAC_MESH_PENDING_SLOTS; i++) { n += p->e[i].used ? 1u : 0u; }
    return n;
}

uint32_t umac_mesh_pending_count_relayed(const struct umac_mesh_pending *p)
{
    uint32_t n = 0;
    if (p == NULL) { return 0; }
    for (uint32_t i = 0; i < UMAC_MESH_PENDING_SLOTS; i++) { n += (p->e[i].used && p->e[i].relayed) ? 1u : 0u; }
    return n;
}
