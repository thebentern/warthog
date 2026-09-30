#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# s18_tt_join: W joins a running L (up 4-5 s) 10 times, at staggered phases of L's OGM timer.
# When W's first TT request reaches L before W's own first OGM has, L cannot route back and drops it
# (tt 6.2); W then asks again at L's next OGM, as Linux does (tt 6.1), so every join holds L's table
# at most 1.5 s after W's first request (a 3 s request guard made those joins 3-4 s).
source "$(dirname "$0")/lib.sh"
bvw_begin
W_TAGS=w
EARLY=0; SLOW=0; MANY=0; WORST=0
for i in $(seq 1 10); do
    bvw_cleanup
    mk_ns l w
    mk_veth l bvw_l0 "$L_HARD" w bvw_w0 "$W_HARD"
    mk_batman_peer l bvw_l0 "$L_HARD" "$L_BAT" "$L_IP/16" client || exit 1
    sleep "4.$(printf %03d $(( (i * 137) % 1000 )))"
    cap_start w bvw_w0 "join$i"
    start_w w w bvw_w0 "$W_HARD" "$W_SOFT" -
    sleep 6
    stop_w w
    cap_stop
    r=$($PY "$BVW_DIR/pcapq.py" ttjoin "$BVW_EVID/$SCEN-join$i.pcap" --src "$W_HARD" --orig "$L_HARD")
    note "join $i: $r"
    n=$(sed -nE 's/.*reqs=([0-9]+).*/\1/p' <<<"$r")
    ms=$(sed -nE 's/.*answer_ms=(-?[0-9]+).*/\1/p' <<<"$r")
    grep -q 'early=1' <<<"$r" && EARLY=$((EARLY + 1))
    { [ -z "$ms" ] || [ "$ms" -lt 0 ] || [ "$ms" -gt 1500 ]; } && SLOW=$((SLOW + 1))
    { [ -z "$n" ] || [ "$n" -gt 2 ]; } && MANY=$((MANY + 1))
    [ -n "$ms" ] && [ "$ms" -gt "$WORST" ] && WORST=$ms
done
CHECK "every join has L's table at most 1.5 s after W's first TT request ($SLOW slow, worst $WORST ms; $EARLY of 10 asked before W's own first OGM)" \
    "[ $SLOW = 0 ]"
CHECK "at most 2 TT requests per join ($MANY joins with more)" "[ $MANY = 0 ]"
[ "$EARLY" -gt 0 ] || SKIP "no join asked before W's own first OGM: the race was not exercised"
finish
