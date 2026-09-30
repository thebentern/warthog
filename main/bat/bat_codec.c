/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bat_codec.h"

int bat_tvlv_next(const uint8_t *area, size_t len, size_t *off, struct bat_tvlv *t)
{
    size_t o = *off;
    if (o > len || len - o < BAT_TVLV_HLEN) {
        return BAT_TVLV_END;
    }
    uint16_t vlen = bat_get16(area + o + 2);
    if (vlen > len - o - BAT_TVLV_HLEN) {
        return BAT_TVLV_OVERRUN;
    }
    t->type = area[o];
    t->version = area[o + 1];
    t->len = vlen;
    t->val = area + o + BAT_TVLV_HLEN;
    *off = o + BAT_TVLV_HLEN + vlen;
    return BAT_TVLV_OK;
}

size_t bat_hdr_len(uint8_t pt)
{
    switch (pt) {
    case BAT_PT_BCAST:   return BAT_BC_HLEN;
    case BAT_PT_ELP:     return BAT_ELP_HLEN;
    case BAT_PT_OGM2:    return BAT_OGM_HLEN;
    case BAT_PT_MCAST:   return BAT_MC_HLEN;
    case BAT_PT_UNICAST: return BAT_UC_HLEN;
    case BAT_PT_FRAG:    return BAT_FR_HLEN;
    case BAT_PT_4ADDR:   return BAT_4A_HLEN;
    case BAT_PT_ICMP:    return BAT_IC_HLEN;
    case BAT_PT_UTVLV:   return BAT_UT_HLEN;
    default:
        return (pt >= BAT_PT_UNI_FIRST && pt <= BAT_PT_UNI_LAST) ? BAT_UC_HLEN : 0;
    }
}
