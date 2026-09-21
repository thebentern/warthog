/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 */
#include "umac_mesh_rmc.h"

#include <string.h>

/* Wrap-safe "now is at or past t". */
static bool past_(uint32_t now_ms, uint32_t t_ms)
{
    return (int32_t)(now_ms - t_ms) >= 0;
}

static uint32_t bucket_(const uint8_t *sa)
{
    /* mac80211 hashes on the last octet alone; fold all six so that two
     * sources differing only in a high octet do not share a bucket. */
    uint32_t h = (uint32_t)sa[0] ^ sa[1] ^ sa[2] ^ sa[3] ^ sa[4] ^ sa[5];
    return h % UMAC_MESH_RMC_BUCKETS;
}

void umac_mesh_rmc_init(struct umac_mesh_rmc *rmc)
{
    if (rmc != NULL)
    {
        memset(rmc, 0, sizeof(*rmc));
    }
}

bool umac_mesh_rmc_check(struct umac_mesh_rmc *rmc, const uint8_t *sa, uint32_t seq,
                         uint32_t now_ms)
{
    if (rmc == NULL || sa == NULL)
    {
        return false;
    }
    struct umac_mesh_rmc_entry *q = rmc->e[bucket_(sa)];
    struct umac_mesh_rmc_entry *slot = NULL;
    struct umac_mesh_rmc_entry *oldest = &q[0];

    for (uint32_t i = 0; i < UMAC_MESH_RMC_QUEUE; i++)
    {
        struct umac_mesh_rmc_entry *e = &q[i];
        if (e->used && past_(now_ms, e->exp_ms))
        {
            e->used = false; /* expire lazily on the way through */
        }
        if (!e->used)
        {
            if (slot == NULL)
            {
                slot = e;
            }
            continue;
        }
        if (e->seq == seq && memcmp(e->sa, sa, 6) == 0)
        {
            return true; /* seen it; do not extend its life */
        }
        if ((int32_t)(e->exp_ms - oldest->exp_ms) < 0 || !oldest->used)
        {
            oldest = e;
        }
    }
    if (slot == NULL)
    {
        slot = oldest; /* bucket full: the oldest live entry makes room */
    }
    slot->seq = seq;
    slot->exp_ms = now_ms + UMAC_MESH_RMC_TIMEOUT_MS;
    memcpy(slot->sa, sa, 6);
    slot->used = true;
    return false;
}

uint32_t umac_mesh_rmc_count(const struct umac_mesh_rmc *rmc, uint32_t now_ms)
{
    if (rmc == NULL)
    {
        return 0;
    }
    uint32_t n = 0;
    for (uint32_t b = 0; b < UMAC_MESH_RMC_BUCKETS; b++)
    {
        for (uint32_t i = 0; i < UMAC_MESH_RMC_QUEUE; i++)
        {
            const struct umac_mesh_rmc_entry *e = &rmc->e[b][i];
            if (e->used && !past_(now_ms, e->exp_ms))
            {
                n++;
            }
        }
    }
    return n;
}
