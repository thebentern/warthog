#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# s06_relay_awc: A - W - C on one shared medium (Linux bridge bvw_med) where ebtables
# drops everything between A's and C's ports, so A and C reach each other only through
# the engine: OGM forwarding, TT across W, unicast/ICMP relay, BCAST re-flood (IPv4
# broadcast and Meshtastic multicast), and transit fragments forwarded unchanged.
source "$(dirname "$0")/lib.sh"
bvw_begin
W_TAGS=w
mk_ns a c w m
NX m ip link add bvw_med type bridge
NX m ip link set bvw_med up
for x in a c w; do
    case $x in a) mac=$A_HARD ;; c) mac=$C_HARD ;; w) mac=$W_HARD ;; esac
    mk_veth $x bvw_${x}0 $mac m bvw_p$x "02:b0:ee:00:00:0$(printf %x "'$x")"
    NX m ip link set bvw_p$x master bvw_med
done
med_private bvw_pa bvw_pc bvw_pw || { echo "FAIL tc pedit"; exit 1; }
NX m ebtables -A FORWARD -i bvw_pa -o bvw_pc -j DROP || { echo "FAIL ebtables"; exit 1; }
NX m ebtables -A FORWARD -i bvw_pc -o bvw_pa -j DROP
NX m ebtables -L FORWARD | sed 's/^/     | /'
mk_batman_peer a bvw_a0 "$A_HARD" "$A_BAT" "$A_IP/16" client 1500 || exit 1
mk_batman_peer c bvw_c0 "$C_HARD" "$C_BAT" "$C_IP/16" server 1500 || exit 1
NX c dnsmasq --no-daemon --conf-file=/dev/null --user=root --interface=bat0 --bind-interfaces \
    --port=0 --dhcp-range=10.41.101.100,10.41.101.150,255.255.0.0,1h --dhcp-leasefile="$BVW_RUN/s06.leases" \
    >"$BVW_RUN/s06.dnsmasq.log" 2>&1 &
cap_start w bvw_w0 w0
start_w w w bvw_w0 "$W_HARD" "$W_SOFT" "$W_IP/16"

WAITFOR 10 "A batctl o routes C via W" "bo_best_via a $C_HARD $W_HARD"
WAITFOR 10 "C batctl o routes A via W" "bo_best_via c $A_HARD $W_HARD"
CHECK "A's path to C carries W's forwarded throughput 5.0 (min(10.0, 10.0/2), 802.11s half duplex)" "[ '$(bo_tput a $C_HARD)' = 5.0 ]"
WAITFOR 3 "W relays: its interface table routes A and C (J), and it forwards OGMs" \
    "wst w | grep -qE '^\+BATO: $A_HARD .* iftput=[0-9]' && wst w | grep -qE '^\+BATO: $C_HARD .* iftput=[0-9]' && [ \$(wcnt w ogm_tx_fwd) -gt 0 ]"
WAITFOR 10 "A's global TT has C's bat0 MAC via C with C's CRCs" \
    "btg a | grep -qE '^ *\* +$C_BAT +-1 .* $C_HARD ' && [ \"\$(btg a | sed -nE 's/^ *\* +[0-9a-f:]{17} +(-?[0-9]+) .* $C_HARD +\( *[0-9]+\) +\(0x([0-9a-f]+)\).*/\1 \2/p' | sort -u)\" = \"\$(btl_vid_crcs c)\" ]"
WAITFOR 10 "C's global TT has A's bat0 MAC via A with A's CRCs" \
    "btg c | grep -qE '^ *\* +$A_BAT +-1 .* $A_HARD ' && [ \"\$(btg c | sed -nE 's/^ *\* +[0-9a-f:]{17} +(-?[0-9]+) .* $A_HARD +\( *[0-9]+\) +\(0x([0-9a-f]+)\).*/\1 \2/p' | sort -u)\" = \"\$(btl_vid_crcs a)\" ]"
WAITFOR 5 "A and C both hold W's soft MAC via W" "btg a | grep -qE '$W_SOFT .* $W_HARD ' && btg c | grep -qE '$W_SOFT .* $W_HARD '"
CHECK "A and C are not neighbours (ebtables holds)" "! bn a | grep -q $C_HARD && ! bn c | grep -q $A_HARD"

bla_settle a
bla_settle c
U0=$(wcnt w uc_fwd)
WAITFOR 25 "first A -> C ping across W" "NX a ping -c 1 -W 1 $C_IP >/dev/null"
CHECK "A -> C ping 5/5" "NX a ping -c 5 -i 0.2 -W 2 $C_IP | grep -q ' 0% packet loss'"
CHECK "C -> A ping 5/5" "NX c ping -c 5 -i 0.2 -W 2 $A_IP | grep -q ' 0% packet loss'"
wfresh
CHECK "W relayed the unicasts (uc_fwd $U0 -> $(wcnt w uc_fwd))" "[ $(wcnt w uc_fwd) -ge $((U0 + 20)) ]"
TR=$(NX a batctl meshif bat0 tr $C_HARD 2>&1)
echo "$TR" | sed 's/^/     | /'
CHECK "A's batctl traceroute to C: hop 1 W, hop 2 C" \
    "echo \"\$TR\" | grep -qE '^ *1: +$W_HARD ' && echo \"\$TR\" | grep -qE '^ *2: +$C_HARD '"
I0=$(wcnt w ic_fwd)
CHECK "A's batctl ping to C across W 3/3" "NX a batctl meshif bat0 ping -c 3 $C_HARD | grep -q '3 packets transmitted, 3 received'"
wfresh
CHECK "W relayed the batman ICMP both ways (ic_fwd $I0 -> $(wcnt w ic_fwd))" "[ $(wcnt w ic_fwd) -ge $((I0 + 6)) ]"

udp_to() {          # udp_to <dst> <port> <from-ns> <from-ip> <label> <ns:ip>...
    local dst=$1 port=$2 fns=$3 fip=$4 lab=$5 tok="bvw-$5-$RANDOM" pids="" x rc=0 grp
    shift 5
    grp=$dst; [ "${dst##*.}" = 255 ] && grp=bcast
    for x in "$@"; do
        ip netns exec "bvw_${x%%:*}" $PY "$BVW_DIR/udp.py" recv "$grp" "$port" "${x#*:}" 8 "$tok" \
            >"$BVW_RUN/s06.$lab.${x%%:*}" 2>&1 &
        pids="$pids $!"
    done
    sleep 0.8
    NX "$fns" $PY "$BVW_DIR/udp.py" send "$dst" "$port" "$fip" "$tok" 3
    for x in $pids; do wait "$x" || rc=1; done
    for x in "$@"; do sed "s/^/     | ${x%%:*}: /" "$BVW_RUN/s06.$lab.${x%%:*}"; done
    return $rc
}
B0=$(wcnt w bc_fwd)
CHECK "IPv4 broadcast A -> 10.41.255.255 reaches C and W" "udp_to 10.41.255.255 4404 a $A_IP a2c c:$C_IP w:$W_IP"
CHECK "IPv4 broadcast C -> 10.41.255.255 reaches A and W" "udp_to 10.41.255.255 4404 c $C_IP c2a a:$A_IP w:$W_IP"
CHECK "Meshtastic 239.0.0.69:4403 A -> C and W" "udp_to 239.0.0.69 4403 a $A_IP m_a2c c:$C_IP w:$W_IP"
CHECK "Meshtastic 239.0.0.69:4403 W -> A and C" "udp_to 239.0.0.69 4403 w $W_IP m_w2ac a:$A_IP c:$C_IP"
wfresh
CHECK "W re-flooded the broadcasts (bc_fwd $B0 -> $(wcnt w bc_fwd))" "[ $(wcnt w bc_fwd) -ge $((B0 + 9)) ]"

F0=$(wcnt w fr_fwd); D0=$(wcnt w fr_done)
CHECK "1472-byte DF ping A -> C (A and C fragment, W relays) 3/3" "NX a ping -c 3 -i 0.3 -s 1472 -M do -W 2 $C_IP | grep -q ' 0% packet loss'"
CHECK "3000-byte ping A -> C 3/3 and C -> A 3/3 across W" \
    "NX a ping -c 3 -i 0.3 -s 3000 -W 2 $C_IP | grep -q ' 0% packet loss' && NX c ping -c 3 -i 0.3 -s 3000 -W 2 $A_IP | grep -q ' 0% packet loss'"
wfresh
CHECK "W forwarded fragments without reassembling (fr_fwd $F0 -> $(wcnt w fr_fwd), fr_done $D0 -> $(wcnt w fr_done))" \
    "[ $(wcnt w fr_fwd) -ge $((F0 + 36)) ] && [ $(wcnt w fr_done) = $D0 ]"
WAITFOR 5 "A (gw client) selected C as its gateway through W's forwarded GW TVLV" "NX a batctl meshif bat0 gwl -H 2>/dev/null | grep -qE '^ *\* +$C_HARD '"
F0=$(wcnt w uc_fwd)
OUT=$(NX a timeout 25 busybox udhcpc -i bat0 -f -q -n -t 5 -T 2 -s /bin/true 2>&1); echo "$OUT" | sed 's/^/     | /'
CHECK "A's DHCP (gateway-client 4ADDR unicast to C) crossed W and got C's lease" "echo \"\$OUT\" | grep -qE 'lease of 10\.41\.101\.[0-9]+ obtained'"
CHECK "W pings A and C (own traffic both routes)" "NX w ping -c 2 -W 2 $A_IP >/dev/null && NX w ping -c 2 -W 2 $C_IP >/dev/null"
snapshot end a c
cap_stop
QA=$($PY "$BVW_DIR/pcapq.py" fragfwd "$BVW_EVID/$SCEN-w0.pcap" --in-src "$A_HARD" --out-src "$W_HARD")
QC=$($PY "$BVW_DIR/pcapq.py" fragfwd "$BVW_EVID/$SCEN-w0.pcap" --in-src "$C_HARD" --out-src "$W_HARD")
CHECK "on the wire: every fragment from A left W byte-identical except TTL-1 ($QA)" \
    "[ '${QA#in=}' != '0 matched=0' ] && [ \"\$(echo '$QA' | sed -E 's/in=([0-9]+) matched=([0-9]+)/\1/')\" = \"\$(echo '$QA' | sed -E 's/in=([0-9]+) matched=([0-9]+)/\2/')\" ]"
Q4=$($PY "$BVW_DIR/pcapq.py" count "$BVW_EVID/$SCEN-w0.pcap" --src "$W_HARD" --dst "$C_HARD" --type 0x42)
CHECK "on the wire: W relayed A's UNICAST_4ADDR DHCP toward C ($Q4)" "[ \$(echo '$Q4' | sed -E 's/count=([0-9]+).*/\1/') -ge 2 ]"
CHECK "on the wire: every fragment from C left W byte-identical except TTL-1 ($QC)" \
    "[ '${QC#in=}' != '0 matched=0' ] && [ \"\$(echo '$QC' | sed -E 's/in=([0-9]+) matched=([0-9]+)/\1/')\" = \"\$(echo '$QC' | sed -E 's/in=([0-9]+) matched=([0-9]+)/\2/')\" ]"
finish
