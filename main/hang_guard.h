/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hang_guard_core.h"

/** AT+HANGTEST: the task that blocks at its next probe (enum warthog_hang_test); storage in at.c. */
extern volatile uint32_t g_warthog_hang_block;
/** The umac event loop's answers to the guard's pings (umac_mmdrv_shim.c); storage in at.c. */
extern volatile uint32_t g_warthog_loop_pongs;

/** First in app_main: takes the last boot's record and clears it; a boot ending before its first tick leaves none. */
void warthog_hang_guard_early(void);
/** From app_main once USB has started or failed: subscribes `main` to the task watchdog, arms the probes. */
void warthog_hang_guard_start(bool usb);
/** From app_main's loop every WARTHOG_HANG_TICK_MS: posts the probes; aborts on one past its limit. */
void warthog_hang_guard_tick(void);
/** AT+HANG?: the guard as of its last tick, and the previous boot's last tick. */
void warthog_hang_guard_view(struct warthog_hang_view *v);
