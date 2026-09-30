#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# s04_mcast: Meshtastic UDP multicast 239.0.0.69:4403 L -> W and W -> L (payload compared).
# OpenMANET forces multicast to flood, so both directions ride BCAST.
source "$(dirname "$0")/lib.sh"
bvw_begin
W_TAGS=w
mk_pair client 1460 || exit 1
WAITFOR 6 "L routes W" "bo_best_via l $W_HARD $W_HARD"
bla_settle l

mc() {              # mc <from-ns> <from-ip> <to-ns> <to-ip> <label>
    local tok="meshtastic-$5-$RANDOM$RANDOM" r
    NX "$3" $PY "$BVW_DIR/udp.py" recv 239.0.0.69 4403 "$4" 8 "$tok" >"$BVW_RUN/s04.$5" 2>&1 &
    r=$!
    sleep 0.8
    NX "$1" $PY "$BVW_DIR/udp.py" send 239.0.0.69 4403 "$2" "$tok" 3
    wait $r
    local rc=$?
    sed 's/^/     | /' "$BVW_RUN/s04.$5"
    return $rc
}
D0=$(wcnt w bc_deliver); T0=$(wcnt w bc_tx_own)
CHECK "239.0.0.69:4403 L -> W received with the sent payload" "mc l $L_IP w $W_IP l2w"
CHECK "239.0.0.69:4403 W -> L received with the sent payload" "mc w $W_IP l $L_IP w2l"
wfresh
CHECK "W delivered L's multicast BCASTs (bc_deliver $D0 -> $(wcnt w bc_deliver))" "[ $(wcnt w bc_deliver) -ge $((D0 + 3)) ]"
CHECK "W sent its multicast as BCAST (bc_tx_own $T0 -> $(wcnt w bc_tx_own))" "[ $(wcnt w bc_tx_own) -ge $((T0 + 3)) ]"
note "L with multicast_forceflood 0 (vanilla batman-adv multicast_mode 1): W announces no MCAST"
note "container, so L counts it as wanting all IPv4 multicast and sends it UNICAST copies"
NX l batctl meshif bat0 multicast_forceflood 0
sleep 2
U0=$(wcnt w uc_deliver); D0=$(wcnt w bc_deliver)
CHECK "239.0.0.69:4403 L -> W received (forceflood 0)" "mc l $L_IP w $W_IP l2w_uc"
wfresh
CHECK "... carried as UNICAST with a multicast client destination (uc_deliver $U0 -> $(wcnt w uc_deliver), bc_deliver $D0 -> $(wcnt w bc_deliver))" \
    "[ $(wcnt w uc_deliver) -ge $((U0 + 3)) ]"
CHECK "239.0.0.69:4403 W -> L received (forceflood 0)" "mc w $W_IP l $L_IP w2l_uc"
snapshot end l
cap_stop
finish
