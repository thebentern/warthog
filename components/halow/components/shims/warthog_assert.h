/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include "mmosal.h"

/** Records the shim keeps in .noinit (a soft reset keeps them, power loss does not). */
#define WARTHOG_ASSERT_RECORDS_MAX 4u

/** AT+ASSERT?: copies the kept MMOSAL_ASSERT records into @p out, oldest first, and sets @p kept;
 *  @returns how many were logged since they were last cleared (record i is number count-kept+i). */
uint32_t warthog_assert_records(struct mmosal_failure_info *out, uint32_t max, uint32_t *kept);

/** AT+ASSERT=0: forget every record. */
void warthog_assert_clear(void);
