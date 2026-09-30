#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# s09_80211s_shapes: which 802.11s frame shapes carry W's batman broadcasts into a Linux
# batman-adv over mac80211 (OpenMANET knobs, mesh_fwding=0). mac80211_hwsim radios=2:
# L = batman-adv on mesh point bvw_m0; the injector station (namespace hw) is a peered
# mesh point bvw_m1 with W's MAC plus a monitor interface bvw_mon.
#  A. Replay of W's own ELP and first OGM2 bytes (captured from batvm_node on a veth)
#     in the three shapes of design 4.3: DA-rewritten replica -> no neighbour; standard
#     group frame and AE-2 replica -> neighbour; AE-2 OGM2 -> originator + TT with W's CRC
#     and no TT request.
#  B. Live: batvm_node receives on bvw_m1 and transmits through bvw_mon as AE-2 replicas
#     (AT+MESHGRP=0) and 4-address unicast: neighbour/originator/TT both ways, Linux's
#     unicast ELP probes dropped, pings and batctl ping over 802.11s.
#  C. Live again with standard group frames and 3 BCAST copies (AT+MESHGRP=1), including
#     three broadcasts queued at once: every group frame W sends keeps 5 ms from the last.
# Skipped when mac80211_hwsim is already loaded by someone else; unloaded only if loaded here.
source "$(dirname "$0")/lib.sh"
bvw_modules
if lsmod | grep -q '^mac80211_hwsim'; then
    SKIP "mac80211_hwsim is already loaded by someone else"
    finish
    exit 0
fi
HWSIM_MINE=0
s09_exit() {
    bvw_cleanup
    if [ "$HWSIM_MINE" = 1 ]; then
        local i
        for i in $(seq 1 20); do modprobe -r mac80211_hwsim 2>/dev/null && break; sleep 0.5; done
    fi
}
bvw_cleanup
trap 's09_exit' EXIT
echo "==== $SCEN  (batman-adv $(cat /sys/module/batman_adv/version), $(uname -r))"
W_TAGS=w

note "part 0: W's own ELP and first OGM2, captured from batvm_node on a veth"
ELP=""; OGM=""; WCRC=""
for try in 1 2 3; do
    mk_ns xl xw
    mk_veth xl bvw_xl0 "$L_HARD" xw bvw_xw0 "$W_HARD"
    mk_batman_peer xl bvw_xl0 "$L_HARD" "$L_BAT" - client >/dev/null
    cap_start xw bvw_xw0 "wframes$try"
    start_w xw x bvw_xw0 "$W_HARD" "$W_SOFT" -
    for i in $(seq 1 30); do bo_best_via xl $W_HARD $W_HARD && break; sleep 0.2; done
    sleep 1.2
    cap_stop
    ELP=$($PY "$BVW_DIR/pcapq.py" extract "$BVW_EVID/$SCEN-wframes$try.pcap" --src "$W_HARD" --type 0x03)
    OGM=$($PY "$BVW_DIR/pcapq.py" extract "$BVW_EVID/$SCEN-wframes$try.pcap" --src "$W_HARD" --type 0x04 --tt-change)
    WCRC=$(w_soft_crc x $W_SOFT)
    stop_w x
    ip netns del bvw_xl; ip netns del bvw_xw
    [ -n "$OGM" ] && [ -n "$ELP" ] && break
done
echo "     | ELP  $ELP"
echo "     | OGM2 $OGM"
CHECK "captured W's 20-byte ELP and its first own OGM2 carrying the TT ADD (W CRC 0x$WCRC)" \
    "[ \${#ELP} = 40 ] && [ -n '$OGM' ] && [ -n '$WCRC' ]"

modprobe mac80211_hwsim radios=2 || { echo "FAIL modprobe mac80211_hwsim"; exit 1; }
HWSIM_MINE=1
sleep 1
PHYS=$(for p in /sys/class/ieee80211/*; do case $(readlink -f "$p/device") in *hwsim*) basename "$p" ;; esac; done)
set -- $PHYS
PL=$1; PW=$2
CHECK "two hwsim radios ($PL $PW)" "[ -n '$PL' ] && [ -n '$PW' ]"
mk_ns hl hw
iw phy "$PL" set netns name bvw_hl
iw phy "$PW" set netns name bvw_hw
NX hl iw phy "$PL" interface add bvw_m0 type mp
NX hw iw phy "$PW" interface add bvw_m1 type mp
NX hw iw phy "$PW" interface add bvw_mon type monitor
NX hl ip link set bvw_m0 address "$L_HARD"
NX hw ip link set bvw_m1 address "$W_HARD"
NX hl sysctl -qw net.ipv6.conf.bvw_m0.disable_ipv6=1
NX hw sysctl -qw net.ipv6.conf.bvw_m1.disable_ipv6=1
NX hl ip link set bvw_m0 up
NX hw ip link set bvw_m1 up
NX hw ip link set bvw_mon up
NX hl iw dev bvw_m0 mesh join bvwmesh freq 2412 HT20 mesh_fwding=0
NX hw iw dev bvw_m1 mesh join bvwmesh freq 2412 HT20 mesh_fwding=0
WAITFOR 15 "802.11s peer link ESTAB both ways" \
    "NX hl iw dev bvw_m0 station dump | grep -q 'mesh plink:.*ESTAB' && NX hw iw dev bvw_m1 station dump | grep -q 'mesh plink:.*ESTAB'"
mk_batman_peer hl bvw_m0 "$L_HARD" "$L_BAT" "$L_IP/16" client || exit 1
cap_start hl bvw_m0 L-mesh0
cap_start hw bvw_mon air
INJ() { NX hw $PY "$BVW_DIR/inj80211.py" bvw_mon "$@" | sed 's/^/     | /'; }
lcount() { sleep 0.3; $PY "$BVW_DIR/pcapq.py" count "$BVW_EVID/$SCEN-L-mesh0.pcap" --src "$W_HARD" "$@" | sed -E 's/count=([0-9]+).*/\1/'; }

note "part A: replayed shapes"
INJ rewrite "$L_HARD" "$W_HARD" "$ELP" 6
sleep 1.5
CHECK "(a) DA-rewritten replica x6: L has no neighbour W" "! bn hl | grep -q $W_HARD"
CHECK "(a) ... yet mac80211 handed all 6 to L's stack as Ethernet unicast to L (batman-adv dropped them)" \
    "[ \$(lcount --dst $L_HARD --type 0x03) -ge 6 ]"
INJ group "$L_HARD" "$W_HARD" "$ELP" 2
WAITFOR 3 "(c) standard group frame: L lists W as neighbour" "bn hl | grep -q $W_HARD"
CHECK "(c) ... delivered as Ethernet broadcast" "[ \$(lcount --dst ff:ff:ff:ff:ff:ff --type 0x03) -ge 2 ]"
del_batman_peer hl
mk_batman_peer hl bvw_m0 "$L_HARD" "$L_BAT" "$L_IP/16" client || exit 1
sleep 0.5
CHECK "bat0 recreated: no neighbour" "! bn hl | grep -q $W_HARD"
B0=$(lcount --dst ff:ff:ff:ff:ff:ff --type 0x03)
INJ ae2 "$L_HARD" "$W_HARD" "$ELP" 2
WAITFOR 3 "(b) AE-2 replica: L lists W as neighbour" "bn hl | grep -q $W_HARD"
CHECK "(b) ... delivered as Ethernet broadcast from W's mesh MAC" "[ \$(lcount --dst ff:ff:ff:ff:ff:ff --type 0x03) -ge $((B0 + 2)) ]"
sleep 0.6
INJ ae2 "$L_HARD" "$W_HARD" "$OGM" 1
WAITFOR 3 "(b) AE-2 OGM2: L batctl o has W" "bo hl | grep -qE '^ *\* +$W_HARD '"
WAITFOR 3 "(b) AE-2 OGM2: L batctl tg has W's soft MAC with W's CRC 0x$WCRC" \
    "btg hl | grep -E '$W_SOFT +-1 ' | grep -q '(0x$WCRC)'"
sleep 1
CHECK "(b) ... and L sent no TT request (tt_request_tx $(bstat hl tt_request_tx))" "[ '$(bstat hl tt_request_tx)' = 0 ]"
NX hl iw dev bvw_m0 mpp dump | sed 's/^/     | mpp: /'

note "part B: live batvm_node, AE-2 replicas (AT+MESHGRP=0)"
del_batman_peer hl
mk_batman_peer hl bvw_m0 "$L_HARD" "$L_BAT" "$L_IP/16" client || exit 1
start_w hw w bvw_m1 "$W_HARD" "$W_SOFT" "$W_IP/16" --inject bvw_mon --shape ae2 --peer "$L_HARD"
WAITFOR 5 "L batctl n lists the live W" "bn hl | grep -qE '^$W_HARD '"
WAITFOR 5 "L routes W" "bo_best_via hl $W_HARD $W_HARD"
WAITFOR 5 "L's tg has W's soft MAC via W with W's CRC, not temporary" \
    "c=\$(w_soft_crc w $W_SOFT); [ -n \"\$c\" ] && btg hl | grep -E '^ *\* +$W_SOFT +-1 +\[[^T]*\] ' | grep -q \"(0x\$c)\""
WAITFOR 5 "W lists L as neighbour and routes it" "wst w | grep -qE '^\+BATN: $L_HARD ' && wst w | grep -qE '^\+BATO: $L_HARD .* nh=$L_HARD'"
WAITFOR 8 "W holds every (VID, CRC) record L announces" "w_tt_matches_peer w hl $L_HARD"
WAITFOR 10 "W dropped Linux's 200-byte unicast ELP probes (elp_probe > 0) and still has 1 neighbour" \
    "[ \$(wcnt w elp_probe) -gt 0 ] && wst w | grep -q '^+BATN: count=1/'"
N1=$(bstat hl tt_request_tx)
sleep 5
CHECK "L's TT requests to W stay at $N1 (now $(bstat hl tt_request_tx))" "[ '$(bstat hl tt_request_tx)' = '$N1' ]"
bla_settle hl
WAITFOR 25 "first L -> W ping over 802.11s" "NX hl ping -c 1 -W 1 $W_IP >/dev/null"
CHECK "L -> W ping 5/5" "NX hl ping -c 5 -i 0.2 -W 2 $W_IP | grep -q ' 0% packet loss'"
CHECK "W -> L ping 5/5" "NX hw ping -c 5 -i 0.2 -W 2 $L_IP | grep -q ' 0% packet loss'"
CHECK "batctl ping -c 3 <W> over 802.11s" "NX hl batctl meshif bat0 ping -c 3 $W_HARD | grep -q '3 packets transmitted, 3 received'"
wfresh
CHECK "W's frames went out through the monitor (air_frames $(wcnt w air_frames), tx_err $(wcnt w tx_err))" \
    "[ $(wcnt w air_frames) -gt 20 ] && [ $(wcnt w tx_err) = 0 ]"
CHECK "L received W's broadcasts as Ethernet broadcast and its unicast as unicast (ELP $(lcount --dst ff:ff:ff:ff:ff:ff --type 0x03), UNICAST $(lcount --dst $L_HARD --type 0x40))" \
    "[ $(lcount --dst ff:ff:ff:ff:ff:ff --type 0x03) -gt 10 ] && [ $(lcount --dst $L_HARD --type 0x40) -ge 5 ]"

Q=$($PY "$BVW_DIR/pcapq.py" bcast "$BVW_EVID/$SCEN-L-mesh0.pcap" --src "$W_HARD")
CHECK "each of W's BCASTs reached L exactly once as an AE-2 replica ($Q)" "echo '$Q' | grep -qE '^bcasts=[1-9][0-9]* min=1 max=1$'"

note "part C: live batvm_node, standard group frames x3 (AT+MESHGRP=1)"
stop_w w
cap_stop
cap_start hl bvw_m0 L-mesh0-group
cap_start hw bvw_mon air-group
sleep 3
start_w hw w bvw_m1 "$W_HARD" "$W_SOFT" "$W_IP/16" --inject bvw_mon --shape group --bcast-copies 3 --peer "$L_HARD"
WAITFOR 6 "L accepts the restarted W (last-seen < 1.5 s)" "seen_lt hl $W_HARD 1.5"
WAITFOR 20 "W -> L ping (W's ARP as a group-frame BCAST)" "NX hw ping -c 1 -W 1 $L_IP >/dev/null"
CHECK "L -> W ping 5/5" "NX hl ping -c 5 -i 0.2 -W 2 $W_IP | grep -q ' 0% packet loss'"
NX hl $PY "$BVW_DIR/udp.py" recv 239.0.0.69 4403 "$L_IP" 10 '#2' >"$BVW_RUN/s09.burst" 2>&1 &
sleep 0.5
NX hw $PY "$BVW_DIR/udp.py" send 239.0.0.69 4403 "$W_IP" burst 3 0
WAITFOR 5 "W sends three multicasts at once (three BCASTs queued together): the last reaches L's socket" \
    "grep -q '^got ' $BVW_RUN/s09.burst"
wfresh
sleep 0.5
Q=$($PY "$BVW_DIR/pcapq.py" bcast "$BVW_EVID/$SCEN-L-mesh0-group.pcap" --src "$W_HARD")
CHECK "each of W's BCASTs reached L three times as a group frame ($Q; bc_tx_own $(wcnt w bc_tx_own))" \
    "echo '$Q' | grep -qE '^bcasts=[1-9][0-9]* min=3 max=3$'"
Q=$($PY "$BVW_DIR/pcapq.py" bcgap "$BVW_EVID/$SCEN-L-mesh0-group.pcap" --src "$W_HARD")
CHECK "... about 5 ms apart, never back to back (dataplane §5.3; $Q)" \
    "echo '$Q' | awk '{ split(\$3, g, \"=\"); exit !(g[2] >= 4.0) }'"
Q=$($PY "$BVW_DIR/pcapq.py" grpgap "$BVW_EVID/$SCEN-L-mesh0-group.pcap" --src "$W_HARD")
CHECK "... and no group frame of W's (ELP, OGM, any broadcast's copy) within 4 ms of the one before, broadcasts queued together included ($Q)" \
    "echo '$Q' | awk '{ split(\$2, g, \"=\"); split(\$3, c, \"=\"); exit !(g[2] >= 4.0 && c[2] >= 2) }'"
snapshot end hl
cap_stop
finish
