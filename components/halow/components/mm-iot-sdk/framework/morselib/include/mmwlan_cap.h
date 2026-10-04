/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * Warthog frame capture (AT+RXCAP, AT+TXCAP): the head of each data frame as the chip delivered
 * it, or as the host handed it to the chip, in a small ring. Freestanding (libc only).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MMWLAN_CAP_SLOTS 16u
#define MMWLAN_CAP_BYTES 64u

enum mmwlan_cap_dir
{
    MMWLAN_CAP_RX = 0,
    MMWLAN_CAP_TX = 1,
};

/* AT+TXCAP modes; AT+RXCAP uses 0 and 1. */
#define MMWLAN_CAP_OFF       0u
#define MMWLAN_CAP_HOST_FRAG 1u /* TX: host fragments (AT+HOSTFRAG) only; RX: every data frame */
#define MMWLAN_CAP_ALL_DATA  2u /* TX: every unicast data frame */

/* One capture. RX: the chip's rx_status flags, rate and signal. TX: the descriptor's flags, TID fields,
 * chain (mcs | log2 MHz << 4 | count << 8 | RTS << 12) and, once read, the chip's TX status. */
struct mmwlan_cap_rec
{
    uint32_t seq; /* 1 for the first frame captured since armed */
    uint32_t t_ms;
    uint32_t flags;
    uint32_t pkt_id;
    uint32_t st_flags;
    uint16_t len;
    uint16_t st_ampdu;
    uint16_t rate[4];
    int16_t rssi;
    uint8_t mcs;
    uint8_t bw_mhz;
    uint8_t tid;
    uint8_t tid_params;
    uint8_t host_frag;
    uint8_t st_tries;
    uint8_t st_done; /* 0 no status yet (or dropped: mmwlan_cap_st_lost), 1 the chip's, 2 untried */
    uint8_t n; /* octets kept in b[] */
    uint8_t b[MMWLAN_CAP_BYTES];
};

/* Armed or not, per direction; read bare by the hooks, so a capture that is off costs one load.
 * Named mmwlan* so libmorse's symbol mangler keeps it visible to main/. */
extern volatile uint32_t mmwlan_cap_mode[2];

/* Arm @p dir in @p mode, clearing the ring (OFF stops it, keeping it), for TA (RX) or RA (TX) @p addr
 * if not NULL. 1 done, 0 no memory, -1 the ring busy (try again). */
int mmwlan_cap_arm(unsigned dir, uint32_t mode, const uint8_t *addr);

/* The filter in force: true and its address in @p addr, or false for any. */
bool mmwlan_cap_filter(unsigned dir, uint8_t addr[6]);

/* A received frame, before anything parses it. Data frames that pass the filter are kept. */
void mmwlan_cap_rx(const uint8_t *frame, uint32_t len, uint32_t flags, uint8_t mcs, uint8_t bw_mhz,
                   int16_t rssi, uint32_t t_ms);

/* A frame handed to the chip, its descriptor fields as sent. Kept per the TX mode and filter. */
void mmwlan_cap_tx(const uint8_t *frame, uint32_t len, uint32_t flags, uint8_t tid,
                   uint8_t tid_params, const uint16_t rate[4], uint32_t pkt_id, bool host_frag,
                   uint32_t t_ms);

/* The chip's TX status for packet @p pkt_id on @p tid, recorded on the newest capture of it; one that
 * finds the ring busy is dropped and counted (mmwlan_cap_st_lost). */
void mmwlan_cap_tx_status(uint32_t pkt_id, uint8_t tid, uint32_t flags, uint8_t tries,
                          uint16_t ampdu);

/* A frame the driver released with no chip status (a host fragment or a DELBA reported untried). */
void mmwlan_cap_tx_untried(uint32_t pkt_id, uint8_t tid);

/* TX statuses dropped since the TX ring was armed because it was busy. */
uint32_t mmwlan_cap_st_lost(void);

/* Copies kept captures with seq above @p after, oldest first, at most @p max; frames seen and
 * lost (the ring busy) in @p seen and @p lost. -1: the ring is busy, try again. */
int mmwlan_cap_read(unsigned dir, uint32_t after, struct mmwlan_cap_rec *out, unsigned max,
                    uint32_t *seen, uint32_t *lost);

/* One AT line for @p r (CR LF included); the snprintf convention. */
int mmwlan_cap_line(char *buf, size_t len, unsigned dir, const struct mmwlan_cap_rec *r);
