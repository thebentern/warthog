/* lwip_default_hooks.h as the firmware's lwIP sees it, for test_lwip_napt_frag. */
#pragma once
#include "lwip/ip4_addr.h"
struct netif *ip4_route_src_hook(const ip4_addr_t *src, const ip4_addr_t *dest);
#if LWIP_NAPT_HOOK
#include "warthog_lwip_hooks.h"
#endif
