/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bat_port.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bat_mode.h"
#include "cfg.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netif.h"
#include "mmhalow.h"
#include "mmpkt.h"
#include "mmwlan.h"
#include "mmwlan_mesh.h"

#ifndef WARTHOG_MESH_SAE
#define WARTHOG_MESH_SAE 0
#endif
#ifdef ESP_PLATFORM
#include "esp_attr.h" /* RTC_NOINIT_ATTR */
#else
#define RTC_NOINIT_ATTR /* host builds of this file (test_glue_guard.sh) */
#endif

static const char *TAG = "warthog.bat";

#define BAT_PORT_RX_SLOTS     6
#define BAT_PORT_TX_SLOTS     4
#define BAT_PORT_TX_SLOT_LEN  1536
#define BAT_PORT_TASK_STACK   6144
#define BAT_PORT_TASK_PRIO    3 /* tasks that reach umac run at <= 3 (mesh.c) */
#define BAT_PORT_RENDER_MS    2000
#define BAT_PORT_TX_READY_MS  50
#define BAT_PORT_TX_SLOT_MS   (BAT_PORT_TX_READY_MS + 50) /* lwIP's wait for a slot: past bat_port_tx's own */
#define BAT_PORT_ANSWER_MS    500 /* refresh of the watched MAC's and the gateway's answers */
#define BAT_PORT_DELIVER_MAX  16  /* copies lwIP may hold at once; more than the two UDP recvmboxes (12) */
#define BAT_PORT_CURSOR_LINE  0xFFFFFFF0u /* AT+BATSTAT?: only the port's line is left (above any engine cursor) */
#ifdef WARTHOG_BAT_BCAST_COPIES
#define BAT_PORT_COPIES       WARTHOG_BAT_BCAST_COPIES
#else
#define BAT_PORT_COPIES       0 /* bat_mode_port_config picks */
#endif

/* bat_mode_link_tput reads the snapshot through its own mirror of the morselib struct. */
_Static_assert(sizeof(struct bat_mode_link) == sizeof(struct mmwlan_mesh_peer_link) &&
               offsetof(struct bat_mode_link, addr) == offsetof(struct mmwlan_mesh_peer_link, addr) &&
               offsetof(struct bat_mode_link, estab) == offsetof(struct mmwlan_mesh_peer_link, estab) &&
               offsetof(struct bat_mode_link, rc_valid) == offsetof(struct mmwlan_mesh_peer_link, rc_valid) &&
               offsetof(struct bat_mode_link, expected_tput_kbps) ==
                   offsetof(struct mmwlan_mesh_peer_link, expected_tput_kbps),
               "struct bat_mode_link mirrors struct mmwlan_mesh_peer_link");

enum { EV_RX, EV_SOFT_TX, EV_RENDER };
struct bat_port_ev {
    uint8_t  kind, slot; /* slot: RX/TX slot index, or the render kind */
    uint16_t len;
};

enum { RENDER_IDLE, RENDER_PENDING, RENDER_DONE, RENDER_ABANDONED };

/* Written once by warthog_bat_port_start, before the task and the hooks exist; s_reason
 * again by warthog_bat_port_mesh_failed. */
static bool s_started, s_running;
static volatile int s_reason = BAT_MODE_OFF;
static uint8_t s_hard[6], s_soft[6], s_copies;
static uint32_t s_tput_override;
/* The engine's sequence numbers across a reset that keeps power (membership §7.4.3). */
static RTC_NOINIT_ATTR struct bat_seq_keep s_seq_keep;
static esp_netif_t *s_netif;

/* Heap, batman mode only. */
static struct bat *s_bat;
static uint8_t *s_rx_slot, *s_tx_slot;
static char *s_render_buf;
static QueueHandle_t s_free_rx, s_free_tx, s_work;
static SemaphoreHandle_t s_render_lock, s_render_done;
static TaskHandle_t s_task;

static volatile bool s_mesh_up;
static volatile unsigned s_routes, s_neighs;
static volatile int s_render_state = RENDER_IDLE;
static portMUX_TYPE s_render_mux = portMUX_INITIALIZER_UNLOCKED;
/* The render request: set by s_render_lock's holder before EV_RENDER, the cursor advanced by the engine task. */
static uint32_t s_render_cursor;
static uint8_t s_render_mac[6];
static bool s_render_has_mac;

/* Engine task only; heap, batman mode only. */
static struct bat_mode_links *s_links;
static uint32_t s_answer_ts;

/* bat0 addressing's question (probe task) and the engine task's answers, under s_q_mux. */
static portMUX_TYPE s_q_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool     watching;
    int8_t   ans;
    uint8_t  gws;
    uint8_t  watch[6], ans_mac[6];
    uint32_t gen, ans_gen;
    struct bat_client_route route;
    struct bat_gw gw;
} s_q;

/* One writer each: the event loop (rx_*, q_rx_full), the tcpip thread (q_tx_full,
 * soft_toobig) or the engine task (tx_*, deliver_*). */
static struct {
    volatile uint32_t q_rx_full, q_tx_full, rx_short, rx_toobig, rx_nonbat, rx_probe, rx_relayed;
    volatile uint32_t tx_busy, tx_nomem, tx_notfound, tx_fail, deliver_nomem, deliver_cap, soft_toobig;
} s_cnt;
/* Delivered copies lwIP still holds: +1 by the engine task, -1 by whichever task frees the pbuf.
 * Signed, so a free of a buffer deliver did not make cannot block deliveries for good. */
static atomic_int s_deliver_out;

static uint32_t bat_port_now_ms(void *user)
{
    (void)user;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint32_t bat_port_rand32(void *user)
{
    (void)user;
    return esp_random();
}

/* ---- engine ops (engine task) ------------------------------------------- */

static int bat_port_tx(void *user, const uint8_t *frame, size_t len)
{
    (void)user;
    if (!s_mesh_up) {
        return BAT_TX_NOPEER;
    }
    if (mmwlan_tx_wait_until_ready(BAT_PORT_TX_READY_MS) != MMWLAN_SUCCESS) {
        s_cnt.tx_busy++;
        return BAT_TX_BUSY;
    }
    struct mmpkt *pkt = mmwlan_alloc_mmpkt_for_tx((uint32_t)len, 0);
    if (pkt == NULL) {
        s_cnt.tx_nomem++;
        return BAT_TX_FAIL;
    }
    struct mmpktview *v = mmpkt_open(pkt);
    mmpkt_append_data(v, frame, (uint32_t)len);
    mmpkt_close(&v);
    /* Unicast goes to that ESTAB peer only; ff:ff takes the datapath's group shape. */
    struct mmwlan_tx_metadata md = MMWLAN_TX_METADATA_INIT;
    md.tid = 0;
    md.ra = bat_mode_tx_ra(frame);
    enum mmwlan_status st = mmwlan_tx_pkt(pkt, &md);
    if (st == MMWLAN_SUCCESS) {
        return BAT_TX_OK;
    }
    if (st == MMWLAN_NOT_FOUND) {
        s_cnt.tx_notfound++;
        return BAT_TX_NOPEER;
    }
    s_cnt.tx_fail++;
    return BAT_TX_FAIL;
}

static void bat_port_deliver(void *user, const uint8_t *frame, size_t len)
{
    (void)user;
    /* The radio's RX buffers are released at the hook, so this is what bounds lwIP's backlog. */
    if (atomic_fetch_add(&s_deliver_out, 1) >= BAT_PORT_DELIVER_MAX) {
        atomic_fetch_sub(&s_deliver_out, 1);
        s_cnt.deliver_cap++;
        return;
    }
    uint8_t *p = malloc(len);
    if (p == NULL) {
        atomic_fetch_sub(&s_deliver_out, 1);
        s_cnt.deliver_nomem++;
        return;
    }
    memcpy(p, frame, len);
    /* wlanif_input owns p from here on every path, freeing it via bat_port_free. */
    (void)esp_netif_receive(s_netif, p, len, p);
}

static bool bat_port_links_query_(void *ctx, struct bat_mode_link *out, uint8_t max, uint8_t *n)
{
    (void)ctx;
    struct mmwlan_mesh_peer_link l[BAT_MODE_LINKS_MAX];
    uint8_t got = 0;
    if (max > BAT_MODE_LINKS_MAX ||
        mmwlan_mesh_query_peer_links(l, max, &got) != MMWLAN_SUCCESS || got > max) {
        return false;
    }
    memcpy(out, l, (size_t)got * sizeof(l[0]));
    *n = got;
    return true;
}

static uint32_t bat_port_link_tput(void *user, const uint8_t hard_addr[BAT_ALEN])
{
    (void)user;
    return bat_mode_link_tput(s_links, bat_port_now_ms(NULL), hard_addr, bat_port_links_query_, NULL);
}

/* ---- the soft interface: the HaLow esp_netif's driver (tcpip thread) ---- */

static esp_err_t bat_port_netif_tx(void *h, void *buffer, size_t len)
{
    (void)h;
    if (len > BAT_PORT_TX_SLOT_LEN) {
        s_cnt.soft_toobig++;
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t slot;
    /* ip4_frag sends a datagram's fragments back to back and ignores a refusal: wait, as halow_transmit does. */
    if (xQueueReceive(s_free_tx, &slot, pdMS_TO_TICKS(BAT_PORT_TX_SLOT_MS)) != pdTRUE) {
        s_cnt.q_tx_full++;
        return ESP_ERR_NO_MEM;
    }
    memcpy(s_tx_slot + (size_t)slot * BAT_PORT_TX_SLOT_LEN, buffer, len);
    struct bat_port_ev ev = { .kind = EV_SOFT_TX, .slot = slot, .len = (uint16_t)len };
    (void)xQueueSend(s_work, &ev, 0); /* sized for every slot: cannot fail */
    return ESP_OK;
}

static esp_err_t bat_port_netif_tx_wrap(void *h, void *buffer, size_t len, void *netstack_buf)
{
    (void)netstack_buf;
    return bat_port_netif_tx(h, buffer, len);
}

static void bat_port_free(void *h, void *buffer)
{
    (void)h;
    free(buffer);
    atomic_fetch_sub(&s_deliver_out, 1);
}

/* ---- the 0x4305 hook (umac event loop: never blocks) --------------------- */

static void bat_port_rx_ext(struct mmpkt *rxbuf, const struct mmwlan_rx_metadata *md, void *arg)
{
    (void)arg;
    struct mmpktview *v = mmpkt_open(rxbuf);
    const uint8_t *data = mmpkt_get_data_start(v);
    uint32_t len = mmpkt_get_data_length(v);
    const uint8_t *ta = (md != NULL) ? md->ta : NULL;
    switch (bat_mode_rx_classify(data, len, ta)) {
    case BAT_MODE_RX_BATMAN: {
        uint8_t slot;
        if (xQueueReceive(s_free_rx, &slot, 0) != pdTRUE) {
            s_cnt.q_rx_full++;
            break;
        }
        memcpy(s_rx_slot + (size_t)slot * BAT_MAX_LINK_FRAME, data, len); /* <= BAT_MODE_RX_MAX */
        struct bat_port_ev ev = { .kind = EV_RX, .slot = slot, .len = (uint16_t)len };
        (void)xQueueSend(s_work, &ev, 0);
        break;
    }
    case BAT_MODE_RX_SHORT:   s_cnt.rx_short++;   break;
    case BAT_MODE_RX_TOOBIG:  s_cnt.rx_toobig++;  break;
    case BAT_MODE_RX_RELAYED: s_cnt.rx_relayed++; break;
    case BAT_MODE_RX_PROBE:   s_cnt.rx_probe++;   break;
    default:                  s_cnt.rx_nonbat++;  break;
    }
    mmpkt_close(&v);
    mmpkt_release(rxbuf);
}

/* ---- engine task ---------------------------------------------------------- */

/* The port's AT+BATSTAT? line if it fits @len whole with its NUL; else nothing (0). */
static size_t bat_port_stat_line(char *buf, size_t len)
{
    if (len == 0) {
        return 0;
    }
    uint32_t now = bat_port_now_ms(NULL);
    int n = snprintf(buf, len,
                     "+BATSTAT: port q_rx_full=%lu q_tx_full=%lu rx_short=%lu rx_toobig=%lu "
                     "rx_nonbat=%lu rx_probe=%lu rx_relayed=%lu tx_busy=%lu tx_nomem=%lu "
                     "tx_notfound=%lu tx_fail=%lu deliver_nomem=%lu deliver_cap=%lu soft_toobig=%lu "
                     "rx_slots=%u/%u tx_slots=%u/%u stack_free=%u tput_cache_age=%ld "
                     "tput_snap_fail=%lu heap_free=%u heap_min=%u heap_largest=%u\r\n",
                     (unsigned long)s_cnt.q_rx_full, (unsigned long)s_cnt.q_tx_full,
                     (unsigned long)s_cnt.rx_short, (unsigned long)s_cnt.rx_toobig,
                     (unsigned long)s_cnt.rx_nonbat, (unsigned long)s_cnt.rx_probe,
                     (unsigned long)s_cnt.rx_relayed, (unsigned long)s_cnt.tx_busy,
                     (unsigned long)s_cnt.tx_nomem, (unsigned long)s_cnt.tx_notfound,
                     (unsigned long)s_cnt.tx_fail, (unsigned long)s_cnt.deliver_nomem,
                     (unsigned long)s_cnt.deliver_cap, (unsigned long)s_cnt.soft_toobig,
                     (unsigned)uxQueueMessagesWaiting(s_free_rx), (unsigned)BAT_PORT_RX_SLOTS,
                     (unsigned)uxQueueMessagesWaiting(s_free_tx), (unsigned)BAT_PORT_TX_SLOTS,
                     (unsigned)uxTaskGetStackHighWaterMark(NULL),
                     s_links->valid ? (long)(now - s_links->ts) : -1L, (unsigned long)s_links->fails,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (n < 0 || (size_t)n >= len) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

static void bat_port_render_(enum bat_render_kind k)
{
    uint32_t cur = s_render_cursor;
    size_t n = 0;
    if (cur == BAT_PORT_CURSOR_LINE) {
        cur = BAT_RENDER_DONE;
        s_render_buf[0] = '\0';
    } else {
        n = bat_render_from(s_bat, k, s_render_has_mac ? s_render_mac : NULL, &cur, s_render_buf, BAT_RENDER_BUF);
    }
    /* After the engine's last chunk, or alone in one more when it does not fit there. */
    if (k == BAT_RENDER_STAT && cur == BAT_RENDER_DONE && n < BAT_RENDER_BUF &&
        bat_port_stat_line(s_render_buf + n, BAT_RENDER_BUF - n) == 0 && n > 0) {
        cur = BAT_PORT_CURSOR_LINE;
    }
    s_render_cursor = cur;
    portENTER_CRITICAL(&s_render_mux);
    bool abandoned = (s_render_state == RENDER_ABANDONED);
    s_render_state = abandoned ? RENDER_IDLE : RENDER_DONE;
    portEXIT_CRITICAL(&s_render_mux);
    /* An abandoned request's lock is ours to release: its AT caller has gone. */
    (void)xSemaphoreGive(abandoned ? s_render_lock : s_render_done);
}

static void bat_port_handle_(const struct bat_port_ev *ev)
{
    switch (ev->kind) {
    case EV_RX:
        bat_rx_hard(s_bat, s_rx_slot + (size_t)ev->slot * BAT_MAX_LINK_FRAME, ev->len);
        (void)xQueueSend(s_free_rx, &ev->slot, 0);
        break;
    case EV_SOFT_TX:
        (void)bat_tx_soft(s_bat, s_tx_slot + (size_t)ev->slot * BAT_PORT_TX_SLOT_LEN, ev->len);
        (void)xQueueSend(s_free_tx, &ev->slot, 0);
        break;
    case EV_RENDER:
        bat_port_render_((enum bat_render_kind)ev->slot);
        break;
    default:
        break;
    }
}

/* Engine task: answers a new watch at once, else refreshes every BAT_PORT_ANSWER_MS. */
static void bat_port_answer_(void)
{
    uint8_t w[6];
    portENTER_CRITICAL(&s_q_mux);
    const bool watching = s_q.watching, fresh = s_q.gen != s_q.ans_gen;
    const uint32_t gen = s_q.gen;
    memcpy(w, s_q.watch, 6);
    portEXIT_CRITICAL(&s_q_mux);
    const uint32_t now = bat_port_now_ms(NULL);
    if (!(watching && fresh) && now - s_answer_ts < BAT_PORT_ANSWER_MS) {
        return;
    }
    s_answer_ts = now;
    struct bat_client_route r = { 0 };
    const int ans = watching ? (bat_client_route(s_bat, w, &r) ? 1 : 0) : -1;
    struct bat_gw g;
    const unsigned gws = bat_gw_best(s_bat, &g);
    portENTER_CRITICAL(&s_q_mux);
    if (s_q.gen == gen) { /* else the probe task moved on meanwhile: answered next pass */
        s_q.ans = (int8_t)ans;
        s_q.route = r;
        memcpy(s_q.ans_mac, w, 6);
        s_q.ans_gen = gen;
    }
    s_q.gws = gws > UINT8_MAX ? UINT8_MAX : (uint8_t)gws;
    s_q.gw = g;
    portEXIT_CRITICAL(&s_q_mux);
}

static void bat_port_task(void *arg)
{
    (void)arg;
    /* Held until start has installed every hook, so a failed start can delete us. */
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint32_t wait = 1;
    for (;;) {
        struct bat_port_ev ev;
        TickType_t t = pdMS_TO_TICKS(wait);
        if (xQueueReceive(s_work, &ev, t ? t : 1) == pdTRUE) {
            bat_port_handle_(&ev);
        }
        wait = bat_tick(s_bat);
        s_routes = bat_route_count(s_bat);
        s_neighs = bat_neigh_count(s_bat);
        bat_port_answer_();
    }
}

/* ---- bring-up ------------------------------------------------------------- */

static esp_err_t bat_port_netif_setup_(void *ctx)
{
    esp_netif_t *n = ctx;
    /* The handle stays the Morse driver's, so mmhalow_set_config keeps working. */
    esp_netif_driver_ifconfig_t d = {
        .handle = esp_netif_get_io_driver(n),
        .transmit = bat_port_netif_tx,
        .transmit_wrap = bat_port_netif_tx_wrap,
        .driver_free_rx_buffer = bat_port_free,
    };
    esp_err_t e = esp_netif_set_driver_config(n, &d);
    struct netif *lw = esp_netif_get_netif_impl(n);
    if (e == ESP_OK && lw != NULL) {
        lw->mtu = BAT_SOFT_MTU_DEFAULT; /* IDF has no MTU setter; tcpip context */
#if LWIP_IPV6 && LWIP_ND6_ALLOW_RA_UPDATES
        lw->mtu6 = BAT_SOFT_MTU_DEFAULT;
#endif
    }
    return e;
}

static void bat_port_free_all_(void)
{
    if (s_task != NULL) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_free_rx) { vQueueDelete(s_free_rx); s_free_rx = NULL; }
    if (s_free_tx) { vQueueDelete(s_free_tx); s_free_tx = NULL; }
    if (s_work) { vQueueDelete(s_work); s_work = NULL; }
    if (s_render_lock) { vSemaphoreDelete(s_render_lock); s_render_lock = NULL; }
    if (s_render_done) { vSemaphoreDelete(s_render_done); s_render_done = NULL; }
    free(s_bat);
    free(s_rx_slot);
    free(s_tx_slot);
    free(s_render_buf);
    free(s_links);
    s_bat = NULL;
    s_rx_slot = s_tx_slot = NULL;
    s_render_buf = NULL;
    s_links = NULL;
}

esp_err_t warthog_bat_port_start(void)
{
    extern volatile uint32_t g_warthog_mesh_batman, g_warthog_mesh_grp, g_warthog_host_ccmp_on;
    s_started = true;
    s_reason = bat_mode_check(warthog_cfg_get_mesh_batman(), warthog_cfg_get_mesh_enable(),
                              warthog_cfg_get_mesh_fwd(), warthog_cfg_get_mesh_bridge(),
                              warthog_bat_port_sae_build(), warthog_bat_port_host_ccmp_build());
    if (s_reason != BAT_MODE_OK) {
        if (s_reason != BAT_MODE_OFF) {
            ESP_LOGE(TAG, "AT+MESHBATMAN=1 is stored but batman cannot run (%s): ordinary leaf",
                     bat_mode_reason_text((enum bat_mode_reason)s_reason));
        }
        return ESP_OK;
    }

    s_netif = mmhalow_get_netif();
    uint8_t factory[6] = { 0 };
    if (mmwlan_get_mac_addr(s_hard) != MMWLAN_SUCCESS) {
        s_reason = BAT_MODE_INIT_FAIL;
        ESP_LOGE(TAG, "batman: no mesh MAC (chip not booted), not started");
        return ESP_FAIL;
    }
    (void)esp_read_mac(factory, ESP_MAC_EFUSE_FACTORY);

    s_bat = calloc(1, bat_ctx_size());
    s_rx_slot = malloc((size_t)BAT_PORT_RX_SLOTS * BAT_MAX_LINK_FRAME);
    s_tx_slot = malloc((size_t)BAT_PORT_TX_SLOTS * BAT_PORT_TX_SLOT_LEN);
    s_render_buf = malloc(BAT_RENDER_BUF);
    s_links = calloc(1, sizeof(*s_links));
    s_free_rx = xQueueCreate(BAT_PORT_RX_SLOTS, sizeof(uint8_t));
    s_free_tx = xQueueCreate(BAT_PORT_TX_SLOTS, sizeof(uint8_t));
    s_work = xQueueCreate(BAT_PORT_RX_SLOTS + BAT_PORT_TX_SLOTS + 1, sizeof(struct bat_port_ev));
    s_render_lock = xSemaphoreCreateBinary();
    s_render_done = xSemaphoreCreateBinary();
    if (s_netif == NULL || !s_bat || !s_rx_slot || !s_tx_slot || !s_render_buf || !s_links || !s_free_rx ||
        !s_free_tx || !s_work || !s_render_lock || !s_render_done) {
        bat_port_free_all_();
        s_reason = BAT_MODE_NOMEM;
        ESP_LOGE(TAG, "batman: out of memory, not started");
        return ESP_ERR_NO_MEM;
    }
    for (uint8_t i = 0; i < BAT_PORT_RX_SLOTS; i++) {
        (void)xQueueSend(s_free_rx, &i, 0);
    }
    for (uint8_t i = 0; i < BAT_PORT_TX_SLOTS; i++) {
        (void)xQueueSend(s_free_tx, &i, 0);
    }
    (void)xSemaphoreGive(s_render_lock);

    struct bat_config cfg;
    bat_config_defaults(&cfg);
    bat_mode_port_config(&cfg, s_hard, factory, warthog_cfg_get_mesh_battp(), g_warthog_mesh_grp != 0,
                         BAT_PORT_COPIES);
    memcpy(s_soft, cfg.soft_addr, 6);
    s_copies = cfg.bcast_copies;
    s_tput_override = cfg.tput_override;
    cfg.seq_keep = &s_seq_keep;
    const struct bat_ops ops = {
        .tx = bat_port_tx,
        .deliver = bat_port_deliver,
        .now_ms = bat_port_now_ms,
        .rand32 = bat_port_rand32,
        .link_tput = bat_port_link_tput,
    };
    const int seq = bat_init(s_bat, &cfg, &ops, NULL);
    if (seq < 0) {
        bat_port_free_all_();
        s_reason = BAT_MODE_INIT_FAIL;
        ESP_LOGE(TAG, "batman: engine rejected its configuration (mesh MAC " MACSTR "), not started",
                 MAC2STR(s_hard));
        return ESP_FAIL;
    }
    if (xTaskCreate(bat_port_task, "warthog_bat", BAT_PORT_TASK_STACK, NULL, BAT_PORT_TASK_PRIO,
                    &s_task) != pdPASS) {
        s_task = NULL;
        bat_port_free_all_();
        s_reason = BAT_MODE_NOMEM;
        ESP_LOGE(TAG, "batman: no memory for the engine task, not started");
        return ESP_ERR_NO_MEM;
    }

    if (esp_netif_set_mac(s_netif, s_soft) != ESP_OK) {
        bat_port_free_all_();
        s_reason = BAT_MODE_INIT_FAIL;
        ESP_LOGE(TAG, "batman: soft MAC not applied, not started");
        return ESP_FAIL;
    }
    if (esp_netif_tcpip_exec(bat_port_netif_setup_, s_netif) != ESP_OK) {
        (void)esp_netif_set_mac(s_netif, s_hard);
        bat_port_free_all_();
        s_reason = BAT_MODE_INIT_FAIL;
        ESP_LOGE(TAG, "batman: soft interface not taken over, not started");
        return ESP_FAIL;
    }
    /* Replaces halow_rx for this boot; nothing may register an RX callback after it. */
    (void)mmwlan_register_rx_pkt_ext_cb(MMWLAN_VIF_UNSPECIFIED, bat_port_rx_ext, NULL);

    g_warthog_mesh_batman = 1;
#ifdef WARTHOG_MESH_HOST_CCMP
    g_warthog_host_ccmp_on = 1; /* peers' ELP/OGM/BCAST are group frames */
#endif
    s_running = true;
    xTaskNotifyGive(s_task);
    ESP_LOGW(TAG, "batman: BATMAN_V member, orig " MACSTR " soft " MACSTR " bcast=%s x%u tput=%s seq=%s",
             MAC2STR(s_hard), MAC2STR(s_soft), g_warthog_mesh_grp ? "std" : "replicate",
             (unsigned)s_copies, s_tput_override ? "override" : "rate-control", seq ? "carried" : "random");
    return ESP_OK;
}

void warthog_bat_port_mesh_up(void)
{
    s_mesh_up = true;
}

void warthog_bat_port_mesh_failed(void)
{
    if (s_running) {
        s_reason = BAT_MODE_MESH_FAILED; /* the engine idles: every tx is BAT_TX_NOPEER */
    }
}

bool warthog_bat_port_running(void)
{
    return s_running && s_reason == BAT_MODE_OK;
}

int warthog_bat_port_reason(void)
{
    if (!s_started) {
        /* Start runs only on the way to mmwlan_mesh_enable(). */
        return warthog_cfg_get_mesh_batman() ? BAT_MODE_MESH_OFF : BAT_MODE_OFF;
    }
    return s_reason;
}

unsigned warthog_bat_port_routes(void)
{
    return warthog_bat_port_running() ? s_routes : 0;
}

unsigned warthog_bat_port_neighs(void)
{
    return warthog_bat_port_running() ? s_neighs : 0;
}

void warthog_bat_port_watch(const uint8_t mac[6])
{
    portENTER_CRITICAL(&s_q_mux);
    if (mac == NULL ? s_q.watching : (!s_q.watching || memcmp(s_q.watch, mac, 6) != 0)) {
        s_q.watching = mac != NULL;
        memset(s_q.watch, 0, 6);
        if (mac != NULL) {
            memcpy(s_q.watch, mac, 6);
        }
        s_q.gen++;
    }
    portEXIT_CRITICAL(&s_q_mux);
}

int warthog_bat_port_watch_answer(const uint8_t mac[6], struct bat_client_route *r)
{
    int a = -1;
    portENTER_CRITICAL(&s_q_mux);
    if (warthog_bat_port_running() && s_q.watching && s_q.ans_gen == s_q.gen && mac != NULL &&
        memcmp(s_q.ans_mac, mac, 6) == 0) {
        a = s_q.ans;
        if (r != NULL) {
            *r = s_q.route;
        }
    }
    portEXIT_CRITICAL(&s_q_mux);
    return a;
}

uint8_t warthog_bat_port_gw(struct bat_gw *out)
{
    portENTER_CRITICAL(&s_q_mux);
    const uint8_t n = warthog_bat_port_running() ? s_q.gws : 0;
    if (out != NULL) {
        *out = s_q.gw;
    }
    portEXIT_CRITICAL(&s_q_mux);
    return n;
}

void warthog_bat_port_soft_mac(uint8_t out[6])
{
    memcpy(out, s_soft, 6);
}

void warthog_bat_port_hard_mac(uint8_t out[6])
{
    memcpy(out, s_hard, 6);
}

uint8_t warthog_bat_port_bcast_copies(void)
{
    return s_copies;
}

uint32_t warthog_bat_port_tput_override(void)
{
    return s_tput_override;
}

bool warthog_bat_port_sae_build(void)
{
    return WARTHOG_MESH_SAE != 0;
}

bool warthog_bat_port_host_ccmp_build(void)
{
#ifdef WARTHOG_MESH_HOST_CCMP
    return true;
#else
    return false;
#endif
}

int warthog_bat_port_render(enum bat_render_kind k, const uint8_t *mac, uint32_t *cursor, const char **out)
{
    if (!warthog_bat_port_running() || out == NULL || cursor == NULL) {
        return WARTHOG_BAT_RENDER_NOT_RUNNING;
    }
    if (xSemaphoreTake(s_render_lock, pdMS_TO_TICKS(BAT_PORT_RENDER_MS)) != pdTRUE) {
        return WARTHOG_BAT_RENDER_BUSY;
    }
    (void)xSemaphoreTake(s_render_done, 0);
    s_render_cursor = *cursor;
    s_render_has_mac = mac != NULL;
    if (mac != NULL) {
        memcpy(s_render_mac, mac, 6);
    }
    portENTER_CRITICAL(&s_render_mux);
    s_render_state = RENDER_PENDING;
    portEXIT_CRITICAL(&s_render_mux);
    struct bat_port_ev ev = { .kind = EV_RENDER, .slot = (uint8_t)k, .len = 0 };
    if (xQueueSend(s_work, &ev, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_render_mux);
        s_render_state = RENDER_IDLE;
        portEXIT_CRITICAL(&s_render_mux);
        (void)xSemaphoreGive(s_render_lock);
        return WARTHOG_BAT_RENDER_BUSY;
    }
    if (xSemaphoreTake(s_render_done, pdMS_TO_TICKS(BAT_PORT_RENDER_MS)) != pdTRUE) {
        portENTER_CRITICAL(&s_render_mux);
        bool abandoned = (s_render_state == RENDER_PENDING);
        if (abandoned) {
            s_render_state = RENDER_ABANDONED; /* the engine task releases the lock */
        }
        portEXIT_CRITICAL(&s_render_mux);
        if (abandoned) {
            return WARTHOG_BAT_RENDER_BUSY;
        }
        (void)xSemaphoreTake(s_render_done, portMAX_DELAY); /* finished just now: given next */
    }
    *cursor = s_render_cursor;
    *out = s_render_buf;
    return WARTHOG_BAT_RENDER_OK;
}

void warthog_bat_port_render_done(void)
{
    portENTER_CRITICAL(&s_render_mux);
    s_render_state = RENDER_IDLE;
    portEXIT_CRITICAL(&s_render_mux);
    (void)xSemaphoreGive(s_render_lock);
}
