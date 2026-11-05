/*
 * The chip VIF pieces no simnode links, compiled out of the SDK and run on stubs. simnode
 * replaces driver.c with its fake chip and stubs umac_ps.c, so without this nothing runs them.
 *
 *  (1-5) driver.c's reads of the chip's own status. morse_cmd_tx returns MMWLAN_COMMAND_ERROR
 *     for any nonzero status in the reply, and copies the reply either way, so the chip's own
 *     verdict is only in the response's status (Linux reads it: morse_driver command.c:214).
 *     The functions are extracted from driver.c by name (the Makefile writes
 *     test_chipvif_glue.fns) and run against a morse_cmd_tx that answers as command.c does: it
 *     copies at most the reply the chip sent, so a status-only refusal leaves the rest of the
 *     response as it was, and returns COMMAND_ERROR for a refusal and the transport's status for
 *     a failed transport. Each *_status helper returns MMWLAN_SUCCESS whenever the chip
 *     answered, its verdict in the status out; a failed transport returns that status and
 *     writes no status. The plain twins return MMWLAN_COMMAND_ERROR for a refusal.
 *     (1) ADD_INTERFACE: a refusal is reported and leaves the VIF id alone; so does a
 *         transport error, with no status; mmdrv_add_if refused is COMMAND_ERROR;
 *     (2) GET_CAPABILITIES: a refusal leaves the capabilities alone; mmdrv_get_capabilities
 *         refused is COMMAND_ERROR (boot asserts it is SUCCESS);
 *     (3) BSSID_SET, BSS_BEACON_CONFIG, MESH_CONFIG: the status is read, and MESH_CONFIG
 *         carries MBCA and its timers 0 either way: beaconing, the STA VIF's tuple, or
 *         beaconless, the MESH VIF's and the OpenMANET Pis';
 *     (4) SET_STA_STATE: the status is read; without a status out a refusal is COMMAND_ERROR,
 *         as upstream's callers expect;
 *     (5) INSTALL_KEY: a refusal, whether the reply echoes an index or carries only the
 *         status, leaves the key's index and AT+KEYINST? alone and never asserts;
 *         mmdrv_install_key returns COMMAND_ERROR for it and counts it. An accepted key at
 *         another hw index is counted on a MESH-VIF build and asserts on the others.
 *  (6) driver/beacon/beacon.c, whole: only the VIF's beacon IRQ is counted (it wakes the driver
 *      task from the spi_irq task, not an ISR), a start resets
 *      the counts, and a host tick yields to a chip that schedules its own TBTT on a
 *      MESH-VIF build only.
 *  (7) umac/ps/umac_ps.c, whole: no CONFIG_PS to a MESH chip VIF; a STA one still gets it.
 *  (8) beacon.c's teardown, which mmdrv_deinit runs at a chip restart before it clears the driver's
 *      state: the host beacon timer is stopped (a tick after it beacons nothing) and kept, not
 *      leaked or freed under a running tick; the next start at the same period takes it back.
 *  (9) umac/health_check/umac_health_check.c, whole (upstream's check: a timeout on the umac
 *      event loop calling mmdrv_health_check): AT+CHIPRESTART (umac_health_check_force_fail) arms
 *      a failure and demands a check, at once; the timeout fails it without asking the chip and
 *      asks for a restart as after a real failure (mmdrv_host_hw_restart_required), once, counted
 *      forced; refused while the check is not started or its interval is 0. A passing check
 *      restarts nothing and re-arms for the interval; a check the chip fails twice (the probe and
 *      its retry) restarts it the same way. Starting or initialising the check disarms it.
 *  (10) umac_health_check_hang_view (mmwlan_hang_health): the timeout's run count and the interval,
 *      only while the check runs periodically (started, an interval, no veto); nothing written
 *      otherwise. Every run counts, one the check then skips (activity 1 s before: re-armed for the
 *      89 s left) too, and AT+HANGTEST=health holds the handler before it until released; another
 *      task's block does not.
 *
 * Built with WARTHOG_MESH_CHIP_VIF_MESH=1 (test_chipvif_glue) and without (_off).
 */
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "common/morse_commands.h"
#include "common/morse_command_utils.h"
#include "mmdrv.h"
#include "mmutils.h"
#include "mmwlan.h"
#include "driver/driver.h"
#include "driver/morse_driver/command.h"
#include "umac/core/umac_core.h"
#include "umac/data/umac_data.h"
#include "umac/health_check/umac_health_check.h"
#include "umac/interface/umac_interface.h"
#include "umac/ps/umac_ps_data.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* ---- what driver.c's extracted functions reach -------------------------- */

#define DRV_TRACE(...) do { } while (0)
static struct driver_data driver_data = { .started = true };
volatile uint32_t g_warthog_keyinst[8];
volatile uint32_t g_warthog_keyinst_n;
volatile uint32_t g_warthog_chipcmd_key_refused, g_warthog_chipcmd_keyidx_mismatch;
volatile int32_t g_warthog_chipcmd_key_status;
volatile uint32_t g_warthog_bcn_chip_irq, g_warthog_bcn_host_yield;
volatile uint32_t g_warthog_bcn_enq_ok, g_warthog_bcn_enq_err;

/* MMOSAL_ASSERT is live (MMOSAL_NOASSERT is set nowhere): its handler jumps back here. */
static jmp_buf s_assert_jb;
static bool s_assert_armed;
static unsigned s_asserts;
void mmosal_log_failure_info(const struct mmosal_failure_info *info) { (void)info; }
void mmosal_impl_assert(void)
{
    s_asserts++;
    if (s_assert_armed) { longjmp(s_assert_jb, 1); }
    printf("FAIL an assert fired outside a guarded call\n");
    failures++;
}

int mmosal_printf(const char *format, ...) { (void)format; return 0; }
const char *mmosal_task_name(void) { return "test"; }
static uint32_t s_now_ms; /* (9)-(10): the event loop's clock; 0 elsewhere */
uint32_t mmosal_get_time_ms(void) { return s_now_ms; }

/* The chip's answer to the next command: its status and the reply's payload after the
 * status, as many octets as it sends; or a failed transport's status instead. */
static struct {
    enum mmwlan_status ret;
    int32_t status;
    uint16_t vif_id;
    uint8_t body[64];
    uint16_t body_len;
} s_ans;
/* The last request, as sent. */
static uint8_t s_req[256];
static uint16_t s_req_id;

static void answer_(enum mmwlan_status ret, int32_t status, const void *body, uint16_t body_len)
{
    memset(&s_ans, 0, sizeof(s_ans));
    s_ans.ret = ret;
    s_ans.status = status;
    if (body != NULL) { memcpy(s_ans.body, body, body_len); }
    s_ans.body_len = body_len;
}

enum mmwlan_status morse_cmd_tx(struct driver_data *driverd, struct morse_cmd_resp *resp,
                                struct morse_cmd_req *cmd, uint32_t resp_maxlen, uint32_t timeout,
                                bool skip_errlog)
{
    (void)driverd; (void)timeout; (void)skip_errlog;
    s_req_id = le16toh(cmd->hdr.message_id);
    memcpy(s_req, cmd, MM_MIN(sizeof(s_req), sizeof(cmd->hdr) + le16toh(cmd->hdr.len)));
    if (s_ans.ret != MMWLAN_SUCCESS) { return s_ans.ret; }
    /* The chip's reply: header, status, body (command.c:184-216). */
    uint8_t rx[sizeof(struct morse_cmd_resp) + sizeof(s_ans.body)];
    struct morse_cmd_resp *r = (struct morse_cmd_resp *)rx;
    memset(rx, 0, sizeof(rx));
    r->hdr.message_id = cmd->hdr.message_id;
    r->hdr.vif_id = htole16(s_ans.vif_id);
    r->hdr.len = htole16((uint16_t)(sizeof(r->status) + s_ans.body_len));
    r->status = htole32((uint32_t)s_ans.status);
    memcpy(rx + sizeof(*r), s_ans.body, s_ans.body_len);
    const uint32_t rx_len = sizeof(r->hdr) + le16toh(r->hdr.len);
    if (resp_maxlen >= sizeof(struct morse_cmd_resp) && resp != NULL)
    {
        memcpy(resp, rx, MM_MIN(rx_len, resp_maxlen));
    }
    return s_ans.status == 0 ? MMWLAN_SUCCESS : MMWLAN_COMMAND_ERROR;
}

#include "test_chipvif_glue.fns"

/* ---- what beacon.c reaches ---------------------------------------------- */

static unsigned s_notify, s_notify_isr;
static timer_callback_t s_timer_cb;
static void *s_timer_arg;
static int s_timer_obj;
static unsigned s_timer_creates, s_timer_stops, s_timer_deletes;

void driver_task_notify_event(struct driver_data *driverd, enum driver_task_event evt)
{
    (void)driverd; (void)evt;
    s_notify++;
}
void driver_task_notify_event_from_isr(struct driver_data *driverd, enum driver_task_event evt)
{
    (void)driverd; (void)evt;
    s_notify_isr++;
}
bool driver_task_notification_check_and_clear(struct driver_data *driverd, enum driver_task_event evt)
{
    (void)driverd; (void)evt;
    return false;
}
int morse_hw_irq_enable(struct driver_data *driverd, uint32_t irq, bool enable)
{
    (void)driverd; (void)irq; (void)enable;
    return 0;
}
struct mmpkt *mmdrv_host_get_beacon(void) { return NULL; }
int morse_skbq_mmpkt_tx(struct morse_skbq *mq, struct mmpkt *mmpkt, uint8_t channel)
{
    (void)mq; (void)mmpkt; (void)channel;
    return 0;
}
struct mmosal_timer *mmosal_timer_create(const char *name, uint32_t timer_period, bool auto_reload,
                                         void *arg, timer_callback_t callback)
{
    (void)name; (void)timer_period; (void)auto_reload;
    s_timer_cb = callback;
    s_timer_arg = arg;
    s_timer_creates++;
    return (struct mmosal_timer *)&s_timer_obj;
}
void *mmosal_timer_get_arg(struct mmosal_timer *timer) { (void)timer; return s_timer_arg; }
bool mmosal_timer_start(struct mmosal_timer *timer) { (void)timer; return true; }
bool mmosal_timer_stop(struct mmosal_timer *timer) { (void)timer; s_timer_stops++; return true; }
void mmosal_timer_delete(struct mmosal_timer *timer) { (void)timer; s_timer_deletes++; }

#include "driver/beacon/beacon.c"

/* ---- what umac_ps.c reaches --------------------------------------------- */

static struct umac_ps_data s_ps;
static enum mmwlan_ps_mode s_ps_cfg;
static uint8_t s_chip_vif_type;
static unsigned s_ps_cmds;
static uint16_t s_ps_vif;
static bool s_ps_on;

struct umac_ps_data *umac_data_get_ps(struct umac_data *umacd) { (void)umacd; return &s_ps; }
enum mmwlan_ps_mode umac_config_get_ps_mode(struct umac_data *umacd) { (void)umacd; return s_ps_cfg; }
uint8_t umac_interface_get_chip_vif_type(struct umac_data *umacd) { (void)umacd; return s_chip_vif_type; }
/* The boot VIF, id 0, is the NONE/STA one umac_ps looks for; there is no AP. */
uint16_t umac_interface_get_vif_id(struct umac_data *umacd, uint16_t type_mask)
{
    (void)umacd;
    return (type_mask & UMAC_INTERFACE_AP) && !(type_mask & UMAC_INTERFACE_STA) ?
               MMDRV_VIF_ID_INVALID : 0;
}
enum mmwlan_status mmdrv_set_chip_power_save_enabled(uint16_t vif_id, bool enabled)
{
    s_ps_cmds++;
    s_ps_vif = vif_id;
    s_ps_on = enabled;
    return MMWLAN_SUCCESS;
}
enum mmwlan_status mmdrv_set_wake_enabled(bool enabled) { (void)enabled; return MMWLAN_SUCCESS; }

#include "umac/ps/umac_ps.c"

/* ---- what umac_health_check.c reaches ---------------------------------- */

volatile uint32_t g_warthog_chiprestart_forced;
volatile uint32_t g_warthog_hang_block;
static struct umac_health_check_data s_hc;
static unsigned s_checks, s_restarts;
static enum mmwlan_status s_check_ret;
/* The one timeout the check keeps on the event loop: armed, its handler and its delay. */
static struct { bool armed; umac_core_timeout_handler_t h; void *a1, *a2; uint32_t delta; } s_to;
static unsigned s_to_regs;

struct umac_health_check_data *umac_data_get_health_check(struct umac_data *umacd)
{
    (void)umacd;
    return &s_hc;
}
bool umac_core_is_running(struct umac_data *umacd) { (void)umacd; return true; }
bool umac_core_register_timeout(struct umac_data *umacd, uint32_t delta_ms,
                                umac_core_timeout_handler_t handler, void *arg1, void *arg2)
{
    (void)umacd;
    s_to.armed = true;
    s_to.h = handler;
    s_to.a1 = arg1;
    s_to.a2 = arg2;
    s_to.delta = delta_ms;
    s_to_regs++;
    return true;
}
int umac_core_cancel_timeout(struct umac_data *umacd, umac_core_timeout_handler_t handler,
                             void *arg1, void *arg2)
{
    (void)umacd; (void)arg1; (void)arg2;
    const int n = (s_to.armed && s_to.h == handler) ? 1 : 0;
    s_to.armed = false;
    return n;
}
/* Counted; the s_sleep_release-th clears g_warthog_hang_block (0: never), as AT+HANGTEST=off would. */
static unsigned s_sleeps, s_sleep_release;
static uint32_t s_sleep_ms, s_sleep_wakes;
static const volatile uint32_t *s_wakes; /* the check's run count, read at each sleep */
void mmosal_task_sleep(uint32_t duration_ms)
{
    s_sleep_ms += duration_ms;
    if (s_wakes != NULL) { s_sleep_wakes = *s_wakes; }
    if (++s_sleeps == s_sleep_release) { g_warthog_hang_block = 0; }
}
enum mmwlan_status mmdrv_health_check(void) { s_checks++; return s_check_ret; }
void mmdrv_host_hw_restart_required(void) { s_restarts++; }

#include "umac/health_check/umac_health_check.c"

/* ---- the cases ---------------------------------------------------------- */

static void t_add_if(void)
{
    int32_t st = 12345;
    uint16_t vif = 0xbeef;
    static const uint8_t mac[6] = { 0x02, 0, 0, 0, 0, 0x01 };

    answer_(MMWLAN_SUCCESS, 0, NULL, 0);
    s_ans.vif_id = 3;
    CHECK(mmdrv_add_if_status(&vif, mac, MMDRV_INTERFACE_TYPE_MESH, &st) == MMWLAN_SUCCESS && st == 0 &&
              vif == 3,
          "(1) an accepted ADD_INTERFACE hands out the chip's VIF id (%u, st %ld)", vif, (long)st);
    const struct morse_cmd_req_add_interface *req = (const void *)s_req;
    CHECK(s_req_id == MORSE_CMD_ID_ADD_INTERFACE && le32toh(req->interface_type) == 5u &&
              le16toh(req->hdr.vif_id) == MMDRV_VIF_ID_INVALID,
          "(1) and it asked for a MESH interface (type %lu) with no VIF id yet",
          (unsigned long)le32toh(req->interface_type));

    vif = 0xbeef;
    answer_(MMWLAN_SUCCESS, -22, NULL, 0);
    s_ans.vif_id = MMDRV_VIF_ID_INVALID;
    CHECK(mmdrv_add_if_status(&vif, mac, MMDRV_INTERFACE_TYPE_MESH, &st) == MMWLAN_SUCCESS && st == -22,
          "(1) a refused ADD_INTERFACE is answered, with the chip's status (%ld)", (long)st);
    CHECK(vif == 0xbeef, "(1) and leaves the VIF id alone (%#x)", vif);
    answer_(MMWLAN_SUCCESS, -22, NULL, 0);
    s_ans.vif_id = 4;
    CHECK(mmdrv_add_if(&vif, mac, MMDRV_INTERFACE_TYPE_STA) == MMWLAN_COMMAND_ERROR && vif == 0xbeef,
          "(1) mmdrv_add_if refused is COMMAND_ERROR, the VIF id alone, whatever the reply's header says");
    answer_(MMWLAN_SUCCESS, 0, NULL, 0);
    s_ans.vif_id = 4;
    CHECK(mmdrv_add_if(&vif, mac, MMDRV_INTERFACE_TYPE_STA) == MMWLAN_SUCCESS && vif == 4,
          "(1) and accepted is SUCCESS with the chip's VIF id (%u)", vif);

    st = 12345;
    vif = 0xbeef;
    answer_(MMWLAN_TIMED_OUT, 0, NULL, 0);
    CHECK(mmdrv_add_if_status(&vif, mac, MMDRV_INTERFACE_TYPE_MESH, &st) == MMWLAN_TIMED_OUT && st == 12345 &&
              vif == 0xbeef,
          "(1) a transport error returns it, writing no status and leaving the VIF id alone (st %ld)", (long)st);
}

static void t_get_caps(void)
{
    struct morse_caps caps;
    int32_t st = 12345;
    memset(&caps, 0xa5, sizeof(caps));
    const struct morse_caps before = caps;
    answer_(MMWLAN_SUCCESS, -1, NULL, 0);
    CHECK(mmdrv_get_capabilities_status(0, &caps, &st) == MMWLAN_SUCCESS && st == -1,
          "(2) a refused GET_CAPABILITIES is answered, with its status (%ld)", (long)st);
    CHECK(memcmp(&caps, &before, sizeof(caps)) == 0, "(2) and leaves the capabilities alone");
    answer_(MMWLAN_SUCCESS, -1, NULL, 0);
    CHECK(mmdrv_get_capabilities(MMDRV_VIF_ID_INVALID, &caps) == MMWLAN_COMMAND_ERROR &&
              memcmp(&caps, &before, sizeof(caps)) == 0,
          "(2) mmdrv_get_capabilities refused is COMMAND_ERROR, the capabilities alone");

    struct morse_cmd_resp_get_capabilities body;
    memset(&body, 0, sizeof(body));
    body.capabilities.ampdu_mss = 7;
    answer_(MMWLAN_SUCCESS, 0, (const uint8_t *)&body + sizeof(struct morse_cmd_resp),
            (uint16_t)(sizeof(body) - sizeof(struct morse_cmd_resp)));
    CHECK(mmdrv_get_capabilities_status(0, &caps, &st) == MMWLAN_SUCCESS && st == 0 && caps.ampdu_mss == 7,
          "(2) accepted ones are read (ampdu_mss %u)", (unsigned)caps.ampdu_mss);
    caps.ampdu_mss = 0;
    answer_(MMWLAN_SUCCESS, 0, (const uint8_t *)&body + sizeof(struct morse_cmd_resp),
            (uint16_t)(sizeof(body) - sizeof(struct morse_cmd_resp)));
    CHECK(mmdrv_get_capabilities(MMDRV_VIF_ID_INVALID, &caps) == MMWLAN_SUCCESS && caps.ampdu_mss == 7 &&
              le16toh(((const struct morse_cmd_req_get_capabilities *)(const void *)s_req)->hdr.vif_id) ==
                  MMDRV_VIF_ID_INVALID,
          "(2) and boot's read, with no VIF, is SUCCESS");
}

static void t_bss_mesh_status(void)
{
    static const uint8_t bssid[6] = { 0x02, 1, 2, 3, 4, 0x5a };
    int32_t st;

    answer_(MMWLAN_SUCCESS, -13, NULL, 0);
    CHECK(mmdrv_set_bssid(1, bssid, &st) == MMWLAN_SUCCESS && st == -13 && s_req_id == MORSE_CMD_ID_BSSID_SET,
          "(3) BSSID_SET: the refusal's status is read (%ld)", (long)st);
    st = 12345;
    answer_(MMWLAN_TIMED_OUT, 0, NULL, 0);
    CHECK(mmdrv_set_bssid(1, bssid, &st) == MMWLAN_TIMED_OUT && st == 12345,
          "(3) BSSID_SET: a transport error is returned, no status written");

    answer_(MMWLAN_SUCCESS, -7, NULL, 0);
    CHECK(mmdrv_cfg_bss_beacon(1, true, &st) == MMWLAN_SUCCESS && st == -7 &&
              s_req_id == MORSE_CMD_ID_BSS_BEACON_CONFIG,
          "(3) BSS_BEACON_CONFIG: the refusal's status is read (%ld)", (long)st);
    answer_(MMWLAN_SUCCESS, 0, NULL, 0);
    CHECK(mmdrv_cfg_bss_beacon(1, true, &st) == MMWLAN_SUCCESS && st == 0, "(3) BSS_BEACON_CONFIG: accepted is 0");
    st = 12345;
    answer_(MMWLAN_TIMED_OUT, 0, NULL, 0);
    CHECK(mmdrv_cfg_bss_beacon(1, true, &st) == MMWLAN_TIMED_OUT && st == 12345,
          "(3) BSS_BEACON_CONFIG: a transport error is returned, no status written");

    answer_(MMWLAN_SUCCESS, -22, NULL, 0);
    CHECK(mmdrv_mesh_config(1, true, true, &st) == MMWLAN_SUCCESS && st == -22 &&
              s_req_id == MORSE_CMD_ID_MESH_CONFIG,
          "(3) MESH_CONFIG: the refusal's status is read (%ld)", (long)st);
    st = 12345;
    answer_(MMWLAN_TIMED_OUT, 0, NULL, 0);
    CHECK(mmdrv_mesh_config(1, true, true, &st) == MMWLAN_TIMED_OUT && st == 12345,
          "(3) MESH_CONFIG: a transport error is returned, no status written");

    const struct morse_cmd_req_mesh_config *mc = (const void *)s_req;
    answer_(MMWLAN_SUCCESS, 0, NULL, 0);
    (void)mmdrv_mesh_config(1, true, true, &st);
    CHECK(mc->mesh_cfg_opcode == MORSE_CMD_MESH_CONFIG_OPCODE_START && mc->enable_beaconing == 1 &&
              mc->mbca_config == 0 && mc->min_beacon_gap_ms == 0 &&
              mc->mbss_start_scan_duration_ms == 0 && mc->tbtt_adj_timer_interval_ms == 0,
          "(3) MESH_CONFIG(START) beaconing: MBCA and its timers 0, as the fork has always sent");
    (void)mmdrv_mesh_config(1, true, false, &st);
    CHECK(mc->mesh_cfg_opcode == MORSE_CMD_MESH_CONFIG_OPCODE_START && mc->enable_beaconing == 0 &&
              mc->mbca_config == 0 && mc->min_beacon_gap_ms == 0 &&
              mc->mbss_start_scan_duration_ms == 0 && mc->tbtt_adj_timer_interval_ms == 0,
          "(3) MESH_CONFIG(START) beaconless: beaconing, MBCA and its timers 0, as the OpenMANET Pis send");
}

static void t_sta_state(void)
{
    static const uint8_t peer[6] = { 0x02, 0, 0, 0, 0, 0x0a };
    int32_t st;
    answer_(MMWLAN_SUCCESS, -1, NULL, 0);
    CHECK(mmdrv_update_sta_state_status(1, 2, peer, MORSE_STA_NONE, &st) == MMWLAN_SUCCESS && st == -1 &&
              s_req_id == MORSE_CMD_ID_SET_STA_STATE,
          "(4) SET_STA_STATE: the refusal's status is read (%ld)", (long)st);
    answer_(MMWLAN_SUCCESS, 0, NULL, 0);
    CHECK(mmdrv_update_sta_state_status(1, 2, peer, MORSE_STA_AUTHORIZED, &st) == MMWLAN_SUCCESS && st == 0,
          "(4) SET_STA_STATE: accepted is 0");
    answer_(MMWLAN_SUCCESS, -1, NULL, 0);
    CHECK(mmdrv_update_sta_state(1, 2, peer, MORSE_STA_NONE) == MMWLAN_COMMAND_ERROR,
          "(4) without a status out a refusal is COMMAND_ERROR, as upstream's callers expect");
    answer_(MMWLAN_SUCCESS, 0, NULL, 0);
    CHECK(mmdrv_update_sta_state(1, 2, peer, MORSE_STA_NONE) == MMWLAN_SUCCESS, "(4) and accepted SUCCESS");
    st = 12345;
    answer_(MMWLAN_NO_MEM, 0, NULL, 0);
    CHECK(mmdrv_update_sta_state_status(1, 2, peer, MORSE_STA_NONE, &st) == MMWLAN_NO_MEM && st == 12345 &&
              mmdrv_update_sta_state(1, 2, peer, MORSE_STA_NONE) == MMWLAN_NO_MEM,
          "(4) a failed transport is returned by both, no status written");
}

/* Install a group key asking for index @p want; true if it asserted. */
static bool install_(enum mmwlan_status (*fn)(uint16_t, uint16_t, struct mmdrv_key_conf *, int32_t *),
                     uint8_t want, struct mmdrv_key_conf *kc, enum mmwlan_status *ret, int32_t *st)
{
    memset(kc, 0, sizeof(*kc));
    kc->key_idx = want;
    kc->length = 16;
    s_assert_armed = true;
    if (setjmp(s_assert_jb) != 0)
    {
        s_assert_armed = false;
        return true;
    }
    *ret = fn != NULL ? fn(0, 0, kc, st) : mmdrv_install_key(0, 0, kc);
    s_assert_armed = false;
    return false;
}

static void t_install_key(void)
{
    struct mmdrv_key_conf kc;
    enum mmwlan_status ret = MMWLAN_SUCCESS;
    int32_t st = 0;
    const uint8_t idx1 = 1, idx0 = 0, idx2 = 2;

    g_warthog_keyinst_n = 0;
    answer_(MMWLAN_SUCCESS, 0, &idx1, 1);
    CHECK(!install_(mmdrv_install_key_status, 1, &kc, &ret, &st) && ret == MMWLAN_SUCCESS && st == 0 &&
              kc.key_idx == 1 && g_warthog_keyinst_n == 1 && s_req_id == MORSE_CMD_ID_INSTALL_KEY,
          "(5) an accepted key is recorded at the chip's hw index (%u, keyinst %lu)",
          (unsigned)kc.key_idx, (unsigned long)g_warthog_keyinst_n);

    g_warthog_keyinst_n = 0;
    answer_(MMWLAN_SUCCESS, -95, NULL, 0);
    CHECK(!install_(mmdrv_install_key_status, 1, &kc, &ret, &st),
          "(5) a status-only refusal does not assert");
    CHECK(ret == MMWLAN_SUCCESS && st == -95 && kc.key_idx == 1 && g_warthog_keyinst_n == 0,
          "(5) it is answered with its status and leaves the key and AT+KEYINST? alone (%ld, %u, %lu)",
          (long)st, (unsigned)kc.key_idx, (unsigned long)g_warthog_keyinst_n);

    answer_(MMWLAN_SUCCESS, -95, &idx0, 1);
    CHECK(!install_(mmdrv_install_key_status, 1, &kc, &ret, &st) && st == -95 && kc.key_idx == 1,
          "(5) a refusal echoing another index does not assert and leaves the key alone");
    answer_(MMWLAN_SUCCESS, -95, &idx1, 1);
    CHECK(!install_(mmdrv_install_key_status, 1, &kc, &ret, &st) && st == -95 && g_warthog_keyinst_n == 0,
          "(5) nor is one echoing the index asked for taken as installed");

    g_warthog_chipcmd_key_refused = 0;
    g_warthog_chipcmd_key_status = 0;
    answer_(MMWLAN_SUCCESS, -95, NULL, 0);
    CHECK(!install_(NULL, 1, &kc, &ret, &st) && ret == MMWLAN_COMMAND_ERROR && kc.key_idx == 1,
          "(5) mmdrv_install_key returns a refusal as COMMAND_ERROR (%d)", (int)ret);
    CHECK(g_warthog_chipcmd_key_refused == 1 && g_warthog_chipcmd_key_status == -95,
          "(5) and counts it with its status (%lu, %ld)", (unsigned long)g_warthog_chipcmd_key_refused,
          (long)g_warthog_chipcmd_key_status);

    g_warthog_chipcmd_keyidx_mismatch = 0;
    answer_(MMWLAN_SUCCESS, 0, &idx2, 1);
    const bool asserted = install_(mmdrv_install_key_status, 1, &kc, &ret, &st);
#if WARTHOG_MESH_CHIP_VIF_MESH
    CHECK(!asserted && ret == MMWLAN_SUCCESS && st == 0 && kc.key_idx == 2 &&
              g_warthog_chipcmd_keyidx_mismatch == 1,
          "(5) an accepted key at another hw index is counted, not asserted (hw %u, %lu)",
          (unsigned)kc.key_idx, (unsigned long)g_warthog_chipcmd_keyidx_mismatch);
#else
    CHECK(asserted && g_warthog_chipcmd_keyidx_mismatch == 0,
          "(5) an accepted key at another hw index asserts, as upstream does");
#endif
}

static void t_beacon(void)
{
    struct driver_data d;
    memset(&d, 0, sizeof(d));
    const uint32_t mine = 1ul << (MORSE_INT_BEACON_BASE_NUM + 1);
    const uint32_t other = 1ul << (MORSE_INT_BEACON_BASE_NUM + 0);

    d.beacon.chip_irqs = 9;
    d.beacon.chip_irqs_seen = 9;
    (void)morse_beacon_start(&d, 1, 100);
    CHECK(d.beacon.chip_irqs == 0 && d.beacon.chip_irqs_seen == 0 && s_timer_cb != NULL,
          "(6) a start resets the chip's beacon IRQ counts and arms the host timer");

    /* The IRQ handler runs on the spi_irq task, so it posts the non-ISR notify. */
    g_warthog_bcn_chip_irq = 0;
    s_notify = s_notify_isr = 0;
    morse_beacon_irq_handle(&d, other);
    CHECK(d.beacon.chip_irqs == 0 && g_warthog_bcn_chip_irq == 0 && s_notify == 0 && s_notify_isr == 0,
          "(6) another VIF's beacon IRQ is not counted");
    morse_beacon_irq_handle(&d, mine | other);
    CHECK(d.beacon.chip_irqs == 1 && g_warthog_bcn_chip_irq == 1 && s_notify == 1 && s_notify_isr == 0,
          "(6) this VIF's is counted, once, and wakes the driver task (%lu)", (unsigned long)d.beacon.chip_irqs);

    /* Kickoff IRQ, host tick, then a TBTT the chip scheduled itself, host tick. */
    s_notify = 0;
    g_warthog_bcn_host_yield = 0;
    s_timer_cb((struct mmosal_timer *)&s_timer_obj);
    CHECK(s_notify == 1, "(6) the tick after the kickoff IRQ alone beacons from the host");
    morse_beacon_irq_handle(&d, mine);
    s_notify--; /* the IRQ's own wake, above */
    s_timer_cb((struct mmosal_timer *)&s_timer_obj);
#if WARTHOG_MESH_CHIP_VIF_MESH
    CHECK(s_notify == 1 && g_warthog_bcn_host_yield == 1,
          "(6) a chip scheduling its own TBTT takes the next tick (notify %u, yield %lu)", s_notify,
          (unsigned long)g_warthog_bcn_host_yield);
    s_timer_cb((struct mmosal_timer *)&s_timer_obj);
    CHECK(s_notify == 2, "(6) and a tick with no IRQ since is the host's again");
#else
    CHECK(s_notify == 2 && g_warthog_bcn_host_yield == 0,
          "(6) without the MESH-VIF flag every tick beacons from the host (notify %u)", s_notify);
#endif
    (void)morse_beacon_stop(&d);
}

static void t_ps(void)
{
    s_ps_cfg = MMWLAN_PS_ENABLED;
    s_chip_vif_type = MMDRV_INTERFACE_TYPE_MESH;
    umac_ps_reset(NULL);
    s_ps_cmds = 0;
    umac_ps_update_mode(NULL);
#if WARTHOG_MESH_CHIP_VIF_MESH
    CHECK(s_ps_cmds == 0 && s_ps.pwr_mode == MMWLAN_PS_DISABLED,
          "(7) power save on, MESH chip VIF: no CONFIG_PS (%u sent)", s_ps_cmds);
#else
    CHECK(s_ps_cmds == 1, "(7) without the flag the chip VIF is never taken as MESH");
#endif

    s_chip_vif_type = MMDRV_INTERFACE_TYPE_STA;
    umac_ps_reset(NULL);
    s_ps_cmds = 0;
    umac_ps_update_mode(NULL);
    CHECK(s_ps_cmds == 1 && s_ps_vif == 0 && s_ps_on && s_ps.pwr_mode == MMWLAN_PS_ENABLED,
          "(7) a STA chip VIF still gets CONFIG_PS (%u sent)", s_ps_cmds);
}

static void t_beacon_teardown(void)
{
    struct driver_data d;
    memset(&d, 0, sizeof(d));
    s_timer_creates = s_timer_stops = s_timer_deletes = 0;
    (void)morse_beacon_start(&d, 0, 102);
    struct mmosal_timer *t = d.beacon.host_timer;
    CHECK(t != NULL && s_timer_creates == 1u && d.beacon.enabled, "(8) a start creates the host beacon timer");
    morse_beacon_teardown(&d);
    CHECK(!d.beacon.enabled && d.beacon.host_timer == NULL && s_timer_stops == 1u && s_timer_deletes == 0u,
          "(8) teardown stops it and takes it out of the driver state, not freed under a running tick");
    s_notify = 0;
    s_timer_cb((struct mmosal_timer *)&s_timer_obj);
    CHECK(s_notify == 0u, "(8) a tick after it beacons nothing");
    memset(&d, 0, sizeof(d)); /* mmdrv_deinit, then mmdrv_init, clear the driver state */
    (void)morse_beacon_start(&d, 0, 102);
    CHECK(d.beacon.host_timer == t && s_timer_creates == 1u && s_timer_deletes == 0u,
          "(8) the next start at that period takes the same timer back (created %u)", s_timer_creates);
    morse_beacon_teardown(&d);
    memset(&d, 0, sizeof(d));
    (void)morse_beacon_start(&d, 0, 204);
    CHECK(s_timer_deletes == 1u && s_timer_creates == 2u && d.beacon.host_timer != NULL,
          "(8) a start at another period deletes the kept one and creates its own");
    morse_beacon_teardown(&d);
    morse_beacon_teardown(&d);
    CHECK(s_timer_stops == 3u, "(8) a teardown with no timer does nothing (%u stops)", s_timer_stops);
}

/* The event loop runs the check's armed timeout once, as it fires: dequeued, then run. */
static bool fire_(void)
{
    if (!s_to.armed) { return false; }
    s_to.armed = false;
    s_to.h(s_to.a1, s_to.a2);
    return true;
}

static void reset_health_(void)
{
    s_now_ms = 1000u;
    umac_health_check_init(NULL);
    s_to.armed = false;
    s_checks = s_restarts = s_to_regs = 0;
    s_check_ret = MMWLAN_SUCCESS;
    g_warthog_chiprestart_forced = 0;
    g_warthog_hang_block = 0;
}

static void t_health_force(void)
{
    reset_health_();
    CHECK(!umac_health_check_force_fail(NULL) && !s_hc.force_fail && !s_to.armed,
          "(9) AT+CHIPRESTART before the check starts: refused, nothing armed");
    umac_health_check_start(NULL);
    CHECK(s_to.armed && s_to.delta == 0u && fire_() && s_checks == 0u && s_to.armed &&
              s_to.delta == s_hc.interval_ms && s_hc.interval_ms == 90000u,
          "(9) a start arms the check at once; it is not due, so it re-arms for the 90 s interval (%lu)",
          (unsigned long)s_to.delta);
    CHECK(umac_health_check_force_fail(NULL) && s_hc.force_fail && s_hc.check_demanded && s_to.armed &&
              s_to.delta == 0u && s_restarts == 0u,
          "(9) AT+CHIPRESTART arms the failure and demands a check at once; nothing fails yet");
    CHECK(fire_() && s_checks == 0u && s_restarts == 1u && !s_to.armed,
          "(9) the timeout fails it without asking the chip and asks for a restart, re-arming nothing "
          "(checks %u, restarts %u)", s_checks, s_restarts);
    CHECK(!s_hc.force_fail && !s_hc.check_demanded && g_warthog_chiprestart_forced == 1u,
          "(9) once, counted forced 1 (%lu)", (unsigned long)g_warthog_chiprestart_forced);
    s_restarts = 0;
    umac_health_check_demand_check(NULL);
    CHECK(fire_() && s_checks == 1u && s_restarts == 0u && s_to.armed && s_to.delta == 90000u,
          "(9) the next demanded check asks the chip and, passing, restarts nothing and re-arms (%lu)",
          (unsigned long)s_to.delta);
    s_check_ret = MMWLAN_TIMED_OUT;
    umac_health_check_demand_check(NULL);
    CHECK(fire_() && s_checks == 3u && s_restarts == 1u && !s_to.armed,
          "(9) a check the chip fails, and its retry, takes the same path (checks %u)", s_checks);
    s_check_ret = MMWLAN_SUCCESS;
    s_hc.force_fail = true;
    umac_health_check_start(NULL);
    CHECK(!s_hc.force_fail, "(9) a start disarms a failure armed before it");
    s_hc.force_fail = true;
    s_hc.wakes = 5u;
    umac_health_check_init(NULL);
    CHECK(!s_hc.force_fail && s_hc.wakes == 0u, "(9) and so does initialising the check, its runs counted from 0");
    umac_health_check_start(NULL);
    s_hc.interval_ms = 0u;
    CHECK(!umac_health_check_force_fail(NULL) && !s_hc.force_fail,
          "(9) refused at interval 0, where no check would ever run it");
    s_hc.interval_ms = 90000u;
    umac_health_check_stop(NULL);
    CHECK(!umac_health_check_force_fail(NULL) && !s_hc.force_fail && !s_to.armed,
          "(9) and once the check is stopped");
}

static void t_hang(void)
{
    reset_health_();
    umac_health_check_start(NULL);
    s_hc.wakes = 61234u;
    uint32_t wakes = 7u, iv = 8u;
    CHECK(umac_health_check_hang_view(NULL, &wakes, &iv) && wakes == 61234u && iv == 90000u,
          "(10) hang_view: the timeout's runs and the interval while the check runs (%lu, %lu)",
          (unsigned long)wakes, (unsigned long)iv);
    static const char *const off[3] = { "the check stopped", "a veto set", "interval 0" };
    for (int c = 0; c < 3; c++) {
        s_hc.is_started = c != 0;
        s_hc.periodic_check_vetoes = c == 1 ? 1ul << UMAC_HEALTH_CHECK_VETO_ID_WNM_SLEEP : 0ul;
        s_hc.interval_ms = c == 2 ? 0u : 90000u;
        wakes = 7u;
        iv = 8u;
        CHECK(!umac_health_check_hang_view(NULL, &wakes, &iv) && wakes == 7u && iv == 8u,
              "(10) hang_view with %s: false, nothing written", off[c]);
    }
    s_hc.is_started = true;
    s_hc.periodic_check_vetoes = 0;
    s_hc.interval_ms = 90000u;

    s_hc.wakes = 0;
    s_sleeps = 0;
    s_sleep_ms = 0;
    s_sleep_release = 3u;
    s_wakes = &s_hc.wakes;
    s_sleep_wakes = UINT32_MAX;
    g_warthog_hang_block = MMWLAN_HANG_BLOCK_HEALTH;
    umac_health_check_demand_check(NULL);
    CHECK(fire_() && s_sleeps == 3u && s_sleep_ms == 3000u && s_checks == 1u && g_warthog_hang_block == 0u &&
              s_sleep_wakes == 0u && s_hc.wakes == 1u,
          "(10) AT+HANGTEST=health: a demanded run waits 1 s at a time, uncounted, until released, then counts and "
          "checks (%u sleeps, %lu ms, %u checks, %lu runs counted while held)", s_sleeps, (unsigned long)s_sleep_ms,
          s_checks, (unsigned long)s_sleep_wakes);
    s_sleeps = 0;
    s_sleep_ms = 0;
    s_sleep_release = 5u; /* ends a wrong wait rather than hang the test */
    g_warthog_hang_block = MMWLAN_HANG_BLOCK_DRV;
    umac_health_check_demand_check(NULL);
    CHECK(fire_() && s_sleeps == 0u && s_checks == 2u,
          "(10) another task's block (drv) does not hold the check (%u sleeps)", s_sleeps);
    g_warthog_hang_block = 0;

    /* A meshed node: every RX notes activity, so the periodic run finds it recent and skips. */
    s_now_ms = 200000u;
    for (int blocked = 0; blocked < 2; blocked++) {
        umac_health_check_note_activity(NULL);
        s_now_ms += 1000u;
        umac_health_check_schedule_timeout(NULL, 0);
        s_hc.wakes = 0;
        s_checks = 0;
        s_sleeps = 0;
        s_sleep_release = 3u;
        g_warthog_hang_block = blocked ? MMWLAN_HANG_BLOCK_HEALTH : 0u;
        CHECK(fire_() && s_checks == 0u && s_hc.wakes == 1u && s_sleeps == (blocked ? 3u : 0u) && s_to.armed &&
                  s_to.delta == 89000u,
              "(10) a run the check skips, activity 1 s before: %s, counted, no check, re-armed for 89 s "
              "(%u sleeps, %lu wakes, %lu ms)", blocked ? "AT+HANGTEST=health holds it" : "not held", s_sleeps,
              (unsigned long)s_hc.wakes, (unsigned long)s_to.delta);
    }
    g_warthog_hang_block = 0;
    s_now_ms = 0;
    s_sleep_release = 0;
}

int main(void)
{
    printf("=== chip VIF glue: driver.c status, beacon.c, umac_ps.c, umac_health_check.c "
           "(WARTHOG_MESH_CHIP_VIF_MESH=%d) ===\n",
           (int)WARTHOG_MESH_CHIP_VIF_MESH);
    t_add_if();
    t_get_caps();
    t_bss_mesh_status();
    t_sta_state();
    t_install_key();
    t_beacon();
    t_ps();
    t_beacon_teardown();
    t_health_force();
    t_hang();
    CHECK(s_asserts == (WARTHOG_MESH_CHIP_VIF_MESH ? 0u : 1u), "no assert fired but the one expected (%u)",
          s_asserts);
    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_chipvif_glue%s: all passed\n", WARTHOG_MESH_CHIP_VIF_MESH ? "" : "_off");
    return 0;
}
