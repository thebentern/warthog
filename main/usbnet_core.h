/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WARTHOG_USBNET_CORE_H
#define WARTHOG_USBNET_CORE_H

/* The USB network class's one owner: every TinyUSB network call runs in the TinyUSB task.
 * Freestanding (libc only), so the host tests run it against TinyUSB's own NCM and ECM drivers. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USBNET_TXQ_N 16u       /* frames waiting for the TinyUSB task; a 14000-byte ping reply is 10 */
#define USBNET_FRAME_MAX 1514u /* Ethernet header + MTU 1500 */

struct usbnet_port {
    /* TinyUSB task only. */
    bool (*can_xmit)(uint16_t len);
    void (*xmit)(void *ref, uint16_t len); /* tud_network_xmit_cb copies ref */
    void (*recv_renew)(void);
    int (*input)(void *frame, uint16_t len); /* takes frame either way; 0 = accepted */
    /* Any task. */
    bool (*ready)(void);
    void (*kick)(void); /* run usbnet_kicked() in the TinyUSB task */
    void (*lock)(void); /* short; no port call is made while it is held */
    void (*unlock)(void);
    uint32_t (*now_ms)(void);
    void *(*alloc)(size_t n);
    void (*free)(void *p);
    bool ncm; /* NCM's receive callback contract (true = consumed); false = ECM's */
};

struct usbnet_frame {
    uint8_t *buf;
    uint16_t len;
    uint32_t t_ms;
};

struct usbnet_stats {
    uint32_t tx_queued, tx_sent, drop_full, drop_nolink, drop_nomem, drop_bad; /* tx_sent: into the class */
    uint32_t txq, txq_hw, tx_stall_ms; /* tx_stall_ms: age of the oldest queued frame, 0 when empty */
    uint32_t tx_busy_ms; /* the class has held a frame this long with no IN transfer finishing, 0 when idle */
    uint32_t rx, rx_xfer, rx_nomem, rx_err, kicks;
    uint32_t rx_idle_ms; /* since the last frame from the host, 0 before the first */
};

struct usbnet {
    const struct usbnet_port *port;
    struct usbnet_frame q[USBNET_TXQ_N]; /* ring, under lock */
    unsigned head, n;
    bool kick_pending; /* a kick is queued and has not started: at most one is */
    bool rx_seen, tx_busy; /* tx_busy: the class took a frame no idle IN completion has followed */
    uint32_t rx_ms, tx_busy_t;
    struct usbnet_stats st; /* under lock */
};

void usbnet_init(struct usbnet *u, const struct usbnet_port *port);

/* Any task (lwIP's linkoutput). Copies @p frame; never blocks beyond port->kick. 0 = queued. */
int usbnet_tx(struct usbnet *u, const void *frame, size_t len);

/* TinyUSB task: the kick, after each of the class's transfers completes, and on detach.
 * @p ep_busy: the class driver armed @p ep_addr again (an IN one still holds frames). */
void usbnet_kicked(struct usbnet *u);
void usbnet_xfer_done(struct usbnet *u, uint8_t ep_addr, bool ep_busy);
void usbnet_flush(struct usbnet *u);

/* TinyUSB task: tud_network_recv_cb and tud_network_xmit_cb. */
bool usbnet_rx(struct usbnet *u, const uint8_t *src, uint16_t len);
uint16_t usbnet_xmit_copy(uint8_t *dst, const void *ref, uint16_t len);

/* Any task. */
void usbnet_stats(struct usbnet *u, struct usbnet_stats *out);

#endif
