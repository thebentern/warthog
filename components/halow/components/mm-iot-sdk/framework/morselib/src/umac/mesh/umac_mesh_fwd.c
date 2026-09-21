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
        if (ae == UMAC_MESH_CTRL_AE_A4)
        {
            umac_mesh_proxy_learn(c->tbl, f->mc.eaddr1, mesh_sa, c->now_ms);
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
    if (ae == UMAC_MESH_CTRL_AE_A5A6)
    {
        umac_mesh_proxy_learn(c->tbl, f->mc.eaddr2, mesh_sa, c->now_ms);
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
    if (next == NULL)
    {
        /* mac80211: PERR back to the transmitter, sn 0, reason no-forward. */
        drop_(r, UMAC_MESH_FWD_DROP_NO_PATH);
        r->perr_len = umac_mesh_hwmp_build_perr(r->perr_body, sizeof(r->perr_body),
                                                c->element_ttl, mesh_da, 0u,
                                                HWMP_REASON_MESH_PATH_ERROR_NO_FORWARDING);
        if (r->perr_len != 0)
        {
            r->send_perr = true;
            memcpy(r->perr_to, f->addr2, 6);
        }
        return;
    }
    r->verdict = UMAC_MESH_FWD_FORWARD;
    memcpy(r->fwd_ra, next, 6);
    r->fwd_mc = f->mc;
    r->fwd_mc.ttl = (uint8_t)(f->mc.ttl - 1u);
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

    /* Individually addressed. Is the 802.3 destination a host behind some
     * mesh node? Then the mesh DA is that node and both ends ride in AE 2. */
    const uint8_t *proxy = umac_mesh_proxy_lookup(c->tbl, da, c->now_ms);
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

    const struct umac_mesh_path *p = umac_mesh_path_lookup(c->tbl, mesh_da, c->now_ms);
    if (p != NULL)
    {
        memcpy(r->ra, p->next_hop, 6);
        r->ok = true;
        return;
    }
    if (c->is_peer != NULL && c->is_peer(mesh_da, c->is_peer_arg))
    {
        memcpy(r->ra, mesh_da, 6);
        r->ok = true;
        return;
    }
    r->ok = false;
    r->need_path = true;
    memcpy(r->path_target, mesh_da, 6);
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
    memcpy(g->t[slot].target, target, 6);
    g->t[slot].last_ms = now_ms;
    g->t[slot].used = true;
    g->any_ms = now_ms;
    g->any_used = true;
    return true;
}
