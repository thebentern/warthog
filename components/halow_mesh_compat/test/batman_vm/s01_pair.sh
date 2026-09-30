#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# s01_pair: W (engine) <-> L (batman-adv 2024.3, OpenMANET profile).
# Neighbour + originator both ways, W's soft MAC in L's global TT with W's CRC and no
# TT request, L's TT held by W with every per-VLAN CRC equal, requests quiesce.
source "$(dirname "$0")/lib.sh"
bvw_begin
W_TAGS=w
mk_pair client 1460 || exit 1

WAITFOR 3 "L batctl n lists W with a non-zero throughput" \
    "bn l | grep -qE '^$W_HARD +[0-9.]+s +\( *[1-9][0-9.]*\)'"
WAITFOR 5 "L batctl o routes W with next hop W" "bo_best_via l $W_HARD $W_HARD"
# W's own OGM advertises the maximum (membership M4), so L's first-hop route is L's link to W.
# A WAITFOR: an OGM that arrives before L's first ELP sample of W is stored with throughput 0.
WAITFOR 3 "L's route to W equals L's link to W (W's own OGM advertises the maximum)" \
    "a=\$(bo_tput l $W_HARD); b=\$(bn_tput l $W_HARD); [ -n \"\$a\" ] && [ \"\$a\" != 0.0 ] && [ \"\$a\" = \"\$b\" ]"
note "L: route to W $(bo_tput l $W_HARD) Mbit/s, link to W $(bn_tput l $W_HARD) Mbit/s"
WAITFOR 5 "L batctl tg has W's soft MAC via W, untagged, not temporary" \
    "btg l | grep -E '^ *\* +$W_SOFT +-1 +\[[^T]*\] .* $W_HARD ' >/dev/null"
WAITFOR 3 "L's TT CRC for W equals W's own committed CRC" \
    "c=\$(w_soft_crc w $W_SOFT); [ -n \"\$c\" ] && [ \"\$c\" != 00000000 ] && btg l | grep '$W_SOFT ' | grep -q \"(0x\$c)\""
note "W's CRC 0x$(w_soft_crc w $W_SOFT); L's tg line:" "$(btg l | grep "$W_SOFT " | oneline)"

WAITFOR 5 "W lists L as neighbour with a non-zero throughput" \
    "wst w | grep -qE '^\+BATN: $L_HARD orig=$L_HARD .* tput=([1-9][0-9]*\.[0-9]|0\.[1-9])'"
WAITFOR 5 "W routes L (next hop L, default table)" \
    "wst w | grep -qE '^\+BATO: $L_HARD .* nh=$L_HARD tput=[0-9]'"
WAITFOR 8 "W holds every (VID, CRC) record L announces" "w_tt_matches_peer w l $L_HARD"
note "L local TT (vid crc):" "$(btl_vid_crcs l | oneline)"
note "W's view of L     :" "$(w_vid_crcs w $L_HARD | oneline)"
LSET=$(btl l | sed -nE 's/^ *\*? *([0-9a-f:]{17}) +(-?[0-9]+) .*/\1 \2/p' | sort -u)
WSET=$(wst w | sed -nE "s/^\+BATTG: ([0-9a-f:]{17}) vid=(-?[0-9]+) via=$L_HARD .*/\1 \2/p" | sort -u)
CHECK "W's TT rows for L are exactly L's local table ($(echo "$LSET" | wc -l) rows)" "[ -n '$LSET' ] && [ '$LSET' = '$WSET' ]"
WAITFOR 3 "W's view of L's TTVN equals L's" \
    "v=\$(NX l batctl meshif bat0 tl | sed -nE 's/.*TTVN: ([0-9]+).*/\1/p'); wst w | grep -qE \"^\\+BATO: $L_HARD .* ttvn=\$v \""

# requests quiesce: sample 10 s after W started, then 10 s later
T10=$(( BVW_WSTART_w + 10000 - $(now_ms) )); [ $T10 -gt 0 ] && sleep $((T10 / 1000)).$((T10 % 1000 / 100))
N1=$(bstat l tt_request_tx); R1=$(wcnt w tt_req_tx); D1=$(wcnt w tt_crc_fail)
sleep 10
N2=$(bstat l tt_request_tx); R2=$(wcnt w tt_req_tx); D2=$(wcnt w tt_crc_fail)
CHECK "L's tt_request_tx stops increasing ($N1 -> $N2 over 10 s)" "[ '$N1' = '$N2' ]"
CHECK "L never requested W's table (tt_request_tx = $N2; W's first OGM is self-sufficient)" "[ '$N2' = 0 ]"
CHECK "W's tt_req_tx stops increasing ($R1 -> $R2), tt_crc_fail stays ($D1 -> $D2)" "[ '$R1' = '$R2' ] && [ '$D1' = '$D2' ]"
CHECK "L's view of W is still fresh (last-seen $(bo_seen l $W_HARD) s)" "awk 'BEGIN { exit !($(bo_seen l $W_HARD) < 2.0) }'"
CHECK "W's engine counters: no link failures (lk_fail $(wcnt w lk_fail)), nothing delivered bad" \
    "[ $(wcnt w lk_fail) = 0 ] && [ $(wcnt w deliver_bad) = 0 ]"
snapshot end l
cap_stop
finish
