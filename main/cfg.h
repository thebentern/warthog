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

/* Mesh identity and credentials, runtime-settable.
 *
 * These used to be compile-time only, which meant a mesh ID or passphrase
 * mismatch against a peer cost a rebuild and reflash -- and a mismatch is the
 * most common field failure, because it peers with nothing while looking like
 * a radio fault. Getters fall back to the build-time defaults, so existing
 * images behave exactly as before until something is set. */
#define WARTHOG_CFG_MESH_ID_MAXLEN 32
#define WARTHOG_CFG_MESH_PASS_MAXLEN 63

esp_err_t warthog_cfg_get_mesh_id(char *out, size_t out_len);
esp_err_t warthog_cfg_set_mesh_id(const char *mesh_id);
esp_err_t warthog_cfg_get_mesh_pass(char *out, size_t out_len);
esp_err_t warthog_cfg_set_mesh_pass(const char *pass);

/* Start the mesh instead of associating as a station.
 *
 * Mesh used to be reachable only from the capability build envs, so a region
 * image could never join one no matter how it was configured. Default is 0
 * (station), preserving existing behaviour; the mesh envs force it on. */
uint8_t   warthog_cfg_get_mesh_enable(void);
esp_err_t warthog_cfg_set_mesh_enable(uint8_t enable);

/* Mesh IP addressing: 1 = try DHCP first, 0 = go straight to the static
 * 10.77.<mac4>.<mac5>/16 fallback.
 *
 * An idiomatic OpenMANET node keeps its mesh interface enslaved to a bridge
 * (and to bat0) with a DHCP server on it. Self-assigning a 10.77 address made
 * that unreachable, which is why the setup guide used to tell operators to
 * un-enslave the peer's mesh interface -- dismantling their batman fabric to
 * talk to us. Taking a lease when one is offered removes that requirement.
 *
 * The static fallback still applies on a mesh with no DHCP server, which is
 * every warthog-to-warthog mesh, so that path is unchanged. */
uint8_t   warthog_cfg_get_mesh_dhcp(void);
esp_err_t warthog_cfg_set_mesh_dhcp(uint8_t on);

/* Wipe the entire "warthog" NVS namespace. */
esp_err_t warthog_cfg_erase(void);
