#pragma once

#include "esp_err.h"

/* Start the CDC AT-command parser task. The CDC ACM interface must be
 * initialized via warthog_usb_net_start() first. */
esp_err_t warthog_at_start(void);
