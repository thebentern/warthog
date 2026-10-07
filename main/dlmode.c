/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "freertos/FreeRTOS.h"

#include "dlmode.h"

#include "boot_guard.h"

#include "esp_private/esp_clk.h"
#include "esp_rom_sys.h"
#include "hal/wdt_hal.h"
#include "soc/rtc.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "tusb.h"
#include "warthog_shim.h"

_Static_assert(WARTHOG_DLMODE_BACK_S >= 60u && WARTHOG_DLMODE_BACK_S <= 28000u, "the RTC watchdog stage holds 32 bits of ticks");

void warthog_enter_download(void)
{
    /* A running MM6108 keeps ACKing its peers' polls without the host; reset, it loses its firmware. */
    warthog_chip_hold_reset();
    esp_rom_delay_us(10000);
    /* An RTC reset clears FORCE_DOWNLOAD_BOOT: a board no reset takes out of download mode boots the app. */
    warthog_boot_mark_download();
    wdt_hal_context_t ctx;
    wdt_hal_init(&ctx, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&ctx);
    /* Ticks from the boot's slow-clock calibration: the nominal RC frequency runs about 14% fast. */
    const uint32_t ticks =
        (uint32_t)((((uint64_t)WARTHOG_DLMODE_BACK_S * 1000000u) << RTC_CLK_CAL_FRACT) / esp_clk_slowclk_cal_get());
    wdt_hal_config_stage(&ctx, WDT_STAGE0, ticks, WDT_STAGE_ACTION_RESET_RTC);
    wdt_hal_enable(&ctx);
    wdt_hal_write_protect_enable(&ctx);
    /* The ROM's own reset into the app leaves USB and IO_MUX exempt from core resets; then the ROM never
     * enumerates. Undo that, and drop D+ so the host sees the board leave. */
    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_USB_RESET_DISABLE | RTC_CNTL_IO_MUX_RESET_DISABLE);
    (void)tud_disconnect();
    esp_rom_delay_us(100000);
    /* A direct core reset: IDF's restart path can hang, and the watchdog reset that then ends it clears the flag. */
    portDISABLE_INTERRUPTS();
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    REG_WRITE(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_SYS_RST);
    while (1) {
    }
}
