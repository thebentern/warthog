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
};

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
