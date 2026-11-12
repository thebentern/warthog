/*
 * L2 bridge between the tethered surfaces and the mesh.
 *
 * NAT mode (the default) gives every warthog the same 192.168.4.1 /
 * 192.168.5.1, so CoT and mDNS a tethered host originates carry addresses that
 * alias the receiver's own subnet. Bridge mode puts USB, the Wi-Fi AP
 * and the mesh netif on one lwIP bridge; a host's frames leave the mesh with
 * the host's MAC in Address Extension and come back the same way, and its
 * address comes from whatever DHCP server the mesh has (an OpenMANET node whose
 * mesh interface is bridged), so two hosts on opposite sides are distinct.
 * Neither mode crosses bat0: a wizard-configured OpenMANET node's apps sit
 * behind batman-adv, which this firmware does not speak.
 *
 * Built in one go from app_main, on a running mesh only: the lwIP bridge netif,
 * every port, and then the ports' own layer 3 (DHCP servers, addresses) going,
 * all in one tcpip-thread run. Anything short of that leaves NAT mode as it was
 * and says why (AT+MESHBRIDGE?). IDF's own bridge glue is not used: it attaches
 * ports only from esp_eth and Wi-Fi events, which the USB and mesh netifs never
 * post and the AP has already posted.
 *
 * Needs CONFIG_ESP_NETIF_BRIDGE_EN. Without it the node says so and stays in
 * NAT mode rather than pretending.
 */
#include "mesh_bridge.h"
#include "mesh.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_net_stack.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "sdkconfig.h"

#ifndef WARTHOG_MESH_SAE
#define WARTHOG_MESH_SAE 0
#endif

#if CONFIG_ESP_NETIF_BRIDGE_EN && CONFIG_LWIP_L2_TO_L3_COPY
/* lwIP answers a ping in the request's own pbuf when it has headroom (a copied one does), and
 * its bridge never sends a pbuf out of the port it came in on: the reply would not leave. */
#error "bridge mode needs CONFIG_LWIP_L2_TO_L3_COPY off"
#endif

static const char *TAG = "warthog.bridge";

#define BR_PORTS_MAX  3
#define BR_AP_WAIT_MS 2000  /* esp_wifi_start() to the AP netif in lwIP: IDF adds it from its event task */
#define BR_HEAP_MIN   4096u /* largest free block wanted: lwIP takes about 1.2 KB for the bridge, and IDF
                             * aborts if it cannot have it */
/* Internal RAM, the word AT+HEAP? reads as "internal": what lwIP's malloc draws on. The PSRAM heap
 * (main/ext_ram.c) has neither capability. */
#define BR_HEAP_CAPS  (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

/* Defined in main/at.c. The gate is cached at mesh enable by main/mesh.c and read by the mesh datapath. */
extern volatile uint32_t g_warthog_mesh_bridge;
extern volatile uint32_t g_warthog_host_ccmp_on, g_warthog_chipvif_type, g_warthog_peer_gtk_mode;

static esp_netif_t *s_br;  /* written last, by app_main: the bridge is whole */
static bool s_failed;
static bool s_mesh_up;     /* written by the mesh-probe task */
static const char *s_reason = "off";
static const char *s_ports = "none";

bool warthog_mesh_bridge_active(void) { return s_br != NULL; }
esp_netif_t *warthog_mesh_bridge_netif(void) { return s_br; }
bool warthog_mesh_bridge_failed(void) { return s_failed; }

bool warthog_mesh_bridge_pending(void)
{
    return g_warthog_mesh_bridge && s_br == NULL && !s_failed;
}

/* This node can open other nodes' standard group frames, which is every broadcast a Linux peer sends:
 * always on an open mesh; under SAE with host CCMP armed (AT+SWCCMP), or with each peer's MGTK in the
 * chip (AT+GTKSTAT? per_sta=on: a chip-key build whose mesh runs on a MESH chip interface). */
static bool br_grp_rx_(void)
{
#if !WARTHOG_MESH_SAE
    return true;
#else
#ifdef WARTHOG_MESH_HOST_CCMP
    if (g_warthog_host_ccmp_on) {
        return true;
    }
#endif
#if !defined(WARTHOG_MESH_AMPE_NO_CHIP_KEY) && defined(WARTHOG_MESH_CHIP_VIF_MESH) && WARTHOG_MESH_CHIP_VIF_MESH
    if (g_warthog_chipvif_type == 5u && g_warthog_peer_gtk_mode != 0u) { /* 5: the chip's MESH interface type */
        return true;
    }
#endif
    return false;
#endif
}

int warthog_mesh_bridge_status(char *buf, size_t len)
{
    if (len == 0) {
        return 0;
    }
    const int w = snprintf(buf, len, "running=%u reason=%s ports=%s mesh_link=%u grp_rx=%u heap_free=%u heap_min=%u",
                           s_br != NULL ? 1u : 0u, s_reason, s_ports, s_mesh_up ? 1u : 0u,
                           br_grp_rx_() ? 1u : 0u, (unsigned)heap_caps_get_free_size(BR_HEAP_CAPS),
                           (unsigned)heap_caps_get_minimum_free_size(BR_HEAP_CAPS));
    /* snprintf returns the length it wanted; the caller appends at what is really there. */
    return w < 0 ? 0 : ((size_t)w >= len ? (int)len - 1 : w);
}

void warthog_mesh_bridge_none(void)
{
    if (warthog_mesh_bridge_pending()) {
        /* The gate was cached for a bridge at mesh enable and app_main builds none (the stored setting
         * read differently the second time, or batman runs): the leaf datapath, and the mesh netif
         * stops waiting. */
        g_warthog_mesh_bridge = 0;
        s_failed = true;
        s_reason = "not-started";
        ESP_LOGE(TAG, "mesh armed for AT+MESHBRIDGE=1 but no bridge is started this boot; NAT mode stands");
    }
}

#if CONFIG_ESP_NETIF_BRIDGE_EN
static esp_netif_t *s_mesh; /* the bridge's mesh port; written by app_main before s_br */

struct br_build {
    esp_netif_t *br;
    esp_netif_t *port[BR_PORTS_MAX]; /* the mesh netif first, then the tethered ones */
    int n;
    bool kept; /* out: a tethered port kept its address or its DHCP server */
};

/* tcpip context: lwIP has @p lw in its list (netif_add ran and nothing removed it). */
static bool br_listed_(const struct netif *lw)
{
    const struct netif *n;
    NETIF_FOREACH(n) {
        if (n == lw) {
            return true;
        }
    }
    return false;
}

/* tcpip context: lwIP's bridge will take it as a port, and no bridge has redirected its input yet. */
static bool br_port_ok_(esp_netif_t *port)
{
    const struct netif *lw = esp_netif_get_netif_impl(port);
    return lw != NULL && br_listed_(lw) && lw->input == tcpip_input && lw->linkoutput != NULL &&
           (lw->flags & NETIF_FLAG_ETHARP) != 0 && (lw->flags & NETIF_FLAG_ETHERNET) != 0;
}

/* tcpip context, one run of it: no frame is handled between the bridge entering lwIP and the last port
 * giving up its address. Nothing is logged here: this is lwIP's own 3 KB stack. ESP_FAIL: lwIP holds
 * the bridge and it must stay; any other error: lwIP never saw it. */
static esp_err_t br_attach_(void *ctx)
{
    struct br_build *b = ctx;
    for (int i = 0; i < b->n; i++) {
        if (!br_port_ok_(b->port[i])) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    /* IDF aborts when lwIP cannot allocate the bridge (ESP_ERROR_CHECK in esp_netif's start), so the
     * heap is asked here, on the thread that allocates next. */
    if (heap_caps_get_largest_free_block(BR_HEAP_CAPS) < BR_HEAP_MIN) {
        return ESP_ERR_NO_MEM;
    }
    /* netif_add with lwIP's bridge init: in lwIP's list, down, no ports. A bridge netif needs no driver. */
    esp_netif_action_start(b->br, NULL, 0, NULL);
    const struct netif *brlw = esp_netif_get_netif_impl(b->br);
    if (brlw == NULL || !br_listed_(brlw) || brlw->linkoutput == NULL) {
        return ESP_FAIL;
    }
    /* The name the mesh netif gives the mesh's DHCP server; lwIP's bridge would say "lwip". */
    (void)esp_netif_set_hostname(b->br, CONFIG_LWIP_LOCAL_HOSTNAME);
    for (int i = 0; i < b->n; i++) {
#ifdef WARTHOG_BRIDGE_TEST_FAIL /* bench build only: the last port is refused, to run the undo on real lwIP */
        const bool refuse = i == b->n - 1;
#else
        const bool refuse = false;
#endif
        if (refuse || esp_netif_bridge_add_port(b->br, b->port[i]) != ESP_OK) {
            /* lwIP cannot give a port back: undo what joining did to the ones before it. */
            while (i-- > 0) {
                struct netif *lw = esp_netif_get_netif_impl(b->port[i]);
                lw->input = tcpip_input;
                netif_set_flags(lw, NETIF_FLAG_ETHARP);
            }
            return ESP_FAIL;
        }
    }
    /* Only now is the bridge whole; the ports give up their own layer 3 last, so every failure
     * above leaves NAT mode exactly as it was. A port keeps no address: lwIP would route its
     * subnet out of the port itself, where the bridge has taken ARP away. */
    static const esp_netif_ip_info_t no_ip = { 0 };
    for (int i = 1; i < b->n; i++) {
        const esp_err_t stop = esp_netif_dhcps_stop(b->port[i]);
        if ((stop != ESP_OK && stop != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) ||
            esp_netif_set_ip_info(b->port[i], &no_ip) != ESP_OK) {
            b->kept = true; /* reported, not undone: bound to the port, its server hears nothing now */
        }
    }
    /* The mesh port asks for no lease of its own, whatever calls it connected. */
    (void)esp_netif_dhcpc_stop(b->port[0]);
    /* By name, as nat.c names the HaLow netif: IDF's own choice goes by route_prio among netifs
     * that are up, and the mesh port (100) outranks the bridge once it is raised. */
    (void)esp_netif_set_default_netif(b->br);
    return ESP_OK;
}

/* tcpip context. Raw lwIP on purpose: esp_netif_action_connected() would start the port's own
 * DHCP client and offer the port (route_prio 100) to IDF as the default route. */
static esp_err_t br_mesh_up_(void *ctx)
{
    struct netif *lw = esp_netif_get_netif_impl(ctx);
    if (lw == NULL) {
        return ESP_FAIL;
    }
    netif_set_up(lw);      /* wlanif_input drops what a down netif receives */
    netif_set_link_up(lw); /* the bridge sends only to a port whose link is up */
    return ESP_OK;
}
#endif

void warthog_mesh_bridge_port_up(void)
{
#if CONFIG_ESP_NETIF_BRIDGE_EN
    if (s_br == NULL || s_mesh_up) {
        return;
    }
    s_mesh_up = esp_netif_tcpip_exec(br_mesh_up_, s_mesh) == ESP_OK;
#endif
}

static esp_err_t warthog_mesh_bridge_start_(esp_netif_t *mesh_netif, esp_netif_t *ap_netif, const uint8_t mac[6])
{
#if CONFIG_ESP_NETIF_BRIDGE_EN
    static const uint8_t no_mac[6] = { 0 };
    if (s_br != NULL) {
        return ESP_OK;
    }
    /* The bridge is addressed through the mesh. Without one running (a station image with AT+MESHEN=0,
     * a mesh that did not start) it would only take the tethered side's DHCP servers and NAT away. */
    if (!warthog_mesh_running() || mesh_netif == NULL || mac == NULL || memcmp(mac, no_mac, sizeof(no_mac)) == 0) {
        s_reason = "mesh-off";
        return ESP_ERR_INVALID_STATE;
    }
    /* The mesh came up as a leaf (the setting read 0 at mesh enable): a host's frames would leave with
     * no Address Extension and no reply could find it, and the mesh netif may be up already. */
    if (!g_warthog_mesh_bridge) {
        s_reason = "not-armed";
        return ESP_ERR_INVALID_STATE;
    }
#ifdef WARTHOG_BRIDGE_NO_AP /* bench build only: USB and the mesh; the AP keeps its DHCP server and has no route */
    ap_netif = NULL;
#endif
    esp_netif_t *usb = esp_netif_get_handle_from_ifkey("USB");
    /* esp_wifi_start() has returned, so the AP netif is coming: IDF adds it to lwIP on WIFI_EVENT_AP_START. */
    for (int ms = 0; ap_netif != NULL && !esp_netif_is_netif_up(ap_netif) && ms < BR_AP_WAIT_MS; ms += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (ap_netif != NULL && !esp_netif_is_netif_up(ap_netif)) {
        s_reason = "ap-not-up"; /* bridging without it would leave its clients a DHCP server and no NAT */
        return ESP_ERR_TIMEOUT;
    }
    struct br_build b = { .n = 0 };
    b.port[b.n++] = mesh_netif;
    if (usb != NULL) {
        b.port[b.n++] = usb;
    }
    if (ap_netif != NULL) {
        b.port[b.n++] = ap_netif;
    }
    if (b.n < 2) {
        s_reason = "no-port";
        return ESP_ERR_NOT_FOUND;
    }

    bridgeif_config_t bcfg = { .max_fdb_dyn_entries = 64, .max_fdb_sta_entries = 8, .max_ports = BR_PORTS_MAX };
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_BR();
    base.if_key = "BR";
    base.if_desc = "warthog_br";
    base.bridge_info = &bcfg;
    base.route_prio = 60;
    /* The mesh netif's events: halow.c logs the address and sets the LED on them. */
    base.get_ip_event = IP_EVENT_STA_GOT_IP;
    base.lost_ip_event = IP_EVENT_STA_LOST_IP;
    esp_netif_config_t cfg = { .base = &base, .driver = NULL, .stack = ESP_NETIF_NETSTACK_DEFAULT_BR };
    esp_netif_t *br = esp_netif_new(&cfg);
    if (br == NULL) {
        s_reason = "nomem";
        return ESP_ERR_NO_MEM;
    }
    /* The bridge's own MAC is the mesh MAC, so the warthog's own IP traffic
     * leaves with SA == mesh SA and needs no Address Extension. Before the
     * start: the lwIP bridge takes its address at init. */
    uint8_t mac_rw[6];
    memcpy(mac_rw, mac, sizeof(mac_rw)); /* the API takes a non-const array */
    (void)esp_netif_set_mac(br, mac_rw);
    b.br = br;
    s_mesh = mesh_netif; /* before s_br: the probe task reads it once s_br is set */
    const esp_err_t e = esp_netif_tcpip_exec(br_attach_, &b);
    if (e != ESP_OK) {
        if (e != ESP_FAIL) {
            esp_netif_destroy(br); /* lwIP never had it */
        }
        /* ESP_FAIL: lwIP has it, and a port may have joined before one was refused. lwIP cannot detach a
         * port or free a bridge, so it stays: down, no address, no port's input, never published. */
        s_reason = e == ESP_ERR_NO_MEM ? "nomem" : "port-refused";
        return e;
    }
    s_ports = usb != NULL ? (ap_netif != NULL ? "usb,ap,mesh" : "usb,mesh") : "ap,mesh";
    s_reason = b.kept ? "ok(port-addr)" : "ok";
    s_br = br; /* last: mesh_addr_netif_poll_ and the status commands take it from here */
    ESP_LOGW(TAG, "L2 bridge up: ports %s -- NAT and the multicast repeater are off", s_ports);
    if (b.kept) {
        ESP_LOGE(TAG, "a bridged port kept its address or its DHCP server (AT+STATUS?)");
    }
    if (!br_grp_rx_()) {
#ifdef WARTHOG_MESH_HOST_CCMP
        ESP_LOGE(TAG, "host CCMP is off: tethered hosts hear no broadcast from a Linux peer until AT+SWCCMP=1");
#else
        ESP_LOGE(TAG, "this node cannot open other nodes' group frames: tethered hosts hear no broadcast "
                      "from a Linux peer (AT+MESHBRIDGE? grp_rx=0)");
#endif
    }
    return ESP_OK;
#else
    (void)mesh_netif; (void)ap_netif; (void)mac;
    s_reason = "not-built";
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t warthog_mesh_bridge_start(esp_netif_t *mesh_netif, esp_netif_t *ap_netif, const uint8_t mac[6])
{
    esp_err_t e = warthog_mesh_bridge_start_(mesh_netif, ap_netif, mac);
    if (e != ESP_OK) {
        /* NAT mode as with the setting at 0: the mesh datapath's bridge shape (Address Extension
         * replicas, path requests) goes too, then mesh_addr_netif_poll_ is released to the mesh netif. */
        g_warthog_mesh_bridge = 0;
        s_failed = true;
        ESP_LOGE(TAG, "AT+MESHBRIDGE=1 not started (%s); NAT mode stands", s_reason);
    }
    return e;
}
