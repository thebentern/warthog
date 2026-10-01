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
           test_mesh_ies_golden test_mesh_mpm_parse test_mesh_data_hdr test_mesh_beacon_id test_mesh_plink_tbl test_aes_ccm test_ccm_crosscheck test_ccmp_hdr test_s1g_beacon test_mesh_hwmp test_mesh_diag test_mudp_classify test_cdc_out test_mesh_ctrl test_mesh_rmc test_mesh_pathtbl test_mesh_hwmp_relay test_mesh_fwd sim_mesh test_supp_mesh_conf test_mesh_bip test_bat_mode test_beacon_yield test_chipvif_glue test_chipvif_glue_off

# ---- simnode: the real stack with only the chip and the RTOS replaced ----
#
# sim_mesh drives the freestanding forwarding ENGINE. simnode links the real
# umac_mesh.c, umac_mesh_fwd_glue.c, umac_datapath*.c, mmpkt.c and
# umac_sta_data.c and cuts at mmdrv_tx_frame(), so what a test asserts on is
# the frame the firmware would have handed to the radio. It covers the glue,
# which is the one mesh file no freestanding test can link.
SIMNODE      := simnode
SIMNODE_TESTS := test_simnode_smoke test_simnode_peering test_simnode_datapath test_simnode_multinode test_simnode_glue test_simnode_keys test_simnode_keys_nochip test_simnode_keys_pnbase_off test_simnode_keys_nochip_pnbase_off test_simnode_capacity test_simnode_discovery test_simnode_mfp test_simnode_mfp_swccmp test_simnode_batman test_simnode_meshvif test_simnode_meshvif_off test_simnode_keys_meshvif test_simnode_keys_pnbase_off_meshvif test_simnode_keys_nochip_meshvif test_simnode_mfp_swccmp_meshvif test_simnode_batman_meshvif test_simnode_multinode_meshvif test_simnode_peergtk_meshvif test_simnode_peergtk test_simnode_peergtk_swccmp_meshvif test_simnode_mfp_meshvif test_simnode_frag test_simnode_frag_nochip test_simnode_frag_swccmp test_simnode_frag_meshvif test_simnode_frag_swccmp_meshvif
# Appended after the fact: TESTS above is an immediate assignment, so a
# reference to this from there would expand to nothing.
TESTS += $(SIMNODE_TESTS)
# The clean-room BATMAN_V engine (main/bat/); must precede `all:` so its tests join TESTS.
include bat.mk
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
                  umac_mesh_ccmp_hdr.c umac_mesh_beacon.c umac_mesh_rx_crypto.c umac_mesh_ccm.c \
                  umac_mesh_bip.c) \
                $(addprefix $(HAP)/crypto/,aes-internal.c aes-internal-enc.c) \
                $(addprefix $(FW)/morselib/src/umac/datapath/,umac_datapath.c umac_datapath_mesh.c datapath_defrag.c) \
                $(addprefix $(FW)/morselib/src/common/,mmpkt.c mmpkt_list.c mac_address.c) \
                $(FW)/morselib/src/umac/data/umac_data.c \
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
                  fake_radio_stack.c warthog_globals.c simnode.c sta_data_seam.c interface_seam.c)
SIMNODE_SRCS = $(SIMNODE_REAL) $(SIMNODE_FAKE)
# umac_sta_data.c is compiled inside sta_data_seam.c, which adds a hook at its peer-address
# read, and umac_ba.c with it; umac_interface.c inside interface_seam.c. Each is a
# prerequisite, so an edit rebuilds, and SIMNODE_C keeps it off the command line.
SIMNODE_SEAMED = $(FW)/morselib/src/umac/data/umac_sta_data.c $(FW)/morselib/src/umac/ba/umac_ba.c \
                 $(FW)/morselib/src/umac/interface/umac_interface.c
SIMNODE_HDRS = $(wildcard $(SIMNODE)/*.h) $(MESHHDRS) $(SIMNODE_SEAMED) \
               $(addprefix $(FW)/morselib/src/umac/interface/,umac_interface.h umac_interface_data.h) \
               $(wildcard $(FW)/morselib/src/umac/datapath/*.h) $(wildcard $(FW)/morselib/src/umac/keys/*.h) \
               $(FW)/morselib/src/internal/mmdrv.h
SIMNODE_C = $(filter-out $(SIMNODE_SEAMED),$(filter %.c,$^))

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

.PHONY: all clean linkcheck freestanding region glueguard simnode-seamed
all: $(TESTS) linkcheck freestanding region glueguard bat-freestanding bat-nostatic bat-registered simnode-seamed
	@echo "=== running host tests ==="
	@fail=0; for t in $(TESTS); do echo "--- $$t ---"; ./$$t || fail=1; done; \
	  if [ $$fail -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "TESTS FAILED"; exit 1; fi

# The build's regulatory domain must reach the radio, not just the log. Shell
# rather than C because what needs checking is the PREPROCESSOR result of the
# real header under each region flag, plus the call sites that consume it.
region:
	@echo "=== regulatory region mapping ==="
	@sh ./test_region_map.sh

# A shipping .c that a simnode source #includes must be in SIMNODE_SEAMED: nothing else
# makes it a prerequisite, so an edit to it would leave every simnode binary stale.
simnode-seamed:
	@echo "=== every .c a simnode source includes is a prerequisite ==="
	@rc=0; for f in $$(sed -n 's/^#include "\(.*\.c\)".*/\1/p' $(SIMNODE)/*.c); do \
	  case " $(SIMNODE_SEAMED) " in *"/$$f "*) echo "ok   $$f is in SIMNODE_SEAMED";; \
	  *) echo "FAIL $(SIMNODE) includes $$f, which is not in SIMNODE_SEAMED: editing it rebuilds nothing"; rc=1;; esac; \
	done; exit $$rc

# Locking and call ordering in umac_mesh_fwd_glue.c, checked structurally: the
# simnode tests link and drive the glue (SIMNODE_REAL) but cannot observe either.
# It also runs firmware glue no test links (main/bat_port.c, mesh.c, at.c, cfg.c, nat.c) on
# stubs, bat_port.c and at.c's renders also on the real engine, with this build's sanitizers.
glueguard:
	@echo "=== forwarding glue invariants ==="
	@CC="$(CC)" SANFLAGS="$(SANFLAGS)" SIMNODE_INCS="$(SIMNODE_INCS)" sh ./test_glue_guard.sh

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

# BATMAN_V member-mode decisions: when batman may run, what the RX hook keeps, the
# port's engine config and link throughput, and how bat0 gets its address.
test_bat_mode: test_bat_mode.c $(MAINDIR)/bat_mode.c $(MAINDIR)/bat_mode.h $(MAINDIR)/bat/bat.h
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(MAINDIR) -I$(MAINDIR)/bat $(filter %.c,$^) -o $@

# When the host beacon timer yields to a chip that schedules its own TBTT (a MESH chip VIF).
BCNDIR := ../../halow/components/mm-iot-sdk/framework/morselib/src/driver/beacon
test_beacon_yield: test_beacon_yield.c $(BCNDIR)/beacon.h
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(BCNDIR) $(filter %.c,$^) -o $@

# The chip VIF pieces no simnode links: driver.c's reads of the chip's own status (extracted
# by name into test_chipvif_glue.fns) against a fake morse_cmd_tx, beacon.c's IRQ count and
# host-tick yield, and umac_ps.c's CONFIG_PS gate; with the MESH-VIF flag and without.
DRVSRC := $(FW)/morselib/src/driver/driver.c
CHIPVIF_FNS := mmdrv_add_if mmdrv_add_if_status mmdrv_set_bssid mmdrv_cfg_bss_beacon \
               mmdrv_mesh_config mmdrv_get_capabilities_status mmdrv_update_sta_state \
               mmdrv_update_sta_state_status mmdrv_install_key mmdrv_install_key_status
CHIPVIF_DEPS := test_chipvif_glue.c test_chipvif_glue.fns $(BCNDIR)/beacon.c $(BCNDIR)/beacon.h \
                $(FW)/morselib/src/umac/ps/umac_ps.c $(SIMNODE_HDRS)
test_chipvif_glue.fns: $(DRVSRC) Makefile
	@for f in $(CHIPVIF_FNS); do \
	  awk -v f="$$f" '$$0 ~ ("^int " f "\\(") {on=1} on {print} on && /^}/ {exit}' $(DRVSRC) | grep . || \
	    { echo "$(DRVSRC) has no int $$f(" >&2; exit 1; }; \
	done > $@.tmp && mv $@.tmp $@

test_chipvif_glue: $(CHIPVIF_DEPS)
	$(CC) $(CFLAGS) $(SANFLAGS) -DWARTHOG_MESH_CHIP_VIF_MESH=1 -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $< -o $@

test_chipvif_glue_off: $(CHIPVIF_DEPS)
	$(CC) $(CFLAGS) $(SANFLAGS) -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $< -o $@

# AT output into the 512 B CDC TX FIFO: a long reply and its OK arrive whole.
test_cdc_out: test_cdc_out.c $(MAINDIR)/cdc_out.c $(MAINDIR)/cdc_out.h
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
	$(CC) $(CFLAGS) $(SANFLAGS) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

test_simnode_peering: $(SIMNODE)/test_simnode_peering.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

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
SIMNODE_NODES := 4
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
	$(CC) $(CFLAGS) $(SANFLAGS) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

# The forwarding glue, end to end: held frames, the discovery retry, the
# bounded pending queue, both rate gates, the queue cap and the peer-loss PERR
# burst -- driven through the shipping umac_mesh_fwd_glue.c.
test_simnode_glue: $(SIMNODE)/test_simnode_glue.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

# The SAE key lifecycle: our own MGTK, which key a broadcast uses, and what a
# surviving link keeps when a peer goes -- read off the fake chip's INSTALL_KEY log.
# Built with WARTHOG_MESH_MGTK_PN_BASE, which no env pairs with chip-held AMPE keys:
# it tests our MGTK's PN base and Key RSC, and one PN allocator with chip MTKs (18).
test_simnode_keys: $(SIMNODE)/test_simnode_keys.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -DWARTHOG_MESH_MGTK_PN_BASE=1 -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

# Peer capacity: the accepting bit, Formation Info, Close(53), the hold-off, the
# SAE offer gate and failed-SAE hold-off, and the candidate RSSI floor.
test_simnode_capacity: $(SIMNODE)/test_simnode_capacity.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

# What a mac80211 peer needs before it takes us as a candidate: the RSN element
# in beacon and probe response under SAE only, and a Mesh ID probe request.
test_simnode_discovery: $(SIMNODE)/test_simnode_discovery.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

# The same file as warthog-mesh-sae-swccmp builds it: AMPE keys stay host-only, and
# host CCMP is compiled in but not armed (-swccmp-on arms it at boot).
test_simnode_keys_nochip: $(SIMNODE)/test_simnode_keys.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -DWARTHOG_MESH_AMPE_NO_CHIP_KEY=1 -DWARTHOG_MESH_HOST_CCMP=1 -DWARTHOG_MESH_MGTK_PN_BASE=1 -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

# And as warthog-mesh-sae ships: no MGTK PN base, so its install PN and Key RSC stay 0.
test_simnode_keys_pnbase_off: $(SIMNODE)/test_simnode_keys.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

# As warthog-mesh-sae-nochipkey: host-only AMPE keys, no MGTK PN base.
test_simnode_keys_nochip_pnbase_off: $(SIMNODE)/test_simnode_keys.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -DWARTHOG_MESH_AMPE_NO_CHIP_KEY=1 -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

# ---- the mesh supplicant shim against real hostap headers -----------------
#
# supplicant_core_mesh.c and hostap's ap_config.c, unmodified, compiled with the
# firmware's own hostap defines (scraped from the component's CMakeLists, so the
# struct layouts and #ifdef paths are the shipping ones) and the REAL hostap
# includes.h -- not the hostcompat shadow the crypto tests use.
HOSTAP_CMAKE := ../../halow/components/hostap/CMakeLists.txt
# Each define in the set(BUILD_DEFINES ...) block must be NAME or NAME=<digits>, one per line.
# SUPP_DEFS_BAD collects any other line, and any other BUILD_DEFINES mention; the recipe stops on it.
HASH := \#
SUPP_DEF_LINES := sed -n '/^set(BUILD_DEFINES/,/^)/p' $(HOSTAP_CMAKE) | sed -e 's/$(HASH).*//' \
                    -e 's/^set(BUILD_DEFINES//' -e 's/^)//' -e '/^[[:space:]]*$$/d'
SUPP_DEF_RE := ^[[:space:]]*\([A-Z][A-Z0-9_]*\(=[0-9]*\)\{0,1\}\)[[:space:]]*$$
SUPP_DEFS := $(shell $(SUPP_DEF_LINES) | sed -n 's/$(SUPP_DEF_RE)/-D\1/p')
SUPP_DEF_USES := -e '^set(BUILD_DEFINES' -e '$${BUILD_DEFINES}'
SUPP_DEFS_BAD := $(shell { $(SUPP_DEF_LINES) | grep -v '$(SUPP_DEF_RE)'; \
                   sed 's/$(HASH).*//' $(HOSTAP_CMAKE) | grep 'BUILD_DEFINES' | grep -v $(SUPP_DEF_USES); } | \
                   tr '\n' '|')
SUPP_INCS := -include hostcompat/hostap_prelude.h -I$(FW)/src -I$(FW)/src/hostap -I$(HAP) \
             -I$(HAP)/utils -I$(HAP)/common -I$(FW)/src/hostap/wpa_supplicant \
             -I$(FW)/morselib/include -I$(FW)/morselib/src -I$(FW)/src/mmutils \
             -I$(FW)/morselib/src/internal -I$(FW)/morselib/mmrc/src/core \
             -I$(FW)/morselib/src/umac/rc/mmrc_osal -I../../halow/components/shims/include/esp32s3 \
             -I$(SIMNODE)/hostshim -idirafter hostcompat
SUPP_HDRS := hostcompat/hostap_prelude.h $(HOSTAP_CMAKE) $(wildcard $(HAP)/ap/*.h) \
             $(wildcard $(FW)/src/hostap/wpa_supplicant/mesh*.h) \
             $(wildcard $(FW)/morselib/src/umac/supplicant_shim/*.h)

# The SAE group list the shim hands hostap: contents, order, that the real
# hostapd_config_free() can release it (bss->conf aliases conf->bss[0]), and its OOM path.
test_supp_mesh_conf: test_supp_mesh_conf.c $(FW)/morselib/src/umac/supplicant_shim/supplicant_core_mesh.c \
                     $(HAP)/ap/ap_config.c $(SIMNODE)/warthog_globals.c $(SUPP_HDRS)
	$(if $(SUPP_DEFS_BAD),$(error $(HOSTAP_CMAKE): BUILD_DEFINES line(s) the SUPP_DEFS scrape does not parse: $(SUPP_DEFS_BAD)))
	$(if $(SUPP_DEFS),,$(error $(HOSTAP_CMAKE): the SUPP_DEFS scrape found no BUILD_DEFINES))
	$(CC) $(CFLAGS) $(SANFLAGS) -Wno-void-pointer-to-int-cast $(SUPP_DEFS) $(SUPP_INCS) $(filter %.c,$^) -o $@

# Path selection against a peer running MFP: CCMP on unicast HWMP, our MGTK on group
# (group-addressed privacy), and what an SAE node accepts. Once as the chip-key builds
# ship, once as -swccmp does.
test_simnode_mfp: $(SIMNODE)/test_simnode_mfp.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

# BATMAN_V frames through the real datapath: the AE-2 replica and group shapes, the
# extended RX hook, the peer-link snapshot, and two engines (main/bat/) across two stacks.
# main/ and main/bat/ come after the SDK paths, so no main/ header shadows an SDK one.
test_simnode_batman: $(SIMNODE)/test_simnode_batman.c $(SIMNODE_SRCS) $(SIMNODE_HDRS) $(BAT_SRCS) $(BAT_HDRS) $(MAINDIR)/bat_mode.c $(MAINDIR)/bat_mode.h
	$(CC) $(CFLAGS) $(SANFLAGS) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) -I$(BATDIR) -I$(MAINDIR) $(SIMNODE_C) -o $@

# The mesh's chip VIF type through the real umac_interface.c: as warthog-mesh-sae-swccmp-meshvif
# builds it (MESH VIF started beaconless, STA fallback), and as warthog-mesh-sae-swccmp does
# (the boot STA VIF).
SWCCMP_DEFS := -DWARTHOG_MESH_AMPE_NO_CHIP_KEY=1 -DWARTHOG_MESH_HOST_CCMP=1 -DWARTHOG_MESH_MGTK_PN_BASE=1
test_simnode_meshvif: $(SIMNODE)/test_simnode_meshvif.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) $(SWCCMP_DEFS) -DWARTHOG_MESH_CHIP_VIF_MESH=1 -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

test_simnode_meshvif_off: $(SIMNODE)/test_simnode_meshvif.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) $(SWCCMP_DEFS) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

# Existing suites again with the mesh on a MESH chip VIF: the keys, crypto, batman and
# multinode behaviour each build pins must hold there too (chip keys included).
MESHVIF := -DWARTHOG_MESH_CHIP_VIF_MESH=1
test_simnode_keys_meshvif: $(SIMNODE)/test_simnode_keys.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) $(MESHVIF) -DWARTHOG_MESH_MGTK_PN_BASE=1 -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

test_simnode_keys_pnbase_off_meshvif: $(SIMNODE)/test_simnode_keys.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) $(MESHVIF) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

test_simnode_keys_nochip_meshvif: $(SIMNODE)/test_simnode_keys.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) $(MESHVIF) $(SWCCMP_DEFS) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

# As warthog-mesh-sae-meshvif: chip keys on a MESH chip VIF, where the chip holds each peer's
# MGTK at its AID and opens the peer's group path selection itself.
test_simnode_mfp_meshvif: $(SIMNODE)/test_simnode_mfp.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) $(MESHVIF) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

test_simnode_mfp_swccmp_meshvif: $(SIMNODE)/test_simnode_mfp.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) $(MESHVIF) $(SWCCMP_DEFS) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

test_simnode_batman_meshvif: $(SIMNODE)/test_simnode_batman.c $(SIMNODE_SRCS) $(SIMNODE_HDRS) $(BAT_SRCS) $(BAT_HDRS) $(MAINDIR)/bat_mode.c $(MAINDIR)/bat_mode.h
	$(CC) $(CFLAGS) $(SANFLAGS) $(MESHVIF) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) -I$(BATDIR) -I$(MAINDIR) $(SIMNODE_C) -o $@

# Its own per-node copies (libsimnodemv*), so it never loads test_simnode_multinode's.
test_simnode_multinode_meshvif: $(SIMNODE)/test_simnode_multinode.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) $(MESHVIF) -Wno-unused-function -Wno-void-pointer-to-int-cast \
	  $(SIMNODE_SOFLAGS) $(SIMNODE_INCS) $(SIMNODE_SRCS) -o libsimnodemv$(SIMNODE_SO)
	@i=0; while [ $$i -lt $(SIMNODE_NODES) ]; do \
	  cp libsimnodemv$(SIMNODE_SO) libsimnodemv$$i$(SIMNODE_SO); i=$$((i+1)); done
	$(CC) $(CFLAGS) $(SANFLAGS) -DSIMNODE_SO_SUFFIX='"$(SIMNODE_SO)"' -DSIMNODE_SO_PREFIX='"libsimnodemv"' \
	  -DSIMNODE_NODES=$(SIMNODE_NODES) $(SIMNODE_INCS) $< -o $@ $(SIMNODE_DLLIBS)

# A peer's MGTK in the chip at its AID, as Linux installs it, and the group frames the
# chip then opens (the fake chip's receive model, simnode_rx_air): as warthog-mesh-sae-meshvif
# builds it, and pinned as warthog-mesh-sae and warthog-mesh-sae-swccmp-meshvif do.
test_simnode_peergtk_meshvif: $(SIMNODE)/test_simnode_peergtk.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) $(MESHVIF) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

test_simnode_peergtk: $(SIMNODE)/test_simnode_peergtk.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

test_simnode_peergtk_swccmp_meshvif: $(SIMNODE)/test_simnode_peergtk.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) $(MESHVIF) $(SWCCMP_DEFS) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

# Fragmented unicast through the real receive path: reassembled before Mesh Control, the
# forwarding engine and leaf learning, with mac80211's FragAttacks checks. As warthog-mesh-sae
# (the chip opens it), -nochipkey (nothing opens SAE data), -swccmp (host CCMP), -meshvif, and
# -swccmp-meshvif, the build measured on air.
test_simnode_frag: $(SIMNODE)/test_simnode_frag.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

test_simnode_frag_nochip: $(SIMNODE)/test_simnode_frag.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -DWARTHOG_MESH_AMPE_NO_CHIP_KEY=1 -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

test_simnode_frag_swccmp: $(SIMNODE)/test_simnode_frag.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) $(SWCCMP_DEFS) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

test_simnode_frag_meshvif: $(SIMNODE)/test_simnode_frag.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) $(MESHVIF) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

test_simnode_frag_swccmp_meshvif: $(SIMNODE)/test_simnode_frag.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) $(MESHVIF) $(SWCCMP_DEFS) -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

test_simnode_mfp_swccmp: $(SIMNODE)/test_simnode_mfp.c $(SIMNODE_SRCS) $(SIMNODE_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) -DWARTHOG_MESH_AMPE_NO_CHIP_KEY=1 -DWARTHOG_MESH_HOST_CCMP=1 -DWARTHOG_MESH_MGTK_PN_BASE=1 -Wno-unused-function -Wno-void-pointer-to-int-cast $(SIMNODE_INCS) $(SIMNODE_C) -o $@

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

# BIP-CMAC-128 (the MMIE an SAE node refuses on group path selection) against RFC 4493,
# hostap's omac1_aes_128, and wlantest's bip_protect construction.
test_mesh_bip: test_mesh_bip.c $(SDKMESH)/umac_mesh_bip.c $(SDKMESH)/umac_mesh_bip.h $(HOSTAP)/src/crypto/aes-omac1.c $(CCMSRC)
	$(CC) $(CFLAGS) $(SANFLAGS) -Wno-unused-parameter -Wno-sign-compare \
	  -Ihostcompat -I$(SDKMESH) -I$(HOSTAP)/src/utils -I$(HOSTAP)/src/crypto -I$(HOSTAP)/src -I$(HOSTAP) $(filter %.c,$^) -o $@

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
	@$(CC) $(CFLAGS) -Werror -fsyntax-only -I../../../main ../../../main/cdc_out.c && echo "ok   cdc_out.c is freestanding"
	@$(CC) $(CFLAGS) -Werror -fsyntax-only -I../../../main -I../../../main/bat ../../../main/bat_mode.c && echo "ok   bat_mode.c is freestanding (libc and the engine's bat.h)"
	@$(CC) $(CFLAGS) -Werror -fsyntax-only -I$(SDKMESH) $(SDKMESH)/umac_mesh_ctrl.c && echo "ok   umac_mesh_ctrl.c is freestanding"
	@$(CC) $(CFLAGS) -Werror -fsyntax-only -I$(SDKMESH) $(SDKMESH)/umac_mesh_rmc.c && echo "ok   umac_mesh_rmc.c is freestanding"
	@$(CC) $(CFLAGS) -Werror -fsyntax-only -I$(SDKMESH) $(SDKMESH)/umac_mesh_pathtbl.c && echo "ok   umac_mesh_pathtbl.c is freestanding"
	@$(CC) $(CFLAGS) -Werror -fsyntax-only -I$(SDKMESH) $(SDKMESH)/umac_mesh_hwmp_relay.c && echo "ok   umac_mesh_hwmp_relay.c is freestanding"
	@$(CC) $(CFLAGS) -Werror -fsyntax-only -I$(SDKMESH) $(SDKMESH)/umac_mesh_fwd.c && echo "ok   umac_mesh_fwd.c is freestanding"
	@$(CC) $(CFLAGS) -Werror -fsyntax-only -include $(BCNDIR)/beacon.h -x c /dev/null && echo "ok   driver/beacon/beacon.h is freestanding"

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
	rm -f $(TESTS) linkcheck_all libsimnode*.dylib libsimnode*.so test_chipvif_glue.fns test_chipvif_glue.fns.tmp
	rm -rf *.dSYM
