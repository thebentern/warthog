/*
 * Copyright 2026 Morse Micro
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 */



#pragma once

#include <stdint.h>

#include "mmwlan.h"
#include "umac/data/umac_data.h"


enum umac_health_check_veto_id
{
    UMAC_HEALTH_CHECK_VETO_ID_STANDBY = 0,
    UMAC_HEALTH_CHECK_VETO_ID_WNM_SLEEP = 1,
};

#if !(defined(DISABLE_MMWLAN_HEALTH_CHECK) && DISABLE_MMWLAN_HEALTH_CHECK)


void umac_health_check_init(struct umac_data *umacd);


void umac_health_check_deinit(struct umac_data *umacd);


void umac_health_check_start(struct umac_data *umacd);


void umac_health_check_stop(struct umac_data *umacd);


void umac_health_check_schedule_timeout(struct umac_data *umacd, uint32_t delta_ms);


enum mmwlan_status umac_health_check_set_interval(struct umac_data *umacd,
                                                  uint32_t min_interval_ms,
                                                  uint32_t max_interval_ms);


void umac_health_check_set_veto(struct umac_data *umacd, enum umac_health_check_veto_id veto_id);


void umac_health_check_unset_veto(struct umac_data *umacd, enum umac_health_check_veto_id veto_id);


void umac_health_check_note_activity(struct umac_data *umacd);


void umac_health_check_demand_check(struct umac_data *umacd);

/* warthog AT+CHIPRESTART: fails the next check, demanded now; false while the check cannot run. */
bool umac_health_check_force_fail(struct umac_data *umacd);

/* warthog hang guard: true while the check runs periodically (started, interval set, no veto);
 * @p wakes counts its runs, at least one an interval. Read from another task. */
bool umac_health_check_hang_view(struct umac_data *umacd, uint32_t *wakes, uint32_t *interval_ms);

#else

#include "mmutils.h"

static inline void umac_health_check_init(struct umac_data *umacd)
{
    MM_UNUSED(umacd);
}

static inline void umac_health_check_deinit(struct umac_data *umacd)
{
    MM_UNUSED(umacd);
}

static inline void umac_health_check_start(struct umac_data *umacd)
{
    MM_UNUSED(umacd);
}

static inline void umac_health_check_stop(struct umac_data *umacd)
{
    MM_UNUSED(umacd);
}

static inline void umac_health_check_schedule_timeout(struct umac_data *umacd, uint32_t delta_ms)
{
    MM_UNUSED(umacd);
    MM_UNUSED(delta_ms);
}

static inline enum mmwlan_status umac_health_check_set_interval(struct umac_data *umacd,
                                                                uint32_t min_interval_ms,
                                                                uint32_t max_interval_ms)
{
    MM_UNUSED(umacd);
    MM_UNUSED(min_interval_ms);
    MM_UNUSED(max_interval_ms);
    return MMWLAN_NOT_SUPPORTED;
}

static inline void umac_health_check_set_veto(struct umac_data *umacd,
                                              enum umac_health_check_veto_id veto_id)
{
    MM_UNUSED(umacd);
    MM_UNUSED(veto_id);
}

static inline void umac_health_check_unset_veto(struct umac_data *umacd,
                                                enum umac_health_check_veto_id veto_id)
{
    MM_UNUSED(umacd);
    MM_UNUSED(veto_id);
}

static inline void umac_health_check_note_activity(struct umac_data *umacd)
{
    MM_UNUSED(umacd);
}

static inline void umac_health_check_demand_check(struct umac_data *umacd)
{
    MM_UNUSED(umacd);
}

static inline bool umac_health_check_force_fail(struct umac_data *umacd)
{
    MM_UNUSED(umacd);
    return false;
}

static inline bool umac_health_check_hang_view(struct umac_data *umacd,
                                               uint32_t *wakes,
                                               uint32_t *interval_ms)
{
    MM_UNUSED(umacd);
    MM_UNUSED(wakes);
    MM_UNUSED(interval_ms);
    return false;
}

#endif


