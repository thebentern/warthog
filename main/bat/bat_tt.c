/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>

#include "bat_crc32c.h"
#include "bat_internal.h"

/* ---- rows ------------------------------------------------------------------------ */

static bool mac_vid_eq(const uint8_t *m, uint16_t v, const uint8_t *mac, uint16_t vid)
{
    return v == vid && bat_mac_eq(m, mac);
}

static void row_free(struct bat *b, struct bat_tt_row *r)
{
    if (r->used) {
        memset(r, 0, sizeof(*r));
        b->tt.nrows--;
    }
}

static struct bat_tt_row *row_find(struct bat *b, const uint8_t *mac, uint16_t vid, unsigned oi)
{
    for (unsigned i = 0; i < BAT_TT_ROWS; i++) {
        struct bat_tt_row *r = &b->tt.rows[i];
        if (r->used && r->orig == oi && mac_vid_eq(r->mac, r->vid, mac, vid)) {
            return r;
        }
    }
    return NULL;
}

static struct bat_tt_row *row_new(struct bat *b, const uint8_t *mac, uint16_t vid, unsigned oi)
{
    for (unsigned i = 0; i < BAT_TT_ROWS; i++) {
        struct bat_tt_row *r = &b->tt.rows[i];
        if (!r->used) {
            memset(r, 0, sizeof(*r));
            memcpy(r->mac, mac, 6);
            r->vid = vid;
            r->orig = (uint8_t)oi;
            r->used = 1;
            b->tt.nrows++;
            return r;
        }
    }
    BAT_INC(b, TT_ROWS_FULL);
    return NULL;
}

/* Oldest temporary row, or NULL; *n (if given) = temporary rows in use. */
static struct bat_tt_row *temp_oldest(struct bat *b, unsigned *n)
{
    struct bat_tt_row *old = NULL;
    unsigned c = 0;
    for (unsigned i = 0; i < BAT_TT_ROWS; i++) {
        struct bat_tt_row *r = &b->tt.rows[i];
        if (r->used && (r->flags & BAT_TTR_TEMP)) {
            c++;
            if (!old || bat_age(b, r->ts) > bat_age(b, old->ts)) {
                old = r;
            }
        }
    }
    if (n) {
        *n = c;
    }
    return old;
}

static void rows_del_orig(struct bat *b, unsigned oi)
{
    for (unsigned i = 0; i < BAT_TT_ROWS; i++) {
        if (b->tt.rows[i].used && b->tt.rows[i].orig == oi) {
            row_free(b, &b->tt.rows[i]);
        }
    }
}

/* Announced add of (mac, vid) by originator @oi (tt §8.1): temporary rows of the client go. */
static bool row_add(struct bat *b, unsigned oi, const uint8_t *mac, uint16_t vid, uint8_t flags,
                    uint8_t ttvn)
{
    for (unsigned i = 0; i < BAT_TT_ROWS; i++) {
        struct bat_tt_row *r = &b->tt.rows[i];
        if (r->used && (r->flags & BAT_TTR_TEMP) && mac_vid_eq(r->mac, r->vid, mac, vid)) {
            row_free(b, r);
        }
    }
    struct bat_tt_row *r = row_find(b, mac, vid, oi);
    if (!r) {
        struct bat_tt_row *t = b->tt.nrows >= BAT_TT_ROWS ? temp_oldest(b, NULL) : NULL;
        if (t) {
            row_free(b, t);   /* an announcement outranks a row learned from data */
        }
        r = row_new(b, mac, vid, oi);
    }
    if (!r) {
        return false;
    }
    r->flags = flags & BAT_TT_SYNC_MASK;
    r->ttvn = ttvn;
    r->ts = b->now;
    return true;
}

uint32_t bat_tt_orig_crc(const struct bat *b, unsigned oi, uint16_t vid, unsigned *n)
{
    uint32_t crc = 0;
    unsigned cnt = 0;
    for (unsigned i = 0; i < BAT_TT_ROWS; i++) {
        const struct bat_tt_row *r = &b->tt.rows[i];
        if (r->used && r->orig == oi && r->vid == vid && !(r->flags & BAT_TTR_TEMP)) {
            crc ^= bat_crc32c_tt(vid, r->flags, r->mac);
            cnt++;
        }
    }
    if (n) {
        *n = cnt;
    }
    return crc;
}

/* tt §5.4: every announced VLAN matches, and no announced-entry VLAN of @oi is left out. */
static bool crc_check(struct bat *b, unsigned oi, const uint8_t *vl, uint16_t nv)
{
    for (uint16_t k = 0; k < nv; k++) {
        uint16_t vid = bat_get16(vl + BAT_TT_VLAN_LEN * k + 4);
        bool exists = false;
        for (unsigned i = 0; i < BAT_TT_ROWS && !exists; i++) {
            const struct bat_tt_row *r = &b->tt.rows[i];
            exists = r->used && r->orig == oi && r->vid == vid;
        }
        if (!exists || bat_tt_orig_crc(b, oi, vid, NULL) != bat_get32(vl + BAT_TT_VLAN_LEN * k)) {
            return false;
        }
    }
    for (unsigned i = 0; i < BAT_TT_ROWS; i++) {
        const struct bat_tt_row *r = &b->tt.rows[i];
        if (!r->used || r->orig != oi || (r->flags & BAT_TTR_TEMP)) {
            continue;
        }
        bool listed = false;
        for (uint16_t k = 0; k < nv && !listed; k++) {
            listed = bat_get16(vl + BAT_TT_VLAN_LEN * k + 4) == r->vid;
        }
        if (!listed) {
            return false;
        }
    }
    return true;
}

/* tt §5.3 (full = a full-table response: DEL/ROAM records skipped). false = an add failed. */
static bool apply(struct bat *b, unsigned oi, const uint8_t *ch, size_t n, uint8_t ttvn, bool full)
{
    static const uint8_t zero[6] = { 0 };
    for (size_t k = 0; k < n; k++) {
        const uint8_t *c = ch + BAT_TT_CHANGE_LEN * k;
        const uint8_t *mac = c + 4;
        uint16_t vid = bat_get16(c + 10);
        if (bat_mac_eq(mac, zero)) {
            continue;
        }
        if (c[0] & (BAT_TT_CLIENT_DEL | BAT_TT_CLIENT_ROAM)) {
            if (full) {
                continue;
            }
            if (c[0] & BAT_TT_CLIENT_DEL) {
                struct bat_tt_row *r = row_find(b, mac, vid, oi);
                if (r) {
                    row_free(b, r);
                }
                continue;
            }
        }
        if (!row_add(b, oi, mac, vid, c[0], ttvn)) {
            return false;
        }
    }
    return true;
}

/* ---- requests and answers (the UNICAST_TVLV header is bat_utvlv.c's) --------------------- */

static uint8_t *tvlv_area(struct bat *b)
{
    return b->txbuf + BAT_ETH_HLEN + BAT_UT_HLEN;
}

/* 3 s (tt §6.1), doubled after each answer that could not be taken, at most 60 s (hardening). Before a
 * table is first held 500 ms: a first request can reach a node that cannot route back yet (tt §6.2). */
static uint32_t req_wait(const struct bat_tt_orig *t)
{
    if (!t->known && !t->stalled && t->backoff == 0) {
        return BAT_TT_REQ_JOIN_MS;
    }
    uint32_t w = BAT_TT_REQ_MS << t->backoff;
    return w < BAT_TT_REQ_MAX_MS ? w : BAT_TT_REQ_MAX_MS;
}

/* The answer to @o's request could not be taken (too big, or rows full): the next waits longer. */
static void stall(struct bat *b, struct bat_orig *o)
{
    o->tt.req_pending = 1;
    o->tt.req_ts = b->now;
    if (!o->tt.stalled) {
        o->tt.stalled = 1;
        if ((BAT_TT_REQ_MS << o->tt.backoff) < BAT_TT_REQ_MAX_MS) {
            o->tt.backoff++;
        }
        BAT_INC(b, TT_REQ_STALL);
    }
}

/* tt §6.1: one outstanding request per originator, forgotten after req_wait. */
static void tt_request(struct bat *b, struct bat_orig *o, uint8_t ttvn, const uint8_t *vl, uint16_t nv,
                       bool full)
{
    if (o->tt.req_pending && bat_age(b, o->tt.req_ts) < req_wait(&o->tt)) {
        return;
    }
    size_t vlen = 4 + (size_t)BAT_TT_VLAN_LEN * nv;
    if (BAT_ETH_HLEN + BAT_UT_HLEN + BAT_TVLV_HLEN + vlen > sizeof(b->txbuf)) {
        BAT_INC(b, TT_BAD);
        return;
    }
    if (!bat_route_nh(b, o, BAT_TBL_DEFAULT)) {
        BAT_INC(b, TT_REQ_NOROUTE);
        return;
    }
    uint8_t *t = tvlv_area(b);
    t[0] = BAT_TVLV_TT;
    t[1] = 1;
    bat_put16(t + 2, (uint16_t)vlen);
    t[4] = full ? (BAT_TT_REQUEST | BAT_TT_FULL_TABLE) : BAT_TT_REQUEST;
    t[5] = ttvn;
    bat_put16(t + 6, nv);
    memcpy(t + 8, vl, (size_t)BAT_TT_VLAN_LEN * nv);
    o->tt.req_pending = 1;
    o->tt.req_ts = b->now;
    o->tt.stalled = 0;
    BAT_INC(b, TT_REQ_TX);
    bat_utvlv_send(b, o, b->cfg.hard_addr, BAT_TVLV_HLEN + vlen);
}

/* ---- own table ------------------------------------------------------------------- */

static bool committed(const struct bat_tt_local *l)
{
    return l->state == BAT_TTL_ON || l->state == BAT_TTL_DEL;
}

static struct bat_tt_local *local_find(struct bat *b, const uint8_t *mac, uint16_t vid)
{
    for (unsigned i = 0; i < b->tt.n_local; i++) {
        if (mac_vid_eq(b->tt.local[i].mac, b->tt.local[i].vid, mac, vid)) {
            return &b->tt.local[i];
        }
    }
    return NULL;
}

static void local_remove(struct bat *b, struct bat_tt_local *l)
{
    unsigned i = (unsigned)(l - b->tt.local);
    memmove(&b->tt.local[i], &b->tt.local[i + 1], (b->tt.n_local - i - 1) * sizeof(*l));
    b->tt.n_local--;
    memset(&b->tt.local[b->tt.n_local], 0, sizeof(*l));
}

/* tt §4.3: an opposite queued event cancels both; returns false when the queue is full. */
static bool queue_event(struct bat *b, const uint8_t *mac, uint16_t vid, uint8_t flags)
{
    struct bat_tt *t = &b->tt;
    bool del = (flags & BAT_TT_CLIENT_DEL) != 0;
    for (unsigned i = 0; i < t->n_changes; i++) {
        struct bat_tt_change *c = &t->changes[i];
        if (mac_vid_eq(c->mac, c->vid, mac, vid) && ((c->flags & BAT_TT_CLIENT_DEL) != 0) != del) {
            memmove(c, c + 1, (t->n_changes - i - 1) * sizeof(*c));
            t->n_changes--;
            return true;
        }
    }
    if (t->n_changes >= BAT_TT_CHANGES_MAX) {
        return false;
    }
    struct bat_tt_change *c = &t->changes[t->n_changes++];
    memcpy(c->mac, mac, 6);
    c->vid = vid;
    c->flags = flags;
    return true;
}

/* VLANs holding a committed local entry, in first-seen order. */
static unsigned local_vlans(const struct bat *b, uint16_t *vids)
{
    unsigned n = 0;
    for (unsigned i = 0; i < b->tt.n_local; i++) {
        const struct bat_tt_local *l = &b->tt.local[i];
        bool seen = false;
        if (!committed(l)) {
            continue;
        }
        for (unsigned k = 0; k < n && !seen; k++) {
            seen = vids[k] == l->vid;
        }
        if (!seen) {
            vids[n++] = l->vid;
        }
    }
    return n;
}

static uint32_t local_crc(const struct bat *b, uint16_t vid)
{
    uint32_t crc = 0;
    for (unsigned i = 0; i < b->tt.n_local; i++) {
        const struct bat_tt_local *l = &b->tt.local[i];
        if (committed(l) && l->vid == vid) {
            crc ^= bat_crc32c_tt(vid, l->flags, l->mac);
        }
    }
    return crc;
}

static size_t vlans_len(const struct bat *b)
{
    uint16_t vids[BAT_TT_LOCAL_MAX];
    return 4 + (size_t)BAT_TT_VLAN_LEN * local_vlans(b, vids);
}

/* flags, TTVN, VLAN records; returns bytes written (the TT value header + records). */
static size_t put_vlans(const struct bat *b, uint8_t *v, uint8_t flags)
{
    uint16_t vids[BAT_TT_LOCAL_MAX];
    unsigned nv = local_vlans(b, vids);
    v[0] = flags;
    v[1] = b->tt.ttvn;
    bat_put16(v + 2, (uint16_t)nv);
    for (unsigned k = 0; k < nv; k++) {
        uint8_t *r = v + 4 + BAT_TT_VLAN_LEN * k;
        bat_put32(r, local_crc(b, vids[k]));
        bat_put16(r + 4, vids[k]);
        bat_put16(r + 6, 0);
    }
    return 4 + (size_t)BAT_TT_VLAN_LEN * nv;
}

static void put_change(uint8_t *c, uint8_t flags, const uint8_t *mac, uint16_t vid)
{
    c[0] = flags;
    c[1] = c[2] = c[3] = 0;
    memcpy(c + 4, mac, 6);
    bat_put16(c + 10, vid);
}

void bat_tt_init(struct bat *b)
{
    memset(&b->tt, 0, sizeof(b->tt));
    (void)bat_tt_local_add(b, b->cfg.soft_addr, 0x0000, 0);
}

/* tt §4.5, run before every own OGM. */
size_t bat_tt_ogm_tvlv_build(struct bat *b, uint8_t *out, size_t max)
{
    struct bat_tt *t = &b->tt;
    uint8_t *v = out + BAT_TVLV_HLEN;
    size_t n;
    if (t->n_changes == 0) {
        if (t->resend > 0 && t->last_tvlv_len > 0) {
            t->resend--;
            if (t->last_tvlv_len > max) {
                return 0;
            }
            memcpy(out, t->last_tvlv, t->last_tvlv_len);
            return t->last_tvlv_len;
        }
        if (max < BAT_TVLV_HLEN + vlans_len(b)) {
            return 0;
        }
        n = put_vlans(b, v, BAT_TT_OGM_DIFF);
    } else {
        for (unsigned i = 0; i < t->n_local;) {
            struct bat_tt_local *l = &t->local[i];
            if (l->state == BAT_TTL_DEL) {
                local_remove(b, l);
                continue;
            }
            l->state = BAT_TTL_ON;
            i++;
        }
        t->ttvn++;
        t->resend = BAT_TT_RESENDS;
        if (max < BAT_TVLV_HLEN + vlans_len(b)) {
            t->n_changes = 0;
            t->last_tvlv_len = 0;
            return 0;
        }
        n = put_vlans(b, v, BAT_TT_OGM_DIFF);
        size_t room = (max < sizeof(t->last_tvlv) ? max : sizeof(t->last_tvlv)) - BAT_TVLV_HLEN;
        if (n + (size_t)BAT_TT_CHANGE_LEN * t->n_changes <= room) {
            for (unsigned i = 0; i < t->n_changes; i++, n += BAT_TT_CHANGE_LEN) {
                put_change(v + n, t->changes[i].flags, t->changes[i].mac, t->changes[i].vid);
            }
        }
        t->n_changes = 0;
    }
    out[0] = BAT_TVLV_TT;
    out[1] = 1;
    bat_put16(out + 2, (uint16_t)n);
    n += BAT_TVLV_HLEN;
    if (n <= sizeof(t->last_tvlv)) {
        memcpy(t->last_tvlv, out, n);
        t->last_tvlv_len = (uint16_t)n;
    }
    return n;
}

int bat_tt_local_add(struct bat *b, const uint8_t mac[6], uint16_t vid, uint8_t flags)
{
    struct bat_tt *t = &b->tt;
    flags &= 0x30;
    if (!bat_mac_unicast_ok(mac)) {
        return -1;
    }
    struct bat_tt_local *l = local_find(b, mac, vid);
    if (l) {
        if (l->flags != flags) {
            return -1;
        }
        if (l->state == BAT_TTL_DEL) {
            if (!queue_event(b, mac, vid, flags)) {
                return -1;
            }
            l->state = BAT_TTL_ON;
        }
        return 0;
    }
    if (t->n_local >= BAT_TT_LOCAL_MAX || !queue_event(b, mac, vid, flags)) {
        return -1;
    }
    l = &t->local[t->n_local++];
    memcpy(l->mac, mac, 6);
    l->vid = vid;
    l->flags = flags;
    l->state = BAT_TTL_NEW;
    return 0;
}

int bat_tt_local_del(struct bat *b, const uint8_t mac[6], uint16_t vid)
{
    struct bat_tt_local *l = local_find(b, mac, vid);
    if (!l) {
        return -1;
    }
    if (l->state == BAT_TTL_DEL) {
        return 0;
    }
    if (!queue_event(b, mac, vid, (uint8_t)(BAT_TT_CLIENT_DEL | l->flags))) {
        return -1;
    }
    if (l->state == BAT_TTL_NEW) {
        local_remove(b, l);
    } else {
        l->state = BAT_TTL_DEL;
    }
    return 0;
}

bool bat_tt_is_own_client(const struct bat *b, const uint8_t mac[6], uint16_t vid)
{
    for (unsigned i = 0; i < b->tt.n_local; i++) {
        const struct bat_tt_local *l = &b->tt.local[i];
        if (l->state != BAT_TTL_DEL && mac_vid_eq(l->mac, l->vid, mac, vid)) {
            return true;
        }
    }
    return false;
}

uint8_t bat_tt_own_ttvn(const struct bat *b)
{
    return b->tt.ttvn;
}

unsigned bat_tt_rows_used(const struct bat *b)
{
    return b->tt.nrows;
}

/* ---- remote tables ------------------------------------------------------------------ */

/* tt §5.2 with "+1" taken modulo 256 (deviation 2.4.6). */
void bat_tt_ogm_rx(struct bat *b, struct bat_orig *o, const uint8_t *val, size_t len)
{
    if (len < 4 || len < 4 + (size_t)BAT_TT_VLAN_LEN * bat_get16(val + 2)) {
        BAT_INC(b, TT_BAD);
        return;
    }
    unsigned oi = bat_orig_index(b, o);
    uint8_t ttvn = val[1];
    uint16_t nv = bat_get16(val + 2);
    const uint8_t *vl = val + 4, *ch = vl + (size_t)BAT_TT_VLAN_LEN * nv;
    size_t nch = (len - 4 - (size_t)BAT_TT_VLAN_LEN * nv) / BAT_TT_CHANGE_LEN;
    struct bat_tt_orig *t = &o->tt;
    /* Each VLAN record costs a pass over the rows in crc_check: a repeated VID is refused. */
    for (uint16_t k = 1; k < nv; k++) {
        for (uint16_t j = 0; j < k; j++) {
            if (bat_get16(vl + BAT_TT_VLAN_LEN * k + 4) == bat_get16(vl + BAT_TT_VLAN_LEN * j + 4)) {
                BAT_INC(b, TT_BAD);
                return;
            }
        }
    }
    t->ann = ttvn;
    t->ann_valid = 1;
    if ((!t->known && ttvn == 1) || ttvn == (uint8_t)(t->ttvn + 1)) {
        if (nch == 0) {
            tt_request(b, o, ttvn, vl, nv, false);
            return;
        }
        BAT_INC(b, TT_DIFF);
        bool ok = apply(b, oi, ch, nch, ttvn, false);
        t->ttvn = ttvn;
        t->known = ok;
        if (!crc_check(b, oi, vl, nv)) {
            BAT_INC(b, TT_CRC_FAIL);
            tt_request(b, o, ttvn, vl, nv, true);
        } else {
            t->backoff = 0;
        }
        return;
    }
    if (!t->known || ttvn != t->ttvn) {
        tt_request(b, o, ttvn, vl, nv, true);
    } else if (!crc_check(b, oi, vl, nv)) {
        BAT_INC(b, TT_CRC_FAIL);
        tt_request(b, o, ttvn, vl, nv, true);
    } else {
        t->backoff = 0;
    }
}

/* tt §6.2: a request addressed to us gets the full table, at most one per requester per
 * BAT_TT_ANSWER_MS (hardening: one packet of 195 request containers draws one answer). */
static void answer(struct bat *b, const uint8_t *src)
{
    struct bat_orig *o = bat_orig_find(b, src);
    if (!o || !bat_route_nh(b, o, BAT_TBL_DEFAULT)) {
        return;
    }
    if (o->tt.answered && bat_age(b, o->tt.ans_ts) < BAT_TT_ANSWER_MS) {
        BAT_INC(b, TT_REQ_PACED);
        return;
    }
    o->tt.answered = 1;
    o->tt.ans_ts = b->now;
    uint8_t *t = tvlv_area(b);
    size_t n = put_vlans(b, t + BAT_TVLV_HLEN, BAT_TT_RESPONSE | BAT_TT_FULL_TABLE);
    for (unsigned i = 0; i < b->tt.n_local; i++) {
        const struct bat_tt_local *l = &b->tt.local[i];
        if (committed(l)) {
            put_change(t + BAT_TVLV_HLEN + n, l->flags, l->mac, l->vid);
            n += BAT_TT_CHANGE_LEN;
        }
    }
    t[0] = BAT_TVLV_TT;
    t[1] = 1;
    bat_put16(t + 2, (uint16_t)n);
    BAT_INC(b, TT_RESP_TX);
    bat_utvlv_send(b, o, b->cfg.hard_addr, BAT_TVLV_HLEN + n);
}

/* tt §6.4: applied whether or not a request is outstanding. */
static void response(struct bat *b, const uint8_t *src, const uint8_t *val, size_t len)
{
    struct bat_orig *o = bat_orig_find(b, src);
    if (!o) {
        BAT_INC(b, TT_RESP_UNKNOWN);
        return;
    }
    BAT_INC(b, TT_RESP_RX);
    unsigned oi = bat_orig_index(b, o);
    uint16_t nv = bat_get16(val + 2);
    const uint8_t *ch = val + 4 + (size_t)BAT_TT_VLAN_LEN * nv;
    size_t nch = (len - 4 - (size_t)BAT_TT_VLAN_LEN * nv) / BAT_TT_CHANGE_LEN;
    bool full = (val[0] & BAT_TT_FULL_TABLE) != 0;
    if (full) {
        rows_del_orig(b, oi);
        BAT_INC(b, TT_FULL);
    } else {
        BAT_INC(b, TT_DIFF);
    }
    o->tt.known = apply(b, oi, ch, nch, val[1], full);
    o->tt.ttvn = val[1];
    o->tt.req_pending = 0;
    if (!o->tt.known) {
        stall(b, o);
    }
}

/* Only the head piece starts with the UNICAST_TVLV header. Its source is the owner; the fragment
 * originator may be a relay answering on the owner's behalf (tt §6.3), so it is never charged. */
void bat_tt_answer_toobig(struct bat *b, const uint8_t *pkt, size_t len)
{
    const uint8_t *t = pkt + BAT_UT_HLEN;
    if (len < BAT_UT_HLEN + BAT_TVLV_HLEN + 1 || pkt[BAT_OFF_TYPE] != BAT_PT_UTVLV ||
        pkt[BAT_OFF_VERSION] != BAT_COMPAT || !bat_is_own(b, pkt + BAT_UT_DEST) || t[0] != BAT_TVLV_TT ||
        t[1] != 1 || (t[BAT_TVLV_HLEN] & 0x0F) != BAT_TT_RESPONSE) {
        return;
    }
    struct bat_orig *o = bat_orig_find(b, pkt + BAT_UT_SRC);
    if (o && o->tt.req_pending) {
        stall(b, o);
    }
}

void bat_tt_utvlv_rx(struct bat *b, const uint8_t src[6], const uint8_t *val, size_t len)
{
    if (len < 4 || len < 4 + (size_t)BAT_TT_VLAN_LEN * bat_get16(val + 2)) {
        BAT_INC(b, TT_BAD);
        return;
    }
    switch (val[0] & 0x0F) {
    case BAT_TT_REQUEST:
        BAT_INC(b, TT_REQ_RX);
        answer(b, src);
        break;
    case BAT_TT_RESPONSE:
        response(b, src, val, len);
        break;
    default:
        BAT_INC(b, TT_BAD);
        break;
    }
}

void bat_tt_orig_gone(struct bat *b, struct bat_orig *o)
{
    rows_del_orig(b, bat_orig_index(b, o));
    o->tt.known = 0;
    o->tt.req_pending = 0;
    o->tt.backoff = 0;
}

void bat_tt_purge(struct bat *b)
{
    for (unsigned i = 0; i < BAT_TT_ROWS; i++) {
        struct bat_tt_row *r = &b->tt.rows[i];
        if (r->used && (r->flags & BAT_TTR_TEMP) && bat_age(b, r->ts) > BAT_TT_TEMP_MS) {
            row_free(b, r);
        }
    }
    for (unsigned i = 0; i < BAT_MAX_ORIG; i++) {
        struct bat_orig *o = &b->orig[i];
        if (o->used && o->tt.req_pending && bat_age(b, o->tt.req_ts) >= req_wait(&o->tt)) {
            o->tt.req_pending = 0;
        }
    }
}

uint16_t bat_frame_vid(const uint8_t *inner, size_t len)
{
    if (len >= BAT_ETH_HLEN + 4 && bat_get16(inner + 12) == 0x8100) {
        return (uint16_t)(BAT_VID_TAGGED | (bat_get16(inner + 14) & 0x0FFF));
    }
    return 0x0000;
}

/* tt §8.3: a client frame delivered from originator @o seeds a temporary row. */
void bat_tt_learn_temp(struct bat *b, struct bat_orig *o, const uint8_t *inner, size_t len)
{
    if (len < BAT_ETH_HLEN) {
        return;
    }
    const uint8_t *src = inner + 6;
    uint16_t vid = bat_frame_vid(inner, len);
    if (!bat_mac_unicast_ok(src) || (src[0] == 0xba && src[1] == 0xbe) || local_find(b, src, vid)) {
        return;
    }
    unsigned oi = bat_orig_index(b, o), nt;
    struct bat_tt_row *r = NULL;
    for (unsigned i = 0; i < BAT_TT_ROWS; i++) {
        struct bat_tt_row *e = &b->tt.rows[i];
        if (e->used && mac_vid_eq(e->mac, e->vid, src, vid)) {
            if (!(e->flags & BAT_TTR_TEMP)) {
                return;                      /* announced: a temporary add is ignored */
            }
            r = e;
        }
    }
    /* tt §8.1 step 4: a temporary row follows the latest originator; its creation time stays. */
    if (r) {
        if (r->orig != oi) {
            r->orig = (uint8_t)oi;
            r->ttvn = o->tt.ttvn;
        }
        return;
    }
    struct bat_tt_row *old = temp_oldest(b, &nt);
    if (old && (nt >= BAT_TT_TEMP_MAX || b->tt.nrows >= BAT_TT_ROWS)) {
        row_free(b, old);                    /* rows learned from data never pass the cap */
    }
    r = row_new(b, src, vid, oi);
    if (r) {
        r->flags = BAT_TTR_TEMP;
        r->ttvn = o->tt.ttvn;
        r->ts = b->now;
        BAT_INC(b, TT_TEMP_NEW);
    }
}

/* tt §9.1: best routed originator among the rows of (mac, vid). */
struct bat_orig *bat_tt_resolve(struct bat *b, const uint8_t mac[6], uint16_t vid)
{
    struct bat_orig *best = NULL;
    uint32_t bt = 0;
    for (unsigned i = 0; i < BAT_TT_ROWS; i++) {
        const struct bat_tt_row *r = &b->tt.rows[i];
        if (!r->used || !mac_vid_eq(r->mac, r->vid, mac, vid)) {
            continue;
        }
        struct bat_orig *o = bat_orig_at(b, r->orig);
        if (!o || !bat_route_nh(b, o, BAT_TBL_DEFAULT)) {
            continue;
        }
        uint32_t t = bat_route_tput(b, o);
        if (!best || t > bt) {
            best = o;
            bt = t;
        }
    }
    return best;
}

/* Hardening: while @o's table cannot be taken (never synced, or answers backed off) the synced TTVN lags
 * what @o announces, and a batman-adv relay drops every packet with it (dataplane §6.2 step 7); the
 * announced one passes. With @o the next hop the synced one stays, so @o can re-resolve (step 6). */
uint8_t bat_tt_uc_ttvn(struct bat *b, const struct bat_orig *o)
{
    const struct bat_tt_orig *t = &o->tt;
    if (!t->ann_valid || !bat_ttvn_older(t->ttvn, t->ann) || (t->known && !t->backoff)) {
        return t->ttvn;
    }
    const uint8_t *nh = bat_route_nh(b, o, BAT_TBL_DEFAULT);
    const struct bat_neigh *n = nh ? bat_neigh_find(b, nh) : NULL;
    if (!nh || bat_mac_eq(nh, o->addr) || (n && bat_mac_eq(n->orig, o->addr))) {
        return t->ttvn;
    }
    return t->ann;
}

/* ---- render ------------------------------------------------------------------------ */

#define TW BAT_WR
#define MACF "%02x:%02x:%02x:%02x:%02x:%02x"
#define MACA(m) (m)[0], (m)[1], (m)[2], (m)[3], (m)[4], (m)[5]

static void tw_vid(struct bat_wr *w, uint16_t vid)
{
    if (vid == 0) {
        TW(w, " vid=-1");
    } else if ((vid & 0xF000) == BAT_VID_TAGGED) {
        TW(w, " vid=%u", (unsigned)(vid & 0x0FFF));
    } else {
        TW(w, " vid=raw:0x%04x", (unsigned)vid);
    }
}

/* Row @i is the first of its (originator, VID): where that VID's CRC line goes. */
static bool first_of_vid(const struct bat *b, unsigned i)
{
    const struct bat_tt_row *r = &b->tt.rows[i];
    for (unsigned j = 0; j < i; j++) {
        const struct bat_tt_row *q = &b->tt.rows[j];
        if (q->used && q->orig == r->orig && q->vid == r->vid) {
            return false;
        }
    }
    return true;
}

static bool orig_has_client(const struct bat *b, unsigned oi, const uint8_t *mac)
{
    for (unsigned i = 0; i < BAT_TT_ROWS; i++) {
        const struct bat_tt_row *r = &b->tt.rows[i];
        if (r->used && r->orig == oi && bat_mac_eq(r->mac, mac)) {
            return true;
        }
    }
    return false;
}

/* Per originator slot: its CRC lines (phase 0), then its rows (phase 1), each in row-slot
 * order; one line per entry, at 1 + (originator * 2 + phase) * BAT_TT_ROWS + row. */
void bat_tt_render_global(struct bat *b, struct bat_wr *w, const uint8_t *mac, uint32_t from)
{
    if (from == 0 && bat_wr_begin(w, 0)) {
        TW(w, "+BATTG: rows=%u/%u\r\n", (unsigned)b->tt.nrows, (unsigned)BAT_TT_ROWS);
        bat_wr_end(w);
    }
    const uint32_t at = from ? from - 1 : 0;
    for (uint32_t oi = at / (2u * BAT_TT_ROWS); oi < BAT_MAX_ORIG; oi++) {
        struct bat_orig *o = bat_orig_at(b, oi);
        const bool all = o && (!mac || bat_mac_eq(o->addr, mac));
        if (!o || (!all && !orig_has_client(b, oi, mac))) {
            continue;
        }
        for (uint32_t ph = 0; ph < 2; ph++) {
            const uint32_t base = (oi * 2u + ph) * BAT_TT_ROWS;
            for (uint32_t i = base < at ? at - base : 0; i < BAT_TT_ROWS; i++) {
                const struct bat_tt_row *r = &b->tt.rows[i];
                if (!r->used || r->orig != oi) {
                    continue;
                }
                if (ph == 0 ? !first_of_vid(b, i) : !all && !bat_mac_eq(r->mac, mac)) {
                    continue;
                }
                if (!bat_wr_begin(w, 1 + base + i)) {
                    return;
                }
                if (ph == 0) {
                    unsigned n = 0;
                    for (unsigned j = i; j < BAT_TT_ROWS; j++) {
                        const struct bat_tt_row *q = &b->tt.rows[j];
                        n += q->used && q->orig == oi && q->vid == r->vid;
                    }
                    const uint32_t crc = bat_tt_orig_crc(b, oi, r->vid, NULL);
                    TW(w, "+BATTG: crc via=" MACF, MACA(o->addr));
                    tw_vid(w, r->vid);
                    TW(w, " crc=0x%08lx entries=%u\r\n", (unsigned long)crc, n);
                } else {
                    TW(w, "+BATTG: " MACF, MACA(r->mac));
                    tw_vid(w, r->vid);
                    TW(w, " via=" MACF " ttvn=%u flags=%c%c%c\r\n", MACA(o->addr), (unsigned)r->ttvn,
                       (r->flags & 0x10) ? 'W' : '-', (r->flags & 0x20) ? 'I' : '-',
                       (r->flags & BAT_TTR_TEMP) ? 'T' : '-');
                }
                bat_wr_end(w);
            }
        }
    }
}

void bat_tt_render_local(struct bat *b, struct bat_wr *w, uint32_t from)
{
    if (from == 0 && bat_wr_begin(w, 0)) {
        TW(w, "+BATTL: ttvn=%u changes=%u resend=%u\r\n", (unsigned)b->tt.ttvn,
           (unsigned)b->tt.n_changes, (unsigned)b->tt.resend);
        bat_wr_end(w);
    }
    for (uint32_t i = from ? from - 1 : 0; i < b->tt.n_local; i++) {
        const struct bat_tt_local *l = &b->tt.local[i];
        if (!bat_wr_begin(w, 1 + i)) {
            return;
        }
        TW(w, "+BATTL: " MACF, MACA(l->mac));
        tw_vid(w, l->vid);
        TW(w, " flags=%c%c crc=0x%08lx\r\n", (l->flags & 0x10) ? 'W' : '-', (l->flags & 0x20) ? 'I' : '-',
           (unsigned long)local_crc(b, l->vid));
        bat_wr_end(w);
    }
}
