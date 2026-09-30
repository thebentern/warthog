#!/bin/sh
# The forwarding glue (umac_mesh_fwd_glue.c) is the one mesh file no host test
# can link: it needs mmosal, mmpkt, mmdrv and the datapath. Its DECISIONS are in
# umac_mesh_fwd.c, which the unit tests and the simulator drive. What is left
# here is locking and call ordering -- invariants a reviewer found broken once
# and a unit test cannot see. Checked structurally, so a revert fails the suite
# instead of waiting for a radio.
set -u
G=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/mesh/umac_mesh_fwd_glue.c
M=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/mesh/umac_mesh.c
fail=0
ok()   { echo "ok   $1"; }
bad()  { echo "FAIL $1"; fail=1; }

[ -f "$G" ] || { echo "FAIL glue not found at $G"; exit 1; }

# 1. Our HWMP sequence number, the PREQ id and the discovery gate are shared by
#    the netif task and the event loop. Every writer runs under the glue lock.
awk '
  /^[a-zA-Z_].*\(/ && !/;$/ { fn=$0; depth=0 }
  /lock_\(\);/ && !/unlock_\(\);/ { depth++ }
  /unlock_\(\);/ { depth-- }
  /umac_mesh_fwd_originate_preq\(/ && !/^[A-Za-z].*originate_preq\(uint8/ && depth<1 { print "UNLOCKED:" NR }
  /umac_mesh_preq_gate_(allow|sent|last)\(/ && depth<1 { print "UNLOCKED:" NR }
' "$G" > .glueguard.tmp
if [ -s .glueguard.tmp ]; then
  bad "the PREQ gate / originator runs outside the glue lock ($(tr '\n' ' ' < .glueguard.tmp))"
else
  ok "the discovery gate and the PREQ originator run under the glue lock"
fi
rm -f .glueguard.tmp

# 2. The keepalive PREQ in umac_mesh.c advances the SAME sequence number.
if grep -q 'umac_mesh_fwd_glue_lock();' "$M" && \
   awk '/int umac_mesh_hwmp_send_preq/,/^}/' "$M" | grep -q 'umac_mesh_fwd_glue_lock();'; then
  ok "umac_mesh_hwmp_send_preq takes the same lock for its own increment"
else
  bad "umac_mesh_hwmp_send_preq advances s_hwmp_sn without the glue lock"
fi

# 3. The lock is not recursive, so no call site may already hold it.
awk '
  /^[a-zA-Z_].*\(/ && !/;$/ { depth=0 }
  /lock_\(\);/ && !/unlock_\(\);/ { depth++ }
  /unlock_\(\);/ { depth-- }
  /maybe_preq_\(|send_now_\(|umac_mesh_tx_action\(|umac_mesh_fwd_glue_tx_classify\(/ && depth>0 { print "NESTED:" NR }
' "$G" > .glueguard.tmp
if [ -s .glueguard.tmp ]; then
  bad "a call that takes the glue lock is made while holding it ($(tr '\n' ' ' < .glueguard.tmp))"
else
  ok "nothing that re-takes the lock is called while it is held"
fi
rm -f .glueguard.tmp

# 4. The umacd a held frame will be flushed with must be visible before the
#    frame is: set it after the push and the first hold of a boot can be missed.
#    Both pushes: ours (tx_pending) and the relay's (hold_relayed_).
order_ok=1
for fn in umac_mesh_fwd_glue_tx_pending hold_relayed_; do
  awk -v f="$fn" '$0 ~ ("^[a-z].*[ *]" f "\\(") && !/;$/ {on=1} on {print} on && /^}/ {exit}' "$G" | \
    awk '/s_pend_umacd = umacd/ {seen=NR} /umac_mesh_pending_push/ {pushed=1; if (!seen) bad=1}
         END {exit (bad || !pushed) ? 1 : 0}' || order_ok=0
done
if [ $order_ok -eq 1 ]; then
  ok "s_pend_umacd is set before either kind of frame is pushed"
else
  bad "s_pend_umacd is set after umac_mesh_pending_push (or a push is missing): the first held frame of a boot can be skipped"
fi

# 4b. The relay ladder's core timeout is armed only from the relayed-frame hold
#     (receive path) and from the timeout itself, both on the umac event loop.
stray=$(awk '
  /^[a-zA-Z_].*\(/ && !/;$/ { fn=$0; sub(/\(.*/, "", fn); sub(/.*[ *]/, "", fn) }
  /relay_arm_\(|umac_core_register_timeout\(|umac_core_deplete_timeout\(/ && !/^static/ &&
    fn !~ /^(hold_relayed_|relay_timer_|relay_arm_)$/ { print NR }
' "$G")
armers=$(awk '
  /^[a-zA-Z_].*\(/ && !/;$/ { fn=$0; sub(/\(.*/, "", fn); sub(/.*[ *]/, "", fn) }
  /^[[:space:]]+relay_arm_\(/ { print fn }
' "$G" | LC_ALL=C sort -u | tr '\n' ' ')
if [ -n "$stray" ]; then
  bad "the relay ladder's timeout is armed off the event loop at line $(echo "$stray" | tr '\n' ' ')"
elif [ "$armers" = "hold_relayed_ relay_timer_ " ]; then
  ok "the relay ladder's timeout is armed only by the hold and by itself, both on the event loop"
else
  bad "relay_arm_ is called from: ${armers:-nowhere} (want hold_relayed_ and relay_timer_)"
fi

# 4c. send_now_ also runs on the netif task (tx_pending): whatever it or
#     send_relayed_ queues must wake the event loop, or it waits for the loop's
#     next wake-up. A host test cannot see this.
wake_ok=1
for fn in send_now_ send_relayed_; do
  awk -v f="$fn" '$0 ~ ("^static[^;]*[ *]" f "\\(") && !/;$/ {on=1} on {print} on && /^}/ {exit}' "$G" | \
    awk '/enqueue_tx_frame\(/ {q=NR} /umac_core_evt_wake\(umacd\);/ && q {w=NR} /return true;/ && w {ok=1}
         END {exit ok ? 0 : 1}' || wake_ok=0
done
if [ $wake_ok -eq 1 ]; then
  ok "send_now_ and send_relayed_ wake the event loop after queueing"
else
  bad "send_now_ or send_relayed_ queues a frame without umac_core_evt_wake: a tick release waits for the next wake-up"
fi

# 5. Held frames must also be reclaimed on the periodic tick, not only when a
#    path-selection frame happens to arrive -- by the event-loop handler the probe
#    task posts, so the tick's PREQs are sent on the loop, never queued from it.
tick_in() {
  awk -v f="$1" '$0 ~ ("^[a-z].*[ *]" f "\\(") && !/;$/ {on=1} on {print} on && /^}/ {exit}' "$M" |
    grep -cE '^[[:space:]]*umac_mesh_fwd_glue_tick\(\);'
}
if [ "$(tick_in mesh_service_evt_)" = 1 ] && [ "$(grep -cE 'umac_mesh_fwd_glue_tick\(\);' "$M")" = 1 ] && \
   grep -q 'void umac_mesh_fwd_glue_tick' "$G"; then
  ok "held frames are also flushed from the service tick, on the event loop (mesh_service_evt_)"
else
  bad "the glue tick is not run once, from mesh_service_evt_ (in umac_mesh_service_tick: $(tick_in umac_mesh_service_tick))"
fi

# 6. A PREQ we originate goes to the broadcast address. A unicast to a
#    non-neighbour target reaches nobody -- this was a shipped bug once.
if awk '/uint16_t umac_mesh_fwd_originate_preq/,/^}/' \
     ../../halow/components/mm-iot-sdk/framework/morselib/src/umac/mesh/umac_mesh_fwd.c \
     | grep -q 'memset(ra_out, 0xff, 6)'; then
  ok "the originated PREQ is addressed to broadcast"
else
  bad "umac_mesh_fwd_originate_preq no longer returns a broadcast RA"
fi

# 7. One allocator for the Mesh Control sequence number, under the lock. Two
#    frames sharing one are dropped as duplicates by the first relay's cache.
D=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/datapath/umac_datapath.c
stray=$(grep -n 'g_warthog_mesh_seq++' "$G" "$D" | grep -v 'umac_mesh_fwd_glue.c:.*uint32_t seq = g_warthog_mesh_seq++;' || true)
if [ -n "$stray" ]; then
  bad "the mesh sequence number is incremented outside umac_mesh_fwd_glue_next_seq ($(echo "$stray" | tr '\n' ' '))"
elif awk '/uint32_t umac_mesh_fwd_glue_next_seq/,/^}/' "$G" | grep -q 'lock_();'; then
  ok "one locked allocator owns the mesh sequence number"
else
  bad "umac_mesh_fwd_glue_next_seq does not take the lock"
fi

# 8. MGTK Key RSCs. driver_ap.c links hostap, which simnode does not, so the lines
#    that carry AMPE's Key RSCs between hostap and the datapath are checked here;
#    the mesh get_seqnum is compiled out of driver_ap.c and run against a stub.
S=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/supplicant_shim/driver_ap.c
if awk '/const struct wpa_driver_ops mmwlan_wpas_ops_mesh/,/^};/' "$S" | \
     grep -qE '^[[:space:]]*\.get_seqnum[[:space:]]*=[[:space:]]*mmwpas_get_seq_num_mesh,'; then
  ok "the mesh driver answers get_seqnum with mmwpas_get_seq_num_mesh"
else
  bad "the mesh driver's get_seqnum is not mmwpas_get_seq_num_mesh"
fi
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
awk '/^static int mmwpas_get_seq_num_mesh/,/^}/' "$S" > "$T/fn.c"
cat > "$T/t.c" <<'EOF'
#include <stdint.h>
#include <stdio.h>
#define MM_UNUSED(x) (void)(x)
enum mmwlan_status { MMWLAN_SUCCESS, MMWLAN_ERROR };
static int calls, last_id = -1, refuse;
enum mmwlan_status umac_datapath_mesh_own_group_rsc(uint8_t key_id, uint8_t rsc[6])
{
    calls++;
    last_id = key_id;
    for (int i = 0; i < 6; i++) { rsc[i] = (uint8_t)(0xa0 + key_id + i); }
    return refuse ? MMWLAN_ERROR : MMWLAN_SUCCESS;
}
#include "fn.c"
#define Q(addr, idx, seq) mmwpas_get_seq_num_mesh("mesh0", NULL, (addr), (idx), 0, (seq))
int main(void)
{
    static const uint8_t peer[6] = { 0x02, 0, 0, 0, 0, 0x0a };
    uint8_t s[6] = { 0 };
    if (Q(NULL, 1, s) != 0 || calls != 1 || last_id != 1 || s[0] != 0xa1 || s[5] != 0xa6)
    { puts("hostap's own-MGTK query (addr NULL, idx 1) is not answered by the datapath"); return 1; }
    if (Q(NULL, 4, s) != 0 || calls != 2 || last_id != 4)
    { puts("idx 4 does not reach the datapath as key id 4"); return 1; }
    if (Q(peer, 1, s) != -1 || Q(NULL, 0x101, s) != -1 || Q(NULL, -1, s) != -1 ||
        Q(NULL, 1, NULL) != -1 || calls != 2)
    { puts("a peer's addr, an idx outside 0..255 or a NULL seq reached the datapath"); return 1; }
    refuse = 1;
    if (Q(NULL, 1, s) != -1) { puts("a datapath error is answered as success"); return 1; }
    return 0;
}
EOF
why=""
if ${CC:-cc} -std=gnu11 -w -I"$T" -o "$T/t" "$T/t.c" 2>/dev/null && why=$("$T/t"); then
  ok "mesh get_seqnum hands (addr NULL, key id) to the datapath and refuses anything else"
else
  bad "mmwpas_get_seq_num_mesh against a stub datapath: ${why:-did not build or run}"
fi
rm -rf "$T"
if awk '/^static int mmwpas_set_key_mesh/,/^}/' "$S" | grep -q 'params->seq, params->seq_len'; then
  ok "mesh set_key hands the peer's Key RSC to the datapath"
else
  bad "mmwpas_set_key_mesh drops params->seq: a peer's MGTK is installed with replay floor 0"
fi

# 9. One TX PN allocator. Every chip key install takes its epoch from
#    mesh_next_pn_base_, whose read-modify-write runs in a critical section.
#    Every install now runs on the umac event loop; the section stays defensive.
DM=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/datapath/umac_datapath_mesh.c
stray=$(awk '/^static uint64_t mesh_next_pn_base_/,/^}/ {next}
             /s_mesh_key_epoch[[:space:]]*(\+\+|--|[-+]?=[^=])|(\+\+|--)[[:space:]]*s_mesh_key_epoch/ {print NR}' "$DM")
if [ -n "$stray" ]; then
  bad "s_mesh_key_epoch is written outside mesh_next_pn_base_ (line $(echo "$stray" | tr '\n' ' '))"
elif awk '/^static uint64_t mesh_next_pn_base_/,/^}/' "$DM" | \
     awk '/MMOSAL_TASK_ENTER_CRITICAL\(\);/ {c=1} /s_mesh_key_epoch/ && !c {bad=1}
          /MMOSAL_TASK_EXIT_CRITICAL\(\);/ {c=0} END {exit bad?1:0}' && \
     awk '/^static uint64_t mesh_next_pn_base_/,/^}/' "$DM" | grep -q 'MMOSAL_TASK_EXIT_CRITICAL();'; then
  ok "one allocator owns the key epoch, under the critical section"
else
  bad "mesh_next_pn_base_ touches s_mesh_key_epoch outside MMOSAL_TASK_ENTER/EXIT_CRITICAL"
fi

# 10. WARTHOG_MESH_MGTK_PN_BASE depends on the chip honouring INSTALL_KEY's pn for a
#     GTK, which is unmeasured: it is set in warthog-mesh-sae-swccmp (and inherited
#     by -swccmp-on) and in no other section, however the -D is spaced.
P=../../../platformio.ini
pnbase_sections() {
  awk '/^\[/ {e=$0} /-D[[:space:]]*WARTHOG_MESH_MGTK_PN_BASE/ && !/^[[:space:]]*;/ {print e}' "$1" |
    LC_ALL=C sort -u | tr '\n' ' '
}
envs=$(pnbase_sections "$P")
if [ "$envs" = "[env:warthog-mesh-sae-swccmp] " ] && \
   awk '/^\[/ {on = ($0 == "[env:warthog-mesh-sae-swccmp-on]")}
        on && /^[[:space:]]*\$\{env:warthog-mesh-sae-swccmp\.build_flags\}[[:space:]]*$/ {f=1}
        END {exit f?0:1}' "$P"; then
  ok "the MGTK PN base is built only into warthog-mesh-sae-swccmp and its -on child"
else
  bad "WARTHOG_MESH_MGTK_PN_BASE is set in: ${envs:-no section} (want [env:warthog-mesh-sae-swccmp] only)"
fi
T=$(mktemp "${TMPDIR:-/tmp}/glueguard.XXXXXX")
printf '%s\n' '[env:a]' 'build_flags =' '  -D WARTHOG_MESH_MGTK_PN_BASE=1' \
  '[env:b]' 'build_flags = -DWARTHOG_MESH_MGTK_PN_BASE=1' '[env:c]' 'build_flags =' \
  '  ; -DWARTHOG_MESH_MGTK_PN_BASE=1' '[common]' > "$T"
printf 'build_flags = -D\tWARTHOG_MESH_MGTK_PN_BASE\n' >> "$T"
seen=$(pnbase_sections "$T")
rm -f "$T"
if [ "$seen" = "[common] [env:a] [env:b] " ]; then
  ok "that scan sees -D NAME and -D<tab>NAME, in any section, and skips comments"
else
  bad "the PN-base scan read a fixture as: ${seen:-nothing} (want [common] [env:a] [env:b])"
fi

# 11. AT+MESHFWDSTAT? prints each checked counter in its own slot (tblfull=,
#     unestab=). main/at.c is only scraped for storage on the host, so nothing
#     runs it: pair each conversion in the format with its argument by position.
A=../../../main/at.c
fwdstat_slot() {
  awk -v want="$1=%lu" '/"MESHFWDSTAT"\) == 0 && terminator == .\?./ {on=1}
  on {
    l = $0
    while (match(l, /"[^"]*"/)) { fmt = fmt substr(l, RSTART + 1, RLENGTH - 2); l = substr(l, RSTART + RLENGTH) }
    while (match(l, /g_warthog_[A-Za-z0-9_]+/)) { arg[++na] = substr(l, RSTART, RLENGTH); l = substr(l, RSTART + RLENGTH) }
  }
  on && /cdc_write\(line\);/ { exit }
  END {
    i = index(fmt, " " want)
    if (i == 0) { print "no " want " in the format"; exit }
    pre = substr(fmt, 1, i); k = gsub(/%[^%]/, "", pre)
    all = fmt; nc = gsub(/%[^%]/, "", all)
    if (nc != na) { print nc " conversions for " na " arguments"; exit }
    print arg[k + 1]
  }' "$A"
}
for pair in tblfull:g_warthog_fwd_drop_tblfull unestab:g_warthog_hwmp_unestab qfail:g_warthog_hwmp_tx_qfail; do
  label=${pair%%:*}; want=${pair#*:}
  slot=$(fwdstat_slot "$label")
  if [ "$slot" = "$want" ]; then
    ok "AT+MESHFWDSTAT? prints $want as $label="
  else
    bad "AT+MESHFWDSTAT?'s $label= slot prints: ${slot:-nothing}"
  fi
done

# 11b. And the whole line fits its static buffer at its longest (every %lu at 10 digits).
fit=$(awk '/"MESHFWDSTAT"\) == 0 && terminator == .\?./ {on=1}
  on && /static char line\[[0-9]+\]/ { match($0, /\[[0-9]+\]/); size = substr($0, RSTART + 1, RLENGTH - 2) + 0 }
  on && /snprintf\(line, sizeof\(line\),/ { args = 1 }
  args { l = $0; while (match(l, /"[^"]*"/)) { fmt = fmt substr(l, RSTART + 1, RLENGTH - 2); l = substr(l, RSTART + RLENGTH) } }
  on && /cdc_write\(line\);/ { exit }
  END {
    gsub(/\\[rn]/, "x", fmt); lu = gsub(/%lu/, "", fmt)
    if (size == 0 || lu == 0 || index(fmt, "%")) { print "unparsed"; exit }
    need = length(fmt) + 10 * lu + 1
    print (need <= size ? "ok " : "short ") need "/" size
  }' "$A")
case "$fit" in
  ok*) ok "AT+MESHFWDSTAT? fits its buffer at its longest (${fit#ok } bytes)" ;;
  *)   bad "AT+MESHFWDSTAT? can outgrow its buffer: ${fit:-nothing parsed}" ;;
esac

# 12. The path sweep shares the table with the event loop and the netif task.
awk '
  /^[a-zA-Z_].*\(/ && !/;$/ { depth=0 }
  /lock_\(\);/ && !/unlock_\(\);/ { depth++ }
  /unlock_\(\);/ { depth-- }
  /umac_mesh_path_expire\(/ { seen=1; if (depth<1) print "UNLOCKED:" NR }
  END { if (!seen) print "ABSENT" }
' "$G" > .glueguard.tmp
if [ -s .glueguard.tmp ]; then
  bad "the path sweep is missing or runs outside the glue lock ($(tr '\n' ' ' < .glueguard.tmp))"
else
  ok "the path sweep runs under the glue lock"
fi
rm -f .glueguard.tmp

# 13. Leaf learning (event loop) writes the proxy table and the RMC that the leaf
#     TX lookups (netif task) and the render (AT task) read: every engine call on
#     the glue's context, the three leaf calls included, holds the glue lock.
awk '
  /^[a-zA-Z_].*\(/ && !/;$/ { depth=0 }
  /lock_\(\);/ && !/unlock_\(\);/ { depth++ }
  /unlock_\(\);/ { depth-- }
  /umac_mesh_fwd_leaf_learn\(&c,/ { l=1 }
  /umac_mesh_fwd_leaf_proxied\(&c,/ { p=1 }
  /umac_mesh_fwd_proxy_via_peer\(&c,/ { v=1 }
  /umac_mesh_fwd_[a-z_]+\(&c,/ && depth<1 { print "UNLOCKED:" NR }
  END { if (!l || !p || !v) print "ABSENT" }
' "$G" > .glueguard.tmp
if [ -s .glueguard.tmp ]; then
  bad "a leaf call is missing or an engine call runs outside the glue lock ($(tr '\n' ' ' < .glueguard.tmp))"
else
  ok "leaf learn, leaf_proxied, proxy_via_peer and every other engine call hold the glue lock"
fi
rm -f .glueguard.tmp

# 14. Every AT reply, and the OK/ERROR after it, waits for room in the 512 B CDC
#     FIFO (cdc_out_write, tested in test_cdc_out). Only the echo and lines from
#     other tasks (esp_ping callbacks, mcast_rx_task) may use the no-wait writer,
#     and those tasks must: a wait there delays pings or overflows the socket.
stray=$(awk '
  /^[a-zA-Z_].*\(/ && !/;$/ { fn=$0; sub(/\(.*/, "", fn); sub(/.*[ *]/, "", fn) }
  /cdc_write_nowait\(/ && fn !~ /^(cdc_write_nowait|mping_on_success|mping_on_timeout|mcast_rx_task|at_task)$/ { print NR }
  /tinyusb_cdcacm_write_queue\(|tud_cdc_n_write\(|cdc_write_long\(/ { print NR }
  /cdc_out_(write|write_nowait|vprintf_nowait)\(/ && fn !~ /^(cdc_write|cdc_write_nowait)$/ { print NR }
  /(^|[^_a-z])(cdc_write|reply_ok|reply_error)\(/ && fn ~ /^(mping_on_success|mping_on_timeout|mcast_rx_task)$/ { print NR }
' "$A")
if [ -n "$stray" ]; then
  bad "AT output bypasses the waiting writer at main/at.c line $(echo "$stray" | tr '\n' ' ')"
elif awk '/^static void cdc_write\(const char \*s\)/,/^}/' "$A" | grep -q 'cdc_out_write(&g_warthog_cdc, s);' && \
     awk '/^static void cdc_write_nowait\(const char \*s\)/,/^}/' "$A" | grep -q 'cdc_out_write_nowait(&g_warthog_cdc, s);' && \
     awk '/^static void reply_ok\(/,/^}/' "$A" | grep -q '^[[:space:]]*cdc_write("OK' && \
     awk '/^static void reply_error\(/,/^}/' "$A" | grep -q '^[[:space:]]*cdc_write("ERROR' && \
     grep -q '"cdc_out.c"' ../../../main/CMakeLists.txt; then
  ok "AT replies and their OK/ERROR wait for CDC FIFO room; only the echo and other tasks do not"
else
  bad "cdc_write, cdc_write_nowait, reply_ok or reply_error no longer reach g_warthog_cdc, or cdc_out.c is not built"
fi

# 15. AT+MPMPEERS? fits its static buffer at its longest: every %lu at 10 digits
#     (32-bit long), %02x at 2, and %s the whole of g_warthog_mpm_links.
fit=$(awk '
  /^volatile char g_warthog_mpm_links\[[0-9]+\]/ { match($0, /\[[0-9]+\]/); links = substr($0, RSTART + 1, RLENGTH - 2) - 1 }
  /^static void cmd_mpmpeers\(void\)/ { on = 1 }
  on && /static char buf\[[0-9]+\]/ { match($0, /\[[0-9]+\]/); size = substr($0, RSTART + 1, RLENGTH - 2) + 0 }
  on && /snprintf\(buf, sizeof\(buf\), "/ { match($0, /"[^"]*"/); fmt = substr($0, RSTART + 1, RLENGTH - 2); on = 0 }
  END {
    gsub(/\\[rn]/, "x", fmt)
    lu = gsub(/%lu/, "", fmt); x = gsub(/%02x/, "", fmt); s = gsub(/%s/, "", fmt)
    if (size == 0 || links <= 0 || lu == 0 || index(fmt, "%")) { print "unparsed"; exit }
    need = length(fmt) + 10 * lu + 2 * x + links * s + 1
    print (need <= size ? "ok " : "short ") need "/" size
  }' "$A")
case "$fit" in
  ok*) ok "AT+MPMPEERS? fits its buffer at its longest (${fit#ok } bytes)" ;;
  *)   bad "AT+MPMPEERS? can outgrow its buffer: ${fit:-nothing parsed}" ;;
esac

# 16. One lock for every CDC ACM 0 writer (main/usb_net.c): the log mirror goes
#     through the no-wait writer, only the port's queue touches the FIFO, and the
#     lock never blocks a no-wait writer or an ISR and exists before the mirror.
U=../../../main/usb_net.c
stray=$(awk '
  /^[a-zA-Z_].*\(/ && !/;$/ { fn=$0; sub(/\(.*/, "", fn); sub(/.*[ *]/, "", fn) }
  /tinyusb_cdcacm_write_queue\(|tud_cdc_n_write\(/ && fn != "cdc_port_queue_" { print NR }
' "$U")
port=$(awk '/^struct cdc_out g_warthog_cdc = \{/,/^\};/' "$U" | tr -d ' \n')
lockfn=$(awk '/^static bool cdc_port_lock_\(bool wait\)/,/^}/' "$U")
if [ -n "$stray" ]; then
  bad "main/usb_net.c writes the CDC FIFO outside the port's queue at line $(echo "$stray" | tr '\n' ' ')"
elif awk '/^static int cdc_vprintf\(/,/^}/' "$U" | grep -q 'cdc_out_vprintf_nowait(&g_warthog_cdc, fmt, args)' && \
     case "$port" in *.room=cdc_port_room_,*) true ;; *) false ;; esac && \
     case "$port" in *.lock=cdc_port_lock_,*) true ;; *) false ;; esac && \
     case "$port" in *.unlock=cdc_port_unlock_,*) true ;; *) false ;; esac && \
     printf '%s\n' "$lockfn" | grep -q 'xPortInIsrContext()' && \
     printf '%s\n' "$lockfn" | grep -q 'wait ? pdMS_TO_TICKS(CDC_OUT_IDLE_MS) : 0' && \
     awk '/^esp_netif_t \*warthog_usb_net_start\(void\)/,/^}/' "$U" | \
       awk '/s_cdc_mtx = xSemaphoreCreateMutexStatic\(/ {m=NR} /esp_log_set_vprintf\(&cdc_vprintf\)/ {v=NR}
            END {exit (m && v && m < v) ? 0 : 1}'; then
  ok "log lines, +MPING/+MCAST and the echo share the reply lock, never wait for it, and the lock exists first"
else
  bad "main/usb_net.c: the log mirror, the port's lock wiring, or the lock's creation order changed"
fi

# 17. Under SAE, beacons and probe responses need the RSN element hostap puts in our
#     Open/Confirm. The shim links hostap (simnode does not), so the hand-over is checked
#     here: after SAE comes up, and after the beacon init that clears it.
C=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/supplicant_shim/supplicant_core_mesh.c
if awk '/^static int passive_init_ifmsh/,/^}/' "$C" | \
     awk '/mesh_rsn_auth_init\(wpa_s, mconf\)/ {a=NR} /g_warthog_sae_init = 1;/ {if (a) b=NR}
          /umac_mesh_beacon_set_rsn\(mconf->rsn_ie, \(uint16_t\)mconf->rsn_ie_len\);/ {if (b) c=NR}
          /MMLOG_INF\("mesh: open mesh/ {if (c) d=NR}
          END {exit (a && b && c && d) ? 0 : 1}' && \
   awk '/^enum mmwlan_status umac_mesh_enable_mesh\(/,/^}/' "$M" | \
     awk '/umac_mesh_beacon_init\(args, own_addr\);/ {i=NR} /umac_supp_add_mesh_interface\(umacd\)/ {if (i) s=NR}
          END {exit (i && s) ? 0 : 1}'; then
  ok "the shim hands hostap's rsn_ie to the beacon module once SAE is up, after the init that clears it"
else
  bad "hostap's RSN element no longer reaches the beacon and probe responses (or is cleared after it does)"
fi

# 18. IGTKs. hostap hands every BIP key to the mesh set_key op, which the host cannot
#     link: run the op against a stub datapath. A peer's IGTK and our own reach
#     umac_datapath_mesh_set_igtk with the IPN as seq; removing key 4/5 clears ours;
#     CCMP keys still reach set_peer_key; any other algorithm reaches nothing.
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
awk '/^static int mmwpas_set_key_mesh/,/^}/' "$S" > "$T/fn.c"
cat > "$T/t.c" <<'EOF'
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define MMLOG_WRN(...) ((void)0)
typedef uint8_t u8;
enum mmwlan_status { MMWLAN_SUCCESS, MMWLAN_ERROR };
enum wpa_alg { WPA_ALG_NONE, WPA_ALG_WEP, WPA_ALG_TKIP, WPA_ALG_CCMP, WPA_ALG_BIP_CMAC_128, WPA_ALG_GCMP };
struct wpa_driver_set_key_params {
    const char *ifname; enum wpa_alg alg; const u8 *addr; int key_idx; int set_tx;
    const u8 *seq; size_t seq_len; const u8 *key; size_t key_len;
};
static int igtk_calls, peer_calls;
static const uint8_t *i_addr, *i_key, *i_rsc; static uint8_t i_len; static uint16_t i_id; static size_t i_rsc_len;
enum mmwlan_status umac_datapath_mesh_set_igtk(const uint8_t *addr, const uint8_t *key, uint8_t key_len,
                                               uint16_t key_id, const uint8_t *rsc, size_t rsc_len)
{ igtk_calls++; i_addr = addr; i_key = key; i_len = key_len; i_id = key_id; i_rsc = rsc; i_rsc_len = rsc_len;
  return MMWLAN_SUCCESS; }
enum mmwlan_status umac_datapath_mesh_set_peer_key(const uint8_t *a, const uint8_t *k, uint8_t kl, uint8_t id,
                                                   bool pw, const uint8_t *rsc, size_t rl)
{ (void)a; (void)k; (void)kl; (void)id; (void)pw; (void)rsc; (void)rl; peer_calls++; return MMWLAN_SUCCESS; }
#include "fn.c"
int main(void)
{
    static const uint8_t peer[6] = { 2, 0, 0, 0, 0, 0x0a }, bc[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    static const uint8_t key[16] = { 1 }, ipn[6] = { 9, 0, 0, 0, 0, 0 };
    struct wpa_driver_set_key_params p = { .alg = WPA_ALG_BIP_CMAC_128, .addr = peer, .key_idx = 4,
                                           .seq = ipn, .seq_len = 6, .key = key, .key_len = 16 };
    if (mmwpas_set_key_mesh(NULL, &p) != 0 || igtk_calls != 1 || i_addr != peer || i_key != key ||
        i_len != 16 || i_id != 4 || i_rsc != ipn || i_rsc_len != 6)
    { puts("a peer's IGTK does not reach umac_datapath_mesh_set_igtk with its IPN"); return 1; }
    p.addr = bc; p.seq = NULL; p.seq_len = 0;
    if (mmwpas_set_key_mesh(NULL, &p) != 0 || igtk_calls != 2 || i_addr != bc)
    { puts("our own IGTK (broadcast address) does not reach the datapath"); return 1; }
    struct wpa_driver_set_key_params n = { .alg = WPA_ALG_NONE, .addr = NULL, .key_idx = 5 };
    if (mmwpas_set_key_mesh(NULL, &n) != 0 || igtk_calls != 3 || i_key != NULL || i_id != 5 ||
        i_addr == NULL || (i_addr[0] & 1) == 0)
    { puts("removing key 5 does not clear our own IGTK"); return 1; }
    n.key_idx = 1;
    struct wpa_driver_set_key_params g = { .alg = WPA_ALG_GCMP, .addr = peer, .key_idx = 0, .key = key, .key_len = 16 };
    if (mmwpas_set_key_mesh(NULL, &n) != 0 || mmwpas_set_key_mesh(NULL, &g) != 0 || igtk_calls != 3 ||
        peer_calls != 0)
    { puts("removing key 1, or a GCMP key, reached the datapath"); return 1; }
    struct wpa_driver_set_key_params c = { .alg = WPA_ALG_CCMP, .addr = peer, .key_idx = 0, .key = key, .key_len = 16 };
    if (mmwpas_set_key_mesh(NULL, &c) != 0 || peer_calls != 1 || igtk_calls != 3)
    { puts("an MTK no longer reaches set_peer_key"); return 1; }
    return 0;
}
EOF
why=""
if ${CC:-cc} -std=gnu11 -w -I"$T" -o "$T/t" "$T/t.c" 2>/dev/null && why=$("$T/t"); then
  ok "mesh set_key keeps IGTKs host-side (a peer's with its IPN, ours from the broadcast address)"
else
  bad "mmwpas_set_key_mesh against a stub datapath: ${why:-did not build or run}"
fi
rm -rf "$T"

# 19. mesh_config_create leaves the management group cipher 0 (our get_capa reports
#     no BIP): with PMF on, wpa_init then rejects the RSN element, and with it off a
#     peer's AMPE IGTK is parsed at 0 octets. The shim sets it before mesh_rsn starts.
C=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/supplicant_shim/supplicant_core_mesh.c
if awk '/mconf = mesh_config_create\(wpa_s, ssid\);/ {c=NR}
        /mconf->mgmt_group_cipher = WPA_CIPHER_AES_128_CMAC;/ && c {m=NR}
        /wpa_s->mesh_rsn = mesh_rsn_auth_init\(wpa_s, mconf\);/ {r=NR}
        END {exit (c && m && r && c < m && m < r) ? 0 : 1}' "$C"; then
  ok "the SAE mesh sets its BIP-CMAC-128 group management cipher before mesh_rsn starts"
else
  bad "supplicant_core_mesh.c: mgmt_group_cipher is not set between mesh_config_create and mesh_rsn_auth_init"
fi

# 20. A failed SAE handshake gives its datapath slot back at once. hostap is not
#     linked into simnode, so mesh_auth_timer is compiled out of mesh_rsn.c and
#     run against stubs: on a timer expiry without SAE_ACCEPTED it reports the
#     address to umac_mesh (hold-off) BEFORE freeing the station, frees it once,
#     and neither retries nor parks it in PLINK_BLOCKED, where it would hold its
#     slot for up to ~300 s. So too for CONFIRMED (our Confirm sent, the peer's
#     never came) and NOTHING (reset by a peer's failure status): the timer is
#     not re-armed, so nothing else would free them. An accepted station is left alone.
R=../../halow/components/mm-iot-sdk/framework/src/hostap/wpa_supplicant/mesh_rsn.c
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
awk '/^void mesh_auth_timer\(/,/^}/' "$R" > "$T/fn.c"
cat > "$T/t.c" <<'EOF'
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
#define MSG_DEBUG 1
#define MSG_INFO 2
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#define MESH_SAE_AUTH_FAILURE "MESH-SAE-AUTH-FAILURE "
#define MESH_SAE_AUTH_BLOCKED "MESH-SAE-AUTH-BLOCKED "
#define MESH_AUTH_RETRY 3
enum { SAE_NOTHING, SAE_COMMITTED, SAE_CONFIRMED, SAE_ACCEPTED };
enum { PLINK_LISTEN, PLINK_BLOCKED = 7 };
struct sae_data { int state; };
struct sta_info { u8 addr[6]; struct sae_data *sae; int sae_auth_retry; };
struct hostapd_bss_config { int ap_max_inactivity; };
struct hostapd_data { struct hostapd_bss_config *conf; };
struct hostapd_iface { struct hostapd_data **bss; };
struct wpa_supplicant { struct hostapd_iface *ifmsh; };
static int n_fail, n_free, n_retry, n_block;
static u8 reported[6];
static struct sta_info *freed;
static void wpa_msg(void *ctx, int level, const char *fmt, ...) { (void)ctx; (void)level; (void)fmt; }
static void wpa_printf(int level, const char *fmt, ...) { (void)level; (void)fmt; }
void umac_mesh_sae_failed(const u8 *addr) { n_fail++; memcpy(reported, addr, 6); }
static void ap_free_sta(struct hostapd_data *h, struct sta_info *s)
{ (void)h; n_free++; freed = s; memset(s->addr, 0xee, 6); /* what a free leaves */ }
static int mesh_rsn_auth_sae_sta(struct wpa_supplicant *w, struct sta_info *s) { (void)w; (void)s; n_retry++; return 0; }
static void wpa_mesh_set_plink_state(struct wpa_supplicant *w, struct sta_info *s, int st)
{ (void)w; (void)s; if (st == PLINK_BLOCKED) { n_block++; } }
#include "fn.c"
int main(void)
{
    static const u8 mac[6] = { 0x02, 0, 0, 0, 1, 0x80 };
    struct hostapd_bss_config conf = { 300 };
    struct hostapd_data hapd = { &conf };
    struct hostapd_data *bss[1] = { &hapd };
    struct hostapd_iface ifmsh = { bss };
    struct wpa_supplicant wpa_s = { &ifmsh };
    struct sae_data sae = { SAE_ACCEPTED };
    struct sta_info sta = { { 0 }, &sae, 0 };
    memcpy(sta.addr, mac, 6);
    mesh_auth_timer(&wpa_s, &sta);
    if (n_fail || n_free || n_retry || n_block) { puts("an accepted station was touched"); return 1; }
    sae.state = SAE_COMMITTED;
    mesh_auth_timer(&wpa_s, &sta);
    if (n_retry || n_block) { puts("the first failure retries or blocks instead of freeing the slot"); return 1; }
    if (n_fail != 1 || n_free != 1 || freed != &sta)
    { puts("the first failure is not reported once and freed once"); return 1; }
    if (memcmp(reported, mac, 6) != 0) { puts("the address was reported after the station was freed"); return 1; }
    memcpy(sta.addr, mac, 6);
    sae.state = SAE_CONFIRMED;
    mesh_auth_timer(&wpa_s, &sta);
    if (n_fail != 2 || n_free != 2 || n_retry || n_block) { puts("a CONFIRMED station at expiry keeps its slot"); return 1; }
    memcpy(sta.addr, mac, 6);
    sae.state = SAE_NOTHING;
    mesh_auth_timer(&wpa_s, &sta);
    if (n_fail != 3 || n_free != 3 || n_retry || n_block) { puts("a station reset to NOTHING at expiry keeps its slot"); return 1; }
    return 0;
}
EOF
why=""
if ${CC:-cc} -std=gnu11 -w -I"$T" -o "$T/t" "$T/t.c" 2>/dev/null && why=$("$T/t"); then
  ok "mesh_auth_timer: a failed SAE (COMMITTED, CONFIRMED or NOTHING) reports the address, then frees the station; no retry, no BLOCKED"
else
  bad "mesh_auth_timer against stubs: ${why:-did not build or run}"
fi
rm -rf "$T"
D=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/datapath/umac_datapath_mesh.c
if awk '/^static void process_rx_mgmt_frame_mesh\(/,/^}/' "$D" | awk '/^[[:space:]]*default:/ {d=1}
     d && /umac_mesh_note_sae_auth\(/ && !s {n=NR} d && /umac_supp_process_mgmt_frame\(/ {s=NR}
     END {exit (n && s && n < s) ? 0 : 1}'; then
  ok "an SAE Commit ends its sender's hold-off before hostap sees the frame"
else
  bad "umac_mesh_note_sae_auth does not run before umac_supp_process_mgmt_frame on an Authentication frame"
fi

# 21. AT+MESHRSSI=<dBm>: the parser is compiled out of main/at.c and run; the
#     command applies the floor live and persists it, and boot restores it.
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
awk '/^static bool meshrssi_parse_\(/,/^}/' "$A" > "$T/fn.c"
cat > "$T/t.c" <<'EOF'
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "fn.c"
static int check(const char *in, bool want_ok, int32_t want)
{
    int32_t v = 12345;
    bool got = meshrssi_parse_(in, &v);
    if (got != want_ok || (got && v != want)) { printf("'%s' parsed as %d/%ld", in, (int)got, (long)v); return 1; }
    return 0;
}
int main(void)
{
    return check("-80", true, -80) || check("0", true, 0) || check("-255", true, -255) ||
           check("-1", true, -1) || check("-256", false, 0) || check("1", false, 0) ||
           check("", false, 0) || check("-80x", false, 0) || check("dBm", false, 0) ||
           check("-", false, 0) || check("0x10", false, 0) ||
           check("-99999999999999999999", false, 0) || meshrssi_parse_(NULL, NULL);
}
EOF
why=""
if ${CC:-cc} -std=gnu11 -w -I"$T" -o "$T/t" "$T/t.c" 2>/dev/null && why=$("$T/t"); then
  ok "AT+MESHRSSI= accepts -255..0 as a whole argument and rejects the rest"
else
  bad "meshrssi_parse_ against its cases: ${why:-did not build or run}"
fi
rm -rf "$T"
if awk '/"MESHRSSI"\) == 0 && terminator == .=./ {on=1} on && /warthog_cfg_set_mesh_rssi\(/ {p=1}
        on && p && /g_warthog_mesh_rssi_floor = v;/ {l=1} on && /reply_ok\(\);/ {exit}
        END {exit (p && l) ? 0 : 1}' "$A" && \
   grep -q 'g_warthog_mesh_rssi_floor = warthog_cfg_get_mesh_rssi();' ../../../main/mesh.c; then
  ok "AT+MESHRSSI= persists the floor and applies it live; boot restores it"
else
  bad "AT+MESHRSSI= does not both persist and apply the floor, or boot does not restore it"
fi

# 22. The shim stores the RSN on its task while the driver task builds the beacon in two
#     passes: only the store and the snapshot touch s_rsn, both inside the critical section,
#     and the builder reads the one snapshot umac_mesh_get_beacon takes before both passes.
BC=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/mesh/umac_mesh_beacon.c
if awk '/^void umac_mesh_beacon_set_rsn\(/ || /^static uint16_t rsn_snapshot_\(/ {f=1}
        f && /^}/ {f=0; next}
        !f && /(^|[^A-Za-z0-9_])s_rsn/ && !/^static uint8_t s_rsn\[UMAC_MESH_IES_RSN_MAXLEN\];$/ &&
          !/^static uint16_t s_rsn_len;$/ {b=1}
        END {exit b}' "$BC" && \
   ! awk '/^static void umac_mesh_build_beacon_\(/,/^}/' "$BC" | grep -q 'rsn_snapshot_' && \
   awk '/^struct mmpkt \*umac_mesh_get_beacon\(/,/^}/' "$BC" | \
     awk '/rsn_snapshot_\(params\.rsn\)/ {a=NR}
          /build_mgmt_frame\(umacd, umac_mesh_build_beacon_, &params\)/ {if (a) b=NR}
          END {exit (a && b) ? 0 : 1}' && \
   awk '/^void umac_mesh_beacon_set_rsn\(/,/^}/' "$BC" | \
     awk '/MMOSAL_TASK_ENTER_CRITICAL\(\);/ {e=NR} /memcpy\(s_rsn, rsn, len\);/ {if (e && !x) m=NR}
          /s_rsn_len = / {if (e && !x) l=NR} /MMOSAL_TASK_EXIT_CRITICAL\(\);/ {if (e && m && l) x=NR}
          END {exit (e && m && l && x) ? 0 : 1}' && \
   awk '/^static uint16_t rsn_snapshot_\(/,/^}/' "$BC" | \
     awk '/MMOSAL_TASK_ENTER_CRITICAL\(\);/ {e=NR} /= s_rsn_len;/ {if (e && !x) r=NR}
          /memcpy\(out, s_rsn, n\);/ {if (e && !x) m=NR} /MMOSAL_TASK_EXIT_CRITICAL\(\);/ {if (e && r && m) x=NR}
          END {exit (e && r && m && x) ? 0 : 1}'; then
  ok "the RSN store and its per-beacon snapshot run under the lock; the beacon builder reads only that snapshot"
else
  bad "umac_mesh_beacon.c: s_rsn is read or written outside the lock, or the beacon passes do not share one snapshot"
fi

# 23. A peering that fails after SAE is held off too. hostap frees the station when its
#     peering FSM restarts (the HOLDING timer, or a Close while HOLDING or IDLE); without
#     a hold-off the next beacon re-offers it at once. mesh_mpm_fsm_restart is compiled
#     out of mesh_mpm.c and run against stubs: a station that never reached ESTAB is
#     reported to umac_mesh BEFORE it is freed, once; an ESTAB one only freed. WLAN_STA_ASSOC
#     is the ESTAB marker: set in mesh_mpm_plink_estab and nowhere else in mesh_mpm.c.
M=../../halow/components/mm-iot-sdk/framework/src/hostap/wpa_supplicant/mesh_mpm.c
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
awk '/^static void mesh_mpm_fsm_restart\(/,/^}/' "$M" > "$T/fn.c"
cat > "$T/t.c" <<'EOF'
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
#define BIT(x) (1u << (x))
#define WLAN_STA_AUTH BIT(0)
#define WLAN_STA_ASSOC BIT(1)
struct sta_info { u8 addr[6]; uint32_t flags; };
struct hostapd_data { int unused; };
struct hostapd_iface { struct hostapd_data **bss; };
struct wpa_supplicant { struct hostapd_iface *ifmsh; };
static int n_fail, n_free, n_cancel;
static u8 reported[6];
static struct sta_info *freed;
static void plink_timer(void *a, void *b) { (void)a; (void)b; }
static void eloop_cancel_timeout(void (*h)(void *, void *), void *a, void *b)
{ (void)a; (void)b; if (h == plink_timer) { n_cancel++; } }
void umac_mesh_plink_failed(const u8 *addr) { n_fail++; memcpy(reported, addr, 6); }
static void ap_free_sta(struct hostapd_data *h, struct sta_info *s)
{ (void)h; n_free++; freed = s; memset(s->addr, 0xee, 6); /* what a free leaves */ }
#include "fn.c"
int main(void)
{
    static const u8 mac[6] = { 0x02, 0, 0, 0, 2, 0x81 };
    struct hostapd_data hapd = { 0 };
    struct hostapd_data *bss[1] = { &hapd };
    struct hostapd_iface ifmsh = { bss };
    struct wpa_supplicant wpa_s = { &ifmsh };
    struct sta_info sta = { { 0 }, WLAN_STA_AUTH };
    memcpy(sta.addr, mac, 6);
    mesh_mpm_fsm_restart(&wpa_s, &sta);
    if (n_fail != 1 || n_free != 1 || freed != &sta || n_cancel != 1)
    { puts("a station that never reached ESTAB is not reported once and freed once"); return 1; }
    if (memcmp(reported, mac, 6) != 0) { puts("the address was reported after the station was freed"); return 1; }
    memcpy(sta.addr, mac, 6);
    sta.flags = WLAN_STA_AUTH | WLAN_STA_ASSOC;
    mesh_mpm_fsm_restart(&wpa_s, &sta);
    if (n_fail != 1 || n_free != 2) { puts("a station that was ESTAB is held off, or not freed"); return 1; }
    return 0;
}
EOF
why=""
if ${CC:-cc} -std=gnu11 -w -I"$T" -o "$T/t" "$T/t.c" 2>/dev/null && why=$("$T/t"); then
  ok "mesh_mpm_fsm_restart: a station that never reached ESTAB is reported, then freed; an ESTAB one only freed"
else
  bad "mesh_mpm_fsm_restart against stubs: ${why:-did not build or run}"
fi
rm -rf "$T"
if awk '/^static void plink_timer\(/,/^}/' "$M" | awk '/case PLINK_HOLDING:/ {h=1; next}
        h && /(case [A-Z_]+:|default:)/ {h=0} h && /mesh_mpm_fsm_restart\(wpa_s, sta\);/ {r=1}
        END {exit r ? 0 : 1}' && \
   [ "$(grep -c '|= *WLAN_STA_ASSOC' "$M")" = "1" ] && \
   awk '/^static void mesh_mpm_plink_estab\(/,/^}/' "$M" | grep -q 'sta->flags |= WLAN_STA_ASSOC;'; then
  ok "the HOLDING timer restarts the FSM, and only mesh_mpm_plink_estab sets the ESTAB marker it reads"
else
  bad "mesh_mpm.c: the HOLDING timer no longer restarts the FSM, or WLAN_STA_ASSOC is set outside mesh_mpm_plink_estab"
fi

# 24. The no-peers diagnosis counts over a window (main/mesh_diag.c, tested in
#     test_mesh_diag), wired on the target only: the watchdog rolls it after each
#     report and resets it on every tick with a peer; the report and AT+MESHCFG?
#     read the windowed counts, never the raw totals; and the totals are read in
#     the same critical section that rolls, resets or reads the window.
MC=../../../main/mesh.c
if awk '/^static void mesh_probe_burst_task\(/,/^}/' "$MC" | \
     awk '/mmwlan_mesh_get_peer_count\(\) > 0/ {p=1} p && /diag_win_\(DIAG_WIN_RESET, NULL\);/ {r=1}
          /mesh_report_unpeered\(0\);/ {u=NR} u && NR == u + 1 && /diag_win_\(DIAG_WIN_ROLL, NULL\);/ {o=1}
          END {exit (r && o) ? 0 : 1}' && \
   awk '/^static void mesh_report_unpeered\(/,/^}/' "$MC" | \
     awk '/warthog_mesh_diag_windowed\(&in\);/ {w=NR} /warthog_mesh_diagnose\(&in\)/ {if (w) d=NR}
          END {exit (w && d) ? 0 : 1}' && \
   awk '/"\+MESHCFG: %s\\r\\n"/ {f=1} END {exit f ? 0 : 1}' "$A" && \
   awk '/warthog_mesh_diag_windowed\(&in\);/ {w=NR} /warthog_mesh_diagnose\(&in\)/ {if (w && NR > w) d=1}
        END {exit d ? 0 : 1}' "$A" && \
   ! grep -Eq '\.(mesh_probes|floor_skips|floor_passes)[[:space:]]*=' "$A" && \
   awk '/^static void diag_win_\(/ {f=1} f && /^}/ {f=0; next}
        !f && /\.(mesh_probes|floor_skips|floor_passes)[[:space:]]*=/ {b=1} END {exit b}' "$MC" && \
   awk '/^static void diag_win_\(/,/^}/' "$MC" | \
     awk '/portENTER_CRITICAL\(&s_diag_mux\);/ {e=NR} /g_warthog_prq_named,/ && e {r=NR}
          /warthog_mesh_diag_window_(fill|roll|reset)\(/ && r {c++} /portEXIT_CRITICAL\(&s_diag_mux\);/ && c == 3 {x=1}
          END {exit x ? 0 : 1}'; then
  ok "the no-peers report and AT+MESHCFG? read the window, rolled after each report and reset while peered"
else
  bad "main/mesh.c or at.c: the diagnosis window is not rolled, reset or read as it must be"
fi

# 25. Every task that transmits runs mmdrv_tx_frame: its periodic mmdrv_tx_frame# line
#     stays at DBG (printf_blackhole at the default level), or printf's frames land on
#     top of whatever sent, lwIP's 3.5 KB tcpip task included.
DRV=../../halow/components/mm-iot-sdk/framework/morselib/src/driver/driver.c
lvl=$(awk '/^int mmdrv_tx_frame\(/,/^}/' "$DRV" | grep -E 'MMLOG_[A-Z]+\("mmdrv_tx_frame#' | sed -E 's/.*(MMLOG_[A-Z]+).*/\1/')
if [ "$lvl" = "MMLOG_DBG" ]; then
  ok "mmdrv_tx_frame's periodic diagnostic is DBG-only"
else
  bad "mmdrv_tx_frame's periodic diagnostic logs at ${lvl:-an unknown level}: printf on every sender's stack"
fi

[ $fail -eq 0 ] && echo "GLUE INVARIANTS OK" || echo "GLUE INVARIANTS FAILED"
exit $fail
