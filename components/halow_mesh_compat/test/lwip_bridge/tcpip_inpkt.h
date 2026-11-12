/* tcpip.h declares these only with an OS; the test defines them as the mailbox (test_lwip_bridge.c). */
#pragma once
#include "lwip/netif.h"
err_t tcpip_inpkt(struct pbuf *p, struct netif *inp, netif_input_fn input_fn);
err_t tcpip_input(struct pbuf *p, struct netif *inp);
