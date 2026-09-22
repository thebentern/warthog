#!/bin/sh
# Regenerate fake_radio_stack.c against the Makefile's own source set.
#
# Two rules this script exists to enforce: the stub list must come from a link
# that EXCLUDES the stubs (or it collapses to nothing), and it must use the
# same source set the Makefile compiles (or a symbol ends up defined twice,
# once by a real TU and once by a stub).
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
FW=$ROOT/../../halow/components/mm-iot-sdk/framework
make -s simnode-srcs | grep -v 'fake_radio_stack\.c' > /tmp/simnode_srcs.txt
make -s simnode-incs > /tmp/simnode_incs.txt
echo 'int main(void){return 0;}' > /tmp/simnode_probe_main.c
clang -std=gnu11 -w $(cat /tmp/simnode_incs.txt) $(cat /tmp/simnode_srcs.txt) \
  /tmp/simnode_probe_main.c -o /tmp/simnode_probe 2>&1 \
  | grep -oE '"_[A-Za-z_][A-Za-z0-9_]*"' | sed 's/"//g; s/^_//' | sort -u \
  > /tmp/simnode_stub_list.txt || true
echo "undefined without stubs: $(wc -l < /tmp/simnode_stub_list.txt)"
cd simnode
python3 gen_stubs.py /tmp/simnode_stub_list.txt \
  "$FW/morselib/src" "$FW/morselib/include" "$FW/src"
