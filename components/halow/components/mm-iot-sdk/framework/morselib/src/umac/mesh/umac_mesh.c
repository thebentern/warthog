/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * umac mesh mode — skeleton.
 *
 * What works: compiles into morselib, reaches umac internal headers, the
 * public mmwlan_mesh_* entry points link and run.
 *
 * What's stubbed: the actual chip command sequence. Bringing the mesh
 * interface up requires, in order:
 *   1. ADD_INTERFACE   (0x0004) with morse_cmd_interface_type = MESH (5)
 *   2. BSS_CONFIG      (0x0006) — mesh BSS parameters
 *   3. BSS_BEACON_CONFIG (0x003D) — beacon template + mesh IEs
 *   4. MESH_CONFIG     (0x0039) opcode START — begin beaconing + MBCA
 * plus INSTALL_KEY (0x000A) when security_type == MMWLAN_SAE.
 * Opcodes per morse_commands.h (semver 56.17.0) from the GPL Linux driver.
 * See docs/mesh-port-scope.md.
 */

/* Per-file MMLOG override: morselib defaults to ERR level, which blackholes
 * MMLOG_INF/_WRN. Raise just this file to INF (5 == MMLOG_LEVEL_INF) so the
 * mesh bring-up trace reaches the console. Must precede any include that
 * pulls mmlog.h. Not a global -D: that links log-only helpers (mm_hexdump
 * etc.) into unrelated umac files the component packaging never links. */
#define MMLOG_LEVEL_OVRD 5

#include "common/common.h"
#include "mmlog.h"

#include "umac_mesh.h"
#include "umac/core/umac_core.h"
#include "umac/data/umac_data.h"
#include "umac/interface/umac_interface.h"
#include "umac/config/umac_config.h"   /* Phase 4f-step19: default QoS params */
/* Phase 4c — call the supplicant shim after MESH_CONFIG(START). */
#include "umac/supplicant_shim/umac_supp_shim.h"
/* Phase 4f-step7 — install mesh-mode datapath ops on mesh startup so frames
 * pass the data->ops NULL check in umac_datapath_rx_frame_filter. */
#include "umac/datapath/umac_datapath.h"
/* Phase 4f-step10 — set the per-VIF operating channel via regdb. */
#include "umac/regdb/umac_regdb.h"
/* Phase 4f-step13 — mesh beacon constructor. Replaces the NULL return in
 * mmdrv_host_get_beacon() when the chip asks for a mesh beacon template. */
#include "umac_mesh_beacon.h"
/* For mmwlan_get_mac_addr() — we need own_addr to stamp into addr2/addr3. */
#include "mmwlan.h"
/* Phase 4f-step29 — manual probe-request burst to test chip TX path. */
#include "umac/frames/frames_common.h"     /* build_mgmt_frame, mgmt_frame_builder_t */
#include "umac/frames/probe_request.h"     /* frame_probe_request_build, frame_data_probe_request */
#include "umac/rc/umac_rc.h"               /* umac_rc_init_rate_table_mgmt */
#include "common/mac_address.h"            /* mac_addr_broadcast */
#include "mmdrv.h"                          /* mmdrv_tx_frame, mmdrv_get_tx_metadata */
/* Phase 4f-step30 — for ESP_LOGW from non-umac task contexts where the
 * MMLOG path silently drops. */
#include "esp_log.h"
/* Phase 4f-step33 — raw chip command header definitions for opcode probe. */
#include "common/morse_commands.h"

/* Phase 4f-step9 — declared as extern in umac_ap.c too; the hostap library
 * defines it. CRC32 of mesh_id → CSSID for the chip's RX BSS filter. */
extern uint32_t ieee80211_crc32(const uint8_t *frame, size_t frame_len);

#include <string.h>

/* Phase 4d — save the args from the last successful enable so
 * wpa_config_read_mesh() can read them. File-static because the MM6108 only
 * has one VIF (mesh and STA/AP are mutually exclusive). s_mesh_args_valid
 * gates the getter — if mesh isn't enabled, the supplicant shouldn't be
 * reading args. */
static struct mmwlan_mesh_args s_mesh_args;
static bool s_mesh_args_valid;

/* Phase 4f-step30 — state for the periodic probe-request burst. The host-
 * side mesh-probe task in main/mesh.c calls umac_mesh_tx_broadcast_probe()
 * which uses these to construct + submit each probe. We stash them once
 * during mesh enable so the task doesn't need to re-discover them. */
static struct umac_data *s_mesh_umacd = NULL;
static uint16_t s_mesh_vif_id = 0;
static uint8_t s_mesh_own_addr[6] = {0};

/* Phase 4f-step32 — shared mesh BSSID derived from CRC32(mesh_id). Both
 * peers on the same mesh_id compute the same value. The beacon constructor
 * uses this as addr3 (BSSID field) so peer chips with the same shared
 * BSSID set as their Addr3 RX filter will admit our beacon. */
static uint8_t s_mesh_shared_bssid[6] = {0};
static bool s_mesh_shared_bssid_valid = false;

/* Public accessor for the beacon constructor. */
const uint8_t *umac_mesh_get_shared_bssid(void)
{
    return s_mesh_shared_bssid_valid ? s_mesh_shared_bssid : NULL;
}

const struct mmwlan_mesh_args *umac_mesh_get_args(void)
{
    return s_mesh_args_valid ? &s_mesh_args : NULL;
}

bool umac_mesh_validate_args(struct umac_data *umacd, const struct mmwlan_mesh_args *args)
{
    (void)umacd;

    if (args == NULL)
    {
        MMLOG_ERR("mesh: NULL args\n");
        return false;
    }
    if (args->mesh_id_len == 0 || args->mesh_id_len > MMWLAN_MESH_ID_MAXLEN)
    {
        MMLOG_ERR("mesh: mesh_id_len %u out of range (1..%u)\n",
                  args->mesh_id_len, MMWLAN_MESH_ID_MAXLEN);
        return false;
    }
    if (args->security_type != MMWLAN_OPEN && args->security_type != MMWLAN_SAE)
    {
        MMLOG_ERR("mesh: security_type %d unsupported (OPEN or SAE only)\n",
                  (int)args->security_type);
        return false;
    }
    if (args->security_type == MMWLAN_SAE && args->passphrase_len == 0)
    {
        MMLOG_ERR("mesh: SAE selected but passphrase is empty\n");
        return false;
    }
    return true;
}

enum mmwlan_status umac_mesh_enable_mesh(struct umac_data *umacd,
                                         const struct mmwlan_mesh_args *args)
{
    MMLOG_INF("mesh: enable requested, mesh_id_len=%u security=%s\n",
              args->mesh_id_len,
              (args->security_type == MMWLAN_SAE) ? "SAE" : "OPEN");

    /* Phase 2: ADD_INTERFACE(type=MESH). Passing NULL for the MAC lets the
     * interface layer use the chip's device MAC. This call is also the
     * diagnostic for whether the bundled mm6108.mbin firmware supports a
     * mesh VIF at all — morselib's morse_commands.h carries the
     * MORSE_CMD_INTERFACE_TYPE_MESH enum value but none of the mesh-specific
     * commands (MESH_CONFIG etc.), so the firmware build's mesh support is
     * unconfirmed until this ADD_INTERFACE is exercised on hardware. */
    uint16_t vif_id = UMAC_INTERFACE_VIF_ID_INVALID;
    enum mmwlan_status status = umac_interface_add(umacd, UMAC_INTERFACE_MESH, NULL, &vif_id);
    if (status != MMWLAN_SUCCESS)
    {
        MMLOG_ERR("mesh: ADD_INTERFACE(type=MESH) failed (status=%d) — the bundled "
                  "mm6108.mbin is likely a STA/AP-only firmware build\n", (int)status);
        return status;
    }

    MMLOG_INF("mesh: chip firmware ACCEPTED a mesh VIF (vif_id=%u)\n", vif_id);

    /* Phase 4f-step10 — set the per-VIF operating channel. AP mode does this
     * via umac_interface_set_channel before BSS_CONFIG (umac_ap.c:245). The
     * chip uses this to know which radio channel the VIF lives on; without
     * it, the chip likely doesn't tune the RX path for the mesh VIF even if
     * the global channel list is populated. Pick the first channel from the
     * regdb — both boards on the same region get the same first channel, so
     * they auto-coordinate. Phase 6+ can let the user specify a channel. */
    const struct mmwlan_s1g_channel *mesh_chan = umac_regdb_get_channel_at_index(umacd, 0);
    if (mesh_chan != NULL)
    {
        enum mmwlan_status ch_status = umac_interface_set_channel_from_regdb(umacd, mesh_chan, false);
        if (ch_status != MMWLAN_SUCCESS)
        {
            MMLOG_ERR("mesh: set_channel_from_regdb failed (status=%d) — "
                      "chip RX may stay closed for this VIF\n", (int)ch_status);
            /* Don't bail: the chip MIGHT still RX via the global channel list.
             * If it doesn't, we'll see zero rx# lines in the diagnostic. */
        }
        else
        {
            MMLOG_INF("mesh: SET_CHANNEL OK — operating chan=%u bw=%uMHz\n",
                      mesh_chan->s1g_chan_num, mesh_chan->bw_mhz);
        }
    }
    else
    {
        MMLOG_WRN("mesh: no channels in regdb — skipping SET_CHANNEL\n");
    }

    /* ===================================================================
     * Phase 4f-step27 — REORDER chip commands to match Linux exactly.
     *
     * Until step 26 our sequence after ADD_INTERFACE was:
     *   SET_CHANNEL → BSS_CONFIG → BSSID_SET → SCAN_CONFIG → QoS×4 →
     *   BSS_BEACON_CONFIG → MESH_CONFIG START → start_beaconing
     *
     * But agent-driven trace of /tmp/morse_driver shows Linux fires its
     * chip commands during mesh start in this order:
     *   add_interface() :    ADD_INTERFACE, GET_CAPABILITIES
     *   config callback :    SET_CHANNEL
     *   conf_tx callback:    SET_QOS_PARAMS × 4 (one per AC)
     *   bss_info_changed():
     *      BSS_CHANGED_BEACON_ENABLED → BSS_BEACON_CONFIG, MESH_CONFIG(START)
     *      BSS_CHANGED_BSSID          → BSSID_SET
     *      BSS_CHANGED_BEACON_INT|SSID→ BSS_CONFIG (cssid)
     *
     * The critical bit: Linux sends BSS_BEACON_CONFIG + MESH_CONFIG(START)
     * BEFORE BSSID_SET + BSS_CONFIG. We had it the other way around.
     *
     * Hypothesis: MESH_CONFIG(START) is the trigger that arms the chip's
     * `mesh_delayed_start` task (confirmed present as a FreeRTOS task name
     * in mm6108.mbin strings). If subsequent commands (BSSID_SET, BSS_CONFIG)
     * are sent AFTER MESH_CONFIG, they're applied to an active mesh state.
     * If sent BEFORE, they may leave the VIF in a config-locked state that
     * MESH_CONFIG can't subsequently transition out of.
     *
     * Also dropping the SCAN_CONFIG attempt — it has been failing with -17
     * (chip "EEXIST"/"already in this state") across every iteration, and
     * a failed command may pollute chip state. Linux never calls SCAN_CONFIG
     * during normal mesh start (only via sw_scan_start callback, which mac80211
     * doesn't fire during mesh join).
     * =================================================================== */

    /* Linux step 1: QoS×4 (via conf_tx callback) — push BEFORE any beacon/
     * mesh-state setup. mac80211 fires conf_tx 4 times during interface
     * setup, one per AC: BE/BK/VI/VO. morse_driver translates each to
     * MORSE_CMD_ID_SET_QOS_PARAMS. For mesh, mac80211 uses S1G WMM defaults
     * (TXOP=15008us per IEEE-802.11-2020 Tbl 9-155). */
    {
        const struct mmwlan_qos_queue_params *qos_params =
            umac_config_get_default_qos_queue_params(umacd);
        if (qos_params != NULL)
        {
            for (int aci = 0; aci < MMWLAN_QOS_QUEUE_NUM_ACIS; aci++)
            {
                int qret = mmdrv_cfg_qos_queue(&qos_params[aci]);
                if (qret != 0)
                {
                    MMLOG_WRN("mesh: cfg_qos_queue ACI=%d ret=%d\n", aci, qret);
                }
            }
            MMLOG_INF("mesh: pushed 4 default QoS queue configs to chip\n");
        }
    }

    /* Phase 4f-step28 — enable NDP (Null Data Packet) probe support for the
     * mesh VIF.
     *
     * Linux enables this UNCONDITIONALLY for all AP-type interfaces
     * (including mesh) via `morse_ndp_probe_req_resp_init()` (called from
     * `morse_mac_vif_init_ap` at mac.c:3260). The Linux path enables a
     * chip→host IRQ for NDP probe req/resp events at bit 25-26 of the IRQ
     * register. Our embedded morselib only calls `mmdrv_set_ndp_probe()`
     * for SCAN-type interfaces (umac_interface.c:131-134), NEVER for
     * mesh — so we've been missing this for 27 iterations.
     *
     * NDP probe requests are S1G's lightweight peer-discovery mechanism:
     * a minimal control-class frame (~22 bytes vs ~80 for a full
     * probe req) that asks "anyone on this BSSID?". Mesh STAs on S1G
     * may use NDP probe req/resp instead of (or alongside) full beacons
     * for fast peer discovery — especially at startup when neither side
     * has cached the other's BSSID yet.
     *
     * If the chip's mesh state machine requires NDP probe support to be
     * enabled before its `mesh_delayed_start` task arms, this is the
     * missing trigger. */
    {
        extern int mmdrv_set_ndp_probe(uint16_t vif_id, bool enabled);
        int ndp_ret = mmdrv_set_ndp_probe(vif_id, true);
        if (ndp_ret != 0)
        {
            MMLOG_WRN("mesh: SET_NDP_PROBE_SUPPORT(enable=1) rejected: %d "
                      "(opcode 0x800C; could be chip firmware doesn't expose it)\n", ndp_ret);
        }
        else
        {
            MMLOG_INF("mesh: [step28] SET_NDP_PROBE_SUPPORT(enable=1) OK — "
                      "chip should now RX NDP probe req/resp for mesh peer discovery\n");
        }
    }

    /* Linux step 2 (BSS_CHANGED_BEACON_ENABLED handler, first thing in
     * morse_mac_bss_info_changed): BSS_BEACON_CONFIG enable=true.
     * This is the chip's "arm the beacon timer hardware" command. */
    int ret = mmdrv_cfg_bss_beacon(vif_id, /*enable=*/true);
    if (ret != 0)
    {
        MMLOG_WRN("mesh: BSS_BEACON_CONFIG(enable=1) rejected: %d\n", ret);
    }
    else
    {
        MMLOG_INF("mesh: [step27] BSS_BEACON_CONFIG(enable=1) OK "
                  "(now sent BEFORE BSSID/BSS_CONFIG, matching Linux)\n");
    }

    /* Linux step 3 (same handler block, immediately after BSS_BEACON_CONFIG):
     * MESH_CONFIG(START) with mbca_config=0 (step25 confirmed MBCA-OFF works
     * and chip accepts), enable_beaconing=true. */
    ret = mmdrv_mesh_config(vif_id, /*start=*/true, /*enable_beaconing=*/true);
    if (ret != 0)
    {
        MMLOG_ERR("mesh: MESH_CONFIG(START) rejected by firmware: %d\n", ret);
        return MMWLAN_ERROR;
    }
    MMLOG_INF("mesh: [step27] MESH_CONFIG(START) accepted — "
              "expecting chip's mesh_delayed_start task to arm now\n");

    /* Phase 4f-step32 — derive a SHARED mesh BSSID from the mesh_id hash.
     *
     * Prior steps 15 + 26 set BSSID to per-board values (own_addr / broadcast).
     * Neither opened chip RX. mac80211 mesh peers EVENTUALLY all share the
     * same BSSID — the first joiner randomly generates it, subsequent peers
     * adopt it via beacon RX. We can't go through that chicken-and-egg
     * because the chip won't deliver any beacon — so we short-circuit it
     * by deriving the BSSID deterministically from the mesh_id:
     *
     *   bssid[0] = 0x02  (locally administered bit set, unicast bit clear)
     *   bssid[1..4] = bytes of CRC32(mesh_id)  (= cssid we already compute)
     *   bssid[5] = 0x5a  (constant nonce so the OUI byte slot isn't zero)
     *
     * Both boards on the SAME mesh_id will produce the SAME 6-byte BSSID.
     * The chip's Addr3 (BSSID) RX filter, set to this value, will pass any
     * mesh frame from a peer that also uses it as its BSSID — finally
     * unblocking the RX path that Phase 4f steps 1-31 couldn't open.
     */
    uint32_t cssid = ieee80211_crc32(args->mesh_id, args->mesh_id_len);
    uint16_t beacon_int = args->beacon_interval_tu ? args->beacon_interval_tu : 100;

    uint8_t shared_bssid[6];
    shared_bssid[0] = 0x02;
    shared_bssid[1] = (uint8_t)((cssid      ) & 0xff);
    shared_bssid[2] = (uint8_t)((cssid >>  8) & 0xff);
    shared_bssid[3] = (uint8_t)((cssid >> 16) & 0xff);
    shared_bssid[4] = (uint8_t)((cssid >> 24) & 0xff);
    shared_bssid[5] = 0x5a;

    ret = mmdrv_set_bssid(vif_id, shared_bssid);
    if (ret != 0)
    {
        MMLOG_WRN("mesh: [step32] BSSID_SET(shared) rejected: %d\n", ret);
    }
    else
    {
        MMLOG_INF("mesh: [step32] BSSID_SET(shared=%02x:%02x:%02x:%02x:%02x:%02x) OK "
                  "(derived from CRC32(mesh_id); identical across peers; expecting "
                  "chip Addr3 filter to now pass peer frames)\n",
                  shared_bssid[0], shared_bssid[1], shared_bssid[2],
                  shared_bssid[3], shared_bssid[4], shared_bssid[5]);
    }
    /* Stash for the beacon constructor — its addr3 must match what we just
     * set as our BSSID, else peers won't recognize our beacons either. */
    memcpy(s_mesh_shared_bssid, shared_bssid, 6);
    s_mesh_shared_bssid_valid = true;

    /* Linux step 5 (BSS_CHANGED_BEACON_INT|SSID handler, LAST in
     * morse_mac_bss_info_changed): BSS_CONFIG with cssid + beacon_int. */
    ret = mmdrv_cfg_bss(vif_id, beacon_int, /*dtim_period=*/1, cssid);
    if (ret != 0)
    {
        MMLOG_ERR("mesh: BSS_CONFIG rejected by firmware: %d "
                  "(beacon_int=%u dtim=1 cssid=0x%08lx)\n",
                  ret, beacon_int, (unsigned long)cssid);
        return MMWLAN_ERROR;
    }
    MMLOG_INF("mesh: [step27] BSS_CONFIG OK — beacon_int=%u dtim=1 cssid=0x%08lx "
              "(after MESH_CONFIG, matching Linux)\n",
              beacon_int, (unsigned long)cssid);

    /* Phase 4f-step13 — initialize the mesh beacon constructor BEFORE
     * enabling the beacon IRQ. The chip's first beacon-template request
     * fires inside mmdrv_start_beaconing (or even during MESH_CONFIG(START)
     * itself per step12 observations), and we need umac_mesh_get_beacon()
     * to have valid mesh_id + own_addr stashed by then. Fetching own_addr
     * here matches the pattern used in supplicant_core_mesh.c. */
    uint8_t own_addr[6];
    enum mmwlan_status mac_status = mmwlan_get_mac_addr(own_addr);
    if (mac_status != MMWLAN_SUCCESS)
    {
        MMLOG_ERR("mesh: mmwlan_get_mac_addr failed (status=%d) — "
                  "beacon will use zero source addr (peer discovery WILL fail)\n",
                  (int)mac_status);
        memset(own_addr, 0, sizeof(own_addr));
    }
    umac_mesh_beacon_init(args, own_addr);

    /* Phase 4f-step12 — discriminating experiment for the chip's mesh
     * beacon model. AP mode calls mmdrv_start_beaconing(vif_id) right
     * after BSS_CONFIG (umac_ap.c:267) — that's what enables the per-VIF
     * beacon IRQ (MORSE_INT_BEACON_BASE_NUM + vif_id). Without it, the
     * chip never asks the host for beacon templates via
     * mmdrv_host_get_beacon. We previously observed zero
     * mmdrv_host_get_beacon calls in 7+ seconds of mesh operation;
     * the most likely explanation is that this call simply isn't being
     * made for the mesh VIF.
     *
     * If after this addition the chip starts calling
     * mmdrv_host_get_beacon (counter increments in the diagnostic
     * inside umac_mmdrv_shim.c), the chip uses the polled-callback
     * beacon model for mesh just like AP, and the next step is
     * building a proper mesh beacon constructor (Mesh ID IE 113,
     * Mesh Configuration IE 114, S1G Capabilities, RSN IE if SAE)
     * in place of umac_ap_get_beacon.
     *
     * If the counter stays at 0, the chip's mesh firmware uses a
     * different beacon path entirely (push-template via
     * MORSE_CMD_ID_BEACON_OFFLOAD 0x0053, which morselib's
     * morse_commands.h doesn't expose), and we'd need to add that
     * opcode wrapper next. */
    ret = mmdrv_start_beaconing(vif_id);
    if (ret != 0)
    {
        MMLOG_ERR("mesh: mmdrv_start_beaconing(vif_id=%u) failed: %d — "
                  "chip's beacon IRQ remains disabled for mesh VIF\n",
                  vif_id, ret);
        /* Don't bail: MESH_CONFIG(START) already succeeded; the chip
         * may still beacon internally (just without a host-provided
         * template). We continue so the rest of the host stack comes
         * up and the diagnostic still gets to run. */
    }
    else
    {
        MMLOG_INF("mesh: mmdrv_start_beaconing OK — beacon IRQ enabled "
                  "for vif_id=%u (watch mmdrv_host_get_beacon counter)\n",
                  vif_id);
    }

    /* Phase 4f-step7 — install mesh-aware datapath ops. This unblocks the
     * RX filter (data->ops != NULL gate) so the chip's delivered frames
     * actually reach umac_datapath_process_rx_mgmt_frame → action-frame
     * switch → SELF_PROTECTED case → supplicant → mesh_mpm. The TX-side
     * ops (dequeue/enqueue/lookup) are safe stubs that return "no peer,
     * no frame" so the chip's autonomous TX poll exits cleanly instead
     * of crashing on a NULL AP-STA deref (which is what umac_ap_* did).
     *
     * See umac_datapath_mesh.c for the per-function rationale. The empty-
     * peer-table case is correct today (mmwlan_mesh_get_peer_count == 0);
     * when we wire a real peer table the lookup functions get real impls
     * but the ops table shape stays the same. */
    umac_datapath_configure_mesh_mode(umacd);
    MMLOG_INF("mesh: datapath ops set to mesh — RX filter will now pass frames\n");

    /* ===================================================================
     * Phase 4f-step29 — DIRECT CHIP-TX-PATH VERIFICATION via manual probe req.
     *
     * After 28 iterations the chip is silent on mesh: beacon_irq fires once
     * at startup, no further IRQs, no TX_STATUS notifications, no RX. The
     * remaining question is whether the chip's TX path is functional AT ALL
     * in mesh mode, or whether the chip's mesh state machine just discards
     * any TX request submitted via the datapath.
     *
     * Test: manually construct a broadcast probe request (with the mesh_id
     * as SSID, mimicking Linux's mesh_beaconless probe path), tag it with
     * the mesh VIF id, and submit via mmdrv_tx_frame. Then watch:
     *   - mmdrv_tx_frame# diagnostic — counts the chip TX submission
     *   - mmdrv_host_process_tx_status# diagnostic — counts chip-reported completion
     *
     * Outcomes:
     *   (a) Both fire → chip TX path works in mesh mode. The silence we saw
     *       is purely an upstream issue (host stack not generating mgmt
     *       frames for mesh, or chip not receiving any peer frames). Build
     *       a periodic probe-request timer next.
     *   (b) tx_frame fires, tx_status doesn't → chip accepts the TX request
     *       but doesn't actually transmit (silently drops in firmware).
     *       Strong evidence chip mesh path is broken.
     *   (c) Neither fires → host stack is blocked from submitting (the
     *       mmdrv_tx_frame call returns an error before chip sees anything).
     *
     * This is a one-shot at startup. If we get peer discovery later we can
     * remove this. */
    {
        extern struct mmpkt *build_mgmt_frame(struct umac_data *umacd,
                                               mgmt_frame_builder_t builder, void *params);
        struct frame_data_probe_request preq = {
            .bssid = mac_addr_broadcast,
            .sta_address = own_addr,  /* uses own_addr from above */
            .ssid = args->mesh_id,
            .ssid_len = args->mesh_id_len,
            .extra_ies = NULL,
            .extra_ies_len = 0,
        };
        struct mmpkt *probe = build_mgmt_frame(umacd, frame_probe_request_build, &preq);
        if (probe == NULL)
        {
            MMLOG_ERR("mesh: [step29] build_mgmt_frame(probe_request) failed — "
                      "TX path broken at mgmt-frame build layer\n");
        }
        else
        {
            struct mmdrv_tx_metadata *tx_md = mmdrv_get_tx_metadata(probe);
            tx_md->flags = MMDRV_TX_FLAG_IMMEDIATE_REPORT;
            tx_md->tid = MMWLAN_MAX_QOS_TID;
            tx_md->vif_id = vif_id;
            umac_rc_init_rate_table_mgmt(umacd, &tx_md->rc_data, false);

            int tx_ret = mmdrv_tx_frame(probe, /*is_mgmt=*/true);
            if (tx_ret < 0)
            {
                MMLOG_ERR("mesh: [step29] mmdrv_tx_frame(probe_request) FAILED: %d\n", tx_ret);
            }
            else
            {
                MMLOG_INF("mesh: [step29] mmdrv_tx_frame(probe_request) submitted OK — "
                          "watch for mmdrv_tx_frame#1 + mmdrv_host_process_tx_status#1 logs\n");
            }
        }
    }

    /* Phase 4d — stash the args before calling the shim. wpa_config_read_mesh()
     * runs inside wpa_supplicant_add_iface() and pulls these out via
     * umac_mesh_get_args() to populate the wpa_ssid (mesh_id + security). */
    memcpy(&s_mesh_args, args, sizeof(s_mesh_args));
    s_mesh_args_valid = true;

    /* Phase 4f-step30 — stash state for the periodic probe-request burst. */
    s_mesh_umacd = umacd;
    s_mesh_vif_id = vif_id;
    memcpy(s_mesh_own_addr, own_addr, 6);

    /* Phase 4c: register a wpa_supplicant mesh interface so hostap's PLINK
     * state machine (mesh_mpm.c) runs against this VIF. Mirrors how umac_ap
     * calls umac_supp_add_ap_interface() after the chip's BSS_CONFIG.
     *
     * Phase 4d hooked up the MESH config reader + driver_ops, so add_iface
     * should now succeed and hostap's mesh layer is actually reached. PLINK
     * peering still needs runtime exercise with two boards. */
    enum mmwlan_status supp_status = umac_supp_add_mesh_interface(umacd);
    if (supp_status == MMWLAN_SUCCESS)
    {
        MMLOG_INF("mesh: supplicant mesh interface added — hostap is now driving the VIF\n");
    }
    else
    {
        MMLOG_WRN("mesh: supplicant mesh interface NOT up (status=%d) — "
                  "MESH config / driver_ops lookup failed; see preceding hostap log\n",
                  (int)supp_status);
        s_mesh_args_valid = false;
    }

    /* TODO Phase 6: mesh datapath (4-address frames). */
    return MMWLAN_SUCCESS;
}

enum mmwlan_status umac_mesh_disable_mesh(struct umac_data *umacd)
{
    (void)umacd;
    return MMWLAN_UNAVAILABLE;
}

/* Phase 4f-step30 — host-driven periodic broadcast probe request.
 *
 * Called from the mesh-probe FreeRTOS task in main/mesh.c every 2s after
 * mesh_enable returns SUCCESS. Each call builds a fresh probe-request frame
 * carrying the mesh_id as SSID, then submits to mmdrv_tx_frame.
 *
 * Returns 0 on success, negative on error.
 *
 * Diagnostic value: per-call the chip-side counters surface
 *   - mmdrv_tx_frame#N: submission seen at chip-cmd layer
 *   - mmdrv_host_process_tx_status#N: chip-reported completion
 * If those numbers climb in lockstep, chip TX works for mesh. If the peer
 * board's mmdrv_host_process_rx_frame#N also climbs, the chip can RX foreign-
 * BSSID mgmt frames in mesh mode — meaning the path to peering is OPEN and
 * Linux-style mesh_beaconless mode would work. */
int umac_mesh_tx_broadcast_probe(void)
{
    static uint32_t s_call_count = 0;
    s_call_count++;
    bool log_this = (s_call_count <= 4 || (s_call_count % 10) == 0);

    if (s_mesh_umacd == NULL || !s_mesh_args_valid)
    {
        if (log_this) {
            ESP_LOGW("umac_mesh", "tx_broadcast_probe#%lu: mesh not active (umacd=%p valid=%d)",
                     (unsigned long)s_call_count, s_mesh_umacd, (int)s_mesh_args_valid);
        }
        return -1;
    }

    struct frame_data_probe_request preq = {
        .bssid = mac_addr_broadcast,
        .sta_address = s_mesh_own_addr,
        .ssid = s_mesh_args.mesh_id,
        .ssid_len = s_mesh_args.mesh_id_len,
        .extra_ies = NULL,
        .extra_ies_len = 0,
    };
    struct mmpkt *probe = build_mgmt_frame(s_mesh_umacd, frame_probe_request_build, &preq);
    if (probe == NULL)
    {
        if (log_this) {
            ESP_LOGW("umac_mesh", "tx_broadcast_probe#%lu: build_mgmt_frame returned NULL",
                     (unsigned long)s_call_count);
        }
        return -2;
    }

    struct mmdrv_tx_metadata *tx_md = mmdrv_get_tx_metadata(probe);
    tx_md->flags = MMDRV_TX_FLAG_IMMEDIATE_REPORT;
    tx_md->tid = MMWLAN_MAX_QOS_TID;
    tx_md->vif_id = s_mesh_vif_id;
    umac_rc_init_rate_table_mgmt(s_mesh_umacd, &tx_md->rc_data, false);

    int ret = mmdrv_tx_frame(probe, /*is_mgmt=*/true);
    if (log_this || ret < 0)
    {
        ESP_LOGW("umac_mesh", "tx_broadcast_probe#%lu: mmdrv_tx_frame returned %d "
                 "(vif=%u ssid_len=%u probe=%p)",
                 (unsigned long)s_call_count, ret,
                 (unsigned)s_mesh_vif_id, (unsigned)s_mesh_args.mesh_id_len, probe);
    }
    return ret;
}

uint8_t umac_mesh_get_peer_count(struct umac_data *umacd)
{
    (void)umacd;
    return 0;
}

/* Phase 4f-step33 — chip opcode probe.
 *
 * Send every opcode in [start_opcode, end_opcode] with empty payload via
 * mmdrv_execute_command (which bypasses opcode validation). Log which
 * opcodes the chip ACCEPTS (returns 0 or MORSE_RET_EPERM=-1 vs unrecognized
 * codes). This is intended to surface undocumented opcodes that might
 * affect RX behavior — promiscuous mode, monitor mode, filter bypass, etc.
 *
 * Run on the umac task. Each call sends 1 opcode and logs the result.
 * Caller iterates from a host task.
 */
int umac_mesh_probe_opcode(uint16_t opcode)
{
    extern int mmdrv_execute_command(uint8_t *command, uint8_t *response, uint32_t *response_len);

    /* Build a minimal request: just header, no payload. */
    struct morse_cmd_req cmd_req;
    memset(&cmd_req, 0, sizeof(cmd_req));
    cmd_req.hdr.flags = MORSE_CMD_TYPE_REQ;
    cmd_req.hdr.message_id = opcode;
    cmd_req.hdr.len = sizeof(struct morse_cmd_header);
    cmd_req.hdr.host_id = 0;
    cmd_req.hdr.vif_id = s_mesh_vif_id;

    uint8_t resp_buf[64];
    uint32_t resp_len = sizeof(resp_buf);
    int ret = mmdrv_execute_command((uint8_t *)&cmd_req, resp_buf, &resp_len);
    return ret;
}
