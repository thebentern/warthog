/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "usbnet_core.h"

#include <string.h>

void usbnet_init(struct usbnet *u, const struct usbnet_port *port)
{
    memset(u, 0, sizeof(*u));
    u->port = port;
}

int usbnet_tx(struct usbnet *u, const void *frame, size_t len)
{
    const struct usbnet_port *p = u->port;
    const bool bad = len == 0 || len > USBNET_FRAME_MAX, down = !bad && !p->ready();
    p->lock();
    const bool full = !bad && !down && u->n == USBNET_TXQ_N; /* no copy for a full queue */
    u->st.drop_full += full;
    p->unlock();
    if (full) {
        return -1;
    }
    uint8_t *copy = bad || down ? NULL : p->alloc(len);
    if (copy != NULL) {
        memcpy(copy, frame, len);
    }
    const uint32_t now = p->now_ms();
    bool queued = false, kick = false;
    p->lock();
    if (bad) {
        u->st.drop_bad++;
    } else if (down) {
        u->st.drop_nolink++;
    } else if (copy == NULL) {
        u->st.drop_nomem++;
    } else if (u->n == USBNET_TXQ_N) { /* filled since the check above */
        u->st.drop_full++;
    } else {
        u->q[(u->head + u->n) % USBNET_TXQ_N] = (struct usbnet_frame){copy, (uint16_t)len, now};
        if (++u->n > u->st.txq_hw) {
            u->st.txq_hw = u->n;
        }
        u->st.tx_queued++;
        queued = true;
        kick = !u->kick_pending;
        u->kick_pending = true;
    }
    p->unlock();
    if (!queued) {
        if (copy != NULL) {
            p->free(copy);
        }
        return -1;
    }
    if (kick) {
        p->kick();
    }
    return 0;
}

/* Only the TinyUSB task pops, so the head frame stays valid outside the lock. */
static void pop_(struct usbnet *u, bool sent, uint32_t now)
{
    const struct usbnet_port *p = u->port;
    p->lock();
    uint8_t *buf = u->q[u->head].buf;
    u->head = (u->head + 1) % USBNET_TXQ_N;
    u->n--;
    if (sent) {
        u->st.tx_sent++;
        if (!u->tx_busy) {
            u->tx_busy = true;
            u->tx_busy_t = now;
        }
    } else {
        u->st.drop_nolink++;
    }
    p->unlock();
    p->free(buf);
}

static void pump_(struct usbnet *u)
{
    const struct usbnet_port *p = u->port;
    for (;;) {
        p->lock();
        const bool have = u->n > 0;
        const struct usbnet_frame f = have ? u->q[u->head] : (struct usbnet_frame){0};
        p->unlock();
        if (!have) {
            return;
        }
        if (!p->ready()) {
            pop_(u, false, 0);
            continue;
        }
        /* Full: the next completion of the class's IN endpoint pumps again. */
        if (!p->can_xmit(f.len)) {
            return;
        }
        p->xmit(f.buf, f.len);
        pop_(u, true, p->now_ms());
    }
}

void usbnet_kicked(struct usbnet *u)
{
    u->port->lock();
    u->kick_pending = false;
    u->st.kicks++;
    u->port->unlock();
    pump_(u);
}

void usbnet_xfer_done(struct usbnet *u, uint8_t ep_addr, bool ep_busy)
{
    const uint32_t now = u->port->now_ms();
    u->port->lock();
    if ((ep_addr & 0x80u) == 0) {
        u->st.rx_xfer++; /* the class's one OUT endpoint: an NTB (ECM: a frame) */
    } else {
        /* Progress; armed again means it holds more. The notification endpoint
         * completes only on link changes. */
        u->tx_busy = ep_busy;
        u->tx_busy_t = now;
    }
    u->port->unlock();
    pump_(u);
}

void usbnet_flush(struct usbnet *u)
{
    u->port->lock();
    u->tx_busy = false; /* detached: the class was reset with whatever it held */
    u->port->unlock();
    for (;;) {
        u->port->lock();
        const bool have = u->n > 0;
        u->port->unlock();
        if (!have) {
            return;
        }
        pop_(u, false, 0);
    }
}

bool usbnet_rx(struct usbnet *u, const uint8_t *src, uint16_t len)
{
    const struct usbnet_port *p = u->port;
    void *copy = len > 0 ? p->alloc(len) : NULL;
    const bool got = copy != NULL;
    bool refused = false;
    if (got) {
        memcpy(copy, src, len);
        refused = p->input(copy, len) != 0; /* takes copy either way */
    }
    const uint32_t now = p->now_ms();
    p->lock();
    if (len > 0) {
        u->rx_seen = true;
        u->rx_ms = now;
        u->st.rx_nomem += !got;
        u->st.rx += got;
        u->st.rx_err += refused;
    }
    p->unlock();
    /* NCM hands up one datagram per renew: without this the rest of a multi-datagram NTB waits. */
    if (p->ncm) {
        p->recv_renew();
    }
    return p->ncm;
}

uint16_t usbnet_xmit_copy(uint8_t *dst, const void *ref, uint16_t len)
{
    memcpy(dst, ref, len);
    return len;
}

/* A stamp taken after @p now (another task, between our clock read and the lock) is age 0, not ~2^32. */
static uint32_t age_(uint32_t now, uint32_t t)
{
    const uint32_t d = now - t;
    return d < 0x80000000u ? d : 0;
}

void usbnet_stats(struct usbnet *u, struct usbnet_stats *out)
{
    const uint32_t now = u->port->now_ms();
    u->port->lock();
    *out = u->st;
    out->txq = u->n;
    out->tx_stall_ms = u->n > 0 ? age_(now, u->q[u->head].t_ms) : 0;
    out->tx_busy_ms = u->tx_busy ? age_(now, u->tx_busy_t) : 0;
    out->rx_idle_ms = u->rx_seen ? age_(now, u->rx_ms) : 0;
    u->port->unlock();
}
