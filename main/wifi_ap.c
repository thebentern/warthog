#include "wifi_ap.h"
#include "cfg.h"

#include "dhcpserver/dhcpserver.h"   /* OFFER_DNS + dhcps_offer_t */
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"

#include <string.h>

static const char *TAG = "warthog.wifi_ap";

#ifndef WARTHOG_AP_GW_IP
#define WARTHOG_AP_GW_IP "192.168.5.1"
#endif
#ifndef WARTHOG_AP_NETMASK
#define WARTHOG_AP_NETMASK "255.255.255.0"
#endif

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base != WIFI_EVENT) {
        return;
    }
    switch (id) {
    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "client+ aid=%d mac=%02x:%02x:%02x:%02x:%02x:%02x",
                 e->aid, e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI(TAG, "client- aid=%d mac=%02x:%02x:%02x:%02x:%02x:%02x",
                 e->aid, e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
        break;
    }
    default:
        break;
    }
}

esp_netif_t *warthog_wifi_ap_start(void)
{
    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    if (!netif) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_ap failed");
        return NULL;
    }

    /* Override the default 192.168.4.1/24 to avoid collision with the USB netif. */
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = esp_ip4addr_aton(WARTHOG_AP_GW_IP);
    ip.gw.addr = ip.ip.addr;
    ip.netmask.addr = esp_ip4addr_aton(WARTHOG_AP_NETMASK);
    /* DHCP option 6 — see usb_net.c's matching block. Same two-step (enable
     * the dhcps DNS-offer flag, then set the IP). dhcps_option setters must
     * run with the server stopped. */
    char dns_str[16] = {0};
    warthog_cfg_get_dns(dns_str, sizeof(dns_str));
    dhcps_offer_t offer_dns = OFFER_DNS;
    esp_netif_dns_info_t dns = {
        .ip = {
            .type = ESP_IPADDR_TYPE_V4,
            .u_addr.ip4.addr = esp_ip4addr_aton(dns_str),
        },
    };
    esp_netif_dhcps_stop(netif);
    esp_netif_set_ip_info(netif, &ip);
    esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET,
                           ESP_NETIF_DOMAIN_NAME_SERVER,
                           &offer_dns, sizeof(offer_dns));
    esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_start(netif);

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed");
        return NULL;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL);

    char ssid[33] = {0};
    char psk[65] = {0};
    warthog_cfg_get_ap_ssid(ssid, sizeof(ssid));
    warthog_cfg_get_ap_psk(psk, sizeof(psk));
    uint8_t channel = warthog_cfg_get_ap_channel();

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid_len = strlen(ssid),
            .channel = channel,
            .max_connection = 4,
            .authmode = (strlen(psk) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
            .pmf_cfg = { .required = false },
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid));
    strncpy((char *)ap_cfg.ap.password, psk, sizeof(ap_cfg.ap.password));

    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK
            || esp_wifi_set_config(WIFI_IF_AP, &ap_cfg) != ESP_OK
            || esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode/config/start failed");
        return NULL;
    }

    ESP_LOGI(TAG, "up: ssid='%s' ch=%d gw=%s mask=%s",
             ssid, channel, WARTHOG_AP_GW_IP, WARTHOG_AP_NETMASK);
    return netif;
}
