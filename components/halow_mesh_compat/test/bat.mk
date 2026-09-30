# SPDX-License-Identifier: GPL-2.0-or-later
# Host tests for the clean-room BATMAN_V engine in main/bat/, included by Makefile.
#
# Every test links the whole engine (all of main/bat/*.c) plus the in-process
# multi-engine simulator bat_sim.c, so what a test drives is the shipping code.
#
#   bat-freestanding  each engine file compiles with nothing but libc headers
#   bat-nostatic      no engine object carries writable static data, so several
#                     engines can live in one process (the simulator relies on it)
#   bat-registered    every batman test source has a TESTS entry, so `make all` runs it

# Rules here precede the Makefile's `all:`; without this a bare `make` builds test_bat_golden.
.DEFAULT_GOAL := all

BAT_MAIN  := ../../../main
BATDIR    := $(BAT_MAIN)/bat
BAT_SRCS  := $(wildcard $(BATDIR)/*.c)
BAT_HDRS  := $(wildcard $(BATDIR)/*.h)
BAT_TESTS := test_bat_codec test_bat_core test_bat_sim test_bat_tt test_bat_data \
             test_bat_golden test_bat_hostile
TESTS     += $(BAT_TESTS)

BAT_UNAME := $(shell uname -s)
BAT_TEST_DEFS :=

# The golden and hostile tests read the committed captures with the header-only pcap reader.
test_bat_golden: BAT_TEST_DEFS := -DBAT_GOLDEN_DIR=\"bat_golden/pcap\"
test_bat_golden: bat_pcap.c bat_pcap.h
test_bat_hostile: bat_pcap.h

$(BAT_TESTS): test_bat_%: test_bat_%.c bat_sim.c bat_sim.h $(BAT_SRCS) $(BAT_HDRS)
	$(CC) $(CFLAGS) $(SANFLAGS) $(BAT_TEST_DEFS) -I$(BATDIR) -I. $(filter %.c,$^) -o $@

.PHONY: bat-freestanding bat-nostatic bat-registered
bat-registered:
	@echo "=== every batman test program is built and run by make all ==="
	@rc=0; for f in test_bat_*.c $(SIMNODE)/test_simnode_bat*.c; do \
	  t=$$(basename $$f .c); \
	  case " $(TESTS) " in *" $$t "*) echo "ok   $$t is in TESTS";; \
	  *) echo "FAIL $$f has no TESTS entry: make all never builds or runs it"; rc=1;; esac; \
	done; \
	if [ "$(.DEFAULT_GOAL)" = all ]; then echo "ok   a bare make is make all"; \
	else echo "FAIL a bare make builds $(.DEFAULT_GOAL), not all"; rc=1; fi; exit $$rc

bat-freestanding:
	@echo "=== freestanding check (batman engine: libc only) ==="
	@bad=$$(grep -h '^[[:space:]]*#[[:space:]]*include[[:space:]]*<' $(BATDIR)/*.[ch] | \
	  grep -v -E '<(stdbool|stddef|stdint|string|stdio)\.h>'); \
	  if [ -n "$$bad" ]; then echo "FAIL engine includes beyond libc: $$bad"; exit 1; fi
	@for f in $(BAT_SRCS); do \
	  $(CC) $(CFLAGS) -Werror -fsyntax-only -I$(BATDIR) $$f || exit 1; \
	  echo "ok   $$(basename $$f) is freestanding"; done

bat-nostatic:
	@echo "=== no writable statics in the batman engine ==="
	@d=$$(mktemp -d); rc=0; \
	for f in $(BAT_SRCS); do \
	  o=$$d/$$(basename $$f .c).o; \
	  $(CC) $(CFLAGS) -c -I$(BATDIR) $$f -o $$o || { rc=1; continue; }; \
	  if [ "$(BAT_UNAME)" = Darwin ]; then \
	    w=$$(size -m $$o | sed -En 's/.*\(__DATA, __(data|bss|common)\): ([0-9]+).*/\2/p' | \
	         awk '{ s += $$1 } END { print s + 0 }'); \
	  else \
	    w=$$(size -A $$o | awk '$$1 ~ /^\.(s?data|s?bss)$$/ { s += $$2 } END { print s + 0 }'); \
	  fi; \
	  if [ "$$w" = 0 ]; then echo "ok   $$(basename $$f) has no writable statics"; \
	  else echo "FAIL $$(basename $$f) has $$w bytes of writable statics"; rc=1; fi; \
	done; rm -rf $$d; exit $$rc
