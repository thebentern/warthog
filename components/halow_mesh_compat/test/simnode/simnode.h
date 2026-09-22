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
 * timing, no interference, no chip behaviour, and nothing about what a real
 * mac80211 peer does with the bytes. A green run here means the firmware's
 * own logic is consistent, not that it works on the air.
 */
#ifndef SIMNODE_H
#define SIMNODE_H

#include <stdbool.h>
#include <stdint.h>

/** A frame the firmware handed to the chip: the exact bytes that would have
 *  gone on the air. */
struct simnode_frame {
    uint8_t  bytes[512];
    uint16_t len;
    bool     is_mgmt;
    uint8_t  vif_id;
    uint8_t  tid;
};

/* ---- lifecycle -------------------------------------------------------- */

/** Bring one node up with @p mac. Gates default off, as the firmware does. */
bool simnode_start(const uint8_t mac[6]);
void simnode_stop(void);

/** The gates AT+MESHFWD / MESHBRIDGE / MESHGRP / MESHSEC set. */
void simnode_set_gates(bool fwd, bool bridge, bool grp_std, bool secure);

/* ---- peers ------------------------------------------------------------ */

/** Add an ESTAB peer, as peering would. */
bool simnode_add_peer(const uint8_t mac[6]);
/** Drop a peer, as the peering watchdog does. */
void simnode_del_peer(const uint8_t mac[6]);

/* ---- driving ---------------------------------------------------------- */

/** Originate an 802.3 frame from the host side, through the real TX path. */
bool simnode_host_tx(const uint8_t da[6], const uint8_t sa[6],
                     const uint8_t *payload, uint16_t payload_len);

/** Inject a received 802.11 frame, through the real RX path. */
bool simnode_rx(const uint8_t *frame, uint16_t len, int16_t rssi);

/** Run the 2 s service tick (held-frame flush, peering watchdog, rekey). */
void simnode_tick(void);

/** Run the event loop's datapath work, which is what actually sends queued
 *  frames. host_tx, rx and tick already pump; call it directly after doing
 *  something that queues work by another route. */
void simnode_pump(void);

/* ---- virtual time (fake_rtos.c) --------------------------------------- */
uint32_t mmosal_get_time_ms(void);
void simnode_set_time_ms(uint32_t t);
void simnode_advance_ms(uint32_t d);

/* ---- observation ------------------------------------------------------ */

unsigned simnode_outbox_count(void);
unsigned simnode_outbox_dropped(void);
void simnode_outbox_clear(void);
const struct simnode_frame *simnode_outbox_get(unsigned i);

/** Calls into a radio-stack stub, so a test can assert a mesh-only path never
 *  fell through into code the simulator does not model. */
unsigned simnode_stub_hits(const char *name);
void simnode_stub_reset(void);

/** Live allocations, for leak assertions. */
unsigned simnode_live_allocs(void);
unsigned simnode_in_critical(void);
unsigned simnode_rssi_calls(void);
bool simnode_rssi_for(const uint8_t *ta, int16_t *rssi);
unsigned simnode_timeouts_registered(void);

/** The +MESHPATH / table dump the AT command prints. */
int simnode_render_paths(char *buf, uint32_t len);

#endif /* SIMNODE_H */
