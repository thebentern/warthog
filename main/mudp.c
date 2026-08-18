/*
 * mudp -- UDP multicast repeater between warthog's netifs.
 *
 * Meshtastic's UDP transport is a multicast to 239.0.0.69:4403. lwIP does
 * not forward multicast between netifs (NAPT is unicast-only), so a
 * Meshtastic node on the Wi-Fi AP or USB side would never reach the HaLow
 * mesh, and vice versa. This is the bridge: one socket, joined to the group
 * on every netif that exists, and every datagram received on one netif is
 * re-sent to the group on each of the others.
 *
 * Which netif a datagram arrived on is classified by source subnet -- the
 * three netifs sit on disjoint /24 // /16 ranges (USB 192.168.4/24, AP
 * 192.168.5/24, HaLow 10.77/16). Datagrams whose source is one of our own
 * addresses are our own repeats and are dropped, which is what prevents a
 * loop; the netif that originated a datagram never receives it back from us
 * because it is excluded from the re-send set.
 *
 * The HaLow netif only appears on the first mesh peering, so membership on
 * it is joined lazily from the same 2 s poll nat.c uses.
 *
 * Counters over AT+MUDP?. This is the observability for step S3/S4 of the
 * transport bring-up: a Meshtastic node's packets show up here as
 * rx_<netif>++ / tx_<other>++ before anything else can see them.
 */

#include "mudp.h"

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
enum { NIF_USB = 0, NIF_AP, NIF_HALOW, NIF_COUNT };
static const char *const k_ifkey[NIF_COUNT] = { "USB", "WIFI_AP_DEF", "WIFI_STA_DEF" };

static struct {
    uint32_t ip;      /* host order; 0 = netif not up / not joined */
    uint32_t netmask; /* host order */
} s_nif[NIF_COUNT];

static int s_sock = -1;
volatile uint32_t g_mudp_rx[NIF_COUNT], g_mudp_tx[NIF_COUNT];
volatile uint32_t g_mudp_drop_self, g_mudp_drop_unknown, g_mudp_tx_err;
/* Last datagram relayed FROM the HaLow mesh, for AT+MUDPLAST? -- lets a host
 * on the USB/AP side verify payload integrity (e.g. decode a MeshPacket)
 * without needing to win macOS multicast routing across three same-subnet
 * ECM links. */
static uint8_t s_last_halow[256]; static uint16_t s_last_halow_len; static uint32_t s_last_halow_src;

/* Join the group on a netif and record its address. Idempotent. */
static void mudp_join_(int slot)
{
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey(k_ifkey[slot]);
    esp_netif_ip_info_t ip = { 0 };
    if (nif == NULL || esp_netif_get_ip_info(nif, &ip) != ESP_OK || ip.ip.addr == 0) {
        return;
    }
    if (s_nif[slot].ip == ntohl(ip.ip.addr)) {
        return; /* already joined on this address */
    }
    struct ip_mreq m = { .imr_multiaddr.s_addr = inet_addr(MUDP_GROUP),
                         .imr_interface.s_addr = ip.ip.addr };
    if (setsockopt(s_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) < 0) {
        ESP_LOGW(TAG, "join on %s (" IPSTR ") failed errno=%d", k_ifkey[slot], IP2STR(&ip.ip), errno);
        return;
    }
    s_nif[slot].ip = ntohl(ip.ip.addr);
    s_nif[slot].netmask = ntohl(ip.netmask.addr);
    ESP_LOGI(TAG, "joined %s:%u on %s " IPSTR, MUDP_GROUP, MUDP_PORT, k_ifkey[slot], IP2STR(&ip.ip));
}

/* Which netif did a datagram from `src` arrive on? -1 if it is one of our own
 * addresses (our own repeat) or matches no netif. */
static int mudp_classify_(uint32_t src)
{
    for (int i = 0; i < NIF_COUNT; i++) {
        if (s_nif[i].ip != 0 && src == s_nif[i].ip) {
            return -2; /* ours */
        }
    }
    for (int i = 0; i < NIF_COUNT; i++) {
        if (s_nif[i].ip != 0 && (src & s_nif[i].netmask) == (s_nif[i].ip & s_nif[i].netmask)) {
            return i;
        }
    }
    return -1;
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

        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 0) {
            continue; /* SO_RCVTIMEO tick, or transient error */
        }

        int in = mudp_classify_(ntohl(from.sin_addr.s_addr));
        if (in == -2) {
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

        /* Re-send to the group on every OTHER netif that is up. Flipping
         * IP_MULTICAST_IF on one socket per send is race-free: this is the
         * only task that touches the socket. */
        struct sockaddr_in dst = { .sin_family = AF_INET, .sin_port = htons(MUDP_PORT),
                                   .sin_addr.s_addr = inet_addr(MUDP_GROUP) };
        for (int out = 0; out < NIF_COUNT; out++) {
            if (out == in || s_nif[out].ip == 0) {
                continue;
            }
            struct in_addr ifa = { .s_addr = htonl(s_nif[out].ip) };
            setsockopt(s_sock, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof(ifa));
            if (sendto(s_sock, buf, (size_t)n, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
                g_mudp_tx_err++;
            } else {
                g_mudp_tx[out]++;
            }
        }
    }
}

esp_err_t warthog_mudp_start(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        ESP_LOGE(TAG, "socket failed errno=%d", errno);
        return ESP_FAIL;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t ttl = 64;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, 1);
    /* No loopback: we must never receive our own repeats through lwIP. This
     * build compiles loopback out anyway; setting it is belt and braces. */
    uint8_t loop = 0;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, 1);

    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(MUDP_PORT),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        ESP_LOGE(TAG, "bind failed errno=%d", errno);
        close(s);
        return ESP_FAIL;
    }
    s_sock = s;

    BaseType_t ok = xTaskCreatePinnedToCore(mudp_task, "warthog_mudp", 4096, NULL,
                                            tskIDLE_PRIORITY + 2, NULL, 0);
    if (ok != pdPASS) {
        close(s);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }
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
    if (s_sock < 0 || data == NULL) {
        return -1;
    }
    struct sockaddr_in dst = { .sin_family = AF_INET, .sin_port = htons(MUDP_PORT),
                               .sin_addr.s_addr = inet_addr(MUDP_GROUP) };
    int sent = 0;
    for (int out = 0; out < NIF_COUNT; out++) {
        if (out == NIF_AP || s_nif[out].ip == 0) {
            continue;
        }
        struct in_addr ifa = { .s_addr = htonl(s_nif[out].ip) };
        setsockopt(s_sock, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof(ifa));
        if (sendto(s_sock, data, len, 0, (struct sockaddr *)&dst, sizeof(dst)) >= 0) {
            g_mudp_tx[out]++; sent++;
        } else {
            g_mudp_tx_err++;
        }
    }
    g_mudp_rx[NIF_AP]++;
    return sent;
}
