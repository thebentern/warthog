/*
 * Copyright 2025 Morse Micro
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 */

#include "umac/datapath/umac_datapath.h"
#include "umac/mesh/umac_mesh_frag.h"
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
/** The chip booted: it holds no key, so no AID is tainted and no peer MGTK is in it. After a
 *  hardware restart (@p restart) the AID 0 group key it held is owed back. */
void umac_datapath_mesh_chip_booted(bool restart);
/** After a chip restart, event loop only: stations, the AID 0 group key and peers' keys back on
 *  @p vif_id above every PN they drew. True if all went in; the rest is retried by the service tick. */
bool umac_datapath_mesh_chip_restored(uint16_t vif_id);
/** Service tick: what a chip restart could not put back goes in now. Event loop only. */
void umac_datapath_mesh_service_restore(void);
/** True if the chip would seal this frame (a group one: @p group) under a key a chip restart could
 *  not put back yet. */
bool umac_datapath_mesh_chip_key_missing(struct umac_sta_data *stad, bool group);
/** One group frame went to the chip for encryption under the AID 0 group key. */
void umac_datapath_mesh_group_tx_note(void);
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
/** AT+HOSTFRAG, event loop only: @p n fragments to @p stad on @p tid (@p chip seals them, @p need
 *  pool blocks beyond the MSDU's) go to the chip under the run lock, which run_end releases. */
void umac_datapath_mesh_frag_run_begin(struct umac_sta_data *stad, uint8_t tid, unsigned n,
                                       bool chip, unsigned need);
void umac_datapath_mesh_frag_run_end(struct umac_sta_data *stad, unsigned unhanded);
/** True if @p stad's next frame @p head must wait: behind a DELBA a cut waits on, or (counted) a
 *  fragment run it could break or the TX pool short of the last cut's blocks. Event loop only. */
bool umac_datapath_mesh_frag_wait(struct umac_sta_data *stad, const struct mmpkt *head);
/** A data frame's TX status (@p frame its 802.11 header, @p attempts 0 untried): a host fragment
 *  is counted, and its MSDU once every fragment is reported. Event loop only. */
void umac_datapath_mesh_frag_status(struct umac_sta_data *stad, const uint8_t *frame, uint32_t len,
                                    uint8_t status_flags, uint8_t attempts);
/** A frame the chip encrypts under @p stad's pairwise key, management or data on @p tid, handed
 *  while a fragment run it could break is in the chip: counted (overlap). */
void umac_datapath_mesh_frag_overlap_note(struct umac_sta_data *stad, bool mgmt, uint8_t tid);
/** Clears runs whose statuses never came, hands held management frames, keeps the TX pool's
 *  reserve, the ADDBA hold-off and AT+AMPDU. Event loop only. */
void umac_datapath_mesh_frag_tick(void);
/** A frame to @p stad on @p tid must be cut: its originator Block Ack session ends (the peer's
 *  frames then wait for that DELBA) and ADDBA on that TID is held off. Event loop only. */
void umac_datapath_mesh_ba_cut(struct umac_sta_data *stad, uint8_t tid);
/** True if this management frame to @p stad is the DELBA umac_datapath_mesh_ba_cut is sending. */
bool umac_datapath_mesh_ba_delba_tagged(const struct umac_sta_data *stad, struct mmpktview *view);
/** True (counted ba_wait, fragments taken) if @p stad's frames wait for a DELBA: the @p n fragments
 *  on @p tid go once it is through. Event loop only. */
bool umac_datapath_mesh_ba_park(struct umac_sta_data *stad, uint8_t tid, struct mmpkt *const *frag,
                                unsigned n, bool chip);
/** True while @p stad's frames wait for the DELBA umac_datapath_mesh_ba_cut sent it. Event loop only. */
bool umac_datapath_mesh_ba_waiting(struct umac_sta_data *stad);
/** The TX status of the DELBA umac_datapath_mesh_ba_cut sent to @p ra came back, @p no_ack: tried and
 *  never acked. Event loop only. */
void umac_datapath_mesh_ba_delba_status(const uint8_t *ra, bool no_ack);
/** Each TX pass the datapath is not paused: frames whose DELBA is through go on. Event loop only. */
void umac_datapath_mesh_ba_release(void);
/** @p n fragments of one MSDU to @p stad on @p tid (@p chip seals them) handed back to back under
 *  the run lock: 0 all handed, -1 the driver refused one (the rest released). Event loop only. */
int umac_datapath_mesh_frags_to_chip(struct umac_sta_data *stad, uint8_t tid,
                                     struct mmpkt *const *frag, unsigned n, bool chip);
/** AT+HOSTFRAG as this build applies it: off with host CCMP, whose fragments after the first chip
 *  firmware 1.17.6 re-encapsulates (on air 2026-10-03); WARTHOG_MESH_HOSTFRAG_ANY lifts that. */
static inline uint32_t umac_datapath_mesh_hostfrag_mode(void)
{
    extern volatile uint32_t g_warthog_hostfrag;
#if defined(WARTHOG_MESH_HOST_CCMP) && !defined(WARTHOG_MESH_HOSTFRAG_ANY)
    return UMAC_MESH_FRAG_OFF;
#else
    return g_warthog_hostfrag;
#endif
}
/** Most fragments AT+HOSTFRAG cuts an MSDU into: what the chip delivers sealing them, or 16. */
#ifdef WARTHOG_MESH_HOSTFRAG_ANY
#define UMAC_DATAPATH_MESH_FRAG_MAX UMAC_MESH_FRAG_MAX
#else
#define UMAC_DATAPATH_MESH_FRAG_MAX UMAC_MESH_FRAG_CHIP_MAX
#endif
/** TX pool blocks kept back while AT+HOSTFRAG is in force: one cut's extra fragments for each peer
 *  and the DELBA that ends its session first; 16 fragments keep UMAC_MESH_FRAG_POOL_RESERVE. */
#ifdef WARTHOG_MESH_HOSTFRAG_ANY
#define UMAC_DATAPATH_MESH_FRAG_RESERVE UMAC_MESH_FRAG_POOL_RESERVE
#else
#define UMAC_DATAPATH_MESH_FRAG_RESERVE ((UMAC_MESH_FRAG_CHIP_MAX - 1u) * UMAC_DATAPATH_MESH_MAX_PEERS + 1u)
#endif
/** False while AT+AMPDU=0 or that TID's ADDBA hold-off runs (counted hold). Event loop only. */
bool umac_datapath_mesh_ba_may_start(struct umac_sta_data *stad, uint8_t tid);
/** Around every use of mesh peer records off the event loop: a record found in between is
 *  not freed before read_end, which takes the value read_begin returned. Never blocks. */
uint8_t umac_datapath_mesh_read_begin(void);
void umac_datapath_mesh_read_end(uint8_t side);
