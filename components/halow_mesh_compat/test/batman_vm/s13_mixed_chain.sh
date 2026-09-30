#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# s13_mixed_chain: L1 - W1 - W2 - L2 on one shared medium (bridge bvw_med, ebtables lets
# only neighbours in the chain hear each other). Two engines relay for each other and
# for batman-adv: 3-hop routes with the half-duplex penalty applied twice, TT across two
# engines, unicast/ICMP/broadcast/fragments end to end, engine-to-engine traffic.
# W1 runs the 802.11s defaults (half duplex, aggregation); W2 runs --wired --no-aggr, so
# both forwarding penalties and the unaggregated OGM path meet real batman-adv.
source "$(dirname "$0")/lib.sh"
bvw_begin
W_TAGS="w1 w2"
W1_HARD=02:b0:77:00:00:01; W1_SOFT=06:b0:77:00:00:01; W1_IP=10.41.253.2
W2_HARD=02:b0:78:00:00:01; W2_SOFT=06:b0:78:00:00:01; W2_IP=10.41.253.3
L1_HARD=02:b0:1c:00:00:01; L1_BAT=02:b0:1c:00:00:ff; L1_IP=10.41.0.1
L2_HARD=02:b0:2c:00:00:01; L2_BAT=02:b0:2c:00:00:ff; L2_IP=10.41.0.2
mk_ns l1 w1 w2 l2 m
NX m ip link add bvw_med type bridge
NX m ip link set bvw_med up
i=1
for x in l1 w1 w2 l2; do
    case $x in l1) mac=$L1_HARD ;; w1) mac=$W1_HARD ;; w2) mac=$W2_HARD ;; l2) mac=$L2_HARD ;; esac
    mk_veth $x bvw_${x}h $mac m bvw_p$x 02:b0:ee:00:01:0$i
    NX m ip link set bvw_p$x master bvw_med
    i=$((i + 1))
done
med_private bvw_pl1 bvw_pw1 bvw_pw2 bvw_pl2 || { echo "FAIL tc pedit"; exit 1; }
for pr in "l1 w2" "l1 l2" "w1 l2"; do
    set -- $pr
    NX m ebtables -A FORWARD -i bvw_p$1 -o bvw_p$2 -j DROP
    NX m ebtables -A FORWARD -i bvw_p$2 -o bvw_p$1 -j DROP
done
mk_batman_peer l1 bvw_l1h "$L1_HARD" "$L1_BAT" "$L1_IP/16" client 1500 || exit 1
mk_batman_peer l2 bvw_l2h "$L2_HARD" "$L2_BAT" "$L2_IP/16" client 1500 || exit 1
cap_start w1 bvw_w1h w1
cap_start w2 bvw_w2h w2
start_w w1 w1 bvw_w1h "$W1_HARD" "$W1_SOFT" "$W1_IP/16" --soft-mtu 1500
start_w w2 w2 bvw_w2h "$W2_HARD" "$W2_SOFT" "$W2_IP/16" --soft-mtu 1500 --wired --no-aggr

WAITFOR 12 "L1 routes L2 via W1" "bo_best_via l1 $L2_HARD $W1_HARD"
WAITFOR 12 "L2 routes L1 via W2" "bo_best_via l2 $L1_HARD $W2_HARD"
WAITFOR 3 "L1's path to L2 = 4.4 Mbit/s (W2 wired: 10.0 x 225/255 = 8.8; W1 half duplex: 8.8 / 2)" "[ \"\$(bo_tput l1 $L2_HARD)\" = 4.4 ]"
WAITFOR 3 "L2's path to L1 = 4.4 Mbit/s (W1 half duplex: 5.0; W2 wired: 5.0 x 225/255)" "[ \"\$(bo_tput l2 $L1_HARD)\" = 4.4 ]"
WAITFOR 5 "W1 routes L2 via W2 and W2 routes L1 via W1" \
    "wst w1 | grep -qE '^\+BATO: $L2_HARD .* nh=$W2_HARD ' && wst w2 | grep -qE '^\+BATO: $L1_HARD .* nh=$W1_HARD '"
WAITFOR 10 "both engines hold every (VID, CRC) of L1 and L2" \
    "w_tt_matches_peer w1 l1 $L1_HARD && w_tt_matches_peer w1 l2 $L2_HARD && w_tt_matches_peer w2 l1 $L1_HARD && w_tt_matches_peer w2 l2 $L2_HARD"
WAITFOR 5 "L1 and L2 hold both engines' soft MACs" \
    "btg l1 | grep -q $W1_SOFT && btg l1 | grep -q $W2_SOFT && btg l2 | grep -q $W1_SOFT && btg l2 | grep -q $W2_SOFT"
CHECK "the ebtables chain holds (L1 hears only W1, L2 only W2)" \
    "[ \$(bn l1 | grep -c ':') = 1 ] && bn l1 | grep -q $W1_HARD && [ \$(bn l2 | grep -c ':') = 1 ] && bn l2 | grep -q $W2_HARD"
bla_settle l1
bla_settle l2
WAITFOR 25 "first L1 -> L2 ping across W1 and W2" "NX l1 ping -c 1 -W 1 $L2_IP >/dev/null"
CHECK "L1 <-> L2 ping 5/5 each way" \
    "NX l1 ping -c 5 -i 0.2 -W 2 $L2_IP | grep -q ' 0% packet loss' && NX l2 ping -c 5 -i 0.2 -W 2 $L1_IP | grep -q ' 0% packet loss'"
TR=$(NX l1 batctl meshif bat0 tr $L2_HARD 2>&1); echo "$TR" | sed 's/^/     | /'
CHECK "L1's traceroute to L2: W1, W2, L2" \
    "echo \"\$TR\" | grep -qE '^ *1: +$W1_HARD ' && echo \"\$TR\" | grep -qE '^ *2: +$W2_HARD ' && echo \"\$TR\" | grep -qE '^ *3: +$L2_HARD '"
CHECK "L1's batctl ping to L2 3/3" "NX l1 batctl meshif bat0 ping -c 3 $L2_HARD | grep -q '3 packets transmitted, 3 received'"
CHECK "1472-byte DF and 3000-byte pings L1 -> L2" \
    "NX l1 ping -c 3 -i 0.3 -s 1472 -M do -W 2 $L2_IP | grep -q ' 0% packet loss' && NX l1 ping -c 3 -i 0.3 -s 3000 -W 2 $L2_IP | grep -q ' 0% packet loss'"
CHECK "engine to engine: W1 <-> W2 ping, W1 -> L2 and W2 -> L1 ping" \
    "NX w1 ping -c 3 -i 0.2 -W 2 $W2_IP | grep -q ' 0% packet loss' && NX w2 ping -c 3 -i 0.2 -W 2 $W1_IP | grep -q ' 0% packet loss' && NX w1 ping -c 3 -i 0.2 -W 2 $L2_IP | grep -q ' 0% packet loss' && NX w2 ping -c 3 -i 0.2 -W 2 $L1_IP | grep -q ' 0% packet loss'"
CHECK "1472-byte DF ping W1 -> L2 (W1 fragments, W2 relays the fragments)" "NX w1 ping -c 3 -i 0.3 -s 1472 -M do -W 2 $L2_IP | grep -q ' 0% packet loss'"
mcast3() {
    local tok="bvw-chain-$RANDOM" r1 r2 rc=0
    ip netns exec bvw_l2 $PY "$BVW_DIR/udp.py" recv 239.0.0.69 4403 "$L2_IP" 8 "$tok" >"$BVW_RUN/s13.l2" 2>&1 & r1=$!
    ip netns exec bvw_w2 $PY "$BVW_DIR/udp.py" recv 239.0.0.69 4403 "$W2_IP" 8 "$tok" >"$BVW_RUN/s13.w2" 2>&1 & r2=$!
    sleep 0.8
    NX l1 $PY "$BVW_DIR/udp.py" send 239.0.0.69 4403 "$L1_IP" "$tok" 3
    wait $r1 || rc=1; wait $r2 || rc=1
    sed 's/^/     | l2: /' "$BVW_RUN/s13.l2"; sed 's/^/     | w2: /' "$BVW_RUN/s13.w2"
    return $rc
}
CHECK "Meshtastic 239.0.0.69:4403 from L1 reaches W2 and L2 (re-flooded by W1 and W2)" "mcast3"
wfresh
CHECK "both engines relayed unicast, broadcast and fragments (W1 uc_fwd $(wcnt w1 uc_fwd) bc_fwd $(wcnt w1 bc_fwd) fr_fwd $(wcnt w1 fr_fwd); W2 uc_fwd $(wcnt w2 uc_fwd) bc_fwd $(wcnt w2 bc_fwd) fr_fwd $(wcnt w2 fr_fwd))" \
    "[ $(wcnt w1 uc_fwd) -gt 10 ] && [ $(wcnt w2 uc_fwd) -gt 10 ] && [ $(wcnt w1 bc_fwd) -gt 0 ] && [ $(wcnt w2 bc_fwd) -gt 0 ] && [ $(wcnt w1 fr_fwd) -gt 0 ] && [ $(wcnt w2 fr_fwd) -gt 0 ]"
R1=$(( $(wcnt w1 tt_req_tx) + $(wcnt w2 tt_req_tx) )); N1=$(( $(bstat l1 tt_request_tx) + $(bstat l2 tt_request_tx) ))
sleep 6
CHECK "TT requests quiet everywhere (engines $R1 -> $(( $(wcnt w1 tt_req_tx) + $(wcnt w2 tt_req_tx) )), Linux $N1 -> $(( $(bstat l1 tt_request_tx) + $(bstat l2 tt_request_tx) )))" \
    "[ $(( $(wcnt w1 tt_req_tx) + $(wcnt w2 tt_req_tx) )) = $R1 ] && [ $(( $(bstat l1 tt_request_tx) + $(bstat l2 tt_request_tx) )) = $N1 ]"
CHECK "W2 (--no-aggr) sent one frame per OGM (ogm_tx_frames $(wcnt w2 ogm_tx_frames) = own $(wcnt w2 ogm_tx_own) + forwarded $(wcnt w2 ogm_tx_fwd)); W1 aggregated ($(wcnt w1 ogm_tx_frames) frames for $(( $(wcnt w1 ogm_tx_own) + $(wcnt w1 ogm_tx_fwd) )) OGMs)" \
    "[ $(wcnt w2 ogm_tx_frames) = $(( $(wcnt w2 ogm_tx_own) + $(wcnt w2 ogm_tx_fwd) )) ] && [ $(wcnt w1 ogm_tx_frames) -lt $(( $(wcnt w1 ogm_tx_own) + $(wcnt w1 ogm_tx_fwd) )) ]"
snapshot end l1 l2
cap_stop
# Broadcast TTL per hop, from each engine's own transmissions (dataplane §5.2): L1's leave
# W1 with 48 and W2 with 47; each engine re-floods the other's own broadcasts with 48, also
# where a batman-adv peer received the same flooded frame (lib.sh med_private).
T1=$($PY "$BVW_DIR/pcapq.py" bcttl "$BVW_EVID/$SCEN-w1.pcap" --src "$W1_HARD" --orig "$L1_HARD")
T2=$($PY "$BVW_DIR/pcapq.py" bcttl "$BVW_EVID/$SCEN-w2.pcap" --src "$W2_HARD" --orig "$L1_HARD")
T3=$($PY "$BVW_DIR/pcapq.py" bcttl "$BVW_EVID/$SCEN-w2.pcap" --src "$W2_HARD" --orig "$W1_HARD")
T4=$($PY "$BVW_DIR/pcapq.py" bcttl "$BVW_EVID/$SCEN-w1.pcap" --src "$W1_HARD" --orig "$W2_HARD")
CHECK "broadcast TTLs: L1's leave W1 with 48 ($T1) and W2 with 47 ($T2); W2 re-floods W1's with 48 ($T3), W1 W2's with 48 ($T4)" \
    "echo '$T1' | grep -qE '^count=[1-9][0-9]* ttls=48\$' && echo '$T2' | grep -qE '^count=[1-9][0-9]* ttls=47\$' && \
     echo '$T3' | grep -qE '^count=[1-9][0-9]* ttls=48\$' && echo '$T4' | grep -qE '^count=[1-9][0-9]* ttls=48\$'"
finish
