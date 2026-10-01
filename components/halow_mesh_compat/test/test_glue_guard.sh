#!/bin/sh
# The forwarding glue (umac_mesh_fwd_glue.c) is the one mesh file no host test
# can link: it needs mmosal, mmpkt, mmdrv and the datapath. Its DECISIONS are in
# umac_mesh_fwd.c, which the unit tests and the simulator drive. What is left
# here is locking and call ordering -- invariants a reviewer found broken once
# and a unit test cannot see. Checked structurally, so a revert fails the suite
# instead of waiting for a radio. The batman sections (26 on) do the same for the
# firmware glue in main/, and run it against stubs where it can run on the host.
set -u
G=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/mesh/umac_mesh_fwd_glue.c
M=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/mesh/umac_mesh.c
fail=0
ok()   { echo "ok   $1"; }
bad()  { echo "FAIL $1"; fail=1; }
# Prints why not, or nothing: every function in $1 calling lwIP ($3, an awk regex without
# backslashes, which awk -v strips) is one esp_netif_tcpip_exec runs, and $2 is among them.
lwip_only_in_tcpip() {
  if ! lw_fns=$(awk -v re="$3" '
      /^[a-z].*\(.*\)$/ || /^[a-z].*\(.*[^;]$/ { if (match($0, /[a-z_0-9]+\(/)) fn = substr($0, RSTART, RLENGTH - 1) }
      /^}/ { fn = "" }
      $0 ~ re && fn != "" && $0 !~ /^ *\/[*\/]/ { print fn }' "$1"); then
    printf ' the lwIP-call scan of %s did not run;' "${1##*/}"
    return
  fi
  lw_fns=$(printf '%s\n' "$lw_fns" | sort -u)
  case " $(echo $lw_fns) " in
    *" $2 "*) ;;
    *) printf ' the lwIP-call scan of %s no longer sees %s;' "${1##*/}" "$2" ;;
  esac
  for lw_f in $lw_fns; do
    grep -q "esp_netif_tcpip_exec($lw_f," "$1" || printf ' %s calls lwIP but is not run by esp_netif_tcpip_exec;' "$lw_f"
  done
}

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
#     unestab=, qfail=, the mgmt tx, path-selection and mgmt gp groups, gp chip= included). main/at.c
#     is only scraped for storage on the host, so nothing runs it: pair each
#     conversion in the format with its argument by position.
A=../../../main/at.c
fwdstat_slot() {
  awk -v want="$1=%lu" -v n="${2:-1}" '/"MESHFWDSTAT"\) == 0 && terminator == .\?./ {on=1}
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
    out = arg[k + 1]; for (j = 2; j <= n; j++) out = out " " arg[k + j]
    print out
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
# The Block Ack protection group, whose labels repeat others': matched whole, three in order.
slot=$(fwdstat_slot "mgmt tx chip=%lu host=%lu drop" 3)
if [ "$slot" = "g_warthog_mgmt_tx_chip g_warthog_mgmt_tx_host g_warthog_mgmt_tx_drop" ]; then
  ok "AT+MESHFWDSTAT? prints the mgmt tx chip/host/drop counters in their slots"
else
  bad "AT+MESHFWDSTAT?'s mgmt tx chip=/host=/drop= slots print: ${slot:-nothing}"
fi
# Group path selection (group-addressed privacy), received, sent and at the CCMP layer.
for grp in "hwmp prot=%lu unprotected=%lu unestab=%lu gp=%lu mmie=%lu nommie|6|g_warthog_hwmp_prot g_warthog_hwmp_unprotected g_warthog_hwmp_unestab g_warthog_hwmp_gp g_warthog_hwmp_mmie g_warthog_hwmp_nommie" \
           "hwmp tx prot=%lu gp=%lu plain|3|g_warthog_hwmp_tx_prot g_warthog_hwmp_tx_gp g_warthog_hwmp_tx_plain" \
           "mgmt gp nodec=%lu own=%lu key=%lu replay=%lu chip|5|g_warthog_mgmt_gp_nodec g_warthog_mgmt_gp_own g_warthog_mgmt_gp_key g_warthog_mgmt_gp_replay g_warthog_mgmt_gp_chip"; do
  label=${grp%%|*}; rest=${grp#*|}; n=${rest%%|*}; want=${rest#*|}
  slot=$(fwdstat_slot "$label" "$n")
  if [ "$slot" = "$want" ]; then
    ok "AT+MESHFWDSTAT? prints ${label%%=*} ... in their slots"
  else
    bad "AT+MESHFWDSTAT?'s ${label%%=*} slots print: ${slot:-nothing} (want $want)"
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

# 11c. AT+FILTSTAT? prints the receive filter's reason 9 (a mesh unicast whose RA is another
#      station) as not_ours=, fits its buffer at its longest, and the histogram behind it has
#      a slot for every reason umac_datapath.c counts, in at.c's storage and in its extern.
D=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/datapath/umac_datapath.c
filt=$(awk '/^static void cmd_filtstat\(void\)/ {on=1}
  on && /char buf\[[0-9]+\]/ { match($0, /\[[0-9]+\]/); size = substr($0, RSTART + 1, RLENGTH - 2) + 0 }
  on && /snprintf\(buf, sizeof\(buf\),/ { args = 1 }
  args {
    l = $0
    while (match(l, /"[^"]*"/)) { fmt = fmt substr(l, RSTART + 1, RLENGTH - 2); l = substr(l, RSTART + RLENGTH) }
    while (match(l, /g_warthog_[A-Za-z0-9_]+(\[[0-9]+\])?/)) { arg[++na] = substr(l, RSTART, RLENGTH); l = substr(l, RSTART + RLENGTH) }
  }
  on && /cdc_write\(buf\);/ { exit }
  END {
    i = index(fmt, " not_ours=%lu")
    pre = substr(fmt, 1, i); k = gsub(/%lu/, "", pre)
    f = fmt; gsub(/\\[rn]/, "x", f); lu = gsub(/%lu/, "", f)
    if (i == 0 || size == 0 || lu != na || index(f, "%")) { print "unparsed"; exit }
    print arg[k + 1] " " length(f) + 10 * lu + 1 " " size
  }' "$A")
read -r f_slot f_need f_size f_more <<FILT
$filt
FILT
if [ "$f_slot" = "g_warthog_filt_hist[9]" ] && [ -z "$f_more" ] && [ "${f_need:-x}" -le "${f_size:-0}" ] 2>/dev/null; then
  ok "AT+FILTSTAT? prints $f_slot as not_ours= and fits its buffer at its longest ($f_need/$f_size bytes)"
else
  bad "AT+FILTSTAT?'s not_ours= slot or its fit: ${filt:-nothing parsed} (want g_warthog_filt_hist[9], need <= size)"
fi
h_at=$(sed -n 's/^volatile uint32_t g_warthog_filt_hist\[\([0-9][0-9]*\)\].*/\1/p' "$A")
h_dp=$(sed -n 's/^extern volatile uint32_t g_warthog_filt_hist\[\([0-9][0-9]*\)\];.*/\1/p' "$D")
h_top=$(grep -o 'g_warthog_filt_hist\[[0-9][0-9]*\]++' "$D" | tr -dc '0-9\n' | sort -n | tail -1)
if [ -n "$h_at" ] && [ "$h_at" = "$h_dp" ] && [ "${h_top:-99}" -lt "$h_at" ] && [ "$h_top" -eq 9 ]; then
  ok "g_warthog_filt_hist[$h_at] in at.c and umac_datapath.c holds every filter reason (highest $h_top)"
else
  bad "g_warthog_filt_hist: at.c [${h_at:-?}], umac_datapath.c extern [${h_dp:-?}], highest reason counted ${h_top:-none} (want 9, below both)"
fi
# 11d. Its second line is the census of mesh unicast management addressed to another station
#      (counted, not dropped): mgmt_nours= and the last one's 16 octets, as at.c stores them.
mn_body=$(awk '/^static void cmd_filtstat\(void\)/ {on=1} on {print} on && /^}/ {exit}' "$A")
mn_len=$(sed -n 's/^volatile uint8_t g_warthog_filt_mgmt_nours_hdr\[\([0-9][0-9]*\)\].*/\1/p' "$A")
if printf '%s\n' "$mn_body" | grep -q '"+FILTSTAT: mgmt_nours=%lu last="' &&
   printf '%s\n' "$mn_body" | grep -q '(unsigned long)g_warthog_filt_mgmt_nours)' &&
   printf '%s\n' "$mn_body" | grep -q "i < ${mn_len:-x}; i++" &&
   printf '%s\n' "$mn_body" | grep -q 'g_warthog_filt_mgmt_nours_hdr\[i\]' &&
   [ "$(printf '%s\n' "$mn_body" | grep -c 'cdc_write(buf);')" -eq 2 ]; then
  ok "AT+FILTSTAT? prints mgmt_nours= and all $mn_len octets of its snapshot on a second line"
else
  bad "AT+FILTSTAT? does not print g_warthog_filt_mgmt_nours and its ${mn_len:-?}-octet snapshot on a second line"
fi

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

# 26. BATMAN_V member mode (main/bat_port.c). Every AT+BAT* verb renders its own table
#     through cmd_bat_render (AT+BATO= and AT+BATTG= through cmd_bat_render_mac, with their
#     own usage), which prints only through the waiting writer and always hands the render
#     buffer back (render_done) before it replies.
BP=../../../main/bat_port.c
bad26=$(awk '/^static void dispatch\(/,/^}/' "$A" | awk '
  BEGIN { k["BATN"] = "NEIGH"; k["BATO"] = "ORIG"; k["BATTG"] = "TT_GLOBAL"; k["BATTL"] = "TT_LOCAL"; k["BATSTAT"] = "STAT" }
  /strcasecmp\(verb, "BAT[A-Z]*"\)/ { v=$0; sub(/.*"BAT/, "BAT", v); sub(/".*/, "", v); q=($0 ~ /terminator == .\?./); want=1; next }
  want && q { if ($0 !~ ("^ *cmd_bat_render\\(BAT_RENDER_" k[v] ", NULL\\);$")) print v "?"; want=0 }
  want { if ($0 !~ ("^ *cmd_bat_render_mac\\(BAT_RENDER_" k[v] ", args, \"usage: AT\\+" v "=<mac>\"\\);$")) print v "="; want=0 }')
nbat=$(awk '/^static void dispatch\(/,/^}/' "$A" | grep -c 'strcasecmp(verb, "BAT')
neq=$(awk '/^static void dispatch\(/,/^}/' "$A" | grep -c 'strcasecmp(verb, "BAT[OT]G*") == 0 && terminator == .=.')
if [ -z "$bad26" ] && [ "$nbat" = 7 ] && [ "$neq" = 2 ] && \
   awk '/^static void cmd_bat_render\(/,/^}/' "$A" | \
     awk '/cdc_write_nowait|cdc_out_|tud_cdc|tinyusb_cdcacm/ {b=1} /cdc_write\(out\);/ {w=NR}
          /warthog_bat_port_render_done\(\);/ {if (w && NR > w) d=NR} /reply_ok\(\);/ {if (d && NR > d) o=1}
          END {exit (!b && o) ? 0 : 1}'; then
  ok "AT+BATN/BATO/BATTG/BATTL/BATSTAT? and AT+BATO/BATTG=<mac> render through cmd_bat_render, which uses cdc_write and releases the buffer"
else
  bad "a batman render verb bypasses cmd_bat_render (${bad26:-$nbat verbs, $neq with =}) or cmd_bat_render writes around cdc_write / skips render_done"
fi

# 27. The AT+MESHBATMAN=, AT+MESHFWD= and AT+MESHBRIDGE= setters ask bat_mode_check
#     before they store, so the console refuses exactly what boot would refuse; the first
#     store is what counts. MESHFWD=/MESHBRIDGE= reject v > 1 before both: (uint8_t)257 is 1.
setter_checks() {
  awk -v v="$1" '/^static void dispatch\(/ {d=1} d && $0 ~ ("strcasecmp\\(verb, \"" v "\"\\) == 0 && terminator == .=.") {on=1}
    on && /v > 1/ && !r {r=NR} on && /bat_mode_check\(/ && !c {c=NR}
    on && /warthog_cfg_set_mesh_/ {if (r && c && r < c) print "ok"; else print "late"; exit}
    on && /^    } else if/ && !/strcasecmp\(verb, "/ {exit}' "$A"
}
r27=""
for v in MESHFWD MESHBRIDGE; do [ "$(setter_checks $v)" = ok ] || r27="$r27 $v"; done
if ! awk '/^static void cmd_meshbatman_set\(/,/^}/' "$A" | \
       awk '/bat_mode_check\(/ && !c {c=NR} /warthog_cfg_set_mesh_batman\(/ && !s {s=NR} END {exit (c && s && c < s) ? 0 : 1}' || \
   ! awk '/^static void dispatch\(/,/^}/' "$A" | grep -q 'cmd_meshbatman_set(args);'; then
  r27="$r27 MESHBATMAN"
fi
if [ -z "$r27" ]; then
  ok "AT+MESHBATMAN=, AT+MESHFWD= and AT+MESHBRIDGE= call bat_mode_check (and bound the value) before storing"
else
  bad "setter(s) store without asking bat_mode_check first:$r27"
fi

# 28. The AT+MESHCFG? mode line and batman line fit line[320] at their longest: both
#     builders are compiled out of main/at.c and run for every flag with every %u at
#     its maximum and the longest refusal reason. They also say the right thing: yes
#     with the counts while running, refused(<why>) only for a stored flag this boot's
#     start refused, no otherwise; and the self= line only prints while running.
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
awk '/^struct meshcfg_mode \{/,/^};/' "$A" > "$T/fn.c"
awk '/^static int meshcfg_mode_line_\(/,/^}/' "$A" >> "$T/fn.c"
awk '/^static int meshcfg_bat_line_\(/,/^}/' "$A" >> "$T/fn.c"
size28=$(awk '/^static void cmd_meshcfg\(void\)/ {on=1} on && /char line\[[0-9]+\];/ {match($0, /\[[0-9]+\]/); print substr($0, RSTART + 1, RLENGTH - 2); exit}' "$A")
cat > "$T/t.c" <<'EOF'
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bat.h"
#include "fn.c"
int main(int argc, char **argv)
{
    unsigned size = (unsigned)atoi(argv[1]);
    const char *reasons[] = { "ok", "off", "mesh-off", "fwd", "bridge", "sae-no-host-ccmp", "nomem", "init-failed",
                              "mesh-failed" };
    char buf[2048];
    struct meshcfg_mode y;
    memset(&y, 0, sizeof(y));
    y.bat_running = y.bat_stored = 1; y.bat_reason = "ok"; y.neigh = 3; y.routes = 7;
    memcpy(y.soft, "\x06\x11\x22\x33\x44\x55", 6); memcpy(y.self, "\x02\xaa\xbb\xcc\xdd\xee", 6);
    meshcfg_mode_line_(buf, sizeof(buf), &y);
    if (!strstr(buf, "routing=batman_v ") || !strstr(buf, "batman=yes(neigh=3 routes=7 soft=06:11:22:33:44:55)\r\n")) {
        printf("content (running) %s", buf); return 0;
    }
    meshcfg_bat_line_(buf, sizeof(buf), &y);
    if (!strstr(buf, "batman self=02:aa:bb:cc:dd:ee soft=06:11:22:33:44:55 ")) { printf("content (self) %s", buf); return 0; }
    memset(&y, 0, sizeof(y));
    y.bat_stored = 1;
    for (unsigned r = 2; r < sizeof(reasons) / sizeof(reasons[0]); r++) {
        char want[64];
        snprintf(want, sizeof(want), "batman=refused(%s)\r\n", reasons[r]);
        y.bat_reason = reasons[r];
        meshcfg_mode_line_(buf, sizeof(buf), &y);
        if (!strstr(buf, want)) { printf("content (stored, %s) %s", reasons[r], buf); return 0; }
    }
    y.bat_reason = "off";
    meshcfg_mode_line_(buf, sizeof(buf), &y);
    if (!strstr(buf, "batman=no\r\n")) { printf("content (stored, off) %s", buf); return 0; }
    y.bat_stored = 0; y.bat_reason = "fwd";
    meshcfg_mode_line_(buf, sizeof(buf), &y);
    if (!strstr(buf, "batman=no\r\n")) { printf("content (not stored) %s", buf); return 0; }
    int worst = 0, lines = 0;
    for (int m = 0; m < 64; m++) {
        for (unsigned r = 0; r < sizeof(reasons) / sizeof(reasons[0]); r++) {
            struct meshcfg_mode x;
            memset(&x, 0xff, sizeof(x));
            x.fwd = m & 1; x.bridge_active = (m >> 1) & 1; x.bridge_stored = (m >> 2) & 1;
            x.bat_running = (m >> 3) & 1; x.bat_stored = (m >> 4) & 1; x.grp_std = (m >> 5) & 1;
            x.bat_reason = reasons[r];
            int n = meshcfg_mode_line_(buf, sizeof(buf), &x);
            if (n > worst) worst = n;
            n = meshcfg_bat_line_(buf, sizeof(buf), &x);
            if (n > worst) worst = n;
            lines += 2;
        }
    }
    (void)argc;
    printf("%s %d/%u (%d lines)\n", worst + 1 <= (int)size ? "ok" : "short", worst + 1, size, lines);
    return 0;
}
EOF
fit28=""
if [ -n "$size28" ] && ${CC:-cc} -std=gnu11 -w -I"$T" -I../../../main/bat -o "$T/t" "$T/t.c" 2>/dev/null; then
  fit28=$("$T/t" "$size28")
fi
rm -rf "$T"
if ! awk '/^static void cmd_meshcfg\(void\)/,/^}/' "$A" | \
     awk '/if \(m\.bat_running\) \{/ {g=1; next} g && /meshcfg_bat_line_\(/ {ok=1} !g && /meshcfg_bat_line_\(/ {b=1}
          /^    }/ {g=0} END {exit (ok && !b) ? 0 : 1}'; then
  fit28="content: cmd_meshcfg prints the batman self= line outside if (m.bat_running)"
fi
case "$fit28" in
  ok*) ok "AT+MESHCFG?'s mode and batman lines say the right thing and fit line[$size28] at their longest (${fit28#ok })" ;;
  *)   bad "AT+MESHCFG?'s mode or batman line is wrong or can outgrow line[${size28:-?}]: ${fit28:-did not build or run}" ;;
esac

# 29. Registering any RX callback silently unregisters the extended one, so outside
#     morselib only mmhalow.c (halow_rx, at mmhalow_init) and bat_port.c (the batman
#     hook, after it) may call mmwlan_register_rx_*, and bat_port.c only the ext one.
regs=$(grep -rl 'mmwlan_register_rx_' ../../../main ../../halow/*.c 2>/dev/null | sed 's|.*/||' | LC_ALL=C sort | tr '\n' ' ')
if [ "$regs" = "bat_port.c mmhalow.c " ] && \
   [ "$(grep -c 'mmwlan_register_rx_' "$BP")" = 1 ] && grep -q 'mmwlan_register_rx_pkt_ext_cb(MMWLAN_VIF_UNSPECIFIED, bat_port_rx_ext' "$BP"; then
  ok "only mmhalow.c and bat_port.c register RX callbacks; bat_port.c registers only the extended one"
else
  bad "RX callbacks are registered from: ${regs:-nowhere} (want bat_port.c mmhalow.c, the ext hook only in bat_port.c)"
fi

# 30. AT+SWCCMP=0 would end all group RX from peers (their ELP/OGM/BCAST) with no
#     error: the setter asks warthog_bat_port_running() before it writes the flag.
if awk '/strcasecmp\(verb, "SWCCMP"\) == 0 && terminator == .=./ {on=1}
        on && /warthog_bat_port_running\(\)/ {r=NR} on && /g_warthog_host_ccmp_on = / {if (r) ok=1; exit}
        END {exit ok ? 0 : 1}' "$A"; then
  ok "AT+SWCCMP= consults warthog_bat_port_running() before switching host CCMP"
else
  bad "AT+SWCCMP= can switch host CCMP off under a running batman engine"
fi

# 31. Every engine source the host tests link is in the firmware build too.
CM=../../../main/CMakeLists.txt
miss31=""
for f in ../../../main/bat/*.c; do
  b=$(basename "$f")
  grep -q "\"bat/$b\"" "$CM" || miss31="$miss31 bat/$b"
done
for f in bat_mode.c bat_port.c; do grep -q "\"$f\"" "$CM" || miss31="$miss31 $f"; done
if [ -z "$miss31" ] && awk '/INCLUDE_DIRS/ {i=1} i && /"bat"/ {f=1} END {exit f ? 0 : 1}' "$CM"; then
  ok "main/CMakeLists.txt builds every main/bat/*.c, bat_mode.c and bat_port.c, with bat/ on the include path"
else
  bad "main/CMakeLists.txt is missing:${miss31:- the bat include dir}"
fi

# 32. With batman off nothing is allocated: warthog_bat_port_start returns on any
#     refusal before its first allocation, and mesh.c starts it before mmwlan_mesh_enable.
if awk '/^esp_err_t warthog_bat_port_start\(/,/^}/' "$BP" | \
     awk '/if \(s_reason != BAT_MODE_OK\)/ {g=NR} /return ESP_OK;/ && g && !r {r=NR}
          /(calloc|malloc|xQueueCreate|xSemaphoreCreate|xTaskCreate)\(/ && !a {a=NR}
          END {exit (g && r && a && r < a) ? 0 : 1}' && \
   awk '/^void warthog_mesh_smoke_test\(/,/^}/' ../../../main/mesh.c | \
     awk '/warthog_bat_port_start\(\);/ {s=NR} /= mmwlan_mesh_enable\(/ {if (s) e=1} END {exit e ? 0 : 1}'; then
  ok "batman allocates nothing unless bat_mode_check says ok, and starts before mmwlan_mesh_enable"
else
  bad "warthog_bat_port_start allocates before its refusal return, or runs after mmwlan_mesh_enable"
fi

# 33. The batman AT setters, parsers and their NVS storage, compiled out of main/at.c and
#     main/cfg.c and run against a fake NVS: every refusal of the design's table with its
#     exact reply, the value stored only on success, out-of-range values refused before
#     the (uint8_t) cast, each getter reading the key its setter writes, and AT+MESHBATMAN?
#     adding the bat0 line, whole at its longest, while batman runs.
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
C=../../../main/cfg.c
{
  for f in reply_ok reply_error trim bat_refusal_ cmd_meshbatman_set cmd_meshbatman_query cmd_meshbattp_set; do
    awk -v f="$f" '$0 ~ ("^static [a-z ]+[*]?" f "\\(") && !/;$/ {p=1} p {print} p && /^}/ {exit}' "$A"
  done
  for v in MESHFWD MESHBRIDGE SWCCMP; do
    echo "static void branch_$v(char *args) {"
    awk -v v="$v" '/^static void dispatch\(/ {d=1}
      d && $0 ~ ("strcasecmp\\(verb, \"" v "\"\\) == 0 && terminator == .=.") {on=1; next}
      on && /^    } else if/ {exit} on {print}' "$A"
    echo "}"
  done
} > "$T/at_fn.c"
for f in batman battp fwd bridge; do
  for g in get set; do
    awk -v f="warthog_cfg_${g}_mesh_$f" '$0 ~ ("^[a-z_0-9]+ " f "\\(") {p=1} p {print} p && /^}/ {exit}' "$C"
  done
done > "$T/cfg_fn.c"
cat > "$T/t.c" <<'EOF'
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bat_mode.h"
typedef int esp_err_t;
typedef int nvs_handle_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NVS_NOT_FOUND 0x1102
enum { NVS_READONLY, NVS_READWRITE };
static const char *NS = "warthog";
static struct { char k[16]; uint32_t v; int used; } nvs[8];
static int kv(const char *k, int create)
{
    for (int i = 0; i < 8; i++) if (nvs[i].used && !strcmp(nvs[i].k, k)) return i;
    for (int i = 0; create && i < 8; i++) if (!nvs[i].used) { nvs[i].used = 1; snprintf(nvs[i].k, 16, "%s", k); return i; }
    return -1;
}
static esp_err_t nvs_open(const char *ns, int m, nvs_handle_t *h) { (void)ns; (void)m; *h = 1; return ESP_OK; }
static void nvs_close(nvs_handle_t h) { (void)h; }
static esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
static esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v)
{ (void)h; int i = kv(k, 0); if (i < 0) return ESP_ERR_NVS_NOT_FOUND; *v = (uint8_t)nvs[i].v; return ESP_OK; }
static esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v)
{ (void)h; if (strlen(k) > 15) return ESP_FAIL; nvs[kv(k, 1)].v = v; return ESP_OK; }
static esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *v)
{ (void)h; int i = kv(k, 0); if (i < 0) return ESP_ERR_NVS_NOT_FOUND; *v = nvs[i].v; return ESP_OK; }
static esp_err_t nvs_set_u32(nvs_handle_t h, const char *k, uint32_t v)
{ (void)h; if (strlen(k) > 15) return ESP_FAIL; nvs[kv(k, 1)].v = v; return ESP_OK; }
#include "cfg_fn.c"
static char out_[1024];
static void cdc_write(const char *s) { strncat(out_, s, sizeof(out_) - strlen(out_) - 1); }
static bool b_sae, b_ccmp, running;
volatile uint32_t g_warthog_host_ccmp_on;
static bool warthog_bat_port_sae_build(void) { return b_sae; }
static bool warthog_bat_port_host_ccmp_build(void) { return b_ccmp; }
static bool warthog_bat_port_running(void) { return running; }
static int f_reason = BAT_MODE_MESH_FAILED;
static int warthog_bat_port_reason(void) { return f_reason; }
static int warthog_mesh_bat0_line(char *buf, size_t len)
{ return snprintf(buf, len, "+MESHBATMAN: bat0 addr=leased%0*d\r\n", BAT_MODE_BAT0_LINE - 32, 7); }
#include "at_fn.c"
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf(__VA_ARGS__); printf(" [%s] | ", out_); } } while (0)
static void run(void (*f)(char *), const char *arg) { char a[48]; snprintf(a, sizeof(a), "%s", arg); out_[0] = 0; f(a); }
static void set(const char *k, uint32_t v) { nvs[kv(k, 1)].v = v; }
static const char *E_FWD = "+ERR: AT+MESHFWD=1 is set; batman needs 802.11s forwarding off\r\nERROR\r\n";
static const char *E_BR = "+ERR: AT+MESHBRIDGE=1 is set; batman mode NATs the tethered side\r\nERROR\r\n";
static const char *E_SAE = "+ERR: this build cannot hear peers' group frames under SAE; "
                           "use warthog-mesh-sae-swccmp or an open mesh\r\nERROR\r\n";
static const char *E_BAT = "+ERR: AT+MESHBATMAN=1 is set; batman needs it off\r\nERROR\r\n";
int main(void)
{
    for (int m = 0; m < 16; m++) {
        set("mesh_fwd", m & 1); set("mesh_br", (m >> 1) & 1); b_sae = (m >> 2) & 1; b_ccmp = (m >> 3) & 1; set("mesh_bat", 0);
        const char *want = (b_sae && !b_ccmp) ? E_SAE : (m & 1) ? E_FWD : ((m >> 1) & 1) ? E_BR : NULL;
        run(cmd_meshbatman_set, "1");
        if (want) CHECK(!strcmp(out_, want) && warthog_cfg_get_mesh_batman() == 0, "MESHBATMAN=1 row %d not refused as the table says", m);
        else CHECK(!strcmp(out_, "+MESHBATMAN: stored; takes effect on next boot (AT+RESET)\r\nOK\r\n") &&
                   warthog_cfg_get_mesh_batman() == 1, "MESHBATMAN=1 row %d not stored", m);
        run(cmd_meshbatman_set, "0");
        CHECK(strstr(out_, "OK\r\n") && warthog_cfg_get_mesh_batman() == 0, "MESHBATMAN=0 row %d not stored", m);
    }
    set("mesh_fwd", 0); set("mesh_br", 0); b_sae = b_ccmp = false;
    static const char *bad01[] = { "1x", "", "2", "10", "-1", "01" };
    for (unsigned i = 0; i < sizeof(bad01) / sizeof(bad01[0]); i++) {
        run(cmd_meshbatman_set, bad01[i]);
        CHECK(!strcmp(out_, "+ERR: usage: AT+MESHBATMAN=<0|1>\r\nERROR\r\n") && warthog_cfg_get_mesh_batman() == 0,
              "AT+MESHBATMAN=%s accepted", bad01[i]);
    }
    run(cmd_meshbatman_set, " 1 ");
    CHECK(warthog_cfg_get_mesh_batman() == 1, "AT+MESHBATMAN= 1  (spaces) not stored");
    out_[0] = 0;
    cmd_meshbatman_query();
    CHECK(!strcmp(out_, "+MESHBATMAN: stored=1 running=0 reason=mesh-failed\r\nOK\r\n"), "AT+MESHBATMAN? wrong");
    running = true; f_reason = BAT_MODE_OK;
    out_[0] = 0;
    cmd_meshbatman_query();
    {
        char want[512], bat0[BAT_MODE_BAT0_LINE + 8];
        (void)warthog_mesh_bat0_line(bat0, sizeof(bat0));
        snprintf(want, sizeof(want), "+MESHBATMAN: stored=1 running=1 reason=ok\r\n%sOK\r\n", bat0);
        CHECK(strlen(bat0) == BAT_MODE_BAT0_LINE - 1 && !strcmp(out_, want),
              "AT+MESHBATMAN? while running: the bat0 line at its longest, whole, before OK");
    }
    running = false; f_reason = BAT_MODE_MESH_FAILED;
    set("mesh_bat", 7);
    CHECK(warthog_cfg_get_mesh_batman() == 0, "a stored 7 reads as on");

    static const struct { const char *in; int ok; uint32_t v; } tp[] = {
        { "0", 1, 0 }, { "4294967295", 1, 4294967295u }, { "25", 1, 25 }, { " 5", 1, 5 }, { "4294967296", 0, 0 },
        { "-1", 0, 0 }, { "", 0, 0 }, { "1x", 0, 0 }, { "+5", 0, 0 }, { "0x10", 0, 0 }, { "99999999999999999999", 0, 0 } };
    for (unsigned i = 0; i < sizeof(tp) / sizeof(tp[0]); i++) {
        uint32_t before = warthog_cfg_get_mesh_battp();
        run(cmd_meshbattp_set, tp[i].in);
        CHECK(tp[i].ok ? (strstr(out_, "OK\r\n") && warthog_cfg_get_mesh_battp() == tp[i].v)
                       : (strstr(out_, "+ERR: usage") && warthog_cfg_get_mesh_battp() == before),
              "AT+MESHBATTP=%s", tp[i].in);
    }

    void (*br[2])(char *) = { branch_MESHFWD, branch_MESHBRIDGE };
    static const char *key[2] = { "mesh_fwd", "mesh_br" }, *name[2] = { "MESHFWD", "MESHBRIDGE" };
    uint8_t (*get[2])(void) = { warthog_cfg_get_mesh_fwd, warthog_cfg_get_mesh_bridge };
    static const char *wrap[] = { "257", "513", "65281", "4294967041", "-255", "2" };
    for (int k = 0; k < 2; k++) {
        set("mesh_fwd", 0); set("mesh_br", 0); set("mesh_bat", 1);
        run(br[k], "1");
        CHECK(!strcmp(out_, E_BAT) && get[k]() == 0, "AT+%s=1 not refused with AT+MESHBATMAN=1", name[k]);
        for (unsigned i = 0; i < sizeof(wrap) / sizeof(wrap[0]); i++) {
            char want[64];
            snprintf(want, sizeof(want), "+ERR: usage: AT+%s=<0|1>\r\nERROR\r\n", name[k]);
            run(br[k], wrap[i]);
            CHECK(!strcmp(out_, want) && get[k]() == 0, "AT+%s=%s not refused as usage", name[k], wrap[i]);
        }
        run(br[k], "0");
        CHECK(strstr(out_, "stored") && get[k]() == 0, "AT+%s=0 refused", name[k]);
        set("mesh_bat", 0);
        run(br[k], "1");
        CHECK(strstr(out_, "stored") && get[k]() == 1, "AT+%s=1 refused without batman", name[k]);
        set(key[k], 9);
        CHECK(get[k]() == 0, "a stored 9 in %s reads as on", key[k]);
    }
    /* A stored AT+MESHBATMAN=1 is inert on warthog-mesh-sae / -nochipkey, so it blocks neither
     * there; on a host-CCMP SAE build it does. */
    for (int ccmp = 0; ccmp < 2; ccmp++) {
        b_sae = true; b_ccmp = ccmp;
        for (int k = 0; k < 2; k++) {
            set("mesh_fwd", 0); set("mesh_br", 0); set("mesh_bat", 1);
            run(br[k], "1");
            if (ccmp) CHECK(!strcmp(out_, E_BAT) && get[k]() == 0, "AT+%s=1 not refused on a host-CCMP SAE build", name[k]);
            else CHECK(strstr(out_, "stored") && get[k]() == 1, "AT+%s=1 refused on an SAE build without host CCMP", name[k]);
        }
    }
    set("mesh_fwd", 0); set("mesh_br", 0); set("mesh_bat", 0);

    b_sae = b_ccmp = true; running = true; g_warthog_host_ccmp_on = 1;
    run(branch_SWCCMP, "0");
    CHECK(!strcmp(out_, "+ERR: batman is running; host CCMP must stay on\r\nERROR\r\n") && g_warthog_host_ccmp_on == 1,
          "AT+SWCCMP=0 under a running engine");
    run(branch_SWCCMP, "1");
    CHECK(strstr(out_, "host ccmp ON") && g_warthog_host_ccmp_on == 1, "AT+SWCCMP=1 refused while running");
    running = false;
    run(branch_SWCCMP, "0");
    CHECK(strstr(out_, "host ccmp OFF") && g_warthog_host_ccmp_on == 0, "AT+SWCCMP=0 refused with batman stopped");
    return fails != 0;
}
EOF
why=""
if ${CC:-cc} -std=gnu11 -w -I"$T" -I../../../main -I../../../main/bat -o "$T/t" "$T/t.c" ../../../main/bat_mode.c \
     2>"$T/cc.log" && why=$("$T/t"); then
  ok "the batman AT setters refuse exactly the design's table, bound their values and store under the keys the getters read"
else
  bad "batman AT setters/storage: ${why:-did not build or run: $(head -3 "$T/cc.log" | tr '\n' ' ')}"
fi
rm -rf "$T"

# 34. main/bat_port.c itself, compiled against pthread-backed FreeRTOS queues, semaphores
#     and tasks, recording ESP-IDF/morselib fakes and a counting engine stub: the start
#     sequence (hook, driver, MTU, soft MAC, engine config, datapath gate, host CCMP), the
#     tx gate and RA, both slot pools under a stuck engine, at most 16 delivered copies held
#     by a stalled lwIP (the rest dropped and counted), the render handshake's abandon
#     and late-finish paths, at.c's cmd_bat_render paging a listing longer than one buffer
#     (a node's rows alone for AT+BATO=/AT+BATTG=<mac>, a stalled cursor refused), the stat line
#     after the last chunk or alone in one more, the engine task's answers about the watched
#     router MAC and the gateways (fresh on a new watch, else every 500 ms), and each refused
#     start allocating nothing. Run with the build's sanitizers under SAN=1.
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
mkdir -p "$T/inc/freertos" "$T/inc/lwip"
for h in esp_err.h esp_heap_caps.h esp_log.h esp_mac.h esp_netif.h esp_netif_net_stack.h esp_random.h esp_timer.h \
         freertos/FreeRTOS.h freertos/queue.h freertos/semphr.h freertos/task.h lwip/netif.h mmhalow.h mmpkt.h \
         mmwlan.h mmwlan_mesh.h; do
  echo '#include "fake.h"' > "$T/inc/$h"
done
cat > "$T/fake.h" <<'EOF'
/* Every ESP-IDF, FreeRTOS and morselib name main/bat_port.c uses; the queues, semaphores
 * and tasks are real (pthreads). 1 tick = 1 ms of the port's clock = 50 us of real time. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef struct fq *QueueHandle_t, *SemaphoreHandle_t;
typedef struct ft *TaskHandle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
void f_crit(int enter);
#define portENTER_CRITICAL(m) ((void)(m), f_crit(1))
#define portEXIT_CRITICAL(m) ((void)(m), f_crit(0))
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t t);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t t);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q);
void vQueueDelete(QueueHandle_t q);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t);
#define vSemaphoreDelete vQueueDelete
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                       TaskHandle_t *out);
void vTaskDelete(TaskHandle_t t);
BaseType_t xTaskNotifyGive(TaskHandle_t t);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t t);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t);
void f_log(const char *fmt, ...);
#define ESP_LOGE(tag, ...) f_log(__VA_ARGS__)
#define ESP_LOGW(tag, ...) f_log(__VA_ARGS__)
#define ESP_LOGI(tag, ...) f_log(__VA_ARGS__)
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
typedef enum { ESP_MAC_EFUSE_FACTORY } esp_mac_type_t;
esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t t);
uint32_t esp_random(void);
int64_t esp_timer_get_time(void);
#define MALLOC_CAP_8BIT 4
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
typedef struct esp_netif_obj esp_netif_t;
typedef struct {
    void *handle;
    esp_err_t (*transmit)(void *h, void *buffer, size_t len);
    esp_err_t (*transmit_wrap)(void *h, void *buffer, size_t len, void *netstack_buffer);
    void (*driver_free_rx_buffer)(void *h, void *buffer);
} esp_netif_driver_ifconfig_t;
void *esp_netif_get_io_driver(esp_netif_t *n);
esp_err_t esp_netif_set_driver_config(esp_netif_t *n, const esp_netif_driver_ifconfig_t *c);
void *esp_netif_get_netif_impl(esp_netif_t *n);
esp_err_t esp_netif_set_mac(esp_netif_t *n, uint8_t mac[]);
esp_err_t esp_netif_tcpip_exec(esp_err_t (*fn)(void *ctx), void *ctx);
esp_err_t esp_netif_receive(esp_netif_t *n, void *buffer, size_t len, void *eb);
struct netif { uint16_t mtu, mtu6; };
struct mmpkt;
struct mmpktview;
struct mmpktview *mmpkt_open(struct mmpkt *p);
void mmpkt_close(struct mmpktview **v);
void mmpkt_append_data(struct mmpktview *v, const uint8_t *d, uint32_t len);
uint8_t *mmpkt_get_data_start(struct mmpktview *v);
uint32_t mmpkt_get_data_length(struct mmpktview *v);
void mmpkt_release(struct mmpkt *p);
enum mmwlan_status { MMWLAN_SUCCESS, MMWLAN_ERROR, MMWLAN_UNAVAILABLE, MMWLAN_NOT_FOUND };
enum mmwlan_vif { MMWLAN_VIF_UNSPECIFIED };
struct mmwlan_rx_metadata { enum mmwlan_vif vif; const uint8_t *ta; };
typedef void (*mmwlan_rx_pkt_ext_cb_t)(struct mmpkt *, const struct mmwlan_rx_metadata *, void *);
enum mmwlan_status mmwlan_register_rx_pkt_ext_cb(enum mmwlan_vif vif, mmwlan_rx_pkt_ext_cb_t cb, void *arg);
struct mmwlan_tx_metadata { uint8_t tid; const uint8_t *ra; };
#define MMWLAN_TX_METADATA_INIT { 7, (const uint8_t *)1 }
enum mmwlan_status mmwlan_tx_wait_until_ready(uint32_t ms);
struct mmpkt *mmwlan_alloc_mmpkt_for_tx(uint32_t len, uint8_t tid);
enum mmwlan_status mmwlan_tx_pkt(struct mmpkt *p, const struct mmwlan_tx_metadata *md);
enum mmwlan_status mmwlan_get_mac_addr(uint8_t *mac);
struct mmwlan_mesh_peer_link { uint8_t addr[6]; uint8_t estab; uint8_t rc_valid; uint32_t expected_tput_kbps; };
enum mmwlan_status mmwlan_mesh_query_peer_links(struct mmwlan_mesh_peer_link *out, uint8_t max, uint8_t *count);
esp_netif_t *mmhalow_get_netif(void);
void *f_malloc(size_t n); /* fails while f_malloc_fail counts down */
#define malloc(n) f_malloc(n)
EOF
cat > "$T/fakes.c" <<'EOF'
/* The host world main/bat_port.c runs in: FreeRTOS on pthreads and recording ESP-IDF,
 * morselib and cfg fakes. */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bat_mode.h"
#include "bat_port.h"
#include "fake.h"

static int fails;
#define CHECK(c, ...) do { int c_ = (c); printf(c_ ? "ok   " : "FAIL "); printf(__VA_ARGS__); printf("\n"); fails += !c_; } while (0)

/* ---- FreeRTOS on pthreads ---- */
struct fq { pthread_mutex_t m; pthread_cond_t c; size_t item, len, head, n; unsigned char *buf; };
struct ft { pthread_t th; void (*fn)(void *); void *arg; struct fq *notify; };
static __thread struct ft *f_self;
static pthread_mutex_t f_cm = PTHREAD_MUTEX_INITIALIZER;
static int f_queues;
static struct fq *f_q[8]; /* in creation order: the port's free RX slots first */
static void (*f_timeout_hook)(struct fq *q);
static uint64_t mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
static void sleep_ms(unsigned port_ms) /* in the port's clock */
{
    struct timespec ts = { 0, (long)port_ms * 50000L };
    nanosleep(&ts, NULL);
}
void f_crit(int enter) { if (enter) pthread_mutex_lock(&f_cm); else pthread_mutex_unlock(&f_cm); }
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item)
{
    struct fq *q = calloc(1, sizeof(*q));
    q->item = item ? item : 1;
    q->len = len;
    q->buf = calloc(len, q->item);
    pthread_mutex_init(&q->m, NULL);
    pthread_cond_init(&q->c, NULL);
    if (f_queues < 8) { f_q[f_queues] = q; }
    f_queues++;
    return q;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t t)
{
    (void)t;
    pthread_mutex_lock(&q->m);
    if (q->n == q->len) { pthread_mutex_unlock(&q->m); return pdFALSE; }
    memcpy(q->buf + ((q->head + q->n) % q->len) * q->item, item, q->item);
    q->n++;
    pthread_cond_broadcast(&q->c);
    pthread_mutex_unlock(&q->m);
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t t)
{
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    long ns = dl.tv_nsec + (long)(t == portMAX_DELAY ? 0 : t) * 50000L;
    dl.tv_sec += ns / 1000000000L;
    dl.tv_nsec = ns % 1000000000L;
    pthread_mutex_lock(&q->m);
    while (q->n == 0) {
        int e = t == portMAX_DELAY ? pthread_cond_wait(&q->c, &q->m) : t ? pthread_cond_timedwait(&q->c, &q->m, &dl) : ETIMEDOUT;
        if (e == ETIMEDOUT && q->n == 0) {
            pthread_mutex_unlock(&q->m);
            if (t && f_self == NULL && f_timeout_hook) { f_timeout_hook(q); }
            return pdFALSE;
        }
    }
    memcpy(item, q->buf + q->head * q->item, q->item);
    q->head = (q->head + 1) % q->len;
    q->n--;
    pthread_mutex_unlock(&q->m);
    return pdTRUE;
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q)
{
    pthread_mutex_lock(&q->m);
    UBaseType_t n = (UBaseType_t)q->n;
    pthread_mutex_unlock(&q->m);
    return n;
}
void vQueueDelete(QueueHandle_t q) { (void)q; /* a failed start may still have a task blocked on it */ }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return xQueueCreate(1, 1); }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { uint8_t b = 1; return xQueueSend(s, &b, 0); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { uint8_t b; return xQueueReceive(s, &b, t); }
static void *f_task_main(void *a) { f_self = a; f_self->fn(f_self->arg); return NULL; }
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                       TaskHandle_t *out)
{
    (void)name; (void)stack; (void)prio;
    struct ft *t = calloc(1, sizeof(*t));
    t->fn = fn; t->arg = arg; t->notify = xSemaphoreCreateBinary();
    *out = t;
    pthread_create(&t->th, NULL, f_task_main, t);
    pthread_detach(t->th);
    return pdPASS;
}
void vTaskDelete(TaskHandle_t t) { (void)t; }
BaseType_t xTaskNotifyGive(TaskHandle_t t) { (void)xSemaphoreGive(t->notify); return pdPASS; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t t) { (void)clear; return xSemaphoreTake(f_self->notify, t) == pdTRUE; }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t) { (void)t; return 4321; }

/* ---- ESP-IDF, morselib and cfg ---- */
static int f_verbose;
void f_log(const char *fmt, ...) { if (f_verbose) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); puts(""); } }
static const uint8_t MESH[6] = { 0x0c, 0xbf, 0x74, 0x28, 0xbf, 0xcd }, FAC[6] = { 0x48, 0xca, 0x43, 0x3c, 0x24, 0x28 };
esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t t) { (void)t; memcpy(mac, FAC, 6); return ESP_OK; }
uint32_t esp_random(void) { return 4; }
int64_t esp_timer_get_time(void) { return (int64_t)(mono_us() / 50u) * 1000; }
size_t heap_caps_get_free_size(uint32_t c) { (void)c; return 111111; }
size_t heap_caps_get_minimum_free_size(uint32_t c) { (void)c; return 22222; }
size_t heap_caps_get_largest_free_block(uint32_t c) { (void)c; return 33333; }
struct esp_netif_obj { int x; } f_netif;
static struct netif f_lw = { 1500, 1500 };
static esp_netif_driver_ifconfig_t f_drv;
static uint8_t f_netif_mac[6], f_rx_copy[2048];
static volatile int f_delivered, f_freed, f_regs;
static size_t f_rx_copy_len;
static void *f_held[64]; /* frames lwIP holds (tcpip mailbox, socket mailboxes) while f_hold */
static int f_hold, f_nheld, f_malloc_fail;
void *f_malloc(size_t n) { if (f_malloc_fail > 0) { f_malloc_fail--; return NULL; } return (malloc)(n); }
void *esp_netif_get_io_driver(esp_netif_t *n) { (void)n; return (void *)&f_lw; }
esp_err_t esp_netif_set_driver_config(esp_netif_t *n, const esp_netif_driver_ifconfig_t *c) { (void)n; f_drv = *c; return ESP_OK; }
void *esp_netif_get_netif_impl(esp_netif_t *n) { (void)n; return &f_lw; }
esp_err_t esp_netif_set_mac(esp_netif_t *n, uint8_t mac[]) { (void)n; memcpy(f_netif_mac, mac, 6); return ESP_OK; }
esp_err_t esp_netif_tcpip_exec(esp_err_t (*fn)(void *ctx), void *ctx) { return fn(ctx); }
esp_err_t esp_netif_receive(esp_netif_t *n, void *buffer, size_t len, void *eb)
{
    (void)n;
    memcpy(f_rx_copy, buffer, len);
    f_rx_copy_len = len;
    f_delivered++;
    if (f_hold && f_nheld < 64) { f_held[f_nheld++] = eb; return ESP_OK; }
    f_drv.driver_free_rx_buffer(f_drv.handle, eb); /* as wlanif does, once lwIP is done with it */
    f_freed++;
    return ESP_OK;
}
struct mmpkt { uint32_t len; uint8_t d[2048]; };
struct mmpktview *mmpkt_open(struct mmpkt *p) { return (struct mmpktview *)p; }
void mmpkt_close(struct mmpktview **v) { *v = NULL; }
void mmpkt_append_data(struct mmpktview *v, const uint8_t *d, uint32_t len)
{ struct mmpkt *p = (struct mmpkt *)v; memcpy(p->d + p->len, d, len); p->len += len; }
uint8_t *mmpkt_get_data_start(struct mmpktview *v) { return ((struct mmpkt *)v)->d; }
uint32_t mmpkt_get_data_length(struct mmpktview *v) { return ((struct mmpkt *)v)->len; }
void mmpkt_release(struct mmpkt *p) { free(p); }
static mmwlan_rx_pkt_ext_cb_t f_rx_cb;
static void *f_rx_arg;
static volatile int f_tx_pkts;
static uint8_t f_tx_ra[6], f_tx_tid = 99;
static int f_tx_ra_null, f_mac_fail;
static enum mmwlan_status f_tx_status = MMWLAN_SUCCESS;
static struct mmwlan_mesh_peer_link f_links[2];
static uint8_t f_links_n;
static int f_links_fail;
enum mmwlan_status mmwlan_register_rx_pkt_ext_cb(enum mmwlan_vif vif, mmwlan_rx_pkt_ext_cb_t cb, void *arg)
{ (void)vif; f_rx_cb = cb; f_rx_arg = arg; f_regs++; return MMWLAN_SUCCESS; }
enum mmwlan_status mmwlan_tx_wait_until_ready(uint32_t ms) { (void)ms; return MMWLAN_SUCCESS; }
struct mmpkt *mmwlan_alloc_mmpkt_for_tx(uint32_t len, uint8_t tid) { (void)len; (void)tid; return calloc(1, sizeof(struct mmpkt)); }
enum mmwlan_status mmwlan_tx_pkt(struct mmpkt *p, const struct mmwlan_tx_metadata *md)
{
    f_tx_ra_null = md->ra == NULL;
    if (md->ra) { memcpy(f_tx_ra, md->ra, 6); }
    f_tx_tid = md->tid;
    mmpkt_release(p);
    if (f_tx_status != MMWLAN_SUCCESS) { return f_tx_status; }
    f_tx_pkts++;
    return MMWLAN_SUCCESS;
}
enum mmwlan_status mmwlan_get_mac_addr(uint8_t *mac) { if (f_mac_fail) return MMWLAN_UNAVAILABLE; memcpy(mac, MESH, 6); return MMWLAN_SUCCESS; }
enum mmwlan_status mmwlan_mesh_query_peer_links(struct mmwlan_mesh_peer_link *out, uint8_t max, uint8_t *count)
{
    if (f_links_fail) { return MMWLAN_ERROR; }
    uint8_t n = f_links_n < max ? f_links_n : max;
    memcpy(out, f_links, n * sizeof(*out));
    *count = n;
    return MMWLAN_SUCCESS;
}
esp_netif_t *mmhalow_get_netif(void) { return &f_netif; }
volatile uint32_t g_warthog_mesh_batman, g_warthog_mesh_grp, g_warthog_host_ccmp_on;
static uint8_t f_stored = 1, f_fwd;
uint8_t warthog_cfg_get_mesh_batman(void) { return f_stored; }
uint8_t warthog_cfg_get_mesh_enable(void) { return 1; }
uint8_t warthog_cfg_get_mesh_fwd(void) { return f_fwd; }
uint8_t warthog_cfg_get_mesh_bridge(void) { return 0; }
uint32_t warthog_cfg_get_mesh_battp(void) { return 25; }

EOF
awk '/^static void cmd_bat_render\(/,/^}/' "$A" > "$T/at_render.c"
awk '/^static void cmd_bat_render_mac\(/,/^}/' "$A" >> "$T/at_render.c"
cat > "$T/t.c" <<'EOF'
/* main/bat_port.c on fakes.c and a counting engine stub, with main/at.c's cmd_bat_render on top:
 * the start sequence, both slot pools, the tx gate and RA, the delivery cap, the render handshake
 * (abandon and late finish), paging and the stat line. argv[1]: run, off, fwd or nomac. */
#include "fakes.c"

/* ---- the engine: counts, and can be held inside bat_rx_hard ---- */
struct bat { int unused; };
static struct bat_config f_cfg;
static struct bat_ops f_ops;
static void *f_user;
static volatile int f_inits, f_rx_hard, f_tx_soft, f_ticks, f_in_rx, f_gate;
static pthread_mutex_t f_gm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t f_gc = PTHREAD_COND_INITIALIZER;
static uint8_t f_last[2048];
static size_t f_last_len;
void bat_config_defaults(struct bat_config *c) { memset(c, 0x5a, sizeof(*c)); c->bcast_copies = 1; }
size_t bat_ctx_size(void) { return sizeof(struct bat); }
int bat_init(struct bat *b, const struct bat_config *cfg, const struct bat_ops *ops, void *user)
{ (void)b; f_cfg = *cfg; f_ops = *ops; f_user = user; f_inits++; return 0; }
static void gate(int closed) { pthread_mutex_lock(&f_gm); f_gate = closed; pthread_cond_broadcast(&f_gc); pthread_mutex_unlock(&f_gm); }
void bat_rx_hard(struct bat *b, uint8_t *frame, size_t len)
{
    (void)b;
    memcpy(f_last, frame, len);
    f_last_len = len;
    pthread_mutex_lock(&f_gm);
    __atomic_store_n(&f_in_rx, 1, __ATOMIC_SEQ_CST);
    while (f_gate) { pthread_cond_wait(&f_gc, &f_gm); }
    __atomic_store_n(&f_in_rx, 0, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&f_gm);
    __atomic_add_fetch(&f_rx_hard, 1, __ATOMIC_SEQ_CST);
}
int bat_tx_soft(struct bat *b, const uint8_t *frame, size_t len)
{ (void)b; memcpy(f_last, frame, len); f_last_len = len; __atomic_add_fetch(&f_tx_soft, 1, __ATOMIC_SEQ_CST); return 0; }
uint32_t bat_tick(struct bat *b) { (void)b; __atomic_add_fetch(&f_ticks, 1, __ATOMIC_SEQ_CST); return 20; }
unsigned bat_route_count(const struct bat *b) { (void)b; return 3; }
unsigned bat_neigh_count(const struct bat *b) { (void)b; return 2; }
/* One chunk of f_render_len bytes; or with f_entries a summary line (cursor 0) and entries 1..f_entries
 * of f_entry_len bytes, paged like the engine's (whole entries below len - 32). f_stall hands the
 * cursor it was given back after the first chunk. */
static size_t f_render_len;
static unsigned f_entries, f_entry_len, f_stall, f_render_calls, f_render_mac_calls, f_render_has_mac;
static uint8_t f_render_mac[6];
static enum bat_render_kind f_render_kind;
static size_t entry(char *o, unsigned i)
{
    int n = snprintf(o, f_entry_len + 1, "+BATO: e%04u ", i);
    memset(o + n, '.', f_entry_len - 2 - (size_t)n);
    memcpy(o + f_entry_len - 2, "\r\n", 3);
    return f_entry_len;
}
size_t bat_render_from(struct bat *b, enum bat_render_kind k, const uint8_t *mac, uint32_t *cursor, char *buf,
                       size_t len)
{
    (void)b;
    f_render_calls++;
    f_render_kind = k;
    f_render_has_mac = mac != NULL;
    if (mac) { memcpy(f_render_mac, mac, 6); f_render_mac_calls++; }
    const uint32_t from = *cursor;
    *cursor = BAT_RENDER_DONE;
    if (!f_entries) { size_t n = f_render_len < len ? f_render_len : len - 1; memset(buf, 'x', n); buf[n] = 0; return n; }
    size_t pos = 0;
    for (uint32_t i = from; i <= f_entries; i++) {
        if (pos + f_entry_len >= len - 32) { *cursor = f_stall && from ? from : i; break; }
        pos += entry(buf + pos, i);
    }
    buf[pos] = 0;
    return pos;
}
static size_t listing(char *o) /* every entry, as one string */
{
    size_t n = 0;
    for (unsigned i = 0; i <= f_entries; i++) { n += entry(o + n, i); }
    return n;
}
static volatile int f_route_calls, f_routed = 1, f_gw_n;
static uint8_t f_asked[6];
static const struct bat_client_route F_ROUTE = { { 2, 0xd4, 0x0b, 0, 0, 1 }, 72, 900 };
static const struct bat_gw F_GW = { { 2, 0xd4, 0x0a, 0, 0, 1 }, 100, 20, 55, 400 };
bool bat_client_route(struct bat *b, const uint8_t mac[BAT_ALEN], struct bat_client_route *out)
{
    (void)b;
    memcpy(f_asked, mac, 6);
    __atomic_add_fetch(&f_route_calls, 1, __ATOMIC_SEQ_CST);
    const int r = __atomic_load_n(&f_routed, __ATOMIC_SEQ_CST);
    if (out) { if (r) { *out = F_ROUTE; } else { memset(out, 0, sizeof(*out)); } }
    return r != 0;
}
typedef __typeof__(bat_gw_best(NULL, NULL)) gw_ret_t; /* what the port must hand on unchanged */
gw_ret_t bat_gw_best(struct bat *b, struct bat_gw *out)
{
    (void)b;
    const int g = __atomic_load_n(&f_gw_n, __ATOMIC_SEQ_CST);
    if (out) { if (g) { *out = F_GW; } else { memset(out, 0, sizeof(*out)); } }
    return (gw_ret_t)g;
}

/* ---- the test ---- */
static int ld(volatile int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static int wait_ge(volatile int *p, int want) /* up to 20 s of port time, 1 s real */
{
    for (int i = 0; i < 20000 && ld(p) < want; i++) { sleep_ms(1); }
    return ld(p) >= want;
}
static const uint8_t TA[6] = { 0x0c, 0xbf, 0x74, 0x28, 0xbf, 0x74 }, BC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static void rx(const uint8_t *src, uint16_t type, uint8_t first, size_t len)
{
    struct mmpkt *p = calloc(1, sizeof(*p));
    memcpy(p->d, BC, 6);
    memcpy(p->d + 6, src, 6);
    p->d[12] = (uint8_t)(type >> 8); p->d[13] = (uint8_t)type; p->d[14] = first;
    for (size_t i = 15; i < len; i++) { p->d[i] = (uint8_t)i; }
    p->len = (uint32_t)len;
    struct mmwlan_rx_metadata md = { MMWLAN_VIF_UNSPECIFIED, TA };
    f_rx_cb(p, &md, f_rx_arg);
}
static esp_err_t soft(size_t len)
{
    static uint8_t f[1600];
    memcpy(f, BC, 6);
    memcpy(f + 6, f_netif_mac, 6);
    f[12] = 0x08; f[13] = 0x00;
    return f_drv.transmit(f_drv.handle, f, len);
}
static uint32_t f_cursor;
static const char *render_next(int *r) /* the AT+BATSTAT? chunk at f_cursor */
{
    const char *out = NULL;
    *r = warthog_bat_port_render(BAT_RENDER_STAT, NULL, &f_cursor, &out);
    return *r == WARTHOG_BAT_RENDER_OK ? out : NULL;
}
static const char *render(int *r) /* its first chunk */
{
    f_cursor = 0;
    return render_next(r);
}
static char at_out[65536];
static size_t at_len;
static int at_ok, at_err;
static void cdc_write(const char *s) { size_t n = strlen(s); if (at_len + n < sizeof(at_out)) { memcpy(at_out + at_len, s, n + 1); at_len += n; } }
static void reply_ok(void) { at_ok++; cdc_write("OK\r\n"); }
static void reply_error(const char *why) { at_err++; cdc_write("+ERR: "); cdc_write(why); cdc_write("\r\n"); }
#include "at_render.c"
static const char *at(enum bat_render_kind k, const char *mac) /* NULL: the '?' form, else AT+BATx=<mac> */
{
    at_len = 0; at_out[0] = 0; at_ok = at_err = 0;
    if (mac) { cmd_bat_render_mac(k, mac, "usage"); } else { cmd_bat_render(k, NULL); }
    return at_out;
}
static struct fq *f_hooked;
static void late_finish(struct fq *q) /* the engine finishes right after the caller's timeout */
{
    f_timeout_hook = NULL;
    f_hooked = q;
    gate(0);
    for (int i = 0; i < 20000 && uxQueueMessagesWaiting(q) == 0; i++) { sleep_ms(1); }
}

static int refused(const char *mode, int want_reason)
{
    esp_err_t e = warthog_bat_port_start();
    CHECK(!warthog_bat_port_running() && warthog_bat_port_reason() == want_reason && f_queues == 0 &&
          f_regs == 0 && f_inits == 0 && g_warthog_mesh_batman == 0,
          "%s: not running (reason %s, start %d), nothing allocated, no hook, gate closed", mode,
          bat_mode_reason_text((enum bat_mode_reason)warthog_bat_port_reason()), e);
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "run";
    f_verbose = getenv("BP_VERBOSE") != NULL;
    if (!strcmp(mode, "off")) { f_stored = 0; return refused("batman off", BAT_MODE_OFF); }
    if (!strcmp(mode, "fwd")) { f_fwd = 1; return refused("AT+MESHFWD=1 stored too", BAT_MODE_FWD); }
    if (!strcmp(mode, "nomac")) { f_mac_fail = 1; return refused("no mesh MAC", BAT_MODE_INIT_FAIL); }

    uint8_t soft_mac[6];
    bat_mode_soft_mac(FAC, MESH, soft_mac);
    CHECK(warthog_bat_port_start() == ESP_OK && warthog_bat_port_running() &&
          warthog_bat_port_reason() == BAT_MODE_OK, "start: running, reason ok");
    CHECK(f_regs == 1 && f_rx_cb != NULL, "one RX ext hook registered (%d)", f_regs);
    CHECK(f_drv.transmit && f_drv.transmit_wrap && f_drv.driver_free_rx_buffer && f_drv.handle == (void *)&f_lw,
          "the netif driver is ours; its handle stays the Morse driver's");
    CHECK(f_lw.mtu == BAT_SOFT_MTU_DEFAULT, "bat0 IP MTU %u (want %u)", f_lw.mtu, BAT_SOFT_MTU_DEFAULT);
    CHECK(memcmp(f_netif_mac, soft_mac, 6) == 0, "the netif takes the soft MAC " MACSTR, MAC2STR(f_netif_mac));
    CHECK(f_inits == 1 && memcmp(f_cfg.hard_addr, MESH, 6) == 0, "engine originator = the mesh MAC (M2) (%d " MACSTR ")", f_inits, MAC2STR(f_cfg.hard_addr));
    CHECK(memcmp(f_cfg.soft_addr, soft_mac, 6) == 0 && f_cfg.tput_override == 25 && f_cfg.bcast_copies == 1 &&
          f_cfg.hard_mtu == 0x5a5a, "engine soft MAC, AT+MESHBATTP 25, 1 copy; the rest from bat_config_defaults");
    CHECK(f_ops.tx && f_ops.deliver && f_ops.now_ms && f_ops.rand32 && f_ops.link_tput, "every engine op is set");
    CHECK(g_warthog_mesh_batman == 1, "the datapath's batman gate is set (AE-2 replica / group shape)");
    CHECK(g_warthog_host_ccmp_on == 1, "host CCMP armed: peers' ELP/OGM/BCAST are group frames under their MGTK");
    CHECK(wait_ge(&f_ticks, 3), "the engine task runs (%d ticks)", ld(&f_ticks));

    /* The stat line, before anything varies its digits: after the engine's last chunk if it fits whole. */
    int r;
    f_render_len = 0;
    const char *o = render(&r);
    size_t L = o ? strlen(o) : 0;
    CHECK(o && L > 2 && !strcmp(o + L - 2, "\r\n") && strstr(o, "rx_slots=6/6 tx_slots=4/4 ") &&
          strstr(o, "tput_cache_age=-1 tput_snap_fail=0 ") && f_cursor == BAT_RENDER_DONE,
          "AT+BATSTAT? port line, %zu bytes", L);
    if (o) { warthog_bat_port_render_done(); }
    f_render_len = BAT_RENDER_BUF - L;
    unsigned rc0 = f_render_calls;
    o = render(&r);
    CHECK(o && strlen(o) == BAT_RENDER_BUF - L && !strstr(o, "+BATSTAT: port") && f_cursor != BAT_RENDER_DONE,
          "room for the line's length only: the engine's chunk alone, the line not cut");
    if (o) { warthog_bat_port_render_done(); }
    o = render_next(&r);
    CHECK(o && strlen(o) == L && !strncmp(o, "+BATSTAT: port ", 15) && f_cursor == BAT_RENDER_DONE &&
          f_render_calls == rc0 + 1, "then the line whole in a chunk of its own, the engine not asked again, and done");
    if (o) { warthog_bat_port_render_done(); }
    f_render_len = BAT_RENDER_BUF - L - 1;
    o = render(&r);
    CHECK(o && strlen(o) == BAT_RENDER_BUF - 1 && o[BAT_RENDER_BUF - 1 - L] == '+' && f_cursor == BAT_RENDER_DONE,
          "room for it and its NUL: whole, in the same chunk");
    if (o) { warthog_bat_port_render_done(); }
    f_render_len = 0;

    /* Paging: at.c's cmd_bat_render asks for chunks until the cursor is done and writes each. */
    static char want[32768];
    f_entries = 150;
    f_entry_len = 100;
    size_t wl = listing(want);
    rc0 = f_render_calls;
    const char *a = at(BAT_RENDER_ORIG, NULL);
    CHECK(wl > 3 * BAT_RENDER_BUF && !strncmp(a, want, wl) && !strcmp(a + wl, "OK\r\n") && at_ok == 1 &&
          at_err == 0 && f_render_calls == rc0 + 4 && f_render_kind == BAT_RENDER_ORIG && !f_render_has_mac,
          "AT+BATO? of %zu bytes: 4 chunks, whole and in order, then one OK (%u chunks)", wl, f_render_calls - rc0);
    a = at(BAT_RENDER_STAT, NULL);
    CHECK(!strncmp(a, want, wl) && !strncmp(a + wl, "+BATSTAT: port ", 15) && strlen(a + wl) == L + 4 && at_ok == 1,
          "AT+BATSTAT? the same way, the port line once, after the last chunk");
    f_entries = 79; /* the second chunk ends the listing with 40 entries: no room for the port line */
    wl = listing(want);
    rc0 = f_render_calls;
    a = at(BAT_RENDER_STAT, NULL);
    CHECK(!strncmp(a, want, wl) && !strncmp(a + wl, "+BATSTAT: port ", 15) && strlen(a + wl) == L + 4 && at_ok == 1 &&
          f_render_calls == rc0 + 2, "a last chunk too full for the port line: the line whole in a third (%u engine "
          "calls)", f_render_calls - rc0);
    static const uint8_t NODE[6] = { 0x0c, 0xbf, 0x74, 0x28, 0xbf, 0xcd };
    const unsigned m0 = f_render_mac_calls;
    rc0 = f_render_calls;
    a = at(BAT_RENDER_TT_GLOBAL, "0C:bf:74:28:bF:cd");
    CHECK(at_ok == 1 && !strncmp(a, want, wl) && f_render_kind == BAT_RENDER_TT_GLOBAL && !memcmp(f_render_mac, NODE, 6) &&
          f_render_calls - rc0 == 2 && f_render_mac_calls - m0 == 2, "AT+BATTG=<mac>: the MAC reaches the engine's "
          "filter with every chunk (%u of %u)", f_render_mac_calls - m0, f_render_calls - rc0);
    rc0 = f_render_calls;
    a = at(BAT_RENDER_ORIG, "0c:bf:74:28:bf");
    CHECK(!strcmp(a, "+ERR: usage\r\n") && f_render_calls == rc0, "a bad <mac>: the usage error, the engine not asked");
    f_stall = 1;
    f_entries = 150;
    a = at(BAT_RENDER_ORIG, NULL);
    CHECK(at_ok == 0 && at_err == 1 && f_render_calls == rc0 + 2 && strstr(a, "+ERR: batman render stalled\r\n"),
          "an engine handing a cursor back unmoved: an error after that chunk, not a loop (%u chunks)",
          f_render_calls - rc0);
    f_stall = 0;
    f_entries = 0;

    uint8_t uni[64] = { 0 };
    memcpy(uni, TA, 6);
    memcpy(uni + 6, MESH, 6);
    uni[12] = 0x43; uni[13] = 0x05; uni[14] = 0x40;
    CHECK(f_ops.tx(f_user, uni, sizeof(uni)) == BAT_TX_NOPEER && f_tx_pkts == 0, "before mesh_up: nothing sent");
    warthog_bat_port_mesh_up();
    CHECK(f_ops.tx(f_user, uni, sizeof(uni)) == BAT_TX_OK && f_tx_pkts == 1 && !f_tx_ra_null &&
          memcmp(f_tx_ra, TA, 6) == 0 && f_tx_tid == 0, "unicast: sent to RA = its destination, TID 0");
    uint8_t bc[64] = { 0 };
    memcpy(bc, BC, 6);
    memcpy(bc + 6, MESH, 6);
    bc[12] = 0x43; bc[13] = 0x05; bc[14] = 0x03;
    CHECK(f_ops.tx(f_user, bc, sizeof(bc)) == BAT_TX_OK && f_tx_pkts == 2 && f_tx_ra_null,
          "broadcast: RA NULL, the datapath's group shape");
    f_tx_status = MMWLAN_NOT_FOUND;
    CHECK(f_ops.tx(f_user, uni, sizeof(uni)) == BAT_TX_NOPEER, "no ESTAB peer for the RA: BAT_TX_NOPEER");
    f_tx_status = MMWLAN_SUCCESS;

    int base = ld(&f_rx_hard), got = 0;
    for (int i = 1; i <= 7; i++) {
        rx(TA, 0x4305, 0x04, 100 + (size_t)i);
        got += wait_ge(&f_rx_hard, base + i);
    }
    CHECK(got == 7 && f_last_len == 107 && f_last[106] == 106 && memcmp(f_last + 6, TA, 6) == 0,
          "7 batman frames, one at a time, each reach bat_rx_hard whole (every RX slot comes back)");
    rx(MESH, 0x4305, 0x04, 60);
    rx(TA, 0x0800, 0x45, 60);
    rx(TA, 0x4305, 0x04, 1601);
    sleep_ms(50);
    CHECK(ld(&f_rx_hard) == base + 7, "relayed, non-batman and oversize frames are dropped at the hook");
    base = ld(&f_tx_soft);
    int okc = 0;
    for (int i = 1; i <= 5; i++) {
        okc += soft(100) == ESP_OK;
        (void)wait_ge(&f_tx_soft, base + i);
    }
    CHECK(okc == 5 && ld(&f_tx_soft) == base + 5, "5 soft frames, one at a time, all reach bat_tx_soft");
    int big = 0;
    for (int i = 0; i < 4; i++) { big += soft(1537) == ESP_ERR_INVALID_ARG; }
    CHECK(big == 4 && soft(1536) == ESP_OK && wait_ge(&f_tx_soft, base + 6) && f_last_len == 1536,
          "1537-byte soft frames refused (4 of 4), 1536 carried whole");

    int rx0 = ld(&f_rx_hard), tx0 = ld(&f_tx_soft);
    gate(1);
    rx(TA, 0x4305, 0x04, 60);
    int busy = wait_ge(&f_in_rx, 1);
    for (int i = 0; i < 5; i++) { rx(TA, 0x4305, 0x04, 60); }
    rx(TA, 0x4305, 0x04, 60); /* the 7th: every slot is taken */
    okc = 0;
    for (int i = 0; i < 4; i++) { okc += soft(100) == ESP_OK; }
    esp_err_t full = soft(100);
    gate(0);
    int drained = wait_ge(&f_rx_hard, rx0 + 6) && wait_ge(&f_tx_soft, tx0 + 4);
    sleep_ms(20);
    CHECK(busy && okc == 4 && full == ESP_ERR_NO_MEM && drained && ld(&f_rx_hard) == rx0 + 6,
          "engine busy: 6 RX + 4 TX queue, the 7th RX and 5th TX are refused, then all 10 get through");

    static const uint8_t in[20] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0x08, 0x00, 0x45 };
    f_ops.deliver(f_user, in, sizeof(in));
    CHECK(f_delivered == 1 && f_freed == 1 && f_rx_copy_len == sizeof(in) && !memcmp(f_rx_copy, in, sizeof(in)),
          "deliver: a copy to esp_netif_receive, freed through the driver");
    static uint8_t full_frame[1514];
    memcpy(full_frame, in, sizeof(in));
    f_hold = 1; /* the tcpip thread stalls (a suspended USB host) and lwIP keeps every frame it is given */
    for (int i = 0; i < 40; i++) { f_ops.deliver(f_user, full_frame, sizeof(full_frame)); }
    int held = f_nheld;
    o = render(&r);
    CHECK(held == 16 && f_delivered == 17 && o && strstr(o, " deliver_nomem=0 deliver_cap=24 "),
          "lwIP stalled: at most 16 delivered copies (~25 KB) outstanding, the other 24 dropped and counted (held %d)",
          held);
    if (o) { warthog_bat_port_render_done(); }
    f_hold = 0;
    for (int i = 0; i < f_nheld; i++) { f_drv.driver_free_rx_buffer(f_drv.handle, f_held[i]); }
    f_nheld = 0;
    f_ops.deliver(f_user, in, sizeof(in));
    CHECK(f_delivered == 18 && f_freed == 2, "once lwIP frees them, frames are delivered again (%d)", f_delivered);
    f_malloc_fail = 20;
    for (int i = 0; i < 20; i++) { f_ops.deliver(f_user, in, sizeof(in)); }
    f_malloc_fail = 0;
    f_ops.deliver(f_user, in, sizeof(in));
    o = render(&r);
    CHECK(f_delivered == 19 && f_freed == 3 && o && strstr(o, " deliver_nomem=20 deliver_cap=24 "),
          "no heap for 20 copies: dropped, counted, and not held against the cap (%d)", f_delivered);
    if (o) { warthog_bat_port_render_done(); }

    uint8_t peer[6];
    memcpy(peer, TA, 6);
    f_links_fail = 1;
    CHECK(f_ops.link_tput(f_user, peer) == BAT_TPUT_UNKNOWN, "link_tput: a failed first query is unknown, not 0");
    f_links_fail = 0;
    memcpy(f_links[0].addr, TA, 6);
    f_links[0].estab = 1; f_links[0].rc_valid = 1; f_links[0].expected_tput_kbps = 7200;
    f_links_n = 1;
    CHECK(f_ops.link_tput(f_user, peer) == 72, "then the snapshot's 7200 kbit/s: 72");
    sleep_ms(BAT_MODE_LINKS_MS + 10);
    f_links_fail = 1;
    CHECK(f_ops.link_tput(f_user, peer) == 72, "a failed refresh keeps the last snapshot");
    f_links_fail = 0;

    o = render(&r);
    CHECK(o && strstr(o, "rx_slots=6/6 tx_slots=4/4 ") && strstr(o, "q_rx_full=1 q_tx_full=1 ") &&
          strstr(o, "rx_nonbat=1 ") && strstr(o, "rx_relayed=1 ") && strstr(o, "rx_toobig=1 ") &&
          strstr(o, "tput_snap_fail=2 ") && strstr(o, "soft_toobig=4 ") && strstr(o, "tx_notfound=1 "),
          "AT+BATSTAT? afterwards: every slot back, each refusal counted once");
    if (!o || fails) { printf("     %s", o ? o : "(no render)\n"); }
    if (o) { warthog_bat_port_render_done(); }

    gate(1);
    rx(TA, 0x4305, 0x04, 60);
    (void)wait_ge(&f_in_rx, 1);
    uint64_t t0 = mono_us();
    o = render(&r);
    CHECK(r == WARTHOG_BAT_RENDER_BUSY && mono_us() - t0 >= 90000, "engine stuck: the render gives up after 2 s, busy");
    o = render(&r);
    CHECK(r == WARTHOG_BAT_RENDER_BUSY, "and the next one too while the abandoned request is queued");
    gate(0);
    for (int i = 0; i < 20 && r != WARTHOG_BAT_RENDER_OK; i++) { o = render(&r); }
    CHECK(r == WARTHOG_BAT_RENDER_OK, "once the engine gets to it, the lock comes back: renders work again");
    if (r == WARTHOG_BAT_RENDER_OK) { warthog_bat_port_render_done(); }

    gate(1);
    rx(TA, 0x4305, 0x04, 60);
    (void)wait_ge(&f_in_rx, 1);
    f_timeout_hook = late_finish;
    o = render(&r);
    CHECK(f_hooked != NULL && r == WARTHOG_BAT_RENDER_OK && o != NULL && f_cursor == BAT_RENDER_DONE,
          "the engine finishes just after the caller's timeout: the render and its cursor are still taken");
    if (r == WARTHOG_BAT_RENDER_OK) { warthog_bat_port_render_done(); }
    o = render(&r);
    CHECK(r == WARTHOG_BAT_RENDER_OK, "and the next render works");
    if (r == WARTHOG_BAT_RENDER_OK) { warthog_bat_port_render_done(); }

    /* bat0 addressing's questions: the lease router's MAC, and the best gateway. */
    static const uint8_t RTR[6] = { 0x02, 0xd4, 0x0b, 0x00, 0x00, 0xff }, RTR2[6] = { 0x02, 0xd4, 0x0c, 0x00, 0x00, 0xff };
    struct bat_client_route cr;
    struct bat_gw gw;
    sleep_ms(600);
    CHECK(ld(&f_route_calls) == 0 && warthog_bat_port_watch_answer(RTR, &cr) == -1,
          "nothing watched: the engine is not asked, no answer");
    warthog_bat_port_watch(RTR);
    int ans = -1;
    for (int i = 0; i < 200 && ans < 0; i++) { sleep_ms(1); ans = warthog_bat_port_watch_answer(RTR, &cr); }
    CHECK(ans == 1 && !memcmp(f_asked, RTR, 6) && !memcmp(&cr, &F_ROUTE, sizeof(cr)),
          "a new watch is answered on the engine's next pass (%d): routed, its originator and OGM age", ans);
    CHECK(warthog_bat_port_watch_answer(RTR2, &cr) == -1, "an answer is only for the MAC watched");
    int c0 = ld(&f_route_calls), k0 = ld(&f_ticks);
    int64_t p0 = esp_timer_get_time();
    sleep_ms(2000);
    int calls = ld(&f_route_calls) - c0, passes = ld(&f_ticks) - k0;
    int64_t span = (esp_timer_get_time() - p0) / 1000;
    CHECK(calls >= 2 && calls <= span / 500 + 2 && passes > 4 * calls,
          "refreshed every 500 ms, not on every engine pass (%d in %lld ms, %d passes)", calls, (long long)span, passes);
    __atomic_store_n(&f_routed, 0, __ATOMIC_SEQ_CST);
    for (int i = 0; i < 1500 && ans != 0; i++) { sleep_ms(1); ans = warthog_bat_port_watch_answer(RTR, &cr); }
    CHECK(ans == 0, "the router stops resolving: the answer follows within 500 ms (%d)", ans);
    warthog_bat_port_watch(RTR2);
    CHECK(warthog_bat_port_watch_answer(RTR, &cr) == -1 && warthog_bat_port_watch_answer(RTR2, &cr) == -1,
          "watching another MAC drops the old answer at once");
    ans = -1;
    for (int i = 0; i < 200 && ans < 0; i++) { sleep_ms(1); ans = warthog_bat_port_watch_answer(RTR2, &cr); }
    CHECK(ans == 0 && !memcmp(f_asked, RTR2, 6), "and the new one is asked on the next pass");
    warthog_bat_port_watch(NULL);
    CHECK(warthog_bat_port_watch_answer(RTR2, &cr) == -1, "watch(NULL): no answer");
    c0 = ld(&f_route_calls);
    CHECK(warthog_bat_port_gw(&gw) == 0, "no gateway yet");
    __atomic_store_n(&f_gw_n, 2, __ATOMIC_SEQ_CST);
    unsigned gn = 0;
    for (int i = 0; i < 1500 && !gn; i++) { sleep_ms(1); gn = warthog_bat_port_gw(&gw); }
    CHECK(gn == (uint8_t)(gw_ret_t)2 && !memcmp(&gw, &F_GW, sizeof(gw)) && ld(&f_route_calls) == c0,
          "gateways are published within 500 ms as bat_gw_best returned them (%u), with the best; with nothing "
          "watched nobody's route is looked up", gn);

    warthog_bat_port_watch(RTR);
    __atomic_store_n(&f_routed, 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < 1500 && ans != 1; i++) { sleep_ms(1); ans = warthog_bat_port_watch_answer(RTR, &cr); }
    warthog_bat_port_mesh_failed();
    CHECK(!warthog_bat_port_running() && warthog_bat_port_reason() == BAT_MODE_MESH_FAILED &&
          warthog_bat_port_routes() == 0 && render(&r) == NULL && r == WARTHOG_BAT_RENDER_NOT_RUNNING &&
          warthog_bat_port_watch_answer(RTR, &cr) == -1 && warthog_bat_port_gw(&gw) == 0 &&
          !strcmp(at(BAT_RENDER_ORIG, NULL), "+ERR: batman not running (mesh-failed)\r\n"),
          "mmwlan_mesh_enable failed: not running, reason mesh-failed, no renders, no answers, no gateway");
    printf(fails ? "bat_port: %d FAILED\n" : "bat_port: all passed\n", fails);
    return fails != 0;
}
EOF
why=""
if ${CC:-cc} -std=gnu11 -w -pthread ${SANFLAGS:-} -DWARTHOG_MESH_SAE=1 -DWARTHOG_MESH_HOST_CCMP=1 -I"$T" -I"$T/inc" \
     -I../../../main -I../../../main/bat -o "$T/t" "$T/t.c" "$BP" ../../../main/bat_mode.c 2>"$T/cc.log"; then
  for m in run off fwd nomac; do
    "$T/t" "$m" > "$T/$m.log" 2>&1 || why="$why $m: $(grep -m2 -E '^FAIL|ERROR|Sanitizer' "$T/$m.log" | tr '\n' ' ')"
  done
else
  why="did not build: $(head -3 "$T/cc.log" | tr '\n' ' ')"
fi
if [ -z "$why" ]; then
  ok "bat_port.c runs: start wiring, tx gate and RA, slot pools, delivery cap, render abandon/late finish, paging through cmd_bat_render, stat line, router/gateway answers, refused starts"
else
  bad "bat_port.c on the host harness:$why"
fi

# 34b. The same bat_port.c and cmd_bat_render with the real engine (main/bat/) behind them, at the
#      engine's table sizes: 32 originators heard through 4 neighbours, each announcing 3 clients,
#      two of them gateways, all through the 0x4305 hook. AT+BATO? and AT+BATTG? list every one
#      of them across chunks with one OK, AT+BATO=/AT+BATTG=<mac> exactly one node's rows, and
#      the port hands on the engine's gateway answer.
cat > "$T/scale.c" <<'EOF'
/* main/bat_port.c on fakes.c, the real engine behind it and main/at.c's cmd_bat_render in front. */
#include "fakes.c"
#include "bat_crc32c.h"
#include "bat_internal.h"
static char at_out[65536];
static size_t at_len;
static int at_ok, at_err;
static void cdc_write(const char *s) { size_t n = strlen(s); if (at_len + n < sizeof(at_out)) { memcpy(at_out + at_len, s, n + 1); at_len += n; } }
static void reply_ok(void) { at_ok++; cdc_write("OK\r\n"); }
static void reply_error(const char *why) { at_err++; cdc_write("+ERR: "); cdc_write(why); cdc_write("\r\n"); }
#include "at_render.c"
static const char *at(enum bat_render_kind k, const char *mac)
{
    at_len = 0; at_out[0] = 0; at_ok = at_err = 0;
    if (mac) { cmd_bat_render_mac(k, mac, "usage"); } else { cmd_bat_render(k, NULL); }
    return at_out;
}
#define NODES 32
#define NB 4
static void addr(uint8_t *o, uint8_t kind, unsigned k) { o[0] = 0x02; o[1] = kind; o[2] = o[3] = o[4] = 0; o[5] = (uint8_t)k; }
/* A broadcast from neighbour f+6, its TA too, through the hook; returns once bat_rx_hard is done with it. */
static void hook(uint8_t *f, size_t len)
{
    memset(f, 0xff, 6);
    f[BAT_LINK_TYPE] = 0x43; f[BAT_LINK_TYPE + 1] = 0x05;
    struct mmpkt *p = calloc(1, sizeof(*p));
    memcpy(p->d, f, len);
    p->len = (uint32_t)len;
    struct mmwlan_rx_metadata md = { MMWLAN_VIF_UNSPECIFIED, f + BAT_LINK_SRC };
    f_rx_cb(p, &md, f_rx_arg);
    for (int i = 0; i < 20000 && uxQueueMessagesWaiting(f_q[0]) < 6; i++) { sleep_ms(1); } /* its RX slot back */
}
static void elp(unsigned j, uint32_t seq)
{
    uint8_t f[64] = { 0 }, *p = f + BAT_ETH_HLEN;
    addr(f + BAT_LINK_SRC, 0xA0, j);
    p[0] = BAT_PT_ELP; p[1] = BAT_COMPAT;
    addr(p + BAT_ELP_ORIG, 0xA0, j);
    bat_put32(p + BAT_ELP_SEQ, seq);
    bat_put32(p + BAT_ELP_INTERVAL, 500);
    hook(f, BAT_ETH_HLEN + BAT_ELP_LEN);
}
/* Node k's OGM through neighbour j: TT (its bat0 MAC untagged and on VID 1, a bridged host) and for
 * nodes 0 and 1 a gateway announcement. */
static void ogm(unsigned j, unsigned k)
{
    uint8_t f[512] = { 0 }, *p = f + BAT_ETH_HLEN, bat0[6], br[6];
    addr(f + BAT_LINK_SRC, 0xA0, j);
    addr(bat0, 0xB0, k); addr(br, 0xC0, k);
    p[0] = BAT_PT_OGM2; p[1] = BAT_COMPAT; p[BAT_OGM_TTL] = j == k ? BAT_OGM_TTL_INIT : BAT_OGM_TTL_INIT - 1;
    bat_put32(p + BAT_OGM_SEQ, 7);
    addr(p + BAT_OGM_ORIG, 0xA0, k);
    bat_put32(p + BAT_OGM_TPUT, 100);
    uint8_t *t = p + BAT_OGM_HLEN;
    if (k < 2) {
        t[0] = BAT_TVLV_GW; t[1] = 1; bat_put16(t + 2, BAT_TVLV_GW_LEN);
        bat_put32(t + 4, 10 + k); bat_put32(t + 8, 5); /* below every route's throughput: node 1 is the best */
        t += BAT_TVLV_HLEN + BAT_TVLV_GW_LEN;
    }
    const uint16_t V0 = 0, V1 = BAT_VID_TAGGED | 1, vlen = 4 + 2 * BAT_TT_VLAN_LEN + 3 * BAT_TT_CHANGE_LEN;
    t[0] = BAT_TVLV_TT; t[1] = 1; bat_put16(t + 2, vlen);
    uint8_t *v = t + BAT_TVLV_HLEN, *c = v + 4 + 2 * BAT_TT_VLAN_LEN;
    v[0] = BAT_TT_OGM_DIFF; v[1] = 1; bat_put16(v + 2, 2);
    bat_put32(v + 4, bat_crc32c_tt(V0, 0, bat0) ^ bat_crc32c_tt(V0, 0, br)); bat_put16(v + 8, V0);
    bat_put32(v + 12, bat_crc32c_tt(V1, 0, bat0)); bat_put16(v + 16, V1);
    memcpy(c + 4, bat0, 6); bat_put16(c + 10, V0); c += BAT_TT_CHANGE_LEN;
    memcpy(c + 4, bat0, 6); bat_put16(c + 10, V1); c += BAT_TT_CHANGE_LEN;
    memcpy(c + 4, br, 6); bat_put16(c + 10, V0); c += BAT_TT_CHANGE_LEN;
    bat_put16(p + BAT_OGM_TVLV_LEN, (uint16_t)(c - (p + BAT_OGM_HLEN)));
    hook(f, (size_t)(c - f));
}
static unsigned lines(const char *buf, const char *pfx, const char *also)
{
    unsigned n = 0;
    for (const char *l = buf; *l;) {
        const char *e = strstr(l, "\r\n");
        const size_t ll = e ? (size_t)(e - l) : strlen(l);
        char tmp[512];
        const size_t c = ll < 511 ? ll : 511;
        memcpy(tmp, l, c);
        tmp[c] = 0;
        n += !strncmp(tmp, pfx, strlen(pfx)) && (!also || strstr(tmp, also));
        l += ll + (e ? 2 : 0);
    }
    return n;
}
typedef __typeof__(bat_gw_best(NULL, NULL)) gw_ret_t;
int main(void)
{
    f_verbose = getenv("BP_VERBOSE") != NULL;
    CHECK(warthog_bat_port_start() == ESP_OK && warthog_bat_port_running(), "started with the real engine");
    warthog_bat_port_mesh_up();
    for (uint32_t s = 1; s <= 3; s++) { for (unsigned j = 0; j < NB; j++) { elp(j, s); } }
    for (int i = 0; i < 400 && lines(at(BAT_RENDER_NEIGH, NULL), "+BATN: 02:a0:", " tput=2.5 ") < NB; i++) {
        sleep_ms(50); /* until our next ELP samples each link: AT+MESHBATTP's 2.5 Mbit/s */
    }
    for (unsigned k = 0; k < NODES; k++) { for (unsigned j = 0; j < NB; j++) { ogm(j, k); } }
    for (int i = 0; i < 2000 && warthog_bat_port_routes() < NODES; i++) { sleep_ms(1); }
    const char *st = at(BAT_RENDER_STAT, NULL);
    CHECK(warthog_bat_port_routes() == NODES && warthog_bat_port_neighs() == NB && strstr(st, " q_rx_full=0 ") &&
          strstr(st, " tt=96/") && lines(st, "+BATSTAT: port ", NULL) == 1 && at_ok == 1,
          "setup: %u routes, %u neighbours, 96 client rows, no frame dropped at the hook", warthog_bat_port_routes(),
          warthog_bat_port_neighs());
    if (fails) { printf("%s", st); }
    const char *o = at(BAT_RENDER_ORIG, NULL);
    CHECK(strlen(o) > BAT_RENDER_BUF && lines(o, "+BATO: 02:a0:", " seen=") == NODES &&
          lines(o, "+BATO:  via ", NULL) == NODES * NB && strstr(o, "+BATO: 02:a0:00:00:00:1f seen=") &&
          !strstr(o, "(truncated)") && at_ok == 1 && at_err == 0,
          "AT+BATO?: all %u originators (%u) and %u candidates (%u) over %zu bytes, node 31 with its ttvn, one OK",
          NODES, lines(o, "+BATO: 02:a0:", " seen="), NODES * NB, lines(o, "+BATO:  via ", NULL), strlen(o));
    const char *g = at(BAT_RENDER_TT_GLOBAL, NULL);
    CHECK(strlen(g) > BAT_RENDER_BUF && lines(g, "+BATTG: 02:", NULL) == NODES * 3 &&
          strstr(g, "+BATTG: 02:c0:00:00:00:1f") && !strstr(g, "(truncated)") && at_ok == 1 && at_err == 0,
          "AT+BATTG?: all %u client rows (%u) over %zu bytes, one OK", NODES * 3, lines(g, "+BATTG: 02:", NULL),
          strlen(g));
    o = at(BAT_RENDER_ORIG, "02:a0:00:00:00:1f");
    CHECK(lines(o, "+BATO: 02:a0:", " seen=") == 1 && strstr(o, "+BATO: 02:a0:00:00:00:1f seen=") &&
          lines(o, "+BATO:  via ", NULL) == NB && at_ok == 1, "AT+BATO=<node 31>: its row and its %u candidates alone",
          NB);
    g = at(BAT_RENDER_TT_GLOBAL, "02:a0:00:00:00:1f");
    CHECK(lines(g, "+BATTG: 02:", NULL) == 3 && lines(g, "+BATTG: 02:", "via=02:a0:00:00:00:1f ") == 3 && at_ok == 1,
          "AT+BATTG=<node 31>: exactly its 3 client rows (%u)", lines(g, "+BATTG: 02:", NULL));
    g = at(BAT_RENDER_TT_GLOBAL, "02:c0:00:00:00:1f");
    CHECK(lines(g, "+BATTG: 02:", NULL) == 1 && strstr(g, "+BATTG: 02:c0:00:00:00:1f") && at_ok == 1,
          "AT+BATTG=<a client>: that row alone");
    struct bat_gw gw;
    unsigned gn = 0;
    for (int i = 0; i < 1500 && !gn; i++) { sleep_ms(1); gn = warthog_bat_port_gw(&gw); }
    CHECK(gn == (uint8_t)(gw_ret_t)2 && gw.orig[1] == 0xA0 && gw.orig[5] == 1 && gw.down == 11,
          "gateways: bat_gw_best's answer for two (%u), node 1 the best", gn);
    printf(fails ? "bat_port_scale: %d FAILED\n" : "bat_port_scale: all passed\n", fails);
    return fails != 0;
}
EOF
why=""
if ${CC:-cc} -std=gnu11 -w -pthread ${SANFLAGS:-} -DWARTHOG_MESH_SAE=1 -DWARTHOG_MESH_HOST_CCMP=1 -I"$T" -I"$T/inc" \
     -I../../../main -I../../../main/bat -o "$T/scale" "$T/scale.c" "$BP" ../../../main/bat_mode.c ../../../main/bat/*.c \
     2>"$T/cc.log"; then
  "$T/scale" > "$T/scale.log" 2>&1 || why=" $(grep -m3 -E '^FAIL|ERROR|Sanitizer' "$T/scale.log" | tr '\n' ' ')"
else
  why=" did not build: $(head -3 "$T/cc.log" | tr '\n' ' ')"
fi
if [ -z "$why" ]; then
  ok "bat_port.c with the real engine: 32 originators x 4 neighbours, AT+BATO?/AT+BATTG? whole over several chunks, =<mac> one node, gateways handed on"
else
  bad "bat_port.c with the real engine:$why"
fi
rm -rf "$T"

# 35. mesh.c brings bat0 up from the batman engine: the probe task polls it only while
#     batman runs (else the plain first-ESTAB bring-up), mmwlan_mesh_enable's result reaches
#     the port either way, and mesh_bat_netif_poll_ is compiled out of mesh.c and run per
#     2 s tick against a model of esp_netif's DHCP-client rules, lwIP's DHCP client and ARP
#     table, DHCP servers and the engine's answers: a lease 36 s after the first route, or
#     one still being requested or ARP-checked at the 45 s deadline, is kept; a lease renewing
#     or rebinding stays leased; no lease ends on a probed static candidate via 10.41.0.1; a
#     held address asks DHCP again beside itself (never dropped, re-applied after a NAK, but
#     never over a lease lwIP bound or is ARP-checking since the tick read it) 30 s later, then
#     after 60 s, sooner when a gateway appears (the port's count, so a second one too); a
#     lease whose router stops resolving for 60 s
#     asks again keeping its address, the wait doubling while the same router comes back and
#     still fails, and a live router whose MAC left TT is ARPed back first; AT+MESHDHCP=0 never
#     asks; and every lwIP call it makes runs inside esp_netif_tcpip_exec.
MC=../../../main/mesh.c
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
awk '/^\/\* Batman mode: bat0 comes up/ {p=1} /^static void mesh_probe_burst_task/ {exit} p' "$MC" > "$T/fn.c"
cat > "$T/t.c" <<'EOF'
/* mesh_bat_netif_poll_, compiled out of main/mesh.c, one call per 2 s probe tick against a model
 * of esp_netif (IDF's DHCP-client rules), lwIP's DHCP client and ARP table, DHCP servers and the
 * engine's answers. lwIP state is only touched inside esp_netif_tcpip_exec, or counted. */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "bat.h"
#include "bat_mode.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_STOPPED 0x5001
#define ESP_ERR_ALREADY 0x5002
typedef int err_t;
#define ERR_OK 0
#define ERR_ARG (-16)
typedef struct { uint32_t addr; } esp_ip4_addr_t, ip4_addr_t;
#define ip4_addr_isany_val(a) ((a).addr == 0)
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
typedef enum { ESP_NETIF_DHCP_INIT, ESP_NETIF_DHCP_STARTED, ESP_NETIF_DHCP_STOPPED } esp_netif_dhcp_status_t;
typedef struct esp_netif_obj esp_netif_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
struct dhcp { uint8_t state; };
#define DHCP_STATE_OFF 0
#define DHCP_STATE_REQUESTING 1
#define DHCP_STATE_REBINDING 4
#define DHCP_STATE_RENEWING 5
#define DHCP_STATE_SELECTING 6
#define DHCP_STATE_CHECKING 8
#define DHCP_STATE_BOUND 10
struct netif { ip4_addr_t ip, mask, gw; struct dhcp *dhcp; int up; };
struct eth_addr { uint8_t addr[6]; };
#define IP4_ADDR(p, a, b, c, d) ((p)->addr = (uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(p) (unsigned)((p)->addr & 0xff), (unsigned)((p)->addr >> 8 & 0xff), (unsigned)((p)->addr >> 16 & 0xff), (unsigned)((p)->addr >> 24)
static int verbose, in_tcpip, outside, in_esp;
#define LWIP_CTX() (outside += !in_tcpip)
static void lg(const char *fmt, ...) { if (verbose) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); puts(""); } }
#define ESP_LOGI(tag, ...) lg(__VA_ARGS__)
#define ESP_LOGW(tag, ...) lg(__VA_ARGS__)
static const char *TAG = "t";
#define MESH_PROBE_BURST_PERIOD_MS 2000

/* ---- the world ---- */
static struct esp_netif_obj { esp_netif_dhcp_status_t st; esp_netif_ip_info_t cache; } N;
static struct netif LW;
static struct dhcp D;
static int64_t clock_us;
static uint8_t cfg_dhcp;
static unsigned routes;
static int bad_set, dhcp_starts, raw_starts, raw_tries, releases, probes, probe_with_ip, dhcp_broken, tick;
static uint32_t held[4];
static int nheld, stable[256];
/* DHCP server: offers to a client that has run srv_delay_ms, which then requests for srv_req_ms
 * and ARP-checks the offer for check_ms before the lease binds; a NAK at nak_ms clears the
 * address. Any address set during the check suspends it for good (lwIP's netif.c). */
static int srv_on, srv_delay_ms, srv_req_ms, check_ms, client_ms, nak_ms, acd_off, set_in_check;
static uint32_t lease_ip, lease_router;
/* The router: answers ARP while rtr_up, heard rtr_age_ms ago. Its node's TT row for its MAC
 * (rtr_tt) goes 600 s after it last sent into batman and an ARP reply puts it back, unless
 * tt_full (the engine's table cannot take it); lwIP's ARP entry for it (rtr_arp_ms old) goes at 300 s. */
static uint32_t rtr_ip;
static int rtr_up, rtr_tt, rtr_learned, rtr_queries, tt_full;
static uint32_t rtr_age_ms, rtr_arp_ms;
/* lwIP binds the lease on the tcpip thread just before the next HOLD op (its ARP check ended). */
static int bind_before_hold;
static struct eth_addr RTR_MAC = { { 0x02, 0xd4, 0x0b, 0x00, 0x00, 0xff } };
static uint8_t watched[6];
static int watching, gw_on;

static uint32_t A4(unsigned a, unsigned b, unsigned c, unsigned d) { ip4_addr_t x; IP4_ADDR(&x, a, b, c, d); return x.addr; }
static esp_netif_t *mmhalow_get_netif(void) { return &N; }
static uint8_t warthog_cfg_get_mesh_dhcp(void) { return cfg_dhcp; }
static unsigned warthog_bat_port_routes(void) { return routes; }
static void warthog_bat_port_soft_mac(uint8_t m[6]) { static const uint8_t s[6] = { 6, 0xca, 0x43, 0x3c, 0x24, 0x28 }; memcpy(m, s, 6); }
static void warthog_bat_port_watch(const uint8_t mac[6]) { watching = mac != NULL; if (mac) memcpy(watched, mac, 6); }
static int warthog_bat_port_watch_answer(const uint8_t mac[6], struct bat_client_route *r)
{
    if (!watching || memcmp(mac, watched, 6)) return -1;
    memset(r, 0, sizeof(*r));
    r->ogm_age_ms = rtr_age_ms;
    return rtr_up && rtr_tt;
}
static uint8_t warthog_bat_port_gw(struct bat_gw *g) /* gw_on: how many gateways have a route */
{
    memset(g, 0, sizeof(*g));
    if (gw_on) { g->orig[0] = 2; g->down = 100; g->up = 20; }
    return (uint8_t)gw_on;
}
static int64_t esp_timer_get_time(void) { return clock_us; }

/* lwIP (tcpip context only) */
static void lw_set(uint32_t ip, uint32_t mask, uint32_t gw) { LW.ip.addr = ip; LW.mask.addr = mask; LW.gw.addr = gw; }
static err_t dhcp_start(struct netif *l)
{
    LWIP_CTX();
    if (!l->up) return ERR_ARG;
    raw_tries += !in_esp;
    if (dhcp_broken) return -1;
    l->dhcp = &D;
    D.state = DHCP_STATE_SELECTING; /* the address stays until a lease binds */
    client_ms = 0;
    raw_starts += !in_esp; /* mesh.c's own starts, beside an address */
    return ERR_OK;
}
/* lwIP's dhcp_supplied_address: a lease is held while bound, renewing (T1) or rebinding (T2). */
static int lw_lease(void) { return D.state == DHCP_STATE_BOUND || D.state == DHCP_STATE_RENEWING || D.state == DHCP_STATE_REBINDING; }
static void dhcp_release_and_stop(struct netif *l)
{
    LWIP_CTX();
    if (!l->dhcp || D.state == DHCP_STATE_OFF) return;
    if (lw_lease()) { releases++; lw_set(0, 0, 0); }
    D.state = DHCP_STATE_OFF;
}
static uint8_t dhcp_supplied_address(const struct netif *l) { LWIP_CTX(); return l->dhcp && lw_lease(); }
static struct dhcp *netif_dhcp_data_(struct netif *l) { LWIP_CTX(); return l->dhcp; }
#define netif_dhcp_data(l) netif_dhcp_data_(l)
static const ip4_addr_t *lw_addr(const ip4_addr_t *a) { LWIP_CTX(); return a; }
#define netif_ip4_addr(l) lw_addr(&(l)->ip)
#define netif_ip4_netmask(l) lw_addr(&(l)->mask)
#define netif_ip4_gw(l) lw_addr(&(l)->gw)
#define ip4_addr_get_u32(p) ((p)->addr)
static void netif_set_addr(struct netif *l, const ip4_addr_t *ip, const ip4_addr_t *m, const ip4_addr_t *g)
{
    LWIP_CTX();
    set_in_check += D.state == DHCP_STATE_CHECKING;
    acd_off |= D.state == DHCP_STATE_CHECKING;
    l->ip = *ip; l->mask = *m; l->gw = *g;
}
static int etharp_find_addr(struct netif *l, const ip4_addr_t *ip, struct eth_addr **e, const ip4_addr_t **r)
{
    (void)l; (void)r;
    LWIP_CTX();
    if (ip->addr == rtr_ip && rtr_ip) { if (!rtr_learned || rtr_arp_ms > 300000) return -1; *e = &RTR_MAC; return 0; }
    return stable[ip->addr >> 24] ? 0 : -1;
}
static int etharp_query(struct netif *l, const ip4_addr_t *ip, void *q)
{
    (void)l; (void)q;
    LWIP_CTX();
    if (ip->addr == rtr_ip && rtr_ip) { /* a broadcast request; the router's reply re-enters its TT */
        rtr_queries++;
        rtr_learned |= rtr_up;
        if (rtr_up) { rtr_tt = !tt_full; rtr_arp_ms = 0; }
        return 0;
    }
    probes++;
    probe_with_ip += LW.ip.addr != 0;
    for (int i = 0; i < nheld; i++) if (held[i] == ip->addr) stable[ip->addr >> 24] = 1; /* its holder replies */
    return 0;
}

/* esp_netif (thread-safe: IPC into tcpip), IDF's DHCP-client rules */
static void *esp_netif_get_netif_impl(esp_netif_t *n) { (void)n; return &LW; }
static void (*tcpip_first)(esp_err_t (*fn)(void *), void *ctx); /* lwIP's own work, due before the call */
static esp_err_t esp_netif_tcpip_exec(esp_err_t (*fn)(void *), void *ctx)
{
    int was = in_tcpip;
    in_tcpip = 1;
    if (tcpip_first) tcpip_first(fn, ctx);
    esp_err_t e = fn(ctx);
    in_tcpip = was;
    return e;
}
static esp_err_t esp_netif_dhcpc_start(esp_netif_t *n)
{
    if (n->st == ESP_NETIF_DHCP_STARTED) return ESP_ERR_ALREADY;
    memset(&n->cache, 0, sizeof(n->cache));
    if (!LW.up) { n->st = ESP_NETIF_DHCP_INIT; return ESP_OK; }
    lw_set(0, 0, 0);
    in_tcpip++;
    in_esp++;
    err_t e = dhcp_start(&LW);
    in_esp--;
    in_tcpip--;
    if (e != ERR_OK) return ESP_FAIL; /* the status stays */
    n->st = ESP_NETIF_DHCP_STARTED;
    dhcp_starts++;
    return ESP_OK;
}
static esp_err_t esp_netif_dhcpc_stop(esp_netif_t *n)
{
    if (n->st == ESP_NETIF_DHCP_STOPPED) return ESP_ERR_ALREADY;
    if (n->st == ESP_NETIF_DHCP_STARTED) {
        in_tcpip++;
        dhcp_release_and_stop(&LW);
        in_tcpip--;
        memset(&n->cache, 0, sizeof(n->cache));
    }
    n->st = ESP_NETIF_DHCP_STOPPED;
    return ESP_OK;
}
static void esp_netif_action_connected(void *n_, const char *b, int id, void *d)
{
    (void)b; (void)id; (void)d;
    esp_netif_t *n = n_;
    LW.up = 1;
    lw_set(n->cache.ip.addr, n->cache.netmask.addr, n->cache.gw.addr); /* esp_netif_up: the cached address */
    if (n->st == ESP_NETIF_DHCP_INIT) (void)esp_netif_dhcpc_start(n);
}
static esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *i)
{
    if (!LW.up) { *i = n->cache; return ESP_OK; }
    i->ip = LW.ip; i->netmask = LW.mask; i->gw = LW.gw;
    return ESP_OK;
}
static esp_err_t esp_netif_set_ip_info(esp_netif_t *n, const esp_netif_ip_info_t *i)
{
    if (n->st != ESP_NETIF_DHCP_STOPPED) { bad_set++; return ESP_ERR_NOT_STOPPED; }
    n->cache = *i;
    if (LW.up) lw_set(i->ip.addr, i->netmask.addr, i->gw.addr);
    return ESP_OK;
}
#include "fn.c"

static void bind_first(esp_err_t (*fn)(void *), void *ctx)
{
    if (bind_before_hold && fn == mesh_bat_io_ && ((struct mesh_bat_io *)ctx)->op == MESH_BAT_IO_HOLD) {
        bind_before_hold = 0;
        D.state = DHCP_STATE_BOUND;
        lw_set(lease_ip, A4(255, 255, 0, 0), lease_router);
    }
}

static int fails;
#define CHECK(c, ...) do { int c_ = !!(c); if (!c_) { fails++; printf(__VA_ARGS__); printf(" | "); } } while (0)
static void reset(int dhcp)
{
    memset(&N, 0, sizeof(N));
    memset(&LW, 0, sizeof(LW));
    memset(&D, 0, sizeof(D));
    memset(&s_bat0, 0, sizeof(s_bat0));
    memset(&s_bat_rtr, 0, sizeof(s_bat_rtr));
    memset(stable, 0, sizeof(stable));
    s_bat_poll_us = 0;
    clock_us = 1000000;
    bad_set = dhcp_starts = raw_starts = raw_tries = releases = probes = probe_with_ip = tick = nheld = dhcp_broken = 0;
    srv_on = client_ms = 0;
    srv_delay_ms = 4000;
    srv_req_ms = check_ms = acd_off = set_in_check = bind_before_hold = 0;
    nak_ms = -1;
    lease_ip = A4(10, 41, 0, 102);
    lease_router = rtr_ip = A4(10, 41, 0, 2);
    rtr_up = rtr_tt = 1;
    rtr_learned = rtr_queries = tt_full = 0;
    rtr_age_ms = 400;
    rtr_arp_ms = 0;
    watching = gw_on = 0;
    cfg_dhcp = (uint8_t)dhcp;
    routes = 1;
    tcpip_first = bind_first;
}
static void step(void)
{
    mesh_bat_netif_poll_();
    clock_us += 2000000;
    tick++;
    rtr_arp_ms += 2000;
    if (LW.dhcp && D.state != DHCP_STATE_OFF && !lw_lease()) { /* the server */
        client_ms += 2000;
        if (client_ms == nak_ms) lw_set(0, 0, 0);
        if (srv_on && client_ms >= srv_delay_ms && D.state == DHCP_STATE_SELECTING) {
            D.state = DHCP_STATE_REQUESTING;
        }
        if (srv_on && client_ms >= srv_delay_ms + srv_req_ms && D.state == DHCP_STATE_REQUESTING) {
            D.state = check_ms ? DHCP_STATE_CHECKING : DHCP_STATE_BOUND;
        }
        if (D.state == DHCP_STATE_CHECKING && !acd_off && client_ms >= srv_delay_ms + srv_req_ms + check_ms) {
            D.state = DHCP_STATE_BOUND;
        }
        if (D.state == DHCP_STATE_BOUND) {
            lw_set(lease_ip, A4(255, 255, 0, 0), lease_router);
        }
    }
}
static unsigned cand(unsigned attempt)
{
    uint8_t s[6], a[4];
    warthog_bat_port_soft_mac(s);
    bat_mode_static_ip(s, attempt, a);
    return A4(a[0], a[1], a[2], a[3]);
}
/* Steps until the static candidate is on bat0 (at most 60); returns the ticks taken. */
static int to_static(void)
{
    int t = 0;
    while (t < 60 && !(LW.ip.addr && N.st == ESP_NETIF_DHCP_STOPPED && LW.ip.addr == N.cache.ip.addr)) { step(); t++; }
    return t;
}
int main(int argc, char **argv)
{
    verbose = argc > 1;
    const uint32_t GW = A4(10, 41, 0, 1), MASK = A4(255, 255, 0, 0);
    char line[BAT_MODE_BAT0_LINE];

    reset(1); /* a node that booted with us: its broadcast hold ends ~31 s in; lease at 36 s */
    srv_on = 1;
    srv_delay_ms = 36000;
    for (int i = 0; i < 40; i++) step();
    CHECK(LW.ip.addr == lease_ip && N.st == ESP_NETIF_DHCP_STARTED && probes == 0 && dhcp_starts == 1,
          "lease at 36 s after the first route not kept (ip %08x st %d probes %d)", LW.ip.addr, N.st, probes);
    (void)warthog_mesh_bat0_line(line, sizeof(line));
    CHECK(strstr(line, " addr=leased ip=10.41.0.102 router=10.41.0.2(ok) "), "AT line while leased: %s", line);

    reset(1); /* the offer comes at 44 s and its REQUEST/ACK takes 4 s: past the 45 s deadline */
    srv_on = 1;
    srv_delay_ms = 44000;
    srv_req_ms = 4000;
    for (int i = 0; i < 30; i++) step();
    CHECK(LW.ip.addr == lease_ip && probes == 0 && dhcp_starts == 1,
          "an offer taken across the 45 s deadline was cut off (ip %08x probes %d)", LW.ip.addr, probes);

    reset(1);
    routes = 0;
    for (int i = 0; i < 5; i++) step();
    CHECK(LW.up == 0 && dhcp_starts == 0, "bat0 came up with no route");
    routes = 1;
    nheld = 1;
    held[0] = cand(0);
    int t_probe = -1;
    for (int i = 0; i < 40; i++) { step(); if (probes && t_probe < 0) t_probe = tick; }
    CHECK(dhcp_starts == 1 && t_probe >= 5 + 23, "DHCP gave up before 45 s (first probe at tick %d)", t_probe);
    CHECK(LW.ip.addr == cand(1) && LW.gw.addr == GW && LW.mask.addr == MASK && N.st == ESP_NETIF_DHCP_STOPPED &&
          D.state == DHCP_STATE_OFF && bad_set == 0, "no lease, candidate 0 answered: want candidate 1/16 via 10.41.0.1, "
          "DHCP stopped (ip %08x gw %08x mask %08x st %d dhcp %d bad_set %d)", LW.ip.addr, LW.gw.addr, LW.mask.addr,
          N.st, D.state, bad_set);
    CHECK(probe_with_ip == 0 && probes >= 3, "ARP probes sent while bat0 had an address, or too few (%d)", probes);
    (void)warthog_mesh_bat0_line(line, sizeof(line));
    CHECK(strstr(line, " addr=static ") && strstr(line, " router=10.41.0.1 retry_in="), "AT line while static: %s", line);
    routes = 0;
    int flap = 0;
    for (int i = 0; i < 20; i++) { step(); flap += LW.ip.addr != cand(1); }
    CHECK(flap == 0 && raw_starts == 0, "no route: the static address kept, DHCP not asked (%d ticks off it)", flap);
    routes = 1;
    srv_on = 1;
    int t = 0;
    while (t < 5 && raw_starts == 0) { step(); t++; flap += LW.ip.addr == 0; }
    CHECK(raw_starts == 1 && t == 1, "a route again after 40 s: DHCP asked beside the address at once (tick %d)", t);
    for (int i = 0; i < 3; i++) { step(); flap += LW.ip.addr == 0; }
    CHECK(flap == 0 && LW.ip.addr == lease_ip && LW.gw.addr == lease_router && dhcp_starts == 1 && bad_set == 0,
          "and a lease replaced the static address without a tick at 0.0.0.0 (flap %d ip %08x)", flap, LW.ip.addr);

    reset(1);
    int ts = to_static();
    uint32_t st_ip = LW.ip.addr;
    t = 0;
    while (t < 40 && raw_starts == 0) { step(); t++; }
    CHECK(st_ip == cand(0) && raw_starts == 1 && t == 15, "static, no server: the first attempt 30 s later (tick %d)", t);
    flap = 0;
    t = 0;
    while (t < 40 && D.state != DHCP_STATE_OFF) { step(); t++; flap += LW.ip.addr != st_ip; }
    CHECK(flap == 0 && t == 23 && LW.ip.addr == st_ip && LW.gw.addr == GW && N.st == ESP_NETIF_DHCP_STOPPED &&
          N.cache.ip.addr == st_ip && bad_set == 0, "the attempt keeps the address for its 46 s, then stops the client and "
          "leaves it (t %d flap %d ip %08x st %d)", t, flap, LW.ip.addr, N.st);
    t = 0;
    while (t < 60 && raw_starts == 1) { step(); t++; }
    CHECK(raw_starts == 2 && t == 30, "the next attempt 60 s later (tick %d)", t);
    for (int i = 0; i < 40 && D.state != DHCP_STATE_OFF; i++) step();
    for (int i = 0; i < 5; i++) step();
    gw_on = 1;
    t = 0;
    while (t < 60 && raw_starts == 2) { step(); t++; }
    CHECK(raw_starts == 3 && t == 10, "a gateway appears 10 s after the last attempt: the next 30 s after it, "
          "not 120 s (tick %d)", t);
    (void)ts;

    reset(1); /* gate A known all along; gate B joins while A still routes, then A's route goes */
    gw_on = 1;
    (void)to_static();
    for (int i = 0; i < 40 && raw_starts == 0; i++) step();
    for (int i = 0; i < 40 && D.state != DHCP_STATE_OFF; i++) step();
    t = 0;
    while (t < 60 && raw_starts == 1) { step(); t++; }
    CHECK(raw_starts == 2 && t == 30, "one gateway all along: the next attempt 60 s later (tick %d)", t);
    for (int i = 0; i < 40 && D.state != DHCP_STATE_OFF; i++) step();
    for (int i = 0; i < 5; i++) step();
    gw_on = 2;
    t = 0;
    while (t < 60 && raw_starts == 2) { step(); t++; }
    CHECK(raw_starts == 3 && t == 10, "a second gateway 10 s after the last attempt: the next 30 s after it, not "
          "120 s (tick %d)", t);
    for (int i = 0; i < 40 && D.state != DHCP_STATE_OFF; i++) step();
    gw_on = 1;
    t = 0;
    while (t < 60 && raw_starts == 3) { step(); t++; }
    CHECK(raw_starts == 4 && t == 30, "then A's route goes (one fewer is no new gateway): the wait doubles on, "
          "60 s (tick %d)", t);

    reset(1);
    (void)to_static();
    st_ip = LW.ip.addr;
    nak_ms = 4000;
    for (int i = 0; i < 40 && raw_starts == 0; i++) step();
    int zero = 0;
    for (int i = 0; i < 23; i++) { step(); zero += LW.ip.addr == 0; }
    CHECK(zero == 1 && LW.ip.addr == st_ip && N.st == ESP_NETIF_DHCP_STOPPED,
          "a NAK took the address mid-attempt: back on the next tick (%d ticks without), held after", zero);

    reset(1); /* lwIP binds between the tick's READ and its HOLD */
    (void)to_static();
    nak_ms = 4000;
    for (int i = 0; i < 40 && raw_starts == 0; i++) step();
    bind_before_hold = 1;
    for (int i = 0; i < 3; i++) step();
    (void)warthog_mesh_bat0_line(line, sizeof(line));
    CHECK(bind_before_hold == 0 && LW.ip.addr == lease_ip && LW.gw.addr == lease_router &&
          strstr(line, " addr=leased ip=10.41.0.102 router=10.41.0.2"), "a lease bound after the tick read no "
          "address: the held address went over it (ip %08x gw %08x) %s", LW.ip.addr, LW.gw.addr, line);

    reset(1); /* a NAK, then another server's ACK, before the next tick: it reads the ARP check running */
    (void)to_static();
    srv_on = 1;
    nak_ms = srv_delay_ms = check_ms = 2000;
    for (int i = 0; i < 40 && raw_starts == 0; i++) step();
    for (int i = 0; i < 3; i++) step();
    CHECK(set_in_check == 0 && D.state == DHCP_STATE_BOUND && LW.ip.addr == lease_ip, "the held address was set "
          "during lwIP's ARP check of an offer (%d), which that suspends (dhcp %d ip %08x)", set_in_check, D.state,
          LW.ip.addr);

    reset(1);
    srv_on = 1;
    for (int i = 0; i < 5; i++) step();
    CHECK(LW.ip.addr == lease_ip && rtr_learned && rtr_queries == 1, "leased; the router's MAC learned with one ARP "
          "request (%d)", rtr_queries);
    for (int i = 0; i < 60; i++) step();
    CHECK(raw_starts == 0 && dhcp_starts == 1 && rtr_queries == 1, "router heard for 2 min: the lease is left alone, "
          "and not ARPed again (%d)", rtr_queries);
    rtr_up = 0;
    lease_ip = A4(10, 41, 7, 9);
    lease_router = A4(10, 41, 7, 1);
    t = 0;
    zero = 0;
    while (t < 60 && raw_starts == 0) { step(); t++; zero += LW.ip.addr == 0; }
    CHECK(raw_starts == 1 && t == BAT_MODE_ROUTER_LOSS_MS / 2000, "the router stops resolving: DHCP again after 60 s "
          "(tick %d)", t);
    for (int i = 0; i < 3; i++) { step(); zero += LW.ip.addr == 0; }
    CHECK(zero == 0 && LW.ip.addr == lease_ip && LW.gw.addr == lease_router && bad_set == 0,
          "another node's lease, the old address kept until it bound (%d ticks at 0.0.0.0, ip %08x)", zero, LW.ip.addr);
    CHECK(rtr_ip != LW.gw.addr, "setup: the new router is another address");

    reset(1);
    srv_on = 1;
    for (int i = 0; i < 5; i++) step();
    const uint32_t old_ip = LW.ip.addr;
    rtr_up = srv_on = 0;
    zero = 0;
    t = 0;
    while (t < 100 && !(raw_starts == 1 && D.state == DHCP_STATE_OFF)) { step(); t++; zero += LW.ip.addr != old_ip; }
    CHECK(zero == 0 && LW.ip.addr == old_ip && LW.gw.addr == GW && LW.mask.addr == MASK && N.st == ESP_NETIF_DHCP_STOPPED &&
          N.cache.ip.addr == old_ip && N.cache.gw.addr == GW && releases == 0 && bad_set == 0,
          "router gone, no server: the lease address held via 10.41.0.1, esp_netif agreeing, nothing released "
          "(ip %08x gw %08x st %d cache %08x)", LW.ip.addr, LW.gw.addr, N.st, N.cache.ip.addr);
    (void)warthog_mesh_bat0_line(line, sizeof(line));
    CHECK(strstr(line, " addr=held ip=10.41.0.102 router=10.41.0.1 retry_in=30s ") && strstr(line, " restarts=1 "),
          "AT line while holding a former lease: %s", line);

    reset(1);
    srv_on = 1;
    rtr_up = 1;
    rtr_age_ms = BAT_MODE_ROUTER_OGM_MS + 1;
    t = 0;
    while (t < 80 && raw_starts == 0) { step(); t++; }
    CHECK(raw_starts == 1 && t >= 31 && t <= 33, "a router TT still resolves but whose OGMs stopped: DHCP again "
          "after 60 s (tick %d)", t);

    reset(1);
    srv_on = 1;
    rtr_up = 0;
    t = 0;
    while (t < 80 && raw_starts == 0) { step(); t++; }
    CHECK(raw_starts == 1 && t >= 31 && t <= 33 && rtr_queries >= 5 && rtr_queries <= 7,
          "a router MAC that never resolves: DHCP again after 60 s (tick %d), ARPed every 10 s, not every tick (%d)",
          t, rtr_queries);

    reset(1); /* an idle live router: 10 min without a frame from it, its OGMs going on */
    srv_on = 1;
    for (int i = 0; i < 305; i++) step();
    int q0 = rtr_queries, lost = 0;
    rtr_tt = 0;
    for (int i = 0; i < 100; i++) { step(); lost += s_bat_view.router == BAT_MODE_ROUTER_LOST; }
    CHECK(raw_starts == 0 && rtr_tt && rtr_queries == q0 + 1 && lost <= 2, "a live router whose MAC left TT: ARPed "
          "back, the lease left alone (DHCP restarts %d, ARPs %d, ticks lost %d)", raw_starts, rtr_queries - q0, lost);
    for (int i = 0; i < 20; i++) step();
    q0 = rtr_queries;
    lost = 0;
    rtr_tt = 0; /* its node restarted batman: the row goes while lwIP still holds the router's MAC */
    for (int i = 0; i < 40; i++) { step(); lost += s_bat_view.router == BAT_MODE_ROUTER_LOST; }
    CHECK(raw_starts == 0 && rtr_tt && rtr_queries == q0 + 1 && lost <= 2, "the same with lwIP's ARP entry still "
          "fresh (DHCP restarts %d, ARPs %d, ticks lost %d)", raw_starts, rtr_queries - q0, lost);
    rtr_tt = rtr_up = 0;
    const int s0 = raw_starts;
    q0 = rtr_queries;
    t = 0;
    while (t < 60 && raw_starts == s0) { step(); t++; }
    CHECK(raw_starts == s0 + 1 && t == BAT_MODE_ROUTER_LOSS_MS / 2000 && rtr_queries - q0 >= 5 && rtr_queries - q0 <= 7,
          "a dead router whose MAC left TT: DHCP again after 60 s (tick %d), ARPed every 10 s (%d)", t, rtr_queries - q0);

    reset(1); /* the router answers ARP but its row never fits the engine's TT; the server hands the same lease back */
    srv_on = tt_full = 1;
    rtr_tt = 0;
    int leased_at = -1, nw = 0, waits[4] = { 0 };
    for (int i = 0; i < 600 && nw < 4; i++) {
        const int before = raw_starts;
        step();
        if (leased_at < 0 && bat_mode_bat0_leased(&s_bat0)) leased_at = tick;
        if (raw_starts > before && leased_at >= 0) { waits[nw++] = (tick - leased_at) * 2; leased_at = -1; }
    }
    (void)warthog_mesh_bat0_line(line, sizeof(line));
    CHECK(nw == 4 && waits[0] == 60 && waits[1] == 120 && waits[2] == 240 && waits[3] == 480 &&
          LW.ip.addr == lease_ip && strstr(line, " restarts=4 "), "a router the check never passes, the same lease "
          "back each time: DHCP again after 60, 120, 240, 480 s, not every 60 s (%d restarts: %d %d %d %d s) %s", nw,
          waits[0], waits[1], waits[2], waits[3], line);

    reset(1);
    (void)to_static();
    dhcp_broken = 1;
    int t1 = -1, t2 = -1;
    for (int i = 0; i < 80 && t2 < 0; i++) {
        int before = raw_tries;
        step();
        if (raw_tries > before) { if (t1 < 0) t1 = i; else t2 = i; }
    }
    CHECK(t1 == 14 && t2 - t1 == 31 && raw_starts == 0 && LW.ip.addr == cand(0),
          "a background start that fails ends that attempt on the next tick: the next one 60 s later "
          "(tries at ticks %d, %d)", t1, t2);

    reset(1);
    dhcp_broken = 1;
    for (int i = 0; i < 4; i++) step();
    CHECK(dhcp_starts == 0 && LW.ip.addr == cand(0) && probe_with_ip == 0 && bad_set == 0,
          "the DHCP client would not start: want candidate 0 after two quiet ticks, not a 45 s wait (ip %08x)", LW.ip.addr);

    reset(0);
    for (int i = 0; i < 4; i++) step();
    CHECK(dhcp_starts == 0 && LW.ip.addr == cand(0) && LW.gw.addr == GW && LW.up && bad_set == 0,
          "AT+MESHDHCP=0: want candidate 0 after two quiet ticks, no DHCP (starts %d ip %08x)", dhcp_starts, LW.ip.addr);
    srv_on = gw_on = 1;
    for (int i = 0; i < 400; i++) { routes = (unsigned)(i / 7) & 1; step(); }
    CHECK(dhcp_starts == 0 && raw_starts == 0 && LW.ip.addr == cand(0), "AT+MESHDHCP=0 never asks DHCP");

    reset(1); /* the ACK at 42 s; lwIP's ARP check of it (PROBE_WAIT, 3 probes, ANNOUNCE_WAIT: 4-7 s) spans 45 s */
    srv_on = 1;
    srv_delay_ms = 40000;
    srv_req_ms = 2000;
    check_ms = 6000;
    int seen_check = 0;
    for (int i = 0; i < 35; i++) {
        const int was = D.state;
        step();
        seen_check += was == DHCP_STATE_CHECKING && s_bat0.ms >= BAT_MODE_DHCP_WAIT_MS;
    }
    CHECK(seen_check >= 1 && LW.ip.addr == lease_ip && D.state == DHCP_STATE_BOUND && probes == 0 && dhcp_starts == 1,
          "an offer lwIP was ARP-checking at the 45 s deadline was cut off (checked past it %d, ip %08x dhcp %d "
          "probes %d)", seen_check, LW.ip.addr, D.state, probes);

    reset(1); /* T1: the unicast renewal goes unanswered for 80 s, T2: the broadcast one for 60 s, then an ACK */
    srv_on = 1;
    for (int i = 0; i < 5; i++) step();
    int off_lease = 0;
    for (int i = 0; i < 70; i++) {
        if (i == 0 || (i == 40 && D.state == DHCP_STATE_RENEWING)) D.state = i ? DHCP_STATE_REBINDING : DHCP_STATE_RENEWING;
        step();
        (void)warthog_mesh_bat0_line(line, sizeof(line));
        off_lease += !strstr(line, " addr=leased ") || LW.ip.addr != lease_ip;
    }
    if (D.state == DHCP_STATE_REBINDING) D.state = DHCP_STATE_BOUND; /* the ACK */
    for (int i = 0; i < 3; i++) step();
    CHECK(off_lease == 0 && raw_starts == 0 && dhcp_starts == 1 && probes == 0 && releases == 0 &&
          LW.ip.addr == lease_ip, "a lease renewing or rebinding was taken for a lost one (%d ticks off it, restarts %d "
          "probes %d releases %d)", off_lease, raw_starts, probes, releases);

    CHECK(outside == 0, "%d lwIP calls outside tcpip context", outside);
    return fails != 0;
}
EOF
why=""
if ! ${CC:-cc} -std=gnu11 -w ${SANFLAGS:-} -I"$T" -I../../../main -I../../../main/bat -o "$T/t" "$T/t.c" \
       ../../../main/bat_mode.c 2>"$T/cc.log"; then
  why="did not build: $(head -3 "$T/cc.log" | tr '\n' ' ')"
elif ! why=$("$T/t" 2>&1); then
  why="${why:-crashed}"
else
  why=""
fi
rm -rf "$T"
if ! awk '/^static void mesh_probe_burst_task\(/,/^}/' "$MC" | \
       awk '/if \(warthog_bat_port_running\(\)\) \{/ {r=NR} r && NR == r + 1 && /^ *mesh_bat_netif_poll_\(\);/ {p=NR}
            p && NR == p + 1 && /} else if \(g_warthog_mpm_estab \|\| g_warthog_hostap_estab\) \{/ {e=NR}
            e && NR == e + 1 && /^ *mesh_netif_up_\(\);/ {u=1} /mesh_netif_up_\(\);/ {n++}
            END {exit (u && n == 1) ? 0 : 1}'; then
  why="$why the probe task does not poll bat0 exactly while batman runs;"
fi
if ! awk '/^void warthog_mesh_smoke_test\(/,/^}/' "$MC" | \
       awk '/st = mmwlan_mesh_enable\(/ {e=NR} e && !s && /if \(st == MMWLAN_SUCCESS\) \{/ {s=NR}
            s && NR == s + 1 && /^ *warthog_bat_port_mesh_up\(\);/ {u=NR} u && NR == u + 1 && /} else \{/ {x=NR}
            x && NR == x + 1 && /^ *warthog_bat_port_mesh_failed\(\);/ {f=1} END {exit f ? 0 : 1}'; then
  why="$why mmwlan_mesh_enable's result does not reach warthog_bat_port_mesh_up / _mesh_failed;"
fi
# Statically too: the lwIP calls sit only in functions esp_netif_tcpip_exec runs.
why="$why$(lwip_only_in_tcpip "$MC" mesh_bat_io_ \
  '(dhcp_start|dhcp_release_and_stop|dhcp_supplied_address|netif_dhcp_data|netif_set_addr|etharp_[a-z_]+|netif_ip4_[a-z]+)[(]')"
if [ -z "$why" ]; then
  ok "bat0: polled while batman runs, DHCP for 45 s from the first route (an offer ARP-checked at the deadline kept), ARP-probed static via 10.41.0.1, retried beside it, router loss restarts DHCP backing off, renewals stay leased, lwIP only in tcpip context"
else
  bad "mesh.c batman bring-up: $why"
fi

# 36. The link throughput batman is given: umac_rc_get_expected_tput_kbps (umac_rc.c) and
#     mmrc's rate table (mmrc.c) compiled out of the SDK and run, so kbit/s stays kbit/s
#     (300 at 1 MHz MCS0, 3 to the engine) and a rate outside the table reads 0, never
#     past it. Nothing else links umac_rc.c on the host.
FW=../../halow/components/mm-iot-sdk/framework
[ -n "${SIMNODE_INCS:-}" ] || SIMNODE_INCS=$(env -u MAKEFLAGS -u MAKELEVEL make -s --no-print-directory simnode-incs)
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
awk '/^u32 mmrc_calculate_theoretical_throughput\(/,/^}/' "$FW/morselib/mmrc/src/core/mmrc.c" > "$T/fn.c"
awk '/^uint32_t umac_rc_get_expected_tput_kbps\(/,/^}/' "$FW/morselib/src/umac/rc/umac_rc.c" >> "$T/fn.c"
cat > "$T/t.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include "umac_rc_data.h"
#include "bat_mode.h"
struct umac_sta_data { struct umac_rc_sta_data rc; };
static struct mmrc_rate best;
struct umac_rc_sta_data *umac_sta_data_get_rc(struct umac_sta_data *stad) { return &stad->rc; }
struct mmrc_rate mmrc_sta_get_best_rate(struct mmrc_table *tb) { (void)tb; return best; }
#include "fn.c"
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf(__VA_ARGS__); printf(" | "); } } while (0)
static uint32_t at(unsigned bw, unsigned mcs, unsigned sgi, struct umac_sta_data *s)
{
    memset(&best, 0, sizeof(best));
    best.bw = bw & 7; best.rate = mcs & 15; best.guard = sgi & 1;
    return umac_rc_get_expected_tput_kbps(s);
}
int main(void)
{
    static struct mmrc_table tb;
    struct umac_sta_data s = { { &tb } };
    uint32_t v;
    CHECK((v = at(MMRC_BW_1MHZ, MMRC_MCS0, 0, &s)) == 300, "1 MHz MCS0 LGI: %u kbit/s, want 300", (unsigned)v);
    CHECK((v = at(MMRC_BW_8MHZ, MMRC_MCS9, 1, &s)) == 43333, "8 MHz MCS9 SGI: %u, want 43333", (unsigned)v);
    CHECK((v = at(MMRC_BW_1MHZ, MMRC_MCS10, 0, &s)) == 150, "1 MHz MCS10 LGI: %u, want 150", (unsigned)v);
    CHECK((v = at(MMRC_BW_2MHZ, MMRC_MCS_UNUSED, 0, &s)) == 0, "no MCS yet: %u, want 0", (unsigned)v);
    CHECK((v = at(MMRC_BW_8MHZ, MMRC_MCS_UNUSED, 1, &s)) == 0, "no MCS yet at 8 MHz SGI: %u, want 0", (unsigned)v);
    CHECK((v = at(MMRC_BW_16MHZ, MMRC_MCS0, 0, &s)) == 0, "16 MHz (no table row): %u, want 0", (unsigned)v);
    s.rc.reference_table = NULL;
    CHECK(at(MMRC_BW_1MHZ, MMRC_MCS0, 0, &s) == 0 && umac_rc_get_expected_tput_kbps(NULL) == 0, "no table / no station: 0");
    s.rc.reference_table = &tb;
    CHECK(bat_mode_kbps_to_units(true, true, true, at(MMRC_BW_1MHZ, MMRC_MCS0, 0, &s)) == 3,
          "1 MHz MCS0 reaches the engine as 3 (x 100 kbit/s)");
    return fails != 0;
}
EOF
why=""
if [ "$(grep -c . "$T/fn.c")" -lt 20 ]; then
  why="the helper or the rate table was not found"
elif ! ${CC:-cc} -std=gnu11 -w ${SANFLAGS:-} $SIMNODE_INCS -I"$FW/morselib/src/umac/rc" -I"$T" \
       -I../../../main -I../../../main/bat -o "$T/t" "$T/t.c" ../../../main/bat_mode.c 2>"$T/cc.log"; then
  why="did not build: $(head -3 "$T/cc.log" | tr '\n' ' ')"
elif ! why=$("$T/t" 2>&1); then
  why="${why:-crashed}"
else
  why=""
fi
rm -rf "$T"
if [ -z "$why" ]; then
  ok "rate control's expected throughput is kbit/s from mmrc's table, 0 outside it, 100 kbit/s units to the engine"
else
  bad "umac_rc_get_expected_tput_kbps: $why"
fi

# 37. Mesh peer records are deleted on the event loop, while the TX entry (netif, batman
#     engine) and the RX filter (chip driver task) look them up on their own tasks, between
#     umac_datapath_mesh_read_begin and _read_end. A record is freed only by the reclaim
#     (mesh_free_retired_), and nothing under the spinlock. test_simnode_datapath and _batman drive
#     the interleavings; this keeps an edit from dropping the bracket or adding a free path.
DP=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/datapath
why=""
frees=$(awk '/^[a-zA-Z_].*\(/ && !/;$/ { fn=$0 } /mmosal_free\(/ { print fn }' "$DP/umac_datapath_mesh.c" | sort -u)
case "$frees" in
  "static void mesh_free_retired_("*) [ "$(printf '%s\n' "$frees" | grep -c .)" = 1 ] || \
    why="$why umac_datapath_mesh.c frees outside mesh_free_retired_ ($frees);" ;;
  *) why="$why umac_datapath_mesh.c frees outside mesh_free_retired_ ($frees);" ;;
esac
in_cs=$(awk '/MMOSAL_TASK_ENTER_CRITICAL\(\)/ { c++ } /MMOSAL_TASK_EXIT_CRITICAL\(\)/ { c-- }
  c > 0 && /mmpkt_release\(|mmosal_free\(|mmpkt_list_clear\(|mesh_free_retired_\(/ { printf "%d ", NR }' \
  "$DP/umac_datapath_mesh.c")
[ -z "$in_cs" ] || why="$why umac_datapath_mesh.c frees under the spinlock (line $in_cs);"
for f in umac_datapath_tx_frame_resolve umac_datapath_rx_frame_filter; do
  n=$(awk -v f="$f(" '/umac_datapath_mesh_read_begin\(\)/ { r=1 } /umac_datapath_mesh_read_end\(/ { r=0 }
    index($0, f) && !/^static / { print (r ? "in" : "out") }' "$DP/umac_datapath.c" | tr '\n' ' ')
  [ "$n" = "in " ] || why="$why $f is called outside read_begin/read_end or more than once ($n);"
done
if [ -z "$why" ]; then
  ok "peer records: TX entry and RX filter read them between read_begin/end, freed only by the reclaim, never under the spinlock"
else
  bad "mesh peer record lifetime: $why"
fi

# 38. NAPT (main/nat.c, which batman mode also uses for the tether) is enabled on the tcpip
#     thread: the first ip_napt_enable_netif arms lwIP's NAPT timer with sys_timeout, which
#     inserts into tcpip's unlocked timer list (no core locking in any sdkconfig). enforce_state
#     is compiled out of nat.c and ticked with the HaLow side's address appearing on the third
#     tick: NAPT goes on for both inside netifs then, or for USB alone without an AP, with every
#     NAPT call inside esp_netif_tcpip_exec; and statically no function outside one it runs
#     calls the NAPT API.
NC=../../../main/nat.c
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
awk '/^static bool s_napt_logged/ {p=1} /^static void nat_task/ {exit} p' "$NC" > "$T/fn.c"
cat > "$T/t.c" <<'EOF'
/* enforce_state, compiled out of main/nat.c, against a model of esp_netif and lwIP's NAPT API.
 * An ip_napt_enable_netif outside esp_netif_tcpip_exec is counted. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
typedef struct esp_netif_obj esp_netif_t;
struct netif { uint8_t napt; };
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(p) (unsigned)((p)->addr & 0xff), (unsigned)((p)->addr >> 8 & 0xff), (unsigned)((p)->addr >> 16 & 0xff), (unsigned)((p)->addr >> 24)
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
static const char *TAG = "t";
static int in_tcpip, outside, timer_arms, napt_table;
struct esp_netif_obj { const char *key; int present; struct netif lw; esp_netif_ip_info_t ip; };
static struct esp_netif_obj STA = { "WIFI_STA_DEF", 1 }, USB = { "USB", 1 }, AP = { "WIFI_AP_DEF", 1 };
static bool warthog_mesh_bridge_active(void) { return false; }
static esp_netif_t *esp_netif_get_handle_from_ifkey(const char *k)
{
    esp_netif_t *n = !strcmp(k, STA.key) ? &STA : !strcmp(k, USB.key) ? &USB : !strcmp(k, AP.key) ? &AP : NULL;
    return n && n->present ? n : NULL;
}
static esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *i) { *i = n->ip; return ESP_OK; }
static esp_err_t esp_netif_set_default_netif(esp_netif_t *n) { (void)n; return ESP_OK; } /* IDF: IPC into tcpip */
static void *esp_netif_get_netif_impl(esp_netif_t *n) { return &n->lw; }
static esp_err_t esp_netif_tcpip_exec(esp_err_t (*fn)(void *), void *ctx)
{
    in_tcpip++;
    esp_err_t e = fn(ctx);
    in_tcpip--;
    return e;
}
/* The first enable of any netif is ip_napt_init: mem_calloc, then sys_timeout(ip_napt_tmr). */
static int ip_napt_enable_netif(struct netif *n, int en)
{
    outside += !in_tcpip;
    timer_arms += en && !napt_table;
    napt_table |= en;
    n->napt = en != 0;
    return 1;
}
#include "fn.c"
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf(__VA_ARGS__); printf(" | "); } } while (0)
int main(void)
{
    for (int tick = 0; tick < 5; tick++) {
        if (tick == 2) STA.ip.ip.addr = 0x6600290au; /* 10.41.0.102 on bat0, or the HaLow STA's lease */
        enforce_state();
        if (tick == 1) CHECK(!USB.lw.napt && !AP.lw.napt, "NAPT on before the HaLow side had an address");
    }
    CHECK(USB.lw.napt && AP.lw.napt && timer_arms == 1, "NAPT not on for both inside netifs (usb %d ap %d, "
          "timer armed %d times)", USB.lw.napt, AP.lw.napt, timer_arms);
    CHECK(outside == 0, "%d NAPT calls outside tcpip context (the first arms lwIP's timer list from the nat task)",
          outside);
    memset(&USB.lw, 0, sizeof(USB.lw));
    memset(&AP.lw, 0, sizeof(AP.lw));
    AP.present = 0;
    s_napt_logged = false;
    enforce_state();
    CHECK(USB.lw.napt && !AP.lw.napt && outside == 0, "without the AP netif: want NAPT on USB alone, in tcpip "
          "context (usb %d, %d outside)", USB.lw.napt, outside);
    return fails != 0;
}
EOF
why=""
if ! grep -q '^static void enforce_state(' "$T/fn.c"; then
  why="enforce_state not found"
elif ! ${CC:-cc} -std=gnu11 -w ${SANFLAGS:-} -I"$T" -o "$T/t" "$T/t.c" 2>"$T/cc.log"; then
  why="did not build: $(head -3 "$T/cc.log" | tr '\n' ' ')"
elif ! why=$("$T/t" 2>&1); then
  why="${why:-crashed}"
else
  why=""
fi
rm -rf "$T"
why="$why$(lwip_only_in_tcpip "$NC" nat_napt_on_ 'ip_napt_[a-z_]+[(]')"
if [ -z "$why" ]; then
  ok "NAPT: enabled on both inside netifs once the HaLow side has an address, only in tcpip context"
else
  bad "nat.c NAPT enable: $why"
fi

# 39. Every key= of the AT+BATSTAT? port line (bat_port_stat_line in main/bat_port.c) is
#     named, in backticks, in the AT+BATSTAT? row of wiki/AT-Command-Reference.md, so a new
#     port counter cannot ship undocumented.
BP=../../../main/bat_port.c
ATREF=../../../wiki/AT-Command-Reference.md
keys=$(awk '/"\+BATSTAT: port / {p=1} p {print} p && /\\r\\n"/ {exit}' "$BP" |
  sed -n 's/^[^"]*"\(.*\)".*$/\1/p' | tr ' ' '\n' | sed -n 's/^\([a-z_][a-z_0-9]*\)=.*/\1/p')
row=$(grep '^| `AT+BATSTAT?`' "$ATREF")
why=""
[ "$(printf '%s\n' "$keys" | grep -c .)" -ge 20 ] || why=" the port line's keys were not found in $BP ($keys);"
[ -n "$row" ] || why="$why no AT+BATSTAT? row in $ATREF;"
for k in $keys; do
  case "$row" in *"\`$k\`"*) ;; *) why="$why $k;" ;; esac
done
if [ -z "$why" ]; then
  ok "every AT+BATSTAT? port key ($(printf '%s\n' "$keys" | grep -c .)) is documented in the AT reference"
else
  bad "AT+BATSTAT? port keys missing from the AT reference row:$why"
fi

# 40. The AT+MESHCFG? chip VIF lines (meshcfg_chipvif_line_ and meshcfg_chipcmd_line_ in
#     main/at.c), compiled out and run: every type the firmware sets is named (and an unknown
#     one shown as other), refusals by command and SET_STA_STATE by state, the MESH_CONFIG
#     sent by name; each line fits line[320] with every field at its extreme, and cmd_meshcfg
#     fills both from the storage morselib writes.
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
{ awk '/^struct meshcfg_chipvif \{/,/^};/' "$A"
  awk '/^static const char \*meshcfg_chipvif_name_\(/,/^}/' "$A"
  awk '/^static int meshcfg_chipvif_line_\(/,/^}/' "$A"
  awk '/^static const char \*meshcfg_sent_name_\(/,/^}/' "$A"
  awk '/^static int meshcfg_chipcmd_line_\(/,/^}/' "$A"; } > "$T/fn.c"
size40=$(awk '/^static void cmd_meshcfg\(void\)/ {on=1} on && /char line\[[0-9]+\];/ {match($0, /\[[0-9]+\]/); print substr($0, RSTART + 1, RLENGTH - 2); exit}' "$A")
cat > "$T/t.c" <<'EOF2'
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fn.c"
int main(int argc, char **argv)
{
    unsigned size = (unsigned)atoi(argv[1]);
    static const struct { uint32_t t; const char *want; } names[] = {
        { 0, "chip_vif=none(0) " }, { 1, "chip_vif=sta(1) " }, { 2, "chip_vif=ap(2) " },
        { 5, "chip_vif=mesh(5) " }, { 99, "chip_vif=other(99) " } };
    char buf[1024];
    struct meshcfg_chipvif c;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        memset(&c, 0, sizeof(c));
        c.type = names[i].t;
        meshcfg_chipvif_line_(buf, sizeof(buf), &c);
        if (strncmp(buf, "+MESHCFG: ", 10) != 0 || !strstr(buf, names[i].want) || !strstr(buf, "built=sta ")) {
            printf("content (type %lu) %s", (unsigned long)names[i].t, buf); return 0;
        }
    }
    memset(&c, 0, sizeof(c));
    c.type = 5; c.vif_id = 2; c.built_mesh = true; c.fallback = 3; c.add_status = -1;
    c.bssid_refused = 4; c.bssid_status = -13; c.meshcfg_refused = 6; c.meshcfg_status = -22;
    meshcfg_chipvif_line_(buf, sizeof(buf), &c);
    if (!strstr(buf, "vif_id=2 built=mesh fallback=3 add_st=-1 bssid_refused=4(st=-13) "
                     "mesh_config_refused=6(st=-22)\r\n")) { printf("content (fields) %s", buf); return 0; }
    memset(&c, 0, sizeof(c));
    c.beacon_refused = 1; c.beacon_status = -7; c.sta_status = -3; c.key_refused = 2;
    c.key_status = -95; c.keyidx_mismatch = 5; c.meshcfg_mode = 2;
    for (unsigned i = 0; i < 5; i++) c.sta_refused[i] = 10 + i;
    meshcfg_chipcmd_line_(buf, sizeof(buf), &c);
    if (strncmp(buf, "+MESHCFG: chip_refused ", 23) != 0 ||
        !strstr(buf, " beacon_config=1(st=-7) sta_state=10/11/12/13/14(st=-3) install_key=2(st=-95) "
                     "keyidx_mismatch=5 mesh_config_sent=beaconless(2)\r\n")) {
        printf("content (refusals) %s", buf); return 0;
    }
    static const struct { uint32_t m; const char *want; } modes[] = {
        { 0, "mesh_config_sent=none(0)" }, { 1, "mesh_config_sent=beaconing(1)" },
        { 2, "mesh_config_sent=beaconless(2)" }, { 7, "mesh_config_sent=other(7)" } };
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        c.meshcfg_mode = modes[i].m;
        meshcfg_chipcmd_line_(buf, sizeof(buf), &c);
        if (!strstr(buf, modes[i].want)) { printf("content (mode %lu) %s", (unsigned long)modes[i].m, buf); return 0; }
    }
    memset(&c, 0xff, sizeof(c));
    c.add_status = c.bssid_status = c.meshcfg_status = INT32_MIN;
    c.beacon_status = c.sta_status = c.key_status = INT32_MIN;
    int n = meshcfg_chipvif_line_(buf, sizeof(buf), &c);
    int n2 = meshcfg_chipcmd_line_(buf, sizeof(buf), &c);
    if (n2 > n) n = n2;
    (void)argc;
    printf("%s %d/%u\n", n + 1 <= (int)size ? "ok" : "short", n + 1, size);
    return 0;
}
EOF2
fit40=""
if [ -n "$size40" ] && ${CC:-cc} -std=gnu11 -w -I"$T" -o "$T/t" "$T/t.c" 2>/dev/null; then
  fit40=$("$T/t" "$size40")
fi
rm -rf "$T"
fill40=$(awk '/^static void cmd_meshcfg\(void\)/,/^}/' "$A" | tr -d ' \n')
for f in '.type=g_warthog_chipvif_type' '.vif_id=g_warthog_chipvif_id' '.fallback=g_warthog_chipvif_fallback' \
         '.add_status=g_warthog_chipvif_add_status' '.bssid_refused=g_warthog_chipcmd_bssid_refused' \
         '.bssid_status=g_warthog_chipcmd_bssid_status' '.meshcfg_refused=g_warthog_chipcmd_meshcfg_refused' \
         '.meshcfg_status=g_warthog_chipcmd_meshcfg_status' \
         '.beacon_refused=g_warthog_chipcmd_beacon_refused' '.beacon_status=g_warthog_chipcmd_beacon_status' \
         '.sta_status=g_warthog_chipcmd_sta_status' '.key_refused=g_warthog_chipcmd_key_refused' \
         '.key_status=g_warthog_chipcmd_key_status' '.keyidx_mismatch=g_warthog_chipcmd_keyidx_mismatch' \
         '.meshcfg_mode=g_warthog_chipcmd_meshcfg_mode' \
         'for(unsignedi=0;i<5;i++){c.sta_refused[i]=g_warthog_chipcmd_sta_refused[i];}' \
         '#ifdefined(WARTHOG_MESH_CHIP_VIF_MESH)&&WARTHOG_MESH_CHIP_VIF_MESH.built_mesh=true,#endif' \
         'meshcfg_chipvif_line_(line,sizeof(line),&c);cdc_write(line);' \
         'meshcfg_chipcmd_line_(line,sizeof(line),&c);cdc_write(line);'; do
  case "$fill40" in *"$f"*) ;; *) fit40="cmd_meshcfg does not set $f"; break ;; esac
done
case "$fit40" in
  ok*) ok "AT+MESHCFG?'s chip VIF lines name every type and refusal, are filled from morselib's storage and fit line[$size40] (${fit40#ok })" ;;
  *)   bad "AT+MESHCFG?'s chip VIF lines: ${fit40:-did not build or run}" ;;
esac

# 41. AT+BCNSTAT? fits its buffer at its longest (every %lu at 10 digits), chip_irq= and
#     host_yield= included.
fit=$(awk '/^static void cmd_bcnstat\(void\)/ {on=1}
  on && /char buf\[[0-9]+\]/ { match($0, /\[[0-9]+\]/); size = substr($0, RSTART + 1, RLENGTH - 2) + 0 }
  on && /snprintf\(buf, sizeof\(buf\),/ { args = 1 }
  args { l = $0; while (match(l, /"[^"]*"/)) { fmt = fmt substr(l, RSTART + 1, RLENGTH - 2); l = substr(l, RSTART + RLENGTH) } }
  on && /cdc_write\(buf\);/ { exit }
  END {
    if (!index(fmt, "chip_irq=%lu host_yield=%lu")) { print "no chip_irq=/host_yield="; exit }
    gsub(/\\[rn]/, "x", fmt); lu = gsub(/%lu/, "", fmt)
    if (size == 0 || lu == 0 || index(fmt, "%")) { print "unparsed"; exit }
    need = length(fmt) + 10 * lu + 1
    print (need <= size ? "ok " : "short ") need "/" size
  }' "$A")
case "$fit" in
  ok*) ok "AT+BCNSTAT? fits its buffer at its longest (${fit#ok } bytes)" ;;
  *)   bad "AT+BCNSTAT?: ${fit:-nothing parsed}" ;;
esac

# 42. main/mesh.c's RESULT line is warthog_mesh_start_result (main/mesh_diag.c, run by
#     test_mesh_diag) fed from what morselib stored: the chip VIF type, its fallback and the
#     MESH_CONFIG answer; a fixed "mesh VIF added" PASS line no longer survives beside it.
#     (driver.c's status reads, beacon.c and umac_ps.c run in test_chipvif_glue.)
MC=../../../main/mesh.c
fill42=$(awk '/^void warthog_mesh_smoke_test\(/,/^}/' "$MC" | sed 's:/\*.*\*/::' | tr -d ' \n')
why=""
for f in '.status=(int)st' '.chip_vif=g_warthog_chipvif_type' '.fallback=g_warthog_chipvif_fallback' \
         '.add_status=g_warthog_chipvif_add_status' '.meshcfg_refused=g_warthog_chipcmd_meshcfg_refused' \
         '.meshcfg_status=g_warthog_chipcmd_meshcfg_status' '.meshcfg_mode=g_warthog_chipcmd_meshcfg_mode' \
         '#ifdefined(WARTHOG_MESH_CHIP_VIF_MESH)&&WARTHOG_MESH_CHIP_VIF_MESH.built_mesh=1,#endif' \
         'warthog_mesh_start_result(&res,result,sizeof(result))'; do
  case "$fill42" in *"$f"*) ;; *) why="$why the start does not set $f;" ;; esac
done
case "$fill42" in *'charresult[WARTHOG_MESH_START_RESULT_LEN]'*) ;; *) why="$why its buffer is not WARTHOG_MESH_START_RESULT_LEN;" ;; esac
if grep -q 'mesh VIF added AND' "$MC"; then why="$why a fixed 'mesh VIF added' line is still printed;"; fi
if [ -z "$why" ]; then
  ok "mesh.c's RESULT line is warthog_mesh_start_result, from the chip VIF and MESH_CONFIG answers morselib stored"
else
  bad "mesh.c RESULT line:$why"
fi

# 43. AT+GTKSTAT? (gtkstat_line_ in main/at.c), compiled out and run: the mode named for each
#     build, chip VIF and AT+GTKPERSTA, every counter in its slot, a held peer key as
#     [<mac> aid id hw] and an empty slot as [-]; it fits line[] with every field at its extreme;
#     cmd_gtkstat fills it from the storage morselib writes and is dispatched.
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
{ awk '/^struct gtkstat \{/,/^};/' "$A"
  awk '/^static int gtkstat_line_\(/,/^}/' "$A"; } > "$T/fn.c"
size43=$(awk '/^static void cmd_gtkstat\(void\)/ {on=1} on && /char line\[[0-9]+\];/ {match($0, /\[[0-9]+\]/); print substr($0, RSTART + 1, RLENGTH - 2); exit}' "$A")
cat > "$T/t.c" <<'EOF2'
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fn.c"
int main(int argc, char **argv)
{
    unsigned size = (unsigned)atoi(argv[1]);
    char buf[1024];
    struct gtkstat s;
    static const struct { bool build; uint32_t vif, mode; const char *want; } modes[] = {
        { false, 5, 1, "per_sta=off(build) " }, { false, 5, 0, "per_sta=off(build) " },
        { true, 1, 1, "per_sta=off(sta_vif) " }, { true, 5, 1, "per_sta=on " },
        { true, 5, 0, "per_sta=off(at) " }, { true, 5, 2, "per_sta=on(pn0) " } };
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        memset(&s, 0, sizeof(s));
        s.build = modes[i].build; s.chip_vif = modes[i].vif; s.mode = modes[i].mode;
        gtkstat_line_(buf, sizeof(buf), &s);
        if (strncmp(buf, "+GTKSTAT: ", 10) != 0 || !strstr(buf, modes[i].want)) { printf("mode %u: %s", i, buf); return 0; }
    }
    memset(&s, 0, sizeof(s));
    s.build = true; s.chip_vif = 5; s.mode = 1; s.inst = 1; s.fail = 2; s.del = 3; s.delfail = 4;
    s.tainted = 5; s.rx_grp = 6; s.forged = 7; s.mgmt_gp = 8; s.fence = 9; s.taint = 10; s.mic_ok = 11;
    s.mic_bad = 12; s.mic_arm = 3; s.micdrop = 13; s.gp_micdrop = 14;
    s.slot[1] = 0x80020107u; s.mac[1] = 0x00bfcdu; s.slot[2] = 0x00030101u;
    gtkstat_line_(buf, sizeof(buf), &s);
    if (strcmp(buf, "+GTKSTAT: per_sta=on inst=1 fail=2 del=3 delfail=4 tainted=5 rx_grp=6 forged=7 "
                    "mgmt_gp=8 fence=9 taint=10 mic_ok=11 mic_bad=12 mic_arm=3 micdrop=13/14 "
                    "[-] [00bfcd aid=2 id=1 hw=7] [-] [-]\r\n") != 0) { printf("fields: %s", buf); return 0; }
    memset(&s, 0xff, sizeof(s));
    s.build = true; s.chip_vif = 1;
    int n = gtkstat_line_(buf, sizeof(buf), &s);
    (void)argc;
    printf("%s %d/%u\n", n + 1 <= (int)size ? "ok" : "short", n + 1, size);
    return 0;
}
EOF2
fit43=""
if [ -n "$size43" ] && ${CC:-cc} -std=gnu11 -w -I"$T" -o "$T/t" "$T/t.c" 2>/dev/null; then
  fit43=$("$T/t" "$size43")
fi
rm -rf "$T"
fill43=$(awk '/^static void cmd_gtkstat\(void\)/,/^}/' "$A" | tr -d ' \n')
for f in '#if!defined(WARTHOG_MESH_AMPE_NO_CHIP_KEY)&&defined(WARTHOG_MESH_CHIP_VIF_MESH)&&WARTHOG_MESH_CHIP_VIF_MESH.build=true,#endif' \
         '.chip_vif=g_warthog_chipvif_type' '.mode=g_warthog_peer_gtk_mode' '.inst=g_warthog_peer_gtk_inst' \
         '.fail=g_warthog_peer_gtk_fail' '.del=g_warthog_peer_gtk_del' '.delfail=g_warthog_peer_gtk_delfail' \
         '.tainted=g_warthog_peer_gtk_tainted' '.rx_grp=g_warthog_rx_grp_chip' \
         '.forged=g_warthog_rx_grp_forged' '.mgmt_gp=g_warthog_mgmt_gp_chip' \
         '.fence=g_warthog_peer_gtk_fence' '.taint=g_warthog_peer_gtk_taint' \
         '.mic_ok=g_warthog_rx_grp_mic_ok' '.mic_bad=g_warthog_rx_grp_mic_bad' \
         '.mic_arm=g_warthog_rx_grp_mic_armed' '.micdrop=g_warthog_rx_grp_micdrop' \
         '.gp_micdrop=g_warthog_mgmt_gp_micdrop' \
         's.slot[i]=g_warthog_peer_gtk[i];s.mac[i]=g_warthog_peer_gtk_mac[i];' \
         'gtkstat_line_(line,sizeof(line),&s);cdc_write(line);'; do
  case "$fill43" in *"$f"*) ;; *) fit43="cmd_gtkstat does not set $f"; break ;; esac
done
grep -q 'strcasecmp(verb, "GTKSTAT") == 0 && terminator == .?.) {' "$A" && \
  awk '/strcasecmp\(verb, "GTKSTAT"\)/ {getline; print}' "$A" | grep -q 'cmd_gtkstat();' || fit43="AT+GTKSTAT? is not dispatched to cmd_gtkstat"
case "$fit43" in
  ok*) ok "AT+GTKSTAT? names its mode, prints every counter and slot, is filled from morselib's storage and fits line[$size43] (${fit43#ok })" ;;
  *)   bad "AT+GTKSTAT?: ${fit43:-did not build or run}" ;;
esac

# 44. AT+GTKPERSTA=<0|1|2>: the parser is compiled out of main/at.c and run; the command
#     persists the mode and applies it live, boot restores it before the first peer's MGTK
#     arrives, morselib reads it (the gate and the service tick, which the mesh tick runs) and
#     every chip boot resets what the mesh holds in the chip.
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
awk '/^static bool gtkpersta_parse_\(/,/^}/' "$A" > "$T/fn.c"
cat > "$T/t.c" <<'EOF2'
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "fn.c"
static int check(const char *in, bool want_ok, uint32_t want)
{
    uint32_t v = 77;
    bool got = gtkpersta_parse_(in, &v);
    if (got != want_ok || (got && v != want)) { printf("'%s' parsed as %d/%lu", in, (int)got, (unsigned long)v); return 1; }
    return 0;
}
int main(void)
{
    return check("0", true, 0) || check("1", true, 1) || check("2", true, 2) || check("3", false, 0) ||
           check("", false, 0) || check("01", false, 0) || check("1 ", false, 0) || check("-1", false, 0) ||
           check("on", false, 0) || gtkpersta_parse_(NULL, NULL);
}
EOF2
why=""
if ${CC:-cc} -std=gnu11 -w -I"$T" -o "$T/t" "$T/t.c" 2>/dev/null && why=$("$T/t"); then
  ok "AT+GTKPERSTA= accepts 0, 1 or 2 as the whole argument and rejects the rest"
else
  bad "gtkpersta_parse_ against its cases: ${why:-did not build or run}"
fi
rm -rf "$T"
M=../../halow/components/mm-iot-sdk/framework/morselib/src/umac
if awk '/"GTKPERSTA"\) == 0 && terminator == .=./ {on=1} on && /warthog_cfg_set_mesh_gtk\(/ {p=1}
        on && p && /g_warthog_peer_gtk_mode = v;/ {l=1} on && /reply_ok\(\);/ {exit}
        END {exit (p && l) ? 0 : 1}' "$A" && \
   grep -q 'strcasecmp(verb, "GTKPERSTA") == 0 && terminator == .?.' "$A" && \
   grep -q 'g_warthog_peer_gtk_mode = warthog_cfg_get_mesh_gtk();' ../../../main/mesh.c && \
   grep -q '"mesh_gtk"' ../../../main/cfg.c && \
   grep -q 'return stad != NULL && g_warthog_peer_gtk_mode != 0u && umac_mesh_sae_active() &&' "$M/datapath/umac_datapath_mesh.c" && \
   grep -q '^    umac_datapath_mesh_service_peer_gtk();' "$M/mesh/umac_mesh.c" && \
   grep -q '^            umac_datapath_mesh_chip_booted();' "$M/interface/umac_interface.c" && \
   grep -q '^            umac_datapath_mesh_chip_booted();' "$M/umac_mmdrv_shim.c" && \
   grep -q 'read_seq = ++g_warthog_rx_read_seq;' "$M/../driver/morse_driver/mm6108/pageset.c" && \
   grep -q 'read_seq = ++g_warthog_rx_read_seq;' "$M/../driver/morse_driver/mm8108/yaps.c"; then
  ok "AT+GTKPERSTA= persists the mode and applies it live; boot restores it; the gate and the tick read it; a chip boot resets the mesh's chip keys; both drivers stamp the read order"
else
  bad "AT+GTKPERSTA= is not persisted, applied live, restored at boot or read by morselib, or a chip boot or driver read is not wired"
fi

# 45. AT+DEFRAG? (defragstat_line_ in main/at.c), compiled out and run: every counter in its
#     slot and the line within line[] with every field at its extreme; cmd_defragstat fills it
#     from the storage morselib writes, every counter is written there, and it is dispatched.
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
{ awk '/^struct defragstat \{/,/^};/' "$A"
  awk '/^static int defragstat_line_\(/,/^}/' "$A"; } > "$T/fn.c"
size45=$(awk '/^static void cmd_defragstat\(void\)/ {on=1} on && /char line\[[0-9]+\];/ {match($0, /\[[0-9]+\]/); print substr($0, RSTART + 1, RLENGTH - 2); exit}' "$A")
cat > "$T/t.c" <<'EOF2'
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fn.c"
int main(int argc, char **argv)
{
    unsigned size = (unsigned)atoi(argv[1]);
    char buf[1024];
    struct defragstat s = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18 };
    defragstat_line_(buf, sizeof(buf), &s);
    if (strcmp(buf, "+DEFRAG: in=1 ok=2 | drop nofirst=3 order=4 pn=5 key=6 prot=7 hdr=8 amsdu=9 "
                    "oversize=10 nomem=11 mcast=12 plain=13 shape=14 | chain expired=15 restart=16 "
                    "flush=17 evict=18\r\n") != 0) {
        printf("fields: %s", buf);
        return 0;
    }
    memset(&s, 0xff, sizeof(s));
    int n = defragstat_line_(buf, sizeof(buf), &s);
    (void)argc;
    printf("%s %d/%u\n", n + 1 <= (int)size ? "ok" : "short", n + 1, size);
    return 0;
}
EOF2
fit45=""
if [ -n "$size45" ] && ${CC:-cc} -std=gnu11 -w -I"$T" -o "$T/t" "$T/t.c" 2>/dev/null; then
  fit45=$("$T/t" "$size45")
fi
rm -rf "$T"
fill45=$(awk '/^static void cmd_defragstat\(void\)/,/^}/' "$A" | tr -d ' \n')
DD=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/datapath
for c in in ok nofirst order pn key prot hdr amsdu oversize nomem mcast plain shape expired restart flush evict; do
  case "$fill45" in *".$c=g_warthog_defrag_$c,"*) ;; *) fit45="cmd_defragstat does not set .$c from g_warthog_defrag_$c"; break ;; esac
  grep -q "^volatile uint32_t .*g_warthog_defrag_$c = 0" "$A" || { fit45="at.c has no storage for g_warthog_defrag_$c"; break; }
  cat "$DD/datapath_defrag.c" "$DD/umac_datapath.c" | grep -Eq "g_warthog_defrag_$c\+\+|&g_warthog_defrag_$c;" || \
    { fit45="morselib never writes g_warthog_defrag_$c"; break; }
done
case "$fill45" in *'defragstat_line_(line,sizeof(line),&s);cdc_write(line);'*) ;; *) fit45="cmd_defragstat does not print its line" ;; esac
awk '/strcasecmp\(verb, "DEFRAG"\) == 0 && terminator == .\?./ {getline; print}' "$A" | grep -q 'cmd_defragstat();' || \
  fit45="AT+DEFRAG? is not dispatched to cmd_defragstat"
case "$fit45" in
  ok*) ok "AT+DEFRAG? prints every counter in its slot, is filled from morselib's storage and fits line[$size45] (${fit45#ok })" ;;
  *)   bad "AT+DEFRAG?: ${fit45:-did not build or run}" ;;
esac

# 46. Reassembly comes before anything reads a data frame's body past the CCMP header: in
#     process_rx_data_frame_after_reorder, datapath_defrag runs after the replay check and the
#     Block Ack update and before Mesh Control is parsed, learned from or relayed on; and a
#     plaintext fragment on a keyed link is refused ahead of the EAPOL exception.
order46=$(awk '/^static void umac_datapath_process_rx_data_frame_after_reorder\(/ {on=1} on && /^}/ {exit}
  on && /ccmp_is_valid\(/ && !v {v=NR} on && /umac_ba_set_expected_rx_seq_num\(/ && !b {b=NR}
  on && /datapath_defrag\(umacd,/ && !d {d=NR} on && /^    if \(mesh_ctrl_present\)/ && !m {m=NR}
  on && /umac_mesh_ctrl_parse\(|umac_mesh_fwd_glue_rx\(|umac_mesh_fwd_glue_leaf_learn\(/ && !p {p=NR}
  on && /datapath_defrag_is_fragment\(header\)\)/ && /^        / && !f {f=NR}
  on && /umac_datapath_is_eapol_frame\(rxbufview\)\)/ && !e {e=NR}
  END { if (v && b && d && m && p && f && e && v < d && b < d && d < m && d < p && f < e) print "ok";
        else printf "replay %d, ba %d, defrag %d, mesh ctrl %d, first parse %d, plain fragment %d, eapol %d\n", v, b, d, m, p, f, e }' \
  "$DD/umac_datapath.c")
if [ "$order46" = ok ] && [ "$(grep -c 'datapath_defrag(umacd,' "$DD/umac_datapath.c")" = 1 ]; then
  ok "reassembly runs once, after the replay check and Block Ack, before Mesh Control is read; plaintext fragments are refused before the EAPOL exception"
else
  bad "receive-path order: ${order46:-not found} (datapath_defrag call sites: $(grep -c 'datapath_defrag(umacd,' "$DD/umac_datapath.c"))"
fi

# 47. Reassembly's resources are bounded and outside what chip RX and hostap need: chain buffers
#     come from the heap, never the chip RX pool; one node-wide table of DEFRAG_CHAINS_MAX (4)
#     chains, DEFRAG_CHAINS_PER_PEER (2) a peer, none in the peer record; one core timeout for
#     them all, not one a chain; chains past their time are swept on every data frame and on the
#     mesh service tick. And the receive path's frame-shape rules: a fragment is group-addressed
#     by addr1, as mac80211 tests it; QoS bit 8 is compared on the mesh only; mesh data with a
#     group RA must be FromDS only and with a unicast RA 4-address (rxdrop 89), before decryption.
DF=$DD/datapath_defrag.c
DH=$DD/umac_datapath_data.h
MS=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/mesh/umac_mesh.c
why47=""
grep -q 'mmpkt_alloc_on_heap(FRAG_CHAIN_HDR_SPACE, FRAG_CHAIN_BODY_SPACE, 0)' "$DF" || why47="chain buffers are not heap-allocated"
grep -Eq 'mmdrv_alloc_mmpkt_for_defrag|mmhal_wlan_alloc_mmpkt_for_rx' "$DF" && why47="datapath_defrag.c takes chip RX pool blocks"
grep -Eq '^#define DEFRAG_CHAINS_MAX +\(4\)' "$DF" && grep -Eq '^#define DEFRAG_CHAINS_PER_PEER +\(2\)' "$DF" && \
  grep -q '^static struct datapath_defrag_chain s_chains\[DEFRAG_CHAINS_MAX\];' "$DF" || why47="${why47:-the chain table or its limits changed}"
awk '/^struct datapath_defrag_data$/,/^};/' "$DH" | grep -Eq 'mmpkt|chain' && why47="${why47:-a peer record holds chain storage again}"
[ "$(grep -c 'umac_core_register_timeout(' "$DF")" = 1 ] && \
  grep -A2 'umac_core_register_timeout(' "$DF" | tr -d ' \n' | grep -q 'datapath_defrag_timeout,umacd,NULL)' || \
  why47="${why47:-reassembly registers other than one node-wide timeout}"
awk '/^static void umac_datapath_process_rx_data_frame_after_reorder\(/ {on=1} on && /^}/ {exit} on' "$DD/umac_datapath.c" > "${TMPDIR:-/tmp}/glueguard47.$$"
AR="${TMPDIR:-/tmp}/glueguard47.$$"
grep -q '^    datapath_defrag_expire(umacd);' "$AR" || why47="${why47:-data frames do not sweep the chains}"
awk '/^static void mesh_service_evt_\(/,/^}/' "$MS" | grep -q 'umac_datapath_defrag_expire(umacd);' || why47="${why47:-the mesh service tick does not sweep the chains}"
grep -q 'datapath_defrag_is_fragment(header) && mm_mac_addr_is_multicast(dot11_get_ra(header))' "$AR" || why47="${why47:-the group-fragment drop does not test addr1}"
grep -q 'frag_mpdu = { .mesh = data->ops == &datapath_ops_mesh };' "$AR" || why47="${why47:-reassembly is not told the mesh from a BSS}"
shape47=$(awk '/g_warthog_rxdrop_reason = 89;/ && !s {s=NR} /umac_mesh_rx_host_ccmp\(stad, header, rxbufview\)/ && !c {c=NR}
  END { print (s && c && s < c) ? "ok" : "no" }' "$AR")
[ "$shape47" = ok ] || why47="${why47:-the mesh frame-shape drop (89) is missing or after decryption}"
rm -f "$AR"
if [ -z "$why47" ]; then
  ok "reassembly: heap buffers outside the chip RX pool, 4 chains a node and 2 a peer, one timeout, swept by data frames and the mesh tick; group fragments by addr1, QoS bit 8 on the mesh only, mesh frame shapes checked before decryption"
else
  bad "reassembly bounds / receive frame shapes: $why47"
fi

[ $fail -eq 0 ] && echo "GLUE INVARIANTS OK" || echo "GLUE INVARIANTS FAILED"
exit $fail
