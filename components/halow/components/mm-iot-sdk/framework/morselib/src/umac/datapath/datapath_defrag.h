/*
 * Copyright 2022-2024 Morse Micro
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 */



#pragma once

#include "mmpkt.h"
#include "dot11/dot11_frames.h"
#include "umac/datapath/umac_datapath_data.h"


/* warthog: what the receive path established about one MPDU before reassembly. */
struct datapath_defrag_mpdu
{
    uint64_t pn;       /* CCMP packet number (protected only) */
    uint32_t key_gen;  /* colour of the key it was opened under (protected only) */
    uint16_t qos;      /* QoS Control, host order; 0 for non-QoS data */
    uint8_t key_id;    /* CCMP key id (protected only) */
    bool is_protected; /* opened and replay-checked under CCMP */
    bool mesh;         /* received on the mesh: QoS bit 8 is Mesh Control Present */
};

/* warthog: an MPDU that is part of a fragmented MSDU (More Fragments, or fragment number > 0). */
static inline bool datapath_defrag_is_fragment(const struct dot11_hdr *header)
{
    return dot11_sequence_control_get_fragment_number(header->sequence_control) != 0 ||
           dot11_frame_control_get_more_fragments(header->frame_control);
}

/* @p rxbufview is positioned at the MPDU's body (after any CCMP header, before its MIC). A
 * whole MSDU returns @p rxbuf unchanged. A fragment is consumed (@p rxbufview and @p rxbuf
 * released, NULL returned) unless it completes its MSDU: then the reassembly buffer is
 * returned, @p data_hdr pointing at the first fragment's header inside it and @p rxbufview
 * at the MSDU body. */
struct mmpkt *datapath_defrag(struct umac_data *umacd,
                              struct datapath_defrag_data *data,
                              const struct dot11_data_hdr **data_hdr,
                              struct mmpktview **rxbufview,
                              struct mmpkt *rxbuf,
                              uint8_t tid_idx,
                              const struct datapath_defrag_mpdu *mpdu);


/* Release @p data's chains (its peer left or rekeyed). */
void datapath_defrag_deinit(struct umac_data *umacd, struct datapath_defrag_data *data);

/* warthog: release every chain 1 s past its first fragment, and arm the expiry timeout if the core
 * could not take it before; cheap while no chain is held. Event loop only. */
void datapath_defrag_expire(struct umac_data *umacd);


