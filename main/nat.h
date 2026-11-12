#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Start the NAT supervisor: NAPT on the inside netifs (USB, Wi-Fi AP) from boot, the HaLow netif the
 * default route once it has an address. */
esp_err_t warthog_nat_start(void);

/* AT+HEAP?: lwIP's NAPT table. */
struct warthog_napt_stat {
    uint32_t entries; /* sessions it holds (IP_NAPT_MAX) */
    uint32_t bytes;   /* what lwIP allocates for it and the port map, at the first enable */
    uint32_t on;      /* 1 once that enable has been made */
    uint32_t refused; /* 2 s passes on which it was not, for want of one free block for it */
};
void warthog_nat_napt_stat(struct warthog_napt_stat *out);
