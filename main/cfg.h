#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/*
 * NVS-backed config store. All getters fall back to the compile-time -D
 * defaults when the NVS entry is missing, so a freshly-flashed board with no
 * NVS state still boots with whatever credentials were baked in.
 *
 * Strings are returned null-terminated. out_len is the buffer size; on
 * success it's updated to the byte length (excluding the null).
 */

esp_err_t warthog_cfg_init(void);

/* Maximum stored lengths, excluding the terminator. These are what the boot
 * paths read into (char ssid[33] / char psk[65] in halow.c and wifi_ap.c);
 * anything longer makes nvs_get_str return ESP_ERR_NVS_INVALID_LENGTH, and the
 * getter then falls back to the build-time default -- so an over-long value
 * was accepted, confirmed as stored, and then silently ignored at boot while
 * the readback still showed it. Reject on the way in instead. */
#define WARTHOG_CFG_SSID_MAXLEN 32
#define WARTHOG_CFG_PSK_MAXLEN 64

esp_err_t warthog_cfg_get_halow_ssid(char *out, size_t out_len);
esp_err_t warthog_cfg_get_halow_psk(char *out, size_t out_len);
esp_err_t warthog_cfg_set_halow(const char *ssid, const char *psk);

esp_err_t warthog_cfg_get_ap_ssid(char *out, size_t out_len);
esp_err_t warthog_cfg_get_ap_psk(char *out, size_t out_len);
uint8_t   warthog_cfg_get_ap_channel(void);
esp_err_t warthog_cfg_set_ap(const char *ssid, const char *psk, int channel);

/* DNS handed out via DHCP option 6 to USB ECM + Wi-Fi AP clients. The default
 * is WARTHOG_DOWNSTREAM_DNS from the build flag (1.1.1.1); set persists to
 * NVS and takes effect on next boot (AT+RESET). Input must be dotted-quad
 * IPv4; invalid strings return ESP_ERR_INVALID_ARG without touching NVS. */
esp_err_t warthog_cfg_get_dns(char *out, size_t out_len);
esp_err_t warthog_cfg_set_dns(const char *dns);

/* Mesh data-plane protection: 1 = keyed (a fixed shared key), 0 = open.
 *
 * Persisted because the alternative is worse than it looks: an unencrypted
 * peer such as stock OpenMANET needs 0, and a node that forgets across reboot
 * comes back peering perfectly and carrying no data, with nothing in any log
 * to say why. Default is the keyed build behaviour. */
uint8_t   warthog_cfg_get_mesh_secure(void);
esp_err_t warthog_cfg_set_mesh_secure(uint8_t secure);

/* Wipe the entire "warthog" NVS namespace. */
esp_err_t warthog_cfg_erase(void);
