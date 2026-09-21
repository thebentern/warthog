/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 */
#include "umac_mesh_ctrl.h"

#include <string.h>

uint16_t umac_mesh_ctrl_len(uint8_t ae)
{
    switch (ae & UMAC_MESH_CTRL_AE_MASK)
    {
        case UMAC_MESH_CTRL_AE_NONE: return 6u;
        case UMAC_MESH_CTRL_AE_A4:   return 12u;
        case UMAC_MESH_CTRL_AE_A5A6: return 18u;
        default:                     return 0u;
    }
}

uint16_t umac_mesh_ctrl_build(uint8_t *out, uint16_t out_len, const struct umac_mesh_ctrl *mc)
{
    if (out == NULL || mc == NULL)
    {
        return 0u;
    }
    uint8_t ae = umac_mesh_ctrl_ae(mc);
    uint16_t need = umac_mesh_ctrl_len(ae);
    if (need == 0u || out_len < need)
    {
        return 0u;
    }
    out[0] = mc->flags;
    out[1] = mc->ttl;
    out[2] = (uint8_t)(mc->seq);
    out[3] = (uint8_t)(mc->seq >> 8);
    out[4] = (uint8_t)(mc->seq >> 16);
    out[5] = (uint8_t)(mc->seq >> 24);
    if (ae >= UMAC_MESH_CTRL_AE_A4)
    {
        memcpy(&out[6], mc->eaddr1, 6);
    }
    if (ae == UMAC_MESH_CTRL_AE_A5A6)
    {
        memcpy(&out[12], mc->eaddr2, 6);
    }
    return need;
}

bool umac_mesh_ctrl_parse(const uint8_t *in, uint16_t in_len, struct umac_mesh_ctrl *out,
                          uint16_t *consumed)
{
    if (in == NULL || out == NULL || in_len < UMAC_MESH_CTRL_LEN_MIN)
    {
        return false;
    }
    uint8_t ae = (uint8_t)(in[0] & UMAC_MESH_CTRL_AE_MASK);
    uint16_t need = umac_mesh_ctrl_len(ae);
    if (need == 0u || in_len < need)
    {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->flags = in[0];
    out->ttl = in[1];
    out->seq = (uint32_t)in[2] | ((uint32_t)in[3] << 8) | ((uint32_t)in[4] << 16) |
               ((uint32_t)in[5] << 24);
    if (ae >= UMAC_MESH_CTRL_AE_A4)
    {
        memcpy(out->eaddr1, &in[6], 6);
    }
    if (ae == UMAC_MESH_CTRL_AE_A5A6)
    {
        memcpy(out->eaddr2, &in[12], 6);
    }
    if (consumed != NULL)
    {
        *consumed = need;
    }
    return true;
}
