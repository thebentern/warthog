#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "at.h"
#include "cfg.h"
#include "halow.h"
#include "led.h"
#include "nat.h"
#include "region.h"
#include "usb_net.h"
#include "wifi_ap.h"

static const char *TAG = "warthog";

void app_main(void)
{
    /* Last reset reason — names the cause when the panic handler can't flush
     * a backtrace (e.g. USB de-enumerates on reset). BROWNOUT/POWERON point at
     * power; PANIC/INT_WDT at software. */
    esp_reset_reason_t rr = esp_reset_reason();
    const char *rr_str =
        rr == ESP_RST_POWERON  ? "POWERON"  :
        rr == ESP_RST_BROWNOUT ? "BROWNOUT" :
        rr == ESP_RST_PANIC    ? "PANIC"    :
        rr == ESP_RST_SW       ? "SW"       :
        rr == ESP_RST_TASK_WDT ? "TASK_WDT" :
        rr == ESP_RST_INT_WDT  ? "INT_WDT"  :
        rr == ESP_RST_WDT      ? "WDT"      :
        rr == ESP_RST_DEEPSLEEP ? "DEEPSLEEP" : "OTHER";
    ESP_LOGW(TAG, "last reset reason: %s (%d)", rr_str, (int)rr);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "Warthog %s booting (region=%s, country=%s)", WARTHOG_VERSION,
             WARTHOG_REGION_NAME, WARTHOG_COUNTRY_CODE);

    ESP_ERROR_CHECK(warthog_cfg_init());
    ESP_ERROR_CHECK(warthog_led_start());
    ESP_ERROR_CHECK(warthog_halow_start());

    /* Sequence USB-OTG bring-up AFTER HaLow association. The USB-OTG
     * enumeration inrush stacked on the SAE handshake TX burst browns out
     * the XIAO's 3V3 LDO -> POWERON reset loop (observed on both a Mac port
     * and a bus-powered hub). Waiting for the HaLow link de-stacks the two
     * transients. Times out so a HaLow failure still yields a USB console. */
    esp_err_t link = warthog_halow_wait_link(20000);
    ESP_LOGI(TAG, "HaLow link %s — bringing up USB",
             link == ESP_OK ? "up" : "not up (timeout)");
    (void)warthog_usb_net_start();

    /* Let the USB-OTG inrush settle before the Wi-Fi AP radio powers on. */
    vTaskDelay(pdMS_TO_TICKS(750));
    (void)warthog_wifi_ap_start();

    ESP_ERROR_CHECK(warthog_nat_start());
    (void)warthog_at_start();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
