/*
 * Copyright 2021-2025 Morse Micro
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 */

#include <errno.h>

#include "common/common.h"
#include "common/morse_commands.h"
#include "common/morse_command_utils.h"
#include "common/morse_error.h"
#include "common/mac_address.h"
#include "mmdrv.h"
#include "mmutils.h"
#include "mmwlan.h"
#include "driver/morse_driver/firmware.h"
#include "driver/morse_driver/skb_header.h"
#include "driver/morse_driver/command.h"
#include "driver/morse_driver/ps.h"
#include "driver/morse_driver/hw.h"
#include "driver/transport/morse_transport.h"
#include "driver.h"
#include "driver/beacon/beacon.h"
#include "mmhal_wlan.h"

#ifdef ENABLE_DRV_TRACE
#include "mmtrace.h"
static mmtrace_channel drv_channel_handle;
#define DRV_TRACE_INIT()     drv_channel_handle = mmtrace_register_channel("drv")
#define DRV_TRACE(_fmt, ...) mmtrace_printf(drv_channel_handle, _fmt, ##__VA_ARGS__)
#else
#define DRV_TRACE_INIT() \
    do {                 \
    } while (0)
#define DRV_TRACE(_fmt, ...) \
    do {                     \
    } while (0)
#endif


SPINLOCK_TRACE_DECLARE


static struct driver_data driver_data;


extern bool morse_caps_supported(const struct morse_caps *caps, enum morse_caps_flags flag);

void mmdrv_pre_init(void)
{
    morse_trns_init();
}

void mmdrv_post_deinit(void)
{
    morse_trns_deinit();
}

static void morse_stale_tx_status_timer_cb(struct mmosal_timer *timer)
{
    struct driver_data *driverd = (struct driver_data *)mmosal_timer_get_arg(timer);
    MMOSAL_ASSERT(driverd == &driver_data);

    if (!driverd || !driverd->stale_status.enabled)
    {
        return;
    }

    driver_task_notify_event(driverd, DRV_EVT_STALE_TX_STATUS_PEND);
}

static enum mmwlan_status morse_stale_tx_status_timer_init(struct driver_data *driverd)
{
    driverd->stale_status.timer = mmosal_timer_create("stale_status_timer",
                                                      morse_skbq_get_tx_status_lifetime_ms(),
                                                      false,
                                                      (void *)(uintptr_t)driverd,
                                                      morse_stale_tx_status_timer_cb);

    if (driverd->stale_status.timer == NULL)
    {
        MMLOG_ERR("Failed to init stale_status_timer\n");
        return MMWLAN_ERROR;
    }

    driverd->stale_status.enabled = 1;

    return MMWLAN_SUCCESS;
}

static int morse_stale_tx_status_timer_finish(struct driver_data *driverd)
{
    if (!driverd->stale_status.enabled)
    {
        return 0;
    }

    driverd->stale_status.enabled = 0;

    mmosal_timer_delete(driverd->stale_status.timer);
    driverd->stale_status.timer = NULL;

    return 0;
}


static enum mmwlan_status mmdrv_fetch_fw_version(struct driver_data *driverd,
                                                 struct mmdrv_fw_version *fw_version)
{
    enum mmwlan_status status;
    char version_string[MMWLAN_FW_VERSION_MAXLEN] = { 0 };
    int major;
    int minor;
    int patch;


    size_t max_version_resp_size =
        sizeof(struct morse_cmd_resp_get_version) +
        (sizeof(((struct morse_cmd_resp_get_version *)0)->version[0]) * MORSE_CMD_MAX_VERSION_LEN +
         1);


    struct morse_cmd_resp_get_version *resp =
        (struct morse_cmd_resp_get_version *)mmosal_malloc(max_version_resp_size);
    if (resp == NULL)
    {
        return MMWLAN_NO_MEM;
    }

    struct morse_cmd_req_get_version cmd =
        MORSE_COMMAND_INIT(cmd, MORSE_CMD_ID_GET_VERSION, MMDRV_VIF_ID_INVALID);

    status = morse_cmd_tx(driverd,
                          (struct morse_cmd_resp *)resp,
                          (struct morse_cmd_req *)&cmd,
                          max_version_resp_size,
                          0,
                          false);
    if (status == MMWLAN_SUCCESS)
    {
        uint32_t length = MM_MIN(resp->length, (int32_t)(sizeof(version_string) - 1));
        memcpy(version_string, resp->version, length);
        version_string[length] = '\0';
    }
    else
    {
        MMLOG_ERR("Get version failed\n");
    }

    mmosal_free(resp);

    if (status != MMWLAN_SUCCESS)
    {
        return status;
    }

    MMLOG_INF("Chip raw firmware version: %s\n", version_string);

    if (sscanf(version_string, "rel_%d_%d_%d", &major, &minor, &patch) != 3)
    {
        MMLOG_ERR("Unreleased FW version detected: %s\n", version_string);
        major = 0;
        minor = 0;
        patch = 0;
    }

    if ((major > UINT8_MAX) || (minor > UINT8_MAX) || (patch > UINT8_MAX))
    {
        MMLOG_ERR("FW version out of range\n");
        return MMWLAN_ERROR;
    }

    fw_version->major = major;
    fw_version->minor = minor;
    fw_version->patch = patch;

    return MMWLAN_SUCCESS;
}


static bool mmdrv_valid_fw_flags(uint32_t firmware_flags)
{
    if ((firmware_flags & MORSE_FW_FLAGS_SUPPORT_S1G) == 0)
    {
        return false;
    }

    if ((firmware_flags & MORSE_FW_FLAGS_SUPPORT_HW_SCAN) == 0)
    {
        return false;
    }

    if ((firmware_flags & MORSE_FW_FLAGS_REPORTS_TX_BEACON_COMPLETION) == 0)
    {
        return false;
    }

    if ((firmware_flags & MORSE_FW_FLAGS_STA_IFACE_MANAGE_SNS_BASELINE) == 0)
    {
        return false;
    }

    if ((firmware_flags & MORSE_FW_FLAGS_STA_IFACE_MANAGE_SNS_INDIV_ADDR_QOS_DATA) == 0)
    {
        return false;
    }

    if ((firmware_flags & MORSE_FW_FLAGS_STA_IFACE_MANAGE_SNS_QOS_NULL) == 0)
    {
        return false;
    }

    return true;
}

enum mmwlan_status mmdrv_init(struct mmdrv_chip_info *chip_info, const char *country_code)
{
    uint8_t *mac_addr = (chip_info != NULL) ? chip_info->mac_addr : NULL;
    enum mmwlan_status status = MMWLAN_ERROR;

    DRV_TRACE_INIT();

    DRV_TRACE("init");

    memset(&driver_data, 0, sizeof(driver_data));

    driver_data.cfg = mmhal_get_chip();
    MMOSAL_ASSERT(driver_data.cfg != NULL);

    driver_data.beacon.vif_id = 0xffff;

    status = errno_to_status(morse_trns_start(&driver_data));
    if (status != MMWLAN_SUCCESS)
    {
        MMLOG_ERR("Transport init failed\n");
        goto error_transport;
    }

    if (driver_data.cfg->enable_sdio_burst_mode)
    {
        driver_data.cfg->enable_sdio_burst_mode(&driver_data);
    }

    MMLOG_DBG("Transport initialised\n");


    if (chip_info != NULL)
    {
        chip_info->morse_chip_id = driver_data.chip_id;
        chip_info->morse_chip_id_string = driver_data.cfg->get_hw_version(chip_info->morse_chip_id);
    }

    MMOSAL_ASSERT(country_code != NULL);
    driver_data.country[0] = country_code[0];
    driver_data.country[1] = country_code[1];
    driver_data.country[2] = '\0';

    status = errno_to_status(
        morse_firmware_init(&driver_data, mmhal_wlan_read_fw_file, mmhal_wlan_read_bcf_file));
    if (status != MMWLAN_SUCCESS)
    {
        MMLOG_ERR("Firmware init failed\n");
        goto error_firmware;
    }

    MMLOG_DBG("Firmware downloaded/booted\n");

    status = errno_to_status(driver_data.cfg->ops->init(&driver_data));
    if (status != MMWLAN_SUCCESS)
    {
        MMLOG_ERR("Pageset init failed\n");
        goto error_pageset;
    }

    MMLOG_DBG("Pagesets initialised\n");

    status = errno_to_status(morse_firmware_parse_extended_host_table(&driver_data, mac_addr));
    if (status != MMWLAN_SUCCESS)
    {
        MMLOG_ERR("Failed to parse extended host table\n");
        goto error_hosttable;
    }

    if (!mmdrv_valid_fw_flags(driver_data.firmware_flags))
    {
        MMLOG_ERR("FW does not have the required capabilities 0x%08x\n",
                  driver_data.firmware_flags);
        MMOSAL_ASSERT(false);
    }

    driver_data.lock = mmosal_mutex_create("driverd");
    if (driver_data.lock == NULL)
    {
        MMLOG_ERR("Mutex creation failed\n");
        status = MMWLAN_NO_MEM;
        goto error_mutex;
    }

    status = errno_to_status(driver_task_start(&driver_data));
    if (status != MMWLAN_SUCCESS)
    {
        MMLOG_ERR("Driver task start failed\n");
        goto error_task;
    }

    status = errno_to_status(morse_cmd_init(&driver_data));
    if (status != MMWLAN_SUCCESS)
    {

        goto error_cmd;
    }


    status = errno_to_status(morse_ps_init(&driver_data));
    if (status != MMWLAN_SUCCESS)
    {
        MMLOG_ERR("Power Save init failed\n");
        goto error_ps;
    }
    MMLOG_DBG("Power Save initialized\n");

    morse_trns_set_irq_enabled(&driver_data, true);

    MMLOG_DBG("SPI IRQ enabled\n");

    driver_data.started = true;

    if (chip_info != NULL)
    {
        status = mmdrv_fetch_fw_version(&driver_data, &chip_info->fw_version);
        if (status != MMWLAN_SUCCESS)
        {
            goto error_fw_version;
        }
    }

    morse_stale_tx_status_timer_init(&driver_data);

    MMLOG_DBG("mmdrv_init success\n");
    DRV_TRACE("init success");

    return MMWLAN_SUCCESS;

error_fw_version:
    morse_ps_deinit(&driver_data);
error_ps:
    morse_cmd_deinit(&driver_data);
error_cmd:
    mmosal_mutex_delete(driver_data.lock);
    driver_data.lock = NULL;
error_mutex:
error_hosttable:
    driver_data.cfg->ops->finish(&driver_data);
error_pageset:
    driver_task_stop(&driver_data);
error_task:
error_firmware:
    morse_trns_stop(&driver_data);
error_transport:
    memset(&driver_data, 0, sizeof(driver_data));
    return status;
}

#ifdef UNIT_TESTS
void mmdrv_init_for_unit_tests(void)
{
    struct mmhal_wlan_pktmem_init_args args = { NULL };
    mmhal_wlan_pktmem_init(&args);
    memset(&driver_data, 0, sizeof(driver_data));
    driver_data.started = true;
}

#endif

void mmdrv_deinit(void)
{
    MMLOG_DBG("\n");

    DRV_TRACE("deinit");


    MMOSAL_ASSERT(driver_data.started);

    driver_data.started = false;
    morse_beacon_teardown(&driver_data); /* warthog: its timer points at what is cleared below */

    morse_stale_tx_status_timer_finish(&driver_data);

    morse_trns_set_irq_enabled(&driver_data, false);

    driver_task_stop(&driver_data);

    mmosal_mutex_delete(driver_data.lock);
    driver_data.lock = NULL;

    morse_cmd_deinit(&driver_data);

    driver_data.cfg->ops->finish(&driver_data);

    morse_trns_stop(&driver_data);

    morse_ps_deinit(&driver_data);

    memset(&driver_data, 0, sizeof(driver_data));

    DRV_TRACE("deinit done");
}

enum mmwlan_status mmdrv_get_bcf_metadata(struct mmwlan_bcf_metadata *metadata)
{
    return errno_to_status(morse_bcf_get_metadata(metadata));
}

enum mmwlan_status mmdrv_set_param(uint16_t vif_id, enum morse_param_id param_id, uint32_t value)
{
    struct morse_cmd_req_get_set_generic_param cmd =
        MORSE_COMMAND_INIT(cmd,
                           MORSE_CMD_ID_GET_SET_GENERIC_PARAM,
                           vif_id,
                           .param_id = param_id,
                           .action = MORSE_CMD_PARAM_ACTION_SET,
                           .value = htole32(value));

    return morse_cmd_tx(&driver_data, NULL, (struct morse_cmd_req *)&cmd, 0, 0, false);
}

enum mmwlan_status mmdrv_set_duty_cycle(uint32_t duty_cycle,
                                        bool duty_cycle_omit_ctrl_resp,
                                        enum mmwlan_duty_cycle_mode mode)
{
    struct morse_cmd_req_set_duty_cycle cmd =
        MORSE_COMMAND_INIT(cmd,
                           MORSE_CMD_ID_SET_DUTY_CYCLE,
                           MMDRV_VIF_ID_INVALID,
                           .config.duty_cycle = htole32(duty_cycle),
                           .config.omit_control_responses = duty_cycle_omit_ctrl_resp ? 1 : 0,
                           .set_cfgs = MORSE_CMD_DUTY_CYCLE_SET_CFG_DUTY_CYCLE |
                                       MORSE_CMD_DUTY_CYCLE_SET_CFG_OMIT_CONTROL_RESP |
                                       MORSE_CMD_DUTY_CYCLE_SET_CFG_EXT,
                           .config_ext.mode = (uint8_t)mode);
    enum mmwlan_status status =
        morse_cmd_tx(&driver_data, NULL, (struct morse_cmd_req *)&cmd, 0, 0, false);
    if (status == MMWLAN_SUCCESS)
    {
        if ((duty_cycle != 10000) && (mode == MMWLAN_DUTY_CYCLE_MODE_SPREAD))
        {
            driver_data.tx_max_pause_time_ms =
                100 * (MMDRV_DUTY_CYCLE_MAX - duty_cycle) / duty_cycle;
        }
        else
        {
            driver_data.tx_max_pause_time_ms = 0;
        }
    }
    return status;

    MM_STATIC_ASSERT((uint8_t)MMWLAN_DUTY_CYCLE_MODE_SPREAD == MORSE_CMD_DUTY_CYCLE_MODE_SPREAD,
                     "enums out of sync");
    MM_STATIC_ASSERT((uint8_t)MMWLAN_DUTY_CYCLE_MODE_BURST == MORSE_CMD_DUTY_CYCLE_MODE_BURST,
                     "enums out of sync");
    MM_STATIC_ASSERT(MORSE_CMD_DUTY_CYCLE_MODE_LAST == MORSE_CMD_DUTY_CYCLE_MODE_BURST,
                     "New modes added, update static assert tests");
}

enum mmwlan_status mmdrv_get_duty_cycle(struct mmwlan_duty_cycle_stats *stats)
{
    MMOSAL_DEV_ASSERT(stats);

    struct MM_PACKED
    {

        struct morse_cmd_header hdr;
    } cmd = MORSE_COMMAND_INIT(cmd, MORSE_CMD_ID_GET_DUTY_CYCLE, MMDRV_VIF_ID_INVALID);
    struct morse_cmd_resp_get_duty_cycle resp = { 0 };
    enum mmwlan_status status = morse_cmd_tx(&driver_data,
                                             (struct morse_cmd_resp *)&resp,
                                             (struct morse_cmd_req *)&cmd,
                                             sizeof(resp),
                                             0,
                                             false);

    if (status != MMWLAN_SUCCESS)
    {
        return status;
    }

    if ((resp.config.omit_control_responses > true) ||
        (resp.config.duty_cycle > MMDRV_DUTY_CYCLE_MAX) ||
        (resp.config.duty_cycle < MMDRV_DUTY_CYCLE_MIN) ||
        (resp.config_ext.set.mode > MORSE_CMD_DUTY_CYCLE_MODE_LAST) ||
        ((resp.config_ext.airtime_remaining_us || resp.config_ext.burst_window_duration_us) &&
         (resp.config_ext.set.mode != MORSE_CMD_DUTY_CYCLE_MODE_BURST)))
    {

        return MMWLAN_ERROR;
    }

    stats->duty_cycle = resp.config.duty_cycle;
    stats->mode = (enum mmwlan_duty_cycle_mode)resp.config_ext.set.mode;
    stats->burst_airtime_remaining_us = resp.config_ext.airtime_remaining_us;
    stats->burst_window_duration_us = resp.config_ext.burst_window_duration_us;
    return status;
}

enum mmwlan_status mmdrv_set_txpower(int32_t *out_power_dbm, int txpower_dbm)
{
    enum mmwlan_status status;
    struct morse_cmd_resp_set_txpower resp;
    struct morse_cmd_req_set_txpower cmd =
        MORSE_COMMAND_INIT(cmd, MORSE_CMD_ID_SET_TXPOWER, MMDRV_VIF_ID_INVALID);


    if (txpower_dbm > 30)
    {
        txpower_dbm = 30;
    }

    cmd.power_qdbm = htole32(DBM_TO_QDBM(txpower_dbm));

    status = morse_cmd_tx(&driver_data,
                          (struct morse_cmd_resp *)&resp,
                          (struct morse_cmd_req *)&cmd,
                          sizeof(resp),
                          0,
                          false);
    if (status == MMWLAN_SUCCESS)
    {
        *out_power_dbm = QDBM_TO_DBM(le32toh(resp.power_qdbm));
    }

    return status;
}

enum mmwlan_status mmdrv_add_if(uint16_t *vif_id,
                                const uint8_t *addr,
                                enum mmdrv_interface_type type)
{
    int32_t chip_status = 0;
    enum mmwlan_status status = mmdrv_add_if_status(vif_id, addr, type, &chip_status);
    if (status == MMWLAN_SUCCESS && chip_status != 0)
    {
        return MMWLAN_COMMAND_ERROR;
    }
    return status;
}

/* morse_cmd_tx returns MMWLAN_COMMAND_ERROR on a refusal but keeps the chip's verdict only in
 * resp.status, which Linux reads (command.c:214); so the chip answering is SUCCESS here. */
enum mmwlan_status mmdrv_add_if_status(uint16_t *vif_id,
                                       const uint8_t *addr,
                                       enum mmdrv_interface_type type,
                                       int32_t *chip_status)
{
    MM_STATIC_ASSERT((int)MMDRV_INTERFACE_TYPE_STA == (int)MORSE_CMD_INTERFACE_TYPE_STA,
                     "MMDRV_INTERFACE_TYPE_STA/MORSE_CMD_INTERFACE_TYPE_STA enum mismatch");
    MM_STATIC_ASSERT((int)MMDRV_INTERFACE_TYPE_AP == (int)MORSE_CMD_INTERFACE_TYPE_AP,
                     "MMDRV_INTERFACE_TYPE_AP/MORSE_CMD_INTERFACE_TYPE_AP enum mismatch");
    MM_STATIC_ASSERT((int)MMDRV_INTERFACE_TYPE_MESH == (int)MORSE_CMD_INTERFACE_TYPE_MESH,
                     "MMDRV_INTERFACE_TYPE_MESH/MORSE_CMD_INTERFACE_TYPE_MESH enum mismatch");

    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    enum morse_cmd_interface_type if_type = MORSE_CMD_INTERFACE_TYPE_INVALID;
    switch (type)
    {
        case MMDRV_INTERFACE_TYPE_STA:
            if_type = MORSE_CMD_INTERFACE_TYPE_STA;
            break;

        case MMDRV_INTERFACE_TYPE_AP:
            if_type = MORSE_CMD_INTERFACE_TYPE_AP;
            break;

        case MMDRV_INTERFACE_TYPE_MESH:
            if_type = MORSE_CMD_INTERFACE_TYPE_MESH;
            break;

        default:
            return MMWLAN_INVALID_ARGUMENT;
    }

    enum mmwlan_status status;
    struct morse_cmd_resp_add_interface resp;
    /* A status-only reply leaves the rest of it unwritten (morse_cmd_tx copies what came). */
    memset(&resp, 0, sizeof(resp));

    struct morse_cmd_req_add_interface cmd = MORSE_COMMAND_INIT(cmd,
                                                                MORSE_CMD_ID_ADD_INTERFACE,
                                                                MMDRV_VIF_ID_INVALID,
                                                                .interface_type = htole32(if_type));

    memcpy(cmd.addr.octet, addr, sizeof(cmd.addr.octet));

    status = morse_cmd_tx(&driver_data,
                          (struct morse_cmd_resp *)&resp,
                          (struct morse_cmd_req *)&cmd,
                          sizeof(resp),
                          0,
                          true);
    if (status == MMWLAN_SUCCESS)
    {
        *vif_id = le16toh(resp.hdr.vif_id);
    }
    else if (status != MMWLAN_COMMAND_ERROR)
    {
        return status;
    }
    if (chip_status != NULL)
    {
        *chip_status = (int32_t)le32toh(resp.status);
    }

    return MMWLAN_SUCCESS;
}

enum mmwlan_status mmdrv_start_beaconing(uint16_t vif_id)
{
    return mmdrv_start_beaconing_period(vif_id, 0);
}

/* @param host_timer_period_ms  0 => rely on the chip's own recurring beacon
 *   IRQ (correct for firmware that self-schedules TBTT). Non-zero => drive
 *   beacons from a host timer at this period, for the MM6108 mesh case where
 *   the chip's beacon IRQ fires only once. */
enum mmwlan_status mmdrv_start_beaconing_period(uint16_t vif_id, uint32_t host_timer_period_ms)
{
    return errno_to_status(morse_beacon_start(&driver_data, vif_id, host_timer_period_ms));
}

enum mmwlan_status mmdrv_stop_beaconing(void)
{
    return errno_to_status(morse_beacon_stop(&driver_data));
}

/* warthog mesh-support fork (step15) — BSSID_SET (opcode 0x0052). The Linux
 * driver issues this in response to mac80211's BSS_CHANGED_BSSID event,
 * which mac80211 raises during mesh interface bring-up. Until now we never
 * configured the chip's BSSID filter at all — the chip would have an
 * implicit default (probably own_addr, set during ADD_INTERFACE), which
 * rejects all peer mesh beacons since their addr3 = peer's own_addr.
 *
 * Caller chooses the BSSID value. For mesh experiments:
 *   - ff:ff:ff:ff:ff:ff (broadcast) — try to put chip into "all-BSS accept"
 *   - 00:00:00:00:00:00 (zero)      — alternate wildcard, some firmwares
 *   - own_addr                       — Linux default (won't help RX directly
 *                                      but pairs with FIF_OTHER_BSS filter
 *                                      flag — which we don't have access to) */
enum mmwlan_status mmdrv_set_bssid(uint16_t vif_id, const uint8_t bssid[6], int32_t *chip_status)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    struct morse_cmd_resp_bssid_set resp;
    memset(&resp, 0, sizeof(resp));
    struct morse_cmd_req_bssid_set cmd =
        MORSE_COMMAND_INIT(cmd, MORSE_CMD_ID_BSSID_SET, vif_id);
    memcpy(cmd.bssid, bssid, sizeof(cmd.bssid));

    enum mmwlan_status status = morse_cmd_tx(&driver_data,
                                             (struct morse_cmd_resp *)&resp,
                                             (struct morse_cmd_req *)&cmd,
                                             sizeof(resp),
                                             0,
                                             true);
    if (status != MMWLAN_SUCCESS && status != MMWLAN_COMMAND_ERROR)
    {
        return status;
    }
    if (chip_status != NULL)
    {
        *chip_status = (int32_t)le32toh(resp.status);
    }
    return MMWLAN_SUCCESS;
}

/* warthog mesh-support fork (step17) — BSS_BEACON_CONFIG. Linux sends it on
 * BSS_CHANGED_BEACON_ENABLED only when beaconing restarts or stops
 * (morse_driver mac.c:4221-4232), never on a VIF's first start. */
enum mmwlan_status mmdrv_cfg_bss_beacon(uint16_t vif_id, bool enable, int32_t *chip_status)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    struct morse_cmd_resp_bss_beacon_config resp;
    memset(&resp, 0, sizeof(resp));
    struct morse_cmd_req_bss_beacon_config cmd =
        MORSE_COMMAND_INIT(cmd,
                           MORSE_CMD_ID_BSS_BEACON_CONFIG,
                           vif_id,
                           .enable = enable ? 1 : 0);

    enum mmwlan_status status = morse_cmd_tx(&driver_data,
                                             (struct morse_cmd_resp *)&resp,
                                             (struct morse_cmd_req *)&cmd,
                                             sizeof(resp),
                                             0,
                                             true);
    if (status != MMWLAN_SUCCESS && status != MMWLAN_COMMAND_ERROR)
    {
        return status;
    }
    if (chip_status != NULL)
    {
        *chip_status = (int32_t)le32toh(resp.status);
    }
    return MMWLAN_SUCCESS;
}

/* warthog mesh-support fork: send MESH_CONFIG (0x0039) to start/stop 802.11s
 * mesh operation on a VIF previously added with MMDRV_INTERFACE_TYPE_MESH.
 *
 * switched from zero-MBCA-fields to the GPL Linux driver
 * defaults (mesh.c:660-668). Specifically:
 *   - mbca_config       = MESH_MBCA_CFG_TBTT_SEL_ENABLE (BIT(0) = 0x01)
 *   - min_beacon_gap_ms = 25  (DEFAULT_MBCA_MIN_BEACON_GAP_MS)
 *   - mbss_start_scan   = 2048 (DEFAULT_MBSS_START_SCAN_DURATION_MS)
 *   - tbtt_adj_interval = 60000 (DEFAULT_TBTT_ADJ_INTERVAL_MSEC)
 *
 * Hypothesis: with TBTT_SEL_ENABLE=0 (what we'd been passing), the chip
 * skips Mesh Beacon Collision Avoidance setup, which means it doesn't
 * register itself as an active mesh participant for beacon timing. The
 * chip's RX may be gated on being an active MBCA participant — peer
 * beacons would be ignored otherwise. With TBTT_SEL_ENABLE=1 we tell the
 * chip "yes, you ARE part of this mesh BSS, participate in TBTT". */
enum mmwlan_status mmdrv_mesh_config(uint16_t vif_id,
                                     bool start,
                                     bool enable_beaconing,
                                     int32_t *chip_status)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    /* GPL morse_driver mesh.h MBCA defaults — kept as constants but the
     * MESH_CONFIG below uses MBCA-OFF for 
     *
     * DISABLE MBCA (mbca_config=0). After step24 confirmed
     * the chip's beacon IRQ fires exactly ONCE at startup and never again
     * in mesh mode, the hypothesis is that MBCA's TBTT-selection state
     * machine is gating the beacon timer until peer beacons are observed —
     * a chicken-and-egg trap when nothing is on-air yet. With mbca_config=0
     * the chip should beacon immediately at a fixed TBTT, regardless of
     * any neighbor state. If the beacon IRQ then starts firing every 100ms
     * and TX_STATUS notifications come through, we know MBCA was blocking
     * TX. We can re-enable MBCA later once peering is working.
     *
     * Note: when MBCA is disabled, min_beacon_gap_ms / mbss_start_scan_
     * duration_ms / tbtt_adj_timer_interval_ms are reportedly ignored
     * by the chip — see Linux mesh.c:97 which zeroes mbca.config for
     * beaconless mode, suggesting the chip only reads MBCA fields when
     * mbca_config != 0. */
    #define WARTHOG_MESH_MBCA_TBTT_SEL_ENABLE   (0x01u)  /* BIT(0) */
    #define WARTHOG_MESH_MBCA_MIN_BEACON_GAP_MS (25u)
    #define WARTHOG_MESH_MBCA_MBSS_SCAN_DUR_MS  (2048u)
    #define WARTHOG_MESH_MBCA_TBTT_ADJ_INT_MS   (60000u)
    /* Step 25 flag — 1 = MBCA-OFF, 0 = MBCA-ON (Linux-defaults baseline).
     *
     * NOW 0. Step 25 set this to 1 on the theory that MBCA's TBTT-selection
     * state machine was gating the beacon timer. But the comment above is the
     * refutation: Linux zeroes mbca.config for *beaconless mode*. So
     * mbca_config == 0 does not mean "MBCA disabled, beacon freely" -- it
     * selects the mode where the chip deliberately does NOT beacon. That fits
     * the observed behaviour exactly: MESH_CONFIG(START) is accepted, the
     * beacon IRQ fires once at startup and never again, and on-air monitor
     * capture shows only the 2 s diagnostic probe bursts -- no ~10/s beacons.
     *
     * Sending the real Linux MBCA defaults (TBTT_SEL_ENABLE + the gap/scan/
     * adjust timers below) is what puts the chip in a beaconing mesh mode. */
    /* From the Linux source (an earlier "measured" note here was not): MBCA 0 does not select
     * beaconless, and no beaconing Linux mesh sends it. Every Linux MESH_POINT VIF
     * gets TBTT_SEL with gap 25, tbtt 60000 and scan 2048 at add_interface (morse_driver
     * mesh.c:914-918), wpa_supplicant overrides them with 1/10/60000/2048 (SET_MCBA_CONF,
     * hostap mesh.c:636-661), and command.c:2544-2552 sends the timers whenever it beacons.
     * Only a beaconless mesh zeroes MBCA (mesh.c:149-150), with enable_beaconing 0. So MBCA
     * and its timers 0 with beaconing on is this fork's own tuple; the STA chip VIF never ran
     * a mesh TBTT on it. A MESH chip VIF gets the beaconless start, the Pis' (umac_mesh.c).
     * Set WARTHOG_MESH_MBCA_ENABLE to 1 to restore the old MBCA-on behaviour. */
    #define WARTHOG_MESH_MBCA_ENABLE 0

    struct morse_cmd_resp_mesh_config resp;
    memset(&resp, 0, sizeof(resp));
    struct morse_cmd_req_mesh_config cmd = MORSE_COMMAND_INIT(
        cmd,
        MORSE_CMD_ID_MESH_CONFIG,
        vif_id,
        .mesh_cfg_opcode = start ? MORSE_CMD_MESH_CONFIG_OPCODE_START
                                 : MORSE_CMD_MESH_CONFIG_OPCODE_STOP,
        .enable_beaconing = enable_beaconing ? 1 : 0,
        .mbca_config = WARTHOG_MESH_MBCA_ENABLE ? WARTHOG_MESH_MBCA_TBTT_SEL_ENABLE : 0u,
        .min_beacon_gap_ms = WARTHOG_MESH_MBCA_ENABLE
                               ? WARTHOG_MESH_MBCA_MIN_BEACON_GAP_MS : 0u,
        .mbss_start_scan_duration_ms =
            htole16(WARTHOG_MESH_MBCA_ENABLE ? WARTHOG_MESH_MBCA_MBSS_SCAN_DUR_MS : 0u),
        .tbtt_adj_timer_interval_ms =
            htole16(WARTHOG_MESH_MBCA_ENABLE ? WARTHOG_MESH_MBCA_TBTT_ADJ_INT_MS : 0u));

    enum mmwlan_status status = morse_cmd_tx(&driver_data,
                                             (struct morse_cmd_resp *)&resp,
                                             (struct morse_cmd_req *)&cmd,
                                             sizeof(resp),
                                             0,
                                             true);
    if (status != MMWLAN_SUCCESS && status != MMWLAN_COMMAND_ERROR)
    {
        return status;
    }
    if (chip_status != NULL)
    {
        *chip_status = (int32_t)le32toh(resp.status);
    }
    return MMWLAN_SUCCESS;
}

enum mmwlan_status mmdrv_rm_if(uint16_t vif_id)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    if (vif_id == driver_data.beacon.vif_id)
    {
        morse_beacon_stop(&driver_data);
    }

    struct morse_cmd_req_remove_interface cmd =
        MORSE_COMMAND_INIT(cmd, MORSE_CMD_ID_REMOVE_INTERFACE, vif_id);

    return morse_cmd_tx(&driver_data, NULL, (struct morse_cmd_req *)&cmd, 0, 0, false);
}

enum mmwlan_status mmdrv_cfg_scan(bool enabled)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    struct morse_cmd_req_scan_config cmd = MORSE_COMMAND_INIT(cmd,
                                                              MORSE_CMD_ID_SCAN_CONFIG,
                                                              MMDRV_VIF_ID_INVALID,
                                                              .enabled = enabled ? 1 : 0);

    return morse_cmd_tx(&driver_data, NULL, (struct morse_cmd_req *)&cmd, 0, 0, false);
}

enum mmwlan_status mmdrv_twt_agreement_install_req(const struct mmdrv_twt_data *twt_data)
{
    struct driver_data *driverd = &driver_data;

    if (!driverd->started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    if (twt_data->agreement_len > TWT_MAX_AGREEMENT_LEN)
    {
        MMLOG_WRN("Invalid TWT agreement data length\n");
        return MMWLAN_INVALID_ARGUMENT;
    }

    struct morse_cmd_req_twt_agreement_install cmd =
        MORSE_COMMAND_INIT(cmd,
                           MORSE_CMD_ID_TWT_AGREEMENT_INSTALL,
                           twt_data->interface_id,
                           .flow_id = twt_data->flow_id,
                           .agreement_len = twt_data->agreement_len);

    memcpy(cmd.agreement, twt_data->agreement, twt_data->agreement_len);

    return morse_cmd_tx(driverd, NULL, (struct morse_cmd_req *)&cmd, 0, 0, false);
}

enum mmwlan_status mmdrv_twt_agreement_validate_req(const struct mmdrv_twt_data *twt_data)
{
    struct driver_data *driverd = &driver_data;

    if (!driverd->started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    if (twt_data->agreement_len > TWT_MAX_AGREEMENT_LEN)
    {
        MMLOG_WRN("Invalid TWT agreement data length\n");
        return MMWLAN_INVALID_ARGUMENT;
    }

    struct morse_cmd_req_twt_agreement_install cmd =
        MORSE_COMMAND_INIT(cmd,
                           MORSE_CMD_ID_TWT_AGREEMENT_VALIDATE,
                           twt_data->interface_id,
                           .flow_id = twt_data->flow_id,
                           .agreement_len = twt_data->agreement_len);

    memcpy(cmd.agreement, twt_data->agreement, twt_data->agreement_len);

    return morse_cmd_tx(driverd, NULL, (struct morse_cmd_req *)&cmd, 0, 0, false);
}

enum mmwlan_status mmdrv_twt_remove_req(const struct mmdrv_twt_data *twt_data)
{
    struct driver_data *driverd = &driver_data;

    if (!driverd->started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    struct morse_cmd_req_twt_agreement_remove cmd =
        MORSE_COMMAND_INIT(cmd,
                           MORSE_CMD_ID_TWT_AGREEMENT_REMOVE,
                           twt_data->interface_id,
                           .flow_id = twt_data->flow_id);

    return morse_cmd_tx(driverd, NULL, (struct morse_cmd_req *)&cmd, 0, 0, false);
}

enum mmwlan_status mmdrv_set_channel(uint32_t op_chan_freq_hz,
                                     uint8_t pri_1mhz_chan_idx,
                                     uint8_t op_bw_mhz,
                                     uint8_t pri_bw_mhz,
                                     bool is_off_channel)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }
    DRV_TRACE("set_channel %u", op_chan_freq_hz);
    enum mmwlan_status ret;
    struct morse_cmd_resp_set_channel resp;

    struct morse_cmd_req_set_channel cmd =
        MORSE_COMMAND_INIT(cmd,
                           MORSE_CMD_ID_SET_CHANNEL,
                           MMDRV_VIF_ID_INVALID,
                           .op_chan_freq_hz = htole32(op_chan_freq_hz),
                           .op_bw_mhz = op_bw_mhz,
                           .pri_bw_mhz = pri_bw_mhz,
                           .pri_1mhz_chan_idx = pri_1mhz_chan_idx,
                           .dot11_mode = MORSE_CMD_DOT11_PROTO_MODE_AH,
                           .is_off_channel = is_off_channel);


    ret = morse_cmd_tx(&driver_data,
                       (struct morse_cmd_resp *)&resp,
                       (struct morse_cmd_req *)&cmd,
                       sizeof(resp),
                       0,
                       true);
    if (ret == MMWLAN_SUCCESS)
    {
        MMLOG_INF("%s channel change f:%lu, o:%u, p:%u, i:%u\n",
                  __func__,
                  cmd.op_chan_freq_hz,
                  cmd.op_bw_mhz,
                  cmd.pri_bw_mhz,
                  cmd.pri_1mhz_chan_idx);
    }
    else if (ret == MMWLAN_TIMED_OUT)
    {
        MMLOG_ERR("Command %02x:%02x timed out\n",
                  le16toh(cmd.hdr.message_id),
                  le16toh(cmd.hdr.host_id));
    }

    else if (ret == MMWLAN_COMMAND_ERROR)
    {
        int fw_status = (int)le32toh(resp.status);

        if (fw_status == MORSE_EPERM)
        {

            return MMWLAN_CHANNEL_INVALID;
        }

        MMLOG_ERR("Command %02x:%02x failed with rc %d (0x%x)\n",
                  le16toh(cmd.hdr.message_id),
                  le16toh(cmd.hdr.host_id),
                  fw_status,
                  (unsigned)fw_status);
    }

    return ret;
}

enum mmwlan_status mmdrv_cfg_mpsw(uint32_t airtime_min_us,
                                  uint32_t airtime_max_us,
                                  uint32_t packet_space_window_length_us)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    struct morse_cmd_resp_mpsw_config resp = { 0 };

    if ((airtime_max_us != 0) &&
        ((airtime_min_us > airtime_max_us) || (airtime_min_us == airtime_max_us)))
    {
        MMLOG_WRN("airtime_min (%lu) must be < airtime max (%lu), or airtime max must be 0.\n",
                  airtime_min_us,
                  airtime_max_us);
        return MMWLAN_INVALID_ARGUMENT;
    }

    struct morse_cmd_req_mpsw_config cmd =
        MORSE_COMMAND_INIT(cmd,
                           MORSE_CMD_ID_MPSW_CONFIG,
                           MMDRV_VIF_ID_INVALID,
                           .config.airtime_min_us = airtime_min_us,
                           .config.airtime_max_us = airtime_max_us,
                           .config.packet_space_window_length_us = packet_space_window_length_us);

    if ((airtime_min_us > 0) || (airtime_max_us > 0))
    {
        cmd.set_cfgs |= MORSE_CMD_SET_MPSW_CFG_PKT_SPC_WIN_LEN;
    }
    if (packet_space_window_length_us > 0)
    {
        cmd.set_cfgs |= MORSE_CMD_SET_MPSW_CFG_AIRTIME_BOUNDS;
    }

    if ((cmd.set_cfgs & MORSE_CMD_SET_MPSW_CFG_PKT_SPC_WIN_LEN) ||
        (cmd.set_cfgs & MORSE_CMD_SET_MPSW_CFG_AIRTIME_BOUNDS))
    {
        cmd.set_cfgs |= MORSE_CMD_SET_MPSW_CFG_ENABLED;
        cmd.config.enable = 1;
    }

    return morse_cmd_tx(&driver_data,
                        (struct morse_cmd_resp *)&resp,
                        (struct morse_cmd_req *)&cmd,
                        sizeof(resp),
                        0,
                        false);
}

enum mmwlan_status mmdrv_update_beacon_vendor_ie_filter(uint16_t vif_id,
                                                        const uint8_t *ouis,
                                                        uint8_t n_ouis)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    if (n_ouis > MMWLAN_BEACON_VENDOR_IE_MAX_OUI_FILTERS)
    {
        return MMWLAN_INVALID_ARGUMENT;
    }

    struct morse_cmd_req_update_oui_filter cmd =
        MORSE_COMMAND_INIT(cmd, MORSE_CMD_ID_UPDATE_OUI_FILTER, vif_id, .n_ouis = n_ouis);

    if (ouis != NULL)
    {
        memcpy(cmd.ouis, ouis, (cmd.n_ouis * MMWLAN_OUI_SIZE));
    }

    return morse_cmd_tx(&driver_data, NULL, (struct morse_cmd_req *)&cmd, 0, 0, false);
}

enum mmwlan_status mmdrv_cfg_bss(uint16_t vif_id,
                                 uint16_t beacon_int,
                                 uint16_t dtim_period,
                                 uint32_t cssid)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    struct morse_cmd_req_bss_config cmd =
        MORSE_COMMAND_INIT(cmd,
                           MORSE_CMD_ID_BSS_CONFIG,
                           vif_id,
                           .beacon_interval_tu = htole16(beacon_int),
                           .cssid = htole32(cssid),
                           .dtim_period = htole16(dtim_period));

    return morse_cmd_tx(&driver_data, NULL, (struct morse_cmd_req *)&cmd, 0, 0, false);
}

enum mmwlan_status mmdrv_update_sta_state(uint16_t vif_id,
                                          uint16_t aid,
                                          const uint8_t *addr,
                                          enum morse_sta_state state)
{
    int32_t chip_status = 0;
    enum mmwlan_status status =
        mmdrv_update_sta_state_status(vif_id, aid, addr, state, &chip_status);
    if (status == MMWLAN_SUCCESS && chip_status != 0)
    {
        return MMWLAN_COMMAND_ERROR;
    }
    return status;
}

enum mmwlan_status mmdrv_update_sta_state_status(uint16_t vif_id,
                                                 uint16_t aid,
                                                 const uint8_t *addr,
                                                 enum morse_sta_state state,
                                                 int32_t *chip_status)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    DRV_TRACE("set_sta_state %u %02x:%02x:%02x:%02x:%02x:%02x",
              state,
              addr[0],
              addr[1],
              addr[2],
              addr[3],
              addr[4],
              addr[5]);

    struct morse_cmd_resp_set_sta_state resp;
    memset(&resp, 0, sizeof(resp));
    struct morse_cmd_req_set_sta_state cmd = MORSE_COMMAND_INIT(cmd,
                                                                MORSE_CMD_ID_SET_STA_STATE,
                                                                vif_id,
                                                                .aid = htole16(aid),
                                                                .state = htole16(state));

    memcpy(cmd.sta_addr, addr, sizeof(cmd.sta_addr));

    enum mmwlan_status status = morse_cmd_tx(&driver_data,
                                             (struct morse_cmd_resp *)&resp,
                                             (struct morse_cmd_req *)&cmd,
                                             sizeof(resp),
                                             0,
                                             true);
    if (status != MMWLAN_SUCCESS && status != MMWLAN_COMMAND_ERROR)
    {
        return status;
    }
    if (chip_status != NULL)
    {
        *chip_status = (int32_t)le32toh(resp.status);
    }
    return MMWLAN_SUCCESS;
}

/* Key-install trace (storage in main/at.c; AT+KEYINST?). */
extern volatile uint32_t g_warthog_keyinst[8];
extern volatile uint32_t g_warthog_keyinst_n;
/* INSTALL_KEYs the chip refused, the last one's status, and hw-index mismatches (AT+MESHCFG?). */
extern volatile uint32_t g_warthog_chipcmd_key_refused, g_warthog_chipcmd_keyidx_mismatch;
extern volatile int32_t g_warthog_chipcmd_key_status;

/* Ask the chip to stop doing crypto and hand protected frames to the host raw.
 *
 * MORSE_CMD_PARAM_ID_CRYPTO_IN_HOST is the documented lever for host software
 * CCMP, and it is the prerequisite for per-link keys: the chip holds only ONE
 * pairwise key per key-index for a VIF (proven on the bench -- re-installing
 * one peer's key breaks another's link), while AMPE derives a distinct MTK per
 * link. Software crypto keyed off the transmitter address has no such limit.
 *
 * MEASURED on chip firmware 1.17.6 with the 2.10.4 driver: nothing changes on air.
 * With CRYPTO_IN_HOST=1 a peered link kept passing 3/3, rx_data kept climbing,
 * and ZERO frames arrived undecrypted (AT+RXCHAN? nodec uni=0, rxdrop=0). The
 * chip carried on decrypting in firmware. Whether it accepted the SET was NOT
 * measured: that morse_cmd_tx() returned 0 for any reply given a response buffer,
 * so a refused GET also "read back" a value (the reply's, or uninitialised stack).
 * AT+CRYPTOHOST? rc now tells them apart (14 = refused); not measured on 1.17.9.
 *
 * Kept because the negative result is worth more than the code -- it is the
 * documented lever, on 2.10.4 it looked like it worked, and only the RX
 * counters showed it changed nothing.
 *
 * The mechanism that DOES deliver protected frames to the host needs no
 * parameter: install no key for a peer and the chip cannot decrypt its frames,
 * so it hands them up with MMDRV_RX_FLAG_DECRYPTED clear. We have already seen
 * exactly that -- group frames arrived intact and were dropped at the "no HW
 * decryption" gate (rxdrop reason=4) with the CCMP header visible in
 * AT+RXHEAD?. That is the hook host software CCMP should use.
 *
 * @returns MMWLAN_SUCCESS, MMWLAN_COMMAND_ERROR if the chip refused it, or the transport's error.
 */
enum mmwlan_status mmdrv_set_crypto_in_host(uint16_t vif_id, bool enable, uint32_t *out_value)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    struct morse_cmd_resp_get_set_generic_param resp = { 0 };
    struct morse_cmd_req_get_set_generic_param cmd =
        MORSE_COMMAND_INIT(cmd, MORSE_CMD_ID_GET_SET_GENERIC_PARAM, vif_id,
                           .param_id = htole32(MORSE_CMD_PARAM_ID_CRYPTO_IN_HOST),
                           .action = htole32(MORSE_CMD_PARAM_ACTION_SET),
                           .flags = 0,
                           .value = htole32(enable ? 1u : 0u));

    enum mmwlan_status status = morse_cmd_tx(&driver_data, (struct morse_cmd_resp *)&resp,
                                             (struct morse_cmd_req *)&cmd, sizeof(resp), 0, false);
    if (status != MMWLAN_SUCCESS)
    {
        MMLOG_WRN("crypto_in_host set failed %u\n", status);
        return status;
    }
    if (out_value != NULL)
    {
        *out_value = le32toh(resp.value);
    }
    return MMWLAN_SUCCESS;
}

/* Read the parameter back. A firmware that ignores an unknown parameter can
 * still answer the SET with success, so the only honest confirmation is
 * reading the value the chip actually holds. */
enum mmwlan_status mmdrv_get_crypto_in_host(uint16_t vif_id, uint32_t *out_value)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    struct morse_cmd_resp_get_set_generic_param resp = { 0 };
    struct morse_cmd_req_get_set_generic_param cmd =
        MORSE_COMMAND_INIT(cmd, MORSE_CMD_ID_GET_SET_GENERIC_PARAM, vif_id,
                           .param_id = htole32(MORSE_CMD_PARAM_ID_CRYPTO_IN_HOST),
                           .action = htole32(MORSE_CMD_PARAM_ACTION_GET),
                           .flags = 0,
                           .value = 0);

    enum mmwlan_status status = morse_cmd_tx(&driver_data, (struct morse_cmd_resp *)&resp,
                                             (struct morse_cmd_req *)&cmd, sizeof(resp), 0, false);
    if (status != MMWLAN_SUCCESS)
    {
        return status;
    }
    if (out_value != NULL)
    {
        *out_value = le32toh(resp.value);
    }
    return MMWLAN_SUCCESS;
}

enum mmwlan_status mmdrv_install_key(uint16_t vif_id, uint16_t aid, struct mmdrv_key_conf *key_conf)
{
    int32_t st = 0;
    enum mmwlan_status status = mmdrv_install_key_status(vif_id, aid, key_conf, &st);
    if (status == MMWLAN_SUCCESS && st != 0)
    {
        g_warthog_chipcmd_key_refused++;
        g_warthog_chipcmd_key_status = st;
        return MMWLAN_COMMAND_ERROR;
    }
    return status;
}

enum mmwlan_status mmdrv_install_key_status(uint16_t vif_id,
                                            uint16_t aid,
                                            struct mmdrv_key_conf *key_conf,
                                            int32_t *chip_status)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    DRV_TRACE("install key %u %u %u %u",
              key_conf->key_idx,
              key_conf->is_pairwise,
              aid,
              key_conf->length);

    uint16_t requested_key_idx = key_conf->key_idx;
    struct morse_cmd_resp_install_key resp;
    /* A status-only reply leaves the rest of it unwritten (morse_cmd_tx copies what came). */
    memset(&resp, 0, sizeof(resp));

    MMLOG_DBG("%s Installing key for vif (%d):\n"
              "\tkey->idx: %d\n"
              "\tkey->cipher: 0x%08x\n"
              "\tkey->pn: 0x" MM_X64_FMT "\n"
              "\tkey->len: %d\n"
              "\tkey->is_pairwise: %d\n"
              "\taid (optional): %d\n",
              __func__,
              vif_id,
              key_conf->key_idx,
              MORSE_CMD_KEY_CIPHER_AES_CCM,
              MM_X64_VAL(key_conf->tx_pn),
              key_conf->length,
              key_conf->is_pairwise,
              aid);

    struct morse_cmd_req_install_key cmd =
        MORSE_COMMAND_INIT(cmd,
                           MORSE_CMD_ID_INSTALL_KEY,
                           vif_id,
                           .pn = htole64(key_conf->tx_pn),
                           .aid = htole32(aid),
                           .cipher = MORSE_CMD_KEY_CIPHER_AES_CCM);

    switch (key_conf->length)
    {
        case 16:
            cmd.key_length = MORSE_CMD_AES_KEY_LEN_LENGTH_128;
            break;

        case 32:
            cmd.key_length = MORSE_CMD_AES_KEY_LEN_LENGTH_256;
            break;

        default:
            return MMWLAN_INVALID_ARGUMENT;
    }
    cmd.key_type = key_conf->is_pairwise ? MORSE_CMD_TEMPORAL_KEY_TYPE_PTK :
                                           MORSE_CMD_TEMPORAL_KEY_TYPE_GTK;

    cmd.key_idx = key_conf->key_idx;
    memcpy(&cmd.key[0], &key_conf->key[0], sizeof(cmd.key));

    enum mmwlan_status status = morse_cmd_tx(&driver_data,
                                             (struct morse_cmd_resp *)&resp,
                                             (struct morse_cmd_req *)&cmd,
                                             sizeof(resp),
                                             0,
                                             true);
    if (status != MMWLAN_SUCCESS && status != MMWLAN_COMMAND_ERROR)
    {
        MMLOG_WRN("mmdrv_add_key - morse_cmd_install_key failed (%u)\n", status);
        return status;
    }
    /* The chip's verdict; morse_cmd_tx only says COMMAND_ERROR. A refused key is not in it. */
    const int32_t fw_status = (int32_t)le32toh(resp.status);
    if (chip_status != NULL)
    {
        *chip_status = fw_status;
    }
    if (status == MMWLAN_COMMAND_ERROR)
    {
        MMLOG_WRN("mmdrv_add_key - chip refused key %u (aid %u): status %ld\n",
                  (unsigned)requested_key_idx, (unsigned)aid, (long)fw_status);
        return MMWLAN_SUCCESS;
    }

    /* Record what the CHIP assigned. It returns the hardware slot it chose,
     * which is the only evidence of whether keys are per-STA or per-VIF on
     * this part -- and the SDK throws it away (the assert below is compiled
     * out in release, and nothing propagates a differing index to the TX
     * path, so a silently remapped key would encrypt with the wrong slot). */
    if (g_warthog_keyinst_n < 8)
    {
        uint32_t i = g_warthog_keyinst_n;
        g_warthog_keyinst[i] = ((uint32_t)aid << 24) | ((uint32_t)key_conf->is_pairwise << 16) |
                               ((uint32_t)requested_key_idx << 8) | (uint32_t)resp.key_idx;
    }
    g_warthog_keyinst_n++;

    key_conf->key_idx = resp.key_idx;
    MMLOG_DBG("%s Installed key @ hw index: %d\n", __func__, resp.key_idx);


#if WARTHOG_MESH_CHIP_VIF_MESH
    /* Linux keeps the chip's index as the key's (morse_driver mac.c:1105): counted. */
    if (requested_key_idx != key_conf->key_idx)
    {
        g_warthog_chipcmd_keyidx_mismatch++;
    }
#else
    MMOSAL_ASSERT(requested_key_idx == key_conf->key_idx);
#endif

    return MMWLAN_SUCCESS;
}

enum mmwlan_status mmdrv_disable_key(uint16_t vif_id,
                                     uint16_t aid,
                                     uint8_t hw_key_idx,
                                     bool is_pairwise)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    if (aid)
    {
        DRV_TRACE("disable key %u %u %u", hw_key_idx, is_pairwise, aid);

        MMLOG_DBG("%s Disabling key for vif (%d):\n"
                  "\tkey->hw_key_idx: %d\n"
                  "\taid (optional): %d\n",
                  __func__,
                  vif_id,
                  hw_key_idx,
                  aid);

        struct morse_cmd_req_disable_key cmd =
            MORSE_COMMAND_INIT(cmd,
                               MORSE_CMD_ID_DISABLE_KEY,
                               vif_id,
                               .aid = htole16(aid),
                               .key_idx = hw_key_idx,
                               .key_type = is_pairwise ? MORSE_CMD_TEMPORAL_KEY_TYPE_PTK :
                                                         MORSE_CMD_TEMPORAL_KEY_TYPE_GTK);

        return morse_cmd_tx(&driver_data, NULL, (struct morse_cmd_req *)&cmd, 0, 0, false);
    }
    return MMWLAN_SUCCESS;
}

struct mmpkt *mmdrv_alloc_mmpkt_for_tx(uint8_t pkt_class,
                                       uint32_t space_at_start,
                                       uint32_t space_at_end)
{
    struct morse_buff_skb_header *hdr;

    if (!driver_data.started)
    {
        return NULL;
    }

    return mmhal_wlan_alloc_mmpkt_for_tx(
        pkt_class,
        FAST_ROUND_UP(space_at_start + sizeof(*hdr), MORSE_PKT_WORD_ALIGN) + MORSE_YAPS_DELIM_SIZE,
        FAST_ROUND_UP(space_at_end, MORSE_PKT_WORD_ALIGN),
        sizeof(struct mmdrv_tx_metadata));
}

uint32_t mmdrv_tx_pool_free(void)
{
    return mmhal_wlan_pktmem_tx_free();
}

void mmdrv_set_tx_pool_reserve(uint32_t blocks)
{
    mmhal_wlan_pktmem_set_tx_reserve(blocks);
}

struct mmpkt *mmdrv_alloc_mmpkt_for_defrag(uint32_t min_capacity, uint32_t max_capacity)
{

    const size_t metadata_len = sizeof(struct mmdrv_rx_metadata);


    struct mmpkt *mmpkt =
        mmhal_wlan_alloc_mmpkt_for_rx(MMHAL_WLAN_PKT_DATA_TID0, max_capacity, metadata_len);
    if (mmpkt != NULL)
    {
        return mmpkt;
    }


    mmpkt = mmhal_wlan_alloc_mmpkt_for_rx(MMHAL_WLAN_PKT_DATA_TID0, UINT32_MAX, metadata_len);
    if (mmpkt != NULL)
    {
        struct mmpktview *pktview = mmpkt_open(mmpkt);
        uint32_t capacity = mmpkt_available_space_at_end(pktview);
        mmpkt_close(&pktview);
        if (capacity < min_capacity)
        {
            mmpkt_release(mmpkt);
            mmpkt = NULL;
        }
        return mmpkt;
    }


    return mmhal_wlan_alloc_mmpkt_for_rx(MMHAL_WLAN_PKT_DATA_TID0, min_capacity, metadata_len);
}

enum mmwlan_status mmdrv_set_frag_threshold(uint32_t frag_threshold)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    if (frag_threshold == 0)
    {
        frag_threshold = UINT32_MAX;
    }

    return mmdrv_set_param(MMDRV_VIF_ID_INVALID, MORSE_PARAM_ID_FRAGMENT_THRESHOLD, frag_threshold);
}

enum mmwlan_status mmdrv_set_dynamic_ps_timeout(uint32_t timeout_ms)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    /* It returns mmdrv_set_param's mmwlan_status, not an errno. */
    return (enum mmwlan_status)morse_ps_set_dynamic_ps_timeout(&driver_data, timeout_ms);
}

enum mmwlan_status mmdrv_tx_frame(struct mmpkt *mmpkt, bool is_mgmt)
{
    struct mmdrv_tx_metadata *tx_metadata = mmdrv_get_tx_metadata(mmpkt);

    tx_metadata->status_flags = MMDRV_TX_STATUS_FLAG_NO_ACK;
    tx_metadata->attempts = 0;

    if (!driver_data.started)
    {
        mmdrv_host_process_tx_status(mmpkt);
        return MMWLAN_NOT_RUNNING;
    }

    /* Diagnostic: count every chip TX, mgmt vs data. DBG only: every task that transmits
     * runs this, and printf's frames on top of a sender's overflow the 3.5 KB tcpip task. */
    static uint32_t s_tx_total = 0;
    static uint32_t s_tx_mgmt = 0;
    s_tx_total++;
    if (is_mgmt) s_tx_mgmt++;
    if (s_tx_total <= 16 || (s_tx_total % 50) == 0)
    {
        MMLOG_DBG("mmdrv_tx_frame#%lu: is_mgmt=%d tid=%u (total mgmt=%lu)\n",
                  (unsigned long)s_tx_total, (int)is_mgmt,
                  (unsigned)tx_metadata->tid, (unsigned long)s_tx_mgmt);
    }

    uint16_t aci = dot11_tid_to_ac(tx_metadata->tid);
    struct driver_data *driverd = &(driver_data);
    struct morse_skbq *mq;
    enum morse_skb_channel channel;

    if (is_mgmt)
    {
        mq = driverd->cfg->ops->skbq_mgmt_tc_q(driverd);
        channel = MORSE_SKB_CHAN_MGMT;
    }
    else if (tx_metadata->flags & MMDRV_TX_FLAG_NO_ACK)
    {
        mq = driverd->cfg->ops->skbq_tc_q_from_aci(driverd, aci);
        channel = MORSE_SKB_CHAN_DATA_NOACK;
    }
    else
    {
        mq = driverd->cfg->ops->skbq_tc_q_from_aci(driverd, aci);
        channel = MORSE_SKB_CHAN_DATA;
    }

    DRV_TRACE("tx %x", mmpkt);

    return errno_to_status(morse_skbq_mmpkt_tx(mq, mmpkt, channel));
}

enum mmwlan_status mmdrv_cfg_qos_queue(const struct mmwlan_qos_queue_params *params)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    struct morse_cmd_req_set_qos_params qparams = {

        .uapsd = 0,
        .queue_idx = params->aci,
        .aifs_slot_count = params->aifs,
        .contention_window_min = params->cw_min,
        .contention_window_max = params->cw_max,
        .max_txop_usec = params->txop_max_us,
    };

    DRV_TRACE("cfg_qos %x", params->aci);

    struct morse_cmd_req_set_qos_params cmd =
        MORSE_COMMAND_INIT(cmd, MORSE_CMD_ID_SET_QOS_PARAMS, MMDRV_VIF_ID_INVALID);

    cmd.uapsd = qparams.uapsd;
    cmd.queue_idx = qparams.queue_idx;
    cmd.aifs_slot_count = qparams.aifs_slot_count;
    cmd.contention_window_min = htole16(qparams.contention_window_min);
    cmd.contention_window_max = htole16(qparams.contention_window_max);
    cmd.max_txop_usec = htole32(qparams.max_txop_usec);

    return morse_cmd_tx(&driver_data, NULL, (struct morse_cmd_req *)&cmd, 0, 0, false);
}

enum mmwlan_status mmdrv_set_wake_enabled(bool enabled)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    if (enabled)
    {
        return errno_to_status(morse_ps_disable_async(&driver_data, PS_WAKER_UMAC));
    }
    else
    {
        return errno_to_status(morse_ps_enable_async(&driver_data, PS_WAKER_UMAC));
    }
}

enum mmwlan_status mmdrv_set_chip_power_save_enabled(uint16_t vif_id, bool enabled)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    MMLOG_DBG("Chip Power Mode set to: %d\n", enabled);


    struct morse_cmd_req_config_ps cmd = MORSE_COMMAND_INIT(cmd,
                                                            MORSE_CMD_ID_CONFIG_PS,
                                                            vif_id,
                                                            .enabled = (uint8_t)enabled,
                                                            .dynamic_ps_offload = (uint8_t)enabled);

    return morse_cmd_tx(&driver_data,
                        NULL,
                        (struct morse_cmd_req *)&cmd,
                        0,
                        MM_CMD_TIMEOUT_PS,
                        false);
}

enum mmwlan_status mmdrv_set_chip_wnm_sleep_enabled(uint16_t vif_id, bool enabled)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    MMLOG_DBG("Chip WNM sleep set to: %d\n", enabled);

    struct morse_cmd_req_set_long_sleep_config cmd =
        MORSE_COMMAND_INIT(cmd,
                           MORSE_CMD_ID_SET_LONG_SLEEP_CONFIG,
                           vif_id,
                           .enabled = (uint8_t)enabled);

    return morse_cmd_tx(&driver_data, NULL, (struct morse_cmd_req *)&cmd, 0, 0, false);
}

enum mmwlan_status mmdrv_health_check(void)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    struct morse_cmd_req_health_check cmd =
        MORSE_COMMAND_INIT(cmd, MORSE_CMD_ID_HEALTH_CHECK, MMDRV_VIF_ID_INVALID);

    return morse_cmd_tx(&driver_data,
                        NULL,
                        (struct morse_cmd_req *)&cmd,
                        0,
                        MM_CMD_TIMEOUT_HEALTH_CHECK,
                        false);
}

void mmdrv_hw_restart_completed(void)
{
    mmdrv_host_set_tx_paused(MMDRV_PAUSE_SOURCE_MASK_HW_RESTART, false);
}

enum mmwlan_status mmdrv_get_stats(uint32_t core_num, uint8_t **buf, uint32_t *len)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    uint16_t cmd_id;

    struct morse_cmd_resp *resp = (struct morse_cmd_resp *)*buf;

    switch (core_num)
    {
        case 0:
            cmd_id = MORSE_CMD_ID_HOST_STATS_LOG;
            break;

        case 1:
            cmd_id = MORSE_CMD_ID_MAC_STATS_LOG;
            break;

        case 2:
            cmd_id = MORSE_CMD_ID_UPHY_STATS_LOG;
            break;

        default:
            return MMWLAN_INVALID_ARGUMENT;
    }

    struct morse_cmd_req cmd = MORSE_COMMAND_INIT(cmd, cmd_id, MMDRV_VIF_ID_INVALID);

    enum mmwlan_status status = morse_cmd_tx(&driver_data, resp, &cmd, *len, 0, false);
    if (status == MMWLAN_SUCCESS)
    {
        *buf = resp->data;
        *len = le16toh(resp->hdr.len);
    }

    return status;
}

enum mmwlan_status mmdrv_reset_stats(uint32_t core_num)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    uint16_t cmd_id;

    switch (core_num)
    {
        case 0:
            cmd_id = MORSE_CMD_ID_HOST_STATS_RESET;
            break;

        case 1:
            cmd_id = MORSE_CMD_ID_MAC_STATS_RESET;
            break;

        case 2:
            cmd_id = MORSE_CMD_ID_UPHY_STATS_RESET;
            break;

        default:
            return MMWLAN_INVALID_ARGUMENT;
    }

    struct morse_cmd_req cmd = MORSE_COMMAND_INIT(cmd, cmd_id, MMDRV_VIF_ID_INVALID);

    return morse_cmd_tx(&driver_data, NULL, &cmd, 0, 0, false);
}

enum mmwlan_status mmdrv_get_capabilities(uint16_t vif_id, struct morse_caps *caps)
{
    int32_t chip_status = 0;
    enum mmwlan_status status = mmdrv_get_capabilities_status(vif_id, caps, &chip_status);
    if (status == MMWLAN_SUCCESS && chip_status != 0)
    {
        return MMWLAN_COMMAND_ERROR;
    }
    return status;
}

/* As mmdrv_get_capabilities; SUCCESS once the chip answered, @p caps set only if it accepted. */
enum mmwlan_status mmdrv_get_capabilities_status(uint16_t vif_id,
                                                 struct morse_caps *caps,
                                                 int32_t *chip_status)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    struct morse_cmd_resp_get_capabilities rsp;
    memset(&rsp, 0, sizeof(rsp));
    struct morse_cmd_req_get_capabilities cmd =
        MORSE_COMMAND_INIT(cmd, MORSE_CMD_ID_GET_CAPABILITIES, vif_id);

    enum mmwlan_status status = morse_cmd_tx(&driver_data,
                                             (struct morse_cmd_resp *)&rsp,
                                             (struct morse_cmd_req *)&cmd,
                                             sizeof(rsp),
                                             0,
                                             true);
    if (status != MMWLAN_SUCCESS && status != MMWLAN_COMMAND_ERROR)
    {
        return status;
    }
    if (chip_status != NULL)
    {
        *chip_status = (int32_t)le32toh(rsp.status);
    }
    if (status == MMWLAN_COMMAND_ERROR)
    {
        return MMWLAN_SUCCESS;
    }

    caps->ampdu_mss = rsp.capabilities.ampdu_mss;
    caps->morse_mmss_offset = rsp.morse_mmss_offset;
    caps->beamformee_sts_capability = rsp.capabilities.beamformee_sts_capability;
    caps->maximum_ampdu_length_exponent = rsp.capabilities.maximum_ampdu_length_exponent;
    caps->number_sounding_dimensions = rsp.capabilities.number_sounding_dimensions;
    for (int i = 0; i < MORSE_CMD_S1G_CAPABILITY_FLAGS_WIDTH; i++)
    {
        caps->flags[i] = le32toh(rsp.capabilities.flags[i]);
    }

    return MMWLAN_SUCCESS;
}

enum mmwlan_status mmdrv_trigger_core_assert(uint32_t core_id)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }
    struct morse_cmd_req_force_assert cmd = MORSE_COMMAND_INIT(cmd,
                                                               MORSE_CMD_ID_FORCE_ASSERT,
                                                               MMDRV_VIF_ID_INVALID,
                                                               .hart_id = core_id);

    return morse_cmd_tx(&driver_data, NULL, (struct morse_cmd_req *)&cmd, 0, 0, false);
}

enum mmwlan_status mmdrv_set_cqm_rssi(uint16_t vif_id, int32_t threshold, uint32_t hysteresis)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    struct morse_cmd_req_set_cqm_rssi cmd = MORSE_COMMAND_INIT(cmd,
                                                               MORSE_CMD_ID_SET_CQM_RSSI,
                                                               vif_id,
                                                               .threshold = htole32(threshold),
                                                               .hysteresis = htole32(hysteresis));

    return morse_cmd_tx(&driver_data, NULL, (struct morse_cmd_req *)&cmd, 0, 0, false);
}

enum mmwlan_status mmdrv_set_ndp_probe(uint16_t vif_id, bool enabled)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    struct morse_cmd_req_set_ndp_probe_support cmd =
        MORSE_COMMAND_INIT(cmd,
                           MORSE_CMD_ID_SET_NDP_PROBE_SUPPORT,
                           vif_id,
                           .enabled = enabled ? true : false,

                           .requested_response_is_pv1 = 0,

                           .tx_bw_mhz = -1);

    return morse_cmd_tx(&driver_data, NULL, (struct morse_cmd_req *)&cmd, 0, 0, false);
}

enum mmwlan_status mmdrv_execute_command(uint8_t *command,
                                         uint8_t *response,
                                         uint32_t *response_len)
{
    struct morse_cmd_resp *resp = (struct morse_cmd_resp *)response;
    struct morse_cmd_req *cmd = (struct morse_cmd_req *)command;
    uint32_t resp_len = (response_len != NULL) ? *response_len : 0;

    if ((resp == NULL) != (response_len == NULL))
    {
        return MMWLAN_INVALID_ARGUMENT;
    }

    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    enum mmwlan_status status = morse_cmd_tx(&driver_data, resp, cmd, resp_len, 0, false);

    if (resp == NULL)
    {
        return status;
    }

    if (status == MMWLAN_SUCCESS || status == MMWLAN_COMMAND_ERROR)
    {

        *response_len = le16toh(resp->hdr.len) + sizeof(resp->hdr);
    }
    else
    {

        resp->hdr.message_id = cmd->hdr.message_id;
        resp->hdr.host_id = cmd->hdr.host_id;
        resp->hdr.vif_id = cmd->hdr.vif_id;
        resp->hdr.len = htole16(sizeof(resp->status));
        resp->hdr.flags = htole16(MORSE_CMD_TYPE_RESP);
        resp->status = htole32(MORSE_FAILED);
        *response_len = sizeof(*resp);
    }

    return status;
}

enum mmwlan_status mmdrv_set_seq_num_spaces(uint16_t vif_id,
                                            const uint16_t *tx_seq_num_spaces,
                                            const uint8_t *addr)
{

    struct morse_cmd_req_sequence_number_spaces req =
        MORSE_COMMAND_INIT(req,
                           MORSE_CMD_ID_SEQUENCE_NUMBER_SPACES,
                           vif_id,
                           .flags = MORSE_CMD_SNS_FLAG_SET |
                                    MORSE_CMD_SNS_FLAG_BASELINE |
                                    MORSE_CMD_SNS_FLAG_INDIV_ADDR_QOS_DATA,
                           .spaces.baseline = tx_seq_num_spaces[MMDRV_SEQ_NUM_BASELINE],
                           .spaces.qos_null = tx_seq_num_spaces[MMDRV_SEQ_NUM_QOS_NULL]);

    MM_STATIC_ASSERT(MMWLAN_MAX_QOS_TID <= MORSE_CMD_SNS_MAX_TIDS,
                     "Driver sequence number spaces exceed the amount supported by the chip\n");
    memcpy(req.spaces.individually_addr_qos_data, tx_seq_num_spaces, MMWLAN_MAX_QOS_TID);
    mac_addr_copy(req.addr, addr);

    return morse_cmd_tx(&driver_data,
                        NULL,
                        (struct morse_cmd_req *)&req,
                        0,
                        MM_CMD_TIMEOUT_DEFAULT,
                        false);
}

enum mmwlan_status mmdrv_set_listen_interval_sleep(uint16_t vif, uint16_t listen_interval)
{
    struct morse_cmd_req_li_sleep req =
        MORSE_COMMAND_INIT(req, MORSE_CMD_ID_LI_SLEEP, vif, .listen_interval = listen_interval);

    return morse_cmd_tx(&driver_data,
                        NULL,
                        (struct morse_cmd_req *)&req,
                        0,
                        MM_CMD_TIMEOUT_DEFAULT,
                        false);
}

MM_STATIC_ASSERT(MMDRV_DIRECTION_OUTGOING == 0 && MMDRV_DIRECTION_INCOMING == 1,
                 "Traffic flow direction enum must match firmware expectation");

enum mmwlan_status mmdrv_set_control_response_bw(uint16_t vif_id,
                                                 enum mmdrv_direction direction,
                                                 bool cr_1mhz_en)
{
    if (!driver_data.started)
    {
        return MMWLAN_NOT_RUNNING;
    }

    struct morse_cmd_req_set_control_response cmd =
        MORSE_COMMAND_INIT(cmd,
                           MORSE_CMD_ID_SET_CONTROL_RESPONSE,
                           vif_id,
                           .control_response_1mhz_en = cr_1mhz_en,
                           .direction = direction);

    return morse_cmd_tx(&driver_data, NULL, (struct morse_cmd_req *)&cmd, 0, 0, false);
}

enum mmwlan_status mmdrv_hw_scan(uint16_t vif_id, struct mmdrv_hw_scan_params *params)
{

    struct morse_cmd_req_hw_scan *cmd =
        (struct morse_cmd_req_hw_scan *)mmosal_malloc(sizeof(*cmd) + params->tlvs_len);
    if (cmd == NULL)
    {
        MMLOG_ERR("HW scan cmd allocation failed\n");
        return MMWLAN_NO_MEM;
    }

    morse_command_reinit_header(
        &cmd->hdr,
        sizeof(struct morse_cmd_req_hw_scan) - sizeof(struct morse_cmd_header) + params->tlvs_len,
        MORSE_CMD_ID_HW_SCAN,
        vif_id);

    cmd->dwell_time_ms = htole32(params->dwell_time_ms);
    cmd->flags = htole32(params->flags);
    if (params->tlvs_len > 0)
    {
        memcpy(cmd->variable, params->tlvs, params->tlvs_len);
    }


    bool is_abort = params->flags & MORSE_CMD_HW_SCAN_FLAGS_ABORT;

    struct morse_cmd_resp resp = { 0 };
    enum mmwlan_status status =
        morse_cmd_tx(&driver_data, &resp, (struct morse_cmd_req *)cmd, sizeof(resp), 0, is_abort);
    if (is_abort)
    {
        if (status == MMWLAN_SUCCESS)
        {
            goto exit;
        }
        else if (status == MMWLAN_COMMAND_ERROR)
        {
            int fw_status = (int)le32toh(resp.status);

            if (fw_status == MORSE_EINVAL || fw_status == MORSE_EALREADY)
            {
                MMLOG_WRN("HW scan already finished or aborting, abort ignored\n");
                status = MMWLAN_SUCCESS;
                goto exit;
            }

            MMLOG_ERR("Command %02x:%02x failed with rc %d (0x%x)\n",
                      le16toh(cmd->hdr.message_id),
                      le16toh(cmd->hdr.host_id),
                      fw_status,
                      (unsigned)fw_status);
        }
        else if (status == MMWLAN_TIMED_OUT)
        {
            MMLOG_ERR("Command %02x:%02x timed out\n",
                      le16toh(cmd->hdr.message_id),
                      le16toh(cmd->hdr.host_id));
        }
        else
        {
            MMLOG_ERR("Command %02x:%02x failed (%u)\n",
                      le16toh(cmd->hdr.message_id),
                      le16toh(cmd->hdr.host_id),
                      status);
        }
        MMLOG_ERR("Failed to execute %s HW_SCAN command\n", "ABORT");
    }
exit:
    mmosal_free(cmd);
    return status;
}
