/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A scan's transmit restrictions (morselib umac/scan/hw_scan.c, umac/regdb/umac_regdb.c), compiled
 * as shipped and driven through the scan state machine with the chip and the rest of umac stubbed.
 *
 * Duty cycle and the packet spacing window (MPSW) reach the chip only on a channel set
 * (umac_interface.c), and a station's first scan, and the mesh image's scan probe, come before
 * any. JP's rows carry a 10% duty cycle and a 2 ms spacing window, KR's a 50 ms one: so before the
 * HW_SCAN START command the scan must program the strictest values of the list it scans (lowest
 * duty, omitting control responses only if every row does, the longest spacing, the lowest
 * airtime_min and the lowest non-zero airtime_max), or not scan at all. A list no row restricts
 * (AU, NZ, US) programs nothing, as upstream. The lists are the ones the board hands the chip:
 * main/region.c's plan over the real regdb, the linked BCF and the module's 902-928 MHz band.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/morse_commands.h"
#include "mmregdb.h"
#include "region.h"
#include "region_bcf.h"
#include "umac/config/umac_config.h"
#include "umac/datapath/umac_datapath.h"
#include "umac/interface/umac_interface.h"
#include "umac/regdb/umac_regdb.h"
#include "umac/scan/umac_scan_data.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* ---- what the scan calls ------------------------------------------------------------------- */

static char log_[512];
static const struct mmwlan_s1g_channel_list *s_list;
static struct umac_scan_data s_scan;
static int s_duty_ret, s_mpsw_ret, s_completed = -1;
static void log_add_(const char *s) { strncat(log_, s, sizeof(log_) - strlen(log_) - 1); }

int mmdrv_set_duty_cycle(uint32_t duty_cycle, bool omit, enum mmwlan_duty_cycle_mode mode)
{
    char b[64];
    snprintf(b, sizeof(b), "duty %u omit %d mode %d; ", (unsigned)duty_cycle, (int)omit, (int)mode);
    log_add_(b);
    return s_duty_ret;
}
int mmdrv_cfg_mpsw(uint32_t airtime_min_us, uint32_t airtime_max_us, uint32_t packet_space_window_length_us)
{
    char b[64];
    snprintf(b, sizeof(b), "mpsw %u/%u/%u; ", (unsigned)airtime_min_us, (unsigned)airtime_max_us,
             (unsigned)packet_space_window_length_us);
    log_add_(b);
    return s_mpsw_ret;
}
int mmdrv_execute_command(uint8_t *command, uint8_t *response, uint32_t *response_len)
{
    (void)response; (void)response_len;
    const struct morse_cmd_req_hw_scan *r = (const struct morse_cmd_req_hw_scan *)command;
    char b[48];
    snprintf(b, sizeof(b), "cmd %u flags %u; ", (unsigned)r->hdr.message_id, (unsigned)r->flags);
    log_add_(b);
    return 0;
}
const struct mmwlan_s1g_channel_list *umac_config_get_channel_list(struct umac_data *umacd) { (void)umacd; return s_list; }
bool umac_config_is_opclass_check_enabled(struct umac_data *umacd) { (void)umacd; return true; }
enum mmwlan_duty_cycle_mode umac_config_get_duty_cycle_mode(struct umac_data *umacd) { (void)umacd; return MMWLAN_DUTY_CYCLE_MODE_SPREAD; }
struct umac_scan_data *umac_data_get_scan(struct umac_data *umacd) { (void)umacd; return &s_scan; }
static struct mmwlan_sta_args s_sta;
const struct mmwlan_sta_args *umac_connection_get_sta_args(struct umac_data *umacd) { (void)umacd; return &s_sta; }
enum mmwlan_status umac_interface_get_vif_mac_addr(struct umac_data *umacd, enum mmwlan_vif vif, uint8_t *mac)
{
    (void)umacd; (void)vif;
    static const uint8_t m[6] = { 2, 0, 0, 0, 0, 1 };
    memcpy(mac, m, 6);
    return MMWLAN_SUCCESS;
}
enum mmwlan_status umac_interface_add(struct umac_data *umacd, enum umac_interface_type type, const uint8_t *mac, uint16_t *vif_id)
{
    (void)umacd; (void)type; (void)mac;
    *vif_id = 1;
    return MMWLAN_SUCCESS;
}
void umac_interface_remove(struct umac_data *umacd, enum umac_interface_type type) { (void)umacd; (void)type; }
void umac_datapath_pause(struct umac_data *umacd, uint16_t source_mask) { (void)umacd; (void)source_mask; }
void umac_datapath_unpause(struct umac_data *umacd, uint16_t source_mask) { (void)umacd; (void)source_mask; }
void umac_stats_increment_num_scans_complete(struct umac_data *umacd) { (void)umacd; }
void frame_probe_request_build(struct umac_data *umacd, struct consbuf *buf, void *args) { (void)umacd; (void)buf; (void)args; }
void *mmosal_malloc_(size_t size) { return malloc(size); }
void mmosal_free(void *p) { free(p); }
uint32_t mmosal_get_time_ms(void) { return 0; }
const char *mmosal_task_name(void) { return "t"; }
int mmosal_printf(const char *f, ...) { (void)f; return 0; }
void mmosal_log_failure_info(const struct mmosal_failure_info *info) { (void)info; }
void mmosal_impl_assert(void) { printf("FAIL a firmware assertion fired\n"); exit(1); }

/* region.c's boot hooks; this test calls warthog_region_plan directly. */
void warthog_cfg_get_region(struct warthog_rg_stored *o) { memset(o, 0, sizeof(*o)); }
uint8_t warthog_cfg_get_mesh_enable(void) { return 0; }
bool warthog_cfg_get_mesh_chan(struct warthog_mesh_chan *o) { (void)o; return false; }
struct warthog_region_ctx warthog_region_ctx_get(void) { struct warthog_region_ctx c = { 0 }; return c; }

/* ---- the scan, as umac_scan.c starts it ----------------------------------------------------------- */

static void complete_(struct umac_data *umacd, enum mmwlan_scan_state s) { (void)umacd; s_completed = (int)s; }

static const char *scan_(const struct mmwlan_s1g_channel_list *list)
{
    static struct umac_scan_req req;
    const struct mmwlan_scan_args a = MMWLAN_SCAN_ARGS_INIT;
    req.args = a;
    req.complete_cb = complete_;
    s_list = list;
    log_[0] = '\0';
    s_completed = -1;
    memset(&s_scan, 0, sizeof(s_scan));
    hw_scan_fsm_init(&s_scan.hw_scan_data.fsm_inst);
    s_scan.hw_scan_data.fsm_inst.arg = &s_scan;
    s_scan.active_scan_req = &req;
    hw_scan_fsm_handle_event(&s_scan.hw_scan_data.fsm_inst, HW_SCAN_FSM_EVENT_REQUEST);
    return log_;
}

/* The plan points into itself (list -> out -> rows), so it is filled in place, never returned by value. */
static void plan_(const char *cc, bool mesh, struct warthog_region_plan *p)
{
    const struct warthog_region_ctx c = { get_regulatory_db(), k_region_bcf, sizeof(k_region_bcf), "AU,JP,KR,NZ,US", 902000000u,
                                          928000000u };
    struct warthog_rg_stored st;
    memset(&st, 0, sizeof(st));
    st.state = WARTHOG_RG_VALID;
    snprintf(st.cc, sizeof(st.cc), "%s", cc);
    warthog_region_plan(&c, &st, mesh, NULL, p);
}

int main(void)
{
    printf("=== a scan programs its list's strictest duty cycle and MPSW first ===\n");
    char want[160];
    snprintf(want, sizeof(want), "cmd %u flags %u; ", (unsigned)MORSE_CMD_ID_HW_SCAN, (unsigned)MORSE_CMD_HW_SCAN_FLAGS_START);
    const char *start = want;

    static const struct { const char *cc; bool mesh; unsigned rows; const char *prog; } k[] = {
        { "JP", false, 12, "duty 1000 omit 1 mode 0; mpsw 2000/100000/2000; " },
        { "JP", true, 1, "duty 1000 omit 1 mode 0; mpsw 2000/100000/2000; " },
        { "KR", false, 13, "duty 10000 omit 0 mode 0; mpsw 0/220000/50000; " },
        { "KR", true, 1, "duty 10000 omit 0 mode 0; mpsw 0/4000000/50000; " },
        { "US", false, 48, "" }, { "US", true, 1, "" }, { "AU", false, 26, "" }, { "NZ", false, 23, "" }, { "NZ", true, 1, "" },
    };
    for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        struct warthog_region_plan p;
        plan_(k[i].cc, k[i].mesh, &p);
        char exp[256];
        snprintf(exp, sizeof(exp), "%s%s", k[i].prog, start);
        const char *got = p.list != NULL ? scan_(p.list) : "(no plan)";
        CHECK(p.list != NULL && p.list->num_channels == k[i].rows && strcmp(got, exp) == 0,
              "%s %s (%u row(s)): %s before HW_SCAN START [%s]", k[i].cc, k[i].mesh ? "mesh" : "station", k[i].rows,
              k[i].prog[0] ? "the strictest duty and MPSW" : "nothing programmed", got);
    }

    /* Every row of every offered list is at least as loose as what the scan programmed. */
    static const char *offered[] = { "AU", "JP", "KR", "NZ", "US" };
    for (unsigned i = 0; i < 5; i++) {
        struct warthog_region_plan p;
        plan_(offered[i], false, &p);
        struct mmwlan_s1g_channel s;
        const bool r = umac_regdb_strictest(p.list, &s);
        bool tight = true;
        for (unsigned j = 0; r && j < p.list->num_channels; j++) {
            const struct mmwlan_s1g_channel *c = &p.list->channels[j];
            tight = tight && s.duty_cycle_sta <= c->duty_cycle_sta && (!s.duty_cycle_omit_ctrl_resp || c->duty_cycle_omit_ctrl_resp) &&
                    s.pkt_spacing_us >= c->pkt_spacing_us && s.airtime_min_us <= c->airtime_min_us &&
                    (c->airtime_max_us == 0 || (s.airtime_max_us != 0 && s.airtime_max_us <= c->airtime_max_us)) &&
                    (s.airtime_max_us == 0 || s.airtime_min_us < s.airtime_max_us);
        }
        CHECK(tight && r == (!strcmp(offered[i], "JP") || !strcmp(offered[i], "KR")),
              "%s: %s", offered[i], r ? "the strictest is no looser than any row, and a valid MPSW" : "no row restricts");
    }
    struct mmwlan_s1g_channel s;
    const struct mmwlan_s1g_channel_list empty = { .country_code = "US", .num_channels = 0, .channels = NULL };
    CHECK(!umac_regdb_strictest(NULL, &s) && !umac_regdb_strictest(&empty, &s), "no list, or no rows: nothing to program");

    /* The chip refusing either: no HW_SCAN at all, the scan reported terminated. */
    struct warthog_region_plan jp;
    plan_("JP", false, &jp);
    s_mpsw_ret = -1;
    const char *got = scan_(jp.list);
    CHECK(strcmp(got, "duty 1000 omit 1 mode 0; mpsw 2000/100000/2000; ") == 0 && s_completed == MMWLAN_SCAN_TERMINATED,
          "MPSW refused: no HW_SCAN, scan terminated [%s]", got);
    s_mpsw_ret = 0; s_duty_ret = -1;
    got = scan_(jp.list);
    CHECK(strcmp(got, "duty 1000 omit 1 mode 0; ") == 0 && s_completed == MMWLAN_SCAN_TERMINATED,
          "duty cycle refused: no MPSW, no HW_SCAN, scan terminated [%s]", got);
    s_duty_ret = 0;

    if (failures) {
        printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_hw_scan: all passed\n");
    return 0;
}
