/*
 * Copyright 2025 Morse Micro
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 */

#include "umac/datapath/umac_datapath.h"
#include "dot11/dot11_frames.h"

#pragma once

struct MM_PACKED umac_8023_hdr
{
    struct
    {
        uint8_t dest_addr[DOT11_MAC_ADDR_LEN];
        uint8_t src_addr[DOT11_MAC_ADDR_LEN];
        uint16_t ethertype_be;
    };
};

MM_STATIC_ASSERT(sizeof(struct umac_8023_hdr) == 14, "Invalid 802.3 definition");
MM_STATIC_ASSERT(
    offsetof(struct umac_8023_hdr, ethertype_be) ==
        sizeof(struct umac_8023_hdr) - MM_MEMBER_SIZE(struct umac_8023_hdr, ethertype_be),
    "Ethernet Type must be the last field");


struct umac_datapath_ops
{

    void (*process_rx_mgmt_frame)(struct umac_data *umacd,
                                  struct umac_sta_data *stad,
                                  struct mmpktview *rxbufview);


    struct umac_sta_data *(*lookup_stad_by_peer_addr)(struct umac_data *umacd,
                                                      const uint8_t *peer_addr);


    struct umac_sta_data *(*lookup_stad_by_tx_dest_addr)(struct umac_data *umacd,
                                                         const uint8_t *dest_addr);


    struct umac_sta_data *(*lookup_stad_by_aid)(struct umac_data *umacd, uint16_t aid);


    bool (*set_stad_sleep_state)(struct umac_sta_data *stad, bool asleep);


    bool (*is_stad_tx_paused)(struct umac_sta_data *stad);


    void (*enqueue_tx_frame)(struct umac_data *umacd,
                             struct umac_sta_data *stad,
                             struct mmpkt *txbuf);


    bool (*dequeue_tx_frame)(struct umac_data *umacd,
                             struct umac_sta_data **stad,
                             struct mmpkt **txbuf);


    void (*construct_80211_data_header)(struct umac_sta_data *stad,
                                        const struct umac_8023_hdr *hdr_8023,
                                        struct dot11_data_hdr *data_hdr);


    enum mmwlan_sta_state (*get_sta_state)(struct umac_sta_data *stad);


    const uint16_t *frames_allowed_pre_association;
};


void umac_datapath_process_rx_action_frame(struct umac_data *umacd,
                                           struct umac_sta_data *stad,
                                           struct mmpktview *rxbufview);


enum mmwlan_status umac_datapath_wait_for_tx_ready_(struct umac_datapath_data *data,
                                                    uint32_t timeout_ms,
                                                    uint16_t mask);

/* Warthog mesh: the header builder only receives the 802.3 header, so the
 * dequeue path hands it the packet's metadata (relayed/proxied endpoints)
 * for the duration of the call. */
struct mmdrv_tx_metadata;
void umac_datapath_mesh_set_cur_tx_md(const struct mmdrv_tx_metadata *md);
const struct mmdrv_tx_metadata *umac_datapath_mesh_cur_tx_md(void);
struct umac_sta_data *umac_datapath_mesh_find_peer(const uint8_t *addr);
/** @p addr's link is established: from add on an open or constant-key mesh, from its
 *  AMPE MTK under SAE. A slot hostap added before SAE finished is not. */
bool umac_datapath_mesh_peer_estab(const uint8_t *addr);
/** Key id of our own TX MGTK once hostap has delivered it, else -1. */
int umac_datapath_mesh_own_group_key_id(void);
/** A group frame the chip opened from @p stad under key id @p key_id, read off the chip at
 *  @p read_seq (mmdrv_rx_metadata), is that peer's: AMPE keyed it, its own MGTK sits in the chip
 *  at its AID under that id (chip-key SAE builds on a MESH chip VIF, AT+GTKPERSTA on), the
 *  frame was read after that key went in (the fence) and no refused DISABLE_KEY leaves a stale
 *  key at the AID (the taint). False everywhere else, where the chip's only group key is our own. */
bool umac_datapath_mesh_peer_gtk_opened(struct umac_sta_data *stad, uint8_t key_id, uint32_t read_seq);
/** The chip booted: it holds no key, so no AID is tainted and no peer MGTK is in it. */
void umac_datapath_mesh_chip_booted(void);
/** Service tick: retire old fences; carry out an AT+GTKPERSTA change. Event loop only. */
void umac_datapath_mesh_service_peer_gtk(void);
/** One frame went to the chip for encryption under our own TX MGTK. */
void umac_datapath_mesh_own_group_tx_note(void);
/** The chip reported TX status for a frame counted by tx_note: it draws no further PN. */
void umac_datapath_mesh_own_group_tx_done(void);
struct umac_sta_data *umac_datapath_mesh_first_peer_except(const uint8_t *excl);
/** A robust unicast management frame to a peer that runs MFP, protected as path selection is:
 *  the frame to send (a new one under host CCMP, @p txbuf released), NULL if it could not be
 *  sealed (released, counted); @p key_id is the key the chip encrypts under, else -1. */
struct mmpkt *umac_datapath_mesh_protect_mgmt(struct mmpkt *txbuf, int *key_id);
/** Around every use of mesh peer records off the event loop: a record found in between is
 *  not freed before read_end, which takes the value read_begin returned. Never blocks. */
uint8_t umac_datapath_mesh_read_begin(void);
void umac_datapath_mesh_read_end(uint8_t side);
