/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * See umac_mesh_plink_tbl.h. Freestanding on purpose -- libc only.
 */
#include "umac_mesh_plink_tbl.h"

#include <stdio.h>
#include <string.h>

void mpm_table_init(struct mpm_table *t)
{
    if (t != NULL)
    {
        memset(t, 0, sizeof(*t));
    }
}

struct mpm_link *mpm_table_find(struct mpm_table *t, const uint8_t *addr)
{
    if (t == NULL || addr == NULL)
    {
        return NULL;
    }
    for (int i = 0; i < MPM_MAX_LINKS; i++)
    {
        if (t->links[i].used && memcmp(t->links[i].addr, addr, MPM_ADDR_LEN) == 0)
        {
            return &t->links[i];
        }
    }
    return NULL;
}

struct mpm_link *mpm_table_get_or_create(struct mpm_table *t, const uint8_t *addr, uint16_t llid,
                                         uint32_t now_ms)
{
    if (t == NULL || addr == NULL)
    {
        return NULL;
    }

    struct mpm_link *l = mpm_table_find(t, addr);
    if (l != NULL)
    {
        return l; /* keep the llid a handshake in progress is already using */
    }

    for (int i = 0; i < MPM_MAX_LINKS; i++)
    {
        if (!t->links[i].used)
        {
            memset(&t->links[i], 0, sizeof(t->links[i]));
            memcpy(t->links[i].addr, addr, MPM_ADDR_LEN);
            t->links[i].llid = llid;
            t->links[i].last_heard_ms = now_ms;
            t->links[i].used = true;
            return &t->links[i];
        }
    }

    t->no_slot++;
    return NULL;
}

void mpm_table_release(struct mpm_table *t, struct mpm_link *l)
{
    (void)t;
    if (l != NULL)
    {
        memset(l, 0, sizeof(*l));
    }
}

bool mpm_table_make_room(struct mpm_table *t, const uint8_t *addr,
                         uint8_t out_addr[MPM_ADDR_LEN], uint16_t *out_llid)
{
    if (t == NULL || addr == NULL || mpm_table_find(t, addr) != NULL)
    {
        return false;
    }
    struct mpm_link *idle = NULL;
    for (int i = 0; i < MPM_MAX_LINKS; i++)
    {
        struct mpm_link *l = &t->links[i];
        if (!l->used)
        {
            return false; /* a free slot: nothing need go */
        }
        if (!l->estab && l->plid == 0 && (idle == NULL || l->opens > idle->opens))
        {
            idle = l;
        }
    }
    if (idle == NULL)
    {
        return false;
    }
    if (out_addr != NULL)
    {
        memcpy(out_addr, idle->addr, MPM_ADDR_LEN);
    }
    if (out_llid != NULL)
    {
        *out_llid = idle->llid;
    }
    mpm_table_release(t, idle);
    return true;
}

uint8_t mpm_table_estab_count(const struct mpm_table *t)
{
    uint8_t n = 0;
    if (t == NULL)
    {
        return 0;
    }
    for (int i = 0; i < MPM_MAX_LINKS; i++)
    {
        if (t->links[i].used && t->links[i].estab)
        {
            n++;
        }
    }
    return n;
}

int mpm_table_expire(struct mpm_table *t, uint32_t now_ms, uint32_t timeout_ms,
                     uint8_t out_addrs[][MPM_ADDR_LEN], int max_out)
{
    int n_out = 0;
    if (t == NULL)
    {
        return 0;
    }
    for (int i = 0; i < MPM_MAX_LINKS; i++)
    {
        if (!t->links[i].used || t->links[i].last_heard_ms == 0)
        {
            continue;
        }
        /* Unsigned difference, so a wrapped millisecond clock still measures
         * the true elapsed interval rather than a huge positive one. */
        if ((uint32_t)(now_ms - t->links[i].last_heard_ms) < timeout_ms)
        {
            continue;
        }
        if (out_addrs != NULL && n_out < max_out)
        {
            memcpy(out_addrs[n_out], t->links[i].addr, MPM_ADDR_LEN);
            n_out++;
        }
        mpm_table_release(t, &t->links[i]);
        t->expired++;
    }
    return n_out;
}

/* Is @p addr stamped less than @p hold_ms before @p now_ms? Unsigned elapsed
 * time, so a wrapped clock is safe; lapsed stamps are freed on the way. */
static bool stamp_live_(struct mpm_stamp *s, int n, const uint8_t *addr, uint32_t now_ms,
                        uint32_t hold_ms)
{
    bool live = false;
    for (int i = 0; i < n; i++)
    {
        if (!s[i].used)
        {
            continue;
        }
        if ((uint32_t)(now_ms - s[i].since_ms) >= hold_ms)
        {
            s[i].used = false;
            continue;
        }
        if (memcmp(s[i].addr, addr, MPM_ADDR_LEN) == 0)
        {
            live = true;
        }
    }
    return live;
}

/* Stamp @p addr at @p now_ms in its own entry, else a free one. False, and
 * nothing evicted, when every entry is live for another address. */
static bool stamp_set_(struct mpm_stamp *s, int n, const uint8_t *addr, uint32_t now_ms)
{
    struct mpm_stamp *slot = NULL;
    for (int i = 0; i < n && slot == NULL; i++)
    {
        if (s[i].used && memcmp(s[i].addr, addr, MPM_ADDR_LEN) == 0)
        {
            slot = &s[i];
        }
    }
    for (int i = 0; i < n && slot == NULL; i++)
    {
        if (!s[i].used)
        {
            slot = &s[i];
        }
    }
    if (slot == NULL)
    {
        return false;
    }
    memcpy(slot->addr, addr, MPM_ADDR_LEN);
    slot->since_ms = now_ms;
    slot->used = true;
    return true;
}

/* Every entry in use? Call after stamp_live_() has freed the lapsed ones. */
static bool stamp_full_(const struct mpm_stamp *s, int n)
{
    for (int i = 0; i < n; i++)
    {
        if (!s[i].used)
        {
            return false;
        }
    }
    return true;
}

void mpm_table_refuse(struct mpm_table *t, const uint8_t *addr, uint32_t now_ms,
                      uint32_t holdoff_ms)
{
    if (t == NULL || addr == NULL)
    {
        return;
    }
    (void)stamp_live_(t->refused, MPM_MAX_LINKS, addr, now_ms, holdoff_ms); /* frees lapsed */
    if (!stamp_set_(t->refused, MPM_MAX_LINKS, addr, now_ms))
    {
        /* No entry can hold it: hold off every address instead, for its full term. */
        t->refused_over.since_ms = now_ms;
        t->refused_over.used = true;
    }
}

void mpm_table_unrefuse(struct mpm_table *t, const uint8_t *addr)
{
    if (t == NULL || addr == NULL)
    {
        return;
    }
    for (int i = 0; i < MPM_MAX_LINKS; i++)
    {
        if (t->refused[i].used && memcmp(t->refused[i].addr, addr, MPM_ADDR_LEN) == 0)
        {
            t->refused[i].used = false;
        }
    }
}

bool mpm_table_refused(struct mpm_table *t, const uint8_t *addr, uint32_t now_ms,
                       uint32_t holdoff_ms)
{
    if (t == NULL || addr == NULL)
    {
        return false;
    }
    if (stamp_live_(t->refused, MPM_MAX_LINKS, addr, now_ms, holdoff_ms))
    {
        return true;
    }
    if (t->refused_over.used && (uint32_t)(now_ms - t->refused_over.since_ms) >= holdoff_ms)
    {
        t->refused_over.used = false;
    }
    /* No room to record this address if it refuses us too, so do not ask it. */
    return t->refused_over.used || stamp_full_(t->refused, MPM_MAX_LINKS);
}

bool mpm_table_quiet_due(struct mpm_table *t, const uint8_t *addr, uint32_t now_ms,
                         uint32_t period_ms)
{
    if (t == NULL || addr == NULL ||
        stamp_live_(t->quiet, MPM_MAX_QUIET, addr, now_ms, period_ms))
    {
        return false;
    }
    return stamp_set_(t->quiet, MPM_MAX_QUIET, addr, now_ms);
}

void mpm_table_sae_hold(struct mpm_table *t, const uint8_t *addr, uint32_t now_ms,
                        uint32_t hold_ms)
{
    if (t == NULL || addr == NULL)
    {
        return;
    }
    (void)stamp_live_(t->sae_held, MPM_MAX_SAE_HELD, addr, now_ms, hold_ms); /* frees lapsed */
    if (stamp_set_(t->sae_held, MPM_MAX_SAE_HELD, addr, now_ms))
    {
        return;
    }
    struct mpm_stamp *oldest = &t->sae_held[0];
    for (int i = 1; i < MPM_MAX_SAE_HELD; i++)
    {
        if ((uint32_t)(now_ms - t->sae_held[i].since_ms) > (uint32_t)(now_ms - oldest->since_ms))
        {
            oldest = &t->sae_held[i];
        }
    }
    memcpy(oldest->addr, addr, MPM_ADDR_LEN);
    oldest->since_ms = now_ms;
}

bool mpm_table_sae_held(struct mpm_table *t, const uint8_t *addr, uint32_t now_ms,
                        uint32_t hold_ms)
{
    return t != NULL && addr != NULL &&
           stamp_live_(t->sae_held, MPM_MAX_SAE_HELD, addr, now_ms, hold_ms);
}

void mpm_table_sae_release(struct mpm_table *t, const uint8_t *addr)
{
    if (t == NULL || addr == NULL)
    {
        return;
    }
    for (int i = 0; i < MPM_MAX_SAE_HELD; i++)
    {
        if (t->sae_held[i].used && memcmp(t->sae_held[i].addr, addr, MPM_ADDR_LEN) == 0)
        {
            t->sae_held[i].used = false;
        }
    }
}

void mpm_table_render(const struct mpm_table *t, char *out, uint32_t out_len)
{
    if (out == NULL || out_len == 0)
    {
        return;
    }
    out[0] = '\0';
    if (t == NULL)
    {
        return;
    }

    uint32_t w = 0;
    for (int i = 0; i < MPM_MAX_LINKS; i++)
    {
        if (!t->links[i].used || w + 56u >= out_len)
        {
            continue;
        }
        int n = snprintf(out + w, out_len - w, "%02x%02x%02x llid=%u plid=%u estab=%u opens=%u; ",
                         t->links[i].addr[3], t->links[i].addr[4], t->links[i].addr[5],
                         (unsigned)t->links[i].llid, (unsigned)t->links[i].plid,
                         (unsigned)t->links[i].estab, (unsigned)t->links[i].opens);
        if (n <= 0)
        {
            break;
        }
        w += (uint32_t)n;
    }
    if (w == 0)
    {
        snprintf(out, out_len, "(none) ");
    }
}
