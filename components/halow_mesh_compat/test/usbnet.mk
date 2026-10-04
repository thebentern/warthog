# SPDX-License-Identifier: GPL-2.0-or-later
# TinyUSB's own NCM and ECM class drivers with main/usbnet_core.c (test_usbnet.c), included by Makefile.
#
# Builds the drivers out of managed_components/, which `pio run` fetches. Without them `make usbnet`
# says SKIP, unless USBNET_REQUIRED=1: CI runs it that way after its PlatformIO build.

TUSB_DIR ?= ../../../managed_components/espressif__tinyusb
USBNET_TESTS := test_usbnet_ncm test_usbnet_ecm
USBNET_DEPS = test_usbnet.c usbnet_tusb/tusb_config.h ../../../main/usbnet_core.c ../../../main/usbnet_core.h
# TinyUSB's headers are not this repo's to warn about; the test and usbnet_core.c are (-Werror below).
USBNET_INCS = -Iusbnet_tusb -I../../../main -isystem $(TUSB_DIR)/src -isystem $(TUSB_DIR)/lib/networking

.PHONY: usbnet
usbnet:
	@echo "=== TinyUSB NCM/ECM class drivers with main/usbnet_core.c (one owner) ==="
	@if [ ! -f "$(TUSB_DIR)/src/class/net/ncm_device.c" ]; then \
	  if [ "$(USBNET_REQUIRED)" = 1 ]; then echo "FAIL no TinyUSB at $(TUSB_DIR)"; exit 1; fi; \
	  echo "SKIP no TinyUSB at $(TUSB_DIR) (pio run fetches it; CI runs this after its build)"; exit 0; fi; \
	$(MAKE) --no-print-directory $(USBNET_TESTS) && for t in $(USBNET_TESTS); do ./$$t || exit 1; done

test_usbnet_ncm: $(USBNET_DEPS) $(TUSB_DIR)/src/class/net/ncm_device.c
test_usbnet_ecm: $(USBNET_DEPS) $(TUSB_DIR)/src/class/net/ecm_rndis_device.c
test_usbnet_ncm: USBNET_DRV = $(TUSB_DIR)/src/class/net/ncm_device.c
test_usbnet_ecm: USBNET_DRV = $(TUSB_DIR)/src/class/net/ecm_rndis_device.c
test_usbnet_ecm: USBNET_DEFS = -DUSBNET_TEST_ECM=1
$(USBNET_TESTS):
	@d=$$(mktemp -d); \
	$(CC) -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -g -O0 $(SANFLAGS) $(USBNET_DEFS) $(USBNET_INCS) \
	  -c test_usbnet.c -o $$d/test.o && \
	$(CC) -std=gnu11 -Wall -Wextra -Werror -g -O0 $(SANFLAGS) -I../../../main -c ../../../main/usbnet_core.c -o $$d/core.o && \
	$(CC) -std=gnu11 -w -g -O0 $(SANFLAGS) $(USBNET_DEFS) $(USBNET_INCS) -c $(USBNET_DRV) -o $$d/drv.o && \
	$(CC) $(SANFLAGS) $$d/*.o -lpthread -o $@; rc=$$?; rm -rf $$d; exit $$rc
