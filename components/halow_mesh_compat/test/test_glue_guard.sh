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
  /umac_mesh_preq_gate_allow\(/ && depth<1 { print "UNLOCKED:" NR }
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
if awk '/umac_mesh_fwd_glue_tx_pending/,/^}/' "$G" | \
   awk '/s_pend_umacd = umacd/ {seen=NR} /umac_mesh_pending_push/ {if (!seen) bad=1} END {exit bad?1:0}'; then
  ok "s_pend_umacd is set before the frame is pushed"
else
  bad "s_pend_umacd is set after umac_mesh_pending_push: the first held frame of a boot can be skipped"
fi

# 5. Held frames must also be reclaimed on the periodic tick, not only when a
#    path-selection frame happens to arrive.
if grep -qE '^[[:space:]]*umac_mesh_fwd_glue_tick\(\);' "$M" && grep -q 'void umac_mesh_fwd_glue_tick' "$G"; then
  ok "held frames are also flushed from the service tick"
else
  bad "nothing flushes held frames periodically: an unanswered discovery parks TX buffers"
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

[ $fail -eq 0 ] && echo "GLUE INVARIANTS OK" || echo "GLUE INVARIANTS FAILED"
exit $fail
