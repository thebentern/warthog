#include "led.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>

static const char *TAG = "warthog.led";

#ifndef WARTHOG_LED_GPIO
#define WARTHOG_LED_GPIO 21
#endif
#ifndef WARTHOG_LED_ACTIVE_LOW
#define WARTHOG_LED_ACTIVE_LOW 1
#endif

#define LED_TICK_MS 50

static _Atomic int s_state = WARTHOG_LED_BOOT;
static _Atomic bool s_usb_attached = false;
static bool s_started = false;

static inline void led_write(bool on)
{
#if WARTHOG_LED_ACTIVE_LOW
    gpio_set_level(WARTHOG_LED_GPIO, on ? 0 : 1);
#else
    gpio_set_level(WARTHOG_LED_GPIO, on ? 1 : 0);
#endif
}

/* `tick` is in 50ms units. */
static bool primary_pattern(warthog_led_state_t state, uint32_t tick)
{
    switch (state) {
    case WARTHOG_LED_BOOT:
        return (tick % 4) < 2;                /* 10 Hz */
    case WARTHOG_LED_HALOW_CONNECTING:
        return (tick % 40) < 20;              /* 0.5 Hz heartbeat */
    case WARTHOG_LED_HALOW_UP:
        return true;                          /* solid on */
    case WARTHOG_LED_ERROR: {
        uint32_t phase = tick % 40;
        return (phase < 2) || (phase >= 4 && phase < 6); /* double-blink */
    }
    }
    return false;
}

/* USB wink: every 2 s, force LED off for 100 ms. Only takes effect when
 * the primary pattern would otherwise be on — we never "add" light, only
 * subtract, so the wink reads cleanly against any background pattern. */
static bool apply_usb_modifier(bool primary_on, uint32_t tick)
{
    if (!primary_on) {
        return false;
    }
    if (!atomic_load(&s_usb_attached)) {
        return true;
    }
    return (tick % 40) >= 2;  /* 100 ms off, 1.9 s on, repeating */
}

static void led_task(void *arg)
{
    (void)arg;
    uint32_t tick = 0;
    int last_state = -1;
    bool last_usb = false;
    while (1) {
        int cur = atomic_load(&s_state);
        bool usb = atomic_load(&s_usb_attached);
        if (cur != last_state || usb != last_usb) {
            ESP_LOGI(TAG, "state=%d usb=%d", cur, usb);
            last_state = cur;
            last_usb = usb;
        }
        bool on = primary_pattern((warthog_led_state_t)cur, tick);
        led_write(apply_usb_modifier(on, tick));
        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
        tick++;
    }
}

esp_err_t warthog_led_start(void)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << WARTHOG_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }
    led_write(false);

    BaseType_t ok = xTaskCreate(led_task, "warthog_led", 2048, NULL,
                                tskIDLE_PRIORITY + 1, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

void warthog_led_set(warthog_led_state_t state)
{
    atomic_store(&s_state, (int)state);
}

void warthog_led_set_usb(bool attached)
{
    atomic_store(&s_usb_attached, attached);
}
