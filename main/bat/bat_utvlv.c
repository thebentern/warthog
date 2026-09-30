/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bat_internal.h"

int bat_utvlv_send(struct bat *b, struct bat_orig *o, const uint8_t src[6], size_t tvlv_len)
{
    uint8_t *p = b->txbuf + BAT_ETH_HLEN;
    p[BAT_OFF_TYPE] = BAT_PT_UTVLV;
    p[BAT_OFF_VERSION] = BAT_COMPAT;
    p[BAT_UT_TTL] = BAT_UC_TTL_INIT;
    p[BAT_UT_RSVD] = 0;
    memcpy(p + BAT_UT_DEST, o->addr, 6);
    memmove(p + BAT_UT_SRC, src, 6);
    bat_put16(p + BAT_UT_TVLV_LEN, (uint16_t)tvlv_len);
    bat_put16(p + BAT_UT_ALIGN, 0);
    return bat_send_to_orig(b, o, b->txbuf, BAT_ETH_HLEN + BAT_UT_HLEN + tvlv_len, BAT_TBL_DEFAULT);
}

/* packets §10.5: every packet not addressed to us is relayed uninspected (deviation 2.4.5). */
void bat_utvlv_rx(struct bat *b, uint8_t *frame, size_t len)
{
    uint8_t *p = frame + BAT_ETH_HLEN;
    size_t pl = len - BAT_ETH_HLEN;
    if (pl < BAT_UT_HLEN) {
        BAT_INC(b, RX_HDR);
        return;
    }
    BAT_INC(b, UT_RX);
    size_t tl = bat_get16(p + BAT_UT_TVLV_LEN);
    if (tl > pl - BAT_UT_HLEN) {
        BAT_INC(b, UT_LEN);
        return;
    }
    if (!bat_is_own(b, p + BAT_UT_DEST)) {
        if (bat_relay(b, frame, len)) {
            BAT_INC(b, UT_FWD);
        }
        return;
    }
    uint8_t src[6];
    memcpy(src, p + BAT_UT_SRC, 6);
    struct bat_tvlv t;
    size_t off = 0;
    while (bat_tvlv_next(p + BAT_UT_HLEN, tl, &off, &t) == BAT_TVLV_OK) {
        if (t.version != 1) {
            continue;
        }
        if (t.type == BAT_TVLV_TT) {
            bat_tt_utvlv_rx(b, src, t.val, t.len);
        } else if (t.type == BAT_TVLV_ROAM) {
            BAT_INC(b, TT_ROAM_RX);
        }
    }
    BAT_INC(b, UT_CONSUMED);
}
