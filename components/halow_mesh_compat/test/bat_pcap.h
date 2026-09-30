/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Minimal classic-pcap reader for the batman golden and hostile-input tests. Header-only
 * (static functions) so a test can use it whether or not its make rule links bat_pcap.c.
 * Reads the whole file into memory; handles both byte orders and the microsecond and
 * nanosecond magic numbers. The link type is reported, not enforced.
 */
#ifndef BAT_PCAP_H
#define BAT_PCAP_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct bat_pcap {
    uint8_t *data;
    size_t len, off;
    uint32_t linktype;
    int swapped, nsec;
};

struct bat_pcap_rec {
    uint64_t t_us;            /* timestamp, microseconds */
    uint32_t caplen, origlen;
    const uint8_t *data;
};

#define BAT_PCAP_GHDR 24
#define BAT_PCAP_RHDR 16

static uint32_t bat_pcap_rd32(const struct bat_pcap *p, const uint8_t *b)
{
    if (p->swapped) {
        return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    }
    return ((uint32_t)b[3] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[1] << 8) | b[0];
}

static void bat_pcap_close(struct bat_pcap *p)
{
    free(p->data);
    p->data = NULL;
    p->len = p->off = 0;
}

/* 0 on success, -1 if the file cannot be read or is not a classic pcap. */
static int bat_pcap_open(struct bat_pcap *p, const char *path)
{
    memset(p, 0, sizeof(*p));
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0) {
        n = ftell(f);
    }
    rewind(f);
    if (n < BAT_PCAP_GHDR) {
        fclose(f);
        return -1;
    }
    p->data = malloc((size_t)n);
    if (!p->data || fread(p->data, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        bat_pcap_close(p);
        return -1;
    }
    fclose(f);
    p->len = (size_t)n;
    const uint8_t *h = p->data;
    uint32_t le = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
    switch (le) {
    case 0xa1b2c3d4u: break;
    case 0xa1b23c4du: p->nsec = 1; break;
    case 0xd4c3b2a1u: p->swapped = 1; break;
    case 0x4d3cb2a1u: p->swapped = 1; p->nsec = 1; break;
    default:
        bat_pcap_close(p);
        return -1;
    }
    p->linktype = bat_pcap_rd32(p, h + 20);
    p->off = BAT_PCAP_GHDR;
    return 0;
}

/* 1 = *r filled, 0 = end of file, -1 = truncated or malformed record. */
static int bat_pcap_next(struct bat_pcap *p, struct bat_pcap_rec *r)
{
    if (p->off == p->len) {
        return 0;
    }
    if (p->len - p->off < BAT_PCAP_RHDR) {
        return -1;
    }
    const uint8_t *h = p->data + p->off;
    uint32_t sec = bat_pcap_rd32(p, h), frac = bat_pcap_rd32(p, h + 4);
    r->caplen = bat_pcap_rd32(p, h + 8);
    r->origlen = bat_pcap_rd32(p, h + 12);
    if (r->caplen > p->len - p->off - BAT_PCAP_RHDR) {
        return -1;
    }
    r->t_us = (uint64_t)sec * 1000000u + (p->nsec ? frac / 1000u : frac);
    r->data = h + BAT_PCAP_RHDR;
    p->off += BAT_PCAP_RHDR + r->caplen;
    return 1;
}

static void bat_pcap_rewind(struct bat_pcap *p)
{
    p->off = BAT_PCAP_GHDR;
}

#endif
