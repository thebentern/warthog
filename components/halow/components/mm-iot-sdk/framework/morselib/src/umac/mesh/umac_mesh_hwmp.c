/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * HWMP PREQ/PREP encode and decode. See umac_mesh_hwmp.h for why this exists.
 *
 * Every offset here is byte-wise on purpose. The fields are not naturally
 * aligned inside the element -- orig_sn lands at element-payload offset 13 and
 * target_sn at 33 -- so casting a packed struct over the body is an unaligned
 * load on Xtensa. Assemble and read one octet at a time.
 *
 * Keep it freestanding.
 */
#include "umac_mesh_hwmp.h"

#include <string.h>

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

bool hwmp_sn_gt(uint32_t a, uint32_t b)
{
    /* Serial-number arithmetic: a is newer than b when b - a is negative as a
     * signed 32-bit value. Equal is not greater. */
    return ((int32_t)(b - a)) < 0;
}

uint16_t umac_mesh_hwmp_build_preq(uint8_t *out, uint16_t out_len, const uint8_t *orig_addr,
                                   uint32_t orig_sn, uint32_t preq_id, const uint8_t *target_addr,
                                   uint32_t lifetime_tu)
{
    if (out == NULL || orig_addr == NULL || target_addr == NULL ||
        out_len < HWMP_PREQ_BODY_LEN)
    {
        return 0;
    }

    out[0] = HWMP_CATEGORY_MESH;
    out[1] = HWMP_ACTION_PATH_SELECTION;
    out[2] = HWMP_EID_PREQ;
    out[3] = HWMP_PREQ_ELEM_LEN;
    out[4] = 0x00;             /* flags: no gate, no proactive PREP, no AE */
    out[5] = 0x00;             /* hop count -- the receiver adds one */
    out[6] = HWMP_DEFAULT_TTL; /* element TTL */
    wr32(&out[7], preq_id);
    memcpy(&out[11], orig_addr, HWMP_ADDR_LEN);
    wr32(&out[17], orig_sn);
    wr32(&out[21], lifetime_tu);
    wr32(&out[25], 0u); /* metric -- the receiver adds its own last hop */
    out[29] = 1u;       /* target count; mac80211 accepts exactly one */
    /* Target Only + Unknown Sequence Number: we want the target itself to
     * answer, and we have no idea what its sequence number is. */
    out[30] = (uint8_t)(HWMP_TGT_FLAG_TO | HWMP_TGT_FLAG_USN);
    memcpy(&out[31], target_addr, HWMP_ADDR_LEN);
    wr32(&out[37], 0u); /* target SN -- meaningless while USN is set */

    return HWMP_PREQ_BODY_LEN;
}

uint16_t umac_mesh_hwmp_build_prep(uint8_t *out, uint16_t out_len, const struct hwmp_preq *preq,
                                   const uint8_t *own_addr, uint32_t own_sn)
{
    if (out == NULL || preq == NULL || own_addr == NULL || out_len < HWMP_PREP_BODY_LEN)
    {
        return 0;
    }

    out[0] = HWMP_CATEGORY_MESH;
    out[1] = HWMP_ACTION_PATH_SELECTION;
    out[2] = HWMP_EID_PREP;
    out[3] = HWMP_PREP_ELEM_LEN;
    out[4] = 0x00;             /* flags: no AE */
    out[5] = 0x00;             /* hop count -- we ARE the target */
    out[6] = HWMP_DEFAULT_TTL;
    /* Target = us, the node that answers. */
    memcpy(&out[7], own_addr, HWMP_ADDR_LEN);
    wr32(&out[13], own_sn);
    /* Lifetime is echoed: the peer only ever extends its expiry, never cuts it. */
    wr32(&out[17], preq->lifetime);
    wr32(&out[21], 0u); /* metric */
    /* Originator = whoever asked. Copied verbatim, including the sequence
     * number: the peer matches the reply to its own request on these two. */
    memcpy(&out[25], preq->orig_addr, HWMP_ADDR_LEN);
    wr32(&out[31], preq->orig_sn);

    return HWMP_PREP_BODY_LEN;
}

bool umac_mesh_hwmp_parse_preq(const uint8_t *body, uint16_t len, struct hwmp_preq *out)
{
    if (body == NULL || out == NULL || len < HWMP_PREQ_BODY_LEN)
    {
        return false;
    }
    if (body[0] != HWMP_CATEGORY_MESH || body[1] != HWMP_ACTION_PATH_SELECTION)
    {
        return false;
    }
    if (body[2] != HWMP_EID_PREQ)
    {
        return false;
    }
    /* The element must be long enough for the fields we are about to read.
     * Trusting the length octet over the buffer is how a short frame becomes
     * an out-of-bounds read. */
    if (body[3] < HWMP_PREQ_ELEM_LEN || (uint16_t)(4 + body[3]) > len)
    {
        return false;
    }
    /* Address Extension changes the layout and mac80211 drops these anyway.
     * Refuse rather than model a shape we never emit. */
    if ((body[4] & HWMP_FLAG_AE) != 0u)
    {
        return false;
    }
    if (body[29] != 1u)
    {
        return false; /* exactly one target, or we cannot represent it */
    }

    out->flags = body[4];
    out->hop_count = body[5];
    out->ttl = body[6];
    out->preq_id = rd32(&body[7]);
    memcpy(out->orig_addr, &body[11], HWMP_ADDR_LEN);
    out->orig_sn = rd32(&body[17]);
    out->lifetime = rd32(&body[21]);
    out->metric = rd32(&body[25]);
    out->target_count = body[29];
    out->target_flags = body[30];
    memcpy(out->target_addr, &body[31], HWMP_ADDR_LEN);
    out->target_sn = rd32(&body[37]);
    return true;
}

uint32_t umac_mesh_hwmp_next_own_sn(uint32_t cur, const struct hwmp_preq *preq)
{
    /* Only consider the peer's value when it claims to have one. */
    if (preq != NULL && (preq->target_flags & HWMP_TGT_FLAG_USN) == 0u &&
        hwmp_sn_gt(preq->target_sn, cur))
    {
        cur = preq->target_sn;
    }
    return cur + 1u; /* wraps at 2^32, which the comparison is built for */
}

bool umac_mesh_hwmp_targets_us(const struct hwmp_preq *preq, const uint8_t *own_addr)
{
    if (preq == NULL || own_addr == NULL)
    {
        return false;
    }
    return memcmp(preq->target_addr, own_addr, HWMP_ADDR_LEN) == 0;
}

/* ---- Forwarding ------------------------------------------------------- */

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint8_t umac_mesh_hwmp_element_id(const uint8_t *body, uint16_t len)
{
    if (body == NULL || len < 4u || body[0] != HWMP_CATEGORY_MESH ||
        body[1] != HWMP_ACTION_PATH_SELECTION)
    {
        return 0u;
    }
    return body[2];
}

bool umac_mesh_hwmp_parse_prep(const uint8_t *body, uint16_t len, struct hwmp_prep *out)
{
    if (body == NULL || out == NULL || len < HWMP_PREP_BODY_LEN)
    {
        return false;
    }
    if (body[0] != HWMP_CATEGORY_MESH || body[1] != HWMP_ACTION_PATH_SELECTION ||
        body[2] != HWMP_EID_PREP)
    {
        return false;
    }
    if (body[3] < HWMP_PREP_ELEM_LEN || (uint16_t)(4 + body[3]) > len)
    {
        return false;
    }
    if ((body[4] & HWMP_FLAG_AE) != 0u)
    {
        return false;
    }
    out->flags = body[4];
    out->hop_count = body[5];
    out->ttl = body[6];
    memcpy(out->target_addr, &body[7], HWMP_ADDR_LEN);
    out->target_sn = rd32(&body[13]);
    out->lifetime = rd32(&body[17]);
    out->metric = rd32(&body[21]);
    memcpy(out->orig_addr, &body[25], HWMP_ADDR_LEN);
    out->orig_sn = rd32(&body[31]);
    return true;
}

uint16_t umac_mesh_hwmp_build_preq_fwd(uint8_t *out, uint16_t out_len,
                                       const struct hwmp_preq *preq, uint32_t link_metric)
{
    if (out == NULL || preq == NULL || out_len < HWMP_PREQ_BODY_LEN || preq->ttl <= 1u)
    {
        return 0;
    }
    out[0] = HWMP_CATEGORY_MESH;
    out[1] = HWMP_ACTION_PATH_SELECTION;
    out[2] = HWMP_EID_PREQ;
    out[3] = HWMP_PREQ_ELEM_LEN;
    out[4] = preq->flags;
    out[5] = (uint8_t)(preq->hop_count + 1u);
    out[6] = (uint8_t)(preq->ttl - 1u);
    wr32(&out[7], preq->preq_id);
    memcpy(&out[11], preq->orig_addr, HWMP_ADDR_LEN);
    wr32(&out[17], preq->orig_sn);
    wr32(&out[21], preq->lifetime);
    wr32(&out[25], preq->metric + link_metric);
    out[29] = 1u;
    out[30] = preq->target_flags;
    memcpy(&out[31], preq->target_addr, HWMP_ADDR_LEN);
    wr32(&out[37], preq->target_sn);
    return HWMP_PREQ_BODY_LEN;
}

uint16_t umac_mesh_hwmp_build_prep_fwd(uint8_t *out, uint16_t out_len,
                                       const struct hwmp_prep *prep, uint32_t link_metric)
{
    if (out == NULL || prep == NULL || out_len < HWMP_PREP_BODY_LEN || prep->ttl <= 1u)
    {
        return 0;
    }
    out[0] = HWMP_CATEGORY_MESH;
    out[1] = HWMP_ACTION_PATH_SELECTION;
    out[2] = HWMP_EID_PREP;
    out[3] = HWMP_PREP_ELEM_LEN;
    out[4] = prep->flags;
    out[5] = (uint8_t)(prep->hop_count + 1u);
    out[6] = (uint8_t)(prep->ttl - 1u);
    memcpy(&out[7], prep->target_addr, HWMP_ADDR_LEN);
    wr32(&out[13], prep->target_sn);
    wr32(&out[17], prep->lifetime);
    wr32(&out[21], prep->metric + link_metric);
    memcpy(&out[25], prep->orig_addr, HWMP_ADDR_LEN);
    wr32(&out[31], prep->orig_sn);
    return HWMP_PREP_BODY_LEN;
}

uint16_t umac_mesh_hwmp_build_perr(uint8_t *out, uint16_t out_len, uint8_t ttl,
                                   const uint8_t *dest_addr, uint32_t dest_sn, uint16_t reason)
{
    if (out == NULL || dest_addr == NULL || out_len < HWMP_PERR_BODY_LEN)
    {
        return 0;
    }
    out[0] = HWMP_CATEGORY_MESH;
    out[1] = HWMP_ACTION_PATH_SELECTION;
    out[2] = HWMP_EID_PERR;
    out[3] = HWMP_PERR_ELEM_LEN;
    out[4] = ttl;
    out[5] = 1u;    /* number of destinations */
    out[6] = 0x00;  /* flags: no AE */
    memcpy(&out[7], dest_addr, HWMP_ADDR_LEN);
    wr32(&out[13], dest_sn);
    wr16(&out[17], reason);
    return HWMP_PERR_BODY_LEN;
}

bool umac_mesh_hwmp_parse_perr(const uint8_t *body, uint16_t len, struct hwmp_perr *out)
{
    if (body == NULL || out == NULL || len < HWMP_PERR_BODY_LEN)
    {
        return false;
    }
    if (body[0] != HWMP_CATEGORY_MESH || body[1] != HWMP_ACTION_PATH_SELECTION ||
        body[2] != HWMP_EID_PERR)
    {
        return false;
    }
    if (body[3] < HWMP_PERR_ELEM_LEN || (uint16_t)(4 + body[3]) > len)
    {
        return false;
    }
    if (body[5] != 1u || (body[6] & HWMP_PERR_FLAG_AE) != 0u)
    {
        return false; /* one destination, no AE, or we cannot represent it */
    }
    out->ttl = body[4];
    out->flags = body[6];
    memcpy(out->dest_addr, &body[7], HWMP_ADDR_LEN);
    out->dest_sn = rd32(&body[13]);
    out->reason = rd16(&body[17]);
    return true;
}
