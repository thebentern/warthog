#include "nat.h"
#include "mesh_bridge.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/lwip_napt.h"
#include "lwip/netif.h"

static const char *TAG = "warthog.nat";

static bool s_napt_logged = false;
/* Track HaLow STA IP so we can detect re-association with a new IP — when that
 * happens, every existing NAPT table entry is keyed on the old upstream IP and
 * silently stops translating new traffic correctly. Currently we only log it;
 * a future fix would flush the NAPT table on change. */
static uint32_t s_last_halow_ip = 0;

/* lwIP callocs its two NAPT tables inside the first enable of any netif and asserts when either is
 * NULL (ip4_napt.c:221-224; both boards reset there on 2026-11-10), so that enable waits for one free
 * block that holds both with 4 KB to spare. IP_NAPT_MAX and IP_PORTMAP_MAX are lwip/lwip_napt.h's
 * own (512 and 32 in ESP-IDF 5.5.6): nothing in this build defines either, so lwIP and this file read
 * the same two. 32 and 16 are the sizes of its table entries, struct ip_napt_entry and struct
 * ip_portmap_entry, which are private to ip4_napt.c (lines 67-98; measured there, and
 * test_lwip_napt_frag fails when they change). */
#define NAT_NAPT_BYTES ((size_t)IP_NAPT_MAX * 32u + (size_t)IP_PORTMAP_MAX * 16u)
#define NAT_NAPT_ROOM  (NAT_NAPT_BYTES + 4096u)
/* lwIP holds its tables: set on the tcpip thread by the enable that took them and never cleared, as
 * nothing here turns NAPT off (netif_add clears a netif's flag without freeing them). AT+HEAP? reads
 * it, and the 2 s passes on which the tables were not taken for want of that block. */
static volatile uint32_t s_napt_on, s_napt_nomem;

static esp_netif_t *get_netif(const char *key)
{
    return esp_netif_get_handle_from_ifkey(key);
}

/* ESP-IDF's lwIP NAPT is *inverted* from the conventional "WAN-facing NAT"
 * model: ip_napt_forward() in ip4_napt.c keys on inp->napt (the *input*
 * netif) and rewrites source to outp->ip_addr. So NAPT goes on the inside
 * netifs (USB, Wi-Fi AP), NOT on the outside (HaLow STA). We bypass
 * esp_netif_napt_enable()'s single-netif exclusivity check via the lwIP
 * direct API so both downstream surfaces get NAT. Full diagnosis +
 * tcpdump evidence in docs/napt-notes.md. */

struct nat_napt_io {
    esp_netif_t *usb, *ap;
    int usb_napt, ap_napt;
    bool nomem; /* the tables are not taken and there is no free block of NAT_NAPT_ROOM: nothing enabled */
    bool took;  /* this call's enable took them */
};

/* tcpip context: the first ip_napt_enable_netif arms lwIP's NAPT timer (sys_timeout). */
static esp_err_t nat_napt_on_(void *ctx)
{
    struct nat_napt_io *io = ctx;
    struct netif *usb_lwip = io->usb ? (struct netif *)esp_netif_get_netif_impl(io->usb) : NULL;
    struct netif *ap_lwip = io->ap ? (struct netif *)esp_netif_get_netif_impl(io->ap) : NULL;
    /* Without the tables the next enable allocates them: only with room, never into lwIP's assert.
     * calloc is MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL here; the size returned is one it can have. */
    const bool have = s_napt_on != 0 || (usb_lwip && usb_lwip->napt) || (ap_lwip && ap_lwip->napt);
    io->nomem = !have &&
                heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL) < NAT_NAPT_ROOM;
    if (!io->nomem) {
        if (usb_lwip) {
            ip_napt_enable_netif(usb_lwip, 1);
        }
        if (ap_lwip) {
            ip_napt_enable_netif(ap_lwip, 1);
        }
    }
    io->usb_napt = usb_lwip ? usb_lwip->napt : -1;
    io->ap_napt = ap_lwip ? ap_lwip->napt : -1;
    io->took = !have && (io->usb_napt == 1 || io->ap_napt == 1);
    if (io->took) {
        s_napt_on = 1;
    }
    return ESP_OK;
}

static void enforce_state(void)
{
    if (warthog_mesh_bridge_active()) {
        return; /* one L2 segment; there is nothing to translate */
    }
    esp_netif_t *halow = get_netif("WIFI_STA_DEF");
    if (!halow) {
        return;
    }

    /* From boot, not from the uplink's first address: lwIP's tables are taken while the heap has room.
     * Until that address exists nothing is translated: no route leaves by a netif at 0.0.0.0. */
    struct nat_napt_io io = { .usb = get_netif("USB"), .ap = get_netif("WIFI_AP_DEF") };
    (void)esp_netif_tcpip_exec(nat_napt_on_, &io);
    if (io.nomem) {
        if (s_napt_nomem++ == 0) {
            ESP_LOGE(TAG, "NAPT off: no free block of %u B for lwIP's tables; asking again every 2 s (AT+HEAP?)",
                     (unsigned)NAT_NAPT_ROOM);
        }
    } else if (io.took) {
        ESP_LOGI(TAG, "NAPT tables taken: %u sessions, %u B", (unsigned)IP_NAPT_MAX, (unsigned)NAT_NAPT_BYTES);
    }

    esp_netif_ip_info_t halow_ip = {0};
    esp_netif_get_ip_info(halow, &halow_ip);
    if (halow_ip.ip.addr == 0) {
        if (s_napt_logged) {
            ESP_LOGW(TAG, "HaLow lease lost");
            s_napt_logged = false;
            s_last_halow_ip = 0;
        }
        return;
    }

    /* HaLow IP changed under us — typically a STA re-association after a
     * brownout / beacon-miss / upstream lease cycle. Existing NAPT entries
     * still reference the old IP and won't translate new packets correctly;
     * iPads/iPhones interpret this as "no internet" inside ~2 min because
     * their iOS captive-network re-probe to apple.com fails. Log it loudly
     * so we can correlate with downstream client failures. */
    if (s_last_halow_ip != 0 && s_last_halow_ip != halow_ip.ip.addr) {
        esp_ip4_addr_t prev = { .addr = s_last_halow_ip };
        ESP_LOGW(TAG, "HaLow STA IP changed: " IPSTR " -> " IPSTR
                      " (NAPT table now stale; downstream clients will see drops)",
                 IP2STR(&prev), IP2STR(&halow_ip.ip));
    }
    s_last_halow_ip = halow_ip.ip.addr;

    esp_netif_set_default_netif(halow);

    if (!s_napt_logged) {
        ESP_LOGI(TAG, "NAPT on inside netifs (usb=%d ap=%d); HaLow default route: ip=" IPSTR " gw=" IPSTR,
                 io.usb_napt, io.ap_napt, IP2STR(&halow_ip.ip), IP2STR(&halow_ip.gw));
        s_napt_logged = true;
    }
}

static void nat_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "NAPT supervisor up");
    uint32_t tick = 0;
    while (1) {
        enforce_state();
#if WARTHOG_DEBUG
        /* Periodic state line for debugging — enable with -DWARTHOG_DEBUG=1. */
        if (tick % 15 == 0) {
            esp_netif_t *halow = get_netif("WIFI_STA_DEF");
            esp_netif_t *usb = get_netif("USB");
            esp_netif_t *ap = get_netif("WIFI_AP_DEF");
            struct netif *usb_lwip = usb ? (struct netif *)esp_netif_get_netif_impl(usb) : NULL;
            struct netif *ap_lwip = ap ? (struct netif *)esp_netif_get_netif_impl(ap) : NULL;
            esp_netif_ip_info_t hip = {0};
            if (halow) {
                esp_netif_get_ip_info(halow, &hip);
            }
            esp_netif_t *cur_default = esp_netif_get_default_netif();
            const char *cur_key = cur_default ? esp_netif_get_ifkey(cur_default) : "(none)";
            if (!cur_key) {
                cur_key = "(?)";
            }
            ESP_LOGI(TAG, "tick: halow_ip=" IPSTR " usb.napt=%d ap.napt=%d default=%s",
                     IP2STR(&hip.ip),
                     usb_lwip ? usb_lwip->napt : -1,
                     ap_lwip ? ap_lwip->napt : -1,
                     cur_key);
        }
#endif
        tick++;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

esp_err_t warthog_nat_start(void)
{
    BaseType_t ok = xTaskCreate(nat_task, "warthog_nat", 3072, NULL,
                                tskIDLE_PRIORITY + 2, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

void warthog_nat_napt_stat(struct warthog_napt_stat *out)
{
    out->entries = IP_NAPT_MAX;
    out->bytes = (uint32_t)NAT_NAPT_BYTES;
    out->on = s_napt_on;
    out->refused = s_napt_nomem;
}
