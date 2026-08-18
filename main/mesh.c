#include "mesh.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mmwlan.h"
#include "mmwlan_mesh.h"
#include "region.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "mmhalow.h"
#include "cfg.h"

static const char *TAG = "warthog.mesh";

/* Phase 4f-step30 — periodic probe-request burst.
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

/* Bring the HaLow netif up over the mesh link.
 *
 * In STA mode this happens through mmhalow_link_state() when the chip reports
 * MMWLAN_LINK_UP; mesh has no such event, so it is done here on the first
 * ESTAB. Static addressing, 10.77.0.0/16 with the host part taken from the
 * low two octets of the HaLow MAC, so two boards never collide and there is
 * no DHCP to run over a link that has no server. That is enough for IP,
 * ARP, and UDP multicast -- which is what a Meshtastic UDP transport needs. */
extern volatile uint32_t g_warthog_mpm_estab;
static bool s_mesh_netif_up;
static void mesh_netif_up_(void)
{
    esp_netif_t *netif = mmhalow_get_netif();
    if (netif == NULL || s_mesh_netif_up) {
        return;
    }
    uint8_t mac[6];
    mmwlan_get_mac_addr(mac);

    esp_netif_dhcpc_stop(netif);
    esp_netif_ip_info_t ip = { 0 };
    IP4_ADDR(&ip.ip, 10, 77, mac[4], mac[5]);
    IP4_ADDR(&ip.netmask, 255, 255, 0, 0);
    IP4_ADDR(&ip.gw, 10, 77, mac[4], mac[5]);
    esp_netif_set_ip_info(netif, &ip);
    esp_netif_action_connected(netif, NULL, 0, NULL);
    s_mesh_netif_up = true;
    ESP_LOGI(TAG, "mesh: netif up at " IPSTR, IP2STR(&ip.ip));
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
        if (g_warthog_mpm_estab) {
            mesh_netif_up_();
        }
        vTaskDelay(pdMS_TO_TICKS(MESH_PROBE_BURST_PERIOD_MS));
    }
}

/* Phase 4f-step33 — chip opcode probe.
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
    extern volatile uint32_t g_warthog_mesh_secure;
    g_warthog_mesh_secure = warthog_cfg_get_mesh_secure();
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
    warthog_mesh_scan_probe();

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
     * OpenMANET parity (see the channel notes in platformio.ini):
     *   -DWARTHOG_MESH_ID='"halowmesh"'  -DWARTHOG_MESH_BEACON_TU=1000
     */
#ifndef WARTHOG_MESH_ID
#define WARTHOG_MESH_ID "warthog-mesh-test"
#endif
#ifndef WARTHOG_MESH_BEACON_TU
#define WARTHOG_MESH_BEACON_TU 100
#endif
    struct mmwlan_mesh_args args = MMWLAN_MESH_ARGS_INIT;
    static const char MESH_ID[] = WARTHOG_MESH_ID;
    args.mesh_id_len = (uint8_t)(sizeof(MESH_ID) - 1);
    memcpy(args.mesh_id, MESH_ID, args.mesh_id_len);
    args.security_type = MMWLAN_OPEN;
    args.beacon_interval_tu = WARTHOG_MESH_BEACON_TU;

    enum mmwlan_status st = mmwlan_mesh_enable(&args);

    /* Phase 3: mmwlan_mesh_enable() returns SUCCESS only when BOTH
     * ADD_INTERFACE(type=MESH) and MESH_CONFIG(START) were accepted by the
     * chip — see umac_mesh_enable_mesh(). The per-step "mesh: ..." lines
     * above (visible via the per-file MMLOG_LEVEL_OVRD in umac_mesh.c)
     * show exactly how far it got. */
    if (st == MMWLAN_SUCCESS) {
        ESP_LOGW(TAG, "RESULT: PASS — mesh VIF added AND MESH_CONFIG(START) accepted.");
        ESP_LOGW(TAG, "        The chip is beaconing as an 802.11s mesh STA.");
        ESP_LOGW(TAG, "        Phase 3 done; next is Phase 4 (PLINK peering).");

        /* Phase 4f-step30 — start the periodic probe-request burst task. */
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

        /* Phase 4f-step33 — opcode probe disabled now that Phase 4i swapped
         * in the Linux 1.17.9 softmac firmware. The probe was useful for
         * exploring the SDK fullmac firmware's command surface but adds
         * boot-time noise that obscures steady-state RX activity. Leave
         * the code in place for future re-enabling if needed. */
        (void)mesh_opcode_probe_task;
    } else {
        ESP_LOGE(TAG, "RESULT: FAIL — mmwlan_mesh_enable() returned status=%d.", (int)st);
        ESP_LOGE(TAG, "        See the 'mesh: ...' lines above for the failing step");
        ESP_LOGE(TAG, "        (ADD_INTERFACE vs MESH_CONFIG) and docs/mesh-port-scope.md.");
    }
}
