#!/usr/bin/env bash
# Watch every Warthog port that appears, with dynamic discovery so a
# reset (USB-Serial-JTAG → USB-OTG/CDC handoff) doesn't lose the log.
#
# A "Warthog port" is any /dev/cu.usbmodem<digits> — that matches both:
#   • USB-Serial-JTAG (e.g. usbmodem11401) — present only during the brief
#     window between RESET and USB-OTG taking over.
#   • TinyUSB CDC (e.g. usbmodem00011, usbmodem3) — present after USB-OTG
#     is up; carries ESP_LOG via the custom vprintf hook plus the AT console.
#
# Same physical board can show up under MULTIPLE port names across a reset;
# this script assigns a fresh label per port path, so don't expect [A] to
# always be the same board. The timing of when each port appears tells you
# which is which.
#
# Usage:
#   ./scripts/watch-multi.sh                    # discover dynamically forever
#   ./scripts/watch-multi.sh -t 60              # auto-quit after 60 s idle
#                                                # (idle = no ports attached)
#   ./scripts/watch-multi.sh /dev/cu.usbmodemX  # explicit ports only (no discovery)
#
# Compatible with bash 3.2 (macOS default) — no associative arrays.

set -u

LABELS=(A B C D E F G H)
COLORS=(36 35 33 32 34 31 96 93)   # cyan, magenta, yellow, green, blue, red, br-cyan, br-yellow

# Parallel arrays — index i refers to the same watcher across all three.
WATCH_PORTS=()
WATCH_PIDS=()
WATCH_LABELS=()

# Index of port in WATCH_PORTS, or -1 if absent.
find_port_index() {
    local needle="$1"
    local i
    for i in "${!WATCH_PORTS[@]}"; do
        if [[ "${WATCH_PORTS[$i]:-}" == "$needle" ]]; then
            echo "$i"
            return
        fi
    done
    echo "-1"
}

label_in_use() {
    local needle="$1"
    local i
    for i in "${!WATCH_LABELS[@]}"; do
        if [[ "${WATCH_LABELS[$i]:-}" == "$needle" ]]; then
            return 0
        fi
    done
    return 1
}

next_free_label_index() {
    local i
    for (( i=0; i<${#LABELS[@]}; i++ )); do
        if ! label_in_use "${LABELS[$i]}"; then
            echo "$i"
            return
        fi
    done
    echo "-1"
}

watch_port() {
    local port="$1"
    local label="$2"
    local color="$3"
    while [[ -e "$port" ]]; do
        stty -f "$port" raw 115200 2>/dev/null || true
        cat "$port" 2>/dev/null | \
            awk -v lbl="$label" -v col="$color" \
                '{ printf "\033[%sm[%s]\033[0m %s\n", col, lbl, $0; fflush() }'
        sleep 0.1
    done
}

start_watcher() {
    local port="$1"
    local idx
    idx=$(next_free_label_index)
    if [[ "$idx" -lt 0 ]]; then
        echo "(no free label slot; ignoring $port)" >&2
        return
    fi
    local label="${LABELS[$idx]}"
    local color="${COLORS[$idx]}"

    printf '\033[%sm[%s] === %s connected to %s ===\033[0m\n' \
           "$color" "$label" "$(date +%H:%M:%S)" "$port"

    watch_port "$port" "$label" "$color" &
    local pid=$!

    WATCH_PORTS+=("$port")
    WATCH_PIDS+=("$pid")
    WATCH_LABELS+=("$label")
}

stop_watcher_at_index() {
    local i="$1"
    local port="${WATCH_PORTS[$i]:-}"
    local pid="${WATCH_PIDS[$i]:-}"
    local label="${WATCH_LABELS[$i]:-}"
    if [[ -n "$pid" ]]; then
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null || true
    fi
    printf '\033[31m[%s] === %s %s gone ===\033[0m\n' \
           "${label:-?}" "$(date +%H:%M:%S)" "$port"
    # Mark slot empty — leaves a hole in the array, which the find_port_index
    # / label_in_use checks treat as "not present". start_watcher will append
    # new entries; we don't compact.
    unset 'WATCH_PORTS['"$i"']'
    unset 'WATCH_PIDS['"$i"']'
    unset 'WATCH_LABELS['"$i"']'
}

stop_watcher_by_port() {
    local port="$1"
    local i
    i=$(find_port_index "$port")
    [[ "$i" -ge 0 ]] && stop_watcher_at_index "$i"
}

stop_all_watchers() {
    local i
    for i in "${!WATCH_PORTS[@]}"; do
        if [[ -n "${WATCH_PORTS[$i]:-}" ]]; then
            stop_watcher_at_index "$i"
        fi
    done
}

# Parse args.
idle_timeout=0
explicit_ports=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -t) idle_timeout="$2"; shift 2 ;;
        *)  explicit_ports+=("$1"); shift ;;
    esac
done

echo "=== watch-multi running. Ctrl-C to quit. ==="
if [[ ${#explicit_ports[@]} -gt 0 ]]; then
    echo "(explicit port mode — no discovery)"
    for p in "${explicit_ports[@]}"; do
        start_watcher "$p"
    done
    trap 'stop_all_watchers; exit 0' INT TERM EXIT
    wait
    exit 0
fi

echo "(discovering /dev/cu.usbmodem[0-9]+ continuously)"
echo

trap 'stop_all_watchers; exit 0' INT TERM EXIT

idle_seconds=0
while true; do
    # Snapshot currently-present ports.
    present_ports=()
    while IFS= read -r p; do
        [[ -n "$p" ]] && present_ports+=("$p")
    done < <(ls /dev/cu.usbmodem* 2>/dev/null | grep -E 'usbmodem[0-9]+$' | sort)

    # New ports → start watchers.
    for p in "${present_ports[@]+"${present_ports[@]}"}"; do
        if [[ "$(find_port_index "$p")" -lt 0 ]]; then
            start_watcher "$p"
        fi
    done

    # Disappeared ports → stop watchers.
    for i in "${!WATCH_PORTS[@]}"; do
        port="${WATCH_PORTS[$i]:-}"
        if [[ -z "$port" ]]; then continue; fi
        still_present=0
        for q in "${present_ports[@]+"${present_ports[@]}"}"; do
            if [[ "$q" == "$port" ]]; then still_present=1; break; fi
        done
        if [[ "$still_present" -eq 0 ]] || ! [[ -e "$port" ]]; then
            stop_watcher_at_index "$i"
        fi
    done

    # Idle-quit logic — count live watchers.
    if [[ "$idle_timeout" -gt 0 ]]; then
        live=0
        for i in "${!WATCH_PORTS[@]}"; do
            [[ -n "${WATCH_PORTS[$i]:-}" ]] && live=$(( live + 1 ))
        done
        if [[ "$live" -eq 0 ]]; then
            idle_seconds=$(( idle_seconds + 1 ))
            if [[ "$idle_seconds" -ge "$idle_timeout" ]]; then
                echo "=== idle ${idle_timeout}s — exiting ==="
                exit 0
            fi
        else
            idle_seconds=0
        fi
    fi

    sleep 0.2
done
