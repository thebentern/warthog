/*
 * TinyUSB CDC-ACM + CDC-NCM composite, backed by an esp_netif.
 *
 * esp_tinyusb 2.x ships default descriptors only for CDC/MSC/NCM; selecting
 * CONFIG_TINYUSB_NET_MODE_* without a custom descriptor leaves the
 * device with no functional interface, so the host enumerates nothing. We
 * supply our own composite (CDC-ACM + CDC-NCM) configuration descriptor here.
 *
 * - NCM gives macOS, Linux, Windows 10+ (usbncm.sys) and iOS/iPadOS a USB
 *   Ethernet adapter with the host's own in-box driver -- no dext, no MFi.
 *   It replaced ECM, which only macOS and Linux would bind, and which left
 *   iPads and Windows machines with no way in over the cable. RNDIS is still
 *   not offered: it would need a second USB configuration, which esp_tinyusb
 *   does not expose, and NCM covers Windows anyway.
 * - CDC-ACM rides alongside so we have a serial console after USB-OTG takes
 *   over from USB-Serial-JTAG. The ESP console is rerouted to CDC after the
 *   stack is up.
 */

#include "usb_net.h"

#include "cfg.h"
#include "dhcpserver/dhcpserver.h"   /* OFFER_DNS + dhcps_offer_t */
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_system.h"               /* esp_restart() for 1200bps DL trick */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led.h"
#include "soc/rtc_cntl_reg.h"         /* RTC_CNTL_OPTION1_REG */
#include "soc/soc.h"                  /* REG_WRITE */
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tusb.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "warthog.usb_net";

#ifndef WARTHOG_USB_GW_IP
#define WARTHOG_USB_GW_IP "192.168.4.1"
#endif
#ifndef WARTHOG_USB_NETMASK
#define WARTHOG_USB_NETMASK "255.255.255.0"
#endif

/* Host-side MAC the host sees on its USB Ethernet adapter. TinyUSB's
 * net_device.h declares this as extern non-const, so it can't be const here. */
uint8_t tud_network_mac_address[6] = {0x02, 0x02, 0x84, 0x6A, 0x96, 0x00};

/* Defined in main/at.c: morselib-style archive rules do not apply here, but
 * every other warthog counter lives there and AT+STATUS? prints these. */
extern volatile uint32_t g_warthog_usb_tx_sent, g_warthog_usb_tx_dropped;

static esp_netif_t *s_usb_netif = NULL;
static volatile bool s_cdc_ready = false;
static vprintf_like_t s_prev_log_fn = NULL;

/* Non-blocking vprintf that mirrors logs to CDC ACM 0 if connected and also
 * chains to whatever logger was installed before us (USB-Serial-JTAG path
 * during early boot — writes to its FIFO are harmless even after USB-OTG
 * takes the pins). Drops CDC bytes when the TX buffer is full — never blocks
 * the caller. The built-in tinyusb_console_init uses freopen+VFS which
 * acquires a stdio mutex and deadlocks against re-entrant log calls during
 * boot — avoid it. */
static int cdc_vprintf(const char *fmt, va_list args)
{
    va_list args_copy;
    va_copy(args_copy, args);

    int n = 0;
    if (s_cdc_ready && tud_cdc_n_connected(0)) {
        char buf[256];
        n = vsnprintf(buf, sizeof(buf), fmt, args);
        if (n > 0) {
            int len = n > (int)sizeof(buf) ? sizeof(buf) : n;
            tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)buf, len);
            tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
        }
    }

    if (s_prev_log_fn) {
        n = s_prev_log_fn(fmt, args_copy);
    }
    va_end(args_copy);
    return n;
}

/* Devloop USB reflash watchdogs.
 *
 * Phases 5a (1200bps) and 5c (DTR/RTS) install CDC callbacks that, on
 * specific host-driven events, latch the ROM's FORCE_DOWNLOAD_BOOT bit and
 * esp_restart() into bootloader mode. This is essential for the no-touch
 * dev flash loop but actively HAZARDOUS in production: any host program
 * that happens to open the port at 1200 baud, or toggle DTR/RTS in the
 * pattern esptool uses, would silently kick a user's device into bootloader.
 *
 * Both watchdogs are gated behind WARTHOG_DEVLOOP (set in the mesh-smoke env
 * via -DWARTHOG_DEVLOOP=1). Production builds (warthog-us etc.) compile them
 * out entirely so production firmware can't be bricked into bootloader by an
 * accidental serial-port quirk. AT+DLMODE remains available in production
 * for explicit field debugging — that command requires an unambiguous host
 * action and has no accidental-trigger surface.
 */
#if defined(WARTHOG_DEVLOOP) && WARTHOG_DEVLOOP

/* Startup grace period: after boot, the macOS USB stack often sends a cached
 * SET_LINE_CODING / SET_CONTROL_LINE_STATE right after CDC enumeration. If
 * the previous host session left a watchdog-tripping value cached, macOS
 * replays it on next boot, creating a bootloop. Both line-coding (1200bps)
 * and line-state (DTR/RTS) callbacks honor this period. */
#define CDC_BAUD_WATCHDOG_GRACE_MS 5000

/* — DTR/RTS state-change → ROM download mode (esptool-compatible).
 *
 * esptool's `--before default_reset` (default mode for serial bridges and the
 * mode we want to support) issues this sequence on the CDC ACM endpoint via
 * SET_CONTROL_LINE_STATE:
 *   :  DTR=0, RTS=1   "EN low"   — chip held in reset
 *   sleep 100 ms
 *   :  DTR=1, RTS=0   "EN high, BOOT low"  — boot with BOOT held
 *   sleep 50 ms
 *   :  DTR=0          "BOOT high"  — release BOOT
 *
 * On a board where DTR and RTS are physically wired to EN/IO0 (FTDI/CP2102
 * with the auto-reset cap network), puts the chip into ROM download
 * mode. We emulate that behaviour entirely in firmware: when we see the
 * → transition (i.e. RTS goes from 1 → 0 while DTR went
 * from 0 → 1), we set FORCE_DOWNLOAD_BOOT and esp_restart(). After the
 * restart the ROM bootloader skips the app and enters USB-Serial-JTAG
 * download mode where esptool can talk to it directly.
 *
 * This complements the 1200bps watchdog: that one trips on SET_LINE_CODING,
 * which esptool DOESN'T use for the default reset path; this one trips on
 * SET_CONTROL_LINE_STATE, which it DOES. With both in place, `pio run -t
 * upload` (which invokes esptool with default reset) works without any
 * preamble step on the host side.
 */
static volatile bool s_saw_reset_phase = false;

static void cdc_line_state_cb(int itf, cdcacm_event_t *event)
{
    (void)itf;
    if (event == NULL) {
        return;
    }
    bool dtr = event->line_state_changed_data.dtr;
    bool rts = event->line_state_changed_data.rts;
    ESP_LOGD(TAG, "CDC line state: dtr=%d rts=%d", (int)dtr, (int)rts);

    /* detection: DTR=0 + RTS=1 → EN low (reset asserted). Latch a
     * flag so subsequent line-state changes can be interpreted relative to
     * this point. */
    if (!dtr && rts) {
        s_saw_reset_phase = true;
        return;
    }

    /* detection: after was seen, DTR=1 + RTS=0 → EN high
     * + BOOT low. This is the bootloader-entry signal. Honor the same
     * startup grace period the 1200bps path uses, since macOS may replay
     * the last session's DTR/RTS state on enumeration. */
    if (s_saw_reset_phase && dtr && !rts) {
        uint32_t up_ms = pdTICKS_TO_MS(xTaskGetTickCount());
        if (up_ms < CDC_BAUD_WATCHDOG_GRACE_MS) {
            ESP_LOGW(TAG, "CDC DTR/RTS reset within %u ms of boot — ignoring",
                     (unsigned)up_ms);
            s_saw_reset_phase = false;
            return;
        }
        ESP_LOGW(TAG, "CDC DTR/RTS reset → entering ROM download mode");
        REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
        esp_restart();
    }

    /* Any other transition clears the phase latch — esptool may have given
     * up, or the host opened/closed the port with a different DTR/RTS
     * combination. Re-arm only on a fresh . */
    if (!(!dtr && rts)) {
        s_saw_reset_phase = false;
    }
}

/* — 1200bps-touch → ROM download mode.
 *
 * Fires on the host's SET_LINE_CODING control transfer. If the new baud is
 * exactly 1200 (the de-facto signal across Arduino/Adafruit/RP2040 stacks),
 * latch the FORCE_DOWNLOAD_BOOT bit in RTC_CNTL_OPTION1_REG and esp_restart().
 * The bit survives soft reset (RTC domain is retained); ROM bootloader reads
 * it on next boot and skips the app, entering USB-Serial-JTAG download mode
 * where esptool can write_flash without BOOT being held physically.
 *
 * Any other baud (typical 9600/57600/115200/460800/921600 etc.) is a no-op:
 * the CDC class accepts the line coding and the serial pipe stays up. There
 * is no observable side-effect to clients that just want to set a baud rate.
 */
static void cdc_line_coding_cb(int itf, cdcacm_event_t *event)
{
    (void)itf;
    if (event == NULL || event->line_coding_changed_data.p_line_coding == NULL) {
        ESP_LOGW(TAG, "CDC line coding cb: NULL event/data");
        return;
    }
    uint32_t baud = event->line_coding_changed_data.p_line_coding->bit_rate;
    /* diagnostic — log every line-coding change at WARN level until
     * we've verified the callback is being invoked at all. Drop to DEBUG once
     * the 1200bps path is confirmed working. */
    ESP_LOGW(TAG, "CDC line coding cb fired: baud=%u", (unsigned)baud);
    if (baud != 1200) {
        return;
    }
    /* Grace-period gate: ignore the first 5 seconds of cached-baud SET_LINE_
     * CODING events from a freshly-enumerated host. xTaskGetTickCount returns
     * ticks since FreeRTOS scheduler started, which is essentially boot time
     * for our purposes (app_main runs before USB enumeration completes). */
    uint32_t up_ms = pdTICKS_TO_MS(xTaskGetTickCount());
    if (up_ms < CDC_BAUD_WATCHDOG_GRACE_MS) {
        ESP_LOGW(TAG, "CDC 1200bps within %u ms of boot — ignoring (grace)",
                 (unsigned)up_ms);
        return;
    }
    /* The host opened us at 1200 baud. This is the bootloader-entry signal.
     * Log at warning level so it surfaces in any monitor regardless of level
     * config, then arm the RTC bit and restart. */
    ESP_LOGW(TAG, "CDC 1200bps touch — entering ROM download mode");
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    /* esp_restart() flushes pending TX queues and gives FreeRTOS tasks ~50ms
     * to wind down before the actual reset. That's enough for the host to
     * see the TinyUSB disconnect cleanly before the ROM bootloader's
     * USB-Serial-JTAG enumerates with a different VID:PID. */
    esp_restart();
}

#endif /* WARTHOG_DEVLOOP */

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC_INTERFACE,
    STRID_NET_INTERFACE,
    STRID_MAC,
    STRID_COUNT,
};

static char s_mac_str[13];      /* 12 hex chars MAC, used by CDC-NCM (STRID_MAC) */
static char s_serial_str[19];   /* "WTHG-<12 hex>" used by USB device iSerial */

/* iSerialNumber is set to s_serial_str ("WTHG-<MAC>" populated at runtime).
 * It needs three properties simultaneously:
 *   1) Unique per board → unique /dev/cu.usbmodem<N> ports for multi-board
 *      development. (Earlier hardcoded "0001" collided across boards.)
 *   2) Distinct from the chip's USB-Serial-JTAG bootloader iSerial (which is
 *      the bare MAC). Without the "WTHG-" prefix, macOS reuses the same port
 *      name across firmware↔bootloader transitions and the new device's
 *      descriptor isn't refreshed, producing the "zombie port" failure mode
 *      where neither AT nor esptool responds on the stale port path.
 *   3) Stable across re-enumerations of the SAME board so devloop scripts
 *      can track a board by port path.
 *
 * STRID_MAC keeps s_mac_str (12 hex chars, no separators) because the CDC-NCM
 * class spec requires that exact format for the MAC string. */
static const char *s_string_desc[STRID_COUNT] = {
    [STRID_LANGID]        = (const char[]){0x09, 0x04},
    [STRID_MANUFACTURER]  = "Warthog",
    [STRID_PRODUCT]       = "Warthog HaLow Bridge",
    [STRID_SERIAL]        = s_serial_str,
    [STRID_CDC_INTERFACE] = "Warthog Console",
    [STRID_NET_INTERFACE] = "Warthog Network",
    [STRID_MAC]           = s_mac_str,
};

static const tusb_desc_device_t s_desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    /* IAD composite class so the host sees two function groups. */
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,
    .idProduct          = 0x4020,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 1,
};

enum {
#if !WARTHOG_USB_NCM_ONLY
    ITF_NUM_CDC_CTRL = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_NET_CTRL,
#else
    ITF_NUM_NET_CTRL = 0,
#endif
    ITF_NUM_NET_DATA,
    ITF_NUM_TOTAL,
};

/* ESP32-S3 USB-OTG full-speed has 5 IN/OUT EP pairs + 1 IN EP (EP0 control).
 * Assigning EP1..4 keeps us comfortably within the budget. */
#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82
#define EPNUM_NET_NOTIF 0x83
#define EPNUM_NET_OUT   0x04
#define EPNUM_NET_IN    0x84

/* WARTHOG_USB_NCM_ONLY drops the CDC-ACM console from the composite, leaving a
 * single NCM function. Used to bisect host-side enumeration problems: it is the
 * exact shape a known-good NCM gadget presents, so if this enumerates and the
 * composite does not, the fault is in combining the two functions rather than
 * in NCM itself. Costs the AT console, so it is a diagnostic, not a default. */
#ifndef WARTHOG_USB_NCM_ONLY
#define WARTHOG_USB_NCM_ONLY 0
#endif

#if WARTHOG_USB_NCM_ONLY
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_NCM_DESC_LEN)
#else
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_CDC_NCM_DESC_LEN)
#endif

static const uint8_t s_desc_fs_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 100),
#if !WARTHOG_USB_NCM_ONLY
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_CTRL, STRID_CDC_INTERFACE,
                       EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
#endif
    TUD_CDC_NCM_DESCRIPTOR(ITF_NUM_NET_CTRL, STRID_NET_INTERFACE, STRID_MAC,
                           EPNUM_NET_NOTIF, 64,
                           EPNUM_NET_OUT, EPNUM_NET_IN, 64,
                           CFG_TUD_NET_MTU),
};

/* The host aborts enumeration if the config descriptor's declared wTotalLength
 * does not match the bytes actually emitted, and the symptom is indirect: the
 * device answers device/string descriptors (so it appears in a hub listing
 * with the right name) while the OS never creates a device for it. Catch a
 * mismatch at build time instead. */
_Static_assert(sizeof(s_desc_fs_config) == CONFIG_TOTAL_LEN,
               "USB config descriptor length does not match CONFIG_TOTAL_LEN");

bool tud_network_recv_cb(const uint8_t *src, uint16_t size)
{
    /* The return value means OPPOSITE things in the two network classes, and
     * getting it wrong fails silently in a way that looks like dead hardware.
     *
     * ECM (ecm_rndis_device.c:354, `if (!tud_network_recv_cb(...))`):
     *   false = "processed synchronously, renew the OUT endpoint now"
     *   true  = "I am holding the buffer; I will renew it myself"
     *
     * NCM (ncm_device.c:702, `if (tud_network_recv_cb(...))`):
     *   true  = "datagram accepted" -- and ONLY then does the driver advance to
     *           the next datagram in the NTB or release the NTB back to the
     *           free list.
     *   false = nothing advances. The receive path stalls on the first
     *           datagram forever and the NTB is never freed.
     *
     * So a callback written for ECM stalls NCM after one frame: the interface
     * comes up, the host sends its DHCP DISCOVER, and nothing is ever received
     * again. Under NCM we therefore return true even when we drop the frame --
     * the driver must advance either way, and a dropped datagram is free
     * (the peer retransmits) whereas a jammed NTB pipeline is not.
     *
     * We copy and hand to esp_netif_receive synchronously, so we never hold
     * the driver's buffer in either class. */
#if defined(CFG_TUD_NCM) && CFG_TUD_NCM
    const bool consumed = true;   /* advance the NTB regardless */
#else
    const bool consumed = false;  /* ECM: renew the OUT endpoint now */
#endif

    if (!s_usb_netif || size == 0) {
        return consumed;
    }
    void *copy = malloc(size);
    if (!copy) {
        return consumed;
    }
    memcpy(copy, src, size);
    if (esp_netif_receive(s_usb_netif, copy, size, NULL) != ESP_OK) {
        free(copy);
    }
    return consumed;
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg)
{
    if (!ref || arg == 0) {
        return 0;
    }
    memcpy(dst, ref, arg);
    return arg;
}

/* These two are declared (non-weak) in TinyUSB's net_device.h, so we must
 * define them even though TinyUSB's NCM path never calls them. LED state on
 * host attach/detach is driven via the tinyusb_event_cb_t hook on the
 * tinyusb_config_t instead. */
void tud_network_init_cb(void) {}
void tud_network_link_state_cb(uint8_t rhport, bool state) { (void)rhport; (void)state; }

/* NCM link state.
 *
 * ECM came up implicitly; NCM does not. The host leaves the interface down
 * until the device reports the link up on the notification endpoint, so
 * without this the gadget enumerates, binds a driver, and then sits there with
 * no address -- which reads as a broken cable rather than a missing
 * notification.
 *
 * Start DOWN and raise it only once the device is configured. iOS runs DHCP
 * exactly once on link-up and never retries, so the netif and its DHCP server
 * must already be running when this fires -- they are, because
 * warthog_usb_net_start() brings the netif up before installing the driver. */
bool tud_network_default_link_state_cb(void) { return false; }


static esp_err_t l2_transmit(void *h, void *buffer, size_t len)
{
    (void)h;
    /* tud_network_can_xmit becomes true once the host has selected alt 1 on the
     * network data interface -- the only correct "can TX" signal in either
     * class. Do not gate on a flag wired from tud_network_init_cb; neither the
     * ECM nor the NCM path calls that callback.
     *
     * KNOWN RACE, deliberately left in place. This runs on the lwIP tcpip
     * thread and drives TinyUSB's transmit state directly, which the TinyUSB
     * task also mutates from netd_xfer_cb. Under ECM that state was one buffer
     * and a flag; under NCM it is a multi-buffer NTB ring, so the window is
     * wider.
     *
     * Deferring the send into the TinyUSB task with usbd_defer_func() -- the
     * textbook fix, and what esp_tinyusb's own glue does (tinyusb_net.c:80) --
     * was tried and REVERTED: it measured 1/200 packets delivered against
     * 300/300 for this version, with tx=9 drop=0 after a 200-packet burst,
     * i.e. the deferred callback was mostly never invoked. usbd_defer_func()
     * returns void, so a full queue cannot even be detected, let alone fallen
     * back from. Do not re-apply that change without solving the queueing
     * first.
     *
     * Measured on this path: 300 packets at a 1400-byte payload, 0% loss, and
     * no observed corruption. The race is real but has never been seen to
     * bite; the deferred version was reliably broken. */
    for (int i = 0; i < 50; i++) {
        if (tud_network_can_xmit(len)) {
            tud_network_xmit(buffer, len);
            g_warthog_usb_tx_sent++;
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    g_warthog_usb_tx_dropped++;
    return ESP_ERR_TIMEOUT;
}

static void on_tinyusb_event(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        ESP_LOGI(TAG, "host attached; raising NCM link");
        warthog_led_set_usb(true);
        /* esp_tinyusb owns tud_mount_cb/tud_umount_cb, so the link is raised
         * from its event hook instead. The netif and DHCP server are already
         * running by now -- warthog_usb_net_start() brings them up before
         * installing the driver -- which matters because iOS runs DHCP exactly
         * once on link-up and never retries. */
        tud_network_link_state(0, true);
        break;
    case TINYUSB_EVENT_DETACHED:
        ESP_LOGI(TAG, "host detached; dropping NCM link");
        warthog_led_set_usb(false);
        tud_network_link_state(0, false);
        break;
    default:
        break;
    }
}

static void l2_free_rx_buffer(void *h, void *buffer)
{
    (void)h;
    free(buffer);
}

esp_netif_t *warthog_usb_net_start(void)
{
    ESP_LOGI(TAG, "init CDC+ECM");

    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY) != ESP_OK) {
        memset(mac, 0x02, sizeof(mac));
    }
    /* Locally-administered, unicast. Host-side MAC differs from device MAC in
     * the low bit so they don't collide on the same L2 segment. */
    mac[0] = 0x02;
    tud_network_mac_address[0] = mac[0];
    tud_network_mac_address[1] = mac[1];
    tud_network_mac_address[2] = mac[2];
    tud_network_mac_address[3] = mac[3];
    tud_network_mac_address[4] = mac[4];
    tud_network_mac_address[5] = mac[5] ^ 0x01;

    snprintf(s_mac_str, sizeof(s_mac_str), "%02X%02X%02X%02X%02X%02X",
             tud_network_mac_address[0], tud_network_mac_address[1],
             tud_network_mac_address[2], tud_network_mac_address[3],
             tud_network_mac_address[4], tud_network_mac_address[5]);
    /* s_serial_str = "WTHG-<MAC>" so the USB iSerial differs from the bare
     * MAC used by the chip's ROM USB-Serial-JTAG bootloader → distinct port
     * names across firmware↔bootloader transitions (see comment above). */
    snprintf(s_serial_str, sizeof(s_serial_str), "WTHG-%s", s_mac_str);

    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = esp_ip4addr_aton(WARTHOG_USB_GW_IP);
    ip.gw.addr = ip.ip.addr;
    ip.netmask.addr = esp_ip4addr_aton(WARTHOG_USB_NETMASK);

    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.if_key = "USB";
    base.if_desc = "warthog_usb";
    base.flags = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP;
    base.ip_info = &ip;
    base.route_prio = 50;

    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = (void *)1,
        .transmit = l2_transmit,
        .driver_free_rx_buffer = l2_free_rx_buffer,
    };

    esp_netif_config_t cfg = {
        .base = &base,
        .driver = &driver_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };

    s_usb_netif = esp_netif_new(&cfg);
    if (!s_usb_netif) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        return NULL;
    }

    /* Device-side MAC. Distinct from host-side tud_network_mac_address. */
    mac[5] ^= 0x02;
    esp_netif_set_mac(s_usb_netif, mac);

    /* DHCP option 6 — without this the host gets an IP + gateway but no DNS,
     * falls back to its primary-interface resolver (often a private LAN IP
     * unreachable through the HaLow NAPT chain), and `curl example.com` hangs
     * while `ping 1.1.1.1` works. Two-step API: dhcps stores the IP via
     * esp_netif_set_dns_info, but only includes DHCP option 6 in offers when
     * dhcps_dns is enabled via esp_netif_dhcps_option — set_dns_info alone is
     * a quiet no-op for clients. Set BEFORE action_start; dhcps reads both
     * when it auto-starts. */
    char dns_str[16] = {0};
    warthog_cfg_get_dns(dns_str, sizeof(dns_str));
    dhcps_offer_t offer_dns = OFFER_DNS;
    esp_netif_dhcps_option(s_usb_netif, ESP_NETIF_OP_SET,
                           ESP_NETIF_DOMAIN_NAME_SERVER,
                           &offer_dns, sizeof(offer_dns));
    esp_netif_dns_info_t dns = {
        .ip = {
            .type = ESP_IPADDR_TYPE_V4,
            .u_addr.ip4.addr = esp_ip4addr_aton(dns_str),
        },
    };
    esp_netif_set_dns_info(s_usb_netif, ESP_NETIF_DNS_MAIN, &dns);
    ESP_LOGI(TAG, "DHCP option 6 (DNS) = %s", dns_str);

    /* ESP_NETIF_FLAG_AUTOUP is consulted inside esp_netif_start, which is
     * triggered by esp_netif_action_start. Without these the underlying lwIP
     * netif is never created and the DHCP server silently no-ops.
     * action_start auto-starts the dhcps for ESP_NETIF_DHCP_SERVER netifs. */
    esp_netif_action_start(s_usb_netif, NULL, 0, NULL);
    esp_netif_action_connected(s_usb_netif, NULL, 0, NULL);

    /* esp_tinyusb 2.x validates xCoreID against the chip's core count, so
     * tskNO_AFFINITY (=0x7FFFFFFF) fails. Pin to CPU0. */
    const tinyusb_config_t tusb_cfg = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
        .task = {
            .size = 8192,
            .priority = 5,
            .xCoreID = 0,
        },
        /* Let esp_tinyusb build the CONFIGURATION descriptor.
         *
         * It already composes CDC + NCM from CFG_TUD_CDC / CFG_TUD_NCM with
         * the interface, endpoint and IAD layout the class drivers expect
         * (usb_descriptors.c). The hand-rolled composite this replaced worked
         * for ECM and was rejected outright for NCM -- the device answered its
         * device and string descriptors, so a hub listing showed the right
         * name, while the host never created a device for it.
         *
         * The DEVICE descriptor and strings stay ours, so VID/PID, product
         * name and the MAC-derived serial are unchanged; only the config
         * descriptor is handed back. */
        .descriptor = {
            .device = &s_desc_device,
            .full_speed_config = NULL,
            .string = s_string_desc,
            .string_count = STRID_COUNT,
        },
        .event_cb = &on_tinyusb_event,
    };
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
        return NULL;
    }

    /* Without tinyusb_cdcacm_init the esp_tinyusb CDC dispatcher has no
     * per-interface state, so the first host SET_LINE_CODING control transfer
     * faults. The console redirect via tinyusb_console_init's stdio freopen
     * deadlocks the stdio mutex during boot — we use a custom vprintf hook
     * that drops bytes when the CDC buffer is full instead. */
    const tinyusb_config_cdcacm_t cdc_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
#if defined(WARTHOG_DEVLOOP) && WARTHOG_DEVLOOP
        /* — 1200bps-touch reset to ROM download mode. Dev-only:
         * production builds compile out this callback so accidental 1200-baud
         * port opens can't kick the device into bootloader. */
        .callback_line_coding_changed = &cdc_line_coding_cb,
#if defined(WARTHOG_DEVLOOP_DTRRTS) && WARTHOG_DEVLOOP_DTRRTS
        /* — DTR/RTS state-machine matches esptool's `--before
         * default_reset` so that `pio run -t upload` triggers bootloader
         * entry on its own. Currently gated OFF — registering this callback
         * causes USB-OTG init to fail (chip stays in ROM USB-Serial-JTAG
         * mode). See WARTHOG_DEVLOOP_DTRRTS comment in platformio.ini. */
        .callback_line_state_changed = &cdc_line_state_cb,
#endif
#endif
    };
    if (tinyusb_cdcacm_init(&cdc_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "tinyusb_cdcacm_init failed; CDC port will be unbound");
    } else {
        s_cdc_ready = true;
        s_prev_log_fn = esp_log_set_vprintf(&cdc_vprintf);
    }

    ESP_LOGI(TAG, "up: gw=%s mask=%s host_mac=%s",
             WARTHOG_USB_GW_IP, WARTHOG_USB_NETMASK, s_mac_str);
    return s_usb_netif;
}
