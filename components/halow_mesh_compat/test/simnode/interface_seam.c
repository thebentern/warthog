/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * umac_interface.c, the shipping file, compiled here: adding, re-adding and removing the
 * chip VIF is what the mesh's chip interface type depends on, so it runs for real and
 * its ADD_INTERFACE / REMOVE_INTERFACE reach the fake chip (fake_chip.c).
 *
 * The harness keeps its own versions of the accessors below (fake_config.c, simnode.c and
 * the generated stubs): the capability set a test toggles, the RX filter hook, the
 * per-VIF extended RX callbacks and the stubbed channel path. The real ones are renamed
 * out of the way; nothing else changes.
 */
#define umac_interface_get_capabilities simnode_real_umac_interface_get_capabilities
#define umac_interface_max_supported_bw simnode_real_umac_interface_max_supported_bw
#define umac_interface_get_control_response_bw_1mhz_out_enabled \
    simnode_real_umac_interface_get_control_response_bw_1mhz_out_enabled
#define umac_interface_addr_matches_mac_addr simnode_real_umac_interface_addr_matches_mac_addr
#define umac_interface_register_rx_pkt_ext_cb simnode_real_umac_interface_register_rx_pkt_ext_cb
#define umac_interface_get_rx_pkt_ext_cb simnode_real_umac_interface_get_rx_pkt_ext_cb
#define umac_interface_set_channel_from_regdb simnode_real_umac_interface_set_channel_from_regdb
#define umac_interface_get_current_s1g_operation_info \
    simnode_real_umac_interface_get_current_s1g_operation_info
#include "umac/interface/umac_interface.c"
