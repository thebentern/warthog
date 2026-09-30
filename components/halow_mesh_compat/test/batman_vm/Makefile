# SPDX-License-Identifier: GPL-2.0-or-later
# VM interop suite for the clean-room BATMAN_V engine (main/bat). Linux only, not part of
# the host test "make all": it needs root, network namespaces and batman-adv 2024.3.
#
#   make              batvm_node      (the engine + batvm_node.c, -O2)
#   make SAN=1        batvm_node_san  (ASan/UBSan, used by s08_hostile_live)
#   make all          both
#   make WERROR=1 all both, warnings as errors (CI compiles this; it runs nothing)
#   make lint         shellcheck: no unquoted $(...) in the scripts (CI runs this)
#
# Run the scenarios with ./run_all.sh (see its header).

CC      ?= gcc
BATDIR  := ../../../../main/bat
SRCS    := batvm_node.c $(wildcard $(BATDIR)/*.c)
HDRS    := $(wildcard $(BATDIR)/*.h)
CFLAGS  := -std=gnu11 -Wall -Wextra -Wno-unused-parameter -g -I$(BATDIR)
# An engine API change the harness missed can be only a warning (an ops callback's pointer type).
ifeq ($(WERROR),1)
CFLAGS  += -Werror
endif

ifeq ($(SAN),1)
default: batvm_node_san
else
default: batvm_node
endif

all: batvm_node batvm_node_san

batvm_node: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -O2 $(filter %.c,$^) -o $@

batvm_node_san: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -O1 -fno-omit-frame-pointer -fsanitize=address,undefined \
	  -fno-sanitize-recover=undefined $(filter %.c,$^) -o $@

# The scenarios print batctl output, where "*" marks the best route: unquoted, it globs.
lint:
	@command -v shellcheck >/dev/null || { echo "FAIL shellcheck is not installed"; exit 1; }
	shellcheck -i SC2046 *.sh

clean:
	rm -f batvm_node batvm_node_san

.PHONY: default all clean lint
