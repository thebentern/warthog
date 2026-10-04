/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * Frame capture rings (AT+RXCAP, AT+TXCAP). Freestanding (libc only). Writers run on the driver
 * and TX tasks and never wait: one that finds the ring busy drops its capture, counted lost.
 */
#include "mmwlan_cap.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile uint32_t mmwlan_cap_mode[2];

struct cap_ring
{
    uint32_t lock;
    uint32_t seen, lost, st_lost, next;
    bool filtered;
    uint8_t addr[6];
    struct mmwlan_cap_rec rec[MMWLAN_CAP_SLOTS];
};

static struct cap_ring *volatile s_ring[2];

static bool cap_trylock_(struct cap_ring *r)
{
    return __atomic_exchange_n(&r->lock, 1u, __ATOMIC_ACQUIRE) == 0u;
}

static void cap_unlock_(struct cap_ring *r)
{
    __atomic_store_n(&r->lock, 0u, __ATOMIC_RELEASE);
}

int mmwlan_cap_arm(unsigned dir, uint32_t mode, const uint8_t *addr)
{
    if (dir > MMWLAN_CAP_TX)
    {
        return 0;
    }
    __atomic_store_n(&mmwlan_cap_mode[dir], MMWLAN_CAP_OFF, __ATOMIC_RELEASE);
    if (mode == MMWLAN_CAP_OFF)
    {
        return 1;
    }
    struct cap_ring *r = s_ring[dir];
    if (r == NULL)
    {
        r = (struct cap_ring *)calloc(1, sizeof(*r));
        if (r == NULL)
        {
            return 0;
        }
        __atomic_store_n(&s_ring[dir], r, __ATOMIC_RELEASE);
    }
    if (!cap_trylock_(r))
    {
        return -1; /* a writer holds it: never spun on, its task may be the one waiting */
    }
    memset(&r->seen, 0, sizeof(*r) - offsetof(struct cap_ring, seen));
    r->filtered = addr != NULL;
    if (addr != NULL)
    {
        memcpy(r->addr, addr, sizeof(r->addr));
    }
    cap_unlock_(r);
    __atomic_store_n(&mmwlan_cap_mode[dir], mode, __ATOMIC_RELEASE);
    return 1;
}

bool mmwlan_cap_filter(unsigned dir, uint8_t addr[6])
{
    struct cap_ring *r = dir <= MMWLAN_CAP_TX ? s_ring[dir] : NULL;
    if (r == NULL || !r->filtered)
    {
        return false;
    }
    memcpy(addr, r->addr, 6);
    return true;
}

/* The slot a new capture takes, the ring locked; NULL (counted lost) if busy or filtered out. */
static struct mmwlan_cap_rec *cap_begin_(unsigned dir, const uint8_t *frame, uint32_t len,
                                         unsigned addr_off, struct cap_ring **out)
{
    struct cap_ring *r = __atomic_load_n(&s_ring[dir], __ATOMIC_ACQUIRE);
    if (r == NULL || frame == NULL || len < 10u + 6u * (addr_off == 10u))
    {
        return NULL;
    }
    if (!cap_trylock_(r))
    {
        __atomic_fetch_add(&r->lost, 1u, __ATOMIC_RELAXED);
        return NULL;
    }
    if (r->filtered && memcmp(frame + addr_off, r->addr, 6) != 0)
    {
        cap_unlock_(r);
        return NULL;
    }
    r->seen++;
    struct mmwlan_cap_rec *c = &r->rec[r->next];
    r->next = (r->next + 1u) % MMWLAN_CAP_SLOTS;
    memset(c, 0, sizeof(*c));
    c->seq = r->seen;
    c->len = (uint16_t)(len > 0xffffu ? 0xffffu : len);
    c->n = (uint8_t)(len < MMWLAN_CAP_BYTES ? len : MMWLAN_CAP_BYTES);
    memcpy(c->b, frame, c->n);
    *out = r;
    return c;
}

void mmwlan_cap_rx(const uint8_t *frame, uint32_t len, uint32_t flags, uint8_t mcs, uint8_t bw_mhz,
                   int16_t rssi, uint32_t t_ms)
{
    if (frame == NULL || len < 2u || (frame[0] & 0x0cu) != 0x08u)
    {
        return; /* data frames only */
    }
    struct cap_ring *r = NULL;
    struct mmwlan_cap_rec *c = cap_begin_(MMWLAN_CAP_RX, frame, len, 10u, &r);
    if (c == NULL)
    {
        return;
    }
    c->t_ms = t_ms;
    c->flags = flags;
    c->mcs = mcs;
    c->bw_mhz = bw_mhz;
    c->rssi = rssi;
    cap_unlock_(r);
}

void mmwlan_cap_tx(const uint8_t *frame, uint32_t len, uint32_t flags, uint8_t tid,
                   uint8_t tid_params, const uint16_t rate[4], uint32_t pkt_id, bool host_frag,
                   uint32_t t_ms)
{
    const uint32_t mode = mmwlan_cap_mode[MMWLAN_CAP_TX];
    if (frame == NULL || len < 10u || (frame[0] & 0x0cu) != 0x08u || (frame[4] & 0x01u) != 0u ||
        (mode == MMWLAN_CAP_HOST_FRAG && !host_frag))
    {
        return; /* unicast data frames only; mode 1 only host fragments */
    }
    struct cap_ring *r = NULL;
    struct mmwlan_cap_rec *c = cap_begin_(MMWLAN_CAP_TX, frame, len, 4u, &r);
    if (c == NULL)
    {
        return;
    }
    c->t_ms = t_ms;
    c->flags = flags;
    c->tid = tid;
    c->tid_params = tid_params;
    c->pkt_id = pkt_id;
    c->host_frag = host_frag ? 1u : 0u;
    if (rate != NULL)
    {
        memcpy(c->rate, rate, sizeof(c->rate));
    }
    cap_unlock_(r);
}

/* The newest capture of @p pkt_id on @p tid with no status yet marked @p done (1 status, 2 untried). */
static void cap_tx_done_(uint32_t pkt_id, uint8_t tid, uint8_t done, uint32_t flags, uint8_t tries,
                         uint16_t ampdu)
{
    struct cap_ring *r = __atomic_load_n(&s_ring[MMWLAN_CAP_TX], __ATOMIC_ACQUIRE);
    if (r == NULL)
    {
        return;
    }
    if (!cap_trylock_(r))
    {
        __atomic_fetch_add(&r->st_lost, 1u, __ATOMIC_RELAXED);
        return;
    }
    for (unsigned k = 1; k <= MMWLAN_CAP_SLOTS; k++)
    {
        struct mmwlan_cap_rec *c = &r->rec[(r->next + MMWLAN_CAP_SLOTS - k) % MMWLAN_CAP_SLOTS];
        if (c->seq != 0u && !c->st_done && c->pkt_id == pkt_id && c->tid == tid)
        {
            c->st_done = done;
            c->st_flags = flags;
            c->st_tries = tries;
            c->st_ampdu = ampdu;
            break;
        }
    }
    cap_unlock_(r);
}

void mmwlan_cap_tx_status(uint32_t pkt_id, uint8_t tid, uint32_t flags, uint8_t tries,
                          uint16_t ampdu)
{
    cap_tx_done_(pkt_id, tid, 1u, flags, tries, ampdu);
}

void mmwlan_cap_tx_untried(uint32_t pkt_id, uint8_t tid)
{
    cap_tx_done_(pkt_id, tid, 2u, 0u, 0u, 0u);
}

uint32_t mmwlan_cap_st_lost(void)
{
    struct cap_ring *r = __atomic_load_n(&s_ring[MMWLAN_CAP_TX], __ATOMIC_ACQUIRE);
    return r == NULL ? 0u : __atomic_load_n(&r->st_lost, __ATOMIC_RELAXED);
}

int mmwlan_cap_read(unsigned dir, uint32_t after, struct mmwlan_cap_rec *out, unsigned max,
                    uint32_t *seen, uint32_t *lost)
{
    struct cap_ring *r = dir <= MMWLAN_CAP_TX ? s_ring[dir] : NULL;
    if (seen != NULL)
    {
        *seen = 0;
    }
    if (lost != NULL)
    {
        *lost = 0;
    }
    if (r == NULL)
    {
        return 0;
    }
    if (!cap_trylock_(r))
    {
        return -1;
    }
    unsigned n = 0;
    for (unsigned k = 0; k < MMWLAN_CAP_SLOTS && n < max; k++)
    {
        const struct mmwlan_cap_rec *c = &r->rec[(r->next + k) % MMWLAN_CAP_SLOTS];
        if (c->seq != 0u && c->seq > after)
        {
            out[n++] = *c;
        }
    }
    if (seen != NULL)
    {
        *seen = r->seen;
    }
    if (lost != NULL)
    {
        *lost = __atomic_load_n(&r->lost, __ATOMIC_RELAXED);
    }
    cap_unlock_(r);
    return (int)n;
}

static char *cap_hex_(char *p, const char *end, const uint8_t *b, unsigned n)
{
    static const char hx[] = "0123456789abcdef";
    for (unsigned i = 0; i < n && p + 2 < end; i++)
    {
        *p++ = hx[b[i] >> 4];
        *p++ = hx[b[i] & 0x0fu];
    }
    return p;
}

int mmwlan_cap_line(char *buf, size_t len, unsigned dir, const struct mmwlan_cap_rec *r)
{
    char head[176];
    int h;
    if (dir == MMWLAN_CAP_RX)
    {
        h = snprintf(head, sizeof(head), "+RXCAP: #%lu t=%lu len=%u fl=%08lx mcs=%u bw=%u rssi=%d | ",
                     (unsigned long)r->seq, (unsigned long)r->t_ms, (unsigned)r->len,
                     (unsigned long)r->flags, (unsigned)r->mcs, (unsigned)r->bw_mhz, (int)r->rssi);
    }
    else
    {
        char ch[4][16];
        for (unsigned i = 0; i < 4u; i++)
        {
            const unsigned v = r->rate[i];
            if ((v >> 8) == 0u)
            {
                snprintf(ch[i], sizeof(ch[i]), "-");
            }
            else
            {
                snprintf(ch[i], sizeof(ch[i]), "%u@%uMx%u%s", v & 0x0fu, 1u << ((v >> 4) & 0x07u),
                         (v >> 8) & 0x0fu, (v & 0x1000u) != 0u ? "r" : "");
            }
        }
        char st[40];
        if (r->st_done == 2u)
        {
            snprintf(st, sizeof(st), "untried");
        }
        else if (r->st_done)
        {
            snprintf(st, sizeof(st), "%08lx/%u/%04x", (unsigned long)r->st_flags,
                     (unsigned)r->st_tries, (unsigned)r->st_ampdu);
        }
        else
        {
            snprintf(st, sizeof(st), "none");
        }
        h = snprintf(head, sizeof(head),
                     "+TXCAP: #%lu t=%lu len=%u id=%lu cf=%08lx tid=%u tp=%02x hf=%u r=%s,%s,%s,%s st=%s | ",
                     (unsigned long)r->seq, (unsigned long)r->t_ms, (unsigned)r->len,
                     (unsigned long)r->pkt_id, (unsigned long)r->flags, (unsigned)r->tid,
                     (unsigned)r->tid_params, (unsigned)r->host_frag, ch[0], ch[1], ch[2], ch[3], st);
    }
    if (h < 0)
    {
        return h;
    }
    const size_t want = (size_t)h + 2u * r->n + 2u;
    if (buf == NULL || len == 0u)
    {
        return (int)want;
    }
    char *p = buf;
    const char *end = buf + len;
    const size_t hc = (size_t)h < len - 1u ? (size_t)h : len - 1u;
    memcpy(p, head, hc);
    p += hc;
    p = cap_hex_(p, end, r->b, r->n);
    if (p + 2 < end)
    {
        *p++ = '\r';
        *p++ = '\n';
    }
    *p = '\0';
    return (int)want;
}
