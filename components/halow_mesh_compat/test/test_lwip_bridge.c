/* lwIP's bridge as IDF builds it and as main/mesh_bridge.c uses it for AT+MESHBRIDGE=1: esp-lwip's
 * netif/bridgeif.c and bridgeif_fdb.c with ethernet.c, etharp.c, ip4.c, icmp.c, udp.c and dhcp.c from the
 * PlatformIO ESP-IDF package the firmware links, built for the host (lwip_bridge.mk). The three ports are
 * netifs as esp_netif adds them (input tcpip_input, an Ethernet-like init), in the firmware's order: the
 * mesh netif down and without an address, USB and the Wi-Fi AP up with their NAT addresses. The test then
 * does in lwIP's terms what br_attach_ does through esp_netif: the bridge netif with the mesh MAC and
 * tcpip_input, the three ports with the mesh port first, the ports' addresses cleared, the bridge the
 * default route; the mesh port raised later (warthog_mesh_bridge_port_up), and the DHCP client on the bridge
 * as main/mesh.c starts it. tcpip_inpkt and tcpip_input are the mailbox: queued, run afterwards, as the
 * tcpip thread does. Received frames come as the firmware's drivers hand them over (wlanif_input,
 * ethernetif_input): dropped while the netif is down, else a PBUF_REF pbuf over a driver buffer of exactly
 * the frame's size.
 *
 * What it pins, in the order it runs:
 *   add_port   a port's input goes to the bridge and its ARP flag is cleared; a netif that is not
 *              Ethernet-like and a fourth port are refused and left as they were.
 *   port_addr  an address left on a port makes lwIP route that subnet out of the port itself; cleared,
 *              there is no route until the bridge has an address, then the bridge.
 *   flood      before the mesh port is raised a host's broadcast reaches the AP only and the mesh port's
 *              own receptions are dropped; afterwards a broadcast into USB reaches the AP, the mesh and the
 *              node's stack, never USB; a port whose link is down gets nothing; an unknown unicast is
 *              flooded and not handed to the stack.
 *   learn      after a frame from a host on USB, a unicast to it from the mesh leaves on USB only, and the
 *              host's unicast to the mesh sender leaves on the mesh only.
 *   dhcp       the bridge's DISCOVER has the mesh MAC as its source, carries the hostname the firmware gives
 *              the bridge and leaves on every port; an OFFER and an ACK fed to the mesh port bind the
 *              bridge; the ports stay at 0.0.0.0.
 *   host_dhcp  a host's DISCOVER from USB reaches the mesh unchanged; the server's unicast OFFER fed to the
 *              mesh port leaves on USB only, unchanged; the node answers neither.
 *   ping_ref   an echo request to the bridge's address in a PBUF_REF pbuf is answered on the port it came
 *              in on, from USB and from the mesh.
 *   ping_copy  the same request in a PBUF_RAM copy with headroom, what CONFIG_LWIP_L2_TO_L3_COPY makes of a
 *              received frame, is NOT answered: lwIP answers in the request's own pbuf and its bridge never
 *              sends a pbuf out of the port it came in on. This is the lwIP behaviour main/mesh_bridge.c's
 *              #error guards; if this check ever fails, lwIP has changed and the #error can go.
 *   flood_err  with one port refusing, the node's own multicast datagram (AT+MSEND's) still reaches the
 *              other two ports and the send returns the refusing port's error.
 *   undo       a port given its input and ARP flag back (br_attach_'s undo) answers ARP itself again; its
 *              frames are neither flooded nor learned by the bridge.
 * Every lwIP allocation and driver buffer is returned. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/inet_chksum.h"
#include "lwip/init.h"
#include "lwip/ip4.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/dhcp.h"
#include "lwip/prot/etharp.h"
#include "lwip/prot/ethernet.h"
#include "lwip/prot/iana.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/udp.h"
#include "lwip/udp.h"
#include "netif/bridgeif.h"
#include "netif/ethernet.h"

/* Every lwIP allocation counted. */
static long live_allocs;
void *lwip_test_malloc(size_t n)
{
    void *p = malloc(n);
    if (p != NULL) { live_allocs++; }
    return p;
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
    live_allocs--;
    free(p);
}

u32_t sys_now(void) { return 0; }

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

static struct netif br;
static int to_stack; /* frames handed to the bridge netif's own input: the node's stack */

static struct { struct pbuf *p; struct netif *n; netif_input_fn fn; } mbox[64];
static int mb_head, mb_tail;
err_t tcpip_inpkt(struct pbuf *p, struct netif *inp, netif_input_fn input_fn)
{
    if (mb_tail - mb_head >= 64) { return ERR_MEM; }
    to_stack += inp == &br;
    mbox[mb_tail % 64].p = p;
    mbox[mb_tail % 64].n = inp;
    mbox[mb_tail % 64].fn = input_fn;
    mb_tail++;
    return ERR_OK;
}
/* lwIP's own (api/tcpip.c), which esp_netif gives every netif as its input. */
err_t tcpip_input(struct pbuf *p, struct netif *inp)
{
    if (inp->flags & (NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET)) {
        return tcpip_inpkt(p, inp, ethernet_input);
    }
    return tcpip_inpkt(p, inp, ip4_input);
}
static void run_mbox(void)
{
    while (mb_head < mb_tail) {
        int i = mb_head++ % 64;
        if (mbox[i].fn(mbox[i].p, mbox[i].n) != ERR_OK) { /* tcpip_thread_handle_msg frees what its input refuses */
            pbuf_free(mbox[i].p);
        }
    }
}

/* A port: what lwIP sent out of it, and a switch to make its driver refuse. */
#define MAXOUT 16
#define MAXFRAME 700
struct port {
    struct netif nif;
    const char *name;
    u8_t mac[6];
    int n, lost;
    struct { u8_t b[MAXFRAME]; u16_t len; } pkt[MAXOUT];
    err_t refuse;
    int refused;
};
static struct port mesh = { .name = "ms", .mac = { 0x3c, 0x1a, 0xcc, 0x4c, 0x73, 0x8d } };
static struct port usb  = { .name = "us", .mac = { 0x02, 0x72, 0xa1, 0xf8, 0x73, 0x8e } };
static struct port ap   = { .name = "ap", .mac = { 0x8c, 0xbf, 0xea, 0x10, 0x20, 0x31 } };
static struct port *const ports[3] = { &mesh, &usb, &ap };

static err_t port_tx(struct netif *nif, struct pbuf *p)
{
    struct port *pt = nif->state;
    if (pt->refuse != ERR_OK) {
        pt->refused++;
        return pt->refuse;
    }
    if (pt->n < MAXOUT && p->tot_len <= MAXFRAME) {
        pt->pkt[pt->n].len = pbuf_copy_partial(p, pt->pkt[pt->n].b, p->tot_len, 0);
        pt->n++;
    } else {
        pt->lost++;
    }
    return ERR_OK;
}
/* As wlanif.c's and ethernetif.c's low_level_init set a netif up. */
static err_t port_init(struct netif *n)
{
    const struct port *pt = n->state;
    n->name[0] = pt->name[0];
    n->name[1] = pt->name[1];
    n->output = etharp_output;
    n->linkoutput = port_tx;
    n->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(n->hwaddr, pt->mac, ETH_HWADDR_LEN);
    n->mtu = 1500;
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    return ERR_OK;
}
static void clear(void)
{
    for (int i = 0; i < 3; i++) {
        ports[i]->n = ports[i]->lost = ports[i]->refused = 0;
    }
}

/* A driver's receive buffer of exactly the frame's size (so ASan sees a read past it) under a PBUF_REF
 * pbuf, as esp_pbuf_allocate makes it; back to the driver when lwIP frees it. */
struct drv { struct pbuf_custom pc; u8_t *b; };
static int drv_out, rx_down;
static void drv_free(struct pbuf *p)
{
    struct drv *d = (struct drv *)(void *)p;
    drv_out--;
    free(d->b);
    free(d);
}
/* One received frame as wlanif_input and ethernetif_input hand it to lwIP, then the mailbox runs.
 * copy: a PBUF_RAM copy in place of the driver's buffer, as wlanif_input makes with CONFIG_LWIP_L2_TO_L3_COPY. */
static void rx(struct port *pt, const u8_t *frame, u16_t len, int copy)
{
    struct pbuf *p;
    if (!netif_is_up(&pt->nif)) {
        rx_down++;
        return;
    }
    if (copy) {
        p = pbuf_alloc(PBUF_RAW, len, PBUF_RAM);
        memcpy(p->payload, frame, len);
    } else {
        struct drv *d = malloc(sizeof(*d));
        d->b = malloc(len);
        memcpy(d->b, frame, len);
        d->pc.custom_free_function = drv_free;
        drv_out++;
        p = pbuf_alloced_custom(PBUF_RAW, len, PBUF_REF, &d->pc, d->b, len);
    }
    if (pt->nif.input(p, &pt->nif) != ERR_OK) {
        pbuf_free(p);
    }
    run_mbox();
}

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL "); } else { printf("ok   "); } \
                           printf(__VA_ARGS__); printf("\n"); } while (0)

static u32_t A(unsigned a, unsigned b, unsigned c, unsigned d) { return PP_HTONL((a << 24) | (b << 16) | (c << 8) | d); }
static const u8_t BC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const u8_t H[6]  = { 0x02, 0x72, 0xa1, 0xf8, 0x73, 0x8d }; /* the host on USB */
static const u8_t PI[6] = { 0x00, 0x0a, 0x52, 0x06, 0x7a, 0x11 }; /* a mesh node: the DHCP server and gateway */
static const u8_t X[6]  = { 0x02, 0xde, 0xad, 0xbe, 0xef, 0x01 }; /* a MAC nobody has heard from */
static const u8_t H3[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x03 }; /* a host first heard on an undone port */

#define ETH_HLEN 14
#define L4 (ETH_HLEN + IP_HLEN)     /* the UDP or ICMP header in a frame */
#define BOOTP (L4 + UDP_HLEN)       /* the BOOTP message in a DHCP frame */

static u16_t eth_put(u8_t *f, const u8_t *dst, const u8_t *src, u16_t type)
{
    memcpy(f, dst, 6);
    memcpy(f + 6, src, 6);
    f[12] = (u8_t)(type >> 8);
    f[13] = (u8_t)type;
    return ETH_HLEN;
}
static u16_t arp_frame(u8_t *f, const u8_t *eth_dst, u16_t op, const u8_t *sha, u32_t spa, const u8_t *tha, u32_t tpa)
{
    struct etharp_hdr a;
    memset(&a, 0, sizeof(a));
    a.hwtype = PP_HTONS(LWIP_IANA_HWTYPE_ETHERNET);
    a.proto = PP_HTONS(ETHTYPE_IP);
    a.hwlen = ETH_HWADDR_LEN;
    a.protolen = sizeof(ip4_addr_t);
    a.opcode = lwip_htons(op);
    memcpy(&a.shwaddr, sha, 6);
    memcpy(&a.sipaddr, &spa, 4);
    if (tha != NULL) { memcpy(&a.dhwaddr, tha, 6); }
    memcpy(&a.dipaddr, &tpa, 4);
    eth_put(f, eth_dst, sha, ETHTYPE_ARP);
    memcpy(f + ETH_HLEN, &a, sizeof(a));
    return (u16_t)(ETH_HLEN + sizeof(a));
}
static u16_t ip_frame(u8_t *f, const u8_t *dst, const u8_t *src, u32_t sip, u32_t dip, u8_t proto, u16_t l4len)
{
    struct ip_hdr *h = (struct ip_hdr *)(void *)(f + ETH_HLEN);
    eth_put(f, dst, src, ETHTYPE_IP);
    memset(h, 0, IP_HLEN);
    IPH_VHL_SET(h, 4, 5);
    IPH_LEN_SET(h, lwip_htons((u16_t)(IP_HLEN + l4len)));
    IPH_TTL_SET(h, 64);
    IPH_PROTO_SET(h, proto);
    h->src.addr = sip;
    h->dest.addr = dip;
    IPH_CHKSUM_SET(h, inet_chksum(h, IP_HLEN));
    return (u16_t)(L4 + l4len);
}
static u16_t udp_frame(u8_t *f, const u8_t *dst, const u8_t *src, u32_t sip, u32_t dip, u16_t sport, u16_t dport,
                       const u8_t *data, u16_t n)
{
    struct udp_hdr *u = (struct udp_hdr *)(void *)(f + L4);
    u->src = lwip_htons(sport);
    u->dest = lwip_htons(dport);
    u->len = lwip_htons((u16_t)(UDP_HLEN + n));
    u->chksum = 0; /* none, which IPv4 allows */
    if (data != NULL) { memcpy(f + BOOTP, data, n); }
    return ip_frame(f, dst, src, sip, dip, IP_PROTO_UDP, (u16_t)(UDP_HLEN + n));
}
static u16_t echo_frame(u8_t *f, const u8_t *dst, const u8_t *src, u32_t sip, u32_t dip, u16_t seq)
{
    struct icmp_echo_hdr *e = (struct icmp_echo_hdr *)(void *)(f + L4);
    const u16_t size = 56;
    memset(e, 0, sizeof(*e));
    e->type = ICMP_ECHO;
    e->id = PP_HTONS(0x4831);
    e->seqno = lwip_htons(seq);
    for (u16_t i = 0; i < size; i++) { f[L4 + 8 + i] = (u8_t)i; }
    e->chksum = inet_chksum(e, (u16_t)(8 + size));
    return ip_frame(f, dst, src, sip, dip, IP_PROTO_ICMP, (u16_t)(8 + size));
}
/* A DHCP message: op 1 from a client (chaddr its MAC), 2 from the server. Returns the BOOTP length. */
static u16_t bootp_put(u8_t *m, u8_t op, u32_t xid_net, const u8_t *chaddr, u32_t yiaddr, u8_t type, u32_t server,
                       u32_t mask, u32_t router)
{
    u16_t o = DHCP_OPTIONS_OFS;
    const u32_t cookie = PP_HTONL(DHCP_MAGIC_COOKIE), lease = PP_HTONL(3600);
    memset(m, 0, DHCP_OPTIONS_OFS);
    m[0] = op;
    m[1] = LWIP_IANA_HWTYPE_ETHERNET;
    m[2] = ETH_HWADDR_LEN;
    memcpy(m + 4, &xid_net, 4);
    memcpy(m + 16, &yiaddr, 4);
    memcpy(m + 28, chaddr, 6);
    memcpy(m + DHCP_MSG_LEN, &cookie, 4);
    m[o++] = DHCP_OPTION_MESSAGE_TYPE; m[o++] = 1; m[o++] = type;
    if (op == DHCP_BOOTREPLY) {
        m[o++] = DHCP_OPTION_SERVER_ID; m[o++] = 4; memcpy(m + o, &server, 4); o += 4;
        m[o++] = DHCP_OPTION_LEASE_TIME; m[o++] = 4; memcpy(m + o, &lease, 4); o += 4;
        m[o++] = DHCP_OPTION_SUBNET_MASK; m[o++] = 4; memcpy(m + o, &mask, 4); o += 4;
        m[o++] = DHCP_OPTION_ROUTER; m[o++] = 4; memcpy(m + o, &router, 4); o += 4;
    }
    m[o++] = DHCP_OPTION_END;
    return o;
}
/* The value of option @p code in the DHCP frame @p f, or NULL; its length in @p len. */
static const u8_t *dhcp_opt(const u8_t *f, u16_t flen, u8_t code, u8_t *len)
{
    u16_t o = BOOTP + DHCP_OPTIONS_OFS;
    while (o + 1 < flen && f[o] != DHCP_OPTION_END) {
        if (f[o] == DHCP_OPTION_PAD) { o++; continue; }
        if (f[o] == code && o + 2 + f[o + 1] <= flen) {
            *len = f[o + 1];
            return f + o + 2;
        }
        o = (u16_t)(o + 2 + f[o + 1]);
    }
    return NULL;
}
static int is_dhcp(const struct port *pt, int i, u8_t type, const u8_t *eth_src)
{
    u8_t n = 0;
    const u8_t *f = pt->pkt[i].b;
    const u16_t len = pt->pkt[i].len;
    const u8_t *t = len > BOOTP + DHCP_OPTIONS_OFS ? dhcp_opt(f, len, DHCP_OPTION_MESSAGE_TYPE, &n) : NULL;
    return t != NULL && n == 1 && t[0] == type && memcmp(f, BC, 6) == 0 && memcmp(f + 6, eth_src, 6) == 0 &&
           f[12] == 0x08 && f[13] == 0x00 && f[BOOTP] == DHCP_BOOTREQUEST && memcmp(f + BOOTP + 28, eth_src, 6) == 0;
}
static int same(const struct port *pt, int i, const u8_t *f, u16_t len)
{
    return pt->n > i && pt->pkt[i].len == len && memcmp(pt->pkt[i].b, f, len) == 0;
}
static u16_t arp_op(const struct port *pt, int i)
{
    const u8_t *f = pt->pkt[i].b;
    return (pt->n > i && pt->pkt[i].len >= ETH_HLEN + 28 && f[12] == 0x08 && f[13] == 0x06) ? (u16_t)(f[20] << 8 | f[21]) : 0;
}
/* The echo reply to echo_frame(..., seq) from @p sip at @p smac to @p dip at @p dmac. */
static int is_echo_reply(const struct port *pt, int i, const u8_t *dmac, const u8_t *smac, u32_t sip, u32_t dip, u16_t seq)
{
    u8_t want[L4 + 8 + 56];
    const u8_t *f = pt->pkt[i].b;
    if (pt->n <= i || pt->pkt[i].len != sizeof(want)) { return 0; }
    (void)echo_frame(want, dmac, smac, sip, dip, seq);
    return memcmp(f, dmac, 6) == 0 && memcmp(f + 6, smac, 6) == 0 && f[12] == 0x08 && f[13] == 0x00 &&
           memcmp(f + ETH_HLEN + 12, &sip, 4) == 0 && memcmp(f + ETH_HLEN + 16, &dip, 4) == 0 && f[L4] == ICMP_ER &&
           memcmp(f + L4 + 4, want + L4 + 4, 4 + 56) == 0 && inet_chksum(f + L4, 8 + 56) == 0;
}

int main(void)
{
    static u8_t f[MAXFRAME], g[MAXFRAME];
    u16_t n, m;
    ip4_addr_t a, mask, gw, none;
    const u32_t LEASE = A(10, 41, 0, 160), MASK16 = A(255, 255, 0, 0), GW = A(10, 41, 254, 1);
    const u32_t HOST_IP = A(10, 41, 0, 50);
    printf("=== IDF lwIP: the bridge of AT+MESHBRIDGE=1 (netif/bridgeif.c) ===\n");

    /* NAT mode as booted. Added in the firmware's order (HaLow, then USB, then the AP). */
    lwip_init();
    ip4_addr_set_zero(&none);
    netif_add(&mesh.nif, &none, &none, &none, &mesh, port_init, tcpip_input);
    mask.addr = A(255, 255, 255, 0);
    a.addr = A(192, 168, 4, 1);
    netif_add(&usb.nif, &a, &mask, &none, &usb, port_init, tcpip_input);
    a.addr = A(192, 168, 5, 1);
    netif_add(&ap.nif, &a, &mask, &none, &ap, port_init, tcpip_input);
    netif_set_up(&usb.nif);
    netif_set_link_up(&usb.nif);
    netif_set_up(&ap.nif);
    netif_set_link_up(&ap.nif);

    /* ---- add_port: what esp_netif_action_start and esp_netif_bridge_add_port do in lwIP ---- */
    bridgeif_initdata_t init = { .max_ports = 3, .max_fdb_dynamic_entries = 64, .max_fdb_static_entries = 8 };
    memcpy(&init.ethaddr, mesh.mac, ETH_HWADDR_LEN);
    CHECK(netif_add(&br, &none, &none, &none, &init, bridgeif_init, tcpip_input) == &br &&
          memcmp(br.hwaddr, mesh.mac, 6) == 0 && netif_is_link_up(&br) && !netif_is_up(&br) && br.linkoutput != NULL &&
          strcmp(br.hostname, "lwip") == 0,
          "the bridge netif: the mesh MAC, link up, down, and lwIP's own hostname until the firmware names it (%s)", br.hostname);
    netif_set_hostname(&br, "espressif"); /* esp_netif_set_hostname(br, CONFIG_LWIP_LOCAL_HOSTNAME) */
    struct netif odd, extra;
    memset(&odd, 0, sizeof(odd));
    odd.flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
    odd.input = tcpip_input;
    CHECK(bridgeif_add_port(&br, &odd) != ERR_OK && odd.input == tcpip_input && (odd.flags & NETIF_FLAG_ETHARP) != 0,
          "add_port: a netif without NETIF_FLAG_ETHERNET is refused and left as it was");
    int joined = 0, redirected = 0, noarp = 0;
    for (int i = 0; i < 3; i++) { /* the mesh port first, as br_attach_ */
        joined += bridgeif_add_port(&br, &ports[i]->nif) == ERR_OK;
        redirected += ports[i]->nif.input != tcpip_input;
        noarp += (ports[i]->nif.flags & (NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET)) == NETIF_FLAG_ETHERNET;
    }
    CHECK(joined == 3 && redirected == 3 && noarp == 3,
          "add_port: three ports join; each one's input goes to the bridge and its ARP flag is cleared (%d %d %d)",
          joined, redirected, noarp);
    memset(&extra, 0, sizeof(extra));
    extra.flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    extra.input = tcpip_input;
    CHECK(bridgeif_add_port(&br, &extra) != ERR_OK && extra.input == tcpip_input && (extra.flags & NETIF_FLAG_ETHARP) != 0,
          "add_port: a fourth port is refused (max_ports 3) and left as it was");

    /* ---- port_addr: why br_attach_ clears the ports' addresses ---- */
    a.addr = A(192, 168, 4, 2);
    CHECK(ip4_route(&a) == &usb.nif, "port_addr: with 192.168.4.1 left on the USB port, lwIP routes 192.168.4.2 out of the port itself");
    netif_set_addr(&usb.nif, &none, &none, &none); /* esp_netif_set_ip_info(port, 0.0.0.0) on a netif that is up */
    netif_set_addr(&ap.nif, &none, &none, &none);
    netif_set_default(&br); /* esp_netif_set_default_netif(br) */
    CHECK(ip4_route(&a) == NULL, "port_addr: cleared, and the bridge down without an address: no route (a send fails cleanly)");

    /* ---- flood ---- */
    clear();
    n = arp_frame(f, BC, ARP_REQUEST, H, HOST_IP, NULL, A(10, 41, 99, 99));
    rx(&usb, f, n, 0);
    CHECK(usb.n == 0 && mesh.n == 0 && same(&ap, 0, f, n) && ap.n == 1,
          "flood, mesh port not raised yet: a host's broadcast from USB reaches the AP only (usb %d ap %d mesh %d)", usb.n, ap.n, mesh.n);
    rx_down = 0;
    m = arp_frame(g, BC, ARP_REQUEST, PI, GW, NULL, A(10, 41, 99, 98));
    clear();
    rx(&mesh, g, m, 0);
    CHECK(rx_down == 1 && usb.n == 0 && ap.n == 0, "flood, mesh port not raised yet: what the mesh receives is dropped at the netif (down)");
    netif_set_up(&mesh.nif); /* br_mesh_up_: the first peer is established */
    netif_set_link_up(&mesh.nif);
    clear();
    to_stack = 0;
    rx(&usb, f, n, 0);
    CHECK(usb.n == 0 && ap.n == 1 && mesh.n == 1 && same(&ap, 0, f, n) && same(&mesh, 0, f, n) && to_stack == 1,
          "flood: a broadcast into USB reaches the AP, the mesh and the node's stack, never USB (usb %d ap %d mesh %d stack %d)",
          usb.n, ap.n, mesh.n, to_stack);
    netif_set_link_down(&ap.nif);
    clear();
    rx(&usb, f, n, 0);
    CHECK(usb.n == 0 && ap.n == 0 && mesh.n == 1, "flood: a port whose link is down gets nothing (ap %d mesh %d)", ap.n, mesh.n);
    netif_set_link_up(&ap.nif);
    clear();
    to_stack = 0;
    n = udp_frame(f, X, H, HOST_IP, A(10, 41, 7, 7), 4000, 4001, (const u8_t *)"unknown", 7);
    rx(&usb, f, n, 0);
    CHECK(usb.n == 0 && same(&ap, 0, f, n) && same(&mesh, 0, f, n) && ap.n == 1 && mesh.n == 1 && to_stack == 0,
          "flood: a unicast to a MAC nobody has heard from goes to the other ports and not to the node's stack (ap %d mesh %d stack %d)",
          ap.n, mesh.n, to_stack);

    /* ---- learn ---- */
    clear();
    to_stack = 0;
    n = udp_frame(f, H, PI, GW, HOST_IP, 5000, 5001, (const u8_t *)"to the host", 11);
    rx(&mesh, f, n, 0);
    CHECK(same(&usb, 0, f, n) && usb.n == 1 && ap.n == 0 && mesh.n == 0 && to_stack == 0,
          "learn: a unicast from the mesh to a host heard on USB leaves on USB only, unchanged (usb %d ap %d mesh %d)", usb.n, ap.n, mesh.n);
    clear();
    n = udp_frame(f, PI, H, HOST_IP, GW, 5001, 5000, (const u8_t *)"and back", 8);
    rx(&usb, f, n, 0);
    CHECK(same(&mesh, 0, f, n) && mesh.n == 1 && ap.n == 0 && usb.n == 0,
          "learn: the host's unicast to that mesh node leaves on the mesh only (usb %d ap %d mesh %d)", usb.n, ap.n, mesh.n);

    /* ---- dhcp: main/mesh.c on the first peer (esp_netif_action_connected, the DHCP client) ---- */
    clear();
    netif_set_up(&br);
    netif_set_link_up(&br);
    CHECK(dhcp_start(&br) == ERR_OK, "dhcp: the client starts on the bridge");
    {
        u8_t hl = 0;
        const u8_t *hn = mesh.n == 1 ? dhcp_opt(mesh.pkt[0].b, mesh.pkt[0].len, DHCP_OPTION_HOSTNAME, &hl) : NULL;
        CHECK(usb.n == 1 && ap.n == 1 && mesh.n == 1 && is_dhcp(&mesh, 0, DHCP_DISCOVER, mesh.mac) &&
              same(&usb, 0, mesh.pkt[0].b, mesh.pkt[0].len) && same(&ap, 0, mesh.pkt[0].b, mesh.pkt[0].len),
              "dhcp: one DISCOVER on every port, the same frame, its source and chaddr the mesh MAC (usb %d ap %d mesh %d)",
              usb.n, ap.n, mesh.n);
        CHECK(hn != NULL && hl == 9 && memcmp(hn, "espressif", 9) == 0, "dhcp: the DISCOVER carries the hostname the mesh netif sends");
    }
    u32_t xid;
    memcpy(&xid, mesh.pkt[0].b + BOOTP + 4, 4);
    m = bootp_put(g, DHCP_BOOTREPLY, xid, mesh.mac, LEASE, DHCP_OFFER, GW, MASK16, GW);
    n = udp_frame(f, mesh.mac, PI, GW, LEASE, LWIP_IANA_PORT_DHCP_SERVER, LWIP_IANA_PORT_DHCP_CLIENT, g, m);
    clear();
    rx(&mesh, f, n, 0);
    {
        u8_t l1 = 0, l2 = 0;
        const u8_t *req = mesh.n == 1 ? dhcp_opt(mesh.pkt[0].b, mesh.pkt[0].len, DHCP_OPTION_REQUESTED_IP, &l1) : NULL;
        const u8_t *sid = mesh.n == 1 ? dhcp_opt(mesh.pkt[0].b, mesh.pkt[0].len, DHCP_OPTION_SERVER_ID, &l2) : NULL;
        CHECK(usb.n == 1 && ap.n == 1 && mesh.n == 1 && is_dhcp(&mesh, 0, DHCP_REQUEST, mesh.mac) && is_dhcp(&usb, 0, DHCP_REQUEST, mesh.mac) &&
              req != NULL && l1 == 4 && memcmp(req, &LEASE, 4) == 0 && sid != NULL && l2 == 4 && memcmp(sid, &GW, 4) == 0,
              "dhcp: an OFFER fed to the mesh port is taken by the bridge (no address yet) and answered with a REQUEST "
              "for it on every port; the OFFER itself is not flooded (usb %d ap %d mesh %d)", usb.n, ap.n, mesh.n);
    }
    if (mesh.n == 1) { memcpy(&xid, mesh.pkt[0].b + BOOTP + 4, 4); }
    m = bootp_put(g, DHCP_BOOTREPLY, xid, mesh.mac, LEASE, DHCP_ACK, GW, MASK16, GW);
    n = udp_frame(f, mesh.mac, PI, GW, LEASE, LWIP_IANA_PORT_DHCP_SERVER, LWIP_IANA_PORT_DHCP_CLIENT, g, m);
    rx(&mesh, f, n, 0);
    CHECK(netif_ip4_addr(&br)->addr == LEASE && netif_ip4_netmask(&br)->addr == MASK16 && netif_ip4_gw(&br)->addr == GW &&
          dhcp_supplied_address(&br), "dhcp: the ACK binds the bridge to 10.41.0.160/16 via 10.41.254.1");
    CHECK(ip4_addr_isany_val(*netif_ip4_addr(&mesh.nif)) && ip4_addr_isany_val(*netif_ip4_addr(&usb.nif)) &&
          ip4_addr_isany_val(*netif_ip4_addr(&ap.nif)), "dhcp: the ports stay at 0.0.0.0");
    a.addr = A(192, 168, 4, 2);
    gw.addr = A(8, 8, 8, 8);
    CHECK(ip4_route(&a) == &br && ip4_route(&gw) == &br, "port_addr: with the ports cleared, everything routes out of the bridge");
    const long base = live_allocs; /* the bridge, its FDB, the DHCP client and lwIP's timers stay */

    /* ---- host_dhcp ---- */
    clear();
    to_stack = 0;
    m = bootp_put(g, DHCP_BOOTREQUEST, PP_HTONL(0x1234abcd), H, 0, DHCP_DISCOVER, 0, 0, 0);
    n = udp_frame(f, BC, H, 0, A(255, 255, 255, 255), LWIP_IANA_PORT_DHCP_CLIENT, LWIP_IANA_PORT_DHCP_SERVER, g, m);
    rx(&usb, f, n, 0);
    CHECK(same(&mesh, 0, f, n) && mesh.n == 1 && same(&ap, 0, f, n) && ap.n == 1 && usb.n == 0 && to_stack == 1,
          "host_dhcp: a host's DISCOVER from USB reaches the mesh unchanged, and the node answers nothing (usb %d ap %d mesh %d)",
          usb.n, ap.n, mesh.n);
    clear();
    to_stack = 0;
    m = bootp_put(g, DHCP_BOOTREPLY, PP_HTONL(0x1234abcd), H, HOST_IP, DHCP_OFFER, GW, MASK16, GW);
    n = udp_frame(f, H, PI, GW, HOST_IP, LWIP_IANA_PORT_DHCP_SERVER, LWIP_IANA_PORT_DHCP_CLIENT, g, m);
    rx(&mesh, f, n, 0);
    CHECK(same(&usb, 0, f, n) && usb.n == 1 && ap.n == 0 && mesh.n == 0 && to_stack == 0,
          "host_dhcp: the server's unicast OFFER fed to the mesh port leaves on USB only, unchanged (usb %d ap %d mesh %d stack %d)",
          usb.n, ap.n, mesh.n, to_stack);

    /* ---- ping_ref ---- */
    clear();
    n = arp_frame(f, BC, ARP_REQUEST, H, HOST_IP, NULL, LEASE);
    rx(&usb, f, n, 0);
    CHECK(usb.n == 1 && arp_op(&usb, 0) == ARP_REPLY && memcmp(usb.pkt[0].b, H, 6) == 0 && memcmp(usb.pkt[0].b + 6, mesh.mac, 6) == 0 &&
          memcmp(usb.pkt[0].b + ETH_HLEN + 8, mesh.mac, 6) == 0 && ap.n == 1 && mesh.n == 1,
          "ping_ref: the host's ARP for the bridge's address is answered on USB with the mesh MAC (usb %d ap %d mesh %d)", usb.n, ap.n, mesh.n);
    clear();
    n = echo_frame(f, mesh.mac, H, HOST_IP, LEASE, 1);
    rx(&usb, f, n, 0);
    CHECK(usb.n == 1 && is_echo_reply(&usb, 0, H, mesh.mac, LEASE, HOST_IP, 1) && ap.n == 0 && mesh.n == 0,
          "ping_ref: an echo request from USB in the driver's own buffer is answered on USB (usb %d ap %d mesh %d)", usb.n, ap.n, mesh.n);
    clear();
    n = arp_frame(f, BC, ARP_REQUEST, PI, GW, NULL, LEASE);
    rx(&mesh, f, n, 0);
    const int arp_ok = mesh.n == 1 && arp_op(&mesh, 0) == ARP_REPLY;
    clear();
    n = echo_frame(f, mesh.mac, PI, GW, LEASE, 2);
    rx(&mesh, f, n, 0);
    CHECK(arp_ok && mesh.n == 1 && is_echo_reply(&mesh, 0, PI, mesh.mac, LEASE, GW, 2) && ap.n == 0 && usb.n == 0,
          "ping_ref: and one from a mesh node on the mesh (usb %d ap %d mesh %d)", usb.n, ap.n, mesh.n);

    /* ---- ping_copy: the lwIP behaviour main/mesh_bridge.c's #error on CONFIG_LWIP_L2_TO_L3_COPY guards ---- */
    clear();
    n = echo_frame(f, mesh.mac, PI, GW, LEASE, 3);
    rx(&mesh, f, n, 1);
    CHECK(mesh.n == 0 && usb.n == 0 && ap.n == 0,
          "ping_copy: the same request in a PBUF_RAM copy (CONFIG_LWIP_L2_TO_L3_COPY) is NOT answered: lwIP replies in the "
          "request's pbuf and its bridge never sends one out of the port it came in on (usb %d ap %d mesh %d). "
          "If this fails, the #error in main/mesh_bridge.c can go", usb.n, ap.n, mesh.n);

    /* ---- flood_err: AT+MSEND's datagram with one port refusing ---- */
    {
        struct udp_pcb *pcb = udp_new();
        ip_addr_t grp;
        ip_addr_set_ip4_u32(&grp, A(239, 0, 0, 69));
        int ok = pcb != NULL;
        err_t e = ERR_OK;
        for (int i = 0; ok && i <= 3; i++) { /* i == 3: nobody refuses */
            struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 2, PBUF_RAM);
            memcpy(p->payload, "h1", 2);
            clear();
            if (i < 3) { ports[i]->refuse = ERR_IF; }
            e = udp_sendto_if(pcb, p, &grp, 4403, &br);
            pbuf_free(p);
            int got = 0;
            for (int k = 0; k < 3; k++) {
                got += ports[k]->n == 1 && ports[k]->pkt[0].b[0] == 0x01 && ports[k]->pkt[0].b[2] == 0x5e &&
                       memcmp(ports[k]->pkt[0].b + 6, mesh.mac, 6) == 0;
            }
            if (i < 3) {
                ok = e == ERR_IF && got == 2 && ports[i]->n == 0 && ports[i]->refused == 1;
                ports[i]->refuse = ERR_OK;
            } else {
                ok = e == ERR_OK && got == 3;
            }
        }
        CHECK(ok, "flood_err: with any one port refusing, the node's own multicast datagram still reaches the other two "
                  "and the send returns that port's error; with none refusing it returns OK (last %d)", (int)e);
        if (pcb != NULL) { udp_remove(pcb); }
    }
    CHECK(drv_out == 0 && live_allocs == base, "every lwIP allocation (%ld outstanding) and driver buffer (%d) returned",
          live_allocs - base, drv_out);

    /* ---- undo: what br_attach_ does to the ports that joined before one was refused ---- */
    usb.nif.input = tcpip_input;
    netif_set_flags(&usb.nif, NETIF_FLAG_ETHARP);
    mask.addr = A(255, 255, 255, 0);
    a.addr = A(192, 168, 4, 1);
    netif_set_addr(&usb.nif, &a, &mask, &none); /* in the firmware it never lost it: the commit had not run */
    clear();
    to_stack = 0;
    n = arp_frame(f, BC, ARP_REQUEST, H3, A(192, 168, 4, 2), NULL, A(192, 168, 4, 1));
    rx(&usb, f, n, 0);
    CHECK(usb.n >= 1 && arp_op(&usb, usb.n - 1) == ARP_REPLY && memcmp(usb.pkt[usb.n - 1].b, H3, 6) == 0 &&
          memcmp(usb.pkt[usb.n - 1].b + 6, usb.mac, 6) == 0 && ap.n == 0 && mesh.n == 0 && to_stack == 0,
          "undo: a port given its input and ARP flag back answers ARP for its own address itself, and its frames "
          "are not flooded or handed to the bridge (usb %d ap %d mesh %d stack %d)", usb.n, ap.n, mesh.n, to_stack);
    clear();
    n = udp_frame(f, H3, PI, GW, A(192, 168, 4, 2), 5000, 5001, (const u8_t *)"unlearned", 9);
    rx(&mesh, f, n, 0);
    CHECK(ap.n == 1 && usb.n == 1, "undo: and the bridge did not learn that host: a unicast to it from the mesh is flooded (usb %d ap %d)",
          usb.n, ap.n);
    CHECK(drv_out == 0 && usb.lost + ap.lost + mesh.lost == 0, "every driver buffer returned (%d out)", drv_out);

    printf("%s: %d/%d\n", fails ? "FAILED" : "PASSED", checks - fails, checks);
    return fails != 0;
}
