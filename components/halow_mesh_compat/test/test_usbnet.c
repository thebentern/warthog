/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The USB network class with one owner (main/usbnet_core.c) against TinyUSB's own class driver,
 * compiled out of managed_components/: ncm_device.c, or ecm_rndis_device.c with -DUSBNET_TEST_ECM=1.
 * A fake usbd layer stands in for usbd.c and the DWC2: it records each endpoint transfer, a fake host
 * completes them through an event queue, and the "TinyUSB task" drains that queue as tud_task does,
 * calling the driver's netd_xfer_cb and then usbnet_xfer_done (usb_net.c's __wrap_netd_xfer_cb).
 * Every usbd call and both class callbacks count a violation when made off the owner thread.
 *
 * Measured on air 2026-10-03 (macOS, NCM, before this change): 1473-byte pings 1/3, 2000-byte 0/3,
 * then even 100-byte pings 0/3 until AT+RESET; ip_reass=2; +USB tx/drop frozen (drop=0). rx_wedge
 * replays that with the receive callback the firmware had (copy, hand up, return true, never
 * tud_network_recv_renew) and gets the same numbers: NCM hands up one datagram per renew, so each
 * two-fragment NTB leaves one behind, and once all 3 receive NTBs hold leftovers no OUT transfer is
 * armed again. A frozen tx with drop=0 is that: nothing arrives, so nothing is answered.
 */
#ifndef USBNET_TEST_ECM
#define USBNET_TEST_ECM 0
#endif
#include "tusb.h"
#include "device/usbd_pvt.h"
#if !USBNET_TEST_ECM
#include "class/net/ncm.h"
#endif
#include "usbnet_core.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if USBNET_TEST_ECM
#define CLASS "ECM"
#else
#define CLASS "NCM"
#endif

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   " CLASS " "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL " CLASS " "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* ---- the owner: every TinyUSB network call runs on this thread ---------------------------- */

static pthread_t owner;
static atomic_uint owner_bad;
static void own_(void)
{
    if (!pthread_equal(pthread_self(), owner)) {
        atomic_fetch_add(&owner_bad, 1);
    }
}

/* ---- fake usbd: endpoint transfers and the TinyUSB event queue ----------------------------- */

#define EP_NOTIF 0x81
#define EP_IN 0x82
#define EP_OUT 0x02

struct fake_ep {
    bool busy, host_done; /* armed by the class; completed by the host, event not yet run */
    uint8_t *buf;
    uint16_t len;
    unsigned xfers, refused, zlps;
};
static struct fake_ep ep_in, ep_out, ep_notif;
static struct fake_ep *ep_of_(uint8_t a)
{
    return a == EP_IN ? &ep_in : a == EP_OUT ? &ep_out : a == EP_NOTIF ? &ep_notif : NULL;
}

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER; /* fake usbd, event queue, host state */
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;

enum { EV_KICK, EV_XFER };
struct ev {
    int kind;
    uint8_t ep;
    uint32_t len;
};
#define EVQ_N 64
static struct ev evq[EVQ_N];
static unsigned evq_head, evq_n, evq_hw, evq_overflow;
static unsigned kicks_queued, kicks_queued_hw, kick_posts;

static void post_locked_(struct ev e)
{
    if (evq_n == EVQ_N) {
        evq_overflow++;
        return;
    }
    evq[(evq_head + evq_n) % EVQ_N] = e;
    if (++evq_n > evq_hw) {
        evq_hw = evq_n;
    }
    if (e.kind == EV_KICK) {
        kick_posts++;
        if (++kicks_queued > kicks_queued_hw) {
            kicks_queued_hw = kicks_queued;
        }
    }
    pthread_cond_broadcast(&g_cv);
}

static bool pop_(struct ev *e, int wait_ms)
{
    pthread_mutex_lock(&g_mx);
    if (evq_n == 0 && wait_ms > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (long)wait_ms * 1000000L;
        ts.tv_sec += ts.tv_nsec / 1000000000L;
        ts.tv_nsec %= 1000000000L;
        pthread_cond_timedwait(&g_cv, &g_mx, &ts);
    }
    const bool have = evq_n > 0;
    if (have) {
        *e = evq[evq_head];
        evq_head = (evq_head + 1) % EVQ_N;
        evq_n--;
        if (e->kind == EV_KICK) {
            kicks_queued--;
        }
    }
    pthread_mutex_unlock(&g_mx);
    return have;
}

bool usbd_edpt_open(uint8_t rhport, tusb_desc_endpoint_t const *desc_ep)
{
    own_();
    return true;
}

bool usbd_open_edpt_pair(uint8_t rhport, uint8_t const *p_desc, uint8_t ep_count, uint8_t xfer_type,
                         uint8_t *ep_out_addr, uint8_t *ep_in_addr)
{
    own_();
    for (int i = 0; i < ep_count; i++) {
        const tusb_desc_endpoint_t *d = (const tusb_desc_endpoint_t *)p_desc;
        if (tu_edpt_dir(d->bEndpointAddress) == TUSB_DIR_IN) {
            *ep_in_addr = d->bEndpointAddress;
        } else {
            *ep_out_addr = d->bEndpointAddress;
        }
        p_desc = tu_desc_next(p_desc);
    }
    return true;
}

bool usbd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buffer, uint16_t total_bytes, bool is_isr)
{
    own_();
    struct fake_ep *e = ep_of_(ep_addr);
    pthread_mutex_lock(&g_mx);
    const bool ok = e != NULL && !e->busy; /* usbd.c: TU_ASSERT on a busy endpoint */
    if (ok) {
        *e = (struct fake_ep){true, false, buffer, total_bytes, e->xfers + 1, e->refused,
                              e->zlps + (total_bytes == 0)};
        pthread_cond_broadcast(&g_cv);
    } else if (e != NULL) {
        e->refused++;
    }
    pthread_mutex_unlock(&g_mx);
    return ok;
}

bool usbd_edpt_busy(uint8_t rhport, uint8_t ep_addr)
{
    own_();
    struct fake_ep *e = ep_of_(ep_addr);
    pthread_mutex_lock(&g_mx);
    const bool b = e != NULL && e->busy;
    pthread_mutex_unlock(&g_mx);
    return b;
}

bool usbd_edpt_claim(uint8_t rhport, uint8_t ep_addr)
{
    own_();
    return true;
}

bool tud_control_xfer(uint8_t rhport, tusb_control_request_t const *request, void *buffer, uint16_t len)
{
    return true;
}

bool tud_control_status(uint8_t rhport, tusb_control_request_t const *request)
{
    return true;
}

tusb_speed_t tud_speed_get(void)
{
    return TUSB_SPEED_FULL;
}

#if USBNET_TEST_ECM
void rndis_class_set_handler(uint8_t *data, int size)
{
}
#endif

/* ---- frames: a stream id and a sequence number, the rest a pattern of both ----------------- */

static void put16_(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32_(uint8_t *p, uint32_t v) { put16_(p, (uint16_t)v); put16_(p + 2, (uint16_t)(v >> 16)); }
static uint16_t get16_(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32_(const uint8_t *p) { return get16_(p) | (uint32_t)get16_(p + 2) << 16; }

#define FRAME_MAGIC 0x54454e55u
#define FRAME_MIN 24u

static void frame_fill_(uint8_t *f, uint16_t len, uint32_t seq)
{
    memset(f, 0xff, 6);
    memset(f + 6, 0x02, 6);
    f[12] = 0x88;
    f[13] = 0xb5;
    put32_(f + 14, FRAME_MAGIC);
    put32_(f + 18, seq);
    put16_(f + 22, len);
    for (unsigned i = FRAME_MIN; i < len; i++) {
        f[i] = (uint8_t)(seq * 7u + i);
    }
}

static bool frame_ok_(const uint8_t *f, uint32_t len, uint32_t *seq)
{
    if (len < FRAME_MIN || get32_(f + 14) != FRAME_MAGIC || get16_(f + 22) != len) {
        return false;
    }
    *seq = get32_(f + 18);
    for (unsigned i = FRAME_MIN; i < len; i++) {
        if (f[i] != (uint8_t)(*seq * 7u + i)) {
            return false;
        }
    }
    return true;
}

/* ---- the fake host: completes IN transfers, sends scripted or random OUT ------------------- */

#define SEQ_MAX 40000u
static uint8_t h_arrived[SEQ_MAX];
static unsigned h_rx, h_bad, h_order, h_last_valid;
static uint32_t h_last;
static bool h_in_paused;

#define NTB_DG_MAX 6 /* wNtbOutMaxDatagrams, the most the host packs */
struct out_ntb {
    int k;
    uint16_t len[NTB_DG_MAX];
};
#define SCRIPT_N 64
static struct out_ntb script[SCRIPT_N];
static unsigned script_n, script_next, out_seq, out_sent_ntbs, out_blocked;
static atomic_bool out_random; /* the threaded test: random NTBs until out_target datagrams */
static unsigned out_target;
static uint32_t rng_state = 2463534242u;
static uint32_t rnd_(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void h_frame_(const uint8_t *f, uint32_t len)
{
    uint32_t seq;
    if (!frame_ok_(f, len, &seq) || seq >= SEQ_MAX) {
        h_bad++;
        return;
    }
    if (h_last_valid && seq <= h_last) {
        h_order++;
    }
    h_last = seq;
    h_last_valid = 1;
    h_arrived[seq]++;
    h_rx++;
}

/* What crossed the bus IN, parsed as the host's class driver would. */
static void h_take_in_(const uint8_t *b, uint32_t len)
{
    if (len == 0) {
        return; /* the ZLP after a transfer of whole packets */
    }
#if USBNET_TEST_ECM
    h_frame_(b, len);
#else
    if (len < 12 + 16 || get32_(b) != NTH16_SIGNATURE || get16_(b + 4) != 12 || get16_(b + 8) != len) {
        h_bad++;
        return;
    }
    const uint16_t ndp = get16_(b + 10);
    if (ndp + 16u > len || get32_(b + ndp) != NDP16_SIGNATURE_NCM0) {
        h_bad++;
        return;
    }
    const unsigned n = (get16_(b + ndp + 4) - 8u) / 4u;
    for (unsigned i = 0; i < n && ndp + 12u + 4u * i <= len; i++) {
        const uint16_t idx = get16_(b + ndp + 8 + 4 * i), dl = get16_(b + ndp + 10 + 4 * i);
        if (idx == 0 || dl == 0) {
            return;
        }
        if ((uint32_t)idx + dl > len) {
            h_bad++;
            return;
        }
        h_frame_(b + idx, dl);
    }
#endif
}

/* The next OUT transfer the host has queued, written into the armed buffer. 0 = none. */
static uint32_t h_build_out_(uint8_t *b, uint16_t cap)
{
    struct out_ntb s;
    if (atomic_load(&out_random)) {
        if (out_seq >= out_target) {
            return 0;
        }
#if USBNET_TEST_ECM
        s = (struct out_ntb){1, {(uint16_t)(60 + rnd_() % (1514 - 60 + 1))}};
#else
        s.k = 1 + (int)(rnd_() % NTB_DG_MAX);
        unsigned room = 3200 - 12 - 8 - 4 * (NTB_DG_MAX + 1) - 4 * NTB_DG_MAX;
        for (int i = 0; i < s.k; i++) {
            unsigned l = 60 + rnd_() % (1514 - 60 + 1);
            if (l > room) {
                l = room;
            }
            if (l < 60) {
                s.k = i;
                break;
            }
            s.len[i] = (uint16_t)l;
            room -= l;
        }
#endif
        if (out_seq + (unsigned)s.k > out_target) {
            s.k = (int)(out_target - out_seq);
        }
    } else {
        if (script_next == script_n) {
            return 0;
        }
        s = script[script_next++];
    }
#if USBNET_TEST_ECM
    if (s.len[0] > cap) {
        abort();
    }
    frame_fill_(b, s.len[0], out_seq++);
    return s.len[0];
#else
    const uint16_t ndp = 12, ndp_len = (uint16_t)(8 + 4 * (s.k + 1));
    uint32_t off = (ndp + ndp_len + 3u) & ~3u;
    put32_(b, NTH16_SIGNATURE);
    put16_(b + 4, 12);
    put16_(b + 6, (uint16_t)out_sent_ntbs);
    put16_(b + 10, ndp);
    put32_(b + ndp, NDP16_SIGNATURE_NCM0);
    put16_(b + ndp + 4, ndp_len);
    put16_(b + ndp + 6, 0);
    for (int i = 0; i < s.k; i++) {
        if (off + s.len[i] > cap) {
            abort();
        }
        frame_fill_(b + off, s.len[i], out_seq++);
        put16_(b + ndp + 8 + 4 * i, (uint16_t)off);
        put16_(b + ndp + 10 + 4 * i, s.len[i]);
        off = (off + s.len[i] + 3u) & ~3u;
    }
    put16_(b + ndp + 8 + 4 * s.k, 0);
    put16_(b + ndp + 10 + 4 * s.k, 0);
    put16_(b + 8, (uint16_t)off);
    return off;
#endif
}

static uint8_t h_in_copy[4096];

/* One pass over the endpoints, as the bus would: completions go to the event queue. */
static bool host_service_(void)
{
    bool did = false;
    pthread_mutex_lock(&g_mx);
    if (ep_in.busy && !ep_in.host_done && !h_in_paused) {
        const uint32_t in_len = ep_in.len;
        if (in_len > sizeof(h_in_copy)) {
            abort();
        }
        if (in_len > 0) {
            memcpy(h_in_copy, ep_in.buf, in_len);
        }
        h_take_in_(h_in_copy, in_len); /* parsed before the device may reuse the buffer */
        ep_in.host_done = true;
        post_locked_((struct ev){EV_XFER, EP_IN, in_len});
        did = true;
    }
    if (ep_notif.busy && !ep_notif.host_done) {
        ep_notif.host_done = true;
        post_locked_((struct ev){EV_XFER, EP_NOTIF, ep_notif.len});
        did = true;
    }
    if (ep_out.busy && !ep_out.host_done) {
        const uint32_t n = h_build_out_(ep_out.buf, ep_out.len);
        if (n > 0) {
            ep_out.host_done = true;
            out_sent_ntbs++;
            post_locked_((struct ev){EV_XFER, EP_OUT, n});
            did = true;
        }
    } else if (!atomic_load(&out_random) && script_next < script_n) {
        out_blocked++; /* the host has a transfer to send and no OUT transfer is armed */
    }
    pthread_mutex_unlock(&g_mx);
    return did;
}

/* ---- the device: the port usb_net.c gives usbnet_core.c, and the IP stack it feeds ---------- */

static struct usbnet U;
static pthread_mutex_t port_mx = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool port_held;
static atomic_uint held_call; /* a port call made with the lock held */
static atomic_bool ready = true, real_clock, input_fail_once;
static atomic_uint now_ms, alloc_fail_in, renews, allocs;

static void held_(void)
{
    if (port_held) {
        atomic_fetch_add(&held_call, 1);
    }
}

#define DLOG_N 64
static atomic_uint d_rx, d_bad, d_order;
static int64_t d_last = -1;
static uint32_t d_log[DLOG_N];

/* The IP stack: takes the frame either way, as ethernetif_input does. */
static int sink_(void *f, uint16_t len)
{
    uint32_t seq;
    int r = 0;
    if (!frame_ok_(f, len, &seq)) {
        atomic_fetch_add(&d_bad, 1);
    } else {
        if ((int64_t)seq <= d_last) {
            atomic_fetch_add(&d_order, 1);
        }
        d_last = seq;
        const unsigned i = atomic_fetch_add(&d_rx, 1);
        if (i < DLOG_N) {
            d_log[i] = seq;
        }
        if (atomic_exchange(&input_fail_once, false)) {
            r = -1;
        }
    }
    free(f);
    return r;
}

static uint32_t mono_ms_(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static bool p_can_xmit_(uint16_t len) { held_(); own_(); return tud_network_can_xmit(len); }
static void p_xmit_(void *ref, uint16_t len) { held_(); own_(); tud_network_xmit(ref, len); }
static void p_renew_(void) { held_(); own_(); atomic_fetch_add(&renews, 1); tud_network_recv_renew(); }
static int p_input_(void *f, uint16_t len) { held_(); own_(); return sink_(f, len); }
static bool p_ready_(void) { held_(); return atomic_load(&ready); }
static void p_kick_(void)
{
    held_();
    pthread_mutex_lock(&g_mx);
    post_locked_((struct ev){EV_KICK, 0, 0});
    pthread_mutex_unlock(&g_mx);
}
static void p_lock_(void) { pthread_mutex_lock(&port_mx); port_held = true; }
static void p_unlock_(void) { port_held = false; pthread_mutex_unlock(&port_mx); }
static uint32_t p_now_(void) { held_(); return atomic_load(&real_clock) ? mono_ms_() : atomic_load(&now_ms); }
static void *p_alloc_(size_t n)
{
    held_();
    unsigned a = atomic_load(&alloc_fail_in);
    atomic_fetch_add(&allocs, 1);
    if (a > 0 && atomic_fetch_sub(&alloc_fail_in, 1) == 1) {
        return NULL;
    }
    return malloc(n);
}
static void p_free_(void *p) { held_(); free(p); }

static const struct usbnet_port port = {
    .can_xmit = p_can_xmit_, .xmit = p_xmit_, .recv_renew = p_renew_, .input = p_input_,
    .ready = p_ready_, .kick = p_kick_, .lock = p_lock_, .unlock = p_unlock_, .now_ms = p_now_,
    .alloc = p_alloc_, .free = p_free_, .ncm = !USBNET_TEST_ECM,
};

/* The receive callback the firmware had before this change. */
static bool legacy_rx;

bool tud_network_recv_cb(const uint8_t *src, uint16_t size)
{
    own_();
    if (legacy_rx) {
        void *copy = malloc(size);
        memcpy(copy, src, size);
        sink_(copy, size);
        return !USBNET_TEST_ECM;
    }
    return usbnet_rx(&U, src, size);
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg)
{
    own_();
    return usbnet_xmit_copy(dst, ref, arg);
}

/* tud_task's XFER_COMPLETE, then what usb_net.c's __wrap_netd_xfer_cb adds. */
static void dispatch_(struct ev e)
{
    if (e.kind == EV_KICK) {
        usbnet_kicked(&U);
        return;
    }
    struct fake_ep *x = ep_of_(e.ep);
    pthread_mutex_lock(&g_mx);
    x->busy = false;
    pthread_mutex_unlock(&g_mx);
    netd_xfer_cb(0, e.ep, XFER_RESULT_SUCCESS, e.len);
    usbnet_xfer_done(&U, e.ep, usbd_edpt_busy(0, e.ep));
}

static void settle_(void)
{
    for (int guard = 0; guard < 200000; guard++) {
        bool did = host_service_();
        struct ev e;
        while (pop_(&e, 0)) {
            dispatch_(e);
            did = true;
        }
        if (!did) {
            return;
        }
    }
    CHECK(0, "the bus settles");
}

static uint8_t frame_buf[1514];
static int tx_(uint16_t len, uint32_t seq)
{
    frame_fill_(frame_buf, len, seq);
    return usbnet_tx(&U, frame_buf, len);
}

/* SET_INTERFACE on the data interface: alternate setting 1 carries data, 0 none. */
static void set_alt_(uint16_t alt)
{
    tusb_control_request_t rq = {0};
    rq.bmRequestType_bit.recipient = TUSB_REQ_RCPT_INTERFACE;
    rq.bmRequestType_bit.type = TUSB_REQ_TYPE_STANDARD;
    rq.bRequest = TUSB_REQ_SET_INTERFACE;
    rq.wValue = alt;
    rq.wIndex = 1;
    netd_control_xfer_cb(0, CONTROL_STAGE_SETUP, &rq);
}

static void dev_up_(void)
{
    pthread_mutex_lock(&g_mx);
    ep_in = ep_out = ep_notif = (struct fake_ep){0};
    evq_head = evq_n = evq_hw = evq_overflow = kicks_queued = kicks_queued_hw = kick_posts = 0;
    memset(h_arrived, 0, sizeof(h_arrived));
    h_rx = h_bad = h_order = h_last_valid = 0;
    h_in_paused = false;
    script_n = script_next = out_seq = out_sent_ntbs = out_blocked = 0;
    atomic_store(&out_random, false);
    pthread_mutex_unlock(&g_mx);
    atomic_store(&d_rx, 0);
    atomic_store(&d_bad, 0);
    atomic_store(&d_order, 0);
    d_last = -1;
    atomic_store(&owner_bad, 0);
    atomic_store(&held_call, 0);
    atomic_store(&ready, true);
    atomic_store(&renews, 0);
    legacy_rx = false;
    owner = pthread_self();
    usbnet_flush(&U);
    usbnet_init(&U, &port);
    netd_init();
#if USBNET_TEST_ECM
    static const uint8_t desc[] = {TUD_CDC_ECM_DESCRIPTOR(0, 0, 0, EP_NOTIF, 64, EP_OUT, EP_IN, 64, 1514)};
#else
    static const uint8_t desc[] = {TUD_CDC_NCM_DESCRIPTOR(0, 0, 0, EP_NOTIF, 64, EP_OUT, EP_IN, 64, 1514)};
#endif
    if (netd_open(0, (const tusb_desc_interface_t *)(desc + 8), sizeof(desc) - 8) != sizeof(desc) - 8) {
        CHECK(0, "netd_open takes the class descriptor");
    }
    set_alt_(1);
    settle_();
}

static struct usbnet_stats st_(void)
{
    struct usbnet_stats s;
    usbnet_stats(&U, &s);
    return s;
}

/* ---- tests ------------------------------------------------------------------------------- */

/* A 14000-byte ping reply as lwIP's ip4_frag hands it over: 10 frames back to back. */
static void test_fragment_train(void)
{
    dev_up_();
    const unsigned in_before = ep_in.xfers;
    for (uint32_t i = 0; i < 10; i++) {
        tx_(i < 9 ? 1514 : 722, i);
    }
    struct usbnet_stats s = st_();
    CHECK(s.txq == 10 && s.tx_queued == 10 && kick_posts == 1 && ep_in.xfers == in_before,
          "10 fragments from lwIP's thread: queued, one kick posted, no TinyUSB call (txq=%u kicks=%u)",
          (unsigned)s.txq, kick_posts);
    settle_();
    s = st_();
    CHECK(h_rx == 10 && h_order == 0 && h_bad == 0 && s.tx_sent == 10 && s.txq == 0 && s.tx_stall_ms == 0 &&
              s.tx_busy_ms == 0,
          "all 10 reach the host, in order and intact, the class idle again (rx=%u order=%u bad=%u sent=%u)", h_rx,
          h_order, h_bad, (unsigned)s.tx_sent);
    /* A transfer of whole 64-byte packets needs a ZLP after it: NCM 56-byte header + 1480, ECM 1472. */
    const uint16_t whole = USBNET_TEST_ECM ? 1472 : 1480;
    const uint16_t lens[] = {whole, 100, whole, whole, 60, whole};
    const unsigned zlps = ep_in.zlps;
    for (uint32_t i = 0; i < 6; i++) {
        tx_(lens[i], 10 + i);
        if (i == 2) {
            settle_();
        }
    }
    settle_();
    CHECK(h_rx == 16 && h_order == 0 && ep_in.zlps > zlps, "frames of whole packets, alone and back to back, ZLPs between (zlps=%u)",
          ep_in.zlps - zlps);
    CHECK(atomic_load(&owner_bad) == 0 && atomic_load(&held_call) == 0 && kicks_queued_hw <= 1,
          "one owner: every TinyUSB call on the TinyUSB task, none under the lock, at most one kick queued");
}

/* The host stops reading IN: the queue fills, drops are counted, and the stall shows. */
static void test_overflow(void)
{
    dev_up_();
    h_in_paused = true;
    atomic_store(&now_ms, 1000);
    unsigned ok = 0;
    const unsigned a0 = atomic_load(&allocs);
    for (uint32_t i = 0; i < 40; i++) {
        atomic_store(&now_ms, i < 8 ? 1000 : 1200); /* the 6th, the oldest left waiting, at 1000 */
        ok += tx_(1514, i) == 0;
    }
    struct usbnet_stats s = st_();
    CHECK(ok == USBNET_TXQ_N && s.drop_full == 40 - USBNET_TXQ_N && kick_posts == 1 &&
              atomic_load(&allocs) - a0 == USBNET_TXQ_N,
          "40 frames into a 16-frame queue: 16 queued, 24 dropped as full with no copy made, one kick "
          "(drop_full=%u allocs=%u)", (unsigned)s.drop_full, atomic_load(&allocs) - a0);
    atomic_store(&alloc_fail_in, 1);
    const unsigned a1 = atomic_load(&allocs);
    s = st_();
    CHECK(tx_(1514, 40) != 0 && st_().drop_full == s.drop_full + 1 && st_().drop_nomem == s.drop_nomem &&
              atomic_load(&allocs) == a1,
          "full queue and no heap: counted as full, not nomem, nothing allocated");
    atomic_store(&alloc_fail_in, 0);
    settle_();
    s = st_();
    /* NCM: 3 NTBs of 3200 B, the first sent alone, then 2 full frames each; ECM: one frame. */
    const unsigned in_class = USBNET_TEST_ECM ? 1 : 5;
    CHECK(s.txq == USBNET_TXQ_N - in_class, "%u went into the class, the rest wait (txq=%u)", in_class,
          (unsigned)s.txq);
    atomic_store(&now_ms, 2500);
    s = st_();
    CHECK(s.tx_stall_ms == 1500 && s.tx_busy_ms == 1300,
          "the oldest waiting frame's age is the stall, the class has held frames since its first, at 1200 "
          "(tx_stall_ms=%u tx_busy_ms=%u)", (unsigned)s.tx_stall_ms, (unsigned)s.tx_busy_ms);
    h_in_paused = false;
    settle_();
    s = st_();
    CHECK(h_rx == USBNET_TXQ_N && h_order == 0 && s.tx_sent == USBNET_TXQ_N && s.tx_stall_ms == 0 &&
              s.tx_busy_ms == 0,
          "the host reads again: all 16 arrive in order, both stall readings clear (rx=%u)", h_rx);
    CHECK(atomic_load(&owner_bad) == 0 && kicks_queued_hw <= 1, "one owner, at most one kick queued");
}

/* A frame queued while the pump runs, after its kick was taken, is not stranded. */
static bool inject_once;
static bool p_can_xmit_inject_(uint16_t len)
{
    if (inject_once) {
        inject_once = false;
        tx_(200, 99);
    }
    return p_can_xmit_(len);
}

static void test_no_lost_wakeup(void)
{
    dev_up_();
    struct usbnet_port inj = port;
    inj.can_xmit = p_can_xmit_inject_;
    U.port = &inj;
    inject_once = true;
    tx_(100, 98);
    settle_();
    U.port = &port;
    CHECK(h_rx == 2 && h_arrived[98] == 1 && h_arrived[99] == 1 && kick_posts == 2,
          "a frame queued mid-pump gets its own kick and is sent (rx=%u kicks=%u)", h_rx, kick_posts);
}

/* The queue fills between usbnet_tx's full check and its enqueue (another task, on the other core):
 * the late check drops the copy and counts it as full. */
static bool alloc_inject;
static void *p_alloc_inject_(size_t n)
{
    if (alloc_inject) {
        alloc_inject = false;
        uint8_t f[100];
        frame_fill_(f, sizeof(f), 77);
        usbnet_tx(&U, f, sizeof(f));
    }
    return p_alloc_(n);
}

static void test_full_race(void)
{
    dev_up_();
    h_in_paused = true;
    for (uint32_t i = 0; i < USBNET_TXQ_N - 1; i++) {
        tx_(1514, i);
    }
    struct usbnet_port inj = port;
    inj.alloc = p_alloc_inject_;
    U.port = &inj;
    alloc_inject = true;
    const int r = tx_(1514, 15);
    U.port = &port;
    struct usbnet_stats s = st_();
    CHECK(r != 0 && s.drop_full == 1 && s.txq == USBNET_TXQ_N && s.tx_queued == USBNET_TXQ_N,
          "the 16th slot taken while the 16th frame was copied: that frame dropped as full (txq=%u drop_full=%u)",
          (unsigned)s.txq, (unsigned)s.drop_full);
    h_in_paused = false;
    settle_();
    CHECK(h_rx == USBNET_TXQ_N && h_arrived[77] == 1 && h_arrived[15] == 0, "the 16 queued arrive, the dropped one not");
}

/* usbnet_stats reads the clock, then another task stamps a millisecond later and takes the lock first
 * (preempted on core 0, or the other core): the age is 0, not 4294967295. */
static int (*lock_inject)(void);
static void p_lock_inject_(void)
{
    if (lock_inject != NULL) {
        int (*f)(void) = lock_inject;
        lock_inject = NULL;
        atomic_fetch_add(&now_ms, 1);
        f();
    }
    p_lock_();
}

static uint8_t rx_frame[100];
static int inject_rx_(void)
{
    frame_fill_(rx_frame, sizeof(rx_frame), 61);
    usbnet_rx(&U, rx_frame, sizeof(rx_frame));
    return 0;
}
static int inject_tx_(void) { return tx_(100, 62); }
static int inject_pump_(void)
{
    usbnet_kicked(&U); /* the TinyUSB task hands the queued frame to the class */
    return 0;
}

static void test_stats_clamp(void)
{
    dev_up_();
    atomic_store(&now_ms, 500);
    inject_rx_();
    struct usbnet_port hook = port;
    hook.lock = p_lock_inject_;
    atomic_store(&now_ms, 1000);
    U.port = &hook;
    lock_inject = inject_rx_;
    struct usbnet_stats s = st_();
    CHECK(s.rx == 2 && s.rx_idle_ms == 0, "a frame received after stats read the clock: rx_idle_ms=%lu (0, not 4294967295)",
          (unsigned long)s.rx_idle_ms);
    atomic_store(&now_ms, 2000);
    h_in_paused = true;
    lock_inject = inject_tx_;
    s = st_();
    U.port = &port;
    CHECK(s.txq == 1 && s.tx_stall_ms == 0, "a frame queued after stats read the clock: txq=1 tx_stall_ms=%lu (0)",
          (unsigned long)s.tx_stall_ms);
    h_in_paused = false;
    settle_();
    atomic_store(&now_ms, 3000);
    tx_(100, 63);
    U.port = &hook;
    lock_inject = inject_pump_;
    s = st_();
    U.port = &port;
    CHECK(s.tx_sent == 2 && s.txq == 0 && s.tx_busy_ms == 0,
          "a frame into the class after stats read the clock: tx_busy_ms=%lu (0)", (unsigned long)s.tx_busy_ms);
    settle_();
    CHECK(h_rx == 2 && st_().tx_busy_ms == 0, "both arrive");
}

/* The host stops reading IN, or leaves the data interface at alt 0: the class holds frames before the
 * queue backs up, so tx_stall_ms reads 0 and tx counts them; tx_busy_ms shows the hold. A 100-byte
 * ping reply (142 bytes) a second; each read 999 ms after its frame. Returns frames the class took. */
static unsigned busy_run_(bool *first)
{
    unsigned took = 0;
    for (uint32_t i = 0; i < 40; i++) {
        atomic_store(&now_ms, 10000 + 1000 * i);
        tx_(142, i);
        settle_();
        atomic_store(&now_ms, 10000 + 1000 * i + 999);
        const struct usbnet_stats s = st_();
        if (i == 0) {
            *first = s.txq == 0 && s.tx_stall_ms == 0 && s.tx_busy_ms == 999 && s.tx_sent == 1 && h_rx == 0;
        }
        if (s.txq > 0) {
            break;
        }
        took = i + 1;
    }
    return took;
}

static void test_tx_busy(void)
{
    dev_up_();
    CHECK(st_().tx_busy_ms == 0, "tx_busy_ms is 0 with nothing sent");
    h_in_paused = true;
    bool first = false;
    unsigned took = busy_run_(&first);
    CHECK(first, "IN stuck: one frame in, tx=1, txq=0, tx_stall_ms=0, tx_busy_ms=999 999 ms later");
    CHECK(took == (USBNET_TEST_ECM ? 1u : 17u), "IN stuck: the class took %u before one waited (%s)", took,
          USBNET_TEST_ECM ? "the one in flight" : "1 in flight, then 2 NTBs of 8");
    h_in_paused = false;
    settle_();
    struct usbnet_stats s = st_();
    CHECK(s.txq == 0 && s.tx_busy_ms == 0 && h_rx == took + 1 && h_order == 0,
          "the host reads again: all %u arrive, tx_busy_ms back to 0", took + 1);
    /* One IN transfer finishes and the host stops again, the queue empty but the class not (NCM: the
     * next NTB; ECM: the ZLP after a frame of whole packets): the hold counts from that completion. */
    dev_up_();
    h_in_paused = true;
    atomic_store(&now_ms, 50000);
    for (uint32_t i = 0; i < (USBNET_TEST_ECM ? 1u : 3u); i++) {
        tx_(USBNET_TEST_ECM ? 1472 : 142, i);
    }
    settle_();
    atomic_store(&now_ms, 50400);
    h_in_paused = false;
    host_service_();
    h_in_paused = true;
    struct ev e;
    while (pop_(&e, 0)) {
        dispatch_(e);
    }
    atomic_store(&now_ms, 51100);
    s = st_();
    CHECK(s.txq == 0 && h_rx == 1 && ep_in.busy && s.tx_busy_ms == 700,
          "one IN transfer done, the class re-armed IN and the host stopped: tx_busy_ms=%u from that completion",
          (unsigned)s.tx_busy_ms);
    h_in_paused = false;
    settle_();
    CHECK(st_().tx_busy_ms == 0 && h_rx == (USBNET_TEST_ECM ? 1u : 3u), "the rest arrive, tx_busy_ms 0");
#if !USBNET_TEST_ECM
    dev_up_();
    set_alt_(0);
    const unsigned in_xfers = ep_in.xfers;
    took = busy_run_(&first);
    CHECK(first && took == 24 && ep_in.xfers == in_xfers,
          "data interface at alt 0: no IN transfer, the class took %u (3 NTBs of 8), tx_busy_ms from the first", took);
    set_alt_(1);
    settle_();
    s = st_();
    CHECK(s.txq == 0 && s.tx_busy_ms == 0 && h_rx == took + 1 && h_order == 0,
          "alt 1 again: the waiting frame restarts the class, all %u arrive, tx_busy_ms 0", took + 1);

    /* The documented blind spot: alt 0 and back to 1 with the queue empty. The driver restarts IN only
     * when offered a frame, and the notification endpoint's last completion leaves it idle, so
     * tx_busy_ms reads 0 while the driver holds frames, until lwIP sends another. */
    dev_up_();
    set_alt_(0);
    atomic_store(&now_ms, 70000);
    for (uint32_t i = 0; i < 5; i++) {
        tx_(142, i);
    }
    settle_();
    atomic_store(&now_ms, 70500);
    set_alt_(1);
    settle_();
    atomic_store(&now_ms, 71000);
    s = st_();
    CHECK(s.txq == 0 && s.tx_sent == 5 && h_rx == 0 && s.tx_busy_ms == 0,
          "alt 0 then 1, queue empty: the driver holds 5, none sent, tx_busy_ms reads 0 (documented)");
    tx_(142, 5);
    settle_();
    CHECK(h_rx == 6 && h_order == 0 && st_().tx_busy_ms == 0, "the next frame from lwIP restarts it: all 6 arrive");
#endif
}

static void test_nolink_and_flush(void)
{
    dev_up_();
    atomic_store(&ready, false);
    const unsigned in_before = ep_in.xfers;
    CHECK(tx_(100, 0) != 0 && st_().drop_nolink == 1 && kick_posts == 0,
          "host not attached: dropped as nolink, no kick");
    atomic_store(&ready, true);
    tx_(100, 1);
    tx_(100, 2);
    tx_(100, 3);
    atomic_store(&ready, false); /* the host goes before the kick runs */
    settle_();
    struct usbnet_stats s = st_();
    CHECK(s.drop_nolink == 4 && s.txq == 0 && h_rx == 0 && ep_in.xfers == in_before,
          "queued, then the link went: flushed as nolink, nothing sent (drop_nolink=%u)", (unsigned)s.drop_nolink);
    atomic_store(&ready, true);
    h_in_paused = true;
    for (uint32_t i = 0; i < 20; i++) {
        tx_(1514, 10 + i);
    }
    settle_();
    const unsigned waiting = st_().txq;
    atomic_fetch_add(&now_ms, 500);
    const unsigned held = st_().tx_busy_ms;
    usbnet_flush(&U); /* TINYUSB_EVENT_DETACHED */
    s = st_();
    CHECK(waiting > 0 && s.txq == 0 && s.drop_nolink == 4 + waiting && held == 500 && s.tx_busy_ms == 0,
          "detach frees what waits (%u) and clears the class's hold (tx_busy_ms %u -> 0)", waiting, held);
    CHECK(tx_(0, 0) != 0 && tx_(1514, 0) == 0 && usbnet_tx(&U, frame_buf, USBNET_FRAME_MAX + 1) != 0 &&
          st_().drop_bad == 2, "empty and oversized frames: dropped as bad");
    atomic_store(&alloc_fail_in, 1);
    CHECK(tx_(100, 0) != 0 && st_().drop_nomem == 1, "no heap for the copy: dropped as nomem");
    usbnet_flush(&U);
}

#if !USBNET_TEST_ECM
static void script_(int k, uint16_t a, uint16_t b)
{
    script[script_n++] = (struct out_ntb){k, {a, b}};
}

/* macOS packs both fragments of a ping into one NTB. Each NTB is offered once the device arms OUT. */
static void run_script_(void)
{
    while (script_next < script_n) {
        const unsigned before = script_next;
        settle_();
        if (script_next == before) {
            break; /* no OUT transfer armed: the host cannot send */
        }
    }
}

static unsigned pairs_(void)
{
    unsigned whole = 0, n = atomic_load(&d_rx);
    for (unsigned i = 0; i + 1 < n && i + 1 < DLOG_N; i++) {
        if (d_log[i] % 2 == 0 && d_log[i + 1] == d_log[i] + 1 && d_log[i] < 12) {
            whole++;
        }
    }
    return whole;
}

static void measured_sequence_(void)
{
    for (int i = 0; i < 3; i++) {
        script_(2, 1514, 35); /* ping -s 1473: 1480 + 1 byte of ICMP */
    }
    for (int i = 0; i < 3; i++) {
        script_(2, 1514, 562); /* ping -s 2000: 1480 + 528 */
    }
    for (int i = 0; i < 3; i++) {
        script_(1, 142, 0); /* ping -s 100 */
    }
}

static void test_rx_wedge(void)
{
    dev_up_();
    legacy_rx = true;
    measured_sequence_();
    run_script_();
    const unsigned got = atomic_load(&d_rx), whole = pairs_();
    CHECK(got == 5 && whole == 2 && !ep_out.busy && script_next == 5,
          "pre-fix callback replays the air: 5 of 12 fragments in, 2 datagrams whole (ip_reass=2), "
          "no OUT armed after the 5th NTB (got=%u whole=%u sent=%u)", got, whole, script_next);
    CHECK(script_n - script_next == 4, "...so the 6th ping and all three 100-byte pings never get onto the bus");

    dev_up_();
    measured_sequence_();
    run_script_();
    struct usbnet_stats s = st_();
    CHECK(atomic_load(&d_rx) == 15 && atomic_load(&d_order) == 0 && pairs_() == 6 && ep_out.busy &&
          script_next == script_n,
          "usbnet_rx renews: all 15 datagrams in order, 6 pings whole, OUT armed again (rx=%u)",
          (unsigned)atomic_load(&d_rx));
    CHECK(s.rx == 15 && s.rx_xfer == 9, "rx=15 over rx_xfer=9: the host packed NTBs (AT+STATUS? shows it)");
    atomic_fetch_add(&now_ms, 1200);
    CHECK(st_().rx_idle_ms == 1200, "rx_idle_ms counts from the last frame in (%u)", (unsigned)st_().rx_idle_ms);
}

static void test_rx_full_ntb(void)
{
    dev_up_();
    script[script_n++] = (struct out_ntb){6, {60, 120, 500, 900, 64, 1300}};
    run_script_();
    CHECK(atomic_load(&d_rx) == 6 && atomic_load(&d_order) == 0 && ep_out.busy,
          "an NTB of 6 datagrams: all 6 in order, OUT armed again");
    atomic_store(&alloc_fail_in, 3);
    atomic_store(&input_fail_once, false);
    script[script_n++] = (struct out_ntb){6, {60, 120, 500, 900, 64, 1300}};
    run_script_();
    struct usbnet_stats s = st_();
    CHECK(atomic_load(&d_rx) == 11 && s.rx_nomem == 1 && ep_out.busy,
          "no heap for the 3rd copy: counted (rx_nomem=1), the other 5 in, the NTB still advances");
    atomic_store(&input_fail_once, true);
    script[script_n++] = (struct out_ntb){2, {100, 100}};
    run_script_();
    s = st_();
    CHECK(atomic_load(&d_rx) == 13 && s.rx_err == 1 && ep_out.busy,
          "the IP stack refuses one (tcpip mailbox full): counted (rx_err=1), the NTB still advances");
}
#else
static void test_rx_ecm(void)
{
    dev_up_();
    CHECK(st_().rx_idle_ms == 0, "rx_idle_ms is 0 before the first frame");
    for (int i = 0; i < 5; i++) {
        script[script_n++] = (struct out_ntb){1, {(uint16_t)(60 + 300 * i)}};
    }
    while (script_next < script_n) {
        const unsigned before = script_next;
        settle_();
        if (script_next == before) {
            break;
        }
    }
    CHECK(atomic_load(&d_rx) == 5 && ep_out.busy && atomic_load(&renews) == 0,
          "ECM: each frame in, the driver re-arms OUT itself (false from the callback), usbnet never renews");
    atomic_fetch_add(&now_ms, 700);
    CHECK(st_().rx == 5 && st_().rx_xfer == 5 && st_().rx_idle_ms == 700, "ECM: rx=rx_xfer, one frame per transfer; rx_idle_ms 700");
}
#endif

/* The pre-change l2_transmit, inline from lwIP's thread: the check sees it. */
static void *legacy_tx_thread_(void *arg)
{
    (void)arg;
    static uint8_t f[200];
    frame_fill_(f, sizeof(f), 0);
    if (tud_network_can_xmit(sizeof(f))) {
        tud_network_xmit(f, sizeof(f));
    }
    return NULL;
}

static void test_legacy_tx_flagged(void)
{
    dev_up_();
    pthread_t t;
    pthread_create(&t, NULL, legacy_tx_thread_, NULL);
    pthread_join(t, NULL);
    const unsigned bad = atomic_load(&owner_bad);
    settle_();
    CHECK(bad > 0, "the pre-change inline transmit is flagged: %u TinyUSB calls from lwIP's thread", bad);
}

/* ---- two tasks: lwIP's thread queues while the TinyUSB task and the host run --------------- */

#define THR_TX 20000u
#define THR_RX 6000u
static atomic_bool stop_usb, stop_host, usb_up;
static uint8_t accepted[SEQ_MAX];
static unsigned accepted_n;

static void *usb_thread_(void *arg)
{
    (void)arg;
    owner = pthread_self();
    atomic_store(&usb_up, true);
    struct ev e;
    while (!atomic_load(&stop_usb)) {
        if (pop_(&e, 5)) {
            dispatch_(e);
        }
    }
    return NULL;
}

static void *host_thread_(void *arg)
{
    (void)arg;
    while (!atomic_load(&stop_host)) {
        if (!host_service_()) {
            usleep(20);
        } else if ((rnd_() & 7) == 0) {
            usleep(rnd_() % 50);
        }
    }
    return NULL;
}

static void test_two_tasks(void)
{
    dev_up_();
    atomic_store(&real_clock, true);
    pthread_mutex_lock(&g_mx);
    out_target = THR_RX;
    atomic_store(&out_random, true);
    pthread_mutex_unlock(&g_mx);
    memset(accepted, 0, sizeof(accepted));
    accepted_n = 0;
    atomic_store(&stop_usb, false);
    atomic_store(&stop_host, false);
    atomic_store(&usb_up, false);
    pthread_t tu, th;
    pthread_create(&tu, NULL, usb_thread_, NULL);
    while (!atomic_load(&usb_up)) {
        usleep(100);
    }
    pthread_create(&th, NULL, host_thread_, NULL);
    /* This thread is lwIP's: fragment trains of 1 to 10 frames, then a pause. */
    uint32_t seq = 0, r = 12345;
    while (seq < THR_TX) {
        r = r * 1103515245u + 12345u;
        unsigned burst = 1 + (r >> 16) % 10;
        for (unsigned i = 0; i < burst && seq < THR_TX; i++, seq++) {
            r = r * 1103515245u + 12345u;
            uint8_t f[1514];
            const uint16_t len = (uint16_t)(FRAME_MIN + (r >> 8) % (1514 - FRAME_MIN + 1));
            frame_fill_(f, len, seq);
            if (usbnet_tx(&U, f, len) == 0) {
                accepted[seq] = 1;
                accepted_n++;
            }
        }
        if ((r >> 4) % 4 == 0) {
            usleep((r >> 20) % 200);
        }
    }
    /* The burst measured before this change (300 pings, 1400-byte payload, 0% loss): one 1442-byte
     * reply per request, each sent once the last has arrived. */
    for (uint32_t t1 = mono_ms_(); st_().txq > 0 && mono_ms_() - t1 < 5000;) {
        usleep(100); /* the trains' backlog drains first */
    }
    unsigned ping_ok = 0;
    const unsigned drops_before = st_().drop_full, offered = seq + 300;
    for (unsigned i = 0; i < 300 && seq < SEQ_MAX; i++, seq++) {
        uint8_t f[1442];
        frame_fill_(f, sizeof(f), seq);
        if (usbnet_tx(&U, f, sizeof(f)) == 0) {
            accepted[seq] = 1;
            accepted_n++;
        }
        const uint32_t t1 = mono_ms_();
        for (;;) {
            pthread_mutex_lock(&g_mx);
            const bool in = h_arrived[seq] != 0;
            pthread_mutex_unlock(&g_mx);
            if (in || mono_ms_() - t1 > 1000) {
                ping_ok += in;
                break;
            }
            usleep(50);
        }
        if (ping_ok <= i) {
            break; /* one lost reply fails the check; the rest would each wait a second */
        }
    }
    CHECK(ping_ok == 300 && st_().drop_full == drops_before,
          "300 replies of 1442 bytes, one per request: %u/300, none dropped (drop_full %u -> %u)", ping_ok,
          drops_before, (unsigned)st_().drop_full);
    const uint32_t t0 = mono_ms_();
    bool done = false;
    while (!done && mono_ms_() - t0 < 20000) {
        usleep(1000);
        pthread_mutex_lock(&g_mx);
        const bool host_done = h_rx >= accepted_n && out_seq >= THR_RX;
        pthread_mutex_unlock(&g_mx);
        done = host_done && atomic_load(&d_rx) >= THR_RX && st_().txq == 0;
    }
    atomic_store(&stop_host, true);
    pthread_join(th, NULL);
    atomic_store(&stop_usb, true);
    pthread_join(tu, NULL);
    atomic_store(&real_clock, false);
    owner = pthread_self();
    unsigned lost = 0, extra = 0;
    for (unsigned i = 0; i < THR_TX; i++) {
        lost += accepted[i] && h_arrived[i] != 1;
        extra += !accepted[i] && h_arrived[i] != 0;
    }
    struct usbnet_stats s = st_();
    CHECK(done, "the run drains within 20 s");
    CHECK(lost == 0 && extra == 0 && h_order == 0 && h_bad == 0 && s.tx_busy_ms == 0 && s.txq == 0,
          "%u frames queued of %u offered; every one arrives once, in order, intact (lost=%u extra=%u order=%u bad=%u)",
          accepted_n, THR_TX, lost, extra, h_order, h_bad);
    CHECK(s.tx_queued == accepted_n && s.tx_sent == accepted_n && s.drop_full == offered - accepted_n,
          "counters add up: queued=%u sent=%u drop_full=%u", (unsigned)s.tx_queued, (unsigned)s.tx_sent,
          (unsigned)s.drop_full);
    CHECK(atomic_load(&d_rx) == THR_RX && atomic_load(&d_order) == 0 && atomic_load(&d_bad) == 0,
          "%u datagrams the host sent in %u transfers: all in, in order", THR_RX, out_sent_ntbs);
    CHECK(atomic_load(&owner_bad) == 0 && atomic_load(&held_call) == 0,
          "one owner across three threads: no TinyUSB call off the TinyUSB task, none under the lock");
    CHECK(kicks_queued_hw <= 1 && evq_hw <= 4 && evq_overflow == 0,
          "TinyUSB's event queue holds at most one kick and one completion per endpoint (hw=%u of 16)", evq_hw);
    pthread_mutex_lock(&g_mx);
    atomic_store(&out_random, false);
    pthread_mutex_unlock(&g_mx);
}

int main(void)
{
    owner = pthread_self();
    usbnet_init(&U, &port);
    test_fragment_train();
    test_overflow();
    test_no_lost_wakeup();
    test_full_race();
    test_stats_clamp();
    test_tx_busy();
    test_nolink_and_flush();
#if !USBNET_TEST_ECM
    test_rx_wedge();
    test_rx_full_ntb();
#else
    test_rx_ecm();
#endif
    test_legacy_tx_flagged();
    test_two_tasks();
    usbnet_flush(&U);
    if (failures) {
        printf("test_usbnet (" CLASS "): %d FAILED\n", failures);
        return 1;
    }
    printf("test_usbnet (" CLASS "): all passed\n");
    return 0;
}
