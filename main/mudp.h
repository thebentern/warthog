#pragma once
#include "esp_err.h"
#include <stddef.h>

/** Start the UDP multicast repeater (239.0.0.69:4403 across all netifs). */
esp_err_t warthog_mudp_start(void);
/** Format the +MUDP: status line into buf. Returns bytes written. */
int warthog_mudp_status(char *buf, size_t len);
/** Hexdump of the last datagram received from the HaLow mesh. */
int warthog_mudp_last_halow(char *buf, size_t len);
/** Inject a datagram as if received on the AP netif; relayed to all others. */
int warthog_mudp_inject_from_ap(const uint8_t *data, size_t len);
