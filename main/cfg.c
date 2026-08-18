#include "cfg.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "warthog.cfg";
static const char *NS = "warthog";

#ifndef HALOW_SSID
#define HALOW_SSID ""
#endif
#ifndef HALOW_PASSPHRASE
#define HALOW_PASSPHRASE ""
#endif
#ifndef WARTHOG_AP_SSID
#define WARTHOG_AP_SSID "warthog"
#endif
#ifndef WARTHOG_AP_PSK
#define WARTHOG_AP_PSK "warthog-default"
#endif
#ifndef WARTHOG_AP_CHANNEL
#define WARTHOG_AP_CHANNEL 6
#endif
#ifndef WARTHOG_DOWNSTREAM_DNS
#define WARTHOG_DOWNSTREAM_DNS "1.1.1.1"
#endif

esp_err_t warthog_cfg_init(void)
{
    /* nvs_flash_init() is already called from app_main; this is a no-op
     * placeholder so callers can be explicit about cfg lifecycle. */
    return ESP_OK;
}

static esp_err_t get_string_or_default(const char *key, const char *fallback,
                                       char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t needed = out_len;
        err = nvs_get_str(h, key, out, &needed);
        nvs_close(h);
        if (err == ESP_OK) {
            return ESP_OK;
        }
    }
    /* NVS missing or namespace not yet created — use the build-time default. */
    size_t n = strlen(fallback);
    if (n >= out_len) {
        n = out_len - 1;
    }
    memcpy(out, fallback, n);
    out[n] = '\0';
    return ESP_OK;
}

esp_err_t warthog_cfg_get_halow_ssid(char *out, size_t out_len)
{
    return get_string_or_default("halow_ssid", HALOW_SSID, out, out_len);
}

esp_err_t warthog_cfg_get_halow_psk(char *out, size_t out_len)
{
    return get_string_or_default("halow_psk", HALOW_PASSPHRASE, out, out_len);
}

esp_err_t warthog_cfg_set_halow(const char *ssid, const char *psk)
{
    if (strlen(ssid) > WARTHOG_CFG_SSID_MAXLEN || strlen(psk) > WARTHOG_CFG_PSK_MAXLEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!ssid || !psk) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "halow_ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "halow_psk", psk);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "halow creds stored (ssid='%s')", ssid);
    }
    return err;
}

esp_err_t warthog_cfg_get_ap_ssid(char *out, size_t out_len)
{
    return get_string_or_default("ap_ssid", WARTHOG_AP_SSID, out, out_len);
}

esp_err_t warthog_cfg_get_ap_psk(char *out, size_t out_len)
{
    return get_string_or_default("ap_psk", WARTHOG_AP_PSK, out, out_len);
}

uint8_t warthog_cfg_get_ap_channel(void)
{
    nvs_handle_t h;
    uint8_t chan = WARTHOG_AP_CHANNEL;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "ap_chan", &v) == ESP_OK && v >= 1 && v <= 13) {
            chan = v;
        }
        nvs_close(h);
    }
    return chan;
}

esp_err_t warthog_cfg_set_ap(const char *ssid, const char *psk, int channel)
{
    if (strlen(ssid) > WARTHOG_CFG_SSID_MAXLEN || strlen(psk) > WARTHOG_CFG_PSK_MAXLEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!ssid || !psk) {
        return ESP_ERR_INVALID_ARG;
    }
    if (channel != 0 && (channel < 1 || channel > 13)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "ap_ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "ap_psk", psk);
    }
    /* channel=0 means "leave channel as-is". */
    if (err == ESP_OK && channel != 0) {
        err = nvs_set_u8(h, "ap_chan", (uint8_t)channel);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ap creds stored (ssid='%s' chan=%d)", ssid, channel);
    }
    return err;
}

uint8_t warthog_cfg_get_mesh_secure(void)
{
    nvs_handle_t h;
    uint8_t secure = 1; /* keyed, matching the build default */
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "mesh_sec", &v) == ESP_OK && v <= 1) {
            secure = v;
        }
        nvs_close(h);
    }
    return secure;
}

esp_err_t warthog_cfg_set_mesh_secure(uint8_t secure)
{
    if (secure > 1) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "mesh_sec", secure);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "mesh data plane stored: %s", secure ? "keyed" : "open");
    }
    return err;
}

esp_err_t warthog_cfg_get_dns(char *out, size_t out_len)
{
    return get_string_or_default("dns", WARTHOG_DOWNSTREAM_DNS, out, out_len);
}

esp_err_t warthog_cfg_set_dns(const char *dns)
{
    if (!dns) {
        return ESP_ERR_INVALID_ARG;
    }
    /* esp_netif_str_to_ip4 wraps inet_pton — rejects empty strings, trailing
     * garbage, octets > 255, IPv6, etc. Stricter than esp_ip4addr_aton (which
     * returns IPADDR_NONE on failure but happily accepts e.g. "1.2.3" too). */
    esp_ip4_addr_t parsed;
    if (esp_netif_str_to_ip4(dns, &parsed) != ESP_OK || parsed.addr == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "dns", dns);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "downstream DNS stored: %s", dns);
    }
    return err;
}

esp_err_t warthog_cfg_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
