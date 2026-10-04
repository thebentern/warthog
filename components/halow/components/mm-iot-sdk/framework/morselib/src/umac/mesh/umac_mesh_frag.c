/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * Host TX fragmentation arithmetic. Freestanding (libc only).
 */
#include "umac_mesh_frag.h"

#include <string.h>

#define FRAG_FCS_LEN 4u
/* SERVICE (16 bits, the longer of the two S1G may use) and BCC tail (6). */
#define FRAG_PHY_EXTRA_BITS 22u
/* A-MPDU delimiter. */
#define FRAG_DELIM_LEN 4u

/* N_SD, data subcarriers, by bandwidth: 1, 2, 4, 8, 16 MHz (802.11-2020 clause 23). */
static uint32_t frag_nsd_(uint8_t bw_mhz)
{
    switch (bw_mhz)
    {
        case 1:  return 24u;
        case 2:  return 52u;
        case 4:  return 108u;
        case 8:  return 234u;
        case 16: return 468u;
        default: return 0u;
    }
}

uint32_t umac_mesh_frag_ndbps(uint8_t bw_mhz, uint8_t mcs)
{
    /* N_BPSCS x R, times 6: BPSK 1/2, QPSK 1/2 and 3/4, 16-QAM 1/2 and 3/4, 64-QAM 2/3, 3/4
     * and 5/6, 256-QAM 3/4 and 5/6. */
    static const uint8_t k6[10] = { 3, 6, 9, 12, 18, 24, 27, 30, 36, 40 };
    const uint32_t nsd = frag_nsd_(bw_mhz);
    if (nsd == 0u)
    {
        return 0u;
    }
    if (mcs == 10u)
    {
        return bw_mhz == 1u ? nsd / 4u : 0u; /* MCS0 repeated twice */
    }
    if (mcs >= 10u || (nsd * k6[mcs]) % 6u != 0u)
    {
        return 0u; /* MCS9 at 2 MHz: not a whole number of bits, so not a valid rate */
    }
    return nsd * k6[mcs] / 6u;
}

uint32_t umac_mesh_frag_mpdu_cap(uint8_t bw_mhz, uint8_t mcs)
{
    const uint32_t ndbps = umac_mesh_frag_ndbps(bw_mhz, mcs);
    if (ndbps == 0u)
    {
        return 0u;
    }
    const uint32_t psdu = (UMAC_MESH_FRAG_MAX_SYMBOLS * ndbps - FRAG_PHY_EXTRA_BITS) / 8u;
    return (psdu & ~3u) - FRAG_DELIM_LEN - UMAC_MESH_FRAG_MORSE_MARGIN;
}

uint32_t umac_mesh_frag_chip_pns(uint32_t mpdu, uint32_t over, uint32_t lim)
{
    if (lim == 0u)
    {
        lim = umac_mesh_frag_mpdu_cap(1, 10);
    }
    if (mpdu <= lim)
    {
        return 1u;
    }
    const uint32_t chunk = lim > over + 1u ? (lim - over) & ~1u : 0u;
    uint32_t n = chunk != 0u && mpdu > over ? (mpdu - over + chunk - 1u) / chunk : UMAC_MESH_FRAG_MAX;
    if (n > UMAC_MESH_FRAG_MAX)
    {
        n = UMAC_MESH_FRAG_MAX;
    }
    return 1u + n;
}

uint32_t umac_mesh_frag_count(uint32_t over, uint32_t body, uint32_t lim)
{
    if (over + body <= lim)
    {
        return 1u;
    }
    const uint32_t chunk = lim > over + 1u ? (lim - over) & ~1u : 0u;
    const uint32_t n = chunk != 0u ? (body + chunk - 1u) / chunk : 0u;
    return n <= UMAC_MESH_FRAG_MAX ? n : 0u;
}

int umac_mesh_frag_slowest_mcs(uint8_t bw_mhz, uint32_t over, uint32_t body, uint32_t thr,
                               uint32_t max)
{
    for (uint8_t mcs = 0; mcs <= 9u; mcs++)
    {
        uint32_t lim = umac_mesh_frag_mpdu_cap(bw_mhz, mcs);
        if (lim == 0u)
        {
            continue;
        }
        lim = (thr != 0u && thr < lim) ? thr : lim;
        const uint32_t n = umac_mesh_frag_count(over, body, lim);
        if (n != 0u && n <= max)
        {
            return mcs;
        }
    }
    return -1;
}

void umac_mesh_frag_plan(const struct umac_mesh_frag_req *req, struct umac_mesh_frag_plan *out)
{
    memset(out, 0, sizeof(*out));
    out->n = 1;
    if (req == NULL || req->mode == UMAC_MESH_FRAG_OFF)
    {
        return;
    }
    const uint32_t max = (req->max_frags >= 2u && req->max_frags < UMAC_MESH_FRAG_MAX)
                             ? req->max_frags : UMAC_MESH_FRAG_MAX;
    const uint32_t over = (uint32_t)req->hdr_len + req->sec_len + FRAG_FCS_LEN;
    const uint32_t mpdu = over + req->body_len;
    uint32_t lim = UINT32_MAX;
    enum umac_mesh_frag_limit which = UMAC_MESH_FRAG_LIM_NONE;
    if (req->mode >= UMAC_MESH_FRAG_THRESH_MIN)
    {
        /* Even, as cfg80211 makes it; counted before encryption. */
        lim = (req->mode & ~1u) + req->sec_len;
        const uint32_t n = umac_mesh_frag_count(over, req->body_len, lim);
        if (n == 0u || n > max)
        {
            /* The least threshold that cuts it in max: an even body in each but the last. */
            lim = over + (((req->body_len + max - 1u) / max + 1u) & ~1u);
            out->clamped = true;
        }
        which = UMAC_MESH_FRAG_LIM_THRESH;
    }
    if (req->chip_thresh != 0u && req->chip_thresh < lim)
    {
        lim = req->chip_thresh;
        which = UMAC_MESH_FRAG_LIM_CHIP;
    }
    out->thr = which != UMAC_MESH_FRAG_LIM_NONE ? lim : 0u;
    if (req->rate_cap != 0u && req->rate_cap < lim)
    {
        lim = req->rate_cap;
        which = UMAC_MESH_FRAG_LIM_RATE;
    }
    if (which == UMAC_MESH_FRAG_LIM_NONE || mpdu <= lim)
    {
        out->clamped = false;
        return;
    }
    out->lim = which;
    out->mpdu_max = lim;
    const uint32_t n = umac_mesh_frag_count(over, req->body_len, lim);
    if (n < 2u || n > max)
    {
        out->n = 0; /* no fragment would fit, or more than max */
        return;
    }
    const uint32_t chunk = (lim - over) & ~1u;
    out->n = (uint8_t)n;
    out->chunk = (uint16_t)chunk;
    out->last = (uint16_t)(req->body_len - (n - 1u) * chunk);
}
