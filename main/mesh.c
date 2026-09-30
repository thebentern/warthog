#include "mesh.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mmwlan.h"
#include "mmwlan_mesh.h"
#include "region.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/prot/dhcp.h"
#include "mmhalow.h"
#include "cfg.h"
#include "mesh_diag.h"
#include "mesh_bridge.h"
#include "bat_mode.h"
#include "bat_port.h"

/* 802.11s security. Default OFF, which matches only a peer set to encryption='none':
 * OpenMANET's mesh wizard writes SAE. The warthog-mesh-sae env turns it on. The
 * passphrase must be identical on every node in the mesh. */
#ifndef WARTHOG_MESH_SAE
#define WARTHOG_MESH_SAE 0
#endif
#ifndef WARTHOG_MESH_PASSPHRASE
#define WARTHOG_MESH_PASSPHRASE "warthog-mesh"
#endif

static const char *TAG = "warthog.mesh";

/* periodic probe-request burst.
 *
 * Step 29 confirmed the chip TX path works in mesh mode (one-shot probe req
 * triggered mmdrv_tx_frame#1 + mmdrv_host_process_tx_status#1). But peer
 * boards never RX anything from each other.
 *
 * This task fires a broadcast probe request every PROBE_BURST_PERIOD_MS so
 * we have a continuous TX signal on-air. Two diagnostic outcomes:
 *
 *   - If RX still doesn't fire on either side: confirms chip RX path is
 *     fundamentally closed for foreign-BSSID frames in mesh mode (the
 *     chip is filtering Linux's mesh peer beacons / probes at the firmware
 *     level we can't reach). This becomes a hard support-ticket question.
 *
 *   - If RX DOES fire (mmdrv_host_process_rx_frame#N increments on either
 *     board): the chip CAN receive, and we just need to drive TX from the
 *     host (Linux's "mesh_beaconless_mode" pattern). Then we wire a proper
 *     periodic probe + peer-discovery state machine.
 */
#define MESH_PROBE_BURST_PERIOD_MS  2000
#define MESH_PROBE_BURST_TASK_STACK 4096
/* Below the umac evtloop / driver / SPI-IRQ tasks, which all run at 4
 * (MMOSAL_TASK_PRI_HIGH). This task reaches build_mgmt_frame()/mmdrv_tx_frame()
 * -- umac internals -- so at 5 it could preempt the evtloop mid-handle_mpm(),
 * which mutates the same MPM link-id state and builds frames of its own. */
#define MESH_PROBE_BURST_TASK_PRIO  3

/* Peering watchdog.
 *
 * A node whose channel, mesh ID, operating class or security does not match
 * the rest of the mesh beacons happily and alone, forever, and says nothing.
 * That is the failure an operator actually hits, so name it: the bus-level
 * beacon count splits the two causes apart. Zero beacons means nothing is
 * audible -- wrong channel or bandwidth, or out of range. Beacons but no
 * peers means we hear the mesh and refuse to join it, which is the mesh ID,
 * the operating class or the security mode. */
#define MESH_PEER_GRACE_TICKS  10   /* ~20 s before the first complaint */
#define MESH_PEER_NAG_TICKS    30   /* ~60 s between repeats */

static char s_mesh_id_active[WARTHOG_CFG_MESH_ID_MAXLEN + 1];
static uint16_t s_mesh_beacon_tu;

/* Rolled or reset by the watchdog (probe task), read by AT+MESHCFG? (AT task). */
static struct warthog_mesh_diag_window s_diag_win;
static portMUX_TYPE s_diag_mux = portMUX_INITIALIZER_UNLOCKED;

enum diag_win_op { DIAG_WIN_FILL, DIAG_WIN_ROLL, DIAG_WIN_RESET };

/* One window operation on the totals as they stand, both under the lock. */
static void diag_win_(enum diag_win_op op, struct warthog_mesh_diag_in *in)
{
    extern volatile uint32_t g_warthog_prq_named, g_warthog_mesh_rssi_skip, g_warthog_mesh_rssi_pass;
    portENTER_CRITICAL(&s_diag_mux);
    const struct warthog_mesh_diag_counts now = {
        .mesh_probes  = g_warthog_prq_named,
        .floor_skips  = g_warthog_mesh_rssi_skip,
        .floor_passes = g_warthog_mesh_rssi_pass,
    };
    if (op == DIAG_WIN_FILL) {
        warthog_mesh_diag_window_fill(&s_diag_win, &now, in);
    } else if (op == DIAG_WIN_ROLL) {
        warthog_mesh_diag_window_roll(&s_diag_win, &now);
    } else {
        warthog_mesh_diag_window_reset(&s_diag_win, &now);
    }
    portEXIT_CRITICAL(&s_diag_mux);
}

void warthog_mesh_diag_windowed(struct warthog_mesh_diag_in *in)
{
    diag_win_(DIAG_WIN_FILL, in);
}

static void mesh_report_unpeered(unsigned int peers)
{
    extern volatile uint32_t g_warthog_rxchan_beacon;
    extern int g_warthog_chan_pin_status;
    extern uint32_t g_warthog_applied_freq_hz;
    extern uint16_t g_warthog_applied_chan;
    extern int16_t  g_warthog_applied_gclass, g_warthog_applied_sclass;
    extern uint8_t  g_warthog_applied_bw_mhz;

    struct warthog_mesh_diag_in in = {
        .peers           = peers,
        .chan_pin_status = g_warthog_chan_pin_status,
        .applied_chan    = g_warthog_applied_chan,
        .beacons_heard   = g_warthog_rxchan_beacon,
    };
    warthog_mesh_diag_windowed(&in);
    enum warthog_mesh_diag d = warthog_mesh_diagnose(&in);

    ESP_LOGE(TAG, "NOT PEERED. Every value below must match the rest of the mesh:");
    ESP_LOGE(TAG, "  mesh id   '%s'", s_mesh_id_active);
    if (g_warthog_applied_chan == 0) {
        ESP_LOGE(TAG, "  channel   NOT PINNED (country %s)", WARTHOG_COUNTRY_CODE);
    } else {
        ESP_LOGE(TAG, "  channel   %u @ %lu Hz, %u MHz BW, class %d/%d, country %s",
                 (unsigned)g_warthog_applied_chan, (unsigned long)g_warthog_applied_freq_hz,
                 (unsigned)g_warthog_applied_bw_mhz, (int)g_warthog_applied_gclass,
                 (int)g_warthog_applied_sclass, WARTHOG_COUNTRY_CODE);
    }
    ESP_LOGE(TAG, "  beacon    %u TU", (unsigned)s_mesh_beacon_tu);
    ESP_LOGE(TAG, "  security  %s", WARTHOG_MESH_SAE ? "SAE/AMPE" : "open");
    ESP_LOGE(TAG, "  heard     %lu beacons", (unsigned long)g_warthog_rxchan_beacon);
    ESP_LOGE(TAG, "  -> %s.", warthog_mesh_diag_text(d));
    ESP_LOGE(TAG, "  AT+MESHCFG? reports the same over the console.");
}

/* Bring the HaLow netif up over the mesh link.
 *
 * In STA mode this happens through mmhalow_link_state() when the chip reports
 * MMWLAN_LINK_UP; mesh has no such event, so it is done here on the first
 * ESTAB. DHCP first when AT+MESHDHCP=1 (the default); otherwise, or with no
 * lease, a static 10.77.0.0/16 address with the host part taken from the low
 * two octets of the HaLow MAC, so two boards never collide. That is enough for
 * IP, ARP, and UDP multicast -- which is what a Meshtastic UDP transport needs. */
extern volatile uint32_t g_warthog_mpm_estab;
extern volatile unsigned int g_warthog_hostap_estab;
/* Long enough for a bridged peer's dnsmasq to answer, short enough not to
 * stall a warthog-only mesh noticeably. */
#define MESH_DHCP_WAIT_MS 6000
static bool s_mesh_netif_up;
static void mesh_netif_up_(void)
{
    /* In bridge mode the bridge is the L3 interface; the mesh netif is a port.
     * A peer can reach ESTAB before app_main has built the bridge, so wait
     * while the start is still to come (the probe task calls this every 2 s);
     * once it has failed the mesh netif takes the address itself, NAT mode. */
    if (warthog_mesh_bridge_pending()) {
        return;
    }
    esp_netif_t *netif = warthog_mesh_bridge_active() ? warthog_mesh_bridge_netif()
                                                      : mmhalow_get_netif();
    if (netif == NULL || s_mesh_netif_up) {
        return;
    }
    uint8_t mac[6];
    mmwlan_get_mac_addr(mac);
    /* Claim the slot before the DHCP wait below, so a second peer reaching
     * ESTAB mid-wait does not start a parallel bring-up. */
    s_mesh_netif_up = true;

    /* A bridged OpenMANET node (mesh iface in br-lan) serves DHCP on the bridge;
     * a lease joins its LAN. A wizard node's iface is a bat0 port and gives none
     * unless batman mode runs (mesh_bat_netif_poll_). Else the static address below. */
    if (warthog_cfg_get_mesh_dhcp()) {
        esp_netif_action_connected(netif, NULL, 0, NULL);
        if (esp_netif_dhcpc_start(netif) == ESP_OK) {
            for (int waited = 0; waited < MESH_DHCP_WAIT_MS; waited += 250) {
                vTaskDelay(pdMS_TO_TICKS(250));
                esp_netif_ip_info_t got;
                if (esp_netif_get_ip_info(netif, &got) == ESP_OK && got.ip.addr != 0) {
                    ESP_LOGI(TAG, "mesh: DHCP lease " IPSTR " — peer bridge reachable",
                             IP2STR(&got.ip));
                    return;
                }
            }
            ESP_LOGI(TAG, "mesh: no DHCP offer in %d ms (expected on a peerless or "
                          "unbridged mesh) — using the static address",
                     MESH_DHCP_WAIT_MS);
        }
    }

    esp_netif_dhcpc_stop(netif);
    esp_netif_ip_info_t ip = { 0 };
    IP4_ADDR(&ip.ip, 10, 77, mac[4], mac[5]);
    IP4_ADDR(&ip.netmask, 255, 255, 0, 0);
    IP4_ADDR(&ip.gw, 10, 77, mac[4], mac[5]);
    esp_netif_set_ip_info(netif, &ip);
    esp_netif_action_connected(netif, NULL, 0, NULL);
    ESP_LOGI(TAG, "mesh: netif up at " IPSTR, IP2STR(&ip.ip));
}

/* Batman mode: bat0 comes up on the first originator with a route; bat_mode_bat0_step decides
 * the rest. lwIP's DHCP client runs beside a held address, which only a lease replaces. */
#define MESH_BAT_ROUTER_ARP_TICKS 5 /* an unknown or unresolved router MAC is ARPed every 5th tick */
static struct bat_mode_bat0 s_bat0;      /* probe task only */
static esp_netif_ip_info_t s_bat_held;   /* probe task only: the address an attempt keeps */
static int64_t s_bat_poll_us;            /* probe task only */
static struct {                          /* probe task only: the lease's router */
    uint32_t ip;
    uint8_t  mac[6];
    bool     known, unresolved;
    uint8_t  asked;
} s_bat_rtr;
static portMUX_TYPE s_bat_view_mux = portMUX_INITIALIZER_UNLOCKED;
static struct bat_mode_bat0 s_bat0_seen; /* AT+MESHBATMAN?'s copy, under s_bat_view_mux */
static struct bat_mode_bat0_view s_bat_view;

enum { MESH_BAT_IO_READ, MESH_BAT_IO_PROBE, MESH_BAT_IO_START, MESH_BAT_IO_STOP, MESH_BAT_IO_HOLD };
struct mesh_bat_io {
    esp_netif_t *netif;
    uint8_t op;
    ip4_addr_t cand;          /* READ with look: answered an ARP probe? PROBE: probe it */
    bool look, ask_router;    /* READ */
    esp_netif_ip_info_t hold; /* HOLD */
    esp_netif_ip_info_t now;  /* READ results from here on */
    bool running, binding, leased, taken, router_known;
    uint8_t router_mac[6];
};

/* tcpip context: every lwIP call bat0 addressing makes. */
static esp_err_t mesh_bat_io_(void *ctx)
{
    struct mesh_bat_io *io = ctx;
    struct netif *lw = esp_netif_get_netif_impl(io->netif);
    if (lw == NULL) {
        return ESP_FAIL;
    }
    struct eth_addr *eth;
    const ip4_addr_t *ip;
    switch (io->op) {
    case MESH_BAT_IO_PROBE: /* RFC 5227: bat0 has no address, so the sender IP is 0 */
        (void)etharp_query(lw, &io->cand, NULL);
        return ESP_OK;
    case MESH_BAT_IO_START: /* dhcp_start leaves the address alone until a lease binds */
        return dhcp_start(lw) == ERR_OK ? ESP_OK : ESP_FAIL;
    case MESH_BAT_IO_STOP: /* nothing is bound, so lwIP keeps the address; esp_netif agrees */
        dhcp_release_and_stop(lw);
        (void)esp_netif_dhcpc_stop(io->netif);
        return ESP_OK;
    case MESH_BAT_IO_HOLD: { /* decided here: lwIP may have bound, or begun ARP-checking an offer, since READ */
        const struct dhcp *d = netif_dhcp_data(lw);
        if (dhcp_supplied_address(lw) || !ip4_addr_isany_val(*netif_ip4_addr(lw)) ||
            (d != NULL && d->state == DHCP_STATE_CHECKING)) { /* an address change would suspend that check */
            return ESP_ERR_INVALID_STATE;
        }
        netif_set_addr(lw, (const ip4_addr_t *)&io->hold.ip, (const ip4_addr_t *)&io->hold.netmask,
                       (const ip4_addr_t *)&io->hold.gw);
        return ESP_OK;
    }
    default:
        break;
    }
    io->now.ip.addr = ip4_addr_get_u32(netif_ip4_addr(lw));
    io->now.netmask.addr = ip4_addr_get_u32(netif_ip4_netmask(lw));
    io->now.gw.addr = ip4_addr_get_u32(netif_ip4_gw(lw));
    const struct dhcp *d = netif_dhcp_data(lw);
    io->running = d != NULL && d->state != DHCP_STATE_OFF;
    io->binding = d != NULL && (d->state == DHCP_STATE_REQUESTING || d->state == DHCP_STATE_CHECKING);
    io->leased = dhcp_supplied_address(lw) != 0;
    if (io->look) {
        io->taken = etharp_find_addr(lw, &io->cand, &eth, &ip) >= 0;
    }
    if (io->now.gw.addr != 0) {
        if (etharp_find_addr(lw, netif_ip4_gw(lw), &eth, &ip) >= 0) {
            memcpy(io->router_mac, eth->addr, 6);
            io->router_known = true;
        }
        if (io->ask_router) { /* always a broadcast request; the reply puts its MAC back in TT */
            (void)etharp_query(lw, netif_ip4_gw(lw), NULL);
        }
    }
    return ESP_OK;
}

static void mesh_bat_candidate_(unsigned attempt, esp_netif_ip_info_t *ip)
{
    uint8_t soft[6], a[4];
    warthog_bat_port_soft_mac(soft);
    bat_mode_static_ip(soft, attempt, a);
    IP4_ADDR(&ip->ip, a[0], a[1], a[2], a[3]);
    IP4_ADDR(&ip->netmask, 255, 255, 0, 0);
    bat_mode_static_gw(a);
    IP4_ADDR(&ip->gw, a[0], a[1], a[2], a[3]);
}

/* The held address with the static gateway: its lease's router, if it had one, is gone. */
static esp_netif_ip_info_t mesh_bat_held_(void)
{
    esp_netif_ip_info_t h = s_bat_held;
    uint8_t g[4];
    bat_mode_static_gw(g);
    IP4_ADDR(&h.gw, g[0], g[1], g[2], g[3]);
    return h;
}

/* The lease's router: its MAC from lwIP's ARP table, learned once per router address. */
static uint8_t mesh_bat_router_(const struct mesh_bat_io *io, bool leased)
{
    if (io->now.gw.addr != s_bat_rtr.ip) {
        memset(&s_bat_rtr, 0, sizeof(s_bat_rtr));
        s_bat_rtr.ip = io->now.gw.addr;
    }
    if (io->router_known) {
        memcpy(s_bat_rtr.mac, io->router_mac, 6);
        s_bat_rtr.known = true;
    }
    if (!leased || !s_bat_rtr.known) {
        s_bat_rtr.unresolved = false;
        warthog_bat_port_watch(NULL);
        return leased ? bat_mode_router_state(s_bat_rtr.ip != 0, false, -1, 0) : BAT_MODE_ROUTER_NONE;
    }
    struct bat_client_route r = { 0 };
    warthog_bat_port_watch(s_bat_rtr.mac);
    const int answer = warthog_bat_port_watch_answer(s_bat_rtr.mac, &r);
    /* A router that sent nothing into batman for 600 s leaves TT (tt 4.2): ARP it back, next tick first. */
    if (answer == 0 && !s_bat_rtr.unresolved) {
        s_bat_rtr.asked = 0;
    }
    s_bat_rtr.unresolved = answer == 0;
    return bat_mode_router_state(s_bat_rtr.ip != 0, true, answer, r.ogm_age_ms);
}

static void mesh_bat_netif_poll_(void)
{
    esp_netif_t *netif = mmhalow_get_netif();
    if (netif == NULL) {
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    const uint32_t elapsed = s_bat_poll_us ? (uint32_t)((now_us - s_bat_poll_us) / 1000)
                                           : MESH_PROBE_BURST_PERIOD_MS;
    s_bat_poll_us = now_us;
    const bool leased = bat_mode_bat0_leased(&s_bat0);
    esp_netif_ip_info_t cand;
    mesh_bat_candidate_(s_bat0.attempt, &cand);
    struct mesh_bat_io io = { .netif = netif, .op = MESH_BAT_IO_READ, .look = bat_mode_bat0_probing(&s_bat0) };
    io.cand.addr = cand.ip.addr;
    io.ask_router = leased && (!s_bat_rtr.known || s_bat_rtr.unresolved) &&
                    s_bat_rtr.asked++ % MESH_BAT_ROUTER_ARP_TICKS == 0;
    if (esp_netif_tcpip_exec(mesh_bat_io_, &io) != ESP_OK) {
        return;
    }
    struct bat_gw gw;
    const uint8_t gws = warthog_bat_port_gw(&gw); /* as many as the engine reports: a second one is a rise too */
    const struct bat_mode_bat0_in in = {
        .elapsed_ms = elapsed, .routes = warthog_bat_port_routes(), .dhcp = warthog_cfg_get_mesh_dhcp() != 0,
        .dhcp_running = io.running, .leased = io.leased, .have_ip = io.now.ip.addr != 0, .taken = io.taken,
        .gws = gws, .binding = io.binding, .router = mesh_bat_router_(&io, leased),
    };
    const uint8_t before = s_bat0.attempt;
    const enum bat_mode_bat0_act act = bat_mode_bat0_step(&s_bat0, &in);
    switch (act) {
    case BAT0_START_DHCP:
        /* Started now, or left in INIT while bat0 is down; bringing bat0 up starts it then. */
        if (!io.running) {
            (void)esp_netif_dhcpc_start(netif);
        }
        esp_netif_action_connected(netif, NULL, 0, NULL); /* up; starts a client still in INIT */
        ESP_LOGI(TAG, "batman: first route; DHCP on bat0 for up to %d s", BAT_MODE_DHCP_WAIT_MS / 1000);
        break;
    case BAT0_LEASED:
        ESP_LOGI(TAG, "batman: DHCP lease " IPSTR " via " IPSTR " on bat0", IP2STR(&io.now.ip), IP2STR(&io.now.gw));
        break;
    case BAT0_PROBE: {
        struct mesh_bat_io op = { .netif = netif, .op = MESH_BAT_IO_STOP };
        (void)esp_netif_tcpip_exec(mesh_bat_io_, &op);
        const esp_netif_ip_info_t none = { 0 };
        (void)esp_netif_set_ip_info(netif, &none); /* an RFC 5227 probe goes out without an address */
        esp_netif_action_connected(netif, NULL, 0, NULL); /* up, with DHCP stopped */
        mesh_bat_candidate_(s_bat0.attempt, &cand);
        op.op = MESH_BAT_IO_PROBE;
        op.cand.addr = cand.ip.addr;
        (void)esp_netif_tcpip_exec(mesh_bat_io_, &op);
        if (s_bat0.ticks == 0) {
            ESP_LOGI(TAG, "batman: %s; ARP-probing " IPSTR, s_bat0.attempt != before ? "address in use"
                     : (in.dhcp ? "no DHCP lease" : "AT+MESHDHCP=0"), IP2STR(&cand.ip));
        }
        break;
    }
    case BAT0_STATIC:
        mesh_bat_candidate_(s_bat0.attempt, &s_bat_held);
        (void)esp_netif_set_ip_info(netif, &s_bat_held); /* DHCP was stopped by the probes */
        esp_netif_action_connected(netif, NULL, 0, NULL);
        ESP_LOGW(TAG, "batman: bat0 static at " IPSTR " via " IPSTR "%s", IP2STR(&s_bat_held.ip),
                 IP2STR(&s_bat_held.gw),
                 s_bat0.attempt >= BAT_MODE_STATIC_TRIES ? " (every probed candidate was in use)" : "");
        break;
    case BAT0_ROUTER_LOST:
    case BAT0_RETRY: {
        if (act == BAT0_ROUTER_LOST) {
            s_bat_held = io.now;
            ESP_LOGW(TAG, "batman: router " IPSTR " unreachable for %lu s; DHCP again, keeping " IPSTR,
                     IP2STR(&io.now.gw), (unsigned long)(s_bat0.lost_ms / 1000), IP2STR(&io.now.ip));
        } else {
            ESP_LOGI(TAG, "batman: DHCP again beside " IPSTR " (attempt %lu)", IP2STR(&s_bat_held.ip),
                     (unsigned long)s_bat0.retries);
        }
        struct mesh_bat_io op = { .netif = netif, .op = MESH_BAT_IO_START };
        if (esp_netif_tcpip_exec(mesh_bat_io_, &op) != ESP_OK) {
            ESP_LOGW(TAG, "batman: the DHCP client did not start"); /* the next tick ends the attempt */
        }
        break;
    }
    case BAT0_HOLD: {
        struct mesh_bat_io op = { .netif = netif, .op = MESH_BAT_IO_HOLD, .hold = mesh_bat_held_() };
        if (esp_netif_tcpip_exec(mesh_bat_io_, &op) == ESP_OK) {
            ESP_LOGW(TAG, "batman: a DHCP server took " IPSTR " back mid-attempt; re-applied", IP2STR(&op.hold.ip));
        }
        break;
    }
    case BAT0_RETRY_FAIL: {
        struct mesh_bat_io op = { .netif = netif, .op = MESH_BAT_IO_STOP };
        (void)esp_netif_tcpip_exec(mesh_bat_io_, &op);
        const esp_netif_ip_info_t h = mesh_bat_held_();
        (void)esp_netif_set_ip_info(netif, &h); /* the same address; the gateway becomes the static one */
        ESP_LOGW(TAG, "batman: no DHCP lease; keeping " IPSTR " via " IPSTR, IP2STR(&h.ip), IP2STR(&h.gw));
        break;
    }
    default:
        break;
    }
    esp_netif_ip_info_t shown = io.now;
    if (act != BAT0_NONE) {
        (void)esp_netif_get_ip_info(netif, &shown);
    }
    portENTER_CRITICAL(&s_bat_view_mux);
    s_bat0_seen = s_bat0;
    s_bat_view = (struct bat_mode_bat0_view){ .s = &s_bat0_seen, .router = in.router, .dhcp = in.dhcp,
                                              .gw = gws != 0, .gw_down = gw.down, .gw_up = gw.up };
    memcpy(s_bat_view.ip, &shown.ip.addr, 4); /* network order: the first octet first */
    memcpy(s_bat_view.router_ip, &shown.gw.addr, 4);
    memcpy(s_bat_view.gw_orig, gw.orig, 6);
    portEXIT_CRITICAL(&s_bat_view_mux);
}

int warthog_mesh_bat0_line(char *buf, size_t len)
{
    struct bat_mode_bat0 s;
    struct bat_mode_bat0_view v;
    portENTER_CRITICAL(&s_bat_view_mux);
    s = s_bat0_seen;
    v = s_bat_view;
    portEXIT_CRITICAL(&s_bat_view_mux);
    v.s = &s;
    return bat_mode_bat0_line(&v, buf, len);
}

static void mesh_probe_burst_task(void *arg)
{
    (void)arg;
    /* Wait a moment so we don't fire during the same TBTT as the initial
     * setup-time probe request. */
    vTaskDelay(pdMS_TO_TICKS(1500));

    uint32_t iter = 0;
    while (1) {
        iter++;
        int ret = mmwlan_mesh_tx_broadcast_probe();
        if (ret < 0) {
            ESP_LOGW(TAG, "probe_burst#%lu: TX failed ret=%d", (unsigned long)iter, ret);
        } else if (iter <= 4 || (iter % 30) == 0) {
            ESP_LOGW(TAG, "probe_burst#%lu: ret=%d", (unsigned long)iter, ret);
        }
        /* Either MPM counts: warthog's own (open mesh) or hostap's (SAE --
         * where warthog's MPM is deliberately disabled and this gate used to
         * keep the netif down forever, leaving a keyed link with no L3). */
        if (warthog_bat_port_running()) {
            mesh_bat_netif_poll_();
        } else if (g_warthog_mpm_estab || g_warthog_hostap_estab) {
            mesh_netif_up_();
        }

        /* Watchdog: complain once after the grace period, then periodically,
         * and say so once when a peer finally appears. */
        {
            static uint32_t unpeered = 0;
            static bool complained = false;
            if (mmwlan_mesh_get_peer_count() > 0) {
                if (complained) {
                    ESP_LOGW(TAG, "peered: %u peer(s) -- the mismatch reports above are resolved",
                             (unsigned)mmwlan_mesh_get_peer_count());
                    complained = false;
                }
                unpeered = 0;
                diag_win_(DIAG_WIN_RESET, NULL);
            } else if (++unpeered == MESH_PEER_GRACE_TICKS ||
                       (unpeered > MESH_PEER_GRACE_TICKS &&
                        ((unpeered - MESH_PEER_GRACE_TICKS) % MESH_PEER_NAG_TICKS) == 0)) {
                mesh_report_unpeered(0);
                diag_win_(DIAG_WIN_ROLL, NULL);
                complained = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(MESH_PROBE_BURST_PERIOD_MS));
    }
}

/* chip opcode probe.
 *
 * Send every opcode in [0x0000, 0x00FF] with empty payload + log responses.
 * Goal: discover undocumented opcodes that affect mesh RX behavior. Runs
 * ONCE at startup (after mesh enable), then exits — leaving the probe-burst
 * task to take over the steady-state mesh-discovery work.
 *
 * The chip's known returns for unknown opcodes:
 *   -32757 (MORSE_RET_CMD_NOT_HANDLED) — chip recognized opcode doesn't exist
 *   -1     (MORSE_RET_EPERM)            — wrong state / not permitted
 *   0                                    — accepted (success — investigate!)
 *   <other negative>                     — chip-specific error (also interesting)
 *
 * Any opcode that returns 0 in our current mesh state (we already configured
 * MESH_CONFIG, BSS_CONFIG, etc.) is potentially relevant. We log success +
 * specific non-CMD_NOT_HANDLED errors to keep noise low.
 */
static void mesh_opcode_probe_task(void *arg)
{
    (void)arg;
    /* Wait 4s after boot so all the normal init logs have flushed and we're
     * in steady state. */
    vTaskDelay(pdMS_TO_TICKS(4000));
    ESP_LOGW(TAG, "[step33] starting opcode probe (0x0000..0x00FF)");

    /* Skip opcodes we already use during init — sending them again may
     * crash the chip or reset state. */
    static const uint16_t skip_list[] = {
        0x0001, /* SET_CHANNEL */
        0x0004, /* ADD_INTERFACE */
        0x0005, /* REMOVE_INTERFACE — bad idea to retry */
        0x0006, /* BSS_CONFIG */
        0x000B, /* DISABLE_KEY — could disrupt anything */
        0x0011, /* SET_QOS_PARAMS */
        0x0025, /* GET_CAPABILITIES */
        0x0039, /* MESH_CONFIG */
        0x003D, /* BSS_BEACON_CONFIG */
        0x0052, /* BSSID_SET */
    };
    int n_success = 0, n_cmd_not_handled = 0, n_other = 0;
    for (uint16_t op = 0x0000; op <= 0x00FF; op++) {
        bool skip = false;
        for (size_t i = 0; i < sizeof(skip_list)/sizeof(skip_list[0]); i++) {
            if (op == skip_list[i]) { skip = true; break; }
        }
        if (skip) continue;

        int ret = mmwlan_mesh_probe_opcode(op);
        if (ret == 0) {
            ESP_LOGW(TAG, "[step33] opcode 0x%04x ACCEPTED (ret=0) — potential mesh-relevant cmd",
                     op);
            n_success++;
        } else if (ret == -32757) {
            n_cmd_not_handled++;
        } else {
            /* Non-zero non-CMD_NOT_HANDLED — chip parsed the opcode but
             * rejected it for some reason. Could be wrong-state or
             * invalid-args. Worth logging. */
            ESP_LOGW(TAG, "[step33] opcode 0x%04x rejected ret=%d (chip knows opcode but couldn't run)",
                     op, ret);
            n_other++;
        }
        /* Small delay between probes so we don't flood the chip queue. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGW(TAG, "[step33] probe done: %d accepted, %d unknown-to-chip, %d state-rejected",
             n_success, n_cmd_not_handled, n_other);
    vTaskDelete(NULL);
}


/* --- POSITIVE CONTROL: plain STA scan on the pinned channel ------------- */
static volatile bool s_scan_done;
static volatile int  s_scan_hits;

static void warthog_scan_rx_cb(const struct mmwlan_scan_result *r, void *arg)
{
    (void)arg;
    s_scan_hits++;
    ESP_LOGW(TAG,
             "SCAN HIT #%d rssi=%d freq=%luHz bw=%u ssid_len=%u "
             "bssid=%02x:%02x:%02x:%02x:%02x:%02x",
             s_scan_hits, (int)r->rssi, (unsigned long)r->channel_freq_hz,
             (unsigned)r->bw_mhz, (unsigned)r->ssid_len,
             r->bssid[0], r->bssid[1], r->bssid[2],
             r->bssid[3], r->bssid[4], r->bssid[5]);
}

static void warthog_scan_done_cb(enum mmwlan_scan_state st, void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "SCAN COMPLETE state=%d hits=%d", (int)st, s_scan_hits);
    s_scan_done = true;
}

static void warthog_mesh_scan_probe(void)
{
    struct mmwlan_scan_req req = MMWLAN_SCAN_REQ_INIT;
    req.scan_rx_cb = warthog_scan_rx_cb;
    req.scan_complete_cb = warthog_scan_done_cb;
    req.args.dwell_time_ms = 500;

    s_scan_done = false;
    s_scan_hits = 0;
    ESP_LOGW(TAG, "=== POSITIVE CONTROL: scanning pinned channel for ANY beacon ===");
    enum mmwlan_status st = mmwlan_scan_request(&req);
    ESP_LOGW(TAG, "mmwlan_scan_request -> %d (0=SUCCESS)", (int)st);
    if (st != MMWLAN_SUCCESS) {
        return;
    }
    for (int i = 0; i < 150 && !s_scan_done; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGW(TAG, "=== SCAN RESULT: %d beacon(s) heard on the pinned channel ===",
             s_scan_hits);
}

/* Repeating variant. A one-shot boot scan has to coincide with a live peer,
 * which couples the result to the peer's uptime -- and a peer that dies
 * mid-run produces a false negative that looks exactly like "RX is broken".
 * Scanning on a loop decouples the two: bring the peer up whenever, and the
 * next sweep sees it. Runs instead of the mesh VIF so the ordinary (known
 * good) STA receive path is what's under test. */
static void warthog_scan_loop_task(void *arg)
{
    (void)arg;
    uint32_t sweep = 0;
    while (1) {
        sweep++;
        struct mmwlan_scan_req req = MMWLAN_SCAN_REQ_INIT;
        req.scan_rx_cb = warthog_scan_rx_cb;
        req.scan_complete_cb = warthog_scan_done_cb;
        req.args.dwell_time_ms = 800;

        s_scan_done = false;
        s_scan_hits = 0;
        enum mmwlan_status st = mmwlan_scan_request(&req);
        if (st != MMWLAN_SUCCESS) {
            ESP_LOGW(TAG, "[scanloop#%lu] scan_request -> %d",
                     (unsigned long)sweep, (int)st);
        } else {
            for (int i = 0; i < 200 && !s_scan_done; i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            ESP_LOGW(TAG, "[scanloop#%lu] hits=%d", (unsigned long)sweep, s_scan_hits);
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void warthog_mesh_smoke_test(void)
{
    ESP_LOGW(TAG, "=== MESH SMOKE TEST — probing chip firmware mesh support ===");

    /* Restore the persisted data-plane setting before any peer is added --
     * umac_datapath_mesh_add_peer() reads it at ESTAB, so setting it later
     * would only affect peers that happen to arrive afterwards. */
    extern volatile uint32_t g_warthog_mesh_secure, g_warthog_mesh_fwd, g_warthog_mesh_bridge,
                             g_warthog_mesh_grp, g_warthog_mesh_pmf;
    g_warthog_mesh_secure = warthog_cfg_get_mesh_secure();
    g_warthog_mesh_fwd = warthog_cfg_get_mesh_fwd();
    g_warthog_mesh_bridge = warthog_cfg_get_mesh_bridge();
    g_warthog_mesh_grp = warthog_cfg_get_mesh_grp();
    /* Must precede the supplicant's mesh_config_create(), which reads it once. */
    g_warthog_mesh_pmf = warthog_cfg_get_mesh_pmf();
    /* Before the first beacon is heard; AT+MESHRSSI= also sets it live. */
    extern volatile int32_t g_warthog_mesh_rssi_floor;
    g_warthog_mesh_rssi_floor = warthog_cfg_get_mesh_rssi();
#if WARTHOG_MESH_SAE && !defined(WARTHOG_MESH_HOST_CCMP)
    if (g_warthog_mesh_grp) {
        ESP_LOGE(TAG, "mesh: standard group frames under SAE with chip crypto: sent under our "
                      "own MGTK, which a Linux peer decrypts but another warthog's chip cannot; "
                      "receiving any peer's group frames needs host CCMP (swccmp build).");
    }
#endif
    /* The gates are read by mmwlan_mesh_enable(), which initialises the
     * forwarding tables and the capability bit before the first beacon. */
    ESP_LOGW(TAG, "mesh: forwarding %s, bridge %s",
             g_warthog_mesh_fwd ? "ON" : "off", g_warthog_mesh_bridge ? "ON" : "off");
    ESP_LOGW(TAG, "mesh data plane: %s (persisted)",
             g_warthog_mesh_secure ? "keyed" : "open");

    /* Report the pre-boot channel pin here: mmhalow_init() runs before the USB
     * CDC console exists, so its own log line is invisible to a host that
     * attaches after boot. 0 = MMWLAN_SUCCESS. */
    extern int g_warthog_chan_pin_status;
    ESP_LOGW(TAG, "channel pin status = %d (0=SUCCESS, 3=UNAVAILABLE)",
             g_warthog_chan_pin_status);

    /* POSITIVE CONTROL for the receive path.
     *
     * Every mesh RX experiment so far has produced silence, but silence is
     * ambiguous: it cannot distinguish "the chip never delivers mesh frames"
     * from "nothing was on air" from "our receiver is misconfigured". A plain
     * scan uses the ordinary, known-good STA receive path on the same pinned
     * channel, so if ANY beacon is on air we will see it here. Run before the
     * mesh VIF is added, while the chip is still in its default state. */
#ifndef WARTHOG_SKIP_SCAN_PROBE
    /* The probe wedges on some chip firmware: the completion callback never
     * fires and the bounded wait never returns, so the VIF is never added. */
    warthog_mesh_scan_probe();
#else
    ESP_LOGW(TAG, "scan probe skipped (WARTHOG_SKIP_SCAN_PROBE)");
#endif

#ifdef WARTHOG_SCAN_ONLY
    /* Receiver-isolation build: keep sweeping and never enter mesh mode, so a
     * peer can be brought up at any time and still be caught. If this never
     * reports a hit while a peer is verifiably transmitting on the pinned
     * channel, the problem is upstream of anything mesh-specific. */
    ESP_LOGW(TAG, "WARTHOG_SCAN_ONLY: staying in scan loop, NOT enabling mesh");
    xTaskCreate(warthog_scan_loop_task, "scanloop", 4096, NULL, 5, NULL);
    return;
#endif

    /* NOTE: the single-channel pin lives in mmhalow_init() (components/halow/
     * mmhalow.c, guarded by WARTHOG_PIN_S1G_CHAN), NOT here.
     * mmwlan_set_channel_list() must run while the WLAN subsystem is inactive
     * and before mmwlan_boot(); calling it from this function returns
     * MMWLAN_UNAVAILABLE (3) and silently leaves the full regulatory list in
     * place, which is what made our operating channel unobservable. */

    /* mmhalow_init() has already run mmwlan_init(), set the channel list, and
     * booted the chip, so mmwlan_mesh_enable() can go straight to
     * ADD_INTERFACE(type=MESH). Open mesh (no SAE) keeps the probe minimal —
     * we are testing whether the firmware accepts the VIF type, not security. */
    /* Mesh ID and beacon interval are part of the profile a peer matches on,
     * so they belong with the channel settings rather than hardcoded here.
     * mesh_matches_local() compares the Mesh ID exactly, and a beacon interval
     * that differs from the peer's is a plausible sync failure -- Morse and
     * OpenMANET both use 1000 TU where the conventional default is 100.
     *
     * Parity with the hand-configured bench OpenMANET nodes (see platformio.ini;
     * the mesh wizard's default ID is 'openmanet'):
     *   -DWARTHOG_MESH_ID='"halowmesh"'  -DWARTHOG_MESH_BEACON_TU=1000
     */
#ifndef WARTHOG_MESH_ID
#define WARTHOG_MESH_ID "warthog-mesh-test"
#endif
#ifndef WARTHOG_MESH_BEACON_TU
#define WARTHOG_MESH_BEACON_TU 100
#endif
    struct mmwlan_mesh_args args = MMWLAN_MESH_ARGS_INIT;
    /* Runtime-settable (AT+MESHID), falling back to the build-time value. */
    char mesh_id[WARTHOG_CFG_MESH_ID_MAXLEN + 1] = {0};
    warthog_cfg_get_mesh_id(mesh_id, sizeof(mesh_id));
    size_t mesh_id_len = strlen(mesh_id);
    if (mesh_id_len == 0 || mesh_id_len > sizeof(args.mesh_id)) {
        ESP_LOGE(TAG, "mesh: mesh ID must be 1..%u chars, got %u -- not starting",
                 (unsigned)sizeof(args.mesh_id), (unsigned)mesh_id_len);
        return;
    }
    args.mesh_id_len = (uint8_t)mesh_id_len;
    memcpy(args.mesh_id, mesh_id, mesh_id_len);
    ESP_LOGW(TAG, "mesh: id '%s' (%u chars)", mesh_id, (unsigned)mesh_id_len);
    snprintf(s_mesh_id_active, sizeof(s_mesh_id_active), "%s", mesh_id);
    /* Mesh security.
     *
     * MMWLAN_SAE runs real 802.11s security: SAE (dragonfly) authentication
     * derives a PMK per peer, and AMPE carries the per-link MTK and the group
     * MGTK inside the peering handshake. That is what hostap's mesh_rsn.c
     * implements and what a mac80211 peer speaks, so it is the only mode that
     * can interoperate with a secured OpenMANET mesh.
     *
     * MMWLAN_OPEN leaves the mesh unauthenticated and unencrypted; it matches
     * only a peer set to encryption='none' (OpenMANET's mesh wizard writes SAE).
     *
     * The passphrase must be identical on every node, exactly as it must be
     * for any 802.11s SAE mesh. */
#if WARTHOG_MESH_SAE
    args.security_type = MMWLAN_SAE;
    {
        /* Runtime-settable (AT+MESHPASS), falling back to the build default. */
        char pw[WARTHOG_CFG_MESH_PASS_MAXLEN + 1] = {0};
        warthog_cfg_get_mesh_pass(pw, sizeof(pw));
        size_t pw_len = strlen(pw);
        if (pw_len == 0 || pw_len >= sizeof(args.passphrase)) {
            ESP_LOGE(TAG, "mesh: passphrase must be 1..%u chars, got %u -- not starting",
                     (unsigned)(sizeof(args.passphrase) - 1), (unsigned)pw_len);
            return;
        }
        memcpy(args.passphrase, pw, pw_len);
        args.passphrase_len = (uint8_t)pw_len;
    }
    ESP_LOGW(TAG, "mesh: SAE/AMPE enabled (passphrase %u chars)", args.passphrase_len);
#else
    args.security_type = MMWLAN_OPEN;
#endif
    args.beacon_interval_tu = WARTHOG_MESH_BEACON_TU;
    s_mesh_beacon_tu = args.beacon_interval_tu;

    /* Before the first peer can exist: the RX hook, the soft interface and the
     * datapath's batman gate all have to be in place for it. */
    (void)warthog_bat_port_start();

    enum mmwlan_status st = mmwlan_mesh_enable(&args);
    if (st == MMWLAN_SUCCESS) {
        warthog_bat_port_mesh_up();
    } else {
        warthog_bat_port_mesh_failed();
    }

    /* : mmwlan_mesh_enable() returns SUCCESS only when BOTH
     * ADD_INTERFACE(type=MESH) and MESH_CONFIG(START) were accepted by the
     * chip — see umac_mesh_enable_mesh(). The per-step "mesh: ..." lines
     * above (visible via the per-file MMLOG_LEVEL_OVRD in umac_mesh.c)
     * show exactly how far it got. */
    if (st == MMWLAN_SUCCESS) {
        ESP_LOGW(TAG, "RESULT: PASS — mesh VIF added AND MESH_CONFIG(START) accepted.");
        ESP_LOGW(TAG, "        The chip is beaconing as an 802.11s mesh STA.");
        ESP_LOGW(TAG, "        done; next is (PLINK peering).");

        /* start the periodic probe-request burst task. */
        BaseType_t tret = xTaskCreate(mesh_probe_burst_task,
                                       "mesh-probe",
                                       MESH_PROBE_BURST_TASK_STACK,
                                       NULL,
                                       MESH_PROBE_BURST_TASK_PRIO,
                                       NULL);
        if (tret == pdPASS) {
            ESP_LOGW(TAG, "[step30] mesh-probe task started (interval=%u ms)",
                     MESH_PROBE_BURST_PERIOD_MS);
        } else {
            ESP_LOGE(TAG, "[step30] xTaskCreate(mesh-probe) failed: %d", (int)tret);
        }

        /* opcode probe disabled now that swapped
         * in the Linux 1.17.9 softmac firmware. The probe was useful for
         * exploring the SDK fullmac firmware's command surface but adds
         * boot-time noise that obscures steady-state RX activity. Leave
         * the code in place for future re-enabling if needed. */
        (void)mesh_opcode_probe_task;
    } else {
        ESP_LOGE(TAG, "RESULT: FAIL — mmwlan_mesh_enable() returned status=%d.", (int)st);
        ESP_LOGE(TAG, "        See the 'mesh: ...' lines above for the failing step");
        ESP_LOGE(TAG, "        (ADD_INTERFACE vs MESH_CONFIG) and docs/history/mesh-port-scope.md.");
    }
}
