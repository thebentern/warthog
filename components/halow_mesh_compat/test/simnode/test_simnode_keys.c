/*
 * The SAE key lifecycle through the real datapath: our own group key, which
 * key a broadcast goes out under, and what a surviving link keeps when another
 * peer is removed.
 *
 * hostap is not linked, so keys arrive the way its set_key driver op delivers
 * them (simnode_set_key), in its order: our own TX MGTK first, at mesh start and
 * before any peer exists; then each peer's MTK and MGTK as its AMPE completes.
 * The fake chip records every INSTALL_KEY, which is what these assertions read.
 *
 * Each case was run against the tree before its fix and failed:
 *  (1) own MGTK before any peer was logged "deferring" and never installed;
 *  (2) a broadcast took the PEER's group key id from that peer's keychain;
 *  (3) removing one peer re-installed the published phase-1 constant over
 *      every survivor's AMPE key, in the chip and in the host keychain;
 *  (5) an OPEN mesh also had that constant pushed into the chip.
 * (4) pins the constant-key mesh's measured behaviour, which must not change.
 * (6) host CCMP RX took a unicast keyed with a peer's MGTK -- a key every
 *     member of the mesh holds -- and decrypted it;
 * (7) AT+REKEY pushed the published constant over an AMPE-keyed link, and onto
 *     an open one;
 * (8) with an unkeyed SAE candidate in the first slot, a broadcast went out in
 *     the clear (and a plaintext replica went to the candidate);
 * (9) a group frame the chip decrypted -- under OUR MGTK, the only group key it
 *     holds -- was delivered as the TA's and booked against the TA's replay counter.
 *     Built with WARTHOG_MESH_CHIP_VIF_MESH (chip keys), where the chip also holds A's
 *     MGTK at A's AID: one sealed under our MGTK in A's name is not opened (rxdrop 4);
 *     the rest of that rule is test_simnode_peergtk's;
 * (10) a survivor's restored MTK restarted the chip's shared PN counter below
 *     frames already sent, so the survivor dropped ours as replays;
 * (11, built with WARTHOG_MESH_AMPE_NO_CHIP_KEY) the restore pushed a key into
 *     the chip on the builds that exist to keep it out, and AT+REKEYSTAT? counted it;
 * (12) cleartext from an SAE candidate AMPE had not keyed was delivered, relayed
 *     under our own MGTK, and could re-point a host learned behind a keyed peer.
 *
 * MGTK replay counters. AMPE carries each node's MGTK with a Key RSC; a receiver
 * installs it as the replay floor and, like mac80211, accepts only PN > RSC.
 * Before these, every warthog advertised RSC 0 (the mesh get_seqnum was the AP
 * one, zeros) and installed its MGTK at chip TX PN 0, and dropped every peer's
 * RSC (set_key's seq never reached the datapath). Built with
 * WARTHOG_MESH_MGTK_PN_BASE (test_simnode_keys, _nochip):
 * (13) our RSC is one below the install PN; once group frames went out the next
 *     Open re-installs our MGTK (same VIF) at a fresh base, advertising one below --
 *     one frame is enough, a retried Open with nothing sent re-installs nothing,
 *     a key id that is not ours answers 0, and the re-install is counted as
 *     mgtk_reinst, not as ampe_mgtk;
 * (14) an RSC asked while our MGTK is out of the chip (a mesh restart re-delivering
 *     it) lies above every PN it used, installs nothing, and its install lies above
 *     that RSC; a NEW key's install lies above every earlier one;
 * (15) with AT+MESHGRP=0 nothing goes out under our MGTK, so nothing re-installs;
 *     (15b, built with host CCMP) also with it armed: a host-sealed unicast is not
 *     booked as a frame under our MGTK;
 * (16) a failed re-install advertises the OLD base - 1 (below every PN the chip
 *     will use), is counted as mgtk_rsc_fail, and the next Open retries; after a
 *     failed FIRST install no Open installs it, the next peer does;
 * (17) a relayed group frame goes out under our MGTK and is counted like our own;
 * (18, chip-key build) one PN allocator: a group re-install lies above an earlier
 *     MTK, and every later MTK install and survivor restore lies above every PN
 *     our MGTK can have used, even past an epoch's 2^20.
 * (23) so does the group re-install itself, after more frames than an epoch holds;
 * (24) past PN 2^32 our RSC still carries all six octets;
 * (25) a re-install goes to the VIF our MGTK was installed on, here a nonzero one;
 * (26) group frames still queued in the chip when an Open re-installs our MGTK go
 *     out from the new base; they went uncounted, so the next Open with nothing sent
 *     since advertised the new base - 1, below their PNs. Once their TX status is
 *     back, an Open re-installs nothing;
 * (27) a unicast's TX status does not release our group frames from that count;
 * (28) a group frame the chip hands back unsent (attempts 0) is released from it,
 *     and a stray release does not wrap it. Only an over-count is at stake, so this
 *     one passes before the change too.
 * Built without it (test_simnode_keys_pnbase_off and _nochip_pnbase_off, as
 * warthog-mesh-sae and -nochipkey ship):
 * (19) our MGTK installs at PN 0, the RSC is 0 and nothing re-installs -- the
 *     unchanged default, pinned; it passes on the tree before this change too.
 * Every build, with hostap's real key id 1 for a peer's MGTK:
 * (20) the peer's RSC (little-endian, all six octets) is the replay floor;
 * (21) the same MGTK installed again on a live link keeps its counter, with RSC 0
 *     or a nonzero RSC below it; an RSC above that counter still wins, and a NEW
 *     MGTK starts from its own RSC;
 * (22) a re-peered link starts from the RSC the peer advertises, not the old
 *     link's counter -- mac80211 parity, so a peer whose PN restarted under an
 *     unchanged MGTK is not locked out.
 * Every build:
 * (31) under SAE a unicast data frame the chip decrypted under a key id other than
 *     the link's MTK (a member holding our MGTK sealing it in a peer's name) is
 *     dropped, reason 96, and moves no replay floor; on no-chip-key builds, where
 *     the chip holds no MTK, every chip-decrypted unicast is. A keyed non-SAE mesh
 *     still takes one under its shared group key id (pin).
 * (33) a peer's QoS data on different TIDs was judged against ONE replay counter per
 *     key, so TID 0 PN 9 after TID 5 PN 10 was dropped as a replay (reason 5); each TID
 *     now has its own, and a lower PN on the same TID is still refused. Unicast and group,
 *     chip-decrypted on every build, host CCMP where it is compiled in.
 * Built with host CCMP (as warthog-mesh-sae-swccmp, the only SAE build batman runs on):
 * (34) batman mode's frames against a wizard node: a peer's group ELP under its MGTK is
 *     opened by host CCMP and reaches the batman hook with the TA as its source, a replay
 *     and a forgery do not, nor anything with host CCMP disarmed (reason 4); our ELP goes
 *     out as a host-sealed AE-2 replica to each keyed peer (none to a candidate), or with
 *     AT+MESHGRP=1 as one group frame the chip encrypts under our MGTK.
 * (35) a keyed peer's unicast to another station (RA not us; measured on air, where the
 *     MM6108 hands such frames up) is dropped by the receive filter as not_ours, before
 *     host CCMP: tried, micfail and the fail snapshot do not move, even when the frame is
 *     one host CCMP could open. Unicast to us still opens, relayed traffic (RA us) still
 *     goes on, group frames are not judged, and a real MIC failure still snapshots. A
 *     Protected unicast management frame to another station is only counted (mgmt_nours):
 *     whether the chip hands those up is not measured, so host CCMP still tries it.
 * Chip-key builds:
 * (30) under SAE a group frame -- replicated, standard, or one we relay -- is queued
 *     to no slot AMPE has not keyed: TX would only drop it there, and a failed SAE
 *     frees such a slot while the netif task may be walking the table (this narrows
 *     that race; it does not close it). An open mesh still copies to every peer.
 * (32) a relay holding a frame for T neither releases it nor forwards the next one
 *     into T's slot while AMPE has not keyed it (TX would drop both, counted as
 *     sent); once T keys, the next tick sends both, keyed.
 */
#include <stdio.h>
#include <string.h>

#include "simnode.h"
#include "mmdrv.h"
#include "mmwlan.h"
#include "umac/datapath/umac_datapath.h"
#include "umac/datapath/umac_datapath_private.h"
#include "umac/mesh/umac_mesh_fwd.h"
#include "umac/mesh/umac_mesh_fwd_glue.h"
#include "umac/keys/umac_keys.h"
#include "umac/data/umac_data.h"
#include "umac/mesh/umac_mesh_ccmp_hdr.h"
#include "umac/mesh/umac_mesh_ccm.h"
#include "umac/mesh/umac_mesh_ctrl.h"
#include "umac/mesh/umac_mesh_ies.h"
#include "mmpkt.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static const uint8_t W[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 }; /* us */
static const uint8_t A[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0a };
static const uint8_t C[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0c };
static const uint8_t BC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static const uint8_t PAY[8] = { 0xc0, 0xff, 0xee, 0x11, 0x22, 0x33, 0x44, 0x55 };

/* Distinct, recognisable key material, and distinct key ids for our group key
 * and a peer's, so a test can tell which one was used. */
static const uint8_t K_OWN_MGTK[16] = { 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
                                        0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf };
static const uint8_t K_A_MTK[16]    = { 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
                                        0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf };
static const uint8_t K_A_MGTK[16]   = { 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
                                        0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf };
static const uint8_t K_C_MTK[16]    = { 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
                                        0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf };
#define OWN_MGTK_ID  1u
#define PEER_MGTK_ID 2u

/* The phase-1 key published in umac_datapath_mesh.c. Must never reach a link
 * that AMPE keyed. */
static const uint8_t P1_MTK[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };

#define MAC_EQ(p, m) (memcmp((p), (m), 6) == 0)

static void fresh(bool sae, bool grp_std, bool secure)
{
    simnode_del_peer(NULL);
    (void)(sae ? simnode_start_sae(W) : simnode_start(W));
    simnode_set_gates(/*fwd=*/false, /*bridge=*/false, grp_std, secure);
    simnode_outbox_clear();
    simnode_host_rx_clear();
    simnode_keyinst_clear();
}

static const struct simnode_keyinst *find_install(const uint8_t key[16], bool pairwise)
{
    for (unsigned i = 0; i < simnode_keyinst_count(); i++)
    {
        const struct simnode_keyinst *k = simnode_keyinst_get(i);
        if (k != NULL && k->pairwise == pairwise && memcmp(k->key, key, 16) == 0) { return k; }
    }
    return NULL;
}

static const struct simnode_frame *frame_to(const uint8_t *addr1)
{
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (f != NULL && f->len >= 10u && MAC_EQ(&f->bytes[4], addr1)) { return f; }
    }
    return NULL;
}

/* ---- 1. our own group key, delivered before any peer exists -------------- */

static void t_own_mgtk_before_first_peer(void)
{
    printf("--- our own MGTK, delivered before any peer, reaches the chip at the first peer ---\n");
    fresh(/*sae=*/true, /*grp_std=*/false, /*secure=*/true);

    (void)simnode_set_key(BC, K_OWN_MGTK, OWN_MGTK_ID, /*pairwise=*/false);
    CHECK(find_install(K_OWN_MGTK, false) == NULL,
          "nothing is installed yet: there is no peer, so no station to hang it on");

    CHECK(simnode_add_peer(A), "the first peer arrives");
    const struct simnode_keyinst *k = find_install(K_OWN_MGTK, false);
    CHECK(k != NULL, "our own MGTK is now in the chip (%u installs recorded)",
          simnode_keyinst_count());
    if (k != NULL)
    {
        CHECK(k->aid == 0u, "in the VIF-wide group slot, aid 0 (got %u)", (unsigned)k->aid);
        CHECK(k->key_idx == OWN_MGTK_ID, "under our key id %u (got %u)", OWN_MGTK_ID,
              (unsigned)k->key_idx);
    }

    /* A second peer must not install it again. */
    simnode_keyinst_clear();
    (void)simnode_add_peer(C);
    CHECK(find_install(K_OWN_MGTK, false) == NULL, "and a second peer does not re-install it");
}

/* ---- 2. which key a broadcast goes out under ------------------------------ */

static void t_group_tx_uses_own_mgtk(void)
{
    printf("--- under SAE, a standard group frame is keyed with OUR MGTK, not a peer's ---\n");
    fresh(/*sae=*/true, /*grp_std=*/true, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_MGTK, OWN_MGTK_ID, false);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, /*pairwise=*/true);
    (void)simnode_set_key(A, K_A_MGTK, PEER_MGTK_ID, /*pairwise=*/false);
    simnode_outbox_clear();

    CHECK(simnode_host_tx(BC, W, PAY, sizeof(PAY)), "host sends a broadcast");
    const struct simnode_frame *f = frame_to(BC);
    CHECK(f != NULL, "it goes out as a standard group frame (%u frames on air)",
          simnode_outbox_count());
    if (f == NULL) { return; }
    CHECK((f->bytes[1] & 0x40u) != 0u, "Protected");
    CHECK((f->tx_flags & MMDRV_TX_FLAG_HW_ENC) != 0u, "the chip is told to encrypt it");
    CHECK(f->key_idx == OWN_MGTK_ID,
          "with our own group key id %u -- the peer's is %u, and a receiver decrypts our "
          "broadcasts with OUR MGTK (got %u)",
          OWN_MGTK_ID, PEER_MGTK_ID, (unsigned)f->key_idx);
}

/* ---- 3. a survivor keeps its own AMPE key --------------------------------- */

static void t_survivor_keeps_ampe_key(void)
{
    printf("--- under SAE, removing one peer leaves every survivor keyed with ITS OWN MTK ---\n");
    fresh(/*sae=*/true, /*grp_std=*/false, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_MGTK, OWN_MGTK_ID, false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    (void)simnode_set_key(C, K_C_MTK, 0, true);
    simnode_keyinst_clear();

    simnode_del_peer(A);

    struct umac_sta_data *c = umac_datapath_mesh_find_peer(C);
    CHECK(c != NULL, "C is still a peer");
    if (c == NULL) { return; }
    const uint8_t *kc = umac_keys_get_key_data(c, 0);
    CHECK(kc != NULL && memcmp(kc, K_C_MTK, 16) == 0,
          "C's host keychain still holds C's AMPE MTK, not the published constant");
    CHECK(find_install(P1_MTK, true) == NULL,
          "the published phase-1 constant is never pushed into the chip");
    CHECK(find_install(K_C_MTK, true) != NULL,
          "C's own MTK is re-pushed to the chip (removing a station takes the slot with it)");
    CHECK(find_install(K_OWN_MGTK, false) == NULL && find_install(K_A_MGTK, false) == NULL,
          "and no group key is touched: the chip's group slot keeps our own TX MGTK");
    CHECK(umac_sta_data_get_security_type(c) == MMWLAN_SAE, "C's link is still keyed");
}

/* ---- 4. the constant-key mesh keeps its measured behaviour ---------------- */

static void t_constant_mesh_unchanged(void)
{
    printf("--- a constant-key (non-SAE) mesh still re-installs the shared key for survivors ---\n");
    fresh(/*sae=*/false, /*grp_std=*/false, /*secure=*/true);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    simnode_keyinst_clear();

    simnode_del_peer(A);
    CHECK(find_install(P1_MTK, true) != NULL,
          "the survivor gets the shared constant back -- measured necessary on that mesh");
}

/* ---- 5. an open mesh installs nothing ------------------------------------- */

static void t_open_mesh_installs_nothing(void)
{
    printf("--- an OPEN mesh installs no key when a peer goes ---\n");
    fresh(/*sae=*/false, /*grp_std=*/false, /*secure=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    simnode_keyinst_clear();

    simnode_del_peer(A);
    CHECK(simnode_keyinst_count() == 0u, "no INSTALL_KEY at all (got %u)",
          simnode_keyinst_count());
}

/* ---- 6. host CCMP RX will not take a unicast under a group key ----------- */

extern volatile uint32_t g_warthog_host_ccmp_on;
extern volatile uint32_t g_warthog_swccmp_grpkey;
extern volatile uint32_t g_warthog_swccmp_micfail;
bool umac_mesh_rx_host_ccmp(struct umac_sta_data *stad, const struct dot11_hdr *header,
                            struct mmpktview *rxbufview);

/* Offers A's keychain a Protected mesh data frame to addr1 under key_id, with a
 * body that cannot authenticate. Returns which counter moved: 'g' grpkey (refused
 * before decrypting), 'm' micfail (it got as far as decrypting), '?' neither. */
static char offer(struct umac_sta_data *a, const uint8_t addr1[6], uint8_t key_id)
{
    uint8_t hdr[32] = { 0x88, 0x43 };             /* QoS data, ToDS|FromDS, Protected */
    memcpy(&hdr[4], addr1, 6);
    memcpy(&hdr[10], A, 6);
    memcpy(&hdr[16], addr1, 6);
    memcpy(&hdr[24], A, 6);

    uint8_t body[UMAC_CCMP_HDR_LEN + 16u + 8u] = { 0 };
    const uint8_t pn[6] = { 1, 0, 0, 0, 0, 0 };
    umac_ccmp_write_header(body, pn, key_id);

    uint8_t buf[256];
    struct mmpkt *pkt = mmpkt_init_buf(buf, sizeof(buf), 0, sizeof(body), 0, NULL);
    struct mmpktview *v = mmpkt_open(pkt);
    mmpkt_append_data(v, body, sizeof(body));

    uint32_t g0 = g_warthog_swccmp_grpkey, m0 = g_warthog_swccmp_micfail;
    bool ok = umac_mesh_rx_host_ccmp(a, (const struct dot11_hdr *)hdr, v);
    mmpkt_close(&v);
    if (ok) { return '!'; }
    if (g_warthog_swccmp_grpkey != g0) { return 'g'; }
    if (g_warthog_swccmp_micfail != m0) { return 'm'; }
    return '?';
}

static void t_rx_unicast_under_group_key(void)
{
    printf("--- host CCMP RX refuses a unicast keyed with a group key ---\n");
    fresh(/*sae=*/true, /*grp_std=*/true, /*secure=*/true);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, /*pairwise=*/true);
    (void)simnode_set_key(A, K_A_MGTK, PEER_MGTK_ID, /*pairwise=*/false);
    struct umac_sta_data *a = umac_datapath_mesh_find_peer(A);
    CHECK(a != NULL && umac_keys_get_key_data(a, PEER_MGTK_ID) != NULL,
          "A's MGTK is in A's host keychain under id %u", PEER_MGTK_ID);
    if (a == NULL) { return; }

    uint32_t was = g_warthog_host_ccmp_on;
    g_warthog_host_ccmp_on = 1;
    char r = offer(a, W, PEER_MGTK_ID);
    CHECK(r == 'g', "unicast to us under A's MGTK: refused before decrypting (got '%c')", r);
    r = offer(a, W, 0);
    CHECK(r == 'm', "unicast to us under A's MTK: decrypted, MIC checked (got '%c')", r);
    r = offer(a, BC, PEER_MGTK_ID);
    CHECK(r == 'm', "broadcast under A's MGTK: decrypted, MIC checked (got '%c')", r);
    g_warthog_host_ccmp_on = was;
}

/* ---- 7. AT+REKEY re-pushes the link's own key --------------------------- */

extern volatile uint32_t g_warthog_rekey_req, g_warthog_rekey_aid, g_warthog_rekey_done;
void umac_datapath_mesh_service_rekey(void);

static void t_rekey_pushes_own_key(void)
{
    printf("--- AT+REKEY re-pushes the peer's own key: AMPE MTK, constant, or nothing ---\n");
    fresh(/*sae=*/true, /*grp_std=*/false, /*secure=*/true);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    (void)simnode_set_key(C, K_C_MTK, 0, true);
    simnode_keyinst_clear();
    g_warthog_rekey_req = 1u + 1u;                /* AT+REKEY=1: C's slot */
    uint32_t done0 = g_warthog_rekey_done;
    umac_datapath_mesh_service_rekey();
    CHECK(find_install(K_C_MTK, true) != NULL, "SAE: C's own AMPE MTK goes back in");
    CHECK(g_warthog_rekey_done == done0 + 1u && g_warthog_rekey_aid != 0xffffffffu,
          "SAE: REKEYSTAT counts it, with C's AID (%lu)", (unsigned long)g_warthog_rekey_aid);
    CHECK(find_install(P1_MTK, true) == NULL, "SAE: the published constant does not");
    struct umac_sta_data *c = umac_datapath_mesh_find_peer(C);
    const uint8_t *kc = c != NULL ? umac_keys_get_key_data(c, 0) : NULL;
    CHECK(kc != NULL && memcmp(kc, K_C_MTK, 16) == 0, "SAE: C's host keychain still holds C's MTK");

    fresh(/*sae=*/false, /*grp_std=*/false, /*secure=*/true);
    (void)simnode_add_peer(A);
    simnode_keyinst_clear();
    g_warthog_rekey_req = 0u + 1u;
    umac_datapath_mesh_service_rekey();
    CHECK(find_install(P1_MTK, true) != NULL, "keyed non-SAE mesh: the shared constant, as before");

    fresh(/*sae=*/false, /*grp_std=*/false, /*secure=*/false);
    (void)simnode_add_peer(A);
    simnode_keyinst_clear();
    g_warthog_rekey_req = 0u + 1u;
    done0 = g_warthog_rekey_done;
    umac_datapath_mesh_service_rekey();
    CHECK(simnode_keyinst_count() == 0u, "open mesh: no key at all (got %u)", simnode_keyinst_count());
    CHECK(g_warthog_rekey_done == done0 && g_warthog_rekey_aid == 0xffffffffu,
          "open mesh: REKEYSTAT reports nothing installed");
}

/* ---- 8. no data to an unkeyed SAE candidate in the clear ---------------- */

static const uint8_t X[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0e }; /* SAE not finished */

static bool any_plain_data(void)
{
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (f != NULL && !f->is_mgmt && (f->bytes[1] & 0x40u) == 0u) { return true; }
    }
    return false;
}

static void t_unkeyed_candidate_first(void)
{
    printf("--- SAE: an unkeyed candidate in the first slot gets nothing in the clear ---\n");
    fresh(/*sae=*/true, /*grp_std=*/true, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_MGTK, OWN_MGTK_ID, false);
    (void)simnode_add_peer(X);                     /* slot 0, never keyed */
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    (void)simnode_set_key(A, K_A_MGTK, PEER_MGTK_ID, false);
    simnode_outbox_clear();

    (void)simnode_host_tx(BC, W, PAY, sizeof(PAY));
    const struct simnode_frame *f = frame_to(BC);
    CHECK(f != NULL, "AT+MESHGRP=1: one standard group frame (%u on air)", simnode_outbox_count());
    if (f != NULL)
    {
        CHECK((f->bytes[1] & 0x40u) != 0u && (f->tx_flags & MMDRV_TX_FLAG_HW_ENC) != 0u &&
                  f->key_idx == OWN_MGTK_ID,
              "keyed with our own MGTK although X carries it (fc1=%02x key=%u)", f->bytes[1],
              (unsigned)f->key_idx);
    }

    simnode_set_gates(false, false, /*grp_std=*/false, true);
    simnode_outbox_clear();
    (void)simnode_host_tx(BC, W, PAY, sizeof(PAY));
    CHECK(frame_to(X) == NULL, "AT+MESHGRP=0: no replica to X");
    CHECK(frame_to(A) != NULL, "and A still gets its keyed replica");
    simnode_outbox_clear();
    (void)simnode_host_tx(X, W, PAY, sizeof(PAY));
    CHECK(simnode_outbox_count() == 0u, "a unicast to X is not sent (%u frames)",
          simnode_outbox_count());
    CHECK(!any_plain_data(), "no data frame left in the clear");
}

/* ---- 9. a group frame the chip decrypted is forged ------------------------ */

extern volatile uint32_t g_warthog_rxdrop_reason;

static uint16_t mk_group_decrypted(uint8_t *f, const uint8_t *ta, uint8_t key_id, uint8_t pn0)
{
    uint16_t n = umac_mesh_ies_build_data_hdr3_group(f, BC, ta, ta);
    f[1] |= 0x40u;                                            /* Protected */
    f[n++] = 0x00;                                            /* QoS: Mesh Control present */
    f[n++] = 0x01;
    const uint8_t pn[6] = { pn0, 0xff, 0xff, 0xff, 0xff, 0xff };
    umac_ccmp_write_header(&f[n], pn, key_id);
    n = (uint16_t)(n + UMAC_CCMP_HDR_LEN);
    struct umac_mesh_ctrl mc = { .flags = 0, .ttl = 31, .seq = 777 };
    n = (uint16_t)(n + umac_mesh_ctrl_build(&f[n], 18u, &mc));
    static const uint8_t snap[8] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00 };
    memcpy(&f[n], snap, sizeof(snap));
    n = (uint16_t)(n + sizeof(snap));
    memcpy(&f[n], PAY, sizeof(PAY));
    n = (uint16_t)(n + sizeof(PAY));
    memset(&f[n], 0xA5, 8);                                   /* MIC octets */
    return (uint16_t)(n + 8u);
}

static void t_decrypted_group_is_forged(void)
{
    printf("--- SAE: a group frame the chip decrypted (under OUR key) is dropped ---\n");
    fresh(/*sae=*/true, /*grp_std=*/true, /*secure=*/true);
    /* hostap's real ids: every MGTK, ours and each peer's, is key id 1. */
    (void)simnode_set_key(BC, K_OWN_MGTK, 1, false);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    (void)simnode_set_key(A, K_A_MGTK, 1, false);
    simnode_host_rx_clear();

    uint8_t frame[160];
    uint16_t n = mk_group_decrypted(frame, A, 1, 0xfe);
#if WARTHOG_MESH_CHIP_VIF_MESH
    /* On a MESH chip VIF the chip holds A's own MGTK at A's AID (test_simnode_peergtk), so key
     * id 1 opened there is A's. One sealed under OUR MGTK in A's name is what a member holding
     * it would send: the chip, trying A's key, does not open it. */
    {
        const uint8_t pn[6] = { 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff };
        uint8_t aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
        const uint32_t hl = umac_ccmp_hdr_len(frame);
        const uint32_t al = umac_ccmp_build_aad(frame, aad);
        umac_ccmp_build_nonce(frame, pn, nonce);
        (void)warthog_ccm_ae(K_OWN_MGTK, nonce, 8, aad, al, frame + hl + UMAC_CCMP_HDR_LEN,
                             (size_t)(n - hl - UMAC_CCMP_HDR_LEN - 8u), frame + n - 8u);
        (void)simnode_rx_air(frame, n, -60);
    }
    CHECK(simnode_host_rx_count() == 0u, "MESH VIF: one sealed under our MGTK is not delivered as "
          "A's (%u delivered)", simnode_host_rx_count());
    CHECK(g_warthog_rxdrop_reason == 4u, "MESH VIF: the chip does not open it, rxdrop 4 (got %lu)",
          (unsigned long)g_warthog_rxdrop_reason);
#else
    (void)simnode_rx_flags(frame, n, -60, MMDRV_RX_FLAG_DECRYPTED);
    CHECK(simnode_host_rx_count() == 0u, "not delivered as A's (%u delivered)",
          simnode_host_rx_count());
    CHECK(g_warthog_rxdrop_reason == 95u, "rxdrop reason 95 (got %lu)",
          (unsigned long)g_warthog_rxdrop_reason);
#endif
}

/* ---- 10. the chip's shared PN counter never goes backwards --------------- */

static uint64_t max_install_pn(void)
{
    uint64_t m = 0;
    for (unsigned i = 0; i < simnode_keyinst_count(); i++)
    {
        const struct simnode_keyinst *k = simnode_keyinst_get(i);
        if (k != NULL && k->pairwise && k->tx_pn > m) { m = k->tx_pn; }
    }
    return m;
}

static unsigned keyed_pairwise_frames(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (f != NULL && !f->is_mgmt && (f->tx_flags & MMDRV_TX_FLAG_HW_ENC) != 0u &&
            (f->bytes[4] & 0x01u) == 0u) { n++; }
    }
    return n;
}

static void t_restore_keeps_pn_forward(void)
{
    printf("--- SAE: a restored MTK starts above every PN the chip has already used ---\n");
    fresh(/*sae=*/true, /*grp_std=*/false, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_MGTK, OWN_MGTK_ID, false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    (void)simnode_set_key(C, K_C_MTK, 0, true);
    uint64_t last = max_install_pn();
    simnode_outbox_clear();
    for (int i = 0; i < 10; i++) { (void)simnode_host_tx(BC, W, PAY, sizeof(PAY)); }
    unsigned sent = keyed_pairwise_frames();
    CHECK(sent == 20u, "ten broadcasts, one keyed replica each to A and C (%u)", sent);

    simnode_keyinst_clear();
    simnode_del_peer(A);
    const struct simnode_keyinst *k = find_install(K_C_MTK, true);
    CHECK(k != NULL && k->tx_pn > last + sent,
          "C's restore PN 0x%llx is past the %u frames sent since the last install at 0x%llx",
          k != NULL ? (unsigned long long)k->tx_pn : 0ull, sent, (unsigned long long)last);

    last = k != NULL ? k->tx_pn : 0u;
    simnode_keyinst_clear();
    for (unsigned s = 0; s < 2u && simnode_keyinst_count() == 0u; s++)
    {
        g_warthog_rekey_req = s + 1u;
        umac_datapath_mesh_service_rekey();
    }
    k = find_install(K_C_MTK, true);
    CHECK(k != NULL && k->tx_pn > last, "AT+REKEY also moves it forward (0x%llx > 0x%llx)",
          k != NULL ? (unsigned long long)k->tx_pn : 0ull, (unsigned long long)last);
}

/* ---- 11. no-chip-key builds keep the chip empty -------------------------- */

static void t_nochip_restore_installs_nothing(void)
{
    printf("--- AMPE_NO_CHIP_KEY: a peer removal and AT+REKEY put no pairwise key in the chip ---\n");
    fresh(/*sae=*/true, /*grp_std=*/false, /*secure=*/true);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    (void)simnode_set_key(C, K_C_MTK, 0, true);
    CHECK(max_install_pn() == 0u && find_install(K_A_MTK, true) == NULL,
          "AMPE left both MTKs host-only");
    simnode_del_peer(A);
    uint32_t done0 = g_warthog_rekey_done;
    g_warthog_rekey_req = 1u + 1u;
    umac_datapath_mesh_service_rekey();
    g_warthog_rekey_req = 0u + 1u;
    umac_datapath_mesh_service_rekey();
    CHECK(find_install(K_C_MTK, true) == NULL, "C's MTK never reaches the chip");
    CHECK(g_warthog_rekey_done == done0 && g_warthog_rekey_aid == 0xffffffffu,
          "and AT+REKEYSTAT? does not claim it did");
}

/* ---- 12. an unkeyed SAE candidate's cleartext goes nowhere ------------- */

extern volatile uint32_t g_warthog_tx_nokey;
bool umac_mesh_fwd_glue_proxy_via_peer(const uint8_t *da, uint8_t out[6]);

static const uint8_t H2[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x88 }; /* host behind A */

/* A 4-address QoS mesh data frame from @p ta to us, mesh SA @p ta, optionally
 * carrying @p ae_host in Address Extension mode 2; protected frames are laid
 * out as the chip hands them up after decrypting in place. */
static uint16_t mk_to_us(uint8_t *f, const uint8_t *ta, bool prot, uint8_t pn,
                         const uint8_t *ae_host)
{
    uint16_t n = umac_mesh_ies_build_data_hdr4(f, W, ta, W, ta);
    if (prot) { f[1] |= 0x40u; }
    f[n++] = 0x00;
    f[n++] = 0x01;
    if (prot)
    {
        const uint8_t pn6[6] = { pn, 0, 0, 0, 0, 0 };
        umac_ccmp_write_header(&f[n], pn6, 0);
        n = (uint16_t)(n + UMAC_CCMP_HDR_LEN);
    }
    struct umac_mesh_ctrl mc = { .flags = 0, .ttl = 31, .seq = (uint32_t)(900u + pn) };
    if (ae_host != NULL)
    {
        mc.flags = UMAC_MESH_CTRL_AE_A5A6;
        memcpy(mc.eaddr1, W, 6);
        memcpy(mc.eaddr2, ae_host, 6);
    }
    n = (uint16_t)(n + umac_mesh_ctrl_build(&f[n], 18u, &mc));
    static const uint8_t snap[8] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00 };
    memcpy(&f[n], snap, sizeof(snap));
    n = (uint16_t)(n + sizeof(snap));
    memcpy(&f[n], PAY, sizeof(PAY));
    n = (uint16_t)(n + sizeof(PAY));
    if (prot)
    {
        memset(&f[n], 0xA5, 8);
        n = (uint16_t)(n + 8u);
    }
    return n;
}

static void t_unkeyed_candidate_rx(void)
{
    printf("--- SAE: cleartext from an unkeyed candidate is not delivered, relayed or learned ---\n");
    fresh(/*sae=*/true, /*grp_std=*/false, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_MGTK, OWN_MGTK_ID, false);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    (void)simnode_add_peer(X);                     /* sta_add'ed, SAE never finished */

    uint8_t frame[200];
    uint16_t n = mk_to_us(frame, A, /*prot=*/true, 1, H2);
    (void)simnode_rx_flags(frame, n, -60, MMDRV_RX_FLAG_DECRYPTED);
    CHECK(simnode_host_rx_count() == 1u, "keyed A's frame from H2 is delivered (%u)",
          simnode_host_rx_count());
    uint8_t via[6] = { 0 };
    CHECK(umac_mesh_fwd_glue_proxy_via_peer(H2, via) && memcmp(via, A, 6) == 0,
          "and H2 is learned behind A");

    simnode_host_rx_clear();
    n = mk_to_us(frame, X, /*prot=*/false, 0, H2);
    (void)simnode_rx(frame, n, -60);
    CHECK(simnode_host_rx_count() == 0u, "X's cleartext unicast is not delivered (%u)",
          simnode_host_rx_count());
    CHECK(g_warthog_rxdrop_reason == 3u, "dropped as plaintext, reason 3 (got %lu)",
          (unsigned long)g_warthog_rxdrop_reason);
    CHECK(umac_mesh_fwd_glue_proxy_via_peer(H2, via) && memcmp(via, A, 6) == 0,
          "and its Address Extension claim does not move H2 off A");

    /* Nor can it teach a host behind a node we do not hear: only a keyed
     * transmitter may name the relay a reply is handed to. */
    static const uint8_t M[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x4d };
    static const uint8_t H6[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x96 };
    n = mk_to_us(frame, X, /*prot=*/false, 0, H6);
    memcpy(&frame[24], M, 6);
    (void)simnode_rx(frame, n, -60);
    CHECK(!umac_mesh_fwd_glue_proxy_via_peer(H6, via) && g_warthog_rxdrop_reason == 3u,
          "X's cleartext naming H6 behind non-neighbour M teaches nothing (reason %lu)",
          (unsigned long)g_warthog_rxdrop_reason);
    n = mk_to_us(frame, A, /*prot=*/true, 2, H6);
    memcpy(&frame[24], M, 6);
    (void)simnode_rx_flags(frame, n, -60, MMDRV_RX_FLAG_DECRYPTED);
    CHECK(umac_mesh_fwd_glue_proxy_via_peer(H6, via) && memcmp(via, A, 6) == 0,
          "keyed A relaying H6 from M does: H6 is sent through A");
    simnode_outbox_clear();
    uint32_t nokey0 = g_warthog_tx_nokey;
    (void)simnode_host_tx(H2, W, PAY, sizeof(PAY));
    CHECK(frame_to(A) != NULL && g_warthog_tx_nokey == nokey0, "a reply to H2 still reaches A");

    /* A relay with standard group frames would otherwise re-send X's broadcast
     * keyed under our own MGTK, laundering it for every keyed peer. */
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/true, /*secure=*/true);
    simnode_host_rx_clear();
    simnode_outbox_clear();
    n = umac_mesh_ies_build_data_hdr3_group(frame, BC, X, X);
    frame[n++] = 0x00;
    frame[n++] = 0x01;
    struct umac_mesh_ctrl mc = { .flags = 0, .ttl = 31, .seq = 4242 };
    n = (uint16_t)(n + umac_mesh_ctrl_build(&frame[n], 18u, &mc));
    static const uint8_t snap[8] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00 };
    memcpy(&frame[n], snap, sizeof(snap));
    n = (uint16_t)(n + sizeof(snap));
    memcpy(&frame[n], PAY, sizeof(PAY));
    n = (uint16_t)(n + sizeof(PAY));
    (void)simnode_rx(frame, n, -60);
    CHECK(simnode_host_rx_count() == 0u && simnode_outbox_count() == 0u,
          "X's cleartext broadcast is neither delivered nor re-flooded (%u delivered, %u sent)",
          simnode_host_rx_count(), simnode_outbox_count());
}

/* ---- 30. group frames skip a slot AMPE has not keyed ----------------------- */

extern volatile uint32_t g_warthog_tx_bcast_dup;
static unsigned own_group_frames(void);

/* Frames waiting in @p addr's datapath queue: the netif task's side of the race
 * with del_peer, which a failed SAE runs on such a slot. */
static uint32_t queued_(const uint8_t *addr)
{
    struct umac_sta_data *s = umac_datapath_mesh_find_peer(addr);
    return s != NULL ? umac_sta_data_get_queued_len(s) : 0xffffffffu;
}

static void t_group_skips_unkeyed_slots(void)
{
    printf("--- 30. SAE: a group frame is queued to no slot AMPE has not keyed ---\n");
    fresh(/*sae=*/true, /*grp_std=*/false, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_MGTK, OWN_MGTK_ID, false);
    (void)simnode_add_peer(X);                     /* slot 0, never keyed */
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    (void)simnode_add_peer(C);
    (void)simnode_set_key(C, K_C_MTK, 0, true);
    uint32_t dup0 = g_warthog_tx_bcast_dup, nokey0 = g_warthog_tx_nokey;
    (void)simnode_host_tx_nopump(BC, W, PAY, sizeof(PAY));
    CHECK(queued_(X) == 0u && queued_(A) == 1u && queued_(C) == 1u && g_warthog_tx_bcast_dup == dup0 + 1u,
          "AT+MESHGRP=0, X first: A takes the original, C one copy, X nothing (X %u A %u C %u)",
          (unsigned)queued_(X), (unsigned)queued_(A), (unsigned)queued_(C));
    simnode_pump();
    CHECK(g_warthog_tx_nokey == nokey0 && frame_to(X) == NULL && frame_to(A) != NULL && frame_to(C) != NULL,
          "so no frame is dropped for want of a key, and A and C each get theirs");

    fresh(/*sae=*/true, /*grp_std=*/false, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_MGTK, OWN_MGTK_ID, false);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    (void)simnode_add_peer(X);                     /* between two keyed peers */
    (void)simnode_add_peer(C);
    (void)simnode_set_key(C, K_C_MTK, 0, true);
    (void)simnode_host_tx_nopump(BC, W, PAY, sizeof(PAY));
    CHECK(queued_(X) == 0u && queued_(A) == 1u && queued_(C) == 1u,
          "X between keyed A and C is copied nothing (X %u A %u C %u)", (unsigned)queued_(X),
          (unsigned)queued_(A), (unsigned)queued_(C));
    simnode_pump();

    fresh(/*sae=*/true, /*grp_std=*/true, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_MGTK, OWN_MGTK_ID, false);
    (void)simnode_add_peer(X);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    (void)simnode_host_tx_nopump(BC, W, PAY, sizeof(PAY));
    CHECK(queued_(X) == 0u && queued_(A) == 1u,
          "AT+MESHGRP=1: the one group frame rides keyed A's queue, not first-slot X's (X %u A %u)",
          (unsigned)queued_(X), (unsigned)queued_(A));
    simnode_pump();
    CHECK(own_group_frames() == 1u, "and goes out under our MGTK (%u)", own_group_frames());

    fresh(/*sae=*/true, /*grp_std=*/false, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_MGTK, OWN_MGTK_ID, false);
    (void)simnode_add_peer(X);
    nokey0 = g_warthog_tx_nokey;
    (void)simnode_host_tx_nopump(BC, W, PAY, sizeof(PAY));
    CHECK(queued_(X) == 0u, "with only unkeyed X, a group frame is queued nowhere (%u)", (unsigned)queued_(X));
    simnode_pump();
    CHECK(simnode_outbox_count() == 0u && g_warthog_tx_nokey == nokey0, "and nothing reaches the chip");

    /* A group frame from A that we re-flood: the original to a keyed peer other than A. */
    fresh(/*sae=*/true, /*grp_std=*/false, /*secure=*/true);
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_MGTK, OWN_MGTK_ID, false);
    (void)simnode_add_peer(X);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    (void)simnode_add_peer(C);
    (void)simnode_set_key(C, K_C_MTK, 0, true);
    uint8_t hdr[32] = { 0 };
    (void)umac_mesh_ies_build_data_hdr3_group(hdr, BC, A, A);
    struct umac_mesh_fwd_rx_result fr;
    memset(&fr, 0, sizeof(fr));
    fr.verdict = UMAC_MESH_FWD_DELIVER_AND_FORWARD;
    memcpy(fr.fwd_ra, BC, 6);
    memcpy(fr.mesh_da, BC, 6);
    memcpy(fr.mesh_sa, A, 6);
    fr.fwd_mc.ttl = 30;
    fr.fwd_mc.seq = 5151;
    struct mmpkt *body = umac_datapath_alloc_mmpkt_for_qos_data_tx(sizeof(PAY),
                                                                   MMDRV_PKT_CLASS_DATA_TID0);
    if (body == NULL) { CHECK(false, "a body buffer"); return; }
    struct mmpktview *bv = mmpkt_open(body);
    mmpkt_append_data(bv, PAY, sizeof(PAY));
    umac_mesh_fwd_glue_forward(umac_data_get_umacd(), bv, 0x0800, (const struct dot11_hdr *)hdr,
                               (const struct dot11_data_hdr *)hdr, &fr);
    mmpkt_close(&bv);
    mmpkt_release(body);
    CHECK(queued_(X) == 0u && queued_(C) == 1u && queued_(A) == 0u,
          "relayed from A: the original goes to keyed C, not first-slot X, none back to A (X %u C %u A %u)",
          (unsigned)queued_(X), (unsigned)queued_(C), (unsigned)queued_(A));
    simnode_pump();

    /* An open mesh has no unkeyed slot: every peer still gets its replica. */
    fresh(/*sae=*/false, /*grp_std=*/false, /*secure=*/false);
    (void)simnode_add_peer(X);
    (void)simnode_add_peer(A);
    (void)simnode_host_tx_nopump(BC, W, PAY, sizeof(PAY));
    CHECK(queued_(X) == 1u && queued_(A) == 1u, "open mesh: X and A each queue one (X %u A %u)",
          (unsigned)queued_(X), (unsigned)queued_(A));
    simnode_pump();
}

/* ---- 32. a relay never hands a held frame to a slot AMPE has not keyed ------ */

extern volatile uint32_t g_warthog_fwd_hold, g_warthog_fwd_hold_tx, g_warthog_fwd_uni;
static const uint8_t T[6]   = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x5c }; /* mesh DA, unknown */
static const uint8_t SRC[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x5d };
static const uint8_t K_T_MTK[16] = { 0x5c, 0x5d, 0x5e, 0x5f, 0x60, 0x61, 0x62, 0x63,
                                     0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b };

/* A 4-address mesh data frame @p ta relays to us for mesh DA @p da, chip-decrypted. */
static uint16_t mk_relayed(uint8_t *f, const uint8_t *ta, const uint8_t *da, uint8_t pn, uint16_t seq)
{
    uint16_t n = umac_mesh_ies_build_data_hdr4(f, W, ta, da, SRC);
    f[1] |= 0x40u;
    f[n++] = 0x00;
    f[n++] = 0x01;
    const uint8_t pn6[6] = { pn, 0, 0, 0, 0, 0 };
    umac_ccmp_write_header(&f[n], pn6, 0);
    n = (uint16_t)(n + UMAC_CCMP_HDR_LEN);
    struct umac_mesh_ctrl mc = { .flags = 0, .ttl = 31, .seq = seq };
    n = (uint16_t)(n + umac_mesh_ctrl_build(&f[n], 18u, &mc));
    static const uint8_t snap[8] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00 };
    memcpy(&f[n], snap, sizeof(snap));
    n = (uint16_t)(n + sizeof(snap));
    memcpy(&f[n], PAY, sizeof(PAY));
    n = (uint16_t)(n + sizeof(PAY));
    memset(&f[n], 0xA5, 8);
    return (uint16_t)(n + 8u);
}

static unsigned data_to_(const uint8_t *addr, bool keyed)
{
    unsigned k = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        k += (f != NULL && !f->is_mgmt && MAC_EQ(&f->bytes[4], addr) &&
              (!keyed || (f->tx_flags & MMDRV_TX_FLAG_HW_ENC) != 0u));
    }
    return k;
}

static void t_relay_skips_unkeyed_candidate(void)
{
    printf("--- 32. SAE relay: a frame held for T waits out T's unkeyed slot ---\n");
    fresh(/*sae=*/true, /*grp_std=*/false, /*secure=*/true);
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/true);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    uint8_t f[256];
    const uint32_t h0 = g_warthog_fwd_hold, tx0 = g_warthog_fwd_hold_tx, u0 = g_warthog_fwd_uni,
                   nk0 = g_warthog_tx_nokey;
    uint16_t n = mk_relayed(f, A, T, 1, 700);
    (void)simnode_rx_flags(f, n, -60, MMDRV_RX_FLAG_DECRYPTED);
    CHECK(g_warthog_fwd_hold - h0 == 1, "A's frame for unknown T is held while we discover T");

    (void)simnode_add_peer(T); /* T turns up as an SAE candidate: a slot, no key yet */
    simnode_outbox_clear();
    simnode_advance_ms(100);
    simnode_tick();
    CHECK(g_warthog_fwd_hold_tx == tx0 && g_warthog_tx_nokey == nk0 && data_to_(T, false) == 0u,
          "the tick does not release it into T's unkeyed slot (hold tx +%lu, nokey +%lu)",
          (unsigned long)(g_warthog_fwd_hold_tx - tx0), (unsigned long)(g_warthog_tx_nokey - nk0));
    n = mk_relayed(f, A, T, 2, 701);
    (void)simnode_rx_flags(f, n, -60, MMDRV_RX_FLAG_DECRYPTED);
    CHECK(g_warthog_fwd_uni == u0 && g_warthog_fwd_hold - h0 == 2 && g_warthog_tx_nokey == nk0,
          "nor is the next one forwarded into it: held too (uni +%lu, hold +%lu)",
          (unsigned long)(g_warthog_fwd_uni - u0), (unsigned long)(g_warthog_fwd_hold - h0));

    (void)simnode_set_key(T, K_T_MTK, 0, true);
    simnode_advance_ms(100);
    simnode_tick();
    CHECK(g_warthog_fwd_hold_tx - tx0 == 2 && data_to_(T, true) == 2u && g_warthog_tx_nokey == nk0,
          "once AMPE keys T, the next tick sends both, keyed (hold tx +%lu, %u on air)",
          (unsigned long)(g_warthog_fwd_hold_tx - tx0), data_to_(T, true));
}

/* ---- 13-28. MGTK replay counters: what we advertise, what we accept -------- */

extern volatile uint32_t g_warthog_ampe_mgtk_installed;
extern volatile uint32_t g_warthog_mgtk_reinst, g_warthog_mgtk_rsc_fail;
extern volatile uint32_t g_warthog_txst_total;
bool ccmp_is_valid(struct umac_sta_data *stad, uint8_t *ccmp_header,
                   enum umac_key_rx_counter_space space);

/* hostap installs every MGTK, ours and each peer's, under key id 1. */
#define HOSTAP_MGTK_ID 1u

static uint64_t le48(const uint8_t b[6])
{
    uint64_t v = 0;
    for (int i = 5; i >= 0; i--) { v = (v << 8) | b[i]; }
    return v;
}

/* What hostap writes into our next AMPE Open's GTKdata for our MGTK. */
static uint64_t own_rsc(void)
{
    uint8_t r[6] = { 0xee, 0xee, 0xee, 0xee, 0xee, 0xee };
    (void)simnode_own_group_rsc(OWN_MGTK_ID, r);
    return le48(r);
}

/* The last INSTALL_KEY of @p key as a group key, or NULL. */
static const struct simnode_keyinst *last_group_install(const uint8_t key[16])
{
    const struct simnode_keyinst *r = NULL;
    for (unsigned i = 0; i < simnode_keyinst_count(); i++)
    {
        const struct simnode_keyinst *k = simnode_keyinst_get(i);
        if (k != NULL && !k->pairwise && memcmp(k->key, key, 16) == 0) { r = k; }
    }
    return r;
}

static uint64_t max_any_install_pn(void)
{
    uint64_t m = 0;
    for (unsigned i = 0; i < simnode_keyinst_count(); i++)
    {
        const struct simnode_keyinst *k = simnode_keyinst_get(i);
        if (k != NULL && k->tx_pn > m) { m = k->tx_pn; }
    }
    return m;
}

/* Group-addressed data frames handed to the chip to encrypt under our MGTK. */
static unsigned own_group_frames(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (f != NULL && !f->is_mgmt && (f->tx_flags & MMDRV_TX_FLAG_HW_ENC) != 0u &&
            (f->bytes[4] & 0x01u) != 0u && f->key_idx == OWN_MGTK_ID) { n++; }
    }
    return n;
}

/* Offer @p stad's keychain a CCMP header at @p pn under @p key_id, exactly as the
 * RX path does after host CCMP: true if the replay check takes it (and so advances). */
static bool replay_ok(struct umac_sta_data *stad, uint8_t key_id, uint64_t pn)
{
    const uint8_t pn6[6] = { (uint8_t)(pn >> 40), (uint8_t)(pn >> 32), (uint8_t)(pn >> 24),
                             (uint8_t)(pn >> 16), (uint8_t)(pn >> 8),  (uint8_t)pn };
    uint8_t hdr[UMAC_CCMP_HDR_LEN];
    umac_ccmp_write_header(hdr, pn6, key_id);
    return ccmp_is_valid(stad, hdr, UMAC_KEY_RX_COUNTER_SPACE_DEFAULT);
}

#ifdef WARTHOG_MESH_MGTK_PN_BASE
/* Our MGTKs, one per case: SDK statics outlive fresh(), key material must not repeat. */
static const uint8_t K_OWN_R1[16] = { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                      0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f };
static const uint8_t K_OWN_R2[16] = { 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                                      0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f };
static const uint8_t K_OWN_R3[16] = { 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
                                      0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f };
static const uint8_t K_OWN_R4[16] = { 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
                                      0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f };
static const uint8_t K_OWN_R5[16] = { 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
                                      0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f };
static const uint8_t K_OWN_R8[16] = { 0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
                                      0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef };
static const uint8_t K_OWN_R6[16] = { 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
                                      0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f };
static void t_own_rsc_covers_frames_sent(void)
{
    printf("--- 13. our MGTK's RSC: a later peer refuses every group frame sent before it ---\n");
    fresh(/*sae=*/true, /*grp_std=*/true, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_R1, OWN_MGTK_ID, false);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    const struct simnode_keyinst *k0 = last_group_install(K_OWN_R1);
    CHECK(k0 != NULL && k0->aid == 0u, "our MGTK is in the chip's group slot");
    if (k0 == NULL) { return; }
    uint64_t p0 = k0->tx_pn;
    uint16_t v0 = k0->vif_id;
    CHECK(own_rsc() + 1u == p0, "with nothing sent yet, RSC 0x%llx is one below the install PN 0x%llx",
          (unsigned long long)own_rsc(), (unsigned long long)p0);

    uint32_t mg0 = g_warthog_ampe_mgtk_installed, re0 = g_warthog_mgtk_reinst;
    simnode_outbox_clear();
    for (int i = 0; i < 5; i++) { (void)simnode_host_tx(BC, W, PAY, sizeof(PAY)); }
    unsigned sent = own_group_frames();
    CHECK(sent == 5u, "five group frames went to the chip under our MGTK (%u)", sent);

    /* C peers now: AMPE asks for our RSC while it builds C's Open. */
    (void)simnode_add_peer(C);
    simnode_keyinst_clear();
    uint64_t r = own_rsc();
    CHECK(r >= p0 + sent, "RSC 0x%llx covers every PN those frames can carry (<= 0x%llx): C "
          "refuses a replay of any of them", (unsigned long long)r, (unsigned long long)(p0 + sent));
    const struct simnode_keyinst *k1 = last_group_install(K_OWN_R1);
    CHECK(k1 != NULL && simnode_keyinst_count() == 1u && k1->aid == 0u &&
              k1->key_idx == OWN_MGTK_ID && k1->vif_id == v0,
          "the chip PN cannot be read, so our MGTK was re-installed once: same VIF, slot and id");
    uint64_t b1 = k1 != NULL ? k1->tx_pn : 0u; /* k1 points into the log, cleared below */
    CHECK(r + 1u == b1, "and RSC 0x%llx is one below the new base 0x%llx: C takes our next frame",
          (unsigned long long)r, (unsigned long long)b1);
    CHECK(b1 > p0 + sent, "A, which holds our MGTK already, sees our PN only move forward");
    CHECK(g_warthog_mgtk_reinst == re0 + 1u && g_warthog_ampe_mgtk_installed == mg0,
          "counted as mgtk_reinst (%lu -> %lu); ampe_mgtk, the first installs, stays %lu",
          (unsigned long)re0, (unsigned long)g_warthog_mgtk_reinst,
          (unsigned long)g_warthog_ampe_mgtk_installed);

    /* A retried Open with nothing sent since: the same answer, no churn. */
    simnode_keyinst_clear();
    uint64_t r2 = own_rsc();
    CHECK(r2 == r && r2 + 1u == b1 && simnode_keyinst_count() == 0u,
          "a retried Open with no group frame since gets the same RSC and no re-install "
          "(0x%llx, %u installs)", (unsigned long long)r2, simnode_keyinst_count());

    /* A key id that is not our MGTK (the IGTK, id 4) was never used: 0. */
    uint8_t rsc3[6] = { 1, 1, 1, 1, 1, 1 };
    (void)simnode_own_group_rsc(4, rsc3);
    CHECK(le48(rsc3) == 0u && simnode_keyinst_count() == 0u && own_rsc() + 1u == b1,
          "a key id that is not our MGTK answers 0 and installs nothing; ours still answers 0x%llx",
          (unsigned long long)(b1 - 1u));

    /* One frame is enough to need a re-install. */
    simnode_outbox_clear();
    (void)simnode_host_tx(BC, W, PAY, sizeof(PAY));
    CHECK(own_group_frames() == 1u, "our next broadcast is still keyed with our MGTK");
    simnode_keyinst_clear();
    uint64_t r4 = own_rsc();
    const struct simnode_keyinst *k2 = last_group_install(K_OWN_R1);
    CHECK(k2 != NULL && r4 >= b1 + 1u && r4 + 1u == k2->tx_pn,
          "after a single group frame the next Open re-installs: RSC 0x%llx >= the PN it used "
          "(<= 0x%llx)", (unsigned long long)r4, (unsigned long long)(b1 + 1u));
}

static void t_own_rsc_before_install(void)
{
    printf("--- 14. an RSC asked while our MGTK is out of the chip still covers it ---\n");
    fresh(/*sae=*/true, /*grp_std=*/true, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_R2, OWN_MGTK_ID, false);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    const struct simnode_keyinst *k0 = last_group_install(K_OWN_R2);
    if (k0 == NULL) { CHECK(false, "our MGTK was installed"); return; }
    uint64_t g = k0->tx_pn;
    for (int i = 0; i < 3; i++) { (void)simnode_host_tx(BC, W, PAY, sizeof(PAY)); }

    /* The mesh restarts and hostap delivers the same key again, before any peer. */
    simnode_del_peer(NULL);
    (void)simnode_set_key(BC, K_OWN_R2, OWN_MGTK_ID, false);
    simnode_keyinst_clear();
    uint64_t r = own_rsc();
    CHECK(r >= g + 3u && simnode_keyinst_count() == 0u,
          "RSC 0x%llx lies above every PN the key used before (<= 0x%llx), and nothing is "
          "installed without a peer", (unsigned long long)r, (unsigned long long)(g + 3u));
    (void)simnode_add_peer(A);
    const struct simnode_keyinst *k = last_group_install(K_OWN_R2);
    CHECK(k != NULL && k->tx_pn > r, "its install at 0x%llx lies above that RSC",
          k != NULL ? (unsigned long long)k->tx_pn : 0ull);
    uint64_t hi = max_any_install_pn();

    /* A new key (the usual restart): above everything, RSC exactly one below. */
    simnode_del_peer(NULL);
    (void)simnode_set_key(BC, K_OWN_R3, OWN_MGTK_ID, false);
    (void)simnode_add_peer(A);
    const struct simnode_keyinst *k3 = last_group_install(K_OWN_R3);
    CHECK(k3 != NULL && k3->tx_pn > hi && own_rsc() + 1u == k3->tx_pn,
          "a NEW MGTK installs at 0x%llx, above every earlier install (0x%llx), RSC one below",
          k3 != NULL ? (unsigned long long)k3->tx_pn : 0ull, (unsigned long long)hi);
}

static void t_meshgrp0_never_reinstalls(void)
{
    printf("--- 15. AT+MESHGRP=0 (default): replicas and unicast never move our MGTK ---\n");
    fresh(/*sae=*/true, /*grp_std=*/false, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_R4, OWN_MGTK_ID, false);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    (void)simnode_add_peer(C);
    (void)simnode_set_key(C, K_C_MTK, 0, true);
    const struct simnode_keyinst *k0 = last_group_install(K_OWN_R4);
    if (k0 == NULL) { CHECK(false, "our MGTK was installed"); return; }
    uint64_t p0 = k0->tx_pn;
    uint32_t re0 = g_warthog_mgtk_reinst;
    simnode_outbox_clear();
    for (int i = 0; i < 5; i++) { (void)simnode_host_tx(BC, W, PAY, sizeof(PAY)); }
    for (int i = 0; i < 3; i++) { (void)simnode_host_tx(A, W, PAY, sizeof(PAY)); }
    CHECK(simnode_outbox_count() >= 8u && own_group_frames() == 0u,
          "traffic went out (%u frames), none under our MGTK", simnode_outbox_count());
    simnode_keyinst_clear();
    uint64_t r = own_rsc();
    CHECK(simnode_keyinst_count() == 0u && r + 1u == p0 && g_warthog_mgtk_reinst == re0,
          "the next Open re-installs nothing and advertises the install base (%u installs, "
          "rsc 0x%llx, base 0x%llx)", simnode_keyinst_count(), (unsigned long long)r,
          (unsigned long long)p0);
}

#ifdef WARTHOG_MESH_HOST_CCMP
static const uint8_t K_OWN_RB[16] = { 0x2b, 0x3b, 0x4b, 0x5b, 0x6b, 0x7b, 0x8b, 0x9b,
                                      0xab, 0xbb, 0xcb, 0xdb, 0xeb, 0xfb, 0x0b, 0x1b };
static void t_meshgrp0_host_ccmp_never_reinstalls(void)
{
    printf("--- 15b. the same with host CCMP armed: host-sealed unicast never moves our MGTK ---\n");
    const uint32_t was = g_warthog_host_ccmp_on;
    g_warthog_host_ccmp_on = 1;
    fresh(/*sae=*/true, /*grp_std=*/false, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_RB, OWN_MGTK_ID, false);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    (void)simnode_add_peer(C);
    (void)simnode_set_key(C, K_C_MTK, 0, true);
    const struct simnode_keyinst *k0 = last_group_install(K_OWN_RB);
    if (k0 == NULL) { CHECK(false, "our MGTK was installed"); g_warthog_host_ccmp_on = was; return; }
    const uint64_t p0 = k0->tx_pn;
    const uint32_t re0 = g_warthog_mgtk_reinst;
    simnode_outbox_clear();
    for (int i = 0; i < 3; i++) { (void)simnode_host_tx(A, W, PAY, sizeof(PAY)); }
    unsigned sealed = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        sealed += (f != NULL && !f->is_mgmt && MAC_EQ(&f->bytes[4], A) && (f->bytes[1] & 0x40u) != 0u &&
                   (f->tx_flags & MMDRV_TX_FLAG_HW_ENC) == 0u);
    }
    CHECK(sealed == 3u && own_group_frames() == 0u,
          "three unicasts to A went out host-sealed (%u), none under our MGTK", sealed);
    simnode_keyinst_clear();
    const uint64_t r = own_rsc();
    CHECK(simnode_keyinst_count() == 0u && r + 1u == p0 && g_warthog_mgtk_reinst == re0,
          "the next Open re-installs nothing and advertises the install base (%u installs, "
          "reinst +%lu, rsc 0x%llx, base 0x%llx)", simnode_keyinst_count(),
          (unsigned long)(g_warthog_mgtk_reinst - re0), (unsigned long long)r,
          (unsigned long long)p0);
    g_warthog_host_ccmp_on = was;
}
#endif

static void t_reinstall_failure_keeps_rsc_below_chip_pn(void)
{
    printf("--- 16. a failed re-install advertises what the OLD install is known to exceed ---\n");
    fresh(/*sae=*/true, /*grp_std=*/true, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_R5, OWN_MGTK_ID, false);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    const struct simnode_keyinst *k0 = last_group_install(K_OWN_R5);
    if (k0 == NULL) { CHECK(false, "our MGTK was installed"); return; }
    uint64_t p0 = k0->tx_pn;
    uint64_t hi = max_any_install_pn();
    for (int i = 0; i < 4; i++) { (void)simnode_host_tx(BC, W, PAY, sizeof(PAY)); }
    (void)simnode_add_peer(C);

    uint32_t re0 = g_warthog_mgtk_reinst, fail0 = g_warthog_mgtk_rsc_fail;
    simnode_keyinst_clear();
    simnode_fail_next_install_key();
    uint64_t r = own_rsc();
    CHECK(r + 1u == p0, "RSC 0x%llx is the old base - 1, below every PN the chip goes on to use "
          "(>= 0x%llx), so C still takes our frames", (unsigned long long)r, (unsigned long long)p0);
    CHECK(g_warthog_mgtk_rsc_fail == fail0 + 1u && g_warthog_mgtk_reinst == re0,
          "counted as mgtk_rsc_fail, not as a re-install");

    uint64_t r2 = own_rsc();
    const struct simnode_keyinst *k1 = last_group_install(K_OWN_R5);
    CHECK(k1 != NULL && r2 + 1u == k1->tx_pn && k1->tx_pn > p0 + 4u && k1->tx_pn > hi &&
              g_warthog_mgtk_reinst == re0 + 1u,
          "the next Open retries: installed at 0x%llx, above the 4 frames and every earlier "
          "install, RSC one below", k1 != NULL ? (unsigned long long)k1->tx_pn : 0ull);

    /* A failed FIRST install: the key is out of the chip, so an Open re-installs nothing
     * (no VIF is known good for it) however much went out; the next peer installs it. */
    simnode_del_peer(NULL);
    (void)simnode_set_key(BC, K_OWN_R8, OWN_MGTK_ID, false);
    simnode_keyinst_clear();
    simnode_fail_next_install_key();
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    for (int i = 0; i < 3; i++) { (void)simnode_host_tx(BC, W, PAY, sizeof(PAY)); }
    uint64_t ra = own_rsc();
    for (int i = 0; i < 2; i++) { (void)simnode_host_tx(BC, W, PAY, sizeof(PAY)); }
    uint64_t rb = own_rsc();
    CHECK(last_group_install(K_OWN_R8) == NULL && rb == ra && ra != 0u,
          "with our MGTK out of the chip, Opens install nothing and keep one RSC (0x%llx)",
          (unsigned long long)rb);
    (void)simnode_add_peer(C);
    const struct simnode_keyinst *k8 = last_group_install(K_OWN_R8);
    CHECK(k8 != NULL && k8->tx_pn > rb, "the next peer installs it, above that RSC (0x%llx)",
          k8 != NULL ? (unsigned long long)k8->tx_pn : 0ull);
}

static void t_relayed_group_frame_counts(void)
{
    printf("--- 17. a group frame we relay goes out under our MGTK and is counted ---\n");
    fresh(/*sae=*/true, /*grp_std=*/true, /*secure=*/true);
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/true, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_R6, OWN_MGTK_ID, false);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    (void)simnode_add_peer(C);
    (void)simnode_set_key(C, K_C_MTK, 0, true);
    uint64_t r0 = own_rsc();

    /* The call the RX path makes for a group frame from A it must re-flood. */
    uint8_t hdr[32] = { 0 };
    (void)umac_mesh_ies_build_data_hdr3_group(hdr, BC, A, A);
    struct umac_mesh_fwd_rx_result fr;
    memset(&fr, 0, sizeof(fr));
    fr.verdict = UMAC_MESH_FWD_DELIVER_AND_FORWARD;
    memcpy(fr.fwd_ra, BC, 6);
    memcpy(fr.mesh_da, BC, 6);
    memcpy(fr.mesh_sa, A, 6);
    fr.fwd_mc.ttl = 30;
    fr.fwd_mc.seq = 5150;
    struct mmpkt *body = umac_datapath_alloc_mmpkt_for_qos_data_tx(sizeof(PAY),
                                                                   MMDRV_PKT_CLASS_DATA_TID0);
    if (body == NULL) { CHECK(false, "a body buffer"); return; }
    struct mmpktview *bv = mmpkt_open(body);
    mmpkt_append_data(bv, PAY, sizeof(PAY));
    simnode_outbox_clear();
    umac_mesh_fwd_glue_forward(umac_data_get_umacd(), bv, 0x0800, (const struct dot11_hdr *)hdr,
                               (const struct dot11_data_hdr *)hdr, &fr);
    mmpkt_close(&bv);
    mmpkt_release(body);
    simnode_pump();
    CHECK(own_group_frames() == 1u, "the relayed frame went to the chip under our MGTK (%u)",
          own_group_frames());

    simnode_keyinst_clear();
    uint64_t r1 = own_rsc();
    const struct simnode_keyinst *k = last_group_install(K_OWN_R6);
    CHECK(k != NULL && r1 >= r0 + 2u && r1 + 1u == k->tx_pn,
          "so the next Open re-installs: RSC 0x%llx covers the PN it used (<= 0x%llx)",
          (unsigned long long)r1, (unsigned long long)(r0 + 2u));
}

#ifndef WARTHOG_MESH_AMPE_NO_CHIP_KEY
static const uint8_t K_OWN_R7[16] = { 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
                                      0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f };

static void t_one_pn_allocator(void)
{
    printf("--- 18. one PN allocator: group and pairwise installs never go below each other ---\n");
    fresh(/*sae=*/true, /*grp_std=*/true, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_R7, OWN_MGTK_ID, false);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    const struct simnode_keyinst *ka = find_install(K_A_MTK, true);
    if (ka == NULL) { CHECK(false, "A's MTK is in the chip"); return; }
    uint64_t pa = ka->tx_pn;
    for (int i = 0; i < 3; i++) { (void)simnode_host_tx(BC, W, PAY, sizeof(PAY)); }
    (void)simnode_add_peer(C);
    (void)own_rsc();
    const struct simnode_keyinst *kg = last_group_install(K_OWN_R7);
    if (kg == NULL) { CHECK(false, "our MGTK was re-installed"); return; }
    uint64_t g = kg->tx_pn;
    CHECK(g > pa, "our MGTK's re-install at 0x%llx lies above A's MTK at 0x%llx",
          (unsigned long long)g, (unsigned long long)pa);

    for (int i = 0; i < 2; i++) { (void)simnode_host_tx(BC, W, PAY, sizeof(PAY)); }
    simnode_keyinst_clear();
    (void)simnode_set_key(C, K_C_MTK, 0, true);
    const struct simnode_keyinst *kc = find_install(K_C_MTK, true);
    CHECK(kc != NULL && kc->tx_pn > g + 2u,
          "C's MTK at 0x%llx lies above the 2 frames our MGTK sent from 0x%llx",
          kc != NULL ? (unsigned long long)kc->tx_pn : 0ull, (unsigned long long)g);

    /* More group frames than one 2^20 epoch holds, counted as the TX path counts them. */
    for (uint32_t i = 0; i < (2u << 20); i++) { umac_datapath_mesh_own_group_tx_note(); }
    uint64_t top = g + 2u + (2u << 20);
    simnode_keyinst_clear();
    simnode_del_peer(A);
    const struct simnode_keyinst *kr = find_install(K_C_MTK, true);
    CHECK(kr != NULL && kr->tx_pn > top,
          "survivor C's restored MTK at 0x%llx lies above every PN our MGTK can have used (0x%llx)",
          kr != NULL ? (unsigned long long)kr->tx_pn : 0ull, (unsigned long long)top);

    for (uint32_t i = 0; i < (2u << 20); i++) { umac_datapath_mesh_own_group_tx_note(); }
    top += (2u << 20);
    (void)simnode_add_peer(A);
    simnode_keyinst_clear();
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    ka = find_install(K_A_MTK, true);
    CHECK(ka != NULL && ka->tx_pn > top, "and so does the next AMPE MTK (0x%llx > 0x%llx)",
          ka != NULL ? (unsigned long long)ka->tx_pn : 0ull, (unsigned long long)top);
    /* Their TX status, as the event loop takes it: nothing stays in flight for later cases. */
    for (uint32_t i = 0; i < (4u << 20); i++) { umac_datapath_mesh_own_group_tx_done(); }
}
#endif /* !WARTHOG_MESH_AMPE_NO_CHIP_KEY */

static const uint8_t K_OWN_R9[16]  = { 0x19, 0x29, 0x39, 0x49, 0x59, 0x69, 0x79, 0x89,
                                       0x99, 0xa9, 0xb9, 0xc9, 0xd9, 0xe9, 0xf9, 0x09 };
static const uint8_t K_OWN_R10[16] = { 0x1a, 0x2a, 0x3a, 0x4a, 0x5a, 0x6a, 0x7a, 0x8a,
                                       0x9a, 0xaa, 0xba, 0xca, 0xda, 0xea, 0xfa, 0x0a };
static const uint8_t K_OWN_R11[16] = { 0x1b, 0x2b, 0x3b, 0x4b, 0x5b, 0x6b, 0x7b, 0x8b,
                                       0x9b, 0xab, 0xbb, 0xcb, 0xdb, 0xeb, 0xfb, 0x0b };

static void t_reinstall_above_a_flood(void)
{
    printf("--- 23. after more group frames than an epoch holds, a re-install is above them ---\n");
    fresh(/*sae=*/true, /*grp_std=*/true, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_R9, OWN_MGTK_ID, false);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    const struct simnode_keyinst *k0 = last_group_install(K_OWN_R9);
    if (k0 == NULL) { CHECK(false, "our MGTK was installed"); return; }
    /* Four epochs' worth, counted as the TX path counts them. */
    const uint64_t n = 4u << 20;
    const uint64_t top = k0->tx_pn + n;
    for (uint64_t i = 0; i < n; i++) { umac_datapath_mesh_own_group_tx_note(); }
    (void)simnode_add_peer(C);
    simnode_keyinst_clear();
    uint64_t r = own_rsc();
    const struct simnode_keyinst *k1 = last_group_install(K_OWN_R9);
    CHECK(k1 != NULL && k1->tx_pn > top && r >= top && r + 1u == k1->tx_pn,
          "C's Open re-installs at 0x%llx, above base + sent (0x%llx), and advertises one below "
          "(0x%llx)", k1 != NULL ? (unsigned long long)k1->tx_pn : 0ull,
          (unsigned long long)top, (unsigned long long)r);
    for (uint64_t i = 0; i < n; i++) { umac_datapath_mesh_own_group_tx_done(); }
}

static const uint8_t K_OWN_RA[16] = { 0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
                                      0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0x0f };

/* (29) hostap re-delivering the SAME own MGTK used to reset its base, so after more
 * than one epoch of frames the next install could reuse PNs under the same key. */
static void t_redelivered_own_mgtk_above_old_pns(void)
{
    printf("--- 29. the same own MGTK delivered again installs above every PN it used ---\n");
    fresh(/*sae=*/true, /*grp_std=*/true, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_RA, OWN_MGTK_ID, false);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    const struct simnode_keyinst *k0 = last_group_install(K_OWN_RA);
    if (k0 == NULL) { CHECK(false, "our MGTK was installed"); return; }
    const uint64_t n = 4u << 20;
    const uint64_t top = k0->tx_pn + n;
    for (uint64_t i = 0; i < n; i++) { umac_datapath_mesh_own_group_tx_note(); }
    for (uint64_t i = 0; i < n; i++) { umac_datapath_mesh_own_group_tx_done(); }
    simnode_keyinst_clear();
    (void)simnode_set_key(BC, K_OWN_RA, OWN_MGTK_ID, false);
    const struct simnode_keyinst *k1 = last_group_install(K_OWN_RA);
    CHECK(k1 != NULL && k1->tx_pn > top,
          "re-delivered, it goes in at 0x%llx, above the 0x%llx its last install reached",
          k1 != NULL ? (unsigned long long)k1->tx_pn : 0ull, (unsigned long long)top);
}

static void t_own_rsc_past_2_32(void)
{
    printf("--- 24. past PN 2^32 our RSC still carries all six octets ---\n");
    fresh(/*sae=*/true, /*grp_std=*/true, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_R10, OWN_MGTK_ID, false);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    (void)simnode_add_peer(C);
    /* Each Open after a broadcast re-installs an epoch (2^20) higher: ~4096 pass 2^32,
     * and the RSC (base - 1) passes it one epoch after the base does. */
    uint8_t rsc[6] = { 0 };
    uint64_t pn = 0;
    unsigned opens = 0;
    while (opens < 5000u && pn <= (1ull << 32))
    {
        simnode_outbox_clear();
        simnode_keyinst_clear();
        (void)simnode_host_tx(BC, W, PAY, sizeof(PAY));
        (void)simnode_own_group_rsc(OWN_MGTK_ID, rsc);
        opens++;
        const struct simnode_keyinst *k = last_group_install(K_OWN_R10);
        if (k == NULL) { break; }
        pn = k->tx_pn;
    }
    CHECK(pn > (1ull << 32), "%u Opens, each after one broadcast, re-installed our MGTK at 0x%llx",
          opens, (unsigned long long)pn);
    CHECK(le48(rsc) + 1u == pn && (rsc[4] | rsc[5]) != 0u,
          "and the last one advertised 0x%llx, one below it, octets above 2^32 included",
          (unsigned long long)le48(rsc));
}

static void t_reinstall_on_mesh_vif(void)
{
    printf("--- 25. our MGTK is re-installed on the VIF it was installed on ---\n");
    simnode_del_peer(NULL);
    (void)simnode_start_sae_vif(W, 3);
    simnode_set_gates(/*fwd=*/false, /*bridge=*/false, /*grp_std=*/true, /*secure=*/true);
    simnode_outbox_clear();
    simnode_keyinst_clear();
    (void)simnode_set_key(BC, K_OWN_R11, OWN_MGTK_ID, false);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    const struct simnode_keyinst *k0 = last_group_install(K_OWN_R11);
    CHECK(k0 != NULL && k0->vif_id == 3u, "with the mesh on VIF 3, our MGTK goes in on VIF %u",
          k0 != NULL ? (unsigned)k0->vif_id : 0xffffu);
    (void)simnode_host_tx(BC, W, PAY, sizeof(PAY));
    (void)simnode_add_peer(C);
    simnode_keyinst_clear();
    (void)own_rsc();
    const struct simnode_keyinst *k1 = last_group_install(K_OWN_R11);
    CHECK(k1 != NULL && k1->vif_id == 3u, "and C's Open re-installs it there too (VIF %u)",
          k1 != NULL ? (unsigned)k1->vif_id : 0xffffu);
    simnode_del_peer(NULL);
}

/* ---- 26-28. our group frames between the host and the air ------------------
 *
 * The host counts a group frame when it hands it to the chip; the chip draws its
 * PN only when it sends it, from whatever install is in the group slot then. An
 * INSTALL_KEY travels on the command channel and overtakes data still queued, so
 * frames counted before a re-install can go out under the new base. The fake chip
 * holds them (simnode_tx_hold) to put them there. */
static const uint8_t D[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0d };
static const uint8_t K_OWN_R12[16] = { 0x1d, 0x2d, 0x3d, 0x4d, 0x5d, 0x6d, 0x7d, 0x8d,
                                       0x9d, 0xad, 0xbd, 0xcd, 0xdd, 0xed, 0xfd, 0x0d };
static const uint8_t K_OWN_R13[16] = { 0x1e, 0x2e, 0x3e, 0x4e, 0x5e, 0x6e, 0x7e, 0x8e,
                                       0x9e, 0xae, 0xbe, 0xce, 0xde, 0xee, 0xfe, 0x0e };
static const uint8_t K_OWN_R14[16] = { 0x1f, 0x2f, 0x3f, 0x4f, 0x5f, 0x6f, 0x7f, 0x8f,
                                       0x9f, 0xaf, 0xbf, 0xcf, 0xdf, 0xef, 0xff, 0x0f };

/* An SAE node, grp_std, our MGTK @p own in the chip and A keyed: true if it went in. */
static bool keyed_with_a(const uint8_t own[16])
{
    fresh(/*sae=*/true, /*grp_std=*/true, /*secure=*/true);
    (void)simnode_set_key(BC, own, OWN_MGTK_ID, false);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    return last_group_install(own) != NULL;
}

/* The chip sends every frame it holds; the event loop then takes their TX status. */
static void send_all_held(void)
{
    while (simnode_tx_send_held(0)) { }
    simnode_pump();
}

static void t_rsc_covers_frames_queued_at_reinstall(void)
{
    printf("--- 26. group frames still queued when an Open re-installs our MGTK count ---\n");
    if (!keyed_with_a(K_OWN_R12)) { CHECK(false, "our MGTK was installed"); return; }
    simnode_tx_hold(true);
    simnode_outbox_clear();
    for (int i = 0; i < 5; i++) { (void)simnode_host_tx(BC, W, PAY, sizeof(PAY)); }
    CHECK(own_group_frames() == 5u && simnode_tx_held() == 5u,
          "five group frames went to the chip under our MGTK and wait in its queue (%u held)",
          simnode_tx_held());

    (void)simnode_add_peer(C);
    simnode_keyinst_clear();
    uint64_t rc = own_rsc();
    const struct simnode_keyinst *k1 = last_group_install(K_OWN_R12);
    uint64_t b = k1 != NULL ? k1->tx_pn : 0u;
    CHECK(k1 != NULL && rc + 1u == b, "C's Open re-installs our MGTK at 0x%llx, RSC one below",
          (unsigned long long)b);

    /* Only now does the chip send them, from the new base. */
    send_all_held();
    simnode_tx_hold(false);
    uint64_t top = 0;
    CHECK(simnode_group_pn_top(&top) && top == b + 4u,
          "the chip encrypted the five at PNs from that base, up to 0x%llx",
          (unsigned long long)top);

    /* D peers with no group frame sent since. */
    (void)simnode_add_peer(D);
    simnode_keyinst_clear();
    uint64_t rd = own_rsc();
    const struct simnode_keyinst *k2 = last_group_install(K_OWN_R12);
    CHECK(rd >= top, "D's RSC 0x%llx lies at or above every PN already used (0x%llx): D refuses "
          "a replay of any of the five", (unsigned long long)rd, (unsigned long long)top);
    CHECK(k2 != NULL && rd + 1u == k2->tx_pn,
          "because D's Open re-installed our MGTK above them (at 0x%llx)",
          k2 != NULL ? (unsigned long long)k2->tx_pn : 0ull);

    /* Their TX status came back, so nothing is in flight any more. */
    simnode_keyinst_clear();
    uint64_t re = own_rsc();
    CHECK(re == rd && simnode_keyinst_count() == 0u,
          "with their TX status taken and nothing sent since, the next Open re-installs nothing "
          "(0x%llx, %u installs)", (unsigned long long)re, simnode_keyinst_count());
}

static void t_only_own_group_status_releases(void)
{
    printf("--- 27. only our own group frames' TX status releases them from the count ---\n");
    if (!keyed_with_a(K_OWN_R13)) { CHECK(false, "our MGTK was installed"); return; }
    simnode_tx_hold(true);
    simnode_outbox_clear();
    for (int i = 0; i < 3; i++) { (void)simnode_host_tx(BC, W, PAY, sizeof(PAY)); }
    for (int i = 0; i < 3; i++) { (void)simnode_host_tx(A, W, PAY, sizeof(PAY)); }
    CHECK(simnode_tx_held() == 6u && own_group_frames() == 3u,
          "three group frames under our MGTK, then three unicasts to A, wait in the chip's queue");

    /* The unicasts go first; their TX status is not ours. */
    for (int i = 0; i < 3; i++) { (void)simnode_tx_send_held(3); }
    simnode_pump();
    (void)simnode_add_peer(C);
    simnode_keyinst_clear();
    (void)own_rsc();
    const struct simnode_keyinst *k1 = last_group_install(K_OWN_R13);
    uint64_t b = k1 != NULL ? k1->tx_pn : 0u;
    CHECK(k1 != NULL, "C's Open re-installs our MGTK at 0x%llx", (unsigned long long)b);

    send_all_held();
    simnode_tx_hold(false);
    uint64_t top = 0;
    (void)simnode_group_pn_top(&top);
    (void)simnode_add_peer(D);
    uint64_t rd = own_rsc();
    CHECK(top == b + 2u && rd >= top,
          "D's RSC 0x%llx covers the three group frames sent after that re-install (up to 0x%llx)",
          (unsigned long long)rd, (unsigned long long)top);
}

static void t_unsent_status_releases(void)
{
    printf("--- 28. a group frame the chip hands back unsent is released from the count ---\n");
    if (!keyed_with_a(K_OWN_R14)) { CHECK(false, "our MGTK was installed"); return; }
    simnode_tx_hold(true);
    for (int i = 0; i < 2; i++) { (void)simnode_host_tx(BC, W, PAY, sizeof(PAY)); }
    (void)simnode_add_peer(C);
    (void)own_rsc();

    /* Out of duty cycle: the chip returns both untried (attempts 0). */
    uint32_t st0 = g_warthog_txst_total;
    while (simnode_tx_return_held(0)) { }
    simnode_pump();
    simnode_tx_hold(false);
    CHECK(g_warthog_txst_total == st0, "frames that never left count in no TX status tally (%lu)",
          (unsigned long)(g_warthog_txst_total - st0));
    (void)simnode_add_peer(D);
    (void)own_rsc(); /* still re-installs: those two were queued at C's install */
    simnode_keyinst_clear();
    (void)own_rsc();
    CHECK(simnode_keyinst_count() == 0u,
          "their status released them, so an Open after D's re-installs nothing (%u installs)",
          simnode_keyinst_count());

    /* A status the count never saw (none can arrive: each marked frame was counted). */
    umac_datapath_mesh_own_group_tx_done();
    (void)simnode_host_tx(BC, W, PAY, sizeof(PAY));
    (void)own_rsc();
    simnode_keyinst_clear();
    (void)own_rsc();
    CHECK(simnode_keyinst_count() == 0u,
          "a stray status leaves the count at 0, not wrapped: after one frame and its re-install, "
          "the next Open re-installs nothing (%u installs)", simnode_keyinst_count());
}

#else /* !WARTHOG_MESH_MGTK_PN_BASE */

static void t_pnbase_off_installs_at_zero(void)
{
    printf("--- 19. no MGTK PN base: install at PN 0, RSC 0, nothing re-installs ---\n");
    fresh(/*sae=*/true, /*grp_std=*/true, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_MGTK, OWN_MGTK_ID, false);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    const struct simnode_keyinst *k0 = find_install(K_OWN_MGTK, false);
    CHECK(k0 != NULL && k0->tx_pn == 0u, "our MGTK goes into the chip at TX PN 0");
    uint32_t re0 = g_warthog_mgtk_reinst;
    for (int i = 0; i < 5; i++) { (void)simnode_host_tx(BC, W, PAY, sizeof(PAY)); }
    (void)simnode_add_peer(C);
    simnode_keyinst_clear();
    uint8_t rsc[6] = { 1, 1, 1, 1, 1, 1 };
    (void)simnode_own_group_rsc(OWN_MGTK_ID, rsc);
    CHECK(le48(rsc) == 0u && simnode_keyinst_count() == 0u && g_warthog_mgtk_reinst == re0,
          "the RSC is 0 after group traffic, and nothing is re-installed");
}

#endif /* WARTHOG_MESH_MGTK_PN_BASE */

static const uint8_t K_A_MGTK_R1[16] = { 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
                                         0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f };
static const uint8_t K_A_MGTK_R2[16] = { 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
                                         0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f };
static const uint8_t K_A_MGTK_R3[16] = { 0x1c, 0x2c, 0x3c, 0x4c, 0x5c, 0x6c, 0x7c, 0x8c,
                                         0x9c, 0xac, 0xbc, 0xcc, 0xdc, 0xec, 0xfc, 0x0c };
static const uint8_t ZERO6[6] = { 0 };            /* what an OpenMANET node advertises */

static void t_peer_rsc_is_replay_floor(void)
{
    printf("--- 20. a peer's MGTK: its advertised RSC is where our replay window starts ---\n");
    fresh(/*sae=*/true, /*grp_std=*/true, /*secure=*/true);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    const uint8_t rsc[6] = { 0x56, 0x34, 0x12, 0x00, 0x00, 0x00 }; /* 0x123456, PN0 first */
    (void)simnode_set_key_rsc(A, K_A_MGTK, HOSTAP_MGTK_ID, /*pairwise=*/false, rsc);
    struct umac_sta_data *a = umac_datapath_mesh_find_peer(A);
    if (a == NULL) { CHECK(false, "A is a peer"); return; }
    CHECK(!replay_ok(a, HOSTAP_MGTK_ID, 0x10u), "a group frame below the advertised RSC is a replay");
    CHECK(!replay_ok(a, HOSTAP_MGTK_ID, 0x123456u) && replay_ok(a, HOSTAP_MGTK_ID, 0x123457u),
          "the floor is exactly the RSC: PN 0x123456 is a replay, 0x123457 is taken");

    /* A patched warthog advertises epoch << 20 - 1: past 2^24 early, past 2^40 in time. */
    const uint8_t r48[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 }; /* 0x060504030201 */
    (void)simnode_set_key_rsc(A, K_A_MGTK_R3, HOSTAP_MGTK_ID, false, r48);
    CHECK(!replay_ok(a, HOSTAP_MGTK_ID, 0x0504030202ull),
          "a new MGTK with RSC 0x060504030201: PN 0x0504030202 (five octets) is a replay");
    CHECK(!replay_ok(a, HOSTAP_MGTK_ID, 0x060504030201ull) &&
              replay_ok(a, HOSTAP_MGTK_ID, 0x060504030202ull),
          "all six octets are the floor: 0x060504030201 is a replay, 0x060504030202 is taken");
}

static void t_peer_rekey_same_stad_keeps_floor(void)
{
    printf("--- 21. the same MGTK installed again on a live link does not lower its floor ---\n");
    fresh(/*sae=*/true, /*grp_std=*/true, /*secure=*/true);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    (void)simnode_set_key_rsc(A, K_A_MGTK_R1, HOSTAP_MGTK_ID, false, ZERO6);
    struct umac_sta_data *a = umac_datapath_mesh_find_peer(A);
    if (a == NULL) { CHECK(false, "A is a peer"); return; }
    CHECK(replay_ok(a, HOSTAP_MGTK_ID, 70u), "A's group frame at PN 70 is taken");
    (void)simnode_set_key_rsc(A, K_A_MGTK_R1, HOSTAP_MGTK_ID, false, ZERO6);
    CHECK(!replay_ok(a, HOSTAP_MGTK_ID, 70u) && replay_ok(a, HOSTAP_MGTK_ID, 71u),
          "installed again with RSC 0, 70 is still a replay and 71 is taken");

    const uint8_t r200[6] = { 200, 0, 0, 0, 0, 0 };
    (void)simnode_set_key_rsc(A, K_A_MGTK_R1, HOSTAP_MGTK_ID, false, r200);
    CHECK(!replay_ok(a, HOSTAP_MGTK_ID, 150u) && replay_ok(a, HOSTAP_MGTK_ID, 201u),
          "an RSC above the live counter (200 > 71) still sets the floor");

    const uint8_t r5[6] = { 5, 0, 0, 0, 0, 0 };
    (void)simnode_set_key_rsc(A, K_A_MGTK_R2, HOSTAP_MGTK_ID, false, r5);
    CHECK(!replay_ok(a, HOSTAP_MGTK_ID, 5u) && replay_ok(a, HOSTAP_MGTK_ID, 6u),
          "a NEW MGTK from A starts from its own RSC (5), not the old key's counter (201)");

    /* Re-advertised with the RSC an earlier Open carried, after frames flowed since. */
    CHECK(replay_ok(a, HOSTAP_MGTK_ID, 500u), "A's frame at PN 500 under that MGTK is taken");
    const uint8_t r40[6] = { 40, 0, 0, 0, 0, 0 };
    (void)simnode_set_key_rsc(A, K_A_MGTK_R2, HOSTAP_MGTK_ID, false, r40);
    CHECK(!replay_ok(a, HOSTAP_MGTK_ID, 450u) && replay_ok(a, HOSTAP_MGTK_ID, 501u),
          "installed again with a nonzero RSC below its counter (40 < 500): 450 is still a "
          "replay, 501 is taken");
}

static void t_repeer_starts_from_advertised_rsc(void)
{
    printf("--- 22. a re-peered link starts from the RSC the peer advertises now ---\n");
    fresh(/*sae=*/true, /*grp_std=*/true, /*secure=*/true);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    (void)simnode_set_key_rsc(A, K_A_MGTK_R1, HOSTAP_MGTK_ID, false, ZERO6);
    struct umac_sta_data *a = umac_datapath_mesh_find_peer(A);
    if (a == NULL) { CHECK(false, "A is a peer"); return; }
    CHECK(replay_ok(a, HOSTAP_MGTK_ID, 500u), "A's group frame at PN 500 is taken");

    /* The link drops; A's radio restarts its PN under the SAME MGTK and re-peers. */
    simnode_del_peer(A);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    const uint8_t r40[6] = { 40, 0, 0, 0, 0, 0 };
    (void)simnode_set_key_rsc(A, K_A_MGTK_R1, HOSTAP_MGTK_ID, false, r40);
    a = umac_datapath_mesh_find_peer(A);
    if (a == NULL) { CHECK(false, "A is a peer again"); return; }
    CHECK(!replay_ok(a, HOSTAP_MGTK_ID, 40u) && replay_ok(a, HOSTAP_MGTK_ID, 41u),
          "the floor is A's new RSC (40), not the old link's 500: A is not locked out");
}

/* ---- 31. a chip-decrypted unicast under a key that is not the link's MTK ---- */

static const uint8_t K_OWN_RC[16] = { 0x2c, 0x3c, 0x4c, 0x5c, 0x6c, 0x7c, 0x8c, 0x9c,
                                      0xac, 0xbc, 0xcc, 0xdc, 0xec, 0xfc, 0x0c, 0x1c };

/* A unicast QoS data frame from @p ta to us, as the chip hands it up after opening it
 * under CCMP key id @p kid at packet number @p pn. */
static uint16_t mk_uni_decrypted(uint8_t *f, const uint8_t *ta, uint8_t kid, uint64_t pn)
{
    uint16_t n = mk_to_us(f, ta, /*prot=*/true, 0, NULL);
    const uint8_t pn6[6] = { (uint8_t)(pn >> 40), (uint8_t)(pn >> 32), (uint8_t)(pn >> 24),
                             (uint8_t)(pn >> 16), (uint8_t)(pn >> 8),  (uint8_t)pn };
    umac_ccmp_write_header(&f[32], pn6, kid);
    return n;
}

static void t_decrypted_unicast_off_pairwise_key(void)
{
    printf("--- 31. SAE: a unicast the chip opened under another key id is dropped (96) ---\n");
    fresh(/*sae=*/true, /*grp_std=*/false, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_RC, HOSTAP_MGTK_ID, false);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    (void)simnode_set_key(A, K_A_MGTK, HOSTAP_MGTK_ID, false);
    (void)simnode_add_peer(C);
    (void)simnode_set_key(C, K_C_MTK, 0, true);
    struct umac_sta_data *a = umac_datapath_mesh_find_peer(A);
    if (a == NULL) { CHECK(false, "A is a peer"); return; }
    simnode_host_rx_clear();
    uint8_t f[200];

    /* C holds our MGTK (id 1, as A's) from AMPE: it seals a unicast in A's name. */
    uint16_t n = mk_uni_decrypted(f, A, HOSTAP_MGTK_ID, 0x7f0000000000ull);
    (void)simnode_rx_flags(f, n, -60, MMDRV_RX_FLAG_DECRYPTED);
    CHECK(simnode_host_rx_count() == 0u && g_warthog_rxdrop_reason == 96u,
          "a unicast in A's name under key id 1 is not delivered: reason %lu",
          (unsigned long)g_warthog_rxdrop_reason);
    CHECK(replay_ok(a, HOSTAP_MGTK_ID, 0x1000u),
          "and A's MGTK replay floor did not move to its PN: A's group frame at 0x1000 is taken");

    simnode_host_rx_clear();
    n = mk_uni_decrypted(f, A, 0, 0x7f0000000000ull);
    (void)simnode_rx_flags(f, n, -60, MMDRV_RX_FLAG_DECRYPTED);
#ifdef WARTHOG_MESH_AMPE_NO_CHIP_KEY
    CHECK(simnode_host_rx_count() == 0u && g_warthog_rxdrop_reason == 96u,
          "no-chip-key build: under key id 0 too, since the chip holds no MTK (reason %lu)",
          (unsigned long)g_warthog_rxdrop_reason);
    CHECK(replay_ok(a, 0, 5u), "and A's MTK floor did not move");
#ifdef WARTHOG_MESH_HOST_CCMP
    {
        /* C's unicast sealed under its MTK, which the chip could not open: host CCMP's. */
        const uint32_t was = g_warthog_host_ccmp_on;
        g_warthog_host_ccmp_on = 1;
        simnode_host_rx_clear();
        n = mk_uni_decrypted(f, C, 0, 0x100u);
        const uint8_t pn6[6] = { 0, 0, 0, 0, 0x01, 0x00 };
        uint8_t aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
        const uint32_t al = umac_ccmp_build_aad(f, aad);
        umac_ccmp_build_nonce(f, pn6, nonce);
        (void)warthog_ccm_ae(K_C_MTK, nonce, 8, aad, al, &f[40], (size_t)(n - 48u), &f[n - 8u]);
        (void)simnode_rx(f, n, -60);
        CHECK(simnode_host_rx_count() == 1u,
              "(pin) one host CCMP opened under C's MTK is delivered, not refused (%u, reason %lu)",
              simnode_host_rx_count(), (unsigned long)g_warthog_rxdrop_reason);
        g_warthog_host_ccmp_on = was;
    }
#endif
#else
    CHECK(simnode_host_rx_count() == 1u, "(pin) under key id 0, the MTK's, it is delivered (%u)",
          simnode_host_rx_count());
#endif

    /* Neither an open nor a keyed non-SAE mesh: every node shares both keys there. */
    fresh(/*sae=*/false, /*grp_std=*/false, /*secure=*/true);
    (void)simnode_add_peer(A);
    n = mk_uni_decrypted(f, A, HOSTAP_MGTK_ID, 7u);
    (void)simnode_rx_flags(f, n, -60, MMDRV_RX_FLAG_DECRYPTED);
    CHECK(simnode_host_rx_count() == 1u,
          "(pin) a keyed non-SAE mesh still takes a decrypted unicast under key id 1 (%u, reason %lu)",
          simnode_host_rx_count(), (unsigned long)g_warthog_rxdrop_reason);
}

/* ---- 33. one receive counter per TID -------------------------------------- */

/* A QoS data frame from @p ta on @p tid, PN @p pn under key @p kid: a unicast to us or a
 * 3-address group frame, laid out as the chip hands up a decrypted one, each with its own
 * Mesh Control seq. With @p seal it is sealed under that key instead, as the peer would. */
static uint16_t mk_tid_(uint8_t *f, bool group, const uint8_t *ta, uint8_t kid, uint8_t tid,
                        uint64_t pn, const uint8_t *seal)
{
    static uint32_t mseq = 5000;
    const uint8_t pn6[6] = { (uint8_t)(pn >> 40), (uint8_t)(pn >> 32), (uint8_t)(pn >> 24),
                             (uint8_t)(pn >> 16), (uint8_t)(pn >> 8),  (uint8_t)pn };
    uint16_t n = group ? umac_mesh_ies_build_data_hdr3_group(f, BC, ta, ta)
                       : umac_mesh_ies_build_data_hdr4(f, W, ta, W, ta);
    f[1] |= 0x40u;
    f[n++] = tid;
    f[n++] = 0x01;
    umac_ccmp_write_header(&f[n], pn6, kid);
    n = (uint16_t)(n + UMAC_CCMP_HDR_LEN);
    const uint16_t body = n;
    struct umac_mesh_ctrl mc = { .flags = 0, .ttl = 31, .seq = ++mseq };
    n = (uint16_t)(n + umac_mesh_ctrl_build(&f[n], 18u, &mc));
    static const uint8_t snap[8] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00 };
    memcpy(&f[n], snap, sizeof(snap));
    n = (uint16_t)(n + sizeof(snap));
    memcpy(&f[n], PAY, sizeof(PAY));
    n = (uint16_t)(n + sizeof(PAY));
    memset(&f[n], 0xA5, 8);
    n = (uint16_t)(n + 8u);
    if (seal != NULL)
    {
        uint8_t aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
        const uint32_t al = umac_ccmp_build_aad(f, aad);
        umac_ccmp_build_nonce(f, pn6, nonce);
        (void)warthog_ccm_ae(seal, nonce, 8, aad, al, &f[body], (size_t)(n - body - 8u), &f[n - 8u]);
    }
    return n;
}

/* A Linux peer's frames on different access categories leave its chip with their PNs
 * interleaved; each TID must be judged against its own counter, as mac80211 does. */
static void tid_steps_(const char *what, bool group, uint8_t kid, uint8_t rx_flags,
                       const uint8_t *seal)
{
    static const struct { uint8_t tid; uint8_t pn; bool take; const char *why; } s[] = {
        { 5, 10, true,  "TID 5, PN 10" },
        { 0,  9, true,  "then TID 0, PN 9: TID 0 has its own counter" },
        { 0,  9, false, "TID 0, PN 9 again" },
        { 5, 10, false, "TID 5, PN 10 again" },
        { 0,  8, false, "TID 0, PN 8: below TID 0's counter" },
        { 6,  1, true,  "TID 6, PN 1: a TID not heard from yet" },
        { 5, 11, true,  "TID 5, PN 11" },
    };
    uint8_t f[200];
    for (unsigned i = 0; i < sizeof(s) / sizeof(s[0]); i++)
    {
        simnode_host_rx_clear();
        g_warthog_rxdrop_reason = 0;
        const uint16_t n = mk_tid_(f, group, A, kid, s[i].tid, s[i].pn, seal);
        (void)simnode_rx_flags(f, n, -60, rx_flags);
        const bool took = simnode_host_rx_count() == 1u;
        CHECK(took == s[i].take && (took || g_warthog_rxdrop_reason == 5u),
              "%s: %s is %s (delivered %u, reason %lu)", what, s[i].why,
              s[i].take ? "delivered" : "refused as a replay (5)", simnode_host_rx_count(),
              (unsigned long)g_warthog_rxdrop_reason);
    }
}

static void t_replay_counter_per_tid(void)
{
    printf("--- 33. a peer's TIDs keep separate replay counters ---\n");
    /* Every build: the keyed non-SAE mesh, chip-decrypted, pairwise key 0 and group key 1. */
    fresh(/*sae=*/false, /*grp_std=*/false, /*secure=*/true);
    (void)simnode_add_peer(A);
    tid_steps_("keyed mesh, unicast", false, 0, MMDRV_RX_FLAG_DECRYPTED, NULL);
    tid_steps_("keyed mesh, group", true, 1, MMDRV_RX_FLAG_DECRYPTED, NULL);
#ifndef WARTHOG_MESH_AMPE_NO_CHIP_KEY
    fresh(/*sae=*/true, /*grp_std=*/false, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_MGTK, OWN_MGTK_ID, false);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    tid_steps_("SAE, unicast the chip opened under A's MTK", false, 0, MMDRV_RX_FLAG_DECRYPTED, NULL);
#endif
#ifdef WARTHOG_MESH_HOST_CCMP
    const uint32_t was = g_warthog_host_ccmp_on;
    g_warthog_host_ccmp_on = 1;
    fresh(/*sae=*/true, /*grp_std=*/false, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_MGTK, OWN_MGTK_ID, false);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    (void)simnode_set_key(A, K_A_MGTK, HOSTAP_MGTK_ID, false);
    tid_steps_("SAE, host CCMP, unicast under A's MTK", false, 0, 0, K_A_MTK);
    tid_steps_("SAE, host CCMP, group under A's MGTK", true, HOSTAP_MGTK_ID, 0, K_A_MGTK);
    g_warthog_host_ccmp_on = was;
#endif
}

#ifdef WARTHOG_MESH_HOST_CCMP
/* ---- 34. batman mode against a wizard (SAE) node: host CCMP both ways ---------- */

extern volatile uint32_t g_warthog_swccmp_ok, g_warthog_swccmp_micfail;

/* A 20-byte ELP, as main/bat/ emits it. */
static const uint8_t BAT_ELP[20] = { 0x03, 0x0f, 0x02, 0x00, 0x00, 0x00, 0x00, 0x0a,
                                     0x12, 0x34, 0x56, 0x78, 0x00, 0x00, 0x01, 0xf4,
                                     0x00, 0x00, 0x00, 0x00 };

/* A Linux peer's ELP: a 3-address group frame from A, sealed under A's MGTK at @p pn. */
static uint16_t mk_bat_group_(uint8_t *f, uint64_t pn, uint32_t mseq)
{
    const uint8_t pn6[6] = { (uint8_t)(pn >> 40), (uint8_t)(pn >> 32), (uint8_t)(pn >> 24),
                             (uint8_t)(pn >> 16), (uint8_t)(pn >> 8),  (uint8_t)pn };
    uint16_t n = umac_mesh_ies_build_data_hdr3_group(f, BC, A, A);
    f[1] |= 0x40u;
    f[n++] = 0x00;
    f[n++] = 0x01;
    umac_ccmp_write_header(&f[n], pn6, HOSTAP_MGTK_ID);
    n = (uint16_t)(n + UMAC_CCMP_HDR_LEN);
    const uint16_t body = n;
    struct umac_mesh_ctrl mc = { .flags = 0, .ttl = 31, .seq = mseq };
    n = (uint16_t)(n + umac_mesh_ctrl_build(&f[n], 18u, &mc));
    static const uint8_t snap[8] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x43, 0x05 };
    memcpy(&f[n], snap, sizeof(snap));
    n = (uint16_t)(n + sizeof(snap));
    memcpy(&f[n], BAT_ELP, sizeof(BAT_ELP));
    n = (uint16_t)(n + sizeof(BAT_ELP));
    n = (uint16_t)(n + 8u);
    uint8_t aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
    const uint32_t al = umac_ccmp_build_aad(f, aad);
    umac_ccmp_build_nonce(f, pn6, nonce);
    (void)warthog_ccm_ae(K_A_MGTK, nonce, 8, aad, al, &f[body], (size_t)(n - body - 8u), &f[n - 8u]);
    return n;
}

/* What bat_mode_rx_classify keeps for the engine: an ELP/OGM/BCAST whose Ethernet source
 * is the transmitter (main/ is not linked into this binary). */
static bool hook_got_elp_from_a_(void)
{
    const struct simnode_extrx *e = simnode_ext_rx_get(0);
    return simnode_ext_rx_count() == 1u && e != NULL && e->have_ta && MAC_EQ(e->ta, A) &&
           MAC_EQ(&e->frame[0], BC) && MAC_EQ(&e->frame[6], A) && e->frame[12] == 0x43 &&
           e->frame[13] == 0x05 && e->len == 14u + sizeof(BAT_ELP) &&
           memcmp(&e->frame[14], BAT_ELP, sizeof(BAT_ELP)) == 0;
}

static void t_batman_sae_host_ccmp(void)
{
    printf("--- 34. batman against an SAE node: a peer's group ELP opened by host CCMP, ours sealed ---\n");
    const uint32_t was = g_warthog_host_ccmp_on;
    fresh(/*sae=*/true, /*grp_std=*/false, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_MGTK, OWN_MGTK_ID, false);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    (void)simnode_set_key(A, K_A_MGTK, HOSTAP_MGTK_ID, false);
    (void)simnode_add_peer(C);                     /* SAE not finished */
    simnode_set_batman(true);
    simnode_set_rx_ext_cb(true);
    g_warthog_host_ccmp_on = 1;                    /* bat_port arms it at boot */

    uint8_t f[200];
    uint16_t n = mk_bat_group_(f, 0x10, 7001);
    const uint32_t ok0 = g_warthog_swccmp_ok;
    g_warthog_rxdrop_reason = 0;
    (void)simnode_rx(f, n, -55);
    CHECK(hook_got_elp_from_a_() && g_warthog_swccmp_ok == ok0 + 1u,
          "A's ELP under its MGTK reaches the hook: dst ff:ff, src A = TA, 0x4305, the ELP intact "
          "(%u, reason %lu)", simnode_ext_rx_count(), (unsigned long)g_warthog_rxdrop_reason);

    simnode_ext_rx_clear();
    n = mk_bat_group_(f, 0x10, 7002);
    (void)simnode_rx(f, n, -55);
    CHECK(simnode_ext_rx_count() == 0u && g_warthog_rxdrop_reason == 5u,
          "the same PN again is a replay (reason %lu)", (unsigned long)g_warthog_rxdrop_reason);

    const uint32_t mf0 = g_warthog_swccmp_micfail;
    n = mk_bat_group_(f, 0x11, 7003);
    f[n - 20u] ^= 0x01u;
    (void)simnode_rx(f, n, -55);
    CHECK(simnode_ext_rx_count() == 0u && g_warthog_swccmp_micfail == mf0 + 1u,
          "a frame altered in flight fails the MIC and reaches nothing");

    g_warthog_host_ccmp_on = 0;
    n = mk_bat_group_(f, 0x12, 7004);
    (void)simnode_rx(f, n, -55);
    CHECK(simnode_ext_rx_count() == 0u && g_warthog_rxdrop_reason == 4u,
          "host CCMP not armed: the chip cannot open a peer's MGTK, dropped as reason 4 (%lu)",
          (unsigned long)g_warthog_rxdrop_reason);
    g_warthog_host_ccmp_on = 1;

    /* Ours, AT+MESHGRP=0: an AE-2 replica per keyed peer, sealed by the host. */
    simnode_outbox_clear();
    int st = simnode_host_tx_eth(NULL, BC, W, 0x4305, BAT_ELP, sizeof(BAT_ELP));
    const struct simnode_frame *fa = frame_to(A);
    CHECK(st == MMWLAN_SUCCESS && simnode_outbox_count() == 1u && fa != NULL && frame_to(C) == NULL,
          "our ELP: one replica, to A; none to the candidate C (%d, %u frames)", st,
          simnode_outbox_count());
    if (fa != NULL && fa->len == 94u)
    {
        CHECK((fa->bytes[1] & 0x40u) != 0u && (fa->tx_flags & MMDRV_TX_FLAG_HW_ENC) == 0u,
              "  Protected, sealed by the host, not handed to the chip to encrypt");
        uint8_t g[94], pn[6], kid = 0xff;
        memcpy(g, fa->bytes, sizeof(g));
        uint8_t aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
        const bool hdr_ok = umac_ccmp_parse_header(&g[32], pn, &kid);
        const uint32_t al = umac_ccmp_build_aad(g, aad);
        umac_ccmp_build_nonce(g, pn, nonce);
        const bool opened = hdr_ok && kid == 0 &&
                            warthog_ccm_ad(K_A_MTK, nonce, 8, aad, al, &g[40], 46, &g[86]) == 0;
        CHECK(opened, "  opens under A's MTK, key id 0 (%u)", kid);
        CHECK(opened && g[40] == UMAC_MESH_CTRL_AE_A5A6 && g[41] == UMAC_MESH_CTRL_TTL_DEFAULT &&
                  MAC_EQ(&g[46], BC) && MAC_EQ(&g[52], W) && g[64] == 0x43 && g[65] == 0x05 &&
                  memcmp(&g[66], BAT_ELP, sizeof(BAT_ELP)) == 0,
              "  inside: Mesh Control AE 2, TTL 31, addr5 ff:ff, addr6 us, LLC 0x4305, the ELP");
    }
    else
    {
        CHECK(false, "  30 MAC + 2 QoS + 8 CCMP + 18 Mesh Control + 8 LLC + 20 + 8 MIC = 94 bytes (got %u)",
              fa != NULL ? fa->len : 0u);
    }
    static uint8_t big[1500];
    memset(big, 0x5a, sizeof(big));
    simnode_outbox_clear();
    (void)simnode_host_tx_eth(NULL, BC, W, 0x4305, big, sizeof(big));
    fa = frame_to(A);
    CHECK(fa != NULL && fa->len == 74u + sizeof(big) && simnode_outbox_dropped() == 0,
          "a 1500-byte batman packet: the largest MPDU batman mode makes, 1574 bytes (got %u)",
          fa != NULL ? fa->len : 0u);

    /* AT+MESHGRP=1: one standard group frame, which the chip encrypts under our MGTK. */
    simnode_set_gates(/*fwd=*/false, /*bridge=*/false, /*grp_std=*/true, /*secure=*/true);
    simnode_outbox_clear();
    (void)simnode_host_tx_eth(NULL, BC, W, 0x4305, BAT_ELP, sizeof(BAT_ELP));
    const struct simnode_frame *fg = simnode_outbox_get(0);
    CHECK(simnode_outbox_count() == 1u && fg != NULL && (fg->bytes[4] & 0x01u) != 0u &&
              (fg->tx_flags & MMDRV_TX_FLAG_HW_ENC) != 0u && fg->key_idx == OWN_MGTK_ID &&
              fg->bytes[26] == 0x00u && fg->bytes[27] == UMAC_MESH_CTRL_TTL_DEFAULT,
          "MESHGRP=1: one group frame, HW_ENC under our MGTK id %u, AE 0, TTL 31 (%u frames)",
          OWN_MGTK_ID, simnode_outbox_count());

    g_warthog_host_ccmp_on = was;
    simnode_set_rx_ext_cb(false);
    simnode_set_batman(false);
}
#endif

#ifdef WARTHOG_MESH_HOST_CCMP
/* ---- 35. host CCMP never sees a unicast addressed to another station ------- */

extern volatile uint32_t g_warthog_filt_hist[10], g_warthog_filt_reason, g_warthog_rxdrop_count;
extern volatile uint32_t g_warthog_filt_mgmt_nours, g_warthog_filt_drop;
extern volatile uint8_t g_warthog_filt_mgmt_nours_hdr[16];
extern volatile uint32_t g_warthog_swccmp_tried, g_warthog_swccmp_fail_len,
    g_warthog_swccmp_fail_keyid;
extern volatile uint8_t g_warthog_swccmp_fail_hdr[32];

/* A's 4-address unicast to next hop @p ra for mesh DA @p da, sealed under @p seal (key id 0)
 * at @p pn, as A's radio puts it on the air. */
static uint16_t mk_sealed_uni_(uint8_t *f, const uint8_t *ra, const uint8_t *da, uint64_t pn,
                               const uint8_t *seal)
{
    static uint32_t mseq = 8000;
    const uint8_t pn6[6] = { (uint8_t)(pn >> 40), (uint8_t)(pn >> 32), (uint8_t)(pn >> 24),
                             (uint8_t)(pn >> 16), (uint8_t)(pn >> 8),  (uint8_t)pn };
    uint16_t n = umac_mesh_ies_build_data_hdr4(f, ra, A, da, A);
    f[1] |= 0x40u;
    f[n++] = 0x00;
    f[n++] = 0x01;
    umac_ccmp_write_header(&f[n], pn6, 0);
    n = (uint16_t)(n + UMAC_CCMP_HDR_LEN);
    const uint16_t body = n;
    struct umac_mesh_ctrl mc = { .flags = 0, .ttl = 31, .seq = ++mseq };
    n = (uint16_t)(n + umac_mesh_ctrl_build(&f[n], 18u, &mc));
    static const uint8_t snap[8] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00 };
    memcpy(&f[n], snap, sizeof(snap));
    n = (uint16_t)(n + sizeof(snap));
    memcpy(&f[n], PAY, sizeof(PAY));
    n = (uint16_t)(n + sizeof(PAY));
    n = (uint16_t)(n + 8u);
    uint8_t aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
    const uint32_t al = umac_ccmp_build_aad(f, aad);
    umac_ccmp_build_nonce(f, pn6, nonce);
    (void)warthog_ccm_ae(seal, nonce, 8, aad, al, &f[body], (size_t)(n - body - 8u), &f[n - 8u]);
    return n;
}

static void t_host_ccmp_skips_others_unicast(void)
{
    printf("--- 35. host CCMP never sees a keyed peer's unicast to another station ---\n");
    const uint32_t was = g_warthog_host_ccmp_on;
    fresh(/*sae=*/true, /*grp_std=*/false, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN_MGTK, OWN_MGTK_ID, false);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    (void)simnode_set_key(A, K_A_MGTK, HOSTAP_MGTK_ID, false);
    (void)simnode_add_peer(C);
    (void)simnode_set_key(C, K_C_MTK, 0, true);
    g_warthog_host_ccmp_on = 1;

    /* On the bench: a warthog's frame to a Linux node, under the link key only those two hold. */
    static const uint8_t K_A_TO_C[16] = { 0xac, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                          0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
    uint8_t f[256];
    uint8_t hdr0[32];
    for (unsigned i = 0; i < sizeof(hdr0); i++)
    {
        hdr0[i] = g_warthog_swccmp_fail_hdr[i];
    }
    uint32_t tr0 = g_warthog_swccmp_tried, mf0 = g_warthog_swccmp_micfail,
             no0 = g_warthog_filt_hist[9], rd0 = g_warthog_rxdrop_count;
    const uint32_t fl0 = g_warthog_swccmp_fail_len;
    simnode_host_rx_clear();
    simnode_outbox_clear();
    uint16_t n = mk_sealed_uni_(f, C, C, 1u, K_A_TO_C);
    (void)simnode_rx(f, n, -60);
    bool hdr_same = true;
    for (unsigned i = 0; i < sizeof(hdr0); i++)
    {
        hdr_same = hdr_same && hdr0[i] == g_warthog_swccmp_fail_hdr[i];
    }
    CHECK(g_warthog_filt_hist[9] == no0 + 1u && g_warthog_filt_reason == 9u,
          "A's unicast to C is dropped by the receive filter (not_ours +%lu, reason %lu)",
          (unsigned long)(g_warthog_filt_hist[9] - no0), (unsigned long)g_warthog_filt_reason);
    CHECK(g_warthog_swccmp_tried == tr0 && g_warthog_swccmp_micfail == mf0 && hdr_same &&
              g_warthog_swccmp_fail_len == fl0,
          "  before host CCMP: tried +%lu, micfail +%lu, the fail snapshot %s",
          (unsigned long)(g_warthog_swccmp_tried - tr0),
          (unsigned long)(g_warthog_swccmp_micfail - mf0), hdr_same ? "untouched" : "overwritten");
    CHECK(g_warthog_rxdrop_count == rd0 && simnode_host_rx_count() == 0u &&
              simnode_outbox_count() == 0u,
          "  and nothing further (%lu datapath drops, %u delivered, %u sent)",
          (unsigned long)(g_warthog_rxdrop_count - rd0), simnode_host_rx_count(),
          simnode_outbox_count());

    /* Relay on, and sealed under A's MTK with us, which host CCMP could open: the RA decides. */
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/true);
    tr0 = g_warthog_swccmp_tried;
    n = mk_sealed_uni_(f, C, W, 2u, K_A_MTK);
    (void)simnode_rx(f, n, -60);
    CHECK(g_warthog_filt_hist[9] == no0 + 2u && g_warthog_swccmp_tried == tr0 &&
              simnode_host_rx_count() == 0u && simnode_outbox_count() == 0u,
          "relay on, RA C, mesh DA us, under a key we hold: dropped unopened (not_ours +%lu, "
          "tried +%lu, %u delivered, %u sent)",
          (unsigned long)(g_warthog_filt_hist[9] - no0),
          (unsigned long)(g_warthog_swccmp_tried - tr0), simnode_host_rx_count(),
          simnode_outbox_count());

    /* Ours: not_ours does not move. */
    const uint32_t ok0 = g_warthog_swccmp_ok;
    no0 = g_warthog_filt_hist[9];
    n = mk_sealed_uni_(f, W, W, 3u, K_A_MTK);
    (void)simnode_rx(f, n, -60);
    CHECK(simnode_host_rx_count() == 1u && g_warthog_swccmp_ok == ok0 + 1u &&
              g_warthog_filt_hist[9] == no0,
          "A's unicast to us is opened and delivered (%u, ok +%lu)", simnode_host_rx_count(),
          (unsigned long)(g_warthog_swccmp_ok - ok0));

    simnode_host_rx_clear();
    simnode_outbox_clear();
    n = mk_sealed_uni_(f, W, C, 4u, K_A_MTK);
    (void)simnode_rx(f, n, -60);
    const struct simnode_frame *fc = frame_to(C);
    CHECK(fc != NULL && !fc->is_mgmt && (fc->bytes[1] & 0x40u) != 0u &&
              simnode_host_rx_count() == 0u && g_warthog_swccmp_ok == ok0 + 2u &&
              g_warthog_filt_hist[9] == no0,
          "relayed: RA us, mesh DA C, opened and sent on to C protected (%u frames out)",
          simnode_outbox_count());

    simnode_host_rx_clear();
    n = mk_tid_(f, /*group=*/true, A, HOSTAP_MGTK_ID, 0, 5u, K_A_MGTK);
    (void)simnode_rx(f, n, -60);
    CHECK(simnode_host_rx_count() == 1u && g_warthog_swccmp_ok == ok0 + 3u &&
              g_warthog_filt_hist[9] == no0,
          "A's group frame under its MGTK is not judged by RA: opened and delivered (%u)",
          simnode_host_rx_count());

    /* A's Protected Block Ack action to C: counted as mgmt_nours, not dropped until the chip
     * is measured handing such frames up, so host CCMP still tries it (MIC fails). */
    no0 = g_warthog_filt_hist[9];
    const uint32_t mn0 = g_warthog_filt_mgmt_nours, fd0 = g_warthog_filt_drop;
    tr0 = g_warthog_swccmp_tried;
    memset(f, 0, 64);
    f[0] = 0xd0;
    f[1] = 0x40;
    memcpy(&f[4], C, 6);
    memcpy(&f[10], A, 6);
    memcpy(&f[16], A, 6);
    const uint8_t mpn[6] = { 0, 0, 0, 0, 0, 7 };
    umac_ccmp_write_header(&f[24], mpn, 0);
    f[32] = 3;                                    /* Block Ack */
    (void)simnode_rx(f, 32u + 8u + 8u, -60);
    CHECK(g_warthog_filt_mgmt_nours == mn0 + 1u && g_warthog_filt_drop == fd0 &&
              g_warthog_filt_hist[9] == no0 &&
              MAC_EQ((const uint8_t *)&g_warthog_filt_mgmt_nours_hdr[4], C) &&
              MAC_EQ((const uint8_t *)&g_warthog_filt_mgmt_nours_hdr[10], A),
          "a Protected unicast action frame to C is counted, mgmt_nours +%lu (addr1 C, addr2 A), "
          "not dropped by the filter",
          (unsigned long)(g_warthog_filt_mgmt_nours - mn0));
    CHECK(g_warthog_swccmp_tried == tr0 + 1u,
          "  so host CCMP still tries it (tried +%lu); AT+FILTSTAT? mgmt_nours says whether the "
          "chip hands such frames up",
          (unsigned long)(g_warthog_swccmp_tried - tr0));

    /* A frame to us that fails the MIC: counted and snapshotted as before. */
    mf0 = g_warthog_swccmp_micfail;
    n = mk_sealed_uni_(f, W, W, 6u, K_A_MTK);
    f[n - 1u] ^= 0x01u;
    (void)simnode_rx(f, n, -60);
    CHECK(g_warthog_swccmp_micfail == mf0 + 1u && g_warthog_swccmp_fail_len == (uint32_t)(n - 32u) &&
              g_warthog_swccmp_fail_keyid == 0u && g_warthog_swccmp_fail_hdr[0] == 0x88u &&
              MAC_EQ((const uint8_t *)&g_warthog_swccmp_fail_hdr[4], W) &&
              MAC_EQ((const uint8_t *)&g_warthog_swccmp_fail_hdr[10], A),
          "a frame to us failing the MIC: micfail +%lu, snapshot len %lu keyid %lu, RA us, TA A",
          (unsigned long)(g_warthog_swccmp_micfail - mf0),
          (unsigned long)g_warthog_swccmp_fail_len, (unsigned long)g_warthog_swccmp_fail_keyid);
    g_warthog_host_ccmp_on = was;
}
#endif

int main(void)
{
    printf("=== simnode keys: the SAE key lifecycle through the real datapath ===\n");
#ifdef WARTHOG_MESH_AMPE_NO_CHIP_KEY
    t_nochip_restore_installs_nothing();
#else
    t_own_mgtk_before_first_peer();
    t_group_tx_uses_own_mgtk();
    t_survivor_keeps_ampe_key();
    t_constant_mesh_unchanged();
    t_open_mesh_installs_nothing();
    t_rx_unicast_under_group_key();
    t_rekey_pushes_own_key();
    t_unkeyed_candidate_first();
    t_decrypted_group_is_forged();
    t_restore_keeps_pn_forward();
    t_unkeyed_candidate_rx();
    t_group_skips_unkeyed_slots();
    t_relay_skips_unkeyed_candidate();
#endif
#ifdef WARTHOG_MESH_MGTK_PN_BASE
    /* Our MGTK is chip-encrypted on every build, host CCMP ones included. */
    t_own_rsc_covers_frames_sent();
    t_own_rsc_before_install();
    t_meshgrp0_never_reinstalls();
#ifdef WARTHOG_MESH_HOST_CCMP
    t_meshgrp0_host_ccmp_never_reinstalls();
#endif
    t_reinstall_failure_keeps_rsc_below_chip_pn();
    t_relayed_group_frame_counts();
#ifndef WARTHOG_MESH_AMPE_NO_CHIP_KEY
    t_one_pn_allocator();
#endif
    t_reinstall_above_a_flood();
    t_redelivered_own_mgtk_above_old_pns();
    t_own_rsc_past_2_32();
    t_reinstall_on_mesh_vif();
    t_rsc_covers_frames_queued_at_reinstall();
    t_only_own_group_status_releases();
    t_unsent_status_releases();
#else
    t_pnbase_off_installs_at_zero();
#endif
    t_peer_rsc_is_replay_floor();
    t_peer_rekey_same_stad_keeps_floor();
    t_repeer_starts_from_advertised_rsc();
    t_decrypted_unicast_off_pairwise_key();
    t_replay_counter_per_tid();
#ifdef WARTHOG_MESH_HOST_CCMP
    t_batman_sae_host_ccmp();
    t_host_ccmp_skips_others_unicast();
#endif

    simnode_del_peer(NULL);
    CHECK(simnode_live_allocs() == 0, "no packet buffer was orphaned (%u live)",
          simnode_live_allocs());
    simnode_stop();

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_simnode_keys: all passed\n");
    return 0;
}
