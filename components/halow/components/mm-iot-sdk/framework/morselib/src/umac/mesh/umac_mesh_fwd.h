/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * Mesh data-plane decisions: for a received mesh data frame, deliver it,
 * forward it, both, or drop it; for a frame we are about to send, which
 * addresses, which Address Extension and which next hop. The rules are
 * mac80211's ieee80211_rx_h_mesh_fwding() and the mesh case of
 * ieee80211_build_hdr(). Decides only; moves no packets.
 *
 * Freestanding (libc only): the simulator and the firmware run this code.
 */
#pragma once

#include "umac_mesh_ctrl.h"
#include "umac_mesh_hwmp.h"
#include "umac_mesh_ies.h"
#include "umac_mesh_pathtbl.h"
#include "umac_mesh_rmc.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Is @p addr an established direct peer? Lets a neighbour be a next hop
 *  before any HWMP path to it exists. */
typedef bool (*umac_mesh_fwd_is_peer_fn)(const uint8_t *addr, void *arg);

struct umac_mesh_fwd_ctx {
    const uint8_t *own_addr;
    struct umac_mesh_pathtbl *tbl;
    struct umac_mesh_rmc *rmc;
    bool forwarding;              /* AT+MESHFWD */
    uint8_t element_ttl;          /* TTL for frames and PERRs we originate */
    uint32_t now_ms;
    umac_mesh_fwd_is_peer_fn is_peer;
    void *is_peer_arg;
};

/* ---- receive ---------------------------------------------------------- */

/** A received mesh data frame, already decrypted, Mesh Control parsed. */
struct umac_mesh_rx_frame {
    bool group;          /* addr1 is group-addressed (3-address form) */
    uint8_t addr1[6];    /* RA, or the group DA */
    uint8_t addr2[6];    /* TA */
    uint8_t addr3[6];    /* mesh DA (4-addr) or mesh SA (group) */
    uint8_t addr4[6];    /* mesh SA (4-addr only) */
    struct umac_mesh_ctrl mc;
};

enum umac_mesh_fwd_verdict {
    UMAC_MESH_FWD_DROP = 0,
    UMAC_MESH_FWD_DELIVER,
    UMAC_MESH_FWD_FORWARD,
    UMAC_MESH_FWD_DELIVER_AND_FORWARD,
    /* Relay with no next hop: hold the copy (fwd_mc, mesh_da/sa) and PREQ for mesh_da. */
    UMAC_MESH_FWD_HOLD,
};

enum umac_mesh_fwd_drop {
    UMAC_MESH_FWD_DROP_NONE = 0,
    UMAC_MESH_FWD_DROP_OWN,       /* our own frame came back */
    UMAC_MESH_FWD_DROP_DUP,       /* recent multicast cache hit */
    UMAC_MESH_FWD_DROP_NOT_FOR_US,/* unicast, RA not us -- chip should not deliver these */
    UMAC_MESH_FWD_DROP_NO_FWD,    /* would forward, forwarding disabled */
    UMAC_MESH_FWD_DROP_TTL,
    UMAC_MESH_FWD_DROP_NO_PATH,   /* the reason a HOLD carries */
    UMAC_MESH_FWD_DROP_BAD_AE,    /* AE mode that makes no sense for the shape */
    UMAC_MESH_FWD_DROP_TTL0,      /* arrived with ttl 0: mac80211 drops these */
};

struct umac_mesh_fwd_rx_result {
    enum umac_mesh_fwd_verdict verdict;
    enum umac_mesh_fwd_drop drop;
    /* For DELIVER: the 802.3 endpoints, proxied ones preferred. */
    uint8_t deliver_da[6];
    uint8_t deliver_sa[6];
    /* For FORWARD: new RA and rebuilt Mesh Control (ttl - 1); addr3/addr4
     * and the body are carried unchanged. HOLD fills fwd_mc, not fwd_ra. */
    uint8_t fwd_ra[6];
    struct umac_mesh_ctrl fwd_mc;
    /* The frame's mesh endpoints as the engine understood them -- a relay
     * must carry THESE forward, not whatever the previous hop put in addr3. */
    uint8_t mesh_da[6];
    uint8_t mesh_sa[6];
};

/**
 * Leaf mode (forwarding and bridge off): learn @p f's proxied source (AE 1 on
 * a group frame, AE 2 on a unicast) as a host behind its mesh SA, reached via
 * its transmitter, which must be a peer. Refused as the relay refuses, and also
 * for the mesh SA itself, ttl 0, or a flood copy the duplicate cache has seen.
 */
bool umac_mesh_fwd_leaf_learn(const struct umac_mesh_fwd_ctx *c, const struct umac_mesh_rx_frame *f);

/** Leaf mode: @p da is a host learned in leaf mode (not a peer, not group). */
bool umac_mesh_fwd_leaf_proxied(const struct umac_mesh_fwd_ctx *c, const uint8_t *da);

/**
 * Leaf mode: the peer a learned host @p da is sent through -- its mesh node
 * if that is a peer, else the peer its traffic last arrived through. False
 * when neither is a peer any more, or @p da is not a leaf-learned host.
 */
bool umac_mesh_fwd_proxy_via_peer(const struct umac_mesh_fwd_ctx *c, const uint8_t *da,
                                  uint8_t out[6]);

void umac_mesh_fwd_rx(const struct umac_mesh_fwd_ctx *c, const struct umac_mesh_rx_frame *f,
                      struct umac_mesh_fwd_rx_result *r);

/**
 * A group frame this chip replicated as unicast arrives 4-address with AE 2
 * and a group address in the extension DA. Rewrite it in place into the
 * group frame it is: addr1 = the group, addr3 = the mesh source, AE 1 with
 * the proxied source. @returns true if it was such a replica.
 */
bool umac_mesh_fwd_normalise_replica(struct umac_mesh_rx_frame *f);

/* ---- on-air shaping: the bytes the firmware emits, testable ------------ */

/** Inputs to the MAC-header decision the SDK builder makes at dequeue time. */
struct umac_mesh_tx_hdr_in {
    const uint8_t *ra;        /* next hop: the peer this copy goes to */
    const uint8_t *own;       /* TA, and mesh SA absent a sidecar */
    const uint8_t *dst8023;   /* 802.3 destination */
    const uint8_t *src8023;   /* 802.3 source */
    bool sidecar_valid;       /* relayed or proxied: mesh_da/mesh_sa apply */
    const uint8_t *mesh_da;
    const uint8_t *mesh_sa;
    bool grp_std;             /* standard 3-address group frames */
};

/**
 * The MAC header the firmware puts on air for a mesh data frame. Byte for
 * byte what umac_datapath_mesh's builder emits: with grp_std a multicast
 * 802.3 destination becomes a 3-address group frame whose addr3 is the
 * sidecar's mesh source when relaying, else us; otherwise 4-address,
 * addr3 the destination (the peer itself for a replicated group frame, or
 * the sidecar's mesh DA), addr4 the source (the sidecar's mesh SA when set).
 * @returns 24 or 30, or 0 on a NULL input.
 */
uint16_t umac_mesh_fwd_tx_header(const struct umac_mesh_tx_hdr_in *in,
                                 uint8_t out[UMAC_MESH_DATA_HDR4_LEN]);

/**
 * The Mesh Control a group frame carries when it is replicated as one
 * unicast per peer: @p native's ttl/seq/flags with AE 2 holding the group DA
 * and the real source, so a receiver can tell it from a unicast to itself.
 */
void umac_mesh_fwd_replica_ctrl(const struct umac_mesh_ctrl *native, const uint8_t *group_da,
                                const uint8_t *src, struct umac_mesh_ctrl *out);

/**
 * Parse a mesh data frame from its Frame Control: 3- or 4-address MAC header,
 * QoS Control with Mesh Control Present, then the Mesh Control. Refuses
 * anything else. @returns bytes consumed up to the body, or 0.
 */
uint16_t umac_mesh_fwd_parse_frame(const uint8_t *hdr, uint16_t len, struct umac_mesh_rx_frame *f);

/* ---- discovery rate limit ---------------------------------------------
 *
 * A host behind us pinging many unknown addresses must not become a PREQ
 * broadcast per packet. Per target, one PREQ per interval; across all
 * targets, a floor; the target table is a small LRU. The bound on PREQs
 * per second is therefore 1000 / floor regardless of how many targets an
 * attacker or a scanner cycles through. */
#ifndef UMAC_MESH_PREQ_TARGETS
#define UMAC_MESH_PREQ_TARGETS 8u
#endif
/* At most the relay ladder's first gap (UMAC_MESH_RELAY_DISC_FIRST_MS), or the
 * gate delays every relayed discovery's first re-ask. */
#ifndef UMAC_MESH_PREQ_MIN_INTERVAL_MS
#define UMAC_MESH_PREQ_MIN_INTERVAL_MS 400u
#endif
#ifndef UMAC_MESH_PREQ_GLOBAL_MIN_MS
#define UMAC_MESH_PREQ_GLOBAL_MIN_MS 50u
#endif
/* Evicting a slot takes a full table of later allows and re-asking its target one more,
 * each a floor apart: this many slots make the per-target interval exact for every target. */
_Static_assert((UMAC_MESH_PREQ_TARGETS + 1u) * UMAC_MESH_PREQ_GLOBAL_MIN_MS >= UMAC_MESH_PREQ_MIN_INTERVAL_MS,
               "an evicted PREQ target could be asked again inside its interval");

struct umac_mesh_preq_gate {
    struct {
        uint8_t target[6]; uint32_t last_ms; bool used;
        uint32_t sent_ms; bool sent; /* the last PREQ for it that went out */
    } t[UMAC_MESH_PREQ_TARGETS];
    uint32_t any_ms;
    bool any_used;
};

void umac_mesh_preq_gate_init(struct umac_mesh_preq_gate *g);

/** true: send a PREQ for @p target now, and it is recorded. false: suppressed. */
bool umac_mesh_preq_gate_allow(struct umac_mesh_preq_gate *g, const uint8_t *target,
                               uint32_t now_ms);

/** The PREQ allowed for @p target at @p sent_ms went out; a failed send is not reported. */
void umac_mesh_preq_gate_sent(struct umac_mesh_preq_gate *g, const uint8_t *target, uint32_t sent_ms);

/** true, with its time in @p sent_ms: the last PREQ for @p target that went out. */
bool umac_mesh_preq_gate_last(const struct umac_mesh_preq_gate *g, const uint8_t *target,
                              uint32_t *sent_ms);

/* ---- transmit --------------------------------------------------------- */

enum umac_mesh_tx_shape {
    UMAC_MESH_TX_UNICAST_4ADDR = 0,
    UMAC_MESH_TX_GROUP_3ADDR,
};

struct umac_mesh_fwd_tx_result {
    enum umac_mesh_tx_shape shape;
    bool ok;             /* false: no route -- see need_path */
    uint8_t ra[6];       /* next hop, or the group DA */
    uint8_t addr3[6];    /* mesh DA (4-addr) or mesh SA (group) */
    uint8_t addr4[6];    /* mesh SA (4-addr only) */
    struct umac_mesh_ctrl mc;   /* flags/AE/eaddrs/ttl/seq filled */
    bool need_path;      /* caller should send a PREQ for path_target */
    uint8_t path_target[6];
    /* The path is about to expire (mac80211 path_refresh_time):
     * still usable, but the caller should send a PREQ for path_target now. */
    bool refresh;
};

/** Vanilla mac80211 path_refresh_time; OpenMANET uses 10000 of a 50000 lifetime (same 1:5). */
#define UMAC_MESH_PATH_REFRESH_MS 1000u

/* ---- protection latch --------------------------------------------------
 * Trust on first protected frame. Between warthogs with PMF off no
 * path-selection frame arrives protected, so there the peer check is a
 * transmitter-address compare, not an authentication boundary (under SAE
 * umac_datapath_mesh_hwmp_rx_ok applies each peer's MFP first). The moment a
 * peer DOES protect its path selection, a plaintext frame claiming to be from
 * it is refused -- so turning PMF on makes the check real without another
 * code change, and nothing is dropped before. */
#define UMAC_MESH_PROT_LATCH_MAX 8u
struct umac_mesh_prot_latch {
    struct { uint8_t ta[6]; bool used; } e[UMAC_MESH_PROT_LATCH_MAX];
    uint32_t next; /* round-robin victim when full */
};
void umac_mesh_prot_latch_init(struct umac_mesh_prot_latch *l);
/** @returns true if a frame from @p ta with this protection may be processed. */
bool umac_mesh_prot_latch_check(struct umac_mesh_prot_latch *l, const uint8_t *ta, bool is_protected);
void umac_mesh_prot_latch_forget(struct umac_mesh_prot_latch *l, const uint8_t *ta);

/* ---- frames held for discovery -------------------------------------------
 * mac80211 queues up to 10 frames per path (OpenMANET 50) while a PREQ is out
 * and sends them when the PREP installs the path; without it the first frame
 * of every flow to a non-neighbour is lost. Bounded small: the handles are
 * TX-pool buffers shared with our own traffic and peering. */
#define UMAC_MESH_PENDING_MAX 4u
#define UMAC_MESH_PENDING_PER_TARGET 2u
/* Longer than the 2 s service tick, so a held frame gets at least one retried
 * PREQ before it lapses. mac80211 sends up to 5 PREQs behind that queue (giving
 * up at 3.1 s; OpenMANET at 6.8 s); ours is shallower because these are TX-pool
 * buffers shared with our own traffic and with peering. */
#define UMAC_MESH_PENDING_MS 3000u
/* Relayed frames held for discovery, capped apart from ours so relaying can never
 * evict our own. Pool of 20: 4 own + 4 relayed + 8 queued to one next hop leaves 4. */
#ifndef UMAC_MESH_PENDING_RELAY_MAX
#define UMAC_MESH_PENDING_RELAY_MAX 4u
#endif
#define UMAC_MESH_PENDING_SLOTS (UMAC_MESH_PENDING_MAX + UMAC_MESH_PENDING_RELAY_MAX)
/* mac80211's relay discovery (OpenMANET 999-0027): PREQs at 0, 0.4, 1.2, 2.8 and
 * 4.8 s (timeout from 2 x 200 ms, doubling, capped at 2 s), then give up at 6.8 s. */
#define UMAC_MESH_RELAY_DISC_FIRST_MS 400u
#define UMAC_MESH_RELAY_DISC_CAP_MS 2000u
#define UMAC_MESH_RELAY_PREQ_RETRIES 4u
struct umac_mesh_pending {
    struct {
        uint8_t target[6]; void *handle; uint32_t exp_ms; uint32_t order;
        uint32_t start_ms; /* relayed: when this target's discovery began */
        uint32_t asked_ms; /* relayed: when the PREQ that took the last step went out */
        uint8_t asks;      /* relayed: ladder steps taken so far */
        bool relayed; bool used;
    } e[UMAC_MESH_PENDING_SLOTS];
    uint32_t order;
};
struct umac_mesh_pending_out {
    void *handle;
    uint8_t target[6];
    uint8_t ra[6];  /* valid when ok */
    bool ok;        /* false: expired, the caller drops it */
    bool relayed;   /* a relay copy: send it as built, never as our own frame */
};
void umac_mesh_pending_init(struct umac_mesh_pending *p);
/** Hold @p handle for @p target. @returns a handle the caller must now drop
 *  (the oldest for that target past UMAC_MESH_PENDING_PER_TARGET, an expired
 *  one, or the oldest overall when full), else NULL. */
void *umac_mesh_pending_push(struct umac_mesh_pending *p, const uint8_t *target, void *handle, uint32_t now_ms);
/** As umac_mesh_pending_push, for a relayed frame whose mesh DA is @p target:
 *  only relayed entries count toward, or are evicted by, its caps. It joins the
 *  target's running discovery, or starts one, and lapses when that gives up. */
void *umac_mesh_pending_push_relayed(struct umac_mesh_pending *p, const uint8_t *target,
                                     void *handle, uint32_t now_ms);
/** Hand back every frame that can now be sent (with its next hop) or has
 *  expired (ok=false). Our own resolve through umac_mesh_fwd_tx(); a relayed
 *  one through a path or peer for its mesh DA only. @returns entries written. */
uint32_t umac_mesh_pending_take(struct umac_mesh_pending *p, const struct umac_mesh_fwd_ctx *c,
                                struct umac_mesh_pending_out *out, uint32_t max);
uint32_t umac_mesh_pending_count(const struct umac_mesh_pending *p);
uint32_t umac_mesh_pending_count_relayed(const struct umac_mesh_pending *p);

/**
 * The distinct targets of OUR held frames still waiting on discovery, so the
 * caller can re-ask. A single PREQ is broadcast and unacknowledged; without a
 * retry a held frame whose PREQ was lost simply expires. Relayed targets ask on
 * their own ladder instead. @returns targets written to @p out.
 */
uint32_t umac_mesh_pending_targets(const struct umac_mesh_pending *p, uint32_t now_ms,
                                   uint8_t (*out)[6], uint32_t max);

/** When PREQ @p k of a relayed discovery is due, from its start; k =
 *  UMAC_MESH_RELAY_PREQ_RETRIES + 1 is when it gives up. */
uint32_t umac_mesh_relay_ask_ms(uint32_t k);
/** Relayed targets whose next PREQ is due at @p now_ms. @returns count written. */
uint32_t umac_mesh_pending_ask_due(const struct umac_mesh_pending *p, uint32_t now_ms,
                                   uint8_t (*out)[6], uint32_t max);
/** A PREQ for relayed target @p target went out at @p sent_ms: advance its ladder
 *  one step if that step was due by then and no earlier PREQ took it. */
void umac_mesh_pending_asked(struct umac_mesh_pending *p, const uint8_t *target, uint32_t sent_ms);
/** Time from @p now_ms to the next relayed PREQ or give-up (0: one is due now).
 *  @returns false when no relayed frame is held. */
bool umac_mesh_pending_next_ms(const struct umac_mesh_pending *p, uint32_t now_ms, uint32_t *delta_ms);

/**
 * Originate a PREQ for @p target: the body, and the address it must be sent
 * to, which is ALWAYS broadcast -- the target is by construction not a
 * neighbour, so a unicast to it reaches nobody. The one-hop responder path
 * that pings a peer directly is a different function and unaffected.
 */
uint16_t umac_mesh_fwd_originate_preq(uint8_t *body, uint16_t body_len, const uint8_t *own,
                                      uint32_t *own_sn, uint32_t *preq_id, const uint8_t *target,
                                      uint32_t lifetime_tu, uint8_t ra_out[6]);

/**
 * Shape an outgoing frame whose 802.3 endpoints are @p da and @p sa.
 * @p sa != own means a host behind us originated it (bridge mode), which is
 * expressed with Address Extension; @p da known as a proxied host is sent to
 * its mesh node with AE 2 -- a leaf-learned one, with no path to that node,
 * through the peer it was learned from. @p seq is our Mesh Control sequence
 * counter.
 */
void umac_mesh_fwd_tx(const struct umac_mesh_fwd_ctx *c, const uint8_t *da, const uint8_t *sa,
                      uint32_t seq, struct umac_mesh_fwd_tx_result *r);

#ifdef __cplusplus
}
#endif
