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

/* 802.11s forwarding: relay other nodes' data and HWMP through this node.
 * Default off -- the proven leaf behaviour. Next boot. */
uint8_t   warthog_cfg_get_mesh_fwd(void);
esp_err_t warthog_cfg_set_mesh_fwd(uint8_t on);

/* L2 bridge mode: put the USB and AP netifs on the mesh segment instead of
 * NATing them. Default off -- NAT is the proven path. Next boot. */
uint8_t   warthog_cfg_get_mesh_bridge(void);
esp_err_t warthog_cfg_set_mesh_bridge(uint8_t on);

/* Group frames as standard 3-address 802.11s broadcasts (1) instead of one
 * unicast per peer (0, the measured path). Default 0. Next boot. */
uint8_t   warthog_cfg_get_mesh_grp(void);
esp_err_t warthog_cfg_set_mesh_grp(uint8_t on);

/* Management frame protection for the mesh: 0 = off (the warthog-to-warthog
 * default that is measured working), 1 = MFP required.
 *
 * Only these two -- "optional" is the one setting that makes the two ends size
 * the AMPE payload differently, so it is deliberately not reachable from here.
 * An OpenMANET peer emits ieee80211w=2 and reports MFP: yes, and that is NOT a
 * reason to turn this on: such a peer reached ESTAB with a warthog that had it
 * off, because the framing follows our own RSN element. This is for a peer that
 * actually refuses unprotected peering. Not run on air. Next boot. */
uint8_t   warthog_cfg_get_mesh_pmf(void);
esp_err_t warthog_cfg_set_mesh_pmf(uint8_t on);

/* S1G channel pin, stored and returned as a set.
 *
 * Channel, frequency, operating class and bandwidth are not independent --
 * class and bandwidth belong to the channel, and a mismatched set peers with
 * nothing while looking like a radio fault. They are written atomically and
 * only ever read together, so a half-applied change cannot exist.
 *
 * Returns false when nothing is stored, in which case the build-time pin is
 * used. Values are sanity-checked on the way in, but the chip's regulatory
 * database is the final arbiter: a set it rejects at boot is discarded and the
 * build-time pin used instead, rather than transmitting something unvetted. */
struct warthog_mesh_chan {
    uint32_t freq_hz;
    uint16_t chan;
    uint8_t  global_op_class;
    uint8_t  op_class;
    uint8_t  bw_mhz;
};

bool      warthog_cfg_get_mesh_chan(struct warthog_mesh_chan *out);
esp_err_t warthog_cfg_set_mesh_chan(const struct warthog_mesh_chan *in);
esp_err_t warthog_cfg_clear_mesh_chan(void);

/* Wipe the entire "warthog" NVS namespace. */
esp_err_t warthog_cfg_erase(void);
