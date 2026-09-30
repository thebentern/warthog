#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# s08_hostile_live: W runs the ASan/UBSan build next to L. hostile.py, sent from L's end
# of W's veth, injects the crafted set of design 6.4 item 3 (every malformed header,
# fragment and TTL case, own/group link sources, own originators, unknown types) and
# 3000 seeded mutations. W must survive with no sanitizer report, count every crafted
# class, and still pass the s01/s02 checks afterwards.
source "$(dirname "$0")/lib.sh"
bvw_begin
W_TAGS=w
F_HARD=02:b0:66:00:00:01
make -s -C "$BVW_DIR" batvm_node_san >/dev/null 2>&1
BVW_NODE_BIN=$BVW_DIR/batvm_node_san
CHECK "SAN build present" "[ -x '$BVW_NODE_BIN' ]"
mk_pair client 1460 || exit 1
WAITFOR 6 "L routes W" "bo_best_via l $W_HARD $W_HARD"
bla_settle l
WAITFOR 20 "L -> W ping before the attack" "NX l ping -c 1 -W 1 $W_IP >/dev/null"
wfresh
wst w | grep '^+BATSTAT: [a-z]' >"$BVW_RUN/s08.before"

note "hostile.py: crafted set + 3000 mutations into W's veth"
NX l $PY "$BVW_DIR/hostile.py" bvw_l0 --w "$W_HARD" --l "$L_HARD" --fake "$F_HARD" --fuzz 3000 --seed 20260927 \
    | tee "$BVW_EVID/$SCEN-hostile.txt" | sed 's/^/     | /'
sleep 2
CHECK "W is still running" "w_alive w"
CHECK "no ASan/UBSan report in W's log" "! grep -qE 'ERROR: AddressSanitizer|runtime error|SUMMARY:' '$BVW_RUN/w.log'"
wfresh
before() { tr ' ' '\n' <"$BVW_RUN/s08.before" | grep "^$1=" | cut -d= -f2; }
for c in elp_probe elp_own_orig rx_src_own rx_src_bad rx_version rx_type rx_short rx_hdr rx_mgmt_dst \
         rx_uni_dst ogm_own ogm_overrun ogm_badrec tt_bad ogm_tput0 ogm_not_neigh bc_own bc_ttl bc_unknown \
         uc_ttl uc_inner_bad uc_noroute uc_4a_dat unk_self ic_ttlx ic_tp ic_rr_full ut_len tt_roam_rx \
         fr_bad fr_toobig fr_dup fr_unknown fr_ttl fr_nested; do
    b=$(before $c); a=$(wcnt w $c)
    CHECK "crafted class counted: $c ${b:-0} -> $a" "[ $a -gt ${b:-0} ]"
done

note "the pair still works (s01/s02 checks)"
WAITFOR 5 "L still routes W" "bo_best_via l $W_HARD $W_HARD && seen_lt l $W_HARD 2"
WAITFOR 5 "W still routes L and holds every TT CRC of L" "wst w | grep -qE '^\+BATO: $L_HARD .* nh=$L_HARD' && w_tt_matches_peer w l $L_HARD"
CHECK "L's TT still has W's soft MAC with W's CRC" "btg l | grep '$W_SOFT ' | grep -q '(0x$(w_soft_crc w $W_SOFT))'"
N1=$(bstat l tt_request_tx); R1=$(wcnt w tt_req_tx)
sleep 6
CHECK "TT requests quiet on both sides (L $N1 -> $(bstat l tt_request_tx), W $R1 -> $(wcnt w tt_req_tx))" \
    "[ $(bstat l tt_request_tx) = $N1 ] && [ $(wcnt w tt_req_tx) = $R1 ]"
CHECK "L -> W ping 5/5" "NX l ping -c 5 -i 0.2 -W 2 $W_IP | grep -q ' 0% packet loss'"
CHECK "W -> L ping 5/5" "NX w ping -c 5 -i 0.2 -W 2 $L_IP | grep -q ' 0% packet loss'"
CHECK "batctl ping -c 3 <W> answered" "NX l batctl meshif bat0 ping -c 3 $W_HARD | grep -q '3 packets transmitted, 3 received'"
CHECK "1432-byte DF ping L -> W (a full 1460-byte bat0 MTU) 2/2" "NX l ping -c 2 -s 1432 -M do -W 2 $W_IP | grep -q ' 0% packet loss'"
stop_w w
sleep 0.5
CHECK "W exited cleanly with no sanitizer report (leak check included)" "! grep -qE 'ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error|SUMMARY:' '$BVW_RUN/w.log'"
cp "$BVW_RUN/w.log" "$BVW_EVID/$SCEN-w.log"
snapshot end l
cap_stop
finish
