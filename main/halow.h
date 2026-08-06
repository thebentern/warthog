#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Boot the MM6108 and start STA association. Async — see the log. */
esp_err_t warthog_halow_start(void);

/* Block until the HaLow STA reaches association (or gets a DHCP lease),
 * whichever lands first, or until timeout_ms elapses. Used to sequence
 * USB-OTG bring-up AFTER the HaLow SAE TX burst so the two inrush
 * transients don't stack and brown out the XIAO 3V3 LDO.
 * Returns ESP_OK if the link came up, ESP_ERR_TIMEOUT otherwise. */
esp_err_t warthog_halow_wait_link(uint32_t timeout_ms);
