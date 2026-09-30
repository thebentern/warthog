/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WARTHOG_BAT_INTERNAL_H
#define WARTHOG_BAT_INTERNAL_H
/* Engine state shared by the bat_*.c files; never included outside main/bat and its tests. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bat.h"
#include "bat_codec.h"

#ifndef BAT_MAX_NEIGH
#define BAT_MAX_NEIGH       8
#endif
#ifndef BAT_MAX_ORIG
#define BAT_MAX_ORIG        32
#endif
#ifndef BAT_CANDS_PER_ORIG
#define BAT_CANDS_PER_ORIG  4
#endif
#ifndef BAT_TT_ROWS
#define BAT_TT_ROWS         256
#endif
#ifndef BAT_TT_LOCAL_MAX
#define BAT_TT_LOCAL_MAX    4
#endif
#ifndef BAT_TT_CHANGES_MAX
#define BAT_TT_CHANGES_MAX  8
#endif
#ifndef BAT_FRAG_SLOTS
#define BAT_FRAG_SLOTS      3
#endif
#ifndef BAT_FRAG_BUF
#define BAT_FRAG_BUF        2048
#endif
#ifndef BAT_FRAG_MAXN
#define BAT_FRAG_MAXN       16
#endif
#ifndef BAT_AGG_BUF
#define BAT_AGG_BUF         1536
#endif
#ifndef BAT_BC_COPY_SLOTS
#define BAT_BC_COPY_SLOTS   4      /* broadcasts with copies waiting to be sent (paced mode) */
#endif

#define BAT_TBL_DEFAULT 0
#define BAT_TBL_IFACE   1          /* the single hard interface ("J") */
#define BAT_NTBL        2

/* timing constants (ms) */
#define BAT_JITTER_MS         20
#define BAT_AGG_MIN_MS        90
#define BAT_AGG_SPREAD_MS     20
#define BAT_PURGE_MS          1000
#define BAT_TT_PURGE_MS       5000
#define BAT_CAND_TIMEOUT_MS   200000u
#define BAT_ORIG_TIMEOUT_MS   400000u
#define BAT_RESTART_MS        30000u
#define BAT_STAMP_CLEAR_MS    60000u
#define BAT_EVICT_AGE_MS      30000u
#define BAT_SEQ_WINDOW        64
#define BAT_SEQ_MAX_JUMP      65536
#define BAT_ROUTE_SWITCH_GAP  5
#define BAT_TPUT_DEFAULT      10   /* link sample when the station has no estimate */
#define BAT_BC_COPY_MS        5    /* dataplane §5.3 copy spacing, kept between all group frames */

#include "bat_tt.h"
#include "bat_data.h"

struct bat_neigh {                 /* elp-ogm §2.4 */
    uint8_t  addr[6];              /* hard address (ELP link source) */
    uint8_t  orig[6];              /* ELP originator field, fixed at creation */
    uint8_t  used, refs;           /* refs = candidates pointing here */
    uint32_t last_seen;            /* creation or last accepted ELP */
    uint32_t elp_seq;              /* 0 at creation */
    uint32_t elp_interval;         /* informational */
    uint64_t tput_acc;             /* EWMA accumulator = average * 1024; 0 = empty */
};
struct bat_cand_tbl { uint32_t tput, seq; uint8_t ttl, valid; };
struct bat_cand {                  /* per originator x neighbour, elp-ogm §4.1 */
    uint8_t  used, neigh;          /* neigh = index into b->neigh */
    uint32_t last_seen;
    struct bat_cand_tbl t[BAT_NTBL];
};
struct bat_otbl {                  /* per-table state per originator */
    int8_t   router;               /* candidate index or -1 */
    uint8_t  restart_valid, last_ttl;
    uint32_t last_seq, last_fwd_seq, restart_ts;   /* seqs start at 0 */
};
struct bat_bwin { uint32_t last; uint64_t bits; uint32_t reset_ts; uint8_t reset_valid; };
struct bat_bc_copy { uint16_t len; uint8_t left; uint32_t ord; uint8_t frame[BAT_MAX_LINK_FRAME]; };
struct bat_orig {
    uint8_t  addr[6];
    uint8_t  used, gw_valid;
    uint32_t last_seen;            /* creation or last accepted OGM record, any table */
    struct bat_otbl  tbl[BAT_NTBL];
    struct bat_cand  cand[BAT_CANDS_PER_ORIG];
    struct bat_bwin  bcast;
    uint32_t gw_down, gw_up;       /* 100 kbit/s units, display only */
    struct bat_tt_orig tt;         /* E2, bat_tt.h */
};
struct bat {
    struct bat_config cfg; struct bat_ops ops; void *user;
    uint32_t now;
    uint32_t elp_seq, ogm_seq, bcast_seq; uint16_t frag_seq;
    uint32_t next_elp, next_ogm, next_agg, next_purge, next_tt_purge;
    struct bat_neigh neigh[BAT_MAX_NEIGH];
    struct bat_orig  orig[BAT_MAX_ORIG];
    uint8_t  agg[BAT_ETH_HLEN + BAT_AGG_BUF]; uint16_t agg_len;  /* records start at agg + 14 */
    uint8_t  txbuf[BAT_MAX_LINK_FRAME];   /* own packets; not re-entrant */
    uint8_t  ogm_own[BAT_OGM_MAX_OWN];
    struct bat_bc_copy bc_copy[BAT_BC_COPY_SLOTS];   /* broadcast copies still to send */
    uint32_t bc_ord, grp_ts; uint8_t grp_sent;        /* arrival counter; ops->tx took our last group frame */
    struct bat_tt   tt;                   /* E2 */
    struct bat_data data;                 /* E2 */
    uint32_t cnt[BAT_C__COUNT];
};

#define BAT_INC(b, C) ((b)->cnt[BAT_C_##C]++)

static inline bool bat_mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}
static inline bool bat_mac_is_group(const uint8_t *a)
{
    return (a[0] & 1) != 0;
}
static inline bool bat_mac_is_zero(const uint8_t *a)
{
    return (a[0] | a[1] | a[2] | a[3] | a[4] | a[5]) == 0;
}
static inline bool bat_mac_is_bcast(const uint8_t *a)
{
    return (a[0] & a[1] & a[2] & a[3] & a[4] & a[5]) == 0xFF;
}
static inline bool bat_mac_unicast_ok(const uint8_t *a)
{
    return !bat_mac_is_zero(a) && !bat_mac_is_group(a);
}
static inline bool bat_is_own(const struct bat *b, const uint8_t *a)
{
    return bat_mac_eq(a, b->cfg.hard_addr);
}
static inline uint32_t bat_age(const struct bat *b, uint32_t ts)
{
    return b->now - ts;
}
static inline bool bat_due(const struct bat *b, uint32_t t)
{
    return (int32_t)(b->now - t) >= 0;
}

/* E1 functions E2 calls */
struct bat_orig *bat_orig_find(struct bat *b, const uint8_t addr[6]);
struct bat_orig *bat_orig_get(struct bat *b, const uint8_t addr[6]);    /* find or create; NULL when full */
/* bat_orig_get for an ELP originator: when full, the worst-routed originator that is no
 * neighbour's own makes room (the OGM path keeps refusing, so it cannot flap back in). */
struct bat_orig *bat_orig_get_neigh(struct bat *b, const uint8_t addr[6]);
unsigned         bat_orig_index(const struct bat *b, const struct bat_orig *o);
struct bat_orig *bat_orig_at(struct bat *b, unsigned idx);              /* NULL if unused */
const uint8_t   *bat_route_nh(struct bat *b, const struct bat_orig *o, int tbl); /* router's hard addr or NULL */
uint32_t         bat_route_tput(const struct bat *b, const struct bat_orig *o);  /* default-table router tput, 0 if none */
struct bat_neigh *bat_neigh_find(struct bat *b, const uint8_t hard[6]);
/* frame[0..13] is link-header room; writes dst, our hard addr, 0x4305, then ops->tx. */
int  bat_link_tx(struct bat *b, const uint8_t dst[6], uint8_t *frame, size_t len);
/* inner frame -> soft interface: drops < 14 bytes and 0x4305 (or 802.1Q + 0x4305) inside. */
void bat_deliver(struct bat *b, const uint8_t *inner, size_t len);
/* re-dispatch a reassembled packet (depth 1); frame has a link header */
void bat_rx_dispatch(struct bat *b, uint8_t *frame, size_t len, unsigned depth);

/* E1-internal */
extern const uint8_t bat_bcast_addr[6];
uint32_t bat_rand(struct bat *b);
void bat_seq_note(struct bat *b);   /* after an own ELP, OGM or BCAST seqno is taken: cfg.seq_keep follows */
bool bat_deliver_frame(struct bat *b, const uint8_t *inner, size_t len); /* false = dropped */
uint32_t bat_neigh_tput(const struct bat_neigh *n);                 /* EWMA read, units */
struct bat_neigh *bat_neigh_get(struct bat *b, const uint8_t hard[6], const uint8_t orig[6]);
struct bat_cand *bat_cand_get(struct bat *b, struct bat_orig *o, unsigned neigh_idx);
void bat_elp_send(struct bat *b);
void bat_elp_rx(struct bat *b, uint8_t *frame, size_t len);
void bat_ogm_own(struct bat *b);
void bat_ogm_flush(struct bat *b);
void bat_ogm_rx(struct bat *b, uint8_t *frame, size_t len);
void bat_orig_purge(struct bat *b);
/* router of each table := best remaining candidate; some -> none tells TT (elp-ogm §6) */
void bat_orig_recompute(struct bat *b, struct bat_orig *o);
int  bat_bcast_tx_own(struct bat *b, const uint8_t *inner, size_t len);
void bat_bcast_rx(struct bat *b, uint8_t *frame, size_t len);
/* Sends the next waiting broadcast copy if the spacing allows; false = none waits, else *due = when
 * the next may go. */
bool bat_bcast_copies(struct bat *b, uint32_t *due);
/* With 2 or 3 broadcast copies: ops->tx took our last group frame under BAT_BC_COPY_MS ago. */
bool bat_grp_held(const struct bat *b);
uint32_t bat_grp_free_at(const struct bat *b);      /* when bat_grp_held turns false */
/* every-neighbour suppression rule of elp-ogm §3.9 / dataplane §5.3 */
bool bat_flood_suppressed(struct bat *b, const uint8_t orig[6], const uint8_t *from_link_src);

/* Render writer (bat_render.c): an entry is kept whole below len - RESERVE or output stops
 * before it; cursor values: 0 = summary line, else 1 + the entry's position. */
#define BAT_RENDER_RESERVE    32            /* room kept for the truncation marker */
#define BAT_RENDER_TRUNC      0xFFFFFFFEu   /* an entry longer than the whole chunk */
struct bat_wr {
    char *buf;
    size_t len, limit, pos, mark;
    uint32_t at, next;      /* cursor of the open entry; where the next chunk starts */
    bool stop, any;         /* an entry did not fit; an entry was kept */
};
bool bat_wr_begin(struct bat_wr *w, uint32_t at);   /* opens entry @at; false once stopped */
void bat_wr_end(struct bat_wr *w);                  /* closes it */
void bat_wr_commit(struct bat_wr *w, int n);
/* Every call inside an open entry appends to it (snprintf arguments; needs <stdio.h>). */
#define BAT_WR(w, ...) \
    bat_wr_commit((w), (w)->stop ? -1 : snprintf((w)->buf + (w)->pos, (w)->limit - (w)->pos, __VA_ARGS__))

#endif
