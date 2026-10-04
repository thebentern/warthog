/* IP fragments through IDF's own lwIP (esp-lwip from the PlatformIO ESP-IDF package the firmware
 * links: ip4.c, ip4_frag.c, ip4_napt.c, icmp.c, udp.c), built for the host and wired as main/nat.c
 * wires it: NAPT on the USB and AP netifs, HaLow (or bat0) the default route without it. Each case
 * cuts a whole datagram as its sender would, feeds the fragments to ip4_input as the firmware's
 * drivers hand them over (a PBUF_REF pbuf over a radio RX block or a batman delivery, as
 * esp_pbuf_allocate makes it) and reassembles what lwIP sends out of each netif. tcpip_inpkt is the
 * mailbox: queued, run from the top afterwards.
 *
 * Four builds (lwip_napt.mk):
 *   LWIP_NAPT_HOOK=1   the firmware: IPv4 reassembly on and main/nat_frag.c's input hook. Everything
 *                      passes: pings to the Warthog, pings and UDP through NAPT both ways, at a Pi
 *                      MTU of 1500 and of 1460 (OpenMANET's bat0). No IP header checksum is checked,
 *                      as CONFIG_LWIP_CHECKSUM_CHECK_IP is unset.
 *   ..._cksum          the same with CHECKSUM_CHECK_IP 1: the hook's own header checksum test.
 *   no hook            reassembly on, stock NAPT: pings to the Warthog pass; through NAPT, a non-first
 *                      fragment keeps the host's source out (ICMP), gets its payload rewritten as if
 *                      it were a UDP header (UDP), or is never translated back in. These are asserted
 *                      as failures: if this build fails on them, IDF's NAPT handles fragments itself.
 *                      Held fragments keep their driver buffers, a fragment to a broadcast or
 *                      multicast address is held and answered with ICMP time exceeded, and a large
 *                      ping's reply is copied into one block its full size.
 *   LWIP_NAPT_REASS=0  IDF's default: a fragmented ping to the Warthog is never answered, the on-air
 *                      result of 2026-10-03 (0/5 in 24 rounds from a Pi at MTU 1460).
 * Also, with the hook: fragments to the Warthog taken with NAPT off too; held fragments are heap
 * copies, so no driver buffer is kept, at most 10 of them (IP_REASS_MAX_PBUFS) under 16 KB; a
 * fragment to a broadcast or multicast address dropped and counted, never answered; a 10-fragment
 * ping answered without one block its full size; fragments between two NAPT netifs forwarded as they
 * came; a tiny first TCP fragment and a full mailbox dropped and counted; a first fragment with a
 * bad header checksum reassembled like any other (dropped and counted with CHECKSUM_CHECK_IP); a lone
 * first fragment held and freed by the reassembly timer; whole TCP, UDP and ICMP packets shorter than
 * their header, to the Warthog or through NAPT, dropped and counted (ip_short_drop): also those to the
 * address of the non-NAPT netif they came in on while it is down, at 0.0.0.0 (a lost lease) or another
 * netif's subnet broadcast, one whose first pbuf ends inside the header, and UDP without its destination
 * port wherever it goes (ip4_input reads that port for DHCP); header-only ones NAPTed, short ones between
 * two NAPT netifs forwarded as they came. Those are fed in driver buffers of exactly their size: with the
 * hook's check removed, ASan (SAN=1) reports stock NAPT's (or ip4_input's) heap-buffer-overflow and the
 * plain build crashes after a TCP session match. Every lwIP allocation and driver buffer returned.
 * Every build: lwIP has DHCP on, as the firmware's, and its netifs are added in the firmware's order;
 * lwIP's ip4_frag ignores a netif that refuses a fragment (bat0 without bat_port_netif_tx's wait for a
 * slot), so that fragment, and the datagram, is lost. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/inet_chksum.h"
#include "lwip/init.h"
#include "lwip/ip4.h"
#include "lwip/lwip_napt.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ip.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/tcp.h"
#include "lwip/prot/udp.h"
#include "lwip/timeouts.h"
#if LWIP_NAPT_HOOK
#include "nat_frag.h"
#endif

/* Every lwIP allocation counted with its size; the largest one too. */
struct alloc_hdr { size_t n; size_t pad; };
static long live_allocs, live_bytes, max_alloc;
void *lwip_test_malloc(size_t n)
{
    struct alloc_hdr *h = malloc(sizeof(*h) + n);
    if (h == NULL) { return NULL; }
    h->n = n;
    live_allocs++;
    live_bytes += (long)n;
    if ((long)n > max_alloc) { max_alloc = (long)n; }
    return h + 1;
}
void *lwip_test_calloc(size_t n, size_t m)
{
    void *p = lwip_test_malloc(n * m);
    if (p != NULL) { memset(p, 0, n * m); }
    return p;
}
void lwip_test_free(void *p)
{
    if (p == NULL) { return; }
    struct alloc_hdr *h = (struct alloc_hdr *)p - 1;
    live_allocs--;
    live_bytes -= (long)h->n;
    free(h);
}

static u32_t now_ms;
u32_t sys_now(void) { return now_ms; }

/* IDF's ip4_route_src_hook (port/hooks/lwip_default_hooks.c): a source that is a netif's own address
 * leaves by that netif; anything else falls back to ip4_route. */
struct netif *ip4_route_src_hook(const ip4_addr_t *src, const ip4_addr_t *dest)
{
    struct netif *n;
    (void)dest;
    if (src == NULL || ip4_addr_isany(src)) { return NULL; }
    NETIF_FOREACH(n) {
        if (netif_is_up(n) && netif_is_link_up(n) && !ip4_addr_isany_val(*netif_ip4_addr(n)) && ip4_addr_eq(src, netif_ip4_addr(n))) {
            return n;
        }
    }
    return NULL;
}

static struct { struct pbuf *p; struct netif *n; netif_input_fn fn; } mbox[64];
static int mb_head, mb_tail, mb_refuse;
err_t tcpip_inpkt(struct pbuf *p, struct netif *inp, netif_input_fn input_fn)
{
    if (mb_refuse || mb_tail - mb_head >= 64) { return ERR_MEM; }
    mbox[mb_tail % 64].p = p;
    mbox[mb_tail % 64].n = inp;
    mbox[mb_tail % 64].fn = input_fn;
    mb_tail++;
    return ERR_OK;
}
static void run_mbox(void)
{
    while (mb_head < mb_tail) {
        int i = mb_head++ % 64;
        mbox[i].fn(mbox[i].p, mbox[i].n);
    }
}

/* A driver's receive buffer under a PBUF_REF pbuf (esp_pbuf_allocate), back to the driver when lwIP frees
 * it: a radio RX block (23 at most) or a batman delivery (16). drv_out counts those lwIP holds. */
struct drv { struct pbuf_custom pc; u8_t b[2048]; };
static int drv_out, drv_peak;
static void drv_free(struct pbuf *p) { drv_out--; free(p); }

#define MAXOUT 64
struct cap { int n; struct { u8_t b[2048]; u16_t len; } pkt[MAXOUT]; };
static struct cap cap_usb, cap_ap, cap_halow;
static struct netif usb, ap, halow;
static int out_slots = -1, out_refused; /* frames the HaLow netif takes before refusing; -1 no limit */
static err_t capture(struct netif *nif, struct pbuf *p, const ip4_addr_t *dst)
{
    (void)dst;
    struct cap *c = nif->state;
    if (nif == &halow && out_slots >= 0) {
        if (out_slots == 0) { out_refused++; return ERR_MEM; }
        out_slots--;
    }
    if (c->n < MAXOUT && p->tot_len <= sizeof(c->pkt[0].b)) {
        c->pkt[c->n].len = pbuf_copy_partial(p, c->pkt[c->n].b, p->tot_len, 0);
        c->n++;
    }
    return ERR_OK;
}
/* wlanif_init sets NETIF_FLAG_BROADCAST */
static err_t nif_init(struct netif *n) { n->output = capture; n->flags |= NETIF_FLAG_BROADCAST; return ERR_OK; }

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL "); } else { printf("ok   "); } \
                           printf(__VA_ARGS__); printf("\n"); } while (0)

#if LWIP_NAPT_HOOK
#define BUILD (CHECKSUM_CHECK_IP ? "firmware hook, CHECKSUM_CHECK_IP on" : "firmware (reassembly on, nat_frag.c hook)")
static const int HOOK = 1;
#else
#define BUILD (IP_REASSEMBLY ? "reassembly on, no hook" : "reassembly off (IDF default)")
static const int HOOK = 0;
#endif
static const int REASS = IP_REASSEMBLY;

static u32_t A(unsigned a, unsigned b, unsigned c, unsigned d) { return PP_HTONL((a << 24) | (b << 16) | (c << 8) | d); }
static u32_t HOST, AP_HOST, PI, WH;

static u16_t ip_build(u8_t *buf, u32_t src, u32_t dst, u8_t proto, u16_t id, const u8_t *l4, u16_t l4len)
{
    struct ip_hdr *h = (struct ip_hdr *)buf;
    memset(h, 0, IP_HLEN);
    IPH_VHL_SET(h, 4, 5);
    IPH_LEN_SET(h, lwip_htons((u16_t)(IP_HLEN + l4len)));
    IPH_ID_SET(h, lwip_htons(id));
    IPH_TTL_SET(h, 64);
    IPH_PROTO_SET(h, proto);
    h->src.addr = src;
    h->dest.addr = dst;
    IPH_CHKSUM_SET(h, inet_chksum(h, IP_HLEN));
    memcpy(buf + IP_HLEN, l4, l4len);
    return (u16_t)(IP_HLEN + l4len);
}

/* iputils ping's payload past its timestamp: byte i is i. */
static u16_t icmp_echo(u8_t *l4, u8_t type, u16_t id, u16_t seq, u16_t size)
{
    struct icmp_echo_hdr *e = (struct icmp_echo_hdr *)l4;
    e->type = type;
    e->code = 0;
    e->chksum = 0;
    e->id = lwip_htons(id);
    e->seqno = lwip_htons(seq);
    for (u32_t i = 0; i < size; i++) { l4[8 + i] = (u8_t)i; }
    e->chksum = inet_chksum(l4, (u16_t)(8 + size));
    return (u16_t)(8 + size);
}

static u16_t udp_sum(const u8_t *l4, u16_t len, u32_t src, u32_t dst)
{
    ip4_addr_t s, d;
    s.addr = src;
    d.addr = dst;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_RAM);
    pbuf_take(p, l4, len);
    u16_t c = ip_chksum_pseudo(p, IP_PROTO_UDP, len, &s, &d);
    pbuf_free(p);
    return c;
}

static u16_t udp_build(u8_t *l4, u16_t sport, u16_t dport, u16_t size, u32_t src, u32_t dst)
{
    struct udp_hdr *u = (struct udp_hdr *)l4;
    u->src = lwip_htons(sport);
    u->dest = lwip_htons(dport);
    u->len = lwip_htons((u16_t)(8 + size));
    u->chksum = 0;
    for (u32_t i = 0; i < size; i++) { l4[8 + i] = (u8_t)(i * 7 + 3); }
    u->chksum = udp_sum(l4, (u16_t)(8 + size), src, dst);
    if (u->chksum == 0) { u->chksum = 0xffff; }
    return (u16_t)(8 + size);
}

/* One received packet, as the driver hands it to lwIP; then the mailbox runs. */
static void inject(const u8_t *b, u16_t len, struct netif *inp)
{
    struct drv *d = malloc(sizeof(*d));
    memcpy(d->b, b, len);
    d->pc.custom_free_function = drv_free;
    drv_out++;
    ip4_input(pbuf_alloced_custom(PBUF_RAW, len, PBUF_REF, &d->pc, d->b, sizeof(d->b)), inp);
    run_mbox();
    if (drv_out > drv_peak) { drv_peak = drv_out; }
}

/* A whole packet in a driver buffer of exactly buflen bytes (the IP datagram, then any link padding), so
 * ASan sees a read or write past it. */
struct drv_exact { struct pbuf_custom pc; u8_t *b; };
static void drv_exact_free(struct pbuf *p)
{
    struct drv_exact *d = (struct drv_exact *)(void *)p;
    drv_out--;
    free(d->b);
    free(d);
}
/* With `first` < buflen, only the first `first` bytes are in that buffer; the rest is chained after it. */
static void inject_split(const u8_t *b, u16_t buflen, u16_t first, struct netif *inp)
{
    struct drv_exact *d = malloc(sizeof(*d));
    d->b = malloc(first);
    memcpy(d->b, b, first);
    d->pc.custom_free_function = drv_exact_free;
    drv_out++;
    struct pbuf *p = pbuf_alloced_custom(PBUF_RAW, first, PBUF_REF, &d->pc, d->b, first);
    if (first < buflen) {
        struct pbuf *rest = pbuf_alloc(PBUF_RAW, (u16_t)(buflen - first), PBUF_RAM);
        pbuf_take(rest, b + first, (u16_t)(buflen - first));
        pbuf_cat(p, rest);
    }
    ip4_input(p, inp);
    run_mbox();
    if (drv_out > drv_peak) { drv_peak = drv_out; }
}
static void inject_exact(const u8_t *b, u16_t buflen, struct netif *inp) { inject_split(b, buflen, buflen, inp); }

/* Cut as a sender with this MTU cuts (data in multiples of 8), first fragment holding `first` bytes
 * if nonzero; feed each piece (reverse: last first), the first of them with a bad header checksum
 * while feed_bad. With `only` nonzero just the first `only` pieces. Returns how many pieces it cut. */
static int feed_bad;
static int feed_n(const u8_t *dg, u16_t dglen, u16_t mtu, struct netif *inp, int reverse, u16_t first, int only)
{
    const u16_t data = (u16_t)(dglen - IP_HLEN), per = (u16_t)(((mtu - IP_HLEN) / 8) * 8);
    u16_t offs[64], lens[64];
    int n = 0;
    for (u16_t off = 0; off < data; n++) {
        u16_t l = (u16_t)(n == 0 && first ? first : per);
        if (l > data - off) { l = (u16_t)(data - off); }
        offs[n] = off;
        lens[n] = l;
        off = (u16_t)(off + l);
    }
    for (int k = 0; k < n && (!only || k < only); k++) {
        const int i = reverse ? n - 1 - k : k;
        u8_t b[2048];
        memcpy(b, dg, IP_HLEN);
        struct ip_hdr *h = (struct ip_hdr *)b;
        IPH_LEN_SET(h, lwip_htons((u16_t)(IP_HLEN + lens[i])));
        IPH_OFFSET_SET(h, lwip_htons((u16_t)((offs[i] / 8) | (i < n - 1 ? IP_MF : 0))));
        IPH_CHKSUM_SET(h, 0);
        IPH_CHKSUM_SET(h, (u16_t)(inet_chksum(h, IP_HLEN) ^ (feed_bad && i == 0 ? 0x1234 : 0)));
        memcpy(b + IP_HLEN, dg + IP_HLEN + offs[i], lens[i]);
        inject(b, (u16_t)(IP_HLEN + lens[i]), inp);
    }
    return n;
}
static int feed(const u8_t *dg, u16_t dglen, u16_t mtu, struct netif *inp, int reverse, u16_t first)
{
    return feed_n(dg, dglen, mtu, inp, reverse, first, 0);
}

/* The datagram the captured packets make up; its length, or -1 if incomplete or not one datagram. */
struct got { int len, n; u32_t src, dst; int mixed; };
static struct got collect(const struct cap *c, u8_t *dg)
{
    struct got g = { -1, 0, 0, 0, 0 };
    int bytes = 0;
    for (int i = 0; i < c->n; i++) {
        const struct ip_hdr *h = (const struct ip_hdr *)c->pkt[i].b;
        const u16_t off = (u16_t)((lwip_ntohs(IPH_OFFSET(h)) & IP_OFFMASK) * 8);
        const u16_t l = (u16_t)(lwip_ntohs(IPH_LEN(h)) - IP_HLEN);
        if (g.n == 0) {
            memcpy(dg, h, IP_HLEN);
            g.src = h->src.addr;
            g.dst = h->dest.addr;
        } else if (h->src.addr != g.src || h->dest.addr != g.dst) {
            g.mixed = 1;
        }
        if (off + l <= 16384) { memcpy(dg + IP_HLEN + off, c->pkt[i].b + IP_HLEN, l); }
        bytes += l;
        if (!(lwip_ntohs(IPH_OFFSET(h)) & IP_MF)) { g.len = IP_HLEN + off + l; }
        g.n++;
    }
    if (g.len < 0 || bytes != g.len - IP_HLEN || g.mixed) { g.len = -1; }
    return g;
}

/* ICMP time exceeded (fragment reassembly) messages captured. */
static int time_exceeded(const struct cap *c)
{
    int n = 0;
    for (int i = 0; i < c->n; i++) {
        const struct ip_hdr *h = (const struct ip_hdr *)c->pkt[i].b;
        n += IPH_PROTO(h) == IP_PROTO_ICMP && c->pkt[i].b[IPH_HL_BYTES(h)] == ICMP_TE && c->pkt[i].b[IPH_HL_BYTES(h) + 1] == 1;
    }
    return n;
}

static void clear(void) { cap_usb.n = cap_ap.n = cap_halow.n = 0; }
static void tick(int seconds)
{
    for (int t = 0; t < seconds; t++) {
        now_ms += 1000;
        sys_check_timeouts();
    }
}
static void counts(uint32_t *r, uint32_t *d)
{
#if LWIP_NAPT_HOOK
    uint32_t c;
    warthog_nat_frag_counts(r, d, &c);
#else
    *r = *d = 0;
#endif
}
/* Whole packets the hook dropped as too short for their transport header (AT+MTU? ip_short_drop). */
static uint32_t cut_short(void)
{
    uint32_t c = 0;
#if LWIP_NAPT_HOOK
    uint32_t r, d;
    warthog_nat_frag_counts(&r, &d, &c);
#endif
    return c;
}

static u8_t l4[16384], dg[16384], out[16384];

/* A Pi pings the Warthog's own HaLow address; pieces_in gets how many fragments it sent. */
static int pieces_in;
static int ping_warthog(u16_t size, u16_t mtu, int reverse, u16_t id)
{
    clear();
    drv_peak = drv_out;
    const u16_t l = icmp_echo(l4, ICMP_ECHO, 0x5150, id, size);
    const u16_t dl = ip_build(dg, PI, WH, IP_PROTO_ICMP, id, l4, l);
    pieces_in = feed(dg, dl, mtu, &halow, reverse, 0);
    const struct got g = collect(&cap_halow, out);
    return g.len == dl && g.src == WH && g.dst == PI && out[IP_HLEN] == ICMP_ER &&
           !memcmp(out + IP_HLEN + 8, dg + IP_HLEN + 8, size) && inet_chksum(out + IP_HLEN, (u16_t)(g.len - IP_HLEN)) == 0;
}

static void napt_cases(u16_t pimtu)
{
    char what[160];
    for (int s = 0; s < 3; s++) {
        const u16_t size = s == 0 ? 1472 : s == 1 ? 2000 : 4000;
        const u16_t id = (u16_t)(0x100 * (pimtu == 1460) + 0x10 * s);
        /* 1. To the Warthog itself (the measured case). */
        const int frag_in = IP_HLEN + 8 + size > pimtu;
        int ok = ping_warthog(size, pimtu, 0, (u16_t)(id + 1));
        snprintf(what, sizeof(what), "Pi (MTU %u) -> Warthog ping -s %u%s", pimtu, size, frag_in ? " in fragments" : "");
        CHECK(ok == (REASS || !frag_in), "%s: %s", what, ok ? "answered" : "not answered");
        if (frag_in && s == 2) {
            const int held = drv_peak;
            CHECK(held == (HOOK || !REASS ? 0 : pieces_in - 1), "  while incomplete, driver buffers lwIP kept: %d of %d fragments%s",
                  held, pieces_in, HOOK ? " (heap copies instead)" : "");
        }

        /* 2. A tethered host (USB, MTU 1500) pings the Pi through NAPT. */
        clear();
        u16_t l = icmp_echo(l4, ICMP_ECHO, 0x4242, (u16_t)(id + 2), size);
        u16_t dl = ip_build(dg, HOST, PI, IP_PROTO_ICMP, (u16_t)(id + 2), l4, l);
        int nf = feed(dg, dl, 1500, &usb, 0, 0);
        struct got g = collect(&cap_halow, out);
        ok = g.len == dl && g.src == WH && g.dst == PI && !memcmp(out + IP_HLEN, dg + IP_HLEN, l);
        CHECK(ok == (HOOK || nf == 1), "host -> Pi (MTU %u) ping -s %u, %d fragment(s) in: %s out the HaLow side (%d pieces%s)",
              pimtu, size, nf, ok ? "the whole request, NAPTed," : "NOT the NAPTed request", g.n,
              cap_halow.n > 1 && ((struct ip_hdr *)cap_halow.pkt[1].b)->src.addr == HOST ? "; a later one kept the host's source" : "");

        /* 3. The Pi's reply to the Warthog's address, which NAPT turns back to the host. */
        clear();
        l = icmp_echo(l4, ICMP_ER, 0x4242, (u16_t)(id + 2), size);
        dl = ip_build(dg, PI, WH, IP_PROTO_ICMP, (u16_t)(id + 3), l4, l);
        nf = feed(dg, dl, pimtu, &halow, 0, 0);
        g = collect(&cap_usb, out);
        ok = g.len == dl && g.src == PI && g.dst == HOST && !memcmp(out + IP_HLEN, dg + IP_HLEN, l);
        CHECK(ok == (HOOK || nf == 1), "Pi (MTU %u) -> host echo reply -s %u, %d fragment(s) in: %s (%d of them reached USB)",
              pimtu, size, nf, ok ? "the whole reply at the host" : "NOT the reply at the host", cap_usb.n);
        tick(5); /* anything held for reassembly times out */
    }

    /* 4. UDP: the host opens a session; the Pi answers it with 3000 bytes, and the host sends 3000. */
    clear();
    u16_t l = udp_build(l4, 40000, 5353, 20, HOST, PI);
    u16_t dl = ip_build(dg, HOST, PI, IP_PROTO_UDP, 0x400, l4, l);
    feed(dg, dl, 1500, &usb, 0, 0);
    struct got g = collect(&cap_halow, out);
    const u16_t mport = ((struct udp_hdr *)(out + IP_HLEN))->src;
    CHECK(g.len == dl && g.src == WH, "host -> Pi UDP, 20 bytes: NAPTed to port %u", lwip_ntohs(mport));
    clear();
    l = udp_build(l4, 5353, lwip_ntohs(mport), 3000, PI, WH);
    dl = ip_build(dg, PI, WH, IP_PROTO_UDP, 0x401, l4, l);
    int nf = feed(dg, dl, pimtu, &halow, 0, 0);
    g = collect(&cap_usb, out);
    int ok = g.len == dl && g.dst == HOST && lwip_ntohs(((struct udp_hdr *)(out + IP_HLEN))->dest) == 40000 &&
             udp_sum(out + IP_HLEN, (u16_t)(g.len - IP_HLEN), g.src, g.dst) == 0;
    CHECK(ok == HOOK, "Pi (MTU %u) -> host UDP 3000 bytes, %d fragments in: %s", pimtu, nf,
          ok ? "whole at the host, checksum good" : "NOT delivered whole to the host");
    clear();
    l = udp_build(l4, 40001, 5353, 3000, HOST, PI);
    dl = ip_build(dg, HOST, PI, IP_PROTO_UDP, 0x402, l4, l);
    nf = feed(dg, dl, 1500, &usb, 0, 0);
    g = collect(&cap_halow, out);
    ok = g.len == dl && g.src == WH && udp_sum(out + IP_HLEN, (u16_t)(g.len - IP_HLEN), g.src, g.dst) == 0 &&
         !memcmp(out + IP_HLEN + 8, dg + IP_HLEN + 8, 3000);
    CHECK(ok == HOOK, "host -> Pi (MTU %u) UDP 3000 bytes, %d fragments in: %s", pimtu, nf,
          ok ? "NAPTed whole, payload and checksum intact" :
          g.len == dl && g.src == WH ? "NAPTed but a later fragment's payload rewritten as a UDP header" : "NOT NAPTed whole");

    /* 5. Fragments last first. */
    ok = ping_warthog(2000, pimtu, 1, 0x403);
    CHECK(ok == REASS, "Pi (MTU %u) -> Warthog ping -s 2000, fragments last first: %s", pimtu, ok ? "answered" : "not answered");
    tick(5);
}

/* Ten fragments are the most one datagram can have: IP_REASS_MAX_PBUFS. */
static void reass_limit(void)
{
    const u16_t most = (u16_t)(10 * 1440 - 8);
    max_alloc = 0;
    int ok = ping_warthog(most, 1460, 0, 0x500);
    const long big = max_alloc;
    CHECK(ok == REASS && (HOOK ? big < 2048 : !REASS || big > most),
          "Pi (MTU 1460) -> Warthog ping -s %u, 10 fragments: %s; largest lwIP allocation %ld bytes%s", most,
          ok ? "answered" : "not answered", big, HOOK ? " (the reply reuses the held copies)" : REASS ? " (the reply copied whole)" : "");
    ok = ping_warthog((u16_t)(most + 1), 1460, 0, 0x501);
    CHECK(!ok, "ping -s %u, 11 fragments (past IP_REASS_MAX_PBUFS): not answered", most + 1);
    tick(5);

    /* bat0 without its wait: a netif that takes 4 frames and refuses the next. */
    halow.mtu = 1460;
    out_slots = 4;
    out_refused = 0;
    ok = ping_warthog(6000, 1460, 0, 0x502);
    out_slots = -1;
    CHECK(!ok && (REASS ? cap_halow.n == 4 && out_refused == 1 : cap_halow.n == 0),
          "the reply to ping -s 6000 at MTU 1460, its 5th fragment refused: %s (%d sent, %d refused)",
          REASS ? "ip4_frag sends the rest and drops that one, the reply lost" : "no reply", cap_halow.n, out_refused);
    halow.mtu = 1500;
    tick(5);
}

/* Only the first fragment of a 2000-byte datagram from the Pi, at MTU 1460. */
static void lone_first(u32_t dst, u8_t proto, u16_t id, struct netif *inp)
{
    u16_t l = proto == IP_PROTO_ICMP ? icmp_echo(l4, ICMP_ER, 0x4242, 1, 2000) : udp_build(l4, 5353, 5353, 2000, PI, dst);
    u16_t dl = ip_build(dg, PI, dst, proto, id, l4, l);
    (void)feed_n(dg, dl, 1460, inp, 0, 0, 1);
}

/* Any node on the segment can send a fragment to a group address, unanswered by design. */
static void group_frag(u32_t dst, const char *what, u16_t id)
{
    uint32_t r0, d0, r1, d1;
    clear();
    counts(&r0, &d0);
    drv_peak = drv_out;
    const long b0 = live_bytes;
    lone_first(dst, IP_PROTO_UDP, id, &halow);
    const long held = live_bytes - b0;
    const int kept = drv_peak;
    counts(&r1, &d1);
    tick(5);
    const int te = time_exceeded(&cap_halow);
    if (HOOK) {
        CHECK(d1 == d0 + 1 && r1 == r0 && held == 0 && kept == 0 && te == 0,
              "a lone first fragment to %s: dropped and counted, nothing held, no ICMP back", what);
    } else {
        CHECK(REASS ? kept == 1 && te == 1 : kept == 0 && te == 0, "a lone first fragment to %s: %s", what,
              REASS ? "held (a driver buffer kept), then answered with ICMP time exceeded" : "dropped");
    }
}

/* Whole packets whose TCP, UDP or ICMP header is cut short. Stock NAPT reads that header from the first
 * pbuf without a length check (ip_napt_forward, ip_napt_recv) and, on a session match, rewrites its port and
 * checksum past the packet's end; ASan (SAN=1) flags it, as the driver buffer ends with the packet. */
static int nothing_out(void) { return cap_usb.n == 0 && cap_ap.n == 0 && cap_halow.n == 0; }
static void short_split(const char *what, u32_t src, u32_t dst, u8_t proto, const u8_t *l4hdr, u16_t l4len, u16_t pad,
                        u16_t first, struct netif *inp)
{
    u8_t b[128];
    memset(b, 0, sizeof(b));
    const u16_t dl = ip_build(b, src, dst, proto, 0xa00, l4hdr, l4len);
    uint32_t r0, d0, r1, d1;
    counts(&r0, &d0);
    const uint32_t c0 = cut_short();
    clear();
    inject_split(b, (u16_t)(dl + pad), first ? first : (u16_t)(dl + pad), inp);
    counts(&r1, &d1);
    CHECK(nothing_out() && cut_short() == c0 + 1 && r1 == r0 && d1 == d0, "%s: dropped, counted (ip_short_drop), nothing out", what);
}
static void short_one(const char *what, u32_t src, u32_t dst, u8_t proto, const u8_t *l4hdr, u16_t l4len, u16_t pad,
                      struct netif *inp)
{
    short_split(what, src, dst, proto, l4hdr, l4len, pad, 0, inp);
}
/* The NAPTed source port (UDP, TCP) or ICMP id of the one packet out the HaLow side. */
static u16_t napted_port(void)
{
    u16_t port = 0;
    if (cap_halow.n == 1) {
        const int icmp = IPH_PROTO((const struct ip_hdr *)cap_halow.pkt[0].b) == IP_PROTO_ICMP;
        memcpy(&port, cap_halow.pkt[0].b + IP_HLEN + (icmp ? 4 : 0), sizeof(port));
    }
    return port;
}
static void short_headers(void)
{
    u8_t t[20], u[8], e[8], b[64];
    u16_t dl;
    const uint32_t c0 = cut_short();

    /* Exactly the header NAPT reads: NAPTed, which opens the sessions the replies below match. */
    memset(u, 0, sizeof(u));
    ((struct udp_hdr *)u)->src = PP_HTONS(40020);
    ((struct udp_hdr *)u)->dest = PP_HTONS(5353);
    ((struct udp_hdr *)u)->len = PP_HTONS(8);
    ((struct udp_hdr *)u)->chksum = 0;
    clear();
    dl = ip_build(b, HOST, PI, IP_PROTO_UDP, 0xa01, u, 8);
    inject_exact(b, dl, &usb);
    const u16_t uport = napted_port();
    CHECK(cap_halow.n == 1 && ((struct ip_hdr *)cap_halow.pkt[0].b)->src.addr == WH, "host -> Pi UDP, header only (8 bytes): NAPTed");

    memset(t, 0, sizeof(t));
    ((struct tcp_hdr *)t)->src = PP_HTONS(40030);
    ((struct tcp_hdr *)t)->dest = PP_HTONS(80);
    TCPH_HDRLEN_FLAGS_SET((struct tcp_hdr *)t, 5, TCP_SYN);
    clear();
    dl = ip_build(b, HOST, PI, IP_PROTO_TCP, 0xa02, t, 20);
    inject_exact(b, dl, &usb);
    const u16_t tport = napted_port();
    CHECK(cap_halow.n == 1 && ((struct ip_hdr *)cap_halow.pkt[0].b)->src.addr == WH, "host -> Pi TCP SYN, header only (20 bytes): NAPTed");

    memset(e, 0, sizeof(e));
    e[0] = ICMP_ECHO;
    ((struct icmp_echo_hdr *)e)->id = PP_HTONS(0x7777);
    ((struct icmp_echo_hdr *)e)->chksum = inet_chksum(e, 8);
    clear();
    dl = ip_build(b, HOST, PI, IP_PROTO_ICMP, 0xa03, e, 8);
    inject_exact(b, dl, &usb);
    CHECK(cap_halow.n == 1 && ((struct ip_hdr *)cap_halow.pkt[0].b)->src.addr == WH, "host -> Pi ICMP echo, header only (8 bytes): NAPTed");

    /* The Pi's reply to the UDP session, header only: NAPT turns it back to the host. */
    ((struct udp_hdr *)u)->src = PP_HTONS(5353);
    ((struct udp_hdr *)u)->dest = uport;
    clear();
    dl = ip_build(b, PI, WH, IP_PROTO_UDP, 0xa04, u, 8);
    inject_exact(b, dl, &halow);
    CHECK(cap_usb.n == 1 && ((struct ip_hdr *)cap_usb.pkt[0].b)->dest.addr == HOST && cut_short() == c0,
          "Pi -> host UDP reply, header only: at the host; none of these counted");

    /* Through NAPT from the host (ip_napt_forward). */
    ((struct udp_hdr *)u)->src = PP_HTONS(40021);
    ((struct udp_hdr *)u)->dest = PP_HTONS(5353);
    short_one("host -> Pi UDP, 4 bytes (ports only; NAPT would rewrite the checksum after them)", HOST, PI, IP_PROTO_UDP, u, 4, 0, &usb);
    short_one("host -> Pi UDP, no header", HOST, PI, IP_PROTO_UDP, u, 0, 0, &usb);
    short_one("host -> Pi UDP, IP length 24 in a 60-byte frame (NAPT would rewrite the padding)", HOST, PI, IP_PROTO_UDP, u, 4, 36, &usb);
    ((struct tcp_hdr *)t)->src = PP_HTONS(40031);
    short_one("host -> Pi TCP, 12 bytes (no flags)", HOST, PI, IP_PROTO_TCP, t, 12, 0, &usb);
    short_one("host -> Pi TCP SYN, 19 bytes", HOST, PI, IP_PROTO_TCP, t, 19, 0, &usb);
    short_one("host -> Pi ICMP echo, 4 bytes (no id)", HOST, PI, IP_PROTO_ICMP, e, 4, 0, &usb);

    /* To the Warthog's HaLow address (ip_napt_recv), matching the sessions above. */
    ((struct udp_hdr *)u)->src = PP_HTONS(5353);
    ((struct udp_hdr *)u)->dest = uport;
    short_one("Pi -> Warthog UDP to the session's port, 4 bytes", PI, WH, IP_PROTO_UDP, u, 4, 0, &halow);
    ((struct tcp_hdr *)t)->src = PP_HTONS(80);
    ((struct tcp_hdr *)t)->dest = tport;
    TCPH_HDRLEN_FLAGS_SET((struct tcp_hdr *)t, 5, TCP_SYN | TCP_ACK);
    short_one("Pi -> Warthog TCP to the session's port, 12 bytes", PI, WH, IP_PROTO_TCP, t, 12, 0, &halow);
    e[0] = ICMP_ER;
    short_one("Pi -> Warthog ICMP echo reply, 4 bytes (no id)", PI, WH, IP_PROTO_ICMP, e, 4, 0, &halow);
    short_one("Pi -> Warthog ICMP, no header", PI, WH, IP_PROTO_ICMP, e, 0, 0, &halow);

    /* Not NAPT's (between two NAPT netifs): forwarded as it came. */
    ((struct udp_hdr *)u)->src = PP_HTONS(40022);
    clear();
    const uint32_t c1 = cut_short();
    dl = ip_build(b, HOST, AP_HOST, IP_PROTO_UDP, 0xa05, u, 4);
    inject_exact(b, dl, &usb);
    CHECK(cap_ap.n == 1 && ((struct ip_hdr *)cap_ap.pkt[0].b)->src.addr == HOST && cap_ap.pkt[0].len == dl && cut_short() == c1,
          "USB host -> AP host UDP, 4 bytes: forwarded as it came, not counted");

    /* NAPT reads the first pbuf only: one that ends inside the header is short, whatever follows it. */
    ((struct udp_hdr *)u)->src = PP_HTONS(40023);
    short_split("host -> Pi UDP, 8 bytes in two pbufs, the first ending 4 bytes into the header", HOST, PI, IP_PROTO_UDP, u, 8, 0,
                IP_HLEN + 4, &usb);

    /* Without its destination port, wherever it goes: ip4_input reads that port when no netif takes a UDP packet
     * (IP_ACCEPT_LINK_LAYER_ADDRESSING, for DHCP). */
    short_one("Pi -> 10.41.0.50 (no netif's) UDP, no header", PI, A(10, 41, 0, 50), IP_PROTO_UDP, u, 0, 0, &halow);
    short_one("Pi -> 10.41.0.50 UDP, 2 bytes", PI, A(10, 41, 0, 50), IP_PROTO_UDP, u, 2, 0, &halow);
    short_one("USB host -> AP host UDP, no header", HOST, AP_HOST, IP_PROTO_UDP, u, 0, 0, &usb);
    tick(5);
}

/* ip4_input calls ip_napt_recv on one test alone: the destination is the address of the non-NAPT netif the packet
 * came in on, up or down, whatever that address is. A lost lease (and batman's probe) leaves HaLow up at 0.0.0.0,
 * NAPT on and its sessions kept; a lease could also be another netif's subnet broadcast. */
static void lease_lost(void)
{
    u8_t t[20], u[8], e[8], b[64];
    u16_t dl;

    memset(u, 0, sizeof(u));
    ((struct udp_hdr *)u)->src = PP_HTONS(40040);
    ((struct udp_hdr *)u)->dest = PP_HTONS(5353);
    ((struct udp_hdr *)u)->len = PP_HTONS(8);
    clear();
    dl = ip_build(b, HOST, PI, IP_PROTO_UDP, 0xb01, u, 8);
    inject_exact(b, dl, &usb);
    const u16_t uport = napted_port();
    memset(t, 0, sizeof(t));
    ((struct tcp_hdr *)t)->src = PP_HTONS(40050);
    ((struct tcp_hdr *)t)->dest = PP_HTONS(80);
    TCPH_HDRLEN_FLAGS_SET((struct tcp_hdr *)t, 5, TCP_SYN);
    clear();
    dl = ip_build(b, HOST, PI, IP_PROTO_TCP, 0xb02, t, 20);
    inject_exact(b, dl, &usb);
    const u16_t tport = napted_port();
    CHECK(uport != 0 && tport != 0, "host -> Pi UDP and TCP, header only: NAPTed (ports %u, %u)", lwip_ntohs(uport), lwip_ntohs(tport));

    ((struct udp_hdr *)u)->src = PP_HTONS(5353);
    ((struct udp_hdr *)u)->dest = uport;
    ((struct tcp_hdr *)t)->src = PP_HTONS(80);
    ((struct tcp_hdr *)t)->dest = tport;
    TCPH_HDRLEN_FLAGS_SET((struct tcp_hdr *)t, 5, TCP_SYN | TCP_ACK);
    memset(e, 0, sizeof(e));
    ip4_addr_t wh, any, bc;
    wh.addr = WH;
    ip4_addr_set_zero(&any);
    bc.addr = A(192, 168, 4, 255);

    netif_set_ipaddr(&halow, &any);
    short_one("HaLow up at 0.0.0.0, Pi -> 0.0.0.0 UDP to the session's port, 4 bytes", PI, 0, IP_PROTO_UDP, u, 4, 0, &halow);
    short_one("HaLow up at 0.0.0.0, Pi -> 0.0.0.0 TCP to the session's port, 12 bytes", PI, 0, IP_PROTO_TCP, t, 12, 0, &halow);
    short_one("HaLow up at 0.0.0.0, Pi -> 0.0.0.0 ICMP, no header", PI, 0, IP_PROTO_ICMP, e, 0, 0, &halow);
    group_frag(0, "0.0.0.0, HaLow up at 0.0.0.0", 0xb04);

    netif_set_ipaddr(&halow, &wh);
    netif_set_down(&halow);
    short_one("HaLow down, its address kept, Pi -> Warthog UDP to the session's port, 4 bytes", PI, WH, IP_PROTO_UDP, u, 4, 0, &halow);
    short_one("HaLow down, its address kept, Pi -> Warthog TCP to the session's port, 12 bytes", PI, WH, IP_PROTO_TCP, t, 12, 0, &halow);
    /* A first fragment holding 8 TCP bytes: the hook's to reassemble, never NAPT's to read. */
    uint32_t r0, d0, r1, d1;
    counts(&r0, &d0);
    const long before = live_allocs;
    clear();
    dl = ip_build(b, PI, WH, IP_PROTO_TCP, 0xb03, t, 8);
    IPH_OFFSET_SET((struct ip_hdr *)b, PP_HTONS(IP_MF));
    IPH_CHKSUM_SET((struct ip_hdr *)b, 0);
    IPH_CHKSUM_SET((struct ip_hdr *)b, inet_chksum(b, IP_HLEN));
    inject_exact(b, dl, &halow);
    const long held = live_allocs - before;
    tick(5);
    counts(&r1, &d1);
    CHECK(nothing_out() && held > 0 && live_allocs <= before && r1 == r0 && d1 == d0,
          "HaLow down, Pi -> Warthog first TCP fragment of 8 bytes: held as a heap copy, freed by the reassembly timer");
    netif_set_up(&halow);

    netif_set_ipaddr(&halow, &bc);
    short_one("HaLow at 192.168.4.255 (USB's subnet broadcast), Pi -> it UDP to the session's port, 4 bytes", PI, bc.addr,
              IP_PROTO_UDP, u, 4, 0, &halow);
    netif_set_ipaddr(&halow, &wh);
    tick(5);
}

/* Lone first fragments to the Warthog, each its own datagram, 1500 bytes (USB's and plain HaLow's MTU). */
static void flood(void)
{
    clear();
    drv_peak = drv_out;
    const long b0 = live_bytes;
    long peak = 0;
    for (int i = 0; i < 12; i++) {
        u16_t l = icmp_echo(l4, ICMP_ECHO, 0x6060, (u16_t)i, 3000);
        u16_t dl = ip_build(dg, PI, WH, IP_PROTO_ICMP, (u16_t)(0x900 + i), l4, l);
        (void)feed_n(dg, dl, 1500, &halow, 0, 0, 1);
        if (live_bytes - b0 > peak) { peak = live_bytes - b0; }
    }
    const int kept = drv_peak;
    tick(5);
    CHECK(HOOK ? kept == 0 && peak > 10 * 1480 && peak < 16384 : REASS ? kept == 10 : kept == 0,
          "12 lone first fragments to the Warthog: %d driver buffers kept, %ld bytes of lwIP heap at most%s", kept, peak,
          HOOK ? " (10 heap copies, IP_REASS_MAX_PBUFS)" : "");
}

int main(void)
{
    HOST = A(192, 168, 4, 2);
    AP_HOST = A(192, 168, 5, 2);
    PI = A(10, 41, 0, 1);
    WH = A(10, 41, 0, 102);
    printf("=== IDF lwIP, IP fragments and NAPT: %s ===\n", BUILD);

    /* Added in the firmware's order (main.c starts HaLow, then USB, then the AP); netif_add prepends. */
    lwip_init();
    ip4_addr_t a, m, gw;
    a.addr = WH;
    m.addr = A(255, 255, 0, 0);
    gw.addr = PI;
    netif_add(&halow, &a, &m, &gw, &cap_halow, nif_init, ip4_input);
    gw.addr = 0;
    a.addr = A(192, 168, 4, 1);
    m.addr = A(255, 255, 255, 0);
    netif_add(&usb, &a, &m, &gw, &cap_usb, nif_init, ip4_input);
    a.addr = A(192, 168, 5, 1);
    netif_add(&ap, &a, &m, &gw, &cap_ap, nif_init, ip4_input);
    struct netif *all[] = { &usb, &ap, &halow };
    for (int i = 0; i < 3; i++) {
        netif_set_up(all[i]);
        netif_set_link_up(all[i]);
        all[i]->mtu = 1500;
    }
    netif_set_default(&halow);
    tick(2);
    const long base0 = live_allocs;

    /* NAPT not on yet (bridge mode, or no HaLow address): fragments to the Warthog are the hook's too. */
    uint32_t r0, d0, r1, d1;
    counts(&r0, &d0);
    int ok = ping_warthog(2000, 1500, 0, 0x600);
    counts(&r1, &d1);
    CHECK(ok == REASS && r1 == r0 + (uint32_t)HOOK && d1 == d0, "NAPT off: ping -s 2000 to the Warthog %s",
          !ok ? "not answered" : HOOK ? "answered, reassembled by the hook" : "answered by lwIP's own reassembly");
    tick(5);
    CHECK(live_allocs == base0 && drv_out == 0, "NAPT off: every lwIP allocation (%ld outstanding) and driver buffer (%d) returned",
          live_allocs - base0, drv_out);

    /* main/nat.c: NAPT on the inside netifs; HaLow the default route. */
    ip_napt_enable_netif(&usb, 1);
    ip_napt_enable_netif(&ap, 1);
    tick(2);
    const long base = live_allocs;

    napt_cases(1500);
    halow.mtu = 1460; /* batman mode's bat0, and an OpenMANET Pi's */
    napt_cases(1460);
    halow.mtu = 1500;
    reass_limit();
    group_frag(A(10, 41, 255, 255), "the HaLow subnet's broadcast", 0x810);
    group_frag(A(255, 255, 255, 255), "255.255.255.255", 0x811);
    group_frag(A(224, 0, 0, 1), "224.0.0.1", 0x812);
    group_frag(A(192, 168, 4, 255), "the USB subnet's broadcast, in on HaLow", 0x813);
    flood();

    /* Between two NAPT netifs (USB host to AP host) NAPT does nothing: fragments go as they came. */
    clear();
    counts(&r0, &d0);
    u16_t l = icmp_echo(l4, ICMP_ECHO, 0x4343, 1, 2000);
    u16_t dl = ip_build(dg, HOST, AP_HOST, IP_PROTO_ICMP, 0x700, l4, l);
    feed(dg, dl, 1500, &usb, 0, 0);
    struct got g = collect(&cap_ap, out);
    counts(&r1, &d1);
    CHECK(g.len == dl && g.n == 2 && g.src == HOST && r1 == r0, "USB host -> AP host ping -s 2000: forwarded as its 2 fragments, not reassembled (%d)", g.n);

    if (HOOK) {
        /* A first TCP fragment of 8 bytes: NAPT would read the TCP header past the first pbuf. */
        clear();
        counts(&r0, &d0);
        u8_t tcp[1000];
        memset(tcp, 0, sizeof(tcp));
        struct tcp_hdr *t = (struct tcp_hdr *)tcp;
        t->src = lwip_htons(40002);
        t->dest = lwip_htons(80);
        TCPH_HDRLEN_FLAGS_SET(t, 5, TCP_SYN);
        dl = ip_build(dg, HOST, PI, IP_PROTO_TCP, 0x701, tcp, sizeof(tcp));
        int nf = feed(dg, dl, 1500, &usb, 0, 8);
        counts(&r1, &d1);
        CHECK(cap_halow.n == 0 && d1 == d0 + 1, "host -> Pi TCP, first fragment 8 bytes (%d fragments): dropped, counted", nf);

        /* A first fragment with a bad header checksum, then the rest. */
        clear();
        counts(&r0, &d0);
        l = icmp_echo(l4, ICMP_ECHO, 0x4242, 1, 2000);
        dl = ip_build(dg, HOST, PI, IP_PROTO_ICMP, 0x702, l4, l);
        feed_bad = 1;
        nf = feed(dg, dl, 1500, &usb, 0, 0);
        feed_bad = 0;
        g = collect(&cap_halow, out);
        counts(&r1, &d1);
#if CHECKSUM_CHECK_IP
        CHECK(cap_halow.n == 0 && d1 == d0 + 1 && r1 == r0,
              "host -> Pi, %d fragments, the first with a bad header checksum: that one dropped and counted, nothing sent", nf);
#else
        CHECK(g.len == dl && g.src == WH && r1 == r0 + 1 && d1 == d0,
              "host -> Pi, %d fragments, the first with a bad header checksum: reassembled and NAPTed whole, as lwIP takes any "
              "packet without CHECKSUM_CHECK_IP", nf);
#endif
        tick(5);
        clear();

        /* The mailbox full when the datagram is whole. */
        counts(&r0, &d0);
        mb_refuse = 1;
        l = icmp_echo(l4, ICMP_ER, 0x4242, 1, 2000);
        dl = ip_build(dg, PI, WH, IP_PROTO_ICMP, 0x703, l4, l);
        feed(dg, dl, 1460, &halow, 0, 0);
        mb_refuse = 0;
        counts(&r1, &d1);
        CHECK(cap_usb.n == 0 && d1 == d0 + 1 && r1 == r0, "the mailbox full: the reassembled datagram dropped, counted");

        /* Only the first fragment ever arrives: held, then freed by the reassembly timer. */
        clear();
        counts(&r0, &d0);
        const long before = live_allocs;
        lone_first(WH, IP_PROTO_ICMP, 0x704, &halow);
        const long held = live_allocs - before;
        const int kept = drv_out;
        tick(5);
        counts(&r1, &d1);
        CHECK(cap_usb.n == 0 && held > 0 && kept == 0 && live_allocs <= before && r1 == r0,
              "a lone first fragment through NAPT: held as a heap copy (%ld allocations, %d driver buffers), freed by the "
              "reassembly timer", held, kept);

        short_headers();
        lease_lost();

        counts(&r1, &d1);
        printf("     hook: %u datagrams reassembled, %u fragments or datagrams dropped, %u short packets dropped\n",
               (unsigned)r1, (unsigned)d1, (unsigned)cut_short());
    }
    tick(10);
    CHECK(live_allocs == base && drv_out == 0, "every lwIP allocation (%ld outstanding) and driver buffer (%d) returned",
          live_allocs - base, drv_out);
    printf("%s: %d/%d\n", fails ? "FAILED" : "PASSED", checks - fails, checks);
    return fails != 0;
}
