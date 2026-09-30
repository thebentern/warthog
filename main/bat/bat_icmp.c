/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bat_internal.h"

/* Turns the packet around toward its source (echo reply or TTL exceeded); false = no route. */
static bool send_back(struct bat *b, uint8_t *frame, size_t len, uint8_t msg)
{
    uint8_t *p = frame + BAT_ETH_HLEN;
    struct bat_orig *o = bat_orig_find(b, p + BAT_IC_ORIG);
    if (!o || !bat_route_nh(b, o, BAT_TBL_DEFAULT)) {
        BAT_INC(b, IC_UNKNOWN_SRC);
        return false;
    }
    memcpy(p + BAT_IC_DEST, p + BAT_IC_ORIG, 6);
    memcpy(p + BAT_IC_ORIG, b->cfg.hard_addr, 6);
    p[BAT_IC_MSGTYPE] = msg;
    p[BAT_IC_TTL] = BAT_UC_TTL_INIT;
    bat_send_to_orig(b, o, frame, len, BAT_TBL_DEFAULT);
    return true;
}

/* dataplane §8.1 / packets §13.2 */
void bat_icmp_rx(struct bat *b, uint8_t *frame, size_t len)
{
    uint8_t *p = frame + BAT_ETH_HLEN;
    size_t pl = len - BAT_ETH_HLEN;
    if (pl < BAT_IC_HLEN) {
        BAT_INC(b, RX_HDR);
        return;
    }
    BAT_INC(b, IC_RX);
    uint8_t mt = p[BAT_IC_MSGTYPE];
    bool echo = mt == BAT_IC_ECHO_REQ || mt == BAT_IC_ECHO_REPLY;
    if (echo && pl >= BAT_IC_RR_LEN) {
        if (p[BAT_IC_RRCOUNT] >= BAT_IC_RR_SLOTS) {
            BAT_INC(b, IC_RR_FULL);
            return;
        }
        memcpy(p + BAT_IC_HLEN + 6 * p[BAT_IC_RRCOUNT], frame + BAT_LINK_DST, 6);
        p[BAT_IC_RRCOUNT]++;
    }
    if (bat_is_own(b, p + BAT_IC_DEST)) {
        if (mt == BAT_IC_ECHO_REQ) {
            if (send_back(b, frame, len, BAT_IC_ECHO_REPLY)) {
                BAT_INC(b, IC_REPLY);
            }
        } else if (mt == BAT_IC_TP) {
            BAT_INC(b, IC_TP);
        } else {
            BAT_INC(b, IC_OTHER);
        }
        return;
    }
    if (p[BAT_IC_TTL] < 2) {
        if (mt != BAT_IC_ECHO_REQ) {
            BAT_INC(b, UC_TTL);
        } else if (send_back(b, frame, len, BAT_IC_TTL_EXC)) {
            BAT_INC(b, IC_TTLX);
        }
        return;
    }
    if (bat_relay(b, frame, len)) {
        BAT_INC(b, IC_FWD);
    }
}

int bat_icmp_send_echo(struct bat *b, const uint8_t dst[6], uint8_t uid, uint16_t seq, size_t len)
{
    struct bat_orig *o = bat_orig_find(b, dst);
    b->now = b->ops.now_ms(b->user);
    if (!o || len < BAT_IC_HLEN || BAT_ETH_HLEN + len > sizeof(b->txbuf) ||
        !bat_route_nh(b, o, BAT_TBL_DEFAULT)) {
        return -1;
    }
    uint8_t *p = b->txbuf + BAT_ETH_HLEN;
    memset(p, 0, len);
    p[BAT_OFF_TYPE] = BAT_PT_ICMP;
    p[BAT_OFF_VERSION] = BAT_COMPAT;
    p[BAT_IC_TTL] = BAT_UC_TTL_INIT;
    p[BAT_IC_MSGTYPE] = BAT_IC_ECHO_REQ;
    memcpy(p + BAT_IC_DEST, dst, 6);
    memcpy(p + BAT_IC_ORIG, b->cfg.hard_addr, 6);
    p[BAT_IC_UID] = uid;
    bat_put16(p + BAT_IC_SEQ, seq);
    if (len >= BAT_IC_RR_LEN) {
        p[BAT_IC_RRCOUNT] = 1;
        memcpy(p + BAT_IC_HLEN, b->cfg.hard_addr, 6);
    }
    int r = bat_send_to_orig(b, o, b->txbuf, BAT_ETH_HLEN + len, BAT_TBL_DEFAULT);
    return (r == BAT_SEND_NOROUTE || r == BAT_SEND_DROP) ? -1 : 0;
}
