/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * Host TX fragmentation (AT+HOSTFRAG): whether a unicast MSDU is cut, into how many
 * fragments and how long each may be. Freestanding (libc only) so the host tests run it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* AT+HOSTFRAG: off, auto, or a threshold in octets as Linux 'iw phy set frag' takes it. */
#define UMAC_MESH_FRAG_OFF        0u
#define UMAC_MESH_FRAG_AUTO       1u
#define UMAC_MESH_FRAG_THRESH_MIN 256u
#define UMAC_MESH_FRAG_THRESH_MAX 2346u

/* Fragment numbers are 4 bits. */
#define UMAC_MESH_FRAG_MAX 16u

/* Fragments of one MSDU chip firmware 1.17.6 delivers when it seals them (HW_ENC); measured on air
 * 2026-10-03: a third arrives with fragment 1's More Fragments cleared and 32 octets more. */
#define UMAC_MESH_FRAG_CHIP_MAX 2u

/* An S1G PPDU's data field holds at most this many OFDM symbols (its SIG Length is 9 bits). */
#define UMAC_MESH_FRAG_MAX_SYMBOLS 511u

/* Morse's Linux driver keeps a 1 MHz MCS0 beacon this far under the symbol limit so the chip
 * does not fragment it (beacon.c FRAGMENTATION_OVERHEAD); every cap keeps the same margin. */
#define UMAC_MESH_FRAG_MORSE_MARGIN 36u

/* TX pool blocks kept back while AT+HOSTFRAG cuts in up to 16 (host tests' WARTHOG_MESH_HOSTFRAG_ANY):
 * a 1500-octet MSDU's extra fragments at 1 MHz MCS10 (5) or at =256 (6). */
#define UMAC_MESH_FRAG_POOL_RESERVE 6u

/* After a frame to a peer needed cutting, ADDBA on its TID is held off this long: mac80211's
 * spacing of ADDBA retries to a peer that refuses them (sta_info.h HT_AGG_RETRIES_PERIOD). */
#define UMAC_MESH_FRAG_BA_HOLD_MS 15000u

/* The first MSDU cut after a session ends waits for its DELBA's TX status and this long more:
 * mac80211 ends its RX session in deferred work, after fragments already in its RX path. */
#define UMAC_MESH_FRAG_BA_GUARD_MS 20u

/* It waits at most this long for that TX status (a DELBA dropped unsent never reports one). */
#define UMAC_MESH_FRAG_BA_WAIT_MAX_MS 500u

/* Data bits per OFDM symbol, one spatial stream, at @p bw_mhz (1, 2, 4, 8 or 16) and S1G
 * MCS @p mcs (0-9; 10 at 1 MHz only); 0 for a rate that does not exist. */
uint32_t umac_mesh_frag_ndbps(uint8_t bw_mhz, uint8_t mcs);

/* The longest MPDU (MAC header to FCS) sent whole at that rate: UMAC_MESH_FRAG_MAX_SYMBOLS less
 * SERVICE, tail, an A-MPDU delimiter and UMAC_MESH_FRAG_MORSE_MARGIN; 0 for an unknown rate. */
uint32_t umac_mesh_frag_mpdu_cap(uint8_t bw_mhz, uint8_t mcs);

/* TX PNs the chip may draw for a whole MPDU of @p mpdu octets (@p over of them header, CCMP, FCS):
 * 1 within @p lim (0: the 1 MHz MCS10 cap), else 1 plus the most fragments it could cut (16 max). */
uint32_t umac_mesh_frag_chip_pns(uint32_t mpdu, uint32_t over, uint32_t lim);

/* Which limit made an MSDU fragment. */
enum umac_mesh_frag_limit
{
    UMAC_MESH_FRAG_LIM_NONE = 0,
    UMAC_MESH_FRAG_LIM_THRESH, /* AT+HOSTFRAG=<n> */
    UMAC_MESH_FRAG_LIM_CHIP,   /* the chip's own threshold (AT+FRAG) */
    UMAC_MESH_FRAG_LIM_RATE,   /* the rate rate control chose */
};

struct umac_mesh_frag_req
{
    uint32_t mode;        /* AT+HOSTFRAG */
    uint32_t chip_thresh; /* the chip's threshold, 0 none */
    uint32_t rate_cap;    /* umac_mesh_frag_mpdu_cap at the chosen rate, 0 unknown */
    uint16_t hdr_len;     /* MAC header and QoS Control */
    uint16_t sec_len;     /* CCMP header and MIC in each MPDU, 0 unprotected */
    uint16_t body_len;    /* Mesh Control, LLC and payload */
    uint8_t max_frags;    /* most fragments it may be cut into, 2-16; 0: 16 */
};

struct umac_mesh_frag_plan
{
    uint8_t n;         /* fragments; 1 sends it whole, 0 sends it whole though over a limit */
    uint16_t chunk;    /* body octets in every fragment but the last (even) */
    uint16_t last;     /* body octets in the last */
    uint32_t mpdu_max; /* the binding limit, MAC header to FCS; 0 none */
    enum umac_mesh_frag_limit lim;
    uint32_t thr;      /* the least of the thresholds (n as raised, the chip's), MPDU octets; 0 none */
    bool clamped;      /* n would have cut it into more than max_frags: raised so it does not */
};

/* Cut over the least of the chip's threshold, the rate's cap and Linux's rule for n (raised if it
 * needs more than max_frags); more than max_frags under the others: n 0, lim says which. */
void umac_mesh_frag_plan(const struct umac_mesh_frag_req *req, struct umac_mesh_frag_plan *out);

/* Fragments an MSDU of @p body octets needs under an MPDU limit @p lim, @p over octets of header,
 * CCMP and FCS in each: 1 whole, 0 if no fragment fits or more than 16 would be needed. */
uint32_t umac_mesh_frag_count(uint32_t over, uint32_t body, uint32_t lim);

/* The slowest MCS (0-9) at @p bw_mhz at which such an MSDU needs at most @p max fragments under
 * that rate's cap and @p thr (0 none); -1 if none. */
int umac_mesh_frag_slowest_mcs(uint8_t bw_mhz, uint32_t over, uint32_t body, uint32_t thr,
                               uint32_t max);
