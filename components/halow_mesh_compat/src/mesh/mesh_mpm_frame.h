/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 802.11s Mesh Peering Management (MPM) frame serialization / deserialization
 * — Warthog softmac port.
 *
 * This is the portable core of the FSM<->radio bridge: it converts between the
 * peering FSM's abstract (action, llid, plid, reason) tuples and the actual
 * self-protected action-frame bytes that go on the air. It is the ported
 * equivalent of the frame-building in mesh_plink_frame_tx() and the parsing in
 * mesh_rx_plink_frame()/mesh_process_plink_frame() (kernel net/mac80211/
 * mesh_plink.c v6.6), minus the parts that reach into mac80211 BSS state
 * (rate/HT/VHT/HE IEs are BSS decoration the peering protocol does not require
 * — a matching Mesh ID + the Peer Management IE carry the peering semantics).
 *
 * No driver/struct-morse/mac80211 dependency: builds and parses raw bytes, so
 * it is fully host-unit-testable. The driver datapath only has to hand the
 * built bytes to morselib TX and feed received bytes to the parser.
 *
 * Link-id perspective (the subtle bit): on the wire the Peer Management IE
 * carries the *sender's* local link id first, then (for Confirm/Close) the
 * sender's view of the peer's id. The kernel notes "the llid in the frame is
 * the plid from the point of view of this host". mpm_parse_frame() therefore
 * outputs llid/plid already swapped into the *receiver's* local perspective,
 * so the result feeds straight into mesh_plink_get_event()/mesh_plink_rx().
 */
#ifndef _WARTHOG_MESH_MPM_FRAME_H_
#define _WARTHOG_MESH_MPM_FRAME_H_

#include <linux/types.h>
#include <linux/if_ether.h>   /* ETH_ALEN */

/* Largest MPM frame this module emits (header + cap/AID + Mesh ID + PEER_MGMT
 * + slack). Mesh IDs are <= 32 bytes. */
#define MPM_FRAME_MAX 96

/*
 * Build a self-protected mesh peering action frame into @buf (capacity @cap).
 *
 * @action     one of WLAN_SP_MESH_PEERING_{OPEN,CONFIRM,CLOSE}
 * @our_llid   this node's local link id (goes in the IE's first link-id field)
 * @our_plid   this node's view of the peer's id (second field; CONFIRM, and
 *             CLOSE when non-zero)
 * @reason     close reason (CLOSE only)
 * @aid        association id (CONFIRM only)
 * @mesh_id    mesh ID bytes (included for OPEN/CONFIRM/CLOSE so the receiver
 *             can match the profile); may be NULL with len 0
 *
 * Returns the frame length in bytes, or 0 if it would not fit / bad args.
 */
size_t mpm_build_frame(u8 *buf, size_t cap,
                       const u8 *da, const u8 *sa,
                       u8 action, u16 our_llid, u16 our_plid,
                       u16 reason, u16 aid,
                       const u8 *mesh_id, u8 mesh_id_len);

/* Parsed, receiver-perspective view of a received MPM frame. */
struct mpm_frame {
    bool valid;
    u8   action;              /* ftype: WLAN_SP_MESH_PEERING_* */
    u8   sa[ETH_ALEN];        /* source (the peer) */
    u8   da[ETH_ALEN];        /* destination (us) */
    u16  llid;                /* receiver-local: our id echoed by peer (0 if absent) */
    u16  plid;                /* receiver-local: the peer's id */
    u16  reason;              /* CLOSE only, else 0 */
    u16  aid;                 /* CONFIRM only, else 0 */
    u8   peer_mgmt_ie_len;    /* 4 / 6 / 8 — pass to mesh_plink_get_event */
    const u8 *mesh_id;        /* points into @buf, or NULL */
    u8   mesh_id_len;
};

/*
 * Parse a received self-protected mesh peering action frame. Returns true and
 * fills @out (with out->valid = true) on a well-formed frame; returns false on
 * anything that is not a parseable MPM frame (wrong type/category, truncated,
 * missing or malformed Peer Management IE, illegal IE length for the action).
 */
bool mpm_parse_frame(const u8 *buf, size_t len, struct mpm_frame *out);

#endif /* _WARTHOG_MESH_MPM_FRAME_H_ */
