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
#include "soc/soc.h"            /* REG_WRITE */
#include "tinyusb_cdc_acm.h"
#include "tusb.h"

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
    snprintf(buf, sizeof(buf), "+USB: ip=" IPSTR " mounted=%d\r\n",
             IP2STR(&usb_ip.ip), tud_mounted() ? 1 : 0);
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
    if (warthog_cfg_set_halow(ssid, psk) != ESP_OK) {
        reply_error("nvs write");
        return;
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
    if (warthog_cfg_set_ap(ssid, psk, channel) != ESP_OK) {
        reply_error("nvs write");
        return;
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
    esp_restart();
}

/* Phase 5a — alternate download-mode entry independent of the 1200bps shim.
 * Use this when the host needs to flash and the CDC line-coding trick isn't
 * working (e.g., macOS not propagating SET_LINE_CODING in some configurations).
 * Sets the same RTC bit the 1200bps path sets, then restarts. ROM bootloader
 * skips the app and enters USB-Serial-JTAG download mode. */
static void cmd_dlmode(void)
{
    cdc_write("+INFO: entering ROM download mode\r\n");
    reply_ok();
    vTaskDelay(pdMS_TO_TICKS(200));
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
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
    } else {
        reply_error("unknown command");
    }
}

static void at_task(void *arg)
{
    (void)arg;
    char line[AT_LINE_MAX];
    size_t len = 0;
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
                line[len] = '\0';
                dispatch(line);
                len = 0;
            } else if (len + 1 < sizeof(line)) {
                line[len++] = (char)c;
                char echo[2] = {(char)c, '\0'};
                cdc_write(echo);
            } else {
                /* Overflow — drop line. */
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
