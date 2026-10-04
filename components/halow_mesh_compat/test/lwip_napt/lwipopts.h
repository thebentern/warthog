/* IDF's lwIP (esp-lwip) on the host, set as the firmware's sdkconfig and lwipopts.h set it for
 * NAPT and IPv4 reassembly; no OS (the test is the tcpip thread and its mailbox). */
#pragma once
#include <stddef.h>

#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define ESP_LWIP 1
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ARP 0
#define LWIP_ETHERNET 0
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_ICMP 1
#define LWIP_RAW 0
#define LWIP_IGMP 0
/* CONFIG_LWIP_DHCP: ip4_input then reads a UDP packet's destination port when no netif takes it. */
#define LWIP_DHCP 1
#define LWIP_DHCP_DOES_ACD_CHECK 0
#define LWIP_DNS 0
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define LWIP_HAVE_LOOPIF 0
#define LWIP_NETIF_LOOPBACK 0
#define LWIP_STATS 0
#define LWIP_TIMERS 1
#define TCP_MSS 1436

/* CONFIG_LWIP_IP_FORWARD, _IPV4_NAPT, _IPV4_NAPT_PORTMAP, _IP4_FRAG, _IP4_REASSEMBLY, _IP_REASS_MAX_PBUFS */
#define IP_FORWARD 1
#define IP_NAPT 1
#define IP_NAPT_PORTMAP 1
#define IP_FRAG 1
#ifndef LWIP_NAPT_REASS
#define LWIP_NAPT_REASS 1
#endif
#define IP_REASSEMBLY LWIP_NAPT_REASS
#define IP_REASS_MAX_PBUFS 10
/* lwipopts.h in IDF's lwIP component */
#define IP_REASS_MAXAGE 3
#define IP_DEFAULT_TTL 64
#define LWIP_NETIF_TX_SINGLE_PBUF 1
#define LWIP_SUPPORT_CUSTOM_PBUF 1
/* CONFIG_LWIP_CHECKSUM_CHECK_IP and _UDP unset, _ICMP set; lwip_napt.mk builds one variant with IP on. */
#ifndef CHECKSUM_CHECK_IP
#define CHECKSUM_CHECK_IP 0
#endif
#define CHECKSUM_CHECK_UDP 0
#define CHECKSUM_CHECK_ICMP 1
/* lwipopts.h routes by source first (port/hooks/lwip_default_hooks.c); the test defines it alike. */
#define LWIP_HOOK_IP4_ROUTE_SRC ip4_route_src_hook

/* Every lwIP allocation counted, so a held or dropped fragment that is never freed shows. */
#define MEM_LIBC_MALLOC 1
#define MEMP_MEM_MALLOC 1
#define MEM_ALIGNMENT 4
void *lwip_test_malloc(size_t n);
void *lwip_test_calloc(size_t n, size_t m);
void lwip_test_free(void *p);
#define mem_clib_malloc lwip_test_malloc
#define mem_clib_calloc lwip_test_calloc
#define mem_clib_free lwip_test_free

/* lwip_default_hooks.h declares this, and includes ESP_IDF_LWIP_HOOK_FILENAME. */
#define LWIP_HOOK_FILENAME "hooks.h"
