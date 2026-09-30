/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>

#include "bat_internal.h"

/* A failed append rewinds to the open entry and stops output; the next chunk starts there,
 * unless nothing was kept yet (the entry can never fit). */
void bat_wr_commit(struct bat_wr *w, int n)
{
    if (w->stop) {
        return;
    }
    if (n < 0 || (size_t)n >= w->limit - w->pos) {
        w->stop = true;
        w->next = w->any ? w->at : BAT_RENDER_TRUNC;
        w->pos = w->mark;
        if (w->pos < w->len) {
            w->buf[w->pos] = '\0';
        }
        return;
    }
    w->pos += (size_t)n;
}

bool bat_wr_begin(struct bat_wr *w, uint32_t at)
{
    if (w->stop) {
        return false;
    }
    w->mark = w->pos;
    w->at = at;
    return true;
}

void bat_wr_end(struct bat_wr *w)
{
    w->any |= !w->stop;
}

#define WR BAT_WR

static void wr_eol(struct bat_wr *w)
{
    WR(w, "\r\n");
}

#define MACF "%02x:%02x:%02x:%02x:%02x:%02x"
#define MACA(m) (m)[0], (m)[1], (m)[2], (m)[3], (m)[4], (m)[5]

static void wr_mb(struct bat_wr *w, const char *key, uint32_t units)
{
    WR(w, " %s=%u.%u", key, (unsigned)(units / 10), (unsigned)(units % 10));
}

static void wr_mb_opt(struct bat_wr *w, const char *key, bool have, uint32_t units)
{
    if (have) {
        wr_mb(w, key, units);
    } else {
        WR(w, " %s=-", key);
    }
}

/* Cursor @from -> first entry index (the summary line is cursor 0). */
static uint32_t first_entry(uint32_t from)
{
    return from ? from - 1 : 0;
}

static void render_neigh(struct bat *b, struct bat_wr *w, uint32_t from)
{
    if (from == 0 && bat_wr_begin(w, 0)) {
        WR(w, "+BATN: count=%u/%u", bat_neigh_count(b), (unsigned)BAT_MAX_NEIGH);
        wr_eol(w);
        bat_wr_end(w);
    }
    for (uint32_t i = first_entry(from); i < BAT_MAX_NEIGH; i++) {
        const struct bat_neigh *n = &b->neigh[i];
        if (!n->used) {
            continue;
        }
        if (!bat_wr_begin(w, 1 + i)) {
            return;
        }
        WR(w, "+BATN: " MACF " orig=" MACF " seen=%lums", MACA(n->addr), MACA(n->orig),
           (unsigned long)bat_age(b, n->last_seen));
        wr_mb(w, "tput", bat_neigh_tput(n));
        WR(w, " interval=%lu cands=%u", (unsigned long)n->elp_interval, (unsigned)n->refs);
        wr_eol(w);
        bat_wr_end(w);
    }
}

static unsigned orig_tt_rows(const struct bat *b, unsigned idx)
{
    unsigned n = 0;
    for (unsigned i = 0; i < BAT_TT_ROWS; i++) {
        n += (b->tt.rows[i].used && b->tt.rows[i].orig == idx) ? 1u : 0u;
    }
    return n;
}

static unsigned orig_count(const struct bat *b)
{
    unsigned n = 0;
    for (unsigned i = 0; i < BAT_MAX_ORIG; i++) {
        n += b->orig[i].used ? 1u : 0u;
    }
    return n;
}

static void render_cand(struct bat *b, struct bat_wr *w, const struct bat_orig *o, unsigned ci)
{
    const struct bat_cand *c = &o->cand[ci];
    const struct bat_cand_tbl *d = &c->t[BAT_TBL_DEFAULT], *j = &c->t[BAT_TBL_IFACE];
    WR(w, "+BATO:  via " MACF, MACA(b->neigh[c->neigh].addr));
    wr_mb_opt(w, "tput", d->valid, d->tput);
    wr_mb_opt(w, "iftput", j->valid, j->tput);
    WR(w, " seq=%lu %c%c", (unsigned long)(d->valid ? d->seq : j->seq),
       o->tbl[BAT_TBL_DEFAULT].router == (int)ci ? '*' : '-',
       o->tbl[BAT_TBL_IFACE].router == (int)ci ? 'J' : '-');
    wr_eol(w);
}

/* One entry per originator slot: its line and its candidate lines, never split. */
static void render_orig(struct bat *b, struct bat_wr *w, const uint8_t *mac, uint32_t from)
{
    if (from == 0 && bat_wr_begin(w, 0)) {
        WR(w, "+BATO: self=" MACF " soft=" MACF " ogmseq=%lu elpseq=%lu ttvn=%u routes=%u count=%u/%u",
           MACA(b->cfg.hard_addr), MACA(b->cfg.soft_addr), (unsigned long)b->ogm_seq,
           (unsigned long)b->elp_seq, (unsigned)bat_tt_own_ttvn(b), bat_route_count(b),
           orig_count(b), (unsigned)BAT_MAX_ORIG);
        wr_eol(w);
        bat_wr_end(w);
    }
    for (uint32_t i = first_entry(from); i < BAT_MAX_ORIG; i++) {
        const struct bat_orig *o = &b->orig[i];
        if (!o->used || (mac && !bat_mac_eq(o->addr, mac))) {
            continue;
        }
        if (!bat_wr_begin(w, 1 + i)) {
            return;
        }
        int rd = o->tbl[BAT_TBL_DEFAULT].router, rj = o->tbl[BAT_TBL_IFACE].router;
        const uint8_t *nh = bat_route_nh(b, o, BAT_TBL_DEFAULT);
        WR(w, "+BATO: " MACF " seen=%lums", MACA(o->addr), (unsigned long)bat_age(b, o->last_seen));
        if (nh) {
            WR(w, " nh=" MACF, MACA(nh));
        } else {
            WR(w, " nh=-");
        }
        wr_mb_opt(w, "tput", rd >= 0, rd >= 0 ? o->cand[rd].t[BAT_TBL_DEFAULT].tput : 0);
        wr_mb_opt(w, "iftput", rj >= 0, rj >= 0 ? o->cand[rj].t[BAT_TBL_IFACE].tput : 0);
        if (o->tt.known) {
            WR(w, " ttvn=%u", (unsigned)o->tt.ttvn);
        } else {
            WR(w, " ttvn=-");
        }
        WR(w, " tt=%u", orig_tt_rows(b, i));
        if (o->gw_valid) {
            WR(w, " gw=%lu.%lu/%lu.%lu", (unsigned long)(o->gw_down / 10),
               (unsigned long)(o->gw_down % 10), (unsigned long)(o->gw_up / 10),
               (unsigned long)(o->gw_up % 10));
        } else {
            WR(w, " gw=-");
        }
        wr_eol(w);
        for (unsigned c = 0; c < BAT_CANDS_PER_ORIG; c++) {
            if (o->cand[c].used) {
                render_cand(b, w, o, c);
            }
        }
        bat_wr_end(w);
    }
}

/* Counter render groups: the first counter of each; a group runs to the next one's first. */
static int group_first(unsigned g)
{
    switch (g) {
    case 0: return BAT_C_RX;
    case 1: return BAT_C_ELP_RX;
    case 2: return BAT_C_OGM_RX;
    case 3: return BAT_C_BC_RX;
    case 4: return BAT_C_UC_RX;
    case 5: return BAT_C_UT_RX;
    case 6: return BAT_C_FR_RX;
    case 7: return BAT_C_IC_RX;
    case 8: return BAT_C_ST_TX;
    case 9: return BAT_C_LK_TX;
    default: return BAT_C__COUNT;
    }
}

static const char *group_name(unsigned g)
{
    switch (g) {
    case 0: return "rx";
    case 1: return "elp";
    case 2: return "ogm";
    case 3: return "bc";
    case 4: return "uc";
    case 5: return "ut";
    case 6: return "fr";
    case 7: return "ic";
    case 8: return "st";
    default: return "lk";
    }
}

static void render_stat(struct bat *b, struct bat_wr *w, uint32_t from)
{
    if (from == 0 && bat_wr_begin(w, 0)) {
        unsigned frag = 0;
        for (unsigned i = 0; i < BAT_FRAG_SLOTS; i++) {
            frag += b->data.slot[i].used ? 1u : 0u;
        }
        WR(w, "+BATSTAT: self=" MACF " soft=" MACF " neigh=%u/%u orig=%u/%u routes=%u tt=%u/%u "
              "frag=%u/%u agg=%u",
           MACA(b->cfg.hard_addr), MACA(b->cfg.soft_addr), bat_neigh_count(b),
           (unsigned)BAT_MAX_NEIGH, orig_count(b), (unsigned)BAT_MAX_ORIG, bat_route_count(b),
           bat_tt_rows_used(b), (unsigned)BAT_TT_ROWS, frag, (unsigned)BAT_FRAG_SLOTS,
           (unsigned)b->agg_len);
        wr_eol(w);
        bat_wr_end(w);
    }
    for (uint32_t g = first_entry(from); group_first(g) < BAT_C__COUNT; g++) {
        if (!bat_wr_begin(w, 1 + g)) {
            return;
        }
        WR(w, "+BATSTAT: %s", group_name(g));
        for (int c = group_first(g); c < group_first(g + 1); c++) {
            WR(w, " %s=%lu", bat_counter_name((enum bat_counter)c),
               (unsigned long)b->cnt[c]);
        }
        wr_eol(w);
        bat_wr_end(w);
    }
}

static const char *render_tag(enum bat_render_kind kind)
{
    switch (kind) {
    case BAT_RENDER_NEIGH: return "BATN";
    case BAT_RENDER_ORIG: return "BATO";
    case BAT_RENDER_TT_GLOBAL: return "BATTG";
    case BAT_RENDER_TT_LOCAL: return "BATTL";
    default: return "BATSTAT";
    }
}

/* @cursor NULL: bat_render, one chunk from the start, "(truncated)" if the listing goes on. */
static size_t render(struct bat *b, enum bat_render_kind kind, const uint8_t *mac, uint32_t *cursor,
                     char *buf, size_t len)
{
    uint32_t from = cursor ? *cursor : 0;
    if (cursor) {
        *cursor = BAT_RENDER_DONE;
    }
    if (!buf || len == 0) {
        return 0;
    }
    buf[0] = '\0';
    if (from == BAT_RENDER_DONE || (unsigned)kind > BAT_RENDER_STAT) {
        return 0;
    }
    struct bat_wr w = { .buf = buf, .len = len };
    w.limit = len > BAT_RENDER_RESERVE ? len - BAT_RENDER_RESERVE : 0;
    b->now = b->ops.now_ms(b->user);
    switch (kind) {
    case BAT_RENDER_NEIGH:
        render_neigh(b, &w, from);
        break;
    case BAT_RENDER_ORIG:
        render_orig(b, &w, mac, from);
        break;
    case BAT_RENDER_TT_GLOBAL:
        bat_tt_render_global(b, &w, mac, from);
        break;
    case BAT_RENDER_TT_LOCAL:
        bat_tt_render_local(b, &w, from);
        break;
    default:
        render_stat(b, &w, from);
        break;
    }
    if (w.stop && (w.next == BAT_RENDER_TRUNC || !cursor)) {
        int n = snprintf(buf + w.pos, len - w.pos, "+%s: (truncated)\r\n", render_tag(kind));
        if (n > 0) {
            size_t room = len - w.pos - 1;
            w.pos += (size_t)n < room ? (size_t)n : room;
        }
    } else if (w.stop && cursor) {
        *cursor = w.next;
    }
    return w.pos;
}

size_t bat_render(struct bat *b, enum bat_render_kind kind, char *buf, size_t len)
{
    return render(b, kind, NULL, NULL, buf, len);
}

size_t bat_render_from(struct bat *b, enum bat_render_kind kind, const uint8_t *mac, uint32_t *cursor,
                       char *buf, size_t len)
{
    uint32_t done = BAT_RENDER_DONE;   /* no cursor: nothing to render */
    return render(b, kind, mac, cursor ? cursor : &done, buf, len);
}
