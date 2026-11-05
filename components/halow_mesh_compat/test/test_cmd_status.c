/*
 * The real morse_cmd_tx (driver/morse_driver/command.c, whole) under driver.c's status helpers,
 * mmdrv_set_txpower, mmdrv_set_param and mmdrv_set_dynamic_ps_timeout, extracted by name with
 * ps.c's morse_ps_set_dynamic_ps_timeout (the Makefile writes test_cmd_status.fns), against a
 * scripted chip that sets the reply packet's length, its hdr.len and its status, or stays silent.
 * test_chipvif_glue fakes morse_cmd_tx and always answers with hdr.len >= 4; this runs the real one.
 *  (1) a refusal whose hdr.len stops short of the status word (the packet still 16 octets):
 *      morse_cmd_tx returns COMMAND_ERROR and resp->status carries the status it read, so every
 *      helper reports it: ADD_INTERFACE refused with no VIF id, INSTALL_KEY refused and counted
 *      for hdr.len 0 to 3, GET_CAPABILITIES and SET_STA_STATE COMMAND_ERROR, and a non-zero chip
 *      status from BSSID_SET, BSS_BEACON_CONFIG and MESH_CONFIG;
 *  (2) controls: an accepted reply, a status-only refusal (12) and an accepted ADD_INTERFACE;
 *  (3) a silent chip: SET_TXPOWER waits the default 600 ms a try, 3 tries, then TIMED_OUT
 *      (upstream passed 0xffff as the timeout: about 197 s, past the hang guard's 180 s);
 *  (4) mmdrv_set_dynamic_ps_timeout returns morse_cmd_tx's mmwlan_status unmapped: TIMED_OUT,
 *      COMMAND_ERROR, SUCCESS with the timeout kept, and NOT_RUNNING with no command queue
 *      (errno_to_status read each as an errno: ERROR or NOT_FOUND).
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/morse_commands.h"
#include "common/morse_command_utils.h"
#include "mmdrv.h"
#include "mmutils.h"
#include "mmwlan.h"
#include "mmpkt.h"
#include "driver/driver.h"
#include "driver/morse_driver/command.h"
#include "driver/morse_driver/skbq.h"
#include "driver/morse_driver/chip_if.h"
#include "driver/morse_driver/hw.h"
#include "driver/morse_driver/ps.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* ---- RTOS / HAL stubs ---------------------------------------------------- */
int mmosal_printf(const char *format, ...) { (void)format; return 0; }
const char *mmosal_task_name(void) { return "test"; }
uint32_t mmosal_get_time_ms(void) { return 0; }
void mmosal_log_failure_info(const struct mmosal_failure_info *info) { (void)info; }
void mmosal_impl_assert(void) { printf("FAIL assert\n"); failures++; }
bool mmosal_mutex_get(struct mmosal_mutex *m, uint32_t t) { (void)m; (void)t; return true; }
bool mmosal_mutex_release(struct mmosal_mutex *m) { (void)m; return true; }
struct mmosal_mutex *mmosal_mutex_create(const char *n) { (void)n; return (struct mmosal_mutex *)1; }
void mmosal_mutex_delete(struct mmosal_mutex *m) { (void)m; }
struct mmosal_semb *mmosal_semb_create(const char *n) { (void)n; return (struct mmosal_semb *)1; }
void mmosal_semb_delete(struct mmosal_semb *s) { (void)s; }
bool mmosal_semb_give(struct mmosal_semb *s) { (void)s; return true; }
void mmhal_set_deep_sleep_veto(uint8_t v) { (void)v; }
void mmhal_clear_deep_sleep_veto(uint8_t v) { (void)v; }
int morse_ps_disable_async(struct driver_data *d, enum ps_waker_id w) { (void)d; (void)w; return 0; }
int morse_ps_enable_async(struct driver_data *d, enum ps_waker_id w) { (void)d; (void)w; return 0; }
void mmosal_task_sleep(uint32_t ms) { (void)ms; }
void *mmosal_malloc_(size_t size) { return malloc(size); }
void mmosal_free(void *p) { free(p); }
int morse_mac_event_recv(struct driver_data *d, struct mmpktview *v) { (void)d; (void)v; return 0; }

/* ---- the scripted chip --------------------------------------------------- */
static struct driver_data driver_data = { .started = true };
static struct morse_skbq s_cmdq;
static bool s_no_cmdq;
static struct morse_skbq *cmd_q_(struct driver_data *d) { (void)d; return s_no_cmdq ? NULL : &s_cmdq; }
static struct chip_if_ops s_ops = { .skbq_cmd_tc_q = cmd_q_ };
static struct mmhal_chip s_cfg;

static struct {
    bool silent;        /* no reply at all */
    uint16_t hdr_len;   /* the reply's hdr.len */
    uint32_t pkt_len;   /* octets the transport hands up */
    uint32_t status;    /* the status word, octets 12..15 */
    uint8_t body[64];   /* octets 16.. */
} s_chip;
static uint16_t s_cmd_id, s_cmd_host_id;
static uint32_t s_waits, s_waited_ms;

struct mmpkt *morse_skbq_alloc_mmpkt_for_cmd(uint32_t len) { return mmpkt_alloc_on_heap(0, len, 0); }
int morse_skbq_mmpkt_tx(struct morse_skbq *q, struct mmpkt *p, uint8_t ch)
{
    (void)q; (void)ch;
    struct mmpktview *v = mmpkt_open(p);
    struct morse_cmd_req *c = (struct morse_cmd_req *)mmpkt_get_data_start(v);
    s_cmd_id = c->hdr.message_id;
    s_cmd_host_id = c->hdr.host_id;
    mmpkt_close(&v);
    return 0;
}
int morse_skbq_tx_finish(struct morse_skbq *q, struct mmpkt *p, struct morse_skb_tx_status *m)
{
    (void)q; (void)m;
    mmpkt_release(p);
    return 0;
}
bool mmosal_semb_wait(struct mmosal_semb *s, uint32_t timeout_ms)
{
    (void)s;
    s_waits++;
    s_waited_ms += timeout_ms;
    if (s_chip.silent) { return false; }
    struct mmpkt *r = mmpkt_alloc_on_heap(0, 128, 0);
    struct mmpktview *v = mmpkt_open(r);
    uint8_t raw[128];
    memset(raw, 0, sizeof(raw));
    struct morse_cmd_resp *rh = (struct morse_cmd_resp *)raw;
    rh->hdr.flags = htole16(MORSE_CMD_TYPE_RESP);
    rh->hdr.message_id = s_cmd_id;
    rh->hdr.host_id = s_cmd_host_id;
    rh->hdr.len = htole16(s_chip.hdr_len);
    rh->status = htole32(s_chip.status);
    memcpy(raw + sizeof(*rh), s_chip.body, sizeof(s_chip.body));
    mmpkt_append_data(v, raw, s_chip.pkt_len);
    driver_data.cmd.rspview = v;
    return true;
}

/* The upstream code under test. */
#include "driver/morse_driver/command.c"

#define DRV_TRACE(...) do { } while (0)
volatile uint32_t g_warthog_keyinst[8];
volatile uint32_t g_warthog_keyinst_n;
volatile uint32_t g_warthog_chipcmd_key_refused, g_warthog_chipcmd_keyidx_mismatch;
volatile int32_t g_warthog_chipcmd_key_status;
#include "test_cmd_status.fns"

static void chip_(bool silent, uint16_t hdr_len, uint32_t pkt_len, uint32_t status)
{
    memset(&s_chip, 0, sizeof(s_chip));
    s_chip.silent = silent;
    s_chip.hdr_len = hdr_len;
    s_chip.pkt_len = pkt_len;
    s_chip.status = status;
    s_waits = 0;
    s_waited_ms = 0;
}

int main(void)
{
    printf("=== morse_cmd_tx under driver.c's status helpers (command.c, driver.c, ps.c) ===\n");
    s_cfg.ops = &s_ops;
    driver_data.cfg = &s_cfg;
    morse_cmd_init(&driver_data);

    /* (1) */
    uint8_t addr[6] = { 2, 0, 0, 0, 0, 1 };
    uint16_t vif = MMDRV_VIF_ID_INVALID;

    chip_(false, 0, sizeof(struct morse_cmd_resp), 5);
    struct morse_cmd_resp_add_interface r;
    memset(&r, 0, sizeof(r));
    struct morse_cmd_req_add_interface c = MORSE_COMMAND_INIT(c, MORSE_CMD_ID_ADD_INTERFACE, 0);
    enum mmwlan_status st = morse_cmd_tx(&driver_data, (struct morse_cmd_resp *)&r,
                                         (struct morse_cmd_req *)&c, sizeof(r), 0, true);
    CHECK(st == MMWLAN_COMMAND_ERROR, "(1) morse_cmd_tx: hdr.len 0, status 5: COMMAND_ERROR (%d)", st);
    CHECK(le32toh(r.status) == 5, "(1) morse_cmd_tx: resp.status carries the status its verdict read (%lu)",
          (unsigned long)le32toh(r.status));

    int32_t cs = 0;
    chip_(false, 0, sizeof(struct morse_cmd_resp), 5);
    st = mmdrv_add_if_status(&vif, addr, MMDRV_INTERFACE_TYPE_MESH, &cs);
    CHECK(st == MMWLAN_SUCCESS && cs != 0, "(1) mmdrv_add_if_status: answered, refused (st %d cs %ld)", st, (long)cs);
    chip_(false, 0, sizeof(struct morse_cmd_resp), 5);
    vif = MMDRV_VIF_ID_INVALID;
    st = mmdrv_add_if(&vif, addr, MMDRV_INTERFACE_TYPE_MESH);
    CHECK(st == MMWLAN_COMMAND_ERROR && vif == MMDRV_VIF_ID_INVALID,
          "(1) mmdrv_add_if: COMMAND_ERROR, no VIF id (st %d vif 0x%x)", st, vif);

    struct mmdrv_key_conf k;
    memset(&k, 0, sizeof(k));
    k.key_idx = 1;
    k.length = 16;
    k.is_pairwise = true;
    for (unsigned hl = 0; hl < 4; hl++)
    {
        chip_(false, (uint16_t)hl, sizeof(struct morse_cmd_resp), 0x01000000u);
        g_warthog_chipcmd_key_refused = 0;
        st = mmdrv_install_key(0, 1, &k);
        CHECK(st == MMWLAN_COMMAND_ERROR && g_warthog_chipcmd_key_refused == 1,
              "(1) mmdrv_install_key: hdr.len %u, status 0x01000000: COMMAND_ERROR, counted (st %d n %lu)",
              hl, st, (unsigned long)g_warthog_chipcmd_key_refused);
    }
    struct morse_caps caps;
    memset(&caps, 0, sizeof(caps));
    chip_(false, 0, sizeof(struct morse_cmd_resp), 7);
    st = mmdrv_get_capabilities(0, &caps);
    CHECK(st == MMWLAN_COMMAND_ERROR, "(1) mmdrv_get_capabilities: COMMAND_ERROR (%d)", st);
    chip_(false, 0, sizeof(struct morse_cmd_resp), 7);
    st = mmdrv_update_sta_state(0, 1, addr, MORSE_STA_AUTHORIZED);
    CHECK(st == MMWLAN_COMMAND_ERROR, "(1) mmdrv_update_sta_state: COMMAND_ERROR (%d)", st);
    cs = 0;
    chip_(false, 0, sizeof(struct morse_cmd_resp), 7);
    st = mmdrv_set_bssid(0, addr, &cs);
    CHECK(st == MMWLAN_SUCCESS && cs != 0, "(1) mmdrv_set_bssid: refused (cs %ld)", (long)cs);
    cs = 0;
    chip_(false, 0, sizeof(struct morse_cmd_resp), 7);
    st = mmdrv_cfg_bss_beacon(0, true, &cs);
    CHECK(st == MMWLAN_SUCCESS && cs != 0, "(1) mmdrv_cfg_bss_beacon: refused (cs %ld)", (long)cs);
    cs = 0;
    chip_(false, 0, sizeof(struct morse_cmd_resp), 7);
    st = mmdrv_mesh_config(0, true, false, &cs);
    CHECK(st == MMWLAN_SUCCESS && cs != 0, "(1) mmdrv_mesh_config: refused (cs %ld)", (long)cs);

    /* (2) a well-formed accept and a well-formed status-only refusal are unchanged. */
    cs = 99;
    chip_(false, 4, sizeof(struct morse_cmd_resp), 0);
    st = mmdrv_set_bssid(0, addr, &cs);
    CHECK(st == MMWLAN_SUCCESS && cs == 0, "(2) an accepted BSSID_SET (st %d cs %ld)", st, (long)cs);
    cs = 0;
    chip_(false, 4, sizeof(struct morse_cmd_resp), 12);
    st = mmdrv_set_bssid(0, addr, &cs);
    CHECK(st == MMWLAN_SUCCESS && cs == 12, "(2) a status-only refusal reads 12 (cs %ld)", (long)cs);
    chip_(false, (uint16_t)(sizeof(struct morse_cmd_resp_add_interface) - sizeof(struct morse_cmd_header)),
          sizeof(struct morse_cmd_resp_add_interface), 0);
    vif = MMDRV_VIF_ID_INVALID;
    st = mmdrv_add_if(&vif, addr, MMDRV_INTERFACE_TYPE_MESH);
    CHECK(st == MMWLAN_SUCCESS && vif == 0, "(2) an accepted ADD_INTERFACE (st %d vif %u)", st, vif);

    /* (3) */
    int32_t pwr = -1;
    chip_(true, 0, 0, 0);
    st = mmdrv_set_txpower(&pwr, 20);
    CHECK(st == MMWLAN_TIMED_OUT, "(3) a silent chip: SET_TXPOWER TIMED_OUT (%d)", st);
    CHECK(s_waits == 3 && s_waited_ms == 3u * MM_CMD_TIMEOUT_DEFAULT,
          "(3) a silent chip: 3 tries at the default %u ms (waits %lu, %lu ms)",
          (unsigned)MM_CMD_TIMEOUT_DEFAULT, (unsigned long)s_waits, (unsigned long)s_waited_ms);

    /* (4) */
    chip_(true, 0, 0, 0);
    st = mmdrv_set_dynamic_ps_timeout(100);
    CHECK(st == MMWLAN_TIMED_OUT, "(4) a silent chip: TIMED_OUT (%d)", st);
    chip_(false, 4, sizeof(struct morse_cmd_resp), 3);
    st = mmdrv_set_dynamic_ps_timeout(100);
    CHECK(st == MMWLAN_COMMAND_ERROR, "(4) refused: COMMAND_ERROR (%d)", st);
    chip_(false, 4, sizeof(struct morse_cmd_resp), 0);
    driver_data.ps.dynamic_ps_timout_ms = 0;
    st = mmdrv_set_dynamic_ps_timeout(100);
    CHECK(st == MMWLAN_SUCCESS && driver_data.ps.dynamic_ps_timout_ms == 100,
          "(4) accepted: SUCCESS, the timeout kept (%d, %lu)", st,
          (unsigned long)driver_data.ps.dynamic_ps_timout_ms);
    s_no_cmdq = true;
    st = mmdrv_set_dynamic_ps_timeout(100);
    CHECK(st == MMWLAN_NOT_RUNNING, "(4) no command queue, the driver started: NOT_RUNNING (%d)", st);
    s_no_cmdq = false;

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_cmd_status: all passed\n");
    return 0;
}
