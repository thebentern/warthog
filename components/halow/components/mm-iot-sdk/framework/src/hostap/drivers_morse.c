/*
 * Copyright 2023-2025 Morse Micro
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdlib.h>
#include "hostap_morse_common.h"

/** WPA Supplicant driver operations descriptor implemented by morselib. */
extern const struct wpa_driver_ops mmwlan_wpas_ops;

/** WPA Supplicant driver operations descriptor implemented by morselib (AP mode). */
extern const struct wpa_driver_ops mmwlan_wpas_ops_ap;

#ifdef CONFIG_MESH
/** Warthog fork — driver_ops for 802.11s mesh (Phase 4d). */
extern const struct wpa_driver_ops mmwlan_wpas_ops_mesh;
#endif

const struct wpa_driver_ops *const wpa_drivers[] = {
    &mmwlan_wpas_ops,
#ifdef CONFIG_AP
    &mmwlan_wpas_ops_ap,
#endif
#ifdef CONFIG_MESH
    &mmwlan_wpas_ops_mesh,
#endif
    NULL,
};

/** Morse WPA Supplicant configuration descriptor for STA mode. */
extern const struct mmwlan_supp_config_entry mmwlan_wpa_config_sta;
/** Morse WPA Supplicant configuration descriptor for DPP mode. */
extern const struct mmwlan_supp_config_entry mmwlan_wpa_config_dpp;
/** Morse WPA Supplicant configuration descriptor for AP mode. */
extern const struct mmwlan_supp_config_entry mmwlan_wpa_config_ap;
#ifdef CONFIG_MESH
/** Warthog fork — config descriptor for 802.11s mesh (Phase 4d). */
extern const struct mmwlan_supp_config_entry mmwlan_wpa_config_mesh;
#endif

/** Array of supported configuration descriptors. */
const struct mmwlan_supp_config_entry *const mmwlan_wpa_configs[] = {
    &mmwlan_wpa_config_sta,
#ifdef CONFIG_DPP
    &mmwlan_wpa_config_dpp,
#endif
#ifdef CONFIG_AP
    &mmwlan_wpa_config_ap,
#endif
#ifdef CONFIG_MESH
    &mmwlan_wpa_config_mesh,
#endif
    NULL,
};
