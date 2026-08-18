/*
 * Copyright 2022 Morse Micro
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 */

#include "umac/keys/umac_keys.h"

/* CCMP RX diagnostics (storage in main/at.c; AT+RXCHAN?). */
extern volatile uint32_t g_warthog_ccmp_last_keyid, g_warthog_ccmp_blank, g_warthog_ccmp_last_pn, g_warthog_ccmp_replay;
#include "dot11/dot11.h"


#define CCMP_HEADER_KEY_OCT_KEY_ID 0xC0


static uint8_t parse_ccmp_key_id(uint8_t *ccmp_header)
{
    return (ccmp_header[3] & CCMP_HEADER_KEY_OCT_KEY_ID) >> 6;
}


static uint64_t parse_ccmp_packet_number(const uint8_t *header)
{
    return (((uint64_t)(*(header)) << 0)) |
           (((uint64_t)(*(header + 1)) << 8)) |
           (((uint64_t)(*(header + 4)) << 16)) |
           (((uint64_t)(*(header + 5)) << 24)) |
           (((uint64_t)(*(header + 6)) << 32)) |
           (((uint64_t)(*(header + 7)) << 40));
}

bool ccmp_is_valid(struct umac_sta_data *stad,
                   uint8_t *ccmp_header,
                   enum umac_key_rx_counter_space space)
{
    if (ccmp_header == NULL || stad == NULL)
    {
        return false;
    }

    uint8_t key_id = parse_ccmp_key_id(ccmp_header);

    g_warthog_ccmp_last_keyid = key_id;
    if (umac_keys_get_key_type(stad, key_id) == UMAC_KEY_TYPE_BLANK)
    {
        g_warthog_ccmp_blank++;
        return false;
    }

    uint64_t packet_number = parse_ccmp_packet_number(ccmp_header);
    g_warthog_ccmp_last_pn = (uint32_t)packet_number;
    enum mmwlan_status status =
        umac_keys_check_and_update_rx_replay(stad, key_id, packet_number, space);
    if (status != MMWLAN_SUCCESS)
    {
        g_warthog_ccmp_replay++;
    }

    return (status == MMWLAN_SUCCESS);
}
