#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# s05_frag: 1472-byte DF pings both ways with L's bat0 MTU 1500 and W's soft MTU 1500:
# L fragments toward W (W reassembles), W fragments toward L. Then W restarts with
# --hard-mtu 600 (3 fragments, none above 600 batman bytes) and the pings repeat.
source "$(dirname "$0")/lib.sh"
bvw_begin
W_TAGS=w
mk_pair client 1500 -- --soft-mtu 1500 || exit 1
WAITFOR 6 "L routes W" "bo_best_via l $W_HARD $W_HARD"
bla_settle l
WAITFOR 20 "small ping L -> W" "NX l ping -c 1 -W 1 $W_IP >/dev/null"

frag_round() {      # frag_round <label> <frags-per-packet>
    local fd0 ft0 lr0 lt0
    fd0=$(wcnt w fr_done); ft0=$(wcnt w fr_tx); lr0=$(bstat l frag_rx); lt0=$(bstat l frag_tx)
    CHECK "$1: 1472-byte DF ping L -> W 3/3" "NX l ping -c 3 -i 0.3 -s 1472 -M do -W 2 $W_IP | grep -q ' 0% packet loss'"
    CHECK "$1: 1472-byte DF ping W -> L 3/3" "NX w ping -c 3 -i 0.3 -s 1472 -M do -W 2 $L_IP | grep -q ' 0% packet loss'"
    wfresh
    CHECK "$1: W reassembled L's packets (fr_done $fd0 -> $(wcnt w fr_done))" "[ $(wcnt w fr_done) -ge $((fd0 + 6)) ]"
    CHECK "$1: W cut its packets into $2 fragments each (fr_tx $ft0 -> $(wcnt w fr_tx))" "[ $(wcnt w fr_tx) -ge $((ft0 + 6 * $2)) ]"
    CHECK "$1: L received W's fragments (frag_rx $lr0 -> $(bstat l frag_rx)) and fragmented (frag_tx $lt0 -> $(bstat l frag_tx))" \
        "[ $(bstat l frag_rx) -ge $((lr0 + 6 * $2)) ] && [ $(bstat l frag_tx) -ge $((lt0 + 12)) ]"
    fd0=$(wcnt w fr_done); ft0=$(wcnt w fr_tx)
    CHECK "$1: 3000-byte ping L -> W 3/3 (IP fragments of 1500, each batman-fragmented)" "NX l ping -c 3 -i 0.3 -s 3000 -W 2 $W_IP | grep -q ' 0% packet loss'"
    CHECK "$1: 3000-byte ping W -> L 3/3" "NX w ping -c 3 -i 0.3 -s 3000 -W 2 $L_IP | grep -q ' 0% packet loss'"
    wfresh
    CHECK "$1: 3000-byte pings used batman fragments both ways (fr_done $fd0 -> $(wcnt w fr_done), fr_tx $ft0 -> $(wcnt w fr_tx))" \
        "[ $(wcnt w fr_done) -ge $((fd0 + 12)) ] && [ $(wcnt w fr_tx) -ge $((ft0 + 12 * $2)) ]"
}
frag_round "hard MTU 1500" 2
note "restart W with --hard-mtu 600"
stop_w w
sleep 3
cap_stop
cap_start w bvw_w0 w0-mtu600
start_w w w bvw_w0 "$W_HARD" "$W_SOFT" "$W_IP/16" --soft-mtu 1500 --hard-mtu 600
WAITFOR 5 "L accepts the restarted W (last-seen < 1.5 s)" "seen_lt l $W_HARD 1.5"
WAITFOR 20 "small ping W -> L after the restart" "NX w ping -c 1 -W 1 $L_IP >/dev/null"
frag_round "hard MTU 600" 3
cap_stop
Q=$($PY "$BVW_DIR/pcapq.py" count "$BVW_EVID/$SCEN-w0-mtu600.pcap" --src "$W_HARD" --type 0x41)
CHECK "on the wire: every W fragment at hard MTU 600 is <= 614 bytes ($Q)" "[ \$(echo '$Q' | sed -E 's/.*maxlen=//') -le 614 ] && [ \$(echo '$Q' | sed -E 's/count=([0-9]+).*/\1/') -ge 54 ]"
snapshot end l
finish
