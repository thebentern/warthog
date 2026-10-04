#include "nat_frag.h"
#include "lwip_hooks/warthog_lwip_hooks.h"

#include "lwip/opt.h"
#include "lwip/def.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip4.h"
#include "lwip/ip4_frag.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ip.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/tcp.h"
#include "lwip/prot/udp.h"
#include "lwip/tcpip.h"

#if !IP_NAPT || !IP_REASSEMBLY
#error "nat_frag.c needs IP_NAPT and IP_REASSEMBLY (CONFIG_LWIP_IP4_REASSEMBLY in sdkconfig.defaults)"
#endif
#if LWIP_TCPIP_CORE_LOCKING_INPUT
#error "tcpip_inpkt would run ip4_input inside the hook"
#endif

/* Written on the tcpip thread only; AT+MTU? reads them. */
static volatile uint32_t s_reass, s_drop, s_short;

enum to { TO_LWIP, TO_GROUP, TO_OURS };

/* ip4_input's whole test for calling ip_napt_recv: inp up or down, its address 0.0.0.0 or another's broadcast */
static int napt_recv_(const ip4_addr_t *dst, const struct netif *inp)
{
    return !inp->napt && ip4_addr_eq(dst, netif_ip4_addr(inp));
}

/* TO_OURS: to one of the Warthog's addresses (ip_napt_recv reads those that came in on a netif without
 * NAPT), or in on a NAPT netif and routed out one without (ip_napt_forward). */
static enum to to_(const struct ip_hdr *h, struct netif *inp)
{
    ip4_addr_t src, dst;
    ip4_addr_copy(src, h->src);
    ip4_addr_copy(dst, h->dest);
    if (napt_recv_(&dst, inp) && !ip4_addr_isany_val(dst)) {
        return TO_OURS; /* a fragment to 0.0.0.0 stays a group's */
    }
    if (ip4_addr_ismulticast(&dst) || ip4_addr_isbroadcast(&dst, inp)) {
        return TO_GROUP;
    }
    struct netif *n;
    NETIF_FOREACH(n) { /* ip4_input_accept's test, on every netif */
        if (netif_is_up(n) && !ip4_addr_isany_val(*netif_ip4_addr(n))) {
            if (ip4_addr_eq(&dst, netif_ip4_addr(n))) {
                return TO_OURS;
            }
            if (ip4_addr_isbroadcast(&dst, n)) {
                return TO_GROUP;
            }
        }
    }
    if (inp->napt) {
        struct netif *out = ip4_route_src(&src, &dst);
        if (out != NULL && out != inp && !out->napt) {
            return TO_OURS;
        }
    }
    return TO_LWIP; /* forwarded as it came */
}

/* ip_napt_recv and ip_napt_forward read (and on a match rewrite) the transport header in the first pbuf
 * unchecked: a TCP, UDP or ICMP packet of ours whose first pbuf or IP length ends inside it is dropped. */
static int cut_short_(const struct pbuf *p, struct netif *inp)
{
    const struct ip_hdr *h = (const struct ip_hdr *)p->payload;
    const u8_t proto = IPH_PROTO(h);
    const unsigned need = proto == IP_PROTO_TCP ? TCP_HLEN : proto == IP_PROTO_UDP ? UDP_HLEN : proto == IP_PROTO_ICMP ? 8u : 0u;
    const unsigned hlen = IPH_HL_BYTES(h), len = lwip_ntohs(IPH_LEN(h));
    const unsigned have = len < p->len ? len : p->len;
    if (need == 0 || have >= hlen + need) {
        return 0;
    }
    if (hlen < IP_HLEN || hlen > have || len > p->tot_len) {
        return 0; /* ip4_input drops these before NAPT */
    }
    if (proto == IP_PROTO_UDP && have < hlen + 4) {
        return 1; /* ip4_input reads the destination port of a UDP packet no netif takes (DHCP) */
    }
    ip4_addr_t dst;
    ip4_addr_copy(dst, h->dest);
    return napt_recv_(&dst, inp) || to_(h, inp) == TO_OURS;
}

static int drop_(struct pbuf *p)
{
    pbuf_free(p);
    s_drop++;
    return 1;
}

/* tcpip thread, first thing in ip4_input. Nonzero: the hook took p. */
int warthog_ip4_input_hook(struct pbuf *p, struct netif *inp)
{
    const struct ip_hdr *h = (const struct ip_hdr *)p->payload;
    if (p->len < IP_HLEN) {
        return 0;
    }
    if ((IPH_OFFSET(h) & PP_HTONS(IP_OFFMASK | IP_MF)) == 0) {
        if (!cut_short_(p, inp)) {
            return 0;
        }
        pbuf_free(p);
        s_short++;
        return 1;
    }
    /* Fragments to a group address are dropped: nothing here takes one, and lwIP's reassembly timeout would
     * answer them with ICMP. ip_napt_recv and ip_napt_forward read ports from any fragment. */
    const enum to to = to_(h, inp);
    if (to != TO_OURS) {
        return to == TO_GROUP ? drop_(p) : 0;
    }
    /* ip4_input's own header checks, which run after this hook */
    const u16_t hlen = IPH_HL_BYTES(h);
    const u16_t len = lwip_ntohs(IPH_LEN(h));
    if (hlen < IP_HLEN || hlen > p->len || len < hlen || len > p->tot_len) {
        return drop_(p);
    }
#if CHECKSUM_CHECK_IP
    if (inet_chksum(p->payload, hlen) != 0) {
        return drop_(p);
    }
#endif
    if (len < p->tot_len) {
        pbuf_realloc(p, len);
    }
    /* Held as a heap copy, so the driver's buffer (radio RX block, batman delivery) goes back now;
     * the link-header room lets an echo reply reuse the copies instead of one block its full size. */
    struct pbuf *q = pbuf_clone(PBUF_LINK, PBUF_RAM, p);
    pbuf_free(p);
    if (q == NULL) {
        s_drop++;
        return 1;
    }
    p = ip4_reass(q);
    if (p == NULL) {
        return 1; /* held for the rest, or dropped by lwIP */
    }
    /* NAPT reads the ports and TCP flags in the first pbuf; a first fragment without them is RFC 1858's attack */
    h = (const struct ip_hdr *)p->payload;
    if (p->len < IPH_HL_BYTES(h) + (IPH_PROTO(h) == IP_PROTO_TCP ? 20u : 8u)) {
        return drop_(p);
    }
    /* Back through the mailbox, not ip4_input from here: no deeper tcpip stack. */
    if (tcpip_inpkt(p, inp, ip4_input) != ERR_OK) {
        return drop_(p);
    }
    s_reass++;
    return 1;
}

void warthog_nat_frag_counts(uint32_t *reass, uint32_t *drop, uint32_t *cut_short)
{
    *reass = s_reass;
    *drop = s_drop;
    *cut_short = s_short;
}
