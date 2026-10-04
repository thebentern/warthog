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
#include "region.h"
#include "mesh_diag.h"
#include "mesh.h"
#include "cdc_out.h"
#include "usb_net.h"
#include "bat_mode.h"
#include "bat_port.h"

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
#include "mmwlan_cap.h"     /* AT+RXCAP, AT+TXCAP */
#include "mmosal.h"
#include "warthog_assert.h" /* AT+ASSERT? */
#include "warthog_shim.h"  /* AT+STACKS? */
#include "boot_guard.h"     /* AT+ASSERT? crash_boots, AT+ASSERTTEST=hang */
#include "mudp.h"
#include "mesh_bridge.h"
#include "nat_frag.h"       /* AT+MTU? ip_reass */
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "esp_timer.h"
#include "lwip/netif.h"
#include <errno.h>
#include "freertos/semphr.h"

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "warthog.at";

#define AT_LINE_MAX 192
#define AT_RX_POLL_MS 20

#ifndef WARTHOG_VERSION
#define WARTHOG_VERSION "0.0.0"
#endif

/* Replies, on the AT task only: waits while the FIFO is full (cdc_out.h). */
static void cdc_write(const char *s)
{
    (void)cdc_out_write(&g_warthog_cdc, s);
}

/* Other tasks and the echo: never waits, drops the whole line when it cannot go in. */
static void cdc_write_nowait(const char *s)
{
    (void)cdc_out_write_nowait(&g_warthog_cdc, s);
}

static void reply_ok(void)
{
    cdc_write("OK\r\n");
}

static void reply_error(const char *why)
{
    if (why) {
        char buf[128];
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

/* AT+MESHRSSI=<dBm>: the whole argument, -255..0. The glue guard runs it. */
static bool meshrssi_parse_(const char *a, int32_t *out)
{
    char *end = NULL;
    if (a == NULL || out == NULL || a[0] == '\0') {
        return false;
    }
    long v = strtol(a, &end, 10);
    if (end == a || *end != '\0' || v < -255 || v > 0) {
        return false;
    }
    *out = (int32_t)v;
    return true;
}

/* AT+GTKPERSTA=<0|1|2>: the whole argument. The glue guard runs it. */
static bool gtkpersta_parse_(const char *a, uint32_t *out)
{
    if (a == NULL || out == NULL || a[0] < '0' || a[0] > '2' || a[1] != '\0') {
        return false;
    }
    *out = (uint32_t)(a[0] - '0');
    return true;
}

/* AT+HOSTFRAG=<0|auto|n>: the whole argument; n 256..2346, made even as cfg80211 does. The glue
 * guard runs it. */
static bool hostfrag_parse_(const char *a, uint32_t *out)
{
    if (a == NULL || out == NULL || a[0] == '\0') {
        return false;
    }
    if (strcasecmp(a, "auto") == 0) {
        *out = 1u;
        return true;
    }
    char *end = NULL;
    unsigned long v = strtoul(a, &end, 10);
    if (end == a || *end != '\0' || a[0] < '0' || a[0] > '9' ||
        (v != 0u && (v < 256u || v > 2346u))) {
        return false;
    }
    *out = (uint32_t)v & ~1u;
    return true;
}

/* AT+TXRATE=<mcs>,<bw MHz> or off: the whole argument; MCS 0-9, 1/2/4/8 MHz. The glue guard runs it. */
static bool txrate_parse_(const char *a, int *mcs, int *bw)
{
    if (a == NULL || mcs == NULL || bw == NULL) {
        return false;
    }
    if (strcasecmp(a, "off") == 0) {
        *mcs = -1;
        *bw = -1;
        return true;
    }
    char *end = NULL;
    long m = strtol(a, &end, 10);
    if (end == a || *end != ',' || a[0] < '0' || a[0] > '9' || m < 0 || m > 9) {
        return false;
    }
    const char *b0 = end + 1;
    long b = strtol(b0, &end, 10);
    if (end == b0 || *end != '\0' || b0[0] < '0' || b0[0] > '9' ||
        (b != 1 && b != 2 && b != 4 && b != 8)) {
        return false;
    }
    *mcs = (int)m;
    *bw = (int)b;
    return true;
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

static void cmd_status(void)
{
    char buf[320]; /* +USBNET: at most 312 */

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
    struct usbnet_stats un;
    warthog_usb_net_stats(&un);
    snprintf(buf, sizeof(buf), "+USB: ip=" IPSTR " mounted=%d tx=%lu drop=%lu\r\n",
             IP2STR(&usb_ip.ip), tud_mounted() ? 1 : 0, (unsigned long)un.tx_sent,
             (unsigned long)(un.drop_full + un.drop_nolink + un.drop_nomem + un.drop_bad));
    cdc_write(buf);
    snprintf(buf, sizeof(buf),
             "+USBNET: tx_queued=%lu txq=%lu/%u txq_hw=%lu tx_stall_ms=%lu tx_busy_ms=%lu drop_full=%lu "
             "drop_nolink=%lu drop_nomem=%lu drop_bad=%lu | rx=%lu rx_xfer=%lu rx_nomem=%lu rx_err=%lu "
             "rx_idle_ms=%lu | kicks=%lu\r\n",
             (unsigned long)un.tx_queued, (unsigned long)un.txq, USBNET_TXQ_N, (unsigned long)un.txq_hw,
             (unsigned long)un.tx_stall_ms, (unsigned long)un.tx_busy_ms, (unsigned long)un.drop_full,
             (unsigned long)un.drop_nolink, (unsigned long)un.drop_nomem, (unsigned long)un.drop_bad,
             (unsigned long)un.rx, (unsigned long)un.rx_xfer, (unsigned long)un.rx_nomem,
             (unsigned long)un.rx_err, (unsigned long)un.rx_idle_ms, (unsigned long)un.kicks);
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
volatile uint32_t g_warthog_bcn_chip_irq = 0;  /* chip beacon IRQs (beacon.c) */
volatile uint32_t g_warthog_bcn_host_yield = 0; /* host ticks that left a TBTT to the chip */

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
/* Software CCMP on the mesh RX path (AT+SWCCMP?). ok climbing while the peer
 * pings is the whole point; micfail climbing instead means the two ends built
 * different AAD, and last_aad is the first thing to compare. */
/* AT+SWCCMP=1 arms it. A build may arm it at boot instead
 * (-DWARTHOG_MESH_HOST_CCMP_DEFAULT_ON=1), which is what makes the feature
 * testable on a board whose console is unreachable: the peer's traffic is
 * then the only instrument needed. */
#ifndef WARTHOG_MESH_HOST_CCMP_DEFAULT_ON
#define WARTHOG_MESH_HOST_CCMP_DEFAULT_ON 0
#endif
volatile uint32_t g_warthog_host_ccmp_on = WARTHOG_MESH_HOST_CCMP_DEFAULT_ON;
volatile uint32_t g_warthog_swccmp_tried = 0;
volatile uint32_t g_warthog_swccmp_tx_ok = 0;
volatile uint32_t g_warthog_swccmp_tx_fail = 0;
volatile uint32_t g_warthog_swccmp_ok = 0;
volatile uint32_t g_warthog_swccmp_micfail = 0;
volatile uint32_t g_warthog_swccmp_nokey = 0;
volatile uint32_t g_warthog_swccmp_grpkey = 0; /* unicast keyed with a group key: refused */
volatile uint32_t g_warthog_swccmp_badhdr = 0;
volatile uint32_t g_warthog_swccmp_short = 0;
volatile uint32_t g_warthog_swccmp_last_keyid = 0;
volatile uint32_t g_warthog_swccmp_last_aadlen = 0;
volatile uint8_t g_warthog_swccmp_last_aad[32] = { 0 };
/* The last MIC failure alone (AT+SWCCMP? second line): body length, key id, PN, 802.11 header. */
volatile uint32_t g_warthog_swccmp_fail_len = 0;
volatile uint32_t g_warthog_swccmp_fail_keyid = 0;
volatile uint8_t g_warthog_swccmp_fail_pn[6] = { 0 };
volatile uint8_t g_warthog_swccmp_fail_hdr[32] = { 0 };
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
/* The chip VIF in use (umac_interface.c): its MMDRV_INTERFACE_TYPE (0 none, 1 STA, 2 AP,
 * 5 MESH) and id, MESH adds that fell back to STA, and the last such add's status or error. */
volatile uint32_t g_warthog_chipvif_type = 0;
volatile uint32_t g_warthog_chipvif_id = 0;
volatile uint32_t g_warthog_chipvif_fallback = 0;
volatile int32_t g_warthog_chipvif_add_status = 0;
/* BSSID_SET / MESH_CONFIG the chip refused (umac_mesh.c), and the last refusal's status. */
volatile uint32_t g_warthog_chipcmd_bssid_refused = 0;
volatile int32_t g_warthog_chipcmd_bssid_status = 0;
volatile uint32_t g_warthog_chipcmd_meshcfg_refused = 0;
volatile int32_t g_warthog_chipcmd_meshcfg_status = 0;
/* BSS_BEACON_CONFIG, SET_STA_STATE (by state, NOTEXIST..AUTHORIZED) and INSTALL_KEY the chip
 * refused, each with the last refusal's status; accepted keys at another hw index than asked
 * (a MESH-VIF build counts them, others assert); and the MESH_CONFIG(START) sent: 0 none,
 * 1 beaconing, 2 beaconless. */
volatile uint32_t g_warthog_chipcmd_beacon_refused = 0;
volatile int32_t g_warthog_chipcmd_beacon_status = 0;
volatile uint32_t g_warthog_chipcmd_sta_refused[5] = {0};
volatile int32_t g_warthog_chipcmd_sta_status = 0;
volatile uint32_t g_warthog_chipcmd_key_refused = 0;
volatile int32_t g_warthog_chipcmd_key_status = 0;
volatile uint32_t g_warthog_chipcmd_keyidx_mismatch = 0;
volatile uint32_t g_warthog_chipcmd_meshcfg_mode = 0;
/* Mesh data-plane protection. 1 (default) = register peers as secured and
 * install the static MTK/MGTK; 0 = leave them OPEN.
 *
 * A keyed mesh cannot carry data to a peer running an OPEN mesh (an OpenMANET
 * node only if set to encryption='none'; its mesh wizard writes SAE). The
 * keying was introduced because an OPEN mesh measured acked-but-never-delivered
 * -- but the chip STA registration that also fixes delivery landed in the same
 * change, so which of the two actually opened the gate was never isolated.
 * AT+MESHSEC= flips this at runtime and re-peers, so the two can be told apart
 * on hardware. */
volatile uint32_t g_warthog_mesh_secure = 1;
/* Forwarding and bridge gates, seeded from NVS in mesh.c before the mesh starts. */
volatile uint32_t g_warthog_mesh_fwd = 0, g_warthog_mesh_bridge = 0, g_warthog_mesh_grp = 0;
/* BATMAN_V member mode, running this boot (set by bat_port.c before the mesh starts). */
volatile uint32_t g_warthog_mesh_batman = 0;
/* Mesh MFP. Read once by the supplicant shim while it builds the mesh config,
 * so unlike AT+MESHSEC= this one cannot be flipped under a live mesh. */
volatile uint32_t g_warthog_mesh_pmf = 0;
volatile uint32_t g_warthog_fwd_uni = 0, g_warthog_fwd_grp = 0, g_warthog_fwd_nomem = 0;
volatile uint32_t g_warthog_fwd_drop_own = 0, g_warthog_fwd_drop_dup = 0, g_warthog_fwd_drop_ttl = 0;
volatile uint32_t g_warthog_fwd_drop_nopath = 0, g_warthog_fwd_drop_nofwd = 0, g_warthog_fwd_drop_bad = 0;
volatile uint32_t g_warthog_fwd_perr_tx = 0, g_warthog_fwd_preq_tx = 0;
volatile uint32_t g_warthog_hwmp_relay_preq = 0, g_warthog_hwmp_relay_prep = 0, g_warthog_hwmp_relay_perr = 0;
volatile uint32_t g_warthog_fwd_drop_full = 0;
/* Relayed unicast held while we discover its mesh DA: held, sent on PREP, lost. */
volatile uint32_t g_warthog_fwd_hold = 0, g_warthog_fwd_hold_tx = 0, g_warthog_fwd_hold_drop = 0;
volatile uint32_t g_warthog_fwd_drop_tblfull = 0; /* path table: no slot for a new destination */
volatile uint32_t g_warthog_fwd_pend_tx = 0, g_warthog_fwd_pend_drop = 0, g_warthog_hwmp_prot = 0;
volatile uint32_t g_warthog_hwmp_unprotected = 0, g_warthog_hwmp_mmie = 0, g_warthog_hwmp_nommie = 0;
/* SAE path selection: group taken Protected (group-addressed privacy); unicast sent protected,
 * group sent under our MGTK or in the clear (no MGTK); group frames not queued to the loop. */
volatile uint32_t g_warthog_hwmp_gp = 0, g_warthog_hwmp_tx_prot = 0;
volatile uint32_t g_warthog_hwmp_tx_gp = 0, g_warthog_hwmp_tx_plain = 0;
volatile uint32_t g_warthog_hwmp_tx_qdrop = 0;
/* Group path selection queued for the umac event loop that it then failed to build or send. */
volatile uint32_t g_warthog_hwmp_tx_qfail = 0;
/* SAE: path selection refused from a station whose link is not established (unkeyed). */
volatile uint32_t g_warthog_hwmp_unestab = 0;
/* Protected management frames on an SAE mesh: opened by the chip, by host CCMP, by neither;
 * unicast ones refused for a key id other than the link's pairwise key. */
volatile uint32_t g_warthog_mgmt_prot_chip = 0, g_warthog_mgmt_prot_host = 0;
volatile uint32_t g_warthog_mgmt_prot_nodec = 0, g_warthog_ampe_igtk_installed = 0;
volatile uint32_t g_warthog_mgmt_prot_grpkey = 0;
/* Protected group ones: opened by neither, by the chip under anything but the sender's MGTK
 * at its AID (so refused), under a key id not the sender's MGTK (refused), or replayed. */
volatile uint32_t g_warthog_mgmt_gp_nodec = 0, g_warthog_mgmt_gp_own = 0;
volatile uint32_t g_warthog_mgmt_gp_key = 0, g_warthog_mgmt_gp_replay = 0;
/* A peer's MGTK in the chip at its AID (chip-key SAE builds on a MESH chip VIF, AT+GTKSTAT?):
 * installs the chip took and that failed, removals it took and that failed; per slot bit 31 =
 * held, AID << 16, key id << 8, the chip's index, and the peer's low three octets. */
volatile uint32_t g_warthog_peer_gtk_inst = 0, g_warthog_peer_gtk_fail = 0;
volatile uint32_t g_warthog_peer_gtk_del = 0, g_warthog_peer_gtk_delfail = 0;
volatile uint32_t g_warthog_peer_gtk[4], g_warthog_peer_gtk_mac[4];
/* Group frames the chip opened under the sender's MGTK at its AID (data, management), and
 * group data it opened under any other key, refused (rxdrop 95). */
volatile uint32_t g_warthog_rx_grp_chip = 0, g_warthog_mgmt_gp_chip = 0;
volatile uint32_t g_warthog_rx_grp_forged = 0;
/* Of the fresh ones (past the replay peek), how many carried MIC octets that verify under the
 * sender's MGTK. A mic ok arms the check for its class (bit 0 data, bit 1 management); once
 * armed a mic bad is dropped (micdrop, data and management). */
volatile uint32_t g_warthog_rx_grp_mic_ok = 0, g_warthog_rx_grp_mic_bad = 0;
volatile uint32_t g_warthog_rx_grp_mic_armed = 0;
volatile uint32_t g_warthog_rx_grp_micdrop = 0, g_warthog_mgmt_gp_micdrop = 0;
/* AT+GTKPERSTA: 0 off, 1 on (a fresh TX PN epoch per install), 2 on at TX PN 0 as Linux.
 * Seeded from NVS at mesh start. */
volatile uint32_t g_warthog_peer_gtk_mode = 1;
/* Chip-opened group frames refused because they were read off the chip before the sender's
 * current MGTK went in (fence), or while a refused DISABLE_KEY leaves a stale key at its AID
 * (taint); and the slots so tainted, one bit each. */
volatile uint32_t g_warthog_peer_gtk_fence = 0, g_warthog_peer_gtk_taint = 0;
volatile uint32_t g_warthog_peer_gtk_tainted = 0;
/* Frames read off the chip (pageset.c / yaps.c stamp each one). */
volatile uint32_t g_warthog_rx_read_seq = 0;
/* Robust unicast management frames (Block Ack) sent to a peer that runs MFP: protected by the
 * chip, sealed with host CCMP, or dropped because they could not be sealed. */
volatile uint32_t g_warthog_mgmt_tx_chip = 0, g_warthog_mgmt_tx_host = 0, g_warthog_mgmt_tx_drop = 0;

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
/* Keyed peer, no active key; or the chip lacks it after a restart (data, management frames). */
volatile uint32_t g_warthog_tx_nokey = 0;
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
/* RX frames whose Mesh Control carried Address Extension -- a peer proxying for
 * a device behind it. Non-zero means a bridged peer is talking to us. */
volatile uint32_t g_warthog_rx_meshctrl_ae = 0;
/* Mesh data frames whose destination is a third node -- i.e. frames a relay
 * would forward. Non-zero proves the chip delivers them to the host, which is
 * the open question gating 802.11s forwarding. */
volatile uint32_t g_warthog_rx_fwd_candidate = 0;

/* Per-peer RSSI, the thing that makes a mesh link diagnosable rather than
 * merely present. Aggregate counters say traffic moves; they do not say which
 * neighbour is marginal. Small fixed table, no allocation, written from the
 * RX path and read by AT+MESHRSSI?. */
#define WARTHOG_RSSI_PEERS 6
struct warthog_peer_rssi {
    uint8_t  mac[6];
    int16_t  last;
    int16_t  min;
    int16_t  max;
    /* Noise gives SNR, which is what actually predicts whether a link holds;
     * bandwidth is here because a neighbour transmitting at a width we did not
     * configure is an interop mismatch, not a weak signal. This driver's RX
     * metadata (struct mmdrv_rx_metadata) carries no MCS, so per-peer MCS is
     * not reportable -- see the note on AT+MESHRSSI? in the wiki. */
    int8_t   noise;
    uint8_t  bw_mhz;
    uint32_t frames;
    bool     used;
};
static struct warthog_peer_rssi s_peer_rssi[WARTHOG_RSSI_PEERS];

void warthog_mesh_rssi_note(const uint8_t *ta, int16_t rssi, int8_t noise, uint8_t bw_mhz)
{
    if (ta == NULL) {
        return;
    }
    int free_slot = -1;
    for (int i = 0; i < WARTHOG_RSSI_PEERS; i++) {
        if (s_peer_rssi[i].used) {
            if (memcmp(s_peer_rssi[i].mac, ta, 6) == 0) {
                s_peer_rssi[i].last = rssi;
                if (rssi < s_peer_rssi[i].min) { s_peer_rssi[i].min = rssi; }
                if (rssi > s_peer_rssi[i].max) { s_peer_rssi[i].max = rssi; }
                s_peer_rssi[i].noise = noise;
                s_peer_rssi[i].bw_mhz = bw_mhz;
                s_peer_rssi[i].frames++;
                return;
            }
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        return; /* more neighbours than slots; keep the ones we know */
    }
    memcpy(s_peer_rssi[free_slot].mac, ta, 6);
    s_peer_rssi[free_slot].last = rssi;
    s_peer_rssi[free_slot].min = rssi;
    s_peer_rssi[free_slot].max = rssi;
    s_peer_rssi[free_slot].noise = noise;
    s_peer_rssi[free_slot].bw_mhz = bw_mhz;
    s_peer_rssi[free_slot].frames = 1;
    s_peer_rssi[free_slot].used = true;
}
volatile uint8_t  g_warthog_rx_fwd_last_da[6] = {0};
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

/* Fragment reassembly (AT+DEFRAG?): unicast fragments taken in and MSDUs rebuilt; fragments
 * dropped, by cause; group and plaintext fragments, and mesh data of a shape mac80211 drops
 * (whole frames too, rxdrop 89), refused before it; chains discarded (evict: to stay within
 * 2 per peer and 4 on the node). */
volatile uint32_t g_warthog_defrag_in = 0, g_warthog_defrag_ok = 0;
volatile uint32_t g_warthog_defrag_nofirst = 0, g_warthog_defrag_order = 0, g_warthog_defrag_pn = 0;
volatile uint32_t g_warthog_defrag_key = 0, g_warthog_defrag_prot = 0, g_warthog_defrag_hdr = 0;
volatile uint32_t g_warthog_defrag_amsdu = 0, g_warthog_defrag_oversize = 0, g_warthog_defrag_nomem = 0;
volatile uint32_t g_warthog_defrag_mcast = 0, g_warthog_defrag_plain = 0, g_warthog_defrag_shape = 0;
volatile uint32_t g_warthog_defrag_expired = 0, g_warthog_defrag_restart = 0, g_warthog_defrag_flush = 0;
volatile uint32_t g_warthog_defrag_evict = 0;

/* Host TX fragmentation (AT+HOSTFRAG): 0 off, 1 auto, else a threshold in octets; seeded from NVS
 * at mesh start. MSDUs cut and fragments handed to the chip, by the limit that cut each. */
volatile uint32_t g_warthog_hostfrag = WARTHOG_CFG_MESH_HOSTFRAG_DEFAULT;
volatile uint32_t g_warthog_hostfrag_msdu = 0, g_warthog_hostfrag_frags = 0;
volatile uint32_t g_warthog_hostfrag_by_thresh = 0, g_warthog_hostfrag_by_chip = 0;
volatile uint32_t g_warthog_hostfrag_by_rate = 0;
/* Over a limit but sent whole: more fragments than the build cuts (2; 16 in the host tests' ANY
 * builds), or no TX buffer for them. Dropped: host CCMP or the driver refused a fragment. */
volatile uint32_t g_warthog_hostfrag_many = 0, g_warthog_hostfrag_pool = 0;
volatile uint32_t g_warthog_hostfrag_seal = 0, g_warthog_hostfrag_drv = 0;
/* Originator Block Ack sessions ended to cut a frame, their DELBA handed to the chip or not; cut
 * MSDUs held for that DELBA's TX status; waits that ended at the limit without it. */
volatile uint32_t g_warthog_hostfrag_ba_end = 0, g_warthog_hostfrag_nodelba = 0;
volatile uint32_t g_warthog_hostfrag_ba_wait = 0, g_warthog_hostfrag_ba_late = 0;
/* ADDBAs the hold-off kept back (one an episode); peer TIDs held now, and for how long after the
 * last frame that needed cutting. */
volatile uint32_t g_warthog_hostfrag_hold = 0;
volatile uint32_t g_warthog_hostfrag_held = 0, g_warthog_hostfrag_hold_ms = 15000;
/* Fragment TX statuses, those sent in an A-MPDU (agg); MSDUs every fragment of which was acked,
 * or not; chip-sealed frames handed while a run they could break was in the chip (overlap). */
volatile uint32_t g_warthog_hostfrag_acked = 0, g_warthog_hostfrag_noack = 0;
volatile uint32_t g_warthog_hostfrag_unsent = 0, g_warthog_hostfrag_agg = 0;
volatile uint32_t g_warthog_hostfrag_ok = 0, g_warthog_hostfrag_fail = 0, g_warthog_hostfrag_overlap = 0;
/* Held while a run is in the chip: a peer's next frame (wait), chip-sealed management (mgmt); runs
 * whose statuses never came (stale); PNs counted beyond one for frames the chip may cut (chippn). */
volatile uint32_t g_warthog_hostfrag_wait = 0, g_warthog_hostfrag_mgmt = 0;
volatile uint32_t g_warthog_hostfrag_stale = 0, g_warthog_hostfrag_chippn = 0;
/* Frames whose later retry rates could not carry them whole, folded into a rate that can. */
volatile uint32_t g_warthog_hostfrag_trim = 0;
/* The last MSDU cut: fragments, body octets, the MPDU limit, the rate (MHz << 8 | MCS, 0xffff none). */
volatile uint32_t g_warthog_hostfrag_last_n = 0, g_warthog_hostfrag_last_len = 0;
volatile uint32_t g_warthog_hostfrag_last_lim = 0, g_warthog_hostfrag_last_rate = 0xffff;
/* MSDUs whose chain was cut to rates needing at most 2 fragments, or replaced by one such rate;
 * AT+HOSTFRAG=n raised to cut in 2. */
volatile uint32_t g_warthog_hostfrag_cap_trim = 0, g_warthog_hostfrag_cap_sub = 0;
volatile uint32_t g_warthog_hostfrag_clamp = 0;
/* AT+SEALFIT: 1 a sealed unicast frame goes only at rates where the chip sends it as 1.17.6 delivers
 * it (RAM only): chains cut, replaced by one such rate, or left (no rate does); group frames too. */
volatile uint32_t g_warthog_sealfit = 1;
volatile uint32_t g_warthog_sealfit_trim = 0, g_warthog_sealfit_sub = 0, g_warthog_sealfit_nofit = 0;
volatile uint32_t g_warthog_grpfit_trim = 0, g_warthog_grpfit_sub = 0, g_warthog_grpfit_nofit = 0;
/* Chip-sealed chains cut or replaced to send the frame whole under this node's Block Ack session. */
volatile uint32_t g_warthog_sealfit_ba = 0;
/* Frames cut while the peer's reorder size to us on that TID was set (its session, or one it ended);
 * waited-on DELBAs the chip gave up on unacked. */
volatile uint32_t g_warthog_hostfrag_ba_rcpt = 0, g_warthog_hostfrag_delba_noack = 0;
/* AT+TIDPARAMS: 1 a descriptor carries Block Ack fields only under our own agreed session and a host
 * fragment none (morse_driver's rule), 0 morselib's; applies while AT+HOSTFRAG is in force or AT+AMPDU=0. */
volatile uint32_t g_warthog_ba_txparm = 1;

/* AT+AMPDU: 1 the mesh starts originator Block Ack sessions, 0 never; seeded from NVS at mesh start.
 * Originator sessions agreed now; those AT+AMPDU=0 ended, their DELBA handed to the chip or not. */
volatile uint32_t g_warthog_ampdu = 1;
volatile uint32_t g_warthog_ampdu_orig = 0, g_warthog_ampdu_ended = 0, g_warthog_ampdu_unsent = 0;
/* Each peer slot: its MAC's last 3 octets (bit 24 set while the slot is in use), and per TID 0-5
 * our session agreed (bits 0-5), ours asked or refused (8-13), its session to us (16-21), held (24-29). */
volatile uint32_t g_warthog_ampdu_peer_mac[4] = { 0 }, g_warthog_ampdu_peer_ba[4] = { 0 };
/* Block Ack frames handed to the chip: ADDBA Requests; DELBAs by reason (4 the ADDBA timed out,
 * 37 a session stopped, any other). Recipient DELBAs a peer sent us, and the last one's reason. */
volatile uint32_t g_warthog_ba_addba_tx = 0, g_warthog_ba_delba_to = 0;
volatile uint32_t g_warthog_ba_delba_end = 0, g_warthog_ba_delba_other = 0;
volatile uint32_t g_warthog_ba_rx_delba = 0, g_warthog_ba_rx_reason = 0;

/* AT+CHIPRESTART?: restarts handled, forced, with the mesh put back in full; requests dropped
 * because the driver was stopped. */
volatile uint32_t g_warthog_chiprestart_n = 0, g_warthog_chiprestart_forced = 0;
volatile uint32_t g_warthog_chiprestart_mesh = 0, g_warthog_chiprestart_dropped = 0;
/* Station records and keys put back and not; other restore commands failed; put back by a retry,
 * still waiting; the last restart's duration. */
volatile uint32_t g_warthog_chiprestart_sta = 0, g_warthog_chiprestart_stafail = 0;
volatile uint32_t g_warthog_chiprestart_keys = 0, g_warthog_chiprestart_keyfail = 0;
volatile uint32_t g_warthog_chiprestart_cmdfail = 0, g_warthog_chiprestart_retried = 0;
volatile uint32_t g_warthog_chiprestart_pending = 0, g_warthog_chiprestart_ms = 0;

/* RX frame-filter drop accounting.
 *
 * umac_datapath_rx_frame_filter() has nine ways to discard a frame and, until
 * now, none of them incremented anything: filter= counts frames ENTERING, so a
 * peer whose traffic dies inside reads as 42 in and 1 delivered with every drop
 * counter at zero. Reasons, in the order they appear in that function:
 *   1 short (frame control)   2 RTS          3 beacon filtered
 *   4 short (header)          5 no datapath ops
 *   6 unknown sender          7 SA is our own address
 *   9 mesh unicast data whose RA (addr1) is another station   8 duplicate frame
 * A histogram plus the last reason is enough to name the cause in one read. */
volatile uint32_t g_warthog_filt_reason = 0, g_warthog_filt_drop = 0;
volatile uint32_t g_warthog_filt_hist[10] = { 0 };
/* Mesh unicast management (beacons aside) whose addr1 is another station: counted, not
 * dropped, with the last one's first 16 octets (AT+FILTSTAT? second line). */
volatile uint32_t g_warthog_filt_mgmt_nours = 0;
volatile uint8_t g_warthog_filt_mgmt_nours_hdr[16] = { 0 };

/* HWMP path selection. A mac80211 peer will not send a unicast data frame to a
 * neighbour it has no PATH to (unless mesh_nolearn is on, as on OpenMANET
 * 1.8.1-dev wizard nodes), and peering ESTAB does not create one -- so
 * before this, warthog was reachable by broadcast and unreachable by anything
 * else. preq_tx is what makes us routable (a peer installs a path to any PREQ
 * originator it accepts); prep_tx is what answers a peer's discovery. */
volatile uint32_t g_warthog_hwmp_rx = 0, g_warthog_hwmp_preq_rx = 0, g_warthog_hwmp_preq_tx = 0;
volatile uint32_t g_warthog_hwmp_prep_tx = 0, g_warthog_hwmp_parse_fail = 0;
volatile uint32_t g_warthog_hwmp_not_ours = 0;
volatile uint32_t g_warthog_hwmp_prep_rx = 0, g_warthog_hwmp_rann_rx = 0, g_warthog_hwmp_perr_rx = 0;

/* AMPE key installs. Non-zero means SAE/AMPE actually derived a key and it
 * reached the chip -- the difference between real mesh security and the
 * hardcoded constant. */
volatile uint32_t g_warthog_ampe_mtk_installed = 0, g_warthog_ampe_mgtk_installed = 0;
/* Re-installs of our own MGTK at a fresh TX PN base for an AMPE Key RSC, and
 * failed ones (WARTHOG_MESH_MGTK_PN_BASE builds; ampe_mgtk counts first installs). */
volatile uint32_t g_warthog_mgtk_reinst = 0, g_warthog_mgtk_rsc_fail = 0;
/* 0 = open mesh, 1 = SAE authenticator initialised, 2 = SAE asked for but
 * mesh_rsn_auth_init() failed (mesh still runs, unsecured). */
volatile uint32_t g_warthog_sae_init = 0;
volatile uint32_t g_warthog_sae_peer_offers = 0, g_warthog_sae_peer_parse_fail = 0;
/* SAE candidates not offered to hostap: new peers while the peer table is full. */
volatile uint32_t g_warthog_sae_offer_full = 0;
/* SAE handshakes that timed out, peerings that failed after SAE (each: slot
 * freed, address held off), and offers refused during such a hold-off. */
volatile uint32_t g_warthog_sae_fail = 0, g_warthog_sae_offer_held = 0, g_warthog_plink_fail = 0;
/* Candidate RSSI floor in dBm (AT+MESHRSSI=, seeded from NVS in mesh.c); 0 or
 * -255 is off. New peerings refused at or below it, and those it let through. */
volatile int32_t g_warthog_mesh_rssi_floor = -80;
volatile uint32_t g_warthog_mesh_rssi_skip = 0, g_warthog_mesh_rssi_pass = 0;
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
 * beacons ARE being served and the problem is downstream of this handshake.
 * chip_irq = beacon IRQs the chip raised (1 on a STA chip VIF; one per TBTT when it
 * schedules its own); host_yield = host timer ticks that left the beacon to it. */
static void cmd_bcnstat(void)
{
    char buf[192];
    snprintf(buf, sizeof(buf),
             "+BCNSTAT: req=%lu served=%lu null=%lu inactive=%lu enq=%lu/%lu txcomp=%lu "
             "chip_irq=%lu host_yield=%lu\r\n",
             (unsigned long)g_warthog_bcn_req, (unsigned long)g_warthog_bcn_served,
             (unsigned long)g_warthog_bcn_null, (unsigned long)g_warthog_bcn_inactive,
             (unsigned long)g_warthog_bcn_enq_ok, (unsigned long)g_warthog_bcn_enq_err,
             (unsigned long)g_warthog_bcn_txcomp, (unsigned long)g_warthog_bcn_chip_irq,
             (unsigned long)g_warthog_bcn_host_yield);
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
    cdc_write_nowait(c->line); /* esp_ping task: a wait would delay the next ping */
}

static void mping_on_timeout(esp_ping_handle_t h, void *args)
{
    mping_ctx_t *c = (mping_ctx_t *)args;
    uint16_t seq = 0;
    esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    snprintf(c->line, sizeof(c->line), "+MPING: seq=%u timeout\r\n", (unsigned)seq);
    cdc_write_nowait(c->line);
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

/* The AT+MESHCFG? mode and batman lines, libc only: the glue guard compiles these
 * out of this file and checks the longest output fits line[320]. */
struct meshcfg_mode {
    bool fwd, bridge_active, bridge_stored, bat_running, bat_stored, grp_std;
    const char *bat_reason;
    unsigned neigh, routes, copies;
    uint32_t tput_override;
    uint8_t self[6], soft[6];
};

static int meshcfg_mode_line_(char *buf, size_t len, const struct meshcfg_mode *m)
{
    if (m->bat_running) {
        return snprintf(buf, len,
                        "+MESHCFG: forwarding=no routing=batman_v l2=no(NAT) "
                        "multicast=meshtastic(239.0.0.69) batman=yes(neigh=%u routes=%u "
                        "soft=%02x:%02x:%02x:%02x:%02x:%02x)\r\n",
                        m->neigh, m->routes, m->soft[0], m->soft[1], m->soft[2], m->soft[3],
                        m->soft[4], m->soft[5]);
    }
    /* Stored for next boot is not a refusal: only a reason this boot's start gave. */
    const bool refused = m->bat_stored && m->bat_reason != NULL && strcmp(m->bat_reason, "off") != 0;
    return snprintf(buf, len, "+MESHCFG: forwarding=%s routing=%s l2=%s multicast=%s batman=%s%s%s\r\n",
                    m->fwd ? "yes(802.11s)" : "no", m->fwd ? "hwmp" : "none",
                    m->bridge_active ? "bridge" : (m->bridge_stored ? "no(NAT;bridge-failed)" : "no(NAT)"),
                    m->bridge_active ? "all(bridged)" : "meshtastic(239.0.0.69)",
                    refused ? "refused(" : "no", refused ? m->bat_reason : "", refused ? ")" : "");
}

static int meshcfg_bat_line_(char *buf, size_t len, const struct meshcfg_mode *m)
{
    return snprintf(buf, len,
                    "+MESHCFG: batman self=%02x:%02x:%02x:%02x:%02x:%02x "
                    "soft=%02x:%02x:%02x:%02x:%02x:%02x hard_mtu=%u soft_mtu=%u bcast=%s "
                    "copies=%u tput_override=%lu\r\n",
                    m->self[0], m->self[1], m->self[2], m->self[3], m->self[4], m->self[5],
                    m->soft[0], m->soft[1], m->soft[2], m->soft[3], m->soft[4], m->soft[5],
                    (unsigned)BAT_HARD_MTU_DEFAULT, (unsigned)BAT_SOFT_MTU_DEFAULT,
                    m->grp_std ? "std" : "replicate", m->copies, (unsigned long)m->tput_override);
}

/* The AT+MESHCFG? chip VIF lines, libc only: the glue guard compiles them out of this file
 * and checks they name each type and refusal and fit line[320]. */
struct meshcfg_chipvif {
    uint32_t type, vif_id, fallback, bssid_refused, meshcfg_refused;
    int32_t add_status, bssid_status, meshcfg_status;
    bool built_mesh;
    uint32_t beacon_refused, sta_refused[5], key_refused, keyidx_mismatch, meshcfg_mode;
    int32_t beacon_status, sta_status, key_status;
};

static const char *meshcfg_chipvif_name_(uint32_t type)
{
    switch (type) {
    case 0: return "none";
    case 1: return "sta";
    case 2: return "ap";
    case 5: return "mesh";
    default: return "other";
    }
}

static int meshcfg_chipvif_line_(char *buf, size_t len, const struct meshcfg_chipvif *c)
{
    return snprintf(buf, len,
                    "+MESHCFG: chip_vif=%s(%lu) vif_id=%lu built=%s fallback=%lu add_st=%ld "
                    "bssid_refused=%lu(st=%ld) mesh_config_refused=%lu(st=%ld)\r\n",
                    meshcfg_chipvif_name_(c->type), (unsigned long)c->type,
                    (unsigned long)c->vif_id, c->built_mesh ? "mesh" : "sta",
                    (unsigned long)c->fallback, (long)c->add_status,
                    (unsigned long)c->bssid_refused, (long)c->bssid_status,
                    (unsigned long)c->meshcfg_refused, (long)c->meshcfg_status);
}

static const char *meshcfg_sent_name_(uint32_t mode)
{
    switch (mode) {
    case 0: return "none";
    case 1: return "beaconing";
    case 2: return "beaconless";
    default: return "other";
    }
}

/* Refusals by command, SET_STA_STATE by state (NOTEXIST/NONE/AUTH/ASSOC/AUTHORIZED). */
static int meshcfg_chipcmd_line_(char *buf, size_t len, const struct meshcfg_chipvif *c)
{
    return snprintf(buf, len,
                    "+MESHCFG: chip_refused beacon_config=%lu(st=%ld) sta_state=%lu/%lu/%lu/%lu/%lu(st=%ld) "
                    "install_key=%lu(st=%ld) keyidx_mismatch=%lu mesh_config_sent=%s(%lu)\r\n",
                    (unsigned long)c->beacon_refused, (long)c->beacon_status,
                    (unsigned long)c->sta_refused[0], (unsigned long)c->sta_refused[1],
                    (unsigned long)c->sta_refused[2], (unsigned long)c->sta_refused[3],
                    (unsigned long)c->sta_refused[4], (long)c->sta_status,
                    (unsigned long)c->key_refused, (long)c->key_status,
                    (unsigned long)c->keyidx_mismatch, meshcfg_sent_name_(c->meshcfg_mode),
                    (unsigned long)c->meshcfg_mode);
}

/* AT+MESHCFG? -- every value a peer matches on, plus what this node will not
 * do. Interop failures are mismatches, and comparing them one AT verb at a
 * time is how they get missed; the capability lines are here so nobody has to
 * infer them from behaviour. */
static void cmd_meshcfg(void)
{
    extern int g_warthog_chan_pin_status;
    extern uint32_t g_warthog_applied_freq_hz;
    extern uint16_t g_warthog_applied_chan;
    extern int16_t  g_warthog_applied_gclass, g_warthog_applied_sclass;
    extern uint8_t  g_warthog_applied_bw_mhz;
    extern volatile uint32_t g_warthog_rxchan_beacon;

    char line[320];
    char id[WARTHOG_CFG_MESH_ID_MAXLEN + 1] = {0};
    char pw[WARTHOG_CFG_MESH_PASS_MAXLEN + 1] = {0};
    warthog_cfg_get_mesh_id(id, sizeof(id));
    warthog_cfg_get_mesh_pass(pw, sizeof(pw));

    snprintf(line, sizeof(line), "+MESHCFG: region=%s country=%s\r\n",
             WARTHOG_REGION_NAME, WARTHOG_COUNTRY_CODE);
    cdc_write(line);
    snprintf(line, sizeof(line),
             "+MESHCFG: enable=%u secure=%u pmf=%s dhcp=%u fwd=%u bridge=%u grp=%s batman=%u id='%s' "
             "pass=%u chars\r\n",
             (unsigned)warthog_cfg_get_mesh_enable(), (unsigned)warthog_cfg_get_mesh_secure(),
             warthog_cfg_get_mesh_pmf() ? "required" : "off",
             (unsigned)warthog_cfg_get_mesh_dhcp(), (unsigned)warthog_cfg_get_mesh_fwd(),
             (unsigned)warthog_cfg_get_mesh_bridge(),
             warthog_cfg_get_mesh_grp() ? "std" : "replicate",
             (unsigned)warthog_cfg_get_mesh_batman(), id, (unsigned)strlen(pw));
    cdc_write(line);
    if (g_warthog_applied_chan == 0) {
        snprintf(line, sizeof(line),
                 "+MESHCFG: applied chan=NONE -- full %s regulatory list, operating "
                 "channel not pinned and not observable (set_channel_list=%d)\r\n",
                 WARTHOG_COUNTRY_CODE, g_warthog_chan_pin_status);
    } else {
        snprintf(line, sizeof(line),
                 "+MESHCFG: applied chan=%u freq=%lu bw=%u gclass=%d sclass=%d (set_channel_list=%d)\r\n",
                 (unsigned)g_warthog_applied_chan, (unsigned long)g_warthog_applied_freq_hz,
                 (unsigned)g_warthog_applied_bw_mhz, (int)g_warthog_applied_gclass,
                 (int)g_warthog_applied_sclass, g_warthog_chan_pin_status);
    }
    cdc_write(line);
    unsigned peers = (unsigned)mmwlan_mesh_get_peer_count();
    snprintf(line, sizeof(line), "+MESHCFG: peers=%u beacons_heard=%lu\r\n",
             peers, (unsigned long)g_warthog_rxchan_beacon);
    cdc_write(line);
    {
        struct meshcfg_chipvif c = {
            .type = g_warthog_chipvif_type,
            .vif_id = g_warthog_chipvif_id,
            .fallback = g_warthog_chipvif_fallback,
            .add_status = g_warthog_chipvif_add_status,
            .bssid_refused = g_warthog_chipcmd_bssid_refused,
            .bssid_status = g_warthog_chipcmd_bssid_status,
            .meshcfg_refused = g_warthog_chipcmd_meshcfg_refused,
            .meshcfg_status = g_warthog_chipcmd_meshcfg_status,
#if defined(WARTHOG_MESH_CHIP_VIF_MESH) && WARTHOG_MESH_CHIP_VIF_MESH
            .built_mesh = true,
#endif
            .beacon_refused = g_warthog_chipcmd_beacon_refused,
            .beacon_status = g_warthog_chipcmd_beacon_status,
            .sta_status = g_warthog_chipcmd_sta_status,
            .key_refused = g_warthog_chipcmd_key_refused,
            .key_status = g_warthog_chipcmd_key_status,
            .keyidx_mismatch = g_warthog_chipcmd_keyidx_mismatch,
            .meshcfg_mode = g_warthog_chipcmd_meshcfg_mode,
        };
        for (unsigned i = 0; i < 5; i++) {
            c.sta_refused[i] = g_warthog_chipcmd_sta_refused[i];
        }
        meshcfg_chipvif_line_(line, sizeof(line), &c);
        cdc_write(line);
        meshcfg_chipcmd_line_(line, sizeof(line), &c);
        cdc_write(line);
    }
    {
        struct warthog_mesh_diag_in in = {
            .peers           = peers,
            .chan_pin_status = g_warthog_chan_pin_status,
            .applied_chan    = g_warthog_applied_chan,
            .beacons_heard   = g_warthog_rxchan_beacon,
        };
        warthog_mesh_diag_windowed(&in);
        snprintf(line, sizeof(line), "+MESHCFG: %s\r\n",
                 warthog_mesh_diag_text(warthog_mesh_diagnose(&in)));
        cdc_write(line);
    }
    /* Stated, not implied. Each of these is a real limitation an OpenMANET
     * operator will otherwise discover on the drone. */
    struct meshcfg_mode m = {
        .fwd = g_warthog_mesh_fwd != 0,
        /* The live bridge, not the stored flag: a failed start falls back to NAT. */
        .bridge_active = warthog_mesh_bridge_active(),
        .bridge_stored = g_warthog_mesh_bridge != 0,
        .bat_running = warthog_bat_port_running(),
        .bat_stored = warthog_cfg_get_mesh_batman() != 0,
        .bat_reason = bat_mode_reason_text((enum bat_mode_reason)warthog_bat_port_reason()),
        .neigh = warthog_bat_port_neighs(),
        .routes = warthog_bat_port_routes(),
        .grp_std = g_warthog_mesh_grp != 0,
        .copies = warthog_bat_port_bcast_copies(),
        .tput_override = warthog_bat_port_tput_override(),
    };
    warthog_bat_port_hard_mac(m.self);
    warthog_bat_port_soft_mac(m.soft);
    meshcfg_mode_line_(line, sizeof(line), &m);
    cdc_write(line);
    if (m.bat_running) {
        meshcfg_bat_line_(line, sizeof(line), &m);
        cdc_write(line);
    }
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
    char buf[240];
    snprintf(buf, sizeof(buf),
             "+HWMPSTAT: rx=%lu preq_rx=%lu preq_tx=%lu prep_rx=%lu prep_tx=%lu "
             "parse_fail=%lu not_ours=%lu rann_rx=%lu perr_rx=%lu\r\n",
             (unsigned long)g_warthog_hwmp_rx, (unsigned long)g_warthog_hwmp_preq_rx,
             (unsigned long)g_warthog_hwmp_preq_tx, (unsigned long)g_warthog_hwmp_prep_rx,
             (unsigned long)g_warthog_hwmp_prep_tx,
             (unsigned long)g_warthog_hwmp_parse_fail, (unsigned long)g_warthog_hwmp_not_ours,
             (unsigned long)g_warthog_hwmp_rann_rx, (unsigned long)g_warthog_hwmp_perr_rx);
    cdc_write(buf);
    reply_ok();
}
/* AT+MACSTATS? (core 1, the MAC) or AT+MACSTATS=<core>[,1] -- every counter in the chip's stats
 * blob as tag=value, reset after reading with ,1. The blob is the TLV stream morse_cli decodes. */
static void cmd_macstats(uint32_t core, bool reset)
{
    struct mmwlan_morse_stats *s = mmwlan_get_morse_stats(core, reset);
    if (s == NULL || s->buf == NULL) {
        mmwlan_free_morse_stats(s);
        reply_error("stats unavailable");
        return;
    }
    char line[224];
    snprintf(line, sizeof(line), "+MACSTATS: core=%lu len=%lu\r\n", (unsigned long)core,
             (unsigned long)s->len);
    cdc_write(line);
    int off = 0;
    const uint8_t *p = s->buf;
    uint32_t left = s->len;
    while (left >= 4u) {
        uint16_t tag = (uint16_t)(p[0] | (p[1] << 8));
        uint16_t len = (uint16_t)(p[2] | (p[3] << 8));
        if ((uint32_t)len + 4u > left) {
            break;
        }
        uint64_t v = 0;
        for (uint16_t b = 0; b < len && b < 8u; b++) {
            v |= (uint64_t)p[4 + b] << (8u * b);
        }
        if (off == 0) {
            off = snprintf(line, sizeof(line), "+MACSTATS:");
        }
        off += snprintf(line + off, sizeof(line) - (size_t)off, len <= 8u ? " %u=%llu" : " %u=len%u",
                        (unsigned)tag, len <= 8u ? (unsigned long long)v : (unsigned long long)len);
        if (off > (int)sizeof(line) - 40) {
            snprintf(line + off, sizeof(line) - (size_t)off, "\r\n");
            cdc_write(line);
            off = 0;
        }
        p += 4u + len;
        left -= 4u + len;
    }
    if (off > 0) {
        snprintf(line + off, sizeof(line) - (size_t)off, "\r\n");
        cdc_write(line);
    }
    mmwlan_free_morse_stats(s);
    reply_ok();
}

/* AT+FRAG=<n> / AT+FRAG?: the chip's TX fragmentation threshold (0 off, else >= 256). Not kept
 * across a reboot; a chip restart puts it back. */
static unsigned s_frag_threshold;
static void cmd_frag_set(const char *args)
{
    unsigned n = 0;
    if (sscanf(args, "%u", &n) != 1) {
        reply_error("usage: AT+FRAG=<0|256..>");
        return;
    }
    enum mmwlan_status st = mmwlan_set_fragment_threshold(n);
    if (st != MMWLAN_SUCCESS) {
        char why[48];
        snprintf(why, sizeof(why), "fragment threshold refused (%d)", (int)st);
        reply_error(why);
        return;
    }
    s_frag_threshold = n;
    reply_ok();
}

static void cmd_frag_query(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "+FRAG: %u\r\n", s_frag_threshold);
    cdc_write(buf);
    reply_ok();
}

static void cmd_filtstat(void)
{
    char buf[240];
    snprintf(buf, sizeof(buf),
             "+FILTSTAT: drop=%lu last=%lu | short_fc=%lu rts=%lu beacon=%lu short_hdr=%lu "
             "no_ops=%lu unknown_sender=%lu sa_is_us=%lu dup=%lu not_ours=%lu\r\n",
             (unsigned long)g_warthog_filt_drop, (unsigned long)g_warthog_filt_reason,
             (unsigned long)g_warthog_filt_hist[1], (unsigned long)g_warthog_filt_hist[2],
             (unsigned long)g_warthog_filt_hist[3], (unsigned long)g_warthog_filt_hist[4],
             (unsigned long)g_warthog_filt_hist[5], (unsigned long)g_warthog_filt_hist[6],
             (unsigned long)g_warthog_filt_hist[7], (unsigned long)g_warthog_filt_hist[8],
             (unsigned long)g_warthog_filt_hist[9]);
    cdc_write(buf);
    int off = snprintf(buf, sizeof(buf), "+FILTSTAT: mgmt_nours=%lu last=",
                       (unsigned long)g_warthog_filt_mgmt_nours);
    for (int i = 0; i < 16; i++)
        off += snprintf(buf + off, sizeof(buf) - off, "%02x", g_warthog_filt_mgmt_nours_hdr[i]);
    snprintf(buf + off, sizeof(buf) - off, "\r\n");
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
/* Why a request to the umac event loop was not posted (mmwlan_force_chip_restart, mmwlan_assert_test). */
static const char *loop_post_error_(enum mmwlan_status st)
{
    return st == MMWLAN_NO_MEM ? "event queue full, try again" : "chip not running";
}

/* AT+CHIPRESTART: the event loop fails the next chip health check, so the chip restarts as after a
 * real failure; with the driver stopped the loop drops the request (dropped in AT+CHIPRESTART?). */
static void cmd_chiprestart(void)
{
    const enum mmwlan_status st = mmwlan_force_chip_restart();
    if (st != MMWLAN_SUCCESS) {
        char why[48];
        snprintf(why, sizeof(why), "%s (%d)", loop_post_error_(st), (int)st);
        reply_error(why);
        return;
    }
    cdc_write("+CHIPRESTART: requested; AT+CHIPRESTART? reads the outcome\r\n");
    reply_ok();
}

static const char *reset_reason_name_(esp_reset_reason_t rr)
{
    switch (rr) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_USB: return "USB";
    default: return "OTHER";
    }
}

/* AT+ASSERT?: one MMOSAL_ASSERT record kept in .noinit; tools/assert_fileid.py names fileid's file,
 * addr2line on the build's ELF names pc (the assert) and lr (its caller). */
static int assert_line_(char *buf, size_t len, uint32_t num, const struct mmosal_failure_info *r)
{
    return snprintf(buf, len,
                    "+ASSERT: #%lu pc=0x%08lx lr=0x%08lx line=%lu fileid=0x%08lx "
                    "info=0x%08lx,0x%08lx,0x%08lx,0x%08lx\r\n",
                    (unsigned long)num, (unsigned long)r->pc, (unsigned long)r->lr,
                    (unsigned long)r->line, (unsigned long)r->fileid,
                    (unsigned long)r->platform_info[0], (unsigned long)r->platform_info[1],
                    (unsigned long)r->platform_info[2], (unsigned long)r->platform_info[3]);
}

static void cmd_assert_query(void)
{
    struct mmosal_failure_info rec[WARTHOG_ASSERT_RECORDS_MAX];
    uint32_t kept = 0;
    const uint32_t count = warthog_assert_records(rec, WARTHOG_ASSERT_RECORDS_MAX, &kept);
    char line[160];
    snprintf(line, sizeof(line), "+ASSERT: count=%lu kept=%lu reset=%s up_s=%lu crash_boots=%lu safe=%u\r\n",
             (unsigned long)count, (unsigned long)kept, reset_reason_name_(esp_reset_reason()),
             (unsigned long)(esp_timer_get_time() / 1000000),
             (unsigned long)warthog_boot_crash_count(), (unsigned)warthog_boot_safe());
    cdc_write(line);
    for (uint32_t i = 0; i < kept; i++) {
        assert_line_(line, sizeof(line), count - kept + i, &rec[i]);
        cdc_write(line);
    }
    reply_ok();
}

/* AT+ASSERTTEST=<at|loop|crit|hang>: an MMOSAL_ASSERT on the AT task, the umac event loop, or the AT task
 * in a critical section; =hang also stops the next crash boots before the HaLow start (boot watchdog). */
static void cmd_asserttest(const char *a)
{
    if (strcasecmp(a, "loop") == 0) {
        const enum mmwlan_status st = mmwlan_assert_test();
        if (st != MMWLAN_SUCCESS) {
            char why[48];
            snprintf(why, sizeof(why), "%s (%d)", loop_post_error_(st), (int)st);
            reply_error(why);
            return;
        }
        char line[64];
        snprintf(line, sizeof(line), "+ASSERTTEST: the umac event loop asserts in %u ms\r\n",
                 (unsigned)MMWLAN_ASSERT_TEST_DELAY_MS);
        cdc_write(line);
        reply_ok();
        return;
    }
    const bool crit = strcasecmp(a, "crit") == 0;
    const bool hang = strcasecmp(a, "hang") == 0;
    if (!crit && !hang && strcasecmp(a, "at") != 0) {
        reply_error("usage: AT+ASSERTTEST=<at|loop|crit|hang>");
        return;
    }
    cdc_write(crit ? "+ASSERTTEST: the AT task asserts in a critical section\r\n"
              : hang ? "+ASSERTTEST: the AT task asserts; the next boots stop before the HaLow start\r\n"
                     : "+ASSERTTEST: the AT task asserts\r\n");
    reply_ok();
    vTaskDelay(pdMS_TO_TICKS(MMWLAN_ASSERT_TEST_DELAY_MS)); /* the reply reaches the host first */
    if (crit) {
        MMOSAL_TASK_ENTER_CRITICAL();
        MMOSAL_ASSERT_LOG_DATA(false, MMWLAN_ASSERT_TEST_CRIT);
        MMOSAL_TASK_EXIT_CRITICAL();
    }
    if (hang) {
        warthog_boot_arm_hang();
        MMOSAL_ASSERT_LOG_DATA(false, MMWLAN_ASSERT_TEST_HANG);
    }
    MMOSAL_ASSERT_LOG_DATA(false, MMWLAN_ASSERT_TEST_AT);
}

/* AT+CHIPRESTART?: restarts handled, forced, put back in full; what they put back and not. */
struct chiprestartstat {
    uint32_t n, forced, dropped, mesh, sta, stafail, keys, keyfail, cmdfail, retried, pending, ms;
};

static int chiprestart_line_(char *buf, size_t len, const struct chiprestartstat *s)
{
    return snprintf(buf, len,
                    "+CHIPRESTART: restarts=%lu forced=%lu dropped=%lu mesh=%lu | sta=%lu stafail=%lu "
                    "keys=%lu keyfail=%lu cmdfail=%lu | retried=%lu pending=%lu | last_ms=%lu\r\n",
                    (unsigned long)s->n, (unsigned long)s->forced, (unsigned long)s->dropped,
                    (unsigned long)s->mesh, (unsigned long)s->sta, (unsigned long)s->stafail,
                    (unsigned long)s->keys, (unsigned long)s->keyfail, (unsigned long)s->cmdfail,
                    (unsigned long)s->retried, (unsigned long)s->pending, (unsigned long)s->ms);
}

static void cmd_chiprestart_query(void)
{
    const struct chiprestartstat s = {
        .n = g_warthog_chiprestart_n, .forced = g_warthog_chiprestart_forced,
        .dropped = g_warthog_chiprestart_dropped,
        .mesh = g_warthog_chiprestart_mesh, .sta = g_warthog_chiprestart_sta,
        .stafail = g_warthog_chiprestart_stafail, .keys = g_warthog_chiprestart_keys,
        .keyfail = g_warthog_chiprestart_keyfail, .cmdfail = g_warthog_chiprestart_cmdfail,
        .retried = g_warthog_chiprestart_retried, .pending = g_warthog_chiprestart_pending,
        .ms = g_warthog_chiprestart_ms,
    };
    char line[256];
    chiprestart_line_(line, sizeof(line), &s);
    cdc_write(line);
    reply_ok();
}

/* AT+DEFRAG? -- fragment reassembly on the receive path. */
struct defragstat {
    uint32_t in, ok, nofirst, order, pn, key, prot, hdr, amsdu, oversize, nomem, mcast, plain;
    uint32_t shape, expired, restart, flush, evict;
};

static int defragstat_line_(char *buf, size_t len, const struct defragstat *s)
{
    return snprintf(buf, len,
                    "+DEFRAG: in=%lu ok=%lu | drop nofirst=%lu order=%lu pn=%lu key=%lu prot=%lu "
                    "hdr=%lu amsdu=%lu oversize=%lu nomem=%lu mcast=%lu plain=%lu shape=%lu "
                    "| chain expired=%lu restart=%lu flush=%lu evict=%lu\r\n",
                    (unsigned long)s->in, (unsigned long)s->ok, (unsigned long)s->nofirst,
                    (unsigned long)s->order, (unsigned long)s->pn, (unsigned long)s->key,
                    (unsigned long)s->prot, (unsigned long)s->hdr, (unsigned long)s->amsdu,
                    (unsigned long)s->oversize, (unsigned long)s->nomem, (unsigned long)s->mcast,
                    (unsigned long)s->plain, (unsigned long)s->shape, (unsigned long)s->expired,
                    (unsigned long)s->restart, (unsigned long)s->flush, (unsigned long)s->evict);
}

static void cmd_defragstat(void)
{
    char line[352];
    const struct defragstat s = {
        .in = g_warthog_defrag_in, .ok = g_warthog_defrag_ok, .nofirst = g_warthog_defrag_nofirst,
        .order = g_warthog_defrag_order, .pn = g_warthog_defrag_pn, .key = g_warthog_defrag_key,
        .prot = g_warthog_defrag_prot, .hdr = g_warthog_defrag_hdr, .amsdu = g_warthog_defrag_amsdu,
        .oversize = g_warthog_defrag_oversize, .nomem = g_warthog_defrag_nomem,
        .mcast = g_warthog_defrag_mcast, .plain = g_warthog_defrag_plain,
        .shape = g_warthog_defrag_shape, .expired = g_warthog_defrag_expired, .restart = g_warthog_defrag_restart,
        .flush = g_warthog_defrag_flush, .evict = g_warthog_defrag_evict,
    };
    defragstat_line_(line, sizeof(line), &s);
    cdc_write(line);
    reply_ok();
}

/* AT+HOSTFRAG? -- host TX fragmentation. */
struct hostfragstat {
    uint32_t mode, stored, chip;
    uint32_t msdu, frags, by_thresh, by_chip, by_rate, many, pool, seal, drv;
    uint32_t acked, noack, unsent, agg, ok, fail, wait, mgmt, stale, overlap;
    uint32_t trim, chippn, ba_end, nodelba, ba_wait, ba_late, hold, held, hold_ms;
    uint32_t last_n, last_len, last_lim, last_rate;
    uint32_t rule, cap_trim, cap_sub, clamp, sealfit, seal_trim, seal_sub, seal_nofit;
    uint32_t txparm, ba_rcpt, delba_noack;
    uint32_t grp_trim, grp_sub, grp_nofit, seal_ba;
};

/* Most fragments AT+HOSTFRAG cuts a frame into on this build, 0 never (umac_datapath_private.h). */
static uint32_t hostfrag_rule_(void)
{
#if defined(WARTHOG_MESH_HOSTFRAG_ANY)
    return 16u;
#elif defined(WARTHOG_MESH_HOST_CCMP)
    return 0u;
#else
    return 2u;
#endif
}

static const char *hostfrag_mode_(uint32_t v, char *buf, size_t len)
{
    if (v == 0u) {
        return "off";
    }
    if (v == 1u) {
        return "auto";
    }
    snprintf(buf, len, "%lu", (unsigned long)v);
    return buf;
}

/* Line @p which (0 to 3) of AT+HOSTFRAG?. */
static int hostfragstat_line_(char *buf, size_t len, const struct hostfragstat *s, int which)
{
    char m[12], st[12], rate[24];
    if (which == 3) {
        return snprintf(buf, len,
                        "+HOSTFRAG: cap_trim=%lu cap_sub=%lu clamp=%lu | sealfit=%lu seal_trim=%lu "
                        "seal_sub=%lu seal_nofit=%lu seal_ba=%lu grp_trim=%lu grp_sub=%lu grp_nofit=%lu | "
                        "tidparams=%lu ba_rcpt=%lu delba_noack=%lu\r\n",
                        (unsigned long)s->cap_trim, (unsigned long)s->cap_sub, (unsigned long)s->clamp,
                        (unsigned long)s->sealfit, (unsigned long)s->seal_trim, (unsigned long)s->seal_sub,
                        (unsigned long)s->seal_nofit, (unsigned long)s->seal_ba,
                        (unsigned long)s->grp_trim, (unsigned long)s->grp_sub,
                        (unsigned long)s->grp_nofit, (unsigned long)s->txparm, (unsigned long)s->ba_rcpt,
                        (unsigned long)s->delba_noack);
    }
    if (which == 0) {
        char rule[16] = "off";
        if (s->rule != 0u) {
            snprintf(rule, sizeof(rule), "max%lu", (unsigned long)s->rule);
        }
        return snprintf(buf, len,
                        "+HOSTFRAG: mode=%s stored=%s rule=%s atfrag=%lu | msdu=%lu frag=%lu by thresh=%lu "
                        "chip=%lu rate=%lu | whole many=%lu pool=%lu | drop seal=%lu drv=%lu\r\n",
                        hostfrag_mode_(s->mode, m, sizeof(m)), hostfrag_mode_(s->stored, st, sizeof(st)),
                        rule, (unsigned long)s->chip, (unsigned long)s->msdu, (unsigned long)s->frags,
                        (unsigned long)s->by_thresh, (unsigned long)s->by_chip,
                        (unsigned long)s->by_rate, (unsigned long)s->many,
                        (unsigned long)s->pool, (unsigned long)s->seal, (unsigned long)s->drv);
    }
    if (which == 1) {
        return snprintf(buf, len,
                        "+HOSTFRAG: txst acked=%lu noack=%lu unsent=%lu agg=%lu | msdu ok=%lu fail=%lu | "
                        "held wait=%lu mgmt=%lu stale=%lu | overlap=%lu\r\n",
                        (unsigned long)s->acked, (unsigned long)s->noack, (unsigned long)s->unsent,
                        (unsigned long)s->agg, (unsigned long)s->ok, (unsigned long)s->fail,
                        (unsigned long)s->wait, (unsigned long)s->mgmt, (unsigned long)s->stale,
                        (unsigned long)s->overlap);
    }
    if (s->last_rate == 0xffffu) {
        snprintf(rate, sizeof(rate), "none");
    } else {
        snprintf(rate, sizeof(rate), "%luM/MCS%lu", (unsigned long)((s->last_rate >> 8) & 0xffffu),
                 (unsigned long)(s->last_rate & 0xffu));
    }
    return snprintf(buf, len,
                    "+HOSTFRAG: trim=%lu chippn=%lu | ba_end=%lu nodelba=%lu ba_wait=%lu ba_late=%lu "
                    "hold=%lu held=%lu hold_ms=%lu | last n=%lu len=%lu lim=%lu at=%s\r\n",
                    (unsigned long)s->trim, (unsigned long)s->chippn, (unsigned long)s->ba_end,
                    (unsigned long)s->nodelba, (unsigned long)s->ba_wait, (unsigned long)s->ba_late,
                    (unsigned long)s->hold, (unsigned long)s->held, (unsigned long)s->hold_ms,
                    (unsigned long)s->last_n, (unsigned long)s->last_len, (unsigned long)s->last_lim, rate);
}

static void cmd_hostfragstat(void)
{
    char line[304];
    const struct hostfragstat s = {
        .mode = hostfrag_rule_() != 0u ? g_warthog_hostfrag : 0u, /* as this build applies it */
        .stored = warthog_cfg_get_mesh_hostfrag(), .chip = s_frag_threshold,
        .msdu = g_warthog_hostfrag_msdu, .frags = g_warthog_hostfrag_frags,
        .by_thresh = g_warthog_hostfrag_by_thresh, .by_chip = g_warthog_hostfrag_by_chip,
        .by_rate = g_warthog_hostfrag_by_rate,
        .many = g_warthog_hostfrag_many, .pool = g_warthog_hostfrag_pool,
        .seal = g_warthog_hostfrag_seal, .drv = g_warthog_hostfrag_drv,
        .acked = g_warthog_hostfrag_acked, .noack = g_warthog_hostfrag_noack,
        .unsent = g_warthog_hostfrag_unsent, .agg = g_warthog_hostfrag_agg,
        .ok = g_warthog_hostfrag_ok, .fail = g_warthog_hostfrag_fail,
        .wait = g_warthog_hostfrag_wait, .mgmt = g_warthog_hostfrag_mgmt,
        .stale = g_warthog_hostfrag_stale, .overlap = g_warthog_hostfrag_overlap,
        .trim = g_warthog_hostfrag_trim, .chippn = g_warthog_hostfrag_chippn,
        .ba_end = g_warthog_hostfrag_ba_end, .nodelba = g_warthog_hostfrag_nodelba,
        .ba_wait = g_warthog_hostfrag_ba_wait, .ba_late = g_warthog_hostfrag_ba_late,
        .hold = g_warthog_hostfrag_hold,
        .held = g_warthog_hostfrag_held, .hold_ms = g_warthog_hostfrag_hold_ms,
        .last_n = g_warthog_hostfrag_last_n,
        .last_len = g_warthog_hostfrag_last_len, .last_lim = g_warthog_hostfrag_last_lim,
        .last_rate = g_warthog_hostfrag_last_rate,
        .rule = hostfrag_rule_(), .cap_trim = g_warthog_hostfrag_cap_trim,
        .cap_sub = g_warthog_hostfrag_cap_sub, .clamp = g_warthog_hostfrag_clamp,
        .sealfit = g_warthog_sealfit, .seal_trim = g_warthog_sealfit_trim,
        .seal_sub = g_warthog_sealfit_sub, .seal_nofit = g_warthog_sealfit_nofit,
        .txparm = g_warthog_ba_txparm,
        .ba_rcpt = g_warthog_hostfrag_ba_rcpt, .delba_noack = g_warthog_hostfrag_delba_noack,
        .grp_trim = g_warthog_grpfit_trim, .grp_sub = g_warthog_grpfit_sub, .grp_nofit = g_warthog_grpfit_nofit,
        .seal_ba = g_warthog_sealfit_ba,
    };
    for (int i = 0; i < 4; i++) {
        hostfragstat_line_(line, sizeof(line), &s, i);
        cdc_write(line);
    }
    reply_ok();
}

/* AT+AMPDU?: the setting in force and in NVS; originator sessions agreed now, and ended by =0;
 * Block Ack frames sent, and the peer's DELBAs for our sessions. */
struct ampdustat {
    uint32_t mode, stored, orig, ended, unsent;
    uint32_t addba_tx, delba_to, delba_end, delba_other, rx_delba, rx_reason;
};

static int ampdu_line_(char *buf, size_t len, const struct ampdustat *s)
{
    return snprintf(buf, len,
                    "+AMPDU: mode=%s stored=%s | orig=%lu ended=%lu unsent=%lu | addba_tx=%lu "
                    "delba_to=%lu delba_end=%lu delba_other=%lu | rx_delba=%lu rx_reason=%lu\r\n",
                    s->mode != 0u ? "on" : "off", s->stored != 0u ? "on" : "off",
                    (unsigned long)s->orig, (unsigned long)s->ended, (unsigned long)s->unsent,
                    (unsigned long)s->addba_tx, (unsigned long)s->delba_to, (unsigned long)s->delba_end,
                    (unsigned long)s->delba_other, (unsigned long)s->rx_delba,
                    (unsigned long)s->rx_reason);
}

/* TIDs 0-7 of bitmap @p m as a list ("0,5"), "-" for none. */
static void tid_list_(char *buf, size_t len, uint32_t m)
{
    size_t w = 0;
    buf[0] = '\0';
    for (unsigned t = 0; t < 8u && w + 3u < len; t++) {
        if ((m & (1u << t)) != 0u) {
            w += (size_t)snprintf(buf + w, len - w, w != 0u ? ",%u" : "%u", t);
        }
    }
    if (w == 0u) {
        snprintf(buf, len, "-");
    }
}

/* One AT+AMPDU? peer line: @p mac as published (bit 24 in use), @p ba its sessions per TID. */
static int ampdu_peer_line_(char *buf, size_t len, uint32_t mac, uint32_t ba)
{
    char ours[16], asked[16], theirs[16], held[16];
    tid_list_(ours, sizeof(ours), ba & 0xffu);
    tid_list_(asked, sizeof(asked), (ba >> 8) & 0xffu);
    tid_list_(theirs, sizeof(theirs), (ba >> 16) & 0xffu);
    tid_list_(held, sizeof(held), (ba >> 24) & 0xffu);
    return snprintf(buf, len, "+AMPDU: peer=%02lx:%02lx:%02lx ours=%s asked=%s theirs=%s held=%s\r\n",
                    (unsigned long)((mac >> 16) & 0xffu), (unsigned long)((mac >> 8) & 0xffu),
                    (unsigned long)(mac & 0xffu), ours, asked, theirs, held);
}

static void cmd_ampdu_query(void)
{
    const struct ampdustat s = {
        .mode = g_warthog_ampdu, .stored = warthog_cfg_get_mesh_ampdu(),
        .orig = g_warthog_ampdu_orig, .ended = g_warthog_ampdu_ended, .unsent = g_warthog_ampdu_unsent,
        .addba_tx = g_warthog_ba_addba_tx, .delba_to = g_warthog_ba_delba_to,
        .delba_end = g_warthog_ba_delba_end, .delba_other = g_warthog_ba_delba_other,
        .rx_delba = g_warthog_ba_rx_delba, .rx_reason = g_warthog_ba_rx_reason,
    };
    char line[224];
    ampdu_line_(line, sizeof(line), &s);
    cdc_write(line);
    for (int i = 0; i < 4; i++) {
        if ((g_warthog_ampdu_peer_mac[i] & 0x1000000u) != 0u) {
            ampdu_peer_line_(line, sizeof(line), g_warthog_ampdu_peer_mac[i], g_warthog_ampdu_peer_ba[i]);
            cdc_write(line);
        }
    }
    reply_ok();
}

/* AT+RXCAP / AT+TXCAP: <mode>[,<mac>], the mac as 12 hex digits with or without ':' or '-'. The glue
 * guard runs it. */
static bool cap_args_parse_(const char *a, uint32_t max_mode, uint32_t *mode, bool *has_mac, uint8_t mac[6])
{
    if (a == NULL || a[0] < '0' || a[0] > (char)('0' + max_mode) || (a[1] != '\0' && a[1] != ',')) {
        return false;
    }
    *mode = (uint32_t)(a[0] - '0');
    *has_mac = a[1] == ',';
    if (!*has_mac) {
        return true;
    }
    const char *p = a + 2;
    for (unsigned i = 0; i < 6u; i++) {
        unsigned v = 0;
        for (unsigned k = 0; k < 2u; k++, p++) {
            const char c = *p;
            const int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                          (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (d < 0) {
                return false;
            }
            v = (v << 4) | (unsigned)d;
        }
        mac[i] = (uint8_t)v;
        if (i < 5u && (*p == ':' || *p == '-')) {
            p++;
        }
    }
    return *p == '\0';
}

static void cmd_cap_set(unsigned dir, char *args)
{
    uint32_t mode = 0;
    bool has = false;
    uint8_t mac[6];
    if (!cap_args_parse_(trim(args), dir == MMWLAN_CAP_TX ? MMWLAN_CAP_ALL_DATA : MMWLAN_CAP_HOST_FRAG,
                         &mode, &has, mac)) {
        reply_error(dir == MMWLAN_CAP_TX ? "usage: AT+TXCAP=<0 off|1 host fragments|2 unicast data>[,<ra>]"
                                         : "usage: AT+RXCAP=<0 off|1 data frames>[,<ta>]");
        return;
    }
    int r = -1;
    for (int i = 0; i < 50 && r < 0; i++) {
        r = mmwlan_cap_arm(dir, mode, has ? mac : NULL);
        if (r < 0) {
            vTaskDelay(1);
        }
    }
    if (r <= 0) {
        reply_error(r == 0 ? "no memory for the capture ring" : "capture ring busy, try again");
        return;
    }
    reply_ok();
}

/* AT+RXCAP? / AT+TXCAP?: the state, then every kept capture oldest first, one line each. */
static void cmd_cap_query(unsigned dir)
{
    static struct mmwlan_cap_rec rec; /* AT task only */
    static char line[320];
    uint32_t seen = 0, lost = 0, after = 0;
    int n = -1;
    for (int i = 0; i < 50 && n < 0; i++) {
        n = mmwlan_cap_read(dir, 0, NULL, 0, &seen, &lost);
        if (n < 0) {
            vTaskDelay(1);
        }
    }
    uint8_t mac[6];
    char who[20] = "any";
    if (mmwlan_cap_filter(dir, mac)) {
        snprintf(who, sizeof(who), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
                 mac[4], mac[5]);
    }
    int w = snprintf(line, sizeof(line), "+%s: mode=%lu addr=%s seen=%lu lost=%lu",
                     dir == MMWLAN_CAP_TX ? "TXCAP" : "RXCAP", (unsigned long)mmwlan_cap_mode[dir], who,
                     (unsigned long)seen, (unsigned long)lost);
    if (dir == MMWLAN_CAP_TX) {
        w += snprintf(line + w, sizeof(line) - (size_t)w, " st_lost=%lu", (unsigned long)mmwlan_cap_st_lost());
    }
    snprintf(line + w, sizeof(line) - (size_t)w, "\r\n");
    cdc_write(line);
    for (unsigned k = 0; k < MMWLAN_CAP_SLOTS; k++) {
        n = -1;
        for (int i = 0; i < 50 && n < 0; i++) {
            n = mmwlan_cap_read(dir, after, &rec, 1, NULL, NULL);
            if (n < 0) {
                vTaskDelay(1);
            }
        }
        if (n <= 0) {
            break;
        }
        after = rec.seq;
        mmwlan_cap_line(line, sizeof(line), dir, &rec);
        cdc_write(line);
    }
    reply_ok();
}

/* AT+STACKS?: the least free stack each task has had, in bytes; a task not running is left out. */
static void cmd_stacks(void)
{
    /* Started by the shim; drv, spi_irq and health restart with the chip, so _min spans every instance. */
    static const char *const shim_names[] = { "evtloop", "drv", "spi_irq", "health" };
    static const char *const names[] = {
        "tiT", "warthog_at", "warthog_bat", "warthog_mudp",
        "warthog_nat", "warthog_led", "mesh-probe", "scanloop", "mcast_rx", "TinyUSB", "esp_timer",
        "Tmr Svc", "sys_evt", "wifi", "main", "ipc0", "ipc1",
    };
    static char line[512]; /* AT task only */
    int w = snprintf(line, sizeof(line), "+STACKS:");
    for (size_t i = 0; i < sizeof(shim_names) / sizeof(shim_names[0]); i++) {
        uint32_t live = UINT32_MAX, min = UINT32_MAX;
        if (!warthog_task_stack(shim_names[i], &live, &min)) {
            continue;
        }
        if (live != UINT32_MAX) {
            w += snprintf(line + w, sizeof(line) - (size_t)w, " %s=%lu", shim_names[i], (unsigned long)live);
        }
        w += snprintf(line + w, sizeof(line) - (size_t)w, " %s_min=%lu", shim_names[i],
                      (unsigned long)(live < min ? live : min));
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && w < (int)sizeof(line) - 32; i++) {
        TaskHandle_t h = xTaskGetHandle(names[i]);
        if (h == NULL) {
            continue;
        }
        const int k = w + 1;
        w += snprintf(line + w, sizeof(line) - (size_t)w, " %s=%u", names[i],
                      (unsigned)uxTaskGetStackHighWaterMark(h));
        for (int j = k; j < w && line[j] != '='; j++) {
            line[j] = line[j] == ' ' ? '_' : line[j]; /* "Tmr Svc" as one key */
        }
    }
    snprintf(line + w, sizeof(line) - (size_t)w, "\r\n");
    cdc_write(line);
    reply_ok();
}

/* AT+TXRATE: the rate override (mmwlan_ate_override_rate_control), RAM only; -1 none. */
static int s_txrate_mcs = -1, s_txrate_bw = -1;

/* AT+RXCHAN?: every counter as the line prints it. */
struct rxchanstat {
    uint32_t pages, data, beacon, mgmt, cmd, txstat, last, shim, notrunning, rxframe, filter;
    uint32_t meshctrl, ae, fwdcand, rxdrop, reason, ccmp_key, blank, replay, pn;
    uint32_t nodec_grp, nodec_uni, nodec_last_grp, nodec_fc, nodec_key;
    uint8_t fwd_da[3], nodec_ta[3];
};

static int rxchan_line_(char *buf, size_t len, const struct rxchanstat *s)
{
    return snprintf(buf, len,
                    "+RXCHAN: pages=%lu data=%lu beacon=%lu mgmt=%lu cmd=%lu txstat=%lu last=0x%02lx "
                    "| shim=%lu notrunning=%lu rxframe=%lu filter=%lu meshctrl=%lu ae=%lu fwdcand=%lu(%02x%02x%02x) | rxdrop=%lu reason=%lu "
                    "ccmp_key=%lu blank=%lu replay=%lu pn=%lu | nodec grp=%lu uni=%lu "
                    "last(grp=%lu fc=%04lx key=%lu ta=%02x%02x%02x)\r\n",
                    (unsigned long)s->pages, (unsigned long)s->data, (unsigned long)s->beacon,
                    (unsigned long)s->mgmt, (unsigned long)s->cmd, (unsigned long)s->txstat,
                    (unsigned long)s->last, (unsigned long)s->shim, (unsigned long)s->notrunning,
                    (unsigned long)s->rxframe, (unsigned long)s->filter, (unsigned long)s->meshctrl,
                    (unsigned long)s->ae, (unsigned long)s->fwdcand, s->fwd_da[0], s->fwd_da[1],
                    s->fwd_da[2], (unsigned long)s->rxdrop, (unsigned long)s->reason,
                    (unsigned long)s->ccmp_key, (unsigned long)s->blank, (unsigned long)s->replay,
                    (unsigned long)s->pn, (unsigned long)s->nodec_grp, (unsigned long)s->nodec_uni,
                    (unsigned long)s->nodec_last_grp, (unsigned long)s->nodec_fc,
                    (unsigned long)s->nodec_key, s->nodec_ta[0], s->nodec_ta[1], s->nodec_ta[2]);
}

static void cmd_rxchan(void)
{
    const struct rxchanstat s = {
        .pages = g_warthog_rxchan_pages, .data = g_warthog_rxchan_data,
        .beacon = g_warthog_rxchan_beacon, .mgmt = g_warthog_rxchan_mgmt,
        .cmd = g_warthog_rxchan_cmd, .txstat = g_warthog_rxchan_txstat,
        .last = g_warthog_rxchan_last, .shim = g_warthog_shim_rx,
        .notrunning = g_warthog_shim_rx_notrunning, .rxframe = g_warthog_rxframe_entry,
        .filter = g_warthog_filter_entry, .meshctrl = g_warthog_rx_meshctrl_stripped,
        .ae = g_warthog_rx_meshctrl_ae, .fwdcand = g_warthog_rx_fwd_candidate,
        .fwd_da = { g_warthog_rx_fwd_last_da[3], g_warthog_rx_fwd_last_da[4], g_warthog_rx_fwd_last_da[5] },
        .rxdrop = g_warthog_rxdrop_count, .reason = g_warthog_rxdrop_reason,
        .ccmp_key = g_warthog_ccmp_last_keyid, .blank = g_warthog_ccmp_blank,
        .replay = g_warthog_ccmp_replay, .pn = g_warthog_ccmp_last_pn,
        .nodec_grp = g_warthog_nodec_group_n, .nodec_uni = g_warthog_nodec_uni_n,
        .nodec_last_grp = g_warthog_nodec_group, .nodec_fc = g_warthog_nodec_fc,
        .nodec_key = g_warthog_nodec_keyid,
        .nodec_ta = { g_warthog_nodec_ta[3], g_warthog_nodec_ta[4], g_warthog_nodec_ta[5] },
    };
    static char buf[512]; /* AT task only; 468 at its longest */
    rxchan_line_(buf, sizeof(buf), &s);
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
            cdc_write_nowait(line);
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
/* AT+REKEY=<n> -- re-push peer n's own pairwise key (none on an open mesh). See
 * umac_datapath_mesh_service_rekey(): the one-slot-vs-per-station probe. */
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
    /* An abort's text: for an MMOSAL_ASSERT, the pc, fileid and line AT+ASSERT? shows. */
    char reason[96];
    if (esp_core_dump_get_panic_reason(reason, sizeof(reason)) == ESP_OK) {
        snprintf(buf, sizeof(buf), "+COREDUMP: reason=%s\r\n", reason);
        cdc_write(buf);
    }
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

/* AT+COREDUMP=0: erases the core-dump partition, so the next AT+COREDUMP? shows only a later panic. */
static void cmd_coredump_erase(void)
{
#if !CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    reply_error("core dump disabled in this build");
#else
    const esp_err_t err = esp_core_dump_image_erase();
    if (err != ESP_OK) {
        char why[40];
        snprintf(why, sizeof(why), "erase failed (0x%x)", err);
        reply_error(why);
        return;
    }
    cdc_write("+COREDUMP: erased\r\n");
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
    static char buf[1024]; /* static: AT commands run one at a time; glue guard checks the fit */
    snprintf(buf, sizeof(buf), "+MPMPEERS: self=%02x%02x%02x %ssae=%lu offers=%lu offer_full=%lu sae_fail=%lu plink_fail=%lu held=%lu pfail=%lu mlme=%lu auth_tx=%lu addp=%lu/%lu authsta=%lu/%lu rates=%lu apx=%lu/%lu rej=c%lu/e%lu/a%lu/r%lu stadd=%lu/%lu rxauth=%lu/%lu/%lu ampe_mtk=%lu ampe_mgtk=%lu mgtk_reinst=%lu mgtk_rsc_fail=%lu no_slot=%lu expired=%lu bcn_peer=%lu close_tx=%lu s1g_bcn=%lu/%lu new=%lu retry=%lu sa=%02x%02x%02x\r\n",
             g_warthog_mesh_self_addr[3], g_warthog_mesh_self_addr[4], g_warthog_mesh_self_addr[5],
             (const char *)g_warthog_mpm_links,
             (unsigned long)g_warthog_sae_init,
             (unsigned long)g_warthog_sae_peer_offers,
             (unsigned long)g_warthog_sae_offer_full,
             (unsigned long)g_warthog_sae_fail, (unsigned long)g_warthog_plink_fail,
             (unsigned long)g_warthog_sae_offer_held,
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
             (unsigned long)g_warthog_mgtk_reinst, (unsigned long)g_warthog_mgtk_rsc_fail,
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

/* AT+GTKSTAT? -- peers' MGTKs in the chip at their AIDs, as Linux installs them (chip-key SAE
 * builds on a MESH chip VIF), and the group frames the chip opened under them. */
struct gtkstat {
    bool build;        /* this image puts a peer's MGTK into the chip */
    uint32_t chip_vif; /* the chip VIF type the mesh runs on (5 = MESH) */
    uint32_t mode;     /* AT+GTKPERSTA */
    uint32_t inst, fail, del, delfail, tainted, rx_grp, forged, mgmt_gp, fence, taint;
    uint32_t mic_ok, mic_bad, mic_arm, micdrop, gp_micdrop;
    uint32_t slot[4], mac[4];
};

static int gtkstat_line_(char *buf, size_t len, const struct gtkstat *s)
{
    const char *mode = !s->build ? "off(build)" : s->chip_vif != 5u ? "off(sta_vif)"
                     : s->mode == 0u ? "off(at)" : s->mode == 2u ? "on(pn0)" : "on";
    int w = snprintf(buf, len,
                     "+GTKSTAT: per_sta=%s inst=%lu fail=%lu del=%lu delfail=%lu tainted=%lx "
                     "rx_grp=%lu forged=%lu mgmt_gp=%lu fence=%lu taint=%lu mic_ok=%lu mic_bad=%lu "
                     "mic_arm=%lu micdrop=%lu/%lu",
                     mode, (unsigned long)s->inst, (unsigned long)s->fail, (unsigned long)s->del,
                     (unsigned long)s->delfail, (unsigned long)s->tainted, (unsigned long)s->rx_grp,
                     (unsigned long)s->forged, (unsigned long)s->mgmt_gp, (unsigned long)s->fence,
                     (unsigned long)s->taint, (unsigned long)s->mic_ok, (unsigned long)s->mic_bad,
                     (unsigned long)s->mic_arm, (unsigned long)s->micdrop,
                     (unsigned long)s->gp_micdrop);
    for (unsigned i = 0; i < 4u && w >= 0 && (size_t)w < len; i++) {
        const uint32_t v = s->slot[i];
        w += (v & 0x80000000u) != 0u
                 ? snprintf(buf + w, len - (size_t)w, " [%06lx aid=%lu id=%lu hw=%lu]",
                            (unsigned long)(s->mac[i] & 0xffffffu), (unsigned long)((v >> 16) & 0xffu),
                            (unsigned long)((v >> 8) & 0xffu), (unsigned long)(v & 0xffu))
                 : snprintf(buf + w, len - (size_t)w, " [-]");
    }
    if (w >= 0 && (size_t)w < len) {
        w += snprintf(buf + w, len - (size_t)w, "\r\n");
    }
    return w;
}

static void cmd_gtkstat(void)
{
    static char line[512]; /* AT task only */
    struct gtkstat s = {
#if !defined(WARTHOG_MESH_AMPE_NO_CHIP_KEY) && defined(WARTHOG_MESH_CHIP_VIF_MESH) && WARTHOG_MESH_CHIP_VIF_MESH
        .build = true,
#endif
        .chip_vif = g_warthog_chipvif_type, .mode = g_warthog_peer_gtk_mode,
        .inst = g_warthog_peer_gtk_inst, .fail = g_warthog_peer_gtk_fail,
        .del = g_warthog_peer_gtk_del, .delfail = g_warthog_peer_gtk_delfail,
        .tainted = g_warthog_peer_gtk_tainted, .rx_grp = g_warthog_rx_grp_chip,
        .forged = g_warthog_rx_grp_forged, .mgmt_gp = g_warthog_mgmt_gp_chip,
        .fence = g_warthog_peer_gtk_fence, .taint = g_warthog_peer_gtk_taint,
        .mic_ok = g_warthog_rx_grp_mic_ok, .mic_bad = g_warthog_rx_grp_mic_bad,
        .mic_arm = g_warthog_rx_grp_mic_armed, .micdrop = g_warthog_rx_grp_micdrop,
        .gp_micdrop = g_warthog_mgmt_gp_micdrop,
    };
    for (unsigned i = 0; i < 4u; i++) {
        s.slot[i] = g_warthog_peer_gtk[i];
        s.mac[i] = g_warthog_peer_gtk_mac[i];
    }
    gtkstat_line_(line, sizeof(line), &s);
    cdc_write(line);
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
    uint32_t reass = 0, drop = 0, cut = 0;
    warthog_nat_frag_counts(&reass, &drop, &cut);
    snprintf(buf, sizeof(buf), "+MTU: ip_reass=%lu ip_reass_drop=%lu ip_short_drop=%lu\r\n", (unsigned long)reass,
             (unsigned long)drop, (unsigned long)cut);
    cdc_write(buf);
    reply_ok();
}

/* ---- BATMAN_V member mode (bat_port.c) ---------------------------------- */

/* The refusal an AT setter gives; NULL = allowed. */
static const char *bat_refusal_(enum bat_mode_reason r)
{
    switch (r) {
    case BAT_MODE_FWD:         return "AT+MESHFWD=1 is set; batman needs 802.11s forwarding off";
    case BAT_MODE_BRIDGE:      return "AT+MESHBRIDGE=1 is set; batman mode NATs the tethered side";
    case BAT_MODE_NO_GROUP_RX: return "this build cannot hear peers' group frames under SAE; "
                                      "use warthog-mesh-sae-swccmp or an open mesh";
    default:                   return NULL;
    }
}

/* AT+MESHBATMAN=<0|1>: persisted, next boot. */
static void cmd_meshbatman_set(char *args)
{
    char *a = trim(args);
    if ((a[0] != '0' && a[0] != '1') || a[1] != '\0') {
        reply_error("usage: AT+MESHBATMAN=<0|1>");
        return;
    }
    const uint8_t on = (uint8_t)(a[0] - '0');
    if (on) {
        /* Mesh off is not refused here: AT+MESHEN=1 may follow. */
        const char *why = bat_refusal_(bat_mode_check(1, 1, warthog_cfg_get_mesh_fwd(),
                                                      warthog_cfg_get_mesh_bridge(),
                                                      warthog_bat_port_sae_build(),
                                                      warthog_bat_port_host_ccmp_build()));
        if (why != NULL) {
            reply_error(why);
            return;
        }
    }
    if (warthog_cfg_set_mesh_batman(on) != ESP_OK) {
        reply_error("nvs write failed");
        return;
    }
    cdc_write("+MESHBATMAN: stored; takes effect on next boot (AT+RESET)\r\n");
    reply_ok();
}

static void cmd_meshbatman_query(void)
{
    char line[BAT_MODE_BAT0_LINE];
    snprintf(line, sizeof(line), "+MESHBATMAN: stored=%u running=%u reason=%s\r\n",
             (unsigned)warthog_cfg_get_mesh_batman(), warthog_bat_port_running() ? 1u : 0u,
             bat_mode_reason_text((enum bat_mode_reason)warthog_bat_port_reason()));
    cdc_write(line);
    if (warthog_bat_port_running()) {
        (void)warthog_mesh_bat0_line(line, sizeof(line));
        cdc_write(line);
    }
    reply_ok();
}

/* AT+MESHBATTP=<units of 100 kbit/s>: the whole argument, 0..4294967295; 0 = rate control. */
static void cmd_meshbattp_set(char *args)
{
    char *a = trim(args);
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(a, &end, 10);
    if (a[0] < '0' || a[0] > '9' || end == a || *end != '\0' || errno != 0 || v > 0xFFFFFFFFull) {
        reply_error("usage: AT+MESHBATTP=<units of 100 kbit/s, 0 = auto>");
        return;
    }
    if (warthog_cfg_set_mesh_battp((uint32_t)v) != ESP_OK) {
        reply_error("nvs write failed");
        return;
    }
    cdc_write("+MESHBATTP: stored; takes effect on next boot (AT+RESET)\r\n");
    reply_ok();
}

static void cmd_meshbattp_query(void)
{
    char line[48];
    const uint32_t v = warthog_cfg_get_mesh_battp();
    snprintf(line, sizeof(line), "+MESHBATTP: %lu%s\r\n", (unsigned long)v, v ? "" : " (auto)");
    cdc_write(line);
    reply_ok();
}

/* AT+BATN? AT+BATO? AT+BATTG? AT+BATTL? AT+BATSTAT? (@mac: AT+BATO= / AT+BATTG=, one node): the engine
 * renders on its own task, one BAT_RENDER_BUF chunk per round trip, until the listing ends. */
static void cmd_bat_render(enum bat_render_kind k, const uint8_t *mac)
{
    uint32_t cursor = 0;
    do {
        const uint32_t from = cursor;
        const char *out = NULL;
        int r = warthog_bat_port_render(k, mac, &cursor, &out);
        if (r == WARTHOG_BAT_RENDER_NOT_RUNNING) {
            char why[64];
            snprintf(why, sizeof(why), "batman not running (%s)",
                     bat_mode_reason_text((enum bat_mode_reason)warthog_bat_port_reason()));
            reply_error(why);
            return;
        }
        if (r != WARTHOG_BAT_RENDER_OK) {
            reply_error("batman engine busy");
            return;
        }
        cdc_write(out);
        warthog_bat_port_render_done();
        if (cursor != BAT_RENDER_DONE && cursor <= from) {
            reply_error("batman render stalled"); /* a cursor only moves forward: never loop on one */
            return;
        }
    } while (cursor != BAT_RENDER_DONE);
    reply_ok();
}

static void cmd_bat_render_mac(enum bat_render_kind k, const char *args, const char *usage)
{
    uint8_t mac[6];
    if (!bat_mode_parse_mac(args, mac)) {
        reply_error(usage);
        return;
    }
    cmd_bat_render(k, mac);
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
    } else if (strcasecmp(verb, "MESHID") == 0 && terminator == '=') {
        esp_err_t e = warthog_cfg_set_mesh_id(args);
        if (e == ESP_OK) { reply_ok(); }
        else if (e == ESP_ERR_INVALID_SIZE) { reply_error("mesh ID must be 1..32 chars"); }
        else { reply_error("nvs write failed"); }
    } else if (strcasecmp(verb, "MESHID") == 0 && terminator == '?') {
        char v[WARTHOG_CFG_MESH_ID_MAXLEN + 1] = {0};
        warthog_cfg_get_mesh_id(v, sizeof(v));
        char line[96];
        snprintf(line, sizeof(line), "+MESHID: %s\r\n", v);
        cdc_write(line);
        reply_ok();
    } else if (strcasecmp(verb, "MESHPASS") == 0 && terminator == '=') {
        esp_err_t e = warthog_cfg_set_mesh_pass(args);
        if (e == ESP_OK) { reply_ok(); }
        else if (e == ESP_ERR_INVALID_SIZE) { reply_error("passphrase must be 1..63 chars"); }
        else { reply_error("nvs write failed"); }
    } else if (strcasecmp(verb, "MESHPASS") == 0 && terminator == '?') {
        /* Length only. Echoing a passphrase to a console that mirrors logs is
         * how it ends up in a paste of an unrelated bug report. */
        char v[WARTHOG_CFG_MESH_PASS_MAXLEN + 1] = {0};
        warthog_cfg_get_mesh_pass(v, sizeof(v));
        char line[64];
        snprintf(line, sizeof(line), "+MESHPASS: set, %u chars\r\n", (unsigned)strlen(v));
        cdc_write(line);
        reply_ok();
    } else if (strcasecmp(verb, "MESHEN") == 0 && terminator == '=') {
        unsigned long v = strtoul(args, NULL, 10);
        esp_err_t e = warthog_cfg_set_mesh_enable((uint8_t)v);
        if (e == ESP_OK) {
            cdc_write("+MESHEN: stored; takes effect on next boot (AT+RESET)\r\n");
            reply_ok();
        } else {
            reply_error("usage: AT+MESHEN=<0|1>");
        }
    } else if (strcasecmp(verb, "MESHRSSI") == 0 && terminator == '=') {
        int32_t v = 0;
        if (!meshrssi_parse_(trim(args), &v)) {
            reply_error("usage: AT+MESHRSSI=<dBm -255..0>; 0 or -255 is off");
        } else if (warthog_cfg_set_mesh_rssi((int16_t)v) != ESP_OK) {
            reply_error("nvs write failed");
        } else {
            g_warthog_mesh_rssi_floor = v; /* live: read per candidate */
            char line[80];
            snprintf(line, sizeof(line), "+MESHRSSI: floor=%ld, stored, applies now\r\n", (long)v);
            cdc_write(line);
            reply_ok();
        }
    } else if (strcasecmp(verb, "MESHRSSI") == 0 && terminator == '?') {
        char line[128];
        int shown = 0;
        const int32_t fl = g_warthog_mesh_rssi_floor;
        if (fl < 0 && fl > -255) {
            snprintf(line, sizeof(line), "+MESHRSSI: floor=%ld skipped=%lu passed=%lu\r\n", (long)fl,
                     (unsigned long)g_warthog_mesh_rssi_skip, (unsigned long)g_warthog_mesh_rssi_pass);
        } else {
            snprintf(line, sizeof(line), "+MESHRSSI: floor=off skipped=%lu passed=%lu\r\n",
                     (unsigned long)g_warthog_mesh_rssi_skip, (unsigned long)g_warthog_mesh_rssi_pass);
        }
        cdc_write(line);
        for (int i = 0; i < WARTHOG_RSSI_PEERS; i++) {
            if (!s_peer_rssi[i].used) { continue; }
            snprintf(line, sizeof(line),
                     "+MESHRSSI: %02x:%02x:%02x:%02x:%02x:%02x last=%d min=%d max=%d "
                     "noise=%d snr=%d bw=%u frames=%lu\r\n",
                     s_peer_rssi[i].mac[0], s_peer_rssi[i].mac[1], s_peer_rssi[i].mac[2],
                     s_peer_rssi[i].mac[3], s_peer_rssi[i].mac[4], s_peer_rssi[i].mac[5],
                     (int)s_peer_rssi[i].last, (int)s_peer_rssi[i].min,
                     (int)s_peer_rssi[i].max, (int)s_peer_rssi[i].noise,
                     (int)(s_peer_rssi[i].last - s_peer_rssi[i].noise),
                     (unsigned)s_peer_rssi[i].bw_mhz,
                     (unsigned long)s_peer_rssi[i].frames);
            cdc_write(line);
            shown++;
        }
        if (shown == 0) {
            cdc_write("+MESHRSSI: no neighbours heard yet\r\n");
        }
        reply_ok();
    } else if (strcasecmp(verb, "MESHCHAN") == 0 && terminator == '=') {
        /* chan,freq_hz,global_class,s1g_class,bw_mhz -- one set, because class
         * and bandwidth belong to the channel. "default" clears the override. */
        if (strcasecmp(args, "default") == 0) {
            if (warthog_cfg_clear_mesh_chan() == ESP_OK) {
                cdc_write("+MESHCHAN: cleared; build-time pin on next boot\r\n");
                reply_ok();
            } else {
                reply_error("nvs erase failed");
            }
        } else {
            struct warthog_mesh_chan c = {0};
            unsigned ch = 0, gc = 0, oc = 0, bw = 0; unsigned long hz = 0;
            if (sscanf(args, "%u,%lu,%u,%u,%u", &ch, &hz, &gc, &oc, &bw) != 5) {
                reply_error("usage: AT+MESHCHAN=<chan>,<freq_hz>,<global_class>,<s1g_class>,<bw_mhz>");
            } else {
                c.chan = (uint16_t)ch; c.freq_hz = (uint32_t)hz;
                c.global_op_class = (uint8_t)gc; c.op_class = (uint8_t)oc;
                c.bw_mhz = (uint8_t)bw;
                esp_err_t e = warthog_cfg_set_mesh_chan(&c);
                if (e == ESP_OK) {
                    cdc_write("+MESHCHAN: stored; applied on next boot, and discarded "
                              "if the regulatory table rejects it\r\n");
                    reply_ok();
                } else {
                    reply_error("rejected: freq must be 750-950 MHz, bw 1|2|4|8, "
                                "chan/classes non-zero");
                }
            }
        }
    } else if (strcasecmp(verb, "MESHCHAN") == 0 && terminator == '?') {
        extern int g_warthog_chan_pin_status;
        struct warthog_mesh_chan c;
        char line[224];
        if (warthog_cfg_get_mesh_chan(&c)) {
            snprintf(line, sizeof(line),
                     "+MESHCHAN: stored chan=%u freq=%lu gclass=%u sclass=%u bw=%u | "
                     "applied=%s (set_channel_list=%d)\r\n",
                     (unsigned)c.chan, (unsigned long)c.freq_hz,
                     (unsigned)c.global_op_class, (unsigned)c.op_class, (unsigned)c.bw_mhz,
                     g_warthog_chan_pin_status == 0 ? "yes" : "NO",
                     g_warthog_chan_pin_status);
        } else {
            snprintf(line, sizeof(line),
                     "+MESHCHAN: build-time pin | applied=%s (set_channel_list=%d)\r\n",
                     g_warthog_chan_pin_status == 0 ? "yes" : "NO",
                     g_warthog_chan_pin_status);
        }
        cdc_write(line);
        reply_ok();
    } else if (strcasecmp(verb, "MESHFWD") == 0 && terminator == '=') {
        unsigned long v = strtoul(args, NULL, 10);
        if (v > 1) { /* before the cast: 257 would store 1 past the batman refusal */
            reply_error("usage: AT+MESHFWD=<0|1>");
        } else if (v == 1 && bat_mode_check(warthog_cfg_get_mesh_batman(), 1, 1, 0,
                                            warthog_bat_port_sae_build(),
                                            warthog_bat_port_host_ccmp_build()) == BAT_MODE_FWD) {
            reply_error("AT+MESHBATMAN=1 is set; batman needs it off");
        } else if (warthog_cfg_set_mesh_fwd((uint8_t)v) == ESP_OK) {
            cdc_write("+MESHFWD: stored; takes effect on next boot (AT+RESET)\r\n");
            reply_ok();
        } else { reply_error("usage: AT+MESHFWD=<0|1>"); }
    } else if (strcasecmp(verb, "MESHPATH") == 0 && terminator == '?') {
        static char big[4096];
        int w = mmwlan_mesh_fwd_render(big, sizeof(big));
        if (w <= 0) { cdc_write("+MESHPATH: (empty)\r\n"); } else { cdc_write(big); }
        reply_ok();
    } else if (strcasecmp(verb, "MESHFWDSTAT") == 0 && terminator == '?') {
        static char line[896]; /* AT task only; kept off its 4 KB stack */
        snprintf(line, sizeof(line),
                 "+MESHFWDSTAT: on=%lu fwd uni=%lu grp=%lu nomem=%lu | drop own=%lu dup=%lu ttl=%lu "
                 "nopath=%lu nofwd=%lu bad=%lu full=%lu tblfull=%lu | perr_tx=%lu preq_tx=%lu | relay preq=%lu prep=%lu perr=%lu "
                 "| pend tx=%lu drop=%lu | hold n=%lu tx=%lu drop=%lu "
                 "| hwmp prot=%lu unprotected=%lu unestab=%lu gp=%lu mmie=%lu nommie=%lu "
                 "| hwmp tx prot=%lu gp=%lu plain=%lu qdrop=%lu qfail=%lu "
                 "| mgmt prot chip=%lu host=%lu nodec=%lu grpkey=%lu "
                 "| mgmt gp nodec=%lu own=%lu key=%lu replay=%lu chip=%lu | mgmt tx chip=%lu host=%lu drop=%lu "
                 "| igtk=%lu\r\n",
                 (unsigned long)g_warthog_mesh_fwd, (unsigned long)g_warthog_fwd_uni,
                 (unsigned long)g_warthog_fwd_grp, (unsigned long)g_warthog_fwd_nomem,
                 (unsigned long)g_warthog_fwd_drop_own, (unsigned long)g_warthog_fwd_drop_dup,
                 (unsigned long)g_warthog_fwd_drop_ttl, (unsigned long)g_warthog_fwd_drop_nopath,
                 (unsigned long)g_warthog_fwd_drop_nofwd, (unsigned long)g_warthog_fwd_drop_bad,
                 (unsigned long)g_warthog_fwd_drop_full, (unsigned long)g_warthog_fwd_drop_tblfull,
                 (unsigned long)g_warthog_fwd_perr_tx, (unsigned long)g_warthog_fwd_preq_tx,
                 (unsigned long)g_warthog_hwmp_relay_preq, (unsigned long)g_warthog_hwmp_relay_prep,
                 (unsigned long)g_warthog_hwmp_relay_perr,
                 (unsigned long)g_warthog_fwd_pend_tx, (unsigned long)g_warthog_fwd_pend_drop,
                 (unsigned long)g_warthog_fwd_hold, (unsigned long)g_warthog_fwd_hold_tx,
                 (unsigned long)g_warthog_fwd_hold_drop,
                 (unsigned long)g_warthog_hwmp_prot, (unsigned long)g_warthog_hwmp_unprotected,
                 (unsigned long)g_warthog_hwmp_unestab, (unsigned long)g_warthog_hwmp_gp,
                 (unsigned long)g_warthog_hwmp_mmie, (unsigned long)g_warthog_hwmp_nommie,
                 (unsigned long)g_warthog_hwmp_tx_prot,
                 (unsigned long)g_warthog_hwmp_tx_gp, (unsigned long)g_warthog_hwmp_tx_plain,
                 (unsigned long)g_warthog_hwmp_tx_qdrop, (unsigned long)g_warthog_hwmp_tx_qfail,
                 (unsigned long)g_warthog_mgmt_prot_chip, (unsigned long)g_warthog_mgmt_prot_host,
                 (unsigned long)g_warthog_mgmt_prot_nodec, (unsigned long)g_warthog_mgmt_prot_grpkey,
                 (unsigned long)g_warthog_mgmt_gp_nodec, (unsigned long)g_warthog_mgmt_gp_own,
                 (unsigned long)g_warthog_mgmt_gp_key, (unsigned long)g_warthog_mgmt_gp_replay,
                 (unsigned long)g_warthog_mgmt_gp_chip, (unsigned long)g_warthog_mgmt_tx_chip, (unsigned long)g_warthog_mgmt_tx_host,
                 (unsigned long)g_warthog_mgmt_tx_drop, (unsigned long)g_warthog_ampe_igtk_installed);
        cdc_write(line);
        reply_ok();
    } else if (strcasecmp(verb, "MESHFWD") == 0 && terminator == '?') {
        char line[64];
        snprintf(line, sizeof(line), "+MESHFWD: %u\r\n", (unsigned)warthog_cfg_get_mesh_fwd());
        cdc_write(line);
        reply_ok();
    } else if (strcasecmp(verb, "MESHGRP") == 0 && terminator == '=') {
        unsigned long v = strtoul(args, NULL, 10);
        if (warthog_cfg_set_mesh_grp((uint8_t)v) == ESP_OK) {
            cdc_write("+MESHGRP: stored; takes effect on next boot (AT+RESET)\r\n");
            reply_ok();
        } else { reply_error("usage: AT+MESHGRP=<0|1>"); }
    } else if (strcasecmp(verb, "MESHGRP") == 0 && terminator == '?') {
        char line[64];
        snprintf(line, sizeof(line), "+MESHGRP: %u\r\n", (unsigned)warthog_cfg_get_mesh_grp());
        cdc_write(line);
        reply_ok();
    } else if (strcasecmp(verb, "MESHPMF") == 0 && terminator == '=') {
        unsigned long v = strtoul(args, NULL, 10);
        if (warthog_cfg_set_mesh_pmf((uint8_t)v) == ESP_OK) {
            cdc_write("+MESHPMF: stored; takes effect on next boot (AT+RESET)\r\n");
            reply_ok();
        } else { reply_error("usage: AT+MESHPMF=<0|1>"); }
    } else if (strcasecmp(verb, "MESHPMF") == 0 && terminator == '?') {
        char line[64];
        snprintf(line, sizeof(line), "+MESHPMF: %u\r\n", (unsigned)warthog_cfg_get_mesh_pmf());
        cdc_write(line);
        reply_ok();
    } else if (strcasecmp(verb, "MESHBRIDGE") == 0 && terminator == '=') {
        unsigned long v = strtoul(args, NULL, 10);
        if (v > 1) { /* before the cast: 257 would store 1 past the batman refusal */
            reply_error("usage: AT+MESHBRIDGE=<0|1>");
        } else if (v == 1 && bat_mode_check(warthog_cfg_get_mesh_batman(), 1, 0, 1,
                                            warthog_bat_port_sae_build(),
                                            warthog_bat_port_host_ccmp_build()) == BAT_MODE_BRIDGE) {
            reply_error("AT+MESHBATMAN=1 is set; batman needs it off");
        } else if (warthog_cfg_set_mesh_bridge((uint8_t)v) == ESP_OK) {
            cdc_write("+MESHBRIDGE: stored; takes effect on next boot (AT+RESET)\r\n");
            reply_ok();
        } else { reply_error("usage: AT+MESHBRIDGE=<0|1>"); }
    } else if (strcasecmp(verb, "MESHBRIDGE") == 0 && terminator == '?') {
        char line[64];
        snprintf(line, sizeof(line), "+MESHBRIDGE: %u\r\n", (unsigned)warthog_cfg_get_mesh_bridge());
        cdc_write(line);
        reply_ok();
    } else if (strcasecmp(verb, "MESHDHCP") == 0 && terminator == '=') {
        unsigned long v = strtoul(args, NULL, 10);
        if (warthog_cfg_set_mesh_dhcp((uint8_t)v) == ESP_OK) { reply_ok(); }
        else { reply_error("usage: AT+MESHDHCP=<0|1>"); }
    } else if (strcasecmp(verb, "MESHDHCP") == 0 && terminator == '?') {
        char line[64];
        snprintf(line, sizeof(line), "+MESHDHCP: %u\r\n",
                 (unsigned)warthog_cfg_get_mesh_dhcp());
        cdc_write(line);
        reply_ok();
    } else if (strcasecmp(verb, "MESHCFG") == 0 && terminator == '?') {
        cmd_meshcfg();
    } else if (strcasecmp(verb, "MESHBATMAN") == 0 && terminator == '=') {
        cmd_meshbatman_set(args);
    } else if (strcasecmp(verb, "MESHBATMAN") == 0 && terminator == '?') {
        cmd_meshbatman_query();
    } else if (strcasecmp(verb, "MESHBATTP") == 0 && terminator == '=') {
        cmd_meshbattp_set(args);
    } else if (strcasecmp(verb, "MESHBATTP") == 0 && terminator == '?') {
        cmd_meshbattp_query();
    } else if (strcasecmp(verb, "BATN") == 0 && terminator == '?') {
        cmd_bat_render(BAT_RENDER_NEIGH, NULL);
    } else if (strcasecmp(verb, "BATO") == 0 && terminator == '?') {
        cmd_bat_render(BAT_RENDER_ORIG, NULL);
    } else if (strcasecmp(verb, "BATO") == 0 && terminator == '=') {
        cmd_bat_render_mac(BAT_RENDER_ORIG, args, "usage: AT+BATO=<mac>");
    } else if (strcasecmp(verb, "BATTG") == 0 && terminator == '?') {
        cmd_bat_render(BAT_RENDER_TT_GLOBAL, NULL);
    } else if (strcasecmp(verb, "BATTG") == 0 && terminator == '=') {
        cmd_bat_render_mac(BAT_RENDER_TT_GLOBAL, args, "usage: AT+BATTG=<mac>");
    } else if (strcasecmp(verb, "BATTL") == 0 && terminator == '?') {
        cmd_bat_render(BAT_RENDER_TT_LOCAL, NULL);
    } else if (strcasecmp(verb, "BATSTAT") == 0 && terminator == '?') {
        cmd_bat_render(BAT_RENDER_STAT, NULL);
    } else if (strcasecmp(verb, "MESHEN") == 0 && terminator == '?') {
        char line[64];
        snprintf(line, sizeof(line), "+MESHEN: %u\r\n", (unsigned)warthog_cfg_get_mesh_enable());
        cdc_write(line);
        reply_ok();
    } else if (strcasecmp(verb, "COREDUMP") == 0 && terminator == '?') {
        cmd_coredump();
    } else if (strcasecmp(verb, "COREDUMP") == 0 && terminator == '=') {
        if (strcmp(trim(args), "0") != 0) {
            reply_error("usage: AT+COREDUMP=0");
        } else {
            cmd_coredump_erase();
        }
    } else if (strcasecmp(verb, "ASSERT") == 0 && terminator == '?') {
        cmd_assert_query();
    } else if (strcasecmp(verb, "ASSERT") == 0 && terminator == '=') {
        if (strcmp(trim(args), "0") != 0) {
            reply_error("usage: AT+ASSERT=0");
        } else {
            warthog_assert_clear();
            cdc_write("+ASSERT: cleared\r\n");
            reply_ok();
        }
    } else if (strcasecmp(verb, "ASSERTTEST") == 0 && terminator == '=') {
        cmd_asserttest(trim(args));
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
    } else if (strcasecmp(verb, "GTKSTAT") == 0 && terminator == '?') {
        cmd_gtkstat();
    } else if (strcasecmp(verb, "GTKPERSTA") == 0 && terminator == '=') {
        uint32_t v = 0;
        if (!gtkpersta_parse_(trim(args), &v)) {
            reply_error("usage: AT+GTKPERSTA=<0 off|1 on|2 on at TX PN 0>");
        } else if (warthog_cfg_set_mesh_gtk((uint8_t)v) != ESP_OK) {
            reply_error("nvs write failed");
        } else {
            g_warthog_peer_gtk_mode = v; /* live: the gate at once, the chip within a tick */
            char line[96];
            snprintf(line, sizeof(line), "+GTKPERSTA: %lu, stored, applies now\r\n", (unsigned long)v);
            cdc_write(line);
            reply_ok();
        }
    } else if (strcasecmp(verb, "GTKPERSTA") == 0 && terminator == '?') {
        char line[64];
        snprintf(line, sizeof(line), "+GTKPERSTA: %lu stored=%u\r\n",
                 (unsigned long)g_warthog_peer_gtk_mode, (unsigned)warthog_cfg_get_mesh_gtk());
        cdc_write(line);
        reply_ok();
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
    } else if (strcasecmp(verb, "SWCCMP") == 0 && terminator == '=') {
        const uint32_t on = (args != NULL && atoi(trim(args)) != 0) ? 1u : 0u;
        /* Host CCMP is the only way peers' ELP/OGM/BCAST (group frames) reach us under SAE. */
        if (!on && warthog_bat_port_running() && warthog_bat_port_host_ccmp_build()) {
            reply_error("batman is running; host CCMP must stay on");
            return;
        }
        g_warthog_host_ccmp_on = on;
        char b[64];
        snprintf(b, sizeof(b), "+SWCCMP: host ccmp %s\r\n",
                 g_warthog_host_ccmp_on ? "ON" : "OFF");
        cdc_write(b); reply_ok();
    } else if (strcasecmp(verb, "SWCCMP") == 0 && terminator == '?') {
        char b[3*32 + 222]; int off = 0;
        off += snprintf(b + off, sizeof(b) - off,
                        "+SWCCMP: on=%lu tried=%lu ok=%lu micfail=%lu tx_ok=%lu tx_fail=%lu nokey=%lu grpkey=%lu badhdr=%lu short=%lu keyid=%lu aadlen=%lu aad=",
                        (unsigned long)g_warthog_host_ccmp_on, (unsigned long)g_warthog_swccmp_tried,
                        (unsigned long)g_warthog_swccmp_ok, (unsigned long)g_warthog_swccmp_micfail,
                        (unsigned long)g_warthog_swccmp_tx_ok, (unsigned long)g_warthog_swccmp_tx_fail,
                        (unsigned long)g_warthog_swccmp_nokey, (unsigned long)g_warthog_swccmp_grpkey,
                        (unsigned long)g_warthog_swccmp_badhdr,
                        (unsigned long)g_warthog_swccmp_short, (unsigned long)g_warthog_swccmp_last_keyid,
                        (unsigned long)g_warthog_swccmp_last_aadlen);
        for (uint32_t i = 0; i < 32 && i < g_warthog_swccmp_last_aadlen && off < (int)sizeof(b) - 4; i++)
            off += snprintf(b + off, sizeof(b) - off, "%02x", g_warthog_swccmp_last_aad[i]);
        off += snprintf(b + off, sizeof(b) - off, "\r\n");
        cdc_write(b);
        off = snprintf(b, sizeof(b), "+SWCCMP: fail len=%lu keyid=%lu pn=",
                       (unsigned long)g_warthog_swccmp_fail_len,
                       (unsigned long)g_warthog_swccmp_fail_keyid);
        for (int i = 5; i >= 0; i--)
            off += snprintf(b + off, sizeof(b) - off, "%02x", g_warthog_swccmp_fail_pn[i]);
        off += snprintf(b + off, sizeof(b) - off, " hdr=");
        for (int i = 0; i < 32; i++)
            off += snprintf(b + off, sizeof(b) - off, "%02x", g_warthog_swccmp_fail_hdr[i]);
        snprintf(b + off, sizeof(b) - off, "\r\n");
        cdc_write(b); reply_ok();
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
    } else if (strcasecmp(verb, "MACSTATS") == 0 && terminator == '?') {
        cmd_macstats(1, false);
    } else if (strcasecmp(verb, "MACSTATS") == 0 && terminator == '=') {
        unsigned core = 1, rst = 0;
        (void)sscanf(args, "%u,%u", &core, &rst);
        cmd_macstats(core, rst != 0);
    } else if (strcasecmp(verb, "FRAG") == 0 && terminator == '=') {
        cmd_frag_set(args);
    } else if (strcasecmp(verb, "FRAG") == 0 && terminator == '?') {
        cmd_frag_query();
    } else if (strcasecmp(verb, "RXREORD") == 0 && terminator == '?') {
        cmd_rxreord();
    } else if (strcasecmp(verb, "CHIPRESTART") == 0 && terminator == '\0') {
        cmd_chiprestart();
    } else if (strcasecmp(verb, "CHIPRESTART") == 0 && terminator == '?') {
        cmd_chiprestart_query();
    } else if (strcasecmp(verb, "DEFRAG") == 0 && terminator == '?') {
        cmd_defragstat();
    } else if (strcasecmp(verb, "HOSTFRAG") == 0 && terminator == '=') {
        uint32_t v = 0;
        char m[12];
        if (!hostfrag_parse_(trim(args), &v)) {
            reply_error("usage: AT+HOSTFRAG=<0 off|auto|256..2346>");
        } else if (warthog_cfg_set_mesh_hostfrag(v) != ESP_OK) {
            reply_error("nvs write failed");
        } else {
            g_warthog_hostfrag = v; /* live: the next frame */
            char line[80];
            snprintf(line, sizeof(line), hostfrag_rule_() != 0u ? "+HOSTFRAG: %s, stored, applies now\r\n"
                                                                : "+HOSTFRAG: %s, stored; off on this build (host CCMP)\r\n",
                     hostfrag_mode_(v, m, sizeof(m)));
            cdc_write(line);
            reply_ok();
        }
    } else if (strcasecmp(verb, "HOSTFRAG") == 0 && terminator == '?') {
        cmd_hostfragstat();
    } else if (strcasecmp(verb, "AMPDU") == 0 && terminator == '=') {
        const char *a = trim(args);
        if (strcmp(a, "0") != 0 && strcmp(a, "1") != 0) {
            reply_error("usage: AT+AMPDU=<0 never start a Block Ack session|1 start them (default)>");
        } else if (warthog_cfg_set_mesh_ampdu((uint8_t)(a[0] - '0')) != ESP_OK) {
            reply_error("nvs write failed");
        } else {
            g_warthog_ampdu = (uint32_t)(a[0] - '0'); /* live: the next frame; =0 ends sessions */
            cdc_write(a[0] == '1' ? "+AMPDU: on, stored, applies now\r\n" : "+AMPDU: off, stored, applies now\r\n");
            reply_ok();
        }
    } else if (strcasecmp(verb, "AMPDU") == 0 && terminator == '?') {
        cmd_ampdu_query();
    } else if (strcasecmp(verb, "SEALFIT") == 0 && terminator == '=') {
        const char *a = trim(args);
        if (strcmp(a, "0") != 0 && strcmp(a, "1") != 0) {
            reply_error("usage: AT+SEALFIT=<0 rate control's rates|1 only rates at which chip firmware 1.17.6 delivers a sealed or group frame (default)>");
        } else {
            g_warthog_sealfit = (uint32_t)(a[0] - '0'); /* RAM only: the next frame */
            reply_ok();
        }
    } else if (strcasecmp(verb, "SEALFIT") == 0 && terminator == '?') {
        char line[192];
        snprintf(line, sizeof(line),
                 "+SEALFIT: %lu seal_trim=%lu seal_sub=%lu seal_nofit=%lu seal_ba=%lu grp_trim=%lu grp_sub=%lu "
                 "grp_nofit=%lu\r\n",
                 (unsigned long)g_warthog_sealfit, (unsigned long)g_warthog_sealfit_trim,
                 (unsigned long)g_warthog_sealfit_sub, (unsigned long)g_warthog_sealfit_nofit,
                 (unsigned long)g_warthog_sealfit_ba,
                 (unsigned long)g_warthog_grpfit_trim, (unsigned long)g_warthog_grpfit_sub,
                 (unsigned long)g_warthog_grpfit_nofit);
        cdc_write(line);
        reply_ok();
    } else if (strcasecmp(verb, "TIDPARAMS") == 0 && terminator == '=') {
        const char *a = trim(args);
        if (strcmp(a, "0") != 0 && strcmp(a, "1") != 0) {
            reply_error("usage: AT+TIDPARAMS=<0 morselib's|1 morse_driver's (default)>");
        } else {
            g_warthog_ba_txparm = (uint32_t)(a[0] - '0'); /* RAM only: the next frame */
            reply_ok();
        }
    } else if (strcasecmp(verb, "TIDPARAMS") == 0 && terminator == '?') {
        cdc_write(g_warthog_ba_txparm != 0u ? "+TIDPARAMS: 1\r\n" : "+TIDPARAMS: 0\r\n");
        reply_ok();
    } else if (strcasecmp(verb, "RXCAP") == 0 && terminator == '=') {
        cmd_cap_set(MMWLAN_CAP_RX, args);
    } else if (strcasecmp(verb, "RXCAP") == 0 && terminator == '?') {
        cmd_cap_query(MMWLAN_CAP_RX);
    } else if (strcasecmp(verb, "TXCAP") == 0 && terminator == '=') {
        cmd_cap_set(MMWLAN_CAP_TX, args);
    } else if (strcasecmp(verb, "TXCAP") == 0 && terminator == '?') {
        cmd_cap_query(MMWLAN_CAP_TX);
    } else if (strcasecmp(verb, "STACKS") == 0 && terminator == '?') {
        cmd_stacks();
    } else if (strcasecmp(verb, "TXRATE") == 0 && terminator == '=') {
        int mcs = -1, bw = -1;
        if (!txrate_parse_(trim(args), &mcs, &bw)) {
            reply_error("usage: AT+TXRATE=<mcs 0-9>,<1|2|4|8 MHz> or AT+TXRATE=off");
        } else if (mmwlan_ate_override_rate_control((enum mmwlan_mcs)mcs, (enum mmwlan_bw)bw,
                                                    MMWLAN_GI_NONE) != MMWLAN_SUCCESS) {
            reply_error("rate override refused");
        } else {
            s_txrate_mcs = mcs;
            s_txrate_bw = bw;
            reply_ok();
        }
    } else if (strcasecmp(verb, "TXRATE") == 0 && terminator == '?') {
        char line[48];
        if (s_txrate_mcs < 0) {
            snprintf(line, sizeof(line), "+TXRATE: off\r\n");
        } else {
            snprintf(line, sizeof(line), "+TXRATE: MCS%d %d MHz\r\n", s_txrate_mcs, s_txrate_bw);
        }
        cdc_write(line);
        reply_ok();
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
            g_warthog_cdc.stalled = false; /* a new session: wait for this host */
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
                    cdc_write_nowait("\b \b");
                }
            } else if (c == '\r' || c == '\n') {
                cdc_write_nowait("\r\n");
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
                cdc_write_nowait(echo);
            } else {
                /* Overflow. Reject the line AND everything up to the next
                 * terminator: without the discard state the bytes past the
                 * limit accumulated into a fresh buffer and the next newline
                 * DISPATCHED that fragment, so one over-long line produced an
                 * error and then ran whatever happened to trail it. */
                discarding = true;
                len = 0;
                cdc_write_nowait("\r\n");
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
