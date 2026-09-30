/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bat_internal.h"

void bat_data_init(struct bat *b)
{
    memset(&b->data, 0, sizeof(b->data));
}

int bat_send_to_orig(struct bat *b, struct bat_orig *o, uint8_t *frame, size_t len, int tbl)
{
    const uint8_t *nh = bat_route_nh(b, o, tbl);
    if (!nh) {
        return BAT_SEND_NOROUTE;
    }
    uint8_t hop[6];
    memcpy(hop, nh, 6);
    if (len - BAT_ETH_HLEN <= b->cfg.hard_mtu) {
        return bat_link_tx(b, hop, frame, len);
    }
    return bat_frag_tx(b, hop, o->addr, frame, len);
}

/* dataplane §4.2: TTL >= 2, known destination with an interface-table router, TTL - 1, send. */
bool bat_relay(struct bat *b, uint8_t *frame, size_t len)
{
    uint8_t *p = frame + BAT_ETH_HLEN;
    if (p[BAT_OFF_TTL] < 2) {
        BAT_INC(b, UC_TTL);
        return false;
    }
    struct bat_orig *o = bat_orig_find(b, p + BAT_OFF_UNI_DEST);
    if (!o || !bat_route_nh(b, o, BAT_TBL_IFACE)) {
        BAT_INC(b, UC_NOROUTE);
        return false;
    }
    p[BAT_OFF_TTL]--;
    int r = bat_send_to_orig(b, o, frame, len, BAT_TBL_IFACE);
    return r != BAT_SEND_NOROUTE && r != BAT_SEND_DROP;
}

int bat_data_tx_soft_unicast(struct bat *b, const uint8_t *inner, size_t len, uint16_t vid)
{
    struct bat_orig *o = bat_tt_resolve(b, inner, vid);
    if (!o || BAT_ETH_HLEN + BAT_UC_HLEN + len > sizeof(b->txbuf)) {
        BAT_INC(b, ST_NOROUTE);
        return -1;
    }
    uint8_t *p = b->txbuf + BAT_ETH_HLEN;
    p[BAT_OFF_TYPE] = BAT_PT_UNICAST;
    p[BAT_OFF_VERSION] = BAT_COMPAT;
    p[BAT_UC_TTL] = BAT_UC_TTL_INIT;
    p[BAT_UC_TTVN] = bat_tt_uc_ttvn(b, o);
    memcpy(p + BAT_UC_DEST, o->addr, 6);
    memcpy(p + BAT_UC_HLEN, inner, len);
    BAT_INC(b, ST_UNICAST);
    int r = bat_send_to_orig(b, o, b->txbuf, BAT_ETH_HLEN + BAT_UC_HLEN + len, BAT_TBL_DEFAULT);
    return (r == BAT_SEND_NOROUTE || r == BAT_SEND_DROP) ? -1 : 0;
}

/* dataplane §6.2 at the destination: group, own client, or a current TTVN is delivered;
 * a stale one is re-routed when TT names another originator, else dropped. */
void bat_unicast_rx(struct bat *b, uint8_t *frame, size_t len, unsigned depth)
{
    uint8_t *p = frame + BAT_ETH_HLEN;
    size_t pl = len - BAT_ETH_HLEN;
    size_t hl = p[BAT_OFF_TYPE] == BAT_PT_4ADDR ? BAT_4A_HLEN : BAT_UC_HLEN;
    (void)depth;
    if (pl < hl) {
        BAT_INC(b, RX_HDR);
        return;
    }
    BAT_INC(b, UC_RX);
    if (pl < hl + BAT_ETH_HLEN) {
        BAT_INC(b, UC_INNER_BAD);
        return;
    }
    if (!bat_is_own(b, p + BAT_UC_DEST)) {
        if (bat_relay(b, frame, len)) {
            BAT_INC(b, UC_FWD);
        }
        return;
    }
    const uint8_t *inner = p + hl;
    size_t il = pl - hl;
    uint16_t vid = bat_frame_vid(inner, il);
    if (!bat_mac_is_group(inner) && !bat_tt_is_own_client(b, inner, vid) &&
        bat_ttvn_older(p[BAT_UC_TTVN], bat_tt_own_ttvn(b))) {
        struct bat_orig *o = bat_tt_resolve(b, inner, vid);
        if (!o) {
            BAT_INC(b, UC_STALE_DROP);
            return;
        }
        memcpy(p + BAT_UC_DEST, o->addr, 6);
        p[BAT_UC_TTVN] = bat_tt_uc_ttvn(b, o);
        BAT_INC(b, UC_REROUTE);
        if (bat_relay(b, frame, len)) {
            BAT_INC(b, UC_FWD);
        }
        return;
    }
    if (hl == BAT_4A_HLEN &&
        (p[BAT_4A_SUBTYPE] == BAT_4A_ST_DAT_GET || p[BAT_4A_SUBTYPE] == BAT_4A_ST_DAT_PUT)) {
        BAT_INC(b, UC_4A_DAT);
        return;
    }
    if (!bat_deliver_frame(b, inner, il)) {
        return;
    }
    BAT_INC(b, UC_DELIVER);
    if (hl == BAT_4A_HLEN && p[BAT_4A_SUBTYPE] == BAT_4A_ST_DATA) {
        struct bat_orig *src = bat_orig_find(b, p + BAT_4A_SRC);
        if (src) {
            bat_tt_learn_temp(b, src, inner, il);
        }
    }
}

/* packets §3.4: 0x45..0x7F addressed to us are dropped, others relayed with byte 3 untouched. */
void bat_unknown_rx(struct bat *b, uint8_t *frame, size_t len)
{
    uint8_t *p = frame + BAT_ETH_HLEN;
    if (len - BAT_ETH_HLEN < BAT_UC_HLEN) {
        BAT_INC(b, RX_HDR);
        return;
    }
    if (bat_is_own(b, p + BAT_OFF_UNI_DEST)) {
        BAT_INC(b, UNK_SELF);
        return;
    }
    if (bat_relay(b, frame, len)) {
        BAT_INC(b, UNK_FWD);
    }
}
