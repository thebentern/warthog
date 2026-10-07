#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "at.h"
#include "bat_port.h"
#include "boot_guard.h"
#include "cfg.h"
#include "halow.h"
#include "hang_guard.h"
#include "mudp.h"
#include "led.h"
#include "nat.h"
#include "region.h"
#include "mesh_bridge.h"
#include "mmhalow.h"
#include "mmwlan.h"
#include "usb_net.h"
#include "warthog_shim.h"
#include "wifi_ap.h"

static const char *TAG = "warthog";

void app_main(void)
{
    /* Before anything that can hang: the boot watchdog runs until USB is up. */
    const bool safe = warthog_boot_guard_start();
    warthog_hang_guard_early();

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
    if (safe) {
        /* Crash boots in a row: USB and AT come up, the HaLow chip held in reset (AT+ASSERT?). */
        ESP_LOGE(TAG, "safe mode: %lu crash boots in a row; HaLow not started",
                 (unsigned long)warthog_boot_crash_count());
        ESP_ERROR_CHECK(warthog_halow_start_safe());
    } else {
        if (warthog_boot_hang_armed()) {
            ESP_LOGE(TAG, "AT+ASSERTTEST=hang: stopping before the HaLow start");
            warthog_chip_hold_reset();
            for (;;) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
        ESP_ERROR_CHECK(warthog_halow_start());

        /* Sequence USB-OTG bring-up AFTER HaLow association. The USB-OTG
         * enumeration inrush stacked on the SAE handshake TX burst browns out
         * the XIAO's 3V3 LDO -> POWERON reset loop (observed on both a Mac port
         * and a bus-powered hub). Waiting for the HaLow link de-stacks the two
         * transients. Times out so a HaLow failure still yields a USB console. */
        esp_err_t link = warthog_halow_wait_link(20000);
        ESP_LOGI(TAG, "HaLow link %s — bringing up USB",
                 link == ESP_OK ? "up" : "not up (timeout)");
    }
    const bool usb = warthog_usb_net_start() != NULL;
    if (usb || safe) {
        /* No USB in a normal boot: the watchdog resets it to retry, twice; safe mode gains nothing by it. */
        warthog_boot_guard_usb_up();
    } else if (warthog_boot_guard_usb_failed()) {
        ESP_LOGE(TAG, "USB did not start: the boot watchdog resets the board to retry");
    } else {
        ESP_LOGE(TAG, "USB did not start after %u retries: running without USB", WARTHOG_BOOT_USB_RETRIES);
    }
    warthog_hang_guard_start(usb);

    /* Let the USB-OTG inrush settle before the Wi-Fi AP radio powers on. */
    vTaskDelay(pdMS_TO_TICKS(750));
    (void)warthog_wifi_ap_start();

    /* Bridge mode replaces NAT and the multicast repeater with one L2 segment.
     * Batman mode refuses bridge at the setter; this only guards a stale NVS pair. */
    if (!safe && warthog_cfg_get_mesh_bridge() && !warthog_bat_port_running()) {
        uint8_t mesh_mac[6] = { 0 };
        mmwlan_get_mac_addr(mesh_mac);
        esp_err_t be = warthog_mesh_bridge_start(mmhalow_get_netif(), mesh_mac);
        if (be != ESP_OK) {
            ESP_LOGE(TAG, "bridge mode requested but not started (%s); NAT mode", esp_err_to_name(be));
        }
    }
    ESP_ERROR_CHECK(warthog_nat_start());
    /* Multicast repeater: bridges 239.0.0.69:4403 (Meshtastic's UDP transport)
     * between the USB, Wi-Fi AP and HaLow-mesh netifs. lwIP does not forward
     * multicast between netifs, so without this a Meshtastic node on the AP
     * side never reaches the mesh. */
    (void)warthog_mudp_start();
    (void)warthog_at_start();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(WARTHOG_HANG_TICK_MS));
        warthog_boot_guard_tick();
        warthog_hang_guard_tick();
    }
}
