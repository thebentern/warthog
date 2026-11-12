/* IDF's lwIP (esp-lwip) on the host for test_lwip_bridge: lwip_napt/lwipopts.h's settings with ARP, Ethernet
 * and lwIP's bridge in place of TCP, NAPT and reassembly; no OS (the test is the tcpip thread and its mailbox). */
#pragma once
#include <stddef.h>

#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define ESP_LWIP 1
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_TCP 0
#define LWIP_UDP 1
#define LWIP_ICMP 1
#define LWIP_RAW 0
#define LWIP_IGMP 0
#define LWIP_DHCP 1
#define LWIP_DHCP_DOES_ACD_CHECK 0
#define LWIP_DNS 0
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define LWIP_HAVE_LOOPIF 0
#define LWIP_NETIF_LOOPBACK 0
#define LWIP_STATS 0
#define LWIP_TIMERS 1

/* CONFIG_ESP_NETIF_BRIDGE_EN with CONFIG_LWIP_NUM_NETIF_CLIENT_DATA=1 (lwipopts.h in IDF's lwIP component):
 * one slot for esp_netif, one for the bridge. */
#define LWIP_NUM_NETIF_CLIENT_DATA 2
/* The firmware has an OS, so a port's frames are forwarded on the tcpip thread (bridgeif_opts.h's default is
 * NO_SYS, which is 1 here). */
#define BRIDGEIF_PORT_NETIFS_OUTPUT_DIRECT 0

/* CONFIG_LWIP_IP_FORWARD; no NAPT, fragments or reassembly in this build. */
#define IP_FORWARD 1
#define IP_NAPT 0
#define IP_FRAG 0
#define IP_REASSEMBLY 0
/* lwipopts.h in IDF's lwIP component */
#define IP_DEFAULT_TTL 64
#define ARP_QUEUEING 1
#define LWIP_NETIF_HOSTNAME 1
#define LWIP_DHCP_DISCOVER_ADD_HOSTNAME 1
#define LWIP_NETIF_TX_SINGLE_PBUF 1
#define LWIP_SUPPORT_CUSTOM_PBUF 1
/* CONFIG_LWIP_CHECKSUM_CHECK_IP and _UDP unset, _ICMP set */
#define CHECKSUM_CHECK_IP 0
#define CHECKSUM_CHECK_UDP 0
#define CHECKSUM_CHECK_ICMP 1
/* lwipopts.h routes by source first (port/hooks/lwip_default_hooks.c); the test defines it alike. */
#define LWIP_HOOK_IP4_ROUTE_SRC ip4_route_src_hook

/* Every lwIP allocation counted, so a flooded frame that is never freed shows. */
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
