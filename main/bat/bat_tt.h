/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WARTHOG_BAT_TT_H
#define WARTHOG_BAT_TT_H
/* Translation table: own clients, learned clients per originator, TTVN, CRC. Via bat_internal.h. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct bat;
struct bat_orig;
struct bat_wr;

/* wire: TT TVLV flags byte (low nibble = kind) and client flags */
#define BAT_TT_OGM_DIFF     0x01
#define BAT_TT_REQUEST      0x02
#define BAT_TT_RESPONSE     0x04
#define BAT_TT_FULL_TABLE   0x10
#define BAT_TT_CLIENT_DEL   0x01
#define BAT_TT_CLIENT_ROAM  0x02
#define BAT_TT_SYNC_MASK    0xF0   /* flags that enter the CRC */
#define BAT_TT_VLAN_LEN     8
#define BAT_TT_CHANGE_LEN   12
#define BAT_VID_TAGGED      0x8000

#define BAT_TTR_TEMP        0x01   /* row flags low nibble: learned from data (tt §8.3) */
enum { BAT_TTL_FREE, BAT_TTL_NEW, BAT_TTL_ON, BAT_TTL_DEL };   /* local entry state */

#define BAT_TT_RESENDS      3
#define BAT_TT_TEMP_MS      600000u
#define BAT_TT_TEMP_MAX     (BAT_TT_ROWS / 4)   /* temporary rows (hardening; Linux has no cap) */
#define BAT_TT_REQ_MS       3000u
#define BAT_TT_REQ_JOIN_MS  500u     /* until a table is first held, as Linux asks at every OGM (tt §6.1) */
#define BAT_TT_REQ_MAX_MS   60000u   /* after answers that could not be taken (hardening) */
#define BAT_TT_ANSWER_MS    500u     /* Linux re-requests once per OGM (1 s on OpenMANET) */

struct bat_tt_orig {
    uint8_t  ttvn, known, req_pending, answered;
    uint8_t  backoff, stalled;     /* request wait = BAT_TT_REQ_MS << backoff (req_wait); this one backed off */
    uint8_t  ann, ann_valid;       /* TTVN of its last OGM with a TT container */
    uint32_t req_ts, ans_ts;
};
struct bat_tt_row  { uint8_t mac[6]; uint16_t vid; uint8_t orig, flags, ttvn, used; uint32_t ts; };
struct bat_tt_local { uint8_t mac[6]; uint16_t vid; uint8_t flags, state; };
struct bat_tt_change { uint8_t mac[6]; uint16_t vid; uint8_t flags; };
struct bat_tt {
    uint8_t ttvn, resend, n_local, n_changes;
    struct bat_tt_local  local[BAT_TT_LOCAL_MAX];
    struct bat_tt_change changes[BAT_TT_CHANGES_MAX];
    uint8_t  last_tvlv[256]; uint16_t last_tvlv_len;   /* for the 3 re-sends */
    uint16_t nrows;
    struct bat_tt_row rows[BAT_TT_ROWS];
};

void   bat_tt_init(struct bat *b);                         /* adds soft MAC, uncommitted */
size_t bat_tt_ogm_tvlv_build(struct bat *b, uint8_t *out, size_t max); /* commit + container(s) */
void   bat_tt_ogm_rx(struct bat *b, struct bat_orig *o, const uint8_t *val, size_t len);
void   bat_tt_orig_gone(struct bat *b, struct bat_orig *o);
void   bat_tt_purge(struct bat *b);                        /* every 5 s */
void   bat_tt_learn_temp(struct bat *b, struct bat_orig *o, const uint8_t *inner, size_t len);
struct bat_orig *bat_tt_resolve(struct bat *b, const uint8_t mac[6], uint16_t vid);
bool   bat_tt_is_own_client(const struct bat *b, const uint8_t mac[6], uint16_t vid);
uint8_t  bat_tt_own_ttvn(const struct bat *b);
unsigned bat_tt_rows_used(const struct bat *b);
/* bat_render_from's TT listings into @w, from cursor @from (bat_internal.h writer). */
void   bat_tt_render_global(struct bat *b, struct bat_wr *w, const uint8_t *mac, uint32_t from);
void   bat_tt_render_local(struct bat *b, struct bat_wr *w, uint32_t from);
/* test hooks (not in bat.h, never called by the firmware) */
int    bat_tt_local_add(struct bat *b, const uint8_t mac[6], uint16_t vid, uint8_t flags);
int    bat_tt_local_del(struct bat *b, const uint8_t mac[6], uint16_t vid);

/* E2-internal */
/* TT v1 container value of a UNICAST_TVLV addressed to us; @src = its source originator. */
void     bat_tt_utvlv_rx(struct bat *b, const uint8_t src[6], const uint8_t *val, size_t len);
/* A fragment addressed to us of a packet too big to reassemble, its payload @pkt (@len bytes): the
 * head piece of a TT response names the table's owner, whose pending request's answer cannot be taken. */
void     bat_tt_answer_toobig(struct bat *b, const uint8_t *pkt, size_t len);
/* TTVN byte of a unicast header to @o (tt §9.1 step 5, with the hardening in bat_tt.c). */
uint8_t  bat_tt_uc_ttvn(struct bat *b, const struct bat_orig *o);
/* VID field of a client frame: 0x8000 | VLAN id for an 802.1Q tag, else 0. */
uint16_t bat_frame_vid(const uint8_t *inner, size_t len);
/* CRC of the rows of originator @oi in @vid (temporary rows excluded); *n = rows counted. */
uint32_t bat_tt_orig_crc(const struct bat *b, unsigned oi, uint16_t vid, unsigned *n);
/* x is older than y (dataplane §6.1: (y - x) mod 256 in 1..127). */
static inline bool bat_ttvn_older(uint8_t x, uint8_t y)
{
    uint8_t d = (uint8_t)(y - x);
    return d >= 1 && d <= 127;
}

#endif
