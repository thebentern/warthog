/* tcpip.h declares tcpip_inpkt only with an OS; the test defines it as the mailbox. */
#pragma once
#include "lwip/netif.h"
err_t tcpip_inpkt(struct pbuf *p, struct netif *inp, netif_input_fn input_fn);
