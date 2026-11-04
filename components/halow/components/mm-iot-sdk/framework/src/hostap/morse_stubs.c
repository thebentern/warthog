#include "utils/morse.h"
#include "mmosal.h"

/* Warthog builds src/utils/morse.c, which defines morse_{sta,ap}_configure_channelization:
 * upstream's stubs of those two would be second definitions. */

/* ---------------------------------------------------------------------------
 * wpa_supplicant.c mesh paths and mesh.c call these CLI
 * helpers expecting a Linux userspace `morse_cli` tool over nl80211 vendor
 * commands. On embedded, real config goes through the morselib host driver
 * (mmdrv_mesh_config sends MESH_CONFIG opcode 0x0039 directly to the chip).
 * These stubs just satisfy the link.
 * --------------------------------------------------------------------------- */

int morse_set_channel(const char *ifname, int oper_freq, int oper_chwidth,
                      u8 prim_chwidth, u8 prim_1mhz_ch_idx)
{
    (void)ifname; (void)oper_freq; (void)oper_chwidth;
    (void)prim_chwidth; (void)prim_1mhz_ch_idx;
    /* No-op: chip is already on the right channel via mmwlan_set_channel_list. */
    return 0;
}

int morse_set_s1g_op_class(const char *ifname, u8 opclass, u8 prim_opclass)
{
    (void)ifname; (void)opclass; (void)prim_opclass;
    return 0;
}

int morse_set_mesh_config(const char *ifname, u8 *mesh_id, u8 mesh_id_len,
                          u8 beaconless_mode, u8 max_plinks)
{
    (void)ifname; (void)mesh_id; (void)mesh_id_len;
    (void)beaconless_mode; (void)max_plinks;
    /* Real mesh config goes via mmdrv_mesh_config(MESH_CONFIG, START, ...). */
    return 0;
}

int morse_mbca_conf(const char *ifname, u8 mbca_config, u8 min_beacon_gap,
                    u8 tbtt_adj_interval, u8 beacon_timing_report_interval,
                    u16 mbss_start_scan_duration)
{
    (void)ifname; (void)mbca_config; (void)min_beacon_gap;
    (void)tbtt_adj_interval; (void)beacon_timing_report_interval;
    (void)mbss_start_scan_duration;
    /* MBCA is an optional 802.11s mesh feature; leave disabled for initial
     * PLINK bring-up. Enable later if mesh density warrants. */
    return 0;
}

int morse_set_mesh_dynamic_peering(const char *ifname, bool enabled,
                                   u8 rssi_margin, u32 blacklist_timeout)
{
    (void)ifname; (void)enabled; (void)rssi_margin; (void)blacklist_timeout;
    return 0;
}
