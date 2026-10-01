/*
 * Copyright 2022-2025 Morse Micro
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 */

#pragma once

#include "mmpkt_list.h"
#include "mmpkt.h"
#include "mmutils.h"
#include "mmwlan.h"
#include "mmwlan_internal.h"
#include "umac_datapath.h"
#include "dot11/dot11.h"


/* warthog: a peer's fragment chains sit in datapath_defrag.c's node-wide table, found by this
 * member's address; nothing is stored in it. */
struct datapath_defrag_data
{
    uint8_t anchor;
};


struct datapath_txq_data
{
    struct mmpkt_list queue;
};


struct umac_datapath_ops;


struct umac_datapath_data
{

    mmwlan_rx_pkt_cb_t rx_pkt_callback;

    mmwlan_rx_cb_t rx_callback;

    void *rx_arg;

    bool filter_beacons;

    struct mmosal_semb *tx_flowcontrol_sem;

    struct mmpkt_list tx_status_q;

    struct mmpkt_list rxq;

    struct mmpkt_list rx_mgmt_q;

    volatile uint16_t tx_paused;

    mmwlan_tx_flow_control_cb_t tx_flow_control_callback;

    void *tx_flow_control_arg;

    mmwlan_rx_frame_cb_t rx_frame_cb;

    void *rx_frame_cb_arg;

    uint32_t rx_frame_filter;

    const struct umac_datapath_ops *ops;
};


struct umac_datapath_sta_data
{

    uint16_t rx_seq_num_spaces[MMDRV_SEQ_NUM_SPACES];

    uint16_t tx_seq_num_spaces[MMDRV_SEQ_NUM_SPACES];

    struct datapath_defrag_data defrag_data;

    struct mmpkt_list rx_reorder_list;

    uint16_t rx_reorder_tid;
};
