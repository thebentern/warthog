#pragma once

#include "esp_err.h"
#include "esp_netif.h"

/* Start the 2.4 GHz AP. SSID/PSK from WARTHOG_AP_SSID / WARTHOG_AP_PSK. */
esp_netif_t *warthog_wifi_ap_start(void);
