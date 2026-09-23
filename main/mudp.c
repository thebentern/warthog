/*
 * mudp -- UDP multicast repeater between warthog's netifs.
 *
 * Meshtastic's UDP transport is a multicast to 239.0.0.69:4403. lwIP does
 * not forward multicast between netifs (NAPT is unicast-only), so a
 * Meshtastic node on the Wi-Fi AP or USB side would never reach the HaLow
 * mesh, and vice versa. This is the bridge: one socket per netif, bound to it
 * with SO_BINDTODEVICE and joined to the group there, so lwIP hands a
 * datagram only to the socket of the netif it arrived on and sends only out of
 * the netif a socket is bound to. Every datagram received on one netif is
 * re-sent to the group on each of the others. Arrival is never inferred from
 * the source address, which cannot tell a mesh sender from a local one.
 * Datagrams whose source is one of our own addresses are our own repeats and
 * are dropped; the arrival netif is excluded from the re-send set.
 *
 * The HaLow netif only appears on the first mesh peering, so membership on
 * it is joined lazily from the same 2 s poll nat.c uses.
 *
 * Counters over AT+MUDP?. This is the observability for step S3/S4 of the
 * transport bring-up: a Meshtastic node's packets show up here as
 * rx_<netif>++ / tx_<other>++ before anything else can see them.
 */

#include "mudp.h"
#include "mudp_classify.h"
#include "mesh_bridge.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <errno.h>
#include <string.h>

static const char *TAG = "warthog.mudp";

#define MUDP_GROUP "239.0.0.69"
#define MUDP_PORT 4403
#define MUDP_MAX_PKT 1500

/* Netif slots, in a fixed order so counters line up. */
enum { NIF_USB = MUDP_NIF_USB, NIF_AP = MUDP_NIF_AP, NIF_HALOW = MUDP_NIF_HALOW,
       NIF_COUNT = MUDP_NIF_COUNT };
static const char *const k_ifkey[NIF_COUNT] = { "USB", "WIFI_AP_DEF", "WIFI_STA_DEF" };

static struct mudp_nif s_nif[NIF_COUNT];

static int s_sock[NIF_COUNT] = { -1, -1, -1 };
static bool s_started;
volatile uint32_t g_mudp_rx[NIF_COUNT], g_mudp_tx[NIF_COUNT];
volatile uint32_t g_mudp_drop_self, g_mudp_drop_unknown, g_mudp_tx_err;
/* Last datagram relayed FROM the HaLow mesh, for AT+MUDPLAST? -- lets a host
 * on the USB/AP side verify payload integrity (e.g. decode a MeshPacket)
 * without needing to win macOS multicast routing across three same-subnet
 * ECM links. */
static uint8_t s_last_halow[256]; static uint16_t s_last_halow_len; static uint32_t s_last_halow_src;

/* A UDP socket on MUDP_PORT that only receives from, and only sends out of, @p nif. */
static int mudp_open_(esp_netif_t *nif)
{
    struct ifreq ifr = { 0 };
    if (esp_netif_get_netif_impl_name(nif, ifr.ifr_name) != ESP_OK) {
        return -1;
    }
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    uint8_t ttl = 64;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, 1);
    /* No loopback: we must never receive our own repeats through lwIP. */
    uint8_t loop = 0;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, 1);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(MUDP_PORT),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) < 0 ||
        bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        ESP_LOGW(TAG, "socket on %s failed errno=%d", ifr.ifr_name, errno);
        close(s);
        return -1;
    }
    return s;
}

/* Open the netif's socket, join the group on it and record its address. Idempotent. */
static void mudp_join_(int slot)
{
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey(k_ifkey[slot]);
    esp_netif_ip_info_t ip = { 0 };
    /* A netif lwIP never added (a softAP that failed to start) still reports its
     * stored address, and its impl name would resolve to loopback. */
    if (nif == NULL || !esp_netif_is_netif_up(nif) || esp_netif_get_ip_info(nif, &ip) != ESP_OK ||
        ip.ip.addr == 0) {
        return;
    }
    if (s_nif[slot].ip == ntohl(ip.ip.addr)) {
        return; /* already joined on this address */
    }
    if (s_sock[slot] < 0 && (s_sock[slot] = mudp_open_(nif)) < 0) {
        return;
    }
    /* lwIP registers a membership before it joins and never releases it on a failed
     * join, from a table shared by every socket: release the old address's on a change,
     * and a failed join's own entry. */
    struct ip_mreq m = { .imr_multiaddr.s_addr = inet_addr(MUDP_GROUP) };
    if (s_nif[slot].ip != 0) {
        m.imr_interface.s_addr = htonl(s_nif[slot].ip);
        (void)setsockopt(s_sock[slot], IPPROTO_IP, IP_DROP_MEMBERSHIP, &m, sizeof(m));
        s_nif[slot].ip = 0;
    }
    m.imr_interface.s_addr = ip.ip.addr;
    if (setsockopt(s_sock[slot], IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) < 0) {
        int e = errno;
        if (e == EADDRNOTAVAIL) {
            (void)setsockopt(s_sock[slot], IPPROTO_IP, IP_DROP_MEMBERSHIP, &m, sizeof(m));
        }
        ESP_LOGW(TAG, "join on %s (" IPSTR ") failed errno=%d", k_ifkey[slot], IP2STR(&ip.ip), e);
        return;
    }
    s_nif[slot].ip = ntohl(ip.ip.addr);
    s_nif[slot].netmask = ntohl(ip.netmask.addr);
    ESP_LOGI(TAG, "joined %s:%u on %s " IPSTR, MUDP_GROUP, MUDP_PORT, k_ifkey[slot], IP2STR(&ip.ip));
}

/* Send to the group on every joined netif except @p skip. Returns how many took it. */
static int mudp_send_others_(int skip, const uint8_t *data, size_t len)
{
    struct sockaddr_in dst = { .sin_family = AF_INET, .sin_port = htons(MUDP_PORT),
                               .sin_addr.s_addr = inet_addr(MUDP_GROUP) };
    int sent = 0;
    for (int out = 0; out < NIF_COUNT; out++) {
        if (out == skip || s_sock[out] < 0 || s_nif[out].ip == 0) {
            continue;
        }
        if (sendto(s_sock[out], data, len, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
            g_mudp_tx_err++;
        } else {
            g_mudp_tx[out]++;
            sent++;
        }
    }
    return sent;
}

static void mudp_task(void *arg)
{
    (void)arg;
    static uint8_t buf[MUDP_MAX_PKT];
    TickType_t last_join = 0;

    for (;;) {
        /* Lazy joins: the HaLow netif appears on first peering; USB/AP may
         * come up after us too. Re-check every 2 s, like nat.c. */
        if (xTaskGetTickCount() - last_join > pdMS_TO_TICKS(2000)) {
            for (int i = 0; i < NIF_COUNT; i++) {
                mudp_join_(i);
            }
            last_join = xTaskGetTickCount();
        }

        fd_set rd;
        FD_ZERO(&rd);
        int maxfd = -1;
        for (int i = 0; i < NIF_COUNT; i++) {
            if (s_sock[i] >= 0) {
                FD_SET(s_sock[i], &rd);
                maxfd = s_sock[i] > maxfd ? s_sock[i] : maxfd;
            }
        }
        if (maxfd < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        struct timeval tv = { .tv_sec = 1 };
        int ready = select(maxfd + 1, &rd, NULL, NULL, &tv);
        if (ready < 0) {
            vTaskDelay(pdMS_TO_TICKS(100)); /* select allocates; never spin on its failure */
        }
        if (ready <= 0) {
            continue; /* 1 s tick for the lazy joins */
        }

        for (int slot = 0; slot < NIF_COUNT; slot++) {
            if (s_sock[slot] < 0 || !FD_ISSET(s_sock[slot], &rd)) {
                continue;
            }
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int n = recvfrom(s_sock[slot], buf, sizeof(buf), MSG_DONTWAIT,
                             (struct sockaddr *)&from, &fl);
            if (n < 0) {
                continue;
            }
            int in = mudp_classify(ntohl(from.sin_addr.s_addr), slot, s_nif);
            if (in == MUDP_FROM_SELF) {
                g_mudp_drop_self++;
                continue;
            }
            if (in < 0) {
                g_mudp_drop_unknown++;
                continue;
            }
            g_mudp_rx[in]++;
            if (in == NIF_HALOW) {
                uint16_t k = n < (int)sizeof(s_last_halow) ? (uint16_t)n : (uint16_t)sizeof(s_last_halow);
                memcpy(s_last_halow, buf, k); s_last_halow_len = k; s_last_halow_src = ntohl(from.sin_addr.s_addr);
            }
            (void)mudp_send_others_(in, buf, (size_t)n);
        }
    }
}

esp_err_t warthog_mudp_start(void)
{
    if (warthog_mesh_bridge_active()) {
        ESP_LOGI(TAG, "bridge mode: L2 multicast crosses the bridge; repeater not started");
        return ESP_OK;
    }
    /* Sockets open per netif, from the task's lazy joins. */
    BaseType_t ok = xTaskCreatePinnedToCore(mudp_task, "warthog_mudp", 4096, NULL,
                                            tskIDLE_PRIORITY + 2, NULL, 0);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "multicast repeater up on :%u", MUDP_PORT);
    return ESP_OK;
}

int warthog_mudp_status(char *buf, size_t len)
{
    return snprintf(buf, len,
                    "+MUDP: usb=" IPSTR " ap=" IPSTR " halow=" IPSTR
                    " | rx usb=%lu ap=%lu halow=%lu | tx usb=%lu ap=%lu halow=%lu"
                    " | drop_self=%lu drop_unknown=%lu tx_err=%lu\r\n",
                    IP2STR((esp_ip4_addr_t *)&(uint32_t){ htonl(s_nif[NIF_USB].ip) }),
                    IP2STR((esp_ip4_addr_t *)&(uint32_t){ htonl(s_nif[NIF_AP].ip) }),
                    IP2STR((esp_ip4_addr_t *)&(uint32_t){ htonl(s_nif[NIF_HALOW].ip) }),
                    (unsigned long)g_mudp_rx[NIF_USB], (unsigned long)g_mudp_rx[NIF_AP],
                    (unsigned long)g_mudp_rx[NIF_HALOW], (unsigned long)g_mudp_tx[NIF_USB],
                    (unsigned long)g_mudp_tx[NIF_AP], (unsigned long)g_mudp_tx[NIF_HALOW],
                    (unsigned long)g_mudp_drop_self, (unsigned long)g_mudp_drop_unknown,
                    (unsigned long)g_mudp_tx_err);
}

int warthog_mudp_last_halow(char *buf, size_t len)
{
    int w = snprintf(buf, len, "+MUDPLAST: src=%u.%u.%u.%u len=%u hex=",
                     (unsigned)(s_last_halow_src >> 24) & 0xff, (unsigned)(s_last_halow_src >> 16) & 0xff,
                     (unsigned)(s_last_halow_src >> 8) & 0xff, (unsigned)s_last_halow_src & 0xff,
                     (unsigned)s_last_halow_len);
    for (uint16_t i = 0; i < s_last_halow_len && w < (int)len - 4; i++) {
        w += snprintf(buf + w, len - w, "%02x", s_last_halow[i]);
    }
    w += snprintf(buf + w, len - w, "\r\n");
    return w;
}

/* Inject a datagram into the repeater as if it had arrived on the AP netif --
 * i.e. exactly what a Meshtastic node attached to warthog's softAP produces.
 * It is sent to the group on every OTHER netif (HaLow included). Used by
 * AT+MINJECT to drive the transport end-to-end from one warthog's console,
 * which is the product topology and sidesteps the Mac's ambiguous routing
 * across several identical ECM subnets. */
int warthog_mudp_inject_from_ap(const uint8_t *data, size_t len)
{
    if (!s_started || data == NULL) {
        return -1;
    }
    g_mudp_rx[NIF_AP]++;
    return mudp_send_others_(NIF_AP, data, len);
}
