#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# s11_line_via_linux: W - B - C where B is a batman-adv relay with two hard interfaces:
# its primary (originator) faces C and its second interface faces W, so every ELP/OGM W
# hears from B carries an originator different from the link source and W's next hop
# (B's second interface) differs from B's originator. B and C both bridge bat0. W is a leaf two hops from C:
# route via B, C's TT (possibly answered by B on C's behalf), unicast/ICMP/broadcast and
# fragments across B. Then client X roams from C's LAN to B's LAN: W must drop X via C
# (DEL|ROAM), learn X via B, keep both CRCs matching and reach X at its new home.
source "$(dirname "$0")/lib.sh"
bvw_begin
W_TAGS=w
B0_HARD=02:b0:0b:00:00:01; B1_HARD=02:b0:0b:00:00:02; B_BAT=02:b0:0b:00:00:ff; B_IP=10.41.0.11
X_MAC=02:b0:0d:00:00:01; X_IP=10.41.0.50
mk_ns w b c x
mk_veth w bvw_w0 "$W_HARD" b bvw_b0 "$B0_HARD"
mk_veth b bvw_b1 "$B1_HARD" c bvw_c0 "$C_HARD"
mk_batman_peer b bvw_b1 "$B1_HARD" "$B_BAT" - server 1500 || exit 1   # primary: B1 = originator
NX b batctl meshif bat0 if add bvw_b0
mk_batman_peer c bvw_c0 "$C_HARD" "$C_BAT" - client 1500 || exit 1
for n in b c; do                               # br-ahwlan: bridge holding bat0 and the address
    NX $n ip link add bvw_br type bridge
    case $n in b) m=$B_BAT ip=$B_IP ;; c) m=$C_BAT ip=$C_IP ;; esac
    NX $n ip link set bvw_br address $m
    NX $n ip link set bat0 master bvw_br
    NX $n ip addr add $ip/16 dev bvw_br
    NX $n ip link set bvw_br up
done
mk_veth x bvw_x0 "$X_MAC" c bvw_cx 02:b0:0c:00:0d:01
NX c ip link set bvw_cx master bvw_br
NX x ip addr add "$X_IP/16" dev bvw_x0
cap_start w bvw_w0 w0
start_w w w bvw_w0 "$W_HARD" "$W_SOFT" "$W_IP/16" --soft-mtu 1500 --tput auto

WAITFOR 10 "W routes C via B (next hop = B's link address toward W, not its originator)" "wst w | grep -qE '^\+BATO: $C_HARD .* nh=$B0_HARD tput='"
CHECK "W's neighbour B is keyed by its link address with originator B1" "wst w | grep -qE '^\+BATN: $B0_HARD orig=$B1_HARD '"
CHECK "W routes B's originator B1 via B's link address" "wst w | grep -qE '^\+BATO: $B1_HARD .* nh=$B0_HARD '"
WAITFOR 3 "--tput auto: W's link to B = veth 10000 Mbit/s" "wst w | grep -qE '^\+BATN: $B0_HARD .* tput=10000\.0 '"
CHECK "W's path to C = min(W->B 10000.0, B's forwarded 8823.5) = 8823.5 Mbit/s" "wst w | grep -qE '^\+BATO: $C_HARD .* tput=8823\.5 '"
WAITFOR 10 "C routes W via B" "bo_best_via c $W_HARD $B1_HARD"
WAITFOR 10 "W holds every (VID, CRC) record of B and of C" "w_tt_matches_peer w b $B1_HARD && w_tt_matches_peer w c $C_HARD"
note "C local TT:" "$(btl_vid_crcs c | oneline)" "| W's view:" "$(w_vid_crcs w $C_HARD | oneline)"
WAITFOR 5 "C holds W's soft MAC via W with W's CRC" \
    "c=\$(w_soft_crc w $W_SOFT); [ -n \"\$c\" ] && btg c | grep -E '$W_SOFT .* $W_HARD ' | grep -q \"(0x\$c)\""
R1=$(wcnt w tt_req_tx); NB=$(bstat b tt_request_tx); NC=$(bstat c tt_request_tx)
sleep 8
CHECK "TT requests quiet: W $R1 -> $(wcnt w tt_req_tx), B $NB -> $(bstat b tt_request_tx), C $NC -> $(bstat c tt_request_tx)" \
    "[ $(wcnt w tt_req_tx) = $R1 ] && [ $(bstat b tt_request_tx) = $NB ] && [ $(bstat c tt_request_tx) = $NC ]"

bla_settle b
bla_settle c
WAITFOR 25 "first W -> C ping across B" "NX w ping -c 1 -W 1 $C_IP >/dev/null"
CHECK "W -> C ping 5/5, C -> W ping 5/5" \
    "NX w ping -c 5 -i 0.2 -W 2 $C_IP | grep -q ' 0% packet loss' && NX c ping -c 5 -i 0.2 -W 2 $W_IP | grep -q ' 0% packet loss'"
CHECK "1472-byte DF pings W <-> C (W and C fragment, B relays)" \
    "NX w ping -c 3 -i 0.3 -s 1472 -M do -W 2 $C_IP | grep -q ' 0% packet loss' && NX c ping -c 3 -i 0.3 -s 1472 -M do -W 2 $W_IP | grep -q ' 0% packet loss'"
CHECK "3000-byte pings W <-> C" \
    "NX w ping -c 3 -i 0.3 -s 3000 -W 2 $C_IP | grep -q ' 0% packet loss' && NX c ping -c 3 -i 0.3 -s 3000 -W 2 $W_IP | grep -q ' 0% packet loss'"
CHECK "C's batctl ping to W across B 3/3" "NX c batctl meshif bat0 ping -c 3 $W_HARD | grep -q '3 packets transmitted, 3 received'"
TR=$(NX c batctl meshif bat0 tr $W_HARD 2>&1); echo "$TR" | sed 's/^/     | /'
CHECK "C's traceroute to W: hop 1 B (its originator), hop 2 W" "echo \"\$TR\" | grep -qE '^ *1: +$B1_HARD ' && echo \"\$TR\" | grep -qE '^ *2: +$W_HARD '"
bc_to() {           # bc_to <from-ns> <from-ip> <to-ns> <to-ip> <label>
    local tok="bvw-$5-$RANDOM" r rc
    NX "$3" $PY "$BVW_DIR/udp.py" recv bcast 4404 "$4" 8 "$tok" >"$BVW_RUN/s11.$5" 2>&1 &
    r=$!; sleep 0.8
    NX "$1" $PY "$BVW_DIR/udp.py" send 10.41.255.255 4404 "$2" "$tok" 3
    wait $r; rc=$?; sed 's/^/     | /' "$BVW_RUN/s11.$5"; return $rc
}
CHECK "IPv4 broadcast W -> C across B" "bc_to w $W_IP c $C_IP w2c"
CHECK "IPv4 broadcast C -> W across B" "bc_to c $C_IP w $W_IP c2w"

note "client X on C's LAN"
WAITFOR 20 "X pings W" "NX x ping -c 1 -W 1 $W_IP >/dev/null"
WAITFOR 10 "W's TT has X via C (announced)" "wst w | grep -qE '^\+BATTG: $X_MAC vid=-1 via=$C_HARD ttvn=[0-9]+ flags=--[^T]'"
CHECK "W -> X ping 3/3" "NX w ping -c 3 -i 0.2 -W 2 $X_IP | grep -q ' 0% packet loss'"

note "X roams from C's LAN to B's LAN"
R2=$(wcnt w tt_req_tx)
NX c ip link del bvw_cx
mk_veth x bvw_x0 "$X_MAC" b bvw_bx 02:b0:0b:00:0d:01
NX b ip link set bvw_bx master bvw_br
NX x ip addr add "$X_IP/16" dev bvw_x0
WAITFOR 30 "X (now behind B) pings W" "NX x ping -c 1 -W 1 $W_IP >/dev/null"
WAITFOR 15 "W's TT has X via B and no longer via C" \
    "wst w | grep -qE '^\+BATTG: $X_MAC vid=-1 via=$B1_HARD .*flags=--[^T]' && ! wst w | grep -qE '^\+BATTG: $X_MAC vid=-1 via=$C_HARD '"
WAITFOR 10 "W still holds every (VID, CRC) record of B and of C" "w_tt_matches_peer w b $B1_HARD && w_tt_matches_peer w c $C_HARD"
CHECK "W -> X ping 3/3 at its new home" "NX w ping -c 3 -i 0.2 -W 2 $X_IP | grep -q ' 0% packet loss'"
R3=$(wcnt w tt_req_tx)
sleep 6
CHECK "W's TT requests settle after the roam (tt_req_tx $R2 -> $R3 -> $(wcnt w tt_req_tx))" "[ $(wcnt w tt_req_tx) = $R3 ]"
note "W counters:" "$(wst w | grep -E '^\+BATSTAT: ut ' | tr ' ' '\n' | grep -E '^(tt_|ut_)' | grep -v '=0$' | oneline)"

note "W restarts as a late joiner of the established B - C mesh"
stop_w w
sleep 2
start_w w w bvw_w0 "$W_HARD" "$W_SOFT" "$W_IP/16" --soft-mtu 1500 --tput auto
WAITFOR 8 "the restarted W routes C via B" "wst w | grep -qE '^\+BATO: $C_HARD .* nh=$B0_HARD tput='"
WAITFOR 8 "the restarted W holds every (VID, CRC) of B and C (X via B included)" \
    "w_tt_matches_peer w b $B1_HARD && w_tt_matches_peer w c $C_HARD && wst w | grep -qE '^\+BATTG: $X_MAC vid=-1 via=$B1_HARD '"
wfresh
note "W:" "$(wst w | grep -E '^\+BATSTAT: ut ' | tr ' ' '\n' | grep -E '^(tt_req_tx|tt_resp_rx|tt_full|tt_diff|tt_crc_fail|tt_resp_unknown)=' | oneline)"
R4=$(wcnt w tt_req_tx); sleep 6
CHECK "the late joiner's TT requests settle ($R4 -> $(wcnt w tt_req_tx))" "[ $(wcnt w tt_req_tx) = $R4 ]"
WAITFOR 20 "the late joiner reaches X and C" "NX w ping -c 1 -W 1 $X_IP >/dev/null && NX w ping -c 1 -W 1 $C_IP >/dev/null"
snapshot end b c
cap_stop
finish
