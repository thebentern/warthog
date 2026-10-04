/*
 * Host TX fragmentation (AT+HOSTFRAG) through the real transmit path, read off what the firmware
 * hands the chip and what the chip then puts on the air (fake_chip.c seals a HW_ENC frame under
 * the key it holds, at that key's next TX PN, when it sends it).
 *
 * On air on 2026-10-01, against two OpenMANET 1.8.0 nodes (MM6108, chip firmware 2.0.1), every
 * frame the Warthog's own chip (firmware 1.17.6) fragmented was lost at the Linux node: with
 * host CCMP the chip split an MPDU the host had already sealed (MIC fail +3 a frame), with chip
 * crypto only the first fragment was decrypted. The chip fragments by itself whatever is too long
 * for the rate it is given (1 MHz MCS0-1 for a full-size frame), or longer than AT+FRAG. The host
 * now cuts such an MSDU itself, as mac80211 does when the driver does not fragment (tx.c
 * ieee80211_tx_h_fragment, ieee80211_fragment): one sequence number, fragment numbers 0..n-1,
 * More Fragments on all but the last, the MAC header and QoS Control copied to each, Mesh Control
 * (and Address Extension) in the first body only, each fragment protected on its own -- host
 * CCMP seals each at n consecutive PNs reserved at once, the chip seals each at its own next PN --
 * and each small enough that the chip does not cut it again.
 *
 * Each case marked (red) failed on the tree before the change it covers: 35dd251, which has no
 * host fragmentation, for all of them; the first host-fragmentation patch too for those its review
 * added; host fragmentation before the Block Ack rule for (7) and (19); the Block Ack rule before
 * the DELBA wait and its counters for (7)'s wait and hold count, (7i)-(7l) and (19)'s unsent.
 * (pin) marks behaviour that must not change and passed there too. "mac80211's rules"
 * are ieee80211_rx_h_defragment's (rx.c): consecutive PNs under one key, fragment numbers in
 * order, one sequence number and TID, the first fragment's frame type and addresses, no
 * A-MSDU, unicast only; checked here on the air copies, each fragment opened under the link's
 * key with its own header as the AAD. A rate's cap is the longest MPDU the chip sends whole at
 * it: 511 S1G data symbols less SERVICE, tail and an A-MPDU delimiter, less the 36 octets Morse's
 * Linux driver keeps a 1 MHz MCS0 beacon below (beacon.c FRAGMENTATION_OVERHEAD); re-derived
 * here, pinned in test_mesh_frag.
 *  (0) (pin) AT+HOSTFRAG=0 is byte-identical to 35dd251: unicast, a TID 5 frame, group
 *      replicas, batman unicast and AE-2 replicas and a relayed frame, at a rate and under a chip
 *      threshold that would cut them, hash the same frames, flags, keys and rate-control calls;
 *  (1) (red) auto at 1 MHz MCS0: a 1000- and a 1500-octet MSDU go out as 2 and 3 fragments, each
 *      MPDU within 720 octets on air (the rate's cap), the shape above, mac80211's rules met, and
 *      the Warthog's own receive path (datapath_defrag) delivers each byte-identical, once;
 *  (2) auto where every rate carries it: 2 MHz MCS0 and 1 MHz MCS2 send 1500 octets whole (pin);
 *      at 1 MHz MCS0 an MPDU of exactly 720 octets goes whole and one octet more in two (red);
 *  (3) (red) auto under the chip's own threshold (AT+FRAG=512) at 2 MHz MCS7: every MPDU, CCMP and
 *      FCS included, within 512; with no threshold that rate sends it whole (pin);
 *  (4) (red) =512 cuts as Linux 'iw phy set frag 512' does: 476 body octets a fragment (MAC
 *      header, body and FCS 512 before encryption); =2346 at 2 MHz MCS7 sends 1500 whole (pin);
 *  (5) (red) the cut is sized for the slower of the chain's first two rates (rate control's best
 *      and the next, or a lookaround and the best), so no fragment loses a retry: with rate
 *      control's own chain for a link at 1 MHz MCS1 (MCS1, then MCS0 three times, one attempt
 *      each), 1500 octets go as 3 fragments within MCS0's cap, each keeping all four attempts; a
 *      first fragment not acked at MCS1 but acked at its second attempt makes the MSDU ok; rate
 *      control is asked once per MSDU, with the size it was asked before (pin);
 * (5b) (red) no rate in any frame's chain is one the chip would fragment it at: with rate
 *      control's chains on a 2 MHz channel (dropping to 1 MHz MCS0) and a 1 MHz one, a later rate
 *      that cannot carry the frame gives its attempts to the slowest rate that can, so a good link
 *      (2 MHz MCS1 or better, 1 MHz MCS2 or better) is not cut for its last-resort rate; off keeps
 *      the chain as it was (pin);
 *  (6) (red) each fragment's TX status reaches rate control with that fragment's own chain and
 *      attempts; acked fragments make an MSDU ok, one not acked makes it fail;
 *  (7) (red) Block Ack: mac80211 never fragments under A-MPDU; it sends the frame whole (tx.c
 *      ieee80211_tx_prepare). Its recipient ends its session at a fragment numbered 1 or more (rx.c
 *      ieee80211_rx_reorder_ampdu). So the Warthog, whose chip would fragment such a frame, first
 *      ends the session with a DELBA as mac80211 sends one: originator, that TID, reason 37,
 *      marked so its TX status comes back, counted delba_end; the session gone, the peer's other
 *      TIDs and other peers untouched. mac80211 acts on that DELBA in deferred work, so the MSDU
 *      and the peer's next frame (another TID) wait for the DELBA's TX status and 20 ms more while
 *      another peer's go; then the cut, no fragment as A-MPDU, counted ba_end and ba_wait,
 *      reassembled; AT+HOSTFRAG? shows the TID held for 15 s (hold_ms 15000 before the mesh has
 *      run, too); while held the next cut goes at once with no second DELBA, and neither it nor 5
 *      small frames after it send an ADDBA, hold counting the one kept back; a frame that needs
 *      cutting every 10 s for 60 s keeps it held (no ADDBA, no DELBA); 15 s after the last one a
 *      small frame starts a session again, and the next frame over the limit ends it again; a
 *      session requested and not answered: the DELBA, its ADDBA retry cancelled, a late response
 *      opening nothing; no session yet: the first cut goes at once and sends no ADDBA; over the
 *      limit but no buffer for its fragments: whole, the session ended first, not as A-MPDU;
 *      (pin) AT+HOSTFRAG=0 keeps the session and sends it whole as A-MPDU;
 * (7i) (red) the wait: a DELBA whose TX status never comes (a queue flush) releases the MSDU at
 *      500 ms, counted ba_late; one handed back untried by the chip or dropped by the driver
 *      still reports, and the MSDU goes 20 ms later; a peer removed while its MSDU waits leaves
 *      nothing allocated or armed and gets nothing; another peer's frames never wait;
 * (7j) (red) a peer that answers each ADDBA 150 ms later, after the first one's 100 ms timeout:
 *      one ADDBA and one DELBA a hold period, the ADDBA backoff kept across our stop (counted
 *      addba_tx, delba_end, no delba_to);
 * (7k) (red) a DELBA that cannot be built: the session still ends, the MSDU goes at once, counted
 *      nodelba, not ba_end; the peer's own DELBA for our session (recipient, reason 38, as a
 *      Linux node sends when a fragment ends its session) counted rx_delba with its reason, no
 *      ADDBA following while held; (pin) a DELBA for a session the peer originated is not counted;
 * (7l) (red) a fragment the chip reports sent in an A-MPDU is counted agg;
 *  (8) group frames: AT+MESHGRP=1 sends a 1500-octet broadcast as one group frame (pin); with
 *      replication (=0) each peer's unicast copy is cut on its own, its own sequence number and
 *      PNs, and each peer's copy reassembles (red);
 *  (9) (red) relay: a frame from A for C, and one from a host behind A (AE 2), relayed at 1 MHz
 *      MCS0, go to C as fragments whose first carries the relayed Mesh Control (TTL - 1, AE)
 *      and which C delivers byte-identical to the same frame relayed whole;
 * (10) (red) batman: a unicast batman frame and a batman broadcast (AE-2 replicas) are cut and
 *      reach the receiver's extended hook byte-identical to the frame sent whole;
 * (11) (red) QoS: a TID 5 MSDU's fragments carry TID 5 and take TID 5's sequence space;
 * (12) (red) sequence numbers: one per MSDU, fragments share it, the next MSDU takes the next;
 * (13) PNs: (red) host CCMP reserves a fragmented MSDU's PNs at once, so path selection taking a
 *      PN from the same counter on another task lands outside the run, and no PN is ever used
 *      twice; (red) the chip's PNs for the fragments are consecutive; chip crypto (red): a frame
 *      to the peer on another access category, and a chip-sealed management frame to it (its
 *      ADDBA, path selection), wait until the run's TX statuses are back, so a chip that sends
 *      them between two fragments cannot break the run, which reassembles; on a STA chip VIF,
 *      whose one pairwise PN counter serves every link, a frame to another peer waits too;
 *      (pin) a frame on the run's own access category queues behind it at once, and on a MESH
 *      VIF (a counter per key) so does a frame to another peer;
 * (14) (red) a fragment buffer that cannot be had sends the MSDU whole (pool) rather than
 *      dropping it, and nothing is left allocated; =0 takes one buffer a frame, as before (pin);
 *      on the firmware's 20-block TX pool (red): an MSDU whose fragments the pool cannot hold goes
 *      whole, never dropped; MSDUs queued faster than the pool frees wait for their fragments'
 *      blocks and go cut; a producer that waits for the pool, as the netif does, has every MSDU
 *      cut (the pool keeps blocks back for fragments while auto is on); off: unchanged (pin);
 * (15) (red) AT+HOSTFRAG applies to the next frame, either way;
 * (16) (red) on an open mesh and a keyed non-SAE mesh too;
 * (17) (red) chip crypto: the host's PN count for a link never falls behind the chip's, also when
 *      the chip cuts a frame the host sent whole (AT+FRAG with AT+HOSTFRAG=0), so a
 *      re-install of the key lies above every PN the chip drew;
 * (18) (red) a fragment's TX status comes back even when the chip never tried it, handed back
 *      by the chip or dropped by the driver (skbq.c), so nothing waits on it and nothing is
 *      counted as overlap; a run whose statuses never come is cleared after 16 s (stale);
 * (19) AT+AMPDU: (red) =0 ends every originator session within a service tick, a DELBA each
 *      (counted ended, orig 0); one whose DELBA cannot be built ends too, counted unsent, not
 *      ended; it starts none on TIDs 0-5; (pin) the recipient side is unchanged (a peer's ADDBA
 *      Request is accepted); (red) =1 starts one again; (pin) =0 and =1 hand the chip the same
 *      frames but for Block Ack frames and the A-MPDU flag;
 * (20) (pin) AT+HOSTFRAG=0 under Block Ack (sessions to two peers, unicast at 1 MHz MCS0 under
 *      AT+FRAG=512, a group frame) is byte-identical to the tree before (7)'s rule, host-sealed
 *      frames compared opened (their PNs depend on the order of starts).
 * (22) Block Ack fields in the descriptor (AT+TIDPARAMS). On air on 2026-10-03 AT+HOSTFRAG? showed
 *      agg (fragments the chip reported sent in an A-MPDU) rising with ba_end 0 while AT+AMPDU?
 *      showed orig 2. Morselib fills every unicast frame's reorder size (tid_params) from the
 *      peer's session to us, so a fragment to a peer that holds one carries a Block Ack field with
 *      no session of ours to end; morse_driver sets it only under its own agreed session. Against
 *      a chip that aggregates on any such field: (red) with =0 (morselib's rule) and sessions to C
 *      (orig 1) and from A, a frame cut for A ends nothing (ba_end 0, ba_rcpt 1) and its fragments
 *      carry A's reorder size and go aggregated (agg > 0); with =1 they carry none and agg stays 0;
 *      whole frames under our session to C carry the A-MPDU flag with no reorder size under =0
 *      (pin) and its agreed size under =1 (red), to A A's size under =0 (pin) and none under =1
 *      (red); AT+HOSTFRAG=0 with AT+AMPDU=1 keeps morselib's rule (pin), AT+AMPDU=0 drops it (red);
 * (24) (red) every fragment carries the flags the connection gives every frame (traveling pilots,
 *      1 MHz control responses), as the frame sent whole does (pin).
 * (25) (red) a DELBA the chip gives up on unacked still ends the wait, 20 ms later, and is counted
 *      delba_noack (the Linux recipient may still hold its session; an acked or untried one is not).
 * (26) A peer that ends its own session to us (its DELBA) leaves morselib's reorder size behind:
 *      (pin) a whole frame to it still carries it under AT+HOSTFRAG=0; (red) a cut under
 *      AT+TIDPARAMS=0, whose fragments carry that stale size, counts ba_rcpt.
 *
 * Builds: test_simnode_hostfrag (warthog-mesh-sae, the chip seals), _meshvif
 * (warthog-mesh-sae-meshvif), _swccmp and _swccmp_meshvif (host CCMP seals), each with
 * WARTHOG_MESH_HOSTFRAG_ANY: up to 16 fragments, and host CCMP cutting too. The shipping builds
 * cut in at most 2, and not at all with host CCMP (chip firmware 1.17.6, measured on air
 * 2026-10-03); test_simnode_hostfrag_rule covers that rule.
 */
#ifndef WARTHOG_MESH_HOSTFRAG_ANY
#error "built with WARTHOG_MESH_HOSTFRAG_ANY (Makefile HOSTFRAG_ANY)"
#endif
#include <stdio.h>
#include <string.h>

#include "simnode.h"
#include "mmdrv.h"
#include "mmwlan.h"
#include "mmpkt.h"
#include "umac/data/umac_data.h"
#include "umac/datapath/umac_datapath.h"
#include "umac/datapath/umac_datapath_private.h"
#include "umac/ba/umac_ba.h"
#include "umac/keys/umac_keys.h"
#include "umac/mesh/umac_mesh.h"
#include "umac/mesh/umac_mesh_ccm.h"
#include "umac/mesh/umac_mesh_ccmp_hdr.h"
#include "umac/mesh/umac_mesh_ctrl.h"
#include "umac/mesh/umac_mesh_frag.h"
#include "umac/mesh/umac_mesh_ies.h"

#if defined(WARTHOG_MESH_HOST_CCMP)
#define HOST_SEALS 1 /* host CCMP seals every unicast; the chip holds no MTK */
#else
#define HOST_SEALS 0 /* the chip seals each frame under the MTK it holds at the peer's AID */
#endif
#ifndef WARTHOG_MESH_CHIP_VIF_MESH
#define WARTHOG_MESH_CHIP_VIF_MESH 0
#endif

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

extern volatile uint32_t g_warthog_hostfrag, g_warthog_hostfrag_msdu, g_warthog_hostfrag_frags,
    g_warthog_hostfrag_by_thresh, g_warthog_hostfrag_by_chip, g_warthog_hostfrag_by_rate,
    g_warthog_hostfrag_ba_end, g_warthog_hostfrag_nodelba, g_warthog_hostfrag_ba_wait,
    g_warthog_hostfrag_ba_late, g_warthog_hostfrag_agg, g_warthog_hostfrag_hold, g_warthog_hostfrag_held,
    g_warthog_hostfrag_hold_ms, g_warthog_hostfrag_many,
    g_warthog_hostfrag_seal, g_warthog_hostfrag_drv,
    g_warthog_hostfrag_acked, g_warthog_hostfrag_noack, g_warthog_hostfrag_unsent,
    g_warthog_hostfrag_ok, g_warthog_hostfrag_fail, g_warthog_hostfrag_overlap,
    g_warthog_hostfrag_last_n, g_warthog_hostfrag_last_lim, g_warthog_hostfrag_pool,
    g_warthog_hostfrag_wait, g_warthog_hostfrag_mgmt, g_warthog_hostfrag_stale,
    g_warthog_hostfrag_chippn, g_warthog_hostfrag_trim;
extern volatile uint32_t g_warthog_host_ccmp_on, g_warthog_mesh_pmf, g_warthog_sealfit;
extern volatile uint32_t g_warthog_defrag_in, g_warthog_defrag_ok, g_warthog_defrag_pn,
    g_warthog_defrag_nofirst, g_warthog_defrag_order, g_warthog_defrag_key, g_warthog_defrag_hdr;
extern volatile uint32_t g_warthog_rxdrop_reason;
extern volatile uint32_t g_warthog_ampdu, g_warthog_ampdu_orig, g_warthog_ampdu_ended, g_warthog_mesh_seq;
extern volatile uint32_t g_warthog_ampdu_unsent, g_warthog_ba_addba_tx, g_warthog_ba_delba_to,
    g_warthog_ba_delba_end, g_warthog_ba_delba_other, g_warthog_ba_rx_delba, g_warthog_ba_rx_reason;
extern volatile uint32_t g_warthog_tx_nokey;
extern volatile uint32_t g_warthog_ba_txparm, g_warthog_hostfrag_ba_rcpt, g_warthog_hostfrag_delba_noack;
extern volatile uint32_t g_warthog_ampdu_peer_mac[4], g_warthog_ampdu_peer_ba[4];

static const uint8_t W[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 }; /* us */
static const uint8_t A[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0a }; /* the Linux node */
static const uint8_t C[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0c }; /* another peer */
static const uint8_t H2[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x88 }; /* a host behind A */
static const uint8_t H3[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x93 }; /* a host behind C */
static const uint8_t BC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static const uint8_t K_OWN[16] = { 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
                                   0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f };
static const uint8_t K_RX[16]  = { 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
                                   0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f };
static const uint8_t K_A[16]   = { 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
                                   0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf };
static const uint8_t K_C[16]   = { 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
                                   0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf };

/* The longest MPDU the chip sends whole at a rate (test_mesh_frag pins the arithmetic). */
#define CAP_1M_MCS0  720u
#define CAP_1M_MCS1  1488u
#define CAP_1M_MCS10 340u

/* The same, re-derived: 511 data symbols of N_DBPS bits, less SERVICE and tail (22 bits), to a
 * whole 4-octet word, less an A-MPDU delimiter (4) and Morse's 36. */
static uint32_t cap_(uint8_t bw_mhz, uint8_t mcs)
{
    static const uint8_t k6[10] = { 3, 6, 9, 12, 18, 24, 27, 30, 36, 40 };
    const uint32_t nsd = bw_mhz == 1u ? 24u : bw_mhz == 2u ? 52u : bw_mhz == 4u ? 108u : 234u;
    const uint32_t ndbps = mcs == 10u ? nsd / 4u : nsd * k6[mcs % 10u] / 6u;
    return ((((511u * ndbps - 22u) / 8u) & ~3u) - 4u) - 36u;
}

static const struct simnode_rate R_1M0[]  = { { 1, 0, 2 } };
static const struct simnode_rate R_1M2[]  = { { 1, 2, 2 } };
static const struct simnode_rate R_2M0[]  = { { 2, 0, 2 } };
static const struct simnode_rate R_2M7[]  = { { 2, 7, 2 }, { 2, 5, 2 }, { 2, 2, 2 }, { 2, 0, 2 } };
/* Rate control's own chains (mmrc.c mmrc_fill_retry_rates): best throughput, one MCS lower, two
 * lower, then the baseline, the bandwidth dropping to 1 MHz once the MCS reaches 0; at these
 * rates every entry gets one attempt (a 1200-octet frame takes longer than its 4 ms window). */
static const struct simnode_rate R_RC_1M1[] = { { 1, 1, 1 }, { 1, 0, 1 }, { 1, 0, 1 }, { 1, 0, 1 } };
static const struct simnode_rate R_RC_1M2[] = { { 1, 2, 1 }, { 1, 1, 1 }, { 1, 0, 1 }, { 1, 0, 1 } };
static const struct simnode_rate R_RC_2M0[] = { { 2, 0, 1 }, { 1, 0, 1 }, { 1, 0, 1 }, { 1, 0, 1 } };
static const struct simnode_rate R_RC_2M1[] = { { 2, 1, 1 }, { 2, 0, 1 }, { 1, 0, 1 }, { 1, 0, 1 } };
static const struct simnode_rate R_RC_2M2[] = { { 2, 2, 1 }, { 2, 1, 1 }, { 2, 0, 1 }, { 1, 0, 1 } };
/* A good 1 MHz link: its baseline is still 1 MHz MCS0, and the best rate gets 2 attempts. */
static const struct simnode_rate R_RC_1M7[] = { { 1, 7, 2 }, { 1, 6, 1 }, { 1, 5, 1 }, { 1, 0, 1 } };
#define SET_RATES(r) simnode_set_rate_chain((r), sizeof(r) / sizeof((r)[0]))

#define MAC_EQ(p, m) (memcmp((p), (m), 6) == 0)
#define HDR 32u /* 4-address QoS data header */

/* ---- counters ---------------------------------------------------------------------------- */

struct hf {
    uint32_t msdu, frags, by_thresh, by_chip, by_rate, ba_end, nodelba, ba_wait, ba_late, agg, hold,
             many, seal, drv, acked, noack, unsent, ok, fail, overlap, pool, wait, mgmt, stale, chippn,
             trim;
};
static struct hf hf_(void)
{
    struct hf s = { g_warthog_hostfrag_msdu, g_warthog_hostfrag_frags,
                    g_warthog_hostfrag_by_thresh, g_warthog_hostfrag_by_chip,
                    g_warthog_hostfrag_by_rate, g_warthog_hostfrag_ba_end, g_warthog_hostfrag_nodelba,
                    g_warthog_hostfrag_ba_wait, g_warthog_hostfrag_ba_late, g_warthog_hostfrag_agg,
                    g_warthog_hostfrag_hold, g_warthog_hostfrag_many,
                    g_warthog_hostfrag_seal, g_warthog_hostfrag_drv,
                    g_warthog_hostfrag_acked, g_warthog_hostfrag_noack,
                    g_warthog_hostfrag_unsent, g_warthog_hostfrag_ok, g_warthog_hostfrag_fail,
                    g_warthog_hostfrag_overlap, g_warthog_hostfrag_pool, g_warthog_hostfrag_wait,
                    g_warthog_hostfrag_mgmt, g_warthog_hostfrag_stale, g_warthog_hostfrag_chippn,
                    g_warthog_hostfrag_trim };
    return s;
}
#define HD(s, f) ((unsigned)(g_warthog_hostfrag_##f - (s).f))

/* ---- the nodes --------------------------------------------------------------------------- */

static uint64_t s_pn_a = 1; /* A's next PN toward us, for frames we relay */
static uint32_t s_mseq_a = 5000;

/* W on an SAE mesh with keyed peers A and C; @p fwd the relay gate. AT+HOSTFRAG off. */
static void up_(bool fwd)
{
    while (simnode_tx_held() != 0u) { (void)simnode_tx_forget_held(0); }
    simnode_tx_pool(false);
    simnode_chip_shared_pairwise_pn(false);
    simnode_tx_retry_next(0);
    simnode_tx_noack_next(0);
    simnode_tx_noack_mgmt_next(0);
    simnode_tx_aggregated_next(0);
    g_warthog_mesh_pmf = 0;
    simnode_del_peer(NULL);
    simnode_set_batman(false);
    simnode_set_ampdu(false);
    g_warthog_ampdu = 1;
    simnode_tx_hold(false);
    (void)simnode_start_sae(W);
    simnode_set_gates(fwd, /*bridge=*/false, /*grp_std=*/false, /*secure=*/true);
    g_warthog_host_ccmp_on = HOST_SEALS;
    g_warthog_mesh_pmf = 0;
    simnode_set_rx_ext_cb(true);
    (void)simnode_set_key(BC, K_OWN, 1, /*pairwise=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    (void)simnode_set_key(A, K_A, 0, /*pairwise=*/true);
    (void)simnode_set_key(C, K_C, 0, /*pairwise=*/true);
    g_warthog_hostfrag = 0;
    g_warthog_sealfit = 0; /* whole host-sealed frames keep their chain; the rule suite tests the fit */
    simnode_set_rate_chain(NULL, 0);
    simnode_set_chip_frag_threshold(0);
    simnode_outbox_clear();
    simnode_ext_rx_clear();
    simnode_rc_clear();
    s_pn_a = 1;
}

/* @p me receiving from @p peer over link key @p key, a leaf, batman mode @p batman. */
static void as_(const uint8_t *me, const uint8_t *peer, const uint8_t *key, bool batman)
{
    while (simnode_tx_held() != 0u) { (void)simnode_tx_forget_held(0); }
    simnode_tx_pool(false);
    simnode_chip_shared_pairwise_pn(false);
    simnode_del_peer(NULL);
    simnode_tx_hold(false);
    simnode_set_ampdu(false);
    g_warthog_ampdu = 1;
    simnode_set_batman(batman);
    (void)simnode_start_sae(me);
    simnode_set_gates(false, false, false, true);
    g_warthog_host_ccmp_on = HOST_SEALS;
    simnode_set_rx_ext_cb(true);
    (void)simnode_set_key(BC, K_RX, 1, false);
    (void)simnode_add_peer(peer);
    (void)simnode_set_key(peer, key, 0, true);
    g_warthog_hostfrag = 0;
    simnode_set_rate_chain(NULL, 0);
    simnode_outbox_clear();
    simnode_ext_rx_clear();
}

static void pay_(uint8_t *p, uint16_t n, uint8_t salt)
{
    for (uint16_t i = 0; i < n; i++) { p[i] = (uint8_t)(i * 13u + salt); }
}

/* ---- what reached the chip and the air ---------------------------------------------------- */

struct cap {
    uint8_t  air[1700];
    uint16_t len;      /* air_len: as it left the chip */
    uint8_t  bytes[1700];
    uint16_t blen;     /* as the host handed it to the chip */
    uint8_t  tx_flags, key_idx, tid, status, attempts, pn_draws, reorder;
    bool     sent, is_mgmt;
    struct simnode_rate chain[4];
};
#define CAP_MAX 24u
struct caps { struct cap f[CAP_MAX]; unsigned n; };

/* The data frames to @p ra from outbox entry @p from on, in the order handed to the chip. */
static void capture_(struct caps *c, const uint8_t *ra, unsigned from)
{
    c->n = 0;
    for (unsigned i = from; i < simnode_outbox_count() && c->n < CAP_MAX; i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (f == NULL || f->is_mgmt || f->len < 24u || !MAC_EQ(f->bytes + 4, ra)) { continue; }
        struct cap *e = &c->f[c->n++];
        memset(e, 0, sizeof(*e));
        memcpy(e->air, f->air, f->air_len);
        e->len = f->air_len;
        memcpy(e->bytes, f->bytes, f->len);
        e->blen = f->len;
        e->tx_flags = f->tx_flags;
        e->key_idx = f->key_idx;
        e->tid = f->tid;
        e->status = f->status_flags;
        e->attempts = f->attempts;
        e->sent = f->sent;
        e->pn_draws = f->pn_draws;
        e->reorder = f->reorder;
        memcpy(e->chain, f->chain, sizeof(e->chain));
    }
}

/* Every rate in each frame's chain carries that frame's MPDU (FCS included) whole. */
static bool carries_(const struct caps *c)
{
    for (unsigned i = 0; i < c->n; i++)
    {
        for (unsigned r = 0; r < 4u; r++)
        {
            const struct simnode_rate *e = &c->f[i].chain[r];
            if (e->attempts != 0u && cap_(e->bw_mhz, e->mcs) < (uint32_t)c->f[i].len + 4u) { return false; }
        }
    }
    return true;
}

/* Attempts a frame's chain allows. */
static unsigned budget_(const struct cap *e)
{
    unsigned n = 0;
    for (unsigned r = 0; r < 4u; r++) { n += e->chain[r].attempts; }
    return n;
}

/* The host's TX PN count for @p peer's pairwise key, and the chip's next PN under it. */
static uint64_t host_pn_(const uint8_t *peer)
{
    struct umac_sta_data *stad = umac_datapath_mesh_find_peer(peer);
    return stad != NULL ? umac_keys_get_tx_seq(stad, UMAC_KEY_TYPE_PAIRWISE) : 0u;
}
static uint64_t chip_pn_(const uint8_t *peer)
{
    struct umac_sta_data *stad = umac_datapath_mesh_find_peer(peer);
    uint64_t pn = 0;
    if (stad != NULL) { (void)simnode_chip_key_next_pn(umac_sta_data_get_aid(stad), true, 0, &pn); }
    return pn;
}

static uint64_t pn_of_(const uint8_t *f)
{
    uint8_t pn[6], kid = 0;
    if (!umac_ccmp_parse_header(f + HDR, pn, &kid)) { return UINT64_MAX; }
    uint64_t v = 0;
    for (int i = 0; i < 6; i++) { v = (v << 8) | pn[i]; }
    return v;
}

/* As mac80211 takes @p n MPDUs off the air (rx.c ieee80211_rx_h_decrypt, then
 * ieee80211_rx_h_defragment): NULL if they make one MSDU, its body (Mesh Control onward) in
 * @p body; else why not. @p key NULL: an open link, nothing protected. */
static const char *mac80211_rx_(const struct cap *f, unsigned n, const uint8_t key[16],
                                uint8_t *body, uint16_t *blen)
{
    const uint16_t sec = key != NULL ? 16u : 0u;
    static char why[96];
    uint16_t off = 0;
    uint64_t prev = 0;
    uint8_t kid0 = 0;
    for (unsigned i = 0; i < n; i++)
    {
        const uint8_t *h = f[i].air;
        const uint16_t len = f[i].len;
        const uint16_t fc = (uint16_t)(h[0] | (h[1] << 8));
        const uint16_t sc = (uint16_t)(h[22] | (h[23] << 8));
        const uint16_t sc0 = (uint16_t)(f[0].air[22] | (f[0].air[23] << 8));
        if (len < HDR + sec + 1u) { snprintf(why, sizeof(why), "fragment %u: %u octets", i, len); return why; }
        if (((fc >> 2) & 3u) != 2u || ((fc >> 4) & 0xfu) != 8u) { snprintf(why, sizeof(why), "fragment %u not QoS data", i); return why; }
        if ((fc & 0x0300u) != 0x0300u || (h[4] & 1u) != 0u) { snprintf(why, sizeof(why), "fragment %u not a 4-address unicast", i); return why; }
        if ((sc & 0x0fu) != i || (sc >> 4) != (sc0 >> 4)) { snprintf(why, sizeof(why), "fragment %u: sequence control 0x%04x after 0x%04x", i, sc, sc0); return why; }
        if (((fc & 0x0400u) != 0u) != (i + 1u < n)) { snprintf(why, sizeof(why), "fragment %u: More Fragments %d", i, (fc & 0x0400u) != 0u); return why; }
        if (memcmp(h + 4, f[0].air + 4, 12) != 0 || memcmp(h + 16, f[0].air + 16, 6) != 0 ||
            memcmp(h + 24, f[0].air + 24, 6) != 0) { snprintf(why, sizeof(why), "fragment %u: addresses differ", i); return why; }
        if ((h[30] & 0x0fu) != (f[0].air[30] & 0x0fu) || (h[30] & 0x80u) != 0u || (h[31] & 0x01u) == 0u)
        { snprintf(why, sizeof(why), "fragment %u: QoS 0x%02x%02x", i, h[31], h[30]); return why; }
        if (key == NULL)
        {
            if ((fc & 0x4000u) != 0u) { snprintf(why, sizeof(why), "fragment %u Protected on an open link", i); return why; }
            memcpy(body + off, h + HDR, (size_t)(len - HDR));
            off = (uint16_t)(off + len - HDR);
            continue;
        }
        if ((fc & 0x4000u) == 0u) { snprintf(why, sizeof(why), "fragment %u not Protected", i); return why; }
        uint8_t pn[6], kid = 0, aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
        if (!umac_ccmp_parse_header(h + HDR, pn, &kid)) { snprintf(why, sizeof(why), "fragment %u: no CCMP header", i); return why; }
        const uint64_t pv = pn_of_(h);
        if (i == 0) { kid0 = kid; }
        else if (kid != kid0) { snprintf(why, sizeof(why), "fragment %u: key id %u, first %u", i, kid, kid0); return why; }
        else if (pv != prev + 1u) { snprintf(why, sizeof(why), "fragment %u: PN %llu after %llu", i, (unsigned long long)pv, (unsigned long long)prev); return why; }
        prev = pv;
        const uint32_t al = umac_ccmp_build_aad(h, aad);
        umac_ccmp_build_nonce(h, pn, nonce);
        const uint16_t pl = (uint16_t)(len - HDR - 16u);
        memcpy(body + off, h + HDR + 8u, pl);
        if (warthog_ccm_ad(key, nonce, 8, aad, al, body + off, pl, h + len - 8u) != 0)
        {
            snprintf(why, sizeof(why), "fragment %u: MIC fails under the link key", i);
            return why;
        }
        off = (uint16_t)(off + pl);
    }
    *blen = off;
    return NULL;
}

/* The MSDU body a whole frame carries: Mesh Control (@p mc_len), SNAP + @p etype, @p pay. */
static bool body_is_(const uint8_t *b, uint16_t blen, uint16_t mc_len, uint8_t ae, uint16_t etype,
                     const uint8_t *pay, uint16_t n)
{
    static const uint8_t snap[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };
    return blen == mc_len + 8u + n && (b[0] & 0x03u) == ae && memcmp(b + mc_len, snap, 6) == 0 &&
           b[mc_len + 6] == (uint8_t)(etype >> 8) && b[mc_len + 7] == (uint8_t)etype &&
           memcmp(b + mc_len + 8u, pay, n) == 0;
}

/* Every fragment's on-air MPDU (FCS included) within @p lim, every body but the last even. */
static bool sized_(const struct caps *c, uint32_t lim)
{
    for (unsigned i = 0; i < c->n; i++)
    {
        if ((uint32_t)c->f[i].len + 4u > lim) { return false; }
        if (i + 1u < c->n && ((c->f[i].len - HDR - 16u) & 1u) != 0u) { return false; }
    }
    return true;
}

/* What the host handed the chip for each fragment: QoS bit 8 and @p tid on every one, never the
 * A-MPDU flag, sealed by whoever this build says. */
static bool handed_(const struct caps *c, uint8_t tid)
{
    for (unsigned i = 0; i < c->n; i++)
    {
        const struct cap *e = &c->f[i];
        if ((e->bytes[30] & 0x0fu) != tid || (e->bytes[31] & 0x01u) == 0u || e->tid != tid ||
            (e->tx_flags & MMDRV_TX_FLAG_AMPDU_ENABLED) != 0u || (e->bytes[1] & 0x40u) == 0u)
        {
            return false;
        }
        const bool hw = (e->tx_flags & MMDRV_TX_FLAG_HW_ENC) != 0u;
        if (HOST_SEALS ? hw : (!hw || e->key_idx != 0u)) { return false; }
    }
    return true;
}

/* Feed @p c into this node off the air; the extended hook's deliveries after each in @p after. */
static void rx_caps_(const struct caps *c, unsigned *after)
{
    for (unsigned i = 0; i < c->n; i++)
    {
        (void)simnode_rx_air(c->f[i].air, c->f[i].len, -60);
        if (after != NULL) { after[i] = simnode_ext_rx_count(); }
    }
}

static bool ext_is_(unsigned i, const uint8_t *da, const uint8_t *sa, uint16_t etype,
                    const uint8_t *pay, uint16_t n)
{
    const struct simnode_extrx *x = simnode_ext_rx_get(i);
    return x != NULL && x->len == 14u + n && MAC_EQ(x->frame, da) && MAC_EQ(x->frame + 6, sa) &&
           x->frame[12] == (uint8_t)(etype >> 8) && x->frame[13] == (uint8_t)etype &&
           memcmp(x->frame + 14, pay, n) == 0;
}

static bool ext_same_(unsigned i, unsigned j)
{
    const struct simnode_extrx *a = simnode_ext_rx_get(i), *b = simnode_ext_rx_get(j);
    return a != NULL && b != NULL && a->len == b->len && memcmp(a->frame, b->frame, a->len) == 0;
}

/* ---- a frame from A, off the air ---------------------------------------------------------- */

/* A unicast A sends us for @p da from @p sa, Mesh Control @p mc, sealed under K_A. */
static uint16_t from_a_(uint8_t *f, const uint8_t *da, const uint8_t *sa, const struct umac_mesh_ctrl *mc,
                        const uint8_t *pay, uint16_t n)
{
    static uint16_t seq = 1;
    uint16_t k = umac_mesh_ies_build_data_hdr4(f, W, A, da, sa);
    f[1] |= 0x40u;
    f[22] = (uint8_t)(seq << 4);
    f[23] = (uint8_t)(seq >> 4);
    seq++;
    f[k++] = 0x00;
    f[k++] = 0x01;
    uint8_t pn[6];
    const uint64_t v = s_pn_a++;
    for (int i = 0; i < 6; i++) { pn[i] = (uint8_t)(v >> (8 * (5 - i))); }
    umac_ccmp_write_header(f + k, pn, 0);
    const uint16_t b0 = (uint16_t)(k + 8u);
    uint16_t m = umac_mesh_ctrl_build(f + b0, UMAC_MESH_CTRL_LEN_MAX, mc);
    static const uint8_t snap[8] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00 };
    memcpy(f + b0 + m, snap, 8);
    memcpy(f + b0 + m + 8u, pay, n);
    const uint16_t blen = (uint16_t)(m + 8u + n);
    uint8_t aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
    const uint32_t al = umac_ccmp_build_aad(f, aad);
    umac_ccmp_build_nonce(f, pn, nonce);
    (void)warthog_ccm_ae(K_A, nonce, 8, aad, al, f + b0, blen, f + b0 + blen);
    return (uint16_t)(b0 + blen + 8u);
}

/* ---- (0) off: byte-identical to 35dd251 --------------------------------------------------- */

static uint64_t s_h;
static void h_(const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { s_h = (s_h ^ b[i]) * 1099511628211ull; }
}

static void h_outbox_(void)
{
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        h_(&f->len, sizeof(f->len));
        h_(f->bytes, f->len);
        const uint8_t meta[5] = { f->is_mgmt, f->tx_flags, f->key_idx, f->tid, f->vif_id };
        h_(meta, sizeof(meta));
    }
    const uint32_t rc[2] = { simnode_rc_table_calls(), simnode_rc_last_size() };
    h_(rc, sizeof(rc));
    simnode_outbox_clear();
}

/* Recorded by this scenario on 35dd251 (the MESH chip VIF changes none of these frames). */
#if HOST_SEALS
#define OFF_GOLDEN 0x19531f24d07dc11cull
#else
#define OFF_GOLDEN 0x43cad29786456c5eull
#endif

static void t_off(void)
{
    printf("--- (0) AT+HOSTFRAG=0: byte-identical to 35dd251 ---\n");
    static uint8_t pay[1500], f[1700];
    s_h = 1469598103934665603ull;
    up_(/*fwd=*/true);
    SET_RATES(R_1M0);
    simnode_set_chip_frag_threshold(512);
    static const uint16_t sizes[] = { 64, 700, 1000, 1500 };
    for (unsigned i = 0; i < 4; i++)
    {
        pay_(pay, sizes[i], (uint8_t)i);
        (void)simnode_host_tx(A, W, pay, sizes[i]);
        h_outbox_();
    }
    pay_(pay, 1000, 9);
    (void)simnode_host_tx_tid(C, W, pay, 1000, 5);
    h_outbox_();
    (void)simnode_host_tx(BC, W, pay, 1000);
    h_outbox_();
    struct umac_mesh_ctrl mc = { .flags = 0, .ttl = 31, .seq = ++s_mseq_a };
    uint16_t n = from_a_(f, C, A, &mc, pay, 1000);
    (void)simnode_rx_air(f, n, -60);
    h_outbox_();
    simnode_set_batman(true);
    (void)simnode_host_tx_eth(A, A, W, 0x4305, pay, 1200);
    h_outbox_();
    (void)simnode_host_tx_eth(NULL, BC, W, 0x4305, pay, 600);
    h_outbox_();
    simnode_set_batman(false);
    CHECK(s_h == OFF_GOLDEN,
          "(pin) unicast, TID 5, group replicas, a relayed frame and batman at 1 MHz MCS0 under "
          "AT+FRAG=512: the frames, flags, keys and rate-control calls 35dd251 produced "
          "(0x%016llx, want 0x%016llx)", (unsigned long long)s_h, (unsigned long long)OFF_GOLDEN);
}

/* ---- (1)-(4) where it cuts and how ----------------------------------------------------- */

/* W sends @p n octets to A at rate chain @p r (count @p rn); captured. */
static void send_a_(struct caps *c, uint16_t n, uint8_t salt, uint8_t *pay)
{
    const unsigned from = simnode_outbox_count();
    pay_(pay, n, salt);
    (void)simnode_host_tx(A, W, pay, n);
    capture_(c, A, from);
}

/* @p c, cut from @p n payload octets, reassembled by mac80211's rules and by A's own receive
 * path, byte-identical, delivered once after the last fragment. */
static void reassembles_(const char *what, const struct caps *c, const uint8_t *pay, uint16_t n)
{
    static uint8_t body[3000];
    uint16_t blen = 0;
    const char *why = mac80211_rx_(c->f, c->n, K_A, body, &blen);
    CHECK(why == NULL && body_is_(body, blen, 6, 0, 0x0800, pay, n),
          "  %s: mac80211's rules met, the MSDU body intact (%s)", what, why ? why : "ok");
    as_(A, W, K_A, false);
    const uint32_t in0 = g_warthog_defrag_in, ok0 = g_warthog_defrag_ok;
    unsigned after[CAP_MAX] = { 0 };
    rx_caps_(c, after);
    bool early = false;
    for (unsigned i = 0; i + 1u < c->n; i++) { early = early || after[i] != 0u; }
    CHECK(!early && simnode_ext_rx_count() == 1u && ext_is_(0, A, W, 0x0800, pay, n) &&
              g_warthog_defrag_in - in0 == c->n && g_warthog_defrag_ok - ok0 == 1u,
          "  %s: the Warthog's receive path delivers it once, after the last, byte-identical "
          "(%u delivered, defrag in %u ok %u, rxdrop %lu)", what, simnode_ext_rx_count(),
          (unsigned)(g_warthog_defrag_in - in0), (unsigned)(g_warthog_defrag_ok - ok0),
          (unsigned long)g_warthog_rxdrop_reason);
}

static void t_auto_rate(void)
{
    printf("--- (1) auto at 1 MHz MCS0: cut to fit, mac80211's shape, reassembled ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    static const struct { uint16_t n; unsigned frags; } cases[] = { { 1000, 2 }, { 1500, 3 } };
    for (unsigned k = 0; k < 2; k++)
    {
        up_(false);
        SET_RATES(R_1M0);
        g_warthog_hostfrag = 1;
        const struct hf s = hf_();
        send_a_(&c, cases[k].n, (uint8_t)(0x10 + k), pay);
        CHECK(c.n == cases[k].frags && sized_(&c, CAP_1M_MCS0) && handed_(&c, 0),
              "(red) %u octets: %u fragments (want %u), each MPDU within %u on air, QoS bit 8 and "
              "TID 0 on each, no A-MPDU, sealed by the %s", cases[k].n, c.n, cases[k].frags,
              CAP_1M_MCS0, HOST_SEALS ? "host" : "chip");
        CHECK(HD(s, msdu) == 1u && HD(s, frags) == c.n && HD(s, by_rate) == 1u &&
                  g_warthog_hostfrag_last_n == c.n && g_warthog_hostfrag_last_lim == CAP_1M_MCS0,
              "  counted: msdu %u frag %u by rate %u, last n %lu lim %lu", HD(s, msdu),
              HD(s, frags), HD(s, by_rate), (unsigned long)g_warthog_hostfrag_last_n,
              (unsigned long)g_warthog_hostfrag_last_lim);
        reassembles_(k == 0 ? "1000 octets" : "1500 octets", &c, pay, cases[k].n);
    }
}

static void t_auto_whole(void)
{
    printf("--- (2) auto where the rate carries it: whole ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    up_(false);
    g_warthog_hostfrag = 1;
    SET_RATES(R_2M0);
    send_a_(&c, 1500, 1, pay);
    CHECK(c.n == 1u && c.f[0].len > 1500u, "(pin) 2 MHz MCS0: 1500 octets go whole (%u)", c.n);
    SET_RATES(R_1M2);
    send_a_(&c, 1500, 2, pay);
    CHECK(c.n == 1u, "(pin) 1 MHz MCS2: whole (%u)", c.n);
    SET_RATES(R_1M0);
    const uint16_t fit = (uint16_t)(CAP_1M_MCS0 - HDR - 16u - 4u - 6u - 8u);
    send_a_(&c, fit, 3, pay);
    CHECK(c.n == 1u && c.f[0].len + 4u == CAP_1M_MCS0,
          "(pin) 1 MHz MCS0: an MPDU of exactly %u octets goes whole (%u frames, %u octets)",
          CAP_1M_MCS0, c.n, c.n ? c.f[0].len + 4u : 0u);
    send_a_(&c, (uint16_t)(fit + 1u), 4, pay);
    CHECK(c.n == 2u && sized_(&c, CAP_1M_MCS0) && c.f[1].len == HDR + 16u + 1u,
          "(red) one octet more goes in two, the second carrying 1 (%u frames)", c.n);
    reassembles_("the octet over", &c, pay, (uint16_t)(fit + 1u));
}

static void t_auto_chip(void)
{
    printf("--- (3) auto under the chip's own threshold (AT+FRAG) ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    up_(false);
    g_warthog_hostfrag = 1;
    SET_RATES(R_2M7);
    send_a_(&c, 1000, 5, pay);
    CHECK(c.n == 1u, "(pin) 2 MHz MCS7 with no chip threshold: 1000 octets go whole (%u)", c.n);
    simnode_set_chip_frag_threshold(512);
    const struct hf s = hf_();
    send_a_(&c, 1000, 6, pay);
    CHECK(c.n == 3u && sized_(&c, 512) && HD(s, by_chip) == 1u,
          "(red) AT+FRAG=512: 3 fragments, every MPDU, CCMP and FCS included, within 512 "
          "(%u, by chip %u)", c.n, HD(s, by_chip));
    reassembles_("under AT+FRAG=512", &c, pay, 1000);
}

static void t_thresh(void)
{
    printf("--- (4) =<n>: Linux's threshold ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    up_(false);
    SET_RATES(R_2M7);
    g_warthog_hostfrag = 512;
    const struct hf s = hf_();
    send_a_(&c, 1000, 7, pay);
    /* mac80211: per fragment 512 - 32 (header) - 4 (FCS) = 476 body octets, CCMP not counted.
     * The body is 6 Mesh Control + 8 SNAP + 1000 = 1014: 476 + 476 + 62. */
    bool linux_cut = c.n == 3u;
    for (unsigned i = 0; linux_cut && i < 3u; i++)
    {
        linux_cut = c.f[i].len == HDR + 16u + (i < 2u ? 476u : 62u);
    }
    CHECK(linux_cut && HD(s, by_thresh) == 1u,
          "(red) =512: 476 + 476 + 62 body octets, as 'iw phy set frag 512' cuts (%u, by thresh %u)",
          c.n, HD(s, by_thresh));
    reassembles_("=512", &c, pay, 1000);
    up_(false);
    SET_RATES(R_2M7);
    g_warthog_hostfrag = 2346;
    send_a_(&c, 1500, 8, pay);
    CHECK(c.n == 1u, "(pin) =2346 at 2 MHz MCS7: 1500 octets go whole (%u)", c.n);
}

/* ---- (5)-(6) rate control and TX status --------------------------------------------------- */

static void t_rates(void)
{
    printf("--- (5) the cut is sized for the whole retry chain; rate control asked once ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    up_(false);
    SET_RATES(R_RC_1M1);
    g_warthog_hostfrag = 1;
    simnode_rc_clear();
    send_a_(&c, 1500, 9, pay);
    const unsigned calls = simnode_rc_table_calls();
    const uint32_t size = simnode_rc_last_size();
    bool full = c.n == 3u;
    for (unsigned i = 0; full && i < c.n; i++)
    {
        full = budget_(&c.f[i]) == 4u && c.f[i].chain[0].mcs == 1u && c.f[i].chain[3].mcs == 0u;
    }
    CHECK(c.n == 3u && sized_(&c, CAP_1M_MCS0) && full && carries_(&c),
          "(red) rate control's chain for a link at 1 MHz MCS1 (MCS1, MCS0 x3, one attempt each): "
          "1500 octets go as 3 fragments within MCS0's %u, each keeping all 4 attempts, every rate "
          "carrying it (%u fragments, attempts %u/%u/%u)", CAP_1M_MCS0, c.n,
          c.n > 0u ? budget_(&c.f[0]) : 0u, c.n > 1u ? budget_(&c.f[1]) : 0u,
          c.n > 2u ? budget_(&c.f[2]) : 0u);
    const uint32_t want = HDR + 6u + 8u + 1500u + (HOST_SEALS ? 16u : 0u);
    CHECK(calls == 1u && size == want,
          "(pin) rate control is asked once for the MSDU, with the size it was asked before "
          "(%u calls, %lu octets, want %lu)", calls, (unsigned long)size, (unsigned long)want);
    reassembles_("sized for the chain", &c, pay, 1500);
    up_(false);
    SET_RATES(R_RC_1M1);
    g_warthog_hostfrag = 1;
    const struct hf s = hf_();
    simnode_tx_retry_next(2);
    send_a_(&c, 1500, 25, pay);
    CHECK(c.n == 3u && c.f[0].attempts == 2u && c.f[0].status == 0u && HD(s, ok) == 1u &&
              HD(s, fail) == 0u,
          "(red) the first fragment, not acked at MCS1, is acked at its second attempt (MCS0): "
          "the MSDU is ok (%u fragments, attempts %u, ok %u, fail %u)", c.n,
          c.n ? c.f[0].attempts : 0u, HD(s, ok), HD(s, fail));
    SET_RATES(R_2M0);
    simnode_rc_clear();
    send_a_(&c, 1500, 24, pay);
    CHECK(c.n == 1u && simnode_rc_table_calls() == 1u && simnode_rc_last_size() == want,
          "(pin) and once for one it sends whole, with that size (%u calls, %lu octets)",
          simnode_rc_table_calls(), (unsigned long)simnode_rc_last_size());
}

static void t_whole_chain(void)
{
    printf("--- (5b) no rate in a frame's chain is one the chip would cut it at ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    /* The cut is sized for the slower of the first two rates; a later rate that cannot carry the
     * frame gives its attempts to the slowest rate that can. */
    static const struct { const struct simnode_rate *r; const char *what; unsigned n, budget; } chains[] = {
        { R_RC_2M0, "2 MHz MCS0, then 1 MHz MCS0 x3", 3, 4 },
        { R_RC_2M1, "2 MHz MCS1, MCS0, then 1 MHz MCS0 x2", 1, 4 },
        { R_RC_2M2, "2 MHz MCS2, MCS1, MCS0, then 1 MHz MCS0", 1, 4 },
        { R_RC_1M2, "1 MHz MCS2, MCS1, MCS0 x2", 2, 4 },
        { R_RC_1M7, "1 MHz MCS7 x2, MCS6, MCS5, MCS0", 1, 5 },
    };
    static const uint32_t modes[] = { 1u, 2346u };
    for (unsigned m = 0; m < 2u; m++)
    {
        for (unsigned k = 0; k < sizeof(chains) / sizeof(chains[0]); k++)
        {
            up_(false);
            simnode_set_rate_chain(chains[k].r, 4);
            g_warthog_hostfrag = modes[m];
            send_a_(&c, 1500, (uint8_t)(40u + k), pay);
            unsigned left = 0;
            for (unsigned i = 0; i < c.n; i++) { left += budget_(&c.f[i]) == chains[k].budget; }
            CHECK(c.n == chains[k].n && carries_(&c) && left == c.n,
                  "(red) %s, chain %s: 1500 octets as %u frame(s) (want %u), every rate in each "
                  "one's chain carrying it, each keeping %u attempts", modes[m] == 1u ? "auto" : "=2346",
                  chains[k].what, c.n, chains[k].n, chains[k].budget);
        }
    }
    up_(false);
    SET_RATES(R_RC_2M0);
    send_a_(&c, 1500, 44, pay);
    CHECK(c.n == 1u && budget_(&c.f[0]) == 4u && c.f[0].chain[3].mcs == 0u && c.f[0].chain[3].attempts == 1u,
          "(pin) off: the frame goes whole with the chain rate control gave it (%u)", c.n);
}

static void t_status(void)
{
    printf("--- (6) TX status: rate control per fragment, the MSDU's outcome ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    up_(false);
    SET_RATES(R_1M0);
    g_warthog_hostfrag = 1;
    struct hf s = hf_();
    simnode_rc_clear();
    send_a_(&c, 1500, 10, pay);
    bool own = simnode_rcfb_count() == c.n && c.n == 3u;
    for (unsigned i = 0; own && i < c.n; i++)
    {
        const struct simnode_rcfb *e = simnode_rcfb_get(i);
        own = e->aid == 1u && e->attempts == 1u && e->status_flags == 0u &&
              e->chain[0].bw_mhz == 1u && e->chain[0].mcs == 0u;
    }
    CHECK(own && HD(s, acked) == 3u && HD(s, ok) == 1u && HD(s, fail) == 0u,
          "(red) 3 fragments, 3 statuses to rate control, each its own; acked %u, msdu ok %u",
          HD(s, acked), HD(s, ok));
    s = hf_();
    simnode_rc_clear();
    simnode_tx_noack_next(1);
    send_a_(&c, 1500, 11, pay);
    const struct simnode_rcfb *e0 = simnode_rcfb_get(0);
    CHECK(c.n == 3u && e0 != NULL && (e0->status_flags & MMDRV_TX_STATUS_FLAG_NO_ACK) != 0u &&
              e0->attempts == 2u && HD(s, noack) == 1u && HD(s, acked) == 2u &&
              HD(s, fail) == 1u && HD(s, ok) == 0u,
          "(red) the first not acked after its 2 attempts: noack %u, msdu fail %u", HD(s, noack),
          HD(s, fail));
}

/* ---- (7)-(8) Block Ack and group frames ---------------------------------------------------- */

/* @p from's NDP ADDBA Response to us: success, immediate policy, @p tid, 16 frames. */
static uint16_t addba_resp_(uint8_t *f, const uint8_t *from, uint8_t token, uint8_t tid)
{
    memset(f, 0, 33);
    f[0] = 0xd0;
    memcpy(&f[4], W, 6);
    memcpy(&f[10], from, 6);
    memcpy(&f[16], from, 6);
    const uint16_t ps = (uint16_t)((1u << 1) | ((uint16_t)tid << 2) | (16u << 6));
    f[24] = 3;   /* Block Ack */
    f[25] = 129; /* NDP ADDBA Response */
    f[26] = token;
    f[29] = (uint8_t)ps;
    f[30] = (uint8_t)(ps >> 8);
    return 33u;
}

/* @p from's NDP ADDBA Request to us for @p tid: immediate policy, 16 frames, no timeout. */
static uint16_t addba_req_(uint8_t *f, const uint8_t *from, uint8_t tid)
{
    memset(f, 0, 33);
    f[0] = 0xd0;
    memcpy(&f[4], W, 6);
    memcpy(&f[10], from, 6);
    memcpy(&f[16], from, 6);
    const uint16_t ps = (uint16_t)((1u << 1) | ((uint16_t)tid << 2) | (16u << 6));
    f[24] = 3;   /* Block Ack */
    f[25] = 128; /* NDP ADDBA Request */
    f[26] = 9;
    f[27] = (uint8_t)ps;
    f[28] = (uint8_t)(ps >> 8);
    return 33u;
}

static bool is_ba_(const struct simnode_frame *f, uint8_t act)
{
    return f->is_mgmt && f->len >= 27u && f->bytes[24] == 3u && f->bytes[25] == act;
}

/* Block Ack frames with action @p act (128 ADDBA Request, 129 Response, 130 DELBA) to @p ra from
 * outbox entry @p from on; the index of the first in @p first. */
static unsigned ba_n_(const uint8_t *ra, uint8_t act, unsigned from, unsigned *first)
{
    unsigned n = 0;
    for (unsigned i = from; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (is_ba_(f, act) && MAC_EQ(f->bytes + 4, ra) && n++ == 0u && first != NULL) { *first = i; }
    }
    return n;
}

/* The index of the first data frame to @p ra from outbox entry @p from on, UINT32_MAX if none. */
static unsigned first_data_(const uint8_t *ra, unsigned from)
{
    for (unsigned i = from; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (!f->is_mgmt && f->len >= 24u && MAC_EQ(f->bytes + 4, ra)) { return i; }
    }
    return UINT32_MAX;
}

/* Outbox entry @p i is a DELBA to @p ra from the originator for @p tid, reason 37 (mac80211's
 * WLAN_REASON_QSTA_NOT_USE when it stops its own session). */
static bool delba_is_(unsigned i, const uint8_t *ra, uint8_t tid)
{
    const struct simnode_frame *f = simnode_outbox_get(i);
    if (f == NULL || !is_ba_(f, 130u) || f->len < 30u || !MAC_EQ(f->bytes + 4, ra)) { return false; }
    const uint16_t ps = (uint16_t)(f->bytes[26] | (f->bytes[27] << 8));
    const uint16_t rc = (uint16_t)(f->bytes[28] | (f->bytes[29] << 8));
    return ((ps >> 11) & 1u) == 1u && (ps >> 12) == tid && rc == 37u;
}

/* The dialog token of the last ADDBA Request to @p ra for @p tid, 0 if none. */
static uint8_t addba_token_(const uint8_t *ra, uint8_t tid)
{
    uint8_t tok = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (is_ba_(f, 128u) && f->len >= 29u && MAC_EQ(f->bytes + 4, ra) && ((f->bytes[27] >> 2) & 0x0fu) == tid)
        {
            tok = f->bytes[26];
        }
    }
    return tok;
}

static bool agreed_(const uint8_t *peer, uint8_t tid)
{
    struct umac_sta_data *stad = umac_datapath_mesh_find_peer(peer);
    return stad != NULL && umac_ba_is_ampdu_permitted(stad, tid);
}

/* A 64-octet frame to @p peer on @p tid sends an ADDBA Request; @p peer accepts it. */
static bool session_(const uint8_t *peer, uint8_t tid)
{
    static uint8_t pay[64], f[64];
    pay_(pay, 64, 0x33);
    (void)simnode_host_tx_tid(peer, W, pay, 64, tid);
    const uint8_t token = addba_token_(peer, tid);
    if (token != 0u) { (void)simnode_rx(f, addba_resp_(f, peer, token, tid), -60); }
    return agreed_(peer, tid);
}

/* @p from's NDP DELBA to us for @p tid with @p reason; @p originator: as the session's originator. */
static uint16_t delba_(uint8_t *f, const uint8_t *from, uint8_t tid, uint16_t reason, bool originator)
{
    memset(f, 0, 30);
    f[0] = 0xd0;
    memcpy(&f[4], W, 6);
    memcpy(&f[10], from, 6);
    memcpy(&f[16], from, 6);
    const uint16_t ps = (uint16_t)(((originator ? 1u : 0u) << 11) | ((uint16_t)tid << 12));
    f[24] = 3;   /* Block Ack */
    f[25] = 130; /* NDP DELBA */
    f[26] = (uint8_t)ps;
    f[27] = (uint8_t)(ps >> 8);
    f[28] = (uint8_t)reason;
    f[29] = (uint8_t)(reason >> 8);
    return 30u;
}

#define GUARD UMAC_MESH_FRAG_BA_GUARD_MS

static void t_ba(void)
{
    printf("--- (7) Block Ack: a session ends before a frame is cut, and stays down while frames need it ---\n");
    static uint8_t pay[1500], sm[64], f[64], body[3000];
    static struct caps c, c7a;
    uint16_t blen = 0;
    pay_(sm, 64, 41);

    /* (7a) */
    up_(false);
    simnode_set_ampdu(true);
    const bool up = session_(A, 0) && session_(A, 5) && session_(C, 0);
    simnode_outbox_clear();
    SET_RATES(R_1M0);
    g_warthog_hostfrag = 1;
    struct hf s = hf_();
    const uint32_t end0 = g_warthog_ba_delba_end;
    send_a_(&c, 1500, 12, pay);
    unsigned d0 = UINT32_MAX;
    const unsigned dn = ba_n_(A, 130u, 0, &d0);
    CHECK(up && dn == 1u && delba_is_(d0, A, 0) && simnode_outbox_get(d0)->ba_wait &&
              g_warthog_ba_delba_end - end0 == 1u,
          "(red) under A's session on TID 0, 1500 octets at 1 MHz MCS0: a DELBA (originator, TID 0, "
          "reason 37) goes to A, its TX status asked for, counted delba_end (%u DELBAs)", dn);
    CHECK(!agreed_(A, 0) && agreed_(A, 5) && agreed_(C, 0),
          "(red) A's TID 0 session is gone; A's TID 5 and C's TID 0 stay");
    (void)simnode_host_tx_tid(A, W, sm, 64, 6);
    (void)simnode_host_tx(C, W, sm, 64);
    CHECK(c.n == 0u && first_data_(A, 0) == UINT32_MAX && first_data_(C, 0) != UINT32_MAX &&
              HD(s, ba_wait) == 1u,
          "(red) the frame waits for that DELBA's TX status and %u ms more, and so does A's next frame "
          "(TID 6); C's goes at once (ba_wait %u)", GUARD, HD(s, ba_wait));
    simnode_advance_run(GUARD - 1u);
    const bool early = first_data_(A, 0) != UINT32_MAX;
    simnode_advance_run(1);
    capture_(&c, A, 0);
    const bool six = c.n == 4u && c.f[3].tid == 6u;
    c.n = c.n > 3u ? 3u : c.n;
    const char *why = mac80211_rx_(c.f, c.n, K_A, body, &blen);
    CHECK(!early && six && c.n == 3u && sized_(&c, CAP_1M_MCS0) && handed_(&c, 0) && why == NULL &&
              d0 < first_data_(A, 0) && HD(s, msdu) == 1u && HD(s, ba_end) == 1u && HD(s, ba_late) == 0u,
          "(red) then, not before, it goes as 3 fragments, none as A-MPDU, mac80211's rules met (%s), "
          "and the TID 6 frame after them; ba_end %u", why ? why : "ok", HD(s, ba_end));
    c7a = c;
    CHECK(g_warthog_hostfrag_held == 1u && g_warthog_hostfrag_hold_ms == 15000u,
          "(red) AT+HOSTFRAG? shows A's TID 0 held (held %lu) for %lu ms after the last frame that "
          "needed cutting", (unsigned long)g_warthog_hostfrag_held,
          (unsigned long)g_warthog_hostfrag_hold_ms);

    /* (7b) */
    s = hf_();
    unsigned from = simnode_outbox_count();
    send_a_(&c, 1500, 14, pay);
    for (unsigned k = 0; k < 5u; k++) { (void)simnode_host_tx(A, W, sm, 64); }
    CHECK(ba_n_(A, 128u, from, NULL) == 0u && ba_n_(A, 130u, from, NULL) == 0u && c.n == 3u &&
              HD(s, ba_end) == 0u && HD(s, ba_wait) == 0u && HD(s, hold) == 1u,
          "(red) held: the next 1500 octets are cut at once, no second DELBA (%u fragments); neither it "
          "nor 5 small frames after it on TID 0 send an ADDBA, and hold counts the one ADDBA kept "
          "back, not each frame (hold %u)", c.n, HD(s, hold));

    /* (7c) */
    s = hf_();
    from = simnode_outbox_count();
    unsigned cut = 0;
    for (unsigned k = 0; k < 6u; k++)
    {
        simnode_advance_ms(10000);
        (void)simnode_host_tx(A, W, sm, 64);
        send_a_(&c, 1500, (uint8_t)(15u + k), pay);
        cut += c.n == 3u;
    }
    CHECK(cut == 6u && ba_n_(A, 128u, from, NULL) == 0u && ba_n_(A, 130u, from, NULL) == 0u &&
              HD(s, ba_end) == 0u,
          "(red) a frame that needs cutting every 10 s for 60 s keeps TID 0 held: %u cut, no ADDBA, no "
          "DELBA (%u, %u)", cut, ba_n_(A, 128u, from, NULL), ba_n_(A, 130u, from, NULL));

    /* (7d) */
    simnode_advance_ms(15001);
    from = simnode_outbox_count();
    s = hf_();
    const bool back = session_(A, 0);
    CHECK(back && ba_n_(A, 128u, from, NULL) == 1u && g_warthog_hostfrag_held == 0u,
          "(red) 15 s after the last frame that needed cutting a small frame starts a session again "
          "(%u ADDBA, held %lu)", ba_n_(A, 128u, from, NULL), (unsigned long)g_warthog_hostfrag_held);
    pay_(pay, 200, 21);
    from = simnode_outbox_count();
    (void)simnode_host_tx(A, W, pay, 200);
    const unsigned w = first_data_(A, from);
    CHECK(w != UINT32_MAX && (simnode_outbox_get(w)->tx_flags & MMDRV_TX_FLAG_AMPDU_ENABLED) != 0u,
          "(pin) a frame that needs no cut goes as A-MPDU under it");
    from = simnode_outbox_count();
    send_a_(&c, 1500, 22, pay);
    simnode_advance_run(GUARD);
    capture_(&c, A, from);
    CHECK(ba_n_(A, 130u, from, &d0) == 1u && delba_is_(d0, A, 0) && c.n == 3u && HD(s, ba_end) == 1u,
          "(red) and the next frame over the limit ends it again: at most one ADDBA and one DELBA a "
          "hold period (ba_end %u)", HD(s, ba_end));

    /* (7e) */
    up_(false);
    simnode_set_ampdu(true);
    pay_(pay, 64, 23);
    (void)simnode_host_tx(A, W, pay, 64);
    struct umac_sta_data *rec = umac_datapath_mesh_find_peer(A);
    const uint8_t token = addba_token_(A, 0);
    const unsigned pend = simnode_timeouts_holding(rec);
    SET_RATES(R_1M0);
    g_warthog_hostfrag = 1;
    simnode_outbox_clear();
    send_a_(&c, 1500, 24, pay);
    simnode_advance_run(GUARD);
    capture_(&c, A, 0);
    d0 = UINT32_MAX;
    CHECK(token != 0u && pend == 1u && ba_n_(A, 130u, 0, &d0) == 1u && delba_is_(d0, A, 0) &&
              d0 < first_data_(A, 0) && simnode_timeouts_holding(rec) == 0u && c.n == 3u,
          "(red) an ADDBA A has not answered: a DELBA before the first fragment, its retry timeout "
          "cancelled (%u -> %u)", pend, simnode_timeouts_holding(rec));
    simnode_outbox_clear();
    simnode_advance_run(250);
    (void)simnode_rx(f, addba_resp_(f, A, token, 0), -60);
    CHECK(ba_n_(A, 130u, 0, NULL) == 0u && !agreed_(A, 0),
          "(red) nothing fires later, and A's late ADDBA Response opens no session");

    /* (7f) */
    up_(false);
    simnode_set_ampdu(true);
    SET_RATES(R_1M0);
    g_warthog_hostfrag = 1;
    s = hf_();
    send_a_(&c, 1500, 25, pay);
    CHECK(c.n == 3u && ba_n_(A, 128u, 0, NULL) == 0u && ba_n_(A, 130u, 0, NULL) == 0u && HD(s, ba_end) == 0u &&
              HD(s, ba_wait) == 0u,
          "(red) no session yet: the first frame that needs cutting goes at once and sends no ADDBA, "
          "and none follows");

    /* (7g) */
    up_(false);
    simnode_set_ampdu(true);
    const bool cs = session_(C, 0);
    SET_RATES(R_1M0);
    g_warthog_hostfrag = 1;
    s = hf_();
    simnode_outbox_clear();
    pay_(pay, 1500, 26);
    simnode_tx_alloc_fail_at(3); /* the frame, its DELBA, the second fragment */
    (void)simnode_host_tx(C, W, pay, 1500);
    simnode_tx_alloc_fail_at(0);
    const unsigned wc = first_data_(C, 0);
    CHECK(cs && HD(s, pool) == 1u && wc != UINT32_MAX &&
              (simnode_outbox_get(wc)->tx_flags & MMDRV_TX_FLAG_AMPDU_ENABLED) == 0u &&
              ba_n_(C, 130u, 0, NULL) == 1u && !agreed_(C, 0),
          "(red) over the limit but no buffer for its fragments: whole, its session ended first, not "
          "as A-MPDU (pool %u)", HD(s, pool));

    /* (7h) */
    up_(false);
    simnode_set_ampdu(true);
    const bool a0 = session_(A, 0);
    SET_RATES(R_1M0);
    simnode_set_chip_frag_threshold(512);
    s = hf_();
    simnode_outbox_clear();
    send_a_(&c, 1500, 27, pay);
    CHECK(a0 && c.n == 1u && (c.f[0].tx_flags & MMDRV_TX_FLAG_AMPDU_ENABLED) != 0u &&
              ba_n_(A, 130u, 0, NULL) == 0u && agreed_(A, 0) && HD(s, ba_end) == 0u && HD(s, ba_wait) == 0u,
          "(pin) AT+HOSTFRAG=0: under A's session 1500 octets go whole as A-MPDU, the session kept");
    simnode_set_ampdu(false);
    pay_(pay, 1500, 12);
    reassembles_("cut after the DELBA", &c7a, pay, 1500);
}

/* W, A and C up with A-MPDU, A's session on TID 0 agreed, auto at 1 MHz MCS0. */
static bool ba_up_(void)
{
    up_(false);
    simnode_set_ampdu(true);
    SET_RATES(R_1M0);
    g_warthog_hostfrag = 1;
    const bool ok = session_(A, 0);
    simnode_outbox_clear();
    return ok;
}

static void t_ba_wait(void)
{
    printf("--- (7i) the first frame cut after a session ends waits for its DELBA ---\n");
    static uint8_t pay[1500], sm[64];
    static struct caps c;
    pay_(sm, 64, 42);

    /* A DELBA the chip never reports (a queue flush): the frame goes at the limit, counted. */
    bool up = ba_up_();
    simnode_tx_hold(true);
    struct hf s = hf_();
    send_a_(&c, 1500, 43, pay);
    const unsigned held = simnode_tx_held();
    (void)simnode_tx_forget_held(0);
    simnode_tx_hold(false);
    simnode_advance_run(UMAC_MESH_FRAG_BA_WAIT_MAX_MS - 1u);
    const unsigned before = first_data_(A, 0) == UINT32_MAX ? 0u : 1u;
    simnode_advance_run(1);
    capture_(&c, A, 0);
    CHECK(up && held == 1u && before == 0u && c.n == 3u && HD(s, ba_wait) == 1u && HD(s, ba_late) == 1u,
          "(red) a DELBA whose TX status never comes: the frame waits %u ms, then goes as 3 "
          "fragments, counted ba_late (%u held, %u, late %u)", UMAC_MESH_FRAG_BA_WAIT_MAX_MS, held, c.n,
          HD(s, ba_late));

    /* Handed back untried by the chip, or dropped by the driver: the status comes, so does the frame. */
    static const char *how[] = { "handed back untried by the chip", "dropped by the driver" };
    for (unsigned k = 0; k < 2u; k++)
    {
        up = ba_up_();
        simnode_tx_hold(true);
        s = hf_();
        send_a_(&c, 1500, (uint8_t)(44u + k), pay);
        (void)(k == 0u ? simnode_tx_return_held(0) : simnode_tx_drop_held(0));
        simnode_tx_hold(false);
        simnode_pump();
        simnode_advance_run(GUARD);
        capture_(&c, A, 0);
        CHECK(up && c.n == 3u && HD(s, ba_late) == 0u,
              "(red) a DELBA %s: its status still comes, and the frame goes %u ms after it (%u)", how[k],
              GUARD, c.n);
    }

    /* (25) Acked, or given up on unacked: the frame goes 20 ms after either; only the second counts. */
    for (unsigned k = 0; k < 2u; k++)
    {
        up = ba_up_();
        simnode_tx_hold(true);
        s = hf_();
        const uint32_t dn0 = g_warthog_hostfrag_delba_noack;
        send_a_(&c, 1500, (uint8_t)(48u + k), pay);
        simnode_tx_noack_mgmt_next(k);
        const bool sent = simnode_tx_send_held(0);
        simnode_tx_hold(false);
        simnode_pump();
        const unsigned early = first_data_(A, 0) == UINT32_MAX ? 0u : 1u;
        simnode_advance_run(GUARD);
        capture_(&c, A, 0);
        CHECK(up && sent && early == 0u && c.n == 3u && HD(s, ba_late) == 0u &&
                  g_warthog_hostfrag_delba_noack - dn0 == k,
              "%s a DELBA %s: the frame goes %u ms after its status (%u fragments), delba_noack +%lu",
              k ? "(red)" : "(pin)", k ? "the chip gave up on unacked" : "acked", GUARD, c.n,
              (unsigned long)(g_warthog_hostfrag_delba_noack - dn0));
    }

    /* A peer removed while its frame waits: the frame goes with it, nothing left allocated. */
    up = ba_up_();
    const unsigned live0 = simnode_live_allocs();
    const unsigned to0 = simnode_timeouts_pending();
    simnode_tx_hold(true);
    send_a_(&c, 1500, 46, pay);
    (void)simnode_tx_forget_held(0);
    simnode_tx_hold(false);
    simnode_del_peer(A);
    simnode_advance_run(UMAC_MESH_FRAG_BA_WAIT_MAX_MS);
    CHECK(up && first_data_(A, 0) == UINT32_MAX && simnode_live_allocs() <= live0 &&
              simnode_timeouts_pending() <= to0,
          "(red) A removed while its cut frame waits: nothing is sent to it, nothing is left allocated "
          "or armed (%u -> %u allocations, %u -> %u timeouts)", live0, simnode_live_allocs(), to0,
          simnode_timeouts_pending());

    /* A frame to C waits for nothing of A's. */
    up = ba_up_();
    simnode_tx_hold(true);
    send_a_(&c, 1500, 47, pay);
    simnode_tx_hold(false);
    (void)simnode_host_tx(C, W, sm, 64);
    CHECK(up && first_data_(C, 0) != UINT32_MAX && first_data_(A, 0) == UINT32_MAX,
          "(red) while A's frame waits, C's go");
    while (simnode_tx_held() != 0u) { (void)simnode_tx_forget_held(0); }
}

static void t_ba_counts(void)
{
    printf("--- (7j)-(7l) Block Ack counts: ADDBA and DELBA sent, the peer's DELBA, DELBAs not sent ---\n");
    static uint8_t pay[1500], sm[64], f[64];
    static struct caps c;
    pay_(sm, 64, 48);

    /* (7j) */
    bool up = ba_up_();
    send_a_(&c, 1500, 49, pay);
    simnode_advance_run(GUARD);
    unsigned per[3][2];
    uint32_t addba0 = g_warthog_ba_addba_tx, to0 = g_warthog_ba_delba_to, end0 = g_warthog_ba_delba_end;
    for (unsigned k = 0; k < 3u; k++)
    {
        simnode_advance_run(15001);
        const unsigned from = simnode_outbox_count();
        for (unsigned j = 0; j < 3u; j++)
        {
            const unsigned had = ba_n_(A, 128u, from, NULL);
            (void)simnode_host_tx(A, W, sm, 64);
            if (ba_n_(A, 128u, from, NULL) > had)
            {
                const uint8_t tok = addba_token_(A, 0);
                simnode_advance_run(150);
                (void)simnode_rx(f, addba_resp_(f, A, tok, 0), -60);
            }
        }
        send_a_(&c, 1500, (uint8_t)(50u + k), pay);
        simnode_advance_run(GUARD);
        per[k][0] = ba_n_(A, 128u, from, NULL);
        per[k][1] = ba_n_(A, 130u, from, NULL);
    }
    bool one = true;
    for (unsigned k = 0; k < 3u; k++) { one = one && per[k][0] == 1u && per[k][1] == 1u; }
    CHECK(up && one && g_warthog_ba_addba_tx - addba0 == 3u && g_warthog_ba_delba_to == to0 &&
              g_warthog_ba_delba_end - end0 == 3u,
          "(red) A answers each ADDBA 150 ms later, after the first ADDBA's 100 ms timeout: one ADDBA "
          "and one DELBA a hold period (%u/%u, %u/%u, %u/%u), counted addba_tx +%lu, delba_to +%lu, "
          "delba_end +%lu", per[0][0], per[0][1], per[1][0], per[1][1], per[2][0], per[2][1],
          (unsigned long)(g_warthog_ba_addba_tx - addba0), (unsigned long)(g_warthog_ba_delba_to - to0),
          (unsigned long)(g_warthog_ba_delba_end - end0));

    /* (7k) */
    up = ba_up_();
    struct hf s = hf_();
    const uint32_t rx0 = g_warthog_ba_rx_delba;
    end0 = g_warthog_ba_delba_end;
    simnode_tx_alloc_fail_at(2); /* the frame, then its DELBA */
    send_a_(&c, 1500, 53, pay);
    simnode_tx_alloc_fail_at(0);
    CHECK(up && ba_n_(A, 130u, 0, NULL) == 0u && !agreed_(A, 0) && c.n == 3u && HD(s, ba_end) == 0u &&
              HD(s, nodelba) == 1u && HD(s, ba_wait) == 0u && g_warthog_ba_delba_end == end0,
          "(red) a DELBA that cannot be built: the session still ends, the frame goes at once as 3 "
          "fragments, counted nodelba, not ba_end (%u, nodelba %u, ba_end %u)", c.n, HD(s, nodelba),
          HD(s, ba_end));
    unsigned from = simnode_outbox_count();
    (void)simnode_rx(f, delba_(f, A, 0, 38, /*originator=*/false), -60);
    (void)simnode_host_tx(A, W, sm, 64);
    CHECK(g_warthog_ba_rx_delba - rx0 == 1u && g_warthog_ba_rx_reason == 38u &&
              ba_n_(A, 128u, from, NULL) == 0u,
          "(red) A ends its own session at a fragment and says so (recipient, reason 38): counted "
          "rx_delba, its reason kept; no ADDBA while held (rx_delba +%lu, reason %lu)",
          (unsigned long)(g_warthog_ba_rx_delba - rx0), (unsigned long)g_warthog_ba_rx_reason);
    const uint32_t rx1 = g_warthog_ba_rx_delba, why1 = g_warthog_ba_rx_reason;
    (void)simnode_rx(f, delba_(f, A, 5, 1, /*originator=*/true), -60);
    CHECK(g_warthog_ba_rx_delba == rx1 && g_warthog_ba_rx_reason == why1,
          "(pin) a DELBA for a session A originates is not one of ours: not counted");

    /* (7l) */
    s = hf_();
    simnode_tx_aggregated_next(1);
    send_a_(&c, 1500, 54, pay);
    CHECK(c.n == 3u && HD(s, agg) == 1u,
          "(red) a fragment whose TX status says it went in an A-MPDU is counted agg (%u)", HD(s, agg));
    simnode_tx_aggregated_next(0);
    simnode_set_ampdu(false);
}

/* ---- (19)-(20) AT+AMPDU, and Block Ack with AT+HOSTFRAG=0 -------------------------------- */

/* Opened under @p key: the header and plaintext of a host-sealed frame @p h of @p len, in @p out. */
static uint16_t open_(const uint8_t *h, uint16_t len, const uint8_t key[16], uint8_t *out)
{
    uint8_t pn[6], kid = 0, aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
    if (len < HDR + 16u || !umac_ccmp_parse_header(h + HDR, pn, &kid)) { return 0; }
    const uint32_t al = umac_ccmp_build_aad(h, aad);
    umac_ccmp_build_nonce(h, pn, nonce);
    const uint16_t pl = (uint16_t)(len - HDR - 16u);
    memcpy(out, h, HDR);
    memcpy(out + HDR, h + HDR + 8u, pl);
    return warthog_ccm_ad(key, nonce, 8, aad, al, out + HDR, pl, h + len - 8u) == 0 ? (uint16_t)(HDR + pl) : 0u;
}

/* The outbox into s_h: ADDBA dialog tokens (random) masked, host-sealed data opened (its packet
 * numbers differ from start to start); @p strip leaves out Block Ack frames and the A-MPDU flag. */
static void h_outbox_ba_(bool strip)
{
    static uint8_t b[1700];
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        const bool ba = f->is_mgmt && f->len >= 27u && f->bytes[24] == 3u;
        if (ba && strip) { continue; }
        uint16_t n = f->len;
        memcpy(b, f->bytes, n);
        if (ba && (b[25] == 128u || b[25] == 129u)) { b[26] = 0; }
        if (HOST_SEALS && !f->is_mgmt && (b[1] & 0x40u) != 0u && (b[4] & 1u) == 0u)
        {
            n = open_(f->bytes, f->len, MAC_EQ(f->bytes + 4, A) ? K_A : K_C, b);
        }
        h_(&n, sizeof(n));
        h_(b, n);
        const uint8_t fl = strip ? (uint8_t)(f->tx_flags & ~MMDRV_TX_FLAG_AMPDU_ENABLED) : f->tx_flags;
        const uint8_t meta[5] = { f->is_mgmt, fl, f->key_idx, f->tid, f->vif_id };
        h_(meta, sizeof(meta));
    }
    const uint32_t rc[2] = { simnode_rc_table_calls(), simnode_rc_last_size() };
    h_(rc, sizeof(rc));
    simnode_outbox_clear();
}

/* AT+HOSTFRAG=0, A-MPDU on: sessions to A (TID 0) and C (TID 5), unicast to each at 1 MHz MCS0
 * under AT+FRAG=512, and a group frame. */
static uint64_t ba_run_(bool strip)
{
    static uint8_t pay[1500];
    s_h = 1469598103934665603ull;
    const uint32_t ampdu = g_warthog_ampdu;
    up_(false);
    g_warthog_ampdu = ampdu;
    g_warthog_mesh_seq = 40000u;
    simnode_set_ampdu(true);
    SET_RATES(R_1M0);
    simnode_set_chip_frag_threshold(512);
    simnode_rc_clear();
    (void)session_(A, 0);
    (void)session_(C, 5);
    h_outbox_ba_(strip);
    static const uint16_t sizes[] = { 64, 1000, 1500 };
    for (unsigned i = 0; i < 3u; i++)
    {
        pay_(pay, sizes[i], (uint8_t)(0x40 + i));
        (void)simnode_host_tx(A, W, pay, sizes[i]);
        (void)simnode_host_tx_tid(C, W, pay, sizes[i], 5);
        h_outbox_ba_(strip);
    }
    (void)simnode_host_tx(BC, W, pay, 1000);
    h_outbox_ba_(strip);
    simnode_set_ampdu(false);
    return s_h;
}

/* Recorded by this scenario on the tree before host fragmentation ended Block Ack sessions (the
 * MESH chip VIF changes none of these frames). */
#if HOST_SEALS
#define BA_GOLDEN 0x37c41e7ffca79e4dull
#else
#define BA_GOLDEN 0x43279873cca45c15ull
#endif

static void t_ba_off(void)
{
    printf("--- (20) AT+HOSTFRAG=0 under Block Ack: byte-identical to the tree before ---\n");
    const uint64_t h = ba_run_(false);
    CHECK(h == BA_GOLDEN,
          "(pin) sessions to A and C, unicast at 1 MHz MCS0 under AT+FRAG=512 and a group frame: the "
          "frames, flags, keys and rate-control calls as before (0x%016llx, want 0x%016llx)",
          (unsigned long long)h, (unsigned long long)BA_GOLDEN);
}

static void t_ampdu(void)
{
    printf("--- (19) AT+AMPDU ---\n");
    static uint8_t pay[1500], f[64];
    up_(false);
    simnode_set_ampdu(true);
    const bool up = session_(A, 0) && session_(A, 5) && session_(C, 0);
    simnode_tick();
    const uint32_t orig = g_warthog_ampdu_orig, e0 = g_warthog_ampdu_ended, u0 = g_warthog_ampdu_unsent;
    simnode_outbox_clear();
    g_warthog_ampdu = 0;
    simnode_tick();
    unsigned da = 0, dc = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        da += (delba_is_(i, A, 0) || delba_is_(i, A, 5));
        dc += delba_is_(i, C, 0);
    }
    CHECK(up && orig == 3u && da == 2u && dc == 1u && !agreed_(A, 0) && !agreed_(A, 5) && !agreed_(C, 0) &&
              g_warthog_ampdu_ended - e0 == 3u && g_warthog_ampdu_unsent == u0 && g_warthog_ampdu_orig == 0u,
          "(red) =0 ends every originator session within a service tick: a DELBA for each of A's "
          "TIDs 0 and 5 and C's TID 0 (orig %lu -> %lu, ended %lu)", (unsigned long)orig,
          (unsigned long)g_warthog_ampdu_orig, (unsigned long)(g_warthog_ampdu_ended - e0));
    g_warthog_ampdu = 1;
    up_(false);
    simnode_set_ampdu(true);
    const bool up2 = session_(A, 0) && session_(A, 5) && session_(C, 0);
    simnode_tick();
    const uint32_t e1 = g_warthog_ampdu_ended, u1 = g_warthog_ampdu_unsent;
    simnode_outbox_clear();
    g_warthog_ampdu = 0;
    simnode_tx_alloc_fail_at(1); /* the first DELBA */
    simnode_tick();
    simnode_tx_alloc_fail_at(0);
    unsigned dl = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++) { dl += is_ba_(simnode_outbox_get(i), 130u); }
    CHECK(up2 && dl == 2u && g_warthog_ampdu_ended - e1 == 2u && g_warthog_ampdu_unsent - u1 == 1u &&
              g_warthog_ampdu_orig == 0u,
          "(red) =0 when one DELBA cannot be built: all 3 sessions end, counted ended for the 2 DELBAs "
          "handed and unsent for the other (%u DELBAs, ended %lu, unsent %lu)", dl,
          (unsigned long)(g_warthog_ampdu_ended - e1), (unsigned long)(g_warthog_ampdu_unsent - u1));
    simnode_outbox_clear();
    pay_(pay, 200, 30);
    for (uint8_t t = 0; t <= 5u; t++) { (void)simnode_host_tx_tid(A, W, pay, 200, t); }
    bool flat = true;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *q = simnode_outbox_get(i);
        flat = flat && (q->tx_flags & MMDRV_TX_FLAG_AMPDU_ENABLED) == 0u;
    }
    CHECK(ba_n_(A, 128u, 0, NULL) == 0u && flat && simnode_outbox_count() == 6u,
          "(red) and starts none: a frame on each of TIDs 0-5 to A, no ADDBA, none as A-MPDU (%u frames)",
          simnode_outbox_count());
    simnode_outbox_clear();
    (void)simnode_rx(f, addba_req_(f, A, 0), -60);
    unsigned r = 0;
    const unsigned rn = ba_n_(A, 129u, 0, &r);
    CHECK(rn == 1u && simnode_outbox_get(r)->bytes[27] == 0u && simnode_outbox_get(r)->bytes[28] == 0u,
          "(pin) the recipient side is unchanged: A's ADDBA Request is accepted (%u responses)", rn);
    g_warthog_ampdu = 1;
    simnode_outbox_clear();
    CHECK(session_(A, 0) && ba_n_(A, 128u, 0, NULL) == 1u, "(red) =1: the next frame starts a session again");

    const uint64_t on = ba_run_(true);
    g_warthog_ampdu = 0;
    const uint64_t off = ba_run_(true);
    g_warthog_ampdu = 1;
    CHECK(on == off,
          "(pin) =0 and =1 hand the chip the same frames but for Block Ack frames and the A-MPDU flag "
          "(0x%016llx, 0x%016llx)", (unsigned long long)on, (unsigned long long)off);
}

static void t_group(void)
{
    printf("--- (8) group frames ---\n");
    static uint8_t pay[1500];
    static struct caps ca, cc;
    up_(false);
    SET_RATES(R_1M0);
    g_warthog_hostfrag = 1;
    simnode_set_gates(false, false, /*grp_std=*/true, true);
    pay_(pay, 1500, 13);
    (void)simnode_host_tx(BC, W, pay, 1500);
    unsigned grp = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *q = simnode_outbox_get(i);
        grp += !q->is_mgmt && (q->bytes[4] & 1u) != 0u && (q->bytes[1] & 0x04u) == 0u &&
               (q->bytes[22] & 0x0fu) == 0u;
    }
    CHECK(grp == 1u && simnode_outbox_count() == 1u,
          "(pin) AT+MESHGRP=1: a 1500-octet broadcast is one group frame (%u frames)",
          simnode_outbox_count());
    simnode_set_gates(false, false, false, true);
    simnode_outbox_clear();
    (void)simnode_host_tx(BC, W, pay, 1500);
    capture_(&ca, A, 0);
    capture_(&cc, C, 0);
    CHECK(ca.n == 3u && cc.n == 3u && sized_(&ca, CAP_1M_MCS0) && sized_(&cc, CAP_1M_MCS0),
          "(red) replication: each peer's unicast copy is cut on its own (%u to A, %u to C)",
          ca.n, cc.n);
    static uint8_t body[3000];
    uint16_t blen = 0;
    const char *wa = mac80211_rx_(ca.f, ca.n, K_A, body, &blen);
    const bool ba = wa == NULL && body_is_(body, blen, 6, 0, 0x0800, pay, 1500);
    const char *wc = mac80211_rx_(cc.f, cc.n, K_C, body, &blen);
    CHECK(ba && wc == NULL && body_is_(body, blen, 6, 0, 0x0800, pay, 1500),
          "  each meets mac80211's rules under its own link key (A: %s, C: %s)", wa ? wa : "ok",
          wc ? wc : "ok");
    as_(C, W, K_C, false);
    rx_caps_(&cc, NULL);
    CHECK(simnode_ext_rx_count() == 1u && ext_is_(0, C, W, 0x0800, pay, 1500),
          "  C delivers its copy once, byte-identical, addressed to it as a replica is (%u)",
          simnode_ext_rx_count());
}

/* ---- (9)-(10) relay and batman ------------------------------------------------------------- */

/* W relays (forwarding on) @p n octets from A for @p da (@p sa, Mesh Control @p mc); captured. */
static void relay_(struct caps *c, const uint8_t *da, const uint8_t *sa, struct umac_mesh_ctrl mc,
                   const uint8_t *pay, uint16_t n)
{
    static uint8_t f[1800];
    simnode_outbox_clear();
    const uint16_t len = from_a_(f, da, sa, &mc, pay, n);
    (void)simnode_rx_air(f, len, -60);
    capture_(c, C, 0);
}

static void t_relay(void)
{
    printf("--- (9) relay: the relayed Mesh Control in the first fragment ---\n");
    static uint8_t pay[1500];
    static struct caps whole, frags, whole2, frags2;
    pay_(pay, 1200, 14);
    up_(/*fwd=*/true);
    SET_RATES(R_1M0);
    relay_(&whole, C, A, (struct umac_mesh_ctrl){ .flags = 0, .ttl = 31, .seq = ++s_mseq_a }, pay, 1200);
    struct umac_mesh_ctrl ae = { .flags = UMAC_MESH_CTRL_AE_A5A6, .ttl = 31, .seq = ++s_mseq_a };
    memcpy(ae.eaddr1, H3, 6);
    memcpy(ae.eaddr2, H2, 6);
    relay_(&whole2, C, A, ae, pay, 1200);
    g_warthog_hostfrag = 1;
    relay_(&frags, C, A, (struct umac_mesh_ctrl){ .flags = 0, .ttl = 31, .seq = ++s_mseq_a }, pay, 1200);
    ae.seq = ++s_mseq_a;
    relay_(&frags2, C, A, ae, pay, 1200);
    static uint8_t body[3000];
    uint16_t blen = 0;
    const char *why = mac80211_rx_(frags.f, frags.n, K_C, body, &blen);
    CHECK(whole.n == 1u && frags.n == 2u && sized_(&frags, CAP_1M_MCS0) && why == NULL &&
              body[1] == 30u && body_is_(body, blen, 6, 0, 0x0800, pay, 1200),
          "(red) relayed to C in 2 fragments; the first carries the relayed Mesh Control, TTL 30 "
          "(%u, %s, ttl %u)", frags.n, why ? why : "ok", body[1]);
    why = mac80211_rx_(frags2.f, frags2.n, K_C, body, &blen);
    CHECK(whole2.n == 1u && frags2.n == 2u && why == NULL &&
              body_is_(body, blen, 18, UMAC_MESH_CTRL_AE_A5A6, 0x0800, pay, 1200) &&
              MAC_EQ(body + 6, H3) && MAC_EQ(body + 12, H2),
          "(red) AE 2 (H3 behind C, from H2 behind A): its 18-octet Mesh Control in the first "
          "body only (%u, %s)", frags2.n, why ? why : "ok");
    /* In the order W sealed them: C's replay check takes each PN once, rising. */
    as_(C, W, K_C, false);
    rx_caps_(&whole, NULL);
    rx_caps_(&whole2, NULL);
    rx_caps_(&frags, NULL);
    rx_caps_(&frags2, NULL);
    CHECK(frags.n == 2u && frags2.n == 2u && simnode_ext_rx_count() == 4u && ext_same_(0, 2) &&
              ext_same_(1, 3) && ext_is_(2, C, A, 0x0800, pay, 1200) &&
              ext_is_(3, H3, H2, 0x0800, pay, 1200),
          "(red) C delivers each fragmented relay byte-identical to the one relayed whole "
          "(%u delivered)", simnode_ext_rx_count());
}

static void t_batman(void)
{
    printf("--- (10) batman ---\n");
    static uint8_t pay[1500];
    static struct caps w1, f1, w2, f2;
    pay_(pay, 1300, 15);
    up_(false);
    simnode_set_batman(true);
    SET_RATES(R_1M0);
    unsigned from = simnode_outbox_count();
    (void)simnode_host_tx_eth(A, A, W, 0x4305, pay, 1300);
    capture_(&w1, A, from);
    from = simnode_outbox_count();
    (void)simnode_host_tx_eth(NULL, BC, W, 0x4305, pay, 900);
    capture_(&w2, A, from);
    g_warthog_hostfrag = 1;
    from = simnode_outbox_count();
    (void)simnode_host_tx_eth(A, A, W, 0x4305, pay, 1300);
    capture_(&f1, A, from);
    from = simnode_outbox_count();
    (void)simnode_host_tx_eth(NULL, BC, W, 0x4305, pay, 900);
    capture_(&f2, A, from);
    static uint8_t body[3000];
    uint16_t blen = 0;
    const char *why = mac80211_rx_(f2.f, f2.n, K_A, body, &blen);
    CHECK(w1.n == 1u && f1.n == 2u && w2.n == 1u && f2.n == 2u && why == NULL &&
              (body[0] & 0x03u) == UMAC_MESH_CTRL_AE_A5A6,
          "(red) a unicast batman frame and A's AE-2 broadcast replica are cut (%u, %u; %s)",
          f1.n, f2.n, why ? why : "ok");
    as_(A, W, K_A, true);
    rx_caps_(&w1, NULL);
    rx_caps_(&w2, NULL);
    rx_caps_(&f1, NULL);
    rx_caps_(&f2, NULL);
    CHECK(f1.n == 2u && f2.n == 2u && simnode_ext_rx_count() == 4u && ext_same_(0, 2) &&
              ext_same_(1, 3),
          "(red) A's batman hook gets each fragmented frame byte-identical to it sent whole (%u)",
          simnode_ext_rx_count());
}

/* ---- (11)-(12) QoS and sequence numbers ---------------------------------------------------- */

static uint16_t seq_(const struct cap *e) { return (uint16_t)((e->bytes[22] | (e->bytes[23] << 8)) >> 4); }

static void t_tid_seq(void)
{
    printf("--- (11)-(12) TID and sequence numbers ---\n");
    static uint8_t pay[1500];
    static struct caps a, b, w, d;
    up_(false);
    SET_RATES(R_1M0);
    g_warthog_hostfrag = 1;
    pay_(pay, 1000, 16);
    unsigned from = simnode_outbox_count();
    (void)simnode_host_tx(A, W, pay, 1000);
    capture_(&b, A, from);
    from = simnode_outbox_count();
    (void)simnode_host_tx_tid(A, W, pay, 1000, 5);
    capture_(&a, A, from);
    CHECK(a.n == 2u && handed_(&a, 5) && (a.f[0].air[30] & 0x0fu) == 5u,
          "(red) a TID 5 MSDU: both fragments QoS TID 5 and handed on TID 5 (%u)", a.n);
    static uint8_t body[3000];
    uint16_t blen = 0;
    const char *why = mac80211_rx_(a.f, a.n, K_A, body, &blen);
    CHECK(why == NULL, "  mac80211's rules met on TID 5 (%s)", why ? why : "ok");
    from = simnode_outbox_count();
    (void)simnode_host_tx(A, W, pay, 100);
    capture_(&w, A, from);
    from = simnode_outbox_count();
    (void)simnode_host_tx(A, W, pay, 1000);
    capture_(&d, A, from);
    CHECK(b.n == 2u && w.n == 1u && d.n == 2u && seq_(&b.f[0]) == seq_(&b.f[1]) &&
              seq_(&w.f[0]) == (uint16_t)(seq_(&b.f[0]) + 1u) &&
              seq_(&d.f[0]) == (uint16_t)(seq_(&w.f[0]) + 1u) && seq_(&d.f[1]) == seq_(&d.f[0]) &&
              (b.f[1].bytes[22] & 0x0fu) == 1u && (d.f[1].bytes[22] & 0x0fu) == 1u,
          "(red) TID 0: an MSDU's fragments share its sequence number, the next MSDU whole or cut "
          "takes the next, the TID 5 MSDU between them none of TID 0's (%u, %u, %u)",
          seq_(&b.f[0]), seq_(&w.f[0]), seq_(&d.f[0]));
    as_(A, W, K_A, false);
    rx_caps_(&a, NULL);
    CHECK(a.n == 2u && simnode_ext_rx_count() == 1u && ext_is_(0, A, W, 0x0800, pay, 1000),
          "(red) A reassembles the TID 5 MSDU (%u)", simnode_ext_rx_count());
}

/* ---- (13) PNs ------------------------------------------------------------------------------- */

static struct umac_sta_data *s_a_rec;
static uint64_t s_hook_pn = UINT64_MAX;
static void take_pn_hook_(void)
{
    /* As path selection to A would on another task: one PN from the link's counter. */
    s_hook_pn = umac_datapath_mesh_take_tx_pn(s_a_rec, 0);
}
/* The MSDU's own buffer is the first TX allocation; its second fragment's the next. */
static void skip_one_hook_(void)
{
    simnode_set_tx_alloc_hook(take_pn_hook_);
}

#if !HOST_SEALS
static void t_pn_chip_(void);
#endif

static void t_pn(void)
{
    printf("--- (13) PNs ---\n");
    static uint8_t pay[1500];
    static struct caps c, all;
    up_(false);
    SET_RATES(R_1M0);
    g_warthog_hostfrag = 1;
    s_a_rec = umac_datapath_mesh_find_peer(A);
    send_a_(&c, 1500, 17, pay);
    bool run = c.n == 3u;
    for (unsigned i = 1; run && i < c.n; i++) { run = pn_of_(c.f[i].air) == pn_of_(c.f[0].air) + i; }
    CHECK(run, "(red) the fragments' PNs on air are consecutive (%llu..)",
          (unsigned long long)(c.n ? pn_of_(c.f[0].air) : 0u));
#if HOST_SEALS
    static struct caps c2;
    s_hook_pn = UINT64_MAX;
    simnode_set_tx_alloc_hook(skip_one_hook_);
    send_a_(&c2, 1500, 18, pay);
    bool run2 = c2.n == 3u, outside = true;
    for (unsigned i = 0; run2 && i < c2.n; i++)
    {
        run2 = pn_of_(c2.f[i].air) == pn_of_(c2.f[0].air) + i;
        outside = outside && pn_of_(c2.f[i].air) != s_hook_pn;
    }
    CHECK(s_hook_pn != UINT64_MAX && run2 && outside &&
              (s_hook_pn + 1u == pn_of_(c2.f[0].air) || s_hook_pn == pn_of_(c2.f[2].air) + 1u),
          "(red) path selection taking a PN while the fragments' buffers are taken lands outside "
          "their run (%llu; run from %llu)", (unsigned long long)s_hook_pn,
          (unsigned long long)(c2.n ? pn_of_(c2.f[0].air) : 0u));
#endif
    (void)simnode_host_tx(A, W, pay, 200);
    capture_(&all, A, 0);
    bool rising = all.n >= 4u;
    for (unsigned i = 1; rising && i < all.n; i++) { rising = pn_of_(all.f[i].air) > pn_of_(all.f[i - 1u].air); }
    CHECK(rising, "(red) every PN to A rises, fragments and whole frames alike: none used twice (%u frames)",
          all.n);
#if !HOST_SEALS
    t_pn_chip_();
#endif
}

#if !HOST_SEALS
/* Data frames to @p ra from outbox entry 0, fragments and whole frames apart. */
static void split_(const uint8_t *ra, struct caps *frags, struct caps *whole)
{
    static struct caps all;
    capture_(&all, ra, 0);
    frags->n = 0;
    whole->n = 0;
    for (unsigned i = 0; i < all.n; i++)
    {
        const bool fr = (all.f[i].air[22] & 0x0fu) != 0u || (all.f[i].air[1] & 0x04u) != 0u;
        struct caps *to = fr ? frags : whole;
        if (to->n < CAP_MAX) { to->f[to->n++] = all.f[i]; }
    }
}

/* The access category of a TID, as the driver queues it. */
static int ac_(int tid)
{
    static const int ac[8] = { 1, 0, 0, 1, 2, 2, 3, 3 };
    return tid < 0 ? -1 : ac[tid & 7];
}

/* The chip sends its oldest held frame, then every frame of another access category it holds,
 * as its per-AC queues may, then the rest in order: a frame on the same AC never overtakes. */
static void chip_overtakes_(void)
{
    const int run_ac = ac_(simnode_tx_held_tid(0));
    (void)simnode_tx_send_held(0);
    for (unsigned i = simnode_tx_held(); i-- > 0u;)
    {
        if (ac_(simnode_tx_held_tid(i)) != run_ac) { (void)simnode_tx_send_held(i); }
    }
    while (simnode_tx_held() != 0u) { (void)simnode_tx_send_held(0); }
    simnode_tx_hold(false);
    simnode_pump();
}

/* Management frames to @p ra in the outbox: how many, and the index of the first. */
static unsigned mgmt_to_(const uint8_t *ra, unsigned *first)
{
    unsigned n = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (f->is_mgmt && f->len >= 24u && MAC_EQ(f->bytes + 4, ra))
        {
            if (n++ == 0u && first != NULL) { *first = i; }
        }
    }
    return n;
}

/* The chip draws a PN as it sends a frame it seals. One it seals under the run's counter
 * between two fragments breaks their run: nothing that could be handed to it then. */
static void t_pn_chip_(void)
{
    static uint8_t pay[1500], body[3000];
    static struct caps fr, wh;
    uint16_t blen = 0;
    /* Another access category to the same peer. */
    up_(false);
    SET_RATES(R_1M0);
    g_warthog_hostfrag = 1;
    simnode_tx_hold(true);
    struct hf s = hf_();
    pay_(pay, 1000, 19);
    (void)simnode_host_tx(A, W, pay, 1000);
    const unsigned held = simnode_tx_held();
    (void)simnode_host_tx_tid(A, W, pay, 50, 6);
    const unsigned held6 = simnode_tx_held();
    CHECK(held == 2u && held6 == 2u && HD(s, wait) >= 1u && HD(s, overlap) == 0u,
          "(red) a TID 6 frame to A while A's TID 0 fragments are in the chip waits for their TX "
          "statuses (%u held, then %u; wait %u, overlap %u)", held, held6, HD(s, wait),
          HD(s, overlap));
    chip_overtakes_();
    split_(A, &fr, &wh);
    const char *why = mac80211_rx_(fr.f, fr.n, K_A, body, &blen);
    CHECK(fr.n == 2u && why == NULL && wh.n == 1u && wh.f[0].tid == 6u,
          "(red) a chip that sends anything it holds between two fragments has nothing to send: "
          "the run's PNs stay consecutive (%s) and the TID 6 frame follows the statuses (%u)",
          why ? why : "ok", wh.n);
    as_(A, W, K_A, false);
    rx_caps_(&fr, NULL);
    CHECK(simnode_ext_rx_count() == 1u && ext_is_(0, A, W, 0x0800, pay, 1000),
          "  and the Warthog's receive path delivers it (%u)", simnode_ext_rx_count());

    /* The run's own access category. */
    up_(false);
    SET_RATES(R_1M0);
    g_warthog_hostfrag = 1;
    simnode_tx_hold(true);
    s = hf_();
    (void)simnode_host_tx(A, W, pay, 1000);
    (void)simnode_host_tx_tid(A, W, pay, 50, 0);
    CHECK(simnode_tx_held() == 3u && HD(s, wait) == 0u && HD(s, overlap) == 0u,
          "(pin) a frame on the fragments' own TID queues behind them at once (%u held)",
          simnode_tx_held());

    /* Chip-sealed management frames to the peer, its ADDBA and path selection under MFP, once
     * the first fragment has gone: the chip seals a management frame as it takes it. */
    up_(false);
    SET_RATES(R_1M0);
    g_warthog_hostfrag = 1;
    simnode_tx_hold(true);
    (void)simnode_host_tx(A, W, pay, 1000);
    (void)simnode_tx_send_held(0);
    g_warthog_mesh_pmf = 1;
    simnode_set_ampdu(true);
    s = hf_();
    (void)simnode_host_tx_tid(A, W, pay, 50, 3); /* TID 0's ADDBA is held off after its cut */
    (void)umac_mesh_hwmp_send_preq(A);
    unsigned first = 0;
    const unsigned during = mgmt_to_(A, NULL);
    CHECK(during == 0u && HD(s, mgmt) == 2u && HD(s, overlap) == 0u,
          "(red) A's ADDBA and a PREQ to A, chip-sealed, wait while its fragments are in the chip "
          "(%u handed, mgmt %u, overlap %u)", during, HD(s, mgmt), HD(s, overlap));
    chip_overtakes_();
    split_(A, &fr, &wh);
    why = mac80211_rx_(fr.f, fr.n, K_A, body, &blen);
    const unsigned after = mgmt_to_(A, &first);
    unsigned last_frag = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (!f->is_mgmt && MAC_EQ(f->bytes + 4, A) && (f->bytes[22] & 0x0fu) != 0u) { last_frag = i; }
    }
    CHECK(fr.n == 2u && why == NULL && after == 2u && first > last_frag,
          "(red) both go once the statuses are in, after the run, whose PNs stay consecutive "
          "(%s; %u management frames)", why ? why : "ok", after);
    simnode_set_ampdu(false);
    g_warthog_mesh_pmf = 0;

    /* Another peer: a STA chip VIF seals every link from one pairwise PN counter. */
    up_(false);
    SET_RATES(R_1M0);
    simnode_chip_shared_pairwise_pn(true);
    g_warthog_hostfrag = 1;
    simnode_tx_hold(true);
    s = hf_();
    (void)simnode_host_tx(A, W, pay, 1000);
    (void)simnode_host_tx_tid(C, W, pay, 50, 6);
    const unsigned heldc = simnode_tx_held();
#if WARTHOG_MESH_CHIP_VIF_MESH
    CHECK(heldc == 3u && HD(s, wait) == 0u,
          "(pin) MESH VIF, a PN counter per key: a TID 6 frame to C goes at once (%u held)", heldc);
#else
    CHECK(heldc == 2u && HD(s, wait) >= 1u,
          "(red) STA VIF, one pairwise PN counter: a TID 6 frame to C waits for A's run (%u held, "
          "wait %u)", heldc, HD(s, wait));
#endif
    chip_overtakes_();
    split_(A, &fr, &wh);
    why = mac80211_rx_(fr.f, fr.n, K_A, body, &blen);
    static struct caps toc, wc;
    split_(C, &toc, &wc);
    CHECK(fr.n == 2u && why == NULL && wc.n == 1u && HD(s, overlap) == 0u,
          "(red) A's run stays consecutive whatever the chip sends between (%s), and C's frame "
          "goes (%u)", why ? why : "ok", wc.n);
}
#endif

/* ---- (14)-(15) memory and the live setting ----------------------------------------------- */

static void t_mem(void)
{
    printf("--- (14) memory ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    up_(false);
    SET_RATES(R_1M0);
    g_warthog_hostfrag = 1;
    pay_(pay, 1500, 20);
    const unsigned live0 = simnode_live_allocs();
    const struct hf s = hf_();
    simnode_outbox_clear();
    simnode_tx_alloc_fail_at(3); /* the frame itself, the second fragment, then the third */
    const unsigned from = simnode_outbox_count();
    (void)simnode_host_tx(A, W, pay, 1500);
    simnode_tx_alloc_fail_at(0);
    capture_(&c, A, from);
    CHECK(c.n == 1u && HD(s, pool) == 1u && HD(s, msdu) == 0u && simnode_live_allocs() == live0,
          "(red) no buffer for the third fragment: the MSDU goes whole, never dropped (pool %u), "
          "nothing left allocated (%u -> %u)", HD(s, pool), live0, simnode_live_allocs());
    g_warthog_hostfrag = 0;
    simnode_outbox_clear();
    const unsigned a0 = simnode_tx_allocs();
    (void)simnode_host_tx(A, W, pay, 1500);
    CHECK(simnode_tx_allocs() - a0 == 1u && simnode_outbox_count() == 1u,
          "(pin) =0: one buffer and one frame for it (%u, %u)", simnode_tx_allocs() - a0,
          simnode_outbox_count());
}

static void t_pool(void)
{
    printf("--- (14) the firmware's 20-block TX pool ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    static const struct { uint32_t mode; unsigned held; const char *what; } full[] = {
        { 1u, 18u, "auto, 3 fragments" }, { 256u, 14u, "=256, 7 fragments" },
    };
    for (unsigned k = 0; k < 2u; k++)
    {
        up_(false);
        SET_RATES(R_1M0);
        simnode_tx_pool(true);
        g_warthog_hostfrag = full[k].mode;
        simnode_tx_hold(true);
        pay_(pay, 64, 40);
        for (unsigned i = 0; i < full[k].held; i++) { (void)simnode_host_tx(C, W, pay, 64); }
        const unsigned held = simnode_tx_held();
        const struct hf s = hf_();
        send_a_(&c, 1500, (uint8_t)(50u + k), pay);
        CHECK(held == full[k].held && c.n == 1u && HD(s, pool) == 1u && HD(s, msdu) == 0u,
              "(red) %s with %u frames in the chip: no room for its fragments, so 1500 octets go "
              "whole, never dropped (%u frames, pool %u)", full[k].what, held, c.n, HD(s, pool));
#if !HOST_SEALS
        CHECK(HD(s, chippn) >= 2u && host_pn_(A) >= chip_pn_(A) + 2u,
              "  the chip may cut it: the host counts its PNs as so (chippn %u)", HD(s, chippn));
#endif
    }

    up_(false);
    SET_RATES(R_1M0);
    simnode_tx_pool(true);
    g_warthog_hostfrag = 1;
    simnode_tx_hold(true);
    struct hf s = hf_();
    pay_(pay, 1500, 52);
    for (unsigned i = 0; i < 8u; i++) { (void)simnode_host_tx_nopump(A, W, pay, 1500); }
    simnode_pump();
    for (unsigned g = 0; g < 200u && simnode_tx_held() != 0u; g++)
    {
        (void)simnode_tx_send_held(0);
        simnode_pump();
    }
    CHECK(HD(s, msdu) == 8u && HD(s, pool) == 0u && HD(s, wait) >= 1u && HD(s, ok) == 8u,
          "(red) 8 MSDUs queued at once: each waits for its fragments' blocks and goes cut "
          "(msdu %u, ok %u, pool %u, wait %u)", HD(s, msdu), HD(s, ok), HD(s, pool), HD(s, wait));

    for (uint32_t mode = 0; mode <= 1u; mode++)
    {
        up_(false);
        SET_RATES(R_1M0);
        simnode_tx_pool(true);
        g_warthog_hostfrag = mode;
        simnode_tx_hold(true);
        s = hf_();
        const unsigned a0 = simnode_outbox_count();
        unsigned offered = 0;
        for (unsigned t = 0; t < 600u; t++)
        {
            while (!simnode_tx_pool_paused() && offered < 60u)
            {
                (void)simnode_host_tx(A, W, pay, 1500);
                offered++;
            }
            (void)simnode_tx_send_held(0);
            simnode_pump();
        }
        if (mode == 0u)
        {
            CHECK(offered == 60u && simnode_outbox_count() - a0 == 60u &&
                      simnode_tx_pool_reserve() == 0u,
                  "(pin) off: a producer that waits for the pool sends 60 MSDUs whole, none lost "
                  "(%u frames, reserve %lu)", simnode_outbox_count() - a0,
                  (unsigned long)simnode_tx_pool_reserve());
        }
        else
        {
            CHECK(offered == 60u && HD(s, msdu) == 60u && HD(s, ok) == 60u && HD(s, pool) == 0u &&
                      simnode_tx_pool_reserve() >= 2u,
                  "(red) auto: a producer that waits for the pool has all 60 MSDUs cut and acked "
                  "(msdu %u, ok %u, pool %u; the pool keeps %lu blocks back)", HD(s, msdu),
                  HD(s, ok), HD(s, pool), (unsigned long)simnode_tx_pool_reserve());
        }
    }
    up_(false);
}

#if !HOST_SEALS
static void t_chip_pn(void)
{
    printf("--- (17) the host's PN count never falls behind the chip's ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    up_(false);
    SET_RATES(R_2M7);
    simnode_set_chip_frag_threshold(512);
    const uint64_t base = chip_pn_(A);
    const struct hf s = hf_();
    for (unsigned i = 0; i < 5u; i++) { send_a_(&c, 1000, (uint8_t)(60u + i), pay); }
    const uint64_t host = host_pn_(A), chip = chip_pn_(A);
    CHECK(c.n == 1u && c.f[0].pn_draws == 3u && chip - base == 15u && host >= chip &&
              HD(s, chippn) >= 10u,
          "(red) AT+HOSTFRAG=0, AT+FRAG=512: the chip cut each of 5 frames in 3 and drew 15 PNs; "
          "the host counted %llu of them (chippn %u)", (unsigned long long)(host - base),
          HD(s, chippn));
    simnode_keyinst_clear();
    simnode_del_peer(C);
    const struct simnode_keyinst *k = NULL;
    for (unsigned i = 0; i < simnode_keyinst_count(); i++)
    {
        const struct simnode_keyinst *e = simnode_keyinst_get(i);
        if (e->pairwise && memcmp(e->key, K_A, 16) == 0) { k = e; }
    }
    CHECK(k != NULL && k->tx_pn >= chip,
          "(pin) C's removal re-installs A's key at 0x%llx, above every PN the chip drew (0x%llx)",
          k ? (unsigned long long)k->tx_pn : 0ull, (unsigned long long)chip);
}
#endif

static void t_untried(void)
{
    printf("--- (18) a fragment's status comes back even untried ---\n");
    static uint8_t pay[1500];
    static const char *how[] = { "handed back by the chip", "dropped by the driver" };
    pay_(pay, 1000, 70);
    for (unsigned k = 0; k < 2u; k++)
    {
        up_(false);
        SET_RATES(R_1M0);
        g_warthog_hostfrag = 1;
        simnode_tx_hold(true);
        const struct hf s = hf_();
        (void)simnode_host_tx(A, W, pay, 1000);
        for (unsigned i = 0; i < 2u; i++)
        {
            (void)(k == 0u ? simnode_tx_return_held(0) : simnode_tx_drop_held(0));
        }
        simnode_pump();
        (void)simnode_host_tx_tid(A, W, pay, 50, 6);
        CHECK(HD(s, unsent) == 2u && HD(s, fail) == 1u && simnode_tx_held() == 1u &&
                  HD(s, overlap) == 0u && HD(s, wait) == 0u,
              "(red) both fragments %s: unsent %u, the MSDU fails (%u), and a TID 6 frame to A goes "
              "at once (%u held, overlap %u)", how[k], HD(s, unsent), HD(s, fail),
              simnode_tx_held(), HD(s, overlap));
    }
    up_(false);
    SET_RATES(R_1M0);
    g_warthog_hostfrag = 1;
    simnode_tx_hold(true);
    const struct hf s = hf_();
    (void)simnode_host_tx(A, W, pay, 1000);
    (void)simnode_tx_forget_held(0);
    (void)simnode_tx_forget_held(0);
    (void)simnode_host_tx_tid(A, W, pay, 50, 6);
    const unsigned before = simnode_tx_held();
    simnode_advance_ms(16001);
    simnode_pump();
    CHECK(before == (HOST_SEALS ? 1u : 0u) && simnode_tx_held() == 1u && HD(s, stale) == 1u,
          "(red) a run whose statuses never come is cleared after 16 s (stale %u)%s (%u, then %u held)",
          HD(s, stale), HOST_SEALS ? "" : ", the TID 6 frame waiting until then", before,
          simnode_tx_held());
}

static void t_live(void)
{
    printf("--- (15) AT+HOSTFRAG applies to the next frame ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    up_(false);
    SET_RATES(R_1M0);
    g_warthog_hostfrag = 1;
    send_a_(&c, 1000, 21, pay);
    const unsigned on = c.n;
    g_warthog_hostfrag = 0;
    send_a_(&c, 1000, 22, pay);
    const unsigned off = c.n;
    g_warthog_hostfrag = 1;
    send_a_(&c, 1000, 23, pay);
    CHECK(on == 2u && off == 1u && c.n == 2u, "(red) auto, off, auto: %u, %u, %u frames", on, off, c.n);
}

/* ---- (16) the open and the keyed non-SAE mesh ------------------------------------------ */

/* The keyed non-SAE mesh's pairwise key: a constant in umac_datapath_mesh.c, not a secret. */
static const uint8_t K_P1[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                  0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };

static void up_plain_(const uint8_t *me, const uint8_t *peer, bool secure)
{
    simnode_del_peer(NULL);
    simnode_set_batman(false);
    simnode_set_ampdu(false);
    g_warthog_ampdu = 1;
    simnode_tx_hold(false);
    (void)simnode_start(me);
    simnode_set_gates(false, false, false, secure);
    g_warthog_host_ccmp_on = 0;
    simnode_set_rx_ext_cb(true);
    (void)simnode_add_peer(peer);
    g_warthog_hostfrag = 0;
    simnode_set_rate_chain(NULL, 0);
    simnode_set_chip_frag_threshold(0);
    simnode_outbox_clear();
    simnode_ext_rx_clear();
}

static void t_plain_meshes(void)
{
    printf("--- (16) the open and the keyed non-SAE mesh ---\n");
    static uint8_t pay[1500], body[3000];
    static struct caps c;
    for (int secure = 0; secure <= 1; secure++)
    {
        const char *what = secure ? "keyed non-SAE" : "open";
        up_plain_(W, A, secure != 0);
        SET_RATES(R_1M0);
        g_warthog_hostfrag = 1;
        send_a_(&c, 1000, (uint8_t)(30 + secure), pay);
        uint16_t blen = 0;
        const char *why = mac80211_rx_(c.f, c.n, secure ? K_P1 : NULL, body, &blen);
        CHECK(c.n == 2u && why == NULL && body_is_(body, blen, 6, 0, 0x0800, pay, 1000) &&
                  sized_(&c, CAP_1M_MCS0),
              "(red) %s mesh: 1000 octets at 1 MHz MCS0 in 2 fragments, mac80211's rules met (%u, %s)",
              what, c.n, why ? why : "ok");
        up_plain_(A, W, secure != 0);
        rx_caps_(&c, NULL);
        CHECK(c.n == 2u && simnode_ext_rx_count() == 1u && ext_is_(0, A, W, 0x0800, pay, 1000),
              "(red) %s mesh: the Warthog's receive path delivers it once, byte-identical (%u)",
              what, simnode_ext_rx_count());
    }
}

/* ---- (22) Block Ack fields in the descriptor ---------------------------------------------- */

/* The slot whose published MAC ends like @p m, -1 if none (AT+AMPDU?'s per-peer line). */
static int ampdu_peer_(const uint8_t *m)
{
    const uint32_t want = 0x1000000u | ((uint32_t)m[3] << 16) | ((uint32_t)m[4] << 8) | m[5];
    for (int i = 0; i < 4; i++) { if (g_warthog_ampdu_peer_mac[i] == want) { return i; } }
    return -1;
}

static void t_ba_params(void)
{
    printf("--- (22) Block Ack fields: none on a fragment, ours only under our session (AT+TIDPARAMS) ---\n");
    static uint8_t pay[1500], f[64];
    static struct caps c;
    for (int legacy = 1; legacy >= 0; legacy--)
    {
        up_(false);
        simnode_set_ampdu(true);
        simnode_chip_agg_on_baparams(true);
        g_warthog_ba_txparm = legacy ? 0u : 1u;
        const bool cs = session_(C, 0);
        (void)simnode_rx(f, addba_req_(f, A, 0), -60);
        struct umac_sta_data *ra = umac_datapath_mesh_find_peer(A);
        const bool rcpt = ra != NULL && umac_ba_recipient_agreed(ra, 0);
        SET_RATES(R_1M0);
        g_warthog_hostfrag = 1;
        struct hf s = hf_();
        const uint32_t rc0 = g_warthog_hostfrag_ba_rcpt;
        send_a_(&c, 1500, (uint8_t)(70 + legacy), pay);
        simnode_tick();
        unsigned withf = 0;
        for (unsigned i = 0; i < c.n; i++) { withf += c.f[i].reorder != 0u || (c.f[i].tx_flags & MMDRV_TX_FLAG_AMPDU_ENABLED) != 0u; }
        const int pa = ampdu_peer_(A), pc = ampdu_peer_(C);
        const bool peers = pa >= 0 && pc >= 0 && (g_warthog_ampdu_peer_ba[pc] & 1u) != 0u &&
                           (g_warthog_ampdu_peer_ba[pa] & 1u) == 0u && (g_warthog_ampdu_peer_ba[pa] & (1u << 16)) != 0u;
        if (legacy)
        {
            CHECK(cs && rcpt && c.n == 3u && g_warthog_ampdu_orig == 1u && HD(s, ba_end) == 0u &&
                      g_warthog_hostfrag_ba_rcpt - rc0 == 1u && withf == 3u && HD(s, agg) == 3u && peers,
                  "(red) =0, our session to C (orig %lu) and A's to us: a frame cut for A ends nothing "
                  "(ba_end %u, ba_rcpt %lu), its %u fragments carry A's reorder size (%u) and go aggregated "
                  "(agg %u); AT+AMPDU? shows C's session ours and A's its own",
                  (unsigned long)g_warthog_ampdu_orig, HD(s, ba_end),
                  (unsigned long)(g_warthog_hostfrag_ba_rcpt - rc0), c.n, c.n ? c.f[0].reorder : 0u, HD(s, agg));
        }
        else
        {
            CHECK(cs && rcpt && c.n == 3u && HD(s, ba_end) == 0u && withf == 0u && HD(s, agg) == 0u && peers,
                  "(red) =1: the same frame's fragments carry no Block Ack field and none goes aggregated "
                  "(%u with one, agg %u)", withf, HD(s, agg));
        }
        /* Whole frames: to C under our session, to A with none of ours. */
        pay_(pay, 200, 72);
        unsigned from = simnode_outbox_count();
        (void)simnode_host_tx(C, W, pay, 200);
        (void)simnode_host_tx(A, W, pay, 200);
        const unsigned wc = first_data_(C, from), wa = first_data_(A, from);
        const struct simnode_frame *fc = wc != UINT32_MAX ? simnode_outbox_get(wc) : NULL;
        const struct simnode_frame *fa = wa != UINT32_MAX ? simnode_outbox_get(wa) : NULL;
        CHECK(fc != NULL && fa != NULL && fc->reorder == (legacy ? 0u : 16u) &&
                  (fc->tx_flags & MMDRV_TX_FLAG_AMPDU_ENABLED) != 0u &&
                  (fa->tx_flags & MMDRV_TX_FLAG_AMPDU_ENABLED) == 0u && fa->reorder == (legacy ? 16u : 0u),
              "%s whole frames: to C as A-MPDU with %s (%u), to A %s (%u)", legacy ? "(pin)" : "(red)",
              legacy ? "no size, morselib's rule" : "its agreed size", fc ? fc->reorder : 0u,
              legacy ? "with A's session's size" : "with none", fa ? fa->reorder : 0u);
    }
    /* AT+HOSTFRAG=0 with AT+AMPDU=1: morselib's rule whatever AT+TIDPARAMS says. */
    g_warthog_hostfrag = 0;
    pay_(pay, 200, 73);
    unsigned from = simnode_outbox_count();
    (void)simnode_host_tx(A, W, pay, 200);
    const unsigned w0 = first_data_(A, from);
    CHECK(w0 != UINT32_MAX && simnode_outbox_get(w0)->reorder == 16u,
          "(pin) AT+HOSTFRAG=0, AT+AMPDU=1: a frame to A carries A's session's size as before (%u)",
          w0 != UINT32_MAX ? simnode_outbox_get(w0)->reorder : 0u);
    g_warthog_ampdu = 0;
    from = simnode_outbox_count();
    (void)simnode_host_tx(A, W, pay, 200);
    const unsigned w1 = first_data_(A, from);
    CHECK(w1 != UINT32_MAX && simnode_outbox_get(w1)->reorder == 0u,
          "(red) AT+AMPDU=0: no Block Ack field on it (%u)", w1 != UINT32_MAX ? simnode_outbox_get(w1)->reorder : 0u);
    g_warthog_ampdu = 1;

    /* (26) A ends its own session to us: morselib keeps its reorder size until A's next ADDBA. */
    up_(false);
    simnode_set_ampdu(true);
    simnode_chip_agg_on_baparams(true);
    g_warthog_ba_txparm = 0;
    (void)simnode_rx(f, addba_req_(f, A, 0), -60);
    (void)simnode_rx(f, delba_(f, A, 0, 37, /*originator=*/true), -60);
    struct umac_sta_data *ra = umac_datapath_mesh_find_peer(A);
    const bool ended = ra != NULL && !umac_ba_recipient_agreed(ra, 0) && umac_ba_get_reorder_buffer_size(ra, 0) == 16u;
    SET_RATES(R_1M0);
    g_warthog_hostfrag = 1;
    struct hf s2 = hf_();
    const uint32_t rc2 = g_warthog_hostfrag_ba_rcpt;
    send_a_(&c, 1500, 75, pay);
    CHECK(ended && c.n == 3u && c.f[0].reorder == 16u && HD(s2, ba_end) == 0u && HD(s2, agg) == 3u &&
              g_warthog_hostfrag_ba_rcpt - rc2 == 1u,
          "(red) =0, A's session ended by A: a frame cut for A carries the stale size (%u), goes aggregated "
          "(agg %u) and counts ba_rcpt (%lu)", c.n ? c.f[0].reorder : 0u, HD(s2, agg),
          (unsigned long)(g_warthog_hostfrag_ba_rcpt - rc2));
    g_warthog_hostfrag = 0;
    pay_(pay, 200, 74);
    from = simnode_outbox_count();
    (void)simnode_host_tx(A, W, pay, 200);
    const unsigned w2 = first_data_(A, from);
    CHECK(w2 != UINT32_MAX && simnode_outbox_get(w2)->reorder == 16u,
          "(pin) after A's DELBA, AT+HOSTFRAG=0: a frame to A still carries A's old size (%u)",
          w2 != UINT32_MAX ? simnode_outbox_get(w2)->reorder : 0u);
    simnode_chip_agg_on_baparams(false);
    g_warthog_ba_txparm = 1;
}

/* ---- (24) the connection's flags on every fragment --------------------------------------- */

static void t_populate(void)
{
    printf("--- (24) the connection's TX flags on every fragment ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    const uint8_t want = MMDRV_TX_FLAG_TP_ENABLED | MMDRV_TX_FLAG_CR_1MHZ_PRE_ENABLED;
    up_(false);
    simnode_set_populate_flags(want);
    SET_RATES(R_1M0);
    g_warthog_hostfrag = 1;
    send_a_(&c, 1500, 90, pay);
    unsigned with = 0;
    for (unsigned i = 0; i < c.n; i++) { with += (c.f[i].tx_flags & want) == want; }
    CHECK(c.n == 3u && with == 3u, "(red) 1500 octets cut in %u: %u carry traveling pilots and 1 MHz "
          "control responses", c.n, with);
    pay_(pay, 200, 91);
    send_a_(&c, 200, 92, pay);
    CHECK(c.n == 1u && (c.f[0].tx_flags & want) == want, "(pin) a frame sent whole carries them too");
    simnode_set_populate_flags(0);
}

int main(void)
{
    CHECK(g_warthog_hostfrag_hold_ms == UMAC_MESH_FRAG_BA_HOLD_MS,
          "(red) AT+HOSTFRAG? reads hold_ms %lu before the mesh has run (want %u)",
          (unsigned long)g_warthog_hostfrag_hold_ms, UMAC_MESH_FRAG_BA_HOLD_MS);
    t_off();
    t_ba_off();
    t_auto_rate();
    t_auto_whole();
    t_auto_chip();
    t_thresh();
    t_rates();
    t_whole_chain();
    t_status();
    t_ba();
    t_ba_wait();
    t_ba_counts();
    t_ampdu();
    t_group();
    t_relay();
    t_batman();
    t_tid_seq();
    t_pn();
    t_mem();
    t_pool();
    t_live();
    t_plain_meshes();
#if !HOST_SEALS
    t_chip_pn();
#endif
    t_untried();
    t_ba_params();
    t_populate();
    simnode_stop();
    printf(failures ? "test_simnode_hostfrag: %d FAILED\n" : "test_simnode_hostfrag: all passed\n",
           failures);
    return failures != 0;
}
