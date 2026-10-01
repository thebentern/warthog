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
#include "umac/core/umac_core.h"
#include "umac/data/umac_data.h"
#include "umac/datapath/umac_datapath.h"
#include "umac/twt/umac_twt.h"
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
 * depend on any particular bit. simnode_set_ampdu sets the one Block Ack needs. */

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

/* The RX filter asks this with the record it just looked up, then writes that record's
 * duplicate cache: the hook runs the event loop in between. */
static void (*s_rx_filter_hook)(void);
void simnode_set_rx_filter_hook(void (*cb)(void)) { s_rx_filter_hook = cb; }

bool umac_interface_addr_matches_mac_addr(struct umac_sta_data *stad, const uint8_t *addr)
{
    (void)stad;
    void (*cb)(void) = s_rx_filter_hook;
    if (cb != NULL && !umac_core_evtloop_is_active(NULL))
    {
        s_rx_filter_hook = NULL;
        cb();
    }
    return addr != NULL && memcmp(addr, s_own_mac, 6) == 0;
}

/* The STA args the capability IE builder asserts on. A mesh node still emits
 * probe requests, and that path runs the STA-mode capability builder, so the
 * harness has to have been "connected" enough to answer this. */
static struct mmwlan_sta_args s_sta_args;

/* ---- the chip VIF ------------------------------------------------------
 *
 * Adding, re-adding and removing the interface is the real umac_interface.c
 * (interface_seam.c); the chip it talks to (fake_chip.c) reports this node's MAC at
 * mmdrv_init and hands out this VIF id at ADD_INTERFACE. */

const uint8_t *simnode_own_mac(void) { return s_own_mac; }
uint16_t simnode_own_vif_id(void) { return s_vif_id; }

/* ---- A-MPDU ------------------------------------------------------------
 *
 * The board runs with it (umac_config.c defaults ampdu_enabled on, the MM6108 reports
 * the capability), so every unicast data frame can start a Block Ack session. Off here
 * unless a test turns it on: the suites written before umac_ba.c was linked count frames. */
static bool s_ampdu;

void simnode_set_ampdu(bool on)
{
    s_ampdu = on;
    const unsigned bit = (unsigned)MORSE_CAPS_AMPDU;
    if (on) { s_caps.flags[bit >> 5] |= 1u << (bit & 31u); }
    else    { s_caps.flags[bit >> 5] &= ~(1u << (bit & 31u)); }
}

bool umac_config_is_ampdu_enabled(struct umac_data *umacd)
{
    (void)umacd;
    return s_ampdu;
}

uint32_t umac_config_get_datapath_rx_reorder_list_maxlen(struct umac_data *umacd)
{
    (void)umacd;
    return UMAC_DATAPATH_DEFAULT_RXREORDERQ_MAXLEN;
}

/* TWT off: a blocking management TX waits the default timeout (umac_config.c). */
const struct mmwlan_twt_config_args *umac_twt_get_config(struct umac_data *umacd)
{
    (void)umacd;
    static const struct mmwlan_twt_config_args none;
    return &none;
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
