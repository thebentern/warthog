/*
 * The radio's configuration, which a real board is given at boot.
 *
 * Only what the mesh path actually reads. These are hand-written rather than
 * generated stubs because a NULL channel list makes umac_mesh_enable_mesh
 * assert: the firmware is entitled to assume it was configured, and a
 * simulated node has to be configured too. The values are a US 902-928 MHz
 * 2 MHz channel, the domain the warthog-us build ships.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "mmwlan.h"
#include "umac/config/umac_config.h"
#include "umac/interface/umac_interface.h"
#include "umac/connection/umac_connection.h"
#include "umac/data/umac_data.h"
#include "mmdrv.h"

#define SIMNODE_CHAN_COUNT 3

static const struct mmwlan_s1g_channel s_channels[SIMNODE_CHAN_COUNT] = {
    { .centre_freq_hz = 903000000u, .duty_cycle_sta = 10000, .duty_cycle_omit_ctrl_resp = false,
      .global_operating_class = 68, .s1g_operating_class = 1, .s1g_chan_num = 5,
      .bw_mhz = 2, .max_tx_eirp_dbm = 21 },
    { .centre_freq_hz = 907000000u, .duty_cycle_sta = 10000, .duty_cycle_omit_ctrl_resp = false,
      .global_operating_class = 68, .s1g_operating_class = 1, .s1g_chan_num = 13,
      .bw_mhz = 2, .max_tx_eirp_dbm = 21 },
    { .centre_freq_hz = 911000000u, .duty_cycle_sta = 10000, .duty_cycle_omit_ctrl_resp = false,
      .global_operating_class = 68, .s1g_operating_class = 1, .s1g_chan_num = 21,
      .bw_mhz = 2, .max_tx_eirp_dbm = 21 },
};

static const struct mmwlan_s1g_channel_list s_list = {
    .country_code = { 'U', 'S', '\0' },
    .num_channels = SIMNODE_CHAN_COUNT,
    .channels = s_channels,
};

const struct mmwlan_s1g_channel_list *umac_config_get_channel_list(struct umac_data *umacd)
{
    (void)umacd;
    return &s_list;
}

/** The channel a simulated node operates on, for a test that cares. */
const struct mmwlan_s1g_channel *simnode_channel(void) { return &s_channels[0]; }

/* ---- interface identity and capabilities ------------------------------
 *
 * The IE builders dereference these without checking, which is correct on a
 * board: the interface exists before any frame is built. The harness has to
 * stand in for that. Capabilities are zeroed rather than invented -- a
 * zeroed capability set is the conservative one, and the mesh path does not
 * depend on any particular bit. */

static struct morse_caps s_caps;
static uint8_t s_own_mac[6];
static uint16_t s_vif_id;

void simnode_set_identity(const uint8_t mac[6], uint16_t vif_id)
{
    memcpy(s_own_mac, mac, 6);
    s_vif_id = vif_id;
}

const struct morse_caps *umac_interface_get_capabilities(struct umac_data *umacd)
{
    (void)umacd;
    return &s_caps;
}

uint8_t umac_interface_max_supported_bw(struct umac_data *umacd)
{
    (void)umacd;
    return 2; /* the 2 MHz the channel list above advertises */
}

bool umac_interface_get_control_response_bw_1mhz_out_enabled(struct umac_data *umacd)
{
    (void)umacd;
    return false;
}

uint16_t umac_interface_get_vif_id(struct umac_data *umacd, uint16_t type_mask)
{
    (void)umacd; (void)type_mask;
    return s_vif_id;
}

enum mmwlan_status umac_interface_get_mac_addr(struct umac_sta_data *stad, uint8_t *mac_addr)
{
    (void)stad;
    if (mac_addr == NULL) { return MMWLAN_INVALID_ARGUMENT; }
    memcpy(mac_addr, s_own_mac, 6);
    return MMWLAN_SUCCESS;
}

bool umac_interface_addr_matches_mac_addr(struct umac_sta_data *stad, const uint8_t *addr)
{
    (void)stad;
    return addr != NULL && memcmp(addr, s_own_mac, 6) == 0;
}

/* The STA args the capability IE builder asserts on. A mesh node still emits
 * probe requests, and that path runs the STA-mode capability builder, so the
 * harness has to have been "connected" enough to answer this. */
static struct mmwlan_sta_args s_sta_args;

/* ---- the mesh VIF ------------------------------------------------------
 *
 * Adding the interface is chip configuration, not radio-stack filler, so it
 * is written out rather than generated. A generated stub returns success and
 * leaves *vif_id untouched, which left umac_mesh.c's pre-seeded
 * UMAC_INTERFACE_VIF_ID_INVALID (0xffff) in place: every mesh frame then went
 * out tagged with the invalid sentinel while the datapath read 0 from
 * umac_interface_get_vif_id, so data and management frames disagreed about
 * the VIF they were on and no chipcfg assertion could catch a wrong one. */

static bool s_vif_add_fails;

/** Make the next interface add fail, for the "chip refused a mesh VIF"
 *  branch -- the diagnostic umac_mesh.c keeps for a firmware image without
 *  mesh support. */
void simnode_fail_vif_add(bool fail) { s_vif_add_fails = fail; }

enum mmwlan_status umac_interface_add(struct umac_data *umacd, enum umac_interface_type type,
                                      const uint8_t *mac_addr, uint16_t *vif_id)
{
    (void)umacd; (void)type;
    if (s_vif_add_fails) { return MMWLAN_ERROR; }
    if (mac_addr != NULL) { simnode_set_identity(mac_addr, s_vif_id); }
    if (vif_id != NULL) { *vif_id = s_vif_id; }
    return MMWLAN_SUCCESS;
}

const struct mmwlan_sta_args *umac_connection_get_sta_args(struct umac_data *umacd)
{
    (void)umacd;
    return &s_sta_args;
}

enum mmwlan_status mmwlan_get_vif_mac_addr(enum mmwlan_vif vif, uint8_t *mac_addr)
{
    (void)vif;
    if (mac_addr == NULL) { return MMWLAN_INVALID_ARGUMENT; }
    memcpy(mac_addr, s_own_mac, 6);
    return MMWLAN_SUCCESS;
}
