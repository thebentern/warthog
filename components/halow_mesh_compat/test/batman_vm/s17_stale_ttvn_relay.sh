#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# s17_stale_ttvn_relay: W - B - C, B a batman-adv relay with two hard interfaces, C bridged with 200
# LAN clients (full table over the engine's 2048-byte reassembly buffer). W is up and in sync with C
# through the diffs, then misses two of C's change sets (its OGM receive blocked 12 s while C adds
# two clients 4 s apart). W cannot take C's full table again, so it keeps its rows and its old
# synced TTVN for C. B drops a unicast whose TTVN is older than its own view of C when TT still
# names C (dataplane 6.2), so W's unicast to C, to rows it still holds, must carry the TTVN C now
# announces, and W -> C must keep working.
source "$(dirname "$0")/lib.sh"
bvw_begin
W_TAGS=w
B0_HARD=02:b0:0b:00:00:01; B1_HARD=02:b0:0b:00:00:02; B_BAT=02:b0:0b:00:00:ff; B_IP=10.41.0.11
mk_ns w b c x
mk_veth w bvw_w0 "$W_HARD" b bvw_b0 "$B0_HARD"
mk_veth b bvw_b1 "$B1_HARD" c bvw_c0 "$C_HARD"
mk_batman_peer b bvw_b1 "$B1_HARD" "$B_BAT" "$B_IP/16" server 1500 || exit 1   # primary: B1 = originator
NX b batctl meshif bat0 if add bvw_b0
mk_batman_peer c bvw_c0 "$C_HARD" "$C_BAT" - client 1500 || exit 1
NX c ip link add bvw_br type bridge
NX c ip link set bvw_br address "$C_BAT"
NX c ip link set bat0 master bvw_br
NX c ip addr add "$C_IP/16" dev bvw_br
NX c ip link set bvw_br up
mk_veth x bvw_x0 02:b0:0d:00:00:01 c bvw_cx 02:b0:0c:00:0d:01
NX c ip link set bvw_cx master bvw_br
cap_start w bvw_w0 w0
cap_start c bvw_c0 c0
start_w w w bvw_w0 "$W_HARD" "$W_SOFT" "$W_IP/16" --soft-mtu 1500 --tput auto
bla_settle c
bla_settle b
for k in 0 1 2 3 4 5 6 7 8 9; do
    NX x $PY "$BVW_DIR/clients.py" bvw_x0 "06:c1:00:00:$(printf %02x $((0x20 + k))):00" 20 | sed 's/^/     | /'
    sleep 2.5
done
rows_c() { wst w | grep -cE "^\+BATTG: [0-9a-f:]{17} .*via=$C_HARD .*flags=--[^T]"; }
wttvn_c() { wst w | sed -nE "s/^\+BATO: $C_HARD .* ttvn=([0-9]+) .*/\1/p"; }
cttvn() { NX c batctl meshif bat0 tl 2>/dev/null | sed -nE 's/.*TTVN: ([0-9]+).*/\1/p' | head -1; }
WAITFOR 25 "W (up) follows C's 200 clients through the diffs: rows and CRCs match" \
    "[ \$(rows_c) -ge 203 ] && w_tt_matches_peer w c $C_HARD"
WAITFOR 20 "W -> C ping (C's bridge address) across B" "NX w ping -c 1 -W 1 $C_IP >/dev/null"
V0=$(wttvn_c)
note "W's view of C:" "$(wst w | grep -E "^\+BATO: $C_HARD" | oneline)"

note "W's OGM receive blocked 12 s; C adds a client, then another 4 s later"
NX w tc qdisc add dev bvw_w0 clsact
NX w tc filter add dev bvw_w0 ingress protocol 0x4305 u32 match u8 0x04 0xff at 0 action drop
sleep 1
NX x $PY "$BVW_DIR/clients.py" bvw_x0 06:c1:00:00:40:00 1 | sed 's/^/     | /'
sleep 4
NX x $PY "$BVW_DIR/clients.py" bvw_x0 06:c1:00:00:41:00 1 | sed 's/^/     | /'
sleep 7
NX w tc qdisc del dev bvw_w0 clsact
sleep 8
wfresh
V1=$(wttvn_c); CV=$(cttvn)
note "W:" "$(wst w | grep -E '^\+BATSTAT: (ut|fr) ' | tr ' ' '\n' | grep -E '^(tt_full|tt_diff|tt_req_tx|tt_req_stall|tt_crc_fail|fr_toobig)=' | oneline)"
note "W's view of C:" "$(wst w | grep -E "^\+BATO: $C_HARD" | oneline)" "; C's TTVN $CV"
CHECK "W missed C's change sets and cannot take C's full table: synced TTVN still $V0 ($V1) while C is at $CV, fr_toobig" \
    "[ '$V1' = '$V0' ] && [ '$CV' != '$V0' ] && [ $(wcnt w fr_toobig) -ge 1 ]"
CHECK "the answers, cut by B on C's behalf, count against C: tt_req_stall $(wcnt w tt_req_stall)" \
    "[ $(wcnt w tt_req_stall) -ge 1 ]"
CHECK "W still holds C's bridge MAC row" "wst w | grep -qE '^\+BATTG: $C_BAT vid=-1 via=$C_HARD '"
NX w ip neigh flush all 2>/dev/null
OUT=$(NX w ping -c 5 -i 0.5 -W 1 $C_IP 2>&1); echo "$OUT" | sed 's/^/     | /'
CHECK "W -> C ping still crosses B after the missed change sets" "echo \"\$OUT\" | grep -qE ' [1-5] received'"
snapshot end b c
cap_stop
finish
