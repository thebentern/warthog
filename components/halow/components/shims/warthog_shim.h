/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/** Holds the MM6108 in reset (RESET_N and WAKE low) with the SPI_IRQ and BUSY interrupts off;
 *  installs no ISR service. A panic resets the CPU only, so the chip stays as the last boot left it. */
void warthog_chip_hold_reset(void);

/** AT+STACKS?: least free stack (bytes) of the running task the shim started as @p name (@p live) and
 *  of any instance as it exited (@p exit_min), UINT32_MAX for none; false if none of that name ran. */
bool warthog_task_stack(const char *name, uint32_t *live, uint32_t *exit_min);
