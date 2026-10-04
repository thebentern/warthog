/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_system.h"

/** Consecutive crash boots (after a panic or watchdog reset) that start safe mode: no HaLow start. */
#define WARTHOG_BOOT_SAFE_AFTER 3u
/** Uptime after which a normal boot with the boot watchdog off clears the crash-boot count. */
#define WARTHOG_BOOT_OK_S 60u
/** The RTC watchdog's limit from app_main until USB has started; it resets the system. */
#define WARTHOG_BOOT_WDT_S 60u
/** Failed USB starts in a row the boot watchdog retries by a reset (not crash boots); then no USB. */
#define WARTHOG_BOOT_USB_RETRIES 2u

/** The crash-boot count after a reset for @p rr, from the one kept in .noinit (@p valid: its magic held). */
uint32_t warthog_boot_next_count(uint32_t kept, bool valid, esp_reset_reason_t rr);

/** app_main's first call: counts this boot and arms the boot watchdog. @returns true for safe mode. */
bool warthog_boot_guard_start(void);

/** Right after USB has started, or in safe mode: the boot watchdog off. */
void warthog_boot_guard_usb_up(void);

/** A normal boot whose USB start failed. @returns true if the boot watchdog resets it to retry; false
 *  once WARTHOG_BOOT_USB_RETRIES are spent: the watchdog off, the board runs without USB. */
bool warthog_boot_guard_usb_failed(void);

/** From app_main's loop: clears the count once a normal boot, the watchdog off, has been up WARTHOG_BOOT_OK_S. */
void warthog_boot_guard_tick(void);

uint32_t warthog_boot_crash_count(void);
bool warthog_boot_safe(void);

/** AT+ASSERTTEST=hang: the next crash boots stop before the HaLow start, until safe mode. */
void warthog_boot_arm_hang(void);
bool warthog_boot_hang_armed(void);
