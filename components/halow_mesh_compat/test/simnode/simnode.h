/*
 * One simulated warthog node.
 *
 * Everything between the 802.3 host interface and mmdrv_tx_frame() is the REAL
 * firmware: umac_mesh.c, umac_mesh_fwd_glue.c, the forwarding engine, the path
 * and proxy tables, umac_datapath.c and umac_datapath_mesh.c, the packet
 * buffers and the STA table. Only the chip (fake_chip.c) and the RTOS
 * (fake_rtos.c) are replaced, plus the radio stack below the mesh
 * (fake_radio_stack.c, generated).
 *
 * What this cannot tell you: anything the radio does. No modulation, no
 * timing, no interference, no chip behaviour beyond a TX status per data frame
 * and the group key slot's PN, and nothing about what a real mac80211 peer
 * does with the bytes. A green run here means the firmware's
 * own logic is consistent, not that it works on the air.
 */
#ifndef SIMNODE_H
#define SIMNODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** A frame the firmware handed to the chip: the exact bytes that would have
 *  gone on the air. */
struct simnode_frame {
    uint8_t  bytes[512];
    uint16_t len;
    bool     is_mgmt;
    uint8_t  vif_id;
    uint8_t  tid;
    /* What the host asked the chip to do with it. On a keyed link the chip
     * adds the CCMP header and MIC, so the bytes above carry neither and these
     * are the only record that the frame was sent encrypted. */
    uint8_t  tx_flags; /* MMDRV_TX_FLAG_* -- HW_ENC means "encrypt this" */
    uint8_t  key_idx;  /* 0xff when no key was selected */
};

/* ---- lifecycle -------------------------------------------------------- */

/** Bring one node up with @p mac.
 *
 * The gates start where warthog_globals.c (generated from main/at.c) puts
 * them, which is NOT all-off: mesh_secure defaults ON, as the shipped
 * firmware does. Suites that call simnode_set_gates(..., secure=false) are
 * choosing an open mesh, and the keyed path they skip is then untested there.
 * The keyed UNICAST relay is covered once, in test_simnode_datapath's
 * t_rx_forward_keyed (via simnode_rx_flags + MMDRV_RX_FLAG_DECRYPTED); keyed
 * group frames and the MGTK rules still are not. */
bool simnode_start(const uint8_t mac[6]);
void simnode_stop(void);

/**
 * Start as an SAE mesh node, as warthog-mesh-sae does (security_type
 * MMWLAN_SAE). hostap is not linked, so no handshake runs: add peers with
 * simnode_add_peer and key them with simnode_set_key, in the order hostap
 * does -- our own MGTK first, at mesh start, before any peer exists.
 */
bool simnode_start_sae(const uint8_t mac[6]);
/** As simnode_start_sae, with the mesh interface on VIF @p vif_id (the others use 0). */
bool simnode_start_sae_vif(const uint8_t mac[6], uint16_t vif_id);
/** Mesh ID for the next start; NULL, empty or over-long restores "simnode". */
void simnode_set_mesh_id(const uint8_t *id, uint8_t len);

/** The gates AT+MESHFWD / MESHBRIDGE / MESHGRP / MESHSEC set. */
void simnode_set_gates(bool fwd, bool bridge, bool grp_std, bool secure);

/* ---- peers ------------------------------------------------------------ */

/** Add an ESTAB peer, as peering would. */
bool simnode_add_peer(const uint8_t mac[6]);
/** Drop a peer, as the peering watchdog does. */
void simnode_del_peer(const uint8_t mac[6]);

/**
 * What hostap's set_key driver op delivers once AMPE completes: a peer's MTK
 * (pairwise) or MGTK, or -- addressed to the broadcast address -- our own TX
 * MGTK. Returns the mmwlan_status the datapath returned.
 */
int simnode_set_key(const uint8_t addr[6], const uint8_t key[16], uint8_t key_id, bool pairwise);

/** As simnode_set_key, with the seq hostap's set_key passes: for a peer's MGTK,
 *  the Key RSC its AMPE carried (6 octets, little-endian). NULL passes none. */
int simnode_set_key_rsc(const uint8_t addr[6], const uint8_t key[16], uint8_t key_id,
                        bool pairwise, const uint8_t rsc[6]);

/** What the mesh driver's get_seqnum(addr NULL, idx) answers: the Key RSC hostap
 *  writes into an AMPE Open's GTKdata for our own MGTK. */
int simnode_own_group_rsc(uint8_t key_id, uint8_t rsc[6]);

/** What set_key delivers for a BIP-CMAC-128 IGTK (key id 4 or 5): ours against the
 *  broadcast address (NULL @p key removes it), else a peer's from its AMPE Open with
 *  @p rsc (its IPN, 6 octets little-endian, or NULL) as the replay floor. */
int simnode_set_igtk(const uint8_t addr[6], const uint8_t key[16], uint16_t key_id,
                     const uint8_t rsc[6]);

/* ---- chip key installs ------------------------------------------------ */

/** One INSTALL_KEY the firmware sent the chip. */
struct simnode_keyinst {
    uint16_t vif_id;
    uint16_t aid;      /* 0 for the VIF-wide group slot */
    bool     pairwise;
    uint8_t  key_idx;
    uint8_t  key[16];
    uint64_t tx_pn;
};
unsigned simnode_keyinst_count(void);
const struct simnode_keyinst *simnode_keyinst_get(unsigned i);
void simnode_keyinst_clear(void);
/** Make the next INSTALL_KEY fail, as the chip would; nothing is recorded for it. */
void simnode_fail_next_install_key(void);

/* ---- the chip's TX queue ---------------------------------------------- */

/** By default the chip sends each data frame as it arrives and reports its TX status.
 *  With hold on, they wait in its queue, as behind an INSTALL_KEY that overtakes them. */
void simnode_tx_hold(bool on);
unsigned simnode_tx_held(void);
/** The chip sends held frame @p i (0 = oldest): a HW_ENC group frame under the group
 *  slot's key draws that slot's next TX PN. False if there is none. */
bool simnode_tx_send_held(unsigned i);
/** The chip hands held frame @p i back untried (attempts 0, duty cycle), drawing no PN. */
bool simnode_tx_return_held(unsigned i);
/** True once the group slot encrypted a frame; @p top gets the highest PN it used. */
bool simnode_group_pn_top(uint64_t *top);

/* ---- driving ---------------------------------------------------------- */

/** Originate an 802.3 frame from the host side, through the real TX path. */
bool simnode_host_tx(const uint8_t da[6], const uint8_t sa[6],
                     const uint8_t *payload, uint16_t payload_len);
/** As simnode_host_tx, returning on the netif task: the event loop has not run yet. */
bool simnode_host_tx_nopump(const uint8_t da[6], const uint8_t sa[6],
                            const uint8_t *payload, uint16_t payload_len);

/** Inject a received 802.11 frame, through the real RX path. */
bool simnode_rx(const uint8_t *frame, uint16_t len, int16_t rssi);

/**
 * As simnode_rx, with the chip's RX flags (MMDRV_RX_FLAG_*). Pass
 * MMDRV_RX_FLAG_DECRYPTED to model a frame the chip decrypted in place: the
 * bytes are then the MAC header, the CCMP header, the PLAINTEXT body and the
 * MIC octets, exactly as the MM6108 hands them up. Without it, a Protected
 * frame is one the chip could not decrypt.
 */
bool simnode_rx_flags(const uint8_t *frame, uint16_t len, int16_t rssi, uint8_t rx_flags);

/** Run the 2 s service tick (held-frame flush, peering watchdog, rekey). */
void simnode_tick(void);

/** Run the event loop: its datapath work, which is what actually sends queued
 *  frames, and the events posted to it (umac_core_evt_queue). host_tx, rx and
 *  tick already pump; call it directly after doing something that queues work
 *  by another route. */
void simnode_pump(void);

/** Events posted to the umac event loop and not yet run by simnode_pump(). */
unsigned simnode_evt_pending(void);
/** Fill the event queue with no-ops, so the next post fails; returns how many. */
unsigned simnode_evt_fill(void);
/** Drop every queued event unrun, as a umac core restart does. */
void simnode_evt_discard(void);

/* ---- virtual time (fake_rtos.c) --------------------------------------- */
uint32_t mmosal_get_time_ms(void);
void simnode_set_time_ms(uint32_t t);
void simnode_advance_ms(uint32_t d);

/* ---- observation ------------------------------------------------------ */

/** An 802.3 frame the firmware delivered UP to the host netif, captured on the
 *  real mmwlan_rx_cb_t the datapath calls. This is the only honest answer to
 *  "did this node's application actually receive it", as opposed to "did a
 *  frame arrive on the air". */
struct simnode_hostrx {
    uint8_t  da[6];
    uint8_t  sa[6];
    uint8_t  payload[256];
    uint16_t len;
};

unsigned simnode_host_rx_count(void);
const struct simnode_hostrx *simnode_host_rx_get(unsigned i);
void simnode_host_rx_clear(void);

unsigned simnode_outbox_count(void);
unsigned simnode_outbox_dropped(void);
void simnode_outbox_clear(void);
const struct simnode_frame *simnode_outbox_get(unsigned i);

/** Calls into a radio-stack stub, so a test can assert a mesh-only path never
 *  fell through into code the simulator does not model. */
unsigned simnode_stub_hits(const char *name);
void simnode_stub_reset(void);

/** The last SAE candidate the firmware offered hostap (umac_supp_mesh_new_peer):
 *  copies its address into @p addr, points @p ies at its IEs, returns their length. */
size_t simnode_last_new_peer(uint8_t addr[6], const uint8_t **ies);

/** Live allocations, for leak assertions. */
unsigned simnode_live_allocs(void);
unsigned simnode_in_critical(void);
/** Run @p cb once, at the next TX packet allocation: between build_mgmt_frame()'s two passes. */
void simnode_set_tx_alloc_hook(void (*cb)(void));
unsigned simnode_rssi_calls(void);
bool simnode_rssi_for(const uint8_t *ta, int16_t *rssi);
unsigned simnode_timeouts_registered(void);
/** Make the next umac_core timeout registration fail, as an empty pool does. */
void simnode_fail_next_timeout(void);
/** umac_core timeouts registered and not yet fired or dequeued. */
unsigned simnode_timeouts_pending(void);
/** Fire every umac_core timeout due at the current virtual time, earliest first,
 *  then run the event loop, as the core task would. @returns how many fired. */
unsigned simnode_run_timeouts(void);
/** Advance virtual time @p ms in 1 ms steps, firing timeouts as they fall due. */
void simnode_advance_run(uint32_t ms);

/** The +MESHPATH / table dump the AT command prints. */
int simnode_render_paths(char *buf, uint32_t len);

#endif /* SIMNODE_H */
