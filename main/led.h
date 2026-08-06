#pragma once

#include <stdbool.h>
#include "esp_err.h"

/*
 * Compound LED states. The HaLow link is the primary pattern; USB attach is a
 * modifier that overlays a brief "wink" on the primary pattern.
 *
 * Primary patterns:
 *   BOOT              — 10 Hz frantic blink
 *   HALOW_CONNECTING  — 0.5 Hz heartbeat
 *   HALOW_UP          — solid on
 *   ERROR             — double-blink heartbeat
 *
 * USB attached modifier: brief 100 ms gap every 2 s when host is connected.
 */
typedef enum {
    WARTHOG_LED_BOOT = 0,
    WARTHOG_LED_HALOW_CONNECTING,
    WARTHOG_LED_HALOW_UP,
    WARTHOG_LED_ERROR,
} warthog_led_state_t;

esp_err_t warthog_led_start(void);

/* Both are safe from ISR / TinyUSB callback context. */
void warthog_led_set(warthog_led_state_t state);
void warthog_led_set_usb(bool attached);
