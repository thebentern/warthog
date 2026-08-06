#pragma once

#include "esp_err.h"
#include "esp_netif.h"

/* Bring up TinyUSB RNDIS+ECM + a DHCP-serving esp_netif. NULL on failure. */
esp_netif_t *warthog_usb_net_start(void);
