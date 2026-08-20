/*
 * AT-style command parser on the TinyUSB CDC-ACM interface.
 *
 * Wire-format is line-oriented, terminated by \r or \n. Echo is local — every
 * received byte is queued back to the host so a dumb terminal shows the user
 * what they're typing. Backspace (0x08 / 0x7F) trims the buffer in place.
 *
 * Replies follow Hayes conventions: each command emits zero or more
 * `+TAG: value` lines, then a terminating `OK` or `ERROR`.
 *
 * Anything that mutates persistent state writes to NVS via cfg.c and prints
 * `OK`; the caller is expected to follow with `AT+RESET` to apply.
 */

#include "at.h"
#include "cfg.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/rtc_cntl_reg.h"   /* RTC_CNTL_OPTION1_REG for AT+DLMODE */
#include "esp_attr.h"   /* RTC_NOINIT_ATTR */
#include "esp_core_dump.h" /* AT+COREDUMP? crash forensics */
#include "soc/soc.h"            /* REG_WRITE */
#include "tinyusb_cdc_acm.h"
#include "tusb.h"
#include "ping/ping_sock.h"
#include "mmwlan_mesh.h"
#include "mudp.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "esp_timer.h"
#include "lwip/netif.h"
#include <errno.h>
#include "freertos/semphr.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "warthog.at";

#define AT_LINE_MAX 192
#define AT_RX_POLL_MS 20

#ifndef WARTHOG_VERSION
#define WARTHOG_VERSION "0.0.0"
#endif

static void cdc_write(const char *s)
{
    if (!s || !tud_cdc_n_connected(0)) {
        return;
    }
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)s, strlen(s));
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
}

static void reply_ok(void)
{
    cdc_write("OK\r\n");
}

static void reply_error(const char *why)
{
    if (why) {
        char buf[80];
        snprintf(buf, sizeof(buf), "+ERR: %s\r\n", why);
        cdc_write(buf);
    }
    cdc_write("ERROR\r\n");
}

/* Trim ASCII whitespace in-place from both ends. Returns the trimmed start. */
static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    if (!*s) {
        return s;
    }
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end-- = '\0';
    }
    return s;
}

/* Case-insensitive prefix check. */
static bool starts_with_i(const char *s, const char *prefix)
{
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) {
            return false;
        }
        s++;
        prefix++;
    }
    return true;
}

/* USB network transmit, counted because the deferred path can drop.
 * sent/dropped are per frame; a rising dropped count means every NCM transmit
 * buffer was busy when the frame arrived, which is a throughput ceiling rather
 * than a fault. */
volatile uint32_t g_warthog_usb_tx_sent = 0, g_warthog_usb_tx_dropped = 0;

static void cmd_status(void)
{
    char buf[160];

    /* HaLow STA netif (driven by morsemicro/halow, key WIFI_STA_DEF) */
    esp_netif_t *halow = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t halow_ip = {0};
    if (halow) {
        esp_netif_get_ip_info(halow, &halow_ip);
    }
    snprintf(buf, sizeof(buf), "+HALOW: ip=" IPSTR " gw=" IPSTR "\r\n",
             IP2STR(&halow_ip.ip), IP2STR(&halow_ip.gw));
    cdc_write(buf);

    /* USB netif */
    esp_netif_t *usb = esp_netif_get_handle_from_ifkey("USB");
    esp_netif_ip_info_t usb_ip = {0};
    if (usb) {
        esp_netif_get_ip_info(usb, &usb_ip);
    }
    snprintf(buf, sizeof(buf), "+USB: ip=" IPSTR " mounted=%d tx=%lu drop=%lu\r\n",
             IP2STR(&usb_ip.ip), tud_mounted() ? 1 : 0,
             (unsigned long)g_warthog_usb_tx_sent, (unsigned long)g_warthog_usb_tx_dropped);
    cdc_write(buf);

    /* Wi-Fi AP netif */
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ap_ip = {0};
    if (ap) {
        esp_netif_get_ip_info(ap, &ap_ip);
    }
    snprintf(buf, sizeof(buf), "+AP: ip=" IPSTR "\r\n", IP2STR(&ap_ip.ip));
    cdc_write(buf);

    /* DNS handed to downstream clients via DHCP option 6 */
    char dns_str[16] = {0};
    warthog_cfg_get_dns(dns_str, sizeof(dns_str));
    snprintf(buf, sizeof(buf), "+DNS: offered=%s\r\n", dns_str);
    cdc_write(buf);

    reply_ok();
}

static void cmd_halow_query(void)
{
    char ssid[33] = {0};
    warthog_cfg_get_halow_ssid(ssid, sizeof(ssid));
    char buf[64];
    snprintf(buf, sizeof(buf), "+HALOW: ssid=\"%s\" (psk hidden)\r\n", ssid);
    cdc_write(buf);
    reply_ok();
}

/* AT+HALOW=<ssid>,<psk> */
static void cmd_halow_set(char *args)
{
    char *comma = strchr(args, ',');
    if (!comma) {
        reply_error("usage: AT+HALOW=<ssid>,<psk>");
        return;
    }
    *comma = '\0';
    char *ssid = trim(args);
    char *psk = trim(comma + 1);
    if (ssid[0] == '\0') {
        reply_error("empty ssid");
        return;
    }
    {
        esp_err_t serr = warthog_cfg_set_halow(ssid, psk);
        if (serr == ESP_ERR_INVALID_SIZE) {
            /* Not an NVS failure -- it would store fine and then be dropped at
             * boot for not fitting the reader's buffer. Say the real reason. */
            reply_error("ssid max 32 chars, psk max 64");
            return;
        }
        if (serr != ESP_OK) {
            reply_error("nvs write");
            return;
        }
    }
    cdc_write("+INFO: stored. AT+RESET to apply.\r\n");
    reply_ok();
}

static void cmd_wifiap_query(void)
{
    char ssid[33] = {0};
    warthog_cfg_get_ap_ssid(ssid, sizeof(ssid));
    uint8_t chan = warthog_cfg_get_ap_channel();
    char buf[80];
    snprintf(buf, sizeof(buf), "+WIFIAP: ssid=\"%s\" chan=%u (psk hidden)\r\n",
             ssid, chan);
    cdc_write(buf);
    reply_ok();
}

/* AT+WIFIAP=<ssid>,<psk>[,<chan>] */
static void cmd_wifiap_set(char *args)
{
    char *first = strchr(args, ',');
    if (!first) {
        reply_error("usage: AT+WIFIAP=<ssid>,<psk>[,<chan>]");
        return;
    }
    *first = '\0';
    char *ssid = trim(args);
    char *rest = trim(first + 1);
    char *psk = rest;
    int channel = 0; /* 0 → keep existing */

    char *second = strchr(rest, ',');
    if (second) {
        *second = '\0';
        psk = trim(rest);
        char *chan_s = trim(second + 1);
        if (chan_s[0] != '\0') {
            channel = atoi(chan_s);
            if (channel < 1 || channel > 13) {
                reply_error("channel must be 1..13");
                return;
            }
        }
    }
    if (ssid[0] == '\0') {
        reply_error("empty ssid");
        return;
    }
    {
        esp_err_t serr = warthog_cfg_set_ap(ssid, psk, channel);
        if (serr == ESP_ERR_INVALID_SIZE) {
            reply_error("ssid max 32 chars, psk max 64");
            return;
        }
        if (serr != ESP_OK) {
            reply_error("nvs write");
            return;
        }
    }
    cdc_write("+INFO: stored. AT+RESET to apply.\r\n");
    reply_ok();
}

static void cmd_dns_query(void)
{
    char dns_str[16] = {0};
    warthog_cfg_get_dns(dns_str, sizeof(dns_str));
    char buf[48];
    snprintf(buf, sizeof(buf), "+DNS: %s\r\n", dns_str);
    cdc_write(buf);
    reply_ok();
}

/* AT+DNS=<ip>  — dotted-quad IPv4. Persists to NVS; AT+RESET to apply. */
static void cmd_dns_set(char *args)
{
    char *ip = trim(args);
    if (ip[0] == '\0') {
        reply_error("usage: AT+DNS=<ipv4>");
        return;
    }
    esp_err_t err = warthog_cfg_set_dns(ip);
    if (err == ESP_ERR_INVALID_ARG) {
        reply_error("not a valid IPv4 address");
        return;
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        reply_error("ssid max 32 chars, psk max 64");
        return;
    }
    if (err != ESP_OK) {
        reply_error("nvs write");
        return;
    }
    cdc_write("+INFO: stored. AT+RESET to apply.\r\n");
    reply_ok();
}

static void cmd_version(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "+VERSION: warthog %s\r\n", WARTHOG_VERSION);
    cdc_write(buf);
    reply_ok();
}

static void cmd_reset(void)
{
    cdc_write("+INFO: rebooting\r\n");
    reply_ok();
    /* Give the host a moment to drain TX before we yank the bus. */
    vTaskDelay(pdMS_TO_TICKS(200));
    /* Same reason cmd_dlmode() avoids esp_restart(): it runs every shutdown
     * handler and resets peripheral modules before the CPU-only soft reset, so
     * anything stuck in between -- a HaLow SPI transaction in flight, a task
     * holding a lock the teardown wants -- lets the watchdog fire instead.
     * Measured with the mesh data plane live: the board never came back on
     * USB and needed a physical power cycle, which is a poor answer to a
     * command whose entire job is "apply this and come back". A direct RTC
     * system reset takes none of those paths.
     *
     * Documented workflows depend on this: AT+HALOW, AT+WIFIAP and AT+DNS all
     * say "AT+RESET to apply". */
    portDISABLE_INTERRUPTS();
    REG_WRITE(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_SYS_RST);
    while (1) { }
}

/* — alternate download-mode entry independent of the 1200bps shim.
 * Use this when the host needs to flash and the CDC line-coding trick isn't
 * working (e.g., macOS not propagating SET_LINE_CODING in some configurations).
 * Sets the same RTC bit the 1200bps path sets, then restarts. ROM bootloader
 * skips the app and enters USB-Serial-JTAG download mode. */
static void cmd_dlmode(void)
{
    cdc_write("+INFO: entering ROM download mode\r\n");
    reply_ok();
    vTaskDelay(pdMS_TO_TICKS(200));
    /* Set the ROM download-boot flag, then reset the SYSTEM directly at the
     * RTC level. Do NOT go through esp_restart(): that runs every shutdown
     * handler, resets peripheral modules, and only then does a CPU-only soft
     * reset -- and if anything hangs in between (a HaLow SPI transaction in
     * flight, a task holding a lock the teardown needs) the task/RTC watchdog
     * fires instead, and THAT reset path clears RTC_CNTL_OPTION1. Result:
     * DLMODE worked on a fresh boot and failed once the mesh data plane was
     * live, re-enumerating as the app (303a:4020) instead of ROM (303a:0009).
     * The flag lives in the RTC domain precisely so a system reset preserves
     * it; a direct RTC_CNTL_SW_SYS_RST is the reset it is designed for. */
    portDISABLE_INTERRUPTS();
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    REG_WRITE(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_SYS_RST);
    while (1) { }
}

/* AT+MESHSTAT? — report the mesh receive-path counters.
 *
 * The board's log output is unreachable: ESP_CONSOLE is USB_SERIAL_JTAG but the
 * app switches the shared USB PHY to TinyUSB for this very console, so MMLOG
 * lines (including RXTAP) go nowhere. These counters are incremented at the RX
 * tap in umac_datapath.c, upstream of every host-side drop, and are the only
 * way to answer "does the chip hand mesh frames to the host?" on real hardware.
 *
 * rx=0 with a peer verifiably transmitting means the frames never reach the
 * host at all. Non-zero means they do and the loss is further up the stack. */
/* Storage lives here, in main, and morselib's RX tap refers to it as extern.
 * The reverse does not link: morselib is an archive, and the linker will not
 * extract an object from it merely to satisfy a reference coming from main. */
#ifdef WARTHOG_MESH_RX_TAP
volatile uint32_t g_warthog_rxtap_total = 0;
volatile uint32_t g_warthog_rxtap_mgmt = 0;
volatile uint32_t g_warthog_rxtap_beacon = 0;
volatile uint16_t g_warthog_rxtap_last_fc = 0;
volatile uint8_t g_warthog_rxtap_last_ta[6] = { 0 };
#endif

/* Beacon-handshake counters, incremented in morselib's mmdrv_host_get_beacon.
 * Defined unconditionally so the beacon path can be instrumented independently
 * of the RX tap. Reported by AT+BCNSTAT?. */
volatile uint32_t g_warthog_bcn_req = 0;      /* chip asked for a template   */
volatile uint32_t g_warthog_bcn_served = 0;   /* host returned a beacon      */
volatile uint32_t g_warthog_bcn_null = 0;     /* mesh active, build returned NULL */
volatile uint32_t g_warthog_bcn_enq_ok = 0;   /* beacon enqueued to chip TX queue */
volatile uint32_t g_warthog_bcn_enq_err = 0;  /* beacon enqueue failed */
volatile uint32_t g_warthog_bcn_txcomp = 0;   /* chip reported beacon TX completion */
volatile uint32_t g_warthog_bcn_inactive = 0; /* asked while mesh not active */

/* Mesh probe-response counters (AT+PRSPSTAT?). rx = probe requests received
 * from peers, tx = probe responses successfully handed to the chip. */
volatile uint32_t g_warthog_prsp_rx = 0;
volatile uint32_t g_warthog_prsp_tx = 0;
volatile uint32_t g_warthog_prsp_fail = 0;
volatile uint8_t g_warthog_prsp_last_da[6] = { 0 };
volatile uint8_t g_warthog_bcn_own[160] = { 0 };
volatile uint16_t g_warthog_bcn_own_len = 0;
volatile uint8_t g_warthog_bcn_rx_frame[160] = { 0 };
volatile uint16_t g_warthog_bcn_rx_len = 0;
/* Beaconless-peer discovery: last received probe request (IEs only) and the
 * counters for how many named our mesh / were offered to hostap as SAE
 * candidates (AT+PRQRX?, and prq_* on AT+PRSPSTAT?). */
volatile uint8_t g_warthog_prq_frame[96] = { 0 };
volatile uint16_t g_warthog_prq_len = 0;
volatile uint8_t g_warthog_prq_ta[6] = { 0 };
volatile uint32_t g_warthog_prq_named = 0;
volatile uint32_t g_warthog_prq_offer = 0;
/* AMPE interop forensics: last SELF_PROTECTED (category 15) action body we
 * transmitted (exactly what hostap's AMPE MIC covers) and the last one we
 * received raw off the air (AT+PLINKTX? / AT+PLINKRX?). */
volatile uint8_t g_warthog_plink_tx[192] = { 0 };
volatile uint16_t g_warthog_plink_tx_len = 0;
volatile uint16_t g_warthog_plink_tx_full = 0;
volatile uint8_t g_warthog_plink_rx[192] = { 0 };
volatile uint16_t g_warthog_plink_rx_len = 0;
volatile uint16_t g_warthog_plink_rx_full = 0;
/* Peering frames converted between the S1G form on air and the 11n form
 * hostap signs (AT+PLINKSTAT?). Both must climb once a mac80211 peer is
 * peering; TX stuck at 0 means our own frames never reached the converter. */
volatile uint32_t g_warthog_mpm_tx_conv = 0;
volatile uint32_t g_warthog_mpm_rx_conv = 0;

/* Mesh peering (MPM) counters (AT+MPMSTAT?). */
volatile uint32_t g_warthog_mpm_rx = 0;
volatile uint32_t g_warthog_mpm_open_tx = 0;
volatile uint32_t g_warthog_mpm_conf_tx = 0;
volatile uint32_t g_warthog_mpm_conf_rx = 0;
volatile uint32_t g_warthog_mpm_close_rx = 0;
volatile uint32_t g_warthog_mpm_parse_fail = 0;
volatile uint32_t g_warthog_mpm_our_llid = 0;
volatile uint32_t g_warthog_mpm_our_plid = 0;

/* Last MPM Confirm body we built, for AT+MPMDUMP? (no on-air capture is
 * possible: this driver does not support monitor mode). */
volatile uint8_t g_warthog_mpm_dump[96];
volatile uint16_t g_warthog_mpm_dump_len = 0;   /* bytes captured (clamped) */
volatile uint16_t g_warthog_mpm_dump_full = 0;  /* true body length */

/* 802.11 reason code from the peer's last Mesh Peering Close (52-60 range). */
volatile uint32_t g_warthog_mpm_close_reason = 0;
/* 1 once a peer's Confirm echoed our llid -- warthog-side ESTAB indicator. */
volatile uint32_t g_warthog_mpm_estab = 0;
/* umac_datapath_mesh_add_peer() failures at ESTAB (table full / no memory). */
volatile uint32_t g_warthog_mesh_peer_add_fail = 0;
/* mmdrv_update_sta_state() failures when registering a mesh peer with the chip. */
volatile uint32_t g_warthog_mesh_chip_sta_fail = 0;
/* umac_keys_install_key() failures at ESTAB. */
volatile uint32_t g_warthog_mesh_key_fail = 0;
/* Mesh data-plane protection. 1 (default) = register peers as secured and
 * install the static MTK/MGTK; 0 = leave them OPEN.
 *
 * A keyed mesh cannot carry data to a peer running an OPEN mesh, which is what
 * stock OpenMANET ships (encryption='none'). The keying was introduced because
 * an OPEN mesh measured acked-but-never-delivered -- but the chip STA
 * registration that also fixes delivery landed in the same change, so which of
 * the two actually opened the gate was never isolated. AT+MESHSEC= flips this
 * at runtime and re-peers, so the two can be told apart on hardware. */
volatile uint32_t g_warthog_mesh_secure = 1;

/* Data-plane counters (AT+DATASTAT?). rxtap_data = data frames the chip
 * delivered; stad_hit/miss = whether the peer table resolved the sender;
 * delivered = frames handed up as 802.3 to lwIP; tx_* = the outbound path. */
volatile uint32_t g_warthog_rxtap_data = 0;
volatile uint32_t g_warthog_rx_data_stad_hit = 0;
volatile uint32_t g_warthog_rx_data_stad_miss = 0;
volatile uint8_t g_warthog_rx_data_miss_ta[6] = { 0 };
volatile uint32_t g_warthog_rx_data_delivered = 0;
volatile uint32_t g_warthog_tx_data_enq = 0;
volatile uint32_t g_warthog_tx_data_deq = 0;
volatile uint32_t g_warthog_tx_data_hdr = 0;
volatile uint32_t g_warthog_tx_drv_ok = 0;      /* mmdrv_tx_frame() returned 0 */
volatile uint32_t g_warthog_tx_drv_err = 0;     /* mmdrv_tx_frame() failed      */
volatile int32_t  g_warthog_tx_drv_last_err = 0;
/* Chip TX-status reports: the chip's own verdict per transmitted frame. */
volatile uint32_t g_warthog_txst_total = 0;
volatile uint32_t g_warthog_txst_acked = 0;
volatile uint32_t g_warthog_txst_noack = 0;
volatile uint32_t g_warthog_txst_unsent = 0;
volatile uint32_t g_warthog_txst_last_flags = 0;
volatile uint32_t g_warthog_tx_protected = 0;  /* frames sent with Protected bit + HW key */
volatile uint32_t g_warthog_tx_nokey = 0;      /* keyed stad but no active key found     */
volatile uint32_t g_warthog_tx_last_key = 0;
/* TX status for DATA frames only (aid != 0). */
volatile uint32_t g_warthog_txst_data_total = 0;
volatile uint32_t g_warthog_txst_data_acked = 0;
volatile uint32_t g_warthog_txst_data_noack = 0;
volatile uint32_t g_warthog_txst_data_unsent = 0;
volatile uint32_t g_warthog_txst_data_last_flags = 0;
/* Bus-level RX page histogram by chip channel (AT+RXCHAN?). Earliest possible
 * observation point on the host: a page counted here was pushed by the chip. */
volatile uint32_t g_warthog_shim_rx = 0, g_warthog_shim_rx_notrunning = 0;
volatile uint32_t g_warthog_rx_meshctrl_stripped = 0; /* RX frames whose Mesh Control we removed */
volatile uint32_t g_warthog_mesh_seq = 0;
volatile uint32_t g_warthog_nodec_group = 0, g_warthog_nodec_fc = 0, g_warthog_nodec_keyid = 0;
volatile uint32_t g_warthog_nodec_group_n = 0, g_warthog_nodec_uni_n = 0;
volatile uint8_t g_warthog_nodec_ta[6];
volatile uint32_t g_warthog_tx_bcast_dup = 0, g_warthog_tx_bcast_copy_fail = 0;
volatile uint32_t g_warthog_keyinst[8]; volatile uint32_t g_warthog_keyinst_n = 0;
volatile uint8_t g_warthog_mesh_self_addr[6];
volatile uint32_t g_warthog_peer_fp[4], g_warthog_peer_mac[4];
volatile uint32_t g_warthog_rekey_req = 0, g_warthog_rekey_done = 0, g_warthog_rekey_aid = 0;
volatile uint32_t g_warthog_mesh_repeer_req = 0;
volatile uint32_t g_warthog_s1g_bcn_rx = 0, g_warthog_s1g_bcn_ours = 0, g_warthog_s1g_bcn_new = 0;
volatile uint32_t g_warthog_s1g_bcn_retry = 0;
volatile uint8_t g_warthog_s1g_bcn_sa[6];
volatile uint32_t g_warthog_bcn_peer_rx = 0; /* beacons matching our Mesh ID */
volatile uint32_t g_warthog_ccmp_kat_ok = 0, g_warthog_ccmp_kat_ran = 0;
volatile uint32_t g_warthog_ccmp_kat_fail_stage = 0;
volatile uint32_t g_warthog_aes_ns_per_block = 0, g_warthog_ccmp_us_per_frame = 0;
volatile uint32_t g_warthog_mbedtls_us_per_frame = 0;
volatile uint32_t g_warthog_aes_setup_ns = 0, g_warthog_aes_ecb_ns = 0;
volatile uint32_t g_warthog_aes_ctr_us = 0;
volatile uint32_t g_warthog_bulk_ccm_us = 0;
volatile uint32_t g_warthog_cryptohost_req = 0, g_warthog_cryptohost_done = 0;
volatile uint32_t g_warthog_cryptohost_rc = 0, g_warthog_cryptohost_val = 0xffffffffu;
volatile uint32_t g_warthog_mpm_close_tx = 0; /* Close frames we sent */
volatile uint32_t g_warthog_mpm_expired = 0; /* links dropped for inactivity */
volatile uint32_t g_warthog_mpm_no_slot = 0; /* Opens refused: MPM link table full */
volatile char g_warthog_mpm_links[256] = "(none) "; /* rendered by mpm_publish_ */
volatile uint16_t g_warthog_fc_ring[32];
volatile uint32_t g_warthog_fc_ring_idx = 0;
/* Which numbered `goto drop` in process_rx_data_frame_after_reorder fired last. */
volatile uint32_t g_warthog_rxdrop_reason = 0, g_warthog_rxdrop_count = 0;

/* RX block-ack reorder accounting.
 *
 * The reorder path can consume a frame in two ways that no existing counter
 * records: a sequence number below the expected one is dropped as outdated,
 * and one above it is parked in the reorder list until a gap fills or a timer
 * fires. Neither touches g_warthog_rxdrop_count, so unicast traffic from a
 * peer whose sequence space we are out of step with disappears with every
 * visible counter reading normal -- rx_data climbs, rxdrop does not move, and
 * nothing is delivered. That is exactly how it presented against an OpenMANET
 * peer. Instrument both, and keep the two sequence numbers from the last
 * outdated drop: the pair is what says whether we are out of step and by how
 * much. */
volatile uint32_t g_warthog_reord_outdated = 0;  /* dropped: seq < expected */
volatile uint32_t g_warthog_reord_buffered = 0;  /* parked: seq > expected */
volatile uint32_t g_warthog_reord_released = 0;  /* handed up out of the list */
volatile uint32_t g_warthog_reord_bypass = 0;    /* delivered, no BA session */
volatile uint32_t g_warthog_reord_last_seq = 0;  /* seq at the last outdated drop */
volatile uint32_t g_warthog_reord_last_exp = 0;  /* expected at that moment */

/* RX frame-filter drop accounting.
 *
 * umac_datapath_rx_frame_filter() has eight ways to discard a frame and, until
 * now, none of them incremented anything: filter= counts frames ENTERING, so a
 * peer whose traffic dies inside reads as 42 in and 1 delivered with every drop
 * counter at zero. Reasons, in the order they appear in that function:
 *   1 short (frame control)   2 RTS          3 beacon filtered
 *   4 short (header)          5 no datapath ops
 *   6 unknown sender          7 SA is our own address    8 duplicate frame
 * A histogram plus the last reason is enough to name the cause in one read. */
volatile uint32_t g_warthog_filt_reason = 0, g_warthog_filt_drop = 0;
volatile uint32_t g_warthog_filt_hist[9] = { 0 };

/* HWMP path selection. A mac80211 peer will not send a unicast data frame to a
 * neighbour it has no PATH to, and peering ESTAB does not create one -- so
 * before this, warthog was reachable by broadcast and unreachable by anything
 * else. preq_tx is what makes us routable (a peer installs a path to any PREQ
 * originator it accepts); prep_tx is what answers a peer's discovery. */
volatile uint32_t g_warthog_hwmp_rx = 0, g_warthog_hwmp_preq_rx = 0, g_warthog_hwmp_preq_tx = 0;
volatile uint32_t g_warthog_hwmp_prep_tx = 0, g_warthog_hwmp_parse_fail = 0;
volatile uint32_t g_warthog_hwmp_not_ours = 0;
volatile uint32_t g_warthog_hwmp_prep_rx = 0;

/* AMPE key installs. Non-zero means SAE/AMPE actually derived a key and it
 * reached the chip -- the difference between real mesh security and the
 * hardcoded constant. */
volatile uint32_t g_warthog_ampe_mtk_installed = 0, g_warthog_ampe_mgtk_installed = 0;
/* 0 = open mesh, 1 = SAE authenticator initialised, 2 = SAE asked for but
 * mesh_rsn_auth_init() failed (mesh still runs, unsecured). */
volatile uint32_t g_warthog_sae_init = 0;
volatile uint32_t g_warthog_sae_peer_offers = 0, g_warthog_sae_peer_parse_fail = 0;
volatile uint32_t g_warthog_sae_mlme_tx = 0, g_warthog_sae_mlme_auth_tx = 0;
volatile unsigned int g_warthog_sae_addpeer_null = 0, g_warthog_sae_addpeer_ok = 0;
volatile unsigned int g_warthog_sae_authsta_fail = 0, g_warthog_sae_authsta_ok = 0;
volatile uint32_t g_warthog_sae_rates_synth = 0;
volatile uint32_t g_warthog_sae_peer_authproto = 0;
volatile uint32_t g_warthog_sae_peer_authval = 0xff;
volatile unsigned int g_warthog_addp_r_crowded = 0, g_warthog_addp_r_exists = 0;
volatile unsigned int g_warthog_addp_r_addfail = 0, g_warthog_addp_r_rates = 0;
volatile uint32_t g_warthog_sae_sta_add_ok = 0, g_warthog_sae_sta_add_fail = 0;
volatile uint32_t g_warthog_rx_auth = 0;
volatile uint32_t g_warthog_rx_auth_router = 0, g_warthog_rx_auth_other = 0;
/* A3-rewrite kill-switch, default OFF: hostap sets AUTH A3=SA, exactly like
 * the ACTION frames that already cross in the open mesh, so try that first. */
volatile uint32_t g_warthog_a3fix_en = 0;
volatile unsigned int g_warthog_sae_hdl_auth = 0, g_warthog_sae_hdl_sae = 0;
volatile unsigned int g_warthog_sae_rx_trans = 0, g_warthog_sae_rx_status = 0;
volatile unsigned int g_warthog_sae_state_now = 0, g_warthog_sae_state_seen = 0;
volatile unsigned int g_warthog_sae_confirm_tx = 0;
volatile unsigned int g_warthog_sae_tx_status = 0, g_warthog_sae_alg_reject = 0;
volatile unsigned int g_warthog_sae_keymgmt = 0, g_warthog_sae_wpa = 0;
volatile unsigned int g_warthog_sae_txfail = 0, g_warthog_sae_txfail_last = 0;
volatile unsigned int g_warthog_sae_rxfail_sa = 0;
volatile unsigned int g_warthog_sae_rx_sa = 0, g_warthog_sae_rx_da = 0;
volatile uint32_t g_warthog_sae_offer_addr = 0;
volatile unsigned int g_warthog_mpm_act_rx = 0, g_warthog_mpm_act_tx = 0;
volatile unsigned int g_warthog_mpm_plink = 0, g_warthog_mpm_plink_seen = 0;
volatile unsigned int g_warthog_hostap_estab = 0, g_warthog_mpm_fsm = 0;
volatile unsigned int g_warthog_accept = 0, g_warthog_accept_nosm = 0, g_warthog_ampe_start = 0;
volatile unsigned int g_warthog_rx_selfprot = 0, g_warthog_rx_action_any = 0;
volatile unsigned int g_warthog_evt_action = 0, g_warthog_evt_mgmt = 0;
volatile unsigned int g_warthog_mesh_act_oversize = 0;
/* Survives the panic SW-reset (only a power-off clears RTC): the last per-peer
 * SAE step reached before a crash. Tagged 0x5AE0xxxx so garbage at power-on is
 * distinguishable from a real reading. */
RTC_NOINIT_ATTR volatile uint32_t g_warthog_sae_stage;
/* SAE discovery-bridge kill-switch. Defaults ON so a SAE build peers on its
 * own; AT+SAEBRIDGE=0 makes the node deaf to candidates, which is the state
 * to debug from when the SAE path itself is suspected (a crash there takes
 * the USB CDC down with it -- see warthog_sae_trace). */
volatile uint32_t g_warthog_sae_bridge_en = 1;

/* Live breadcrumb for the SAE path: stamps RTC (survives SW reset) AND prints
 * straight to the AT CDC. The CDC rides the TinyUSB task, so when the SAE
 * path hangs the evtloop with interrupts held, everything already queued
 * still drains -- the last [SAE:n] seen on the wire is the step that died. */
void warthog_sae_trace(uint32_t n)
{
    /* RTC stamp only. This deliberately does NOT print: it runs on the
     * evtloop task for every step of every peer offer, and writing to the AT
     * CDC from here interleaved "[SAE:n]" into the middle of command
     * responses, making the management interface unreliable. Read the last
     * step with AT+SAESTAGE? -- it survives the panic reset, which was the
     * point. Re-enable the print only while chasing a hang that kills USB
     * before AT can be reached. */
    g_warthog_sae_stage = 0x5AE00000u | n;
}

volatile uint8_t g_warthog_hwmp_dump[48];
volatile uint16_t g_warthog_hwmp_dump_len = 0, g_warthog_hwmp_dump_full = 0;
volatile uint8_t g_warthog_rxdata_head[64]; volatile uint16_t g_warthog_rxdata_head_len = 0;
volatile uint32_t g_warthog_ccmp_last_keyid = 0, g_warthog_ccmp_blank = 0, g_warthog_ccmp_last_pn = 0, g_warthog_ccmp_replay = 0;             /* TX Mesh Control sequence number */
volatile uint32_t g_warthog_rxframe_entry = 0, g_warthog_filter_entry = 0;
volatile uint32_t g_warthog_rxchan_pages = 0, g_warthog_rxchan_data = 0, g_warthog_rxchan_beacon = 0,
    g_warthog_rxchan_mgmt = 0, g_warthog_rxchan_cmd = 0, g_warthog_rxchan_txstat = 0, g_warthog_rxchan_last = 0;

static void cmd_meshstat(void)
{
#ifdef WARTHOG_MESH_RX_TAP
    char buf[160];
    snprintf(buf, sizeof(buf),
             "+MESHSTAT: rx=%lu mgmt=%lu beacon=%lu last_fc=0x%04x "
             "last_ta=%02x:%02x:%02x:%02x:%02x:%02x\r\n",
             (unsigned long)g_warthog_rxtap_total, (unsigned long)g_warthog_rxtap_mgmt,
             (unsigned long)g_warthog_rxtap_beacon, (unsigned)g_warthog_rxtap_last_fc,
             g_warthog_rxtap_last_ta[0], g_warthog_rxtap_last_ta[1],
             g_warthog_rxtap_last_ta[2], g_warthog_rxtap_last_ta[3],
             g_warthog_rxtap_last_ta[4], g_warthog_rxtap_last_ta[5]);
    cdc_write(buf);
    reply_ok();
#else
    reply_error("built without WARTHOG_MESH_RX_TAP");
#endif
}

/* AT+BCNSTAT? — beacon-handshake counters.
 *
 * req      = times the chip asked the host for a beacon template
 * served   = times the host handed back a real beacon
 * null     = mesh active but the beacon build returned NULL
 * inactive = chip asked while mesh beaconing was not active
 *
 * The documented symptom is "the beacon IRQ fires once at startup and never
 * again". req==1 confirms the chip stopped asking; req climbing with served==0
 * means the host is failing to build; req climbing with served climbing means
 * beacons ARE being served and the problem is downstream of this handshake. */
static void cmd_bcnstat(void)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "+BCNSTAT: req=%lu served=%lu null=%lu inactive=%lu enq=%lu/%lu txcomp=%lu\r\n",
             (unsigned long)g_warthog_bcn_req, (unsigned long)g_warthog_bcn_served,
             (unsigned long)g_warthog_bcn_null, (unsigned long)g_warthog_bcn_inactive,
             (unsigned long)g_warthog_bcn_enq_ok, (unsigned long)g_warthog_bcn_enq_err,
             (unsigned long)g_warthog_bcn_txcomp);
    cdc_write(buf);
    reply_ok();
}

/* AT+PRSPSTAT? — mesh probe-request/response counters. */
static void cmd_prspstat(void)
{
    char buf[144];
    snprintf(buf, sizeof(buf),
             "+PRSPSTAT: req_rx=%lu rsp_tx=%lu rsp_fail=%lu last_da=%02x:%02x:%02x:%02x:%02x:%02x"
             " prq_named=%lu prq_offer=%lu\r\n",
             (unsigned long)g_warthog_prsp_rx, (unsigned long)g_warthog_prsp_tx,
             (unsigned long)g_warthog_prsp_fail,
             g_warthog_prsp_last_da[0], g_warthog_prsp_last_da[1], g_warthog_prsp_last_da[2],
             g_warthog_prsp_last_da[3], g_warthog_prsp_last_da[4], g_warthog_prsp_last_da[5],
             (unsigned long)g_warthog_prq_named, (unsigned long)g_warthog_prq_offer);
    cdc_write(buf);
    reply_ok();
}

/* AT+MPMSTAT? — mesh peering handshake counters. */
static void cmd_mpmstat(void)
{
    char buf[240];
    snprintf(buf, sizeof(buf),
             "+MPMSTAT: rx=%lu open_tx=%lu conf_tx=%lu conf_rx=%lu close_rx=%lu "
             "parse_fail=%lu llid=%lu plid=%lu close_reason=%lu estab=%lu\r\n",
             (unsigned long)g_warthog_mpm_rx, (unsigned long)g_warthog_mpm_open_tx,
             (unsigned long)g_warthog_mpm_conf_tx, (unsigned long)g_warthog_mpm_conf_rx,
             (unsigned long)g_warthog_mpm_close_rx, (unsigned long)g_warthog_mpm_parse_fail,
             (unsigned long)g_warthog_mpm_our_llid, (unsigned long)g_warthog_mpm_our_plid,
             (unsigned long)g_warthog_mpm_close_reason, (unsigned long)g_warthog_mpm_estab);
    cdc_write(buf);
    reply_ok();
}

/* AT+MPMDUMP? — hexdump of the last Mesh Peering Confirm body we transmitted.
 * Diff this against a Linux mesh_plink_frame_tx() Confirm to find the field
 * mac80211 is rejecting. Body starts at the action category byte (0x0f). */
static void cmd_mpmdump(void)
{
    char line[3 * 32 + 2];
    uint16_t len = g_warthog_mpm_dump_len;
    if (len > sizeof(g_warthog_mpm_dump)) {
        len = (uint16_t)sizeof(g_warthog_mpm_dump);
    }
    char hdr[72];
    /* len = the real body length, captured = what fits in the buffer. If they
     * differ the hex below is truncated -- which matters, because this command
     * exists to be diffed byte-for-byte against a mac80211 Confirm. */
    snprintf(hdr, sizeof(hdr), "+MPMDUMP: len=%u captured=%u\r\n",
             (unsigned)g_warthog_mpm_dump_full, (unsigned)len);
    cdc_write(hdr);
    for (uint16_t off = 0; off < len; off += 16) {
        int w = 0;
        for (uint16_t i = off; i < len && i < off + 16; i++) {
            w += snprintf(line + w, sizeof(line) - w, "%02x ", g_warthog_mpm_dump[i]);
        }
        snprintf(line + w, sizeof(line) - w, "\r\n");
        cdc_write(line);
    }
    reply_ok();
}

/* AT+MPING=<ipv4>[,<count>] -- ICMP echo over the HaLow netif.
 *
 * The end-to-end proof for the mesh data plane: a reply means an IP packet
 * left this board as a 4-address mesh data frame, was carried by the vendor
 * datapath, decoded on the peer, answered by its lwIP, and came back the same
 * way. Blocks for up to count x 1 s; runs on the AT task, which is fine for a
 * diagnostic. Reports per-reply RTT and a summary. */
typedef struct {
    SemaphoreHandle_t done;
    uint32_t sent, recv;
    uint32_t last_rtt_ms;
    char line[96];
} mping_ctx_t;

static void mping_on_success(esp_ping_handle_t h, void *args)
{
    mping_ctx_t *c = (mping_ctx_t *)args;
    uint32_t rtt = 0; uint16_t seq = 0; ip_addr_t target;
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &rtt, sizeof(rtt));
    esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    esp_ping_get_profile(h, ESP_PING_PROF_IPADDR, &target, sizeof(target));
    c->recv++; c->last_rtt_ms = rtt;
    snprintf(c->line, sizeof(c->line), "+MPING: reply from %s seq=%u time=%lums\r\n",
             ipaddr_ntoa(&target), (unsigned)seq, (unsigned long)rtt);
    cdc_write(c->line);
}

static void mping_on_timeout(esp_ping_handle_t h, void *args)
{
    mping_ctx_t *c = (mping_ctx_t *)args;
    uint16_t seq = 0;
    esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    snprintf(c->line, sizeof(c->line), "+MPING: seq=%u timeout\r\n", (unsigned)seq);
    cdc_write(c->line);
}

static void mping_on_end(esp_ping_handle_t h, void *args)
{
    mping_ctx_t *c = (mping_ctx_t *)args;
    esp_ping_get_profile(h, ESP_PING_PROF_REQUEST, &c->sent, sizeof(c->sent));
    esp_ping_get_profile(h, ESP_PING_PROF_REPLY, &c->recv, sizeof(c->recv));
    xSemaphoreGive(c->done);
}

static void cmd_mping(char *args)
{
    char *comma = strchr(args, ',');
    int count = 4;
    if (comma) { *comma = '\0'; count = atoi(trim(comma + 1)); }
    if (count < 1 || count > 20) count = 4;
    char *ip_s = trim(args);

    ip_addr_t target;
    if (!ipaddr_aton(ip_s, &target)) {
        reply_error("usage: AT+MPING=<ipv4>[,<count 1-20>]");
        return;
    }

    static mping_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.done = xSemaphoreCreateBinary();

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = (uint32_t)count;
    cfg.interval_ms = 1000;
    cfg.timeout_ms = 1000;
    cfg.data_size = 32;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = mping_on_success,
        .on_ping_timeout = mping_on_timeout,
        .on_ping_end = mping_on_end,
        .cb_args = &ctx,
    };
    esp_ping_handle_t h;
    if (esp_ping_new_session(&cfg, &cbs, &h) != ESP_OK) {
        vSemaphoreDelete(ctx.done);
        reply_error("ping session");
        return;
    }
    esp_ping_start(h);
    xSemaphoreTake(ctx.done, pdMS_TO_TICKS((count + 2) * 1100));
    esp_ping_delete_session(h);
    vSemaphoreDelete(ctx.done);

    char buf[96];
    snprintf(buf, sizeof(buf), "+MPING: %lu sent, %lu received, %lu%% loss\r\n",
             (unsigned long)ctx.sent, (unsigned long)ctx.recv,
             ctx.sent ? (unsigned long)(100 * (ctx.sent - ctx.recv) / ctx.sent) : 100UL);
    cdc_write(buf);
    reply_ok();
}

/* AT+PEERS? -- mesh data-plane peer table. */
static void cmd_peers(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "+PEERS: count=%u add_fail=%lu chip_sta_fail=%lu key_fail=%lu\r\n",
             (unsigned)mmwlan_mesh_get_peer_count(), (unsigned long)g_warthog_mesh_peer_add_fail,
             (unsigned long)g_warthog_mesh_chip_sta_fail, (unsigned long)g_warthog_mesh_key_fail);
    cdc_write(buf);
    reply_ok();
}

/* AT+DATASTAT? -- where does a data frame get to on each side? */
static void cmd_datastat(void)
{
    char buf[420];
    snprintf(buf, sizeof(buf),
             "+DATASTAT: rx_data=%lu stad_hit=%lu stad_miss=%lu miss_ta=%02x:%02x:%02x:%02x:%02x:%02x "
             "delivered=%lu | tx_enq=%lu tx_deq=%lu tx_hdr=%lu drv_ok=%lu drv_err=%lu last_err=%ld | txst=%lu acked=%lu noack=%lu unsent=%lu flags=0x%02lx | bdup=%lu bfail=%lu prot=%lu nokey=%lu key=%lu | DATA txst=%lu acked=%lu noack=%lu unsent=%lu flags=0x%02lx\r\n",
             (unsigned long)g_warthog_rxtap_data, (unsigned long)g_warthog_rx_data_stad_hit,
             (unsigned long)g_warthog_rx_data_stad_miss,
             g_warthog_rx_data_miss_ta[0], g_warthog_rx_data_miss_ta[1], g_warthog_rx_data_miss_ta[2],
             g_warthog_rx_data_miss_ta[3], g_warthog_rx_data_miss_ta[4], g_warthog_rx_data_miss_ta[5],
             (unsigned long)g_warthog_rx_data_delivered, (unsigned long)g_warthog_tx_data_enq,
             (unsigned long)g_warthog_tx_data_deq, (unsigned long)g_warthog_tx_data_hdr,
             (unsigned long)g_warthog_tx_drv_ok, (unsigned long)g_warthog_tx_drv_err,
             (long)g_warthog_tx_drv_last_err, (unsigned long)g_warthog_txst_total,
             (unsigned long)g_warthog_txst_acked, (unsigned long)g_warthog_txst_noack,
             (unsigned long)g_warthog_txst_unsent, (unsigned long)g_warthog_txst_last_flags,
             /* Order matters and was wrong: the format reads
              * "bdup bfail prot nokey key" but these were passed as
              * protected, nokey, bcast_dup, bcast_copy_fail -- so the number
              * printed under prot= was actually the broadcast duplicate count,
              * and vice versa. Anyone reading prot= to decide whether the data
              * plane was encrypted was reading the wrong counter. */
             (unsigned long)g_warthog_tx_bcast_dup, (unsigned long)g_warthog_tx_bcast_copy_fail,
             (unsigned long)g_warthog_tx_protected, (unsigned long)g_warthog_tx_nokey,
             (unsigned long)g_warthog_tx_last_key, (unsigned long)g_warthog_txst_data_total,
             (unsigned long)g_warthog_txst_data_acked, (unsigned long)g_warthog_txst_data_noack,
             (unsigned long)g_warthog_txst_data_unsent, (unsigned long)g_warthog_txst_data_last_flags);
    cdc_write(buf);
    reply_ok();
}

/* AT+RXCHAN? -- pages the chip pushed to the host, by channel. data(0x00)=0
 * while a peer is verifiably sending us ACKed data frames means the CHIP is
 * not delivering them; data>0 with rx_data=0 in DATASTAT means the host is. */
static void cmd_hwmpdump(void)
{
    char hdr[64];
    uint16_t len = g_warthog_hwmp_dump_len;
    if (len > sizeof(g_warthog_hwmp_dump)) { len = (uint16_t)sizeof(g_warthog_hwmp_dump); }
    snprintf(hdr, sizeof(hdr), "+HWMPDUMP: len=%u captured=%u\r\n",
             (unsigned)g_warthog_hwmp_dump_full, (unsigned)len);
    cdc_write(hdr);
    char line[3 * 16 + 4];
    for (uint16_t off = 0; off < len; off += 16) {
        int w = 0;
        for (uint16_t i = off; i < len && i < off + 16; i++) {
            w += snprintf(line + w, sizeof(line) - w, "%02x ", g_warthog_hwmp_dump[i]);
        }
        snprintf(line + w, sizeof(line) - w, "\r\n");
        cdc_write(line);
    }
    reply_ok();
}
static void cmd_hwmpstat(void)
{
    char buf[190];
    snprintf(buf, sizeof(buf),
             "+HWMPSTAT: rx=%lu preq_rx=%lu preq_tx=%lu prep_rx=%lu prep_tx=%lu "
             "parse_fail=%lu not_ours=%lu\r\n",
             (unsigned long)g_warthog_hwmp_rx, (unsigned long)g_warthog_hwmp_preq_rx,
             (unsigned long)g_warthog_hwmp_preq_tx, (unsigned long)g_warthog_hwmp_prep_rx,
             (unsigned long)g_warthog_hwmp_prep_tx,
             (unsigned long)g_warthog_hwmp_parse_fail, (unsigned long)g_warthog_hwmp_not_ours);
    cdc_write(buf);
    reply_ok();
}
static void cmd_filtstat(void)
{
    char buf[220];
    snprintf(buf, sizeof(buf),
             "+FILTSTAT: drop=%lu last=%lu | short_fc=%lu rts=%lu beacon=%lu short_hdr=%lu "
             "no_ops=%lu unknown_sender=%lu sa_is_us=%lu dup=%lu\r\n",
             (unsigned long)g_warthog_filt_drop, (unsigned long)g_warthog_filt_reason,
             (unsigned long)g_warthog_filt_hist[1], (unsigned long)g_warthog_filt_hist[2],
             (unsigned long)g_warthog_filt_hist[3], (unsigned long)g_warthog_filt_hist[4],
             (unsigned long)g_warthog_filt_hist[5], (unsigned long)g_warthog_filt_hist[6],
             (unsigned long)g_warthog_filt_hist[7], (unsigned long)g_warthog_filt_hist[8]);
    cdc_write(buf);
    reply_ok();
}
static void cmd_rxreord(void)
{
    char buf[200];
    snprintf(buf, sizeof(buf),
             "+RXREORD: outdated=%lu buffered=%lu released=%lu bypass=%lu "
             "last(seq=0x%04lx exp=0x%04lx)\r\n",
             (unsigned long)g_warthog_reord_outdated, (unsigned long)g_warthog_reord_buffered,
             (unsigned long)g_warthog_reord_released, (unsigned long)g_warthog_reord_bypass,
             (unsigned long)g_warthog_reord_last_seq, (unsigned long)g_warthog_reord_last_exp);
    cdc_write(buf);
    reply_ok();
}
static void cmd_rxchan(void)
{
    char buf[460];
    snprintf(buf, sizeof(buf),
             "+RXCHAN: pages=%lu data=%lu beacon=%lu mgmt=%lu cmd=%lu txstat=%lu last=0x%02lx "
             "| shim=%lu notrunning=%lu rxframe=%lu filter=%lu meshctrl=%lu | rxdrop=%lu reason=%lu "
             "ccmp_key=%lu blank=%lu replay=%lu pn=%lu | nodec grp=%lu uni=%lu "
             "last(grp=%lu fc=%04lx key=%lu ta=%02x%02x%02x)\r\n",
             (unsigned long)g_warthog_rxchan_pages, (unsigned long)g_warthog_rxchan_data,
             (unsigned long)g_warthog_rxchan_beacon, (unsigned long)g_warthog_rxchan_mgmt,
             (unsigned long)g_warthog_rxchan_cmd, (unsigned long)g_warthog_rxchan_txstat,
             (unsigned long)g_warthog_rxchan_last, (unsigned long)g_warthog_shim_rx,
             (unsigned long)g_warthog_shim_rx_notrunning, (unsigned long)g_warthog_rxframe_entry,
             (unsigned long)g_warthog_filter_entry, (unsigned long)g_warthog_rx_meshctrl_stripped,
             (unsigned long)g_warthog_rxdrop_count, (unsigned long)g_warthog_rxdrop_reason,
             (unsigned long)g_warthog_ccmp_last_keyid, (unsigned long)g_warthog_ccmp_blank,
             (unsigned long)g_warthog_ccmp_replay, (unsigned long)g_warthog_ccmp_last_pn,
             (unsigned long)g_warthog_nodec_group_n, (unsigned long)g_warthog_nodec_uni_n,
             (unsigned long)g_warthog_nodec_group, (unsigned long)g_warthog_nodec_fc,
             (unsigned long)g_warthog_nodec_keyid, g_warthog_nodec_ta[3], g_warthog_nodec_ta[4],
             g_warthog_nodec_ta[5]);
    cdc_write(buf);
    reply_ok();
}

/* AT+FCRING? -- frame control of the last 32 frames the chip delivered, oldest
 * first. Answers "what IS arriving" when the type counters say "not data". */
static void cmd_fcring(void)
{
#ifdef WARTHOG_MESH_RX_TAP
    char buf[200]; int w = 0;
    w += snprintf(buf + w, sizeof(buf) - w, "+FCRING: n=%lu ", (unsigned long)g_warthog_fc_ring_idx);
    uint32_t n = g_warthog_fc_ring_idx < 32 ? g_warthog_fc_ring_idx : 32;
    uint32_t start = g_warthog_fc_ring_idx >= 32 ? g_warthog_fc_ring_idx - 32 : 0;
    for (uint32_t i = 0; i < n && w < (int)sizeof(buf) - 8; i++) {
        w += snprintf(buf + w, sizeof(buf) - w, "%04x ", g_warthog_fc_ring[(start + i) & 31]);
    }
    snprintf(buf + w, sizeof(buf) - w, "\r\n");
    cdc_write(buf);
    reply_ok();
#else
    /* The buffers this reports are only written inside the RX tap. Without
     * it they are permanently empty, and reporting len=0 reads as "nothing is
     * arriving" rather than "this build does not record it". */
    reply_error("built without WARTHOG_MESH_RX_TAP");
#endif
}

/* AT+RXHEAD? -- first bytes of the last DATA frame the chip delivered. */
static void cmd_rxhead(void)
{
#ifdef WARTHOG_MESH_RX_TAP
    char buf[240]; int w = 0;
    w += snprintf(buf + w, sizeof(buf) - w, "+RXHEAD: len=%u ", (unsigned)g_warthog_rxdata_head_len);
    for (uint16_t i = 0; i < g_warthog_rxdata_head_len && w < (int)sizeof(buf) - 6; i++)
        w += snprintf(buf + w, sizeof(buf) - w, "%02x", g_warthog_rxdata_head[i]);
    snprintf(buf + w, sizeof(buf) - w, "\r\n");
    cdc_write(buf);
    reply_ok();
#else
    /* The buffers this reports are only written inside the RX tap. Without
     * it they are permanently empty, and reporting len=0 reads as "nothing is
     * arriving" rather than "this build does not record it". */
    reply_error("built without WARTHOG_MESH_RX_TAP");
#endif
}

/* ---- UDP multicast over the mesh: the Meshtastic transport gate ------------
 *
 * Meshtastic's UdpMulticastHandler needs exactly one thing from the network:
 * working IPv4 UDP multicast to 239.0.0.69:4403. These three commands prove
 * that shape crosses the HaLow mesh, using the same socket calls AsyncUDP
 * makes underneath (bind 0.0.0.0:4403, IP_ADD_MEMBERSHIP, sendto the group).
 *   AT+MCAST=1     join the group on the HaLow netif and start a receiver task
 *   AT+MCAST=0     leave
 *   AT+MCAST?      counters + last datagram
 *   AT+MSEND=<txt> send one datagram to 239.0.0.69:4403 (source port 4403,
 *                  like AsyncUDP)
 * No multicast loopback in this lwIP build, so the sender never sees its own
 * datagram -- always read the FAR board's counters. */
#define MC_GROUP "239.0.0.69"
#define MC_PORT  4403
static int s_mc_sock = -1;
static volatile bool s_mc_run;
static volatile uint32_t g_mc_rx, g_mc_rx_bytes, g_mc_tx, g_mc_tx_err;
static char s_mc_last[40], s_mc_last_from[24];

static void mcast_rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[512];
    while (s_mc_run) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(s_mc_sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) continue;
            break;
        }
        g_mc_rx++; g_mc_rx_bytes += (uint32_t)n;
        snprintf(s_mc_last_from, sizeof(s_mc_last_from), "%s:%u", inet_ntoa(from.sin_addr),
                 (unsigned)ntohs(from.sin_port));
        int k = n < 32 ? n : 32; memcpy(s_mc_last, buf, k); s_mc_last[k] = 0;
        /* Announce the first few and then go quiet. A line per datagram over
         * USB CDC cannot keep up with the link: during a throughput run the
         * receive queue overflowed behind the print and the loss being
         * measured was this logging, not the radio. Counters are exact either
         * way -- read AT+MCAST?. */
        if (g_mc_rx <= 3 || (g_mc_rx % 500) == 0)
        {
            char line[110];
            snprintf(line, sizeof(line), "+MCAST: rx %d bytes from %s (#%lu)\r\n", n,
                     s_mc_last_from, (unsigned long)g_mc_rx);
            cdc_write(line);
        }
    }
    close(s_mc_sock);   /* also drops the IGMP membership */
    s_mc_sock = -1;
    vTaskDelete(NULL);
}

static void cmd_mcast_set(char *args)
{
    /* atoi("on") is 0, so every word argument used to take the STOP path and
     * answer OK, leaving the operator believing the receiver was up. */
    {
        char *ma = trim(args);
        if ((ma[0] != '0' && ma[0] != '1') || ma[1] != '\0') {
            reply_error("usage: AT+MCAST=<0|1>");
            return;
        }
    }
    if (!atoi(trim(args))) { s_mc_run = false; reply_ok(); return; }
    if (s_mc_sock >= 0) { reply_error("already running"); return; }

    /* Must join AFTER the mesh netif has its 10.77.x.y address, which only
     * happens on first ESTAB; an IGMP join pinned to an address no netif owns
     * fails. */
    esp_netif_t *halow = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    if (!halow || esp_netif_get_ip_info(halow, &ip) != ESP_OK || ip.ip.addr == 0) {
        reply_error("halow netif not up (no ESTAB yet)"); return;
    }
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) { reply_error("socket"); return; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int rcvbuf = 16384; /* absorb a burst while the reader task is scheduled */
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(MC_PORT),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { close(s); reply_error("bind"); return; }
    struct ip_mreq m = { .imr_multiaddr.s_addr = inet_addr(MC_GROUP),
                         .imr_interface.s_addr = ip.ip.addr };
    if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) < 0) {
        close(s); reply_error("IP_ADD_MEMBERSHIP"); return;
    }
    struct in_addr ifa = { .s_addr = ip.ip.addr };
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof(ifa));
    uint8_t ttl = 64;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, 1);

    s_mc_sock = s; s_mc_run = true;
    g_mc_rx = g_mc_rx_bytes = g_mc_tx = g_mc_tx_err = 0;
    s_mc_last[0] = 0; s_mc_last_from[0] = 0;
    xTaskCreatePinnedToCore(mcast_rx_task, "mcast_rx", 4096, NULL, tskIDLE_PRIORITY + 2, NULL, 0);
    char buf[96];
    snprintf(buf, sizeof(buf), "+MCAST: joined %s:%u on " IPSTR "\r\n", MC_GROUP, MC_PORT, IP2STR(&ip.ip));
    cdc_write(buf);
    reply_ok();
}

static void cmd_mcast_query(void)
{
    char buf[200];
    snprintf(buf, sizeof(buf),
             "+MCAST: run=%d rx=%lu bytes=%lu tx=%lu tx_err=%lu last_from=%s last='%s'\r\n",
             s_mc_sock >= 0, (unsigned long)g_mc_rx, (unsigned long)g_mc_rx_bytes,
             (unsigned long)g_mc_tx, (unsigned long)g_mc_tx_err, s_mc_last_from, s_mc_last);
    cdc_write(buf);
    reply_ok();
}

static void cmd_msend(char *args)
{
    if (s_mc_sock < 0) { reply_error("AT+MCAST=1 first"); return; }
    struct sockaddr_in d = { .sin_family = AF_INET, .sin_port = htons(MC_PORT),
                             .sin_addr.s_addr = inet_addr(MC_GROUP) };
    int n = sendto(s_mc_sock, args, strlen(args), 0, (struct sockaddr *)&d, sizeof(d));
    if (n < 0) { g_mc_tx_err++; reply_error("sendto"); return; }
    g_mc_tx++;
    char buf[64];
    snprintf(buf, sizeof(buf), "+MSEND: %d bytes -> %s:%u\r\n", n, MC_GROUP, MC_PORT);
    cdc_write(buf);
    reply_ok();
}

/* AT+MUDP? -- multicast repeater counters (the resident 239.0.0.69:4403 bridge). */
static void cmd_mudp(void)
{
    char buf[260];
    warthog_mudp_status(buf, sizeof(buf));
    cdc_write(buf);
    reply_ok();
}

/* AT+MINJECT=<hex> -- inject raw bytes as an AP-side datagram (a MeshPacket
 * from a node on the softAP) and relay them across the mesh. */
static void cmd_minject(char *args)
{
    char *h = trim(args); size_t hl = strlen(h);
    if (hl == 0 || (hl & 1) || hl > 2 * 300) { reply_error("usage: AT+MINJECT=<hex, <=300 bytes>"); return; }
    uint8_t bin[300]; size_t n = 0;
    for (size_t i = 0; i < hl; i += 2) {
        unsigned v; if (sscanf(h + i, "%2x", &v) != 1) { reply_error("bad hex"); return; }
        bin[n++] = (uint8_t)v;
    }
    int sent = warthog_mudp_inject_from_ap(bin, n);
    char buf[64]; snprintf(buf, sizeof(buf), "+MINJECT: %u bytes -> %d netif(s)\r\n", (unsigned)n, sent);
    cdc_write(buf); reply_ok();
}

static void cmd_mudplast(void)
{
    static char buf[600];
    warthog_mudp_last_halow(buf, sizeof(buf));
    cdc_write(buf);
    reply_ok();
}

/* AT+MPMPEERS? -- one line per MPM link (addr, our llid, their plid, estab).
 * The aggregate AT+MPMSTAT? cannot show which neighbour a link id belongs to,
 * which is exactly what goes wrong with three or more boards on air. */
/* AT+KEYFP? -- per-peer pairwise-key fingerprint. For a per-link key the two
 * ends of one link MUST print the same value; a mismatch means the derivation
 * is not symmetric and only the sender can decrypt. */
/* AT+REKEY=<n> -- re-install peer n's pairwise key only. See
 * umac_datapath_mesh_rekey_peer(): this is the one-slot-vs-per-station probe. */
/* AT+CRYPTOHOST=<0|1> ask the chip to stop decrypting in firmware (the
 * prerequisite for host software CCMP and therefore for per-link keys);
 * AT+CRYPTOHOST? read back what it holds. Serviced from the probe path within
 * ~2 s, like AT+REKEY. rc is the driver return; val is what the chip reports,
 * which is the only evidence the setting actually took. */
/* AT+CCMPKAT? -- did AES-CCM pass its known-answer test on THIS chip, through
 * the mbedtls path the firmware actually uses, and what does it cost?
 * ok=1 means the vector, the in-place case and MIC rejection all passed.
 * us_frame is one 256-byte CCMP encrypt; ns_block is the implied per-AES-block
 * cost, which is what decides whether host software CCMP is affordable. */
static void cmd_ccmpkat(void)
{
    char buf[260];
    snprintf(buf, sizeof(buf),
             "+CCMPKAT: ran=%lu ok=%lu fail_stage=%lu us_frame=%lu ns_block=%lu mbedtls_us=%lu setup_ns=%lu ecb_ns=%lu ctr256_us=%lu bulk_ccm_us=%lu\r\n",
             (unsigned long)g_warthog_ccmp_kat_ran, (unsigned long)g_warthog_ccmp_kat_ok,
             (unsigned long)g_warthog_ccmp_kat_fail_stage,
             (unsigned long)g_warthog_ccmp_us_per_frame,
             (unsigned long)g_warthog_aes_ns_per_block,
             (unsigned long)g_warthog_mbedtls_us_per_frame, (unsigned long)g_warthog_aes_setup_ns,
             (unsigned long)g_warthog_aes_ecb_ns, (unsigned long)g_warthog_aes_ctr_us,
             (unsigned long)g_warthog_bulk_ccm_us);
    cdc_write(buf);
    reply_ok();
}

static void cmd_cryptohost_set(char *args)
{
    /* atoi("on") is 0, so every word argument used to queue the DISABLE
     * request and then report success. Require exactly 0 or 1. */
    {
        char *a = trim(args);
        if ((a[0] != '0' && a[0] != '1') || a[1] != '\0')
        {
            reply_error("usage: AT+CRYPTOHOST=<0|1>");
            return;
        }
        g_warthog_cryptohost_req = (a[0] == '1') ? 1u : 2u;
    }
    cdc_write("+CRYPTOHOST: queued (serviced within ~2s)\r\n");
    reply_ok();
}

static void cmd_cryptohost_query(void)
{
    if (g_warthog_cryptohost_done == 0) { g_warthog_cryptohost_req = 3u; }
    char buf[110];
    snprintf(buf, sizeof(buf), "+CRYPTOHOST: done=%lu rc=%ld value=0x%08lx\r\n",
             (unsigned long)g_warthog_cryptohost_done, (long)(int32_t)g_warthog_cryptohost_rc,
             (unsigned long)g_warthog_cryptohost_val);
    cdc_write(buf);
    reply_ok();
}

static void cmd_meshsec(char *args)
{
    char *a = trim(args);
    /* Whole argument, not just its first character: "10" used to read as keyed
     * and "0x1" as open, both silently. A setting that decides whether the
     * data plane can talk to a given peer should not be guessable from a
     * typo. */
    if ((a[0] != '0' && a[0] != '1') || a[1] != '\0')
    {
        reply_error("usage: AT+MESHSEC=<0 open|1 keyed>");
        return;
    }
    g_warthog_mesh_secure = (a[0] == '1') ? 1u : 0u;
    /* Persist. Forgetting this across a reboot brings the node back peering
     * perfectly and carrying no data against an unencrypted peer, with nothing
     * in any log to explain it. */
    if (warthog_cfg_set_mesh_secure((uint8_t)g_warthog_mesh_secure) != ESP_OK)
    {
        reply_error("nvs write");
        return;
    }
    /* Re-peer, or the change only applies to peers added from here on. Restart
     * the peer's mesh too: a node holding an ESTAB link ignores a new Open. */
    g_warthog_mesh_repeer_req = 1;
    char buf[96];
    snprintf(buf, sizeof(buf), "+MESHSEC: %s, stored, re-peering (~2s)\r\n",
             g_warthog_mesh_secure ? "keyed" : "open");
    cdc_write(buf);
    reply_ok();
}
/* AT+COREDUMP? -- read back the last panic from the flash coredump partition.
 *
 * This exists because a live panic is unreadable on this board: the ESP32-S3
 * has one USB PHY, and once TinyUSB takes it for CDC/NCM the USB-Serial-JTAG
 * console the panic handler prints to is disconnected. Flash survives the
 * power cycle a wedged USB needs; RTC memory does not.
 * Prints the faulting task, PC, and raw backtrace for addr2line.
 */
static void cmd_coredump(void)
{
#if !CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    cdc_write("+COREDUMP: disabled in this build\r\n");
    reply_ok();
#else
    char buf[160];
    esp_core_dump_summary_t *sum = calloc(1, sizeof(*sum));
    if (sum == NULL)
    {
        cdc_write("+ERR: no mem\r\nERROR\r\n");
        return;
    }
    esp_err_t err = esp_core_dump_get_summary(sum);
    if (err != ESP_OK)
    {
        snprintf(buf, sizeof(buf), "+COREDUMP: none (err=0x%x)\r\n", err);
        cdc_write(buf);
        free(sum);
        reply_ok();
        return;
    }
    snprintf(buf, sizeof(buf), "+COREDUMP: task=%s pc=0x%08lx\r\n",
             sum->exc_task, (unsigned long)sum->exc_pc);
    cdc_write(buf);
    int n = sum->exc_bt_info.depth;
    if (n > 16) { n = 16; }
    for (int i = 0; i < n; i++)
    {
        snprintf(buf, sizeof(buf), "+COREDUMP: bt%d=0x%08lx%s\r\n", i,
                 (unsigned long)sum->exc_bt_info.bt[i],
                 sum->exc_bt_info.corrupted ? " (corrupt)" : "");
        cdc_write(buf);
    }
    free(sum);
    reply_ok();
#endif
}

static void cmd_saestage(void)
{
    char buf[64];
    uint32_t v = g_warthog_sae_stage;
    if ((v & 0xFFFF0000u) != 0x5AE00000u)
    {
        cdc_write("+SAESTAGE: (unset)\r\n");
    }
    else
    {
        snprintf(buf, sizeof(buf), "+SAESTAGE: %lu\r\n", (unsigned long)(v & 0xFFFFu));
        cdc_write(buf);
    }
    reply_ok();
}

static void cmd_meshsec_q(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "+MESHSEC: %lu (%s)\r\n", (unsigned long)g_warthog_mesh_secure,
             g_warthog_mesh_secure ? "keyed" : "open");
    cdc_write(buf);
    reply_ok();
}
static void cmd_rekey(char *args)
{
    /* atoi() of a non-numeric string is 0, which used to become peer slot 0 --
     * so "AT+REKEY=oops" silently rekeyed the first peer. Require digits. */
    char *a = trim(args);
    if (a[0] == '\0')
    {
        reply_error("usage: AT+REKEY=<peer slot>");
        return;
    }
    for (const char *c = a; *c != '\0'; c++)
    {
        if (*c < '0' || *c > '9')
        {
            reply_error("usage: AT+REKEY=<peer slot>");
            return;
        }
    }
    g_warthog_rekey_req = (uint32_t)atoi(a) + 1u;
    char buf[80];
    snprintf(buf, sizeof(buf), "+REKEY: queued peer=%s (serviced within ~2s)\r\n", trim(args));
    cdc_write(buf);
    reply_ok();
}

static void cmd_rekeystat(void)
{
    char buf[80];
    snprintf(buf, sizeof(buf), "+REKEYSTAT: done=%lu aid=%lu pending=%lu\r\n",
             (unsigned long)g_warthog_rekey_done, (unsigned long)g_warthog_rekey_aid,
             (unsigned long)g_warthog_rekey_req);
    cdc_write(buf);
    reply_ok();
}

static void cmd_keyfp(void)
{
    char buf[220];
    int w = snprintf(buf, sizeof(buf), "+KEYFP: self=%02x%02x%02x ",
                     g_warthog_mesh_self_addr[3], g_warthog_mesh_self_addr[4],
                     g_warthog_mesh_self_addr[5]);
    for (int i = 0; i < 4 && w < (int)sizeof(buf) - 32; i++) {
        if (g_warthog_peer_mac[i] == 0) continue;
        w += snprintf(buf + w, sizeof(buf) - w, "[%06lx fp=%08lx] ",
                      (unsigned long)g_warthog_peer_mac[i], (unsigned long)g_warthog_peer_fp[i]);
    }
    snprintf(buf + w, sizeof(buf) - w, "\r\n");
    cdc_write(buf);
    reply_ok();
}

static void cmd_mpmpeers(void)
{
    char buf[400];
    snprintf(buf, sizeof(buf), "+MPMPEERS: self=%02x%02x%02x %ssae=%lu offers=%lu pfail=%lu mlme=%lu auth_tx=%lu addp=%lu/%lu authsta=%lu/%lu rates=%lu apx=%lu/%lu rej=c%lu/e%lu/a%lu/r%lu stadd=%lu/%lu rxauth=%lu/%lu/%lu ampe_mtk=%lu ampe_mgtk=%lu no_slot=%lu expired=%lu bcn_peer=%lu close_tx=%lu s1g_bcn=%lu/%lu new=%lu retry=%lu sa=%02x%02x%02x\r\n",
             g_warthog_mesh_self_addr[3], g_warthog_mesh_self_addr[4], g_warthog_mesh_self_addr[5],
             (const char *)g_warthog_mpm_links,
             (unsigned long)g_warthog_sae_init,
             (unsigned long)g_warthog_sae_peer_offers,
             (unsigned long)g_warthog_sae_peer_parse_fail,
             (unsigned long)g_warthog_sae_mlme_tx,
             (unsigned long)g_warthog_sae_mlme_auth_tx,
             (unsigned long)g_warthog_sae_addpeer_ok,
             (unsigned long)g_warthog_sae_addpeer_null,
             (unsigned long)g_warthog_sae_authsta_ok,
             (unsigned long)g_warthog_sae_authsta_fail,
             (unsigned long)g_warthog_sae_rates_synth,
             (unsigned long)g_warthog_sae_peer_authproto,
             (unsigned long)g_warthog_sae_peer_authval,
             (unsigned long)g_warthog_addp_r_crowded,
             (unsigned long)g_warthog_addp_r_exists,
             (unsigned long)g_warthog_addp_r_addfail,
             (unsigned long)g_warthog_addp_r_rates,
             (unsigned long)g_warthog_sae_sta_add_ok,
             (unsigned long)g_warthog_sae_sta_add_fail,
             (unsigned long)g_warthog_rx_auth,
             (unsigned long)g_warthog_rx_auth_router,
             (unsigned long)g_warthog_rx_auth_other,
             (unsigned long)g_warthog_ampe_mtk_installed,
             (unsigned long)g_warthog_ampe_mgtk_installed,
             (unsigned long)g_warthog_mpm_no_slot, (unsigned long)g_warthog_mpm_expired,
             (unsigned long)g_warthog_bcn_peer_rx, (unsigned long)g_warthog_mpm_close_tx,
             (unsigned long)g_warthog_s1g_bcn_ours, (unsigned long)g_warthog_s1g_bcn_rx,
             (unsigned long)g_warthog_s1g_bcn_new, (unsigned long)g_warthog_s1g_bcn_retry,
             g_warthog_s1g_bcn_sa[3], g_warthog_s1g_bcn_sa[4], g_warthog_s1g_bcn_sa[5]);
    cdc_write(buf);
    reply_ok();
}

/* AT+KEYINST? -- every key the host pushed to the chip: aid, pairwise flag,
 * the index we asked for, and the hardware slot the chip actually assigned.
 * Whether that slot varies per AID decides if per-link (SAE/AMPE) keys are
 * possible at all on this part. */
static void cmd_keyinst(void)
{
    char buf[300]; int w = snprintf(buf, sizeof(buf), "+KEYINST: n=%lu ",
                                    (unsigned long)g_warthog_keyinst_n);
    uint32_t n = g_warthog_keyinst_n < 8 ? g_warthog_keyinst_n : 8;
    for (uint32_t i = 0; i < n && w < (int)sizeof(buf) - 40; i++) {
        uint32_t v = g_warthog_keyinst[i];
        w += snprintf(buf + w, sizeof(buf) - w, "[aid=%lu pw=%lu req=%lu hw=%lu] ",
                      (unsigned long)(v >> 24), (unsigned long)((v >> 16) & 0xff),
                      (unsigned long)((v >> 8) & 0xff), (unsigned long)(v & 0xff));
    }
    snprintf(buf + w, sizeof(buf) - w, "\r\n");
    cdc_write(buf);
    reply_ok();
}

/* AT+MTPUT=<ip>,<count>,<size> -- push <count> UDP datagrams of <size> bytes
 * to <ip>:4403 as fast as lwIP accepts them, and report elapsed time and the
 * resulting goodput. The far end counts them on the socket AT+MCAST=1 already
 * opened (bound 0.0.0.0:4403, so it takes unicast as well as the group), so
 * loss is (sent - far-end rx) with no extra plumbing.
 *
 * Sender-side rate only: it measures what the stack and the link accept, not
 * what arrived. Always read the receiver's AT+MCAST? for the other half. */
static void cmd_mtput(char *args)
{
    char *ip = trim(args);
    char *c1 = strchr(ip, ',');
    if (!c1) { reply_error("usage: AT+MTPUT=<ip>,<count 1-100000>,<size 1-1400>"); return; }
    *c1++ = 0;
    char *c2 = strchr(c1, ',');
    if (!c2) { reply_error("usage: AT+MTPUT=<ip>,<count>,<size>"); return; }
    *c2++ = 0;
    int count = atoi(c1), size = atoi(c2);
    if (count <= 0 || count > 100000 || size <= 0 || size > 1400) { reply_error("count 1-100000, size 1-1400"); return; }
    if (s_mc_sock < 0) { reply_error("AT+MCAST=1 first"); return; }

    static uint8_t payload[1400];
    memset(payload, 0x5a, (size_t)size);
    struct sockaddr_in d = { .sin_family = AF_INET, .sin_port = htons(MC_PORT),
                             .sin_addr.s_addr = inet_addr(ip) };
    if (d.sin_addr.s_addr == INADDR_NONE) { reply_error("bad ip"); return; }

    int ok = 0, err = 0;
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < count; i++) {
        if (sendto(s_mc_sock, payload, (size_t)size, 0, (struct sockaddr *)&d, sizeof(d)) < 0) {
            err++;
            if (errno == ENOMEM || errno == EWOULDBLOCK) { vTaskDelay(1); }
        } else {
            ok++;
        }
        /* The success path never yielded, so a long run starved the AT task at
         * its own priority and the console stopped answering for the duration.
         * One tick every 256 datagrams is far below the send rate and keeps
         * the command interruptible. */
        if ((i & 0xff) == 0xff) { vTaskDelay(1); }
    }
    int64_t us = esp_timer_get_time() - t0;
    if (us <= 0) { us = 1; }
    /* bits/s: bytes*8 * 1e6 / us, kept in 64-bit so it cannot overflow. */
    uint64_t bps = ((uint64_t)ok * (uint64_t)size * 8ull * 1000000ull) / (uint64_t)us;
    char buf[160];
    snprintf(buf, sizeof(buf),
             "+MTPUT: sent=%d err=%d size=%d ms=%lld kbps=%llu\r\n",
             ok, err, size, (long long)(us / 1000), (unsigned long long)(bps / 1000));
    cdc_write(buf);
    reply_ok();
}

/* AT+MTU? -- MTU of each netif. batman-adv needs the mesh netif to carry
 * 1500 + ~32 bytes of its own header, so this is a prerequisite check for
 * that path, not just curiosity. */
static void cmd_mtu(void)
{
    static const char *const keys[] = { "WIFI_STA_DEF", "USB", "WIFI_AP_DEF" };
    static const char *const names[] = { "halow", "usb", "ap" };
    char buf[160]; int w = snprintf(buf, sizeof(buf), "+MTU:");
    for (int i = 0; i < 3 && w < (int)sizeof(buf) - 24; i++) {
        /* IDF 5.4 has no esp_netif_get_mtu(); go to the lwIP netif behind it. */
        esp_netif_t *n = esp_netif_get_handle_from_ifkey(keys[i]);
        unsigned mtu = 0;
        if (n) {
            struct netif *lw = netif_get_by_index((u8_t)esp_netif_get_netif_impl_index(n));
            if (lw) { mtu = lw->mtu; }
        }
        w += snprintf(buf + w, sizeof(buf) - w, " %s=%u", names[i], mtu);
    }
    snprintf(buf + w, sizeof(buf) - w, "\r\n");
    cdc_write(buf);
    reply_ok();
}

static void cmd_erase(void)
{
    if (warthog_cfg_erase() != ESP_OK) {
        reply_error("nvs erase");
        return;
    }
    cdc_write("+INFO: NVS warthog namespace cleared. AT+RESET to apply.\r\n");
    reply_ok();
}

static void dispatch(char *line)
{
    line = trim(line);
    if (line[0] == '\0') {
        return;
    }
    if (!starts_with_i(line, "AT")) {
        reply_error("commands start with AT");
        return;
    }
    char *rest = line + 2;

    if (rest[0] == '\0') {
        reply_ok();
        return;
    }
    if (rest[0] != '+') {
        reply_error("expected AT+...");
        return;
    }
    rest++; /* past '+' */

    /* Find the '?', '=', or end-of-string to split verb from args. */
    char *verb_end = rest;
    while (*verb_end && *verb_end != '?' && *verb_end != '=') {
        verb_end++;
    }
    char terminator = *verb_end;
    *verb_end = '\0';
    char *verb = rest;
    char *args = (terminator == '\0') ? "" : verb_end + 1;

    if (strcasecmp(verb, "VERSION") == 0 && terminator == '?') {
        cmd_version();
    } else if (strcasecmp(verb, "STATUS") == 0 && terminator == '?') {
        cmd_status();
    } else if (strcasecmp(verb, "HALOW") == 0 && terminator == '?') {
        cmd_halow_query();
    } else if (strcasecmp(verb, "HALOW") == 0 && terminator == '=') {
        cmd_halow_set(args);
    } else if (strcasecmp(verb, "WIFIAP") == 0 && terminator == '?') {
        cmd_wifiap_query();
    } else if (strcasecmp(verb, "WIFIAP") == 0 && terminator == '=') {
        cmd_wifiap_set(args);
    } else if (strcasecmp(verb, "DNS") == 0 && terminator == '?') {
        cmd_dns_query();
    } else if (strcasecmp(verb, "DNS") == 0 && terminator == '=') {
        cmd_dns_set(args);
    } else if (strcasecmp(verb, "RESET") == 0 && terminator == '\0') {
        cmd_reset();
    } else if (strcasecmp(verb, "DLMODE") == 0 && terminator == '\0') {
        cmd_dlmode();
    } else if (strcasecmp(verb, "ERASE") == 0 && terminator == '\0') {
        cmd_erase();
    } else if (strcasecmp(verb, "MESHSTAT") == 0 && terminator == '?') {
        cmd_meshstat();
    } else if (strcasecmp(verb, "BCNSTAT") == 0 && terminator == '?') {
        cmd_bcnstat();
    } else if (strcasecmp(verb, "PRSPSTAT") == 0 && terminator == '?') {
        cmd_prspstat();
    } else if (strcasecmp(verb, "CCMPKAT") == 0 && terminator == '?') {
        cmd_ccmpkat();
    } else if (strcasecmp(verb, "CRYPTOHOST") == 0 && terminator == '=') {
        cmd_cryptohost_set(args);
    } else if (strcasecmp(verb, "CRYPTOHOST") == 0 && terminator == '?') {
        cmd_cryptohost_query();
    } else if (strcasecmp(verb, "REKEYSTAT") == 0 && terminator == '?') {
        cmd_rekeystat();
    } else if (strcasecmp(verb, "MESHSEC") == 0 && terminator == '=') {
        cmd_meshsec(args);
    } else if (strcasecmp(verb, "MESHSEC") == 0 && terminator == '?') {
        cmd_meshsec_q();
    } else if (strcasecmp(verb, "COREDUMP") == 0 && terminator == '?') {
        cmd_coredump();
    } else if (strcasecmp(verb, "SAESTAGE") == 0 && terminator == '?') {
        cmd_saestage();
    } else if (strcasecmp(verb, "SAEBRIDGE") == 0 && terminator == '=') {
        g_warthog_sae_bridge_en = (uint32_t)strtoul(args, NULL, 10);
        reply_ok();
    } else if (strcasecmp(verb, "SAERX") == 0 && terminator == '?') {
        char rbuf[512];
        snprintf(rbuf, sizeof(rbuf),
                 "+SAERX: hdl_auth=%lu hdl_sae=%lu trans=%lu status=%lu state=%lu seen=0x%lx confirm_tx=%lu txstat=%lu algrej=%lu txfail=%lu/%lu rxfail_sa=%06lx rxsa=%06lx rxda=%06lx offer=%06lx | act=%lu/%lu fsm=%lu plink=%lu seen=0x%lx ESTAB=%lu acc=%lu/%lu ampe_start=%lu rxact=%lu/%lu evt=%lu/%lu oversz=%lu\r\n",
                 (unsigned long)g_warthog_sae_hdl_auth, (unsigned long)g_warthog_sae_hdl_sae,
                 (unsigned long)g_warthog_sae_rx_trans, (unsigned long)g_warthog_sae_rx_status,
                 (unsigned long)g_warthog_sae_state_now, (unsigned long)g_warthog_sae_state_seen,
                 (unsigned long)g_warthog_sae_confirm_tx,
                 (unsigned long)g_warthog_sae_tx_status,
                 (unsigned long)g_warthog_sae_alg_reject,
                 (unsigned long)g_warthog_sae_txfail,
                 (unsigned long)g_warthog_sae_txfail_last,
                 (unsigned long)g_warthog_sae_rxfail_sa,
                 (unsigned long)g_warthog_sae_rx_sa,
                 (unsigned long)g_warthog_sae_rx_da,
                 (unsigned long)g_warthog_sae_offer_addr,
                 (unsigned long)g_warthog_mpm_act_tx,
                 (unsigned long)g_warthog_mpm_act_rx,
                 (unsigned long)g_warthog_mpm_fsm,
                 (unsigned long)g_warthog_mpm_plink,
                 (unsigned long)g_warthog_mpm_plink_seen,
                 (unsigned long)g_warthog_hostap_estab,
                 (unsigned long)g_warthog_accept,
                 (unsigned long)g_warthog_accept_nosm,
                 (unsigned long)g_warthog_ampe_start,
                 (unsigned long)g_warthog_rx_action_any,
                 (unsigned long)g_warthog_rx_selfprot,
                 (unsigned long)g_warthog_evt_mgmt,
                 (unsigned long)g_warthog_evt_action,
                 (unsigned long)g_warthog_mesh_act_oversize);
        cdc_write(rbuf); reply_ok();
    } else if (strcasecmp(verb, "SAEBRIDGE") == 0 && terminator == '?') {
        char bbuf[40];
        snprintf(bbuf, sizeof(bbuf), "+SAEBRIDGE: %lu\r\n", (unsigned long)g_warthog_sae_bridge_en);
        cdc_write(bbuf);
        reply_ok();
    } else if (strcasecmp(verb, "SAESTAGE") == 0 && terminator == '=') {
        g_warthog_sae_stage = 0x5AE00000u; /* clear to 'no step yet' */
        reply_ok();
    } else if (strcasecmp(verb, "REKEY") == 0 && terminator == '=') {
        cmd_rekey(args);
    } else if (strcasecmp(verb, "KEYFP") == 0 && terminator == '?') {
        cmd_keyfp();
    } else if (strcasecmp(verb, "KEYINST") == 0 && terminator == '?') {
        cmd_keyinst();
    } else if (strcasecmp(verb, "MPMPEERS") == 0 && terminator == '?') {
        cmd_mpmpeers();
    } else if (strcasecmp(verb, "MPMSTAT") == 0 && terminator == '?') {
        cmd_mpmstat();
    } else if (strcasecmp(verb, "MPMDUMP") == 0 && terminator == '?') {
        cmd_mpmdump();
    } else if (strcasecmp(verb, "MPING") == 0 && terminator == '=') {
        cmd_mping(args);
    } else if (strcasecmp(verb, "MUDP") == 0 && terminator == '?') {
        cmd_mudp();
    } else if (strcasecmp(verb, "MUDPLAST") == 0 && terminator == '?') {
        cmd_mudplast();
    } else if (strcasecmp(verb, "MINJECT") == 0 && terminator == '=') {
        cmd_minject(args);
    } else if (strcasecmp(verb, "MCAST") == 0 && terminator == '=') {
        cmd_mcast_set(args);
    } else if (strcasecmp(verb, "MCAST") == 0 && terminator == '?') {
        cmd_mcast_query();
    } else if (strcasecmp(verb, "MTPUT") == 0 && terminator == '=') {
        cmd_mtput(args);
    } else if (strcasecmp(verb, "MTU") == 0 && terminator == '?') {
        cmd_mtu();
    } else if (strcasecmp(verb, "MSEND") == 0 && terminator == '=') {
        cmd_msend(args);
    } else if (strcasecmp(verb, "PEERS") == 0 && terminator == '?') {
        cmd_peers();
    } else if (strcasecmp(verb, "BCNDUMP") == 0 && terminator == '?') {
        char hx[3*160 + 32]; int off = 0;
        off += snprintf(hx + off, sizeof(hx) - off, "+BCNDUMP: len=%u ", g_warthog_bcn_own_len);
        for (int i = 0; i < 160 && i < g_warthog_bcn_own_len && off < (int)sizeof(hx) - 4; i++)
            off += snprintf(hx + off, sizeof(hx) - off, "%02x", g_warthog_bcn_own[i]);
        off += snprintf(hx + off, sizeof(hx) - off, "\r\n");
        cdc_write(hx); reply_ok();
    } else if (strcasecmp(verb, "PLINKSTAT") == 0 && terminator == '?') {
        char b[96];
        snprintf(b, sizeof(b), "+PLINKSTAT: tx_conv=%lu rx_conv=%lu\r\n",
                 (unsigned long)g_warthog_mpm_tx_conv,
                 (unsigned long)g_warthog_mpm_rx_conv);
        cdc_write(b); reply_ok();
    } else if (strcasecmp(verb, "PLINKTX") == 0 && terminator == '?') {
        char hx[3*192 + 48]; int off = 0;
        off += snprintf(hx + off, sizeof(hx) - off, "+PLINKTX: full=%u len=%u ",
                        g_warthog_plink_tx_full, g_warthog_plink_tx_len);
        for (int i = 0; i < 192 && i < g_warthog_plink_tx_len && off < (int)sizeof(hx) - 4; i++)
            off += snprintf(hx + off, sizeof(hx) - off, "%02x", g_warthog_plink_tx[i]);
        off += snprintf(hx + off, sizeof(hx) - off, "\r\n");
        cdc_write(hx); reply_ok();
    } else if (strcasecmp(verb, "PLINKRX") == 0 && terminator == '?') {
        char hx[3*192 + 48]; int off = 0;
        off += snprintf(hx + off, sizeof(hx) - off, "+PLINKRX: full=%u len=%u ",
                        g_warthog_plink_rx_full, g_warthog_plink_rx_len);
        for (int i = 0; i < 192 && i < g_warthog_plink_rx_len && off < (int)sizeof(hx) - 4; i++)
            off += snprintf(hx + off, sizeof(hx) - off, "%02x", g_warthog_plink_rx[i]);
        off += snprintf(hx + off, sizeof(hx) - off, "\r\n");
        cdc_write(hx); reply_ok();
    } else if (strcasecmp(verb, "PRQRX") == 0 && terminator == '?') {
        char hx[3*96 + 64]; int off = 0;
        off += snprintf(hx + off, sizeof(hx) - off,
                        "+PRQRX: ta=%02x:%02x:%02x:%02x:%02x:%02x len=%u ",
                        g_warthog_prq_ta[0], g_warthog_prq_ta[1], g_warthog_prq_ta[2],
                        g_warthog_prq_ta[3], g_warthog_prq_ta[4], g_warthog_prq_ta[5],
                        g_warthog_prq_len);
        for (int i = 0; i < 96 && i < g_warthog_prq_len && off < (int)sizeof(hx) - 4; i++)
            off += snprintf(hx + off, sizeof(hx) - off, "%02x", g_warthog_prq_frame[i]);
        off += snprintf(hx + off, sizeof(hx) - off, "\r\n");
        cdc_write(hx); reply_ok();
    } else if (strcasecmp(verb, "BCNRX") == 0 && terminator == '?') {
        char hx[3*160 + 32]; int off = 0;
        off += snprintf(hx + off, sizeof(hx) - off, "+BCNRX: len=%u ", g_warthog_bcn_rx_len);
        for (int i = 0; i < 160 && i < g_warthog_bcn_rx_len && off < (int)sizeof(hx) - 4; i++)
            off += snprintf(hx + off, sizeof(hx) - off, "%02x", g_warthog_bcn_rx_frame[i]);
        off += snprintf(hx + off, sizeof(hx) - off, "\r\n");
        cdc_write(hx); reply_ok();
    } else if (strcasecmp(verb, "DATASTAT") == 0 && terminator == '?') {
        cmd_datastat();
    } else if (strcasecmp(verb, "HWMPDUMP") == 0 && terminator == '?') {
        cmd_hwmpdump();
    } else if (strcasecmp(verb, "HWMPSTAT") == 0 && terminator == '?') {
        cmd_hwmpstat();
    } else if (strcasecmp(verb, "FILTSTAT") == 0 && terminator == '?') {
        cmd_filtstat();
    } else if (strcasecmp(verb, "RXREORD") == 0 && terminator == '?') {
        cmd_rxreord();
    } else if (strcasecmp(verb, "RXCHAN") == 0 && terminator == '?') {
        cmd_rxchan();
    } else if (strcasecmp(verb, "FCRING") == 0 && terminator == '?') {
        cmd_fcring();
    } else if (strcasecmp(verb, "RXHEAD") == 0 && terminator == '?') {
        cmd_rxhead();
    } else {
        reply_error("unknown command");
    }
}

static void at_task(void *arg)
{
    (void)arg;
    char line[AT_LINE_MAX];
    size_t len = 0;
    bool discarding = false;
    bool was_connected = false;

    while (1) {
        bool connected = tud_cdc_n_connected(0);
        if (connected && !was_connected) {
            cdc_write("\r\n+READY: warthog AT interface\r\n");
            cdc_write("OK\r\n");
            len = 0;
        }
        was_connected = connected;

        if (!connected) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        uint8_t buf[64];
        size_t got = 0;
        if (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buf, sizeof(buf), &got) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(AT_RX_POLL_MS));
            continue;
        }
        if (got == 0) {
            vTaskDelay(pdMS_TO_TICKS(AT_RX_POLL_MS));
            continue;
        }

        for (size_t i = 0; i < got; i++) {
            uint8_t c = buf[i];
            /* Local echo so dumb terminals show input. */
            if (c == 0x7F || c == 0x08) {
                if (len > 0) {
                    len--;
                    cdc_write("\b \b");
                }
            } else if (c == '\r' || c == '\n') {
                cdc_write("\r\n");
                if (discarding) {
                    /* Tail of a line we already rejected. */
                    discarding = false;
                    len = 0;
                    continue;
                }
                line[len] = '\0';
                dispatch(line);
                len = 0;
            } else if (discarding) {
                continue; /* swallow the rest of an over-long line */
            } else if (len + 1 < sizeof(line)) {
                line[len++] = (char)c;
                char echo[2] = {(char)c, '\0'};
                cdc_write(echo);
            } else {
                /* Overflow. Reject the line AND everything up to the next
                 * terminator: without the discard state the bytes past the
                 * limit accumulated into a fresh buffer and the next newline
                 * DISPATCHED that fragment, so one over-long line produced an
                 * error and then ran whatever happened to trail it. */
                discarding = true;
                len = 0;
                cdc_write("\r\n");
                reply_error("line too long");
            }
        }
    }
}

esp_err_t warthog_at_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(at_task, "warthog_at", 4096, NULL,
                                            tskIDLE_PRIORITY + 2, NULL, 0);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "AT parser on CDC ACM 0");
    return ESP_OK;
}
