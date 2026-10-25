/*
 * Copyright 2022 Morse Micro
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 */

#include "umac_regdb.h"
#include "umac/config/umac_config.h"

const char *umac_regdb_get_country_code(struct umac_data *umacd)
{
    const struct mmwlan_s1g_channel_list *channel_list = umac_config_get_channel_list(umacd);

    if (channel_list == NULL)
    {
        return "??";
    }
    return (const char *)channel_list->country_code;
}

bool umac_regdb_op_class_match(struct umac_data *umacd,
                               uint8_t op_class,
                               const struct mmwlan_s1g_channel *chan)
{
    if (!umac_config_is_opclass_check_enabled(umacd) ||
        (chan->global_operating_class == MMWLAN_SKIP_OP_CLASS_CHECK) ||
        (chan->s1g_operating_class == MMWLAN_SKIP_OP_CLASS_CHECK))
    {
        return true;
    }

    if ((op_class == (uint8_t)chan->global_operating_class) ||
        (op_class == (uint8_t)chan->s1g_operating_class))
    {
        return true;
    }

    return false;
}

const struct mmwlan_s1g_channel *umac_regdb_get_channel(struct umac_data *umacd,
                                                        uint8_t s1g_chan_num)
{
    uint32_t ii;
    const struct mmwlan_s1g_channel_list *channel_list = umac_config_get_channel_list(umacd);

    if (channel_list == NULL)
    {
        MMLOG_WRN("NULL regdom\n");
        return NULL;
    }

    for (ii = 0; ii < channel_list->num_channels; ii++)
    {
        const struct mmwlan_s1g_channel *chan = &(channel_list->channels[ii]);
        if (chan->s1g_chan_num == s1g_chan_num)
        {
            return chan;
        }
    }

    return NULL;
}

const struct mmwlan_s1g_channel *umac_regdb_get_channel_from_freq_and_bw(struct umac_data *umacd,
                                                                         uint32_t centre_freq_hz,
                                                                         uint8_t bw_mask)
{
    const struct mmwlan_s1g_channel_list *channel_list = umac_config_get_channel_list(umacd);
    const struct mmwlan_s1g_channel *match = NULL;

    if (channel_list == NULL)
    {
        MMLOG_WRN("NULL regdom\n");
        return NULL;
    }

    for (uint32_t ii = 0; ii < channel_list->num_channels; ii++)
    {
        const struct mmwlan_s1g_channel *chan = &channel_list->channels[ii];
        MMOSAL_DEV_ASSERT(chan->bw_mhz != 0 && (chan->bw_mhz & (chan->bw_mhz - 1)) == 0);

        if (chan->centre_freq_hz != centre_freq_hz || (bw_mask & chan->bw_mhz) == 0)
        {
            continue;
        }

        if (match != NULL)
        {

            return NULL;
        }
        match = chan;
    }

    return match;
}

bool umac_regdb_strictest(const struct mmwlan_s1g_channel_list *list, struct mmwlan_s1g_channel *out)
{
    if (list == NULL || list->num_channels == 0)
    {
        return false;
    }
    struct mmwlan_s1g_channel r = { .duty_cycle_sta = 10000, .duty_cycle_omit_ctrl_resp = true };
    bool restricted = false;
    for (uint32_t ii = 0; ii < list->num_channels; ii++)
    {
        const struct mmwlan_s1g_channel *c = &list->channels[ii];
        restricted |= c->duty_cycle_sta < 10000 || c->pkt_spacing_us || c->airtime_min_us || c->airtime_max_us;
        r.duty_cycle_sta = c->duty_cycle_sta < r.duty_cycle_sta ? c->duty_cycle_sta : r.duty_cycle_sta;
        /* Omitting control responses from the duty count loosens it: only when every row does. */
        r.duty_cycle_omit_ctrl_resp = r.duty_cycle_omit_ctrl_resp && c->duty_cycle_omit_ctrl_resp;
        r.pkt_spacing_us = c->pkt_spacing_us > r.pkt_spacing_us ? c->pkt_spacing_us : r.pkt_spacing_us;
        r.airtime_min_us = (ii == 0 || c->airtime_min_us < r.airtime_min_us) ? c->airtime_min_us : r.airtime_min_us;
        if (c->airtime_max_us != 0 && (r.airtime_max_us == 0 || c->airtime_max_us < r.airtime_max_us))
        {
            r.airtime_max_us = c->airtime_max_us;
        }
    }
    if (restricted)
    {
        *out = r;
    }
    return restricted;
}
