/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 802.11s mesh peering (MPM) state machine — Warthog softmac port.
 *
 * This is a faithful port of the peer-link finite state machine from the Linux
 * kernel `net/mac80211/mesh_plink.c` (v6.6), restructured for the embedded
 * target: instead of reaching into `struct sta_info` / `struct
 * ieee80211_sub_if_data` and the whole mac80211 framework, the state machine
 * operates on a small `struct mesh_peer` and routes every side effect (frame
 * TX, peering timers, established/deactivated notifications, link-id
 * generation) through a `struct mesh_plink_ops` vtable. That keeps the FSM
 * logic — the actual standard-defined behaviour — identical to mac80211 while
 * making it host-unit-testable and free of kernel internals.
 *
 * The frames the FSM emits/consumes are S1G mesh peering action frames; the
 * dot11ah layer (already ported) does the S1G<->11n IE translation, and the
 * driver bridge feeds decoded (ftype, llid, plid) tuples in here.
 */
#ifndef _WARTHOG_MESH_PLINK_H_
#define _WARTHOG_MESH_PLINK_H_

#include <linux/types.h>
#include <linux/ieee80211.h>   /* WLAN_SP_MESH_PEERING_*, WLAN_REASON_MESH_* */
#include <linux/if_ether.h>    /* ETH_ALEN */

/* Peer-link states (values match enum nl80211_plink_state). */
enum mesh_plink_state {
    MESH_PLINK_LISTEN = 0,
    MESH_PLINK_OPN_SNT,
    MESH_PLINK_OPN_RCVD,
    MESH_PLINK_CNF_RCVD,
    MESH_PLINK_ESTAB,
    MESH_PLINK_HOLDING,
    MESH_PLINK_BLOCKED,
};

/* MPM events driving the FSM (mirrors kernel enum plink_event). */
enum mesh_plink_event {
    MPL_UNDEFINED = 0,
    MPL_OPN_ACPT,   /* accepted peering Open */
    MPL_OPN_RJCT,   /* rejected peering Open (config mismatch) */
    MPL_OPN_IGNR,   /* ignored peering Open */
    MPL_CNF_ACPT,   /* accepted peering Confirm */
    MPL_CNF_RJCT,   /* rejected peering Confirm */
    MPL_CNF_IGNR,   /* ignored peering Confirm */
    MPL_CLS_ACPT,   /* accepted peering Close */
    MPL_CLS_IGNR,   /* ignored peering Close */
};

struct mesh_peer;
struct mesh_plink_ctx;

/* Side-effect hooks the FSM invokes. Implementations bridge to morselib TX,
 * FreeRTOS timers, and the MBSS bookkeeping. All may be NULL in tests that
 * only assert state transitions (the FSM null-checks them). */
struct mesh_plink_ops {
    /* Allocate a fresh, non-zero local link id. */
    u16  (*new_llid)(struct mesh_plink_ctx *ctx);
    /* Transmit a self-protected mesh peering action frame to @peer. @action is
     * one of WLAN_SP_MESH_PEERING_{OPEN,CONFIRM,CLOSE}. */
    void (*tx_frame)(struct mesh_plink_ctx *ctx, struct mesh_peer *peer,
                     u8 action, u16 llid, u16 plid, u16 reason);
    /* Arm/cancel the per-peer peering retry/confirm/holding timer. */
    void (*set_timer)(struct mesh_plink_ctx *ctx, struct mesh_peer *peer,
                      u32 timeout_ms);
    void (*del_timer)(struct mesh_plink_ctx *ctx, struct mesh_peer *peer);
    /* Link became established / was torn down (MBSS counters, paths). */
    void (*on_established)(struct mesh_plink_ctx *ctx, struct mesh_peer *peer);
    void (*on_deactivated)(struct mesh_plink_ctx *ctx, struct mesh_peer *peer);
};

/* Mesh interface peering config (subset of mac80211 struct mesh_config). */
struct mesh_plink_cfg {
    u32 retry_timeout_ms;     /* dot11MeshRetryTimeout */
    u32 confirm_timeout_ms;   /* dot11MeshConfirmTimeout */
    u32 holding_timeout_ms;   /* dot11MeshHoldingTimeout */
    u16 max_peer_links;       /* dot11MeshMaxPeerLinks */
};

/* Per-interface mesh peering context (replaces sdata->u.mesh). */
struct mesh_plink_ctx {
    struct mesh_plink_cfg cfg;
    const struct mesh_plink_ops *ops;
    void *priv;               /* driver back-pointer for the ops */
    u16 num_estab;            /* established peer links (for free-count) */
};

/* Per-peer state (replaces sta->mesh). */
struct mesh_peer {
    u8 addr[ETH_ALEN];
    enum mesh_plink_state plink_state;
    u16 llid;                 /* our local link id for this peer */
    u16 plid;                 /* peer's link id */
    u16 reason;               /* last close reason */
    u8  plink_retries;
    bool authenticated;       /* peer passed auth (kernel WLAN_STA_AUTH) */
    struct mesh_plink_ctx *ctx;
};

/* Initialise a peer to LISTEN. */
void mesh_peer_init(struct mesh_peer *peer, struct mesh_plink_ctx *ctx,
                    const u8 *addr, bool authenticated);

/* Number of additional peer links that may still be established. */
u16 mesh_plink_free_count(const struct mesh_plink_ctx *ctx);

/* Begin peering with @peer (sends Open, moves LISTEN/BLOCKED -> OPN_SNT). */
void mesh_plink_open(struct mesh_peer *peer);

/* Force @peer into BLOCKED (administrative). */
void mesh_plink_block(struct mesh_peer *peer);

/* Map a received MPM action frame to an FSM event. @ftype is one of
 * WLAN_SP_MESH_PEERING_{OPEN,CONFIRM,CLOSE}; @matches_local is whether the
 * frame's mesh profile matches ours; @ie_len is the peering-IE length (used to
 * disambiguate Close). Returns MPL_UNDEFINED on error/ignore. */
enum mesh_plink_event mesh_plink_get_event(struct mesh_plink_ctx *ctx,
                                           struct mesh_peer *peer, u8 ftype,
                                           bool matches_local, u8 ie_len,
                                           u16 llid, u16 plid);

/* Step @peer's MPM by @event. Returns true if MBSS state changed. */
bool mesh_plink_fsm(struct mesh_peer *peer, enum mesh_plink_event event);

/* Convenience: process a received MPM frame end-to-end (get_event + fsm). */
void mesh_plink_rx(struct mesh_peer *peer, u8 ftype, bool matches_local,
                   u8 ie_len, u16 llid, u16 plid);

/* Human-readable names (for logs/tests). */
const char *mesh_plink_state_name(enum mesh_plink_state s);
const char *mesh_plink_event_name(enum mesh_plink_event e);

#endif /* _WARTHOG_MESH_PLINK_H_ */
