#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# s16_tt_onbehalf: W - B - C, B a batman-adv relay with two hard interfaces, C bridged with 200 LAN
# clients, so C's full table (about 2.5 KB) is over the engine's 2048-byte reassembly buffer. W
# starts late. B holds C's table in sync and answers W's full-table requests for C itself, with
# source C, cutting the answer itself: the fragment originator is B, not C. W must charge those
# answers to C and back its requests for C off, leave its requests to B alone so it still learns
# B's own table, and reach C's bridge address through a temporary row although C's table was never
# taken: a unicast carrying TTVN 0 would be dropped at B (dataplane 6.2), so it carries C's
# announced TTVN.
source "$(dirname "$0")/lib.sh"
bvw_begin
W_TAGS=w
B0_HARD=02:b0:0b:00:00:01; B1_HARD=02:b0:0b:00:00:02; B_BAT=02:b0:0b:00:00:ff; B_IP=10.41.0.11
mk_ns w b c x
mk_veth w bvw_w0 "$W_HARD" b bvw_b0 "$B0_HARD"
mk_veth b bvw_b1 "$B1_HARD" c bvw_c0 "$C_HARD"
mk_batman_peer b bvw_b1 "$B1_HARD" "$B_BAT" - server 1500 || exit 1   # primary: B1 = originator
NX b batctl meshif bat0 if add bvw_b0
NX b ip addr add "$B_IP/16" dev bat0
mk_batman_peer c bvw_c0 "$C_HARD" "$C_BAT" - client 1500 || exit 1
NX c ip link add bvw_br type bridge
NX c ip link set bvw_br address "$C_BAT"
NX c ip link set bat0 master bvw_br
NX c ip addr add "$C_IP/16" dev bvw_br
NX c ip link set bvw_br up
mk_veth x bvw_x0 02:b0:0d:00:00:01 c bvw_cx 02:b0:0c:00:0d:01
NX c ip link set bvw_cx master bvw_br
bla_settle c
for k in 0 1 2 3 4; do
    NX x $PY "$BVW_DIR/clients.py" bvw_x0 "06:c1:00:00:$(printf %02x $((0x20 + k))):00" 40 | sed 's/^/     | /'
    sleep 2.5
done
WAITFOR 20 "C's local TT holds 200 clients" "[ \$(btl c | grep -c ':') -ge 203 ]"
WAITFOR 20 "B holds C's table, announced (no T flag)" "[ \$(btg b | grep '$C_HARD' | grep -vc 'T]') -ge 203 ]"
cap_start w bvw_w0 w0
start_w w w bvw_w0 "$W_HARD" "$W_SOFT" "$W_IP/16" --soft-mtu 1500 --tput auto
WAITFOR 15 "W routes C via B" "wst w | grep -qE '^\+BATO: $C_HARD .* nh=$B0_HARD tput='"
BR0=$(bstat b tt_response_tx); CR0=$(bstat c tt_request_rx)
sleep 10
wfresh
R1=$(wcnt w tt_req_tx); S1=$(wcnt w tt_req_stall); T1=$(wcnt w fr_toobig)
sleep 40
wfresh
R2=$(wcnt w tt_req_tx); S2=$(wcnt w tt_req_stall); T2=$(wcnt w fr_toobig)
note "40 s window: W tt_req_tx $R1 -> $R2, tt_req_stall $S1 -> $S2, fr_toobig $T1 -> $T2;" \
    "B tt_response_tx $BR0 -> $(bstat b tt_response_tx), C tt_request_rx $CR0 -> $(bstat c tt_request_rx)"
CHECK "B answered W's requests for C on C's behalf (C got none of them)" \
    "[ \$(bstat b tt_response_tx) -gt $BR0 ] && [ \$(bstat c tt_request_rx) = $CR0 ]"
CHECK "W's requests back off: at most 4 in 40 s ($R1 -> $R2; every 3 s would be 13)" "[ $((R2 - R1)) -le 4 ]"
CHECK "tt_req_stall rises with fr_toobig ($S1 -> $S2 while fr_toobig $T1 -> $T2)" "[ $S2 -gt $S1 ] && [ $T2 -gt $T1 ]"
note "W's view of C:" "$(wst w | grep -E "^\+BATO: $C_HARD" | oneline)"
note "W's view of B:" "$(wst w | grep -E "^\+BATO: $B1_HARD" | oneline)"
CHECK "W holds B's own table: B's bat0 MAC resolves via B" \
    "wst w | grep -qE '^\+BATTG: $B_BAT vid=-1 via=$B1_HARD ' && wst w | grep -qE '^\+BATO: $B1_HARD .* ttvn=[0-9]+ '"
CHECK "W reaches B's own address" "NX w ping -c 3 -i 0.5 -W 1 $B_IP >/dev/null"
OUT=$(NX c ping -c 5 -i 0.5 -W 1 "$W_IP" 2>&1); echo "$OUT" | sed 's/^/     | /'
note "W's rows for C:" "$(wst w | grep -E "^\+BATTG: [0-9a-f:]{17} .*via=$C_HARD" | oneline)"
CHECK "C -> W ping across B: W answers C's bridge address (a temporary row) with C's announced TTVN" \
    "echo \"\$OUT\" | grep -qE ' [1-5] received'"
snapshot end b c
cap_stop
finish
