#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# s03_dhcp: L is a gateway (gw_mode server) running dnsmasq on bat0, as an OpenMANET
# mesh point does on br-ahwlan. W's soft interface (no address) gets a lease with
# busybox udhcpc: DISCOVER/REQUEST leave W as BCAST, OFFER/ACK come back as UNICAST
# with a broadcast client destination (membership 6.2) and are delivered.
source "$(dirname "$0")/lib.sh"
bvw_begin
W_TAGS=w
mk_ns l w
mk_veth l bvw_l0 "$L_HARD" w bvw_w0 "$W_HARD"
mk_batman_peer l bvw_l0 "$L_HARD" "$L_BAT" "$L_IP/16" server || exit 1
LEASES=$BVW_RUN/s03.leases
rm -f "$LEASES"
NX l dnsmasq --no-daemon --conf-file=/dev/null --user=root --interface=bat0 --bind-interfaces \
    --port=0 --dhcp-range=10.41.100.100,10.41.100.150,255.255.0.0,1h --dhcp-leasefile="$LEASES" \
    >"$BVW_RUN/s03.dnsmasq.log" 2>&1 &
cap_start w bvw_w0 w0
start_w w w bvw_w0 "$W_HARD" "$W_SOFT" -
WAITFOR 6 "L routes W" "bo_best_via l $W_HARD $W_HARD"
CHECK "L announces itself as a gateway (W parsed L's GW TVLV)" "wst w | grep -qE '^\+BATO: $L_HARD .* gw=[1-9]'"
bla_settle l
B0=$(wcnt w bc_tx_own); U0=$(wcnt w uc_deliver)
OUT=""
for try in 1 2 3; do
    OUT=$(NX w timeout 20 busybox udhcpc -i bvw_wt -f -q -n -t 5 -T 2 -s /bin/true 2>&1)
    echo "$OUT" | sed 's/^/     | /'
    echo "$OUT" | grep -q 'obtained' && break
done
CHECK "udhcpc on W's soft interface obtained a lease from L's dnsmasq" "echo \"\$OUT\" | grep -qE 'lease of 10\.41\.100\.[0-9]+ obtained'"
wfresh
CHECK "L's lease file names W's soft MAC" "grep -qi '$W_SOFT' '$LEASES'"
CHECK "W sent DISCOVER/REQUEST as BCAST (bc_tx_own $B0 -> $(wcnt w bc_tx_own))" "[ $(wcnt w bc_tx_own) -ge $((B0 + 2)) ]"
CHECK "W delivered OFFER/ACK received as UNICAST (uc_deliver $U0 -> $(wcnt w uc_deliver))" "[ $(wcnt w uc_deliver) -ge $((U0 + 2)) ]"
cap_stop
Q=$($PY "$BVW_DIR/pcapq.py" dhcpuc "$BVW_EVID/$SCEN-w0.pcap" --src "$L_HARD")
CHECK "on the wire: L -> W UNICAST frames carrying DHCP server->client ($Q)" "[ '${Q#count=}' -ge 2 ]"
snapshot end l
finish
