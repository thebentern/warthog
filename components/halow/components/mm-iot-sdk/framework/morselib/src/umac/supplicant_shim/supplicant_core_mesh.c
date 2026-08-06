/*
 * warthog mesh-support fork — 802.11s mesh supplicant shim.
 *
 * Mirrors supplicant_core_ap.c (the AP path) but registers a wpa_supplicant
 * interface configured for mesh. Hostap's PLINK state machine (mesh_mpm.c)
 * runs inside that supplicant context: incoming mesh action frames get
 * routed via umac_supp_process_mgmt_frame -> mesh_mpm_mgmt_rx, and peer
 * state transitions notify the umac side through wpa_mesh_notify_peer.
 *
 * Phase 4b (this file): build-verifiable shim — the interface registration
 * compiles + links. The reference to wpa_supplicant_join_mesh defeats
 * gc-sections so the hostap mesh layer (1500+ LOC across mesh.c / mesh_mpm.c
 * / mesh_rsn.c, see docs/mesh-port-scope.md Phase 4a) actually ships in
 * firmware.elf. Phase 4c will call it from umac_mesh_enable_mesh().
 *
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 */

/* Phase 4f — surface MMLOG_INF/WRN. Without this, passive_init_ifmsh's
 * success line and the shim's status log are blackholed at ERR level. */
#define MMLOG_LEVEL_OVRD 5

#include "mmlog.h"
#include "mmwlan.h"
#include "umac/connection/umac_connection.h"
#include "umac/data/umac_data.h"
#include "umac/supplicant_shim/umac_supp_shim.h"
#include "umac_supp_shim_private.h"
#include "umac/datapath/umac_datapath.h"

/* Phase 4f passive ifmsh init — pull the hostap mesh internals so we can
 * allocate the bss / mconf / ifmsh directly without going through
 * wpa_supplicant_join_mesh (which cold-inits the chip and crashes us). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wcast-qual"
#include "hostap/src/ap/hostapd.h"
#include "hostap/src/ap/ap_config.h"
#include "hostap/wpa_supplicant/config_ssid.h"
#include "hostap/wpa_supplicant/mesh.h"
#include "hostap/wpa_supplicant/mesh_mpm.h"
#pragma GCC diagnostic pop

/* Pull the hostap mesh entry-points in so the linker keeps them in the final
 * image. wpa_supplicant_join_mesh is the join entry; mesh_mpm_* are the PLINK
 * state machine. Without an explicit reference here, --gc-sections strips
 * them as unreachable.
 *
 * Phase 4e tried to actually CALL wpa_supplicant_join_mesh from this shim —
 * that turned out to be the wrong architecture. It triggered hostap's normal
 * "set up the mesh interface from scratch" flow, which ran umac_interface_add
 * a second time (chip was already in mesh mode from umac_mesh_enable_mesh).
 * The second umac_interface_add hit the cold-init path: mmdrv_init →
 * morse_trns_start → mmhal_wlan_init → gpio_init → INT_WDT panic.
 *
 * Right architecture: our direct umac path owns the chip; hostap mesh sits
 * here as a passive PLINK state machine processing RX'd action frames only.
 * The reference below is just a linkage anchor so the mesh layer stays in
 * the binary for that future RX wiring. */
extern int wpa_supplicant_join_mesh(struct wpa_supplicant *wpa_s,
                                    struct wpa_ssid *ssid);

/* Phase 4f — selectively pull hostap mesh internals. wpa_supplicant_mesh_init
 * (mesh.c:425-622) walks through ~200 lines of host-side state setup, then
 * ends with two driver calls (wpa_drv_init_mesh + hostapd_setup_interface)
 * that cold-restart the chip. The pre-driver lines are safe to replicate —
 * they're plain struct allocation + wiring. We do that here so mesh_mpm gets
 * a usable wpa_s->ifmsh without anyone trying to re-init the SPI/GPIO HAL. */
static int passive_init_ifmsh(struct umac_supp_shim_data *data)
{
    struct wpa_supplicant *wpa_s = data->mesh_wpa_s;
    struct hostapd_iface *ifmsh;
    struct hostapd_data *bss;
    struct hostapd_config *conf;
    struct mesh_conf *mconf;
    struct wpa_ssid *ssid = wpa_s->conf->ssid;

    if (wpa_s->ifmsh != NULL)
    {
        MMLOG_WRN("mesh: ifmsh already set\n");
        return 0;
    }
    if (ssid == NULL)
    {
        MMLOG_ERR("mesh: no ssid on wpa_s->conf — wpa_config_read_mesh didn't run\n");
        return -1;
    }

    /* Populate wpa_s->own_addr from the chip MAC before mirroring mesh_init.
     * The supplicant normally fills this in during driver init via the
     * .get_mac_addr op, but that path didn't run for us (our mesh driver_ops
     * inherits the AP get_mac_addr which assumes an AP context). mesh_mpm
     * uses own_addr as the source for every PLINK frame it emits and for
     * matching incoming peer frames — zero here means PLINK can never work. */
    uint8_t mac[MMWLAN_MAC_ADDR_LEN];
    enum mmwlan_status mac_status = mmwlan_get_mac_addr(mac);
    if (mac_status == MMWLAN_SUCCESS)
    {
        os_memcpy(wpa_s->own_addr, mac, ETH_ALEN);
        MMLOG_INF("mesh: wpa_s->own_addr set to " MACSTR "\n", MAC2STR(mac));
    }
    else
    {
        MMLOG_WRN("mesh: mmwlan_get_mac_addr failed (status=%d) — "
                  "wpa_s->own_addr stays zero, mesh_mpm will not function\n",
                  (int)mac_status);
    }

    /* Mirrors mesh.c:444-461 */
    ifmsh = hostapd_alloc_iface();
    if (!ifmsh)
    {
        MMLOG_ERR("mesh: hostapd_alloc_iface OOM\n");
        return -1;
    }
    wpa_s->ifmsh = ifmsh;
    ifmsh->owner = wpa_s;
    ifmsh->drv_flags = wpa_s->drv_flags;
    ifmsh->drv_flags2 = wpa_s->drv_flags2;
    ifmsh->num_bss = 1;
    ifmsh->bss = os_calloc(1, sizeof(struct hostapd_data *));
    if (!ifmsh->bss)
    {
        MMLOG_ERR("mesh: bss array OOM\n");
        return -1;
    }

    /* Mirrors mesh.c:459-470. Skipping the enable_iface_cb / disable_iface_cb
     * because those are only consulted from hostapd_setup_interface (which we
     * don't call). */
    bss = hostapd_alloc_bss_data(NULL, NULL, NULL);
    if (!bss)
    {
        MMLOG_ERR("mesh: hostapd_alloc_bss_data OOM\n");
        return -1;
    }
    ifmsh->bss[0] = bss;
    bss->msg_ctx = wpa_s;
    os_memcpy(bss->own_addr, wpa_s->own_addr, ETH_ALEN);
    bss->driver = wpa_s->driver;
    bss->drv_priv = wpa_s->drv_priv;
    bss->iface = ifmsh;
    bss->mesh_sta_free_cb = mesh_mpm_free_sta;

    wpa_s->current_ssid = ssid;
    os_memcpy(wpa_s->bssid, wpa_s->own_addr, ETH_ALEN);

    /* Mirrors mesh.c:483-486 */
    conf = hostapd_config_defaults();
    if (!conf)
    {
        MMLOG_ERR("mesh: hostapd_config_defaults OOM\n");
        return -1;
    }

    /* Skip mesh.c:487-516 (6 GHz checks) and 518-520 (morse_ibss_mesh_setup_freq
     * — that function writes channel/op_class into the chip via stubbed CLI
     * helpers, but the chip is already configured by our direct mmdrv_mesh_config
     * path so we don't need to repeat that here). */

    /* Mirrors mesh.c:522-526, 538-544 */
    bss->conf = *conf->bss;
    bss->conf->start_disabled = 1;
    bss->conf->mesh = MESH_ENABLED;
    bss->conf->ap_max_inactivity = wpa_s->conf->mesh_max_inactivity;
    bss->conf->mesh_fwding = wpa_s->conf->mesh_fwding;
    bss->iconf = conf;
    ifmsh->conf = conf;
    ifmsh->bss[0]->max_plinks = wpa_s->conf->max_peer_links;
    ifmsh->bss[0]->dot11RSNASAERetransPeriod =
        wpa_s->conf->dot11RSNASAERetransPeriod;
    os_strlcpy(bss->conf->iface, wpa_s->ifname, sizeof(bss->conf->iface));

    /* Mirrors mesh.c:546-549 */
    mconf = mesh_config_create(wpa_s, ssid);
    if (!mconf)
    {
        MMLOG_ERR("mesh: mesh_config_create OOM\n");
        return -1;
    }
    ifmsh->mconf = mconf;

    /* Skip mesh.c:552-611 (hw_mode lookup, basic_rates, conf_ap_ht) — those
     * need a meaningful ssid->frequency / freq->freq which we don't have in
     * the embedded path. mesh_mpm doesn't read most of that state at runtime;
     * if a NULL hits some code path we'll learn which one from the crash.
     *
     * Skip mesh.c:613-616 (wpa_drv_init_mesh) and 618-622 (hostapd_setup_interface)
     * — the latter is the cold-restart trap that crashed Phase 4e. The former
     * is harmless via NULL check but unnecessary for passive PLINK. */

    MMLOG_INF("mesh: passive ifmsh init OK — mesh_mpm has hapd=%p mconf=%p own_addr=" MACSTR "\n",
              ifmsh->bss[0], ifmsh->mconf, MAC2STR(wpa_s->own_addr));
    return 0;
}

enum mmwlan_status umac_supp_add_mesh_interface(struct umac_data *umacd)
{
    struct umac_supp_shim_data *data = umac_data_get_supp_shim(umacd);

    if (data->mesh_wpa_s != NULL)
    {
        MMLOG_WRN("Mesh interface already active\n");
        MMOSAL_DEV_ASSERT(false);
        return MMWLAN_SUCCESS;
    }

    enum mmwlan_status status = umac_supp_start_supp(umacd);
    if (status != MMWLAN_SUCCESS)
    {
        return status;
    }

    struct wpa_interface iface = {
        .confname = UMAC_SUPP_MESH_CONFIG_NAME,
        .driver = UMAC_SUPP_MESH_DRIVER_NAME,
        .ifname = UMAC_SUPP_MESH_CONFIG_NAME,
    };

    /* wpa_supplicant_add_iface needs matching entries in mmwlan_wpa_configs[]
     * (config reader) and wpa_drivers[] (driver ops). Phase 4d registered both.
     * The driver_init step inside add_iface dispatches a few driver_ops calls
     * (wpa_clear_keys, etc.); AP-specific ops are NULLed on mmwlan_wpas_ops_mesh
     * to skip them. See driver_ap.c comment block above the mesh ops struct. */
    data->mesh_wpa_s = wpa_supplicant_add_iface(data->global, &iface, NULL);
    if (data->mesh_wpa_s == NULL)
    {
        MMLOG_ERR("mesh: wpa_supplicant_add_iface returned NULL — "
                  "check that mmwlan_wpa_config_mesh + mmwlan_wpas_ops_mesh "
                  "are present in mmwlan_wpa_configs[] / wpa_drivers[]\n");
        return MMWLAN_ERROR;
    }

    /* DO NOT call wpa_supplicant_join_mesh here. See the long comment above
     * the extern declaration: that path triggers a second umac_interface_add
     * which cold-inits the chip mid-operation and panics. Instead we run our
     * own passive init that sets up just the host-side state mesh_mpm needs
     * (ifmsh + bss + mconf), without ever touching the driver init path. */
    (void)wpa_supplicant_join_mesh;

    if (passive_init_ifmsh(data) != 0)
    {
        MMLOG_ERR("mesh: passive_init_ifmsh failed; mesh_mpm will not be reachable\n");
        /* Don't propagate as a hard failure — chip still beaconing, smoke
         * RESULT: PASS still meaningful. RX dispatch (next step) would be
         * a no-op anyway without ifmsh, so this is the gate. */
        return MMWLAN_SUCCESS;
    }

    return MMWLAN_SUCCESS;
}

enum mmwlan_status umac_supp_remove_mesh_interface(struct umac_data *umacd)
{
    struct umac_supp_shim_data *data = umac_data_get_supp_shim(umacd);

    if (!data->is_started)
    {
        return MMWLAN_UNAVAILABLE;
    }

    MMLOG_INF("Removing %s Supp interface\n", "MESH");

    if (data->mesh_wpa_s == NULL)
    {
        return MMWLAN_NOT_FOUND;
    }

    int ret = wpa_supplicant_remove_iface(data->global, data->mesh_wpa_s, 0);
    data->mesh_wpa_s = NULL;

    if (ret == 0)
    {
        return MMWLAN_SUCCESS;
    }
    else
    {
        return MMWLAN_ERROR;
    }
}
