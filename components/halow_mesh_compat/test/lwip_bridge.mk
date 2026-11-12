# SPDX-License-Identifier: GPL-2.0-or-later
# lwIP's bridge as IDF builds it and AT+MESHBRIDGE=1 uses it (test_lwip_bridge.c), included by Makefile
# after lwip_napt.mk, whose package lookup (IDF_LWIP, LWIP_SRC) it shares.
#
# Builds esp-lwip's IPv4 core, ARP, the DHCP client and netif/bridgeif*.c straight out of the PlatformIO
# ESP-IDF package the firmware links, for the host. Without the package (`pio run` fetches it)
# `make lwip-bridge` says SKIP, unless LWIP_BRIDGE_REQUIRED=1.
LWIP_BRIDGE_SRCS = $(addprefix $(LWIP_SRC)/core/,init.c def.c inet_chksum.c ip.c mem.c memp.c netif.c pbuf.c \
                     timeouts.c udp.c) \
                   $(addprefix $(LWIP_SRC)/core/ipv4/,dhcp.c etharp.c icmp.c ip4.c ip4_addr.c) \
                   $(addprefix $(LWIP_SRC)/netif/,ethernet.c bridgeif.c bridgeif_fdb.c)
LWIP_BRIDGE_TESTS := test_lwip_bridge
LWIP_BRIDGE_DEPS = test_lwip_bridge.c $(wildcard lwip_bridge/*.h lwip_napt/arch/*.h)
# lwip_bridge/ first: its lwipopts.h, hooks.h and tcpip_inpkt.h; arch/cc.h is lwip_napt's.
LWIP_BRIDGE_INCS = -Ilwip_bridge -Ilwip_napt -I$(LWIP_SRC)/include -include lwip_bridge/tcpip_inpkt.h

.PHONY: lwip-bridge
lwip-bridge:
	@echo "=== IDF lwIP: the bridge (netif/bridgeif.c) as AT+MESHBRIDGE=1 builds it ==="
	@if [ ! -f "$(LWIP_SRC)/netif/bridgeif.c" ]; then \
	  if [ "$(LWIP_BRIDGE_REQUIRED)" = 1 ]; then echo "FAIL no ESP-IDF lwIP at $(IDF_LWIP)"; exit 1; fi; \
	  echo "SKIP no ESP-IDF lwIP at $(IDF_LWIP) (pio run fetches it; CI runs this after its build)"; exit 0; fi; \
	$(MAKE) --no-print-directory $(LWIP_BRIDGE_TESTS) && for t in $(LWIP_BRIDGE_TESTS); do ./$$t || exit 1; done

# lwIP's own sources are not this repo's to warn about; the test is (-Werror).
$(LWIP_BRIDGE_TESTS): $(LWIP_BRIDGE_DEPS) $(LWIP_BRIDGE_SRCS)
	@d=$$(mktemp -d); \
	$(CC) -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -g -O0 $(SANFLAGS) $(LWIP_BRIDGE_INCS) \
	  -c test_lwip_bridge.c -o $$d/test_lwip_bridge.o || { rm -rf $$d; exit 1; }; \
	for f in $(LWIP_BRIDGE_SRCS); do \
	  $(CC) -std=gnu11 -w -g -O0 $(SANFLAGS) $(LWIP_BRIDGE_INCS) -c $$f \
	    -o $$d/lwip_$$(basename $$f .c).o || { rm -rf $$d; exit 1; }; done; \
	$(CC) $(SANFLAGS) $$d/*.o -o $@; rc=$$?; rm -rf $$d; exit $$rc
