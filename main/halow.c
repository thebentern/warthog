#include "halow.h"
#include "cfg.h"
#include "led.h"
#include "mesh.h"
#include "region.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mmhalow.h"

#include <string.h>

static const char *TAG = "warthog.halow";

/* Set once the HaLow STA associates (or gets a DHCP lease). app_main waits
 * on this so USB-OTG bring-up is sequenced after the SAE TX burst. */
#define HALOW_LINK_BIT BIT0
static EventGroupHandle_t s_halow_events;

/* Optional cap on HaLow max TX power (dBm). 0 = no override, use the
 * regulatory maximum from the BCF. Was a firmware mitigation for the MM6108
 * PA-inrush brownout before the hardware bulk cap landed; see
 * docs/power-notes.md. Set non-zero via -DHALOW_MAX_TX_DBM=N if a board
 * variant needs it. */
#ifndef HALOW_MAX_TX_DBM
#define HALOW_MAX_TX_DBM 0
#endif

static void on_sta_state(enum mmwlan_sta_state state)
{
    switch (state) {
    case MMWLAN_STA_DISABLED:
        ESP_LOGI(TAG, "STA disabled");
        break;
    case MMWLAN_STA_CONNECTING:
        ESP_LOGI(TAG, "STA connecting");
        break;
    case MMWLAN_STA_CONNECTED:
        ESP_LOGI(TAG, "STA connected (waiting for DHCP)");
        /* SAE handshake / association TX burst is done — release the
         * USB-OTG bring-up that app_main is holding. */
        if (s_halow_events) {
            xEventGroupSetBits(s_halow_events, HALOW_LINK_BIT);
        }
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base != IP_EVENT) {
        return;
    }
    switch (id) {
    case IP_EVENT_STA_GOT_IP: {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "DHCP lease: ip=" IPSTR " gw=" IPSTR " mask=" IPSTR,
                 IP2STR(&evt->ip_info.ip), IP2STR(&evt->ip_info.gw),
                 IP2STR(&evt->ip_info.netmask));
        int32_t rssi = mmwlan_get_rssi();
        if (rssi != INT32_MIN) {
            ESP_LOGI(TAG, "AP RSSI %ld dBm", (long)rssi);
        }
        warthog_led_set(WARTHOG_LED_HALOW_UP);
        /* Belt-and-suspenders: also release the USB-OTG hold here in case
         * the STA-state callback's CONNECTED edge was missed. */
        if (s_halow_events) {
            xEventGroupSetBits(s_halow_events, HALOW_LINK_BIT);
        }
        break;
    }
    case IP_EVENT_STA_LOST_IP:
        ESP_LOGW(TAG, "DHCP lease lost");
        warthog_led_set(WARTHOG_LED_HALOW_CONNECTING);
        break;
    default:
        break;
    }
}

esp_err_t warthog_halow_start(void)
{
    ESP_LOGI(TAG, "init region=%s country=%s",
             WARTHOG_REGION_NAME, WARTHOG_COUNTRY_CODE);

    /* Created before any handler that sets HALOW_LINK_BIT can run. */
    s_halow_events = xEventGroupCreate();
    if (!s_halow_events) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL),
        TAG, "register ip event handler");

    ESP_RETURN_ON_ERROR(mmhalow_init(NULL), TAG, "mmhalow init");
    mmhalow_print_version_info();

#if HALOW_MAX_TX_DBM > 0
    /* Shrink the MM6108 PA current spike — see HALOW_MAX_TX_DBM above.
     * Takes effect at the next channel switch (the association below). */
    enum mmwlan_status txp = mmwlan_override_max_tx_power(HALOW_MAX_TX_DBM);
    ESP_LOGW(TAG, "HaLow max TX power capped to %d dBm (status=%d)",
             HALOW_MAX_TX_DBM, (int)txp);
#endif

#ifdef WARTHOG_MESH_SMOKE
    /* Diagnostic build (warthog-mesh-smoke env): probe whether the chip
     * firmware accepts a mesh VIF, then stop — skip STA association so the
     * mesh attempt owns the single VIF. See docs/mesh-port-scope.md. */
    warthog_mesh_smoke_test();
    /* No STA association in this build — don't let app_main stall waiting. */
    xEventGroupSetBits(s_halow_events, HALOW_LINK_BIT);
    return ESP_OK;
#endif

    char ssid[33] = {0};
    char psk[65] = {0};
    warthog_cfg_get_halow_ssid(ssid, sizeof(ssid));
    warthog_cfg_get_halow_psk(psk, sizeof(psk));

    if (ssid[0] == '\0') {
        ESP_LOGW(TAG, "HaLow SSID empty — chip idle. Set via AT+HALOW=... or -DHALOW_SSID at build time.");
        /* Nothing to associate — don't let app_main stall waiting. */
        xEventGroupSetBits(s_halow_events, HALOW_LINK_BIT);
        return ESP_OK;
    }

    mmhalow_wifi_config_t conf = { .sta = MMWLAN_STA_ARGS_INIT };

    size_t ssid_len = strnlen(ssid, sizeof(conf.sta.ssid));
    memcpy(conf.sta.ssid, ssid, ssid_len);
    conf.sta.ssid_len = ssid_len;

    size_t psk_len = strnlen(psk, sizeof(conf.sta.passphrase));
    memcpy(conf.sta.passphrase, psk, psk_len);
    conf.sta.passphrase_len = psk_len;

    /* HaLow is SAE-only in practice; open is the unencrypted fallback. */
    conf.sta.security_type = (psk_len > 0) ? MMWLAN_SAE : MMWLAN_OPEN;

    ESP_RETURN_ON_ERROR(mmhalow_set_config(WIFI_IF_STA, &conf), TAG, "set_config");

    ESP_LOGI(TAG, "associating with SSID '%s' (%s)", ssid,
             (conf.sta.security_type == MMWLAN_SAE) ? "SAE" : "open");
    ESP_RETURN_ON_ERROR(mmhalow_connect(on_sta_state), TAG, "mmhalow_connect");
    warthog_led_set(WARTHOG_LED_HALOW_CONNECTING);
    return ESP_OK;
}

esp_err_t warthog_halow_wait_link(uint32_t timeout_ms)
{
    if (!s_halow_events) {
        /* warthog_halow_start() never ran (or failed before creating the
         * group) — nothing to wait on. */
        return ESP_ERR_INVALID_STATE;
    }
    EventBits_t bits = xEventGroupWaitBits(s_halow_events, HALOW_LINK_BIT,
                                           pdFALSE /* don't clear */,
                                           pdTRUE /* all (only) bits */,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & HALOW_LINK_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}
