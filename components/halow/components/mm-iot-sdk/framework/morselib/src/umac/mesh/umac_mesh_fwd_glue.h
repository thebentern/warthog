/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * Glue between the freestanding forwarding engines and the SDK: owns the
 * tables and their lock, moves packets, sends action frames. Everything that
 * decides lives in umac_mesh_fwd.c / umac_mesh_hwmp_relay.c; this file only
 * carries the decisions out. Not freestanding by design.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct umac_data;
struct umac_sta_data;
struct dot11_hdr;
struct dot11_data_hdr;
struct mmpkt;
struct mmpktview;
struct umac_mesh_ctrl;
struct umac_mesh_fwd_rx_result;

void umac_mesh_fwd_glue_init(void);

/** Decide what to do with a received mesh data frame (Mesh Control parsed). */
void umac_mesh_fwd_glue_rx(struct umac_data *umacd, struct umac_sta_data *stad,
                           const struct dot11_hdr *hdr, const struct dot11_data_hdr *dhdr,
                           const struct umac_mesh_ctrl *mc,
                           struct umac_mesh_fwd_rx_result *out);

/** Queue a copy of @p body (LLC already stripped) toward the decided next hop. */
void umac_mesh_fwd_glue_forward(struct umac_data *umacd, struct mmpktview *body,
                                uint16_t ethertype, const struct dot11_hdr *hdr,
                                const struct dot11_data_hdr *dhdr,
                                const struct umac_mesh_fwd_rx_result *r);

/** Send the PERR a NO_PATH decision asked for. */
void umac_mesh_fwd_glue_send_perr(const struct umac_mesh_fwd_rx_result *r);

/** Locally originated frame: fill the sidecar for proxying, kick discovery. */
void umac_mesh_fwd_glue_tx_classify(struct mmpkt *txbuf, const uint8_t *da, const uint8_t *sa);

/** No STA for a unicast destination: if discovery is what is missing, take
 *  ownership of @p txbuf until the PREP arrives (or 2 s pass). @returns true
 *  if held; the caller must then not release it. */
bool umac_mesh_fwd_glue_tx_pending(struct umac_data *umacd, struct mmpkt *txbuf, const uint8_t *dest);

/** Periodic (2 s): release held frames whose discovery never answered. */
void umac_mesh_fwd_glue_tick(void);

/** The glue's table lock, for the one HWMP sequence-number writer outside
 *  this file (the keepalive PREQ in umac_mesh.c). */
void umac_mesh_fwd_glue_lock(void);
void umac_mesh_fwd_glue_unlock(void);

/** Next hop for a non-neighbour destination into @p out; false = none (a PREQ was sent). */
bool umac_mesh_fwd_glue_next_hop(const uint8_t *dest, uint8_t out[6]);

/** HWMP action body received; carries out what the relay engine decides. */
void umac_mesh_fwd_glue_hwmp_rx(const uint8_t *body, uint16_t len, const uint8_t *ta,
                                uint32_t *own_sn, bool is_protected, bool is_group_addressed);

/** A peer link is gone. */
void umac_mesh_fwd_glue_peer_lost(const uint8_t *peer);

/** AT+MESHPATH? */
int umac_mesh_fwd_glue_render(char *buf, uint32_t len);
