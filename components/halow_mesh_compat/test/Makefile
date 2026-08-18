# SPDX-License-Identifier: GPL-2.0-or-later
# Host unit-test build for the ported dot11ah / mesh sources.
#
# Runs on the dev host (clang/gcc) — no ESP32 hardware required. Each ported
# translation unit gets a test here; this is how the softmac mesh port is
# validated incrementally before it ever touches the firmware build.
#
#   make            # build + run all unit tests, then the full link check
#   make clean

CC      ?= clang
ifdef SAN
SANFLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all
endif

# -Wno-macro-redefined and -Wno-visibility are clang spellings; gcc warns
# that it does not recognise them, which buries real diagnostics in CI.
CLANG_ONLY := $(shell $(CC) --version 2>/dev/null | grep -qi clang && echo -Wno-macro-redefined -Wno-visibility)
CFLAGS  := -std=gnu11 -Wall -Wextra -Wno-unused-parameter $(CLANG_ONLY) \
           -Wno-sign-compare -Wno-unused-function -g -O0
INCS    := -I../src/compat -I../src/dot11ah -I../src
COMPAT  := ../src/compat/compat.c

# Every ported dot11ah translation unit (the full S1G<->11n layer).
DOT11AH := ../src/dot11ah/s1g_ieee80211.c \
           ../src/dot11ah/ie.c \
           ../src/dot11ah/s1g_channels.c \
           ../src/dot11ah/s1g_channels_rules.c \
           ../src/dot11ah/tim.c \
           ../src/dot11ah/reg.c \
           ../src/dot11ah/reg_rules.c \
           ../src/dot11ah/tx_11n_to_s1g.c \
           ../src/dot11ah/rx_s1g_to_11n.c \
           ../src/dot11ah/main.c \
           ../src/dot11ah/debug.c

TESTS   := test_s1g_ieee80211 test_ie_codec test_mesh_plink test_mpm_frame \
           test_mesh_ies_golden test_mesh_mpm_parse test_mesh_data_hdr test_mesh_beacon_id test_mesh_plink_tbl test_aes_ccm test_ccmp_hdr test_s1g_beacon test_mesh_hwmp

# The shipping mesh byte-layout code, compiled straight out of the vendored SDK.
# These tests link the REAL firmware source, not a copy of it -- umac_mesh_ies.c
# is deliberately freestanding (libc only) so it can be built here. `make
# freestanding` fails loudly if an SDK include ever creeps back in and quietly
# un-tests the on-air byte layout.
SDKMESH := ../../halow/components/mm-iot-sdk/framework/morselib/src/umac/mesh
MESHIES := $(SDKMESH)/umac_mesh_ies.c

.PHONY: all clean linkcheck freestanding region
all: $(TESTS) linkcheck freestanding region
	@echo "=== running host tests ==="
	@fail=0; for t in $(TESTS); do echo "--- $$t ---"; ./$$t || fail=1; done; \
	  if [ $$fail -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "TESTS FAILED"; exit 1; fi

# The build's regulatory domain must reach the radio, not just the log. Shell
# rather than C because what needs checking is the PREPROCESSOR result of the
# real header under each region flag, plus the call sites that consume it.
region:
	@echo "=== regulatory region mapping ==="
	@sh ./test_region_map.sh

# Note: -I$(SDKMESH) ONLY, deliberately not $(INCS) -- if these tests could see
# the compat/dot11ah headers, an SDK dependency could sneak into umac_mesh_ies.c
# without anything failing.
test_mesh_hwmp: test_mesh_hwmp.c $(SDKMESH)/umac_mesh_hwmp.c
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SDKMESH) $^ -o $@

test_mesh_ies_golden: test_mesh_ies_golden.c $(MESHIES)
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SDKMESH) $^ -o $@

test_mesh_mpm_parse: test_mesh_mpm_parse.c $(MESHIES)
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SDKMESH) $^ -o $@

test_mesh_beacon_id: test_mesh_beacon_id.c $(MESHIES)
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SDKMESH) $^ -o $@

test_mesh_plink_tbl: test_mesh_plink_tbl.c $(SDKMESH)/umac_mesh_plink_tbl.c
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SDKMESH) $^ -o $@

test_ccmp_hdr: test_ccmp_hdr.c $(SDKMESH)/umac_mesh_ccmp_hdr.c
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SDKMESH) $^ -o $@

test_s1g_beacon: test_s1g_beacon.c $(MESHIES)
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SDKMESH) $^ -o $@

# AES-CCM against the RFC 3610 vectors, using hostap's own software AES so the
# cipher is tested independently of the on-target mbedtls shim.
HOSTAP := ../../halow/components/mm-iot-sdk/framework/src/hostap
CCMSRC := $(HOSTAP)/src/crypto/aes-ccm.c $(HOSTAP)/src/crypto/aes-internal.c \
          $(HOSTAP)/src/crypto/aes-internal-enc.c
test_aes_ccm: test_aes_ccm.c $(CCMSRC)
	$(CC) $(CFLAGS) $(SANFLAGS) -Wno-unused-parameter -Wno-sign-compare \
	  -Ihostcompat -I$(HOSTAP)/src/utils -I$(HOSTAP)/src/crypto -I$(HOSTAP)/src -I$(HOSTAP) $^ -o $@

test_mesh_data_hdr: test_mesh_data_hdr.c $(MESHIES)
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SDKMESH) $^ -o $@

# Proves the mesh byte-layout TU still compiles with nothing but libc.
freestanding:
	@echo "=== freestanding check (mesh byte layout must not depend on the SDK) ==="
	@$(CC) $(CFLAGS) -Werror -fsyntax-only -I$(SDKMESH) $(MESHIES) && echo "ok   umac_mesh_ies.c is freestanding"
	@$(CC) $(CFLAGS) -Werror -fsyntax-only -I$(SDKMESH) $(SDKMESH)/umac_mesh_plink_tbl.c && echo "ok   umac_mesh_plink_tbl.c is freestanding"
	@$(CC) $(CFLAGS) -Werror -fsyntax-only -I$(SDKMESH) $(SDKMESH)/umac_mesh_ccmp_hdr.c && echo "ok   umac_mesh_ccmp_hdr.c is freestanding"
	@$(CC) $(CFLAGS) -Werror -fsyntax-only -I$(SDKMESH) $(SDKMESH)/umac_mesh_hwmp.c && echo "ok   umac_mesh_hwmp.c is freestanding"

test_s1g_ieee80211: test_s1g_ieee80211.c ../src/dot11ah/s1g_ieee80211.c $(COMPAT)
	$(CC) $(CFLAGS) $(INCS) $^ -o $@

# Functional IE-codec test. ie.c is interdependent with the rest of the layer
# (it calls into the transforms and the S1G channel math), so it links against
# the full dot11ah set — exercising the codec in its real link context.
test_ie_codec: test_ie_codec.c $(DOT11AH) $(COMPAT)
	$(CC) $(CFLAGS) $(INCS) $^ -o $@

# Mesh peering FSM test — fully self-contained (FSM depends only on the
# 802.11 constant headers + libc, not on compat.c or the dot11ah layer).
test_mesh_plink: test_mesh_plink.c ../src/mesh/mesh_plink.c
	$(CC) $(CFLAGS) $(INCS) $^ -o $@

# MPM frame bridge: wire round-trip + a full two-node handshake driven entirely
# through serialized/parsed frames (FSM + ser/deser together).
test_mpm_frame: test_mpm_frame.c ../src/mesh/mesh_mpm_frame.c ../src/mesh/mesh_plink.c
	$(CC) $(CFLAGS) $(INCS) $^ -o $@

# Link check: compile + link ALL dot11ah units together against a trivial main,
# proving the full layer has no duplicate or unresolved cross-unit symbols.
linkcheck: linkcheck_main.c $(DOT11AH) $(COMPAT)
	@echo "=== link check: all 11 dot11ah units + compat ==="
	$(CC) $(CFLAGS) $(INCS) $^ -o linkcheck_all
	@echo "link OK: dot11ah layer is self-contained"

clean:
	rm -f $(TESTS) linkcheck_all
	rm -rf *.dSYM
