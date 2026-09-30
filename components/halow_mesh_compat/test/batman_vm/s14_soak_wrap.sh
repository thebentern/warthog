#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# s14_soak_wrap: W - L for 150 s of steady traffic with W's millisecond clock started
# 60 s before the 2^32 wrap (--clock-base), so every engine timer, age and deadline
# crosses the wrap live. Nothing may flap: L's last-seen for W stays fresh, no TT
# requests, no restart/duplicate/old counters beyond start-up. Then L departs (bat0
# deleted) and W must drop L's route and TT rows 200 s after L's last OGM (not before
# ~199 s; elp-ogm section 6), keep running, and re-learn L when it returns.
source "$(dirname "$0")/lib.sh"
bvw_begin
W_TAGS=w
mk_pair client 1460 -- --clock-base 0xFFFF15A0 || exit 1
WAITFOR 6 "L routes W" "bo_best_via l $W_HARD $W_HARD"
WAITFOR 6 "W routes L and holds L's TT" "wst w | grep -qE '^\+BATO: $L_HARD .* nh=$L_HARD' && w_tt_matches_peer w l $L_HARD"
bla_settle l
WAITFOR 20 "first L -> W ping" "NX l ping -c 1 -W 1 $W_IP >/dev/null"
wfresh
now0=$(wcnt w now)
note "engine clock now $now0 ms (wraps at 4294967296)"
snap() { wst w | grep '^+BATSTAT: [a-z]' | tr ' ' '\n' | grep -E '^(ogm_restart|ogm_restart_blocked|ogm_old|elp_dup|bc_dup|bc_restart_blocked|tt_req_tx|tt_crc_fail|route_lost|neigh_purged|orig_purged|lk_fail)='; }
S0=$(snap | tr '\n' ' '); N0=$(bstat l tt_request_tx)
ip netns exec bvw_l ping -i 0.5 -W 1 "$W_IP" >"$BVW_RUN/s14.ping_lw" 2>&1 &
ip netns exec bvw_w ping -i 0.5 -W 1 "$L_IP" >"$BVW_RUN/s14.ping_wl" 2>&1 &
MAXSEEN=0; WRAPPED=0; t0=$(now_ms)
while [ $(( $(now_ms) - t0 )) -lt 150000 ]; do
    s=$(bo_seen l $W_HARD); [ -z "$s" ] && s=999
    MAXSEEN=$(awk -v a="$MAXSEEN" -v b="$s" 'BEGIN { print (b > a) ? b : a }')
    n=$(wcnt w now); [ "$n" -lt "$now0" ] && WRAPPED=1
    if [ $(( ($(now_ms) - t0) / 1000 % 15 )) = 0 ]; then
        NX w $PY "$BVW_DIR/udp.py" send 239.0.0.69 4403 "$W_IP" soak 1 >/dev/null 2>&1
    fi
    sleep 1
done
pkill -INT -f "ping -i 0.5 -W 1 $W_IP" ; pkill -INT -f "ping -i 0.5 -W 1 $L_IP"
sleep 1
wfresh
S1=$(snap | tr '\n' ' ')
note "before: $S0"
note "after : $S1"
CHECK "W's clock wrapped past 2^32 during the soak (now $(wcnt w now))" "[ $WRAPPED = 1 ]"
CHECK "L's last-seen for W stayed below 2.5 s throughout (max $MAXSEEN s)" "awk 'BEGIN { exit !($MAXSEEN < 2.5) }'"
CHECK "no flaps, restarts, duplicates or requests in W across the wrap" "[ '$S0' = '$S1' ]"
CHECK "L sent no TT request during the soak ($N0 -> $(bstat l tt_request_tx))" "[ $(bstat l tt_request_tx) = $N0 ]"
LW=$(tail -2 "$BVW_RUN/s14.ping_lw" | head -1); WL=$(tail -2 "$BVW_RUN/s14.ping_wl" | head -1)
note "L -> W: $LW"; note "W -> L: $WL"
CHECK "continuous pings both ways lost nothing" "echo '$LW' | grep -q ' 0% packet loss' && echo '$WL' | grep -q ' 0% packet loss'"

note "L departs (bat0 deleted)"
del_batman_peer l
T_GONE=$(now_ms)
while [ $(( $(now_ms) - T_GONE )) -lt 215000 ]; do
    wst w | grep -qE "^\+BATO: $L_HARD .* nh=$L_HARD" || break
    sleep 1
done
T_LOST=$(( $(now_ms) - T_GONE ))
note "W dropped the route to L after $T_LOST ms"
CHECK "W dropped L's route 198-205 s after L's last OGM ($T_LOST ms)" "[ $T_LOST -ge 198000 ] && [ $T_LOST -le 205000 ]"
wfresh
CHECK "L's TT rows went with the route" "! wst w | grep -qE '^\+BATTG: [0-9a-f:]{17} vid=-?[0-9]+ via=$L_HARD '"
CHECK "W is still running" "w_alive w"
mk_batman_peer l bvw_l0 "$L_HARD" "$L_BAT" "$L_IP/16" client || exit 1
WAITFOR 10 "L returns: W routes L and holds its TT again" "wst w | grep -qE '^\+BATO: $L_HARD .* nh=$L_HARD' && w_tt_matches_peer w l $L_HARD"
WAITFOR 10 "and L routes W with W's CRC" "bo_best_via l $W_HARD $W_HARD && c=\$(w_soft_crc w $W_SOFT) && btg l | grep '$W_SOFT ' | grep -q \"(0x\$c)\""
snapshot end l
cap_stop
finish
