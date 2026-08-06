#include "mesh.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mmwlan.h"
#include "mmwlan_mesh.h"

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
#define MESH_PROBE_BURST_TASK_PRIO  5

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

void warthog_mesh_smoke_test(void)
{
    ESP_LOGW(TAG, "=== MESH SMOKE TEST — probing chip firmware mesh support ===");

    /* mmhalow_init() has already run mmwlan_init(), set the channel list, and
     * booted the chip, so mmwlan_mesh_enable() can go straight to
     * ADD_INTERFACE(type=MESH). Open mesh (no SAE) keeps the probe minimal —
     * we are testing whether the firmware accepts the VIF type, not security. */
    struct mmwlan_mesh_args args = MMWLAN_MESH_ARGS_INIT;
    static const char MESH_ID[] = "warthog-mesh-test";
    args.mesh_id_len = (uint8_t)(sizeof(MESH_ID) - 1);
    memcpy(args.mesh_id, MESH_ID, args.mesh_id_len);
    args.security_type = MMWLAN_OPEN;

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
