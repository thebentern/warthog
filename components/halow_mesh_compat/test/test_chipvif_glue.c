/*
 * The chip VIF pieces no simnode links, compiled out of the SDK and run on stubs. simnode
 * replaces driver.c with its fake chip and stubs umac_ps.c, so without this nothing runs them.
 *
 *  (1-5) driver.c's reads of the chip's own status. morse_cmd_tx returns 0 once any response
 *     arrives when it is given a response buffer, so the chip's verdict is only in the
 *     response's status (Linux reads it: morse_driver command.c:214). The functions are
 *     extracted from driver.c by name (the Makefile writes test_chipvif_glue.fns) and run
 *     against a morse_cmd_tx that answers as command.c does: it copies at most the reply the
 *     chip sent, so a status-only refusal leaves the rest of the response as it was.
 *     (1) ADD_INTERFACE: a refusal is reported and leaves the VIF id alone; so does a
 *         transport error, with no status;
 *     (2) GET_CAPABILITIES: a refusal leaves the capabilities alone;
 *     (3) BSSID_SET, BSS_BEACON_CONFIG, MESH_CONFIG: the status is read, and MESH_CONFIG
 *         carries MBCA and its timers 0 either way: beaconing, the STA VIF's tuple, or
 *         beaconless, the MESH VIF's and the OpenMANET Pis';
 *     (4) SET_STA_STATE: the status is read; without a status out a refusal is still 0;
 *     (5) INSTALL_KEY: a refusal, whether the reply echoes an index or carries only the
 *         status, leaves the key's index and AT+KEYINST? alone and never asserts;
 *         mmdrv_install_key returns it as an error and counts it. An accepted key at
 *         another hw index is counted on a MESH-VIF build and asserts on the others.
 *  (6) driver/beacon/beacon.c, whole: only the VIF's beacon IRQ is counted, a start resets
 *      the counts, and a host tick yields to a chip that schedules its own TBTT on a
 *      MESH-VIF build only.
 *  (7) umac/ps/umac_ps.c, whole: no CONFIG_PS to a MESH chip VIF; a STA one still gets it.
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
#include "umac/data/umac_data.h"
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
uint32_t mmosal_get_time_ms(void) { return 0; }

/* The chip's answer to the next command: its status and the reply's payload after the
 * status, as many octets as it sends; or a transport error instead. */
static struct {
    int ret;
    int32_t status;
    uint16_t vif_id;
    uint8_t body[64];
    uint16_t body_len;
} s_ans;
/* The last request, as sent. */
static uint8_t s_req[256];
static uint16_t s_req_id;

static void answer_(int ret, int32_t status, const void *body, uint16_t body_len)
{
    memset(&s_ans, 0, sizeof(s_ans));
    s_ans.ret = ret;
    s_ans.status = status;
    if (body != NULL) { memcpy(s_ans.body, body, body_len); }
    s_ans.body_len = body_len;
}

int morse_cmd_tx(struct driver_data *driverd, struct morse_cmd_resp *resp,
                 struct morse_cmd_req *cmd, uint32_t resp_maxlen, uint32_t timeout)
{
    (void)driverd; (void)timeout;
    s_req_id = le16toh(cmd->hdr.message_id);
    memcpy(s_req, cmd, MM_MIN(sizeof(s_req), sizeof(cmd->hdr) + le16toh(cmd->hdr.len)));
    if (s_ans.ret != 0) { return s_ans.ret; }
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
        return 0;
    }
    return (int)s_ans.status;
}

#include "test_chipvif_glue.fns"

/* ---- what beacon.c reaches ---------------------------------------------- */

static unsigned s_notify, s_notify_isr;
static timer_callback_t s_timer_cb;
static void *s_timer_arg;
static int s_timer_obj;

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
    return (struct mmosal_timer *)&s_timer_obj;
}
void *mmosal_timer_get_arg(struct mmosal_timer *timer) { (void)timer; return s_timer_arg; }
bool mmosal_timer_start(struct mmosal_timer *timer) { (void)timer; return true; }
bool mmosal_timer_stop(struct mmosal_timer *timer) { (void)timer; return true; }

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
               UMAC_INTERFACE_VIF_ID_INVALID : 0;
}
int mmdrv_set_chip_power_save_enabled(uint16_t vif_id, bool enabled)
{
    s_ps_cmds++;
    s_ps_vif = vif_id;
    s_ps_on = enabled;
    return 0;
}
int mmdrv_set_wake_enabled(bool enabled) { (void)enabled; return 0; }

#include "umac/ps/umac_ps.c"

/* ---- the cases ---------------------------------------------------------- */

static void t_add_if(void)
{
    int32_t st = 12345;
    uint16_t vif = 0xbeef;
    static const uint8_t mac[6] = { 0x02, 0, 0, 0, 0, 0x01 };

    answer_(0, 0, NULL, 0);
    s_ans.vif_id = 3;
    CHECK(mmdrv_add_if_status(&vif, mac, MMDRV_INTERFACE_TYPE_MESH, &st) == 0 && st == 0 && vif == 3,
          "(1) an accepted ADD_INTERFACE hands out the chip's VIF id (%u, st %ld)", vif, (long)st);
    const struct morse_cmd_req_add_interface *req = (const void *)s_req;
    CHECK(s_req_id == MORSE_CMD_ID_ADD_INTERFACE && le32toh(req->interface_type) == 5u,
          "(1) and it asked for a MESH interface (type %lu)", (unsigned long)le32toh(req->interface_type));

    vif = 0xbeef;
    answer_(0, -22, NULL, 0);
    s_ans.vif_id = UNKNOWN_VIF_ID;
    CHECK(mmdrv_add_if_status(&vif, mac, MMDRV_INTERFACE_TYPE_MESH, &st) == 0 && st == -22,
          "(1) a refused ADD_INTERFACE is answered, with the chip's status (%ld)", (long)st);
    CHECK(vif == 0xbeef, "(1) and leaves the VIF id alone (%#x)", vif);

    st = 12345;
    answer_(-110, 0, NULL, 0);
    CHECK(mmdrv_add_if_status(&vif, mac, MMDRV_INTERFACE_TYPE_MESH, &st) == -110 && st == 0 && vif == 0xbeef,
          "(1) a transport error returns it, with no status and the VIF id alone (st %ld)", (long)st);
}

static void t_get_caps(void)
{
    struct morse_caps caps;
    int32_t st = 12345;
    memset(&caps, 0xa5, sizeof(caps));
    const struct morse_caps before = caps;
    answer_(0, -1, NULL, 0);
    CHECK(mmdrv_get_capabilities_status(0, &caps, &st) == 0 && st == -1,
          "(2) a refused GET_CAPABILITIES is answered, with its status (%ld)", (long)st);
    CHECK(memcmp(&caps, &before, sizeof(caps)) == 0, "(2) and leaves the capabilities alone");

    struct morse_cmd_resp_get_capabilities body;
    memset(&body, 0, sizeof(body));
    body.capabilities.ampdu_mss = 7;
    answer_(0, 0, (const uint8_t *)&body + sizeof(struct morse_cmd_resp),
            (uint16_t)(sizeof(body) - sizeof(struct morse_cmd_resp)));
    CHECK(mmdrv_get_capabilities_status(0, &caps, &st) == 0 && st == 0 && caps.ampdu_mss == 7,
          "(2) accepted ones are read (ampdu_mss %u)", (unsigned)caps.ampdu_mss);
}

static void t_bss_mesh_status(void)
{
    static const uint8_t bssid[6] = { 0x02, 1, 2, 3, 4, 0x5a };
    int32_t st;

    answer_(0, -13, NULL, 0);
    CHECK(mmdrv_set_bssid(1, bssid, &st) == 0 && st == -13 && s_req_id == MORSE_CMD_ID_BSSID_SET,
          "(3) BSSID_SET: the refusal's status is read (%ld)", (long)st);
    answer_(-110, 0, NULL, 0);
    CHECK(mmdrv_set_bssid(1, bssid, &st) == -110 && st == 0, "(3) BSSID_SET: a transport error has no status");

    answer_(0, -7, NULL, 0);
    CHECK(mmdrv_cfg_bss_beacon(1, true, &st) == 0 && st == -7 && s_req_id == MORSE_CMD_ID_BSS_BEACON_CONFIG,
          "(3) BSS_BEACON_CONFIG: the refusal's status is read (%ld)", (long)st);
    answer_(0, 0, NULL, 0);
    CHECK(mmdrv_cfg_bss_beacon(1, true, &st) == 0 && st == 0, "(3) BSS_BEACON_CONFIG: accepted is 0");
    answer_(-110, 0, NULL, 0);
    CHECK(mmdrv_cfg_bss_beacon(1, true, &st) == -110 && st == 0,
          "(3) BSS_BEACON_CONFIG: a transport error has no status");

    answer_(0, -22, NULL, 0);
    CHECK(mmdrv_mesh_config(1, true, true, &st) == 0 && st == -22 && s_req_id == MORSE_CMD_ID_MESH_CONFIG,
          "(3) MESH_CONFIG: the refusal's status is read (%ld)", (long)st);
    answer_(-110, 0, NULL, 0);
    CHECK(mmdrv_mesh_config(1, true, true, &st) == -110 && st == 0, "(3) MESH_CONFIG: a transport error has no status");

    const struct morse_cmd_req_mesh_config *mc = (const void *)s_req;
    answer_(0, 0, NULL, 0);
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
    answer_(0, -1, NULL, 0);
    CHECK(mmdrv_update_sta_state_status(1, 2, peer, MORSE_STA_NONE, &st) == 0 && st == -1 &&
              s_req_id == MORSE_CMD_ID_SET_STA_STATE,
          "(4) SET_STA_STATE: the refusal's status is read (%ld)", (long)st);
    answer_(0, 0, NULL, 0);
    CHECK(mmdrv_update_sta_state_status(1, 2, peer, MORSE_STA_AUTHORIZED, &st) == 0 && st == 0,
          "(4) SET_STA_STATE: accepted is 0");
    answer_(0, -1, NULL, 0);
    CHECK(mmdrv_update_sta_state(1, 2, peer, MORSE_STA_NONE) == 0,
          "(4) without a status out a refusal still returns 0, as upstream's callers expect");
}

/* Install a group key asking for index @p want; true if it asserted. */
static bool install_(int (*fn)(uint16_t, uint16_t, struct mmdrv_key_conf *, int32_t *),
                     uint8_t want, struct mmdrv_key_conf *kc, int *ret, int32_t *st)
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
    int ret = 0;
    int32_t st = 0;
    const uint8_t idx1 = 1, idx0 = 0, idx2 = 2;

    g_warthog_keyinst_n = 0;
    answer_(0, 0, &idx1, 1);
    CHECK(!install_(mmdrv_install_key_status, 1, &kc, &ret, &st) && ret == 0 && st == 0 &&
              kc.key_idx == 1 && g_warthog_keyinst_n == 1 && s_req_id == MORSE_CMD_ID_INSTALL_KEY,
          "(5) an accepted key is recorded at the chip's hw index (%u, keyinst %lu)",
          (unsigned)kc.key_idx, (unsigned long)g_warthog_keyinst_n);

    g_warthog_keyinst_n = 0;
    answer_(0, -95, NULL, 0);
    CHECK(!install_(mmdrv_install_key_status, 1, &kc, &ret, &st),
          "(5) a status-only refusal does not assert");
    CHECK(ret == 0 && st == -95 && kc.key_idx == 1 && g_warthog_keyinst_n == 0,
          "(5) it is answered with its status and leaves the key and AT+KEYINST? alone (%ld, %u, %lu)",
          (long)st, (unsigned)kc.key_idx, (unsigned long)g_warthog_keyinst_n);

    answer_(0, -95, &idx0, 1);
    CHECK(!install_(mmdrv_install_key_status, 1, &kc, &ret, &st) && st == -95 && kc.key_idx == 1,
          "(5) a refusal echoing another index does not assert and leaves the key alone");
    answer_(0, -95, &idx1, 1);
    CHECK(!install_(mmdrv_install_key_status, 1, &kc, &ret, &st) && st == -95 && g_warthog_keyinst_n == 0,
          "(5) nor is one echoing the index asked for taken as installed");

    g_warthog_chipcmd_key_refused = 0;
    g_warthog_chipcmd_key_status = 0;
    answer_(0, -95, NULL, 0);
    CHECK(!install_(NULL, 1, &kc, &ret, &st) && ret == -95 && kc.key_idx == 1,
          "(5) mmdrv_install_key returns a refusal as an error (%d)", ret);
    CHECK(g_warthog_chipcmd_key_refused == 1 && g_warthog_chipcmd_key_status == -95,
          "(5) and counts it with its status (%lu, %ld)", (unsigned long)g_warthog_chipcmd_key_refused,
          (long)g_warthog_chipcmd_key_status);

    g_warthog_chipcmd_keyidx_mismatch = 0;
    answer_(0, 0, &idx2, 1);
    const bool asserted = install_(mmdrv_install_key_status, 1, &kc, &ret, &st);
#if WARTHOG_MESH_CHIP_VIF_MESH
    CHECK(!asserted && ret == 0 && st == 0 && kc.key_idx == 2 && g_warthog_chipcmd_keyidx_mismatch == 1,
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

    g_warthog_bcn_chip_irq = 0;
    s_notify_isr = 0;
    morse_beacon_irq_handle(&d, other);
    CHECK(d.beacon.chip_irqs == 0 && g_warthog_bcn_chip_irq == 0 && s_notify_isr == 0,
          "(6) another VIF's beacon IRQ is not counted");
    morse_beacon_irq_handle(&d, mine | other);
    CHECK(d.beacon.chip_irqs == 1 && g_warthog_bcn_chip_irq == 1 && s_notify_isr == 1,
          "(6) this VIF's is counted, once (%lu)", (unsigned long)d.beacon.chip_irqs);

    /* Kickoff IRQ, host tick, then a TBTT the chip scheduled itself, host tick. */
    s_notify = 0;
    g_warthog_bcn_host_yield = 0;
    s_timer_cb((struct mmosal_timer *)&s_timer_obj);
    CHECK(s_notify == 1, "(6) the tick after the kickoff IRQ alone beacons from the host");
    morse_beacon_irq_handle(&d, mine);
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
    CHECK(s_ps_cmds == 0 && umac_ps_get_mode(NULL) == MMWLAN_PS_DISABLED,
          "(7) power save on, MESH chip VIF: no CONFIG_PS (%u sent)", s_ps_cmds);
#else
    CHECK(s_ps_cmds == 1, "(7) without the flag the chip VIF is never taken as MESH");
#endif

    s_chip_vif_type = MMDRV_INTERFACE_TYPE_STA;
    umac_ps_reset(NULL);
    s_ps_cmds = 0;
    umac_ps_update_mode(NULL);
    CHECK(s_ps_cmds == 1 && s_ps_vif == 0 && s_ps_on && umac_ps_get_mode(NULL) == MMWLAN_PS_ENABLED,
          "(7) a STA chip VIF still gets CONFIG_PS (%u sent)", s_ps_cmds);
}

int main(void)
{
    printf("=== chip VIF glue: driver.c status, beacon.c, umac_ps.c (WARTHOG_MESH_CHIP_VIF_MESH=%d) ===\n",
           (int)WARTHOG_MESH_CHIP_VIF_MESH);
    t_add_if();
    t_get_caps();
    t_bss_mesh_status();
    t_sta_state();
    t_install_key();
    t_beacon();
    t_ps();
    CHECK(s_asserts == (WARTHOG_MESH_CHIP_VIF_MESH ? 0u : 1u), "no assert fired but the one expected (%u)",
          s_asserts);
    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_chipvif_glue%s: all passed\n", WARTHOG_MESH_CHIP_VIF_MESH ? "" : "_off");
    return 0;
}
