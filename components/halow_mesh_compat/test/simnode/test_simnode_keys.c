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
 *     holds -- was delivered as the TA's and booked against the TA's replay counter;
 * (10) a survivor's restored MTK restarted the chip's shared PN counter below
 *     frames already sent, so the survivor dropped ours as replays;
 * (11, built with WARTHOG_MESH_AMPE_NO_CHIP_KEY) the restore pushed a key into
 *     the chip on the builds that exist to keep it out, and AT+REKEYSTAT? counted it;
 * (12) cleartext from an SAE candidate AMPE had not keyed was delivered, relayed
 *     under our own MGTK, and could re-point a host learned behind a keyed peer.
 */
#include <stdio.h>
#include <string.h>

#include "simnode.h"
#include "mmdrv.h"
#include "mmwlan.h"
#include "umac/datapath/umac_datapath_private.h"
#include "umac/keys/umac_keys.h"
#include "umac/data/umac_data.h"
#include "umac/mesh/umac_mesh_ccmp_hdr.h"
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
    (void)simnode_rx_flags(frame, n, -60, MMDRV_RX_FLAG_DECRYPTED);
    CHECK(simnode_host_rx_count() == 0u, "not delivered as A's (%u delivered)",
          simnode_host_rx_count());
    CHECK(g_warthog_rxdrop_reason == 95u, "rxdrop reason 95 (got %lu)",
          (unsigned long)g_warthog_rxdrop_reason);
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
#endif

    simnode_del_peer(NULL);
    CHECK(simnode_live_allocs() == 0, "no packet buffer was orphaned (%u live)",
          simnode_live_allocs());
    simnode_stop();

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_simnode_keys: all passed\n");
    return 0;
}
