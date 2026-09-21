/*
 * L2 bridge between the tethered surfaces and the mesh.
 *
 * NAT mode (the default) gives every warthog the same 192.168.4.1 /
 * 192.168.5.1, which is why CoT and mDNS cannot cross it: the addresses they
 * carry alias the receiver's own subnet. Bridge mode puts USB, the Wi-Fi AP
 * and the mesh netif on one lwIP bridge; a host's frames leave the mesh with
 * the host's MAC in Address Extension and come back the same way, and its
 * address comes from whatever DHCP server the mesh has (an OpenMANET node's
 * dnsmasq), so two hosts on opposite sides are distinct.
 *
 * Needs CONFIG_ESP_NETIF_BRIDGE_EN. Without it the node says so and stays in
 * NAT mode rather than pretending.
 */
#include "mesh_bridge.h"

#include "esp_log.h"
#include "sdkconfig.h"
#if CONFIG_ESP_NETIF_BRIDGE_EN
#include "esp_netif_br_glue.h"
#endif

static const char *TAG = "warthog.bridge";
static esp_netif_t *s_br;

bool warthog_mesh_bridge_active(void) { return s_br != NULL; }
esp_netif_t *warthog_mesh_bridge_netif(void) { return s_br; }

esp_err_t warthog_mesh_bridge_start(esp_netif_t *mesh_netif, const uint8_t mac[6])
{
#if CONFIG_ESP_NETIF_BRIDGE_EN
    if (s_br != NULL) {
        return ESP_OK;
    }
    if (mesh_netif == NULL || mac == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_netif_t *usb = esp_netif_get_handle_from_ifkey("USB");
    esp_netif_t *ap  = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

    /* Ports carry no layer 3 of their own; addresses come through the bridge. */
    if (usb != NULL) { (void)esp_netif_dhcps_stop(usb); }
    if (ap  != NULL) { (void)esp_netif_dhcps_stop(ap); }

    bridgeif_config_t bcfg = { .max_fdb_dyn_entries = 64, .max_fdb_sta_entries = 8, .max_ports = 3 };
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_BR();
    base.if_key = "BR";
    base.if_desc = "warthog_br";
    base.bridge_info = &bcfg;
    base.route_prio = 60;
    esp_netif_config_t cfg = { .base = &base, .driver = NULL, .stack = ESP_NETIF_NETSTACK_DEFAULT_BR };
    s_br = esp_netif_new(&cfg);
    if (s_br == NULL) {
        ESP_LOGE(TAG, "bridge netif failed");
        return ESP_FAIL;
    }
    /* The bridge's own MAC is the mesh MAC, so the warthog's own IP traffic
     * leaves with SA == mesh SA and needs no Address Extension. Must precede
     * attach: the lwIP bridge takes its address at init. */
    (void)esp_netif_set_mac(s_br, mac);

    esp_netif_br_glue_handle_t glue = esp_netif_br_glue_new();
    if (glue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (usb != NULL) { (void)esp_netif_br_glue_add_port(glue, usb); }
    if (ap  != NULL) { (void)esp_netif_br_glue_add_wifi_port(glue, ap); }
    (void)esp_netif_br_glue_add_port(glue, mesh_netif);
    esp_err_t e = esp_netif_attach(s_br, glue);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "bridge attach failed: %s", esp_err_to_name(e));
        s_br = NULL;
        return e;
    }
    ESP_LOGW(TAG, "L2 bridge up: ports usb=%s ap=%s mesh=yes -- NAT and the multicast repeater are off",
             usb ? "yes" : "no", ap ? "yes" : "no");
    return ESP_OK;
#else
    (void)mesh_netif; (void)mac;
    ESP_LOGE(TAG, "AT+MESHBRIDGE=1 but this image lacks CONFIG_ESP_NETIF_BRIDGE_EN -- staying in NAT mode");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
