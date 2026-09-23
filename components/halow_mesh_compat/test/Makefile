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
           test_mesh_ies_golden test_mesh_mpm_parse test_mesh_data_hdr test_mesh_beacon_id test_mesh_plink_tbl test_aes_ccm test_ccm_crosscheck test_ccmp_hdr test_s1g_beacon test_mesh_hwmp test_mesh_diag test_mudp_classify test_mesh_ctrl test_mesh_rmc test_mesh_pathtbl test_mesh_hwmp_relay test_mesh_fwd sim_mesh

# ---- simnode: the real stack with only the chip and the RTOS replaced ----
#
# sim_mesh drives the freestanding forwarding ENGINE. simnode links the real
# umac_mesh.c, umac_mesh_fwd_glue.c, umac_datapath*.c, mmpkt.c and
# umac_sta_data.c and cuts at mmdrv_tx_frame(), so what a test asserts on is
# the frame the firmware would have handed to the radio. It covers the glue,
# which is the one mesh file no freestanding test can link.
SIMNODE      := simnode
SIMNODE_TESTS := test_simnode_smoke test_simnode_peering test_simnode_datapath test_simnode_multinode test_simnode_glue test_simnode_keys test_simnode_keys_nochip
# Appended after the fact: TESTS above is an immediate assignment, so a
# reference to this from there would expand to nothing.
TESTS += $(SIMNODE_TESTS)
FW           := ../../halow/components/mm-iot-sdk/framework
HAP          := $(FW)/src/hostap/src
SIMNODE_INCS = -I$(SIMNODE)/hostshim -Ihostcompat -I../../halow/components/shims/include/esp32s3 \
                -I$(SDKMESH)/../.. -I$(FW)/morselib/include -I$(FW)/src -I$(FW)/src/mmutils \
                -I$(FW)/morselib/src/internal -I$(FW)/morselib/mmrc/src/core \
                -I$(FW)/morselib/src/umac/rc/mmrc_osal -I$(HAP) -I$(HAP)/utils \
                -I$(SIMNODE) -I$(SDKMESH)
# The shipping code the simulator puts under test.
SIMNODE_REAL = $(addprefix $(SDKMESH)/,umac_mesh.c umac_mesh_fwd_glue.c umac_mesh_fwd.c \
                  umac_mesh_ctrl.c umac_mesh_rmc.c umac_mesh_pathtbl.c umac_mesh_hwmp.c \
                  umac_mesh_hwmp_relay.c umac_mesh_ies.c umac_mesh_plink_tbl.c \
                  umac_mesh_ccmp_hdr.c umac_mesh_beacon.c umac_mesh_rx_crypto.c umac_mesh_ccm.c) \
                $(addprefix $(FW)/morselib/src/umac/datapath/,umac_datapath.c umac_datapath_mesh.c datapath_defrag.c) \
                $(addprefix $(FW)/morselib/src/common/,mmpkt.c mmpkt_list.c mac_address.c) \
                $(addprefix $(FW)/morselib/src/umac/data/,umac_sta_data.c umac_data.c) \
                $(addprefix $(FW)/morselib/src/umac/keys/,umac_keys.c connection_keys.c) \
                $(addprefix $(FW)/morselib/src/umac/frames/,frame_constructor.c action.c probe_request.c \
                  probe_response.c utils.c association.c authentication.c deauthentication.c disassociation.c) \
                $(addprefix $(FW)/morselib/src/umac/ies/,ies_common.c s1g_capabilities.c) \
                $(FW)/morselib/src/umac/supplicant_shim/ccmp.c $(FW)/morselib/src/umac/stats/umac_stats.c \
                $(FW)/morselib/src/dot11/dot11_utils.c \
                $(FW)/morselib/src/umac/regdb/umac_regdb.c
# What it replaces: the chip, the RTOS, the radio stack below the mesh, and
# the app-side storage that lives in main/at.c on the firmware.
SIMNODE_FAKE = $(addprefix $(SIMNODE)/,fake_rtos.c fake_chip.c fake_config.c fake_app.c \
                  fake_radio_stack.c warthog_globals.c simnode.c)
SIMNODE_SRCS = $(SIMNODE_REAL) $(SIMNODE_FAKE)
SIMNODE_HDRS = $(wildcard $(SIMNODE)/*.h) $(MESHHDRS)

# The shipping mesh byte-layout code, compiled straight out of the vendored SDK.
# These tests link the REAL firmware source, not a copy of it -- umac_mesh_ies.c
# is deliberately freestanding (libc only) so it can be built here. `make
# freestanding` fails loudly if an SDK include ever creeps back in and quietly
# un-tests the on-air byte layout.
SDKMESH := ../../halow/components/mm-iot-sdk/framework/morselib/src/umac/mesh
MESHIES := $(SDKMESH)/umac_mesh_ies.c

# Header prerequisites. Without these, editing a struct or a constant in a mesh
# header leaves every test binary stale and the suite reports PASS against code
# that is no longer built -- which is exactly what a green run must never mean.
MESHHDRS := $(wildcard $(SDKMESH)/*.h)

.PHONY: all clean linkcheck freestanding region glueguard
all: $(TESTS) linkcheck freestanding region glueguard
	@echo "=== running host tests ==="
	@fail=0; for t in $(TESTS); do echo "--- $$t ---"; ./$$t || fail=1; done; \
	  if [ $$fail -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "TESTS FAILED"; exit 1; fi

# The build's regulatory domain must reach the radio, not just the log. Shell
# rather than C because what needs checking is the PREPROCESSOR result of the
# real header under each region flag, plus the call sites that consume it.
region:
	@echo "=== regulatory region mapping ==="
	@sh ./test_region_map.sh

# Locking and call ordering in umac_mesh_fwd_glue.c, checked structurally: the
# simnode tests link and drive the glue (SIMNODE_REAL) but cannot observe either.
glueguard:
	@echo "=== forwarding glue invariants ==="
	@sh ./test_glue_guard.sh

# Note: -I$(SDKMESH) ONLY, deliberately not $(INCS) -- if these tests could see
# the compat/dot11ah headers, an SDK dependency could sneak into umac_mesh_ies.c
# without anything failing.
# The peering watchdog's cause-selection, compiled straight out of main/ --
# the shipped decision, not a copy of it.
MAINDIR := ../../../main
test_mesh_diag: test_mesh_diag.c $(MAINDIR)/mesh_diag.c
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(MAINDIR) $^ -o $@

test_mudp_classify: test_mudp_classify.c $(MAINDIR)/mudp_classify.c $(MAINDIR)/mudp_classify.h
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(MAINDIR) $(filter %.c,$^) -o $@

# The simulator links every forwarding TU the firmware ships.
sim_mesh: sim_mesh.c $(SDKMESH)/umac_mesh_fwd.c $(SDKMESH)/umac_mesh_hwmp_relay.c $(SDKMESH)/umac_mesh_ctrl.c $(SDKMESH)/umac_mesh_rmc.c $(SDKMESH)/umac_mesh_pathtbl.c $(SDKMESH)/umac_mesh_hwmp.c $(MESHIES) $(MESHHDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SDKMESH) $(filter %.c,$^) -o $@

# The generated files are checked in (so a plain `make` needs no python), but
# regenerate them whenever their source of truth moves.
$(SIMNODE)/warthog_globals.c: ../../../main/at.c $(SIMNODE)/gen_warthog_globals.py
	@cd $(SIMNODE) && python3 gen_warthog_globals.py ../../../../main/at.c

# Used by simnode/regen_stubs.sh, so the stub set is always computed against
# the same source list this file compiles.
.PHONY: simnode-srcs simnode-incs
simnode-srcs:
	@for f in $(SIMNODE_SRCS); do echo $$f; done
simnode-incs:
	@echo $(SIMNODE_INCS)

test_simnode_smoke: $(SIMNODE)/test_simnode_smoke.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(filter %.c,$^) -o $@

test_simnode_peering: $(SIMNODE)/test_simnode_peering.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(filter %.c,$^) -o $@

# ---- several nodes in one process ---------------------------------------
#
# The SDK keeps its state in file-scope statics, so a second simnode_start()
# in the same image would share one path table, one HWMP sequence number, one
# peer list and one clock with the first. The whole simulator is therefore
# built as ONE shared object and loaded N times from N DISTINCT FILES: the
# loader gives each file its own copy of .data/.bss, so every node gets
# private statics while libc stays shared. RTLD_LOCAL plus macOS's two-level
# namespace (-Bsymbolic on ELF) is what keeps node B's calls bound to node B's
# own code rather than to whichever copy loaded first.
SIMNODE_NODES := 3
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
SIMNODE_SO      := .dylib
SIMNODE_SOFLAGS := -dynamiclib -fPIC
SIMNODE_DLLIBS  :=
else
SIMNODE_SO      := .so
SIMNODE_SOFLAGS := -shared -fPIC -Wl,-Bsymbolic
SIMNODE_DLLIBS  := -ldl
endif

test_simnode_multinode: $(SIMNODE)/test_simnode_multinode.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -Wno-unused-function -Wno-void-pointer-to-int-cast \
	  $(SIMNODE_SOFLAGS) $(SIMNODE_INCS) $(SIMNODE_SRCS) -o libsimnode$(SIMNODE_SO)
	@i=0; while [ $$i -lt $(SIMNODE_NODES) ]; do \
	  cp libsimnode$(SIMNODE_SO) libsimnode$$i$(SIMNODE_SO); i=$$((i+1)); done
	$(CC) $(CFLAGS) $(SANFLAGS) -DSIMNODE_SO_SUFFIX='"$(SIMNODE_SO)"' \
	  -DSIMNODE_NODES=$(SIMNODE_NODES) $(SIMNODE_INCS) $< -o $@ $(SIMNODE_DLLIBS)

test_simnode_datapath: $(SIMNODE)/test_simnode_datapath.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(filter %.c,$^) -o $@

# The forwarding glue, end to end: held frames, the discovery retry, the
# bounded pending queue, both rate gates, the queue cap and the peer-loss PERR
# burst -- driven through the shipping umac_mesh_fwd_glue.c.
test_simnode_glue: $(SIMNODE)/test_simnode_glue.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(filter %.c,$^) -o $@

# The SAE key lifecycle: our own MGTK, which key a broadcast uses, and what a
# surviving link keeps when a peer goes -- read off the fake chip's INSTALL_KEY log.
test_simnode_keys: $(SIMNODE)/test_simnode_keys.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(filter %.c,$^) -o $@

# The same file on the warthog-mesh-sae-nochipkey/-swccmp key path: AMPE keys stay host-only.
test_simnode_keys_nochip: $(SIMNODE)/test_simnode_keys.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -DWARTHOG_MESH_AMPE_NO_CHIP_KEY=1 -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(filter %.c,$^) -o $@

test_mesh_fwd: test_mesh_fwd.c $(SDKMESH)/umac_mesh_fwd.c $(SDKMESH)/umac_mesh_ctrl.c $(SDKMESH)/umac_mesh_rmc.c $(SDKMESH)/umac_mesh_pathtbl.c $(SDKMESH)/umac_mesh_hwmp.c $(MESHIES) $(MESHHDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SDKMESH) $(filter %.c,$^) -o $@

test_mesh_hwmp_relay: test_mesh_hwmp_relay.c $(SDKMESH)/umac_mesh_hwmp_relay.c $(SDKMESH)/umac_mesh_pathtbl.c $(SDKMESH)/umac_mesh_hwmp.c $(MESHHDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SDKMESH) $(filter %.c,$^) -o $@

test_mesh_pathtbl: test_mesh_pathtbl.c $(SDKMESH)/umac_mesh_pathtbl.c $(SDKMESH)/umac_mesh_hwmp.c $(MESHHDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SDKMESH) $(filter %.c,$^) -o $@

test_mesh_rmc: test_mesh_rmc.c $(SDKMESH)/umac_mesh_rmc.c $(MESHHDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SDKMESH) $(filter %.c,$^) -o $@

test_mesh_ctrl: test_mesh_ctrl.c $(SDKMESH)/umac_mesh_ctrl.c $(MESHHDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SDKMESH) $(filter %.c,$^) -o $@

test_mesh_hwmp: test_mesh_hwmp.c $(SDKMESH)/umac_mesh_hwmp.c $(MESHHDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SDKMESH) $(filter %.c,$^) -o $@

test_mesh_ies_golden: test_mesh_ies_golden.c $(MESHIES)
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SDKMESH) $(filter %.c,$^) -o $@

test_mesh_mpm_parse: test_mesh_mpm_parse.c $(MESHIES)
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SDKMESH) $(filter %.c,$^) -o $@

test_mesh_beacon_id: test_mesh_beacon_id.c $(MESHIES)
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SDKMESH) $(filter %.c,$^) -o $@

test_mesh_plink_tbl: test_mesh_plink_tbl.c $(SDKMESH)/umac_mesh_plink_tbl.c
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SDKMESH) $(filter %.c,$^) -o $@

test_ccmp_hdr: test_ccmp_hdr.c $(SDKMESH)/umac_mesh_ccmp_hdr.c
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SDKMESH) $(filter %.c,$^) -o $@

test_s1g_beacon: test_s1g_beacon.c $(MESHIES)
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SDKMESH) $(filter %.c,$^) -o $@

# AES-CCM against the RFC 3610 vectors, using hostap's own software AES so the
# cipher is tested independently of the on-target mbedtls shim.
HOSTAP := ../../halow/components/mm-iot-sdk/framework/src/hostap
CCMSRC := $(HOSTAP)/src/crypto/aes-ccm.c $(HOSTAP)/src/crypto/aes-internal.c \
          $(HOSTAP)/src/crypto/aes-internal-enc.c
test_aes_ccm: test_aes_ccm.c $(CCMSRC)
	$(CC) $(CFLAGS) $(SANFLAGS) -Wno-unused-parameter -Wno-sign-compare \
	  -Ihostcompat -I$(HOSTAP)/src/utils -I$(HOSTAP)/src/crypto -I$(HOSTAP)/src -I$(HOSTAP) $^ -o $@

test_ccm_crosscheck: test_ccm_crosscheck.c $(SDKMESH)/umac_mesh_ccm.c $(CCMSRC)
	$(CC) $(CFLAGS) $(SANFLAGS) -Wno-unused-parameter -Wno-sign-compare \
	  -Ihostcompat -I$(SDKMESH) -I$(HOSTAP)/src/utils -I$(HOSTAP)/src/crypto -I$(HOSTAP)/src -I$(HOSTAP) $^ -o $@

test_mesh_data_hdr: test_mesh_data_hdr.c $(MESHIES)
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SDKMESH) $(filter %.c,$^) -o $@

# Proves the mesh byte-layout TU still compiles with nothing but libc.
freestanding:
	@echo "=== freestanding check (mesh byte layout must not depend on the SDK) ==="
	@$(CC) $(CFLAGS) -Werror -fsyntax-only -I$(SDKMESH) $(MESHIES) && echo "ok   umac_mesh_ies.c is freestanding"
	@$(CC) $(CFLAGS) -Werror -fsyntax-only -I$(SDKMESH) $(SDKMESH)/umac_mesh_plink_tbl.c && echo "ok   umac_mesh_plink_tbl.c is freestanding"
	@$(CC) $(CFLAGS) -Werror -fsyntax-only -I$(SDKMESH) $(SDKMESH)/umac_mesh_ccmp_hdr.c && echo "ok   umac_mesh_ccmp_hdr.c is freestanding"
	@$(CC) $(CFLAGS) -Werror -fsyntax-only -I$(SDKMESH) $(SDKMESH)/umac_mesh_hwmp.c && echo "ok   umac_mesh_hwmp.c is freestanding"
	@$(CC) $(CFLAGS) -Werror -fsyntax-only -I../../../main ../../../main/mesh_diag.c && echo "ok   mesh_diag.c is freestanding"
	@$(CC) $(CFLAGS) -Werror -fsyntax-only -I../../../main ../../../main/mudp_classify.c && echo "ok   mudp_classify.c is freestanding"
	@$(CC) $(CFLAGS) -Werror -fsyntax-only -I$(SDKMESH) $(SDKMESH)/umac_mesh_ctrl.c && echo "ok   umac_mesh_ctrl.c is freestanding"
	@$(CC) $(CFLAGS) -Werror -fsyntax-only -I$(SDKMESH) $(SDKMESH)/umac_mesh_rmc.c && echo "ok   umac_mesh_rmc.c is freestanding"
	@$(CC) $(CFLAGS) -Werror -fsyntax-only -I$(SDKMESH) $(SDKMESH)/umac_mesh_pathtbl.c && echo "ok   umac_mesh_pathtbl.c is freestanding"
	@$(CC) $(CFLAGS) -Werror -fsyntax-only -I$(SDKMESH) $(SDKMESH)/umac_mesh_hwmp_relay.c && echo "ok   umac_mesh_hwmp_relay.c is freestanding"
	@$(CC) $(CFLAGS) -Werror -fsyntax-only -I$(SDKMESH) $(SDKMESH)/umac_mesh_fwd.c && echo "ok   umac_mesh_fwd.c is freestanding"

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
	rm -f $(TESTS) linkcheck_all libsimnode*.dylib libsimnode*.so
	rm -rf *.dSYM
