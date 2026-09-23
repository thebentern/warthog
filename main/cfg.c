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

/* Mesh identity/credentials. Build-time values remain the fallback, so an
 * unconfigured board is identical to one built before these existed. */
#ifndef WARTHOG_MESH_ID
#define WARTHOG_MESH_ID "warthog-mesh-test"
#endif
#ifndef WARTHOG_MESH_PASSPHRASE
#define WARTHOG_MESH_PASSPHRASE "warthog-mesh"
#endif

esp_err_t warthog_cfg_get_mesh_id(char *out, size_t out_len)
{
    return get_string_or_default("mesh_id", WARTHOG_MESH_ID, out, out_len);
}

esp_err_t warthog_cfg_set_mesh_id(const char *mesh_id)
{
    if (!mesh_id) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = strlen(mesh_id);
    /* 802.11s Mesh ID is 0..32 octets, but an empty one peers with nothing
     * and looks like a radio fault, so refuse it here rather than on air. */
    if (n == 0 || n > WARTHOG_CFG_MESH_ID_MAXLEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "mesh_id", mesh_id);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t warthog_cfg_get_mesh_pass(char *out, size_t out_len)
{
    return get_string_or_default("mesh_pass", WARTHOG_MESH_PASSPHRASE, out, out_len);
}

esp_err_t warthog_cfg_set_mesh_pass(const char *pass)
{
    if (!pass) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = strlen(pass);
    if (n == 0 || n > WARTHOG_CFG_MESH_PASS_MAXLEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "mesh_pass", pass);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

uint8_t warthog_cfg_get_mesh_enable(void)
{
#ifdef WARTHOG_MESH_SMOKE
    /* The capability envs exist to run mesh; do not let stale NVS disable it. */
    return 1;
#else
    nvs_handle_t h;
    uint8_t en = 0; /* station uplink, the historical default */
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "mesh_en", &v) == ESP_OK && v <= 1) {
            en = v;
        }
        nvs_close(h);
    }
    return en;
#endif
}

esp_err_t warthog_cfg_set_mesh_enable(uint8_t enable)
{
    if (enable > 1) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "mesh_en", enable);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* S1G occupies roughly 750-950 MHz across all regions. This is a sanity bound
 * to catch a typo, not a regulatory check -- mmwlan_set_channel_list() applies
 * the real one against the country's table at boot. */
#define WARTHOG_S1G_FREQ_MIN_HZ 750000000u
#define WARTHOG_S1G_FREQ_MAX_HZ 950000000u

bool warthog_cfg_get_mesh_chan(struct warthog_mesh_chan *out)
{
    if (!out) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    struct warthog_mesh_chan tmp = {0};
    size_t len = sizeof(tmp);
    bool ok = (nvs_get_blob(h, "mesh_chan", &tmp, &len) == ESP_OK && len == sizeof(tmp));
    nvs_close(h);
    if (ok) {
        *out = tmp;
    }
    return ok;
}

esp_err_t warthog_cfg_set_mesh_chan(const struct warthog_mesh_chan *in)
{
    if (!in) {
        return ESP_ERR_INVALID_ARG;
    }
    if (in->freq_hz < WARTHOG_S1G_FREQ_MIN_HZ || in->freq_hz > WARTHOG_S1G_FREQ_MAX_HZ) {
        return ESP_ERR_INVALID_ARG;
    }
    /* S1G bandwidths are 1, 2, 4, 8 and 16 MHz; the MM6108 does not do 16. */
    if (in->bw_mhz != 1 && in->bw_mhz != 2 && in->bw_mhz != 4 && in->bw_mhz != 8) {
        return ESP_ERR_INVALID_ARG;
    }
    if (in->chan == 0 || in->global_op_class == 0 || in->op_class == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, "mesh_chan", in, sizeof(*in));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t warthog_cfg_clear_mesh_chan(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, "mesh_chan");
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

uint8_t warthog_cfg_get_mesh_dhcp(void)
{
    nvs_handle_t h;
    uint8_t on = 1; /* try DHCP first; falls back on its own */
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "mesh_dhcp", &v) == ESP_OK && v <= 1) {
            on = v;
        }
        nvs_close(h);
    }
    return on;
}

esp_err_t warthog_cfg_set_mesh_dhcp(uint8_t on)
{
    if (on > 1) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "mesh_dhcp", on);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

uint8_t warthog_cfg_get_mesh_fwd(void)
{
    nvs_handle_t h;
    uint8_t on = 0;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "mesh_fwd", &v) == ESP_OK && v <= 1) {
            on = v;
        }
        nvs_close(h);
    }
    return on;
}

esp_err_t warthog_cfg_set_mesh_fwd(uint8_t on)
{
    if (on > 1) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "mesh_fwd", on);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

uint8_t warthog_cfg_get_mesh_bridge(void)
{
    nvs_handle_t h;
    uint8_t on = 0;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "mesh_br", &v) == ESP_OK && v <= 1) {
            on = v;
        }
        nvs_close(h);
    }
    return on;
}

esp_err_t warthog_cfg_set_mesh_bridge(uint8_t on)
{
    if (on > 1) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "mesh_br", on);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

uint8_t warthog_cfg_get_mesh_grp(void)
{
    nvs_handle_t h;
    uint8_t on = 0;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "mesh_grp", &v) == ESP_OK && v <= 1) {
            on = v;
        }
        nvs_close(h);
    }
    return on;
}

esp_err_t warthog_cfg_set_mesh_grp(uint8_t on)
{
    if (on > 1) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "mesh_grp", on);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

uint8_t warthog_cfg_get_mesh_pmf(void)
{
    nvs_handle_t h;
    uint8_t on = 0; /* off -- the warthog-to-warthog value that is measured working */
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "mesh_pmf", &v) == ESP_OK && v <= 1) {
            on = v;
        }
        nvs_close(h);
    }
    return on;
}

esp_err_t warthog_cfg_set_mesh_pmf(uint8_t on)
{
    if (on > 1) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "mesh_pmf", on);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
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
