# SPDX-License-Identifier: GPL-2.0-or-later
# shellcheck shell=bash
# Shared helpers for the batman_vm scenarios (sourced by every sNN_*.sh).
#
# Scenarios run as root (they re-exec themselves under sudo -n). Every namespace,
# veth and bridge they create starts with "bvw_"; an EXIT trap kills every process
# in those namespaces and deletes them, pass or fail. The VM NICs enp0s1/enp0s2 are
# never touched: all links live inside bvw_* namespaces.
#
# Batman state is read only by parsing batctl text output; engine state only from
# batvm_node's status file (its renders).
#
# Environment:
#   BVW_EVID   evidence directory for pcaps and logs   (default /tmp/bvw_evidence)
#   BVW_RUN    scratch directory for status files      (default /tmp/bvw_run)
#   BVW_KEEP=1 keep the namespaces after the scenario (debugging only)

set -u
if [ "$(id -u)" != 0 ]; then
    exec sudo -n -E bash "$0" "$@"
fi
export LC_ALL=C

BVW_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BVW_EVID=${BVW_EVID:-/tmp/bvw_evidence}
BVW_RUN=${BVW_RUN:-/tmp/bvw_run}
BVW_NODE_BIN=${BVW_NODE_BIN:-$BVW_DIR/batvm_node}
SCEN=${SCEN:-$(basename "$0" .sh)}
mkdir -p "$BVW_EVID" "$BVW_RUN"
PASS=0
FAILN=0
SKIPN=0
BVW_CAPS=""

# ---- result lines ----------------------------------------------------------

now_ms() { echo $(( $(date +%s%N) / 1000000 )); }

CHECK() {           # CHECK "<description>" <shell expression>
    local d=$1
    shift
    if eval "$*"; then
        echo "ok   $d"
        PASS=$((PASS + 1))
        return 0
    fi
    echo "FAIL $d"
    FAILN=$((FAILN + 1))
    return 1
}

WAITFOR() {         # WAITFOR <seconds> "<description>" <shell expression>
    local t=$1 d=$2
    shift 2
    local t0
    t0=$(now_ms)
    while :; do
        if eval "$*"; then
            local el=$(( $(now_ms) - t0 ))
            echo "ok   $d ($((el / 1000)).$(printf %03d $((el % 1000)))s)"
            PASS=$((PASS + 1))
            return 0
        fi
        if [ $(( $(now_ms) - t0 )) -ge $((t * 1000)) ]; then
            echo "FAIL $d (not within ${t}s)"
            FAILN=$((FAILN + 1))
            return 1
        fi
        sleep 0.25
    done
}

SKIP() { echo "SKIP $1"; SKIPN=$((SKIPN + 1)); }
note() { echo "---- $*"; }
# stdin on one line, blank runs squeezed; for a quoted note argument ("*" in batctl output globs)
oneline() { tr -s ' \t\n' ' ' | sed -E 's/^ //; s/ $//'; }

finish() {
    echo "RESULT $SCEN pass=$PASS fail=$FAILN skip=$SKIPN"
    [ "$FAILN" = 0 ]
}

# ---- environment -----------------------------------------------------------

bvw_modules() {
    if [ "$(cat /sys/module/batman_adv/version 2>/dev/null)" != 2024.3 ]; then
        modprobe cfg80211
        modprobe bridge
        modprobe libcrc32c
        modprobe -r batman_adv 2>/dev/null
        insmod /opt/batman-adv/batman-adv.ko
    fi
    if [ "$(cat /sys/module/batman_adv/version 2>/dev/null)" != 2024.3 ]; then
        echo "FAIL batman-adv 2024.3 is not loaded"
        exit 1
    fi
    if ! batctl ra 2>/dev/null | grep -q BATMAN_V; then
        echo "FAIL batctl ra does not offer BATMAN_V"
        exit 1
    fi
}

bvw_cleanup() {
    [ "${BVW_KEEP:-0}" = 1 ] && return 0
    local ns p
    for ns in $(ip netns list 2>/dev/null | awk '{print $1}' | grep '^bvw_'); do
        for p in $(ip netns pids "$ns" 2>/dev/null); do kill -TERM "$p" 2>/dev/null; done
    done
    sleep 0.6
    for ns in $(ip netns list 2>/dev/null | awk '{print $1}' | grep '^bvw_'); do
        for p in $(ip netns pids "$ns" 2>/dev/null); do kill -KILL "$p" 2>/dev/null; done
        ip netns del "$ns" 2>/dev/null
    done
    for p in $BVW_CAPS; do kill -INT "$p" 2>/dev/null; done
    BVW_CAPS=""
    return 0
}

bvw_begin() {       # modules, stale cleanup, trap, header
    bvw_modules
    bvw_cleanup
    trap 'bvw_cleanup' EXIT
    echo "==== $SCEN  (batman-adv $(cat /sys/module/batman_adv/version), $(uname -r))"
}

# ---- topology --------------------------------------------------------------

NX() { local ns=$1; shift; ip netns exec "bvw_$ns" "$@"; }

mk_ns() {           # mk_ns <name>...
    local n
    for n in "$@"; do
        ip netns add "bvw_$n"
        ip -n "bvw_$n" link set lo up
    done
}

mk_veth() {         # mk_veth <ns1> <if1> <mac1> <ns2> <if2> <mac2>
    ip link add "$2" netns "bvw_$1" address "$3" type veth peer "$5" netns "bvw_$4" address "$6"
    NX "$1" sysctl -qw "net.ipv6.conf.$2.disable_ipv6=1"
    NX "$4" sysctl -qw "net.ipv6.conf.$5.disable_ipv6=1"
    NX "$1" ip link set "$2" up
    NX "$4" ip link set "$5" up
}

# OpenMANET wizard profile (BATMAN_V, bla 1, hop_penalty 30, bonding 1, aggregation 1,
# ap_isolation 0, fragmentation 1, orig_interval 1000, dat 1, multicast forced to
# flood, network_coding 1, gw_mode server|client); hard MTU 1500, bat0 MTU 1460.
mk_batman_peer() {  # mk_batman_peer <ns> <hardif> <hard-mac> <bat0-mac> <ip/16|-> <gw_mode> [bat0-mtu]
    local ns=$1 hif=$2 hmac=$3 bmac=$4 ip=$5 gw=$6 bmtu=${7:-1460} kv
    if [ "$(NX "$ns" cat "/sys/class/net/$hif/address")" != "$hmac" ]; then
        NX "$ns" ip link set "$hif" down         # (a joined mesh point is left alone)
        NX "$ns" ip link set "$hif" address "$hmac" mtu 1500
    fi
    NX "$ns" ip link set "$hif" up
    NX "$ns" ip link add bat0 type batadv ra BATMAN_V || return 1
    NX "$ns" ip link set bat0 address "$bmac"
    for kv in "bridge_loop_avoidance 1" "hop_penalty 30" "bonding 1" "aggregation 1" \
              "ap_isolation 0" "fragmentation 1" "orig_interval 1000" \
              "distributed_arp_table 1" "multicast_forceflood 1" "gw_mode $gw"; do
        NX "$ns" batctl meshif bat0 $kv || { echo "FAIL batctl $kv on $ns"; return 1; }
    done
    # accepted silently by batctl; batman-adv 2024.3 here is built without NC
    NX "$ns" batctl meshif bat0 network_coding 1 >/dev/null 2>&1 || true
    NX "$ns" batctl meshif bat0 if add "$hif" || return 1
    NX "$ns" ip link set bat0 mtu "$bmtu"
    [ "$ip" != - ] && NX "$ns" ip addr add "$ip" dev bat0
    NX "$ns" ip link set bat0 up
    eval "BVW_UP_$ns=$(now_ms)"
}

del_batman_peer() { NX "$1" ip link del bat0; }

# Ports of the shared medium (bridge bvw_med in namespace m): every receiver of a flooded
# frame gets its own copy of the data. batman-adv decrements a BCAST's TTL in place, so
# without this every later reader of a clone sharing the data (an engine's recvfrom, a
# receive-side tcpdump, another peer) saw TTL - 1. A tc mirred fan-out shares data too.
med_private() {     # med_private <bridge-port>...
    local p
    for p in "$@"; do
        NX m tc qdisc add dev "$p" clsact || return 1
        NX m tc filter add dev "$p" egress protocol 0x4305 matchall \
            action pedit munge offset 0 u32 preserve >/dev/null || return 1
    done
}

# Wait until the peer's bat0 has been up for <s> seconds (BLA start-up broadcast hold).
bla_settle() {      # bla_settle <ns> [seconds=32]
    local v="BVW_UP_$1" s=${2:-32}
    local left=$(( ${!v} + s * 1000 - $(now_ms) ))
    if [ "$left" -gt 0 ]; then
        note "waiting $((left / 1000)) s for $1's BLA start-up broadcast hold"
        sleep "$((left / 1000)).$(printf %03d $((left % 1000)))"
    fi
}

# ---- the engine node -------------------------------------------------------

# start_w <ns> <tag> <hardif> <hard-mac> <soft-mac> <tap-ip/16|-> [batvm_node args...]
# The TAP is bvw_<tag>t; status file $BVW_RUN/<tag>.status; stderr in <tag>.log.
start_w() {
    local ns=$1 tag=$2 hif=$3 hmac=$4 smac=$5 ip=$6 i
    shift 6
    local tap="bvw_${tag}t" st="$BVW_RUN/$tag.status"
    rm -f "$st" "$st.tmp"
    # not through the NX function: $! must be the node itself, not a subshell
    ip netns exec "bvw_$ns" env ASAN_OPTIONS=abort_on_error=0:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1 \
        "$BVW_NODE_BIN" --hard "$hif" --tap "$tap" --hard-mac "$hmac" --soft-mac "$smac" \
        --status "$st" "$@" >>"$BVW_RUN/$tag.log" 2>&1 &
    echo $! >"$BVW_RUN/$tag.pid"
    disown $! 2>/dev/null
    for i in $(seq 1 40); do
        NX "$ns" ip link show "$tap" >/dev/null 2>&1 && break
        sleep 0.1
    done
    NX "$ns" sysctl -qw "net.ipv6.conf.$tap.disable_ipv6=0" 2>/dev/null
    [ "$ip" != - ] && NX "$ns" ip addr add "$ip" dev "$tap"
    NX "$ns" ip link set "$tap" up
    for i in $(seq 1 40); do
        [ -s "$st" ] && break
        sleep 0.1
    done
    eval "BVW_WNS_$tag=$ns"
    eval "BVW_WSTART_$tag=$(now_ms)"
}

w_alive() { kill -0 "$(cat "$BVW_RUN/$1.pid" 2>/dev/null)" 2>/dev/null; }

stop_w() {          # stop_w <tag> [signal=TERM]
    local pid i
    pid=$(cat "$BVW_RUN/$1.pid" 2>/dev/null) || return 0
    kill -"${2:-TERM}" "$pid" 2>/dev/null
    for i in $(seq 1 30); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    kill -KILL "$pid" 2>/dev/null
    return 0
}

wst() { cat "$BVW_RUN/$1.status" 2>/dev/null; }
wfresh() { sleep 1.2; }                            # the status file is rewritten every second
wcnt() {            # wcnt <tag> <counter>  (engine counter or +NODE field)
    local v
    v=$(wst "$1" | grep -E '^\+(BATSTAT|NODE):' | tr ' ' '\n' | grep "^$2=" | head -1 | cut -d= -f2)
    echo "${v:-0}"
}
w_soft_crc() {      # W's own TT CRC for its soft MAC (untagged)
    wst "$1" | grep "^+BATTL: $2 " | sed -E 's/.*crc=0x([0-9a-f]+).*/\1/'
}

# ---- batctl text parsing (peer side) ----------------------------------------

bn()  { NX "$1" batctl meshif bat0 n -H 2>/dev/null; }
bo()  { NX "$1" batctl meshif bat0 o -H 2>/dev/null; }
btg() { NX "$1" batctl meshif bat0 tg -H 2>/dev/null; }
btl() { NX "$1" batctl meshif bat0 tl -H 2>/dev/null; }
bstat() { NX "$1" batctl meshif bat0 s 2>/dev/null | awk -v k="$2:" '$1 == k { print $2 }'; }

# Originator line of <orig> whose next hop is <nh> and marked best ("*").
bo_best_via() { bo "$1" | grep -E "^ *\* +$2 .*$3" >/dev/null; }
# last-seen seconds (float) of originator <orig> in batctl o, or empty
bo_seen() { bo "$1" | grep -E "^ *\* +$2 " | head -1 | sed -E 's/^ *\* +[0-9a-f:]+ +([0-9.]+)s.*/\1/'; }
# last-seen of <orig> in the peer's batctl o is below <seconds>
seen_lt() { local s; s=$(bo_seen "$1" "$2"); [ -n "$s" ] && awk -v s="$s" -v t="$3" 'BEGIN { exit !(s < t) }'; }
# throughput (Mbit/s, float) of the best route to <orig>
bo_tput() { bo "$1" | grep -E "^ *\* +$2 " | head -1 | sed -E 's/.*\( *([0-9.]+)\).*/\1/'; }
# link throughput estimate (Mbit/s, float) of neighbour <hard> in batctl n
bn_tput() { bn "$1" | grep -E "^ *$2 " | head -1 | sed -E 's/.*\( *([0-9.]+)\).*/\1/'; }

# (vid crc) pairs of the peer's local TT, one per line, e.g. "-1 6c7cbcd6"
btl_vid_crcs() {
    btl "$1" | sed -nE 's/^ *\*? *[0-9a-f:]{17} +(-?[0-9]+) .*\(0x([0-9a-f]+)\) *$/\1 \2/p' | sort -u
}
# (vid crc) pairs W holds for originator <orig>
w_vid_crcs() {
    wst "$1" | sed -nE "s/^\+BATTG: crc via=$2 vid=(-?[0-9]+) crc=0x([0-9a-f]+).*/\1 \2/p" | sort -u
}
# W holds exactly the peer's announced (vid, crc) set for it
w_tt_matches_peer() {   # <tag> <peer-ns> <peer-orig>
    local a b
    a=$(btl_vid_crcs "$2")
    b=$(w_vid_crcs "$1" "$3")
    [ -n "$a" ] && [ "$a" = "$b" ]
}

# ---- captures ---------------------------------------------------------------

cap_start() {       # cap_start <ns> <if> <label>
    ip netns exec "bvw_$1" tcpdump -i "$2" -U --immediate-mode -n -s 0 -Z root -w "$BVW_EVID/$SCEN-$3.pcap" >/dev/null 2>&1 &
    BVW_CAPS="$BVW_CAPS $!"
    sleep 0.4
}

cap_stop() {
    local p
    for p in $BVW_CAPS; do kill -INT "$p" 2>/dev/null; done
    sleep 0.5
    BVW_CAPS=""
}

# Save the peer and engine tables into the evidence directory.
snapshot() {        # snapshot <label> <ns>... (peer namespaces) ; engine tags via W_TAGS
    local out="$BVW_EVID/$SCEN-$1.state.txt" ns t
    shift
    {
        for ns in "$@"; do
            for t in n o tg tl gwl; do
                echo "## $ns batctl $t"
                NX "$ns" batctl meshif bat0 $t 2>&1
            done
            echo "## $ns batctl s"
            NX "$ns" batctl meshif bat0 s 2>&1
        done
        for t in ${W_TAGS:-}; do
            echo "## engine $t status"
            wst "$t"
        done
    } >"$out" 2>&1
}

# ---- address plan (all locally administered) --------------------------------
# W = the engine (batvm_node): hard = originator = mesh MAC; soft = 0x06-prefixed (design D2)
W_HARD=02:b0:77:00:00:01; W_SOFT=06:b0:77:00:00:01; W_IP=10.41.253.2
# batman-adv peers: hard interface MAC = originator; bat0 MAC; bat0 address in 10.41/16
L_HARD=02:b0:1c:00:00:01; L_BAT=02:b0:1c:00:00:ff; L_IP=10.41.0.1
A_HARD=02:b0:0a:00:00:01; A_BAT=02:b0:0a:00:00:ff; A_IP=10.41.0.10
C_HARD=02:b0:0c:00:00:01; C_BAT=02:b0:0c:00:00:ff; C_IP=10.41.0.12
PY=python3

# The standard pair: L (batman-adv, OpenMANET profile) <-veth-> W (engine), captured on W's veth.
mk_pair() {         # mk_pair [gw_mode=client] [bat0-mtu=1460] -- [batvm_node args...]
    local gw=${1:-client} bmtu=${2:-1460}
    shift 2 2>/dev/null
    [ "${1:-}" = -- ] && shift
    mk_ns l w
    mk_veth l bvw_l0 "$L_HARD" w bvw_w0 "$W_HARD"
    mk_batman_peer l bvw_l0 "$L_HARD" "$L_BAT" "$L_IP/16" "$gw" "$bmtu" || return 1
    cap_start w bvw_w0 w0
    start_w w w bvw_w0 "$W_HARD" "$W_SOFT" "$W_IP/16" "$@"
}
