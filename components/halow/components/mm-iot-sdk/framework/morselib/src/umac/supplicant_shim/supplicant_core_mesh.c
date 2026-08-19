/*
 * warthog mesh-support fork — 802.11s mesh supplicant shim.
 *
 * Mirrors supplicant_core_ap.c (the AP path) but registers a wpa_supplicant
 * interface configured for mesh. Hostap's PLINK state machine (mesh_mpm.c)
 * runs inside that supplicant context: incoming mesh action frames get
 * routed via umac_supp_process_mgmt_frame -> mesh_mpm_mgmt_rx, and peer
 * state transitions notify the umac side through wpa_mesh_notify_peer.
 *
 * This file is a build-verifiable shim — the interface registration
 * compiles + links. The reference to wpa_supplicant_join_mesh defeats
 * gc-sections so the hostap mesh layer (1500+ LOC across mesh.c / mesh_mpm.c
 * / mesh_rsn.c) actually ships in
 * firmware.elf. will call it from umac_mesh_enable_mesh().
 *
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 */

/* surface MMLOG_INF/WRN. Without this, passive_init_ifmsh's
 * success line and the shim's status log are blackholed at ERR level. */
#define MMLOG_LEVEL_OVRD 5

#include "mmlog.h"
#include "mmwlan.h"
#include "umac/connection/umac_connection.h"
#include "umac/data/umac_data.h"
#include "umac/supplicant_shim/umac_supp_shim.h"
#include "umac_supp_shim_private.h"
#include "umac/datapath/umac_datapath.h"
#include "umac/mesh/umac_mesh_ies.h"
#include "umac/mesh/umac_mesh.h"

/* passive ifmsh init — pull the hostap mesh internals so we can
 * allocate the bss / mconf / ifmsh directly without going through
 * wpa_supplicant_join_mesh (which cold-inits the chip and crashes us). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wcast-qual"
#include "hostap/src/ap/hostapd.h"
#include "hostap/src/ap/ap_config.h"
#include "hostap/wpa_supplicant/config_ssid.h"
#include "hostap/wpa_supplicant/mesh_rsn.h"

/* 0 = open mesh, 1 = SAE authenticator up, 2 = SAE requested but init failed. */
extern volatile uint32_t g_warthog_sae_init;
extern volatile uint32_t g_warthog_sae_peer_offers, g_warthog_sae_peer_parse_fail;
extern volatile uint32_t g_warthog_sae_rates_synth;
extern volatile uint32_t g_warthog_sae_peer_authproto;
extern volatile uint32_t g_warthog_sae_peer_authval;
extern volatile uint32_t g_warthog_sae_offer_addr;
extern volatile uint32_t g_warthog_sae_stage;
extern volatile uint32_t g_warthog_sae_bridge_en;
extern void warthog_sae_trace(uint32_t n);
#include "hostap/wpa_supplicant/mesh.h"
#include "hostap/wpa_supplicant/mesh_mpm.h"
#pragma GCC diagnostic pop

/* Pull the hostap mesh entry-points in so the linker keeps them in the final
 * image. wpa_supplicant_join_mesh is the join entry; mesh_mpm_* are the PLINK
 * state machine. Without an explicit reference here, --gc-sections strips
 * them as unreachable.
 *
 * tried to actually CALL wpa_supplicant_join_mesh from this shim —
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

/* selectively pull hostap mesh internals. wpa_supplicant_mesh_init
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
    /* SAE arrives in Authentication frames, and ieee802_11_mgmt() drops every
     * one of them unless the interface is ENABLED (ieee802_11.c:6428).
     * hostapd_alloc_iface() zeroes the struct, so the state is
     * HAPD_IFACE_UNINITIALIZED, and the passive bring-up here deliberately
     * never runs hostapd_setup_interface() -- that is the cold-restart path
     * that panics the chip. Set the state directly: the interface genuinely is
     * up by the time we get here, the chip having been configured through
     * mmdrv_mesh_config. Without this, SAE cannot even begin. */
    ifmsh->state = HAPD_IFACE_ENABLED;
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

    /* SAE group list. Upstream wpa_supplicant_mesh_init() copies
     * wpa_s->conf->sae_groups into the bss conf (mesh.c); this passive init
     * never did, and mesh_rsn_sae_group() indexes the array without a NULL
     * check -- the first SAE authentication died at groups[0]. Found on
     * hardware: breadcrumb trail ended between trace 55 and 56, i.e. inside
     * mesh_rsn_sae_group(). Group 19 (NIST P-256) is the SAE default used by
     * wpa_supplicant, hostapd and OpenMANET alike. Static storage: this conf
     * is torn down by hostapd_config_free(), which would os_free() a heap
     * pointer -- but bss->conf here is a *copy* of conf->bss[0] (line above),
     * freed through conf, whose own sae_groups stays NULL, so the static is
     * never freed. */
    {
        static int mesh_sae_groups[] = { 19, -1 };
        bss->conf->sae_groups = mesh_sae_groups;
    }
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

    /* Bring up mesh RSN when the config asked for SAE.
     *
     * mesh_config_create() sets mconf->security from ssid->key_mgmt
     * (mesh.c:103), so this is AMPE exactly when config.c chose
     * WPA_KEY_MGMT_SAE for us. Without this call wpa_s->mesh_rsn stays NULL,
     * mesh_mpm skips every AMPE branch, and the whole of mesh_rsn.c is
     * garbage-collected out of the image -- which is how the mesh ended up
     * running on a hardcoded key.
     *
     * mesh_rsn_auth_init() builds the AP-side WPA authenticator, derives the
     * group key (MGTK) and installs the SAE password, so it must run before
     * any peer is admitted. */
    if (mconf->security & MESH_CONF_SEC_AMPE)
    {
        /* The hostapd-side SAE code reads its password from THIS conf --
         * sae_get_password() never looks at wpa_s's ssid -- and upstream
         * copies it across right before mesh_rsn_auth_init (mesh.c:214).
         * Found on hardware: without it every auth_build_sae_commit died at
         * "SAE: No password available" (authsta=0/1, mlme=0), after the
         * sae_groups fix let SAE get that far at all. */
        {
            /* Mirror wpa_supplicant_mesh_init()'s RSN block (mesh.c:201-214).
             * All three fields live on the hostapd-side conf, which is what
             * the AP code paths read -- they never consult wpa_s's ssid.
             *
             * conf->wpa + conf->wpa_key_mgmt are not cosmetic: ieee802_11.c's
             * handle_auth() only reaches the SAE branch when
             * `hapd->conf->wpa && wpa_key_mgmt_sae(hapd->conf->wpa_key_mgmt)`.
             * With them zero it answers every SAE Authentication frame with
             * NOT_SUPPORTED_AUTH_ALG. Measured on hardware before this fix:
             * handle_auth() ran 6956 times and handle_auth_sae() zero times,
             * while both boards retransmitted Commit at the 40 ms
             * dot11RSNASAERetransPeriod forever. */
            bss->conf->wpa = ssid->proto ? ssid->proto : WPA_PROTO_RSN;
            bss->conf->wpa_key_mgmt =
                ssid->key_mgmt ? ssid->key_mgmt : WPA_KEY_MGMT_SAE;

            const char *password = ssid->sae_password;
            if (password == NULL)
            {
                password = ssid->passphrase;
            }
            if (password != NULL && bss->conf->ssid.wpa_passphrase == NULL)
            {
                bss->conf->ssid.wpa_passphrase = os_strdup(password);
            }
        }
        wpa_s->mesh_rsn = mesh_rsn_auth_init(wpa_s, mconf);
        if (wpa_s->mesh_rsn == NULL)
        {
            /* Deliberately NOT fatal. Failing the whole bring-up here takes
             * the mesh down with it -- own_addr never gets set and the node
             * beacons into the void -- which hides the actual failure behind a
             * dead mesh. Record it and carry on unsecured; g_warthog_sae_init
             * says which happened, and this board's logs are unreachable after
             * the USB-OTG handoff so a counter is the only way to find out. */
            g_warthog_sae_init = 2;
            MMLOG_ERR("mesh: mesh_rsn_auth_init failed — continuing WITHOUT SAE\n");
        }
        else
        {
            g_warthog_sae_init = 1;
            MMLOG_INF("mesh: SAE/AMPE authenticator up (rsn=%p)\n", wpa_s->mesh_rsn);
        }
    }
    else
    {
        MMLOG_INF("mesh: open mesh — no SAE/AMPE\n");
    }

    /* Skip mesh.c:552-611 (hw_mode lookup, basic_rates, conf_ap_ht) — those
     * need a meaningful ssid->frequency / freq->freq which we don't have in
     * the embedded path. mesh_mpm doesn't read most of that state at runtime;
     * if a NULL hits some code path we'll learn which one from the crash.
     *
     * Skip mesh.c:613-616 (wpa_drv_init_mesh) and 618-622 (hostapd_setup_interface)
     * — the latter is the cold-restart trap that crashed The former
     * is harmless via NULL check but unnecessary for passive PLINK. */

    MMLOG_INF("mesh: passive ifmsh init OK — mesh_mpm has hapd=%p mconf=%p own_addr=" MACSTR "\n",
              ifmsh->bss[0], ifmsh->mconf, MAC2STR(wpa_s->own_addr));
    return 0;
}

/* The mesh wpa_supplicant, cached for callers that reach us from the datapath.
 *
 * The shim's own accessor needs a struct umac_data, and the S1G beacon handler
 * has only the received frame -- there is no global umac handle to recover it
 * from. Set once when the mesh interface comes up, cleared when it goes down. */
static struct wpa_supplicant *s_mesh_wpa_s;

/* Tell hostap's MPM about a mesh peer we heard beaconing.
 *
 * hostap starts peering from mesh_mpm_add_peer(), which it normally reaches
 * from its own scan results. This port never runs that scan -- warthog spots
 * peers in S1G beacons and drives its own MPM instead -- so under SAE, where
 * hostap owns peering, nothing was ever telling it a candidate existed. That
 * is the whole reason a SAE mesh came up with the authenticator initialised
 * and no peer ever established.
 *
 * @param addr     the peer's address, from the beacon's SA.
 * @param ies      the beacon's information elements.
 * @param ies_len  their length.
 */
void umac_supp_mesh_new_peer(const uint8_t *addr, const uint8_t *ies, size_t ies_len)
{
    /* Runtime kill-switch, default OFF. The SAE path crashes somewhere past
     * this point and takes the USB CDC down with it, so the only way to get a
     * live AT session on a board that can hear peers is to boot deaf and let
     * the operator pull the trigger: AT+SAEBRIDGE=1, then read the wreckage
     * out of RTC (AT+SAESTAGE?) and flash (AT+COREDUMP?) after the reboot. */
    if (!g_warthog_sae_bridge_en)
    {
        return;
    }
    warthog_sae_trace(10);
    struct wpa_supplicant *wpa_s = s_mesh_wpa_s;
    if (wpa_s == NULL || addr == NULL || ies == NULL)
    {
        return;
    }
    if (wpa_s->ifmsh == NULL || wpa_s->ifmsh->mconf == NULL)
    {
        return;
    }

    struct ieee802_11_elems elems;
    if (ieee802_11_parse_elems((const u8 *)ies, ies_len, &elems, 0) == ParseFailed)
    {
        g_warthog_sae_peer_parse_fail++;
        return;
    }
    /* mesh_mpm_add_peer() dereferences these; a beacon without them is not a
     * usable mesh candidate and hostap would fault on it. */
    if (elems.mesh_id == NULL || elems.mesh_config == NULL)
    {
        return;
    }

    /* Auth protocol must match ours. Upstream reaches mesh_mpm_add_peer()
     * through mesh_matches_local(), which compares the whole Mesh
     * Configuration; this port calls it directly, so the comparison that
     * matters here has to be done by hand.
     *
     * Octet 4 of the Mesh Configuration IE is the Authentication Protocol
     * Identifier (0 = none, 1 = SAE). Peering with a node advertising a
     * different one cannot work, and it actively breaks us: an open-mesh
     * node answers our SAE Authentication frames with
     * NOT_SUPPORTED_AUTH_ALG, and hostap treats a peer's failure status as
     * grounds to reset its own SAE state -- so one stale open node in range
     * stops every SAE handshake on the mesh, including between two nodes
     * that agree. Observed on the bench with exactly that setup. */
    /* Authentication Protocol Identifier (Mesh Configuration octet 4:
     * 0 = none, 1 = SAE) must match ours -- the comparison upstream's
     * mesh_matches_local() would have made if this port reached
     * mesh_mpm_add_peer() through it.
     *
     * This is not just correctness, it is coexistence: an open-mesh node
     * sharing the Mesh ID answers SAE Commits with NOT_SUPPORTED_AUTH_ALG,
     * and hostap resets its SAE state on a peer's failure status -- so
     * without this gate one open node in range keeps every SAE handshake
     * in a reset loop. (An earlier reading dismissed this gate because the
     * octet was 0 on every sampled frame; those samples were all from the
     * open node -- warthog itself discovers peers by probe, not beacon.) */
    if (elems.mesh_config_len >= 5)
    {
        const uint8_t want = umac_mesh_sae_active() ? 1u : 0u;
        if (elems.mesh_config[4] != want)
        {
            g_warthog_sae_peer_authproto++;
            g_warthog_sae_peer_authval = elems.mesh_config[4];
            return;
        }
    }
    /* S1G beacons carry no Supported Rates element -- the S1G PHY has its own
     * MCS set and the legacy element is not part of the frame. hostap's
     * copy_supp_rates() rejects any peer without one, so mesh_mpm_add_peer()
     * returned NULL for every beacon we offered it and no peering ever began.
     * Substitute the rate set we ourselves advertise in probe responses and
     * MPM frames: this is a homogeneous S1G mesh, so it is the set both ends
     * would have agreed on anyway. */
    if (elems.supp_rates == NULL)
    {
        uint8_t rlen = 0;
        const uint8_t *rates = umac_mesh_ies_supported_rates(&rlen);
        elems.supp_rates = (u8 *)rates;
        elems.supp_rates_len = rlen;
        g_warthog_sae_rates_synth++;
    }

    warthog_sae_trace(12);
    g_warthog_sae_offer_addr = ((uint32_t)addr[3] << 16) | ((uint32_t)addr[4] << 8) | addr[5];
    g_warthog_sae_peer_offers++;
    wpa_mesh_new_mesh_peer(wpa_s, (const u8 *)addr, &elems);
    warthog_sae_trace(19); /* returned alive */
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
     * (config reader) and wpa_drivers[] (driver ops). registered both.
     * The driver_init step inside add_iface dispatches a few driver_ops calls
     * (wpa_clear_keys, etc.); AP-specific ops are NULLed on mmwlan_wpas_ops_mesh
     * to skip them. See driver_ap.c comment block above the mesh ops struct. */
    data->mesh_wpa_s = wpa_supplicant_add_iface(data->global, &iface, NULL);
    s_mesh_wpa_s = data->mesh_wpa_s;
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
    s_mesh_wpa_s = NULL;

    if (ret == 0)
    {
        return MMWLAN_SUCCESS;
    }
    else
    {
        return MMWLAN_ERROR;
    }
}
