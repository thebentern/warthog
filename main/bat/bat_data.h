/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WARTHOG_BAT_DATA_H
#define WARTHOG_BAT_DATA_H
/* Data plane: unicast family, fragments, ICMP, UNICAST_TVLV. Via bat_internal.h. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct bat;
struct bat_orig;

#define BAT_FRAG_TIMEOUT_MS 10000u
#define BAT_FRAG_STEAL_MS   1000u  /* another originator's slot is taken only after this idle */
#define BAT_SEND_NOROUTE    (-10)  /* bat_send_to_orig: no next hop, nothing sent */
#define BAT_SEND_DROP       (-11)  /* bat_send_to_orig: not sendable (counted), nothing sent */

struct bat_frag_piece { uint8_t num, used; uint16_t off, len; };
struct bat_frag_slot {
    uint8_t  used, orig; uint16_t seq, total, have;   /* orig = originator index */
    uint32_t ts; uint8_t link_hdr[14];
    struct bat_frag_piece piece[BAT_FRAG_MAXN];
    uint8_t  buf[BAT_FRAG_BUF];
};
struct bat_data {
    struct bat_frag_slot slot[BAT_FRAG_SLOTS];
    uint8_t asm_frame[14 + BAT_FRAG_BUF];     /* reassembly output, link header in front */
    uint8_t fragbuf[14 + 1280];               /* one outgoing fragment */
};
void bat_data_init(struct bat *b);
void bat_unicast_rx(struct bat *b, uint8_t *frame, size_t len, unsigned depth); /* 0x40, 0x42 */
void bat_unknown_rx(struct bat *b, uint8_t *frame, size_t len);                 /* 0x45..0x7F */
void bat_utvlv_rx(struct bat *b, uint8_t *frame, size_t len);                   /* 0x44 */
void bat_frag_rx(struct bat *b, uint8_t *frame, size_t len, unsigned depth);    /* 0x41 */
void bat_icmp_rx(struct bat *b, uint8_t *frame, size_t len);                    /* 0x43 */
int  bat_data_tx_soft_unicast(struct bat *b, const uint8_t *inner, size_t len, uint16_t vid);
/* route toward o using table tbl, fragment if len - 14 > hard_mtu; frame has link-header room */
int  bat_send_to_orig(struct bat *b, struct bat_orig *o, uint8_t *frame, size_t len, int tbl);
void bat_frag_purge(struct bat *b);                    /* every 1 s */
void bat_frag_orig_gone(struct bat *b, unsigned orig_idx);
/* test hook: build and send an ICMP echo request */
int  bat_icmp_send_echo(struct bat *b, const uint8_t dst[6], uint8_t uid, uint16_t seq, size_t len);

/* E2-internal */
/* Relays a unicast-family packet not addressed to us (dataplane §4.2); true = handed to the link. */
bool    bat_relay(struct bat *b, uint8_t *frame, size_t len);
/* Cuts a packet into fragments toward next hop @nh (dataplane §7.2); frame has link-header room. */
int     bat_frag_tx(struct bat *b, const uint8_t nh[6], const uint8_t dest[6], const uint8_t *frame,
                    size_t len);
/* 802.1D priority of a unicast-family packet's client frame (dataplane §3.2, real PCP). */
uint8_t bat_frag_prio(const uint8_t *pkt, size_t len);
/* UNICAST_TVLV to originator @o (default route) whose @tvlv_len bytes of TVLVs sit at
 * txbuf + 14 + 20; @src is the source field. Returns bat_send_to_orig's result. */
int     bat_utvlv_send(struct bat *b, struct bat_orig *o, const uint8_t src[6], size_t tvlv_len);

#endif
