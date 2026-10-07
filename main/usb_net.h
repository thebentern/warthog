#pragma once

#include "cdc_out.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "hang_guard_core.h"
#include "usbnet_core.h"

/* Bring up TinyUSB CDC-NCM (CDC-ECM on warthog-us-ecm) + a DHCP-serving esp_netif. NULL on failure. */
esp_netif_t *warthog_usb_net_start(void);

/* The USB network counters AT+STATUS? prints; zeros before warthog_usb_net_start. */
void warthog_usb_net_stats(struct usbnet_stats *out);

/* The hang guard's probe of the TinyUSB task: OFF before TinyUSB starts, FULL while its queue holds an event. */
enum warthog_hang_post warthog_usb_net_ping(void);
uint32_t warthog_usb_net_pongs(void);

/* CDC ACM 0 output: the AT task's replies and every no-wait writer, one lock. */
extern struct cdc_out g_warthog_cdc;
