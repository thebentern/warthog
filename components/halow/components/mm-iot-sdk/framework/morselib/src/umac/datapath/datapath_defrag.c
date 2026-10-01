/*
 * Copyright 2022-2024 Morse Micro
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 */

#include <string.h>

#include "mmlog.h"
#include "mmosal.h"
#include "umac/core/umac_core.h"
#include "umac/datapath/datapath_defrag.h"
#include "dot11/dot11_utils.h"
#include "common/mac_address.h"


#define DEFRAG_TIMEOUT_MS (1000)

/* warthog: chains held at once on the whole node, and per peer (mac80211 keeps 4 a station). */
#define DEFRAG_CHAINS_MAX      (4)
#define DEFRAG_CHAINS_PER_PEER (2)

/* warthog: QoS TIDs 0-7, and 8 (non-QoS data, which TID 8 shares); a higher TID has no chain. */
#define DEFRAG_TID_IDX_MAX (MMWLAN_MAX_QOS_TID + 1)

/* A chain's buffer: the first fragment's MAC header, then up to the largest MSDU body. */
#define FRAG_CHAIN_HDR_SPACE  (sizeof(struct dot11_data_hdr))
#define FRAG_CHAIN_BODY_SPACE (DOT11_MAX_PAYLOAD_LEN)

/* warthog: QoS Control's A-MSDU Present, and the bits every fragment of one MSDU shares: the TID,
 * and on the mesh Mesh Control Present (in a BSS bit 8 is TXOP or queue size, which may change). */
#define DEFRAG_QOS_AMSDU     (0x0080u)
#define DEFRAG_QOS_SAME      (0x000fu)
#define DEFRAG_QOS_SAME_MESH (0x010fu)

/* warthog: frame control's protocol version, type, subtype, To DS and From DS. */
#define DEFRAG_FC_SAME_MASK (0x03ffu)

/* warthog: AT+DEFRAG? (storage in main/at.c); a fragment dropped here is rxdrop 97. */
extern volatile uint32_t g_warthog_defrag_in, g_warthog_defrag_ok, g_warthog_defrag_nofirst,
    g_warthog_defrag_order, g_warthog_defrag_pn, g_warthog_defrag_key, g_warthog_defrag_prot,
    g_warthog_defrag_hdr, g_warthog_defrag_amsdu, g_warthog_defrag_oversize,
    g_warthog_defrag_nomem, g_warthog_defrag_expired, g_warthog_defrag_restart,
    g_warthog_defrag_flush, g_warthog_defrag_evict;
extern volatile uint32_t g_warthog_rxdrop_reason, g_warthog_rxdrop_count;

/* warthog: one MSDU being reassembled. The node's chains sit in one table outside the peer
 * records; @c owner, its peer's defrag data, is compared and never dereferenced. */
struct datapath_defrag_chain
{
    uint64_t pn; /* the last fragment's CCMP PN */
    const struct datapath_defrag_data *owner;
    struct mmpkt *buf; /* the first fragment's MAC header, then the bodies taken so far */
    uint32_t key_gen;
    uint32_t first_ms; /* the first fragment's arrival, for expiry */
    uint32_t age;      /* begin order, for eviction */
    uint16_t sequence_number;
    uint16_t qos; /* the first fragment's QoS Control */
    uint8_t tid_idx;
    uint8_t last_frag;
    uint8_t key_id;
    bool is_protected;
};

static struct datapath_defrag_chain s_chains[DEFRAG_CHAINS_MAX];
static uint32_t s_begun;

static void datapath_defrag_timeout(void *arg1, void *arg2);

static bool datapath_defrag_older(const struct datapath_defrag_chain *a,
                                  const struct datapath_defrag_chain *b)
{
    return b == NULL || (int32_t)(a->age - b->age) < 0;
}

static void datapath_defrag_release(struct datapath_defrag_chain *frag_chain)
{
    mmpkt_release(frag_chain->buf);
    memset(frag_chain, 0, sizeof(*frag_chain));
}

/* Release every chain DEFRAG_TIMEOUT_MS past its first fragment, whatever its sequence number. */
static void datapath_defrag_sweep(void)
{
    for (unsigned i = 0; i < DEFRAG_CHAINS_MAX; i++)
    {
        struct datapath_defrag_chain *frag_chain = &s_chains[i];
        if (frag_chain->buf != NULL &&
            mmosal_time_has_passed(frag_chain->first_ms + DEFRAG_TIMEOUT_MS))
        {
            datapath_defrag_release(frag_chain);
            g_warthog_defrag_expired++;
        }
    }
}

/* Sweep, then keep one core timeout, at the oldest chain's expiry, while any chain is held. One
 * the core cannot take is tried again at the next call; the sweeps cover the gap. */
static void datapath_defrag_settle(struct umac_data *umacd)
{
    datapath_defrag_sweep();
    const struct datapath_defrag_chain *oldest = NULL;
    for (unsigned i = 0; i < DEFRAG_CHAINS_MAX; i++)
    {
        const struct datapath_defrag_chain *frag_chain = &s_chains[i];
        if (frag_chain->buf != NULL &&
            (oldest == NULL || mmosal_time_lt(frag_chain->first_ms, oldest->first_ms)))
        {
            oldest = frag_chain;
        }
    }
    (void)umac_core_cancel_timeout(umacd, datapath_defrag_timeout, umacd, NULL);
    if (oldest != NULL &&
        !umac_core_register_timeout(umacd,
                                    oldest->first_ms + DEFRAG_TIMEOUT_MS - mmosal_get_time_ms(),
                                    datapath_defrag_timeout, umacd, NULL))
    {
        MMLOG_WRN("Failed to register timeout for defrag\n");
    }
}

static void datapath_defrag_timeout(void *arg1, void *arg2)
{
    MM_UNUSED(arg2);
    datapath_defrag_settle((struct umac_data *)arg1);
}

void datapath_defrag_expire(struct umac_data *umacd)
{
    for (unsigned i = 0; i < DEFRAG_CHAINS_MAX; i++)
    {
        if (s_chains[i].buf != NULL)
        {
            datapath_defrag_settle(umacd);
            return;
        }
    }
}

/* @p data's chain for @p tid_idx, or NULL. */
static struct datapath_defrag_chain *datapath_defrag_find(const struct datapath_defrag_data *data,
                                                          uint8_t tid_idx)
{
    for (unsigned i = 0; i < DEFRAG_CHAINS_MAX; i++)
    {
        struct datapath_defrag_chain *frag_chain = &s_chains[i];
        if (frag_chain->buf != NULL && frag_chain->owner == data && frag_chain->tid_idx == tid_idx)
        {
            return frag_chain;
        }
    }
    return NULL;
}

/* An empty entry for a new chain of @p data's: the peer's oldest is evicted if it holds
 * DEFRAG_CHAINS_PER_PEER, else the node's oldest if the table is full. */
static struct datapath_defrag_chain *datapath_defrag_slot(const struct datapath_defrag_data *data)
{
    struct datapath_defrag_chain *empty = NULL, *oldest = NULL, *own_oldest = NULL;
    unsigned own = 0;
    for (unsigned i = 0; i < DEFRAG_CHAINS_MAX; i++)
    {
        struct datapath_defrag_chain *frag_chain = &s_chains[i];
        if (frag_chain->buf == NULL)
        {
            empty = empty != NULL ? empty : frag_chain;
            continue;
        }
        oldest = datapath_defrag_older(frag_chain, oldest) ? frag_chain : oldest;
        if (frag_chain->owner == data)
        {
            own++;
            own_oldest = datapath_defrag_older(frag_chain, own_oldest) ? frag_chain : own_oldest;
        }
    }
    struct datapath_defrag_chain *victim =
        own >= DEFRAG_CHAINS_PER_PEER ? own_oldest : (empty == NULL ? oldest : NULL);
    if (victim == NULL)
    {
        return empty;
    }
    MMLOG_INF("Evicting a frag chain to begin another.\n");
    datapath_defrag_release(victim);
    g_warthog_defrag_evict++;
    return victim;
}

/* A buffer for a new chain holding @p first's MAC header: from the heap, outside the chip's RX
 * pool, so reassembly never takes a block chip RX needs. */
static bool datapath_defrag_begin(struct datapath_defrag_chain *frag_chain,
                                  const struct dot11_data_hdr *first)
{
    frag_chain->buf = mmpkt_alloc_on_heap(FRAG_CHAIN_HDR_SPACE, FRAG_CHAIN_BODY_SPACE, 0);
    if (frag_chain->buf == NULL)
    {
        MMLOG_WRN("Failed to allocate a frag chain buffer\n");
        return false;
    }
    struct mmpktview *view = mmpkt_open(frag_chain->buf);
    mmpkt_prepend_data(view, (const uint8_t *)first, dot11_data_hdr_get_len(first));
    mmpkt_close(&view);
    return true;
}

/* warthog: is @p hdr the same frame as the chain's first fragment: type, subtype, DS bits and
 * every address (mac80211 compares type, addr1 and addr2; the mesh reads addr3 and addr4 too). */
static bool datapath_defrag_same_frame(const struct dot11_data_hdr *first,
                                       const struct dot11_data_hdr *hdr)
{
    if (((first->base.frame_control ^ hdr->base.frame_control) & htole16(DEFRAG_FC_SAME_MASK)) != 0)
    {
        return false;
    }
    if (!mm_mac_addr_is_equal(first->base.addr1, hdr->base.addr1) ||
        !mm_mac_addr_is_equal(first->base.addr2, hdr->base.addr2) ||
        !mm_mac_addr_is_equal(first->base.addr3, hdr->base.addr3))
    {
        return false;
    }
    return !dot11_is_4addr_hdr(hdr->base.frame_control) ||
           mm_mac_addr_is_equal(first->addr4, hdr->addr4);
}

struct mmpkt *datapath_defrag(struct umac_data *umacd,
                              struct datapath_defrag_data *data,
                              const struct dot11_data_hdr **data_hdr,
                              struct mmpktview **rxbufview,
                              struct mmpkt *rxbuf,
                              uint8_t tid_idx,
                              const struct datapath_defrag_mpdu *mpdu)
{
    const struct dot11_hdr *header = &(*data_hdr)->base;

    if (!datapath_defrag_is_fragment(header))
    {
        return rxbuf;
    }

    g_warthog_defrag_in++;
    const uint8_t frag =
        (uint8_t)dot11_sequence_control_get_fragment_number(header->sequence_control);
    const uint16_t seq = dot11_sequence_control_get_sequence_number(header->sequence_control);
    const uint16_t qos_same = mpdu->mesh ? DEFRAG_QOS_SAME_MESH : DEFRAG_QOS_SAME;
    volatile uint32_t *drop = NULL;
    struct mmpkt *return_buffer = NULL;
    struct mmpktview *return_view = NULL;
    struct datapath_defrag_chain *frag_chain = NULL;

    datapath_defrag_sweep();
    if (tid_idx > DEFRAG_TID_IDX_MAX)
    {
        drop = &g_warthog_defrag_hdr;
        goto exit;
    }
    /* An A-MSDU is never fragmented; its flag is not covered by the MIC (CVE-2020-24588). */
    if ((mpdu->qos & DEFRAG_QOS_AMSDU) != 0)
    {
        drop = &g_warthog_defrag_amsdu;
        goto exit;
    }

    if (frag == 0)
    {
        frag_chain = datapath_defrag_find(data, tid_idx);
        if (frag_chain != NULL)
        {
            MMLOG_WRN("Dropping existing frag_chain_buffer as new chain has begun.\n");
            datapath_defrag_release(frag_chain);
            g_warthog_defrag_restart++;
        }
        else
        {
            frag_chain = datapath_defrag_slot(data);
        }
        if (!datapath_defrag_begin(frag_chain, *data_hdr))
        {
            drop = &g_warthog_defrag_nomem;
            goto exit;
        }
        frag_chain->owner = data;
        frag_chain->tid_idx = tid_idx;
        frag_chain->sequence_number = seq;
        frag_chain->last_frag = 0;
        frag_chain->is_protected = mpdu->is_protected;
        frag_chain->key_id = mpdu->key_id;
        frag_chain->key_gen = mpdu->key_gen;
        frag_chain->pn = mpdu->pn;
        frag_chain->qos = mpdu->qos;
        frag_chain->first_ms = mmosal_get_time_ms();
        frag_chain->age = s_begun++;
    }
    else
    {
        /* As mac80211's ieee80211_rx_h_defragment: a later fragment joins only the chain of its
         * sequence number, as the next fragment, under the first's protection, key and
         * consecutive PNs. A mismatch drops the fragment and keeps the chain. */
        frag_chain = datapath_defrag_find(data, tid_idx);
        if (frag_chain == NULL || frag_chain->sequence_number != seq)
        {
            MMLOG_INF("Missed the first fragment for this chain.\n");
            drop = &g_warthog_defrag_nofirst;
            goto exit;
        }
        if (frag != (uint8_t)(frag_chain->last_frag + 1u))
        {
            drop = &g_warthog_defrag_order;
            goto exit;
        }
        struct mmpktview *first_view = mmpkt_open(frag_chain->buf);
        const bool same = datapath_defrag_same_frame(
            (const struct dot11_data_hdr *)mmpkt_get_data_start(first_view), *data_hdr);
        mmpkt_close(&first_view);
        if (!same || ((mpdu->qos ^ frag_chain->qos) & qos_same) != 0)
        {
            drop = &g_warthog_defrag_hdr;
            goto exit;
        }
        if (mpdu->is_protected != frag_chain->is_protected)
        {
            MMLOG_WRN("Fragment does not match the current chain's protection status.\n");
            drop = &g_warthog_defrag_prot;
            goto exit;
        }
        if (mpdu->is_protected &&
            (mpdu->key_id != frag_chain->key_id || mpdu->key_gen != frag_chain->key_gen))
        {
            drop = &g_warthog_defrag_key;
            goto exit;
        }
        if (mpdu->is_protected && mpdu->pn != frag_chain->pn + 1u)
        {
            drop = &g_warthog_defrag_pn;
            goto exit;
        }
        frag_chain->last_frag = frag;
        frag_chain->pn = mpdu->pn;
    }

    {
        struct mmpktview *chain_view = mmpkt_open(frag_chain->buf);
        const uint32_t len = mmpkt_get_data_length(*rxbufview);
        const bool fits = mmpkt_available_space_at_end(chain_view) >= len;
        if (fits)
        {
            mmpkt_append_data(chain_view, mmpkt_get_data_start(*rxbufview), len);
        }
        mmpkt_close(&chain_view);
        if (!fits)
        {
            MMLOG_WRN("Fragment buffer space exceeded.\n");
            datapath_defrag_release(frag_chain);
            drop = &g_warthog_defrag_oversize;
            goto exit;
        }
    }

    if (dot11_frame_control_get_more_fragments(header->frame_control))
    {
        goto exit;
    }

    /* The MSDU is whole: hand it on under its first fragment's header. */
    return_buffer = frag_chain->buf;
    frag_chain->buf = NULL;
    datapath_defrag_release(frag_chain);
    return_view = mmpkt_open(return_buffer);
    *data_hdr = (const struct dot11_data_hdr *)mmpkt_get_data_start(return_view);
    (void)mmpkt_remove_from_start(return_view, dot11_data_hdr_get_len(*data_hdr));
    g_warthog_defrag_ok++;

exit:
    if (drop != NULL)
    {
        (*drop)++;
        g_warthog_rxdrop_reason = 97;
        g_warthog_rxdrop_count++;
    }
    datapath_defrag_settle(umacd);
    mmpkt_close(rxbufview);
    *rxbufview = return_view;
    mmpkt_release(rxbuf);
    return return_buffer;
}

void datapath_defrag_deinit(struct umac_data *umacd, struct datapath_defrag_data *data)
{
    for (unsigned i = 0; i < DEFRAG_CHAINS_MAX; i++)
    {
        struct datapath_defrag_chain *frag_chain = &s_chains[i];
        if (frag_chain->buf != NULL && frag_chain->owner == data)
        {
            datapath_defrag_release(frag_chain);
            g_warthog_defrag_flush++;
        }
    }
    datapath_defrag_settle(umacd);
}
