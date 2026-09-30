/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * BATMAN_V member mode through the real 802.11s datapath: the exact air bytes a
 * batman link frame becomes, what the receive hook main/bat_port.c registers is
 * handed back, and the peer-link snapshot the engine's link throughput reads.
 *
 * Why the broadcast shape matters: batman-adv on a Linux peer accepts ELP, OGM2
 * and BCAST only with Ethernet destination ff:ff:ff:ff:ff:ff. mac80211 rebuilds
 * the Ethernet header of an individually addressed mesh frame from Address
 * Extension mode 2 (addr5 -> dst, addr6 -> src), so a per-peer replica carrying
 * addr5 = ff:ff:ff:ff:ff:ff reaches batman as a broadcast. warthog's default leaf
 * replica rewrites the destination to the peer and carries no AE: batman-adv drops
 * it. With g_warthog_mesh_batman set, group frames take the AE-2 shape (MESHGRP=0)
 * or stay one standard group frame (MESHGRP=1); unicast is unchanged.
 *
 * The last case carries frames the clean-room engine (main/bat/) emits across two
 * real stacks in turn -- sender datapath, air bytes, receiver datapath, receive
 * hook, receiver engine -- until an ELP exchange, an OGM and a unicast soft frame
 * have crossed and been delivered.
 *
 * What this cannot tell you: anything the radio or a mac80211 peer does. The shapes
 * a Linux peer accepts were checked separately against mac80211_hwsim (the VM
 * suite); the Morse chip's handling of them has never been measured on air.
 */
#include <stdio.h>
#include <string.h>

#include "simnode.h"
#include "mmdrv.h"
#include "mmpkt.h"
#include "mmwlan.h"
#include "mmwlan_mesh.h"
#include "umac_mesh_ctrl.h"
#include "umac_mesh_fwd.h"
#include "umac_mesh_ies.h"

#include "bat.h"
#include "bat_mode.h"

extern volatile uint32_t g_warthog_rxdrop_reason, g_warthog_rxdrop_count;

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static const uint8_t W[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 }; /* us */
static const uint8_t A[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0a };
static const uint8_t C[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0c };
static const uint8_t D[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0d };
static const uint8_t E[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0e }; /* not a peer */
static const uint8_t H2[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x88 }; /* host behind A */
static const uint8_t BC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static const uint8_t SNAP_BAT[8] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x43, 0x05 };

/* A 20-byte ELP: type, version, originator, seqno, interval, throughput 0. */
static const uint8_t ELP[20] = { 0x03, 0x0f, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
                                 0x12, 0x34, 0x56, 0x78, 0x00, 0x00, 0x01, 0xf4,
                                 0x00, 0x00, 0x00, 0x00 };

#define MAC_EQ(p, m) (memcmp((p), (m), 6) == 0)

static void hexdump(const char *tag, const uint8_t *b, unsigned n)
{
    printf("     %s (%u):", tag, n);
    for (unsigned i = 0; i < n; i++) { printf("%s%02x", (i % 6) ? "" : " ", b[i]); }
    printf("\n");
}

static uint16_t ethertype_of(const struct simnode_extrx *e)
{
    return (uint16_t)((e->frame[12] << 8) | e->frame[13]);
}

/* Injected frames are built with the shipping builders, as test_simnode_datapath does. */
static uint16_t put_tail_(uint8_t *f, uint16_t n, const struct umac_mesh_ctrl *mc,
                          const uint8_t *pay, uint16_t pay_len)
{
    f[n++] = 0x00;
    f[n++] = 0x01; /* QoS Control: TID 0, Mesh Control Present */
    n = (uint16_t)(n + umac_mesh_ctrl_build(&f[n], 18u, mc));
    memcpy(&f[n], SNAP_BAT, sizeof(SNAP_BAT));
    n = (uint16_t)(n + sizeof(SNAP_BAT));
    memcpy(&f[n], pay, pay_len);
    return (uint16_t)(n + pay_len);
}

static uint16_t mk_uni(uint8_t *f, const uint8_t *ra, const uint8_t *ta, const uint8_t *da,
                       const uint8_t *sa, const struct umac_mesh_ctrl *mc, const uint8_t *pay,
                       uint16_t pay_len)
{
    return put_tail_(f, umac_mesh_ies_build_data_hdr4(f, ra, ta, da, sa), mc, pay, pay_len);
}

static uint16_t mk_grp(uint8_t *f, const uint8_t *gda, const uint8_t *ta, const uint8_t *msa,
                       const struct umac_mesh_ctrl *mc, const uint8_t *pay, uint16_t pay_len)
{
    return put_tail_(f, umac_mesh_ies_build_data_hdr3_group(f, gda, ta, msa), mc, pay, pay_len);
}

static const struct simnode_frame *frame_to(const uint8_t *addr1)
{
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (f != NULL && !f->is_mgmt && f->len >= 10u && MAC_EQ(&f->bytes[4], addr1)) { return f; }
    }
    return NULL;
}

/* Node @p self, open mesh, the gates batman mode runs with (forwarding and bridge off). */
static void fresh(const uint8_t *self, bool batman, bool grp_std, bool ext)
{
    simnode_del_peer(NULL);
    (void)simnode_start(self);
    simnode_set_gates(/*fwd=*/false, /*bridge=*/false, grp_std, /*secure=*/false);
    simnode_set_batman(batman);
    simnode_set_rx_ext_cb(ext);
    simnode_outbox_clear();
    simnode_host_rx_clear();
    simnode_ext_rx_clear();
    g_warthog_rxdrop_count = 0;
    g_warthog_rxdrop_reason = 0;
}

/* ---- 1. batman on, MESHGRP=0: one AE-2 replica per ESTAB peer ------------ */

static void t_tx_replica_ae2(void)
{
    printf("--- TX: batman on, MESHGRP=0: a broadcast ELP is one AE-2 replica per peer ---\n");
    fresh(W, /*batman=*/true, /*grp_std=*/false, /*ext=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    (void)simnode_add_peer(D);

    int st = simnode_host_tx_eth(NULL, BC, W, 0x4305, ELP, sizeof(ELP));
    CHECK(st == MMWLAN_SUCCESS, "mmwlan_tx_pkt-shaped TX of ff:ff/0x4305 with ra NULL succeeds (%d)", st);
    CHECK(simnode_outbox_count() == 3, "exactly three frames, one per ESTAB peer (got %u)",
          simnode_outbox_count());

    const uint8_t *peers[3] = { A, C, D };
    uint32_t seq0 = 0;
    for (int i = 0; i < 3; i++)
    {
        const struct simnode_frame *f = frame_to(peers[i]);
        CHECK(f != NULL, "a copy addressed to peer %02x", peers[i][5]);
        if (f == NULL) { continue; }
        if (f->len != 58u + sizeof(ELP)) { hexdump("replica", f->bytes, f->len); }
        CHECK(f->len == 58u + sizeof(ELP),
              "  30 MAC + 2 QoS + 18 Mesh Control + 8 LLC + 20 ELP = %u bytes (got %u)",
              (unsigned)(58u + sizeof(ELP)), f->len);
        if (f->len < 58u + sizeof(ELP)) { continue; }
        CHECK(f->bytes[0] == 0x88 && f->bytes[1] == 0x03,
              "  [0..1] FC 0x0388: QoS Data, ToDS|FromDS (got %02x%02x)", f->bytes[1], f->bytes[0]);
        CHECK(MAC_EQ(&f->bytes[4], peers[i]) && MAC_EQ(&f->bytes[16], peers[i]),
              "  [4]/[16] addr1 = addr3 = the peer: mac80211 takes it as addressed to itself");
        CHECK(MAC_EQ(&f->bytes[10], W) && MAC_EQ(&f->bytes[24], W), "  [10]/[24] addr2 = addr4 = us");
        CHECK(f->bytes[30] == 0x00 && f->bytes[31] == 0x01,
              "  [30..31] QoS 0x0100: TID 0, only the Mesh Control Present bit (got %02x%02x)",
              f->bytes[31], f->bytes[30]);
        CHECK(f->bytes[32] == 0x02, "  [32] Mesh Control flags 0x02: AE mode 2, nothing else (got %02x)",
              f->bytes[32]);
        CHECK(f->bytes[33] == UMAC_MESH_CTRL_TTL_DEFAULT,
              "  [33] TTL %u: mac80211 drops a mesh data frame at TTL 0 (got %u)",
              UMAC_MESH_CTRL_TTL_DEFAULT, f->bytes[33]);
        CHECK(MAC_EQ(&f->bytes[38], BC), "  [38] addr5 = ff:ff:ff:ff:ff:ff -> the peer's Ethernet dst");
        CHECK(MAC_EQ(&f->bytes[44], W), "  [44] addr6 = our mesh MAC -> the peer's Ethernet src");
        CHECK(memcmp(&f->bytes[50], SNAP_BAT, sizeof(SNAP_BAT)) == 0,
              "  [50] LLC aa aa 03 00 00 00 43 05");
        CHECK(memcmp(&f->bytes[58], ELP, sizeof(ELP)) == 0, "  [58] the 20 ELP bytes, unchanged");
        uint32_t seq = (uint32_t)f->bytes[34] | ((uint32_t)f->bytes[35] << 8) |
                       ((uint32_t)f->bytes[36] << 16) | ((uint32_t)f->bytes[37] << 24);
        if (i == 0) { seq0 = seq; }
        CHECK(seq == seq0, "  one Mesh Control sequence number for all replicas (%lu)", (unsigned long)seq);
    }
    CHECK(simnode_stub_hits("umac_connection_get_state") == 0, "nothing fell into STA-mode code");
}

/* ---- 2. batman on, MESHGRP=1: one standard group frame ------------------- */

static void t_tx_std_group(void)
{
    printf("--- TX: batman on, MESHGRP=1: one standard 3-address group frame ---\n");
    fresh(W, /*batman=*/true, /*grp_std=*/true, /*ext=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    (void)simnode_add_peer(D);

    int st = simnode_host_tx_eth(NULL, BC, W, 0x4305, ELP, sizeof(ELP));
    CHECK(st == MMWLAN_SUCCESS && simnode_outbox_count() == 1,
          "one transmission for three peers (%d, %u frames)", st, simnode_outbox_count());
    const struct simnode_frame *f = simnode_outbox_get(0);
    if (f == NULL || f->len != 40u + sizeof(ELP))
    {
        CHECK(false, "24 MAC + 2 QoS + 6 Mesh Control + 8 LLC + 20 = %u bytes (got %u)",
              (unsigned)(40u + sizeof(ELP)), f ? f->len : 0u);
        return;
    }
    CHECK(f->bytes[0] == 0x88 && f->bytes[1] == 0x02, "[0..1] FC 0x0288: FromDS only (got %02x%02x)",
          f->bytes[1], f->bytes[0]);
    CHECK(MAC_EQ(&f->bytes[4], BC), "[4]  addr1 = ff:ff:ff:ff:ff:ff");
    CHECK(MAC_EQ(&f->bytes[10], W) && MAC_EQ(&f->bytes[16], W), "[10]/[16] addr2 = addr3 = us");
    CHECK(f->bytes[24] == 0x00 && f->bytes[25] == 0x01,
          "[24..25] QoS 0x0100: TID 0, only the Mesh Control Present bit (got %02x%02x)",
          f->bytes[25], f->bytes[24]);
    CHECK(f->bytes[26] == 0x00, "[26] Mesh Control flags 0: AE 0 (got %02x)", f->bytes[26]);
    CHECK(f->bytes[27] == UMAC_MESH_CTRL_TTL_DEFAULT, "[27] TTL %u (got %u)",
          UMAC_MESH_CTRL_TTL_DEFAULT, f->bytes[27]);
    CHECK(memcmp(&f->bytes[32], SNAP_BAT, sizeof(SNAP_BAT)) == 0 &&
              memcmp(&f->bytes[40], ELP, sizeof(ELP)) == 0,
          "[32] LLC 0x4305 then the ELP");
}

/* ---- 3. batman off: today's pinned leaf shape, unchanged ------------------ */

static void t_tx_batman_off(void)
{
    printf("--- TX: batman off (regression): plain replicas, DA rewritten to each peer ---\n");
    fresh(W, /*batman=*/false, /*grp_std=*/false, /*ext=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);

    (void)simnode_host_tx_eth(NULL, BC, W, 0x4305, ELP, sizeof(ELP));
    int plain = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (f != NULL && f->len == 46u + sizeof(ELP) && f->bytes[32] == 0x00 &&
            MAC_EQ(&f->bytes[16], &f->bytes[4]))
        {
            plain++;
        }
    }
    CHECK(plain == 2 && simnode_outbox_count() == 2,
          "two 6-octet-Mesh-Control replicas, addr3 = the peer, no AE (%d of %u)", plain,
          simnode_outbox_count());
}

/* ---- 4. unicast with ra ------------------------------------------------- */

static void t_tx_unicast_ra(void)
{
    printf("--- TX: batman unicast goes to the next hop given as ra, or nowhere ---\n");
    fresh(W, /*batman=*/true, /*grp_std=*/false, /*ext=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);

    static const uint8_t UC[30] = { 0x40, 0x0f, 0x32, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x0c };
    int st = simnode_host_tx_eth(C, C, W, 0x4305, UC, sizeof(UC));
    CHECK(st == MMWLAN_SUCCESS && simnode_outbox_count() == 1, "ra = C: one frame (%d, %u)", st,
          simnode_outbox_count());
    const struct simnode_frame *f = frame_to(C);
    if (f != NULL && f->len == 46u + sizeof(UC))
    {
        CHECK(f->bytes[0] == 0x88 && f->bytes[1] == 0x03 && MAC_EQ(&f->bytes[16], C) &&
                  MAC_EQ(&f->bytes[24], W),
              "4-address, addr3 = C, addr4 = us");
        CHECK(f->bytes[32] == 0x00, "Mesh Control without AE (got %02x)", f->bytes[32]);
        CHECK(memcmp(&f->bytes[38], SNAP_BAT, 8) == 0 && memcmp(&f->bytes[46], UC, sizeof(UC)) == 0,
              "LLC 0x4305 and the batman packet");
    }
    else
    {
        CHECK(false, "a %u-byte frame to C (got %u)", (unsigned)(46u + sizeof(UC)), f ? f->len : 0u);
    }

    simnode_outbox_clear();
    unsigned live = simnode_live_allocs();
    st = simnode_host_tx_eth(E, E, W, 0x4305, UC, sizeof(UC));
    CHECK(st == MMWLAN_NOT_FOUND, "ra = a non-neighbour: MMWLAN_NOT_FOUND (%d), bat_port's BAT_TX_NOPEER", st);
    CHECK(simnode_outbox_count() == 0, "nothing on air (%u)", simnode_outbox_count());
    CHECK(simnode_live_allocs() == live, "and the packet was consumed (%u -> %u live)", live,
          simnode_live_allocs());
}

/* A 1514-byte link frame, the hard MTU's full 1500 batman bytes, in each shape the datapath
 * gives it: the MPDU lengths the chip must carry (no FCS). */
static void t_tx_max_size(void)
{
    printf("--- TX/RX: a 1500-byte batman payload in every shape ---\n");
    static uint8_t big[1500];
    for (unsigned i = 0; i < sizeof(big); i++) { big[i] = (uint8_t)(i * 7u + 1u); }
    big[0] = 0x40;

    fresh(W, /*batman=*/true, /*grp_std=*/false, /*ext=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    (void)simnode_host_tx_eth(C, C, W, 0x4305, big, sizeof(big));
    const struct simnode_frame *f = frame_to(C);
    CHECK(simnode_outbox_count() == 1 && f != NULL && f->len == 46u + sizeof(big) &&
              memcmp(&f->bytes[46], big, sizeof(big)) == 0,
          "unicast to C: 30 MAC + 2 QoS + 6 Mesh Control + 8 LLC + 1500 = 1546 bytes (%u, got %u)",
          simnode_outbox_count(), f != NULL ? f->len : 0u);

    simnode_outbox_clear();
    (void)simnode_host_tx_eth(NULL, BC, W, 0x4305, big, sizeof(big));
    f = frame_to(A);
    CHECK(simnode_outbox_count() == 2 && f != NULL && f->len == 58u + sizeof(big) &&
              memcmp(&f->bytes[58], big, sizeof(big)) == 0,
          "AE-2 replica: 30 + 2 + 18 + 8 + 1500 = 1558 bytes (%u, got %u)",
          simnode_outbox_count(), f != NULL ? f->len : 0u);

    fresh(W, /*batman=*/true, /*grp_std=*/true, /*ext=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_host_tx_eth(NULL, BC, W, 0x4305, big, sizeof(big));
    f = simnode_outbox_get(0);
    CHECK(simnode_outbox_count() == 1 && f != NULL && f->len == 40u + sizeof(big),
          "standard group frame: 24 + 2 + 6 + 8 + 1500 = 1540 bytes (got %u)", f != NULL ? f->len : 0u);
    CHECK(simnode_outbox_dropped() == 0, "none too big for the capture (%u)", simnode_outbox_dropped());

    fresh(W, /*batman=*/true, /*grp_std=*/false, /*ext=*/true);
    (void)simnode_add_peer(A);
    static uint8_t frame[1600];
    struct umac_mesh_ctrl mc = { .flags = 0, .ttl = 31, .seq = 1200 };
    uint16_t n = mk_grp(frame, BC, A, A, &mc, big, sizeof(big));
    (void)simnode_rx(frame, n, -55);
    const struct simnode_extrx *e = simnode_ext_rx_get(0);
    CHECK(e != NULL && e->len == 14u + sizeof(big) && memcmp(&e->frame[14], big, sizeof(big)) == 0 &&
              bat_mode_rx_classify(e->frame, e->len, e->ta) == BAT_MODE_RX_BATMAN,
          "a Linux peer's 1540-byte group frame reaches the hook as 1514 bytes, intact, kept (%u)",
          e != NULL ? e->len : 0u);
}

/* Under SAE a slot exists from hostap's sta_add until AMPE keys it (and again while a
 * dropped link re-runs SAE). Unicast queued to it would only be dropped at TX (tx_nokey),
 * with MMWLAN_SUCCESS already returned: bat_port would count BAT_TX_OK, not NOPEER. */
static void t_tx_unicast_ra_candidate(void)
{
    printf("--- TX: under SAE, ra = a peer AMPE has not keyed is MMWLAN_NOT_FOUND ---\n");
    static const uint8_t K_A[16] = { 0xb0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    static const uint8_t K_C[16] = { 0xc0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    static const uint8_t UC[30] = { 0x40, 0x0f, 0x32, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x0c };
    simnode_del_peer(NULL);
    CHECK(simnode_start_sae(W), "an SAE node");
    simnode_set_gates(/*fwd=*/false, /*bridge=*/false, /*grp_std=*/false, /*secure=*/true);
    simnode_set_batman(true);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A, 0, true);
    (void)simnode_add_peer(C);                       /* sta_add'ed; AMPE not done */
    simnode_outbox_clear();

    const unsigned live = simnode_live_allocs();
    int st = simnode_host_tx_eth(C, C, W, 0x4305, UC, sizeof(UC));
    CHECK(st == MMWLAN_NOT_FOUND, "ra = the candidate C: MMWLAN_NOT_FOUND (%d)", st);
    CHECK(simnode_outbox_count() == 0 && simnode_live_allocs() == live,
          "nothing on air, and the packet was consumed (%u frames, %u -> %u live)",
          simnode_outbox_count(), live, simnode_live_allocs());

    (void)simnode_set_key(C, K_C, 0, true);
    st = simnode_host_tx_eth(C, C, W, 0x4305, UC, sizeof(UC));
    const struct simnode_frame *f = frame_to(C);
    CHECK(st == MMWLAN_SUCCESS && simnode_outbox_count() == 1 && f != NULL &&
              (f->tx_flags & MMDRV_TX_FLAG_HW_ENC) != 0,
          "once AMPE keys C: MMWLAN_SUCCESS and one keyed frame to C (%d, %u)", st,
          simnode_outbox_count());
    simnode_del_peer(NULL);
}

/* The engine task resolves the peer and walks the replica copies on its own task
 * (mmwlan_tx_pkt), while the event loop's del_peer frees a peer. Here del_peer runs at
 * the first copy's allocation: after the lookup chose A, before A's own frame is queued. */
static void del_a_(void) { simnode_del_peer(A); }
static void arm_del_a_(void) { simnode_set_tx_alloc_hook(del_a_); } /* past the frame's own */

static void t_tx_del_peer_mid_walk(void)
{
    printf("--- TX: a peer deleted while a broadcast's copies are made gets nothing queued ---\n");
    fresh(W, /*batman=*/true, /*grp_std=*/false, /*ext=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    (void)simnode_add_peer(D);
    const unsigned live = simnode_live_allocs();
    simnode_set_tx_alloc_hook(arm_del_a_);
    int st = simnode_host_tx_eth(NULL, BC, W, 0x4305, ELP, sizeof(ELP));
    simnode_set_tx_alloc_hook(NULL);
    CHECK(st == MMWLAN_SUCCESS && simnode_outbox_count() == 2 && frame_to(A) == NULL &&
              frame_to(C) != NULL && frame_to(D) != NULL,
          "C and D get their copies, deleted A none (%d, %u frames)", st, simnode_outbox_count());
    CHECK(simnode_live_allocs() + 1u == live,
          "A's record is freed and its frame released, not queued to it (%u -> %u live)", live,
          simnode_live_allocs());
}

/* The engine's periodic ELP and a unicast to A, with the event loop's del_peer(A) run while
 * the engine task's lookup has A's record pointer loaded and has not read the record yet. */
static unsigned s_loop_ran;
static void loop_del_a_(void) { s_loop_ran++; simnode_del_peer(A); }

static void t_tx_del_peer_mid_lookup(void)
{
    printf("--- TX: a peer deleted while the engine task's lookup reads it is not read freed ---\n");
    fresh(W, /*batman=*/true, /*grp_std=*/false, /*ext=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    (void)simnode_add_peer(D);
    unsigned live = simnode_live_allocs();
    s_loop_ran = 0;
    simnode_set_peer_read_hook(loop_del_a_);
    int st = simnode_host_tx_eth(NULL, BC, W, 0x4305, ELP, sizeof(ELP));
    simnode_set_peer_read_hook(NULL);
    CHECK(s_loop_ran == 1 && st == MMWLAN_SUCCESS && simnode_outbox_count() == 2 &&
              frame_to(A) == NULL && frame_to(C) != NULL && frame_to(D) != NULL,
          "ELP: C and D get their replicas, A deleted mid-lookup none (%d, %u frames)", st,
          simnode_outbox_count());
    CHECK(simnode_live_allocs() + 1u == live, "A's record is freed once the TX is done (%u -> %u live)",
          live, simnode_live_allocs());

    (void)simnode_add_peer(A);
    simnode_outbox_clear();
    live = simnode_live_allocs();
    s_loop_ran = 0;
    simnode_set_peer_read_hook(loop_del_a_);
    st = simnode_host_tx_eth(A, A, W, 0x4305, ELP, sizeof(ELP));
    simnode_set_peer_read_hook(NULL);
    CHECK(s_loop_ran == 1 && st == MMWLAN_NOT_FOUND && simnode_outbox_count() == 0,
          "ra = A, deleted mid-lookup: MMWLAN_NOT_FOUND, bat_port's BAT_TX_NOPEER (%d, %u frames)", st,
          simnode_outbox_count());
    CHECK(simnode_live_allocs() + 1u == live, "its record freed and the packet consumed (%u -> %u live)",
          live, simnode_live_allocs());
}

/* ---- 5-8. what the receive hook is handed -------------------------------- */

static void t_rx_linux_group(void)
{
    printf("--- RX: a Linux peer's batman broadcast (3-address group frame, no AE) ---\n");
    fresh(W, /*batman=*/true, /*grp_std=*/false, /*ext=*/true);
    (void)simnode_add_peer(A);

    uint8_t frame[256];
    struct umac_mesh_ctrl mc = { .flags = 0, .ttl = 31, .seq = 700 };
    uint16_t n = mk_grp(frame, BC, A, A, &mc, ELP, sizeof(ELP));
    CHECK(simnode_rx(frame, n, -55), "A's ELP arrives as a group frame");
    CHECK(simnode_ext_rx_count() == 1 && simnode_host_rx_count() == 0,
          "it goes to the extended callback, never to the raw one (%u / %u)",
          simnode_ext_rx_count(), simnode_host_rx_count());
    const struct simnode_extrx *e = simnode_ext_rx_get(0);
    if (e == NULL) { return; }
    CHECK(MAC_EQ(&e->frame[0], BC), "Ethernet dst ff:ff:ff:ff:ff:ff");
    CHECK(MAC_EQ(&e->frame[6], A) && e->have_ta && MAC_EQ(e->ta, A), "Ethernet src = A = the TA");
    CHECK(ethertype_of(e) == 0x4305 && e->len == 14u + sizeof(ELP) &&
              memcmp(&e->frame[14], ELP, sizeof(ELP)) == 0,
          "ethertype 0x4305 and the ELP bytes (%u bytes)", e->len);
    CHECK(bat_mode_rx_classify(e->frame, e->len, e->ta) == BAT_MODE_RX_BATMAN,
          "the hook's classifier keeps it for the engine");
}

/* umac_interface.c keeps an extended callback per VIF; mesh data is looked up on the AP one.
 * bat_port.c registers MMWLAN_VIF_UNSPECIFIED, which sets both. */
static void t_rx_ext_vif(void)
{
    printf("--- RX: mesh data reaches the extended callback registered for the AP VIF ---\n");
    uint8_t frame[256];
    struct umac_mesh_ctrl mc = { .flags = 0, .ttl = 31, .seq = 1300 };
    fresh(W, /*batman=*/true, /*grp_std=*/false, /*ext=*/false); /* clears both VIFs' */
    simnode_set_rx_ext_cb_vif(true, MMWLAN_VIF_STA);
    (void)simnode_add_peer(A);
    uint16_t n = mk_grp(frame, BC, A, A, &mc, ELP, sizeof(ELP));
    (void)simnode_rx(frame, n, -55);
    CHECK(simnode_ext_rx_count() == 0 && simnode_host_rx_count() == 0,
          "registered for the STA VIF only: not delivered (%u / %u)", simnode_ext_rx_count(),
          simnode_host_rx_count());

    simnode_set_rx_ext_cb(false);
    simnode_set_rx_ext_cb_vif(true, MMWLAN_VIF_AP);
    mc.seq++;
    n = mk_grp(frame, BC, A, A, &mc, ELP, sizeof(ELP));
    (void)simnode_rx(frame, n, -55);
    CHECK(simnode_ext_rx_count() == 1, "registered for the AP VIF: delivered (%u)",
          simnode_ext_rx_count());
    simnode_set_rx_ext_cb(false);
}

static void t_rx_warthog_replica(void)
{
    printf("--- RX: another warthog's AE-2 replica ---\n");
    fresh(W, /*batman=*/true, /*grp_std=*/false, /*ext=*/true);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);

    uint8_t frame[256];
    struct umac_mesh_ctrl mc = { .flags = UMAC_MESH_CTRL_AE_A5A6, .ttl = 31, .seq = 800 };
    memcpy(mc.eaddr1, BC, 6);
    memcpy(mc.eaddr2, A, 6);
    uint16_t n = mk_uni(frame, W, A, W, A, &mc, ELP, sizeof(ELP));
    CHECK(simnode_rx(frame, n, -55), "A's replica, addressed to us");
    const struct simnode_extrx *e = simnode_ext_rx_get(0);
    CHECK(e != NULL && MAC_EQ(&e->frame[0], BC) && MAC_EQ(&e->frame[6], A) && e->have_ta &&
              MAC_EQ(e->ta, A),
          "the hook sees Ethernet dst ff:ff, src A = TA");
    CHECK(e != NULL && bat_mode_rx_classify(e->frame, e->len, e->ta) == BAT_MODE_RX_BATMAN,
          "and keeps it");
    CHECK(simnode_outbox_count() == 0, "a leaf with batman on re-floods nothing at 802.11s (%u)",
          simnode_outbox_count());
    char paths[512] = { 0 };
    (void)simnode_render_paths(paths, sizeof(paths));
    CHECK(strstr(paths, "proxies=0") != NULL && strstr(paths, "behind=") == NULL,
          "no proxy entry: batman frames never teach leaf proxying");
}

static void t_rx_leaf_learning(void)
{
    printf("--- RX: leaf proxy learning is off in batman mode, on without it ---\n");
    uint8_t frame[256];
    struct umac_mesh_ctrl mc = { .flags = UMAC_MESH_CTRL_AE_A5A6, .ttl = 31, .seq = 900 };
    memcpy(mc.eaddr1, W, 6);
    memcpy(mc.eaddr2, H2, 6);
    char paths[512];

    fresh(W, /*batman=*/false, /*grp_std=*/false, /*ext=*/false);
    (void)simnode_add_peer(A);
    uint16_t n = mk_uni(frame, W, A, W, A, &mc, ELP, sizeof(ELP));
    (void)simnode_rx(frame, n, -55);
    memset(paths, 0, sizeof(paths));
    (void)simnode_render_paths(paths, sizeof(paths));
    CHECK(strstr(paths, "host=000088 behind=00000a") != NULL, "batman off: H2 learned behind A");

    fresh(W, /*batman=*/true, /*grp_std=*/false, /*ext=*/true);
    (void)simnode_add_peer(A);
    mc.seq = 901;
    n = mk_uni(frame, W, A, W, A, &mc, ELP, sizeof(ELP));
    (void)simnode_rx(frame, n, -55);
    memset(paths, 0, sizeof(paths));
    (void)simnode_render_paths(paths, sizeof(paths));
    CHECK(strstr(paths, "behind=") == NULL, "batman on: nothing learned");
    const struct simnode_extrx *e = simnode_ext_rx_get(0);
    CHECK(e != NULL && MAC_EQ(&e->frame[0], W) && MAC_EQ(&e->frame[6], H2) &&
              bat_mode_rx_classify(e->frame, e->len, e->ta) == BAT_MODE_RX_RELAYED,
          "the frame still reaches the hook (src H2 != TA A), which drops it as relayed");
}

static void t_rx_unicast(void)
{
    printf("--- RX: a 4-address unicast, and one another node relayed ---\n");
    fresh(W, /*batman=*/true, /*grp_std=*/false, /*ext=*/true);
    (void)simnode_add_peer(A);

    static const uint8_t UC[24] = { 0x40, 0x0f, 0x32, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
    uint8_t frame[256];
    struct umac_mesh_ctrl mc = { .flags = 0, .ttl = 31, .seq = 1000 };
    uint16_t n = mk_uni(frame, W, A, W, A, &mc, UC, sizeof(UC));
    (void)simnode_rx(frame, n, -55);
    const struct simnode_extrx *e = simnode_ext_rx_get(0);
    CHECK(e != NULL && MAC_EQ(&e->frame[0], W) && MAC_EQ(&e->frame[6], A) && MAC_EQ(e->ta, A) &&
              ethertype_of(e) == 0x4305 && memcmp(&e->frame[14], UC, sizeof(UC)) == 0,
          "DA = us, SA = A = TA, the batman bytes intact");
    CHECK(e != NULL && bat_mode_rx_classify(e->frame, e->len, e->ta) == BAT_MODE_RX_BATMAN, "kept");

    /* A Linux node with mesh_fwding=1 relaying E's frame to us: mesh SA E, TA A. */
    simnode_ext_rx_clear();
    mc.seq = 1001;
    n = mk_uni(frame, W, A, W, E, &mc, UC, sizeof(UC));
    (void)simnode_rx(frame, n, -55);
    e = simnode_ext_rx_get(0);
    CHECK(e != NULL && MAC_EQ(&e->frame[6], E) && MAC_EQ(e->ta, A) &&
              bat_mode_rx_classify(e->frame, e->len, e->ta) == BAT_MODE_RX_RELAYED,
          "SA E, TA A: dropped as relayed -- batman links are single-hop");
}

/* ---- 9. the peer-link snapshot ------------------------------------------- */

static void t_peer_links(void)
{
    printf("--- umac_datapath_mesh_peer_links: what the engine's link throughput reads ---\n");
    fresh(W, /*batman=*/true, /*grp_std=*/false, /*ext=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    simnode_set_peer_tput(A, 7200, true);
    simnode_set_peer_tput(C, 0, false);

    struct mmwlan_mesh_peer_link l[4];
    memset(l, 0xa5, sizeof(l));
    uint8_t n = simnode_peer_links(l, 4);
    CHECK(n == 2, "two peers listed (got %u)", n);
    const struct mmwlan_mesh_peer_link *la = NULL, *lc = NULL;
    for (uint8_t i = 0; i < n; i++)
    {
        if (MAC_EQ(l[i].addr, A)) { la = &l[i]; }
        if (MAC_EQ(l[i].addr, C)) { lc = &l[i]; }
    }
    CHECK(la != NULL && la->estab == 1 && la->rc_valid == 1 && la->expected_tput_kbps == 7200,
          "A: ESTAB, rate control 7200 kbit/s");
    CHECK(lc != NULL && lc->estab == 1 && lc->rc_valid == 0 && lc->expected_tput_kbps == 0,
          "C: ESTAB, no best rate yet");
    CHECK(la != NULL && bat_mode_kbps_to_units(true, la->estab, la->rc_valid, la->expected_tput_kbps) == 72,
          "  which the port maps to 72 units");
    CHECK(lc != NULL && bat_mode_kbps_to_units(true, lc->estab, lc->rc_valid, lc->expected_tput_kbps) ==
              BAT_TPUT_UNKNOWN,
          "  and to unknown (the engine samples 1 Mbit/s)");
    CHECK(simnode_peer_links(l, 1) == 1, "max is honoured");

    /* mmwlan_mesh_query_peer_links: a query that never ran is an error, not an empty table. */
    uint8_t cnt = 0x77;
    CHECK(simnode_peer_links_query(l, 4, &cnt, /*on_loop=*/true) == MMWLAN_SUCCESS && cnt == 2,
          "query on the event loop: SUCCESS, two peers (%u)", cnt);
    const unsigned filled = simnode_evt_fill();
    memset(l, 0xa5, sizeof(l));
    cnt = 0x77;
    int st = simnode_peer_links_query(l, 4, &cnt, /*on_loop=*/false);
    simnode_evt_discard();
    CHECK(filled > 0 && st == MMWLAN_NOT_RUNNING && cnt == 0,
          "from the port's task with the event pool full: NOT_RUNNING, count 0 (%d, %u)", st, cnt);
    CHECK(l[0].addr[0] == 0xa5 && l[0].estab == 0xa5 && l[0].expected_tput_kbps == 0xa5a5a5a5u,
          "and the caller's last snapshot is untouched");
    CHECK(simnode_peer_links_query(NULL, 4, &cnt, true) == MMWLAN_INVALID_ARGUMENT &&
              simnode_peer_links_query(l, 4, NULL, true) == MMWLAN_INVALID_ARGUMENT,
          "no buffer or no count: MMWLAN_INVALID_ARGUMENT");
    cnt = 0x77;
    CHECK(simnode_peer_links_query(l, 0, &cnt, false) == MMWLAN_SUCCESS && cnt == 0 &&
              simnode_evt_pending() == 0,
          "max 0: nothing to take and nothing posted");

    simnode_del_peer(A);
    n = simnode_peer_links(l, 4);
    CHECK(n == 1 && MAC_EQ(l[0].addr, C), "a deleted peer is absent (%u listed)", n);

    /* Under SAE a slot exists before AMPE keys it. */
    static const uint8_t K_MTK[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    simnode_del_peer(NULL);
    CHECK(simnode_start_sae(W), "restart as an SAE node");
    simnode_set_gates(false, false, false, true);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    CHECK(simnode_set_key(A, K_MTK, 0, true) == 0, "A's MTK arrives; C is still a candidate");
    n = simnode_peer_links(l, 4);
    la = lc = NULL;
    for (uint8_t i = 0; i < n; i++)
    {
        if (MAC_EQ(l[i].addr, A)) { la = &l[i]; }
        if (MAC_EQ(l[i].addr, C)) { lc = &l[i]; }
    }
    CHECK(n == 2 && la != NULL && la->estab == 1 && lc != NULL && lc->estab == 0,
          "A estab 1, the unkeyed candidate C estab 0");
    CHECK(lc != NULL && bat_mode_kbps_to_units(true, lc->estab, lc->rc_valid, lc->expected_tput_kbps) == 0,
          "and C reads as no station");
    simnode_del_peer(NULL);
}

/* ---- 10. two engines over two real stacks ---------------------------------- */

struct eng {
    const uint8_t *hard;
    uint8_t soft[6];
    struct bat *b;
    uint8_t tx[16][600];
    size_t txlen[16];
    unsigned ntx;
    uint8_t rx[600];
    size_t rxlen;
    unsigned nrx;
    uint32_t rng;
};
static uint32_t g_now;
static struct eng g_w, g_a;

static int e_tx(void *user, const uint8_t *frame, size_t len)
{
    struct eng *e = user;
    if (e->ntx < 16 && len <= sizeof(e->tx[0]))
    {
        memcpy(e->tx[e->ntx], frame, len);
        e->txlen[e->ntx++] = len;
    }
    return BAT_TX_OK;
}
static void e_deliver(void *user, const uint8_t *frame, size_t len)
{
    struct eng *e = user;
    e->nrx++;
    e->rxlen = len < sizeof(e->rx) ? len : sizeof(e->rx);
    memcpy(e->rx, frame, e->rxlen);
}
static uint32_t e_now(void *user) { (void)user; return g_now; }
static uint32_t e_rand(void *user)
{
    struct eng *e = user;
    e->rng ^= e->rng << 13;
    e->rng ^= e->rng >> 17;
    e->rng ^= e->rng << 5;
    return e->rng;
}
static uint32_t e_tput(void *user, const uint8_t hard[6]) { (void)user; (void)hard; return 100; }

static bool eng_init(struct eng *e, const uint8_t *hard, uint8_t soft_last, uint32_t seed)
{
    static uint64_t mem_w[(1 << 16) / 8], mem_a[(1 << 16) / 8]; /* aligned for struct bat */
    memset(e, 0, sizeof(*e));
    e->hard = hard;
    e->rng = seed;
    memcpy(e->soft, hard, 6);
    e->soft[0] = 0x06;
    e->soft[5] = soft_last;
    if (bat_ctx_size() > sizeof(mem_w)) { return false; }
    e->b = (struct bat *)(void *)(e == &g_w ? mem_w : mem_a);
    struct bat_config cfg;
    bat_config_defaults(&cfg);
    memcpy(cfg.hard_addr, hard, 6);
    memcpy(cfg.soft_addr, e->soft, 6);
    const struct bat_ops ops = { .tx = e_tx, .deliver = e_deliver, .now_ms = e_now,
                                 .rand32 = e_rand, .link_tput = e_tput };
    return bat_init(e->b, &cfg, &ops, e) == 0;
}

static void eng_run(struct eng *e, uint32_t until)
{
    while (g_now < until)
    {
        g_now += 10;
        (void)bat_tick(e->b);
    }
}

static int find_tx(const struct eng *e, uint8_t type)
{
    for (unsigned i = 0; i < e->ntx; i++)
    {
        if (e->txlen[i] > 14 && e->tx[i][14] == type) { return (int)i; }
    }
    return -1;
}

/* s's link frame k: s's datapath -> air bytes -> d's datapath -> hook -> d's engine. */
static bool carry(struct eng *s, int k, struct eng *d)
{
    if (k < 0) { return false; }
    const uint8_t *f = s->tx[k];
    const size_t len = s->txlen[k];
    fresh(s->hard, /*batman=*/true, /*grp_std=*/false, /*ext=*/false);
    (void)simnode_add_peer(d->hard);
    int st = simnode_host_tx_eth((f[0] & 0x01) ? NULL : f, f, f + 6, 0x4305, f + 14, (uint16_t)(len - 14));
    const struct simnode_frame *af = frame_to(d->hard);
    if (st != MMWLAN_SUCCESS || af == NULL) { return false; }
    uint8_t air[512];
    uint16_t n = af->len;
    memcpy(air, af->bytes, n);

    fresh(d->hard, /*batman=*/true, /*grp_std=*/false, /*ext=*/true);
    (void)simnode_add_peer(s->hard);
    (void)simnode_rx(air, n, -50);
    const struct simnode_extrx *e = simnode_ext_rx_get(0);
    if (e == NULL || !e->have_ta || e->len != len ||
        bat_mode_rx_classify(e->frame, e->len, e->ta) != BAT_MODE_RX_BATMAN)
    {
        return false;
    }
    uint8_t copy[600];
    memcpy(copy, e->frame, e->len);
    bat_rx_hard(d->b, copy, e->len);
    (void)bat_tick(d->b);
    return true;
}

static void t_gate_matters(void)
{
    printf("--- why the gate exists: an ELP in the leaf replica shape makes no neighbour ---\n");
    g_now = 1000;
    CHECK(eng_init(&g_w, W, 0x01, 0x1111u) && eng_init(&g_a, A, 0x0a, 0x2222u), "engines W and A");
    eng_run(&g_w, 1600);
    int k = find_tx(&g_w, 0x03);
    CHECK(k >= 0, "W emitted an ELP");
    if (k < 0) { return; }
    fresh(W, /*batman=*/false, /*grp_std=*/false, /*ext=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_host_tx_eth(NULL, g_w.tx[k], W, 0x4305, g_w.tx[k] + 14, (uint16_t)(g_w.txlen[k] - 14));
    const struct simnode_frame *af = frame_to(A);
    uint8_t air[512];
    uint16_t n = af != NULL ? af->len : 0;
    if (af != NULL) { memcpy(air, af->bytes, n); }
    fresh(A, /*batman=*/true, /*grp_std=*/false, /*ext=*/true);
    (void)simnode_add_peer(W);
    (void)simnode_rx(air, n, -50);
    const struct simnode_extrx *e = simnode_ext_rx_get(0);
    CHECK(e != NULL && MAC_EQ(&e->frame[0], A),
          "with the gate off, A's stack rebuilds Ethernet dst = A, not ff:ff");
    CHECK(e != NULL && bat_mode_rx_classify(e->frame, e->len, e->ta) == BAT_MODE_RX_PROBE,
          "so A's hook takes it for a unicast ELP probe and drops it");
    if (e == NULL) { return; }
    uint8_t copy[600];
    memcpy(copy, e->frame, e->len);
    bat_rx_hard(g_a.b, copy, e->len);
    CHECK(bat_neigh_count(g_a.b) == 0 && bat_counter(g_a.b, BAT_C_ELP_PROBE) == 1,
          "and A's engine would too: no neighbour, elp_probe 1 (batman-adv behaves alike)");
}

static void t_two_engines(void)
{
    printf("--- two engines, two real stacks: ELP both ways, an OGM, a unicast soft frame ---\n");
    g_now = 1000;
    CHECK(eng_init(&g_w, W, 0x01, 0x1234567u) && eng_init(&g_a, A, 0x0a, 0x7654321u),
          "engines W and A initialised");

    /* Each carry ends with the receiver's bat_tick, as the port task does after every
     * event, so a receiver's captures are cleared before the frame reaches it. */
    eng_run(&g_w, 1600);
    g_a.ntx = 0;
    CHECK(carry(&g_w, find_tx(&g_w, 0x03), &g_a), "W's ELP crossed W's and A's datapaths");
    CHECK(bat_neigh_count(g_a.b) == 1, "A has W as a neighbour (%u)", bat_neigh_count(g_a.b));

    eng_run(&g_a, 1700);
    g_w.ntx = 0;
    CHECK(carry(&g_a, find_tx(&g_a, 0x03), &g_w), "A's ELP crossed back");
    CHECK(bat_neigh_count(g_w.b) == 1, "W has A as a neighbour (%u)", bat_neigh_count(g_w.b));

    eng_run(&g_w, 3200);
    int ogm = find_tx(&g_w, 0x04);
    CHECK(ogm >= 0 && g_w.tx[ogm][0] == 0xff, "W sent an OGM2 to ff:ff (frame %d)", ogm);
    CHECK(carry(&g_w, ogm, &g_a), "W's OGM2 crossed as an AE-2 replica");
    CHECK(bat_route_count(g_a.b) == 1, "A now routes to W (%u routes)", bat_route_count(g_a.b));

    uint8_t soft[60] = { 0 };
    memcpy(soft, g_w.soft, 6);
    memcpy(soft + 6, g_a.soft, 6);
    soft[12] = 0x08;
    soft[13] = 0x00;
    for (unsigned i = 14; i < sizeof(soft); i++) { soft[i] = (uint8_t)i; }
    g_a.ntx = 0;
    CHECK(bat_tx_soft(g_a.b, soft, sizeof(soft)) == 0, "A's soft interface sends to W's soft MAC");
    int uc = find_tx(&g_a, 0x40);
    CHECK(uc >= 0 && MAC_EQ(g_a.tx[uc], W), "as a batman UNICAST to W's hard address (frame %d)", uc);
    CHECK(carry(&g_a, uc, &g_w), "it crossed as a 4-address unicast with ra = W");
    CHECK(g_w.nrx == 1 && g_w.rxlen == sizeof(soft) && memcmp(g_w.rx, soft, sizeof(soft)) == 0,
          "W delivered the soft frame once, byte for byte (%u delivered, %u bytes)", g_w.nrx,
          (unsigned)g_w.rxlen);
}

int main(void)
{
    printf("=== simnode batman: BATMAN_V frames through the real 802.11s datapath ===\n");
    t_tx_replica_ae2();
    t_tx_std_group();
    t_tx_batman_off();
    t_tx_unicast_ra();
    t_tx_unicast_ra_candidate();
    t_tx_del_peer_mid_walk();
    t_tx_del_peer_mid_lookup();
    t_tx_max_size();
    t_rx_linux_group();
    t_rx_ext_vif();
    t_rx_warthog_replica();
    t_rx_leaf_learning();
    t_rx_unicast();
    t_peer_links();
    t_gate_matters();
    t_two_engines();

    simnode_del_peer(NULL);
    simnode_set_rx_ext_cb(false);
    CHECK(simnode_live_allocs() == 0, "no packet buffer orphaned across the run (%u live)",
          simnode_live_allocs());
    CHECK(simnode_frees_in_critical() == 0, "and none freed inside a critical section (%u)",
          simnode_frees_in_critical());
    simnode_stop();
    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_simnode_batman: all passed\n");
    return 0;
}
