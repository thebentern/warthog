/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bench only, compiled to nothing unless the build defines WARTHOG_USBDIAG=1 (tools/bench/README.md,
 * "The H6 tests"). Never in a release image.
 *
 * -DWARTHOG_USBDIAG=1 -DWARTHOG_USBDIAG_DEST=\"<host ip on the mesh LAN>\"
 *     Four times a second, sends the USB OTG controller's registers and TinyUSB's view of the console
 *     endpoints to UDP port 5005 over the HaLow interface, so a board whose USB has stopped can still be
 *     read (tools/bench/h6/usb_diag_listen.py logs them).
 * -DWARTHOG_USBDIAG_SPAM=1 (with the above)
 *     Also writes a 90-byte line to the console every tick from a task on core 1 while a host has the port
 *     open: the console load of the H6 stress (tools/bench/h6/usb_stress.py). */
#ifdef WARTHOG_USBDIAG
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "tusb.h"
#include "device/usbd_pvt.h"

#include "cdc_out.h"
#include "usb_net.h"
#include "usbnet_core.h"

#ifndef WARTHOG_USBDIAG_DEST
#error "WARTHOG_USBDIAG needs WARTHOG_USBDIAG_DEST, the address the register stream is sent to"
#endif

extern struct cdc_out g_warthog_cdc;

/* ESP32-S3 USB OTG (DWC2) register block. Offsets below are the DWC2 ones. */
#define OTG 0x60080000u
#define R(off) (*(volatile uint32_t *)(OTG + (off)))
/* The console's bulk endpoints in both of main/usb_net.c's descriptors. */
#define CDC_EP_IN 0x82
#define CDC_EP_OUT 0x02

#ifdef WARTHOG_USBDIAG_SPAM
static void usbdiag_spam_task(void *arg)
{
    (void)arg;
    uint32_t k = 0;
    char l[112];
    while (1) {
        if (tud_cdc_n_connected(0)) {
            snprintf(l, sizeof(l), "spam %08lu abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvw\r\n",
                     (unsigned long)k++);
            (void)cdc_out_write_nowait(&g_warthog_cdc, l);
        }
        vTaskDelay(1);
    }
}
#endif

static void usbdiag_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(8000));
    int s = -1;
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(5005) };
    to.sin_addr.s_addr = inet_addr(WARTHOG_USBDIAG_DEST);
    static char line[1400];
    uint32_t seq = 0;
    uint32_t prev_int = 0;
    for (;;) {
        if (s < 0) {
            s = socket(AF_INET, SOCK_DGRAM, 0);
        }
        struct usbnet_stats st;
        warthog_usb_net_stats(&st);
        /* Global and device registers. */
        int n = snprintf(line, sizeof(line),
                         "D %lu up=%lu gahb=%08lx grst=%08lx gints=%08lx gintm=%08lx grxstsr=%08lx dcfg=%08lx dctl=%08lx dsts=%08lx "
                         "daint=%08lx daintm=%08lx doepmsk=%08lx diepmsk=%08lx empmsk=%08lx pcg=%08lx |",
                         (unsigned long)seq++, (unsigned long)(esp_timer_get_time() / 1000000), (unsigned long)R(0x008),
                         (unsigned long)R(0x010), (unsigned long)R(0x014), (unsigned long)R(0x018), (unsigned long)R(0x01c),
                         (unsigned long)R(0x800), (unsigned long)R(0x804), (unsigned long)R(0x808), (unsigned long)R(0x818),
                         (unsigned long)R(0x81c), (unsigned long)R(0x814), (unsigned long)R(0x810), (unsigned long)R(0x834),
                         (unsigned long)R(0xe00));
        /* Per endpoint: OUT ctl/int/tsiz/dma, IN ctl/int/tsiz/txfsts (words free in its TX FIFO). */
        for (unsigned ep = 0; ep < 6 && n < (int)sizeof(line) - 400; ep++) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, " o%u=%08lx/%08lx/%08lx/%08lx i%u=%08lx/%08lx/%08lx/%08lx", ep,
                          (unsigned long)R(0xb00 + 0x20 * ep), (unsigned long)R(0xb08 + 0x20 * ep),
                          (unsigned long)R(0xb10 + 0x20 * ep), (unsigned long)R(0xb14 + 0x20 * ep), ep,
                          (unsigned long)R(0x900 + 0x20 * ep), (unsigned long)R(0x908 + 0x20 * ep),
                          (unsigned long)R(0x910 + 0x20 * ep), (unsigned long)R(0x918 + 0x20 * ep));
        }
        /* poll=<console IN DIEPINT at the last sample>/<now>. NAK (bit 13) and "IN token with the FIFO empty"
         * (bit 4) are cleared after every sample and are masked, so the driver never sees them: if they are
         * set again, the host polled the endpoint since and the controller had nothing to send. */
        const uint32_t now_int = R(0x900 + 0x20 * (CDC_EP_IN & 0x0f) + 0x08);
        n += snprintf(line + n, sizeof(line) - (size_t)n,
                      " | mnt=%d susp=%d rdy=%d cdc=%d ls=%u wav=%u rav=%u inbusy=%d outbusy=%d stalled=%d at=%d pongs=%lu txq=%u "
                      "rx=%lu txs=%lu busyms=%lu poll=%08lx/%08lx",
                      (int)tud_mounted(), (int)tud_suspended(), (int)tud_ready(), (int)tud_cdc_n_connected(0),
                      (unsigned)tud_cdc_n_get_line_state(0), (unsigned)tud_cdc_n_write_available(0), (unsigned)tud_cdc_n_available(0),
                      (int)usbd_edpt_busy(0, CDC_EP_IN), (int)usbd_edpt_busy(0, CDC_EP_OUT), (int)g_warthog_cdc.stalled,
                      (int)eTaskGetState(xTaskGetHandle("warthog_at")), (unsigned long)warthog_usb_net_pongs(), (unsigned)st.txq,
                      (unsigned long)st.rx, (unsigned long)st.tx_sent, (unsigned long)st.tx_busy_ms, (unsigned long)prev_int,
                      (unsigned long)now_int);
        prev_int = now_int;
        R(0x900 + 0x20 * (CDC_EP_IN & 0x0f) + 0x08) = (1u << 13) | (1u << 4);
        if (s >= 0) {
            (void)sendto(s, line, (size_t)n, 0, (struct sockaddr *)&to, sizeof(to));
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void warthog_usbdiag_start(void)
{
    xTaskCreatePinnedToCore(usbdiag_task, "usbdiag", 4096, NULL, tskIDLE_PRIORITY + 3, NULL, 1);
#ifdef WARTHOG_USBDIAG_SPAM
    xTaskCreatePinnedToCore(usbdiag_spam_task, "usbdiag_spam", 3072, NULL, tskIDLE_PRIORITY + 2, NULL, 1);
#endif
}
#endif /* WARTHOG_USBDIAG */
