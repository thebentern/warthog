/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hang_guard.h"

#include "boot_guard.h"
#include "esp_attr.h"
#include "esp_freertos_hooks.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "lwip/tcpip.h"
#include "lwip/timeouts.h"
#include "mmwlan.h"
#include "mmwlan_mesh.h"
#include "sdkconfig.h"
#include "usb_net.h"

#if !defined(CONFIG_ESP_TASK_WDT_PANIC) || CONFIG_ESP_TASK_WDT_TIMEOUT_S != WARTHOG_HANG_TWDT_S || \
    !defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0) || !defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1)
#error "task watchdog config predates sdkconfig.defaults: delete sdkconfig.<env> and rebuild"
#endif
_Static_assert(MMWLAN_HANG_BLOCK_LOOP == WARTHOG_HANG_TEST_LOOP && MMWLAN_HANG_BLOCK_HEALTH ==
               WARTHOG_HANG_TEST_HEALTH && MMWLAN_HANG_BLOCK_DRV == WARTHOG_HANG_TEST_DRV,
               "AT+HANGTEST ids differ between morselib and main");
_Static_assert(WARTHOG_HANG_LIMIT_S >= WARTHOG_BOOT_OK_S + 10u,
               "a hang abort must come after the crash-boot count clears");

static const char *TAG = "warthog.hang";

static __NOINIT_ATTR struct warthog_hang_rec s_hang_nv;
static struct warthog_hang_rec s_prev;
static bool s_prev_valid;
static struct warthog_hang_state s_probe[WARTHOG_HANG_PROBES];
static bool s_twdt, s_started;
static struct tcpip_callback_msg *s_tcpip_msg;
static volatile bool s_tcpip_pending;
static volatile uint32_t s_tcpip_pongs;
static esp_timer_handle_t s_timer;
static volatile uint32_t s_timer_beats;
static volatile uint32_t s_idle_ms[2];
static bool s_idle_on[2];
static uint32_t s_idle_gap[2], s_idle_max[2];
static char s_reason[WARTHOG_HANG_REASON_MAX];

static uint32_t now_ms_(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }
/* AT+HANGTEST: the task that answers probe @p id waits here, 1 s at a time, until released. */
static void block_wait_(uint32_t id) { while (g_warthog_hang_block == id) { vTaskDelay(pdMS_TO_TICKS(1000)); } }

/* On tiT, from lwIP's timers, which run before each mailbox fetch: it beats while the mailbox stays full. */
static void tcpip_beat_(void *arg)
{
    (void)arg;
    block_wait_(WARTHOG_HANG_TEST_TCPIP);
    s_tcpip_pongs++;
    sys_timeout(WARTHOG_HANG_TICK_MS, tcpip_beat_, NULL);
}

/* On tiT: answers, and restarts the beat, so exactly one runs. */
static void tcpip_pong_(void *ctx)
{
    (void)ctx;
    s_tcpip_pending = false;
    sys_untimeout(tcpip_beat_, NULL);
    tcpip_beat_(NULL);
}

/* On the esp_timer task. */
static void timer_beat_(void *arg)
{
    (void)arg;
    block_wait_(WARTHOG_HANG_TEST_TIMER);
    s_timer_beats++;
}

static bool idle0_(void) { s_idle_ms[0] = now_ms_(); return true; }
static bool idle1_(void) { s_idle_ms[1] = now_ms_(); return true; }

static enum warthog_hang_post loop_post_(void)
{
    const enum mmwlan_status st = mmwlan_loop_ping();
    return st == MMWLAN_SUCCESS ? WARTHOG_HANG_POSTED : st == MMWLAN_NO_MEM ? WARTHOG_HANG_FULL : WARTHOG_HANG_OFF;
}

/* One static message at most in tiT's mailbox; never waits for room. */
static enum warthog_hang_post tcpip_post_(void)
{
    if (s_tcpip_msg == NULL) {
        return WARTHOG_HANG_OFF;
    }
    if (s_tcpip_pending) {
        return WARTHOG_HANG_POSTED;
    }
    s_tcpip_pending = true;
    if (tcpip_callbackmsg_trycallback(s_tcpip_msg) != ERR_OK) {
        s_tcpip_pending = false;
        return WARTHOG_HANG_FULL;
    }
    return WARTHOG_HANG_POSTED;
}

void warthog_hang_guard_early(void)
{
    if (s_hang_nv.magic == WARTHOG_HANG_MAGIC) {
        s_prev = s_hang_nv;
        s_prev_valid = true;
    }
    s_hang_nv.magic = 0;
}

void warthog_hang_guard_start(bool usb)
{
    warthog_hang_init(s_probe);
    if (usb) {
        /* The ROM console is dead once TinyUSB owns the PHY: the TWDT's prints then go nowhere at once. */
        esp_rom_install_channel_putc(1, NULL);
        esp_rom_install_channel_putc(2, NULL);
    }
    s_twdt = esp_task_wdt_add(NULL) == ESP_OK;
    s_tcpip_msg = tcpip_callbackmsg_new(tcpip_pong_, NULL);
    const esp_timer_create_args_t args = {
        .callback = timer_beat_,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "hang",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &s_timer) != ESP_OK) {
        s_timer = NULL;
    } else if (esp_timer_start_periodic(s_timer, 1000000) != ESP_OK) {
        (void)esp_timer_delete(s_timer);
        s_timer = NULL;
    }
    s_idle_ms[0] = s_idle_ms[1] = now_ms_();
    s_idle_on[0] = esp_register_freertos_idle_hook_for_cpu(idle0_, 0) == ESP_OK;
    s_idle_on[1] = esp_register_freertos_idle_hook_for_cpu(idle1_, 1) == ESP_OK;
    ESP_LOGI(TAG, "hang guard: twdt=%d tcpip=%d timer=%d idle=%d%d usb=%d limit %u s",
             (int)s_twdt, s_tcpip_msg != NULL, s_timer != NULL, (int)s_idle_on[0], (int)s_idle_on[1], (int)usb,
             (unsigned)WARTHOG_HANG_LIMIT_S);
    s_started = true;
}

/* No log, allocation, lock or wait: only AT+HANGTEST=main's block sleeps here. */
void warthog_hang_guard_tick(void)
{
    block_wait_(WARTHOG_HANG_TEST_MAIN);
    if (!s_started) {
        return;
    }
    const uint32_t now = now_ms_();
    uint32_t a = g_warthog_loop_pongs;
    warthog_hang_step(&s_probe[WARTHOG_HANG_LOOP], a, loop_post_(), now);
    a = s_tcpip_pongs;
    warthog_hang_step(&s_probe[WARTHOG_HANG_TCPIP], a, tcpip_post_(), now);
    a = warthog_usb_net_pongs();
    warthog_hang_step(&s_probe[WARTHOG_HANG_USB], a, warthog_usb_net_ping(), now);
    warthog_hang_step(&s_probe[WARTHOG_HANG_TIMER], s_timer_beats,
                      s_timer != NULL ? WARTHOG_HANG_POSTED : WARTHOG_HANG_OFF, now);
    uint32_t wakes = 0, interval = 0;
    const bool health = mmwlan_hang_health(&wakes, &interval);
    s_probe[WARTHOG_HANG_HEALTH].limit_ms = warthog_hang_health_limit_ms(interval);
    warthog_hang_step(&s_probe[WARTHOG_HANG_HEALTH], wakes, health ? WARTHOG_HANG_POSTED : WARTHOG_HANG_OFF, now);
    for (int c = 0; c < 2; c++) {
        s_idle_gap[c] = s_idle_on[c] ? warthog_hang_gap_ms(now, s_idle_ms[c]) : WARTHOG_HANG_NONE;
        if (s_idle_on[c] && s_idle_gap[c] > s_idle_max[c]) {
            s_idle_max[c] = s_idle_gap[c];
        }
    }
    const uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000);
    warthog_hang_rec_fill(&s_hang_nv, s_probe, s_idle_gap, up_s, g_warthog_hang_block);
    const int stalled = warthog_hang_stalled(s_probe);
    if (stalled >= 0) {
        (void)warthog_hang_reason(s_reason, sizeof(s_reason), stalled, g_warthog_hang_block, s_probe, up_s);
        esp_system_abort(s_reason);
    }
    if (s_twdt) { esp_task_wdt_reset(); }
}

void warthog_hang_guard_view(struct warthog_hang_view *v)
{
    /* No lock: each field is one aligned word a tick wrote; one line's fields may span adjacent ticks. */
    v->twdt = s_twdt;
    for (int i = 0; i < WARTHOG_HANG_PROBES; i++) {
        v->probe[i] = s_probe[i];
    }
    for (int c = 0; c < 2; c++) {
        v->idle_max_ms[c] = s_idle_on[c] ? s_idle_max[c] : WARTHOG_HANG_NONE;
    }
    v->test = g_warthog_hang_block;
    v->up_s = (uint32_t)(esp_timer_get_time() / 1000000);
    v->prev_valid = s_prev_valid;
    v->prev = s_prev;
}
