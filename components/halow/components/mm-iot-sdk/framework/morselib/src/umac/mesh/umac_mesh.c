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
 * See docs/history/mesh-port-scope.md.
 */

/* Per-file MMLOG override: morselib defaults to ERR level, which blackholes
 * MMLOG_INF/_WRN. Raise just this file to INF (5 == MMLOG_LEVEL_INF) so the
 * mesh bring-up trace reaches the console. Must precede any include that
 * pulls mmlog.h. Not a global -D: that links log-only helpers (mm_hexdump
 * etc.) into unrelated umac files the component packaging never links. */
#define MMLOG_LEVEL_OVRD 5

#include "common/common.h"
#include "mmlog.h"
#include "umac/mesh/umac_mesh_plink_tbl.h"
#include "umac/mesh/umac_mesh_ccmp_kat.h"
#include "mmosal.h"
#include "mmhal_core.h"

#include "umac_mesh.h"
#include "umac/core/umac_core.h"
#include "umac/data/umac_data.h"
#include "umac/interface/umac_interface.h"
#include "umac/config/umac_config.h"   /* default QoS params */
/* Call the supplicant shim after MESH_CONFIG(START). */
#include "umac/supplicant_shim/umac_supp_shim.h"
/* install mesh-mode datapath ops on mesh startup so frames
 * pass the data->ops NULL check in umac_datapath_rx_frame_filter. */
#include "umac/datapath/umac_datapath.h"
/* set the per-VIF operating channel via regdb. */
#include "umac/regdb/umac_regdb.h"
#include "umac/ies/s1g_capabilities.h"
#include "umac/mesh/umac_mesh_hwmp.h"
/* mesh beacon constructor. Replaces the NULL return in
 * mmdrv_host_get_beacon() when the chip asks for a mesh beacon template. */
#include "umac_mesh_beacon.h"
#include "umac_mesh_ies.h"
/* For mmwlan_get_mac_addr() — we need own_addr to stamp into addr2/addr3. */
#include "mmwlan.h"
/* manual probe-request burst to test chip TX path. */
#include "umac/frames/frames_common.h"     /* build_mgmt_frame, mgmt_frame_builder_t */
#include "umac/frames/probe_request.h"     /* frame_probe_request_build, frame_data_probe_request */
#include "umac/frames/probe_response.h"    /* frame_probe_response_build, frame_data_probe_response */
#include "umac/frames/action.h"           /* frame_action_build, frame_data_action */
#include "umac/rc/umac_rc.h"               /* umac_rc_init_rate_table_mgmt */
#include "common/mac_address.h"            /* mac_addr_broadcast */
#include "mmdrv.h"                          /* mmdrv_tx_frame, mmdrv_get_tx_metadata */
/* for ESP_LOGW from non-umac task contexts where the
 * MMLOG path silently drops. */
#include "esp_log.h"
/* raw chip command header definitions for opcode probe. */
#include "common/morse_commands.h"

/* declared as extern in umac_ap.c too; the hostap library
 * defines it. CRC32 of mesh_id → CSSID for the chip's RX BSS filter. */
extern uint32_t ieee80211_crc32(const uint8_t *frame, size_t frame_len);

#include <string.h>

extern volatile unsigned int g_warthog_mesh_act_oversize;

/* save the args from the last successful enable so
 * wpa_config_read_mesh() can read them. File-static because the MM6108 only
 * has one VIF (mesh and STA/AP are mutually exclusive). s_mesh_args_valid
 * gates the getter — if mesh isn't enabled, the supplicant shouldn't be
 * reading args. */
static struct mmwlan_mesh_args s_mesh_args;
static bool s_mesh_args_valid;

/* state for the periodic probe-request burst. The host-
 * side mesh-probe task in main/mesh.c calls umac_mesh_tx_broadcast_probe()
 * which uses these to construct + submit each probe. We stash them once
 * during mesh enable so the task doesn't need to re-discover them. */
/* Probe-response counters, reported by AT+PRSPSTAT?. Storage lives in
 * main/at.c -- morselib links as an archive, so main must own it. */
extern volatile uint32_t g_warthog_prsp_rx;
extern volatile uint32_t g_warthog_prsp_tx;
extern volatile uint32_t g_warthog_prsp_fail;
extern volatile uint32_t g_warthog_mpm_rx;
extern volatile uint32_t g_warthog_mpm_open_tx;
extern volatile uint32_t g_warthog_mpm_conf_tx;
extern volatile uint32_t g_warthog_mpm_conf_rx;
extern volatile uint32_t g_warthog_mpm_close_rx;
extern volatile uint32_t g_warthog_mpm_parse_fail;
extern volatile uint32_t g_warthog_mpm_our_llid;
extern volatile uint32_t g_warthog_mpm_our_plid;
extern volatile uint8_t g_warthog_mpm_dump[96];
extern volatile uint16_t g_warthog_mpm_dump_len;
extern volatile uint16_t g_warthog_mpm_dump_full;
extern volatile uint32_t g_warthog_mpm_close_reason;
extern volatile uint32_t g_warthog_mpm_estab;
extern volatile uint32_t g_warthog_mpm_no_slot;
extern volatile uint32_t g_warthog_mpm_expired;
extern volatile uint32_t g_warthog_mpm_close_tx;
extern volatile uint32_t g_warthog_hwmp_rx, g_warthog_hwmp_preq_rx, g_warthog_hwmp_preq_tx;
extern volatile uint32_t g_warthog_hwmp_prep_tx, g_warthog_hwmp_parse_fail, g_warthog_hwmp_not_ours;
extern volatile uint32_t g_warthog_hwmp_prep_rx;
extern volatile uint8_t g_warthog_hwmp_dump[48];
extern volatile uint16_t g_warthog_hwmp_dump_len, g_warthog_hwmp_dump_full;
extern volatile uint32_t g_warthog_cryptohost_req, g_warthog_cryptohost_done;
extern volatile uint32_t g_warthog_cryptohost_rc, g_warthog_cryptohost_val;
extern volatile uint32_t g_warthog_ccmp_kat_ran;
void umac_datapath_mesh_service_rekey(void);
extern volatile char g_warthog_mpm_links[256];
extern volatile uint32_t g_warthog_mesh_peer_add_fail;

static struct umac_data *s_mesh_umacd = NULL;
static uint16_t s_mesh_vif_id = 0;
static uint8_t s_mesh_own_addr[6] = {0};

/* shared mesh BSSID derived from CRC32(mesh_id). Both
 * peers on the same mesh_id compute the same value. The beacon constructor
 * uses this as addr3 (BSSID field) so peer chips with the same shared
 * BSSID set as their Addr3 RX filter will admit our beacon. */
static uint8_t s_mesh_shared_bssid[6] = {0};
static bool s_mesh_shared_bssid_valid = false;

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

    /* ADD_INTERFACE(type=MESH). Passing NULL for the MAC lets the
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

    /* Install the mesh datapath ops HERE, immediately after the vif exists and
     * BEFORE any command that can make the chip start delivering frames.
     *
     * The reference port binds umac_datapath_ops_mesh at umac_interface_add()
     * time for exactly this reason. Warthog used to install them near the end
     * of this function -- after MESH_CONFIG(START) -- which leaves a window
     * where the chip is already beaconing/receiving while data->ops is still
     * NULL, and umac_datapath_rx_frame_filter() drops every frame with
     * "Frame received before datapath configured".
     *
     * Idempotent (it just assigns a pointer), so the later call is harmless. */
    umac_datapath_configure_mesh_mode(umacd);
    MMLOG_INF("mesh: datapath ops installed early (before any RX can arrive)\n");

    /* set the per-VIF operating channel. AP mode does this
     * via umac_interface_set_channel before BSS_CONFIG (umac_ap.c:245). The
     * chip uses this to know which radio channel the VIF lives on; without
     * it, the chip likely doesn't tune the RX path for the mesh VIF even if
     * the global channel list is populated. Pick the first channel from the
     * regdb — both boards on the same region get the same first channel, so
     * they auto-coordinate. A future revision can let the user specify a channel. */
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
     * REORDER chip commands to match Linux exactly.
     *
     * Until step 26 our sequence after ADD_INTERFACE was:
     *   SET_CHANNEL → BSS_CONFIG → BSSID_SET → SCAN_CONFIG → QoS×4 →
     *   BSS_BEACON_CONFIG → MESH_CONFIG START → start_beaconing
     *
     * But the Linux morse driver fires its
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

    /* enable NDP (Null Data Packet) probe support for the
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
    /* DISABLED BY DEFAULT (step28 was speculative and is a prime RX suspect).
     *
     * The known-working ESP32 mesh port never sends SET_NDP_PROBE_SUPPORT at all
     * -- zero occurrences in its entire mesh implementation. NDP (Null Data
     * Packet) probes are matched by CSSID, the same field we were wrongly
     * programming non-zero above. Enabling NDP probe support plausibly switches
     * the chip's discovery/match path to NDP+CSSID and stops ordinary mgmt
     * frames from being delivered -- which is exactly the failure we measured
     * (TX fine, MESH_CONFIG accepted, zero RX once the mesh vif is active).
     *
     * Define WARTHOG_MESH_NDP_PROBE to restore it for an A/B. */
    MMLOG_INF("mesh: SET_NDP_PROBE_SUPPORT skipped (reference port never sends it)\n");


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

    /* MESH_CONFIG(START) USED TO BE HERE — it is now the LAST chip command in
     * this function. See the block after mmdrv_start_beaconing() below.
     *
     * Rationale: MESH_CONFIG(START, enable_beaconing=1) makes the firmware run
     * an MBSS TBTT-selection scan and then raise a beacon interrupt every TBTT,
     * expecting the HOST to serve each beacon template via
     * mmdrv_host_get_beacon(). Issuing it here -- before BSSID_SET, BSS_CONFIG,
     * the beacon constructor init, and mmdrv_start_beaconing() -- points the
     * firmware at a host that cannot answer yet. The firmware then beacons into
     * an unready host and the command channel backs up (page exhaustion), which
     * matches both symptoms we measured: no beacons on air (monitor capture saw
     * only the 2 s diagnostic probe bursts, never ~10/s beacons) and RXCHAN
     * never showing anything but chan=0xfe, the command channel.
     *
     * The ordering is load-bearing: configure the BSS, then make the host able
     * to serve a beacon, and only then tell the firmware to start. */

    /* derive a SHARED mesh BSSID from the mesh_id hash.
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
     * mesh frame from a peer that also uses it as its BSSID, which is what
     * lets peer frames reach the host at all.
     */
    uint32_t cssid = ieee80211_crc32(args->mesh_id, args->mesh_id_len);
    uint16_t beacon_int = args->beacon_interval_tu ? args->beacon_interval_tu : 100;

    uint8_t shared_bssid[6];
#ifdef WARTHOG_MESH_BSSID_OWN_MAC
    /* Use our OWN MAC as the mesh BSSID -- what mac80211 actually does
     * (bss_conf.bssid = vif->addr for NL80211_IFTYPE_MESH_POINT), and what the
     * known-working ESP32 mesh port does ("Set the BSSID = this node's own MAC
     * ... Unlike IBSS, MESH_CONFIG doesn't carry the BSSID, so it must be set
     * separately first").
     *
     * The step-32 derived-BSSID scheme below was invented to force two warthog
     * boards onto a common Addr3 so the chip's BSSID RX filter would pass peer
     * frames. But it makes us incompatible with any REAL 802.11s peer: a Linux
     * mesh node beacons with addr3 = its own MAC, so a derived BSSID guarantees
     * a mismatch in both directions. Measured directly: the Linux peer hears us
     * fine in monitor mode (+7 dBm) but shows RX=0 and no peers once it is in
     * mesh mode with its own-MAC BSSID. */
    if (mmwlan_get_mac_addr(shared_bssid) != MMWLAN_SUCCESS)
    {
        MMLOG_ERR("mesh: get_mac_addr failed; cannot set own-MAC BSSID\n");
        return MMWLAN_ERROR;
    }
#else
    shared_bssid[0] = 0x02;
    shared_bssid[1] = (uint8_t)((cssid      ) & 0xff);
    shared_bssid[2] = (uint8_t)((cssid >>  8) & 0xff);
    shared_bssid[3] = (uint8_t)((cssid >> 16) & 0xff);
    shared_bssid[4] = (uint8_t)((cssid >> 24) & 0xff);
    shared_bssid[5] = 0x5a;
#endif

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
    /* cssid = 0 for mesh, deliberately.
     *
     * The CSSID (Compressed SSID) is an S1G matching field the chip uses for
     * NDP probe / short-beacon matching. Programming our own CRC32(mesh_id)
     * here tells the chip to match on THAT value; a peer's frames need not
     * carry it (a Linux mac80211 mesh node does not populate a matching
     * cssid at all), so a non-zero value can gate RX in exactly the way we
     * measured: chip accepts MESH_CONFIG, TX works, and nothing is ever
     * delivered up once the mesh vif is active.
     *
     * The known-working ESP32 mesh port passes 0 here
     * (mmdrv_cfg_bss(vif_id, beacon_interval_tu, 1, 0)). Match it.
     * WARTHOG_MESH_CSSID_NONZERO restores the old behaviour for A/B testing. */
    ret = mmdrv_cfg_bss(vif_id, beacon_int, /*dtim_period=*/1, /*cssid=*/0);

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

    /* initialize the mesh beacon constructor BEFORE
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

    /* discriminating experiment for the chip's mesh
     * beacon model. AP mode calls mmdrv_start_beaconing(vif_id) right
     * after BSS_CONFIG (umac_ap.c:267) — that's what enables the per-VIF
     * beacon IRQ (MORSE_INT_BEACON_BASE_NUM + vif_id). Without it, the
     * chip never asks the host for beacon templates via
     * mmdrv_host_get_beacon. Without this call the mesh VIF produces zero
     * mmdrv_host_get_beacon requests (measured over 7+ seconds of mesh
     * operation).
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
    /* Host beacon timer period. The MM6108 firmware fires the mesh beacon IRQ
     * exactly once and never re-arms its TBTT, so beacons are driven from a
     * host timer at the advertised beacon interval (1 TU = 1024 us). Peers
     * (mac80211 / OpenMANET) discover a mesh STA from its beacons, so without
     * this the node is invisible to a beacon-driven peer. Override the cadence
     * with -DWARTHOG_MESH_BEACON_MS=<ms> (0 restores chip-IRQ-only). */
    uint16_t bcn_tu = args->beacon_interval_tu ? args->beacon_interval_tu : 100;
#ifdef WARTHOG_MESH_BEACON_MS
    uint32_t bcn_period_ms = (uint32_t)WARTHOG_MESH_BEACON_MS;
#else
    uint32_t bcn_period_ms = ((uint32_t)bcn_tu * 1024u) / 1000u;
#endif
    ret = mmdrv_start_beaconing_period(vif_id, bcn_period_ms);
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

    /* MESH_CONFIG(START, enable_beaconing=1) — LAST chip command, deliberately.
     *
     * Everything the firmware needs before it may beacon is now in place:
     * BSSID_SET, BSS_CONFIG (beacon_int + cssid), the beacon constructor
     * (umac_mesh_beacon_init), and the host beacon engine / beacon IRQ
     * (mmdrv_start_beaconing). Only now is it safe to let the firmware start
     * requesting beacons, because mmdrv_host_get_beacon() can actually answer.
     *
     * mbca_config must be NON-ZERO (see mmdrv_mesh_config in driver.c): zero
     * selects beaconless mode, which contradicts enable_beaconing=1 and leaves
     * the chip accepting START while never beaconing -- exactly what we
     * measured on air before this change.
     *
     * The firmware runs an MBSS TBTT-selection scan (~2 s) before the first
     * beacon interrupt, so expect a short delay before beacons appear. */
    ret = mmdrv_mesh_config(vif_id, /*start=*/true, /*enable_beaconing=*/true);
    if (ret != 0)
    {
        MMLOG_ERR("mesh: MESH_CONFIG(START) rejected by firmware: %d\n", ret);
        return MMWLAN_ERROR;
    }
    MMLOG_INF("mesh: MESH_CONFIG(START) accepted LAST (after BSSID/BSS_CONFIG/"
              "beacon-engine) — firmware TBTT scan ~2s, then beacon IRQs\n");

    /* install mesh-aware datapath ops. This unblocks the
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
     * DIRECT CHIP-TX-PATH VERIFICATION via manual probe req.
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

    /* stash the args before calling the shim. wpa_config_read_mesh()
     * runs inside wpa_supplicant_add_iface() and pulls these out via
     * umac_mesh_get_args() to populate the wpa_ssid (mesh_id + security). */
    memcpy(&s_mesh_args, args, sizeof(s_mesh_args));
    s_mesh_args_valid = true;

    /* stash state for the periodic probe-request burst. */
    s_mesh_umacd = umacd;
    s_mesh_vif_id = vif_id;
    memcpy(s_mesh_own_addr, own_addr, 6);

    /* Register a wpa_supplicant mesh interface so hostap's PLINK state
     * machine (mesh_mpm.c) runs against this VIF. Mirrors how umac_ap calls
     * umac_supp_add_ap_interface() after the chip's BSS_CONFIG. */
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

    return MMWLAN_SUCCESS;
}

enum mmwlan_status umac_mesh_disable_mesh(struct umac_data *umacd)
{
    (void)umacd;
    return MMWLAN_UNAVAILABLE;
}

/* host-driven periodic broadcast probe request.
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

/* Answer a peer's mesh probe request.
 *
 * This closes the discovery loop. A Linux MM8108 in beaconless mesh mode
 * (mesh_beaconless_mode=1) discovers peers by probing rather than by listening
 * for beacons, and warthog was receiving those probe requests and dropping them
 * -- "ignoring probe req (no responder yet)" in umac_datapath_mesh.c. With no
 * response the peer never learns warthog exists, so no candidate and no plink.
 *
 * That path matters because warthog's chip does not currently beacon in mesh
 * mode at all (the beacon IRQ never fires; see AT+BCNSTAT?), so probe/response
 * is the only discovery mechanism available to us.
 *
 * addr3 is our own address: mac80211 uses vif->addr as the mesh BSSID, so a
 * Linux peer matches on that. The IEs carry Mesh ID + Mesh Configuration with
 * exactly the values our beacon would advertise, because mesh_matches_local()
 * compares them before accepting a candidate. */
int umac_mesh_tx_probe_response(const uint8_t *da)
{
    if (s_mesh_umacd == NULL || !s_mesh_args_valid || da == NULL)
    {
        g_warthog_prsp_fail++;
        return -1;
    }

    uint8_t ies[UMAC_MESH_DISCOVERY_IES_MAXLEN];
    uint16_t ies_len = umac_mesh_build_discovery_ies(ies, (uint16_t)sizeof(ies));
    if (ies_len == 0)
    {
        g_warthog_prsp_fail++;
        return -2;
    }

    /* Chip stamps the real TSF on TX; zero here is fine. */
    uint8_t timestamp[8] = { 0 };
    struct frame_data_probe_response prsp = {
        .destination_address = da,
        .timestamp = timestamp,
        .bssid = s_mesh_own_addr,
        .ssid = NULL,
        .ssid_len = 0,
        .ies = ies,
        .ies_len = ies_len,
        .beacon_interval = s_mesh_args.beacon_interval_tu ? s_mesh_args.beacon_interval_tu : 100,
        .capability_info = (s_mesh_args.security_type == MMWLAN_SAE) ? 0x0010 : 0x0000,
    };

    struct mmpkt *rsp = build_mgmt_frame(s_mesh_umacd, frame_probe_response_build, &prsp);
    if (rsp == NULL)
    {
        g_warthog_prsp_fail++;
        return -3;
    }

    struct mmdrv_tx_metadata *tx_md = mmdrv_get_tx_metadata(rsp);
    tx_md->flags = MMDRV_TX_FLAG_IMMEDIATE_REPORT;
    tx_md->tid = MMWLAN_MAX_QOS_TID;
    tx_md->vif_id = s_mesh_vif_id;
    umac_rc_init_rate_table_mgmt(s_mesh_umacd, &tx_md->rc_data, false);

    int ret = mmdrv_tx_frame(rsp, /*is_mgmt=*/true);
    if (ret >= 0)
    {
        g_warthog_prsp_tx++;
        extern volatile uint8_t g_warthog_prsp_last_da[6];
        memcpy((void *)g_warthog_prsp_last_da, da, 6);
    }
    else
    {
        g_warthog_prsp_fail++;
    }
    return ret;
}

/* Beaconless-peer discovery (Morse "dynamic peering").
 *
 * A peer running mesh_beaconless_mode never beacons: it advertises itself
 * only by broadcasting a probe request for the Mesh ID (once ~5 s after its
 * mesh starts, then every ~60 s -- morse_driver mesh.c,
 * MESH_DISCOVERY_PROBE_PERIOD_S). Answering that probe (the caller does) is
 * not enough under SAE: nothing tells hostap a candidate exists, so neither
 * side ever initiates. Offer the requester as a candidate here.
 *
 * A probe request carries no Mesh Configuration element, which
 * umac_supp_mesh_new_peer() requires, so the offer substitutes our own
 * discovery IEs. That mirrors what the beaconless peer itself does on the
 * other side: on receiving SAE Authentication from an unknown address, its
 * driver fabricates a probe response from its own IE template and feeds it
 * to mac80211 so its supplicant learns the peer, then drops the frame and
 * relies on the initiator's retransmit (process_mesh_rx_mgmt_beaconless).
 * The two fabrications meet in the middle: this is the designed protocol,
 * not a trick.
 *
 * Only a request that names our mesh -- SSID(0) or Mesh ID(114) element
 * matching -- is offered. Wildcard probes are scanners, not peers, and
 * offering them would burn SAE attempts on every station in range. */
void umac_mesh_handle_probe_req_discovery(const uint8_t *ta, const uint8_t *ies,
                                          uint32_t ies_len)
{
    extern volatile uint8_t g_warthog_prq_frame[96];
    extern volatile uint16_t g_warthog_prq_len;
    extern volatile uint8_t g_warthog_prq_ta[6];
    extern volatile uint32_t g_warthog_prq_named;
    extern volatile uint32_t g_warthog_prq_offer;

    uint32_t cap = ies_len > 96u ? 96u : ies_len;
    memcpy((void *)g_warthog_prq_frame, ies, cap);
    g_warthog_prq_len = (uint16_t)ies_len;
    memcpy((void *)g_warthog_prq_ta, ta, 6);

    if (!umac_mesh_sae_active() || !s_mesh_args_valid)
    {
        return;
    }

    bool named = false;
    uint32_t off = 0;
    while (off + 2u <= ies_len)
    {
        uint8_t eid = ies[off];
        uint8_t elen = ies[off + 1];
        if (off + 2u + elen > ies_len)
        {
            break;
        }
        if ((eid == 0u /* SSID */ || eid == 114u /* Mesh ID */) &&
            elen == s_mesh_args.mesh_id_len &&
            memcmp(&ies[off + 2], s_mesh_args.mesh_id, elen) == 0)
        {
            named = true;
            break;
        }
        off += 2u + elen;
    }
    if (!named)
    {
        return;
    }
    g_warthog_prq_named++;

    uint8_t disc[UMAC_MESH_DISCOVERY_IES_MAXLEN];
    uint16_t disc_len = umac_mesh_build_discovery_ies(disc, (uint16_t)sizeof(disc));
    if (disc_len == 0)
    {
        return;
    }
    g_warthog_prq_offer++;
    umac_supp_mesh_new_peer(ta, disc, disc_len);
}

/* ---- Mesh Peering Management (MPM) ------------------------------------- *
 *
 * Minimal responder so a peer's peering handshake can complete. The Phase-1
 * port of mac80211's full MPM FSM lives in components/halow_mesh_compat, but
 * that component is host-test only and is NOT linked into the firmware, so the
 * on-air behaviour has to live here.
 *
 * Observed state before this: the Linux peer reaches OPN_SNT with a valid llid
 * and plid=0, retransmitting Open and never hearing back, so the plink never
 * reaches ESTAB. We answer an Open with our own Open followed by a Confirm,
 * which is what mac80211's mesh_plink does for a passive peer.
 *
 * Frame layout follows mesh_mpm_frame.c (the ported builder):
 *   category(1)=15 SELF_PROTECTED, action(1), capability(2),
 *   [AID(2) for CONFIRM], Mesh ID IE, Mesh Configuration IE,
 *   Peer Management IE(117): len, peering_proto(2)=0, llid(2), [plid(2)]
 *
 * Link-id perspective: the llid carried in a received frame is, from our point
 * of view, the *peer's* id -- i.e. our plid. Confirm must echo it back.
 */
#define MPM_CATEGORY_SELF_PROTECTED 15
#define MPM_ACTION_OPEN 1
#define MPM_ACTION_CONFIRM 2
#define MPM_ACTION_CLOSE 3

/** Unanswered Opens before we assume the peer holds a stale link and Close it.
 *  Opens go out one per received beacon (~1 s), so this is a few seconds. */
#ifndef MPM_OPEN_RETRY_MAX
#define MPM_OPEN_RETRY_MAX 8
#endif
/** 802.11 reason 52, MESH-PEERING-CANCELLED. */
#define MPM_REASON_PEER_CANCELED 52

/** Largest action body umac_mesh_tx_action carries: an MPM Open/Confirm with
 *  AMPE. hostap caps its own peering buffer well under this. */
#define UMAC_MESH_ACTION_BODY_MAX 512

/* Send an action body verbatim (no appended elements). */
/* ---- Peering-frame S1G <-> 11n conversion ------------------------------- *
 *
 * A mac80211 peer does not put its peering frames on air in the form its
 * supplicant built them. The vendor driver rewrites every Mesh Peering
 * Open/Confirm on the way out (11n elements removed, S1G elements added) and
 * rebuilds the 11n form on the way in, before its supplicant sees the frame.
 * hostap therefore protects one form while the air carries another, which only
 * works because the AMPE MIC covers just six octets: category, action,
 * capability, and the first element's id and length (MESH_RSN_FRAME_MIC_OFFSET
 * in mesh_rsn.c, under CONFIG_IEEE80211AH -- both ends of this link build with
 * it). Everything past those six octets, including element order and the whole
 * HT/S1G capability question, is outside the MIC by design: the rebuild could
 * not reproduce it.
 *
 * The peer's rebuild always forces Supported Rates first from a fixed table,
 * so the two authenticated element bytes are always "01 08". Our supplicant
 * now emits the same element first (see the rates block in
 * supplicant_core_mesh.c), so what we sign matches what the peer verifies.
 * These two functions supply the rest of the contract:
 *
 *   RX -- rebuild the 11n form the peer's supplicant signed, so ours verifies
 *         the same six octets. Without this the frame arrives S1G-first and
 *         our AAD reads "d9 0f".
 *   TX -- add the S1G Capabilities element, which the peer's driver requires
 *         on a Confirm and drops the frame without.
 *
 * Peering Close (action 3) is never converted in either direction: its AAD
 * reaches into the first element's value, so touching it breaks a frame type
 * that already works.
 */
#define MPM_ACTION_OPEN_    1u
#define MPM_ACTION_CONFIRM_ 2u

/* AMPE trailer sizes, from the peer driver's mesh.h. The trailer is opaque --
 * SIV plus ciphertext -- and must survive both directions byte for byte. */
#define MPM_AMPE_OPEN_LEN_    98u
#define MPM_AMPE_IGTK_LEN_    24u
#define MPM_AMPE_CONFIRM_LEN_ 70u

#define MPM_IE_SUPP_RATES_ 1u
#define MPM_IE_HT_CAP_     45u
#define MPM_IE_RSN_        48u
#define MPM_IE_MIC_        140u
#define MPM_IE_VENDOR_     221u
#define MPM_IE_S1G_CAP_    217u
#define MPM_IE_S1G_OPER_   232u

/* The peer's rebuild substitutes these exact eight rates, so they are the ones
 * both supplicants end up signing: 1, 2, 5.5, 6*, 11, 12*, 18, 24* Mbit/s. */
static const uint8_t k_mpm_supp_rates_ie_[] = {
    MPM_IE_SUPP_RATES_, 8, 0x02, 0x04, 0x0b, 0x8c, 0x16, 0x98, 0x24, 0xb0,
};

static uint16_t mpm_fixed_hdr_len_(uint8_t action)
{
    /* category, action, capability -- plus the AID a Confirm carries. */
    return (action == MPM_ACTION_CONFIRM_) ? 6u : 4u;
}

/* RSN capabilities live after the version, group cipher and the two suite
 * lists. Only the management-frame-protection bits matter here. */
static bool mpm_rsn_caps_(const uint8_t *v, uint8_t vlen, uint16_t *caps)
{
    uint32_t off = 2u + 4u; /* version, group cipher suite */
    uint16_t n;

    if (vlen < off + 2u)
    {
        return false;
    }
    n = (uint16_t)(v[off] | ((uint16_t)v[off + 1] << 8));
    off += 2u + 4u * (uint32_t)n; /* pairwise suite list */
    if (off + 2u > vlen)
    {
        return false;
    }
    n = (uint16_t)(v[off] | ((uint16_t)v[off + 1] << 8));
    off += 2u + 4u * (uint32_t)n; /* AKM suite list */
    if (off + 2u > vlen)
    {
        return false;
    }
    *caps = (uint16_t)(v[off] | ((uint16_t)v[off + 1] << 8));
    return true;
}

/* Size of the AMPE trailer, decided exactly as the peer decides it -- the two
 * ends must agree or the element region and the ciphertext both slice wrong. */
static uint16_t mpm_ampe_len_(const uint8_t *body, uint16_t body_len)
{
    uint8_t action = body[1];
    uint16_t hdr = mpm_fixed_hdr_len_(action);
    uint16_t cap;
    uint16_t len;
    const uint8_t *p;
    const uint8_t *end;

    if (body_len <= hdr)
    {
        return 0;
    }
    cap = (uint16_t)(body[2] | ((uint16_t)body[3] << 8));
    if ((cap & 0x0010u) == 0u) /* Privacy clear: unprotected peering */
    {
        return 0;
    }
    if (action == MPM_ACTION_CONFIRM_)
    {
        return MPM_AMPE_CONFIRM_LEN_;
    }

    len = MPM_AMPE_OPEN_LEN_;
    p = body + hdr;
    end = body + body_len;
    while (p + 2 <= end)
    {
        uint8_t eid = p[0];
        uint8_t elen = p[1];
        if (p + 2 + elen > end)
        {
            break;
        }
        if (eid == MPM_IE_RSN_)
        {
            uint16_t rsn_caps = 0;
            /* An Open carries the group key, and a second one when management
             * frame protection is both required and capable. */
            if (mpm_rsn_caps_(p + 2, elen, &rsn_caps) &&
                (rsn_caps & 0x0040u) && (rsn_caps & 0x0080u))
            {
                len += MPM_AMPE_IGTK_LEN_;
            }
            break;
        }
        p += 2 + elen;
    }
    return len;
}

/* HT Capabilities as the peer's rebuild synthesises it. Only two octets vary,
 * both read out of the S1G Capabilities element the sender advertised. */
static uint16_t mpm_build_ht_cap_(const uint8_t *s1g_cap, uint8_t s1g_cap_len, uint8_t *out)
{
    uint16_t cap_info = 0x000eu; /* greenfield-free defaults plus 20/40 support */
    uint8_t ampdu = 0;

    if (s1g_cap != NULL && s1g_cap_len >= 1u && (s1g_cap[0] & 0x1eu) != 0u)
    {
        cap_info |= 0x0060u; /* short GI at 20 and 40 MHz */
    }
    if (s1g_cap != NULL && s1g_cap_len >= 4u)
    {
        uint8_t c3 = s1g_cap[3];
        ampdu = (uint8_t)(((c3 >> 3) & 0x03u) | (((c3 >> 5) & 0x07u) << 2));
    }

    out[0] = MPM_IE_HT_CAP_;
    out[1] = 26;
    out[2] = (uint8_t)(cap_info & 0xffu);
    out[3] = (uint8_t)(cap_info >> 8);
    out[4] = ampdu;
    out[5] = 0xff; /* MCS rx_mask: one spatial stream */
    memset(&out[6], 0, 9);
    out[15] = 0x41; /* rx_highest */
    out[16] = 0x00;
    out[17] = 0x01; /* tx_params */
    memset(&out[18], 0, 10);
    return 28u;
}

/* Rebuild the 11n form of a received peering frame. Returns 0 when the frame
 * is not one we convert, in which case the caller passes it through. */
uint16_t umac_mesh_mpm_s1g_to_11n(const uint8_t *body, uint16_t body_len,
                                  uint8_t *out, uint16_t out_cap)
{
    struct
    {
        const uint8_t *ptr;
        uint8_t eid;
        uint8_t len;
    } ies[24];
    uint8_t n_ies = 0;
    const uint8_t *mic = NULL;
    uint8_t mic_len = 0;
    const uint8_t *s1g_cap = NULL;
    uint8_t s1g_cap_len = 0;
    uint16_t hdr;
    uint16_t ampe;
    uint16_t off = 0;
    const uint8_t *p;
    const uint8_t *end;
    uint8_t action;

    if (body == NULL || out == NULL || body_len < 6u)
    {
        return 0;
    }
    if (body[0] != MPM_CATEGORY_SELF_PROTECTED)
    {
        return 0;
    }
    action = body[1];
    if (action != MPM_ACTION_OPEN_ && action != MPM_ACTION_CONFIRM_)
    {
        return 0;
    }
    hdr = mpm_fixed_hdr_len_(action);
    ampe = mpm_ampe_len_(body, body_len);
    if ((uint32_t)hdr + ampe > body_len)
    {
        return 0;
    }

    p = body + hdr;
    end = body + body_len - ampe;
    while (p + 2 <= end && n_ies < (uint8_t)(sizeof(ies) / sizeof(ies[0])))
    {
        uint8_t eid = p[0];
        uint8_t elen = p[1];
        if (p + 2 + elen > end)
        {
            break;
        }
        if (eid == MPM_IE_MIC_)
        {
            mic = p + 2;
            mic_len = elen;
        }
        else if (eid == MPM_IE_S1G_CAP_)
        {
            s1g_cap = p + 2;
            s1g_cap_len = elen;
        }
        else if (eid != MPM_IE_SUPP_RATES_ && eid != MPM_IE_S1G_OPER_ &&
                 eid != MPM_IE_VENDOR_)
        {
            ies[n_ies].ptr = p + 2;
            ies[n_ies].eid = eid;
            ies[n_ies].len = elen;
            n_ies++;
        }
        p += 2 + elen;
    }

    /* Rebuild only what we fully understood. A truncated element, or more
     * elements than the table holds, would otherwise produce a frame that is
     * silently missing something hostap needs -- Peer Management, or the MIC
     * itself -- and a rebuild that loses the MIC fails verification in a way
     * that looks exactly like the bug this conversion exists to fix. Handing
     * the original frame on is no worse than not converting at all. */
    if (p != end)
    {
        return 0;
    }
    if (ampe > 0u && mic == NULL)
    {
        return 0;
    }

    /* Fixed fields verbatim: they are the first four authenticated octets. */
    if ((uint32_t)hdr + sizeof(k_mpm_supp_rates_ie_) > out_cap)
    {
        return 0;
    }
    memcpy(out, body, hdr);
    off = hdr;

    /* Supported Rates first -- this is what makes our AAD read "01 08". */
    memcpy(out + off, k_mpm_supp_rates_ie_, sizeof(k_mpm_supp_rates_ie_));
    off += (uint16_t)sizeof(k_mpm_supp_rates_ie_);

    /* Remaining elements in ascending id order, matching the peer's rebuild. */
    for (uint16_t want = 0; want < 256u; want++)
    {
        for (uint8_t i = 0; i < n_ies; i++)
        {
            if (ies[i].eid != (uint8_t)want)
            {
                continue;
            }
            if ((uint32_t)off + 2u + ies[i].len > out_cap)
            {
                return 0;
            }
            out[off++] = ies[i].eid;
            out[off++] = ies[i].len;
            memcpy(out + off, ies[i].ptr, ies[i].len);
            off += ies[i].len;
        }
    }

    /* HT Capabilities, then the MIC, then the AMPE trailer -- the peer's
     * rebuild emits them in exactly this order. */
    if ((uint32_t)off + 28u > out_cap)
    {
        return 0;
    }
    off += mpm_build_ht_cap_(s1g_cap, s1g_cap_len, out + off);

    if (mic != NULL)
    {
        if ((uint32_t)off + 2u + mic_len > out_cap)
        {
            return 0;
        }
        out[off++] = MPM_IE_MIC_;
        out[off++] = mic_len;
        memcpy(out + off, mic, mic_len);
        off += mic_len;
    }
    if (ampe > 0u)
    {
        if ((uint32_t)off + ampe > out_cap)
        {
            return 0;
        }
        memcpy(out + off, body + body_len - ampe, ampe);
        off += ampe;
    }
    return off;
}

/* Add the S1G Capabilities element the peer's driver insists on. Everything
 * else rides through untouched: the MIC does not cover it, and the peer
 * rebuilds the element order anyway. */
uint16_t umac_mesh_mpm_11n_to_s1g(const uint8_t *body, uint16_t body_len,
                                  uint8_t *out, uint16_t out_cap)
{
    uint8_t s1g[64];
    struct consbuf cb = CONSBUF_INIT_WITH_BUF(s1g, sizeof(s1g));
    uint16_t hdr;
    uint16_t off;
    uint8_t action;

    if (body == NULL || out == NULL || body_len < 6u || s_mesh_umacd == NULL)
    {
        return 0;
    }
    if (body[0] != MPM_CATEGORY_SELF_PROTECTED)
    {
        return 0;
    }
    action = body[1];
    if (action != MPM_ACTION_OPEN_ && action != MPM_ACTION_CONFIRM_)
    {
        return 0;
    }
    hdr = mpm_fixed_hdr_len_(action);
    if (hdr > body_len)
    {
        return 0;
    }

    ie_s1g_capabilities_build(s_mesh_umacd, &cb);
    if (cb.offset == 0u || (uint32_t)body_len + cb.offset > out_cap)
    {
        return 0;
    }

    memcpy(out, body, hdr);
    off = hdr;
    memcpy(out + off, s1g, cb.offset);
    off += (uint16_t)cb.offset;
    memcpy(out + off, body + hdr, body_len - hdr);
    off += (uint16_t)(body_len - hdr);
    return off;
}

static int mesh_tx_action_raw_(const uint8_t *da, const uint8_t *body, uint16_t body_len)
{
    if (s_mesh_umacd == NULL || !s_mesh_args_valid || da == NULL || body == NULL || body_len == 0)
    {
        return -1;
    }
    /* AMPE interop forensics: keep the exact bytes hostap protected (the MIC
     * covers this body), so AT+PLINKTX? can be diffed against the peer's view. */
    if (body[0] == MPM_CATEGORY_SELF_PROTECTED)
    {
        extern volatile uint8_t g_warthog_plink_tx[192];
        extern volatile uint16_t g_warthog_plink_tx_len;
        extern volatile uint16_t g_warthog_plink_tx_full;
        uint16_t cp = body_len > 192u ? 192u : body_len;
        memcpy((void *)g_warthog_plink_tx, body, cp);
        g_warthog_plink_tx_len = cp;
        g_warthog_plink_tx_full = body_len;
    }

    /* Put the peering frame on air in the S1G form a mac80211 peer expects.
     * The snapshot above deliberately keeps the pre-conversion bytes, since
     * those are the ones hostap signed. */
    static uint8_t s_mpm_tx_buf[512];
    if (body[0] == MPM_CATEGORY_SELF_PROTECTED)
    {
        uint16_t n = umac_mesh_mpm_11n_to_s1g(body, body_len, s_mpm_tx_buf,
                                              (uint16_t)sizeof(s_mpm_tx_buf));
        if (n > 0u)
        {
            extern volatile uint32_t g_warthog_mpm_tx_conv;
            g_warthog_mpm_tx_conv++;
            body = s_mpm_tx_buf;
            body_len = n;
        }
    }

    struct frame_data_action act = {
        .bssid = s_mesh_own_addr,
        .dst_address = da,
        .src_address = s_mesh_own_addr,
        .action_field = (uint8_t *)body,
        .action_field_len = body_len,
    };
    struct mmpkt *frm = build_mgmt_frame(s_mesh_umacd, frame_action_build, &act);
    if (frm == NULL)
    {
        return -3;
    }
    return umac_mesh_tx_mgmt_mmpkt(frm);
}

static int mesh_tx_hwmp_(const uint8_t *da, const uint8_t *body, uint16_t body_len);

/** Path lifetime we advertise, in TUs. 4882 TU ~= 5 s, which is what
 *  mac80211 uses for dot11MeshHWMPactivePathTimeout by default. */
#define UMAC_MESH_HWMP_LIFETIME_TU 4882u

/** How often we refresh our path at each peer. Must stay comfortably inside
 *  the peer's 5 s active-path timeout. */
#define UMAC_MESH_HWMP_PREQ_PERIOD_MS 2000u
#define MPM_IE_PEER_MGMT 117

/* Per-peer MPM link state. The table itself lives in the freestanding
 * umac_mesh_plink_tbl.c so the host tests drive the real code; this file keeps
 * the policy that needs the radio -- llid minting, teardown, publication. */
/* Drop a link we have heard nothing from for this long. Peers probe every 2 s
 * and that probe refreshes the timer, so 30 s is ~15 missed probes. */
#define MPM_PEER_TIMEOUT_MS 30000u

static struct mpm_table s_mpm;

/* Mint an llid for a NEW link.
 *
 * Must be random, not derived from the MAC: a deterministic llid makes a
 * rebooted node come back with the id it had before, so its peers cannot tell
 * the link restarted -- the "re-opened with a new llid" teardown never fires,
 * they keep a stale stad and a stale chip registration, and the returning node
 * can talk to nobody. Measured exactly that (estab=1 on every side, 0/4 both
 * ways). mac80211 randomises llid per plink for this reason.
 *
 * mpm_table_get_or_create() ignores this for an EXISTING link, so calling it
 * per frame cannot disturb a handshake in progress.
 */
/* Per-link AID for the Confirm: the link's slot, 1-based. Non-zero (AID 0 is
 * the group key) and distinct per peer, mirroring what wpa_supplicant assigns.
 * Returns 1 for an unknown link rather than 0 -- a wrong-but-valid AID still
 * peers; a zero one does not. */
static uint16_t mpm_aid_for_(const struct mpm_link *l)
{
    if (l == NULL)
    {
        return 1;
    }
    long idx = (long)(l - &s_mpm.links[0]);
    if (idx < 0 || idx >= MPM_MAX_LINKS)
    {
        return 1;
    }
    return (uint16_t)(idx + 1);
}

static uint16_t mpm_mint_llid_(void)
{
    uint16_t v = (uint16_t)mmhal_random_u32(1, 0xfffe);
    return v ? v : (uint16_t)0x1234;
}

static void mpm_publish_(const struct mpm_link *l);

/* Expire links we have stopped hearing from, tearing down the datapath peer
 * for each. Driven from the probe path rather than a timer: peers probe every
 * 2 s, so this runs often enough and costs nothing when idle. */
static void mpm_expire_stale_(uint32_t now_ms)
{
    uint8_t gone[MPM_MAX_LINKS][MPM_ADDR_LEN];
    int n = mpm_table_expire(&s_mpm, now_ms, MPM_PEER_TIMEOUT_MS, gone, MPM_MAX_LINKS);
    for (int i = 0; i < n; i++)
    {
        umac_datapath_mesh_del_peer(gone[i]);
    }
    if (n > 0)
    {
        g_warthog_mpm_expired = s_mpm.expired;
        mpm_publish_(NULL);
    }
}

/* Mirror the aggregate view the AT counters expose. `estab` is the number of
 * established links, so the two-board case still reads estab=1. */
static void mpm_publish_(const struct mpm_link *l)
{
    if (l != NULL)
    {
        g_warthog_mpm_our_llid = l->llid;
        g_warthog_mpm_our_plid = l->plid;
    }
    g_warthog_mpm_estab = mpm_table_estab_count(&s_mpm);
    g_warthog_mpm_no_slot = s_mpm.no_slot;
    g_warthog_mpm_expired = s_mpm.expired;

    /* Render into main's buffer for AT+MPMPEERS?. Pushed rather than exposed
     * as a getter: morselib is a static archive, and the linker will not
     * extract an object merely to satisfy a call from main. */
    mpm_table_render(&s_mpm, (char *)g_warthog_mpm_links, sizeof(g_warthog_mpm_links));
}

static int mesh_tx_mpm_(const uint8_t *da, uint8_t action, uint16_t llid, uint16_t plid,
                        uint16_t reason, uint16_t aid)
{
    if (s_mesh_umacd == NULL || !s_mesh_args_valid || da == NULL)
    {
        return -1;
    }

    /* S1G Capabilities. The far side's Morse driver validates this on every
     * peering frame -- without it the receiving driver logs
     * "S1G capabilities mismatch - fc 0x00d0" and the frame never reaches
     * wpa_supplicant, so a peer answers our Open but never sees our Confirm.
     * Built with morselib's own element builder so it describes this chip
     * rather than a hand-rolled guess. */
    uint8_t s1g_caps[64];
    struct consbuf cb;
    consbuf_reinit(&cb, s1g_caps, sizeof(s1g_caps));
    ie_s1g_capabilities_build(s_mesh_umacd, &cb);
    uint16_t s1g_caps_len = (uint16_t)cb.offset;

    uint8_t body[UMAC_MESH_MPM_BODY_MAXLEN + sizeof(s1g_caps)];
    uint16_t n = umac_mesh_ies_build_mpm_body(body, (uint16_t)sizeof(body), action, llid, plid, reason, aid,
                                              s_mesh_args.mesh_id, s_mesh_args.mesh_id_len,
                                              s_mesh_args.security_type == MMWLAN_SAE,
                                              s1g_caps, s1g_caps_len);
    if (n == 0)
    {
        return -2; /* unsupported action (CLOSE) or bad arguments */
    }

    struct frame_data_action act = {
        .bssid = s_mesh_own_addr, /* mesh: BSSID = our own address */
        .dst_address = da,
        .src_address = s_mesh_own_addr,
        .action_field = body,
        .action_field_len = n,
    };

    /* Stash the exact bytes for AT+MPMDUMP?. This driver cannot capture frames
     * on air (NL80211_IFTYPE_MONITOR is absent from its interface_modes, so iw
     * monitor mode silently captures nothing), and the remaining MPM failure
     * can only be diagnosed by diffing our Confirm against a Linux-generated
     * one field by field. Dumping what we build is the only way to get there. */
    if (action == MPM_ACTION_CONFIRM)
    {
        uint16_t cp = (n < sizeof(g_warthog_mpm_dump)) ? n : (uint16_t)sizeof(g_warthog_mpm_dump);
        memcpy((void *)g_warthog_mpm_dump, body, cp);
        g_warthog_mpm_dump_len = cp;
        g_warthog_mpm_dump_full = n;
    }

    struct mmpkt *frm = build_mgmt_frame(s_mesh_umacd, frame_action_build, &act);
    if (frm == NULL)
    {
        return -3;
    }

    struct mmdrv_tx_metadata *tx_md = mmdrv_get_tx_metadata(frm);
    tx_md->flags = MMDRV_TX_FLAG_IMMEDIATE_REPORT;
    tx_md->tid = MMWLAN_MAX_QOS_TID;
    tx_md->vif_id = s_mesh_vif_id;
    umac_rc_init_rate_table_mgmt(s_mesh_umacd, &tx_md->rc_data, false);

    return mmdrv_tx_frame(frm, /*is_mgmt=*/true);
}

/* Our HWMP sequence number and path-discovery id. Both are ours alone to
 * increment; a peer only ever compares them against what it last saw from us,
 * so they must be monotonic across the life of the link and must never be
 * reset while a peer still holds a path to us. */
static uint32_t s_hwmp_sn;
static uint32_t s_hwmp_preq_id;
static uint32_t s_hwmp_last_preq_ms;

/* Send an HWMP action frame. Same recipe as mesh_tx_mpm_, including the S1G
 * Capabilities element: the far side's Morse driver validates that on action
 * frames and drops the ones that lack it, which is what made our Confirms
 * vanish before. A mac80211 peer parses elements generically and length-checks
 * only the PREQ/PREP element, so the extra element is harmless to Linux and
 * required by a warthog peer. */
/* Transmit an arbitrary action frame to a mesh peer.
 *
 * Public because hostap's mesh MPM needs it: mesh_mpm.c builds PLINK
 * Open/Confirm/Close bodies itself and sends them through the driver's
 * .send_action op, which has nowhere else to go on this port.
 *
 * S1G Capabilities is appended here, not by the caller. The Morse driver on
 * the far side validates that element on peering frames and drops the ones
 * that lack it -- which is true of another warthog and of an OpenMANET node
 * alike, since both run that driver. hostap has no idea it needs to emit an
 * S1G element, so the port supplies it. A mac80211 peer parses elements
 * generically and ignores the extra one.
 */
/* Transmit a fully built management frame on the mesh VIF.
 *
 * hostap hands send_mlme a complete 802.11 frame (SAE Authentication, in
 * practice). Sending it through umac_datapath_tx_mgmt_frame_ap -- the AP
 * path -- put it on metadata the mesh VIF never validated, and whether those
 * frames ever left the antenna was unprovable. This uses the exact metadata
 * recipe of mesh_tx_hwmp_(), whose frames demonstrably fly between boards.
 */
int umac_mesh_tx_mgmt_mmpkt(struct mmpkt *frm)
{
    if (s_mesh_umacd == NULL || !s_mesh_args_valid || frm == NULL)
    {
        return -1;
    }
    struct mmdrv_tx_metadata *tx_md = mmdrv_get_tx_metadata(frm);
    tx_md->flags = MMDRV_TX_FLAG_IMMEDIATE_REPORT;
    tx_md->tid = MMWLAN_MAX_QOS_TID;
    tx_md->vif_id = s_mesh_vif_id;
    umac_rc_init_rate_table_mgmt(s_mesh_umacd, &tx_md->rc_data, false);
    return mmdrv_tx_frame(frm, /*is_mgmt=*/true);
}

static int mesh_tx_action_raw_(const uint8_t *da, const uint8_t *body, uint16_t body_len);

int umac_mesh_tx_action(const uint8_t *da, const uint8_t *body, uint16_t body_len)
{
    /* Category 15 (Self-protected) bodies must go out BYTE-FOR-BYTE.
     *
     * mesh_tx_hwmp_ appends an S1G Capabilities element, which the vendor MPM
     * peer wants on peering frames. Under AMPE that is destructive: hostap
     * ends an Open/Confirm with the AMPE element, whose MIC covers the body,
     * so anything appended afterwards invalidates it and the peer rejects the
     * peering. Symptom: plink advances OPN_SNT -> HOLDING and never reaches
     * ESTAB. HWMP (category 13) still gets the element. */
    if (body_len >= 1 && body[0] == 15u)
    {
        return mesh_tx_action_raw_(da, body, body_len);
    }
    return mesh_tx_hwmp_(da, body, body_len);
}

static int mesh_tx_hwmp_(const uint8_t *da, const uint8_t *body, uint16_t body_len)
{
    if (s_mesh_umacd == NULL || !s_mesh_args_valid || da == NULL || body == NULL || body_len == 0)
    {
        return -1;
    }

    uint8_t s1g_caps[64];
    struct consbuf cb;
    consbuf_reinit(&cb, s1g_caps, sizeof(s1g_caps));
    ie_s1g_capabilities_build(s_mesh_umacd, &cb);
    uint16_t s1g_caps_len = (uint16_t)cb.offset;

    /* Sized for the LARGEST action body this path carries, which is not a
     * HWMP PREQ. hostap's MPM peering frames (Open/Confirm) carry the peering
     * elements plus AMPE's encrypted key blob and run several hundred bytes;
     * with a HWMP_PREQ_BODY_LEN + caps budget (105 B) every one of them
     * fails the bounds check below and returns before reaching the radio --
     * SAE completes on both peers and peering silently never starts. */
    uint8_t frame[UMAC_MESH_ACTION_BODY_MAX + sizeof(s1g_caps)];
    if ((uint32_t)body_len + s1g_caps_len > sizeof(frame))
    {
        g_warthog_mesh_act_oversize++;
        return -2;
    }
    memcpy(frame, body, body_len);
    memcpy(frame + body_len, s1g_caps, s1g_caps_len);

    struct frame_data_action act = {
        .bssid = s_mesh_own_addr,
        .dst_address = da,
        .src_address = s_mesh_own_addr,
        .action_field = frame,
        .action_field_len = (uint16_t)(body_len + s1g_caps_len),
    };

    struct mmpkt *frm = build_mgmt_frame(s_mesh_umacd, frame_action_build, &act);
    if (frm == NULL)
    {
        return -3;
    }

    struct mmdrv_tx_metadata *tx_md = mmdrv_get_tx_metadata(frm);
    tx_md->flags = MMDRV_TX_FLAG_IMMEDIATE_REPORT;
    tx_md->tid = MMWLAN_MAX_QOS_TID;
    tx_md->vif_id = s_mesh_vif_id;
    umac_rc_init_rate_table_mgmt(s_mesh_umacd, &tx_md->rc_data, false);

    return mmdrv_tx_frame(frm, /*is_mgmt=*/true);
}

/* Ask @p da for a path to itself.
 *
 * Emitting this is what makes US reachable: a peer that accepts a PREQ installs
 * a path to the ORIGINATOR, so the request does double duty. Targeting the peer
 * itself (rather than the broadcast address, which selects the peer's
 * proactive-root branch) also makes it answer with a PREP. */
int umac_mesh_hwmp_send_preq(const uint8_t *da)
{
    uint8_t body[HWMP_PREQ_BODY_LEN];
    uint16_t n = umac_mesh_hwmp_build_preq(body, sizeof(body), s_mesh_own_addr, ++s_hwmp_sn,
                                           ++s_hwmp_preq_id, da, UMAC_MESH_HWMP_LIFETIME_TU);
    if (n == 0)
    {
        return -1;
    }
    int rc = mesh_tx_hwmp_(da, body, n);
    if (rc >= 0)
    {
        g_warthog_hwmp_preq_tx++;
    }
    return rc;
}

/* Handle a received mesh action frame (category 13).
 *
 * Answer a PREQ that targets us with a PREP; a PREQ for anyone else is counted
 * and ignored, because warthog does not forward. */
void umac_mesh_handle_hwmp(const uint8_t *body, uint16_t len, const uint8_t *ta)
{
    struct hwmp_preq preq;

    g_warthog_hwmp_rx++;
    {
        /* Snapshot whatever category 13 actually delivers. parse_fail counting
         * every frame while the link works says the shape is not what we
         * assumed, and there is no other way to see it on this board. */
        uint16_t cp = (len < sizeof(g_warthog_hwmp_dump)) ? len
                                                          : (uint16_t)sizeof(g_warthog_hwmp_dump);
        memcpy((void *)g_warthog_hwmp_dump, body, cp);
        g_warthog_hwmp_dump_len = cp;
        g_warthog_hwmp_dump_full = len;
    }
    /* A PREP is the peer ANSWERING one of our PREQs -- it means the path we
     * asked for exists. Count it as the success it is; lumping it into
     * parse_fail reads as "we cannot decode anything" on a link that is in
     * fact working. We do not act on it: mac80211 installs the path to us
     * from our own PREQ's originator block, which is the whole point of
     * emitting one. */
    if (len >= 3u && body[2] == HWMP_EID_PREP)
    {
        g_warthog_hwmp_prep_rx++;
        return;
    }
    if (!umac_mesh_hwmp_parse_preq(body, len, &preq))
    {
        g_warthog_hwmp_parse_fail++;
        return;
    }
    g_warthog_hwmp_preq_rx++;
    if (!umac_mesh_hwmp_targets_us(&preq, s_mesh_own_addr))
    {
        g_warthog_hwmp_not_ours++;
        return; /* we do not forward, so a PREQ for a third party is not ours */
    }

    /* Our sequence number must stay ahead of anything the peer has recorded
     * for us, or it treats our PREP as stale and keeps discovering -- but only
     * where the peer actually has a value. See umac_mesh_hwmp_next_own_sn(). */
    s_hwmp_sn = umac_mesh_hwmp_next_own_sn(s_hwmp_sn, &preq);

    uint8_t prep[HWMP_PREP_BODY_LEN];
    uint16_t n = umac_mesh_hwmp_build_prep(prep, sizeof(prep), &preq, s_mesh_own_addr, s_hwmp_sn);
    if (n == 0)
    {
        return;
    }
    /* Reply to the transmitter, which for a one-hop peer is the originator. */
    if (mesh_tx_hwmp_(ta != NULL ? ta : preq.orig_addr, prep, n) >= 0)
    {
        g_warthog_hwmp_prep_tx++;
    }
}

/* Pull the reason code out of a Close frame's Peer Management IE.
 *
 * mac80211 states WHY it refused the peering here -- 802.11 reason codes 52-60
 * (MESH_MAX_PEERS, MESH_CONFIG_POLICY_VIOLATION, MESH_CONFIRM_TIMEOUT,
 * MESH_INCONSISTENT_PARAMS ...). That is far more informative than guessing at
 * which IE field it dislikes, and there is no way to capture the frame on air
 * with this driver. Reason is the last two octets of the PM IE. */

/* Locate the Peer Management IE and pull out the sender's link id. */

/* Second link-id field of a Confirm: the sender's view of OUR llid. */

/* Proactively open a peering with a node we have just heard from.
 *
 * warthog-to-warthog needs this: both ends only ever ANSWERED an Open, so two
 * warthog boards sat waiting for each other forever. mac80211 initiates from
 * mesh_neighbour_update when auto_open_plinks is set; this is the equivalent.
 * Driven off the peer's probe request (every 2 s), which doubles as the
 * retransmit timer -- MPM tolerates duplicate Opens. */
void umac_mesh_maybe_initiate_mpm(const uint8_t *ta)
{
    if (s_mesh_umacd == NULL || !s_mesh_args_valid || ta == NULL)
    {
        return;
    }
    const uint32_t now_ms = (uint32_t)mmosal_get_time_ms();
    mpm_expire_stale_(now_ms);
    umac_datapath_mesh_service_rekey(); /* AT+REKEY=<n>, serviced here */

    /* Validate AES-CCM through the shipping mbedtls path, once. The host test
     * links hostap's software AES, so this is the only thing that exercises
     * crypto_mbedtls_mm.c -- whose aes_encrypt_init() built a DECRYPTION key
     * schedule until recently, which would make every result silently wrong. */
    if (g_warthog_ccmp_kat_ran == 0)
    {
        umac_mesh_ccmp_kat_run();
    }

    /* AT+CRYPTOHOST=<0|1> / ? -- same flag-and-service pattern, because main
     * cannot call into the morselib archive directly. Set, then read back:
     * a firmware that ignores an unknown parameter can still answer the SET
     * with success, so only the read-back says whether it took. */
    {
        uint32_t req = g_warthog_cryptohost_req;
        if (req != 0)
        {
            g_warthog_cryptohost_req = 0;
            uint32_t val = 0xffffffffu;
            int rc = 0;
            if (req == 3)
            {
                rc = mmdrv_get_crypto_in_host(s_mesh_vif_id, &val);
            }
            else
            {
                rc = mmdrv_set_crypto_in_host(s_mesh_vif_id, req == 1, &val);
                if (rc == 0)
                {
                    (void)mmdrv_get_crypto_in_host(s_mesh_vif_id, &val);
                }
            }
            g_warthog_cryptohost_rc = (uint32_t)rc;
            g_warthog_cryptohost_val = val;
            g_warthog_cryptohost_done++;
        }
    }

    /* Keep a path to us alive at every established peer.
     *
     * The peer's path expires after dot11MeshHWMPactivePathTimeout (5 s by
     * default) and, once it has, the peer silently stops sending us unicast --
     * broadcast keeps working, so the link looks up while nothing routes. Our
     * own PREQ refreshes it: a peer installs a path to any PREQ originator it
     * accepts, so this is what keeps warthog reachable without waiting to be
     * asked. Driven off the probe cadence, which is warthog's own clock, not
     * the peer's -- a peer that goes quiet is exactly when this must not stop. */
    {
        const uint32_t now = (uint32_t)mmosal_get_time_ms();
        if (s_hwmp_last_preq_ms == 0u ||
            (uint32_t)(now - s_hwmp_last_preq_ms) >= UMAC_MESH_HWMP_PREQ_PERIOD_MS)
        {
            s_hwmp_last_preq_ms = now;
            for (int i = 0; i < MPM_MAX_LINKS; i++)
            {
                struct mpm_link *pl = &s_mpm.links[i];
                if (pl->used && pl->estab)
                {
                    (void)umac_mesh_hwmp_send_preq(pl->addr);
                }
            }
        }
    }

    struct mpm_link *l = mpm_table_get_or_create(&s_mpm, ta, mpm_mint_llid_(), (uint32_t)mmosal_get_time_ms());
    if (l == NULL)
    {
        return; /* table full -- a real mesh would answer Close(MESH_MAX_PEERS) */
    }
    l->last_heard_ms = now_ms;
    if (l->estab)
    {
        return; /* already peered with THIS neighbour; others still open freely */
    }
    /* A peer that already holds an ESTAB link to us ignores Opens carrying a
     * new link id, so a node that restarted while its neighbour did not will
     * retry forever against a peer that considers the link up. That is not
     * hypothetical: it is what warthog does after a reflash, and it left the
     * peering established on one side and stuck at plid=0 on ours.
     *
     * After MPM_OPEN_RETRY_MAX unanswered Opens, send Close so the peer drops
     * its half and runs the handshake again, then forget the link so the next
     * beacon is a first sighting and re-opens with a fresh llid. Close carries
     * our llid in the plid position: that is the id the peer recorded for us,
     * and it is all we can cite while our own plid is still unknown. */
    if (l->opens >= MPM_OPEN_RETRY_MAX)
    {
        if (mesh_tx_mpm_(ta, MPM_ACTION_CLOSE, 0, l->llid, MPM_REASON_PEER_CANCELED, 0) >= 0)
        {
            g_warthog_mpm_close_tx++;
        }
        mpm_table_release(&s_mpm, l);
        return;
    }
    if (mesh_tx_mpm_(ta, MPM_ACTION_OPEN, l->llid, 0, 0, 0) >= 0)
    {
        g_warthog_mpm_open_tx++;
        l->opens++;
    }
    mpm_publish_(l);
}

void umac_mesh_handle_mpm(const uint8_t *ta, const uint8_t *body, uint32_t len)
{
    if (ta == NULL || body == NULL || len < 4)
    {
        return;
    }

    uint8_t action = body[1];
    uint16_t peer_llid = 0;
    /* Track parse success: a silent failure here would leave peer_llid at 0 and
     * make our Confirm carry plid=0, which mac80211 answers with CNF_IGNR
     * (it requires frame plid == its own llid) -- leaving the peer stuck in
     * OPN_RCVD exactly as observed. Surfaced via AT+MPMSTAT?. */
    if (!umac_mesh_ies_get_peer_llid(body, len, &peer_llid))
    {
        g_warthog_mpm_parse_fail++;
    }

    g_warthog_mpm_rx++;
    {
        struct mpm_link *heard = mpm_table_find(&s_mpm, ta);
        if (heard != NULL)
        {
            heard->last_heard_ms = (uint32_t)mmosal_get_time_ms();
        }
    }

    if (action == MPM_ACTION_OPEN)
    {
        /* Their llid is our plid. If it failed to parse, stay quiet: a Confirm
         * carrying plid=0 is answered with CNF_IGNR (mac80211 requires the
         * frame's plid to equal its own llid), so sending one would burn the
         * peer's 100 ms confirm window on a frame guaranteed to be dropped. */
        if (peer_llid == 0)
        {
            return;
        }
        struct mpm_link *l = mpm_table_get_or_create(&s_mpm, ta, mpm_mint_llid_(), (uint32_t)mmosal_get_time_ms());
        if (l == NULL)
        {
            /* Table full. Say so rather than dropping the Open in silence:
             * 802.11s expects Close(MESH-MAX-PEERS), and a peer that gets no
             * answer at all just retries until it times out, having learned
             * nothing. plid is the requester's llid; ours is 0, we have none. */
            if (peer_llid != 0 &&
                mesh_tx_mpm_(ta, MPM_ACTION_CLOSE, 0, peer_llid,
                             UMAC_MESH_REASON_MAX_PEERS, 0) >= 0)
            {
                g_warthog_mpm_close_tx++;
            }
            g_warthog_mpm_no_slot = s_mpm.no_slot;
            return;
        }
        /* A peer that restarted returns with a fresh llid -- tear the link down
         * so the full handshake runs again rather than answering for a dead
         * one. Only THIS link is affected; other neighbours stay up. */
        if (l->estab && peer_llid != l->plid)
        {
            umac_datapath_mesh_del_peer(ta);
            l->estab = false;
        }

        l->plid = peer_llid;
        l->opens = 0; /* the peer answered: it is not holding a stale link */

        /* Open first, then Confirm. Confirm-first was tried on the theory that
         * the Confirm is the latency-critical frame; it measured worse (peer
         * stalled in OPN_SNT where Open-first reliably reached OPN_RCVD). The
         * peer's mesh_confirm_timeout is only 100 ms, so neither may be delayed.
         *
         * Once ESTAB, answer with the Confirm ALONE. Replying to an Open with
         * our own Open makes the peer reply with its Open, which makes us reply
         * again -- a symmetric loop with no terminating condition, measured at
         * ~94 frames/s of pure MPM between two established boards. mac80211's
         * FSM does the same thing: OPN_ACPT in ESTAB emits a Confirm, not an
         * Open. The peer is already established, so it quiesces on our Confirm
         * and the exchange stops. */
        if (!l->estab)
        {
            if (mesh_tx_mpm_(ta, MPM_ACTION_OPEN, l->llid, 0, 0, 0) >= 0)
            {
                g_warthog_mpm_open_tx++;
            }
        }
        if (mesh_tx_mpm_(ta, MPM_ACTION_CONFIRM, l->llid, l->plid, 0, mpm_aid_for_(l)) >= 0)
        {
            g_warthog_mpm_conf_tx++;
        }
        mpm_publish_(l);
    }
    else if (action == MPM_ACTION_CONFIRM)
    {
        g_warthog_mpm_conf_rx++;

        /* Their Confirm echoes our llid in the plid field. If it matches, the
         * peer has accepted us and the link is up from our side -- this is the
         * ESTAB indicator, since warthog has no station table to inspect. */
        struct mpm_link *l = mpm_table_get_or_create(&s_mpm, ta, mpm_mint_llid_(), (uint32_t)mmosal_get_time_ms());
        if (l == NULL)
        {
            return;
        }

        uint16_t echoed = 0;
        if (umac_mesh_ies_get_peer_plid(body, len, &echoed) && echoed == l->llid &&
            l->llid != 0)
        {
            if (!l->estab)
            {
                /* Link is up: hand the peer to the data plane. From here the
                 * vendor datapath carries 4-address data frames both ways. */
                enum mmwlan_status st = umac_datapath_mesh_add_peer(
                    s_mesh_umacd, s_mesh_vif_id, s_mesh_own_addr, ta,
                    s_mesh_args.security_type == MMWLAN_SAE);
                if (st != MMWLAN_SUCCESS)
                {
                    g_warthog_mesh_peer_add_fail++;
                }
            }
            l->estab = true;
        }

        /* The peer retransmits Confirm while it waits for OURS. Measured:
         * conf_rx=4 against conf_tx=1 -- we answered once and then went quiet,
         * so a single lost or too-early Confirm stalls the handshake forever
         * and the peer eventually times back out to LISTEN.
         *
         * Re-send our Confirm on every one of theirs. This is the retransmit
         * behaviour a real MPM timer provides, without needing a timer here. */
        if (peer_llid != 0)
        {
            l->plid = peer_llid;
        l->opens = 0; /* the peer answered: it is not holding a stale link */
        }

        /* Answer only while the link is still coming up. Retransmitting is
         * what rescued the handshake (the peer resends Confirm while waiting),
         * but once ESTAB it becomes a feedback loop: each side answers the
         * other's Confirm forever, measured at ~80 frames/s of pure MPM. Real
         * MPM quiesces after ESTAB, so stop once the peer has echoed our llid. */
        if (l->plid != 0 && !l->estab)
        {
            if (mesh_tx_mpm_(ta, MPM_ACTION_CONFIRM, l->llid, l->plid, 0, mpm_aid_for_(l)) >= 0)
            {
                g_warthog_mpm_conf_tx++;
            }
        }
        mpm_publish_(l);
    }
    else if (action == MPM_ACTION_CLOSE)
    {
        uint16_t reason = 0;
        if (umac_mesh_ies_get_close_reason(body, len, &reason))
        {
            g_warthog_mpm_close_reason = reason;
        }
        /* Clear ESTAB too. umac_mesh_maybe_initiate_mpm() early-returns on it
         * and the Confirm retransmit is gated on !estab, so leaving it latched
         * after a Close permanently wedges peering until reboot -- and with no
         * other initiator on warthog<->warthog, nothing would ever reopen it.
         * Safe against the ~80 fps ping-pong: both link ids are zero here, so
         * the quiesce gate re-arms cleanly on the next ESTAB. */
        umac_datapath_mesh_del_peer(ta);
        mpm_table_release(&s_mpm, mpm_table_find(&s_mpm, ta));
        g_warthog_mpm_our_llid = 0;
        g_warthog_mpm_our_plid = 0;
        g_warthog_mpm_estab = mpm_table_estab_count(&s_mpm);
        g_warthog_mpm_close_rx++;
    }
}

uint8_t umac_mesh_get_peer_count(struct umac_data *umacd)
{
    (void)umacd;
    return umac_datapath_mesh_peer_count();
}



/* Does this beacon body advertise OUR mesh? Wraps the freestanding matcher so
 * the datapath does not need the mesh args. */
bool umac_mesh_beacon_is_our_mesh(const uint8_t *body, uint32_t len)
{
    if (!s_mesh_args_valid)
    {
        return false;
    }
    return umac_mesh_ies_beacon_has_mesh_id(body, len, s_mesh_args.mesh_id,
                                            s_mesh_args.mesh_id_len);
}

/* Does this S1G Beacon advertise OUR mesh? Wraps the freestanding matcher so
 * the datapath does not need the mesh args. */
bool umac_mesh_s1g_beacon_is_our_mesh(const uint8_t *frame, uint32_t len)
{
    if (!s_mesh_args_valid)
    {
        return false;
    }
    return umac_mesh_ies_s1g_beacon_has_mesh_id(frame, len, s_mesh_args.mesh_id,
                                                s_mesh_args.mesh_id_len);
}

/* First sight of this peer? Refreshes its liveness either way.
 *
 * A beacon must NOT be answered with a probe response every time. mac80211
 * raises NL80211_CMD_NEW_PEER_CANDIDATE for each probe response from a station
 * it has no plink for, and wpa_supplicant answers a candidate by starting a
 * FRESH peering -- tearing down the one already in progress with
 * Close(reason 52, MESH-PEER-CANCELED) and a new link id. Answering every
 * beacon therefore livelocks the handshake: captured on air as an endless
 * Open/Confirm/Close cycle where both ends' link ids changed every second and
 * the peer never left OPN_RCVD, with every individual frame verifiably
 * correct.
 *
 * Answer the first beacon, then stay quiet and let the ordinary MPM path
 * retransmit.
 */
/* Forget every peer link, so the next beacon from each is treated as a first
 * sighting and the peering runs again from LISTEN.
 *
 * The MPM table IS the "have I seen this peer" state, so clearing it re-arms
 * the announce-once logic too. The peer must be restarted alongside: a node
 * that still holds an ESTAB link ignores Opens carrying a new link id, which
 * is what leaves one side established and the other at plid=0.
 */
void umac_mesh_reset_links(void)
{
    mpm_table_init(&s_mpm);
}

/* True when the mesh was brought up with SAE, i.e. hostap's MPM owns peering
 * and warthog's own MPM must not intercept SELF_PROTECTED frames. */
/* Register a datapath peer on hostap's behalf.
 *
 * Under SAE, hostap's MPM -- not ours -- decides when a peer exists, and it
 * does so through .sta_add. The vif id and our own mesh address are file
 * statics here, so the driver op cannot build the call itself. */
enum mmwlan_status umac_mesh_add_datapath_peer(const uint8_t *peer_addr)
{
    if (!s_mesh_args_valid || peer_addr == NULL)
    {
        return MMWLAN_INVALID_ARGUMENT;
    }
    return umac_datapath_mesh_add_peer(s_mesh_umacd, s_mesh_vif_id, s_mesh_own_addr, peer_addr,
                                       s_mesh_args.security_type == MMWLAN_SAE);
}

const uint8_t *umac_mesh_get_shared_bssid(void)
{
    return s_mesh_shared_bssid_valid ? s_mesh_shared_bssid : NULL;
}

bool umac_mesh_sae_active(void)
{
    return s_mesh_args_valid && s_mesh_args.security_type == MMWLAN_SAE;
}

bool umac_mesh_note_s1g_beacon(const uint8_t *sa)
{
    if (sa == NULL)
    {
        return false;
    }
    struct mpm_link *l = mpm_table_find(&s_mpm, sa);
    if (l != NULL)
    {
        l->last_heard_ms = (uint32_t)mmosal_get_time_ms();
        return false; /* already known -- do not re-announce ourselves */
    }
    return true;
}

/* Is a peering with this peer still unfinished? Used to retransmit our Open.
 *
 * Announcing ourselves once is right for the PROBE RESPONSE -- repeating that
 * makes mac80211 raise NEW_PEER_CANDIDATE and restart the peering. It is wrong
 * for the Open: with no retransmission a single lost Open strands the link
 * forever, which is exactly what happened -- the peer reached ESTAB from an
 * earlier attempt while our side sat at plid=0 with nothing to drive it.
 * mac80211 tolerates repeated Opens (it answers each with a Confirm), and the
 * reference port retransmits in OPN_SNT/OPN_RCVD/CNF_RCVD for the same reason.
 */
bool umac_mesh_peering_incomplete(const uint8_t *sa)
{
    struct mpm_link *l = mpm_table_find(&s_mpm, sa);
    return l != NULL && !l->estab;
}
