#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

struct warthog_region_plan;

/* Boot the MM6108 and start STA association. Async — see the log. */
esp_err_t warthog_halow_start(void);

/* Safe mode: the event loop and netif warthog_halow_start sets up, the chip held in reset. */
esp_err_t warthog_halow_start_safe(void);

/* Block until the HaLow STA reaches association (or gets a DHCP lease),
 * whichever lands first, or until timeout_ms elapses. Used to sequence
 * USB-OTG bring-up AFTER the HaLow SAE TX burst so the two inrush
 * transients don't stack and brown out the XIAO 3V3 LDO.
 * Returns ESP_OK if the link came up, ESP_ERR_TIMEOUT otherwise. */
esp_err_t warthog_halow_wait_link(uint32_t timeout_ms);

/* This boot's region plan (region.h), or NULL before one was made (safe mode, the hang stop). */
const struct warthog_region_plan *warthog_region_running(void);

/* The chip was booted with a channel list: false in safe mode and while the region holds it. */
bool warthog_halow_radio_up(void);
