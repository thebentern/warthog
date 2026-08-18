/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * CCMP framing: the 8-byte header, the AAD, and the nonce.
 *
 * Ported from hostap's wlantest/ccmp.c ccmp_aad_nonce() and kept freestanding
 * (libc only) so the host tests drive the real shipping code, the way
 * umac_mesh_ies.c is. Every field here is one a peer computes independently
 * and compares by way of a MIC: get a single masked bit wrong and every frame
 * is rejected as forged, with nothing to see on either side but a counter.
 * That is precisely the kind of thing worth pinning to golden bytes.
 *
 * Our mesh frames are the awkward case -- 4-address AND QoS -- so both
 * optional parts of the AAD are exercised in practice, not just in theory.
 *
 * Keep it freestanding. `make -C components/halow_mesh_compat/test
 * freestanding` fails the build if an SDK include creeps back in.
 */
#include "umac_mesh_ccmp_hdr.h"

#include <string.h>

#define FC_TYPE_MGMT 0u
#define FC_TYPE_DATA 2u
#define FC_TODS 0x0100u
#define FC_FROMDS 0x0200u
#define FC_RETRY 0x0800u
#define FC_PWRMGT 0x1000u
#define FC_MOREDATA 0x2000u
#define FC_PROTECTED 0x4000u
#define FC_ORDER 0x8000u /* HT Control present, when set on a QoS data frame */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}

bool umac_ccmp_is_4addr(const uint8_t *hdr)
{
    uint16_t fc = rd16(hdr);
    return (fc & (FC_TODS | FC_FROMDS)) == (FC_TODS | FC_FROMDS);
}

bool umac_ccmp_is_qos(const uint8_t *hdr)
{
    uint16_t fc = rd16(hdr);
    uint16_t type = (uint16_t)((fc >> 2) & 3u);
    uint16_t stype = (uint16_t)((fc >> 4) & 0xfu);
    return type == FC_TYPE_DATA && (stype & 0x08u) != 0u;
}

uint32_t umac_ccmp_hdr_len(const uint8_t *hdr)
{
    uint32_t n = 24u;
    if (umac_ccmp_is_4addr(hdr))
    {
        n += 6u;
    }
    if (umac_ccmp_is_qos(hdr))
    {
        n += 2u;
    }
    return n;
}

void umac_ccmp_write_header(uint8_t out[UMAC_CCMP_HDR_LEN], const uint8_t pn[6], uint8_t key_id)
{
    /* PN0 PN1 rsvd KeyID|ExtIV PN2 PN3 PN4 PN5 -- the split layout is the
     * standard's, not a mistake: the Ext IV bit and key id sit between the two
     * low PN octets and the four high ones. */
    out[0] = pn[5];
    out[1] = pn[4];
    out[2] = 0;
    out[3] = (uint8_t)(0x20u | ((key_id & 0x03u) << 6)); /* Ext IV always set for CCMP */
    out[4] = pn[3];
    out[5] = pn[2];
    out[6] = pn[1];
    out[7] = pn[0];
}

bool umac_ccmp_parse_header(const uint8_t in[UMAC_CCMP_HDR_LEN], uint8_t pn[6], uint8_t *key_id)
{
    if ((in[3] & 0x20u) == 0u)
    {
        return false; /* Ext IV clear: not a CCMP header (WEP/TKIP framing) */
    }
    pn[0] = in[7];
    pn[1] = in[6];
    pn[2] = in[5];
    pn[3] = in[4];
    pn[4] = in[1];
    pn[5] = in[0];
    if (key_id != NULL)
    {
        *key_id = (uint8_t)((in[3] >> 6) & 0x03u);
    }
    return true;
}

uint32_t umac_ccmp_build_aad(const uint8_t *hdr, uint8_t *aad)
{
    uint16_t fc = rd16(hdr);
    uint16_t type = (uint16_t)((fc >> 2) & 3u);
    bool addr4 = umac_ccmp_is_4addr(hdr);
    bool qos = umac_ccmp_is_qos(hdr);
    uint8_t *pos = aad;

    /* Subtype bits are masked for data frames so that, e.g., a QoS-Null and a
     * QoS-Data authenticate the same way; Retry/PwrMgt/MoreData are masked
     * because they legitimately change in flight and must not break the MIC.
     * Protected is forced on: the receiver computes the AAD after clearing it. */
    if (type == FC_TYPE_DATA)
    {
        fc &= (uint16_t)~0x0070u;
        if (qos)
        {
            fc &= (uint16_t)~FC_ORDER;
        }
    }
    fc &= (uint16_t)~(FC_RETRY | FC_PWRMGT | FC_MOREDATA);
    fc |= FC_PROTECTED;

    wr16(pos, fc);
    pos += 2;

    memcpy(pos, hdr + 4, 3u * 6u); /* A1, A2, A3 */
    pos += 3u * 6u;

    /* Sequence number is masked out; the fragment number is not. */
    wr16(pos, (uint16_t)(rd16(hdr + 22) & 0x000fu));
    pos += 2;

    if (addr4)
    {
        memcpy(pos, hdr + 24, 6u); /* A4 */
        pos += 6u;
    }

    if (qos)
    {
        /* TID only. The rest of the QoS control -- ack policy, EOSP, A-MSDU
         * present -- is masked, so a retransmission with a different ack
         * policy still authenticates. */
        const uint8_t *qc = hdr + (addr4 ? 30u : 24u);
        *pos++ = (uint8_t)(qc[0] & 0x0fu);
        *pos++ = 0x00;
    }

    return (uint32_t)(pos - aad);
}

void umac_ccmp_build_nonce(const uint8_t *hdr, const uint8_t pn[6], uint8_t nonce[13])
{
    uint16_t fc = rd16(hdr);
    uint16_t type = (uint16_t)((fc >> 2) & 3u);
    bool addr4 = umac_ccmp_is_4addr(hdr);

    nonce[0] = 0;
    if (type == FC_TYPE_DATA && umac_ccmp_is_qos(hdr))
    {
        const uint8_t *qc = hdr + (addr4 ? 30u : 24u);
        nonce[0] = (uint8_t)(qc[0] & 0x0fu); /* priority = TID */
    }
    else if (type == FC_TYPE_MGMT)
    {
        nonce[0] |= 0x10u; /* Management bit */
    }

    memcpy(&nonce[1], hdr + 10, 6u); /* A2 -- the transmitter */
    nonce[7] = pn[0];                /* PN big-endian from here */
    nonce[8] = pn[1];
    nonce[9] = pn[2];
    nonce[10] = pn[3];
    nonce[11] = pn[4];
    nonce[12] = pn[5];
}
