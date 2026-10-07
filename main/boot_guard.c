/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "boot_guard.h"

#include "esp_attr.h"
#include "esp_timer.h"
#include "hal/wdt_hal.h"
#include "soc/rtc.h"

#define BOOT_GUARD_MAGIC 0xb007ca5eu

/* Kept across a panic or watchdog reset, as the shim's assert records are; power loss loses it. */
static __NOINIT_ATTR struct {
    uint32_t magic;
    uint32_t crash_boots;
    uint32_t hang;
    uint32_t usb_fail; /* BOOT_GUARD_MAGIC: this boot's USB start failed, the watchdog left to retry */
    uint32_t usb_retries;
    uint32_t dl; /* BOOT_GUARD_MAGIC: download mode entered; its RTC watchdog's return is no crash */
} s_boot;
static bool s_safe;
static bool s_cleared;
static bool s_wdt_off;
static bool s_dl_back;

uint32_t warthog_boot_next_count(uint32_t kept, bool valid, esp_reset_reason_t rr)
{
    if (rr != ESP_RST_PANIC && rr != ESP_RST_INT_WDT && rr != ESP_RST_TASK_WDT && rr != ESP_RST_WDT) {
        return 0;
    }
    if (!valid) {
        kept = 0;
    }
    return kept < 255u ? kept + 1u : kept;
}

/* A system reset on expiry keeps .noinit and the reset reason; wdt_hal_init drops the bootloader's stage. */
static void boot_wdt_arm_(uint32_t s)
{
    wdt_hal_context_t ctx;
    wdt_hal_init(&ctx, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&ctx);
    wdt_hal_config_stage(&ctx, WDT_STAGE0, (uint32_t)((uint64_t)s * rtc_clk_slow_freq_get_hz()),
                         WDT_STAGE_ACTION_RESET_SYSTEM);
    wdt_hal_enable(&ctx);
    wdt_hal_write_protect_enable(&ctx);
}

bool warthog_boot_guard_start(void)
{
    const bool valid = s_boot.magic == BOOT_GUARD_MAGIC;
    const esp_reset_reason_t rr = esp_reset_reason();
    s_dl_back = valid && rr == ESP_RST_WDT && s_boot.dl == BOOT_GUARD_MAGIC;
    /* The watchdog retrying a failed USB start, or ending a download mode nobody used, is no crash. */
    if (!s_dl_back && !(valid && rr == ESP_RST_WDT && s_boot.usb_fail == BOOT_GUARD_MAGIC)) {
        s_boot.crash_boots = warthog_boot_next_count(s_boot.crash_boots, valid, rr);
    }
    if (!valid || warthog_boot_next_count(0, true, rr) == 0 || s_dl_back) {
        s_boot.usb_retries = 0; /* a clean reset or lost magic: the USB retries back */
    }
    s_boot.usb_fail = 0;
    s_boot.dl = 0;
    s_safe = s_boot.crash_boots >= WARTHOG_BOOT_SAFE_AFTER;
    if (!valid || s_boot.crash_boots == 0 || s_safe) {
        s_boot.hang = 0;
    }
    s_boot.magic = BOOT_GUARD_MAGIC;
    boot_wdt_arm_(WARTHOG_BOOT_WDT_S);
    return s_safe;
}

static void boot_wdt_off_(void)
{
    wdt_hal_context_t ctx = RWDT_HAL_CONTEXT_DEFAULT();
    wdt_hal_write_protect_disable(&ctx);
    wdt_hal_disable(&ctx);
    wdt_hal_write_protect_enable(&ctx);
    s_wdt_off = true;
}

void warthog_boot_guard_usb_up(void)
{
    boot_wdt_off_();
    s_boot.usb_retries = 0;
}

bool warthog_boot_guard_usb_failed(void)
{
    if (s_boot.usb_retries >= WARTHOG_BOOT_USB_RETRIES) {
        boot_wdt_off_();
        return false;
    }
    s_boot.usb_retries++;
    s_boot.usb_fail = BOOT_GUARD_MAGIC;
    return true;
}

/* Only with the watchdog off: a clear just before its reset would make every retry crash boot 1. */
void warthog_boot_guard_tick(void)
{
    if (!s_safe && s_wdt_off && !s_cleared && esp_timer_get_time() >= (int64_t)WARTHOG_BOOT_OK_S * 1000000) {
        s_boot.crash_boots = 0;
        s_cleared = true;
    }
}

uint32_t warthog_boot_crash_count(void)
{
    return s_boot.crash_boots;
}

bool warthog_boot_safe(void)
{
    return s_safe;
}

bool warthog_boot_download_return(void)
{
    return s_dl_back;
}

void warthog_boot_mark_download(void)
{
    s_boot.magic = BOOT_GUARD_MAGIC;
    s_boot.dl = BOOT_GUARD_MAGIC;
}

void warthog_boot_arm_hang(void)
{
    s_boot.hang = BOOT_GUARD_MAGIC;
}

bool warthog_boot_hang_armed(void)
{
    return s_boot.hang == BOOT_GUARD_MAGIC;
}
