/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * In-process multi-engine simulator for the BATMAN_V engine (main/bat/).
 *
 * Up to BAT_SIM_MAX_NODES engines share one process and one virtual clock. Each node
 * owns a struct bat allocated with bat_ctx_size() and gets its own ops: link TX is
 * queued and delivered 1 ms later (so no engine is ever re-entered), a broadcast
 * reaches every node j whose directional link from the sender is up (minus loss), a
 * unicast reaches the node owning the destination hard address if that link is up and
 * otherwise ops->tx returns BAT_TX_NOPEER, link_tput is the directional link's units
 * (0 when down), and every node draws from its own deterministic xorshift32 unless a
 * test scripts it with bat_sim_set_rand().
 *
 * Node i has hard address 02:5a:00:00:00:<i+1> and soft address 06:5a:00:00:00:<i+1>.
 */
#ifndef BAT_SIM_H
#define BAT_SIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bat.h"

#define BAT_SIM_MAX_NODES 8
#define BAT_SIM_BCAST     0xFFFFFFFFu   /* bat_sim_frame.to of a broadcast */
#define BAT_SIM_NONE      0xFFFFFFFEu   /* bat_sim_frame.to of a unicast nobody owns */

struct bat_sim;                                   /* up to 8 nodes */
struct bat_sim *bat_sim_new(unsigned nodes, uint32_t seed);
void  bat_sim_free(struct bat_sim *s);
struct bat_config *bat_sim_cfg(struct bat_sim *s, unsigned i);   /* edit before start */
void  bat_sim_start(struct bat_sim *s, unsigned i);              /* bat_init with sim ops */
void  bat_sim_restart(struct bat_sim *s, unsigned i);            /* fresh ctx, new seqnos */
void  bat_sim_stop(struct bat_sim *s, unsigned i);               /* silent: not ticked, RX dropped */
void  bat_sim_link(struct bat_sim *s, unsigned a, unsigned b, bool up, uint32_t tput_units);
void  bat_sim_link_dir(struct bat_sim *s, unsigned from, unsigned to, bool up, uint32_t tput_units);
void  bat_sim_loss(struct bat_sim *s, unsigned from, unsigned to, uint32_t permille);
void  bat_sim_set_rand(struct bat_sim *s, unsigned i, uint32_t (*fn)(void *), void *arg);
void  bat_sim_run(struct bat_sim *s, uint32_t ms);               /* virtual time, 1 ms steps */
uint32_t bat_sim_now(const struct bat_sim *s);
struct bat *bat_sim_engine(struct bat_sim *s, unsigned i);
const uint8_t *bat_sim_hard(const struct bat_sim *s, unsigned i); /* 02:5a:00:00:00:<i+1> */
const uint8_t *bat_sim_soft(const struct bat_sim *s, unsigned i); /* 06:5a:00:00:00:<i+1> */
int   bat_sim_soft_tx(struct bat_sim *s, unsigned i, const uint8_t *frame, size_t len);
struct bat_sim_frame { unsigned from, to; uint32_t t; size_t len; uint8_t bytes[1600]; };
unsigned bat_sim_soft_rx_count(const struct bat_sim *s, unsigned i);
const struct bat_sim_frame *bat_sim_soft_rx_get(const struct bat_sim *s, unsigned i, unsigned k);
void  bat_sim_soft_rx_clear(struct bat_sim *s, unsigned i);
void  bat_sim_inject(struct bat_sim *s, unsigned to, const uint8_t *link_frame, size_t len);
void  bat_sim_capture(struct bat_sim *s, bool on);               /* record every link TX */
unsigned bat_sim_captured(const struct bat_sim *s);
const struct bat_sim_frame *bat_sim_captured_get(const struct bat_sim *s, unsigned k);
size_t bat_sim_mk_eth(uint8_t *out, const uint8_t dst[6], const uint8_t src[6], uint16_t type,
                      size_t payload_len, uint8_t fill);

/* Additions beyond the design's list. */
/* Virtual time before any node starts (default 0); lets a test run across the 2^32 wrap. */
void  bat_sim_set_now(struct bat_sim *s, uint32_t now);
/* Clear the capture log (capture stays on or off as it was). */
void  bat_sim_capture_clear(struct bat_sim *s);
/* Counter of node i (0 if not started). */
uint32_t bat_sim_counter(struct bat_sim *s, unsigned i, enum bat_counter c);
/* Node index owning hard address @hard, or -1. */
int   bat_sim_node_of(const struct bat_sim *s, const uint8_t hard[6]);

#endif
