#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# s02_ping: IPv4 ping both ways between L's bat0 and W's soft interface (TAP); batctl
# ping (plain, record route, of W's soft MAC) and batctl traceroute to W.
source "$(dirname "$0")/lib.sh"
bvw_begin
W_TAGS=w
mk_pair client 1460 || exit 1
WAITFOR 6 "L routes W" "bo_best_via l $W_HARD $W_HARD"
bla_settle l
WAITFOR 20 "first L -> W ping answered (ARP via BCAST, reply via UNICAST)" "NX l ping -c 1 -W 1 $W_IP >/dev/null"
U0=$(wcnt w uc_deliver); S0=$(wcnt w st_unicast)
CHECK "L -> W ping 5/5" "NX l ping -c 5 -i 0.2 -W 2 $W_IP | grep -q ' 0% packet loss'"
CHECK "W -> L ping 5/5" "NX w ping -c 5 -i 0.2 -W 2 $L_IP | grep -q ' 0% packet loss'"
CHECK "3000-byte ping L -> W 3/3 (IP fragments within the 1460 soft MTU)" "NX l ping -c 3 -i 0.3 -s 3000 -W 2 $W_IP | grep -q ' 0% packet loss'"
CHECK "3000-byte ping W -> L 3/3" "NX w ping -c 3 -i 0.3 -s 3000 -W 2 $L_IP | grep -q ' 0% packet loss'"
CHECK "L's ARP entry for W's IP is W's soft MAC" "NX l ip neigh show $W_IP | grep -q $W_SOFT"
CHECK "W's ARP entry for L's IP is L's bat0 MAC" "NX w ip neigh show $L_IP | grep -q $L_BAT"
wfresh
CHECK "W delivered unicast (uc_deliver $U0 -> $(wcnt w uc_deliver)) and sent unicast (st_unicast $S0 -> $(wcnt w st_unicast))" \
    "[ $(wcnt w uc_deliver) -ge $((U0 + 10)) ] && [ $(wcnt w st_unicast) -ge $((S0 + 10)) ]"
I0=$(wcnt w ic_reply)
CHECK "batctl ping -c 3 <W originator> answered 3/3" \
    "NX l batctl meshif bat0 ping -c 3 $W_HARD | grep -qE '3 packets transmitted, 3 received'"
CHECK "batctl ping -c 2 <W soft MAC> (resolved through L's TT) answered" \
    "NX l batctl meshif bat0 ping -c 2 $W_SOFT | grep -qE '2 packets transmitted, 2 received'"
RR=$(NX l batctl meshif bat0 ping -c 1 -R $W_HARD 2>&1)
echo "$RR" | sed 's/^/     | /'
CHECK "batctl ping -R records W on the route" "echo \"\$RR\" | grep -q '1 received' && echo \"\$RR\" | grep -q $W_HARD"
TR=$(NX l batctl meshif bat0 tr $W_HARD 2>&1)
echo "$TR" | sed 's/^/     | /'
CHECK "batctl traceroute ends at W" "echo \"\$TR\" | tail -1 | grep -qE '^ *1: +$W_HARD'"
wfresh
CHECK "W answered every batctl echo (ic_reply $I0 -> $(wcnt w ic_reply))" "[ $(wcnt w ic_reply) -ge $((I0 + 7)) ]"
snapshot end l
cap_stop
finish
