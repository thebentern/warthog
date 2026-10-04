/*
 * One simulated warthog node.
 *
 * Everything between the 802.3 host interface and mmdrv_tx_frame() is the REAL
 * firmware: umac_mesh.c, umac_mesh_fwd_glue.c, the forwarding engine, the path
 * and proxy tables, umac_datapath.c and umac_datapath_mesh.c, Block Ack
 * (umac_ba.c), the packet buffers and the STA table. Only the chip
 * (fake_chip.c) and the RTOS (fake_rtos.c) are replaced, plus the radio stack
 * below the mesh (fake_radio_stack.c, generated).
 *
 * What this cannot tell you: anything the radio does. No modulation, no
 * timing, no interference, no chip behaviour beyond a TX status per data frame
 * (and per frame under our group key, and per DELBA a cut MSDU waits on), the group
 * key slot's PN and, through
 * simnode_rx_air only, which key the chip opens a received frame under, and nothing
 * about what a real mac80211 peer
 * does with the bytes. A green run here means the firmware's
 * own logic is consistent, not that it works on the air.
 */
#ifndef SIMNODE_H
#define SIMNODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** One entry of a rate chain: bandwidth in MHz, S1G MCS (10 is MCS10), attempts at it. */
struct simnode_rate {
    uint8_t bw_mhz;
    uint8_t mcs;
    uint8_t attempts;
};

/** A frame the firmware handed to the chip: the exact bytes that would have
 *  gone on the air. */
struct simnode_frame {
    uint8_t  bytes[1600]; /* a 1500-byte payload in its largest shape, host CCMP included */
    uint16_t len;
    bool     is_mgmt;
    uint8_t  vif_id;
    uint8_t  tid;
    /* What the host asked the chip to do with it. On a keyed link the chip
     * adds the CCMP header and MIC, so the bytes above carry neither and these
     * are the only record that the frame was sent encrypted. */
    uint8_t  tx_flags; /* MMDRV_TX_FLAG_* -- HW_ENC means "encrypt this" */
    uint8_t  key_idx;  /* 0xff when no key was selected */
    /* What the chip put on the air once it sent the frame: a HW_ENC one sealed under the key
     * it holds (the station's pairwise key, or the group slot's) at that key's next TX PN,
     * anything else as handed. air_len 0 until sent, or when it holds no such key. */
    bool     sent;
    uint8_t  air[1600];
    uint16_t air_len;
    uint64_t pn;           /* the TX PN the chip drew for a HW_ENC frame */
    uint8_t  status_flags; /* the TX status it reported (MMDRV_TX_STATUS_*) */
    uint8_t  attempts;
    uint8_t  aid;          /* the metadata's AID */
    struct simnode_rate chain[4]; /* its rate table as handed (attempts 0: entry unused) */
    uint8_t  chain_rts;    /* bit r: chain entry r asks for RTS/CTS */
    uint8_t  pn_draws;     /* TX PNs the chip drew sealing it: one per fragment it cut it into */
    bool     host_frag;    /* the metadata's mesh.host_frag */
    bool     ba_wait;      /* the metadata's mesh.ba_wait: a DELBA a cut MSDU waits on */
    uint8_t  reorder;      /* the metadata's tid_max_reorder_buf_size: tid_params' reorder size */
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

/** A-MPDU as the board runs it (config on, the chip's capability set), so unicast data
 *  starts Block Ack sessions through the real umac_ba.c. Off by default; survives a start. */
void simnode_set_ampdu(bool on);

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

/* ---- chip interface, BSS and station commands --------------------------- */

/** One interface, BSS, station or key-removal command the firmware sent the chip, with what
 *  it answered. The start sends the boot ADD_INTERFACE (mmwlan_boot) before the mesh's own. */
struct simnode_chipcmd {
    uint16_t id;      /* MORSE_CMD_ID_* */
    uint16_t vif_id;  /* ADD_INTERFACE: the id the chip handed out (UINT16_MAX: none) */
    uint32_t arg;     /* ADD_INTERFACE: interface type; SET_STA_STATE: state; MESH_CONFIG: start;
                       * BSS_CONFIG: beacon interval; BSS_BEACON_CONFIG: enable;
                       * REMOVE_INTERFACE: the VIF id; DISABLE_KEY: the hardware key index */
    bool     beaconing; /* MESH_CONFIG: enable_beaconing */
    bool     pairwise;  /* DISABLE_KEY: a pairwise key, else a group one */
    uint16_t aid;     /* SET_STA_STATE, DISABLE_KEY */
    uint8_t  addr[6]; /* ADD_INTERFACE: VIF MAC; BSSID_SET: BSSID; SET_STA_STATE: station */
    int32_t  status;  /* the chip's own status in its response; 0 accepted */
    int      ret;     /* the transport result */
};
unsigned simnode_chipcmd_count(void);
const struct simnode_chipcmd *simnode_chipcmd_get(unsigned i);
void simnode_chipcmd_clear(void);
/** Logged commands with @p id and, unless it is UINT32_MAX, @p arg. */
unsigned simnode_chipcmd_n(uint16_t id, uint32_t arg);
/** Arm a refusal: the chip answers an ADD_INTERFACE of @p type (0: any) with @p status, or
 *  fails its transport with @p ret when that is non-zero. Refusals are taken in the order
 *  armed, each by the first ADD_INTERFACE after the one before it that matches. */
void simnode_chip_refuse_add_if(uint32_t type, int32_t status, int ret);
/** The next REMOVE_INTERFACE fails with @p ret. */
void simnode_chip_refuse_rm_if(int ret);
/** The chip answers the next BSSID_SET, MESH_CONFIG, BSS_BEACON_CONFIG, SET_STA_STATE or
 *  INSTALL_KEY (@p id) with @p status; a refused key is not installed. */
void simnode_chip_refuse_next(uint16_t id, int32_t status);
/** As simnode_chip_refuse_next, for the next such command whose arg is @p arg: SET_STA_STATE
 *  its state, INSTALL_KEY its aid. REMOVE_INTERFACE (arg: the VIF id) and DISABLE_KEY (arg:
 *  its aid; the key stays) fail their transport with @p status instead, having no status
 *  of their own. */
void simnode_chip_refuse_next_arg(uint16_t id, uint32_t arg, int32_t status);
/** Refusals armed and not yet taken, of any kind; and dropping them all. */
unsigned simnode_chip_refusals_armed(void);
void simnode_chip_refusals_clear(void);
/** ADD_INTERFACE for a MESH VIF hands out @p vif_id (the others keep the start's). */
void simnode_chip_set_mesh_vif_id(uint16_t vif_id);
/** Run the boot scan probe's interface churn (SCAN add and remove) at the next starts,
 *  as every mesh env but -swccmp-on does (main/mesh.c). Off by default. */
void simnode_set_boot_scan(bool on);

/* ---- the chip's state, and a hardware restart ---------------------------- */

/** What the chip holds now. All of it is lost when it boots (mmdrv_init), which a hardware
 *  restart does again; boots and restarts_done count across boots. */
struct simnode_chipstate {
    unsigned boots;            /* mmdrv_init calls since the simulator was loaded */
    unsigned restarts_done;    /* mmdrv_hw_restart_completed calls */
    bool     down;             /* between mmdrv_deinit and the next mmdrv_init */
    bool     vif;              /* an interface is added */
    uint32_t vif_type;
    uint16_t vif_id;
    unsigned qos;              /* SET_QOS_PARAMS since it booted */
    uint16_t beacon_int;       /* BSS_CONFIG's beacon interval; 0 none */
    bool     bss_beacon;       /* BSS_BEACON_CONFIG(enable) taken */
    bool     bssid_set;
    uint8_t  bssid[6];
    uint32_t beacon_period_ms; /* the host beacon timer's period; 0 not started */
    bool     mesh_started;     /* MESH_CONFIG(START) taken */
    bool     mesh_beaconing;
    uint32_t frag_threshold;   /* its own TX fragmentation threshold (AT+FRAG); 0 off */
    bool     crypto_in_host_set;
    bool     crypto_in_host;
    uint32_t health_ms;        /* the health check's minimum interval; 0 none set */
    uint32_t flush_wm;         /* MORSE_PARAM_ID_TX_STATUS_FLUSH_WATERMARK; 0 none set */
    unsigned dyn_ps_sets;      /* dynamic power-save timeouts set since it booted */
    uint32_t dyn_ps_ms;        /* the last one */
    unsigned stas, keys;       /* station records and keys it holds */
};
const struct simnode_chipstate *simnode_chipstate(void);
/** The SET_STA_STATE state the chip holds for @p aid (its address to @p addr), or -1. */
int simnode_chip_sta_state(uint16_t aid, uint8_t addr[6]);
/** A health check failed: what driver_health.c does then (the TX path paused, the restart event
 *  posted at the head of the event queue), and the event loop run, which restarts the chip
 *  (mmdrv_deinit, mmdrv_init: it loses everything above) through umac_mmdrv_shim.c. */
void simnode_chip_restart(void);
/** As simnode_chip_restart, returning once the event is posted: the loop has not run. */
void simnode_chip_restart_queued(void);
/** The health check the driver runs when its interval is next set (at a restart, right after the
 *  chip boots) fails, so the health task posts another restart. */
void simnode_chip_fail_next_health_check(void);
/** Run @p cb once, as the next restart completes (mmdrv_hw_restart_completed), standing for
 *  another task at that moment. */
void simnode_chip_on_restart_done(void (*cb)(void));
/** The keys the chip holds now, up to @p max, each with its next TX PN; returns how many. */
struct simnode_keyinst;
unsigned simnode_chip_keys(struct simnode_keyinst *out, unsigned max);
/** Run @p fn; true if a firmware assertion fired in it (the board would reset there). */
bool simnode_expect_assert(void (*fn)(void));
/** Calls that reached the chip off the umac event loop (which runs a restart): AT+CRYPTOHOST's
 *  set and read, and AT+CHIPRESTART's forced health check failure. */
unsigned simnode_chip_calls_off_loop(void);
/** umac_interface_set_channel_from_regdb (a stub) returns @p status from now on; 0 by default. */
void simnode_set_channel_status(int status);

/* ---- the chip's keys and its receive crypto (fake_chip.c) ---------------- */

/** True while the chip holds a key at @p aid of that kind and index (a group key's index is
 *  its key id), copied to @p key if non-NULL. Held from the INSTALL_KEY it accepted until a
 *  DISABLE_KEY or an install over it; a station's removal is not assumed to take it. */
bool simnode_chip_key_held(uint16_t aid, bool pairwise, uint8_t idx, uint8_t key[16]);
/** A frame off the air, received through the chip. A Protected one is opened under the key
 *  the chip holds for its transmitter's station (from SET_STA_STATE) under the frame's key
 *  id -- pairwise for a unicast, that station's group key for a group frame on a MESH VIF,
 *  the VIF's group key at AID 0 on a STA VIF -- and handed up decrypted
 *  (MMDRV_RX_FLAG_DECRYPTED, MIC octets still in place); else it goes up as it came. */
bool simnode_rx_air(const uint8_t *frame, uint16_t len, int16_t rssi);
/** As simnode_rx_air, returning once the frame is read and queued: the event loop has not
 *  run (simnode_rx_flags_queued). */
bool simnode_rx_air_queued(const uint8_t *frame, uint16_t len, int16_t rssi);
/** On a MESH VIF, a group frame whose station holds no group key under its key id is tried
 *  under the VIF's group key at AID 0, as a chip with that fallback would. Off by default;
 *  survives a start. */
void simnode_chip_group_fallback(bool on);
/** On a MESH VIF, a group frame whose MIC fails under its station's group key is tried
 *  under the VIF's group key at AID 0 too, and handed up with that key's MIC octets in place:
 *  a chip with that fallback (not measured on the MM6108). Off by default; survives a start. */
void simnode_chip_group_fallback_mic(bool on);
/** Frames simnode_rx_air's chip has opened since the simulator was loaded. */
unsigned simnode_chip_rx_opened(void);

/* ---- the chip's TX queue ---------------------------------------------- */

/** By default the chip sends each data frame (and each DELBA a cut MSDU waits on) as it arrives
 *  and reports its TX status. With hold on, they wait in its queue, as behind an INSTALL_KEY
 *  that overtakes them. */
void simnode_tx_hold(bool on);
unsigned simnode_tx_held(void);
/** The QoS TID of held frame @p i, -1 if there is none or it was not recorded. */
int simnode_tx_held_tid(unsigned i);
/** The chip sends held frame @p i (0 = oldest): a HW_ENC group frame under the group
 *  slot's key draws that slot's next TX PN. False if there is none. */
bool simnode_tx_send_held(unsigned i);
/** The chip hands held frame @p i back untried (attempts 0, duty cycle), drawing no PN. */
bool simnode_tx_return_held(unsigned i);
/** On a STA VIF every pairwise seal draws one TX PN counter, which each pairwise install
 *  sets (the one-slot chip the firmware's comments describe); the key used is still the
 *  station's own. Off (default): a counter per key. Survives a start. */
void simnode_chip_shared_pairwise_pn(bool on);
/** The TX PN the chip would seal the next frame under its key at @p aid (pairwise or group,
 *  key index @p idx) with; false if it holds none. */
bool simnode_chip_key_next_pn(uint16_t aid, bool pairwise, uint8_t idx, uint64_t *pn);
/** True once the group slot encrypted a frame; @p top gets the highest PN it used. */
bool simnode_group_pn_top(uint64_t *top);
/** Frames the group slot has encrypted since the simulator was loaded. */
unsigned simnode_group_pn_draws(void);
/** The next @p n unicast data frames the chip sends are reported not ACKed, after every
 *  attempt their rate chain allows (simnode_frame.status_flags, .attempts). */
void simnode_tx_noack_next(unsigned n);
/* The next @p n unicast management frames the chip sends are given up on unacked, every attempt used. */
void simnode_tx_noack_mgmt_next(unsigned n);
/** The next unicast data frame the chip sends is acked at its @p attempts-th attempt if its
 *  rate chain allows that many, else reported not acked after every attempt it allows. */
void simnode_tx_retry_next(unsigned attempts);
/** The next @p n unicast data frames the chip sends are reported sent in an A-MPDU
 *  (MMDRV_TX_STATUS_WAS_AGGREGATED), whatever their flags. */
void simnode_tx_aggregated_next(unsigned n);
/** A chip that aggregates any unicast data frame whose descriptor carries a Block Ack field (the
 *  A-MPDU flag or a reorder size), reporting it WAS_AGGREGATED. Off by default; survives a start. */
void simnode_chip_agg_on_baparams(bool on);
/** Flags (MMDRV_TX_FLAG_*) the connection's populate step adds to every frame; 0 by default. */
void simnode_set_populate_flags(uint8_t flags);
/** The driver releases held frame @p i the chip never reported (its bus write failed, a page
 *  was invalid, or 15 s passed): a host fragment, or a DELBA a cut waits on, comes back as an
 *  untried TX status, as skbq.c reports one; any other frame is released silently. */
bool simnode_tx_drop_held(unsigned i);
/** Held frame @p i is released with no TX status at all, as a queue flush does. */
bool simnode_tx_forget_held(unsigned i);
/** The @p k-th TX packet allocation from now (1 the next) fails, once; 0 disarms. */
void simnode_tx_alloc_fail_at(unsigned k);
/** TX packet allocations since the simulator was loaded. */
unsigned simnode_tx_allocs(void);
/** On: TX packets come from the firmware's own pool (mmpktmem_heap.c, 20 blocks, as every
 *  mesh sdkconfig), pausing the TX path as it fills; off (default): heap, unlimited. */
void simnode_tx_pool(bool on);
/** The pool's flow control: true while it has the TX path paused. */
bool simnode_tx_pool_paused(void);
/** Blocks the pool has free now (mmhal_wlan_pktmem_tx_free). */
uint32_t simnode_tx_pool_free(void);
/** The reserve the firmware last asked the pool for (mmdrv_set_tx_pool_reserve). */
uint32_t simnode_tx_pool_reserve(void);

/* ---- driving ---------------------------------------------------------- */

/** Originate an 802.3 frame from the host side, through the real TX path. */
bool simnode_host_tx(const uint8_t da[6], const uint8_t sa[6],
                     const uint8_t *payload, uint16_t payload_len);
/** As simnode_host_tx, returning on the netif task: the event loop has not run yet. */
bool simnode_host_tx_nopump(const uint8_t da[6], const uint8_t sa[6],
                            const uint8_t *payload, uint16_t payload_len);
/** As simnode_host_tx on QoS TID @p tid, as mmwlan_tx_pkt sets it from its metadata. */
bool simnode_host_tx_tid(const uint8_t da[6], const uint8_t sa[6],
                         const uint8_t *payload, uint16_t payload_len, uint8_t tid);

/* ---- rate control (umac_rc.c is not linked) ---------------------------- */

/** The chain umac_rc_init_rate_table_data hands every data frame from now on, @p n (1-4)
 *  entries; 0 restores the default, which leaves the frame's table as its metadata had it
 *  (zero: no rate, no attempts). Survives a start. */
void simnode_set_rate_chain(const struct simnode_rate *chain, unsigned n);
/** umac_rc_init_rate_table_mgmt (group data, management, EAPOL) fills MCS0 at this primary
 *  width, @p mhz 1 or 2, 5 attempts, as umac_rc.c does; 0, the default, leaves the frame's table
 *  as its metadata had it. Survives a start. */
void simnode_set_mgmt_rate_bw(uint8_t mhz);
/** What umac_rc_feedback got for one frame: the chip's attempts and status, and its chain
 *  (attempts 0: entry unused). */
struct simnode_rcfb {
    uint16_t aid;
    uint8_t  attempts;
    uint8_t  status_flags;
    struct simnode_rate chain[4];
};
/** umac_rc_init_rate_table_data calls since the last clear, and the frame size the last got. */
unsigned simnode_rc_table_calls(void);
uint32_t simnode_rc_last_size(void);
unsigned simnode_rcfb_count(void);
const struct simnode_rcfb *simnode_rcfb_get(unsigned i);
void simnode_rc_clear(void);

/** The chip's own TX fragmentation threshold, as AT+FRAG sets it (umac_config); 0 none. */
void simnode_set_chip_frag_threshold(uint32_t octets);

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
/** As simnode_rx_flags, returning on the chip driver's task: the frame is read off the chip
 *  (its read order stamped, as pageset.c does) and queued; the event loop has not run. */
bool simnode_rx_flags_queued(const uint8_t *frame, uint16_t len, int16_t rssi, uint8_t rx_flags);

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
/** Blocks freed inside a critical section so far (packets included). */
unsigned simnode_frees_in_critical(void);
/** Run @p cb once, at the next TX packet allocation: between build_mgmt_frame()'s two passes. */
void simnode_set_tx_alloc_hook(void (*cb)(void));

/* ---- the event loop between another task's steps ------------------------
 *
 * A test's own calls stand for the tasks that are not the event loop (the netif and
 * batman engine TX, the chip driver's RX filter). These run @p cb, standing for the
 * loop, once, at an exact point inside such a task; the loop's own calls do not fire them. */

/** At the next read of a peer record's address (umac_sta_data_matches_peer_addr): the
 *  record pointer is loaded, the record is read after @p cb returns. */
void simnode_set_peer_read_hook(void (*cb)(void));
/** At the next mutex take (the forwarding glue's lock), before it is held. */
void simnode_set_lock_hook(void (*cb)(void));
/** In the RX filter, after it looked a sender's record up and before it writes that
 *  record's duplicate cache (at its own-address check, umac_interface_addr_matches_mac_addr). */
void simnode_set_rx_filter_hook(void (*cb)(void));
/** Once @p mac's station record is freed, the next station record allocated takes its
 *  address, as a heap gives a freed block to the next caller of its size. NULL disarms,
 *  freeing a block parked and never reused. False if @p mac is not a peer. */
bool simnode_recycle_peer_record(const uint8_t *mac);
unsigned simnode_rssi_calls(void);
bool simnode_rssi_for(const uint8_t *ta, int16_t *rssi);
unsigned simnode_timeouts_registered(void);
/** Make the next umac_core timeout registration fail, as an empty pool does. */
void simnode_fail_next_timeout(void);
/** umac_core timeouts registered and not yet fired or dequeued. */
unsigned simnode_timeouts_pending(void);
/** Pending timeouts with an argument inside station record @p rec (addresses only: it may be
 *  freed). A record del_peer freed must have none left. */
unsigned simnode_timeouts_holding(const void *rec);
/** Fire every umac_core timeout due at the current virtual time, earliest first,
 *  then run the event loop, as the core task would. @returns how many fired. */
unsigned simnode_run_timeouts(void);
/** Advance virtual time @p ms in 1 ms steps, firing timeouts as they fall due. */
void simnode_advance_run(uint32_t ms);

/** The +MESHPATH / table dump the AT command prints. */
int simnode_render_paths(char *buf, uint32_t len);

/* ---- BATMAN_V member mode (main/bat_port.c) ---------------------------- */

/** The gate AT+MESHBATMAN sets at boot (g_warthog_mesh_batman). */
void simnode_set_batman(bool on);

/** An 802.3 frame of any ethertype from the host side, as bat_port.c's mmwlan_tx_pkt:
 *  @p ra non-NULL sends to that peer only (mmwlan_tx_metadata.ra), NULL looks the DA up.
 *  @returns the mmwlan_status the datapath returned; the frame is consumed either way. */
int simnode_host_tx_eth(const uint8_t *ra, const uint8_t da[6], const uint8_t sa[6],
                        uint16_t ethertype, const uint8_t *payload, uint16_t payload_len);

/** A frame the extended RX callback took (the hook bat_port.c registers): the whole
 *  802.3 frame and the transmitter address the datapath passed with it. */
struct simnode_extrx {
    uint8_t  frame[1600];
    uint16_t len;       /* whole 802.3 frame, header included (frame holds the first 1600) */
    uint8_t  ta[6];
    bool     have_ta;
};
/** On: register the extended RX callback, which silences the raw one, as on the firmware.
 *  Off: the raw callback again. Survives simnode_start. */
void simnode_set_rx_ext_cb(bool on);
/** As simnode_set_rx_ext_cb, registered for VIF @p vif (an mmwlan_vif) only; bat_port.c,
 *  like simnode_set_rx_ext_cb, uses MMWLAN_VIF_UNSPECIFIED (STA and AP). */
void simnode_set_rx_ext_cb_vif(bool on, unsigned vif);
unsigned simnode_ext_rx_count(void);
const struct simnode_extrx *simnode_ext_rx_get(unsigned i);
void simnode_ext_rx_clear(void);

/** What rate control reports for peer @p mac: @p kbps when @p valid, else none (0). */
void simnode_set_peer_tput(const uint8_t mac[6], uint32_t kbps, bool valid);
struct mmwlan_mesh_peer_link;
/** umac_mesh_peer_links_snapshot, which mmwlan_mesh_query_peer_links runs: on the event loop
 *  (@p on_loop), else as from the port's task. The simulator runs no loop behind a wait, so
 *  off the loop only a failed post means anything: fill the queue first (simnode_evt_fill).
 *  @returns the mmwlan_status. */
int simnode_peer_links_query(struct mmwlan_mesh_peer_link *out, uint8_t max, uint8_t *count,
                             bool on_loop);
/** simnode_peer_links_query on the event loop; the count, 0 on failure. */
uint8_t simnode_peer_links(struct mmwlan_mesh_peer_link *out, uint8_t max);

#endif /* SIMNODE_H */
