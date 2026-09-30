#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# VM interop suite: the clean-room BATMAN_V engine (main/bat) against batman-adv 2024.3.
#
# usage: ./run_all.sh [sNN ...]        (default: every sNN_*.sh in this directory)
#
# Needs Linux with root via sudo -n, network namespaces, veth, bridge, ebtables, tc (pedit),
# tcpdump, dnsmasq, busybox, python3, gcc, iw and mac80211_hwsim (s09), and batman-adv 2024.3 at
# /opt/batman-adv/batman-adv.ko (loaded as lib.sh's bvw_modules describes). Builds
# batvm_node and batvm_node_san first. Every namespace/link/bridge is named bvw_* and
# removed at the end of each scenario. Per-scenario output, pcaps and batctl/engine
# state snapshots go to $BVW_EVID (default /tmp/bvw_evidence). Exit 0 only if every
# CHECK passed; a SKIP is reported but is not a failure.
set -u
cd "$(dirname "$0")" || exit 2
if [ "$(id -u)" != 0 ]; then
    exec sudo -n -E bash "$0" "$@"
fi
export BVW_EVID=${BVW_EVID:-/tmp/bvw_evidence}
mkdir -p "$BVW_EVID"
make -s all || { echo "FAIL build"; exit 1; }
if [ $# -gt 0 ]; then
    list=""
    for s in "$@"; do list="$list $(ls ${s%.sh}*.sh 2>/dev/null | head -1)"; done
else
    list=$(ls s[0-9][0-9]_*.sh)
fi
rc=0
summary=""
logs=(/dev/null)    # so cat below never reads stdin, even with no scenario
for f in $list; do
    s=$(basename "$f" .sh)
    logs+=("$BVW_EVID/$s.log")
    SCEN=$s bash "./$f" 2>&1 | tee "$BVW_EVID/$s.log"
    r=$(grep '^RESULT ' "$BVW_EVID/$s.log" | tail -1)
    [ -z "$r" ] && r="RESULT $s (no result line: aborted)"
    grep -q ' fail=0 ' <<<"$r" || rc=1
    summary="$summary$r
"
done
ok=$(cat "${logs[@]}" | grep -c '^ok   ')
fail=$(cat "${logs[@]}" | grep -c '^FAIL ')
skip=$(cat "${logs[@]}" | grep -c '^SKIP ')
echo "================ summary"
printf '%s' "$summary"
echo "TOTAL ok=$ok FAIL=$fail SKIP=$skip"
left=$(ip netns list | grep -c '^bvw_')
echo "namespaces left: $left"
[ "$left" = 0 ] || rc=1
exit $rc
