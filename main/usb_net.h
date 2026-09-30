#pragma once

#include "cdc_out.h"
#include "esp_err.h"
#include "esp_netif.h"

/* Bring up TinyUSB RNDIS+ECM + a DHCP-serving esp_netif. NULL on failure. */
esp_netif_t *warthog_usb_net_start(void);

/* CDC ACM 0 output: the AT task's replies and every no-wait writer, one lock. */
extern struct cdc_out g_warthog_cdc;
