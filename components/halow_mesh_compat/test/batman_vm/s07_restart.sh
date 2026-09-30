#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# s07_restart: restart behaviour against batman-adv's sequence-number rules
# (membership 7.1/7.3, elp-ogm 3.5):
#  1. W killed and restarted > 30 s after first contact (new random ELP/OGM/BCAST
#     seqnos): L accepts it within 5 s; nothing re-requested.
#  2. W restarted again 4 s later: L ignores its OGMs until 30 s after the previous
#     reset (last-seen climbs past 20 s) while the neighbour, route and TT stay; then
#     recovers. From here W keeps its counters in --seq-file (the port's RTC record), which
#     starts empty: still random seqnos.
#  3. L's bat0 recreated with a new bat0 MAC: W re-learns L's originator and its new TT.
#  4. W restarted twice 4 s apart with its counters kept (membership 7.4.3): each resumes
#     256 ahead, L never holds W (last-seen stays low) and W -> L ping (an ARP broadcast)
#     works within 3 s of each.
source "$(dirname "$0")/lib.sh"
bvw_begin
W_TAGS=w
SEQF=$BVW_RUN/w.seq
rm -f "$SEQF"
L_BAT2=02:b0:1c:00:01:ff
mk_pair client 1460 || exit 1
WAITFOR 6 "L routes W" "bo_best_via l $W_HARD $W_HARD"
T_FIRST=$(now_ms)
bla_settle l
WAITFOR 20 "L -> W ping before any restart" "NX l ping -c 1 -W 1 $W_IP >/dev/null"
left=$(( T_FIRST + 34000 - $(now_ms) )); [ $left -gt 0 ] && sleep $((left / 1000))

ogmseq() { wst w | sed -nE 's/^\+BATO: self=.* ogmseq=([0-9]+) elpseq=([0-9]+) .*/\1 \2/p'; }
note "phase 1: kill W, restart it with new random sequence numbers"
SEQ1=$(ogmseq); N0=$(bstat l tt_request_tx)
stop_w w KILL
sleep 3
CHECK "L's last-seen for W grows while W is down ($(bo_seen l $W_HARD) s)" "! seen_lt l $W_HARD 2.5"
start_w w w bvw_w0 "$W_HARD" "$W_SOFT" "$W_IP/16"
T1=$(now_ms)
WAITFOR 5 "L accepts the restarted W's OGMs (last-seen < 1 s)" "seen_lt l $W_HARD 1.0"
SEQ2=$(ogmseq)
CHECK "the restart drew new OGM/ELP sequence numbers ($SEQ1 -> $SEQ2)" "[ -n '$SEQ2' ] && [ '$SEQ1' != '$SEQ2' ]"
WAITFOR 5 "W re-learns L: route and every TT CRC" "wst w | grep -qE '^\+BATO: $L_HARD .* nh=$L_HARD' && w_tt_matches_peer w l $L_HARD"
WAITFOR 5 "L keeps W's soft MAC with the restarted W's CRC" \
    "c=\$(w_soft_crc w $W_SOFT); [ -n \"\$c\" ] && btg l | grep '$W_SOFT ' | grep -q \"(0x\$c)\""
CHECK "L requested nothing after W's restart (tt_request_tx $N0 -> $(bstat l tt_request_tx))" "[ $(bstat l tt_request_tx) = $N0 ]"
WAITFOR 10 "W -> L ping after the restart (W's new BCAST seqnos accepted)" "NX w ping -c 1 -W 1 $L_IP >/dev/null"

note "phase 2: restart W again 4 s after the first restart"
left=$(( T1 + 4000 - $(now_ms) )); [ $left -gt 0 ] && sleep "$((left / 1000)).$(printf %03d $((left % 1000)))"
stop_w w KILL
start_w w w bvw_w0 "$W_HARD" "$W_SOFT" "$W_IP/16" --seq-file "$SEQF"
T2=$(now_ms)
CHECK "an empty record: W still drew random seqnos (seq_carried $(wcnt w seq_carried))" "[ $(wcnt w seq_carried) = 0 ]"
MAX=0; REC=""; NB=1; RT=1; TG=1
while [ $(( $(now_ms) - T1 )) -lt 45000 ]; do
    s=$(bo_seen l $W_HARD)
    [ -z "$s" ] && RT=0
    bn l | grep -q "$W_HARD" || NB=0
    btg l | grep -q "$W_SOFT" || TG=0
    if [ -n "$s" ]; then
        MAX=$(awk -v a="$MAX" -v b="$s" 'BEGIN { print (b > a) ? b : a }')
        if awk -v m="$MAX" -v s="$s" 'BEGIN { exit !(m > 5 && s < 1.5) }'; then
            REC=$(( $(now_ms) - T1 ))
            break
        fi
    fi
    sleep 0.5
done
note "max last-seen $MAX s; recovered at T1+${REC:-never} ms"
CHECK "L ignored the second restart's OGMs: last-seen climbed past 20 s (max $MAX s)" "awk 'BEGIN { exit !($MAX > 20) }'"
CHECK "L accepted W again about 30 s after the first restart (T1+${REC:-never} ms, expected 29-35 s)" \
    "[ -n '$REC' ] && [ ${REC:-0} -ge 29000 ] && [ ${REC:-0} -le 35000 ]"
CHECK "W stayed in L's batctl n throughout (ELP restart adopted at once)" "[ $NB = 1 ]"
CHECK "L's route and TT entry for W stayed throughout the window" "[ $RT = 1 ] && [ $TG = 1 ]"
WAITFOR 15 "W <-> L ping after recovery" "NX w ping -c 1 -W 1 $L_IP >/dev/null && NX l ping -c 1 -W 1 $W_IP >/dev/null"

note "phase 3: recreate L's bat0 with a new bat0 MAC"
left=$(( T2 + 33000 - $(now_ms) )); [ $left -gt 0 ] && sleep $((left / 1000 + 1))
R0=$(wcnt w tt_req_tx)
del_batman_peer l
sleep 1
mk_batman_peer l bvw_l0 "$L_HARD" "$L_BAT2" "$L_IP/16" client || exit 1
WAITFOR 10 "W re-learns the restarted L: TT holds the new bat0 MAC, not the old, CRCs equal L's" \
    "w_tt_matches_peer w l $L_HARD && wst w | grep -q '^+BATTG: $L_BAT2 ' && ! wst w | grep -q '^+BATTG: $L_BAT '"
CHECK "W fetched L's new table with a TT request (tt_req_tx $R0 -> $(wcnt w tt_req_tx))" "[ $(wcnt w tt_req_tx) -gt $R0 ]"
WAITFOR 10 "the restarted L routes W and holds W's soft MAC with W's CRC" \
    "bo_best_via l $W_HARD $W_HARD && c=\$(w_soft_crc w $W_SOFT) && [ -n \"\$c\" ] && btg l | grep '$W_SOFT ' | grep -q \"(0x\$c)\""
bla_settle l
WAITFOR 60 "L -> W ping after L's restart" "NX l ping -c 1 -W 1 $W_IP >/dev/null"
WAITFOR 30 "W -> L ping after L's restart" "NX w ping -c 1 -W 1 $L_IP >/dev/null"

note "phase 4: restart W twice, 4 s apart, its counters kept"
MAX=0
sample_seen() {     # MAX = the highest last-seen of W at L so far
    local s
    s=$(bo_seen l $W_HARD)
    [ -n "$s" ] && MAX=$(awk -v a="$MAX" -v b="$s" 'BEGIN { print (b > a) ? b : a }')
}
for k in 1 2; do
    O1=$(ogmseq); O1=${O1%% *}
    stop_w w KILL
    start_w w w bvw_w0 "$W_HARD" "$W_SOFT" "$W_IP/16" --seq-file "$SEQF"
    TK=$(now_ms)
    O2=$(ogmseq); O2=${O2%% *}
    D=$(( (${O2:-0} - ${O1:-0}) & 0xffffffff ))
    CHECK "kept restart $k: W resumed its counters (seq_carried $(wcnt w seq_carried), OGM seqno $O1 -> $O2, +$D)" \
        "[ $(wcnt w seq_carried) = 1 ] && [ -n '$O1' ] && [ -n '$O2' ] && [ $D -ge 256 ] && [ $D -le 260 ]"
    WAITFOR 3 "kept restart $k: W -> L ping (W's ARP broadcast taken at once)" "sample_seen; NX w ping -c 1 -W 1 $L_IP >/dev/null"
    while [ $(( $(now_ms) - TK )) -lt $(( k == 1 ? 4000 : 12000 )) ]; do
        sample_seen
        sleep 0.25
    done
done
note "phase 4: max last-seen of W at L $MAX s"
CHECK "L never held the kept restarts: W's last-seen at L stayed under 4 s (max $MAX s; phase 2 climbed past 20)" \
    "awk 'BEGIN { exit !($MAX < 4) }'"
snapshot end l
cap_stop
finish
