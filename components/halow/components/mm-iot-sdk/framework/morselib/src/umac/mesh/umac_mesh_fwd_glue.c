/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 */
#include "umac/mesh/umac_mesh_fwd_glue.h"

#include "dot11/dot11.h"
#include "dot11/dot11_utils.h"
#include "mmdrv.h"
#include "mmosal.h"
#include "mmpkt.h"
#include "umac/data/umac_data.h"
#include "umac/datapath/umac_datapath.h"
#include "umac/datapath/umac_datapath_private.h"
#include "umac/datapath/umac_datapath_data.h"
#include "umac/mesh/umac_mesh_fwd.h"
#include "umac/mesh/umac_mesh_hwmp_relay.h"
#include "umac/core/umac_core.h"

#include <stdio.h>
#include <string.h>

/* Set from NVS at boot (main/mesh.c); defined beside the other mesh gates. */
extern volatile uint32_t g_warthog_mesh_fwd, g_warthog_mesh_bridge;

/* Counters, read by AT+MESHFWDSTAT?. Defined app-side in main/at.c like every
 * other g_warthog_* -- morselib symbols are prefixed at archive time, so a
 * definition here would be invisible to the app. */
extern volatile uint32_t g_warthog_fwd_uni, g_warthog_fwd_grp, g_warthog_fwd_nomem;
extern volatile uint32_t g_warthog_fwd_drop_own, g_warthog_fwd_drop_dup, g_warthog_fwd_drop_ttl;
extern volatile uint32_t g_warthog_fwd_drop_nopath, g_warthog_fwd_drop_nofwd, g_warthog_fwd_drop_bad;
extern volatile uint32_t g_warthog_fwd_perr_tx, g_warthog_fwd_preq_tx;
extern volatile uint32_t g_warthog_hwmp_relay_preq, g_warthog_hwmp_relay_prep, g_warthog_hwmp_relay_perr;

static struct umac_mesh_pathtbl s_tbl;
static struct umac_mesh_rmc s_rmc;
static struct mmosal_mutex *s_lock;
/* Discovery and error rate limits: the tested gate in the engine, twice. */
static struct umac_mesh_preq_gate s_preq_gate;
static struct umac_mesh_preq_gate s_perr_gate;
static uint32_t s_hwmp_preq_id;
static struct umac_mesh_prot_latch s_prot;
static struct umac_mesh_pending s_pend;
/* Set by the first held frame; the flush needs the datapath it came from. */
static struct umac_data *s_pend_umacd;
extern volatile uint32_t g_warthog_fwd_pend_tx, g_warthog_fwd_pend_drop, g_warthog_hwmp_prot;
extern volatile uint32_t g_warthog_fwd_perr_suppressed, g_warthog_fwd_drop_full;
extern volatile uint32_t g_warthog_hwmp_unprotected, g_warthog_hwmp_mmie, g_warthog_hwmp_nommie;
/* Forwarded frames queued to one peer beyond this are dropped: the pool is
 * shared with our own traffic and peering, and a relay must not starve them. */
#ifndef UMAC_MESH_FWD_QUEUE_CAP
#define UMAC_MESH_FWD_QUEUE_CAP 8u
#endif

/* Hop cost. mac80211's airtime metric for one S1G hop lands in the low
 * thousands; a constant keeps route choice sane without a rate feed. */
#ifndef UMAC_MESH_FWD_HOP_METRIC
#define UMAC_MESH_FWD_HOP_METRIC 4096u
#endif

extern struct umac_sta_data *umac_datapath_mesh_find_peer(const uint8_t *addr);
extern int umac_mesh_tx_action(const uint8_t *da, const uint8_t *body, uint16_t body_len);
extern const uint8_t *umac_mesh_own_addr(void);
extern uint32_t *umac_mesh_hwmp_own_sn_ptr(void);
extern uint8_t umac_mesh_ies_cap_forwarding;

static bool is_peer_(const uint8_t *addr, void *arg)
{
    (void)arg;
    return umac_datapath_mesh_find_peer(addr) != NULL;
}

static void lock_(void)   { if (s_lock != NULL) { (void)mmosal_mutex_get(s_lock, UINT32_MAX); } }
static void unlock_(void) { if (s_lock != NULL) { (void)mmosal_mutex_release(s_lock); } }

static struct umac_mesh_fwd_ctx fctx_(void)
{
    struct umac_mesh_fwd_ctx c = {
        .own_addr = umac_mesh_own_addr(), .tbl = &s_tbl, .rmc = &s_rmc,
        .forwarding = g_warthog_mesh_fwd != 0, .element_ttl = UMAC_MESH_CTRL_TTL_DEFAULT,
        .now_ms = mmosal_get_time_ms(), .is_peer = is_peer_, .is_peer_arg = NULL,
    };
    return c;
}

void umac_mesh_fwd_glue_init(void)
{
    if (s_lock == NULL)
    {
        s_lock = mmosal_mutex_create("mesh_fwd");
    }
    lock_();
    umac_mesh_pathtbl_init(&s_tbl);
    umac_mesh_rmc_init(&s_rmc);
    umac_mesh_preq_gate_init(&s_preq_gate);
    umac_mesh_preq_gate_init(&s_perr_gate);
    umac_mesh_prot_latch_init(&s_prot);
    umac_mesh_pending_init(&s_pend);
    unlock_();
    umac_mesh_ies_cap_forwarding = g_warthog_mesh_fwd ? 1u : 0u;
}

void umac_mesh_fwd_glue_rx(struct umac_data *umacd, struct umac_sta_data *stad,
                           const struct dot11_hdr *hdr, const struct dot11_data_hdr *dhdr,
                           const struct umac_mesh_ctrl *mc,
                           struct umac_mesh_fwd_rx_result *out)
{
    (void)umacd; (void)stad;
    struct umac_mesh_rx_frame f;
    memset(&f, 0, sizeof(f));
    f.group = mm_mac_addr_is_multicast(dot11_get_ra(hdr));
    memcpy(f.addr1, dot11_get_ra(hdr), 6);
    memcpy(f.addr2, dot11_get_ta(hdr), 6);
    if (f.group)
    {
        /* 3-address: addr3 is the mesh SA. */
        memcpy(f.addr3, hdr->addr3, 6);
    }
    else
    {
        memcpy(f.addr3, dot11_get_da(hdr), 6);
        memcpy(f.addr4, dot11_get_sa_data(dhdr), 6);
    }
    f.mc = *mc;
    /* A group frame a warthog replicated as unicast is recognised and
     * rewritten by the tested engine, not here. */
    (void)umac_mesh_fwd_normalise_replica(&f);
    struct umac_mesh_fwd_ctx c = fctx_();
    lock_();
    umac_mesh_fwd_rx(&c, &f, out);
    unlock_();
    switch (out->drop)
    {
        case UMAC_MESH_FWD_DROP_OWN:    g_warthog_fwd_drop_own++; break;
        case UMAC_MESH_FWD_DROP_DUP:    g_warthog_fwd_drop_dup++; break;
        case UMAC_MESH_FWD_DROP_TTL:    if (out->verdict == UMAC_MESH_FWD_DROP) { g_warthog_fwd_drop_ttl++; } break;
        case UMAC_MESH_FWD_DROP_NO_PATH:g_warthog_fwd_drop_nopath++; break;
        case UMAC_MESH_FWD_DROP_NO_FWD: if (out->verdict == UMAC_MESH_FWD_DROP) { g_warthog_fwd_drop_nofwd++; } break;
        case UMAC_MESH_FWD_DROP_BAD_AE:
        case UMAC_MESH_FWD_DROP_NOT_FOR_US: g_warthog_fwd_drop_bad++; break;
        default: break;
    }
}

void umac_mesh_fwd_glue_send_perr(const struct umac_mesh_fwd_rx_result *r)
{
    if (r == NULL || !r->send_perr)
    {
        return;
    }
    /* One PERR per unroutable destination per interval, and a floor overall:
     * a stream of frames for random destinations must not become a stream of
     * management allocations and transmissions (mac80211: perrMinInterval).
     * Under the lock like every other gate, so it stays correct if a second
     * task ever reaches this path. */
    lock_();
    bool allow = umac_mesh_preq_gate_allow(&s_perr_gate, r->mesh_da, mmosal_get_time_ms());
    unlock_();
    if (!allow)
    {
        g_warthog_fwd_perr_suppressed++;
        return;
    }
    if (umac_mesh_tx_action(r->perr_to, r->perr_body, r->perr_len) >= 0)
    {
        g_warthog_fwd_perr_tx++;
    }
}

void umac_mesh_fwd_glue_forward(struct umac_data *umacd, struct mmpktview *body,
                                uint16_t ethertype, const struct dot11_hdr *hdr,
                                const struct dot11_data_hdr *dhdr,
                                const struct umac_mesh_fwd_rx_result *r)
{
    struct umac_datapath_data *data = umac_data_get_datapath(umacd);
    bool group = mm_mac_addr_is_multicast(r->fwd_ra);
    uint32_t len = mmpkt_get_data_length(body);
    struct mmpkt *copy = umac_datapath_alloc_mmpkt_for_qos_data_tx(len + sizeof(struct umac_8023_hdr),
                                                                   MMDRV_PKT_CLASS_DATA_TID0);
    if (copy == NULL)
    {
        g_warthog_fwd_nomem++;
        return;
    }
    /* Rebuild as the 802.3 frame the TX entry expects. The mesh endpoints are
     * the ENGINE's view -- on a replica addr3 is the previous hop, not the
     * source, and the duplicate cache downstream is keyed on the source. */
    (void)dhdr;
    const uint8_t *mesh_sa = r->mesh_sa;
    const uint8_t *mesh_da = r->mesh_da;
    struct umac_8023_hdr h8023;
    memcpy(h8023.dest_addr, mesh_da, 6);
    memcpy(h8023.src_addr, mesh_sa, 6);
    h8023.ethertype_be = htobe16(ethertype);
    struct mmpktview *v = mmpkt_open(copy);
    mmpkt_append_data(v, (const uint8_t *)&h8023, sizeof(h8023));
    mmpkt_append_data(v, mmpkt_get_data_start(body), len);
    mmpkt_close(&v);

    struct mmdrv_tx_metadata *md = mmdrv_get_tx_metadata(copy);
    memset(&md->mesh, 0, sizeof(md->mesh));
    md->enc = ENCRYPTION_ENABLED;
    md->tid = 0;
    struct umac_mesh_ctrl mc = r->fwd_mc;
    extern volatile uint32_t g_warthog_mesh_grp;
    if (group && g_warthog_mesh_grp)
    {
        /* Standard group frame: the native Mesh Control goes out once on a
         * 3-address frame; the sender drops its own rebroadcast as a
         * duplicate. AE 2 here would be refused by every receiver. */
    }
    else if (group)
    {
        /* Replicated as unicast per peer: the group DA and the source ride
         * in AE 2 so a mac80211 receiver rebuilds the real Ethernet frame. */
        const uint8_t *src = umac_mesh_ctrl_ae(&r->fwd_mc) == UMAC_MESH_CTRL_AE_A4
                                 ? r->fwd_mc.eaddr1 : mesh_sa;
        umac_mesh_fwd_replica_ctrl(&r->fwd_mc, mesh_da, src, &mc);
        md->mesh.exclude_valid = 1;
        memcpy(md->mesh.exclude_ta, dot11_get_ta(hdr), 6);
    }
    md->mesh.mc_len = (uint8_t)umac_mesh_ctrl_build(md->mesh.mc, sizeof(md->mesh.mc), &mc);
    md->mesh.addr_valid = 1;
    memcpy(md->mesh.mesh_da, mesh_da, 6);
    memcpy(md->mesh.mesh_sa, mesh_sa, 6);

    struct umac_sta_data *stad = NULL;
    if (group)
    {
        /* The original replica must not go back to whoever sent it; the
         * fan-out in mesh_enqueue_tx_frame excludes the sender for the copies.
         * A standard group frame is one broadcast and any peer's queue will do. */
        extern struct umac_sta_data *umac_datapath_mesh_first_peer_except(const uint8_t *excl);
        stad = umac_datapath_mesh_first_peer_except(g_warthog_mesh_grp ? NULL : dot11_get_ta(hdr));
    }
    else
    {
        stad = umac_datapath_mesh_find_peer(r->fwd_ra);
    }
    if (stad == NULL)
    {
        mmpkt_release(copy);
        g_warthog_fwd_drop_nopath++;
        return;
    }
    if (umac_sta_data_get_queued_len(stad) >= UMAC_MESH_FWD_QUEUE_CAP)
    {
        mmpkt_release(copy);
        g_warthog_fwd_drop_full++;
        return;
    }
    if (group) { g_warthog_fwd_grp++; } else { g_warthog_fwd_uni++; }
    data->ops->enqueue_tx_frame(umacd, stad, copy);
}

static void maybe_preq_(const uint8_t *target)
{
    /* Reached from the netif task (TX) and the event loop (flush); the gate,
     * our HWMP sequence number and the PREQ id are shared with the relay, so
     * the decision and the body are made under the lock, the send outside. */
    uint8_t body[HWMP_PREQ_BODY_LEN], ra[6];
    uint16_t n = 0;
    lock_();
    if (umac_mesh_preq_gate_allow(&s_preq_gate, target, mmosal_get_time_ms()))
    {
        /* Broadcast, through the engine: the target is not a neighbour, so a
         * unicast PREQ to it would reach nobody. */
        n = umac_mesh_fwd_originate_preq(body, sizeof(body), umac_mesh_own_addr(),
                                         umac_mesh_hwmp_own_sn_ptr(), &s_hwmp_preq_id, target,
                                         4882u, ra);
    }
    unlock_();
    if (n != 0 && umac_mesh_tx_action(ra, body, n) >= 0)
    {
        g_warthog_fwd_preq_tx++;
    }
}

void umac_mesh_fwd_glue_lock(void) { lock_(); }
void umac_mesh_fwd_glue_unlock(void) { unlock_(); }

uint32_t umac_mesh_fwd_glue_next_seq(void)
{
    /* The one allocator. Read-and-increment is not atomic, and three tasks
     * originate mesh frames -- the netif task, the receive path's flush and
     * the service tick. Two frames sharing a sequence number are dropped as
     * duplicates by the first relay's cache, so this is silent data loss. */
    extern volatile uint32_t g_warthog_mesh_seq;
    lock_();
    uint32_t seq = g_warthog_mesh_seq++;
    unlock_();
    return seq;
}

/* An 802.3 frame from the TX entry, now that @p ra is its next hop: queued
 * the way a fresh TX would be, classified first. @returns false (frame not
 * taken) when @p ra is not a peer. Same peer-record lifetime as every other
 * enqueue in this file: del_peer on another task can free the record
 * between the lookup and the queue -- a shape the SDK's own TX path shares. */
static bool send_now_(struct umac_data *umacd, struct mmpkt *pkt, const uint8_t *ra)
{
    struct umac_sta_data *stad = umac_datapath_mesh_find_peer(ra);
    if (stad == NULL)
    {
        return false;
    }
    uint8_t da[6], sa[6];
    struct mmpktview *v = mmpkt_open(pkt);
    const struct umac_8023_hdr *h = (const struct umac_8023_hdr *)mmpkt_get_data_start(v);
    memcpy(da, h->dest_addr, 6);
    memcpy(sa, h->src_addr, 6);
    mmpkt_close(&v);
    struct mmdrv_tx_metadata *md = mmdrv_get_tx_metadata(pkt);
    md->enc = ENCRYPTION_ENABLED;
    memset(&md->mesh, 0, sizeof(md->mesh));
    umac_mesh_fwd_glue_tx_classify(pkt, da, sa);
    struct umac_datapath_data *data = umac_data_get_datapath(umacd);
    data->ops->enqueue_tx_frame(umacd, stad, pkt);
    umac_core_evt_wake(umacd);
    return true;
}

/* Frames held for discovery whose path now exists go out; expired ones are
 * dropped. Called after every path-selection frame and from the 2 s service
 * tick, so an unanswered discovery still gives its buffers back on time. */
static void flush_pending_(void)
{
    struct umac_mesh_fwd_ctx c = fctx_();
    struct umac_mesh_pending_out out[UMAC_MESH_PENDING_MAX];
    lock_();
    struct umac_data *umacd = s_pend_umacd;
    uint32_t n = (umacd != NULL) ? umac_mesh_pending_take(&s_pend, &c, out, UMAC_MESH_PENDING_MAX) : 0;
    unlock_();
    for (uint32_t i = 0; i < n; i++)
    {
        struct mmpkt *pkt = (struct mmpkt *)out[i].handle;
        if (out[i].ok && send_now_(umacd, pkt, out[i].ra))
        {
            g_warthog_fwd_pend_tx++;
            continue;
        }
        mmpkt_release(pkt);
        g_warthog_fwd_pend_drop++;
    }
    /* Anything still waiting gets its PREQ re-asked: the first one is a single
     * unacknowledged broadcast, so losing it must not cost the frame. The gate
     * decides whether one actually goes out. */
    uint8_t again[UMAC_MESH_PENDING_MAX][6];
    lock_();
    uint32_t m = umac_mesh_pending_targets(&s_pend, c.now_ms, again, UMAC_MESH_PENDING_MAX);
    unlock_();
    for (uint32_t i = 0; i < m; i++)
    {
        maybe_preq_(again[i]);
    }
}

void umac_mesh_fwd_glue_tick(void)
{
    flush_pending_();
}

void umac_mesh_fwd_glue_learn_proxy(const uint8_t *ext, const uint8_t *mesh_sa)
{
    struct umac_mesh_fwd_ctx c = fctx_();
    lock_();
    (void)umac_mesh_fwd_learn_proxy(&c, ext, mesh_sa);
    unlock_();
}

bool umac_mesh_fwd_glue_proxy_via_peer(const uint8_t *da, uint8_t out[6])
{
    struct umac_mesh_fwd_ctx c = fctx_();
    lock_();
    bool ok = umac_mesh_fwd_proxy_via_peer(&c, da, out);
    unlock_();
    return ok;
}

bool umac_mesh_fwd_glue_next_hop(const uint8_t *dest, uint8_t out[6])
{
    struct umac_mesh_fwd_ctx c = fctx_();
    struct umac_mesh_fwd_tx_result t;
    lock_();
    umac_mesh_fwd_tx(&c, dest, c.own_addr, 0, &t);
    unlock_();
    if (t.ok && t.shape == UMAC_MESH_TX_UNICAST_4ADDR)
    {
        memcpy(out, t.ra, 6);
        if (t.refresh) { maybe_preq_(t.path_target); }
        return true;
    }
    if (t.need_path)
    {
        maybe_preq_(t.path_target);
    }
    return false;
}

void umac_mesh_fwd_glue_tx_classify(struct mmpkt *txbuf, const uint8_t *da, const uint8_t *sa)
{
    struct umac_mesh_fwd_ctx c = fctx_();
    struct umac_mesh_fwd_tx_result t;
    /* One sequence number per frame, allocated once. Burning one on a frame
     * that turns out to need no Mesh Control sidecar is what mac80211 does
     * too, and is cheaper than reading and incrementing separately. */
    uint32_t seq = umac_mesh_fwd_glue_next_seq();
    lock_();
    umac_mesh_fwd_tx(&c, da, sa, seq, &t);
    unlock_();
    struct mmdrv_tx_metadata *md = mmdrv_get_tx_metadata(txbuf);
    /* A leaf reaches here only for a learned proxied host; if its peer went since
     * the gate, it still must not originate a PREQ. */
    if ((t.need_path || t.refresh) && (g_warthog_mesh_fwd || g_warthog_mesh_bridge))
    {
        maybe_preq_(t.path_target);
    }
    extern volatile uint32_t g_warthog_mesh_grp;
    if (t.shape == UMAC_MESH_TX_GROUP_3ADDR && g_warthog_mesh_grp)
    {
        /* Standard group frame: the engine's own Mesh Control (AE 1 carries a
         * proxied source, else no extension) goes out as-is. */
        md->mesh.mc_len = (uint8_t)umac_mesh_ctrl_build(md->mesh.mc, sizeof(md->mesh.mc), &t.mc);
        md->mesh.addr_valid = 1;
        memcpy(md->mesh.mesh_da, da, 6);
        memcpy(md->mesh.mesh_sa, c.own_addr, 6);
        return;
    }
    if (t.shape == UMAC_MESH_TX_GROUP_3ADDR)
    {
        /* Group frames still go out as one unicast per peer on this chip. So
         * the receiver can tell a broadcast from a unicast to itself -- and a
         * second warthog can re-flood it -- the group DA and the source ride
         * in AE 2, the form the receive side recognises as a group replica.
         * Without it a warthog's own broadcasts stop at the first hop, and a
         * bridged host's went out as AE 1 on a 4-address frame, a shape
         * nobody accepts. */
        struct umac_mesh_ctrl mc;
        umac_mesh_fwd_replica_ctrl(&t.mc, da, sa, &mc);
        md->mesh.mc_len = (uint8_t)umac_mesh_ctrl_build(md->mesh.mc, sizeof(md->mesh.mc), &mc);
        md->mesh.addr_valid = 1;
        memcpy(md->mesh.mesh_da, da, 6);
        memcpy(md->mesh.mesh_sa, c.own_addr, 6);
        return;
    }
    /* Unicast: only a proxied frame needs the sidecar. */
    if (umac_mesh_ctrl_ae(&t.mc) != UMAC_MESH_CTRL_AE_NONE)
    {
        md->mesh.mc_len = (uint8_t)umac_mesh_ctrl_build(md->mesh.mc, sizeof(md->mesh.mc), &t.mc);
        md->mesh.addr_valid = 1;
        memcpy(md->mesh.mesh_da, t.addr3, 6);
        memcpy(md->mesh.mesh_sa, c.own_addr, 6);
    }
}

void umac_mesh_fwd_glue_hwmp_rx(const uint8_t *body, uint16_t len, const uint8_t *ta,
                                uint32_t *own_sn, bool is_protected, bool is_group_addressed)
{
    extern volatile uint32_t g_warthog_mesh_secure;
    if (is_protected) { g_warthog_hwmp_prot++; }
    /* Unicast path selection: trust on first protected frame (see the latch
     * in umac_mesh_fwd.h). Group-addressed ones would be BIP-protected with
     * a trailing MMIE the chip may or may not hand up -- counted, so the
     * bench can say what arrives before anything is gated on it. */
    if (!is_group_addressed)
    {
        lock_();
        bool ok = umac_mesh_prot_latch_check(&s_prot, ta, is_protected);
        unlock_();
        if (!ok)
        {
            g_warthog_hwmp_unprotected++;
            return;
        }
    }
    else if (g_warthog_mesh_secure)
    {
        /* MMIE: element 76, length 16, at the very end of the body. */
        if (len >= 18u && body[len - 18u] == 76u && body[len - 17u] == 16u) { g_warthog_hwmp_mmie++; }
        else { g_warthog_hwmp_nommie++; }
    }
    struct umac_mesh_hwmp_ctx c = {
        .own_addr = umac_mesh_own_addr(), .tbl = &s_tbl, .forwarding = g_warthog_mesh_fwd != 0,
        .link_metric = UMAC_MESH_FWD_HOP_METRIC, .path_lifetime_ms = UMAC_MESH_PATH_LIFETIME_MS,
        .now_ms = mmosal_get_time_ms(), .own_sn = own_sn,
    };
    /* mac80211 processes path selection only from an ESTAB peer; anyone else
     * on the channel could otherwise poison the table or trigger PERRs. */
    if (umac_datapath_mesh_find_peer(ta) == NULL)
    {
        g_warthog_fwd_drop_bad++;
        return;
    }
    struct umac_mesh_hwmp_action a; enum umac_mesh_hwmp_drop why;
    lock_();
    (void)umac_mesh_hwmp_relay(&c, body, len, ta, &a, &why);
    unlock_();
    switch (a.kind)
    {
        case UMAC_MESH_HWMP_SEND_PREP:        (void)umac_mesh_tx_action(a.to, a.body, a.body_len); break;
        case UMAC_MESH_HWMP_REBROADCAST_PREQ: g_warthog_hwmp_relay_preq++; (void)umac_mesh_tx_action(a.to, a.body, a.body_len); break;
        case UMAC_MESH_HWMP_FORWARD_PREP:     g_warthog_hwmp_relay_prep++; (void)umac_mesh_tx_action(a.to, a.body, a.body_len); break;
        case UMAC_MESH_HWMP_FORWARD_PERR:     g_warthog_hwmp_relay_perr++; (void)umac_mesh_tx_action(a.to, a.body, a.body_len); break;
        default: break;
    }
    flush_pending_();
}

bool umac_mesh_fwd_glue_tx_pending(struct umac_data *umacd, struct mmpkt *txbuf, const uint8_t *dest)
{
    struct umac_mesh_fwd_ctx c = fctx_();
    struct umac_mesh_fwd_tx_result t;
    void *evicted = NULL;
    bool held = false;
    lock_();
    s_pend_umacd = umacd; /* before the push is visible to a flush */
    umac_mesh_fwd_tx(&c, dest, c.own_addr, 0, &t);
    if (!t.ok && t.need_path)
    {
        evicted = umac_mesh_pending_push(&s_pend, t.path_target, txbuf, c.now_ms);
        held = true;
    }
    unlock_();
    if (evicted != NULL)
    {
        mmpkt_release((struct mmpkt *)evicted);
        g_warthog_fwd_pend_drop++;
    }
    if (held)
    {
        return true;
    }
    /* The PREP beat us here: the lookup that sent the PREQ found no peer,
     * the event loop installed the path meanwhile. Send, do not drop. */
    if (t.ok && t.shape == UMAC_MESH_TX_UNICAST_4ADDR && send_now_(umacd, txbuf, t.ra))
    {
        g_warthog_fwd_pend_tx++;
        return true;
    }
    return false;
}

void umac_mesh_fwd_glue_peer_lost(const uint8_t *peer)
{
    struct umac_mesh_hwmp_ctx c = {
        .own_addr = umac_mesh_own_addr(), .tbl = &s_tbl, .forwarding = g_warthog_mesh_fwd != 0,
        .link_metric = UMAC_MESH_FWD_HOP_METRIC, .path_lifetime_ms = UMAC_MESH_PATH_LIFETIME_MS,
        .now_ms = mmosal_get_time_ms(), .own_sn = NULL,
    };
    struct umac_mesh_hwmp_action acts[4];
    lock_();
    umac_mesh_prot_latch_forget(&s_prot, peer);
    uint32_t n = umac_mesh_hwmp_lose_neighbour(&c, peer, acts, 4);
    unlock_();
    for (uint32_t i = 0; i < n; i++)
    {
        if (umac_mesh_tx_action(acts[i].to, acts[i].body, acts[i].body_len) >= 0)
        {
            g_warthog_fwd_perr_tx++;
        }
    }
}

int umac_mesh_fwd_glue_render(char *buf, uint32_t len)
{
    uint32_t now = mmosal_get_time_ms();
    int w = 0;
    lock_();
    w += snprintf(buf + w, len - (uint32_t)w, "+MESHPATH: paths=%lu proxies=%lu rmc=%lu rmc_evict=%lu pending=%lu\r\n",
                  (unsigned long)umac_mesh_path_count(&s_tbl, now),
                  (unsigned long)umac_mesh_proxy_count(&s_tbl, now),
                  (unsigned long)umac_mesh_rmc_count(&s_rmc, now),
                  (unsigned long)s_rmc.evictions,
                  (unsigned long)umac_mesh_pending_count(&s_pend));
    for (uint32_t i = 0; i < UMAC_MESH_PATH_MAX && w < (int)len - 96; i++)
    {
        const struct umac_mesh_path *p = &s_tbl.p[i];
        if (!p->used) { continue; }
        int32_t left = (int32_t)(p->exp_ms - now);
        w += snprintf(buf + w, len - (uint32_t)w,
                      "+MESHPATH: dst=%02x%02x%02x via=%02x%02x%02x sn=%lu metric=%lu hops=%u %s ttl=%lds\r\n",
                      p->dst[3], p->dst[4], p->dst[5], p->next_hop[3], p->next_hop[4], p->next_hop[5],
                      (unsigned long)p->sn, (unsigned long)p->metric, p->hop_count,
                      (p->flags & UMAC_MESH_PATH_ACTIVE) ? "active" : "dead",
                      (long)(left > 0 ? left / 1000 : 0));
    }
    for (uint32_t i = 0; i < UMAC_MESH_PROXY_MAX && w < (int)len - 64; i++)
    {
        const struct umac_mesh_proxy *x = &s_tbl.x[i];
        if (!x->used || (int32_t)(now - x->exp_ms) >= 0) { continue; }
        w += snprintf(buf + w, len - (uint32_t)w, "+MESHPATH: host=%02x%02x%02x behind=%02x%02x%02x\r\n",
                      x->ext[3], x->ext[4], x->ext[5], x->mesh_sta[3], x->mesh_sta[4], x->mesh_sta[5]);
    }
    unlock_();
    return w;
}
