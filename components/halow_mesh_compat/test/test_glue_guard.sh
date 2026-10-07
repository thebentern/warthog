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
#     here: after SAE comes up, and after the beacon init that clears it, which runs on the
#     first start only: the chip start a restart repeats (mesh_chip_start_, NULL) skips it.
C=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/supplicant_shim/supplicant_core_mesh.c
if awk '/^static int passive_init_ifmsh/,/^}/' "$C" | \
     awk '/mesh_rsn_auth_init\(wpa_s, mconf\)/ {a=NR} /g_warthog_sae_init = 1;/ {if (a) b=NR}
          /umac_mesh_beacon_set_rsn\(mconf->rsn_ie, \(uint16_t\)mconf->rsn_ie_len\);/ {if (b) c=NR}
          /MMLOG_INF\("mesh: open mesh/ {if (c) d=NR}
          END {exit (a && b && c && d) ? 0 : 1}' && \
   awk '/^enum mmwlan_status umac_mesh_enable_mesh\(/,/^}/' "$M" | \
     awk '/status = mesh_chip_start_\(umacd, vif_id, args, own_addr\);/ {c=NR}
          /mesh_enable_rest_\(umacd, vif_id, args, own_addr\)/ {if (c) r=NR} END {exit (c && r) ? 0 : 1}' && \
   awk '/^static enum mmwlan_status mesh_chip_start_\(/ {n++} n == 2 {print} n == 2 && /^}/ {exit}' "$M" | tr -d ' \n' | \
     grep -q 'if(own_addr!=NULL){enummmwlan_statusmac_status=mmwlan_get_mac_addr(own_addr);.*umac_mesh_beacon_init(args,own_addr);}' && \
   awk '/^static enum mmwlan_status mesh_enable_rest_\(/ {n++} n == 2 {print} n == 2 && /^}/ {exit}' "$M" | \
     grep -q 'umac_supp_add_mesh_interface(umacd)' && \
   awk '/^enum mmwlan_status umac_mesh_handle_hw_restarted\(/,/^}/' "$M" | \
     grep -q 'mesh_chip_start_(umacd, vif_id, &s_mesh_args, NULL)' && \
   [ "$(grep -c 'umac_mesh_beacon_init(' "$M")" -eq 1 ]; then
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
#     tx gate and RA, both slot pools under a stuck engine, lwIP's wait for a TX slot (a fragment burst),
#     at most 16 delivered copies held
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
static struct fq *f_wait_q; /* a finite wait on it runs f_wait_hook first, once (no zero-tick poll does) */
static void (*f_wait_hook)(void);
static TickType_t f_wait_t;
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
    if (q == f_wait_q && t && t != portMAX_DELAY && f_wait_hook) {
        void (*h)(void) = f_wait_hook;
        f_wait_hook = NULL;
        f_wait_t = t;
        h();
    }
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
static void open_gate_for_slot(void) /* the engine gets going while lwIP waits for a TX slot */
{
    gate(0);
    for (int i = 0; i < 20000 && uxQueueMessagesWaiting(f_q[1]) == 0; i++) { sleep_ms(1); }
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
          "engine busy: 6 RX + 4 TX queue, the 7th RX and 5th TX (after its wait) are refused, then all 10 get through");

    /* lwIP's burst (ip4_frag ignores a refusal): the 5th frame waits for a slot the engine frees meanwhile. */
    rx0 = ld(&f_rx_hard);
    tx0 = ld(&f_tx_soft);
    gate(1);
    rx(TA, 0x4305, 0x04, 60);
    busy = wait_ge(&f_in_rx, 1);
    okc = 0;
    for (int i = 0; i < 4; i++) { okc += soft(100) == ESP_OK; }
    const unsigned idle_tx = uxQueueMessagesWaiting(f_q[1]);
    f_wait_q = f_q[1];
    f_wait_hook = open_gate_for_slot;
    esp_err_t waited = soft(100);
    f_wait_q = NULL;
    gate(0);
    drained = wait_ge(&f_rx_hard, rx0 + 1) && wait_ge(&f_tx_soft, tx0 + 5);
    CHECK(busy && okc == 4 && idle_tx == 0 && waited == ESP_OK && f_wait_t > 50 && f_wait_t < 1000 && drained,
          "a 5th soft frame while the engine is busy waits (%u ms, past bat_port_tx's 50) and is taken when a slot "
          "frees; all 5 reach bat_tx_soft", (unsigned)f_wait_t);

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
   grep -q '^            umac_datapath_mesh_chip_booted(false);' "$M/interface/umac_interface.c" && \
   grep -q '^            umac_datapath_mesh_chip_booted(true);' "$M/umac_mmdrv_shim.c" && \
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

# 48. AT+HOSTFRAG? (hostfragstat_line_ in main/at.c), compiled out and run: the mode as off, auto or
#     the threshold and the build's rule (max2, off, max16), every counter in its slot (agg and the
#     Block Ack ones, ba_end nodelba ba_wait ba_late hold held hold_ms, too; hold_ms reads the hold-off
#     before the mesh runs), the last rate as <MHz>M/MCS<n> or none, the fragment cap's and
#     AT+SEALFIT's counters (seal_*, grp_*) and AT+TIDPARAMS' state and counters (ba_rcpt,
#     delba_noack) on the fourth line, and each of its four lines within line[] with every field at
#     its extreme; cmd_hostfragstat fills it from the storage morselib writes, mode as the build
#     applies it (off where the rule is), every counter is written there, and it is dispatched.
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
{ awk '/^struct hostfragstat \{/,/^};/' "$A"
  awk '/^static const char \*hostfrag_mode_\(/,/^}/' "$A"
  awk '/^static int hostfragstat_line_\(/,/^}/' "$A"; } > "$T/fn.c"
size48=$(awk '/^static void cmd_hostfragstat\(void\)/ {on=1} on && /char line\[[0-9]+\];/ {match($0, /\[[0-9]+\]/); print substr($0, RSTART + 1, RLENGTH - 2); exit}' "$A")
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
    struct hostfragstat s = { 1, 512, 300, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15, 16, 40, 17, 18,
                              19, 20, 21, 22, 23, 24, 31, 35, 36, 37, 32, 33, 34, 25, 26, 27, 0x108,
                              2, 46, 47, 48, 1, 49, 50, 51, 1, 44, 45, 52, 53, 54, 55 };
    static const char *want[4] = {
        "+HOSTFRAG: mode=auto stored=512 rule=max2 atfrag=300 | msdu=4 frag=5 by thresh=6 chip=7 rate=8 | "
        "whole many=10 pool=11 | drop seal=12 drv=13\r\n",
        "+HOSTFRAG: txst acked=14 noack=15 unsent=16 agg=40 | msdu ok=17 fail=18 | held wait=19 "
        "mgmt=20 stale=21 | overlap=22\r\n",
        "+HOSTFRAG: trim=23 chippn=24 | ba_end=31 nodelba=35 ba_wait=36 ba_late=37 hold=32 held=33 "
        "hold_ms=34 | last n=25 len=26 lim=27 at=1M/MCS8\r\n",
        "+HOSTFRAG: cap_trim=46 cap_sub=47 clamp=48 | sealfit=1 seal_trim=49 seal_sub=50 seal_nofit=51 "
        "seal_ba=55 grp_trim=52 grp_sub=53 grp_nofit=54 | tidparams=1 ba_rcpt=44 delba_noack=45\r\n",
    };
    for (int i = 0; i < 4; i++) {
        hostfragstat_line_(buf, sizeof(buf), &s, i);
        if (strcmp(buf, want[i]) != 0) { printf("line %d: %s", i, buf); return 0; }
    }
    s.mode = 0; s.last_rate = 0xffff; s.rule = 0;
    hostfragstat_line_(buf, sizeof(buf), &s, 0);
    if (strncmp(buf, "+HOSTFRAG: mode=off stored=512 rule=off ", 40) != 0) { printf("off: %s", buf); return 0; }
    s.rule = 16;
    hostfragstat_line_(buf, sizeof(buf), &s, 0);
    if (strstr(buf, " rule=max16 ") == NULL) { printf("rule 16: %s", buf); return 0; }
    hostfragstat_line_(buf, sizeof(buf), &s, 2);
    if (strstr(buf, " at=none\r\n") == NULL) { printf("no rate: %s", buf); return 0; }
    int worst = 0;
    for (int i = 0; i < 4; i++) {
        memset(&s, 0xff, sizeof(s));
        int n = hostfragstat_line_(buf, sizeof(buf), &s, i);
        worst = n > worst ? n : worst;
    }
    (void)argc;
    printf("%s %d/%u\n", worst + 1 <= (int)size ? "ok" : "short", worst + 1, size);
    return 0;
}
EOF2
fit48=""
if [ -n "$size48" ] && ${CC:-cc} -std=gnu11 -w -I"$T" -o "$T/t" "$T/t.c" 2>/dev/null; then
  fit48=$("$T/t" "$size48")
fi
rm -rf "$T"
fill48=$(awk '/^static void cmd_hostfragstat\(void\)/,/^}/' "$A" | tr -d ' \n')
DD=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/datapath
for c in hostfrag_msdu hostfrag_frags hostfrag_by_thresh hostfrag_by_chip hostfrag_by_rate hostfrag_many hostfrag_pool \
         hostfrag_seal hostfrag_drv hostfrag_acked hostfrag_noack hostfrag_unsent hostfrag_agg hostfrag_ok hostfrag_fail \
         hostfrag_wait hostfrag_mgmt hostfrag_stale hostfrag_overlap hostfrag_trim hostfrag_chippn hostfrag_ba_end \
         hostfrag_nodelba hostfrag_ba_wait hostfrag_ba_late hostfrag_hold hostfrag_held hostfrag_hold_ms hostfrag_last_n \
         hostfrag_last_len hostfrag_last_lim hostfrag_last_rate hostfrag_cap_trim hostfrag_cap_sub hostfrag_clamp \
         sealfit_trim sealfit_sub sealfit_nofit sealfit_ba grpfit_trim grpfit_sub grpfit_nofit hostfrag_ba_rcpt \
         hostfrag_delba_noack; do
  case "$fill48" in *"g_warthog_$c"*) ;; *) fit48="cmd_hostfragstat does not read g_warthog_$c"; break ;; esac
  grep -q "^volatile uint32_t .*g_warthog_$c = " "$A" || { fit48="at.c has no storage for g_warthog_$c"; break; }
  cat "$DD/umac_datapath.c" "$DD/umac_datapath_mesh.c" | grep -Eq "g_warthog_$c(\+\+| = | \+= )|&g_warthog_$c;" || \
    { fit48="morselib never writes g_warthog_$c"; break; }
done
case "$fill48" in *'.mode=hostfrag_rule_()!=0u?g_warthog_hostfrag:0u,'*'.stored=warthog_cfg_get_mesh_hostfrag(),.chip=s_frag_threshold'*) ;; *) fit48="cmd_hostfragstat does not fill mode (as applied), stored and chip" ;; esac
case "$fill48" in *'for(inti=0;i<4;i++){hostfragstat_line_(line,sizeof(line),&s,i);cdc_write(line);}'*) ;; *) fit48="cmd_hostfragstat does not print all four lines" ;; esac
case "$fill48" in *'.rule=hostfrag_rule_(),'*'.sealfit=g_warthog_sealfit,'*'.txparm=g_warthog_ba_txparm,'*) ;; *) fit48="cmd_hostfragstat does not fill the rule, sealfit and tidparams" ;; esac
awk '/strcasecmp\(verb, "HOSTFRAG"\) == 0 && terminator == .\?./ {getline; print}' "$A" | grep -q 'cmd_hostfragstat();' || \
  fit48="AT+HOSTFRAG? is not dispatched to cmd_hostfragstat"
# Every key= of every line is named, in backticks, in the AT+HOSTFRAG row of the AT reference.
keys48=$(awk '/^static int hostfragstat_line_\(/,/^}/' "$A" | grep '"' | sed -n 's/^[^"]*"\(.*\)".*$/\1/p' |
  tr ' ' '\n' | sed -n 's/^\([a-z_][a-z_0-9]*\)=.*/\1/p' | sort -u)
row48=$(grep '^| `AT+HOSTFRAG=' ../../../wiki/AT-Command-Reference.md)
[ "$(printf '%s\n' "$keys48" | grep -c .)" -ge 36 ] || fit48="the AT+HOSTFRAG? keys were not found ($keys48)"
hold48=$(sed -n 's/^#define UMAC_MESH_FRAG_BA_HOLD_MS \([0-9]*\)u$/\1/p' ../../halow/components/mm-iot-sdk/framework/morselib/src/umac/mesh/umac_mesh_frag.h)
grep -q "^volatile uint32_t g_warthog_hostfrag_held = 0, g_warthog_hostfrag_hold_ms = ${hold48:-x};" "$A" || \
  fit48="hold_ms does not read the hold-off (${hold48:-none}) before the mesh runs"
[ -n "$row48" ] || fit48="no AT+HOSTFRAG row in the AT reference"
for k in $keys48; do
  case "$row48" in *"\`$k\`"*) ;; *) fit48="the AT reference row does not name \`$k\`" ;; esac
done
case "$fit48" in
  ok*) ok "AT+HOSTFRAG? names its mode, prints every counter in its slot, is filled from morselib's storage and each line fits line[$size48] (${fit48#ok })" ;;
  *)   bad "AT+HOSTFRAG?: ${fit48:-did not build or run}" ;;
esac

# 49. AT+HOSTFRAG=<0|auto|n>: the parser is compiled out of main/at.c and run (n even, 256..2346, as
#     cfg80211 takes a threshold); the command persists the mode and applies it live, boot restores it
#     before the first data frame, NVS holds only a value the parser could give; cfg.c's getter, run
#     against a fake NVS with each build's flags, gives a value never stored as auto on SAE builds
#     with chip keys and off on the rest (host CCMP, no chip key, no SAE) and a stored one as
#     stored, and at.c boots with that default (the host tests' storage is generated from at.c and
#     cfg.h, and the simnode builds are SAE builds); the transmit path
#     reads it once a frame and enters fragmentation only when it is not off, for unicast data that
#     is not EAPOL, and asks rate control exactly once either way; the Block Ack check runs once a
#     frame, on that path after the cut decision (which may end the session) and before any cut.
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
awk '/^static bool hostfrag_parse_\(/,/^}/' "$A" > "$T/fn.c"
cat > "$T/t.c" <<'EOF2'
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "fn.c"
static int check(const char *in, bool want_ok, uint32_t want)
{
    uint32_t v = 77;
    bool got = hostfrag_parse_(in, &v);
    if (got != want_ok || (got && v != want)) { printf("'%s' parsed as %d/%lu", in, (int)got, (unsigned long)v); return 1; }
    return 0;
}
int main(void)
{
    return check("0", true, 0) || check("auto", true, 1) || check("AUTO", true, 1) ||
           check("256", true, 256) || check("257", true, 256) || check("512", true, 512) ||
           check("2346", true, 2346) || check("2347", false, 0) || check("255", false, 0) ||
           check("1", false, 0) || check("", false, 0) || check("512 ", false, 0) ||
           check(" 512", false, 0) || check("-512", false, 0) || check("+512", false, 0) ||
           check("autox", false, 0) || check("0x200", false, 0) ||
           check("99999999999999999999", false, 0) || hostfrag_parse_(NULL, NULL);
}
EOF2
why=""
if ${CC:-cc} -std=gnu11 -w -I"$T" -o "$T/t" "$T/t.c" 2>/dev/null && why=$("$T/t"); then
  ok "AT+HOSTFRAG= accepts 0, auto or an even 256..2346 (an odd n rounded down) as the whole argument and rejects the rest"
else
  bad "hostfrag_parse_ against its cases: ${why:-did not build or run}"
fi
rm -rf "$T"
CF=../../../main/cfg.c
why49=""
awk '/"HOSTFRAG"\) == 0 && terminator == .=./ {on=1} on && /warthog_cfg_set_mesh_hostfrag\(/ {p=1}
     on && p && /g_warthog_hostfrag = v;/ {l=1} on && /reply_ok\(\);/ {exit}
     END {exit (p && l) ? 0 : 1}' "$A" || why49="AT+HOSTFRAG= does not both persist and apply the mode"
grep -q 'g_warthog_hostfrag = warthog_cfg_get_mesh_hostfrag();' ../../../main/mesh.c || why49="${why49:-boot does not restore it}"
grep -q '"mesh_hfrag"' "$CF" && awk '/^static bool hostfrag_valid_\(/,/^}/' "$CF" | tr -d ' \n' | \
  grep -q 'returnv==0u||v==1u||(v>=256u&&v<=2346u&&(v&1u)==0u);' || why49="${why49:-NVS takes values the parser cannot give}"
grep -q '^volatile uint32_t g_warthog_hostfrag = WARTHOG_CFG_MESH_HOSTFRAG_DEFAULT;' "$A" || \
  why49="${why49:-at.c does not boot with the NVS default}"
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
awk '/^#if/ {b=""; on=1} on {b=b $0 "\n"} on && /^#endif/ {on=0; if (b ~ /WARTHOG_CFG_MESH_HOSTFRAG_DEFAULT/) printf "%s", b}' \
  ../../../main/cfg.h > "$T/def.h"
for f in hostfrag_valid_ warthog_cfg_get_mesh_hostfrag warthog_cfg_set_mesh_hostfrag; do
  awk -v f="$f" '$0 ~ ("^[a-z_0-9 ]+[ *]" f "\\(") && !/;$/ {p=1} p {print} p && /^}/ {exit}' "$CF"
done > "$T/cfg_fn.c"
cat > "$T/t.c" <<'EOF2'
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "def.h"
typedef int esp_err_t;
typedef int nvs_handle_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NVS_NOT_FOUND 0x1102
enum { NVS_READONLY, NVS_READWRITE };
static const char *NS = "warthog";
static int have;
static uint16_t val;
static esp_err_t nvs_open(const char *ns, int m, nvs_handle_t *h) { (void)ns; (void)m; *h = 1; return ESP_OK; }
static void nvs_close(nvs_handle_t h) { (void)h; }
static esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
static esp_err_t nvs_get_u16(nvs_handle_t h, const char *k, uint16_t *v)
{ (void)h; if (strcmp(k, "mesh_hfrag") != 0 || !have) return ESP_ERR_NVS_NOT_FOUND; *v = val; return ESP_OK; }
static esp_err_t nvs_set_u16(nvs_handle_t h, const char *k, uint16_t v)
{ (void)h; if (strcmp(k, "mesh_hfrag") != 0) return ESP_ERR_INVALID_ARG; have = 1; val = v; return ESP_OK; }
#include "cfg_fn.c"
int main(void)
{
    const uint32_t def = WANT;
    if (WARTHOG_CFG_MESH_HOSTFRAG_DEFAULT != def) { printf("default %u", (unsigned)WARTHOG_CFG_MESH_HOSTFRAG_DEFAULT); return 0; }
    if (warthog_cfg_get_mesh_hostfrag() != def) { printf("never stored: %lu", (unsigned long)warthog_cfg_get_mesh_hostfrag()); return 0; }
    static const uint32_t stored[] = { 0, 1, 512, 2346 };
    for (unsigned i = 0; i < 4; i++) {
        if (warthog_cfg_set_mesh_hostfrag(stored[i]) != ESP_OK || warthog_cfg_get_mesh_hostfrag() != stored[i]) {
            printf("stored %lu read %lu", (unsigned long)stored[i], (unsigned long)warthog_cfg_get_mesh_hostfrag()); return 0;
        }
    }
    have = 1; val = 513;
    if (warthog_cfg_get_mesh_hostfrag() != def) { printf("invalid stored: %lu", (unsigned long)warthog_cfg_get_mesh_hostfrag()); return 0; }
    printf("ok");
    return 0;
}
EOF2
# Each build's flags (platformio.ini) and the default it must boot with.
for d49 in "1 -DWARTHOG_MESH_SAE=1" "1 -DWARTHOG_MESH_SAE=1 -DWARTHOG_MESH_CHIP_VIF_MESH=1" \
           "0 -DWARTHOG_MESH_SAE=1 -DWARTHOG_MESH_AMPE_NO_CHIP_KEY=1 -DWARTHOG_MESH_HOST_CCMP=1" \
           "0 -DWARTHOG_MESH_SAE=1 -DWARTHOG_MESH_AMPE_NO_CHIP_KEY=1" "0 -DWARTHOG_MESH_SMOKE=1" "0 -DWARTHOG_REGION_US=1"; do
  r49=""
  ${CC:-cc} -std=gnu11 -w -DWANT=${d49%% *} ${d49#* } -I"$T" -o "$T/t" "$T/t.c" 2>/dev/null && r49=$("$T/t")
  [ "$r49" = ok ] || why49="${why49:-the NVS default (${d49#* }, want ${d49%% *}): ${r49:-did not build or run}}"
done
# The simnode builds are SAE builds: their storage boots with the default of the build each models.
grep -q '^$(SIMNODE_TESTS): CFLAGS += -DWARTHOG_MESH_SAE=1$' Makefile || \
  why49="${why49:-the simnode builds do not define WARTHOG_MESH_SAE}"
rm -rf "$T"
PT=$(awk '/^enum mmwlan_status umac_datapath_process_tx_frame\(/ {on=1} on && /^}/ {exit} on' "$DD/umac_datapath.c")
[ "$(printf '%s\n' "$PT" | grep -cE 'g_warthog_hostfrag([^_]|$)')" = 0 ] && \
  [ "$(printf '%s\n' "$PT" | grep -c 'umac_datapath_mesh_hostfrag_mode()')" = 1 ] && \
  printf '%s\n' "$PT" | grep -q 'const uint32_t hostfrag = umac_datapath_mesh_hostfrag_mode();' || why49="${why49:-the transmit path does not read the mode in force once}"
printf '%s\n' "$PT" | tr -d ' \n' | grep -q 'constboolcut_path=hostfrag!=UMAC_MESH_FRAG_OFF&&data->ops==&datapath_ops_mesh&&!is_multicast&&!is_eapol;' && \
  printf '%s\n' "$PT" | tr -d ' \n' | grep -q 'if(cut_path){' || \
  why49="${why49:-fragmentation is entered off, or for group, EAPOL or non-mesh frames}"
[ "$(printf '%s\n' "$PT" | grep -c 'umac_datapath_aggr_check(')" = 2 ] && \
  printf '%s\n' "$PT" | tr -d ' \n' | grep -q 'if(!is_multicast&&!is_eapol&&!cut_path){umac_datapath_aggr_check(' && \
  printf '%s\n' "$PT" | awk '/umac_datapath_mesh_frag_plan\(/ && !p {p=NR} /umac_datapath_aggr_check\(/ {a=NR}
    /umac_datapath_tx_mesh_frags\(/ && !f {f=NR} END {exit (p && a && f && p < a && a < f) ? 0 : 1}' || \
  why49="${why49:-the Block Ack check is not once a frame, after the cut decision and before the cut}"
[ "$(printf '%s\n' "$PT" | grep -c 'umac_rc_init_rate_table_data(')" = 1 ] && \
  printf '%s\n' "$PT" | tr -d ' \n' | grep -q 'else{if(!rc_done){MMOSAL_DEV_ASSERT(stad!=NULL);umac_rc_init_rate_table_data(' || \
  why49="${why49:-rate control is not asked exactly once a frame}"
if [ -z "$why49" ]; then
  ok "AT+HOSTFRAG= persists the mode and applies it live, boot restores it, NVS holds only valid modes; never stored it is auto on SAE builds with chip keys and off on the rest, a stored value wins, at.c boots with it; off never enters fragmentation, and rate control is asked once a frame"
else
  bad "AT+HOSTFRAG wiring: $why49"
fi

# 50. AT+TXRATE=<mcs>,<bw>|off: the parser is compiled out of main/at.c and run (MCS 0-9; 1, 2, 4 or 8
#     MHz); the command hands it to mmwlan_ate_override_rate_control, off as no override.
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
awk '/^static bool txrate_parse_\(/,/^}/' "$A" > "$T/fn.c"
cat > "$T/t.c" <<'EOF2'
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "fn.c"
static int check(const char *in, bool want_ok, int wm, int wb)
{
    int m = 77, b = 77;
    bool got = txrate_parse_(in, &m, &b);
    if (got != want_ok || (got && (m != wm || b != wb))) { printf("'%s' parsed as %d/%d,%d", in, (int)got, m, b); return 1; }
    return 0;
}
int main(void)
{
    return check("0,1", true, 0, 1) || check("9,8", true, 9, 8) || check("7,2", true, 7, 2) ||
           check("off", true, -1, -1) || check("OFF", true, -1, -1) || check("10,1", false, 0, 0) ||
           check("0,3", false, 0, 0) || check("0,16", false, 0, 0) || check("0", false, 0, 0) ||
           check("0,", false, 0, 0) || check(",1", false, 0, 0) || check("0,1 ", false, 0, 0) ||
           check("-1,1", false, 0, 0) || check("0,-1", false, 0, 0) || check("", false, 0, 0) ||
           txrate_parse_(NULL, NULL, NULL);
}
EOF2
why=""
if ${CC:-cc} -std=gnu11 -w -I"$T" -o "$T/t" "$T/t.c" 2>/dev/null && why=$("$T/t"); then
  ok "AT+TXRATE= accepts <0-9>,<1|2|4|8> or off as the whole argument and rejects the rest"
else
  bad "txrate_parse_ against its cases: ${why:-did not build or run}"
fi
rm -rf "$T"
if awk '/"TXRATE"\) == 0 && terminator == .=./ {on=1} on && /mmwlan_ate_override_rate_control\(\(enum mmwlan_mcs\)mcs, \(enum mmwlan_bw\)bw,/ {c=1}
        on && c && /MMWLAN_GI_NONE\)/ {g=1} on && /reply_ok\(\);/ {exit} END {exit (c && g) ? 0 : 1}' "$A" && \
   grep -q 'strcasecmp(verb, "TXRATE") == 0 && terminator == .?.' "$A"; then
  ok "AT+TXRATE= sets the rate override (off clears it) and AT+TXRATE? reports it"
else
  bad "AT+TXRATE is not wired to mmwlan_ate_override_rate_control, or has no query"
fi

# 51. Host fragmentation's invariants the simulator cannot see: umac_mesh_frag.c is in the firmware
#     build and the simulator's; a fragmented MSDU's buffers are all taken, and every fragment built,
#     before the first reaches the chip; host CCMP's PNs come from one reservation, whose increment
#     runs inside the critical section path selection's does; no fragment carries the A-MPDU flag,
#     and a frame over its limit ends its TID's originator session before it is counted cut, sent
#     whole or cut; one call site.
DM=$DD/umac_datapath_mesh.c
why51=""
grep -q '/src/umac/mesh/umac_mesh_frag.c' ../../halow/components/morselib/CMakeLists.txt || why51="umac_mesh_frag.c is not in the firmware build"
grep -q 'umac_mesh_bip.c umac_mesh_frag.c)' Makefile || why51="${why51:-umac_mesh_frag.c is not in SIMNODE_REAL}"
FR=$(awk '/^static int umac_datapath_tx_mesh_frags\(/ {on=1} on && /^}/ {exit} on' "$DD/umac_datapath.c")
FH=$(awk '/^int umac_datapath_mesh_frags_to_chip\(/ {on=1} on && /^}/ {exit} on' "$DD/umac_datapath.c")
order51=$(printf '%s\n' "$FR" | awk '/umac_datapath_alloc_raw_tx_mmpkt\(/ && !a {a=NR} /umac_datapath_mesh_take_tx_pns\(/ && !p {p=NR}
  /umac_mesh_tx_host_ccmp\(stad,/ && !s {s=NR} /umac_datapath_mesh_ba_park\(stad, tid, frag, plan->n, chip\)/ && !k {k=NR}
  /return umac_datapath_mesh_frags_to_chip\(stad, tid, frag, plan->n, chip\);/ && !t {t=NR}
  END { if (a && p && s && k && t && a < p && p < s && s < k && k < t) print "ok"; else printf "alloc %d, pns %d, seal %d, park %d, tx %d\n", a, p, s, k, t }')
[ "$order51" = ok ] || why51="${why51:-fragment order: $order51}"
[ "$(printf '%s\n' "$FR" | grep -c 'mmdrv_tx_frame(')" = 0 ] && [ "$(printf '%s\n' "$FH" | grep -c 'mmdrv_tx_frame(')" = 1 ] && \
  [ "$(grep -c 'umac_datapath_mesh_frags_to_chip(' "$DD/umac_datapath.c")" = 2 ] || \
  why51="${why51:-more than one hand-off to the chip}"
printf '%s\n%s\n' "$FR" "$FH" | grep -q 'AMPDU' && why51="${why51:-the flags of a fragment mention A-MPDU}"
printf '%s\n' "$FR" | grep -q 'md->flags = (key_id >= 0 && !host_seal) ? MMDRV_TX_FLAG_HW_ENC : 0u;' || why51="${why51:-the flags of a fragment are not HW_ENC or nothing}"
awk '/^uint64_t umac_datapath_mesh_take_tx_pns\(/,/^}/' "$DM" | tr -d ' \n' | \
  grep -q 'MMOSAL_TASK_ENTER_CRITICAL();if(kd->keys\[key_id\]!=NULL){pn=kd->keys\[key_id\]->tx_seq;kd->keys\[key_id\]->tx_seq+=n;}MMOSAL_TASK_EXIT_CRITICAL();' || \
  why51="${why51:-the PN reservation is not one step under the critical section}"
awk '/^uint64_t umac_datapath_mesh_take_tx_pn\(/,/^}/' "$DM" | grep -q 'return umac_datapath_mesh_take_tx_pns(stad, key_id, 1u);' || \
  why51="${why51:-path selection does not reserve from the same allocator}"
PL=$(awk '/^static void umac_datapath_mesh_frag_plan\(/ {on=1} on && /^}/ {exit} on' "$DD/umac_datapath.c")
printf '%s\n' "$PL" | awk '/if \(plan->n == 1u\)/ && !o {o=NR} /umac_datapath_mesh_ba_cut\(stad, md->tid\);/ && !b {b=NR}
  /g_warthog_hostfrag_many\+\+/ && !m {m=NR} /g_warthog_hostfrag_pool\+\+/ && !q {q=NR} /g_warthog_hostfrag_by_/ && !c {c=NR}
  END {exit (o && b && m && q && c && o < b && b < m && b < q && b < c) ? 0 : 1}' && \
  ! printf '%s\n' "$PL" | grep -q 'umac_ba_is_ampdu_permitted' || \
  why51="${why51:-a frame over its limit does not end its Block Ack session before it is cut or sent whole}"
[ "$(grep -c 'umac_datapath_tx_mesh_frags(' "$DD/umac_datapath.c")" = 2 ] || why51="${why51:-umac_datapath_tx_mesh_frags has other than one call site}"
if [ -z "$why51" ]; then
  ok "host fragmentation: in both builds; every buffer taken and fragment built before the first reaches the chip; one PN reservation under the critical section; no A-MPDU; one call site"
else
  bad "host fragmentation invariants: $why51"
fi

# 52. A host fragment's TX status always comes back (the driver is not in the simulator): skbq.c's
#     release without a chip status (bus write failed, page invalid, 15 s stale) strips the
#     driver's header and reports a host fragment untried instead of freeing it; the datapath
#     queues such a status, and takes an untried host fragment before rate control sees it; every
#     fragment is marked, a whole frame never is.
SQ=../../halow/components/mm-iot-sdk/framework/morselib/src/driver/morse_driver/skbq.c
why52=""
# The second such line is the definition; the first, its prototype.
TF=$(awk '/^static int __skbq_data_tx_finish\(struct mmpkt_list \*skbq,$/ {n++} n == 2 && /^{/ {on=1} on {print} on && /^}/ {exit}' "$SQ" |
  sed 's:/\*[^*]*\*/::g' | tr -d ' \n')
case "$TF" in
  *'elseif(tx_metadata->mesh.host_frag!=0||tx_metadata->mesh.ba_wait!=0){structmmpktview*view=mmpkt_open(mmpkt);if(mmwlan_cap_mode[MMWLAN_CAP_TX]!=MMWLAN_CAP_OFF){conststructmorse_buff_skb_header*h=(conststructmorse_buff_skb_header*)mmpkt_get_data_start(view);mmwlan_cap_tx_untried(h->tx_info.pkt_id,h->tx_info.tid);}morse_skb_remove_padding_after_sent_to_chip(view);mmpkt_close(&view);tx_metadata->attempts=0;tx_metadata->status_flags=0;}else{mmpkt_release(mmpkt);return0;}mmdrv_host_process_tx_status(mmpkt);'*) ;;
  *) why52="skbq.c frees a host fragment or a waited-on DELBA it has no chip status for" ;;
esac
HS=$(awk '/^void umac_datapath_handle_tx_status\(/,/^}/' "$DD/umac_datapath.c" | tr -d ' \n')
case "$HS" in *'if(tx_metadata->attempts!=0||tx_metadata->mesh.own_group!=0||tx_metadata->mesh.host_frag!=0||tx_metadata->mesh.ba_wait!=0)'*) ;; *) why52="${why52:-the status of an untried host fragment or waited-on DELBA is released unread}" ;; esac
awk '/^static inline void umac_datapath_process_tx_status_queue\(/ {on=1} on && /^}/ {exit} on' "$DD/umac_datapath.c" | \
  awk '/tx_metadata->mesh.host_frag != 0 && tx_metadata->attempts == 0/ && !h {h=NR}
       /if \(tx_metadata->mesh.ba_wait != 0 && data->ops == &datapath_ops_mesh\)/ && !b {b=NR}
       /umac_datapath_mesh_ba_delba_status\(/ && !d {d=NR} /umac_rc_feedback\(/ && !r {r=NR}
       END {exit (h && b && d && r && h < r && b < d && d < r) ? 0 : 1}' || why52="${why52:-an untried host fragment or DELBA reaches rate control, or the status of a DELBA is not read first}"
FR=$(awk '/^static int umac_datapath_tx_mesh_frags\(/ {on=1} on && /^}/ {exit} on' "$DD/umac_datapath.c")
printf '%s\n' "$FR" | grep -q 'md->mesh.host_frag = 1;' || why52="${why52:-a fragment is not marked}"
awk '/^enum mmwlan_status umac_datapath_process_tx_frame\(/ {on=1} on && /^}/ {exit} on' "$DD/umac_datapath.c" | \
  grep -q 'tx_metadata->mesh.host_frag = 0;' || why52="${why52:-a whole frame may carry the mark}"
if [ -z "$why52" ]; then
  ok "a host fragment's or waited-on DELBA's TX status always returns: the driver reports one it never got a chip status for untried, and the datapath reads it before rate control"
else
  bad "host fragment TX status: $why52"
fi

# 53. A fragment run the chip seals cannot be broken by a frame the host hands it: its fragments go
#     back to back under the run lock (run_begin before the first, run_end after the last); the
#     mesh dequeue asks umac_datapath_mesh_frag_wait before it pops a peer's next frame; every
#     chip-sealed management frame (path selection, the datapath's robust frames) goes through
#     umac_datapath_mesh_tx_chip_mgmt, which hands or holds it under that lock; held frames are
#     handed when a run ends and on the tick, which also clears a run 16 s without its statuses.
MS=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/mesh/umac_mesh.c
why53=""
printf '%s\n' "$FH" | awk '/umac_datapath_mesh_frag_run_begin\(/ && !b {b=NR} /mmdrv_tx_frame\(/ && !t {t=NR}
  /umac_datapath_mesh_frag_run_end\(/ && !e {e=NR} END {exit (b && t && e && b < t && t < e) ? 0 : 1}' || \
  why53="fragments are not handed between run_begin and run_end"
awk '/^void umac_datapath_mesh_frag_run_begin\(/,/^}/' "$DM" | grep -q 'mesh_frag_lock_();' && \
  awk '/^void umac_datapath_mesh_frag_run_end\(/,/^}/' "$DM" | tr -d ' \n' | grep -q 'mesh_frag_drain_locked_();mesh_frag_unlock_();}$' || \
  why53="${why53:-the run lock is not held from run_begin to run_end}"
awk '/^int umac_datapath_mesh_tx_chip_mgmt\(/,/^}/' "$DM" | tr -d ' \n' | \
  grep -q 'mesh_frag_lock_();if(mesh_frag_blocks_(slot,true,0)&&mmpkt_list_length(&s_frag_held)<MESH_FRAG_HOLD_MAX){mmpkt_list_append(&s_frag_held,pkt);.*ret=mmdrv_tx_frame(pkt,true);}mesh_frag_unlock_();umac_datapath_mesh_read_end(side);returnret;}$' || \
  why53="${why53:-a chip-sealed management frame is not handed or held under the run lock}"
awk '/^static bool mesh_dequeue_tx_frame\(/,/^}/' "$DM" | tr -d ' \n' | \
  grep -q 'umac_datapath_mesh_frag_wait(stad,umac_sta_data_peek_pkt(stad))){continue;}structmmpkt\*txbuf=umac_sta_data_pop_pkt(stad);' || \
  why53="${why53:-the mesh dequeue pops a frame without asking the run gate}"
awk '/^static int mesh_tx_hwmp_now_\(/,/^}/' "$MS" | tr -d ' \n' | \
  grep -q 'intrc=k.how==UMAC_MESH_HWMP_PROT_CHIP?umac_datapath_mesh_tx_chip_mgmt(frm):mmdrv_tx_frame(frm,/\*is_mgmt=\*/true);' || \
  why53="${why53:-chip-sealed path selection bypasses the run gate}"
awk '/^enum mmwlan_status umac_datapath_tx_mgmt_frame\(/,/^}/' "$DD/umac_datapath.c" | tr -d ' \n' | \
  grep -q 'constintret=(data->ops==&datapath_ops_mesh&&key_id>=0)?umac_datapath_mesh_tx_chip_mgmt(txbuf):mmdrv_tx_frame(txbuf,true);' || \
  why53="${why53:-a chip-sealed robust frame bypasses the run gate}"
awk '/^void umac_datapath_mesh_frag_status\(/,/^}/' "$DM" | tr -d ' \n' | \
  grep -q 'if(s_frag\[slot\].inflight==0u){mesh_frag_drain_locked_();}mesh_frag_unlock_();}$' || \
  why53="${why53:-the end of a run does not hand what it held}"
awk '/^void umac_datapath_mesh_frag_tick\(/,/^}/' "$DM" | grep -q 'MESH_FRAG_STALE_MS' && \
  grep -q 'umac_datapath_mesh_frag_tick();' "$MS" && \
  awk '/^static inline bool umac_datapath_process_tx\(/,/^}/' "$DD/umac_datapath.c" | grep -q 'umac_datapath_mesh_frag_tick();' || \
  why53="${why53:-the tick does not clear stale runs on the service tick and each TX pass}"
grep -q '^#define MESH_FRAG_STALE_MS 16000u$' "$DM" && grep -q 'static uint32_t tx_status_lifetime_ms = (15 \* 1000);' "$SQ" || \
  why53="${why53:-the stale limit is not above the 15 s status lifetime of the driver}"
if [ -z "$why53" ]; then
  ok "a chip-sealed fragment run: handed back to back under its lock; the dequeue and every chip-sealed management frame wait for it; held frames go when it ends; stale after 16 s"
else
  bad "fragment run gate: $why53"
fi

# 54. Whole frames and the TX pool: on the mesh the host counts a whole unicast frame the chip seals
#     as every PN the chip could draw cutting it (umac_datapath_chip_pns), after its rate table is
#     filled and before it is handed; elsewhere one, as the vendor did. The pool's pause and unpause
#     thresholds less the reserve the frag tick asks for while AT+HOSTFRAG is on.
PK=../../halow/components/mm-iot-sdk/framework/src/mmpktmem/mmpktmem_heap.c
DRV=../../halow/components/mm-iot-sdk/framework/morselib/src/driver/driver.c
why54=""
awk '/^enum mmwlan_status umac_datapath_process_tx_frame\(/ {on=1} on && /^}/ {exit} on' "$DD/umac_datapath.c" | \
  awk '/if \(!rc_done\)/ && !r {r=NR} /umac_datapath_chip_pns\(umacd, &tx_metadata->rc_data,/ && !p {p=NR}
       /umac_datapath_mesh_take_tx_pns\(stad, \(uint8_t\)key_id, pns\);/ && !t {t=NR} /mmdrv_tx_frame\(txbuf, false\)/ && !h {h=NR}
       END {exit (r && p && t && h && r < p && p < t && t < h) ? 0 : 1}' || why54="the PNs of whole frames are not counted after the rates and before the hand-off"
awk '/^enum mmwlan_status umac_datapath_process_tx_frame\(/ {on=1} on && /^}/ {exit} on' "$DD/umac_datapath.c" | tr -d ' \n' | \
  grep -q 'elseif(!host_encrypted&&data->ops==&datapath_ops_mesh&&!is_multicast){chip_pns=true;' || \
  why54="${why54:-the own cut of the chip is not counted for mesh unicast}"
awk '/^static uint32_t umac_datapath_chip_pns\(/,/^}/' "$DD/umac_datapath.c" | tr -d ' \n' | \
  grep -q 'returnumac_mesh_frag_chip_pns(mpdu,over,lim);' || why54="${why54:-the PN bound is not umac_mesh_frag_chip_pns}"
grep -q 'TX_DATA_POOL_UNPAUSE_THRESHOLD \\' "$PK" && grep -q '((int_least32_t)MMPKTMEM_TX_POOL_N_BLOCKS - 2 - (int_least32_t)pktmem.tx_reserve)' "$PK" && \
  grep -q '((int_least32_t)MMPKTMEM_TX_POOL_N_BLOCKS - 1 - (int_least32_t)pktmem.tx_reserve)' "$PK" || \
  why54="${why54:-the pool thresholds do not keep the reserve}"
awk '/^uint32_t mmdrv_tx_pool_free\(/,/^}/' "$DRV" | grep -q 'return mmhal_wlan_pktmem_tx_free();' && \
  awk '/^void mmdrv_set_tx_pool_reserve\(/,/^}/' "$DRV" | grep -q 'mmhal_wlan_pktmem_set_tx_reserve(blocks);' || \
  why54="${why54:-the driver does not pass the pool query and reserve through}"
awk '/^void umac_datapath_mesh_frag_tick\(/,/^}/' "$DM" | tr -d ' \n' | \
  grep -q 'constuint32_twant=umac_datapath_mesh_hostfrag_mode()!=0u?UMAC_DATAPATH_MESH_FRAG_RESERVE:0u;if(want!=s_frag_reserve){mmdrv_set_tx_pool_reserve(want);s_frag_reserve=want;}' || \
  why54="${why54:-the reserve does not follow AT+HOSTFRAG}"
if [ -z "$why54" ]; then
  ok "whole frames: the chip's possible cut counted in the PN floor after the rates; the TX pool keeps its reserve while AT+HOSTFRAG is on"
else
  bad "whole-frame PNs / TX pool: $why54"
fi

# 55. AT+RXCHAN? (rxchan_line_ in main/at.c), compiled out and run: every counter in its slot, and
#     the line within buf[] with every field at its extreme (it needs 468 bytes; buf was 460);
#     cmd_rxchan fills it from at.c's storage, and it is dispatched.
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
{ awk '/^struct rxchanstat \{/,/^};/' "$A"
  awk '/^static int rxchan_line_\(/,/^}/' "$A"; } > "$T/fn.c"
size55=$(awk '/^static void cmd_rxchan\(void\)/ {on=1} on && /char buf\[[0-9]+\];/ {match($0, /\[[0-9]+\]/); print substr($0, RSTART + 1, RLENGTH - 2); exit}' "$A")
cat > "$T/t.c" <<'EOF2'
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fn.c"
int main(int argc, char **argv)
{
    unsigned size = (unsigned)atoi(argv[1]);
    char buf[2048];
    struct rxchanstat s = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
                            { 0xa1, 0xa2, 0xa3 }, { 0xb1, 0xb2, 0xb3 } };
    rxchan_line_(buf, sizeof(buf), &s);
    if (strcmp(buf, "+RXCHAN: pages=1 data=2 beacon=3 mgmt=4 cmd=5 txstat=6 last=0x07 | shim=8 notrunning=9 "
                    "rxframe=10 filter=11 meshctrl=12 ae=13 fwdcand=14(a1a2a3) | rxdrop=15 reason=16 ccmp_key=17 "
                    "blank=18 replay=19 pn=20 | nodec grp=21 uni=22 last(grp=23 fc=0018 key=25 ta=b1b2b3)\r\n") != 0) {
        printf("fields: %s", buf);
        return 0;
    }
    memset(&s, 0xff, sizeof(s));
    int n = rxchan_line_(buf, sizeof(buf), &s);
    (void)argc;
    printf("%s %d/%u\n", n + 1 <= (int)size ? "ok" : "short", n + 1, size);
    return 0;
}
EOF2
fit55=""
if [ -n "$size55" ] && ${CC:-cc} -std=gnu11 -w -I"$T" -o "$T/t" "$T/t.c" 2>/dev/null; then
  fit55=$("$T/t" "$size55")
fi
rm -rf "$T"
fill55=$(awk '/^static void cmd_rxchan\(void\)/,/^}/' "$A" | tr -d ' \n')
for f in .pages=g_warthog_rxchan_pages, .data=g_warthog_rxchan_data, .beacon=g_warthog_rxchan_beacon, \
         .mgmt=g_warthog_rxchan_mgmt, .cmd=g_warthog_rxchan_cmd, .txstat=g_warthog_rxchan_txstat, \
         .last=g_warthog_rxchan_last, .shim=g_warthog_shim_rx, .notrunning=g_warthog_shim_rx_notrunning, \
         .rxframe=g_warthog_rxframe_entry, .filter=g_warthog_filter_entry, \
         .meshctrl=g_warthog_rx_meshctrl_stripped, .ae=g_warthog_rx_meshctrl_ae, \
         .fwdcand=g_warthog_rx_fwd_candidate, \
         '.fwd_da={g_warthog_rx_fwd_last_da[3],g_warthog_rx_fwd_last_da[4],g_warthog_rx_fwd_last_da[5]},' \
         .rxdrop=g_warthog_rxdrop_count, .reason=g_warthog_rxdrop_reason, .ccmp_key=g_warthog_ccmp_last_keyid, \
         .blank=g_warthog_ccmp_blank, .replay=g_warthog_ccmp_replay, .pn=g_warthog_ccmp_last_pn, \
         .nodec_grp=g_warthog_nodec_group_n, .nodec_uni=g_warthog_nodec_uni_n, \
         .nodec_last_grp=g_warthog_nodec_group, .nodec_fc=g_warthog_nodec_fc, .nodec_key=g_warthog_nodec_keyid, \
         '.nodec_ta={g_warthog_nodec_ta[3],g_warthog_nodec_ta[4],g_warthog_nodec_ta[5]},' \
         'rxchan_line_(buf,sizeof(buf),&s);cdc_write(buf);'; do
  case "$fill55" in *"$f"*) ;; *) fit55="cmd_rxchan does not set $f"; break ;; esac
done
awk '/strcasecmp\(verb, "RXCHAN"\) == 0 && terminator == .\?./ {getline; print}' "$A" | grep -q 'cmd_rxchan();' || \
  fit55="AT+RXCHAN? is not dispatched to cmd_rxchan"
case "$fit55" in
  ok*) ok "AT+RXCHAN? prints every counter in its slot, is filled from at.c's storage and fits buf[$size55] (${fit55#ok })" ;;
  *)   bad "AT+RXCHAN?: ${fit55:-did not build or run}" ;;
esac

# 56. AT+CHIPRESTART and AT+CHIPRESTART? (chiprestart_line_ in main/at.c): the line compiled out and
#     run, every counter in its slot and within line[] at its extreme; filled from at.c's storage,
#     which morselib writes (the shim's handler, the health task, the mesh restore and its retry);
#     the trigger is mmwlan_force_chip_restart, a request the loop drops (counted dropped) when the
#     driver is stopped; both are dispatched; every key is named in the AT reference row.
SH=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/umac_mmdrv_shim.c
DH=../../halow/components/mm-iot-sdk/framework/morselib/src/driver/health/driver_health.c
DM=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/datapath/umac_datapath_mesh.c
MM=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/mesh/umac_mesh.c
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
{ awk '/^struct chiprestartstat \{/,/^};/' "$A"
  awk '/^static int chiprestart_line_\(/,/^}/' "$A"; } > "$T/fn.c"
size56=$(awk '/^static void cmd_chiprestart_query\(void\)/ {on=1} on && /char line\[[0-9]+\];/ {match($0, /\[[0-9]+\]/); print substr($0, RSTART + 1, RLENGTH - 2); exit}' "$A")
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
    struct chiprestartstat s = { 1, 2, 12, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    chiprestart_line_(buf, sizeof(buf), &s);
    if (strcmp(buf, "+CHIPRESTART: restarts=1 forced=2 dropped=12 mesh=3 | sta=4 stafail=5 keys=6 "
                    "keyfail=7 cmdfail=8 | retried=9 pending=10 | last_ms=11\r\n") != 0) {
        printf("fields: %s", buf);
        return 0;
    }
    memset(&s, 0xff, sizeof(s));
    int n = chiprestart_line_(buf, sizeof(buf), &s);
    (void)argc;
    printf("%s %d/%u\n", n + 1 <= (int)size ? "ok" : "short", n + 1, size);
    return 0;
}
EOF2
fit56=""
if [ -n "$size56" ] && ${CC:-cc} -std=gnu11 -w -I"$T" -o "$T/t" "$T/t.c" 2>/dev/null; then
  fit56=$("$T/t" "$size56")
fi
rm -rf "$T"
fill56=$(awk '/^static void cmd_chiprestart_query\(void\)/,/^}/' "$A" | tr -d ' \n')
for pair in "n:$SH" "forced:$DH" "dropped:$SH" "mesh:$SH" "sta:$DM" "stafail:$DM" "keys:$DM" "keyfail:$DM" \
            "cmdfail:$MM" "retried:$DM" "pending:$DM" "ms:$SH"; do
  c=${pair%%:*}; src=${pair#*:}
  case "$fill56" in *".$c=g_warthog_chiprestart_$c,"*) ;; *) fit56="cmd_chiprestart_query does not set .$c"; break ;; esac
  grep -q "^volatile uint32_t .*g_warthog_chiprestart_$c = 0" "$A" || { fit56="at.c has no storage for g_warthog_chiprestart_$c"; break; }
  grep -Eq "g_warthog_chiprestart_$c(\+\+| = | \+= )" "$src" || { fit56="${src##*/} never writes g_warthog_chiprestart_$c"; break; }
done
case "$fill56" in *'chiprestart_line_(line,sizeof(line),&s);cdc_write(line);'*) ;; *) fit56="cmd_chiprestart_query does not print its line" ;; esac
awk '/^static void cmd_chiprestart\(void\)/,/^}/' "$A" | grep -q 'mmwlan_force_chip_restart()' || \
  fit56="AT+CHIPRESTART does not call mmwlan_force_chip_restart"
awk '/^static void chip_restart_request_evt_handler\(/,/^}/' "$SH" | tr -d ' \n' | \
  grep -q 'if(mmdrv_force_health_check_fail()!=0){g_warthog_chiprestart_dropped++;' || \
  fit56="a request the loop cannot pass to a stopped driver is not counted dropped"
awk '/strcasecmp\(verb, "CHIPRESTART"\) == 0 && terminator == .\\0./ {getline; print}' "$A" | grep -q 'cmd_chiprestart();' || \
  fit56="AT+CHIPRESTART is not dispatched to cmd_chiprestart"
awk '/strcasecmp\(verb, "CHIPRESTART"\) == 0 && terminator == .\?./ {getline; print}' "$A" | grep -q 'cmd_chiprestart_query();' || \
  fit56="AT+CHIPRESTART? is not dispatched to cmd_chiprestart_query"
row56=$(grep '^| `AT+CHIPRESTART`' ../../../wiki/AT-Command-Reference.md)
[ -n "$row56" ] || fit56="no AT+CHIPRESTART row in the AT reference"
for k in restarts forced dropped mesh sta stafail keys keyfail cmdfail retried pending last_ms; do
  case "$row56" in *"\`$k\`"*) ;; *) fit56="the AT reference row does not name \`$k\`"; break ;; esac
done
case "$fit56" in
  ok*) ok "AT+CHIPRESTART triggers through mmwlan_force_chip_restart; ? prints every counter in its slot from storage morselib writes and fits line[$size56] (${fit56#ok })" ;;
  *)   bad "AT+CHIPRESTART: ${fit56:-did not build or run}" ;;
esac

# 57. A chip restart under the mesh: the shim's handler reloads the chip before it looks at what
#     can go back, hands the mesh to umac_mesh_handle_hw_restarted, counts mesh only for a complete
#     restore and asserts only when that fails (and on an AP, as upstream); the STA path runs only
#     without a mesh. mmdrv_deinit stops the host beacon timer before it clears what the timer
#     points at. The health task's failure path, which AT+CHIPRESTART takes, pauses TX before it
#     posts the restart; a driver that starts again has no forced failure armed. Every other task
#     reaches the driver through the event loop the restart runs on: the probe burst and the mesh
#     service tick only post (AT+CRYPTOHOST is served on the loop), AT+CHIPRESTART posts its request
#     and the shim's handler fails the check, and AT+FRAG='s threshold is set from the loop.
DRV=../../halow/components/mm-iot-sdk/framework/morselib/src/driver/driver.c
UM=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/umac.c
MW=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/mesh/mmwlan_mesh.c
why57=""
awk '/^static void hw_restart_evt_handler\(/,/^}/' "$SH" | \
  awk '/UMAC_INTERFACE_AP\) != UMAC_INTERFACE_VIF_ID_INVALID/ {ap=NR} /MMOSAL_ASSERT\(false\);/ {a++; if (!f) f=NR}
       /mmdrv_deinit\(\);/ {d=NR} /MMOSAL_ASSERT\(mmdrv_init\(NULL, country_code\) == 0\);/ {i=NR}
       /umac_datapath_mesh_chip_booted\(true\);/ {b=NR} /if \(!mesh\)/ {s=NR} /umac_connection_handle_hw_restarted\(umacd\);/ {c=NR}
       /else if \(umac_mesh_handle_hw_restarted\(umacd, &complete\) == MMWLAN_SUCCESS\)/ {m=NR}
       /g_warthog_chiprestart_mesh \+= complete \? 1u : 0u;/ {k=NR} /mmdrv_hw_restart_completed\(\);/ {r=NR}
       END {exit (a == 2 && ap < f && f < d && d < i && i < b && b < s && s < c && c < m && m < k && k < r) ? 0 : 1}' || \
  why57="the handler does not reload, then hand the mesh to umac_mesh_handle_hw_restarted (counting only a complete restore, asserting only on its failure), in that order"
awk '/^void mmdrv_deinit\(/,/^}/' "$DRV" | \
  awk '/driver_data.started = false;/ {s=NR} /morse_beacon_teardown\(&driver_data\);/ {t=NR} /driver_task_stop\(/ {k=NR}
       /memset\(&driver_data, 0, sizeof\(driver_data\)\);/ {m=NR} END {exit (s && t && k && m && s < t && t < k && k < m) ? 0 : 1}' || \
  why57="${why57:-mmdrv_deinit does not stop the host beacon timer before it clears the driver}"
awk '/^static void morse_reset_chip\(/,/^}/' "$DH" | \
  awk '/mmdrv_host_set_tx_paused\(MMDRV_PAUSE_SOURCE_MASK_HW_RESTART, true\);/ {p=NR} /mmdrv_host_hw_restart_required\(\);/ {r=NR}
       END {exit (p && r && p < r) ? 0 : 1}' || why57="${why57:-the failure path of the health task no longer pauses TX before it posts the restart}"
grep -q '^            morse_reset_chip();' "$DH" || why57="${why57:-a failed check no longer restarts the chip}"
awk '/^int driver_health_init\(/,/^}/' "$DH" | grep -q 'driverd->health_check.force_fail = false;' || \
  why57="${why57:-driver_health_init leaves a forced failure armed}"
for fn in '^int umac_mesh_tx_broadcast_probe\(' '^void umac_mesh_service_tick\(' ; do
  body=$(awk "/$fn/,/^}/" "$MM")
  [ -n "$body" ] || { why57="${why57:-$fn is gone}"; break; }
  case "$body" in *mmdrv_*) why57="${why57:-$fn calls the driver from the probe task}" ;; esac
  case "$body" in *'umac_core_evt_queue(s_mesh_umacd, &evt)'*) ;; *) why57="${why57:-$fn does not post to the event loop}" ;; esac
done
awk '/^static void mesh_probe_evt_\(/,/^}/' "$MM" | grep -q 'mmdrv_tx_frame(probe' || \
  why57="${why57:-the probe request is not sent from the event loop}"
awk '/^static void mesh_service_evt_\(/,/^}/' "$MM" | grep -q 'mesh_service_cryptohost_();' || \
  why57="${why57:-AT+CRYPTOHOST is not served on the event loop}"
mw=$(awk '/^enum mmwlan_status mmwlan_force_chip_restart\(/,/^}/' "$MW")
case "$mw" in *mmdrv_*) why57="${why57:-AT+CHIPRESTART reaches the driver from the AT task}" ;; esac
case "$mw" in *'umac_chip_restart_request(umacd)'*) ;; *) why57="${why57:-AT+CHIPRESTART is not posted to the event loop}" ;; esac
awk '/^enum mmwlan_status umac_chip_restart_request\(/,/^}/' "$SH" | grep -q 'umac_core_evt_queue(umacd, &evt)' && \
  awk '/^static void chip_restart_request_evt_handler\(/,/^}/' "$SH" | grep -q 'mmdrv_force_health_check_fail()' || \
  why57="${why57:-the AT+CHIPRESTART request does not reach the driver from the event loop}"
fr=$(awk '/^enum mmwlan_status mmwlan_set_fragment_threshold\(/,/^}/' "$UM")
case "$fr" in *mmdrv_set_frag_threshold*) why57="${why57:-AT+FRAG= sets the threshold from the AT task}" ;; esac
case "$fr" in *'UMAC_QUEUE_EVT_AND_WAIT(umac_set_frag_threshold_evt_handler'*) ;; *) why57="${why57:-AT+FRAG= is not sent through the event loop}" ;; esac
awk '/^static void umac_set_frag_threshold_evt_handler\(/,/^}/' "$UM" | grep -q 'mmdrv_set_frag_threshold(threshold)' || \
  why57="${why57:-the AT+FRAG= event handler does not set the threshold}"
if [ -z "$why57" ]; then
  ok "a chip restart reloads the chip, then the mesh puts back what it held (an AP or a failed restore reset the board); the beacon timer stops before the driver is cleared; the probe burst, AT+CRYPTOHOST, AT+CHIPRESTART and AT+FRAG= reach the driver only from the event loop"
else
  bad "chip restart: $why57"
fi

# 58. AT+AMPDU=<0|1> and AT+AMPDU? (ampdu_line_ in main/at.c, compiled out and run): the line names
#     the setting in force and in NVS (on/off) and every counter (the sessions =0 ended and those
#     whose DELBA it could not send; the Block Ack frames umac_ba.c sends, the DELBAs by reason;
#     the peer's DELBAs for our sessions), within line[] at their extreme,
#     filled from at.c's storage, which morselib writes; =0 and =1 only, persisted in NVS
#     (mesh_ampdu, default 1, nothing else read back) and applied live; boot restores it before the
#     first data frame; both are dispatched; every key is named in the AT reference row.
MD=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/datapath/umac_datapath_mesh.c
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
{ awk '/^struct ampdustat \{/,/^};/' "$A"
  awk '/^static int ampdu_line_\(/,/^}/' "$A"; } > "$T/fn.c"
size58=$(awk '/^static void cmd_ampdu_query\(void\)/ {on=1} on && /char line\[[0-9]+\];/ {match($0, /\[[0-9]+\]/); print substr($0, RSTART + 1, RLENGTH - 2); exit}' "$A")
cat > "$T/t.c" <<'EOF2'
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fn.c"
int main(int argc, char **argv)
{
    unsigned size = (unsigned)atoi(argv[1]);
    char buf[512];
    struct ampdustat s = { 0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 38 };
    ampdu_line_(buf, sizeof(buf), &s);
    if (strcmp(buf, "+AMPDU: mode=off stored=on | orig=3 ended=4 unsent=5 | addba_tx=6 delba_to=7 "
                    "delba_end=8 delba_other=9 | rx_delba=10 rx_reason=38\r\n") != 0) { printf("fields: %s", buf); return 0; }
    memset(&s, 0xff, sizeof(s));
    int n = ampdu_line_(buf, sizeof(buf), &s);
    if (strncmp(buf, "+AMPDU: mode=on stored=on ", 26) != 0) { printf("on: %s", buf); return 0; }
    (void)argc;
    printf("%s %d/%u\n", n + 1 <= (int)size ? "ok" : "short", n + 1, size);
    return 0;
}
EOF2
fit58=""
if [ -n "$size58" ] && ${CC:-cc} -std=gnu11 -w -I"$T" -o "$T/t" "$T/t.c" 2>/dev/null; then
  fit58=$("$T/t" "$size58")
fi
rm -rf "$T"
fill58=$(awk '/^static void cmd_ampdu_query\(void\)/,/^}/' "$A" | tr -d ' \n')
case "$fill58" in *'.mode=g_warthog_ampdu,.stored=warthog_cfg_get_mesh_ampdu(),.orig=g_warthog_ampdu_orig,.ended=g_warthog_ampdu_ended,.unsent=g_warthog_ampdu_unsent,'*) ;;
  *) fit58="cmd_ampdu_query does not fill mode, stored, orig, ended and unsent" ;; esac
grep -q '^volatile uint32_t g_warthog_ampdu = 1;' "$A" || fit58="the default is not on"
grep -q '^volatile uint32_t g_warthog_ampdu_orig = 0, g_warthog_ampdu_ended = 0, g_warthog_ampdu_unsent = 0;' "$A" || fit58="at.c has no storage for orig, ended and unsent"
grep -q 'g_warthog_ampdu_orig = orig;' "$MD" && grep -q 'g_warthog_ampdu_ended += ended > 0 ? 1u : 0u;' "$MD" && \
  grep -q 'g_warthog_ampdu_unsent += ended < 0 ? 1u : 0u;' "$MD" || fit58="morselib does not write orig, ended and unsent"
BA=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/ba/umac_ba.c
for c in addba_tx delba_to delba_end delba_other rx_delba rx_reason; do
  case "$fill58" in *".$c=g_warthog_ba_$c"*) ;; *) fit58="cmd_ampdu_query does not set .$c"; break ;; esac
  grep -q "^volatile uint32_t .*g_warthog_ba_$c = 0" "$A" || { fit58="at.c has no storage for g_warthog_ba_$c"; break; }
  grep -Eq "g_warthog_ba_$c(\+\+| = )|&g_warthog_ba_$c( |;)" "$BA" || { fit58="umac_ba.c never writes g_warthog_ba_$c"; break; }
done
awk '/^static enum mmwlan_status umac_ba_tx_delba\(/,/^}/' "$BA" | tr -d ' \n' | \
  grep -q 'if(st==MMWLAN_SUCCESS){volatileuint32_t\*n=reason==DOT11_REASON_INACTIVITY?&g_warthog_ba_delba_to:reason==DOT11_REASON_END_TS_BS?&g_warthog_ba_delba_end:&g_warthog_ba_delba_other;(\*n)++;}returnst;' || \
  fit58="a DELBA is counted other than once, by its reason, when handed to the chip"
awk '/^static void umac_ba_rx_delba\(/,/^}/' "$BA" | \
  awk '/session = &data->sessions.originator\[tid\];/ {o=NR} /g_warthog_ba_rx_delba\+\+;/ {c=NR} /session->status == UMAC_BA_DISABLED/ {d=NR}
       END {exit (o && c && d && o < c && c < d) ? 0 : 1}' || fit58="a peer's DELBA for our session is not counted before the ended-session return"
DA=$(awk '/strcasecmp\(verb, "AMPDU"\) == 0 && terminator == .=./ {on=1} on {print} on && /reply_ok\(\);/ {exit}' "$A" | tr -d ' \n')
case "$DA" in *'if(strcmp(a,"0")!=0&&strcmp(a,"1")!=0){reply_error('*'}elseif(warthog_cfg_set_mesh_ampdu((uint8_t)(a[0]-'"'"'0'"'"'))!=ESP_OK){'*'g_warthog_ampdu=(uint32_t)(a[0]-'"'"'0'"'"');'*) ;;
  *) fit58="AT+AMPDU= does not take exactly 0 or 1, persist it and apply it live" ;; esac
awk '/strcasecmp\(verb, "AMPDU"\) == 0 && terminator == .\?./ {getline; print}' "$A" | grep -q 'cmd_ampdu_query();' || \
  fit58="AT+AMPDU? is not dispatched to cmd_ampdu_query"
CF=../../../main/cfg.c
awk '/^uint8_t warthog_cfg_get_mesh_ampdu\(/,/^}/' "$CF" | tr -d ' \n' | \
  grep -q 'uint8_ton=1;.*nvs_get_u8(h,"mesh_ampdu",&v)==ESP_OK&&v<=1' || fit58="NVS reads back something other than 0 or 1, or the default is not on"
awk '/^esp_err_t warthog_cfg_set_mesh_ampdu\(/,/^}/' "$CF" | tr -d ' \n' | grep -q 'if(on>1){returnESP_ERR_INVALID_ARG;}' || \
  fit58="NVS stores values other than 0 or 1"
grep -q 'g_warthog_ampdu = warthog_cfg_get_mesh_ampdu();' ../../../main/mesh.c || fit58="boot does not restore AT+AMPDU"
row58=$(grep '^| `AT+AMPDU=' ../../../wiki/AT-Command-Reference.md)
[ -n "$row58" ] || fit58="no AT+AMPDU row in the AT reference"
for k in mode stored orig ended unsent addba_tx delba_to delba_end delba_other rx_delba rx_reason; do
  case "$row58" in *"\`$k\`"*) ;; *) fit58="the AT reference row does not name \`$k\`"; break ;; esac
done
case "$fit58" in
  ok*) ok "AT+AMPDU= takes 0 or 1, persists and applies it, boot restores it; ? prints every counter from morselib's storage and fits line[$size58] (${fit58#ok })" ;;
  *)   bad "AT+AMPDU: ${fit58:-did not build or run}" ;;
esac

# 59. Block Ack under host fragmentation, as the simulator cannot see it: the ADDBA gate sits in
#     umac_datapath_aggr_check after the configuration and capability checks and before the
#     session starts, mesh only; umac_ba_originator_stop acts on a requested or agreed session only,
#     cancels its ADDBA retry, sends a DELBA as originator with reason 37 (mac80211's
#     WLAN_REASON_QSTA_NOT_USE for its own stop) and clears the session whether or not that DELBA
#     could go, keeping its ADDBA backoff; ba_end and ended count only a DELBA handed to the chip;
#     the hold-off is 15 s (mac80211's HT_AGG_RETRIES_PERIOD), re-armed by each frame that needed
#     cutting, counted once a re-arm for the ADDBA it keeps back, and off with AT+HOSTFRAG=0;
#     AT+AMPDU=0 closes the gate and ends every session once, from the tick after it releases the
#     run lock a DELBA it sends may take; a peer added or removed starts with no hold.
BA=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/ba/umac_ba.c
why59=""
awk '/^static void umac_datapath_aggr_check\(/,/^}/' "$DD/umac_datapath.c" | \
  awk '/umac_config_is_ampdu_enabled\(umacd\)/ {c=NR} /if \(data->ops == &datapath_ops_mesh && !umac_datapath_mesh_ba_may_start\(stad, tid\)\)/ {g=NR}
       /umac_ba_session_init\(/ {i=NR} END {exit (c && g && i && c < g && g < i) ? 0 : 1}' || \
  why59="the ADDBA gate is not between the capability check and the session start"
ST=$(awk '/^int umac_ba_originator_stop\(/,/^}/' "$BA" | tr -d ' \n' | sed 's:/\*[^*]*\*/::g')
case "$ST" in *'if(session->status!=UMAC_BA_REQUESTED&&session->status!=UMAC_BA_SUCCESS){return0;}(void)umac_core_cancel_timeout(umac_sta_data_get_umacd(stad),umac_ba_addba_req_timeout_handler,stad,session);constenummmwlan_statusst=umac_ba_tx_delba(stad,DOT11_DELBA_INITIATOR_ORIGINATOR,tid,DOT11_REASON_END_TS_BS);constuint32_tbackoff=session->attempt_backoff;memset(session,0,sizeof(*session));session->attempt_backoff=backoff;'*'returnst==MMWLAN_SUCCESS?1:-1;}'*) ;;
  *) why59="${why59:-umac_ba_originator_stop does not cancel the retry, send a DELBA as originator and clear the session, its backoff kept}" ;; esac
grep -q 'DOT11_REASON_END_TS_BS = 37,' ../../halow/components/mm-iot-sdk/framework/morselib/src/dot11/dot11.h || \
  why59="${why59:-DOT11_REASON_END_TS_BS is not 37}"
grep -q '^#define UMAC_MESH_FRAG_BA_HOLD_MS 15000u$' ../../halow/components/mm-iot-sdk/framework/morselib/src/umac/mesh/umac_mesh_frag.h || \
  why59="${why59:-the hold-off is not 15 s}"
awk '/^static bool mesh_ba_holds_\(/,/^}/' "$MD" | tr -d ' \n' | \
  grep -q 'return(s_ba_held\[slot\]&(1u<<tid))!=0u&&umac_datapath_mesh_hostfrag_mode()!=UMAC_MESH_FRAG_OFF&&(uint32_t)(now-s_ba_hold_at\[slot\]\[tid\])<UMAC_MESH_FRAG_BA_HOLD_MS;' || \
  why59="${why59:-the hold-off is not bounded by its period, or applies with AT+HOSTFRAG=0}"
awk '/^void umac_datapath_mesh_ba_cut\(/,/^}/' "$MD" | \
  awk '/umac_ba_originator_stop\(stad, tid\)/ {s=NR} /s_ba_hold_at\[slot\]\[tid\] = now;/ {a=NR} END {exit (s && a && s < a) ? 0 : 1}' || \
  why59="${why59:-a frame that needs cutting does not end the session and re-arm the hold}"
awk '/^void umac_datapath_mesh_ba_cut\(/,/^}/' "$MD" | tr -d ' \n' | \
  grep -q 'if(ended>0){g_warthog_hostfrag_ba_end++;.*}elseif(ended<0){g_warthog_hostfrag_nodelba++;}s_ba_held\[slot\]|=(uint8_t)(1u<<tid);s_ba_hold_counted\[slot\]&=(uint8_t)~(1u<<tid);' || \
  why59="${why59:-ba_end counts a DELBA not sent, or a re-arm does not start a new hold count}"
awk '/^bool umac_datapath_mesh_ba_may_start\(/,/^}/' "$MD" | tr -d ' \n' | \
  grep -q 'if(g_warthog_ampdu==0u){returnfalse;}.*if(umac_ba_originator_idle(stad,tid)&&(s_ba_hold_counted\[slot\]&(1u<<tid))==0u){s_ba_hold_counted\[slot\]|=(uint8_t)(1u<<tid);g_warthog_hostfrag_hold++;' || \
  why59="${why59:-AT+AMPDU=0 does not close the gate, or hold counts more than the ADDBA kept back a re-arm}"
awk '/^void umac_datapath_mesh_frag_tick\(/,/^}/' "$MD" | \
  awk '/mesh_frag_unlock_\(\);/ {u=NR} /mesh_ba_tick_\(now\);/ {b=NR} END {exit (u && b && u < b) ? 0 : 1}' || \
  why59="${why59:-the tick ends sessions under the run lock}"
awk '/^static void mesh_ba_tick_\(/,/^}/' "$MD" | tr -d ' \n' | \
  grep -q 'constintended=want==0u&&s_ampdu_applied!=0u?umac_ba_originator_stop(stad,t):0;' || \
  why59="${why59:-AT+AMPDU=0 does not end every session once}"
[ "$(grep -c '    mesh_ba_clear_(slot);\|            mesh_ba_clear_(i);' "$MD")" = 2 ] || \
  why59="${why59:-a peer added or removed may inherit a hold}"
if [ -z "$why59" ]; then
  ok "Block Ack: the ADDBA gate before the session start; a session ends as mac80211's own stop (retry cancelled, DELBA reason 37, cleared, backoff kept); counted only with its DELBA sent; 15 s hold re-armed per cut frame, its kept-back ADDBA counted once a re-arm; AT+AMPDU=0 ends sessions once, outside the run lock"
else
  bad "Block Ack under host fragmentation: $why59"
fi

# 60. Readers and keys of chip-sealed management frames: umac_datapath_mesh_tx_chip_mgmt and
#     umac_datapath_mesh_hwmp_tx_key run on any task, so each resolves the peer between read_begin
#     and read_end, and hwmp_tx_key uses the record its address matched, never the slot again; a
#     held or new one whose peer's key a chip restart still owes is dropped (counted nokey), on the
#     flag the data path reads; a chip boot asks the TX pool's reserve again once; the firmware
#     links mmpktmem_heap.c, the only pool that defines the reserve and the free count
#     (mmpktmem_static.c is as Morse ships it).
why60=""
for fn in '^int umac_datapath_mesh_tx_chip_mgmt\(' '^void umac_datapath_mesh_hwmp_tx_key\('; do
  awk "/$fn/,/^}/" "$MD" | awk '/umac_datapath_mesh_read_begin\(\)/ && !b {b=NR} /mesh_slot_of_\(|mesh_hwmp_tx_key_\(/ && !l {l=NR}
       /umac_datapath_mesh_read_end\(side\);/ {e=NR} /return ret;|^}/ {r=NR} END {exit (b && l && e && b < l && l < e) ? 0 : 1}' || \
    { why60="$fn does not resolve the peer between read_begin and read_end"; break; }
done
HK=$(awk '/^static void mesh_hwmp_tx_key_\(/,/^}/' "$MD" | tr -d ' \n')
case "$HK" in *'structumac_sta_data*stad=NULL;constintslot=mesh_slot_rec_of_(da,&stad);if(!mesh_stad_mfp_(slot,stad)){return;}'*) ;;
  *) why60="${why60:-hwmp_tx_key does not take the record its address matched}" ;; esac
case "$HK" in *'s_peers['*|*'mesh_slot_mfp_('*) why60="${why60:-hwmp_tx_key loads the record of the slot again}" ;; esac
awk '/^static bool mesh_chip_mgmt_keyless_\(/,/^}/' "$MD" | grep -q 'return slot >= 0 && (s_restore\[slot\] & MESH_RESTORE_MTK) != 0u;' && \
  awk '/^bool umac_datapath_mesh_chip_key_missing\(/,/^}/' "$MD" | grep -q '(s_restore\[slot\] & MESH_RESTORE_MTK) != 0u' || \
  why60="${why60:-management frames and data read different key-owed flags}"
awk '/^static void mesh_frag_drain_locked_\(/,/^}/' "$MD" | tr -d ' \n' | \
  grep -q 'elseif(mesh_chip_mgmt_keyless_(slot)){mmpkt_release(pkt);g_warthog_tx_nokey++;}else{(void)mmdrv_tx_frame(pkt,true);}' || \
  why60="${why60:-a held management frame goes to a chip that lacks its key}"
awk '/^int umac_datapath_mesh_tx_chip_mgmt\(/,/^}/' "$MD" | tr -d ' \n' | \
  grep -q 'elseif(mesh_chip_mgmt_keyless_(slot)){mmpkt_release(pkt);g_warthog_tx_nokey++;ret=-1;}' || \
  why60="${why60:-a new management frame goes to a chip that lacks its key}"
awk '/^void umac_datapath_mesh_chip_booted\(/,/^}/' "$MD" | \
  awk '/s_frag_reserve = UINT32_MAX;/ {n++; r=NR} /for \(int i = 0; i < MESH_MAX_PEERS; i\+\+\)/ && !f {f=NR}
       END {exit (n == 1 && r < f) ? 0 : 1}' || why60="${why60:-a chip boot asks the pool reserve again more than once}"
PM=../../halow/components/mm-iot-sdk/framework/src/mmpktmem
grep -q 'mmpktmem_heap.c' ../../halow/components/mmpktmem/CMakeLists.txt && \
  ! grep -q 'mmpktmem_static.c' ../../halow/components/mmpktmem/CMakeLists.txt && \
  grep -q '^uint32_t mmhal_wlan_pktmem_tx_free(void)' "$PM/mmpktmem_heap.c" && \
  ! grep -q 'tx_reserve\|mmhal_wlan_pktmem_tx_free' "$PM/mmpktmem_static.c" || \
  why60="${why60:-the TX pool reserve is not in exactly the pool the firmware links}"
if [ -z "$why60" ]; then
  ok "chip-sealed management frames: the peer read as a reader on any task, its record used as matched; dropped (nokey) while a chip restart owes the key, held or new; the pool reserve asked once a boot, in the one pool built"
else
  bad "chip-sealed management frames / pool: $why60"
fi

# 61. The wait for a cut's DELBA (AT+HOSTFRAG), as the simulator cannot see it: only the DELBA
#     umac_datapath_mesh_ba_cut sends while it stops a session is marked (the peer named around that
#     one call, the frame a Block Ack DELBA), on the frame handed to the chip after host CCMP; the
#     dequeue holds the peer while it waits; it is released only on a TX pass the datapath is not
#     paused, never into a run it could break, under the data path's key check; the guard is 20 ms
#     and the limit 500 ms; a chip boot makes it due; a peer added or removed starts with none.
why61=""
CB=$(awk '/^void umac_datapath_mesh_ba_cut\(/,/^}/' "$MD" | tr -d ' \n')
case "$CB" in *'s_ba_delba_to=stad;constintended=umac_ba_originator_stop(stad,tid);s_ba_delba_to=NULL;'*) ;;
  *) why61="the DELBA is not marked around the one stop that sends it" ;; esac
[ "$(grep -c 's_ba_delba_to = ' "$MD")" = 2 ] || why61="${why61:-the marked peer is set elsewhere}"
awk '/^bool umac_datapath_mesh_ba_delba_tagged\(/,/^}/' "$MD" | tr -d ' \n' | \
  grep -q 'returnstad!=NULL&&stad==s_ba_delba_to&&mmpkt_get_data_length(view)>=26u&&d\[24\]==DOT11_ACTION_CATEGORY_BLOCK_ACK&&d\[25\]==DOT11_BA_ACTION_NDP_DELBA;' || \
  why61="${why61:-a frame other than that DELBA can be marked}"
awk '/^enum mmwlan_status umac_datapath_tx_mgmt_frame\(/,/^}/' "$DD/umac_datapath.c" | \
  awk '/const bool ba_wait = umac_datapath_mesh_ba_delba_tagged\(stad, txbufview\);/ {t=NR}
       /txbuf = umac_datapath_mesh_protect_mgmt\(txbuf, &key_id\);/ {p=NR}
       /tx_metadata->mesh.ba_wait = ba_wait \? 1u : 0u;/ {m=NR} END {exit (t && p && m && t < p && p < m) ? 0 : 1}' || \
  why61="${why61:-the mark is not read before host CCMP and set on the frame that goes}"
awk '/^bool umac_datapath_mesh_frag_wait\(/,/^}/' "$MD" | tr -d ' \n' | grep -q 'if(s_ba_wait\[slot\].active){returntrue;' || \
  why61="${why61:-the dequeue does not hold a waiting peer}"
awk '/^static inline bool umac_datapath_process_tx\(/,/^}/' "$DD/umac_datapath.c" | tr -d ' \n' | \
  grep -q 'if(!umac_datapath_tx_is_paused(data,~MMDRV_PAUSE_SOURCE_MASK_PKTMEM)){umac_datapath_mesh_ba_release();' || \
  why61="${why61:-a wait is released while the datapath is paused}"
[ "$(grep -c 'umac_datapath_mesh_ba_release();' "$DD/umac_datapath.c")" = 1 ] || why61="${why61:-a wait is released from elsewhere}"
RL=$(awk '/^void umac_datapath_mesh_ba_release\(/,/^}/' "$MD" | tr -d ' \n')
case "$RL" in *'mesh_frag_blocks_(i,false,mesh_tid_ac_(s_ba_wait[i].tid))'*'if(n!=0u&&s_ba_wait[i].chip&&umac_datapath_mesh_chip_key_missing(s_peers[i],false)){'*'g_warthog_tx_nokey++;'*'umac_datapath_mesh_frags_to_chip('*) ;;
  *) why61="${why61:-a released MSDU may break a run in the chip or go under a key the chip lacks}" ;; esac
FH61=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/mesh/umac_mesh_frag.h
grep -q '^#define UMAC_MESH_FRAG_BA_GUARD_MS 20u$' "$FH61" && grep -q '^#define UMAC_MESH_FRAG_BA_WAIT_MAX_MS 500u$' "$FH61" || \
  why61="${why61:-the guard is not 20 ms or the limit not 500 ms}"
awk '/^void umac_datapath_mesh_chip_booted\(/,/^}/' "$MD" | grep -q 'mesh_ba_wait_lost_(i);' || \
  why61="${why61:-a chip boot leaves a wait on a DELBA it purged}"
awk '/^static void mesh_ba_clear_\(/,/^}/' "$MD" | grep -q 'mesh_ba_wait_end_(slot);' && \
  awk '/^static void mesh_ba_wait_end_\(/,/^}/' "$MD" | grep -q 'mmpkt_list_clear(&s_ba_wait\[slot\].frags);' || \
  why61="${why61:-a peer added or removed may inherit a wait, or its waiting fragments leak}"
if [ -z "$why61" ]; then
  ok "the DELBA wait: only the cut's own DELBA is marked, on the frame that goes; the dequeue holds the peer; released on an unpaused TX pass, never into a run it could break or under a missing key; 20 ms guard, 500 ms limit; a chip boot makes it due; peers start with none"
else
  bad "DELBA wait: $why61"
fi

# 62. An MMOSAL_ASSERT ends in the panic path (shims/mmosal_shim_freertos_esp32.c): mmosal_impl_assert
#     calls esp_system_abort and nothing that prints, sleeps, reaches the scheduler or runs esp_restart's
#     shutdown handlers (TinyUSB owns the console's USB PHY; an ISR or a critical section can assert);
#     nothing in the shim prints to the ROM console or dumps records at boot; mmosal.h logs the record
#     before the handler runs; the records are in .noinit and a record's pc is its call site where
#     mmport.h reads none. Compiled out and run: the reason the core dump keeps, formatted without libc
#     (its exact text, and at its longest within its buffer), and AT+ASSERT?'s read of the records
#     (oldest first, numbered from the count, none without the magic, AT+ASSERT=0 clears them).
#     sdkconfig.defaults keeps the silent reboot and the ELF core dump in flash, with the core-dump
#     writer's logs off (they print to the same console) and the dump on its own stack of at least
#     1792 bytes (with 0 it runs on the stack that asserted, which need not have room for it);
#     every generated sdkconfig.warthog-* that dumps to flash says the same, deprecated alias
#     included, since PlatformIO does not re-apply the defaults to an existing one.
SHIM=../../halow/components/shims/mmosal_shim_freertos_esp32.c
MO=../../halow/components/mm-iot-sdk/framework/morselib/include/mmosal.h
why62=""
IA=$(awk '/^void mmosal_impl_assert\(void\)/,/^}/' "$SHIM")
case "$IA" in *'esp_system_abort(assert_reason_());'*) ;; *) why62="mmosal_impl_assert does not end in esp_system_abort" ;; esac
for no62 in printf esp_backtrace mmosal_task_sleep vTaskDelay mmhal_reset esp_restart mmhal_log_flush while \
            MMPORT_BREAKPOINT mmosal_disable_interrupts; do
  case "$IA" in *"$no62"*) why62="${why62:-mmosal_impl_assert calls $no62}" ;; esac
done
grep -Eq 'ets_printf|esp_rom_printf|esp_backtrace_print|ESP_SYSTEM_INIT_FN|HALT_ON_ASSERT|ESP_EARLY_LOG' "$SHIM" && \
  why62="${why62:-the shim prints to the ROM console, dumps records at boot or halts on an assert}"
{ awk '/^static const char \*assert_reason_\(void\)/,/^}/' "$SHIM"; awk '/^static char \*assert_put/,/^}/' "$SHIM"; } | \
  grep -Eq '(printf|str(len|n?cpy|n?cat|chr)|mem(cpy|set|move)|[a-z]toa)\(' && why62="${why62:-the reason is formatted with libc}"
awk '/^#define MMOSAL_ASSERT\(expr\)/,/^#endif/' "$MO" | \
  awk '/MMOSAL_LOG_FAILURE_INFO\(0\);/ {l=NR} /mmosal_impl_assert\(\);/ {a=NR} END {exit (l && a && l < a) ? 0 : 1}' || \
  why62="${why62:-MMOSAL_ASSERT no longer logs the record before the handler}"
grep -q '^struct mmosal_preserved_failure_info preserved_failure_info __attribute__((section(".noinit")));$' "$SHIM" || \
  why62="${why62:-the records are not in .noinit}"
awk '/^void mmosal_log_failure_info\(/,/^}/' "$SHIM" | tr -d ' \n' | \
  grep -q 'if(info->pc==0){preserved_failure_info.info\[record_num\].pc=(uint32_t)(uintptr_t)__builtin_return_address(0);}' || \
  why62="${why62:-a record keeps pc 0 where mmport.h reads none}"
for k62 in '^CONFIG_ESP_SYSTEM_PANIC_SILENT_REBOOT=y$' '^CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y$' '^CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y$' \
           '^# CONFIG_ESP_COREDUMP_LOGS is not set$'; do
  grep -q "$k62" ../../../sdkconfig.defaults || why62="${why62:-sdkconfig.defaults lacks $k62}"
done
# The dump on its own stack, not the asserting task's; generated sdkconfigs keep old values.
for g62 in ../../../sdkconfig.defaults ../../../sdkconfig.warthog-*; do
  [ -f "$g62" ] && grep -q '^CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y$' "$g62" || continue
  s62=$(sed -n 's/^CONFIG_ESP_COREDUMP_STACK_SIZE=\([0-9][0-9]*\)$/\1/p' "$g62")
  d62=$(sed -n 's/^CONFIG_ESP32_CORE_DUMP_STACK_SIZE=\([0-9][0-9]*\)$/\1/p' "$g62")
  if [ "${s62:-0}" -lt 1792 ] || { [ -n "$d62" ] && [ "$d62" != "$s62" ]; }; then
    why62="${why62:-${g62##*/}: the core dump runs on the asserting stack (CONFIG_ESP_COREDUMP_STACK_SIZE ${s62:-unset}${d62:+, ESP32_CORE_DUMP_STACK_SIZE $d62})}"
  fi
  case "$g62" in *defaults) ;; *)
    grep -q '^# CONFIG_ESP_COREDUMP_LOGS is not set$' "$g62" || why62="${why62:-${g62##*/}: the core-dump writer prints to the console}" ;;
  esac
done
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
{ grep '^#define MAX_FAILURE_RECORDS ' "$SHIM"
  grep '^#define FAST_MOD(' "$SHIM"
  awk '/^struct mmosal_preserved_failure_info$/,/^};/' "$SHIM"
  grep '^#define ASSERT_INFO_MAGIC ' "$SHIM"
  echo 'static struct mmosal_preserved_failure_info preserved_failure_info;'
  grep '^static char s_assert_reason\[' "$SHIM"
  awk '/^uint32_t warthog_assert_records\(/,/^}/' "$SHIM"
  awk '/^void warthog_assert_clear\(void\)/,/^}/' "$SHIM"
  awk '/^static char \*assert_put_\(/,/^}/' "$SHIM"
  awk '/^static char \*assert_put_hex_\(/,/^}/' "$SHIM"
  awk '/^static const char \*assert_reason_\(void\)/,/^}/' "$SHIM"; } > "$T/fn.c"
cat > "$T/t.c" <<'EOF2'
#include <stdint.h>
#include <stdio.h>
#include <string.h>
struct mmosal_failure_info { uint32_t pc, lr, fileid, line, platform_info[4]; };
#include "fn.c"
int main(void)
{
    struct mmosal_failure_info out[4];
    uint32_t kept = 9;
    if (strcmp(assert_reason_(), "MMOSAL_ASSERT") != 0) { printf("no record: %s\n", s_assert_reason); return 0; }
    if (warthog_assert_records(out, 4, &kept) != 0 || kept != 0) { printf("no magic: kept %u\n", kept); return 0; }
    preserved_failure_info.magic = ASSERT_INFO_MAGIC;
    preserved_failure_info.failure_count = 6;
    for (uint32_t i = 0; i < 4; i++) { preserved_failure_info.info[i].line = 100 + i; }
    preserved_failure_info.info[1].pc = 0x4200abcd;
    preserved_failure_info.info[1].fileid = 0xf9d51701;
    preserved_failure_info.info[1].line = 186;
    if (strcmp(assert_reason_(), "MMOSAL_ASSERT pc=0x4200abcd fileid=0xf9d51701 line=186") != 0) {
        printf("newest: %s\n", s_assert_reason); return 0;
    }
    if (warthog_assert_records(out, 4, &kept) != 6 || kept != 4 || out[0].line != 102 || out[1].line != 103 ||
        out[2].line != 100 || out[3].line != 186) { printf("order: kept %u\n", kept); return 0; }
    if (warthog_assert_records(out, 2, &kept) != 6 || kept != 2 || out[0].line != 100 || out[1].line != 186) {
        printf("max 2: kept %u\n", kept); return 0;
    }
    preserved_failure_info.info[1].pc = preserved_failure_info.info[1].fileid = preserved_failure_info.info[1].line = 0xffffffffu;
    size_t n = strlen(assert_reason_());
    if (n + 1 > sizeof(s_assert_reason) || strcmp(s_assert_reason, "MMOSAL_ASSERT pc=0xffffffff fileid=0xffffffff line=4294967295") != 0) {
        printf("longest: %s\n", s_assert_reason); return 0;
    }
    warthog_assert_clear();
    if (warthog_assert_records(out, 4, &kept) != 0 || kept != 0) { printf("cleared: kept %u\n", kept); return 0; }
    printf("ok %u/%u\n", (unsigned)n + 1, (unsigned)sizeof(s_assert_reason));
    return 0;
}
EOF2
run62=""
if ${CC:-cc} -std=gnu11 -w ${SANFLAGS:-} -I"$T" -o "$T/t" "$T/t.c" 2>/dev/null; then
  run62=$("$T/t")
fi
rm -rf "$T"
case "$run62" in ok*) ;; *) why62="${why62:-the reason or the record read: ${run62:-did not build or run}}" ;; esac
if [ -z "$why62" ]; then
  ok "an assert ends in esp_system_abort with nothing printed, slept or shut down on the way; no boot dump; the record is logged first, in .noinit, its pc the call site; the core dump's reason exact and within its buffer (${run62#ok }); AT+ASSERT? reads the records oldest first; silent reboot, ELF core dump on its own stack, no core-dump log, in sdkconfig.defaults and every generated sdkconfig"
else
  bad "assert path: $why62"
fi

# 63. AT+ASSERT?, AT+ASSERT=0 and AT+ASSERTTEST (main/at.c): assert_line_ compiled out and run, every
#     field in its slot and within line[] at its extreme; ? prints the count line, then each kept record
#     numbered from the count; =0 clears; AT+ASSERTTEST=at, =crit and =hang reply, wait, then assert on
#     the AT task (=crit inside a critical section, =hang right after arming the boot hang), each with
#     its own marker; =loop posts through mmwlan_assert_test, and the loop asserts from a timeout
#     MMWLAN_ASSERT_TEST_DELAY_MS later; all dispatched. MMOSAL_FILEID: the CMake recipe and
#     tools/assert_fileid.py hash the same path the same way, for every library that asserts, main/ and
#     components/halow (mmhalow.c), and, derived from the tree, every firmware .c that asserts outside
#     the SDK tree and the shims has a nearest CMakeLists.txt that applies it; AT+COREDUMP? prints the
#     abort's reason; the AT reference and Troubleshooting name the keys and the method, and the two
#     AT rows say what was measured on air on 2026-10-03 (at and loop) and what is not (crit, hang,
#     safe mode).
SHM=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/umac_mmdrv_shim.c
MWH=../../halow/components/mm-iot-sdk/framework/morselib/include/mmwlan_mesh.h
why63=""
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
awk '/^static int assert_line_\(/,/^}/' "$A" > "$T/fn.c"
size63=$(awk '/^static void cmd_assert_query\(void\)/ {on=1} on && /char line\[[0-9]+\];/ {match($0, /\[[0-9]+\]/); print substr($0, RSTART + 1, RLENGTH - 2); exit}' "$A")
cat > "$T/t.c" <<'EOF2'
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct mmosal_failure_info { uint32_t pc, lr, fileid, line, platform_info[4]; };
#include "fn.c"
int main(int argc, char **argv)
{
    unsigned size = (unsigned)atoi(argv[1]);
    char buf[512];
    struct mmosal_failure_info r = { 0x4200abcd, 0x42001234, 0xf9d51701, 186, { 0x7e570002, 6, 7, 8 } };
    assert_line_(buf, sizeof(buf), 3, &r);
    if (strcmp(buf, "+ASSERT: #3 pc=0x4200abcd lr=0x42001234 line=186 fileid=0xf9d51701 "
                    "info=0x7e570002,0x00000006,0x00000007,0x00000008\r\n") != 0) { printf("fields: %s", buf); return 0; }
    memset(&r, 0xff, sizeof(r));
    int n = assert_line_(buf, sizeof(buf), 0xffffffffu, &r);
    (void)argc;
    printf("%s %d/%u\n", n + 1 <= (int)size ? "ok" : "short", n + 1, size);
    return 0;
}
EOF2
run63=""
if [ -n "$size63" ] && ${CC:-cc} -std=gnu11 -w -I"$T" -o "$T/t" "$T/t.c" 2>/dev/null; then
  run63=$("$T/t" "$size63")
fi
rm -rf "$T"
case "$run63" in ok*) ;; *) why63="the record line: ${run63:-did not build or run}" ;; esac
AQ=$(awk '/^static void cmd_assert_query\(void\)/,/^}/' "$A" | tr -d ' \n')
case "$AQ" in *'constuint32_tcount=warthog_assert_records(rec,WARTHOG_ASSERT_RECORDS_MAX,&kept);'*'for(uint32_ti=0;i<kept;i++){assert_line_(line,sizeof(line),count-kept+i,&rec[i]);cdc_write(line);}reply_ok();'*) ;;
  *) why63="${why63:-AT+ASSERT? does not print every kept record numbered from the count}" ;; esac
case "$AQ" in *'reset_reason_name_(esp_reset_reason())'*) ;; *) why63="${why63:-AT+ASSERT? does not name the last reset}" ;; esac
awk '/strcasecmp\(verb, "ASSERT"\) == 0 && terminator == .\?./ {getline; print}' "$A" | grep -q 'cmd_assert_query();' || \
  why63="${why63:-AT+ASSERT? is not dispatched}"
awk '/strcasecmp\(verb, "ASSERT"\) == 0 && terminator == .=./,/strcasecmp\(verb, "ASSERTTEST"\)/' "$A" | tr -d ' \n' | \
  grep -q 'if(strcmp(trim(args),"0")!=0){reply_error("usage:AT+ASSERT=0");}else{warthog_assert_clear();' || \
  why63="${why63:-AT+ASSERT=0 does not clear, or clears on another argument}"
awk '/strcasecmp\(verb, "ASSERTTEST"\) == 0 && terminator == .=./ {getline; print}' "$A" | grep -q 'cmd_asserttest(trim(args));' || \
  why63="${why63:-AT+ASSERTTEST= is not dispatched}"
AT63=$(awk '/^static void cmd_asserttest\(const char \*a\)/,/^}/' "$A")
printf '%s\n' "$AT63" | awk '/mmwlan_assert_test\(\)/ {m=NR} /reply_ok\(\);/ {o[++no]=NR} /vTaskDelay\(pdMS_TO_TICKS\(MMWLAN_ASSERT_TEST_DELAY_MS\)\);/ {d=NR}
     /MMOSAL_TASK_ENTER_CRITICAL\(\);/ {c=NR} /MMOSAL_ASSERT_LOG_DATA\(false, MMWLAN_ASSERT_TEST_CRIT\);/ {x=NR}
     /warthog_boot_arm_hang\(\);/ {h=NR} /MMOSAL_ASSERT_LOG_DATA\(false, MMWLAN_ASSERT_TEST_HANG\);/ {hx=NR}
     /MMOSAL_ASSERT_LOG_DATA\(false, MMWLAN_ASSERT_TEST_AT\);/ {t=NR}
     END {exit (m && no == 2 && m < o[1] && o[2] < d && d < c && c < x && x < h && h + 1 == hx && hx < t) ? 0 : 1}' || \
  why63="${why63:-AT+ASSERTTEST does not reply, wait, then assert (=crit in a critical section, =hang after arming the boot hang, =loop through mmwlan_assert_test)}"
printf '%s\n' "$AT63" | grep -q 'strcasecmp(a, "hang") == 0' || why63="${why63:-AT+ASSERTTEST=hang is not parsed}"
awk '/^static void assert_test_evt_handler\(/,/^}/' "$SHM" | grep -q 'umac_core_register_timeout(umacd, MMWLAN_ASSERT_TEST_DELAY_MS, assert_test_fire_, NULL, NULL)' && \
  awk '/^static void assert_test_fire_\(/,/^}/' "$SHM" | grep -q 'MMOSAL_ASSERT_LOG_DATA(false, MMWLAN_ASSERT_TEST_LOOP);' || \
  why63="${why63:-the loop variant does not assert from a timeout with its marker}"
[ "$(sed -n 's/^#define MMWLAN_ASSERT_TEST_[A-Z]* *\(0x[0-9a-f]*u\)$/\1/p' "$MWH" | sort -u | wc -l | tr -d ' ')" = 4 ] || \
  why63="${why63:-the four AT+ASSERTTEST markers are not distinct}"
FC=../../halow/components/mmosal_fileid.cmake
tr -d ' \n' < "$FC" | grep -q 'file(RELATIVE_PATHrel"${CMAKE_SOURCE_DIR}""${abs}")string(SHA256hash"${rel}")string(SUBSTRING"${hash}"08id)set_property(SOURCE"${abs}"TARGET_DIRECTORY${target}APPENDPROPERTYCOMPILE_DEFINITIONS"MMOSAL_FILEID=0x${id}")' || \
  why63="${why63:-the CMake fileid is not the first 8 hex of SHA-256 of the path from the project root}"
grep -q '^foreach(t libmorse mmhostap shims mmpktmem mmutils)$' ../../halow/components/CMakeLists.txt && \
  grep -q '^warthog_mmosal_fileid(${COMPONENT_LIB} ASSERTING)$' ../../../main/CMakeLists.txt && \
  grep -q '^warthog_mmosal_fileid(${COMPONENT_LIB})$' ../../halow/CMakeLists.txt || \
  why63="${why63:-a library that asserts, main/ or components/halow (mmhalow.c), is built without MMOSAL_FILEID}"
# ASSERTING picks the sources that assert; pioarduino puts main/'s first source's defines on every
# source, so that first source must not assert (else 29 'MMOSAL_FILEID redefined' warnings return).
tr -d ' \n' < "$FC" | grep -q 'if(FID_ASSERTING)file(STRINGS"${abs}"hitsREGEX"MMOSAL_ASSERT|MMOSAL_LOG_FAILURE_INFO")if(NOThits)continue()endif()endif()' || \
  why63="${why63:-ASSERTING does not pick the sources that assert}"
first63=$(awk '/^idf_component_register\(/ {on=1} on && /^        "[^"]*\.c"$/ {gsub(/[ "]/, ""); print; exit}' ../../../main/CMakeLists.txt)
[ -n "$first63" ] && ! grep -Eq 'MMOSAL_ASSERT|MMOSAL_LOG_FAILURE_INFO' "../../../main/$first63" || \
  why63="${why63:-the first main/ source (${first63:-none}) asserts: pioarduino would put its fileid on every main/ source}"
# Derived: a firmware .c that asserts outside the SDK tree and the shims (the foreach) has a nearest
# CMakeLists.txt that applies it.
nf63=$(find ../../../main ../../halow .. -name '*.c' -not -path '*/mm-iot-sdk/*' -not -path '*/shims/*' \
         -not -path '*/test/*' 2>/dev/null | xargs grep -l -E 'MMOSAL_ASSERT|MMOSAL_LOG_FAILURE_INFO' 2>/dev/null | \
       while read -r f63; do
         d63=$(dirname "$f63")
         while [ ! -f "$d63/CMakeLists.txt" ] && [ "$d63" != "." ] && [ "$d63" != "/" ]; do d63=$(dirname "$d63"); done
         grep -Eq '^warthog_mmosal_fileid\(\$\{COMPONENT_LIB\}( ASSERTING)?\)$' "$d63/CMakeLists.txt" 2>/dev/null || echo "$f63" | sed 's#^\(\.\./\)*##'
       done)
[ -z "$nf63" ] || why63="${why63:-built without MMOSAL_FILEID: $(echo $nf63)}"
printf '%s\n' "$(find ../../../main ../../halow -name '*.c' -not -path '*/mm-iot-sdk/*' -not -path '*/shims/*' | \
  xargs grep -l 'MMOSAL_ASSERT' 2>/dev/null)" | grep -q 'mmhalow.c$' || why63="${why63:-the fileid coverage scan no longer sees mmhalow.c}"
grep -q "hashlib.sha256(rel.encode()).hexdigest()\[:8\]" ../../../tools/assert_fileid.py || \
  why63="${why63:-tools/assert_fileid.py hashes otherwise}"
if command -v python3 >/dev/null 2>&1; then
  if command -v sha256sum >/dev/null 2>&1; then H63="sha256sum"; else H63="shasum -a 256"; fi
  for f63 in main/at.c components/halow/components/mm-iot-sdk/framework/morselib/src/umac/umac_mmdrv_shim.c; do
    id63=$(printf '%s' "$f63" | $H63 | cut -c1-8)
    python3 ../../../tools/assert_fileid.py "fileid=0x$id63" | grep -qx "0x$id63 $f63" || \
      why63="${why63:-tools/assert_fileid.py does not name $f63 for 0x$id63}"
  done
fi
awk '/^static void cmd_coredump\(void\)/,/^}/' "$A" | grep -q 'esp_core_dump_get_panic_reason(reason, sizeof(reason)) == ESP_OK' || \
  why63="${why63:-AT+COREDUMP? does not print the abort reason}"
row63=$(grep '^| `AT+ASSERT?`' ../../../wiki/AT-Command-Reference.md)
for k63 in count kept reset up_s crash_boots safe pc lr line fileid info tools/assert_fileid.py addr2line 'AT+ASSERT=0' \
           'Measured on air on 2026-10-03' 'not measured on air'; do
  case "$row63" in *"\`$k63\`"*|*"$k63"*) ;; *) why63="${why63:-the AT+ASSERT? row does not name $k63}" ;; esac
done
row63t=$(grep '^| `AT+ASSERTTEST=' ../../../wiki/AT-Command-Reference.md)
for k63 in '`at`' '`loop`' '`crit`' '`hang`' 0x7e570001 0x7e570002 0x7e570003 0x7e570004 'Measured on air on 2026-10-03' \
           'not measured on air'; do
  case "$row63t" in *"$k63"*) ;; *) why63="${why63:-the AT+ASSERTTEST row does not name $k63}" ;; esac
done
TS63=../../../wiki/Troubleshooting.md
grep -q '^## The board drops off USB$' "$TS63" && grep -q 'AT+ASSERT?' "$TS63" && grep -q 'AT+COREDUMP?' "$TS63" && \
  grep -q 'tools/assert_fileid.py' "$TS63" || why63="${why63:-Troubleshooting does not say how to read the records}"
if [ -z "$why63" ]; then
  ok "AT+ASSERT? prints every kept record in its slot within line[$size63] (${run63#ok }), =0 clears; AT+ASSERTTEST replies, then asserts on the AT task (=crit in a critical section) or on the loop from a timeout; the build and tools/assert_fileid.py hash fileids alike; documented"
else
  bad "AT+ASSERT / AT+ASSERTTEST: $why63"
fi

# 64. A request to the umac event loop that is not posted says why (AT+CHIPRESTART, AT+ASSERTTEST=loop):
#     the shim answers NO_MEM only with the loop running and not stopping (its queue full), else
#     UNAVAILABLE; at.c's text, compiled out and run with mmwlan.h's statuses: NO_MEM "event queue
#     full, try again", every other one "chip not running"; both commands use it; the AT reference
#     names both.
why64=""
LP=$(awk '/^static enum mmwlan_status loop_post_failed_\(/,/^}/' "$SHM" | tr -d ' \n')
case "$LP" in *'return(core->evtloop_task!=NULL&&!core->evtloop_shutting_down)?MMWLAN_NO_MEM:MMWLAN_UNAVAILABLE;'*) ;;
  *) why64="the shim does not tell a full queue from a loop down or stopping" ;; esac
for fn64 in umac_chip_restart_request umac_assert_test_request; do
  awk "/^enum mmwlan_status $fn64\\(/,/^}/" "$SHM" | grep -q 'return umac_core_evt_queue(umacd, &evt) ? MMWLAN_SUCCESS : loop_post_failed_(umacd);' || \
    why64="${why64:-$fn64 does not say why a post failed}"
done
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
{ awk '/^enum mmwlan_status$/,/^};/' ../../halow/components/mm-iot-sdk/framework/morselib/include/mmwlan.h
  awk '/^static const char \*loop_post_error_\(/,/^}/' "$A"; } > "$T/fn.c"
cat > "$T/t.c" <<'EOF2'
#include <stdio.h>
#include <string.h>
#include "fn.c"
int main(void)
{
    if (strcmp(loop_post_error_(MMWLAN_NO_MEM), "event queue full, try again") != 0) { printf("no_mem\n"); return 0; }
    for (int s = MMWLAN_ERROR; s <= MMWLAN_VIF_ERROR; s++) {
        if (s != MMWLAN_NO_MEM && strcmp(loop_post_error_((enum mmwlan_status)s), "chip not running") != 0) {
            printf("status %d: %s\n", s, loop_post_error_((enum mmwlan_status)s)); return 0;
        }
    }
    printf("ok\n");
    return 0;
}
EOF2
run64=""
if ${CC:-cc} -std=gnu11 -w -I"$T" -o "$T/t" "$T/t.c" 2>/dev/null; then
  run64=$("$T/t")
fi
rm -rf "$T"
[ "$run64" = ok ] || why64="${why64:-the AT text: ${run64:-did not build or run}}"
for fn64 in cmd_chiprestart cmd_asserttest; do
  awk "/^static void $fn64\\(/,/^}/" "$A" | grep -q 'snprintf(why, sizeof(why), "%s (%d)", loop_post_error_(st), (int)st);' || \
    why64="${why64:-$fn64 does not say why its request was not posted}"
done
row64=$(grep '^| `AT+CHIPRESTART`' ../../../wiki/AT-Command-Reference.md)
case "$row64" in
  *'+ERR: chip not running (<status>)'*'+ERR: event queue full, try again (<status>)'*) ;;
  *) why64="${why64:-the AT+CHIPRESTART row does not name both errors}" ;; esac
if [ -z "$why64" ]; then
  ok "a request the umac event loop did not take says why: event queue full (NO_MEM) or chip not running (the loop down or stopping, or morselib not up), for AT+CHIPRESTART and AT+ASSERTTEST=loop"
else
  bad "loop requests: $why64"
fi

# 65. A boot that cannot reach USB reboots, and a crash loop stops (main/boot_guard.c, main/main.c,
#     main/halow.c). CONFIG_BOOTLOADER_WDT_DISABLE_IN_USER_CODE=y keeps the bootloader's RTC watchdog
#     armed into app_main (sdkconfig.defaults, and every generated sdkconfig.warthog-* with the flash
#     core dump; the rest predate it); warthog_boot_guard_start(), app_main's first call, re-arms it
#     for WARTHOG_BOOT_WDT_S with a system reset (keeps .noinit and the reset reason; no flashboot) and
#     warthog_boot_guard_usb_up() turns it off right after warthog_usb_net_start() succeeds, or in
#     safe mode; a USB start that fails in a normal boot goes to warthog_boot_guard_usb_failed().
#     boot_guard.c compiled and run against stub IDF headers across simulated boots: a panic or
#     watchdog reset is one more consecutive crash boot (from 0 without the magic, saturating), any
#     other reset clears the count; WARTHOG_BOOT_SAFE_AFTER (3) start safe mode, which keeps
#     counting, clears the boot hang AT+ASSERTTEST=hang arms (also cleared by a count of 0 or a lost
#     magic); a normal boot clears the count after WARTHOG_BOOT_OK_S only once the watchdog is off
#     (a tick past it with the watchdog armed keeps it: the watchdog runs on the RC clock from
#     app_main), safe mode never; a failed USB start leaves the watchdog armed for
#     WARTHOG_BOOT_USB_RETRIES (2) resets that do not count as crash boots (a panic after one does,
#     and so does a watchdog reset with no failed USB start), then turns it off with no USB; a crash
#     reset keeps the spent retries, a clean reset or USB up restores them;
#     a download-mode mark makes only the next boot's watchdog reset no crash boot, names that boot and
#     restores the USB retries; a panic or power-on after the mark counts as without it, and a mark without
#     the magic excuses nothing; the watchdog is armed at
#     exactly WARTHOG_BOOT_WDT_S of slow clock with RESET_SYSTEM and off after USB. app_main: safe mode runs
#     warthog_halow_start_safe() (event loop, netif, link bit; no chip) in place of the HaLow start,
#     the link wait and the bridge; the armed hang stops a normal boot before the HaLow start; the
#     loop ticks the guard. AT+ASSERT? prints crash_boots and safe; AT+COREDUMP=0 erases the dump (any
#     other argument refused); the AT reference and Troubleshooting say so.
BG=../../../main/boot_guard.c
MN=../../../main/main.c
HL=../../../main/halow.c
why65=""
for g65 in ../../../sdkconfig.defaults ../../../sdkconfig.warthog-*; do
  [ -f "$g65" ] || continue
  case "$g65" in *defaults) ;; *) grep -q '^CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y$' "$g65" || continue ;; esac
  grep -q '^CONFIG_BOOTLOADER_WDT_DISABLE_IN_USER_CODE=y$' "$g65" || \
    why65="${why65:-${g65##*/}: IDF turns the boot watchdog off before app_main}"
done
if [ ! -f "$BG" ]; then
  why65="${why65:-main/boot_guard.c is missing}"
else
  grep -q '^static __NOINIT_ATTR struct' "$BG" || why65="${why65:-the crash-boot count is not in .noinit}"
  awk '/^static void boot_wdt_arm_\(/,/^}/' "$BG" | grep -q 'WDT_STAGE_ACTION_RESET_SYSTEM' || \
    why65="${why65:-the boot watchdog does not reset the system}"
  grep -q 'flashboot' "$BG" && why65="${why65:-the boot watchdog touches flashboot}"
  T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
  mkdir -p "$T/hal" "$T/soc"
  cat > "$T/esp_system.h" <<'EOF2'
#pragma once
typedef enum { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_INT_WDT,
               ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO, ESP_RST_USB,
               ESP_RST_JTAG, ESP_RST_EFUSE, ESP_RST_PWR_GLITCH, ESP_RST_CPU_LOCKUP } esp_reset_reason_t;
esp_reset_reason_t esp_reset_reason(void);
EOF2
  printf '#pragma once\n#define __NOINIT_ATTR\n' > "$T/esp_attr.h"
  printf '#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n' > "$T/esp_timer.h"
  printf '#pragma once\n#include <stdint.h>\nuint32_t rtc_clk_slow_freq_get_hz(void);\n' > "$T/soc/rtc.h"
  cat > "$T/hal/wdt_hal.h" <<'EOF2'
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum { WDT_RWDT, WDT_MWDT0, WDT_MWDT1 } wdt_inst_t;
typedef enum { WDT_STAGE0, WDT_STAGE1, WDT_STAGE2, WDT_STAGE3 } wdt_stage_t;
typedef enum { WDT_STAGE_ACTION_OFF, WDT_STAGE_ACTION_INT, WDT_STAGE_ACTION_RESET_CPU,
               WDT_STAGE_ACTION_RESET_SYSTEM, WDT_STAGE_ACTION_RESET_RTC } wdt_stage_action_t;
typedef struct { wdt_inst_t inst; } wdt_hal_context_t;
#define RWDT_HAL_CONTEXT_DEFAULT() { .inst = WDT_RWDT }
void wdt_hal_init(wdt_hal_context_t *h, wdt_inst_t i, uint32_t p, bool e);
void wdt_hal_write_protect_disable(wdt_hal_context_t *h);
void wdt_hal_write_protect_enable(wdt_hal_context_t *h);
void wdt_hal_config_stage(wdt_hal_context_t *h, wdt_stage_t s, uint32_t t, wdt_stage_action_t a);
void wdt_hal_enable(wdt_hal_context_t *h);
void wdt_hal_disable(wdt_hal_context_t *h);
EOF2
  cat > "$T/t.c" <<'EOF2'
#include <stdio.h>
#include <string.h>
#include "boot_guard.c"
static esp_reset_reason_t rr;
static int64_t now_us;
static int wdt_on, wdt_unlocked, wdt_stage, wdt_action;
static uint32_t wdt_ticks;
esp_reset_reason_t esp_reset_reason(void) { return rr; }
int64_t esp_timer_get_time(void) { return now_us; }
uint32_t rtc_clk_slow_freq_get_hz(void) { return 136000; }
void wdt_hal_init(wdt_hal_context_t *h, wdt_inst_t i, uint32_t p, bool e)
{ (void)p; (void)e; h->inst = i; if (i == WDT_RWDT) { wdt_on = 0; wdt_ticks = 0; } }
void wdt_hal_write_protect_disable(wdt_hal_context_t *h) { (void)h; wdt_unlocked = 1; }
void wdt_hal_write_protect_enable(wdt_hal_context_t *h) { (void)h; wdt_unlocked = 0; }
void wdt_hal_config_stage(wdt_hal_context_t *h, wdt_stage_t s, uint32_t t, wdt_stage_action_t a)
{ if (h->inst == WDT_RWDT && wdt_unlocked) { wdt_stage = s; wdt_ticks = t; wdt_action = a; } }
void wdt_hal_enable(wdt_hal_context_t *h) { if (h->inst == WDT_RWDT && wdt_unlocked) wdt_on = 1; }
void wdt_hal_disable(wdt_hal_context_t *h) { if (h->inst == WDT_RWDT && wdt_unlocked) wdt_on = 0; }
/* One boot: RAM but .noinit as a reset leaves it. */
static int boot(esp_reset_reason_t r)
{
    rr = r; now_us = 0; s_safe = false; s_cleared = false; s_wdt_off = false;
    return warthog_boot_guard_start();
}
#define CHECK(c, ...) do { if (!(c)) { printf(__VA_ARGS__); printf("\n"); return 0; } } while (0)
int main(void)
{
    static const esp_reset_reason_t crash[] = { ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT };
    static const esp_reset_reason_t clean[] = { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW,
        ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO, ESP_RST_USB, ESP_RST_JTAG, ESP_RST_EFUSE,
        ESP_RST_PWR_GLITCH, ESP_RST_CPU_LOCKUP };
    for (unsigned i = 0; i < 4; i++) {
        CHECK(warthog_boot_next_count(5, true, crash[i]) == 6, "crash %u counts", i);
        CHECK(warthog_boot_next_count(7, false, crash[i]) == 1, "crash %u without magic", i);
        CHECK(warthog_boot_next_count(255, true, crash[i]) == 255, "crash %u saturates", i);
    }
    for (unsigned i = 0; i < sizeof(clean) / sizeof(clean[0]); i++) {
        CHECK(warthog_boot_next_count(5, true, clean[i]) == 0, "reset %d clears", (int)clean[i]);
    }
    CHECK(WARTHOG_BOOT_SAFE_AFTER == 3, "safe after %u", (unsigned)WARTHOG_BOOT_SAFE_AFTER);
    memset(&s_boot, 0x5a, sizeof(s_boot)); /* power-on RAM */
    CHECK(!boot(ESP_RST_POWERON) && warthog_boot_crash_count() == 0 && !warthog_boot_hang_armed(), "power on");
    CHECK(wdt_on && wdt_action == WDT_STAGE_ACTION_RESET_SYSTEM && wdt_stage == WDT_STAGE0 &&
          wdt_ticks == WARTHOG_BOOT_WDT_S * 136000u && !wdt_unlocked, "watchdog: on %d action %d ticks %u",
          wdt_on, wdt_action, (unsigned)wdt_ticks);
    warthog_boot_guard_usb_up();
    CHECK(!wdt_on && !wdt_unlocked, "watchdog off after USB");
    warthog_boot_arm_hang();
    CHECK(!boot(ESP_RST_PANIC) && warthog_boot_crash_count() == 1 && warthog_boot_hang_armed(), "hang armed");
    CHECK(!boot(ESP_RST_WDT) && warthog_boot_crash_count() == 2 && warthog_boot_hang_armed(), "hang kept");
    CHECK(boot(ESP_RST_WDT) && warthog_boot_safe() && warthog_boot_crash_count() == 3 &&
          !warthog_boot_hang_armed(), "safe mode at 3");
    now_us = (int64_t)WARTHOG_BOOT_OK_S * 1000000;
    warthog_boot_guard_tick();
    CHECK(warthog_boot_crash_count() == 3, "safe mode keeps the count");
    warthog_boot_arm_hang();
    CHECK(boot(ESP_RST_PANIC) && warthog_boot_crash_count() == 4 && !warthog_boot_hang_armed(), "safe mode clears the hang");
    CHECK(!boot(ESP_RST_SW) && warthog_boot_crash_count() == 0 && !warthog_boot_safe(), "software reset leaves safe mode");
    warthog_boot_arm_hang();
    CHECK(!boot(ESP_RST_SW) && !warthog_boot_hang_armed(), "a clean reset clears the hang");
    CHECK(!boot(ESP_RST_PANIC) && warthog_boot_crash_count() == 1, "one crash");
    /* A tick past OK_S while the watchdog runs (it fires on the uncalibrated RC clock, after app_main). */
    now_us = (int64_t)WARTHOG_BOOT_OK_S * 1000000 + 300000;
    warthog_boot_guard_tick();
    CHECK(warthog_boot_crash_count() == 1 && wdt_on, "cleared while the boot watchdog runs");
    CHECK(!boot(ESP_RST_WDT) && warthog_boot_crash_count() == 2, "the watchdog reset after that tick did not count");
    warthog_boot_guard_usb_up();
    now_us = (int64_t)WARTHOG_BOOT_OK_S * 1000000 - 1;
    warthog_boot_guard_tick();
    CHECK(warthog_boot_crash_count() == 2, "cleared early");
    now_us++;
    warthog_boot_guard_tick();
    CHECK(warthog_boot_crash_count() == 0, "not cleared after WARTHOG_BOOT_OK_S with USB up");
    s_boot.magic ^= 1u; s_boot.crash_boots = 2; s_boot.hang = s_boot.magic ^ 1u;
    CHECK(!boot(ESP_RST_PANIC) && warthog_boot_crash_count() == 1 && !warthog_boot_hang_armed(), "lost magic");
    /* A failed USB start: WARTHOG_BOOT_USB_RETRIES watchdog resets that are not crash boots, then no USB. */
    CHECK(WARTHOG_BOOT_USB_RETRIES == 2, "USB retries %u", (unsigned)WARTHOG_BOOT_USB_RETRIES);
    CHECK(warthog_boot_guard_usb_failed() && wdt_on, "a failed USB start does not leave the watchdog to retry");
    now_us = (int64_t)WARTHOG_BOOT_OK_S * 1000000 + 300000;
    warthog_boot_guard_tick();
    CHECK(warthog_boot_crash_count() == 1, "cleared before the USB retry's watchdog reset");
    CHECK(!boot(ESP_RST_WDT) && warthog_boot_crash_count() == 1, "the USB retry counted as a crash boot");
    CHECK(warthog_boot_guard_usb_failed() && wdt_on, "no second USB retry");
    CHECK(!boot(ESP_RST_WDT) && warthog_boot_crash_count() == 1, "the second USB retry counted");
    CHECK(!warthog_boot_guard_usb_failed() && !wdt_on && !wdt_unlocked, "retries spent: the watchdog not off");
    now_us = (int64_t)WARTHOG_BOOT_OK_S * 1000000;
    warthog_boot_guard_tick();
    CHECK(warthog_boot_crash_count() == 0, "a boot running without USB, the watchdog off, kept the count after a minute");
    CHECK(!boot(ESP_RST_PANIC) && !warthog_boot_guard_usb_failed() && !wdt_on, "a crash boot restored the retries");
    CHECK(!boot(ESP_RST_SW) && warthog_boot_guard_usb_failed(), "a clean reset did not restore the retries");
    CHECK(!boot(ESP_RST_PANIC) && warthog_boot_crash_count() == 1, "a panic after a failed USB start not counted");
    CHECK(!boot(ESP_RST_WDT) && warthog_boot_crash_count() == 2, "a watchdog reset with no USB failure not counted");
    /* Download mode nobody used: its RTC watchdog return is no crash boot, once. */
    CHECK(!boot(ESP_RST_SW) && warthog_boot_crash_count() == 0, "a clean reset");
    warthog_boot_mark_download();
    CHECK(!boot(ESP_RST_WDT) && warthog_boot_crash_count() == 0, "a download mode's watchdog return counted");
    CHECK(!boot(ESP_RST_WDT) && warthog_boot_crash_count() == 1, "the download mark outlived its boot");
    warthog_boot_mark_download();
    CHECK(!boot(ESP_RST_POWERON) && warthog_boot_crash_count() == 0, "flashed: a power-on reset");
    CHECK(!boot(ESP_RST_WDT) && warthog_boot_crash_count() == 1, "the mark outlived a flashed boot");
    warthog_boot_mark_download();
    CHECK(!boot(ESP_RST_PANIC) && warthog_boot_crash_count() == 2, "the mark excused a panic");
    CHECK(!boot(ESP_RST_SW) && warthog_boot_crash_count() == 0, "a clean reset again");
    warthog_boot_mark_download();
    CHECK(!boot(ESP_RST_WDT) && warthog_boot_download_return(), "a download return not named");
    CHECK(!boot(ESP_RST_WDT) && !warthog_boot_download_return(), "a later watchdog reset named a download return");
    s_boot.usb_retries = WARTHOG_BOOT_USB_RETRIES;
    warthog_boot_mark_download();
    CHECK(!boot(ESP_RST_WDT) && warthog_boot_guard_usb_failed(), "a download return did not restore the USB retries");
    CHECK(!boot(ESP_RST_SW) && warthog_boot_crash_count() == 0, "a clean reset once more");
    s_boot.magic ^= 1u; s_boot.dl = BOOT_GUARD_MAGIC;
    CHECK(!boot(ESP_RST_WDT) && warthog_boot_crash_count() == 1 && !warthog_boot_download_return(),
          "a mark without the magic excused a watchdog reset");
    printf("ok\n");
    return 0;
}
EOF2
  run65=""
  if ${CC:-cc} -std=gnu11 -w ${SANFLAGS:-} -I"$T" -I../../../main -o "$T/t" "$T/t.c" 2>"$T/err"; then
    run65=$("$T/t")
  else
    run65="build: $(head -3 "$T/err" | tr '\n' ' ')"
  fi
  rm -rf "$T"
  [ "$run65" = ok ] || why65="${why65:-the boot guard: ${run65:-did not run}}"
fi
AM=$(awk '/^void app_main\(void\)/,/^}/' "$MN")
printf '%s\n' "$AM" | awk '/const bool safe = warthog_boot_guard_start\(\);/ {g=NR} /nvs_flash_init\(\)/ && !n {n=NR}
     /if \(safe\) \{/ {s=NR} /ESP_ERROR_CHECK\(warthog_halow_start_safe\(\)\);/ {hs=NR}
     /warthog_boot_hang_armed\(\)/ {h=NR} /ESP_ERROR_CHECK\(warthog_halow_start\(\)\);/ {hn=NR}
     /warthog_halow_wait_link\(/ {w=NR} /const bool usb = warthog_usb_net_start\(\) != NULL;/ {u=NR}
     /if \(usb \|\| safe\) \{/ {uc=NR} /warthog_boot_guard_usb_up\(\);/ {ud=NR}
     /\} else if \(warthog_boot_guard_usb_failed\(\)\) \{/ {uf=NR}
     /!safe && warthog_cfg_get_mesh_bridge\(\)/ {b=NR} /while \(1\) \{/ {l=NR} /warthog_boot_guard_tick\(\);/ {t=NR}
     END {exit (g && g < n && n < s && s < hs && hs < h && h < hn && hn < w && w < u && uc == u + 1 && ud > uc && ud <= uc + 2 && uf == ud + 1 && u < b && b < l && l < t) ? 0 : 1}' || \
  why65="${why65:-app_main does not count the boot first, start safe mode without the chip, stop an armed hang before the HaLow start, end the watchdog right after USB starts (or in safe mode), report a failed USB start to the guard, keep the bridge out of safe mode, or tick the guard}"
grep -q '"boot_guard.c"' ../../../main/CMakeLists.txt || why65="${why65:-main/boot_guard.c is not built}"
HS=$(awk '/^esp_err_t warthog_halow_start_safe\(void\)/,/^}/' "$HL")
case "$HS" in *'halow_base_start_()'*'xEventGroupSetBits(s_halow_events, HALOW_LINK_BIT);'*) ;; *) why65="${why65:-safe mode does not start the event loop and netif, or leaves the link bit clear}" ;; esac
case "$HS" in *mmhalow_*|*mmwlan_*|*warthog_mesh_*) why65="${why65:-safe mode starts the chip}" ;; esac
awk '/^esp_err_t warthog_halow_start\(void\)/,/^}/' "$HL" | grep -q 'halow_base_start_()' || \
  why65="${why65:-warthog_halow_start does not share the base start}"
awk '/^static void cmd_assert_query\(void\)/,/^}/' "$A" | tr -d ' \n' | \
  grep -q 'crash_boots=%lusafe=%u\\r\\n",.*(unsignedlong)warthog_boot_crash_count(),(unsigned)warthog_boot_safe());' || \
  why65="${why65:-AT+ASSERT? does not print crash_boots and safe}"
awk '/strcasecmp\(verb, "COREDUMP"\) == 0 && terminator == .=./,/strcasecmp\(verb, "ASSERT"\)/' "$A" | tr -d ' \n' | \
  grep -q 'if(strcmp(trim(args),"0")!=0){reply_error("usage:AT+COREDUMP=0");}else{cmd_coredump_erase();}' || \
  why65="${why65:-AT+COREDUMP=0 is not dispatched, or erases on another argument}"
awk '/^static void cmd_coredump_erase\(void\)/,/^}/' "$A" | grep -q 'esp_core_dump_image_erase()' || \
  why65="${why65:-AT+COREDUMP=0 does not erase the dump}"
grep '^| `AT+COREDUMP?`' ../../../wiki/AT-Command-Reference.md | grep -q 'AT+COREDUMP=0' || \
  why65="${why65:-the AT reference does not name AT+COREDUMP=0}"
for k65 in 'safe mode' 'AT+COREDUMP=0' 'crash_boots' 'reset=WDT' "$(sed -n 's/^#define WARTHOG_BOOT_WDT_S \([0-9]*\)u.*/\1/p' ../../../main/boot_guard.h 2>/dev/null) s"; do
  grep -q "$k65" "$TS63" || why65="${why65:-Troubleshooting does not name $k65}"
done
if [ -z "$why65" ]; then
  ok "the boot watchdog stays armed into app_main, re-armed with a system reset until USB starts; three crash boots in a row start safe mode (no HaLow start, no hang), counted in .noinit and cleared by a clean reset or a minute up; AT+ASSERT? shows it, AT+COREDUMP=0 erases the dump; a download mode's watchdog return, once, is no crash boot; documented"
else
  bad "boot guard: $why65"
fi

# 66. The chip's GPIO interrupts and RESET_N across a CPU-only reset (esp_restart_noos keeps the GPIO
#     matrix, its interrupt types and output levels): warthog_chip_hold_reset() (shims/mmhal_os.c)
#     drives RESET_N and WAKE low as outputs and sets every pin the shims give an ISR to no interrupt,
#     installing nothing; mmhal_init calls it before gpio_install_isr_service, and so do safe mode
#     (warthog_halow_start_safe) and the AT+ASSERTTEST=hang stop before their own work, so a chip a
#     panic left out of reset is held there (a chip held in reset holds SPI_IRQ low: a level
#     interrupt storm, then INT_WDT; measured on air 2026-10-03). Across main/ and components/
#     (tests and managed components aside) gpio_install_isr_service and gpio_isr_register appear
#     only after warthog_chip_hold_reset() in the same function, and a file outside the shims that
#     attaches a GPIO ISR sets no level interrupt type. SPI_IRQ and BUSY are disabled in
#     mmhal_wlan_deinit before RESET_N goes low. Task stacks with a printf in their failure path:
#     health and spi_irq at 1024 words or more; AT+STACKS? names every task morselib creates and
#     reads those through the shim, which keeps each name's least free stack as an instance exits
#     (a chip restart ends drv, spi_irq and health) and reads a live one only under the lock that
#     exit takes; the table compiled and run on stubs.
SH66=../../halow/components/shims
why66=""
HO66=$(awk '/^void warthog_chip_hold_reset\(void\)/,/^}/' "$SH66/mmhal_os.c")
[ -n "$HO66" ] || why66="no warthog_chip_hold_reset() in shims/mmhal_os.c"
for pin66 in $(sed -n 's/.*gpio_isr_handler_add(\(CONFIG_[A-Z_]*\),.*/\1/p' "$SH66"/*.c | sort -u); do
  printf '%s\n' "$HO66" | grep -qF "gpio_set_intr_type($pin66, GPIO_INTR_DISABLE);" || \
    why66="${why66:-warthog_chip_hold_reset does not disable the interrupt of $pin66}"
done
[ -n "$(sed -n 's/.*gpio_isr_handler_add(\(CONFIG_[A-Z_]*\),.*/\1/p' "$SH66"/*.c)" ] || why66="${why66:-no ISR pins found in the shims}"
ho66=$(printf '%s\n' "$HO66" | tr -d ' \n')
case "$ho66" in *'io_conf.mode=GPIO_MODE_OUTPUT;'*'(1ull<<CONFIG_MM_RESET_N)|(1ull<<CONFIG_MM_WAKE)'*'gpio_set_level(CONFIG_MM_RESET_N,0);gpio_set_level(CONFIG_MM_WAKE,0);gpio_config(&io_conf);'*) ;;
  *) why66="${why66:-warthog_chip_hold_reset does not drive RESET_N and WAKE low as outputs}" ;; esac
case "$ho66" in *gpio_install_isr_service*|*gpio_isr_register*|*gpio_isr_handler_add*) why66="${why66:-warthog_chip_hold_reset installs an ISR}" ;; esac
grep -q '^void warthog_chip_hold_reset(void);' "$SH66/warthog_shim.h" 2>/dev/null || why66="${why66:-warthog_shim.h does not declare warthog_chip_hold_reset}"
awk '/^void mmhal_init\(void\)/,/^}/' "$SH66/mmhal_os.c" | awk '/warthog_chip_hold_reset\(\);/ && !h {h=NR}
    /gpio_install_isr_service\(0\);/ {i=NR} END {exit (h && i && h < i) ? 0 : 1}' || \
  why66="${why66:-mmhal_init does not hold the chip in reset before installing the ISR service}"
awk '/^esp_err_t warthog_halow_start_safe\(void\)/,/^}/' ../../../main/halow.c | awk 'NR > 1 && /[a-z_]+\(/ && !f {f=$0} END {exit f ~ /warthog_chip_hold_reset\(\);/ ? 0 : 1}' || \
  why66="${why66:-safe mode does not hold the chip in reset first}"
awk '/^void app_main\(void\)/,/^}/' ../../../main/main.c | awk '/if \(warthog_boot_hang_armed\(\)\) \{/ {a=NR}
    a && /warthog_chip_hold_reset\(\);/ && !h {h=NR} a && /for \(;;\) \{/ && !l {l=NR} END {exit (a && h > a && l > h) ? 0 : 1}' || \
  why66="${why66:-the AT+ASSERTTEST=hang stop does not hold the chip in reset before it spins}"
for f66 in $(grep -rlE 'gpio_install_isr_service\(|gpio_isr_register\(|gpio_isr_handler_add\(' ../../../main ../.. --include='*.c' 2>/dev/null | \
             grep -v -e '/test/' -e '/managed_components/' -e '/platforms/'); do
  awk '/^[A-Za-z_][A-Za-z0-9_ *]*\(/ {h=0} /warthog_chip_hold_reset\(\);/ {h=1}
       /^[^\/]*(gpio_install_isr_service|gpio_isr_register)\(/ && !h {bad=1} END {exit bad ? 1 : 0}' "$f66" || \
    why66="${why66:-${f66#../../../} installs a GPIO ISR without holding the chip in reset first}"
  case "$f66" in "$SH66"/*) ;; *)
    grep -qE 'GPIO_INTR_(LOW|HIGH)_LEVEL' "$f66" && why66="${why66:-${f66#../../../} attaches a GPIO ISR with a level interrupt}" ;; esac
done
awk '/^void mmhal_wlan_deinit\(void\)/,/^}/' "$SH66/mmhal_wlan.c" | awk '/gpio_set_intr_type\(CONFIG_MM_SPI_IRQ, GPIO_INTR_DISABLE\);/ && !s {s=NR}
    /gpio_set_intr_type\(CONFIG_MM_BUSY, GPIO_INTR_DISABLE\);/ && !b {b=NR} /gpio_set_level\(CONFIG_MM_RESET_N, 0\);/ && !r {r=NR}
    END {exit (s && b && r && s < r && b < r) ? 0 : 1}' || \
  why66="${why66:-mmhal_wlan_deinit lowers RESET_N before the SPI_IRQ and BUSY interrupts are off}"
DRV66=../../halow/components/mm-iot-sdk/framework/morselib/src/driver
hs66=$(sed -n 's/^#define HEALTH_CHECK_TASK_STACK_SIZE_WORDS \([0-9]*\).*/\1/p' "$DRV66/health/driver_health.c")
si66=$(sed -n 's/^#define SPI_IRQ_TASK_STACK *(\([0-9]*\)).*/\1/p' "$DRV66/transport/sdio.c")
[ "${hs66:-0}" -ge 1024 ] && [ "${si66:-0}" -ge 1024 ] || why66="${why66:-health ($hs66) or spi_irq ($si66) under 1024 words}"
ST66=$(awk '/^static void cmd_stacks\(void\)/,/^}/' "$A")
n66=0
for t66 in $(grep -rhoE 'mmosal_task_create\([^;]*"[a-z_]+"\)' ../../halow/components/mm-iot-sdk/framework/morselib/src 2>/dev/null | \
             sed -n 's/.*"\([a-z_]*\)")$/\1/p'; \
           grep -rh -A5 'mmosal_task_create(' "$DRV66" ../../halow/components/mm-iot-sdk/framework/morselib/src/umac/core | \
             sed -n 's/^ *"\([a-z_]*\)");$/\1/p'); do
  n66=$((n66 + 1))
  printf '%s\n' "$ST66" | grep -q "\"$t66\"" || why66="${why66:-AT+STACKS? does not name the morselib task $t66}"
done
[ "$n66" -ge 4 ] || why66="${why66:-only $n66 morselib tasks found (evtloop, drv, spi_irq, health expected)}"
printf '%s\n' "$ST66" | grep -q 'uxTaskGetStackHighWaterMark(h)' || why66="${why66:-AT+STACKS? does not read the high-water mark}"
# A chip restart ends and restarts drv, spi_irq and health: the shim keeps each name's least free
# stack as an instance exits and reads a live one only under the lock its exit takes (no freed TCB).
OS66="$SH66/mmosal_shim_freertos_esp32.c"
awk '/^void mmosal_task_main\(void \*arg\)/,/^}/' "$OS66" | tr -d ' \n' | \
  grep -q 'task_stack_enter_();task_arg.task_fn(task_arg.task_fn_arg);task_stack_exit_();mmosal_task_delete(NULL);' || \
  why66="${why66:-mmosal_task_main does not record the stack of its task as it starts and exits}"
grep -q '^bool warthog_task_stack(const char \*name, uint32_t \*live, uint32_t \*exit_min);' "$SH66/warthog_shim.h" 2>/dev/null || \
  why66="${why66:-warthog_shim.h does not declare warthog_task_stack}"
printf '%s\n' "$ST66" | tr -d ' \n' | grep -q 'warthog_task_stack(shim_names\[i\],&live,&min)' || \
  why66="${why66:-AT+STACKS? does not read the morselib tasks through the shim}"
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
awk '/^#define WARTHOG_TASK_SLOTS/ {on=1} on {print} on && /^bool warthog_task_stack\(/ {f=1} f && /^}/ {exit}' "$OS66" > "$T/fn.c"
cat > "$T/t.c" <<'EOF2'
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef void *TaskHandle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
static int crit, bad_read;
#define portENTER_CRITICAL(m) ((void)(m), crit++)
#define portEXIT_CRITICAL(m) ((void)(m), crit--)
static intptr_t cur;
static const char *cur_name;
static uint32_t hwm[16];
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (TaskHandle_t)cur; }
char *pcTaskGetName(TaskHandle_t h) { return h == NULL ? (char *)cur_name : (char *)"?"; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t h)
{
    if (h != NULL && (intptr_t)h != cur && crit != 1) { bad_read = 1; }
    return hwm[h == NULL ? cur : (intptr_t)h];
}
#include "fn.c"
static void run_(intptr_t h, const char *name, uint32_t at_start) { cur = h; cur_name = name; hwm[h] = at_start; task_stack_enter_(); }
static void exit_(intptr_t h, uint32_t at_exit) { cur = h; hwm[h] = at_exit; task_stack_exit_(); cur = 0; }
#define CHECK(c, ...) do { if (!(c)) { printf(__VA_ARGS__); printf("\n"); return 0; } } while (0)
int main(void)
{
    uint32_t live = 0, min = 0;
    CHECK(!warthog_task_stack("health", &live, &min), "a task never run is found");
    run_(1, "health", 3000);
    cur = 9;
    CHECK(warthog_task_stack("health", &live, &min) && live == 3000u && min == UINT32_MAX, "first instance: %u %u", (unsigned)live, (unsigned)min);
    exit_(1, 1200);
    CHECK(warthog_task_stack("health", &live, &min) && live == UINT32_MAX && min == 1200u, "exited: %u %u", (unsigned)live, (unsigned)min);
    run_(2, "health", 3900);
    cur = 9;
    CHECK(warthog_task_stack("health", &live, &min) && live == 3900u && min == 1200u, "restarted: %u %u", (unsigned)live, (unsigned)min);
    exit_(2, 3500);
    run_(3, "health", 3950);
    cur = 9;
    CHECK(warthog_task_stack("health", &live, &min) && live == 3950u && min == 1200u, "the least kept: %u", (unsigned)min);
    run_(4, "drv", 2000);
    cur = 9;
    CHECK(warthog_task_stack("drv", &live, &min) && live == 2000u && warthog_task_stack("health", &live, &min) && live == 3950u, "two names");
    static const char *more[] = { "a", "b", "c", "d", "e", "f", "g" };
    for (unsigned i = 0; i < 7; i++) { run_(5 + (intptr_t)i, more[i], 100); }
    cur = 9;
    CHECK(!warthog_task_stack("g", &live, &min) && warthog_task_stack("f", &live, &min), "slots: 8 names kept, the 9th not");
    CHECK(crit == 0 && !bad_read, "a live stack read outside the lock (%d %d)", crit, bad_read);
    printf("ok\n");
    return 0;
}
EOF2
run66=""
if ${CC:-cc} -std=gnu11 -w ${SANFLAGS:-} -I"$T" -o "$T/t" "$T/t.c" 2>"$T/err"; then run66=$("$T/t"); else run66="build: $(head -2 "$T/err" | tr '\n' ' ')"; fi
rm -rf "$T"
[ "$run66" = ok ] || why66="${why66:-the task stack table: ${run66:-did not run}}"
awk '/strcasecmp\(verb, "STACKS"\) == 0 && terminator == .\?./ {getline; print}' "$A" | grep -q 'cmd_stacks();' || \
  why66="${why66:-AT+STACKS? is not dispatched}"
grep -q '^| `AT+STACKS?`' ../../../wiki/AT-Command-Reference.md || why66="${why66:-no AT+STACKS? row in the AT reference}"
if [ -z "$why66" ]; then
  ok "the chip held in reset with its GPIO interrupts off before any ISR service attaches (mmhal_init, safe mode, the hang stop) and before mmhal_wlan_deinit lowers RESET_N; no level-interrupt ISR elsewhere; health and spi_irq stacks 1024 words; AT+STACKS? reads every morselib task, the shim keeping each least free stack across chip restarts"
else
  bad "GPIO interrupt reset / task stacks: $why66"
fi

# 67. AT+RXCAP and AT+TXCAP (mmwlan_cap.h, umac_mesh_cap.c, skbq.c): the module is in the firmware build
#     and freestanding; each hook in skbq.c runs only behind its mode (a load and a branch when off):
#     RX in morse_skbq_process_rx after the rx_status is read and before mmdrv_host_process_rx_frame,
#     TX in morse_skbq_tx under the queue lock right after the packet id is set (the bytes the chip
#     will read), only for a frame the queue took; TX status in __skbq_data_tx_finish, and a frame
#     released there untried marked so before its header goes; writers never wait on the ring; a
#     status that finds it busy is counted (st_lost, on the AT+TXCAP? line); the AT argument parser
#     compiled out of main/at.c and run; dispatched; in the AT reference.
SQ67=../../halow/components/mm-iot-sdk/framework/morselib/src/driver/morse_driver/skbq.c
CM67=../../halow/components/mm-iot-sdk/framework/morselib/src/umac/mesh/umac_mesh_cap.c
why67=""
grep -q '/src/umac/mesh/umac_mesh_cap.c' ../../halow/components/morselib/CMakeLists.txt || why67="umac_mesh_cap.c is not in the firmware build"
${CC:-cc} -std=gnu11 -Wall -Werror -fsyntax-only -I../../halow/components/mm-iot-sdk/framework/morselib/include "$CM67" 2>/dev/null || \
  why67="${why67:-umac_mesh_cap.c is not freestanding}"
RX67=$(awk '/^void morse_skbq_process_rx\(/,/^}/' "$SQ67" | tr -d ' \n')
case "$RX67" in *'rx_metadata->vif_id=MORSE_RX_STATUS_FLAGS_VIF_ID_GET(hdr->rx_status.flags);if(mmwlan_cap_mode[MMWLAN_CAP_RX]!=MMWLAN_CAP_OFF){'*'mmwlan_cap_rx(mmpkt_get_data_start(view),mmpkt_get_data_length(view),'*'}mmpkt_close(&view);mmdrv_host_process_rx_frame(mmpkt,channel);'*) ;;
  *) why67="${why67:-the RX capture is not behind its mode, after the rx_status and before the frame goes up}" ;; esac
TX67=$(awk '/^static int morse_skbq_tx\(/,/^}/' "$SQ67" | tr -d ' \n')
case "$TX67" in *'__morse_skbq_pkt_id(mq,mmpkt);if(mmwlan_cap_mode[MMWLAN_CAP_TX]!=MMWLAN_CAP_OFF&&rc==0&&'*'skbq_cap_tx_(mmpkt);}spin_unlock(&mq->lock);'*) ;;
  *) why67="${why67:-the TX capture is not behind its mode, of a frame queued, under the queue lock right after the packet id}" ;; esac
TS67=$(awk '/^static int __skbq_data_tx_finish\(struct mmpkt_list \*skbq,$/ {n++} n == 2 && /^{/ {on=1} on {print} on && /^}/ {exit}' "$SQ67" | tr -d ' \n')
case "$TS67" in *'if(mmwlan_cap_mode[MMWLAN_CAP_TX]!=MMWLAN_CAP_OFF){'*'mmwlan_cap_tx_status(tx_sts->pkt_id,tx_sts->tid,'*) ;;
  *) why67="${why67:-the TX status is not recorded behind the TX mode}" ;; esac
case "$TS67" in *'elseif(tx_metadata->mesh.host_frag!=0||tx_metadata->mesh.ba_wait!=0){'*'if(mmwlan_cap_mode[MMWLAN_CAP_TX]!=MMWLAN_CAP_OFF){'*'mmwlan_cap_tx_untried(h->tx_info.pkt_id,h->tx_info.tid);}morse_skb_remove_padding_after_sent_to_chip(view);'*) ;;
  *) why67="${why67:-a frame released untried is not recorded as such, behind the TX mode, before its header goes}" ;; esac
awk '/^static void cap_tx_done_\(/,/^}/' "$CM67" | tr -d ' \n' | \
  grep -q 'if(!cap_trylock_(r)){__atomic_fetch_add(&r->st_lost,1u,__ATOMIC_RELAXED);return;}' || \
  why67="${why67:-a TX status that finds the ring busy is not counted}"
awk '/^static void cmd_cap_query\(unsigned dir\)/,/^}/' "$A" | tr -d ' \n' | grep -q 'if(dir==MMWLAN_CAP_TX){w+=snprintf(line+w,sizeof(line)-(size_t)w,"st_lost=%lu",(unsignedlong)mmwlan_cap_st_lost());}' || \
  why67="${why67:-AT+TXCAP? does not print st_lost}"
grep -q 'while (!cap_trylock_' "$CM67" && why67="${why67:-a capture writer or the AT task spins on the ring}"
T=$(mktemp -d "${TMPDIR:-/tmp}/glueguard.XXXXXX")
awk '/^static bool cap_args_parse_\(/,/^}/' "$A" > "$T/fn.c"
cat > "$T/t.c" <<'EOF2'
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "fn.c"
int main(void)
{
    uint32_t m = 9; bool h = true; uint8_t mac[6];
    if (!cap_args_parse_("1", 1, &m, &h, mac) || m != 1 || h) { printf("plain\n"); return 0; }
    if (!cap_args_parse_("2,02:00:00:00:00:0A", 2, &m, &h, mac) || m != 2 || !h || mac[5] != 0x0a || mac[0] != 2) { printf("colons\n"); return 0; }
    if (!cap_args_parse_("0,0200000000ff", 1, &m, &h, mac) || m != 0 || mac[5] != 0xff) { printf("bare\n"); return 0; }
    static const char *bad[] = { "2", "", "1,", "1,02:00:00:00:00", "1,02:00:00:00:00:0a:", "x", "11", "1,0200000000fg" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (cap_args_parse_(bad[i], 1, &m, &h, mac)) { printf("took %s\n", bad[i]); return 0; }
    }
    printf("ok\n");
    return 0;
}
EOF2
run67=""
if ${CC:-cc} -std=gnu11 -w ${SANFLAGS:-} -I"$T" -o "$T/t" "$T/t.c" 2>/dev/null; then run67=$("$T/t"); fi
rm -rf "$T"
[ "$run67" = ok ] || why67="${why67:-the AT+RXCAP/TXCAP parser: ${run67:-did not build or run}}"
for v67 in RXCAP TXCAP; do
  awk -v v="$v67" '$0 ~ "strcasecmp\\(verb, \"" v "\"\\) == 0 && terminator == .=." {getline; print}' "$A" | grep -q "cmd_cap_set(MMWLAN_CAP_${v67%CAP}, args);" || \
    why67="${why67:-AT+$v67= is not dispatched}"
  awk -v v="$v67" '$0 ~ "strcasecmp\\(verb, \"" v "\"\\) == 0 && terminator == .\\?." {getline; print}' "$A" | grep -q "cmd_cap_query(MMWLAN_CAP_${v67%CAP});" || \
    why67="${why67:-AT+$v67? is not dispatched}"
  grep -q "^| \`AT+$v67=" ../../../wiki/AT-Command-Reference.md || why67="${why67:-no AT+$v67 row in the AT reference}"
done
if [ -z "$why67" ]; then
  ok "AT+RXCAP/AT+TXCAP: built and freestanding; each hook behind its mode, where the chip's bytes are (RX before anything parses them, TX under the queue lock after the packet id, its status on it); writers never wait; parser run; dispatched; documented"
else
  bad "frame capture: $why67"
fi

# 68. AT+TIDPARAMS as the simulator cannot see it: both unicast data sites take the reorder size
#     from umac_datapath_tx_reorder_size, which keeps morselib's rule unless the switch is on, the
#     frame is mesh, and AT+HOSTFRAG is in force or AT+AMPDU=0; a fragment's flags are re-populated
#     from the connection after they are set; AT+AMPDU? prints each peer's sessions; AT+TIDPARAMS and
#     AT+SEALFIT dispatched, RAM only.
why68=""
UD68=$(cat "$DD/umac_datapath.c")
[ "$(printf '%s\n' "$UD68" | grep -c 'tid_max_reorder_buf_size = umac_datapath_tx_reorder_size(data, stad,')" = 2 ] && \
  ! printf '%s\n' "$UD68" | grep -q 'tid_max_reorder_buf_size = umac_ba_get_reorder_buffer_size' || \
  why68="reorder sizes do not all come from umac_datapath_tx_reorder_size"
awk '/^static uint8_t umac_datapath_tx_reorder_size\(/,/^}/' "$DD/umac_datapath.c" | tr -d ' \n' | \
  grep -q 'if(data->ops==&datapath_ops_mesh&&g_warthog_ba_txparm!=0u&&(umac_datapath_mesh_hostfrag_mode()!=UMAC_MESH_FRAG_OFF||g_warthog_ampdu==0u)){returnumac_ba_get_originator_buffer_size(stad,tid);}returnumac_ba_get_reorder_buffer_size(stad,tid);' || \
  why68="${why68:-umac_datapath_tx_reorder_size changes the morselib rule outside AT+TIDPARAMS=1 with AT+HOSTFRAG in force or AT+AMPDU=0}"
awk '/^static int umac_datapath_tx_mesh_frags\(/,/^}/' "$DD/umac_datapath.c" | tr -d ' \n' | \
  grep -q 'md->flags=(key_id>=0&&!host_seal)?MMDRV_TX_FLAG_HW_ENC:0u;umac_connection_populate_tx_metadata(umacd,md);' || \
  why68="${why68:-the flags of a fragment are not re-populated from the connection}"
awk '/^static void cmd_ampdu_query\(void\)/,/^}/' "$A" | tr -d ' \n' | \
  grep -q 'if((g_warthog_ampdu_peer_mac\[i\]&0x1000000u)!=0u){ampdu_peer_line_(line,sizeof(line),g_warthog_ampdu_peer_mac\[i\],g_warthog_ampdu_peer_ba\[i\]);cdc_write(line);}' || \
  why68="${why68:-AT+AMPDU? does not print the sessions of each peer}"
for v68 in TIDPARAMS SEALFIT; do
  grep -q "strcasecmp(verb, \"$v68\") == 0 && terminator == '='" "$A" && grep -q "strcasecmp(verb, \"$v68\") == 0 && terminator == '?'" "$A" || \
    why68="${why68:-AT+$v68 is not dispatched}"
  grep -q "^| \`AT+$v68=" ../../../wiki/AT-Command-Reference.md || why68="${why68:-no AT+$v68 row in the AT reference}"
done
grep -qi 'nvs.*sealfit\|nvs.*tidparams' ../../../main/cfg.c && why68="${why68:-AT+SEALFIT or AT+TIDPARAMS persists}"
if [ -z "$why68" ]; then
  ok "AT+TIDPARAMS changes the reorder size only on the mesh with AT+HOSTFRAG in force or AT+AMPDU=0; fragments keep the connection's flags; AT+AMPDU? lists each peer's sessions; AT+TIDPARAMS and AT+SEALFIT dispatched, RAM only"
else
  bad "Block Ack fields glue: $why68"
fi

# 69. The fragment rule of chip firmware 1.17.6 (measured on air 2026-10-03), as the simulator cannot
#     see it: AT+HOSTFRAG is off with host CCMP and cuts in at most 2 otherwise, unless
#     WARTHOG_MESH_HOSTFRAG_ANY, which no env sets and the host tests' ANY builds alone define;
#     at.c's rule names the same three cases; the plan asks for that cap and, when the rate is what
#     needs more, recuts once on rates that need no more; AT+SEALFIT runs on every sealed unicast
#     mesh frame the host does not cut (whole when host-sealed, or chip-sealed under this node's
#     requested or agreed Block Ack session on its TID or before the DELBA a cut sent the peer is
#     through, counted seal_ba; else, and when AT+FRAG leaves no rate whole, at most the cap when
#     chip-sealed; AT+FRAG counted) after its rate table and before its PN count, and on mesh group
#     frames (whole, no threshold) after theirs; a head the trim puts in keeps the old head's RTS;
#     the pool reserve is one extra fragment per peer and a DELBA; the LED task's stack is 3072 bytes.
why69=""
PV=$(awk '/^static inline uint32_t umac_datapath_mesh_hostfrag_mode\(void\)/,/^}/' "$DD/umac_datapath_private.h" | tr -d ' \n')
case "$PV" in *'#ifdefined(WARTHOG_MESH_HOST_CCMP)&&!defined(WARTHOG_MESH_HOSTFRAG_ANY)returnUMAC_MESH_FRAG_OFF;#elsereturng_warthog_hostfrag;#endif}') ;;
  *) why69="the mode in force is not off with host CCMP" ;; esac
tr -d ' \n' < "$DD/umac_datapath_private.h" | \
  grep -q '#ifdefWARTHOG_MESH_HOSTFRAG_ANY#defineUMAC_DATAPATH_MESH_FRAG_MAXUMAC_MESH_FRAG_MAX#else#defineUMAC_DATAPATH_MESH_FRAG_MAXUMAC_MESH_FRAG_CHIP_MAX#endif' || \
  why69="${why69:-the cap is not what the chip delivers}"
grep -q '^#define UMAC_MESH_FRAG_CHIP_MAX 2u$' ../../halow/components/mm-iot-sdk/framework/morselib/src/umac/mesh/umac_mesh_frag.h || \
  why69="${why69:-UMAC_MESH_FRAG_CHIP_MAX is not 2}"
grep -n 'WARTHOG_MESH_HOSTFRAG_ANY' ../../../platformio.ini ../../../main/CMakeLists.txt ../../halow/components/morselib/CMakeLists.txt 2>/dev/null | \
  grep -v ':[[:space:]]*;' | grep -q . && why69="${why69:-a firmware build sets WARTHOG_MESH_HOSTFRAG_ANY}"
[ "$(grep -c 'HOSTFRAG_ANY)' Makefile)" = 4 ] || why69="${why69:-other than the 4 ANY host builds define WARTHOG_MESH_HOSTFRAG_ANY}"
RU=$(awk '/^static uint32_t hostfrag_rule_\(void\)/,/^}/' "$A" | tr -d ' \n')
case "$RU" in *'#ifdefined(WARTHOG_MESH_HOSTFRAG_ANY)return16u;#elifdefined(WARTHOG_MESH_HOST_CCMP)return0u;#elsereturn2u;#endif}') ;;
  *) why69="${why69:-the rule AT+HOSTFRAG? prints does not name the three cases}" ;; esac
PL69=$(awk '/^static void umac_datapath_mesh_frag_plan\(/ {on=1} on && /^}/ {exit} on' "$DD/umac_datapath.c")
printf '%s\n' "$PL69" | grep -q '\.max_frags = UMAC_DATAPATH_MESH_FRAG_MAX,' || why69="${why69:-the plan does not ask for the cap}"
printf '%s\n' "$PL69" | tr -d ' \n' | \
  grep -q 'if(plan->n==0u&&plan->lim==UMAC_MESH_FRAG_LIM_RATE){.*umac_datapath_cap_rates(&md->rc_data,hdr_len+sec_len+DOT11_FCS_FIELD_LEN,body_len,plan->thr,req.max_frags);if(r>0){.*req.rate_cap=umac_datapath_chain_cap(&md->rc_data,2u,&slow);umac_mesh_frag_plan(&req,plan);}}' || \
  why69="${why69:-a frame the rate would cut beyond the cap is not recut on rates that need no more}"
[ "$(grep -c 'umac_datapath_seal_fit(' "$DD/umac_datapath.c")" = 2 ] || why69="${why69:-AT+SEALFIT has other than one call site}"
awk '/^enum mmwlan_status umac_datapath_process_tx_frame\(/ {on=1} on && /^}/ {exit} on' "$DD/umac_datapath.c" | \
  awk '/umac_rc_init_rate_table_data\(stad,/ && !r {r=NR} /if \(key_id >= 0 && data->ops == &datapath_ops_mesh && g_warthog_sealfit != 0u\)/ && !f {f=NR}
       /const bool ba = !host_encrypted && \(umac_ba_originator_held\(stad, \(uint8_t\)tid\) \|\|$/ && !b {b=NR}
       /^ *umac_datapath_mesh_ba_waiting\(stad\)\);$/ && b && NR == b + 1 {w=NR}
       /umac_datapath_seal_fit\(umacd, &tx_metadata->rc_data, over,/ && !c {c=NR} /host_encrypted \? 1u : UMAC_MESH_FRAG_CHIP_MAX, ba\);/ && !m {m=NR}
       /umac_datapath_chip_pns\(umacd, &tx_metadata->rc_data,/ && !p {p=NR} /mmdrv_tx_frame\(txbuf, false\)/ && !h {h=NR}
       END {exit (r && f && b && w && c && m && p && h && r < f && f < b && b < c && c < m && m < p && p < h) ? 0 : 1}' || \
  why69="${why69:-AT+SEALFIT does not run on every sealed unicast mesh frame, whole under a Block Ack session or its DELBA wait, after the rate table and before its PN count}"
awk '/^bool umac_datapath_mesh_ba_waiting\(/,/^}/' "$DD/umac_datapath_mesh.c" | tr -d ' \n' | \
  grep -q 'returnslot>=0&&s_ba_wait\[slot\].active;}' || why69="${why69:-the DELBA wait is not the open wait of the peer}"
awk '/^bool umac_ba_originator_held\(/,/^}/' ../../halow/components/mm-iot-sdk/framework/morselib/src/umac/ba/umac_ba.c | tr -d ' \n' | \
  grep -q 'returntid<UMAC_BA_MAX_SESSIONS&&(data->sessions.originator\[tid\].status==UMAC_BA_REQUESTED||data->sessions.originator\[tid\].status==UMAC_BA_SUCCESS);' || \
  why69="${why69:-a session is held other than while requested or agreed}"
awk '/^enum mmwlan_status umac_datapath_process_tx_frame\(/ {on=1} on && /^}/ {exit} on' "$DD/umac_datapath.c" | \
  awk '/else if \(is_multicast\)/ && !g {g=NR} /umac_rc_init_rate_table_mgmt\(umacd, &tx_metadata->rc_data, false\);/ && g && !t {t=NR}
       /if \(data->ops == &datapath_ops_mesh && g_warthog_sealfit != 0u\)/ && !f {f=NR}
       /umac_datapath_group_fit\(&tx_metadata->rc_data, over,/ && !c {c=NR} /if \(!rc_done\)/ && !u {u=NR}
       END {exit (g && t && f && c && u && g < t && t < f && f < c && c < u) ? 0 : 1}' || \
  why69="${why69:-AT+SEALFIT does not run on mesh group frames after their rate table}"
[ "$(grep -c 'umac_datapath_group_fit(' "$DD/umac_datapath.c")" = 2 ] || why69="${why69:-the group fit has other than one call site}"
SF69=$(awk '/^static void umac_datapath_seal_fit\(/,/^}/' "$DD/umac_datapath.c" | sed 's:/\*.*\*/::g' | tr -d ' \n')
case "$SF69" in *'constuint32_tthr=umac_config_get_frag_threshold(umacd);if(ba){constintw=umac_datapath_cap_rates(t,over,body,thr,1u);if(w>=0){g_warthog_sealfit_ba+=w>0?1u:0u;return;}g_warthog_sealfit_nofit++;}constintr=umac_datapath_cap_rates(t,over,body,thr,max);if(r==1)'*'elseif(r<0&&!ba){g_warthog_sealfit_nofit++;}}') ;;
  *) why69="${why69:-AT+SEALFIT does not count AT+FRAG, a Block Ack fit as seal_ba, or fall back to the cap when no rate is whole}" ;; esac
awk '/^static void umac_datapath_group_fit\(/,/^}/' "$DD/umac_datapath.c" | tr -d ' \n' | \
  grep -q 'constintr=umac_datapath_cap_rates(t,over,body,0u,1u);' || why69="${why69:-a group frame is not fitted whole}"
CR69=$(awk '/^static int umac_datapath_cap_rates\(/,/^}/' "$DD/umac_datapath.c" | tr -d ' \n')
case "$CR69" in *'constunsignedhead_rts=t->rates[0].flags&rts;'*'kept[0].flags=(uint8_t)(((unsigned)kept[0].flags&~rts)|head_rts);'*'for(unsignedi=0;i<MMRC_MAX_CHAIN_LENGTH;i++){if(i<keep)'*) ;;
  *) why69="${why69:-a head the trim puts in does not keep the RTS of the old head}" ;; esac
tr -d ' \n' < "$DD/umac_datapath_private.h" | \
  grep -q '#ifdefWARTHOG_MESH_HOSTFRAG_ANY#defineUMAC_DATAPATH_MESH_FRAG_RESERVEUMAC_MESH_FRAG_POOL_RESERVE#else#defineUMAC_DATAPATH_MESH_FRAG_RESERVE((UMAC_MESH_FRAG_CHIP_MAX-1u)\*UMAC_DATAPATH_MESH_MAX_PEERS+1u)#endif' || \
  why69="${why69:-the pool reserve is not one extra fragment per peer and a DELBA}"
grep -q '^volatile uint32_t g_warthog_sealfit = 1;' "$A" || why69="${why69:-AT+SEALFIT is not on by default}"
grep -q '"warthog_led", 3072,' ../../../main/led.c || why69="${why69:-the LED task stack is not 3072}"
if [ -z "$why69" ]; then
  ok "1.17.6's fragment rule: AT+HOSTFRAG off with host CCMP and at most 2 fragments otherwise (ANY in host tests only); AT+HOSTFRAG? names it; a rate that needs more is replaced; AT+SEALFIT on sealed unicast (whole under a Block Ack session or its DELBA wait, else the cap; AT+FRAG counted) and group frames, after the rates and before the PN count; a new head keeps the old RTS; the pool reserve follows the cap; the LED task has 3072 bytes"
else
  bad "fragment rule / stacks: $why69"
fi

# 70. IP fragments (measured on air 2026-10-03: a Pi at MTU 1460 fragments a 1472-byte ping and IDF's
#     default lwIP drops every fragment to it). sdkconfig.defaults turns on IPv4 reassembly with
#     IP_REASS_MAX_PBUFS 10 (Kconfig's floor) and leaves IPv6 reassembly off (IDF's default IPv6 input
#     hook drops all IPv6 while no netif has a link-local address), and every generated
#     sdkconfig.warthog-* says the same, since PlatformIO does not re-apply the defaults to an existing
#     one; no defaults overlay an env names turns IPv4 reassembly off. What the host test (lwip_napt.mk,
#     which runs IDF's own lwIP) cannot see: main/CMakeLists.txt builds nat_frag.c and hands lwIP
#     warthog_lwip_hooks.h as ESP_IDF_LWIP_HOOK_FILENAME (from a directory holding only that header);
#     the header defines LWIP_HOOK_IP4_INPUT as warthog_ip4_input_hook; the hook holds heap copies
#     (pbuf_clone with link-header room), gives a whole datagram back through tcpip_inpkt, never
#     ip4_input on the tcpip task's stack, and refuses to build without reassembly or with core-locked
#     input. Whole packets go through cut_short_ first: stock NAPT reads and, on a session match,
#     rewrites a TCP, UDP or ICMP header past a short packet's end (ASan in test_lwip_napt_frag).
#     Its napt_recv_ is ip4_input's whole condition for ip_napt_recv (inp at 0.0.0.0 or down too), checked
#     against the package's ip4.c when present; a UDP packet without its destination port is dropped
#     wherever it goes, as ip4_input reads that port for DHCP, which the host test's lwIP has on.
#     make all runs lwip-napt, and CI runs it (plain and SAN=1) after the PlatformIO build with
#     LWIP_NAPT_REQUIRED=1. AT+MTU? prints the hook's counts, ip_short_drop among them; the AT reference,
#     Troubleshooting, OpenMANET Interop, napt-notes and the fork inventory say so.
why70=""
for k70 in '^CONFIG_LWIP_IP4_REASSEMBLY=y$' '^CONFIG_LWIP_IP_REASS_MAX_PBUFS=10$' '^CONFIG_LWIP_IPV4_NAPT=y$' \
           '^CONFIG_LWIP_IP_FORWARD=y$'; do
  grep -q "$k70" ../../../sdkconfig.defaults || why70="${why70:-sdkconfig.defaults lacks $k70}"
done
grep -q '^CONFIG_LWIP_IP6_REASSEMBLY=y$' ../../../sdkconfig.defaults && why70="${why70:-sdkconfig.defaults turns on IPv6 reassembly, which no IPv6 packet reaches}"
n70=0
for g70 in ../../../sdkconfig.warthog-*; do
  [ -f "$g70" ] || continue
  n70=$((n70 + 1))
  for k70 in '^CONFIG_LWIP_IP4_REASSEMBLY=y$' '^# CONFIG_LWIP_IP6_REASSEMBLY is not set$' '^CONFIG_LWIP_IP_REASS_MAX_PBUFS=10$' \
             '^CONFIG_LWIP_HOOK_IP6_INPUT_DEFAULT=y$'; do
    grep -q "$k70" "$g70" || why70="${why70:-${g70##*/}: no $k70 (PlatformIO keeps a generated sdkconfig)}"
  done
done
for o70 in $(sed -n 's/.*SDKCONFIG_DEFAULTS="\([^"]*\)".*/\1/p' ../../../platformio.ini | tr ';' ' '); do
  [ -f "../../../$o70" ] || continue
  grep -Eq '^# CONFIG_LWIP_IP4_REASSEMBLY is not set$|^CONFIG_LWIP_IP4_REASSEMBLY=n$' "../../../$o70" && \
    why70="${why70:-$o70 turns reassembly off}"
done
MCM=../../../main/CMakeLists.txt
grep -q '^        "nat_frag.c"$' "$MCM" || why70="${why70:-main/CMakeLists.txt does not build nat_frag.c}"
tr -d ' \n' < "$MCM" | grep -q 'idf_component_get_property(lwiplwipCOMPONENT_LIB)target_compile_definitions(${lwip}PRIVATE"ESP_IDF_LWIP_HOOK_FILENAME=\\"warthog_lwip_hooks.h\\"")target_include_directories(${lwip}PRIVATE"${CMAKE_CURRENT_LIST_DIR}/lwip_hooks")' || \
  why70="${why70:-main/CMakeLists.txt does not hand lwIP the hook file and its directory}"
[ "$(ls ../../../main/lwip_hooks)" = warthog_lwip_hooks.h ] || why70="${why70:-main/lwip_hooks holds more than the hook header}"
grep -q '^#define LWIP_HOOK_IP4_INPUT(p, inp) warthog_ip4_input_hook((p), (inp))$' ../../../main/lwip_hooks/warthog_lwip_hooks.h || \
  why70="${why70:-warthog_lwip_hooks.h does not define LWIP_HOOK_IP4_INPUT as warthog_ip4_input_hook}"
NF=../../../main/nat_frag.c
HK70=$(awk '/^int warthog_ip4_input_hook\(/,/^}/' "$NF")
case "$HK70" in *'tcpip_inpkt(p, inp, ip4_input)'*) ;; *) why70="${why70:-the hook does not give the datagram back through tcpip_inpkt}" ;; esac
case "$HK70" in *'pbuf_clone(PBUF_LINK, PBUF_RAM, p)'*) ;; *) why70="${why70:-the hook does not hold heap copies with link-header room}" ;; esac
printf '%s\n' "$HK70" | grep -q 'ip4_input(p' && why70="${why70:-the hook calls ip4_input on the tcpip stack}"
printf '%s\n' "$HK70" | awk '/cut_short_\(p, inp\)/ {c=NR} /to_\(h, inp\);/ {f=NR} END {exit (c && f > c) ? 0 : 1}' || \
  why70="${why70:-the hook does not check whole packets for a short transport header before its fragment path}"
grep -q '^#if !IP_NAPT || !IP_REASSEMBLY$' "$NF" && grep -q '^#if LWIP_TCPIP_CORE_LOCKING_INPUT$' "$NF" || \
  why70="${why70:-nat_frag.c builds without reassembly or with core-locked input}"
awk '/^static int napt_recv_\(/,/^}/' "$NF" | grep -q '^    return !inp->napt && ip4_addr_eq(dst, netif_ip4_addr(inp));$' || \
  why70="${why70:-napt_recv_ is not the condition ip4_input calls ip_napt_recv on}"
CS70=$(awk '/^static int cut_short_\(/,/^}/' "$NF")
case "$CS70" in *'proto == IP_PROTO_UDP && have < hlen + 4'*'return napt_recv_(&dst, inp) || to_(h, inp) == TO_OURS;'*) ;;
  *) why70="${why70:-cut_short_ does not drop UDP without its port, then whatever napt_recv_ or to_ says NAPT reads}" ;; esac
I70="${IDF_LWIP:-$HOME/.platformio/packages/framework-espidf/components/lwip}/lwip/src/core/ipv4/ip4.c"
if [ -f "$I70" ]; then
  [ "$(grep -c 'ip_napt_recv(' "$I70")" = 1 ] && \
    grep -A1 -x '  if (!inp->napt && ip4_addr_cmp(&iphdr->dest, netif_ip4_addr(inp)))' "$I70" | grep -qx '    ip_napt_recv(p, iphdr);' || \
    why70="${why70:-ip4.c in the IDF package calls ip_napt_recv on another condition than napt_recv_ copies}"
fi
grep -q '^#define LWIP_DHCP 1$' lwip_napt/lwipopts.h && grep -q 'dhcp.c' lwip_napt.mk || \
  why70="${why70:-the host test builds lwIP without DHCP, so the UDP port read of ip4_input never runs}"
grep -q '^all:.* lwip-napt' Makefile && grep -q '^include lwip_napt.mk$' Makefile || why70="${why70:-make all does not run lwip-napt}"
grep -q 'make -C components/halow_mesh_compat/test lwip-napt LWIP_NAPT_REQUIRED=1$' ../../../.github/workflows/ci.yml && \
  grep -q 'make -C components/halow_mesh_compat/test clean lwip-napt SAN=1 LWIP_NAPT_REQUIRED=1$' ../../../.github/workflows/ci.yml && \
  awk '/run: pio run -e warthog-us$/ {b=NR} /lwip-napt LWIP_NAPT_REQUIRED=1$/ && !l {l=NR} END {exit (b && l > b) ? 0 : 1}' ../../../.github/workflows/ci.yml || \
  why70="${why70:-CI does not run lwip-napt after its PlatformIO build}"
awk '/^static void cmd_mtu\(void\)/,/^}/' ../../../main/at.c | grep -q '"+MTU: ip_reass=%lu ip_reass_drop=%lu ip_short_drop=%lu\\r\\n"' || \
  why70="${why70:-AT+MTU? does not print the counts of the hook}"
for w70 in ip_reass_drop ip_short_drop; do
  grep '^| `AT+MTU?`' ../../../wiki/AT-Command-Reference.md | grep -q "$w70" || why70="${why70:-the AT+MTU? row does not name $w70}"
done
grep -q 'ip_short_drop' ../../../docs/napt-notes.md || why70="${why70:-napt-notes does not cover short packets}"
grep -q 'CONFIG_LWIP_IP4_REASSEMBLY' ../../../wiki/Troubleshooting.md || why70="${why70:-Troubleshooting does not name CONFIG_LWIP_IP4_REASSEMBLY}"
grep -q 'ip_reass' ../../../wiki/OpenMANET-Interop.md || why70="${why70:-OpenMANET Interop does not cover fragments to bat0-MTU nodes}"
grep -q 'nat_frag.c' ../../../docs/napt-notes.md || why70="${why70:-napt-notes does not cover fragments}"
grep -q 'ESP_IDF_LWIP_HOOK_FILENAME' ../../../docs/fork-inventory.md || why70="${why70:-the fork inventory does not list the lwIP hook}"
if [ -z "$why70" ]; then
  ok "IP fragments: IPv4 reassembly on (IPv6 off), 10 pbufs, in sdkconfig.defaults and all $n70 generated sdkconfigs; the input hook wired into lwIP, dropping short whole packets on ip4_input's own NAPT condition, holding heap copies and handing datagrams back through the mailbox; lwip-napt (lwIP with DHCP) in make all and CI; AT+MTU? counts; documented"
else
  bad "IP fragments: $why70"
fi

# 71. The USB network class: every generated sdkconfig.warthog-* builds CDC-NCM, sdkconfig.defaults'
#     choice, except warthog-us-ecm (sdkconfig.ecm). PlatformIO keeps an explicit line of an existing
#     generated file over the defaults, so one written before NCM became the default (1b3e608) builds
#     ECM until it is edited or deleted (warthog-eu/jp/kr/au did until 2026-10-03). sdkconfig.ecm must
#     be tracked: .gitignore's sdkconfig.* hid it, and IDF stops at CMake configure when a file named
#     in SDKCONFIG_DEFAULTS is missing, so warthog-us-ecm built only on the machine that had it.
#     esp_tinyusb's default configuration descriptor has CDC and NCM functions only, so under ECM
#     usb_net.c must hand it its own CDC + ECM one (until 2026-10-03 it passed NULL and the ECM image
#     enumerated a console and no network). CI builds warthog-us-ecm and checks its ELF holds that
#     descriptor (154 bytes: TinyUSB's TUD_CDC_ECM_DESC_LEN is 79, not the 71 its comment says).
why71=""
grep -q '^CONFIG_TINYUSB_NET_MODE_NCM=y$' ../../../sdkconfig.defaults || why71="sdkconfig.defaults does not choose NCM"
grep -q '^  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ecm"$' ../../../platformio.ini || \
  why71="${why71:-platformio.ini does not give warthog-us-ecm sdkconfig.ecm}"
if [ ! -f ../../../sdkconfig.ecm ]; then
  why71="${why71:-sdkconfig.ecm is missing; warthog-us-ecm names it in SDKCONFIG_DEFAULTS}"
elif ! grep -q '^CONFIG_TINYUSB_NET_MODE_ECM_RNDIS=y$' ../../../sdkconfig.ecm; then
  why71="${why71:-sdkconfig.ecm does not choose ECM}"
fi
grep -qx '!sdkconfig.ecm' ../../../.gitignore || why71="${why71:-.gitignore does not re-include sdkconfig.ecm, so a clean checkout lacks it}"
if git -C ../../.. rev-parse --is-inside-work-tree >/dev/null 2>&1 && git -C ../../.. check-ignore -q sdkconfig.ecm; then
  why71="${why71:-git ignores sdkconfig.ecm}"
fi
grep -q '^ *pio run -e warthog-us-ecm$' ../../../.github/workflows/ci.yml || why71="${why71:-CI does not build warthog-us-ecm}"
grep -qF "xtensa-esp32s3-elf-nm -S .pio/build/warthog-us-ecm/firmware.elf | grep ' 0000009a [dr] s_desc_fs_config\$'" ../../../.github/workflows/ci.yml || \
  why71="${why71:-CI does not check that the ECM image holds the usb_net.c configuration descriptor}"
U71=../../../main/usb_net.c
awk '/^#if CFG_TUD_ECM_RNDIS$/,/^#endif$/' "$U71" | tr -d '\n' | \
  grep -q 'static const uint8_t s_desc_fs_config\[\] = {.*TUD_CDC_DESCRIPTOR(.*TUD_CDC_ECM_DESCRIPTOR(.*#define USB_NET_FS_CONFIG s_desc_fs_config#else#define USB_NET_FS_CONFIG NULL#endif' || \
  why71="${why71:-usb_net.c has no CDC + ECM configuration descriptor under CFG_TUD_ECM_RNDIS}"
grep -q '^            \.full_speed_config = USB_NET_FS_CONFIG,$' "$U71" || why71="${why71:-usb_net.c does not hand esp_tinyusb the ECM descriptor}"
n71=0
for g71 in ../../../sdkconfig.warthog-*; do
  [ -f "$g71" ] || continue
  n71=$((n71 + 1))
  case "${g71##*/}" in sdkconfig.warthog-us-ecm) w71=ECM_RNDIS ;; *) w71=NCM ;; esac
  grep -q "^CONFIG_TINYUSB_NET_MODE_$w71=y$" "$g71" || \
    why71="${why71:-${g71##*/}: not CONFIG_TINYUSB_NET_MODE_$w71 (PlatformIO keeps a generated sdkconfig)}"
done
if [ -z "$why71" ]; then
  ok "USB network class: NCM in all $n71 generated sdkconfigs but warthog-us-ecm's ECM; sdkconfig.ecm not gitignored; under ECM usb_net.c hands esp_tinyusb a CDC + ECM descriptor; CI builds warthog-us-ecm and checks its ELF holds it"
else
  bad "USB network class: $why71"
fi

# 72. One owner of the USB network class (measured on air 2026-10-03: the USB link wedged after a
#     few fragmented pings). TinyUSB's class driver runs only in the TinyUSB task: lwIP's linkoutput
#     (l2_transmit) only queues (usbnet_tx); tud_network_can_xmit, _xmit and _recv_renew are called
#     only from usb_net.c's port one-liners, which only usbnet_core.c's pump and receive path call
#     (test_usbnet.c runs those against TinyUSB's own NCM and ECM drivers); tud_network_link_state
#     only from the TinyUSB event callback. What the host test cannot see: main/CMakeLists.txt
#     builds usbnet_core.c and links with -Wl,--wrap=netd_xfer_cb; __wrap_netd_xfer_cb runs the
#     driver's callback, then usbnet_xfer_done with whether the driver armed that endpoint again
#     (tx_busy_ms); the kick is usbd_defer_func(un_kicked_, NULL, false) and the hang guard's ping
#     usbd_defer_func(un_pong_, NULL, false), posted only into an empty queue: the only two
#     usbd_defer_func in main/ (each at most one queued); the receive callback is usbnet_rx and
#     the port is NCM's under CFG_TUD_NCM; un_input_ never frees after esp_netif_receive (it frees on
#     every failure); detach flushes the queue. The test's TinyUSB config matches the NTB counts and
#     sizes sdkconfig.defaults pins (esp_tinyusb's Kconfig defaults, which a package bump could move),
#     which no other SDKCONFIG_DEFAULTS file overrides, and every generated sdkconfig's; CI re-runs
#     this guard after its builds, since a fresh checkout has no generated sdkconfig. make all runs
#     usbnet, CI runs it with USBNET_REQUIRED=1 (plain and SAN=1) after the PlatformIO build;
#     AT+STATUS? prints +USBNET with tx_stall_ms, tx_busy_ms and rx_idle_ms; documented.
why72=""
U72=../../../main/usb_net.c
grep -q '"usbnet_core.c"' ../../../main/CMakeLists.txt || why72="main/CMakeLists.txt does not build usbnet_core.c"
grep -q '^target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=netd_xfer_cb")$' ../../../main/CMakeLists.txt || \
  why72="${why72:-main/CMakeLists.txt does not link with --wrap=netd_xfer_cb}"
awk '/^bool __wrap_netd_xfer_cb\(/,/^}/' "$U72" | awk '/__real_netd_xfer_cb\(/ {r=NR}
  /usbnet_xfer_done\(&s_usbnet, ep_addr, usbd_edpt_busy\(rhport, ep_addr\)\);/ {d=NR} END {exit (r && d > r) ? 0 : 1}' || \
  why72="${why72:-__wrap_netd_xfer_cb does not run the driver callback, then usbnet_xfer_done with the endpoint busy state}"
awk '/^static esp_err_t l2_transmit\(/,/^}/' "$U72" | grep -q 'usbnet_tx(&s_usbnet, buffer, len)' || \
  why72="${why72:-l2_transmit does not queue through usbnet_tx}"
awk '/^static esp_err_t l2_transmit\(/,/^}/' "$U72" | grep -Eq 'tud_|usbd_|vTaskDelay' && \
  why72="${why72:-l2_transmit calls TinyUSB or waits on the lwIP thread}"
for f72 in tud_network_can_xmit tud_network_xmit tud_network_recv_renew; do
  n72=$(grep -c "$f72(" "$U72")
  [ "$n72" = 1 ] && grep -Eq "^static [a-z]+ un_[a-z_]+\(.*\) \{ (return )?$f72\(" "$U72" || \
    why72="${why72:-$f72 is called outside its usb_net.c port one-liner ($n72 calls)}"
done
awk '/tud_network_link_state\(0, (true|false)\)/ && fn != "on_tinyusb_event" {print NR}
  /^[a-z].*\(/ && !/;$/ { if (match($0, /[a-z_0-9]+\(/)) fn = substr($0, RSTART, RLENGTH - 1) }' "$U72" | grep -q . && \
  why72="${why72:-tud_network_link_state is called outside the TinyUSB event callback}"
for o72 in ../../../main/*.c ../../../main/bat/*.c; do
  [ "$o72" = "$U72" ] && continue
  grep -Eq 'tud_network_(can_xmit|xmit|recv_renew|link_state)\(|usbd_defer_func\(|netd_xfer_cb' "$o72" && \
    why72="${why72:-${o72##*/} calls the USB network class or the TinyUSB defer queue}"
done
[ "$(grep -c 'usbd_defer_func(' "$U72")" = 2 ] && grep -q '^static void un_kick_(void) { usbd_defer_func(un_kicked_, NULL, false); }$' "$U72" || \
  why72="${why72:-the usbd_defer_func in usb_net.c are not the kick, usbd_defer_func(un_kicked_, NULL, false), and the hang guard ping}"
awk '/^enum warthog_hang_post warthog_usb_net_ping\(void\)/,/^}/' "$U72" | awk '/if \(!tud_inited\(\)\)/ {a=NR}
  /if \(s_ping_pending\)/ {b=NR} /if \(tud_task_event_ready\(\)\)/ {c=NR} /s_ping_pending = true;/ {d=NR}
  /usbd_defer_func\(un_pong_, NULL, false\);/ {e=NR} END {exit (a && a < b && b < c && c < d && d < e) ? 0 : 1}' || \
  why72="${why72:-warthog_usb_net_ping does not post one ping at most, only into the empty queue of a started TinyUSB}"
awk '/^static void un_pong_\(void \*arg\)/,/^}/' "$U72" | awk '/while \(g_warthog_hang_block == WARTHOG_HANG_TEST_USB\)/ {w=NR}
  /s_ping_pending = false;/ {f=NR} /s_pongs\+\+;/ {p=NR} END {exit (w && w < f && f < p) ? 0 : 1}' || \
  why72="${why72:-un_pong_ does not wait on AT+HANGTEST=usb, then let the next ping, then count}"
grep -q '^static void un_kicked_(void \*arg) { (void)arg; usbnet_kicked(&s_usbnet); }$' "$U72" || why72="${why72:-un_kicked_ does not run usbnet_kicked}"
grep -q '^bool tud_network_recv_cb(const uint8_t \*src, uint16_t size) { return usbnet_rx(&s_usbnet, src, size); }$' "$U72" || \
  why72="${why72:-tud_network_recv_cb is not usbnet_rx}"
awk '/^static const struct usbnet_port s_usbnet_port/,/^};/' "$U72" | tr '\n' ' ' | \
  grep -q '#if defined(CFG_TUD_NCM) && CFG_TUD_NCM *\.ncm = true, *#endif' || why72="${why72:-the port is not the NCM one under CFG_TUD_NCM}"
awk '/^static int un_input_\(/,/^}/' "$U72" | awk '/esp_netif_receive\(/ {r=NR} /free\(/ {f++; if (r) late=1}
  END {exit (r && f == 1 && !late) ? 0 : 1}' || why72="${why72:-un_input_ frees a frame esp_netif_receive already freed}"
awk '/case TINYUSB_EVENT_DETACHED:/,/break;/' "$U72" | grep -q 'usbnet_flush(&s_usbnet);' || why72="${why72:-detach does not flush the queue}"
for k72 in 'NCM_OUT_NTB_N 3' 'NCM_IN_NTB_N 3' 'NCM_OUT_NTB_MAX_SIZE 3200' 'NCM_IN_NTB_MAX_SIZE 3200'; do
  grep -q "^#define CFG_TUD_$k72$" usbnet_tusb/tusb_config.h || why72="${why72:-usbnet_tusb/tusb_config.h lacks CFG_TUD_$k72}"
done
for k72 in 'OUT_NTB_BUFFS_COUNT=3' 'IN_NTB_BUFFS_COUNT=3' 'OUT_NTB_BUFF_MAX_SIZE=3200' 'IN_NTB_BUFF_MAX_SIZE=3200'; do
  grep -q "^CONFIG_TINYUSB_NCM_$k72$" ../../../sdkconfig.defaults || \
    why72="${why72:-sdkconfig.defaults does not pin CONFIG_TINYUSB_NCM_$k72, which the host test models}"
done
for d72 in $(sed -n 's/^ *-DSDKCONFIG_DEFAULTS="\(.*\)"$/\1/p' ../../../platformio.ini | tr ';' '\n' | sort -u); do
  [ "$d72" = sdkconfig.defaults ] && continue
  grep -q '^CONFIG_TINYUSB_NCM_' "../../../$d72" 2>/dev/null && \
    why72="${why72:-$d72 overrides the NCM buffers sdkconfig.defaults pins}"
done
for g72 in ../../../sdkconfig.warthog-*; do
  [ -f "$g72" ] || continue
  case "${g72##*/}" in sdkconfig.warthog-us-ecm) continue ;; esac
  for k72 in 'OUT_NTB_BUFFS_COUNT=3' 'IN_NTB_BUFFS_COUNT=3' 'OUT_NTB_BUFF_MAX_SIZE=3200' 'IN_NTB_BUFF_MAX_SIZE=3200'; do
    grep -q "^CONFIG_TINYUSB_NCM_$k72$" "$g72" || why72="${why72:-${g72##*/}: not CONFIG_TINYUSB_NCM_$k72, which the host test models}"
  done
done
grep -q '^all:.* usbnet$' Makefile || why72="${why72:-make all does not run usbnet}"
grep -q 'make -C components/halow_mesh_compat/test usbnet USBNET_REQUIRED=1$' ../../../.github/workflows/ci.yml && \
  grep -q 'make -C components/halow_mesh_compat/test clean usbnet SAN=1 USBNET_REQUIRED=1$' ../../../.github/workflows/ci.yml && \
  awk '/run: pio run -e warthog-us$/ {b=NR} /usbnet USBNET_REQUIRED=1$/ && !l {l=NR} END {exit (b && l > b) ? 0 : 1}' ../../../.github/workflows/ci.yml || \
  why72="${why72:-CI does not run usbnet after its PlatformIO build}"
awk '/run: pio run -e warthog-us$/ {b=NR} /run: make -C components\/halow_mesh_compat\/test glueguard$/ {g=NR}
  END {exit (b && g > b) ? 0 : 1}' ../../../.github/workflows/ci.yml || why72="${why72:-CI does not re-run the glue guard after its builds}"
awk '/^static void cmd_status\(void\)/,/^}/' ../../../main/at.c | grep -q '"+USBNET: tx_queued=%lu txq=%lu/%u txq_hw=%lu tx_stall_ms=%lu tx_busy_ms=%lu' && \
  awk '/^static void cmd_status\(void\)/,/^}/' ../../../main/at.c | grep -q 'rx_idle_ms=%lu' || \
  why72="${why72:-AT+STATUS? does not print +USBNET with all three stall indicators}"
for w72 in '+USBNET:' tx_stall_ms tx_busy_ms rx_idle_ms; do
  grep -qF -- "$w72" ../../../wiki/AT-Command-Reference.md || why72="${why72:-the AT reference does not document $w72}"
done
for w72 in tx_stall_ms tx_busy_ms rx_idle_ms; do
  grep -qF -- "$w72" ../../../wiki/Troubleshooting.md || why72="${why72:-Troubleshooting does not cover $w72}"
done
grep -q 'wrap=netd_xfer_cb' ../../../docs/fork-inventory.md || why72="${why72:-the fork inventory does not list the netd_xfer_cb wrap}"
if [ -z "$why72" ]; then
  ok "USB network class: one owner (the TinyUSB task), lwIP only queues, the --wrap completion pump with the endpoint's busy state, one kick and one hang-guard ping at most, NCM renews per datagram; host test config matches sdkconfig.defaults' pinned NTBs and every NCM sdkconfig; usbnet in make all and CI, glue guard re-run after the builds; +USBNET documented"
else
  bad "USB network class: $why72"
fi

# 73. The hang guard (main/hang_guard.c, hang_guard_core.c, the shim's ping, the health and driver test blocks,
#     usb_net.c's ping, AT+HANG?, AT+HANGTEST): the task watchdog panics at 60 s with both idle tasks watched
#     and the interrupt watchdog on, in sdkconfig.defaults and every generated sdkconfig.warthog-* with the
#     flash core dump, and hang_guard.c refuses a config without it; app_main starts the guard after USB and
#     ticks it last in its loop; the tick logs, allocates, locks and waits on nothing but AT+HANGTEST=main,
#     aborts on a stall and resets the watchdog last; each probe's post and answer are exact; tiT's message is
#     allocated once and only tried, and tiT also beats from one lwIP timeout, which runs while its mailbox is
#     full; the ROM console is silenced once USB is up; the record is in .noinit, its magic written last, and
#     taken and cleared first in app_main; the health probe counts the health task's wakes; only hang_guard.c
#     subscribes to the watchdog, and nothing in main/ reconfigures it, uninstalls TinyUSB or stops the umac
#     event loop the guard posts to; the reason is formatted without libc; the shim's ping waits one at most;
#     each test block holds only its task; AT+HANG? and AT+HANGTEST are dispatched, AT+HANGTEST replies before
#     it blocks; where present, IDF's caption and no-wait mailbox post and TinyUSB's queue are as relied on;
#     documented.
HG73=../../../main/hang_guard.c
HC73=../../../main/hang_guard_core.c
HH73=../../../main/hang_guard_core.h
MN73=../../../main/main.c
DF73=../../../sdkconfig.defaults
MLB73=../../halow/components/mm-iot-sdk/framework/morselib
why73=""
TW73='CONFIG_ESP_TASK_WDT_EN=y CONFIG_ESP_TASK_WDT_INIT=y CONFIG_ESP_TASK_WDT_PANIC=y CONFIG_ESP_TASK_WDT_TIMEOUT_S=60
      CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y'
for k73 in $TW73; do
  grep -qx "$k73" "$DF73" || why73="${why73:-sdkconfig.defaults lacks $k73}"
done
grep -Eq '^# CONFIG_ESP_INT_WDT is not set$|^CONFIG_ESP_INT_WDT=n$' "$DF73" && why73="${why73:-sdkconfig.defaults turns the interrupt watchdog off}"
for g73 in ../../../sdkconfig.warthog-*; do
  [ -f "$g73" ] && grep -q '^CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y$' "$g73" || continue
  for k73 in $TW73; do
    grep -qx "$k73" "$g73" || { why73="${why73:-${g73##*/}: the task watchdog does not panic at 60 s (PlatformIO keeps a generated sdkconfig: delete it)}"; break; }
  done
done
tw73=$(sed -n 's/^#define WARTHOG_HANG_TWDT_S \([0-9]*\)u.*/\1/p' "$HH73")
[ -n "$tw73" ] && [ "$tw73" = "$(sed -n 's/^CONFIG_ESP_TASK_WDT_TIMEOUT_S=\([0-9]*\)$/\1/p' "$DF73")" ] || \
  why73="${why73:-WARTHOG_HANG_TWDT_S (${tw73:-none}) is not CONFIG_ESP_TASK_WDT_TIMEOUT_S in sdkconfig.defaults}"
er73=$(awk '/^#if !defined\(CONFIG_ESP_TASK_WDT_PANIC\)/,/^#endif/' "$HG73" | tr -d ' \n\\')
for k73 in '!defined(CONFIG_ESP_TASK_WDT_PANIC)' 'CONFIG_ESP_TASK_WDT_TIMEOUT_S!=WARTHOG_HANG_TWDT_S' \
           '!defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0)' '!defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1)' '#error"'; do
  case "$er73" in *"$k73"*) ;; *) why73="${why73:-hang_guard.c does not refuse a task watchdog config without $k73}" ;; esac
done
tr -d ' \n' < "$HG73" | grep -q '_Static_assert(MMWLAN_HANG_BLOCK_LOOP==WARTHOG_HANG_TEST_LOOP&&MMWLAN_HANG_BLOCK_HEALTH==WARTHOG_HANG_TEST_HEALTH&&MMWLAN_HANG_BLOCK_DRV==WARTHOG_HANG_TEST_DRV,' && \
  tr -d ' \n' < "$HG73" | grep -q '_Static_assert(WARTHOG_HANG_LIMIT_S>=WARTHOG_BOOT_OK_S+10u,' || \
  why73="${why73:-hang_guard.c does not assert the AT+HANGTEST ids match those of morselib, or that a hang aborts after the crash count clears}"
grep -q '"hang_guard.c"' ../../../main/CMakeLists.txt && grep -q '"hang_guard_core.c"' ../../../main/CMakeLists.txt || \
  why73="${why73:-main/CMakeLists.txt does not build hang_guard.c and hang_guard_core.c}"
awk '/^void app_main\(void\)/,/^}/' "$MN73" | awk '/const bool usb = warthog_usb_net_start\(\) != NULL;/ {u=NR}
  /USB did not start after/ {r=NR} /warthog_hang_guard_start\(usb\);/ {h=NR} /vTaskDelay\(pdMS_TO_TICKS\(750\)\);/ {d=NR}
  /warthog_wifi_ap_start\(\)/ {w=NR} /while \(1\) \{/ {l=NR} /vTaskDelay\(pdMS_TO_TICKS\(WARTHOG_HANG_TICK_MS\)\);/ {t=NR}
  /warthog_boot_guard_tick\(\);/ {b=NR} /warthog_hang_guard_tick\(\);/ {g=NR}
  END {exit (u && u < r && r < h && h < d && d < w && w < l && l < t && t < b && b < g) ? 0 : 1}' || \
  why73="${why73:-app_main does not start the guard right after USB, or does not tick it every WARTHOG_HANG_TICK_MS after the boot guard}"
awk '/^void app_main\(void\)/,/^}/' "$MN73" | awk '/const bool safe = warthog_boot_guard_start\(\);/ {s=NR}
  /warthog_hang_guard_early\(\);/ {e=NR} /if \(warthog_boot_hang_armed\(\)\)/ {a=NR} /ESP_ERROR_CHECK\(warthog_halow_start\(\)\);/ {h=NR}
  END {exit (s && e == s + 1 && e < a && a < h) ? 0 : 1}' || \
  why73="${why73:-app_main does not take and clear the previous record right after the boot guard starts, before the HaLow start}"
TK73=$(awk '/^void warthog_hang_guard_tick\(void\)/,/^}/' "$HG73")
[ -n "$TK73" ] || why73="${why73:-warthog_hang_guard_tick is gone}"
for no73 in ESP_LOG printf malloc calloc 'tcpip_callback(' 'tcpip_try_callback(' 'tcpip_callbackmsg_new(' esp_netif_ \
            xSemaphoreTake xQueueReceive mmosal_semb_wait mmosal_task_sleep vTaskDelay; do
  case "$TK73" in *"$no73"*) why73="${why73:-warthog_hang_guard_tick calls $no73}" ;; esac
done
[ "$(printf '%s\n' "$TK73" | sed -n '3p' | sed 's#/\*.*\*/##' | tr -d ' ')" = 'block_wait_(WARTHOG_HANG_TEST_MAIN);' ] && \
  [ "$(printf '%s\n' "$TK73" | grep -c 'block_wait_(')" = 1 ] || \
  why73="${why73:-warthog_hang_guard_tick waits on more than the AT+HANGTEST=main block, its first statement}"
case "$TK73" in *'esp_system_abort(s_reason);'*) ;; *) why73="${why73:-warthog_hang_guard_tick does not abort on a stall}" ;; esac
[ "$(printf '%s\n' "$TK73" | sed '$d' | sed -n '$p' | sed 's/^ *//')" = 'if (s_twdt) { esp_task_wdt_reset(); }' ] || \
  why73="${why73:-the task watchdog reset is not the last statement of the tick}"
fn_of73() {
  awk -v re="$2" '/^[a-z].*\(.*\)$/ || /^[a-z].*\(.*[^;]$/ { if (match($0, /[a-z_0-9]+\(/)) fn = substr($0, RSTART, RLENGTH - 1) }
    /^}/ { fn = "" } index($0, re) && $0 !~ /^ *\/[*\/]/ { print (fn == "" ? "-" : fn) }' "$1" | sort -u | tr '\n' ' '
}
[ "$(fn_of73 "$HG73" 'tcpip_callbackmsg_trycallback(')" = 'tcpip_post_ ' ] && \
  [ "$(fn_of73 "$HG73" 'tcpip_callbackmsg_new(')" = 'warthog_hang_guard_start ' ] || \
  why73="${why73:-the tiT message is not allocated once in warthog_hang_guard_start and only tried in tcpip_post_}"
body73() { awk -v f="$2" '/^[a-z]/ && index($0, " " f "(") { on = 1 } on { print } on && /^}/ { exit }' "$1" | tr -d ' \n'; }
[ "$(body73 "$HG73" loop_post_)" = 'staticenumwarthog_hang_postloop_post_(void){constenummmwlan_statusst=mmwlan_loop_ping();returnst==MMWLAN_SUCCESS?WARTHOG_HANG_POSTED:st==MMWLAN_NO_MEM?WARTHOG_HANG_FULL:WARTHOG_HANG_OFF;}' ] || \
  why73="${why73:-loop_post_ does not age the loop probe on a full queue and turn it off only without a loop}"
[ "$(body73 "$HG73" tcpip_post_)" = 'staticenumwarthog_hang_posttcpip_post_(void){if(s_tcpip_msg==NULL){returnWARTHOG_HANG_OFF;}if(s_tcpip_pending){returnWARTHOG_HANG_POSTED;}s_tcpip_pending=true;if(tcpip_callbackmsg_trycallback(s_tcpip_msg)!=ERR_OK){s_tcpip_pending=false;returnWARTHOG_HANG_FULL;}returnWARTHOG_HANG_POSTED;}' ] || \
  why73="${why73:-tcpip_post_ does not post one message at most, aging the probe on a full mailbox}"
[ "$(body73 "$HG73" tcpip_beat_)" = 'staticvoidtcpip_beat_(void*arg){(void)arg;block_wait_(WARTHOG_HANG_TEST_TCPIP);s_tcpip_pongs++;sys_timeout(WARTHOG_HANG_TICK_MS,tcpip_beat_,NULL);}' ] && \
  [ "$(body73 "$HG73" tcpip_pong_)" = 'staticvoidtcpip_pong_(void*ctx){(void)ctx;s_tcpip_pending=false;sys_untimeout(tcpip_beat_,NULL);tcpip_beat_(NULL);}' ] && \
  [ "$(fn_of73 "$HG73" 'sys_timeout(')" = 'tcpip_beat_ ' ] && [ "$(fn_of73 "$HG73" 'sys_untimeout(')" = 'tcpip_pong_ ' ] || \
  why73="${why73:-tiT does not answer by its message and one 1 s lwIP timeout, restarted by each message, waiting only on AT+HANGTEST=tcpip}"
[ "$(body73 "$HG73" timer_beat_)" = 'staticvoidtimer_beat_(void*arg){(void)arg;block_wait_(WARTHOG_HANG_TEST_TIMER);s_timer_beats++;}' ] || \
  why73="${why73:-the esp_timer beat does not count, waiting only on AT+HANGTEST=timer}"
[ "$(fn_of73 "$HG73" 'block_wait_(WARTHOG')" = 'tcpip_beat_ timer_beat_ warthog_hang_guard_tick ' ] || \
  why73="${why73:-an AT+HANGTEST block in hang_guard.c holds another task than its own}"
case "$(printf '%s\n' "$TK73" | tr -d ' \n')" in
  *'constuint32_tnow=now_ms_();uint32_ta=g_warthog_loop_pongs;warthog_hang_step(&s_probe[WARTHOG_HANG_LOOP],a,loop_post_(),now);a=s_tcpip_pongs;warthog_hang_step(&s_probe[WARTHOG_HANG_TCPIP],a,tcpip_post_(),now);a=warthog_usb_net_pongs();warthog_hang_step(&s_probe[WARTHOG_HANG_USB],a,warthog_usb_net_ping(),now);warthog_hang_step(&s_probe[WARTHOG_HANG_TIMER],s_timer_beats,s_timer!=NULL?WARTHOG_HANG_POSTED:WARTHOG_HANG_OFF,now);uint32_twakes=0,interval=0;constboolhealth=mmwlan_hang_health(&wakes,&interval);s_probe[WARTHOG_HANG_HEALTH].limit_ms=warthog_hang_health_limit_ms(interval);warthog_hang_step(&s_probe[WARTHOG_HANG_HEALTH],wakes,health?WARTHOG_HANG_POSTED:WARTHOG_HANG_OFF,now);'*) ;;
  *) why73="${why73:-the tick does not step each probe once, its answers read before its post, the health limit from its interval}" ;;
esac
[ "$(printf '%s\n' "$TK73" | grep -c 'warthog_hang_step(')" = 5 ] || why73="${why73:-the tick does not step exactly the five probes}"
[ "$(body73 "$HG73" warthog_hang_guard_early)" = 'voidwarthog_hang_guard_early(void){if(s_hang_nv.magic==WARTHOG_HANG_MAGIC){s_prev=s_hang_nv;s_prev_valid=true;}s_hang_nv.magic=0;}' ] && \
  [ "$(fn_of73 "$HG73" 's_hang_nv.magic = 0;')" = 'warthog_hang_guard_early ' ] && [ "$(fn_of73 "$HG73" 's_prev = ')" = 'warthog_hang_guard_early ' ] || \
  why73="${why73:-the previous record is not taken and cleared in warthog_hang_guard_early alone}"
awk '/^void warthog_hang_guard_start\(bool usb\)/,/^}/' "$HG73" | tr -d ' \n' | \
  grep -q 'if(usb){[^}]*esp_rom_install_channel_putc(1,NULL);esp_rom_install_channel_putc(2,NULL);}' || \
  why73="${why73:-warthog_hang_guard_start does not silence ROM putc channels 1 and 2 once USB is up}"
grep -qx 'static __NOINIT_ATTR struct warthog_hang_rec s_hang_nv;' "$HG73" || why73="${why73:-the record is not in .noinit}"
awk '/^void warthog_hang_rec_fill\(/,/^}/' "$HC73" | grep -q 'volatile struct warthog_hang_rec \*v = r;' && \
  [ "$(awk '/^void warthog_hang_rec_fill\(/,/^}/' "$HC73" | sed '$d' | sed -n '$p' | sed 's/^ *//')" = 'v->magic = WARTHOG_HANG_MAGIC;' ] || \
  why73="${why73:-the magic of the record is not its last store}"
wa73=$(grep -l 'esp_task_wdt_add' ../../../main/*.c ../../../main/bat/*.c 2>/dev/null | sed 's#.*/##' | tr '\n' ' ')
[ "$wa73" = 'hang_guard.c ' ] || why73="${why73:-esp_task_wdt_add is not in hang_guard.c alone (${wa73:-none})}"
grep -El 'esp_task_wdt_(init|deinit|reconfigure|delete)|tinyusb_driver_uninstall' ../../../main/*.[ch] ../../../main/bat/*.[ch] 2>/dev/null | \
  grep -q . && why73="${why73:-main/ reconfigures or deletes the task watchdog, or uninstalls TinyUSB}"
# The guard posts to the event loop every second, and umac_core_stop deletes the loop's semaphore under a concurrent post.
st73=$(grep -El '(mmwlan_shutdown|mmwlan_sta_disable(_nowait)?|mmwlan_ap_disable|mmhalow_deinit|mmhalow_disconnect)\(' \
         ../../../main/*.c ../../../main/bat/*.c 2>/dev/null | sed 's#.*/##' | tr '\n' ' ')
[ -z "$st73" ] || why73="${why73:-main/ stops the umac event loop the guard posts to (${st73% })}"
rs73=$({ awk '/^size_t warthog_hang_reason\(/,/^}/' "$HC73"; awk '/^static char \*hang_put_/,/^}/' "$HC73"; })
[ -n "$rs73" ] && ! printf '%s\n' "$rs73" | grep -Eq '(printf|str(len|n?cpy|n?cat|chr)|mem(cpy|set|move)|[a-z]toa)\(' || \
  why73="${why73:-the hang reason is formatted with libc}"
LR73=$(awk '/^enum mmwlan_status umac_loop_ping_request\(/,/^}/' "$SHM" | sed 's#/\*.*\*/##' | tr -d ' \n')
case "$LR73" in *'if(core->evtloop_task==NULL||core->evtloop_shutting_down){s_loop_ping_queued=false;returnMMWLAN_UNAVAILABLE;}if(s_loop_ping_queued){returnMMWLAN_SUCCESS;}'*'s_loop_ping_queued=true;if(!umac_core_evt_queue(umacd,&evt)){s_loop_ping_queued=false;returnloop_post_failed_(umacd);}returnMMWLAN_SUCCESS;'*) ;;
  *) why73="${why73:-umac_loop_ping_request does not post one ping at most, or keeps it waiting across a loop down or a failed post}" ;; esac
awk '/^static void loop_ping_evt_handler\(/,/^}/' "$SHM" | awk '/while \(g_warthog_hang_block == MMWLAN_HANG_BLOCK_LOOP\)/ {w=NR}
  /s_loop_ping_queued = false;/ {f=NR} /g_warthog_loop_pongs\+\+;/ {p=NR} END {exit (w && w < f && f < p) ? 0 : 1}' || \
  why73="${why73:-the ping handler does not wait on AT+HANGTEST=loop, then let the next ping, then count}"
awk '/^void umac_core_stop\(/,/^}/' "$MLB73/src/umac/core/umac_evtloop.c" | grep -q evtq && \
  why73="${why73:-umac_core_stop touches the event queue: a ping queued across a stop no longer runs after the start}"
ei73=$(grep -rn 'umac_evtq_init(' "$MLB73/src" | grep -v ':void umac_evtq_init(')
[ "$(printf '%s\n' "$ei73" | grep -c .)" = 1 ] && \
  awk '/^void umac_core_init\(/,/^}/' "$MLB73/src/umac/core/umac_core.c" | grep -q 'umac_evtq_init(' || \
  why73="${why73:-umac_evtq_init is called outside umac_core_init}"
awk '/^enum mmwlan_status mmwlan_loop_ping\(void\)/,/^}/' "$MLB73/src/umac/mesh/mmwlan_mesh.c" | tr -d ' \n' | \
  grep -q 'if(!umac_data_is_initialised(umacd)){returnMMWLAN_NOT_INITIALIZED;}returnumac_loop_ping_request(umacd);' || \
  why73="${why73:-mmwlan_loop_ping does not refuse before morselib is up}"
HH73B=$(awk '/^int mmdrv_hang_health\(/,/^}/' "$MLB73/src/driver/driver.c" | tr -d ' \n')
for k73 in '!driver_data.started' '!driver_data.health_check.task_running' '!driver_data.health_check.task_enabled' \
           'atomic_load(&driver_data.health_check.periodic_check_vetoes)!=0' 'driver_data.health_check.interval_ms==0' \
           'return-ENODEV;' '*wakes=driver_data.health_check.wakes;'; do
  case "$HH73B" in *"$k73"*) ;; *) why73="${why73:-mmdrv_hang_health does not test $k73}" ;; esac
done
wk73=$(grep -rE 'health_check\.wakes *(=[^=]|\+=|-=|\+\+|--)' "$MLB73/src" | cut -d: -f1 | sed 's#.*/##' | tr '\n' ' ')
[ "$wk73" = 'driver_health.c ' ] || why73="${why73:-the wake count of the health task has another writer than the task (${wk73:-none})}"
DH73=$(awk '/^static void driver_health_task_main\(/,/^}/' "$MLB73/src/driver/health/driver_health.c")
printf '%s\n' "$DH73" | \
  awk '/bool semb_taken = mmosal_semb_wait\(driverd->health_check.pending_semb, next_interval_ms\);/ {w=NR}
       w && !e && /if \(!driverd->health_check.task_enabled\)/ {e=NR} e && !k && /break;/ {k=NR}
       /while \(g_warthog_hang_block == MMWLAN_HANG_BLOCK_HEALTH\) \{ mmosal_task_sleep\(1000\); \}/ {b=NR}
       /driverd->health_check.wakes\+\+;/ {c=NR; n++} /if \(should_skip\(semb_taken, driverd\)\)/ {s=NR}
       /if \(driverd->health_check.force_fail\)/ {f=NR}
       END {exit (w && e == w + 1 && k == e + 2 && b == k + 2 && c == b + 1 && n == 1 && c < s && s < f) ? 0 : 1}' || \
  why73="${why73:-the health task does not take the AT+HANGTEST=health block, then count its wake, right after each wait and before its skip test}"
printf '%s\n' "$DH73" | tr -d ' \n' | grep -qF 'constuint32_tlast_ms=driverd->health_check.last_checked;constuint32_tsince_ms=mmosal_get_time_ms()-last_ms;next_interval_ms=since_ms>=driverd->health_check.interval_ms?0:driverd->health_check.interval_ms-since_ms;' || \
  why73="${why73:-the wait of the health task can wrap past its deadline (to about 49 days), which the hang guard would take for a hang}"
awk '/^void driver_task_main\(/,/^}/' "$MLB73/src/driver/driver_task.c" | \
  awk '/^    while \(true\)$/ && !w {w=NR} /while \(g_warthog_hang_block == MMWLAN_HANG_BLOCK_DRV\) \{ mmosal_task_sleep\(1000\); \}/ {b=NR}
       END {exit (w && b == w + 2) ? 0 : 1}' || \
  why73="${why73:-the AT+HANGTEST=drv block is not the first statement of the driver task loop}"
grep -q '^extern volatile uint32_t g_warthog_loop_pongs, g_warthog_hang_block;$' "$SHM" && \
  grep -q '^extern volatile uint32_t g_warthog_hang_block;$' "$MLB73/src/driver/health/driver_health.c" && \
  grep -q '^extern volatile uint32_t g_warthog_hang_block;$' "$MLB73/src/driver/driver_task.c" && \
  grep -q '^volatile uint32_t g_warthog_loop_pongs = 0, g_warthog_hang_block = 0;$' "$A" || \
  why73="${why73:-the ping count or the AT+HANGTEST block is not stored in at.c}"
awk '/strcasecmp\(verb, "ASSERTTEST"\) == 0 && terminator == .=./ {a=NR}
     /strcasecmp\(verb, "HANG"\) == 0 && terminator == .\?./ {h=NR} /^        cmd_hang_query\(\);$/ {hq=NR}
     /strcasecmp\(verb, "HANGTEST"\) == 0 && terminator == .=./ {t=NR} /^        cmd_hangtest\(trim\(args\)\);$/ {tq=NR}
     END {exit (a && a < h && hq == h + 1 && h < t && tq == t + 1) ? 0 : 1}' "$A" || \
  why73="${why73:-AT+HANG? and AT+HANGTEST= are not dispatched after AT+ASSERTTEST}"
awk '/^static void cmd_hangtest\(const char \*a\)/,/^}/' "$A" | awk '/reply_ok\(\);/ {r=NR}
  /vTaskDelay\(pdMS_TO_TICKS\(MMWLAN_ASSERT_TEST_DELAY_MS\)\);/ {d=NR; rd=r} /g_warthog_hang_block = id;/ {s=NR}
  END {exit (rd && rd < d && d < s) ? 0 : 1}' || \
  why73="${why73:-AT+HANGTEST does not reply, then wait, then block}"
use73=$(sed -n 's/.*"usage: AT+HANGTEST=<\([^>]*\)>".*/|\1|/p' "$A")
for n73 in $(awk '/^const char \*const warthog_hang_test_names\[/,/};/' "$HC73" | grep -o '"[a-z]*"' | tr -d '"') off; do
  case "$use73" in *"|$n73|"*) ;; *) why73="${why73:-the AT+HANGTEST usage does not name $n73}" ;; esac
done
awk '/^static void cmd_coredump\(void\)/,/^}/' "$A" | awk '/static char reason\[160\]/ {r=NR} /warthog_hang_coredump_reason\(reason\);/ {c=NR}
  /"\+COREDUMP: reason=%s\\r\\n", reason/ {p=NR} END {exit (r && r < c && c < p) ? 0 : 1}' || \
  why73="${why73:-AT+COREDUMP? does not put the abort reason on one line before it prints it}"
up73=""
IDFS73=${IDF_ESP_SYSTEM:-$HOME/.platformio/packages/framework-espidf/components/esp_system}
if [ -f "$IDFS73/task_wdt/task_wdt.c" ]; then
  ic73=$(awk '/const char \*caption = /,/;/' "$IDFS73/task_wdt/task_wdt.c" | grep -o '"[^"]*"' | tr -d '"\n')
  oc73=$(awk '/^static const char k_twdt_caption\[\] = /,/;/' "$HC73" | grep -o '"[^"]*"' | tr -d '"\n')
  [ -n "$ic73" ] && [ "$ic73" = "$oc73" ] || why73="${why73:-the IDF task watchdog caption is not the one warthog_hang_coredump_reason matches}"
  grep -qF 'msg_handler(opaque, "\n - ");' "$IDFS73/task_wdt/task_wdt.c" || why73="${why73:-the IDF task watchdog no longer lists tasks after a newline and a dash}"
  up73="$up73 task_wdt.c"
fi
if [ -f "$IDFS73/../lwip/port/freertos/sys_arch.c" ]; then
  awk '/^sys_mbox_trypost\(sys_mbox_t \*mbox, void \*msg\)/,/^}/' "$IDFS73/../lwip/port/freertos/sys_arch.c" | \
    grep -q 'xQueueSend((\*mbox)->os_mbox, &msg, 0)' && \
    awk '/^tcpip_callbackmsg_trycallback\(struct tcpip_callback_msg \*msg\)/,/^}/' "$IDFS73/../lwip/lwip/src/api/tcpip.c" | \
    grep -q 'return sys_mbox_trypost(&tcpip_mbox, msg);' || \
    why73="${why73:-the IDF tcpip_callbackmsg_trycallback may wait for room in the tiT mailbox}"
  grep -qF '#define TCPIP_MBOX_FETCH(mbox, msg) tcpip_timeouts_mbox_fetch(mbox, msg)' "$IDFS73/../lwip/lwip/src/api/tcpip.c" && \
    awk '/^tcpip_timeouts_mbox_fetch\(sys_mbox_t \*mbox, void \*\*msg\)/,/^}/' "$IDFS73/../lwip/lwip/src/api/tcpip.c" | tr -d ' \n' | \
    grep -qF 'sleeptime=sys_timeouts_sleeptime();if(sleeptime==SYS_TIMEOUTS_SLEEPTIME_INFINITE){UNLOCK_TCPIP_CORE();sys_arch_mbox_fetch(mbox,msg,0);LOCK_TCPIP_CORE();return;}elseif(sleeptime==0){sys_check_timeouts();' && \
    grep -Eq '^#define MEMP_MEM_MALLOC +1$' "$IDFS73/../lwip/port/include/lwipopts.h" || \
    why73="${why73:-the IDF tiT no longer runs due lwIP timeouts before each mailbox fetch, or takes them from a pool: the tcpip beat can stop while tiT runs}"
  up73="$up73 sys_arch.c tcpip.c"
fi
TU73=${TUSB_DIR:-../../../managed_components/espressif__tinyusb}
if [ -d "$TU73/src/device" ]; then
  awk '/^void usbd_defer_func\(/,/^}/' "$TU73/src/device/usbd.c" | grep -q 'queue_event(&event, in_isr);' && \
    awk '/osal_queue_send\(osal_queue_t qhdl/,/^}/' "$TU73/src/osal/osal_freertos.h" | tr -d ' \n' | grep -q 'if(!in_isr){returnxQueueSendToBack(' && \
    awk '/^bool tud_task_event_ready\(void\)/,/^}/' "$TU73/src/device/usbd.c" | grep -q 'return !osal_queue_empty(_usbd_q);' || \
    why73="${why73:-the TinyUSB defer queue or its empty test is not what warthog_usb_net_ping relies on}"
  up73="$up73 tinyusb"
fi
R73=../../../wiki/AT-Command-Reference.md
rh73=$(grep '^| `AT+HANG?`' "$R73")
rt73=$(grep '^| `AT+HANGTEST=' "$R73")
rows73="$rh73 $rt73"
ls73=$(sed -n 's/^#define WARTHOG_HANG_LIMIT_S \([0-9]*\)u.*/\1/p' "$HH73")
for k73 in $(awk '/^const char \*const warthog_hang_names\[/,/};/' "$HC73" | grep -o '"[a-z]*"' | tr -d '"') \
           $(awk '/^const char \*const warthog_hang_test_names\[/,/};/' "$HC73" | grep -o '"[a-z]*"' | tr -d '"') \
           TASK_WDT reset=PANIC; do
  case "$rows73" in *"$k73"*) ;; *) why73="${why73:-the AT+HANG? and AT+HANGTEST rows do not name $k73}" ;; esac
done
for k73 in "${ls73:-?} s" "${tw73:-?} s"; do
  case "$rows73" in *"$k73"*) ;; *) why73="${why73:-the AT+HANG? and AT+HANGTEST rows do not say $k73}" ;; esac
done
for r73 in "$rh73" "$rt73"; do
  case "$r73" in *' Measured on air on 2026-10-07 '*' |') ;; *) why73="${why73:-an AT+HANG? or AT+HANGTEST row is missing, or does not give its on-air measurement}" ;; esac
done
grep '^| `AT+COREDUMP?`' "$R73" | grep -F 'HANG <probe>:' | grep -q 'TASK_WDT' || \
  why73="${why73:-the AT+COREDUMP? row does not name the HANG and TASK_WDT reasons}"
TH73=$(awk '/^### Hangs$/ {on=1; print; next} on && /^##/ {exit} on {print}' ../../../wiki/Troubleshooting.md)
for k73 in 'AT+HANG?' 'AT+HANGTEST' 'reason=HANG' 'TASK_WDT' "$ls73 s" "$tw73 s" 'safe mode'; do
  case "$TH73" in *"$k73"*) ;; *) why73="${why73:-the Hangs section of Troubleshooting does not name $k73}" ;; esac
done
for k73 in mmwlan_loop_ping mmwlan_hang_health health_check.wakes g_warthog_hang_block tcpip_timeouts_mbox_fetch \
           'Re-check the fifteen entries'; do
  grep -qF "$k73" ../../../docs/fork-inventory.md || why73="${why73:-the fork inventory does not name $k73}"
done
if [ -z "$why73" ]; then
  ok "hang guard: the task watchdog panics at 60 s (main and both idle tasks), a stale sdkconfig refused; main ticks the guard every second after USB, waiting on nothing, and aborts on a probe past its limit; one ping each to the loop, tiT and TinyUSB, none waits, each probe's post and answer exact; tiT beats from one lwIP timeout too; health counts the health task's wakes, its wait never wraps; .noinit record taken first in app_main, ROM console silenced, libc-free reason; nothing in main/ stops the loop or reconfigures the watchdog; test blocks hold only their task; AT+HANG? and AT+HANGTEST documented (upstream compared:${up73:- none present})"
else
  bad "hang guard: $why73"
fi

# 74. ROM download mode (main/dlmode.c; measured on air 2026-10-07: a board left in it kept its peers'
#     802.11s links up, the MM6108 ACKing their polls; after the ROM's own reset into the app 7 of 7 entries
#     never enumerated the ROM until the USB and IO_MUX reset exemption was cleared, then 12 of 12 within 1.1 s):
#     AT+DLMODE and each devloop CDC callback enter it once, only through warthog_enter_download(), which
#     resets the HaLow chip, marks the boot guard, unlocks and arms the RTC watchdog stage 0 to reset the RTC
#     domain (clearing FORCE_DOWNLOAD_BOOT) after WARTHOG_DLMODE_BACK_S of the calibrated slow clock, clears
#     RTC_CNTL_USB_CONF_REG's reset exemptions, leaves the bus for 100 ms, and only then writes the flag to
#     OPTION1 and SW_SYS_RST to OPTIONS0, never through esp_restart(); comments are stripped before matching;
#     only boot_guard.c and dlmode.c touch the RTC watchdog; AT+ASSERT? names the return DLMODE; the AT
#     reference, Flashing and Troubleshooting document it with its time limit.
why74=""
DM74=../../../main/dlmode.c
back74=$(sed -n 's/^#define WARTHOG_DLMODE_BACK_S \([0-9]*\)u.*/\1/p' ../../../main/dlmode.h)
[ -n "$back74" ] || why74="no WARTHOG_DLMODE_BACK_S in dlmode.h"
code74=$(sed -e 's://.*$::' -e 's:/\*.*\*/::g' "$DM74" | grep -Ev '^[[:space:]]*(/\*|\*)')
seq74=$(printf '%s\n' "$code74" | awk '/^void warthog_enter_download\(void\)$/,/^}$/' | grep -o \
  -e 'warthog_chip_hold_reset();' -e 'warthog_boot_mark_download();' -e 'wdt_hal_init(&ctx, WDT_RWDT, 0, false);' \
  -e 'wdt_hal_write_protect_disable(&ctx);' \
  -e 'WARTHOG_DLMODE_BACK_S \* 1000000u) << RTC_CLK_CAL_FRACT) / esp_clk_slowclk_cal_get()' \
  -e 'wdt_hal_config_stage(&ctx, WDT_STAGE0, ticks, WDT_STAGE_ACTION_RESET_RTC);' -e 'wdt_hal_enable(&ctx);' \
  -e 'CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_USB_RESET_DISABLE | RTC_CNTL_IO_MUX_RESET_DISABLE);' \
  -e 'tud_disconnect();' -e 'esp_rom_delay_us(100000);' -e 'portDISABLE_INTERRUPTS();' \
  -e 'REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);' -e 'REG_WRITE(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_SYS_RST);' | tr '\n' ' ')
want74='warthog_chip_hold_reset(); warthog_boot_mark_download(); wdt_hal_init(&ctx, WDT_RWDT, 0, false); wdt_hal_write_protect_disable(&ctx); WARTHOG_DLMODE_BACK_S * 1000000u) << RTC_CLK_CAL_FRACT) / esp_clk_slowclk_cal_get() wdt_hal_config_stage(&ctx, WDT_STAGE0, ticks, WDT_STAGE_ACTION_RESET_RTC); wdt_hal_enable(&ctx); CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_USB_RESET_DISABLE | RTC_CNTL_IO_MUX_RESET_DISABLE); tud_disconnect(); esp_rom_delay_us(100000); portDISABLE_INTERRUPTS(); REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT); REG_WRITE(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_SYS_RST); '
[ "$seq74" = "$want74" ] || why74="${why74:-warthog_enter_download does not reset the chip, mark, unlock and arm the RTC watchdog stage 0 on the calibrated slow clock to RESET_RTC, clear the ROM USB reset exemption, leave the bus for 100 ms and only then set the flag and reset the core, in that order (got: $seq74)}"
printf '%s\n' "$code74" | grep -q 'esp_restart\|RESET_SYSTEM\|RESET_CPU' && why74="${why74:-dlmode.c restarts through esp_restart or a watchdog action that keeps FORCE_DOWNLOAD_BOOT}"
for f74 in ../../../main/*.c; do
  case "$f74" in */dlmode.c) continue ;; esac
  grep -q 'RTC_CNTL_FORCE_DOWNLOAD_BOOT' "$f74" && why74="${why74:-$(basename "$f74") sets FORCE_DOWNLOAD_BOOT itself}"
  case "$f74" in */boot_guard.c) ;; *) grep -q 'WDT_RWDT\|RWDT_HAL_CONTEXT' "$f74" && why74="${why74:-$(basename "$f74") touches the RTC watchdog}" ;; esac
done
grep -q 'esp_restart' ../../../main/usb_net.c && why74="${why74:-usb_net.c still restarts through esp_restart}"
for cb74 in cdc_line_coding_cb cdc_line_state_cb; do
  [ "$(awk "/^static void ${cb74}\\(/,/^}\$/" ../../../main/usb_net.c | grep -c 'warthog_enter_download();')" = 1 ] || \
    why74="${why74:-$cb74 does not enter through warthog_enter_download}"
done
awk '/^static void cmd_dlmode\(void\)$/,/^}$/' ../../../main/at.c | grep -q 'warthog_enter_download();' || \
  why74="${why74:-AT+DLMODE does not enter through warthog_enter_download}"
awk '/^static const char \*reset_reason_name_/,/^}$/' ../../../main/at.c | tr -d ' \n' | \
  grep -q '{if(rr==ESP_RST_WDT&&warthog_boot_download_return()){return"DLMODE";}switch(rr){' || \
  why74="${why74:-AT+ASSERT? does not name a download-mode return DLMODE}"
rd74=$(grep '^| `AT+DLMODE`' ../../../wiki/AT-Command-Reference.md)
for k74 in "$back74 s" 'reset=DLMODE' 'BOOT button has no limit'; do
  case "$rd74" in *"$k74"*) ;; *) why74="${why74:-the AT+DLMODE row does not say $k74}" ;; esac
done
grep -q "$back74 s" ../../../wiki/Flashing.md || why74="${why74:-Flashing does not give the $back74 s download-mode limit}"
grep '^| `AT+ASSERT?`' ../../../wiki/AT-Command-Reference.md | grep -q '`DLMODE` after' || \
  why74="${why74:-the AT+ASSERT? reset list does not name DLMODE}"
grep -q '^## Mesh: a node stays peered but answers nothing$' ../../../wiki/Troubleshooting.md || \
  why74="${why74:-Troubleshooting does not cover a node left in download mode}"
if [ -z "$why74" ]; then
  ok "ROM download mode: one entry (chip reset, boot guard marked, RTC watchdog armed to reset the RTC domain after ${back74} s, USB reset exemption cleared, off the bus 100 ms, then the flag and a core reset), used by AT+DLMODE and both devloop triggers; no esp_restart; only boot_guard.c and dlmode.c touch the RTC watchdog; the return named DLMODE; documented"
else
  bad "download mode: $why74"
fi

[ $fail -eq 0 ] && echo "GLUE INVARIANTS OK" || echo "GLUE INVARIANTS FAILED"
exit $fail
