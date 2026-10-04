# SPDX-License-Identifier: GPL-2.0-or-later
# IP fragments through IDF's own lwIP (test_lwip_napt_frag.c), included by Makefile.
#
# Builds esp-lwip's IPv4 core straight out of the PlatformIO ESP-IDF package the firmware links,
# with main/nat_frag.c, for the host. Without the package (`pio run` fetches it) `make lwip-napt`
# says SKIP, unless LWIP_NAPT_REQUIRED=1: CI runs it that way after its PlatformIO build.

IDF_LWIP ?= $(HOME)/.platformio/packages/framework-espidf/components/lwip
LWIP_SRC := $(IDF_LWIP)/lwip/src
LWIP_NAPT_SRCS = $(addprefix $(LWIP_SRC)/core/,init.c def.c inet_chksum.c ip.c mem.c memp.c netif.c pbuf.c \
                   timeouts.c udp.c tcp.c tcp_in.c tcp_out.c) \
                 $(addprefix $(LWIP_SRC)/core/ipv4/,dhcp.c icmp.c ip4.c ip4_addr.c ip4_frag.c ip4_napt.c)
LWIP_NAPT_TESTS := test_lwip_napt_frag test_lwip_napt_frag_cksum test_lwip_napt_frag_nohook test_lwip_napt_frag_noreass
LWIP_NAPT_DEPS = test_lwip_napt_frag.c $(wildcard lwip_napt/*.h lwip_napt/arch/*.h) \
                 ../../../main/nat_frag.c ../../../main/nat_frag.h ../../../main/lwip_hooks/warthog_lwip_hooks.h
# lwIP's own sources are not this repo's to warn about; the test and nat_frag.c are (-Werror below).
LWIP_NAPT_INCS = -Ilwip_napt -I../../../main -I../../../main/lwip_hooks -I$(LWIP_SRC)/include \
                 -include lwip_napt/tcpip_inpkt.h

.PHONY: lwip-napt
lwip-napt:
	@echo "=== IDF lwIP: IP fragments, reassembly and NAPT (main/nat_frag.c) ==="
	@if [ ! -f "$(LWIP_SRC)/core/ipv4/ip4_napt.c" ]; then \
	  if [ "$(LWIP_NAPT_REQUIRED)" = 1 ]; then echo "FAIL no ESP-IDF lwIP at $(IDF_LWIP)"; exit 1; fi; \
	  echo "SKIP no ESP-IDF lwIP at $(IDF_LWIP) (pio run fetches it; CI runs this after its build)"; exit 0; fi; \
	$(MAKE) --no-print-directory $(LWIP_NAPT_TESTS) && for t in $(LWIP_NAPT_TESTS); do ./$$t || exit 1; done

$(LWIP_NAPT_TESTS): $(LWIP_NAPT_DEPS) $(LWIP_NAPT_SRCS)
test_lwip_napt_frag: LWIP_NAPT_DEFS := -DLWIP_NAPT_HOOK=1
test_lwip_napt_frag test_lwip_napt_frag_cksum: LWIP_NAPT_OWN := ../../../main/nat_frag.c
# The firmware checks no IP header checksum (CONFIG_LWIP_CHECKSUM_CHECK_IP unset); this runs the hook's check.
test_lwip_napt_frag_cksum: LWIP_NAPT_DEFS := -DLWIP_NAPT_HOOK=1 -DCHECKSUM_CHECK_IP=1
test_lwip_napt_frag_noreass: LWIP_NAPT_DEFS := -DLWIP_NAPT_REASS=0
$(LWIP_NAPT_TESTS):
	@d=$$(mktemp -d); \
	for f in test_lwip_napt_frag.c $(LWIP_NAPT_OWN); do \
	  $(CC) -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -g -O0 $(SANFLAGS) $(LWIP_NAPT_DEFS) $(LWIP_NAPT_INCS) \
	    -c $$f -o $$d/$$(basename $$f .c).o || { rm -rf $$d; exit 1; }; done; \
	for f in $(LWIP_NAPT_SRCS); do \
	  $(CC) -std=gnu11 -w -g -O0 $(SANFLAGS) $(LWIP_NAPT_DEFS) $(LWIP_NAPT_INCS) -c $$f \
	    -o $$d/lwip_$$(basename $$f .c).o || { rm -rf $$d; exit 1; }; done; \
	$(CC) $(SANFLAGS) $$d/*.o -o $@; rc=$$?; rm -rf $$d; exit $$rc
