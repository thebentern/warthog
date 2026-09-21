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
};

enum umac_mesh_fwd_drop {
    UMAC_MESH_FWD_DROP_NONE = 0,
    UMAC_MESH_FWD_DROP_OWN,       /* our own frame came back */
    UMAC_MESH_FWD_DROP_DUP,       /* recent multicast cache hit */
    UMAC_MESH_FWD_DROP_NOT_FOR_US,/* unicast, RA not us -- chip should not deliver these */
    UMAC_MESH_FWD_DROP_NO_FWD,    /* would forward, forwarding disabled */
    UMAC_MESH_FWD_DROP_TTL,
    UMAC_MESH_FWD_DROP_NO_PATH,   /* a PERR goes back instead */
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
     * and the body are carried unchanged. */
    uint8_t fwd_ra[6];
    struct umac_mesh_ctrl fwd_mc;
    /* The frame's mesh endpoints as the engine understood them -- a relay
     * must carry THESE forward, not whatever the previous hop put in addr3. */
    uint8_t mesh_da[6];
    uint8_t mesh_sa[6];
    /* NO_PATH: a PERR to send back to the transmitter. */
    bool send_perr;
    uint8_t perr_to[6];
    uint8_t perr_body[HWMP_PERR_BODY_LEN];
    uint16_t perr_len;
};

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
#define UMAC_MESH_PREQ_TARGETS 4u
#endif
#ifndef UMAC_MESH_PREQ_MIN_INTERVAL_MS
#define UMAC_MESH_PREQ_MIN_INTERVAL_MS 500u
#endif
#ifndef UMAC_MESH_PREQ_GLOBAL_MIN_MS
#define UMAC_MESH_PREQ_GLOBAL_MIN_MS 50u
#endif

struct umac_mesh_preq_gate {
    struct { uint8_t target[6]; uint32_t last_ms; bool used; } t[UMAC_MESH_PREQ_TARGETS];
    uint32_t any_ms;
    bool any_used;
};

void umac_mesh_preq_gate_init(struct umac_mesh_preq_gate *g);

/** true: send a PREQ for @p target now, and it is recorded. false: suppressed. */
bool umac_mesh_preq_gate_allow(struct umac_mesh_preq_gate *g, const uint8_t *target,
                               uint32_t now_ms);

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
    /* The path is about to expire (mac80211 dot11MeshHWMPpathRefreshInterval):
     * still usable, but the caller should send a PREQ for path_target now. */
    bool refresh;
};

/** mac80211 default path refresh interval. */
#define UMAC_MESH_PATH_REFRESH_MS 1000u

/* ---- protection latch --------------------------------------------------
 * Trust on first protected frame. Warthog's own mesh runs without management
 * frame protection, so today no path-selection frame arrives protected and
 * the peer check is a transmitter-address compare, not an authentication
 * boundary. The moment a peer DOES protect its path selection, a plaintext
 * frame claiming to be from it is refused -- so turning PMF on makes the
 * check real without another code change, and nothing is dropped before. */
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
 * mac80211 queues up to 10 frames per path while a PREQ is out and sends
 * them when the PREP installs the path; without it the first frame of every
 * flow to a non-neighbour is lost. Bounded small: the handles are TX-pool
 * buffers shared with our own traffic and peering. */
#define UMAC_MESH_PENDING_MAX 4u
#define UMAC_MESH_PENDING_PER_TARGET 2u
#define UMAC_MESH_PENDING_MS 2000u
struct umac_mesh_pending {
    struct { uint8_t target[6]; void *handle; uint32_t exp_ms; uint32_t order; bool used; } e[UMAC_MESH_PENDING_MAX];
    uint32_t order;
};
struct umac_mesh_pending_out {
    void *handle;
    uint8_t target[6];
    uint8_t ra[6];  /* valid when ok */
    bool ok;        /* false: expired, the caller drops it */
};
void umac_mesh_pending_init(struct umac_mesh_pending *p);
/** Hold @p handle for @p target. @returns a handle the caller must now drop
 *  (the oldest for that target past UMAC_MESH_PENDING_PER_TARGET, an expired
 *  one, or the oldest overall when full), else NULL. */
void *umac_mesh_pending_push(struct umac_mesh_pending *p, const uint8_t *target, void *handle, uint32_t now_ms);
/** Hand back every frame that can now be sent (with its next hop) or has
 *  expired (ok=false). Resolution is umac_mesh_fwd_tx(), so the precedence
 *  rules are the shipped ones. @returns entries written to @p out. */
uint32_t umac_mesh_pending_take(struct umac_mesh_pending *p, const struct umac_mesh_fwd_ctx *c,
                                struct umac_mesh_pending_out *out, uint32_t max);
uint32_t umac_mesh_pending_count(const struct umac_mesh_pending *p);

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
 * its mesh node with AE 2. @p seq is our Mesh Control sequence counter.
 */
void umac_mesh_fwd_tx(const struct umac_mesh_fwd_ctx *c, const uint8_t *da, const uint8_t *sa,
                      uint32_t seq, struct umac_mesh_fwd_tx_result *r);

#ifdef __cplusplus
}
#endif
