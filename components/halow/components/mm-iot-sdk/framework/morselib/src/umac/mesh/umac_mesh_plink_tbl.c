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
