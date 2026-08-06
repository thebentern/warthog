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
CFLAGS  := -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-macro-redefined \
           -Wno-sign-compare -Wno-unused-function -Wno-visibility -g -O0
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

TESTS   := test_s1g_ieee80211 test_ie_codec test_mesh_plink test_mpm_frame

.PHONY: all clean linkcheck
all: $(TESTS) linkcheck
	@echo "=== running host tests ==="
	@fail=0; for t in $(TESTS); do echo "--- $$t ---"; ./$$t || fail=1; done; \
	  if [ $$fail -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "TESTS FAILED"; exit 1; fi

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
