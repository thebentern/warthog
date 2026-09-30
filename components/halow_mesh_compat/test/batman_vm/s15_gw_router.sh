#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# s15_gw_router: the two questions bat0 addressing asks the engine (bat_gw_best,
# bat_client_route), against batman-adv. A is a mesh point (gw_mode client) whose bat0 is
# a port of a bridge with its own MAC and address, serving DHCP there as an OpenMANET
# point does on br-ahwlan; L is a gate (gw_mode server, default 10000/2000 kbit/s) without
# DHCP; W, A and L share one medium. W takes a lease from A with busybox udhcpc, learns the
# router's MAC by ARP and watches it: TT resolves it to A's originator while A is heard.
# With A's hard interface out of its bat0, TT still resolves it (batman keeps the route
# 200 s) while the OGM age passes 10 s, which is what the lease watchdog acts on. L is W's
# best gateway until L turns gw_mode off; A announcing itself as a gateway too makes it two.
source "$(dirname "$0")/lib.sh"
bvw_begin
W_TAGS=w
A_BR=02:b0:0a:00:00:bb
WATCH=$BVW_RUN/s15.watch
rm -f "$WATCH"
mk_ns a l w m
NX m ip link add bvw_med type bridge
NX m ip link set bvw_med up
for x in a l w; do
    case $x in a) mac=$A_HARD ;; l) mac=$L_HARD ;; w) mac=$W_HARD ;; esac
    mk_veth $x bvw_${x}0 $mac m bvw_p$x "02:b0:ee:00:00:0$(printf %x "'$x")"
    NX m ip link set bvw_p$x master bvw_med
done
med_private bvw_pa bvw_pl bvw_pw || { echo "FAIL tc pedit"; exit 1; }
mk_batman_peer a bvw_a0 "$A_HARD" "$A_BAT" - client 1500 || exit 1
NX a ip link add bvw_br type bridge
NX a ip link set bvw_br address "$A_BR"
NX a ip link set bat0 master bvw_br
NX a ip addr add "$A_IP/16" dev bvw_br
NX a ip link set bvw_br up
NX a dnsmasq --no-daemon --conf-file=/dev/null --user=root --interface=bvw_br --bind-interfaces \
    --port=0 --dhcp-range=10.41.102.100,10.41.102.150,255.255.0.0,1h --dhcp-leasefile="$BVW_RUN/s15.leases" \
    >"$BVW_RUN/s15.dnsmasq.log" 2>&1 &
mk_batman_peer l bvw_l0 "$L_HARD" "$L_BAT" "$L_IP/16" server 1500 || exit 1
cap_start w bvw_w0 w0
start_w w w bvw_w0 "$W_HARD" "$W_SOFT" - --watch-file "$WATCH"

WAITFOR 10 "W routes A and L" "wst w | grep -qE '^\+BATO: $A_HARD .* nh=$A_HARD ' && wst w | grep -qE '^\+BATO: $L_HARD .* nh=$L_HARD '"
WAITFOR 10 "W's best gateway: L, as announced (100/20 = 10.0/2.0 Mbit/s)" \
    "wst w | grep -qE '^\+QRY: gw=$L_HARD down=100 up=20 tput=[1-9][0-9]* age=[0-9]+$'"
CHECK "one gateway counted" "wst w | grep -qx '+QRY: gws=1'"
NX a batctl meshif bat0 gw_mode server
WAITFOR 5 "A announces itself as a gateway too: two counted (bat0 addressing's rise)" "wst w | grep -qx '+QRY: gws=2'"
NX a batctl meshif bat0 gw_mode client
WAITFOR 5 "A back to client: one again, L the best" \
    "wst w | grep -qx '+QRY: gws=1' && wst w | grep -qE '^\+QRY: gw=$L_HARD down=100 up=20 '"
bla_settle a
cat >"$BVW_RUN/s15.udhcpc.sh" <<'EOF'
#!/bin/sh
[ "$1" = bound ] && env | grep -E '^(ip|subnet|router|serverid)=' | sort >"$LEASEF"
exit 0
EOF
chmod +x "$BVW_RUN/s15.udhcpc.sh"
LEASEF=$BVW_RUN/s15.lease.env
rm -f "$LEASEF"
for try in 1 2 3; do
    NX w env LEASEF="$LEASEF" timeout 20 busybox udhcpc -i bvw_wt -f -q -n -t 5 -T 2 -s "$BVW_RUN/s15.udhcpc.sh" 2>&1 | sed 's/^/     | /'
    [ -s "$LEASEF" ] && break
done
sed 's/^/     | lease: /' "$LEASEF" 2>/dev/null
LIP=$(sed -n 's/^ip=//p' "$LEASEF" 2>/dev/null)
RTR=$(sed -n 's/^router=//p' "$LEASEF" 2>/dev/null)
CHECK "a lease from A's pool, router = A's bridge address $A_IP (got ip=$LIP router=$RTR)" \
    "echo '$LIP' | grep -q '^10\.41\.102\.' && [ '$RTR' = '$A_IP' ]"
NX w ip addr add "$LIP/16" dev bvw_wt 2>/dev/null
WAITFOR 10 "W reaches its router" "NX w ping -c 1 -W 1 $A_IP >/dev/null"
RMAC=$(NX w ip neigh show "$A_IP" dev bvw_wt | awk '/lladdr/ {print $3}')
CHECK "the router's MAC by ARP is A's bridge MAC $A_BR (got $RMAC)" "[ '$RMAC' = '$A_BR' ]"
WAITFOR 10 "once A's bridge has sent through bat0, W's TT has its MAC untagged via A" \
    "wst w | grep -qE '^\+BATTG: $A_BR vid=-1 via=$A_HARD '"
note "$(wst w | grep "^+BATTG: $A_BR " | tr '\n' ' ')"
echo "$RMAC" >"$WATCH"
WAITFOR 5 "the router's MAC resolves to A's originator, last OGM under 10 s old" \
    "wst w | grep -qE '^\+QRY: watch=$A_BR routed=1 orig=$A_HARD tput=[1-9][0-9]* age=[0-9]{1,4}$'"
note "$(wst w | grep '^+QRY:' | tr '\n' ' ')"
echo 02:b0:99:00:00:01 >"$WATCH"
WAITFOR 3 "an unknown MAC does not resolve" "wst w | grep -qE '^\+QRY: watch=02:b0:99:00:00:01 routed=0 orig=00:00:00:00:00:00 tput=0 age=0$'"
echo "$RMAC" >"$WATCH"
WAITFOR 3 "watching the router again" "wst w | grep -qE '^\+QRY: watch=$A_BR routed=1 '"

note "A's hard interface leaves its bat0: A stops sending ELP and OGMs"
NX a batctl meshif bat0 if del bvw_a0
T0=$(now_ms)
WAITFOR 20 "A silent: its OGMs age past 10 s" "wst w | grep -qE '^\+QRY: watch=$A_BR routed=1 orig=$A_HARD tput=[0-9]+ age=[1-9][0-9]{4,}$'"
CHECK "while TT still resolves the router to A, with a route (batman keeps it 200 s)" \
    "wst w | grep -qE '^\+BATO: $A_HARD .* nh=$A_HARD tput=[0-9]' && [ \$(( \$(now_ms) - T0 )) -lt 60000 ]"
CHECK "the gateway is still L" "wst w | grep -qE '^\+QRY: gw=$L_HARD down=100 up=20 '"

note "L turns gw_mode off"
NX l batctl meshif bat0 gw_mode off
WAITFOR 5 "L's next OGM carries no GW TVLV: no gateway" "wst w | grep -qE '^\+QRY: gw=none$' && wst w | grep -qx '+QRY: gws=0'"
cap_stop
snapshot end a l
finish
