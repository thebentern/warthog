#pragma once

#include "esp_err.h"

/* Enable lwIP NAPT on the HaLow uplink once it gets an IP. */
esp_err_t warthog_nat_start(void);
