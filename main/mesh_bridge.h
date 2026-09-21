#pragma once
#include "esp_err.h"
#include "esp_netif.h"
#include <stdbool.h>
#include <stdint.h>

/* L2 bridge mode (AT+MESHBRIDGE=1): the USB and Wi-Fi AP netifs become ports
 * of one lwIP bridge together with the mesh netif, so tethered hosts sit on
 * the mesh segment with their own MACs instead of behind NAT. */
esp_err_t   warthog_mesh_bridge_start(esp_netif_t *mesh_netif, const uint8_t mac[6]);
bool        warthog_mesh_bridge_active(void);
esp_netif_t *warthog_mesh_bridge_netif(void);
