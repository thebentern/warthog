#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# s10_bridged_peer: L's bat0 becomes a port of a Linux bridge (as OpenMANET's br-ahwlan),
# with a LAN client X on another port. L's TT changes live: bat0's MAC gains VLAN 1
# (0x8001, bridge default PVID), its multicast-listener entries are withdrawn, and X is
# added later. W must apply those diffs, match every per-VLAN CRC without a request
# loop, and reach X through L (and X reach W).
source "$(dirname "$0")/lib.sh"
bvw_begin
W_TAGS=w
X_MAC=02:b0:0d:00:00:01; X_IP=10.41.0.50
mk_ns x
mk_pair client 1460 || exit 1
WAITFOR 6 "L routes W" "bo_best_via l $W_HARD $W_HARD"
WAITFOR 8 "W holds every (VID, CRC) record of the unbridged L" "w_tt_matches_peer w l $L_HARD"
note "L before bridging:" "$(btl_vid_crcs l | oneline)"
R0=$(wcnt w tt_req_tx); F0=$(wcnt w tt_crc_fail); N0=$(bstat l tt_request_tx)

note "bridge L's bat0 into bvw_br together with LAN port bvw_lx (client X)"
NX l ip link add bvw_br type bridge
NX l ip link set bvw_br address "$L_BAT"
NX l ip addr del "$L_IP/16" dev bat0
NX l ip link set bat0 master bvw_br
mk_veth x bvw_x0 "$X_MAC" l bvw_lx 02:b0:1c:00:0d:01
NX l ip link set bvw_lx master bvw_br
NX l ip addr add "$L_IP/16" dev bvw_br
NX l ip link set bvw_br up
NX x ip addr add "$X_IP/16" dev bvw_x0
WAITFOR 10 "L announces its bat0 MAC in VLAN 1 (bridge PVID)" "btl l | grep -qE '^ *\*? *$L_BAT +1 '"
WAITFOR 10 "W holds every (VID, CRC) record of the bridged L, VLAN 1 included" \
    "w_tt_matches_peer w l $L_HARD && w_vid_crcs w $L_HARD | grep -q '^1 '"
note "L after bridging :" "$(btl_vid_crcs l | oneline)"
note "W's view of L    :" "$(w_vid_crcs w $L_HARD | oneline)"
LSET=$(btl l | sed -nE 's/^ *\*? *([0-9a-f:]{17}) +(-?[0-9]+) .*/\1 \2/p' | sort -u)
WSET=$(wst w | sed -nE "s/^\+BATTG: ([0-9a-f:]{17}) vid=(-?[0-9]+) via=$L_HARD .*/\1 \2/p" | sort -u)
CHECK "W's rows for L equal L's local table after the diff ($(echo "$LSET" | wc -l) rows)" "[ '$LSET' = '$WSET' ]"
R1=$(wcnt w tt_req_tx); F1=$(wcnt w tt_crc_fail)
sleep 10
R2=$(wcnt w tt_req_tx); F2=$(wcnt w tt_crc_fail)
CHECK "W's TT requests toward L stopped (tt_req_tx $R0 -> $R1 -> $R2, tt_crc_fail $F0 -> $F1 -> $F2)" \
    "[ $R1 = $R2 ] && [ $F1 = $F2 ] && [ $((R2 - R0)) -le 1 ]"
CHECK "L still holds W's soft MAC with W's CRC and requested nothing (tt_request_tx $N0 -> $(bstat l tt_request_tx))" \
    "btg l | grep '$W_SOFT ' | grep -q '(0x$(w_soft_crc w $W_SOFT))' && [ $(bstat l tt_request_tx) = $N0 ]"

note "client X behind L's bridge"
bla_settle l
WAITFOR 45 "X (on L's LAN) pings W across the mesh" "NX x ping -c 1 -W 1 $W_IP >/dev/null"
WAITFOR 10 "W's global TT has X via L, announced (not temporary)" \
    "wst w | grep -qE '^\+BATTG: $X_MAC vid=-1 via=$L_HARD ttvn=[0-9]+ flags=--[^T]'"
CHECK "W -> X ping 5/5 (unicast resolved through W's TT)" "NX w ping -c 5 -i 0.2 -W 2 $X_IP | grep -q ' 0% packet loss'"
CHECK "X -> W ping 5/5" "NX x ping -c 5 -i 0.2 -W 2 $W_IP | grep -q ' 0% packet loss'"
WAITFOR 5 "W still matches every (VID, CRC) of L with X announced" "w_tt_matches_peer w l $L_HARD"
R3=$(wcnt w tt_req_tx)
sleep 6
CHECK "W's TT requests stay quiet ($R3 -> $(wcnt w tt_req_tx))" "[ $(wcnt w tt_req_tx) = $R3 ]"
CHECK "W -> L's bridge address ping" "NX w ping -c 3 -i 0.2 -W 2 $L_IP | grep -q ' 0% packet loss'"
note "X also sends on 802.1Q VLAN 5 through L's bridge (VID 5 registered on L's bat0)"
NX l ip link add link bat0 name bat0.5 type vlan id 5
NX l ip link set bat0.5 up
D5=$(wcnt w bc_deliver)
NX x ip link add link bvw_x0 name bvw_x0.5 type vlan id 5
NX x ip addr add 10.42.0.50/16 dev bvw_x0.5
NX x ip link set bvw_x0.5 up
# L's BLA holds broadcasts of a VLAN for ~30 s after the VLAN first carries traffic
ip netns exec bvw_x ping -i 1 -W 1 -b 10.42.255.255 >/dev/null 2>&1 &
WAITFOR 45 "L announces X in VLAN 5 (after L's per-VLAN BLA start-up hold)" "btl l | grep -qE '^ *\*? *$X_MAC +5 '"
WAITFOR 10 "W holds X in VLAN 5 via L and every (VID, CRC) of L, VLAN 5 included" \
    "wst w | grep -qE '^\+BATTG: $X_MAC vid=5 via=$L_HARD ' && w_tt_matches_peer w l $L_HARD && w_vid_crcs w $L_HARD | grep -q '^5 '"
note "L:" "$(btl_vid_crcs l | oneline)" "| W:" "$(w_vid_crcs w $L_HARD | oneline)"
R4=$(wcnt w tt_req_tx); sleep 6
CHECK "W's TT requests quiet with the tagged VLAN ($R4 -> $(wcnt w tt_req_tx))" "[ $(wcnt w tt_req_tx) = $R4 ]"
wfresh
CHECK "W delivered X's tagged broadcasts to its soft interface (bc_deliver $D5 -> $(wcnt w bc_deliver))" "[ $(wcnt w bc_deliver) -ge $((D5 + 3)) ]"
snapshot end l
cap_stop
finish
