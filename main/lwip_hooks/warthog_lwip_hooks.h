#pragma once

/* lwIP's own sources include this (ESP_IDF_LWIP_HOOK_FILENAME, main/CMakeLists.txt). */
struct pbuf;
struct netif;

/* main/nat_frag.c: before NAPT reads them, reassembles IPv4 fragments to the Warthog or through NAPT and
 * drops whole ones too short for the TCP, UDP or ICMP header NAPT reads. */
int warthog_ip4_input_hook(struct pbuf *p, struct netif *inp);
#define LWIP_HOOK_IP4_INPUT(p, inp) warthog_ip4_input_hook((p), (inp))
