#pragma once
#include "esp_err.h"
#include "esp_netif.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* L2 bridge mode (AT+MESHBRIDGE=1): the USB and Wi-Fi AP netifs become ports
 * of one lwIP bridge together with the mesh netif, so tethered hosts sit on
 * the mesh segment with their own MACs instead of behind NAT. */

/* app_main, once, after the Wi-Fi AP start and before NAT and the multicast repeater start. Builds the
 * bridge whole or not at all: on any error nothing of NAT mode was touched. @p ap_netif is
 * warthog_wifi_ap_start()'s result (NULL: the AP did not start and is no port). */
esp_err_t   warthog_mesh_bridge_start(esp_netif_t *mesh_netif, esp_netif_t *ap_netif, const uint8_t mac[6]);
/* app_main, in place of the start when no bridge is to be built this boot: ends any wait for one. */
void        warthog_mesh_bridge_none(void);
bool        warthog_mesh_bridge_active(void);
/* Bridge mode is on for this boot and start has neither succeeded nor failed
 * yet: the mesh netif must wait for it. False once it has failed, so the NAT
 * fallback runs; runtime state, never the persisted key. */
bool        warthog_mesh_bridge_pending(void);
/* A bridge was to be built this boot and none was: the node runs NAT, as with the setting at 0. */
bool        warthog_mesh_bridge_failed(void);
esp_netif_t *warthog_mesh_bridge_netif(void);
/* mesh-probe task, every tick once a peer has reached ESTAB: raises the bridge's mesh port, once. */
void        warthog_mesh_bridge_port_up(void);
/* AT+MESHBRIDGE?'s tail, without the line end; returns the characters written (at most len - 1). */
int         warthog_mesh_bridge_status(char *buf, size_t len);
