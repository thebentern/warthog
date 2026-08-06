#include "nat.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/lwip_napt.h"
#include "lwip/netif.h"

static const char *TAG = "warthog.nat";

static bool s_napt_logged = false;
/* Track HaLow STA IP so we can detect re-association with a new IP — when that
 * happens, every existing NAPT table entry is keyed on the old upstream IP and
 * silently stops translating new traffic correctly. Currently we only log it;
 * a future fix would flush the NAPT table on change. */
static uint32_t s_last_halow_ip = 0;

static esp_netif_t *get_netif(const char *key)
{
    return esp_netif_get_handle_from_ifkey(key);
}

/* ESP-IDF's lwIP NAPT is *inverted* from the conventional "WAN-facing NAT"
 * model: ip_napt_forward() in ip4_napt.c keys on inp->napt (the *input*
 * netif) and rewrites source to outp->ip_addr. So NAPT goes on the inside
 * netifs (USB ECM, Wi-Fi AP), NOT on the outside (HaLow STA). We bypass
 * esp_netif_napt_enable()'s single-netif exclusivity check via the lwIP
 * direct API so both downstream surfaces get NAT. Full diagnosis +
 * tcpdump evidence in docs/napt-notes.md. */

static void enforce_state(void)
{
    esp_netif_t *halow = get_netif("WIFI_STA_DEF");
    if (!halow) {
        return;
    }
    esp_netif_ip_info_t halow_ip = {0};
    esp_netif_get_ip_info(halow, &halow_ip);
    if (halow_ip.ip.addr == 0) {
        if (s_napt_logged) {
            ESP_LOGW(TAG, "HaLow lease lost");
            s_napt_logged = false;
            s_last_halow_ip = 0;
        }
        return;
    }

    /* HaLow IP changed under us — typically a STA re-association after a
     * brownout / beacon-miss / upstream lease cycle. Existing NAPT entries
     * still reference the old IP and won't translate new packets correctly;
     * iPads/iPhones interpret this as "no internet" inside ~2 min because
     * their iOS captive-network re-probe to apple.com fails. Log it loudly
     * so we can correlate with downstream client failures. */
    if (s_last_halow_ip != 0 && s_last_halow_ip != halow_ip.ip.addr) {
        esp_ip4_addr_t prev = { .addr = s_last_halow_ip };
        ESP_LOGW(TAG, "HaLow STA IP changed: " IPSTR " -> " IPSTR
                      " (NAPT table now stale; downstream clients will see drops)",
                 IP2STR(&prev), IP2STR(&halow_ip.ip));
    }
    s_last_halow_ip = halow_ip.ip.addr;

    esp_netif_set_default_netif(halow);

    struct netif *usb_lwip = NULL;
    struct netif *ap_lwip = NULL;
    esp_netif_t *usb = get_netif("USB");
    esp_netif_t *ap = get_netif("WIFI_AP_DEF");
    if (usb) {
        usb_lwip = (struct netif *)esp_netif_get_netif_impl(usb);
    }
    if (ap) {
        ap_lwip = (struct netif *)esp_netif_get_netif_impl(ap);
    }
    if (usb_lwip) {
        ip_napt_enable_netif(usb_lwip, 1);
    }
    if (ap_lwip) {
        ip_napt_enable_netif(ap_lwip, 1);
    }

    if (!s_napt_logged) {
        ESP_LOGI(TAG, "NAPT on inside netifs (usb=%d ap=%d); HaLow default route: ip=" IPSTR " gw=" IPSTR,
                 usb_lwip ? usb_lwip->napt : -1,
                 ap_lwip ? ap_lwip->napt : -1,
                 IP2STR(&halow_ip.ip), IP2STR(&halow_ip.gw));
        s_napt_logged = true;
    }
}

static void nat_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "NAPT supervisor up");
    uint32_t tick = 0;
    while (1) {
        enforce_state();
#if WARTHOG_DEBUG
        /* Periodic state line for debugging — enable with -DWARTHOG_DEBUG=1. */
        if (tick % 15 == 0) {
            esp_netif_t *halow = get_netif("WIFI_STA_DEF");
            esp_netif_t *usb = get_netif("USB");
            esp_netif_t *ap = get_netif("WIFI_AP_DEF");
            struct netif *usb_lwip = usb ? (struct netif *)esp_netif_get_netif_impl(usb) : NULL;
            struct netif *ap_lwip = ap ? (struct netif *)esp_netif_get_netif_impl(ap) : NULL;
            esp_netif_ip_info_t hip = {0};
            if (halow) {
                esp_netif_get_ip_info(halow, &hip);
            }
            esp_netif_t *cur_default = esp_netif_get_default_netif();
            const char *cur_key = cur_default ? esp_netif_get_ifkey(cur_default) : "(none)";
            if (!cur_key) {
                cur_key = "(?)";
            }
            ESP_LOGI(TAG, "tick: halow_ip=" IPSTR " usb.napt=%d ap.napt=%d default=%s",
                     IP2STR(&hip.ip),
                     usb_lwip ? usb_lwip->napt : -1,
                     ap_lwip ? ap_lwip->napt : -1,
                     cur_key);
        }
#endif
        tick++;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

esp_err_t warthog_nat_start(void)
{
    BaseType_t ok = xTaskCreate(nat_task, "warthog_nat", 3072, NULL,
                                tskIDLE_PRIORITY + 2, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
