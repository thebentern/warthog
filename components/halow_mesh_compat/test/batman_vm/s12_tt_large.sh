#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# s12_tt_large: L (bridged) learns 120 LAN clients while W is up (large OGM diffs), then W
# restarts and must fetch L's whole table as a late joiner: the full-table TT response is
# larger than the 1500-byte hard MTU, so L fragments it and W reassembles it. Every row
# and per-VLAN CRC must match with requests quiet. Finally L grows to 200 clients, whose
# full table exceeds the engine's 2048-byte reassembly buffer (design section 9 limit):
# the diffs still keep W in sync while it stays up (its TT listing, several render chunks long,
# shows every row), and a restarted W reports fr_toobig and backs its requests off.
source "$(dirname "$0")/lib.sh"
bvw_begin
W_TAGS=w
mk_ns x
mk_pair client 1460 || exit 1
NX l ip link add bvw_br type bridge
NX l ip link set bvw_br address "$L_BAT"
NX l ip addr del "$L_IP/16" dev bat0
NX l ip link set bat0 master bvw_br
mk_veth x bvw_x0 02:b0:0d:00:00:01 l bvw_lx 02:b0:1c:00:0d:01
NX l ip link set bvw_lx master bvw_br
NX l ip addr add "$L_IP/16" dev bvw_br
NX l ip link set bvw_br up
WAITFOR 10 "W holds every (VID, CRC) of the bridged L" "w_tt_matches_peer w l $L_HARD && w_vid_crcs w $L_HARD | grep -q '^1 '"
bla_settle l

rows_match() {      # W's rows via L == L's local table (MAC, VID)
    local a b
    a=$(btl l | sed -nE 's/^ *\*? *([0-9a-f:]{17}) +(-?[0-9]+) .*/\1 \2/p' | sort -u)
    b=$(wst w | sed -nE "s/^\+BATTG: ([0-9a-f:]{17}) vid=(-?[0-9]+) via=$L_HARD .*/\1 \2/p" | sort -u)
    [ -n "$a" ] && [ "$a" = "$b" ]
}
lrows() { btl l | grep -c ':'; }
wrows() { wst w | sed -nE 's/^\+BATTG: rows=([0-9]+)\/.*/\1/p'; }
paged_whole() {     # one status snapshot: one summary, and a client line for each of rows=N (> 150)
    local st n
    st=$(wst w)
    n=$(grep -cE '^\+BATTG: [0-9a-f:]{17} ' <<<"$st")
    [ "$(grep -c '^+BATTG: rows=' <<<"$st")" = 1 ] && [ "$n" -gt 150 ] &&
        [ "$n" = "$(sed -nE 's/^\+BATTG: rows=([0-9]+)\/.*/\1/p' <<<"$st")" ]
}

note "phase 1: 120 clients appear behind L while W is up"
NX x $PY "$BVW_DIR/clients.py" bvw_x0 06:c1:00:00:10:00 120 | sed 's/^/     | /'
WAITFOR 15 "L's local TT holds the 120 clients ($(lrows) rows so far)" "[ \$(lrows) -ge 123 ]"
WAITFOR 15 "W's TT rows for L equal L's local table and every CRC matches" "rows_match && w_tt_matches_peer w l $L_HARD"
note "L rows $(lrows), W rows $(wrows); W:" "$(wst w | grep -E '^\+BATSTAT: ut ' | tr ' ' '\n' | grep -E '^tt_(diff|full|req_tx|crc_fail)=' | oneline)"
R1=$(wcnt w tt_req_tx); sleep 6
CHECK "W's TT requests quiet ($R1 -> $(wcnt w tt_req_tx))" "[ $(wcnt w tt_req_tx) = $R1 ]"

note "phase 2: W restarts and fetches the table as a late joiner"
stop_w w
sleep 2
start_w w w bvw_w0 "$W_HARD" "$W_SOFT" "$W_IP/16"
WAITFOR 15 "the restarted W's rows equal L's local table and every CRC matches" "rows_match && w_tt_matches_peer w l $L_HARD"
wfresh
note "W:" "$(wst w | grep -E '^\+BATSTAT: (ut|fr) ' | tr ' ' '\n' | grep -E '^(tt_full|tt_diff|tt_req_tx|fr_done|fr_rx|fr_toobig)=' | oneline)"
CHECK "W received L's full table as a fragmented TT response (tt_full $(wcnt w tt_full), fr_done $(wcnt w fr_done))" \
    "[ $(wcnt w tt_full) -ge 1 ] && [ $(wcnt w fr_done) -ge 1 ]"
R1=$(wcnt w tt_req_tx); sleep 6
CHECK "W's TT requests quiet after the sync ($R1 -> $(wcnt w tt_req_tx))" "[ $(wcnt w tt_req_tx) = $R1 ]"
WAITFOR 20 "W reaches a client-side address: W -> L ping" "NX w ping -c 1 -W 1 $L_IP >/dev/null"

note "phase 3: L grows to 200 clients"
NX x $PY "$BVW_DIR/clients.py" bvw_x0 06:c1:00:00:20:00 80 | sed 's/^/     | /'
WAITFOR 15 "L's local TT holds 200 clients" "[ \$(lrows) -ge 203 ]"
WAITFOR 15 "W (up) keeps in sync through the diffs: rows and CRCs match" "rows_match && w_tt_matches_peer w l $L_HARD"
note "L rows $(lrows), W rows $(wrows)"
CHECK "W's TT listing, paged in 4096-byte render chunks, shows all $(wrows) rows once" "paged_whole"
stop_w w
sleep 2
start_w w w bvw_w0 "$W_HARD" "$W_SOFT" "$W_IP/16"
sleep 12
wfresh
note "W:" "$(wst w | grep -E '^\+BATSTAT: (ut|fr) ' | tr ' ' '\n' | grep -E '^(tt_full|tt_req_tx|tt_req_stall|fr_done|fr_toobig|fr_rx)=' | oneline)"
if rows_match; then
    CHECK "late joiner at 200 clients: synced" "true"
else
    CHECK "late joiner at 200 clients: not synced, reported as fr_toobig (documented 2048-byte limit)" "[ $(wcnt w fr_toobig) -ge 1 ]"
    R1=$(wcnt w tt_req_tx); sleep 30
    R2=$(wcnt w tt_req_tx)
    CHECK "... and its full-table requests back off: 6, 12, 24 s apart ($R1 -> $R2 in 30 s, not 10; tt_req_stall $(wcnt w tt_req_stall))" \
        "[ $((R2 - R1)) -le 3 ] && [ $(wcnt w tt_req_stall) -ge 2 ]"
fi
snapshot end l
cap_stop
finish
