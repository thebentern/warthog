/*
 * Copyright 2023-2025 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mmhalow.h"

/* Regulatory domain.
 *
 * region.h maps the build's WARTHOG_REGION_<XX> flag to the country code the
 * Morse regulatory database is keyed on ("US", "EU", "JP", "KR", "AU" -- all
 * present in mmregdb.c). Before this, the channel list came from
 * CONFIG_HALOW_COUNTRY_CODE, hardcoded to "US" in sdkconfig.defaults, while
 * WARTHOG_COUNTRY_CODE reached nothing but two log lines. Every non-US build
 * therefore booted the US channel list and said otherwise in its own log --
 * an EU image transmitting on 902-928 MHz. Keep this deriving from the build
 * region, not from Kconfig. */
#include "region.h"
#ifndef WARTHOG_COUNTRY_CODE
#define WARTHOG_COUNTRY_CODE CONFIG_HALOW_COUNTRY_CODE
#endif
#include "esp_log.h"
#include "mmlog.h"
#include <string.h>

int g_warthog_chan_pin_status = -1;

static const char *TAG = "Morse Micro HaLow NetIF";

static esp_netif_t *halow_netif = NULL;

esp_netif_t *mmhalow_get_netif(void)
{
    return halow_netif;
}

static void mmhalow_link_state(enum mmwlan_link_state link_state, void *arg)
{
    esp_netif_t *esp_netif = (esp_netif_t *)arg;

    if (link_state == MMWLAN_LINK_DOWN)
    {
        ESP_LOGD(TAG, "Link down");
        esp_netif_action_disconnected(esp_netif, NULL, 0, NULL);
    }
    else
    {
        ESP_LOGD(TAG, "Link up");
        esp_netif_action_connected(esp_netif, NULL, 0, NULL);
    }
}

static void mmhalow_free(void *h, void *buffer)
{
    struct mmpktview *pktview = (struct mmpktview *)buffer;
    struct mmpkt *rxpkt = mmpkt_from_view(pktview);
    mmpkt_close(&pktview);
    mmpkt_release(rxpkt);
}

static void halow_rx(struct mmpkt *rxpkt, void *arg)
{
    esp_netif_t *esp_netif = (esp_netif_t *)arg;
    assert(esp_netif);

    struct mmpktview *pktview = mmpkt_open(rxpkt);
    uint32_t data_len = mmpkt_get_data_length(pktview);
    esp_err_t ret =
        esp_netif_receive(halow_netif, mmpkt_get_data_start(pktview), data_len, pktview);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_netif_receive failed - %d", ret);
        mmpkt_close(&pktview);
        mmpkt_release(rxpkt);
    }
}

static esp_err_t halow_transmit(void *h, void *buffer, size_t len)
{
    struct mmpkt *pkt;
    struct mmpktview *pktview;
    enum mmwlan_status status;
    struct mmwlan_tx_metadata metadata = {
        .tid = 0,
    };

    status = mmwlan_tx_wait_until_ready(1000);
    if (status != MMWLAN_SUCCESS)
    {
        ESP_LOGE(TAG, "Transmit blocked: %d", status);
        return ESP_FAIL;
    }

    pkt = mmwlan_alloc_mmpkt_for_tx(len, metadata.tid);
    if (pkt == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate packet for transmit.");
        return ESP_ERR_NO_MEM;
    }
    pktview = mmpkt_open(pkt);
    mmpkt_append_data(pktview, (const uint8_t *)buffer, len);
    mmpkt_close(&pktview);

    status = mmwlan_tx_pkt(pkt, &metadata);
    if (status != MMWLAN_SUCCESS)
    {
        ESP_LOGE(TAG, "Packet failed to send - %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t halow_transmit_wrap(void *h, void *buffer, size_t len, void *netstack_buf)
{
    return halow_transmit(h, buffer, len);
}

static esp_err_t halow_driver_start(esp_netif_t *esp_netif, void *args)
{
    mmhalow_netif_driver_t *driver = (mmhalow_netif_driver_t *)args;
    driver->base.netif = esp_netif;
    esp_netif_driver_ifconfig_t driver_ifconfig = { .handle = driver,
                                                    .transmit = halow_transmit,
                                                    .transmit_wrap = halow_transmit_wrap,
                                                    .driver_free_rx_buffer = mmhalow_free };
    halow_netif = esp_netif;
    return esp_netif_set_driver_config(esp_netif, &driver_ifconfig);
}

static mmhalow_netif_driver_t *mmhalow_create_if_driver(void)
{
    mmhalow_netif_driver_t *driver = calloc(1, sizeof(mmhalow_netif_driver_t));
    if (driver == NULL)
    {
        ESP_LOGE(TAG, "No memory to create a Wi-Fi interface handle");
        return NULL;
    }
    driver->base.post_attach = halow_driver_start;

    /* Initialise the driver's sta_args structure */
    const struct mmwlan_sta_args sta_args = MMWLAN_STA_ARGS_INIT;
    memcpy(&driver->sta_args, &sta_args, sizeof(struct mmwlan_sta_args));

    return driver;
}

void mmhalow_print_version_info(void)
{
    enum mmwlan_status status;
    struct mmwlan_version version;
    struct mmwlan_bcf_metadata bcf_metadata;

    ESP_LOGI(TAG, "-----------------------------------");

    status = mmwlan_get_bcf_metadata(&bcf_metadata);
    if (status == MMWLAN_SUCCESS)
    {
        ESP_LOGI(TAG,
                 "  BCF API version:         %u.%u.%u",
                 bcf_metadata.version.major,
                 bcf_metadata.version.minor,
                 bcf_metadata.version.patch);
        if (bcf_metadata.build_version[0] != '\0')
        {
            ESP_LOGI(TAG, "  BCF build version:       %s", bcf_metadata.build_version);
        }
        if (bcf_metadata.board_desc[0] != '\0')
        {
            ESP_LOGI(TAG, "  BCF board description:   %s", bcf_metadata.board_desc);
        }
    }
    else
    {
        ESP_LOGE(TAG, "Error occured whilst retrieving BCF metadata - %d", status);
    }

    status = mmwlan_get_version(&version);
    if (status != MMWLAN_SUCCESS)
    {
        ESP_LOGE(TAG, "Error occured whilst retrieving version info - %d", status);
    }

    ESP_LOGI(TAG, "  Morselib version:        %s", version.morselib_version);
    ESP_LOGI(TAG, "  Morse firmware version:  %s", version.morse_fw_version);
    ESP_LOGI(TAG, "  Morse chip ID:           0x%04lx", version.morse_chip_id);
    ESP_LOGI(TAG, "-----------------------------------");
}

static void wifi_start(void *esp_netif)
{
    uint8_t mac[6];

    mmwlan_get_mac_addr(mac);
    ESP_LOGI(TAG, "Wi-Fi MAC address: " MM_MAC_ADDR_FMT, MM_MAC_ADDR_VAL(mac));

    esp_netif_set_mac(esp_netif, mac);
    esp_netif_action_start(esp_netif, NULL, 0, NULL);
}

esp_err_t mmhalow_init(const wifi_init_config_t *config)
{
    ESP_UNUSED(config);
    MMOSAL_ASSERT(halow_netif == NULL);

    /* Initialize Morse subsystems, note that they must be called in this order. */
    mmhal_init();
    mmwlan_init();

    const struct mmwlan_regulatory_db *db = get_regulatory_db();
    const struct mmwlan_s1g_channel_list *channel_list =
        mmwlan_lookup_regulatory_domain(db, WARTHOG_COUNTRY_CODE);

#ifdef WARTHOG_PIN_S1G_CHAN
    /* Pin the radio to a SINGLE S1G channel. mmwlan_set_channel_list() must be
     * called while the WLAN subsystem is inactive and BEFORE mmwlan_boot() --
     * calling it later returns MMWLAN_UNAVAILABLE (3), so the pin has to live
     * here rather than in the mesh bring-up path.
     *
     * A single-entry list makes the operating channel deterministic, which is
     * what lets an external radio (Linux morse_driver on an MM8108) be brought
     * up on a matching channel to sniff or to peer with us. The full
     * regulatory list leaves the channel implicit and unobservable.
     *
     * WARTHOG_PIN_S1G_CHAN selects the S1G channel number; the entry below is
     * the matching row from the US regulatory table. */
    /* Operating class and bandwidth travel WITH the channel -- they are not
     * independent knobs. A peer's mesh_matches_local() compares the operating
     * class it sees against its own, so a channel change with a stale class
     * peers with nothing and looks like a radio fault. The values here must
     * match a row in mmregdb.c for the configured country:
     *
     *   ch 5  = 904.5 MHz, 1 MHz, global 68 / s1g 1   (our default)
     *   ch 42 = 923.0 MHz, 2 MHz, global 69 / s1g 2   (mmregdb.c US row;
     *                                                  OpenMANET's default)
     *
     * Overridable together from build flags so a channel profile is one
     * coherent choice; see the openmanet-parity notes in platformio.ini. */
#ifndef WARTHOG_PIN_S1G_GLOBAL_OP_CLASS
#define WARTHOG_PIN_S1G_GLOBAL_OP_CLASS 68
#endif
#ifndef WARTHOG_PIN_S1G_OP_CLASS
#define WARTHOG_PIN_S1G_OP_CLASS 1
#endif
#ifndef WARTHOG_PIN_S1G_BW_MHZ
#define WARTHOG_PIN_S1G_BW_MHZ 1
#endif
#ifndef WARTHOG_PIN_S1G_EIRP_DBM
#define WARTHOG_PIN_S1G_EIRP_DBM 36
#endif

    static const struct mmwlan_s1g_channel warthog_pinned_chan[] = {
        /* freq_hz, duty_cycle, omit_ctrl_resp, global_op_class, s1g_op_class,
         * s1g_chan, bw_mhz, max_eirp_dbm, pkt_spacing, airtime_min, airtime_max */
        { WARTHOG_PIN_S1G_FREQ_HZ, 10000, false, WARTHOG_PIN_S1G_GLOBAL_OP_CLASS,
          WARTHOG_PIN_S1G_OP_CLASS, WARTHOG_PIN_S1G_CHAN, WARTHOG_PIN_S1G_BW_MHZ,
          WARTHOG_PIN_S1G_EIRP_DBM, 0, 0, 0 },
    };
    static const struct mmwlan_s1g_channel_list warthog_pinned_list = {
        .country_code = WARTHOG_COUNTRY_CODE,
        .num_channels = 1,
        .channels = warthog_pinned_chan,
    };
    channel_list = &warthog_pinned_list;
    ESP_LOGW(TAG, "PINNED S1G chan %d (%u Hz, %d MHz BW) -- single-channel list",
             (int)WARTHOG_PIN_S1G_CHAN, (unsigned)WARTHOG_PIN_S1G_FREQ_HZ,
             (int)WARTHOG_PIN_S1G_BW_MHZ);
#endif

    ESP_LOGI(TAG, "Setting Channel List %s", WARTHOG_COUNTRY_CODE);
    enum mmwlan_status chan_st = mmwlan_set_channel_list(channel_list);
    ESP_LOGI(TAG, "mmwlan_set_channel_list -> %d (0=SUCCESS)", (int)chan_st);
    /* Stash for later reporting: this runs before the USB CDC console exists,
     * so the log line above is invisible to a host attaching after boot. */
    g_warthog_chan_pin_status = (int)chan_st;

    /* Boot the WLAN interface so that we can retrieve the firmware version. */
    struct mmwlan_boot_args boot_args = MMWLAN_BOOT_ARGS_INIT;
    (void)mmwlan_boot(&boot_args);
    mmhalow_print_version_info();
#if !CONFIG_HALOW_PS_MODE
    mmwlan_set_power_save_mode(MMWLAN_PS_DISABLED);
#endif /* CONFIG_HALOW_PS_MODE */

    enum mmwlan_status status;

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_WIFI_STA();
    esp_netif_t *netif = esp_netif_new(&cfg);
    assert(netif);
    mmhalow_netif_driver_t *driver = mmhalow_create_if_driver();

    esp_netif_attach(netif, driver);

    status = mmwlan_register_rx_pkt_cb(halow_rx, netif);
    MMOSAL_ASSERT(status == MMWLAN_SUCCESS);

    status = mmwlan_register_link_state_cb(mmhalow_link_state, netif);
    MMOSAL_ASSERT(status == MMWLAN_SUCCESS);

    wifi_start(netif);

    return status;
}

esp_err_t mmhalow_deinit()
{
    /* Shutdown wlan interface */
    return mmwlan_shutdown();
}

esp_err_t mmhalow_scan(struct mmhalow_scan_args *args)
{
    MMOSAL_ASSERT(args);
    struct mmwlan_scan_req scan_req = MMWLAN_SCAN_REQ_INIT;
    scan_req.scan_rx_cb = args->rx_cb;
    scan_req.scan_complete_cb = args->complete_cb;
    scan_req.scan_cb_arg = args->cb_arg;
    return mmwlan_scan_request(&scan_req);
}

static void set_config_sta(struct mmwlan_sta_args *conf)
{
    mmhalow_netif_driver_t *morse_drv = esp_netif_get_io_driver(halow_netif);
    memcpy(&morse_drv->sta_args, conf, sizeof(*conf));
}

static void set_config_ap(struct mmwlan_ap_args *conf)
{
    mmhalow_netif_driver_t *morse_drv = esp_netif_get_io_driver(halow_netif);
    MMOSAL_ASSERT(morse_drv);

    memcpy(&morse_drv->ap_args, conf, sizeof(*conf));
}

esp_err_t mmhalow_set_config(wifi_interface_t interface, mmhalow_wifi_config_t *conf)
{
    MMOSAL_ASSERT(conf);
    switch (interface)
    {
        case WIFI_IF_STA:
            set_config_sta(&conf->sta);
            break;
        case WIFI_IF_AP:
            set_config_ap(&conf->ap);
            break;
        case WIFI_IF_NAN:
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

static void get_config_sta(struct mmwlan_sta_args *conf)
{
    mmhalow_netif_driver_t *morse_drv = esp_netif_get_io_driver(halow_netif);
    MMOSAL_ASSERT(morse_drv);

    (void)mmosal_safer_strcpy((char *)conf->ssid,
                              (char *)morse_drv->sta_args.ssid,
                              sizeof(conf->ssid));

    /* We cannot safely copy password from sta_args.passphrase into conf->password because:
     * sta_args.passphrase == 100 bytes
     * conf->password == 64 bytes
     * GCC complains about truncation
     */
}

esp_err_t mmhalow_get_config(wifi_interface_t interface, mmhalow_wifi_config_t *conf)
{
    MMOSAL_ASSERT(conf);
    switch (interface)
    {
        case WIFI_IF_STA:
            get_config_sta(&conf->sta);
            break;
        case WIFI_IF_AP:
        case WIFI_IF_NAN:
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

esp_err_t mmhalow_connect(mmwlan_sta_status_cb_t cb)
{
    mmhalow_netif_driver_t *morse_drv = esp_netif_get_io_driver(halow_netif);
    MMOSAL_ASSERT(morse_drv);

    ESP_LOGI(TAG, "Attempting to connect to: %s", morse_drv->sta_args.ssid);

    return mmwlan_sta_enable(&morse_drv->sta_args, cb);
}

esp_err_t mmhalow_disconnect()
{
    return mmwlan_sta_disable();
}

enum mmwlan_status mmhalow_status()
{
    return mmwlan_get_sta_state();
}

void mmhalow_wifi_start(){
    mmhalow_netif_driver_t *morse_drv = esp_netif_get_io_driver(halow_netif);
    mmwlan_ap_enable(&morse_drv->ap_args);
}
