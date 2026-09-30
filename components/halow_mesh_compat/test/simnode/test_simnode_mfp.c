/*
 * Path selection against a peer that runs management frame protection, through the
 * real datapath (SAE mesh). A wizard OpenMANET node runs ieee80211w=2: hostap marks
 * every SAE peer MFP, and its mac80211 then drops unicast Mesh Action frames that are
 * not CCMP-protected (rx.c ieee80211_drop_unencrypted_mgmt). Group Mesh Action frames
 * are group-addressed privacy (802.11 Table 9-47; ieee80211_is_group_privacy_action):
 * mac80211 sends every one CCMP-protected under its own MGTK whenever it holds one (tx.c
 * ieee80211_select_link_key), opens one under the sender's MGTK (rx.c
 * ieee80211_rx_h_decrypt, link_sta->gtk) above that key's management replay counter,
 * and drops one from an MFP peer that is in the clear or carries a BIP MMIE. What these
 * cases pin, each failing on the tree before the change unless marked (pin), which
 * marks behaviour that must NOT change and so passes before it too:
 *
 * TX
 *  (1) a PREP to a keyed peer whose AMPE carried an IGTK goes out Protected under that
 *      link's key: HW_ENC + its key id on the chip build, host CCMP on the swccmp
 *      build, whichever the data path would use;
 *  (2) a peer that sent no IGTK gets plaintext (pin), until it sends protected
 *      unicast path selection, which latches it; AT+MESHPMF=1 marks every keyed peer,
 *      as hostap's ieee80211w=2 does; an unkeyed candidate never (pin);
 *  (3) group path selection (a relay's or bridge's PREQ and PERR) goes out Protected
 *      under our own MGTK, sealed by the chip as our group data is (HW_ENC under its
 *      key id, no MMIE, an IGTK or not): one frame whatever the peers' MFP, counted
 *      tx gp, and in flight under that MGTK until its TX status, so the Key RSC AMPE
 *      advertises next lies above the PN it drew. Before hostap delivers our MGTK, or
 *      while its chip install has failed (the chip would be asked to seal it under a
 *      key id its group slot lacks), it goes in the clear, counted tx plain. It is
 *      built and sent on the umac event loop only: from any other task the frame is
 *      queued for it, and a relay ladder step asked from the 2 s tick whose buffer
 *      cannot be had is retried, not taken;
 *  (4) swccmp build: the protected HWMP decrypts under the MTK and draws its PN from
 *      the counter the link's data frames use.
 * RX, from a keyed peer that runs MFP:
 *  (5) unicast plaintext is refused; protected (decrypted by the chip or by host
 *      CCMP, replay-checked) is taken, but only under the link's pairwise key id;
 *      plaintext from a peer without MFP is taken (pin);
 *  (6) a group frame is taken only Protected: opened by host CCMP under that peer's
 *      own MGTK (swccmp builds) above that key's management replay counter, whose
 *      floor is the RSC its AMPE carried, and answered with a unicast CCMP PREP. In
 *      the clear, with an MMIE (even one that verifies under its IGTK), opened by the
 *      chip (whose only group key is our own MGTK, which every peer holds), under a
 *      key id that is not its MGTK, or replayed, it is refused, and a refused one
 *      moves no replay counter. A peer without MFP is held to the same, in the clear
 *      included (stricter than mac80211: every keyed peer holds an MGTK and protects
 *      with it), and a Protected group frame from it is no evidence of MFP; a 26-octet
 *      MMIE counts as one. So a relay re-sends under our MGTK no PREQ or PERR anyone
 *      could have sent in a keyed peer's name, and the forwarding glue refuses one in
 *      the clear itself. The chip build opens none, another Warthog relay's on the
 *      same image included: dropped, counted;
 *  (7) MFP comes from the IGTK in the peer's AMPE (authenticated; hostap sends one
 *      exactly when it runs ieee80211w != 0), never from the RSN element of an Open,
 *      which anyone can send in the peer's name.
 *  (8) path selection from a candidate that holds a slot but AMPE has not keyed is
 *      refused and counted (unestab) in every mode, as mac80211 takes it only from an
 *      ESTAB peer: a leaf does not answer it, a relay neither rebroadcasts its PREQ
 *      under our MGTK nor installs a path from its PREQ or PREP, and the forwarding
 *      glue's own gate refuses it too; once AMPE keys it, it is judged as any keyed
 *      peer's (6).
 *  (9) Block Ack (umac_ba.c, the A-MPDU setup every unicast data frame can start): the
 *      ADDBA request, its DELBA and our ADDBA response to a peer that runs MFP are
 *      protected as (1) and (4) are, and one that cannot be sealed is dropped, never
 *      sent in the clear, each counted once (mgmt tx chip/host/drop); a peer without
 *      MFP gets them in the clear, uncounted (pin); del_peer
 *      leaves no ADDBA retry pointing into the freed record.
 * An open or non-SAE keyed mesh is untouched (pin), a relay there included.
 */
#include <stdio.h>
#include <string.h>

#include "simnode.h"
#include "mmdrv.h"
#include "mmwlan.h"
#include "mmpkt.h"
#include "umac/datapath/umac_datapath.h"
#include "umac/datapath/umac_datapath_private.h"
#include "umac/mesh/umac_mesh_bip.h"
#include "umac/mesh/umac_mesh_ccm.h"
#include "umac/mesh/umac_mesh_ccmp_hdr.h"
#include "umac/mesh/umac_mesh_hwmp.h"
#include "umac/mesh/umac_mesh_ctrl.h"
#include "umac/mesh/umac_mesh_ies.h"
#include "umac/mesh/umac_mesh_fwd.h"
#include "umac/mesh/umac_mesh_fwd_glue.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

extern volatile uint32_t g_warthog_mesh_pmf, g_warthog_host_ccmp_on;
extern volatile uint32_t g_warthog_hwmp_prot, g_warthog_hwmp_unprotected;
extern volatile uint32_t g_warthog_hwmp_gp, g_warthog_hwmp_mmie, g_warthog_hwmp_nommie;
extern volatile uint32_t g_warthog_hwmp_tx_prot, g_warthog_hwmp_tx_gp, g_warthog_hwmp_tx_plain;
extern volatile uint32_t g_warthog_mgmt_prot_chip, g_warthog_mgmt_prot_host,
    g_warthog_mgmt_prot_nodec, g_warthog_mgmt_prot_grpkey;
extern volatile uint32_t g_warthog_mgmt_gp_nodec, g_warthog_mgmt_gp_own, g_warthog_mgmt_gp_key,
    g_warthog_mgmt_gp_replay;
extern volatile uint32_t g_warthog_hwmp_tx_qdrop, g_warthog_hwmp_relay_preq;
extern volatile uint32_t g_warthog_hwmp_unestab, g_warthog_fwd_drop_bad;

static const uint8_t W[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 }; /* us */
static const uint8_t A[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0a }; /* runs MFP */
static const uint8_t B[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0b }; /* does not */
static const uint8_t E[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0e };
static const uint8_t R[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x77 }; /* undiscovered */
static const uint8_t R2[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x78 };
static const uint8_t BC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static const uint8_t K_MTK[16]  = { 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
                                    0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf };
static const uint8_t K_IGTK[16] = { 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
                                    0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f };
static const uint8_t K_OWN[16]  = { 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
                                    0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f };
static const uint8_t K_MGTK_B[16] = { 0x9b, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                      0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
static const uint8_t K_MGTK_A[16] = { 0x9a, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                      0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f };
/* Our own MGTK: hostap's mesh_rsn makes it at mesh start, key id 1, as a peer's is. */
static const uint8_t K_MGTK_W[16] = { 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
                                      0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f };
static const uint8_t RSC5[6] = { 5, 0, 0, 0, 0, 0 };

struct snap { uint32_t prot, unprot, mmie, nommie, gp, tprot, tgp, tplain, chip, host, nodec,
              gnodec, gown, gkey, greplay; };
static struct snap snap_(void)
{
    struct snap s = { g_warthog_hwmp_prot, g_warthog_hwmp_unprotected, g_warthog_hwmp_mmie,
                      g_warthog_hwmp_nommie, g_warthog_hwmp_gp, g_warthog_hwmp_tx_prot,
                      g_warthog_hwmp_tx_gp, g_warthog_hwmp_tx_plain, g_warthog_mgmt_prot_chip,
                      g_warthog_mgmt_prot_host, g_warthog_mgmt_prot_nodec, g_warthog_mgmt_gp_nodec,
                      g_warthog_mgmt_gp_own, g_warthog_mgmt_gp_key, g_warthog_mgmt_gp_replay };
    return s;
}

/* A mesh start, with our own MGTK delivered first unless @p mgtk is false. Nothing in the
 * datapath forgets that key once delivered, as on the firmware, so only a case that runs
 * before any other can see a mesh without it. */
static void fresh_(bool mgtk)
{
    simnode_del_peer(NULL);
    (void)simnode_start_sae(W);
    simnode_set_gates(/*fwd=*/false, /*bridge=*/false, /*grp_std=*/false, /*secure=*/true);
    g_warthog_mesh_pmf = 0;
    g_warthog_host_ccmp_on = 0;
    (void)simnode_set_igtk(BC, NULL, 4, NULL);
    if (mgtk)
    {
        (void)simnode_set_key(BC, K_MGTK_W, 1, /*pairwise=*/false);
    }
    simnode_outbox_clear();
}

static void fresh(void) { fresh_(true); }

/* ---- frames a peer sends ----------------------------------------------- */

static void hdr_(uint8_t *f, const uint8_t *a1, const uint8_t *ta, bool prot)
{
    memset(f, 0, 24);
    f[0] = 0xd0; /* management, action */
    f[1] = prot ? 0x40 : 0x00;
    memcpy(f + 4, a1, 6);
    memcpy(f + 10, ta, 6);
    memcpy(f + 16, ta, 6);
}

/* Mesh Peering Open: category, action, capability, Supported Rates, RSN (caps as given,
 * or none), Mesh ID, then the MIC and AMPE elements, after which only ciphertext follows. */
static void rx_open_(const uint8_t *ta, int rsn_caps)
{
    uint8_t f[160];
    hdr_(f, W, ta, false);
    uint16_t n = 24;
    const uint8_t fixed[] = { 15, 1, 0x00, 0x00, 1, 8, 0x82, 0x84, 0x8b, 0x0c, 0x96, 0x98, 0x24, 0x30 };
    memcpy(f + n, fixed, sizeof(fixed)); n += sizeof(fixed);
    if (rsn_caps >= 0)
    {
        const uint8_t rsn[] = { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4,
                                1, 0, 0x00, 0x0f, 0xac, 8, (uint8_t)rsn_caps, (uint8_t)(rsn_caps >> 8) };
        memcpy(f + n, rsn, sizeof(rsn)); n += sizeof(rsn);
    }
    const uint8_t tail[] = { 114, 7, 's', 'i', 'm', 'n', 'o', 'd', 'e', 140, 16, 0, 0, 0, 0, 0, 0,
                             0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 139, 4, 0xde, 0xad, 0xbe, 0xef };
    memcpy(f + n, tail, sizeof(tail)); n += sizeof(tail);
    (void)simnode_rx(f, n, -50);
}

/* A peer that peered and keyed, as hostap's ESTAB delivers it: its Open (MFPC set when
 * it runs MFP), its AMPE MTK, then the IGTK its AMPE carried if it runs MFP. */
static void keyed_(const uint8_t *peer, bool mfp)
{
    (void)simnode_add_peer(peer);
    rx_open_(peer, mfp ? 0x00c0 : 0x0000);
    (void)simnode_set_key(peer, K_MTK, 0, /*pairwise=*/true);
    if (mfp)
    {
        (void)simnode_set_igtk(peer, K_IGTK, 4, NULL);
    }
}

static uint16_t preq_(uint8_t *body, const uint8_t *ta, uint32_t sn)
{
    return umac_mesh_hwmp_build_preq(body, 64, ta, sn, sn, W, 5000);
}

/* A PREQ for us, in the clear, to @p a1 (us, or broadcast). */
static void rx_preq_plain_(const uint8_t *ta, const uint8_t *a1, uint32_t sn)
{
    uint8_t f[128];
    hdr_(f, a1, ta, false);
    uint16_t n = (uint16_t)(24 + preq_(f + 24, ta, sn));
    (void)simnode_rx(f, n, -50);
}

/* A PREQ the chip decrypted under CCMP key @p kid: header, CCMP header, plaintext, MIC. */
static void rx_preq_chipdec_kid_(const uint8_t *ta, const uint8_t *a1, uint32_t sn, uint8_t pn0,
                                 uint8_t kid)
{
    uint8_t f[128];
    hdr_(f, a1, ta, true);
    const uint8_t pn[6] = { 0, 0, 0, 0, 0, pn0 };
    umac_ccmp_write_header(f + 24, pn, kid);
    uint16_t n = (uint16_t)(32 + preq_(f + 32, ta, sn));
    memset(f + n, 0, 8);
    (void)simnode_rx_flags(f, (uint16_t)(n + 8), -50, MMDRV_RX_FLAG_DECRYPTED);
}

static void rx_preq_chipdec_(const uint8_t *ta, const uint8_t *a1, uint32_t sn, uint8_t pn0)
{
    rx_preq_chipdec_kid_(ta, a1, sn, pn0, 0);
}

/* A group PREQ carrying an MMIE under @p key; @p tamper flips a body octet after sealing. */
static void rx_preq_bip_(const uint8_t *ta, const uint8_t key[16], uint16_t kid, uint64_t ipn,
                         uint32_t sn, bool tamper)
{
    uint8_t f[128];
    hdr_(f, BC, ta, false);
    uint16_t bl = preq_(f + 24, ta, sn);
    size_t n = umac_mesh_bip_protect(key, kid, ipn, f, f + 24, bl, sizeof(f) - 24);
    if (tamper) { f[30] ^= 0x01; }
    (void)simnode_rx(f, (uint16_t)(24 + n), -50);
}

/* Group path-selection body @p body sent by @p ta as mac80211 sends it (group-addressed
 * privacy): Protected, CCMP under @p key at PN @p pn with key id @p kid, and handed up as the
 * chip leaves a frame it could not open. */
static void rx_gp_body_(const uint8_t *ta, const uint8_t key[16], uint8_t kid, uint64_t pn64,
                        const uint8_t *body, uint16_t bl)
{
    uint8_t f[160];
    hdr_(f, BC, ta, true);
    const uint8_t pn[6] = { (uint8_t)(pn64 >> 40), (uint8_t)(pn64 >> 32), (uint8_t)(pn64 >> 24),
                            (uint8_t)(pn64 >> 16), (uint8_t)(pn64 >> 8), (uint8_t)pn64 };
    umac_ccmp_write_header(f + 24, pn, kid);
    memcpy(f + 32, body, bl);
    uint8_t aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
    const uint32_t al = umac_ccmp_build_aad(f, aad);
    umac_ccmp_build_nonce(f, pn, nonce);
    (void)warthog_ccm_ae(key, nonce, 8, aad, al, f + 32, bl, f + 32 + bl);
    (void)simnode_rx(f, (uint16_t)(32 + bl + 8), -50);
}

/* A group PREQ for us that way. A Linux node sends it under its own MGTK, key id 1. */
static void rx_preq_gp_(const uint8_t *ta, const uint8_t key[16], uint8_t kid, uint8_t pn0,
                        uint32_t sn)
{
    uint8_t b[64];
    rx_gp_body_(ta, key, kid, pn0, b, preq_(b, ta, sn));
}

/* A group PREQ in the clear ending in a 26-octet MMIE (BIP-CMAC-256/GMAC), key id 4. */
static void rx_preq_mmie16_(const uint8_t *ta, uint32_t sn)
{
    uint8_t f[128];
    hdr_(f, BC, ta, false);
    uint16_t n = (uint16_t)(24 + preq_(f + 24, ta, sn));
    memset(f + n, 0x3c, 26);
    f[n] = 76;
    f[n + 1] = 24;
    f[n + 2] = 4;
    f[n + 3] = 0;
    (void)simnode_rx(f, (uint16_t)(n + 26), -50);
}

#ifdef WARTHOG_MESH_HOST_CCMP
/* The peer's side of host CCMP: seal a unicast PREQ under the MTK at @p pn0. */
static void rx_preq_sealed_(const uint8_t *ta, uint32_t sn, uint8_t pn0, bool tamper)
{
    uint8_t f[128];
    hdr_(f, W, ta, true);
    const uint8_t pn[6] = { 0, 0, 0, 0, 0, pn0 };
    umac_ccmp_write_header(f + 24, pn, 0);
    uint16_t bl = preq_(f + 32, ta, sn);
    uint8_t aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
    uint32_t al = umac_ccmp_build_aad(f, aad);
    umac_ccmp_build_nonce(f, pn, nonce);
    (void)warthog_ccm_ae(K_MTK, nonce, 8, aad, al, f + 32, bl, f + 32 + bl);
    if (tamper) { f[34] ^= 0x01; }
    (void)simnode_rx(f, (uint16_t)(32 + bl + 8), -50);
}
#endif

/* @p peer's MGTK as its AMPE delivers it: key id 1, its Key RSC 5 the replay floor. */
static void mgtk_(const uint8_t *peer, const uint8_t key[16])
{
    (void)simnode_set_key_rsc(peer, key, 1, /*pairwise=*/false, RSC5);
}

/* ---- what we sent ------------------------------------------------------- */

/* The i-th HWMP frame whose (plaintext) body carries element @p eid, or NULL. */
static const struct simnode_frame *hwmp_(uint8_t eid, unsigned nth)
{
    unsigned seen = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (!f->is_mgmt || f->len < 27 || f->bytes[0] != 0xd0) { continue; }
        const uint8_t *b = f->bytes + 24;
        if (b[0] == 13 && b[2] == eid && seen++ == nth) { return f; }
    }
    return NULL;
}

static bool protected_(const struct simnode_frame *f) { return f != NULL && (f->bytes[1] & 0x40); }
static bool hw_enc_(const struct simnode_frame *f)
{
    return f != NULL && (f->tx_flags & MMDRV_TX_FLAG_HW_ENC) != 0;
}
static bool to_(const struct simnode_frame *f, const uint8_t *da)
{
    return f != NULL && memcmp(f->bytes + 4, da, 6) == 0;
}

/* The i-th PREQ we sent that asks for @p target. */
static const struct simnode_frame *preq_for_(const uint8_t *target, unsigned nth)
{
    unsigned seen = 0;
    for (unsigned i = 0; hwmp_(HWMP_EID_PREQ, i) != NULL; i++)
    {
        const struct simnode_frame *f = hwmp_(HWMP_EID_PREQ, i);
        struct hwmp_preq q;
        uint16_t bl = (uint16_t)(f->len - 24u);
        uint16_t kid = 0;
        if (umac_mesh_bip_parse(f->bytes + 24, bl, &kid, NULL)) { bl = (uint16_t)(bl - UMAC_MESH_MMIE_LEN); }
        if (umac_mesh_hwmp_parse_preq(f->bytes + 24, bl, &q) && memcmp(q.target_addr, target, 6) == 0 &&
            seen++ == nth)
        {
            return f;
        }
    }
    return NULL;
}

static unsigned preqs_for_(const uint8_t *target)
{
    unsigned n = 0;
    while (preq_for_(target, n) != NULL) { n++; }
    return n;
}

/* Sent Protected under our own MGTK as the chip seals it: HW_ENC, key id 1, no MMIE. */
static bool under_own_mgtk_(const struct simnode_frame *f)
{
    return f != NULL && (f->bytes[1] & 0x40) && hw_enc_(f) && f->key_idx == 1 &&
           !umac_mesh_bip_parse(f->bytes + 24, f->len - 24u, NULL, NULL);
}

/* The PREP a host-sealed frame @p f carries, opened under K_MTK as its peer would. */
static bool open_prep_(const struct simnode_frame *f, struct hwmp_prep *q)
{
    uint8_t c[256], pn[6], kid = 9;
    if (f == NULL || f->len <= 40u || f->len > sizeof(c) || !(f->bytes[1] & 0x40) || hw_enc_(f) ||
        !umac_ccmp_parse_header(f->bytes + 24, pn, &kid) || kid != 0)
    {
        return false;
    }
    memcpy(c, f->bytes, f->len);
    uint8_t aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
    const uint32_t al = umac_ccmp_build_aad(c, aad);
    umac_ccmp_build_nonce(c, pn, nonce);
    const uint32_t pl = f->len - 40u;
    return warthog_ccm_ad(K_MTK, nonce, 8, aad, al, c + 32, pl, c + 32 + pl) == 0 &&
           umac_mesh_hwmp_parse_prep(c + 32, (uint16_t)pl, q);
}

/* ---- cases ------------------------------------------------------------- */

static void t_tx_unicast_to_mfp_peer(void)
{
    printf("--- (1) a PREP to a peer whose Open advertised MFP goes out protected ---\n");
    fresh();
    keyed_(A, true);
    CHECK(umac_datapath_mesh_peer_mfp(A), "A (keyed, its AMPE carried an IGTK) runs MFP");
    struct snap s = snap_();
    rx_preq_chipdec_(A, W, 1, 1);
    const struct simnode_frame *p = hwmp_(HWMP_EID_PREP, 0);
    CHECK(to_(p, A) && protected_(p), "A's protected PREQ is answered by a PREP with the Protected bit");
    CHECK(hw_enc_(p) && p->key_idx == 0, "which asks the chip to encrypt under key 0, the MTK"
          " (flags 0x%02x key %u)", p ? p->tx_flags : 0, p ? p->key_idx : 0);
    struct hwmp_prep q;
    CHECK(hw_enc_(p) && umac_mesh_hwmp_parse_prep(p->bytes + 24, (uint16_t)(p->len - 24), &q) &&
              memcmp(q.target_addr, W, 6) == 0,
          "handing it the plaintext PREP: the chip adds CCMP");
    CHECK(g_warthog_hwmp_tx_prot - s.tprot == 1 && g_warthog_hwmp_prot - s.prot == 1,
          "counted: tx prot %u, rx prot %u", g_warthog_hwmp_tx_prot - s.tprot,
          g_warthog_hwmp_prot - s.prot);
    CHECK(g_warthog_mgmt_prot_chip - s.chip == 1, "and the chip-decrypted PREQ as such (%u)",
          g_warthog_mgmt_prot_chip - s.chip);
}

static void t_rx_unicast_plain_refused(void)
{
    printf("--- (5) plaintext unicast path selection from an MFP peer is refused ---\n");
    fresh();
    keyed_(A, true);
    struct snap s = snap_();
    rx_preq_plain_(A, W, 2);
    CHECK(hwmp_(HWMP_EID_PREP, 0) == NULL, "A's plaintext PREQ draws no PREP (%u frames out)",
          simnode_outbox_count());
    CHECK(g_warthog_hwmp_unprotected - s.unprot == 1, "counted as unprotected (%u)",
          g_warthog_hwmp_unprotected - s.unprot);

    keyed_(B, false);
    simnode_outbox_clear();
    s = snap_();
    rx_preq_plain_(B, W, 3);
    const struct simnode_frame *p = hwmp_(HWMP_EID_PREP, 0);
    CHECK(p != NULL && to_(p, B) && !protected_(p) && !hw_enc_(p),
          "(pin) B, which sent no IGTK, is answered in the clear as before");
    CHECK(g_warthog_hwmp_unprotected - s.unprot == 0 && g_warthog_hwmp_tx_prot - s.tprot == 0,
          "(pin) and nothing about it is counted as protection");
}

static void t_latch_on_evidence(void)
{
    printf("--- (2) a peer that protects its path selection is treated as MFP from then on ---\n");
    fresh();
    keyed_(B, false);
    CHECK(!umac_datapath_mesh_peer_mfp(B), "(pin) B starts without MFP");
    rx_preq_chipdec_(B, W, 4, 1);
    const struct simnode_frame *p = hwmp_(HWMP_EID_PREP, 0);
    CHECK(protected_(p) && hw_enc_(p), "B's protected PREQ is answered protected");
    CHECK(umac_datapath_mesh_peer_mfp(B), "B now runs MFP");
    simnode_outbox_clear();
    rx_preq_plain_(B, W, 5);
    CHECK(hwmp_(HWMP_EID_PREP, 0) == NULL, "and B's plaintext is refused after it");
    rx_open_(B, 0x0000);
    rx_preq_plain_(B, W, 6);
    CHECK(hwmp_(HWMP_EID_PREP, 0) == NULL && umac_datapath_mesh_peer_mfp(B),
          "an Open without MFP on the keyed link does not undo it");
}

static void t_pmf_on_marks_every_keyed_peer(void)
{
    printf("--- (2) AT+MESHPMF=1: every keyed peer, as hostap's ieee80211w=2 does ---\n");
    fresh();
    g_warthog_mesh_pmf = 1;
    keyed_(B, false);
    CHECK(umac_datapath_mesh_peer_mfp(B), "B, no MFP in its Open, runs MFP to us");
    rx_preq_plain_(B, W, 7);
    CHECK(hwmp_(HWMP_EID_PREP, 0) == NULL, "its plaintext PREQ is refused");
    rx_preq_chipdec_(B, W, 8, 1);
    CHECK(protected_(hwmp_(HWMP_EID_PREP, 0)), "its protected one is answered protected");

    (void)simnode_add_peer(E);
    rx_open_(E, 0x00c0);
    simnode_outbox_clear();
    const uint32_t u0 = g_warthog_hwmp_unestab;
    rx_preq_plain_(E, W, 9);
    CHECK(!umac_datapath_mesh_peer_mfp(E), "(pin) an SAE candidate with no key yet is not MFP");
    CHECK(hwmp_(HWMP_EID_PREP, 0) == NULL && g_warthog_hwmp_unestab - u0 == 1,
          "and its PREQ is refused, not answered in the clear (8): unestab +%u",
          g_warthog_hwmp_unestab - u0);
    g_warthog_mesh_pmf = 0;
}

static void t_rx_group(void)
{
    printf("--- (6) group path selection from an MFP peer: group-addressed privacy, never an MMIE ---\n");
    fresh();
    keyed_(A, true);
    mgtk_(A, K_MGTK_A);
    (void)simnode_set_igtk(A, K_IGTK, 4, RSC5); /* A's IGTK from its AMPE: key id 4, IPN 5 */
    struct snap s = snap_();
    rx_preq_plain_(A, BC, 10);
    CHECK(hwmp_(HWMP_EID_PREP, 0) == NULL, "(pin) a group PREQ from A in the clear is refused");
    CHECK(g_warthog_hwmp_nommie - s.nommie == 1 && g_warthog_hwmp_unprotected - s.unprot == 1,
          "(pin) counted nommie %u, unprotected %u", g_warthog_hwmp_nommie - s.nommie,
          g_warthog_hwmp_unprotected - s.unprot);

    s = snap_();
    rx_preq_bip_(A, K_IGTK, 4, 6, 11, false);
    CHECK(hwmp_(HWMP_EID_PREP, 0) == NULL && g_warthog_hwmp_mmie - s.mmie == 1 &&
              g_warthog_hwmp_gp == s.gp,
          "the same PREQ with an MMIE that verifies under A's IGTK is refused, counted mmie %u: "
          "mac80211 drops it from an MFP peer (rx.c ieee80211_rx_h_decrypt)",
          g_warthog_hwmp_mmie - s.mmie);

    s = snap_();
    rx_preq_chipdec_kid_(A, BC, 12, 200, 1);
    CHECK(hwmp_(HWMP_EID_PREP, 0) == NULL && g_warthog_mgmt_gp_own - s.gown == 1 &&
              g_warthog_mgmt_prot_chip == s.chip && g_warthog_hwmp_gp == s.gp,
          "a group PREQ the chip opened, so under our own MGTK, is refused as forged in A's "
          "name (gp own %u)", g_warthog_mgmt_gp_own - s.gown);

    keyed_(B, false);
    mgtk_(B, K_MGTK_B);
    simnode_outbox_clear();
    s = snap_();
    rx_preq_bip_(B, K_IGTK, 4, 9, 22, false);
    CHECK(hwmp_(HWMP_EID_PREP, 0) == NULL && g_warthog_hwmp_mmie - s.mmie == 1,
          "(pin) an MMIE from B, which sent no IGTK, is refused although B is not MFP (mmie %u)",
          g_warthog_hwmp_mmie - s.mmie);
    simnode_outbox_clear();
    s = snap_();
    rx_preq_plain_(B, BC, 23);
    CHECK(hwmp_(HWMP_EID_PREP, 0) == NULL && g_warthog_hwmp_nommie - s.nommie == 1 &&
              g_warthog_hwmp_unprotected - s.unprot == 1,
          "B's group PREQ in the clear is refused too, although B runs no MFP: B is keyed, so it "
          "holds an MGTK and protects its own with it (nommie %u, unprotected %u)",
          g_warthog_hwmp_nommie - s.nommie, g_warthog_hwmp_unprotected - s.unprot);
    s = snap_();
    rx_preq_mmie16_(B, 24);
    CHECK(hwmp_(HWMP_EID_PREP, 0) == NULL && g_warthog_hwmp_mmie - s.mmie == 1 &&
              g_warthog_hwmp_nommie == s.nommie,
          "so is one ending in a 26-octet MMIE, counted mmie as mac80211's "
          "ieee80211_get_mmie_keyidx finds it (mmie %u, nommie %u)",
          g_warthog_hwmp_mmie - s.mmie, g_warthog_hwmp_nommie - s.nommie);

#ifdef WARTHOG_MESH_HOST_CCMP
    g_warthog_host_ccmp_on = 1;
    simnode_outbox_clear();
    s = snap_();
    rx_preq_gp_(A, K_MGTK_A, 1, 6, 30);
    const struct simnode_frame *p = hwmp_(HWMP_EID_PREP, 0);
    struct hwmp_prep q;
    CHECK(g_warthog_mgmt_prot_host - s.host == 1 && g_warthog_hwmp_gp - s.gp == 1 &&
              g_warthog_hwmp_unprotected == s.unprot,
          "a Linux-shaped group PREQ from A, Protected under A's MGTK at PN 6, is opened by the "
          "host and taken (gp %u)", g_warthog_hwmp_gp - s.gp);
    const bool clear = p != NULL;
    p = NULL;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (f->is_mgmt && to_(f, A)) { p = f; }
    }
    CHECK(!clear && open_prep_(p, &q) && memcmp(q.target_addr, W, 6) == 0 &&
              memcmp(q.orig_addr, A, 6) == 0,
          "and answered by a unicast PREP to A, CCMP under the link's MTK, which A opens");

    simnode_outbox_clear();
    s = snap_();
    rx_preq_gp_(A, K_MGTK_A, 1, 6, 31);
    CHECK(simnode_outbox_count() == 0 && g_warthog_mgmt_gp_replay - s.greplay == 1 &&
              g_warthog_hwmp_gp == s.gp,
          "the same PN again is refused as a replay (gp replay %u)", g_warthog_mgmt_gp_replay - s.greplay);
    s = snap_();
    rx_preq_gp_(A, K_MGTK_A, 1, 5, 32);
    CHECK(simnode_outbox_count() == 0 && g_warthog_mgmt_gp_replay - s.greplay == 1,
          "and PN 5, the RSC A's AMPE advertised, too");
    s = snap_();
    rx_preq_gp_(A, K_MGTK_A, 1, 7, 33);
    CHECK(simnode_outbox_count() == 1 && g_warthog_hwmp_gp - s.gp == 1,
          "PN 7 is taken: the forged PN 200 the chip opened above moved no floor");

    simnode_outbox_clear();
    s = snap_();
    rx_preq_gp_(A, K_MGTK_W, 1, 50, 34);
    CHECK(simnode_outbox_count() == 0 && g_warthog_mgmt_gp_nodec - s.gnodec == 1 &&
              g_warthog_hwmp_gp == s.gp,
          "sealed under our own MGTK and left to the host, it does not open: the host tries "
          "only the TA's own key (gp nodec %u)", g_warthog_mgmt_gp_nodec - s.gnodec);
    s = snap_();
    rx_preq_gp_(A, K_MTK, 0, 100, 35);
    CHECK(simnode_outbox_count() == 0 && g_warthog_mgmt_gp_key - s.gkey == 1 &&
              g_warthog_hwmp_gp == s.gp,
          "under key id 0, A's MTK, it opens but is refused: not A's MGTK (gp key %u)",
          g_warthog_mgmt_gp_key - s.gkey);
    rx_preq_sealed_(A, 36, 9, false);
    CHECK(open_prep_(simnode_outbox_count() ? simnode_outbox_get(0) : NULL, &q),
          "which moved no counter: A's unicast PREQ under its MTK at PN 9 is still answered");

    simnode_outbox_clear();
    s = snap_();
    rx_preq_gp_(B, K_MGTK_B, 1, 6, 40);
    CHECK(g_warthog_hwmp_gp - s.gp == 1 && simnode_outbox_count() == 1,
          "B, without MFP, is held to the same: its Protected group PREQ under its MGTK is taken");
    CHECK(!umac_datapath_mesh_peer_mfp(B), "(pin) and marks B nothing: mac80211 protects its group "
          "frames whatever its MFP");
    simnode_outbox_clear();
    rx_preq_plain_(B, W, 41);
    const struct simnode_frame *u = hwmp_(HWMP_EID_PREP, 0);
    CHECK(u != NULL && !protected_(u), "(pin) so B's unicast in the clear is still answered in the clear");

    (void)simnode_add_peer(E);
    simnode_outbox_clear();
    s = snap_();
    rx_preq_gp_(E, K_MGTK_B, 1, 6, 42);
    CHECK(simnode_outbox_count() == 0 && g_warthog_mgmt_prot_nodec - s.nodec == 1,
          "(pin) a candidate AMPE has not keyed has no MGTK here: its group PREQ does not open");
    simnode_del_peer(E);

    mgtk_(A, K_MGTK_A);
    simnode_outbox_clear();
    s = snap_();
    rx_preq_gp_(A, K_MGTK_A, 1, 7, 43);
    CHECK(simnode_outbox_count() == 0 && g_warthog_mgmt_gp_replay - s.greplay == 1,
          "the same MGTK delivered again with an older RSC keeps its management floor: PN 7 "
          "again is refused");
    g_warthog_host_ccmp_on = 0;
    s = snap_();
    rx_preq_gp_(A, K_MGTK_A, 1, 60, 44);
    CHECK(simnode_outbox_count() == 0 && g_warthog_mgmt_gp_nodec - s.gnodec == 1,
          "with host CCMP disarmed it cannot be opened: dropped, counted gp nodec");
#else
    simnode_outbox_clear();
    s = snap_();
    rx_preq_gp_(A, K_MGTK_A, 1, 6, 30);
    CHECK(simnode_outbox_count() == 0 && g_warthog_mgmt_gp_nodec - s.gnodec == 1 &&
              g_warthog_mgmt_prot_nodec - s.nodec == 1 && g_warthog_hwmp_gp == s.gp,
          "chip build: a group PREQ under A's MGTK cannot be opened (the chip holds only our "
          "own), dropped and counted gp nodec %u", g_warthog_mgmt_gp_nodec - s.gnodec);
#endif
}

static void t_tx_group_no_mgtk(void)
{
    printf("--- (3) before hostap delivers our MGTK, group path selection goes in the clear ---\n");
    fresh_(false);
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/true);
    keyed_(A, true);
    struct snap s = snap_();
    static const uint8_t pay[16] = { 1, 2, 3 };
    (void)simnode_host_tx(R, W, pay, sizeof(pay));
    const struct simnode_frame *q = hwmp_(HWMP_EID_PREQ, 0);
    CHECK(to_(q, BC) && !protected_(q) && !hw_enc_(q) &&
              !umac_mesh_bip_parse(q->bytes + 24, q->len - 24u, NULL, NULL),
          "(pin) the broadcast PREQ for an undiscovered host goes in the clear, no MMIE");
    CHECK(g_warthog_hwmp_tx_plain - s.tplain == 1 && g_warthog_hwmp_tx_gp == s.tgp,
          "counted tx plain (%u)", g_warthog_hwmp_tx_plain - s.tplain);
}

static void t_tx_group(void)
{
    printf("--- (3) group path selection goes out Protected under our own MGTK ---\n");
    fresh();
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/true);
    keyed_(A, true);
    keyed_(B, false);
    struct snap s = snap_();
    static const uint8_t pay[16] = { 1, 2, 3 };
    (void)simnode_host_tx(R, W, pay, sizeof(pay));
    const struct simnode_frame *q = hwmp_(HWMP_EID_PREQ, 0);
    CHECK(to_(q, BC) && under_own_mgtk_(q),
          "a broadcast PREQ for an undiscovered host is Protected, HW_ENC under key id %u (our "
          "MGTK), no MMIE", q != NULL ? q->key_idx : 0u);
    CHECK(preqs_for_(R) == 1u, "(pin) one frame for A (MFP) and B (not): mac80211 sends one, under one key");
    CHECK(g_warthog_hwmp_tx_gp - s.tgp == 1 && g_warthog_hwmp_tx_plain == s.tplain &&
              g_warthog_hwmp_tx_prot == s.tprot,
          "counted tx gp (%u)", g_warthog_hwmp_tx_gp - s.tgp);
#ifdef WARTHOG_MESH_MGTK_PN_BASE
    uint64_t top = 0, adv = 0;
    uint8_t rsc[6] = { 0 };
    const bool drew = simnode_group_pn_top(&top);
    const bool got = simnode_own_group_rsc(1, rsc) == MMWLAN_SUCCESS;
    for (int i = 5; i >= 0; i--) { adv = (adv << 8) | rsc[i]; }
    CHECK(drew && got && adv >= top,
          "the chip drew its PN (%llu) from our MGTK's slot, and the Key RSC AMPE advertises next "
          "(%llu) lies above it", (unsigned long long)top, (unsigned long long)adv);
#endif

    (void)simnode_set_igtk(BC, K_OWN, 4, NULL); /* AT+MESHPMF=1: hostap's own IGTK */
    simnode_advance_ms(600);
    simnode_outbox_clear();
    (void)simnode_host_tx(R2, W, pay, sizeof(pay));
    q = hwmp_(HWMP_EID_PREQ, 0);
    CHECK(under_own_mgtk_(q), "with an IGTK of ours as well it is the same: our MGTK, no MMIE");
    uint8_t rsc4[6] = { 0xff };
    CHECK(simnode_own_group_rsc(4, rsc4) == MMWLAN_SUCCESS && rsc4[0] == 0 && rsc4[1] == 0,
          "and our IGTK's RSC stays 0: nothing drew an IPN");

    simnode_del_peer(A);
    simnode_advance_ms(600);
    simnode_outbox_clear();
    static const uint8_t R3[6] = { 0x02, 0, 0, 0, 0, 0x79 };
    (void)simnode_host_tx(R3, W, pay, sizeof(pay));
    CHECK(under_own_mgtk_(hwmp_(HWMP_EID_PREQ, 0)),
          "with only B, which runs no MFP, still Protected: mac80211 does not ask");
}

static void t_relay_mode(void)
{
    printf("--- (1)(5) relay mode: the engine's PREP is protected, each frame counted once ---\n");
    fresh();
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/true);
    keyed_(A, true);
    struct snap s = snap_();
    rx_preq_plain_(A, W, 61);
    CHECK(hwmp_(HWMP_EID_PREP, 0) == NULL && g_warthog_hwmp_unprotected - s.unprot == 1,
          "A's plaintext PREQ, before A ever protected one, never reaches the engine (unprotected %u)",
          g_warthog_hwmp_unprotected - s.unprot);
    s = snap_();
    rx_preq_plain_(A, BC, 62);
    CHECK(hwmp_(HWMP_EID_PREP, 0) == NULL && g_warthog_hwmp_nommie - s.nommie == 1 &&
              g_warthog_hwmp_unprotected - s.unprot == 1,
          "nor does its group PREQ without an MMIE, counted once (nommie %u)",
          g_warthog_hwmp_nommie - s.nommie);
    s = snap_();
    rx_preq_chipdec_(A, W, 60, 1);
    const struct simnode_frame *p = hwmp_(HWMP_EID_PREP, 0);
    CHECK(to_(p, A) && protected_(p) && hw_enc_(p) && g_warthog_hwmp_prot - s.prot == 1,
          "its protected PREQ draws a protected PREP from the engine, counted once (prot %u)",
          g_warthog_hwmp_prot - s.prot);
}

static void t_igtk_api(void)
{
    printf("--- IGTK install: key ids 4 and 5 only, 16 octets, a known peer ---\n");
    fresh();
    keyed_(A, true);
    CHECK(simnode_set_igtk(A, K_IGTK, 3, NULL) == MMWLAN_INVALID_ARGUMENT &&
              simnode_set_igtk(A, K_IGTK, 6, NULL) == MMWLAN_INVALID_ARGUMENT,
          "key ids 3 and 6 are refused");
    CHECK(simnode_set_igtk(E, K_IGTK, 4, NULL) == MMWLAN_ERROR, "a stranger's IGTK is refused");
    keyed_(B, false);
    struct snap s = snap_();
    const bool took = simnode_set_igtk(B, K_IGTK, 5, NULL) == MMWLAN_SUCCESS;
    CHECK(took && umac_datapath_mesh_peer_mfp(B), "(pin) key id 5 is taken, and marks B MFP");
    rx_preq_bip_(B, K_IGTK, 5, 1, 30, false);
    CHECK(hwmp_(HWMP_EID_PREP, 0) == NULL && g_warthog_hwmp_mmie - s.mmie == 1,
          "so B's group PREQ with an MMIE under it, although it verifies, is refused (mmie %u)",
          g_warthog_hwmp_mmie - s.mmie);
}

static void t_mfp_from_igtk(void)
{
    printf("--- (7) MFP comes from the IGTK in the peer's AMPE, never from an Open ---\n");
    fresh();
    (void)simnode_add_peer(A);
    rx_open_(A, 0x00c0);
    rx_open_(A, 0x0000); /* forged in A's name: nothing checks an Open before hostap */
    for (uint8_t i = 0; i < 8u; i++)
    {
        const uint8_t other[6] = { 0x02, 0x00, 0x00, 0x00, 0x05, i };
        rx_open_(other, 0x0000);
    }
    (void)simnode_set_key(A, K_MTK, 0, /*pairwise=*/true);
    CHECK(!umac_datapath_mesh_peer_mfp(A), "keyed but no IGTK yet: A is not MFP, whatever the Opens said");
    CHECK(simnode_set_igtk(A, K_IGTK, 4, NULL) == MMWLAN_SUCCESS && umac_datapath_mesh_peer_mfp(A),
          "A's IGTK, which hostap sends only with ieee80211w != 0, marks it MFP");
    struct snap s = snap_();
    rx_preq_plain_(A, W, 90);
    CHECK(hwmp_(HWMP_EID_PREP, 0) == NULL && g_warthog_hwmp_unprotected - s.unprot == 1,
          "so a plaintext PREQ in A's name is refused, after a forged Open and eight others");
    rx_open_(A, 0x0000);
    (void)simnode_set_key(A, K_MTK, 0, /*pairwise=*/true);
    CHECK(umac_datapath_mesh_peer_mfp(A), "an Open without MFP and the MTK installed again do not undo it");

    (void)simnode_add_peer(E);
    rx_open_(E, 0x00c0);
    (void)simnode_set_key(E, K_MTK, 0, /*pairwise=*/true);
    simnode_outbox_clear();
    rx_preq_plain_(E, W, 91);
    const struct simnode_frame *p = hwmp_(HWMP_EID_PREP, 0);
    CHECK(!umac_datapath_mesh_peer_mfp(E) && p != NULL && to_(p, E) && !protected_(p),
          "an Open advertising MFP with no IGTK behind it changes nothing: E is answered in the clear");
    CHECK(simnode_set_igtk(E, K_IGTK, 3, NULL) == MMWLAN_INVALID_ARGUMENT &&
              !umac_datapath_mesh_peer_mfp(E),
          "an IGTK install refused (key id 3) marks nothing");

    keyed_(B, false);
    (void)simnode_set_igtk(B, K_IGTK, 4, NULL);
    simnode_outbox_clear();
    s = snap_();
    rx_preq_plain_(B, W, 92);
    CHECK(hwmp_(HWMP_EID_PREP, 0) == NULL && umac_datapath_mesh_peer_mfp(B),
          "B is MFP from its IGTK before it sends anything: its plaintext unicast is refused");
    rx_preq_bip_(B, K_IGTK, 4, 1, 93, false);
    CHECK(hwmp_(HWMP_EID_PREP, 0) == NULL && g_warthog_hwmp_mmie - s.mmie == 1,
          "and so is its group PREQ with an MMIE that verifies under that IGTK");
}

static void t_rx_unicast_group_key(void)
{
    printf("--- (5) protected unicast path selection must be under the link's pairwise key ---\n");
    fresh();
    keyed_(B, false);
    (void)simnode_set_key(B, K_MGTK_B, 1, /*pairwise=*/false); /* B's MGTK: every member has it */
    struct snap s = snap_();
    const uint32_t g0 = g_warthog_mgmt_prot_grpkey;
    rx_preq_chipdec_kid_(B, W, 94, 9, 1);
    CHECK(hwmp_(HWMP_EID_PREP, 0) == NULL && g_warthog_mgmt_prot_grpkey - g0 == 1 &&
              g_warthog_hwmp_prot - s.prot == 0,
          "a unicast PREQ the chip opened under key id 1, a group key, is refused (grpkey %u)",
          g_warthog_mgmt_prot_grpkey - g0);
    CHECK(!umac_datapath_mesh_peer_mfp(B), "and latches nothing: any member could have sent it");
    rx_preq_chipdec_kid_(B, W, 95, 10, 0);
    CHECK(protected_(hwmp_(HWMP_EID_PREP, 0)) && g_warthog_hwmp_prot - s.prot == 1 &&
              g_warthog_mgmt_prot_grpkey - g0 == 1,
          "the same under key id 0, the MTK, is taken");
    simnode_outbox_clear();
    s = snap_();
    const uint32_t g2 = g_warthog_mgmt_prot_grpkey;
    rx_preq_chipdec_kid_(R, W, 97, 12, 0);
    CHECK(simnode_outbox_count() == 0 && g_warthog_mgmt_prot_grpkey == g2 &&
              g_warthog_hwmp_prot == s.prot,
          "(pin) a decrypted frame from a stranger is dropped, as before, not judged on its key id");
    simnode_outbox_clear();
    s = snap_();
    const uint32_t g1 = g_warthog_mgmt_prot_grpkey;
    rx_preq_chipdec_kid_(B, BC, 96, 11, 1);
    CHECK(hwmp_(HWMP_EID_PREP, 0) == NULL && g_warthog_mgmt_prot_grpkey == g1 &&
              g_warthog_mgmt_gp_own - s.gown == 1,
          "a group frame is not judged by it: one the chip opened is refused as under our own MGTK");
}

static void t_rx_pins(void)
{
    printf("--- (5)(6) the sender is the TA ---\n");
    fresh();
    keyed_(A, true);
    keyed_(B, false);
    simnode_outbox_clear();
    struct snap s = snap_();
    {
        /* The engine answers A2; an A3 naming a peer without MFP must not pick the policy. */
        uint8_t f[128];
        hdr_(f, W, A, false);
        memcpy(f + 16, B, 6);
        uint16_t n = (uint16_t)(24 + preq_(f + 24, A, 70));
        (void)simnode_rx(f, n, -50);
    }
    CHECK(hwmp_(HWMP_EID_PREP, 0) == NULL && g_warthog_hwmp_unprotected - s.unprot == 1,
          "plaintext unicast with TA A (MFP) and A3 B (no MFP) is refused");

#ifdef WARTHOG_MESH_HOST_CCMP
    /* A group frame is opened under its TA's own MGTK: sealed under A's, sent in B's name. */
    fresh();
    g_warthog_host_ccmp_on = 1;
    keyed_(A, true);
    keyed_(B, false);
    mgtk_(A, K_MGTK_A);
    mgtk_(B, K_MGTK_B);
    simnode_outbox_clear();
    s = snap_();
    rx_preq_gp_(B, K_MGTK_A, 1, 9, 71);
    CHECK(simnode_outbox_count() == 0 && g_warthog_mgmt_prot_nodec - s.nodec == 1,
          "(pin) a group PREQ in B's name under A's MGTK does not open: B's own key is the only one tried");
    g_warthog_host_ccmp_on = 0;
#endif
}

static void t_tx_group_on_loop(void)
{
    printf("--- (3) group path selection goes out from the umac event loop; other tasks queue it ---\n");
    fresh();
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/true);
    keyed_(A, true);
    static const uint8_t pay[16] = { 4 };
    uint8_t t[6] = { 0x02, 0x00, 0x00, 0x00, 0x06, 0x00 };

    struct snap s = snap_();
    const unsigned draws0 = simnode_group_pn_draws();
    (void)simnode_host_tx_nopump(R, W, pay, sizeof(pay));
    CHECK(hwmp_(HWMP_EID_PREQ, 0) == NULL && g_warthog_hwmp_tx_gp == s.tgp &&
              simnode_group_pn_draws() == draws0 && simnode_evt_pending() == 1,
          "(pin) from the netif task the PREQ is queued for the event loop: nothing sent, no PN drawn");
    simnode_pump();
    const struct simnode_frame *q = preq_for_(R, 0);
    CHECK(under_own_mgtk_(q) && g_warthog_hwmp_tx_gp - s.tgp == 1 &&
              simnode_group_pn_draws() - draws0 == 1u,
          "the loop sends it, under our MGTK, and the chip draws its PN from that slot there");

    simnode_outbox_clear();
    for (unsigned i = 0; i < 4u; i++)
    {
        simnode_advance_ms(60);
        t[5] = (uint8_t)(0x10 + i);
        (void)simnode_host_tx_nopump(t, W, pay, sizeof(pay));
    }
    const unsigned posted = simnode_evt_pending();
    simnode_pump();
    bool order = true;
    for (unsigned i = 0; i < 4u; i++)
    {
        t[5] = (uint8_t)(0x10 + i);
        order = order && preq_for_(t, 0) == hwmp_(HWMP_EID_PREQ, i) && under_own_mgtk_(hwmp_(HWMP_EID_PREQ, i));
    }
    CHECK(posted == 1u && order, "four queued before the loop runs: one event, sent in order, each under our MGTK");

    simnode_outbox_clear();
    uint32_t d0 = g_warthog_hwmp_tx_qdrop;
    for (unsigned i = 0; i < 5u; i++)
    {
        simnode_advance_ms(60);
        t[5] = (uint8_t)(0x20 + i);
        (void)simnode_host_tx_nopump(t, W, pay, sizeof(pay));
    }
    CHECK(g_warthog_hwmp_tx_qdrop - d0 == 1, "(pin) a fifth while four wait is dropped, counted qdrop");
    simnode_pump();
    t[5] = 0x24;
    CHECK(hwmp_(HWMP_EID_PREQ, 3) != NULL && hwmp_(HWMP_EID_PREQ, 4) == NULL && preq_for_(t, 0) == NULL,
          "(pin) the four that waited go out");

    simnode_outbox_clear();
    d0 = g_warthog_hwmp_tx_qdrop;
    (void)simnode_evt_fill();
    simnode_advance_ms(60);
    t[5] = 0x30;
    (void)simnode_host_tx_nopump(t, W, pay, sizeof(pay));
    CHECK(g_warthog_hwmp_tx_qdrop - d0 == 1, "(pin) with the loop's queue full the PREQ is dropped, counted");
    simnode_pump();
    const uint8_t late[6] = { 0x02, 0x00, 0x00, 0x00, 0x06, 0x30 };
    CHECK(preq_for_(late, 0) == NULL, "(pin) and is not sent late");
    simnode_advance_ms(60);
    t[5] = 0x31;
    (void)simnode_host_tx_nopump(t, W, pay, sizeof(pay));
    unsigned again = simnode_evt_pending();
    simnode_pump();
    CHECK(again == 1u && preq_for_(t, 0) != NULL && preq_for_(late, 0) == NULL,
          "(pin) the next one posts again and goes out, alone");

    /* A mesh start forgets a post the core threw away. */
    simnode_advance_ms(60);
    t[5] = 0x32;
    (void)simnode_host_tx_nopump(t, W, pay, sizeof(pay));
    simnode_evt_discard();
    fresh();
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/true);
    keyed_(A, true);
    t[5] = 0x33;
    (void)simnode_host_tx_nopump(t, W, pay, sizeof(pay));
    again = simnode_evt_pending();
    simnode_pump();
    CHECK(again == 1u && preq_for_(t, 0) != NULL,
          "(pin) after a mesh start whose core dropped the event, it posts again");

    /* The same for the 2 s service event, which now carries the glue tick. */
    {
        void umac_mesh_service_tick(void);
        simnode_pump();
        umac_mesh_service_tick();
        simnode_evt_discard();
        fresh();
        umac_mesh_service_tick();
        const unsigned svc = simnode_evt_pending();
        simnode_pump();
        CHECK(svc == 1u, "(pin) after a mesh start whose core dropped the service event, the tick posts it again (%u)", svc);
        simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/true);
        keyed_(A, true);
    }

    /* Our IGTK (AT+MESHPMF=1) changes nothing: still queued, still our MGTK, no MMIE. */
    (void)simnode_set_igtk(BC, K_OWN, 4, NULL);
    simnode_outbox_clear();
    simnode_advance_ms(60);
    t[5] = 0x34;
    s = snap_();
    (void)simnode_host_tx_nopump(t, W, pay, sizeof(pay));
    const bool early = preq_for_(t, 0) != NULL;
    const unsigned ev_igtk = simnode_evt_pending();
    simnode_pump();
    q = preq_for_(t, 0);
    CHECK(!early && ev_igtk == 1u && under_own_mgtk_(q) && g_warthog_hwmp_tx_gp - s.tgp == 1,
          "with our IGTK stored the netif task queues the PREQ too; the loop sends it under our MGTK");

    /* Unicast is not queued: the probe task's keepalive PREQ goes out from that task. */
    int umac_mesh_hwmp_send_preq(const uint8_t *da);
    simnode_outbox_clear();
    const int kr = umac_mesh_hwmp_send_preq(A);
    const struct simnode_frame *k = preq_for_(A, 0);
    CHECK(kr >= 0 && to_(k, A) && protected_(k) && simnode_evt_pending() == 0u,
          "(pin) a unicast PREQ to MFP peer A is protected by the task that sends it, not queued");

    {
        /* Longer than any group path selection we build: dropped rather than sent from here. */
        int umac_mesh_tx_action(const uint8_t *da, const uint8_t *body, uint16_t body_len);
        uint8_t big[HWMP_PREQ_BODY_LEN + 1] = { 13, 1 };
        simnode_outbox_clear();
        d0 = g_warthog_hwmp_tx_qdrop;
        s = snap_();
        const int rc = umac_mesh_tx_action(BC, big, sizeof(big));
        const unsigned ev = simnode_evt_pending();
        simnode_pump();
        CHECK(rc < 0 && g_warthog_hwmp_tx_qdrop - d0 == 1 && ev == 0u && simnode_outbox_count() == 0 &&
                  g_warthog_hwmp_tx_gp == s.tgp,
              "(pin) a group body too long to queue is dropped, counted, never sent off the loop");
    }

#ifdef WARTHOG_MESH_HOST_CCMP
    /* Only host CCMP opens a peer's group PREQ, so only here does a relay take one to re-send. */
    fresh();
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/true);
    g_warthog_host_ccmp_on = 1;
    keyed_(B, false);
    mgtk_(B, K_MGTK_B);
    const uint32_t r0 = g_warthog_hwmp_relay_preq;
    uint8_t b[64];
    const uint16_t bl = umac_mesh_hwmp_build_preq(b, sizeof(b), B, 300, 300, R2, 5000);
    (void)simnode_evt_fill();
    rx_gp_body_(B, K_MGTK_B, 1, 6, b, bl);
    q = preq_for_(R2, 0);
    CHECK(g_warthog_hwmp_relay_preq - r0 == 1 && to_(q, BC) && under_own_mgtk_(q),
          "on the loop the relay rebroadcasts B's Protected PREQ inline under our MGTK, even with "
          "the loop's queue full");
    g_warthog_host_ccmp_on = 0;
#endif
}

/* A relayed unicast for mesh DA @p da from keyed peer @p ta under its MTK: decrypted by
 * the chip, or on the swccmp build (whose chip holds no pairwise key) sealed for the host. */
static bool rx_relayed_(const uint8_t *ta, const uint8_t *da, uint32_t seq)
{
    static uint8_t pn0 = 1;
    uint8_t f[160];
    uint16_t n = umac_mesh_ies_build_data_hdr4(f, W, ta, da, ta);
    if (n == 0) { return false; }
    f[1] |= 0x40;
    f[n++] = 0x00;
    f[n++] = 0x01; /* QoS: TID 0, Mesh Control Present */
    const uint8_t pn[6] = { 0, 0, 0, 0, 0, pn0++ };
    umac_ccmp_write_header(f + n, pn, 0);
    const uint16_t hl = (uint16_t)(n + UMAC_CCMP_HDR_LEN);
    n = hl;
    struct umac_mesh_ctrl mc = { .flags = 0, .ttl = 31, .seq = seq };
    n = (uint16_t)(n + umac_mesh_ctrl_build(f + n, 18u, &mc));
    static const uint8_t snap8[8] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00 };
    memcpy(f + n, snap8, sizeof(snap8));
    n += sizeof(snap8);
    for (int i = 0; i < 16; i++) { f[n++] = (uint8_t)(0xa0 + i); }
#ifdef WARTHOG_MESH_HOST_CCMP
    uint8_t aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
    const uint32_t al = umac_ccmp_build_aad(f, aad);
    umac_ccmp_build_nonce(f, pn, nonce);
    (void)warthog_ccm_ae(K_MTK, nonce, 8, aad, al, f + hl, (uint32_t)(n - hl), f + n);
    return simnode_rx_flags(f, (uint16_t)(n + 8u), -60, 0);
#else
    memset(f + n, 0xA5, 8); /* MIC octets the chip left in place */
    return simnode_rx_flags(f, (uint16_t)(n + 8u), -60, MMDRV_RX_FLAG_DECRYPTED);
#endif
}

extern void simnode_fail_next_alloc(void);
static void fail_tx_alloc_(void) { simnode_fail_next_alloc(); }

static void t_ladder_step_from_tick(void)
{
    printf("--- (3) a relay ladder step asked from the 2 s tick, no buffer on the loop: retried ---\n");
    extern volatile uint32_t g_warthog_fwd_preq_tx, g_warthog_fwd_hold, g_warthog_fwd_hold_drop,
        g_warthog_hwmp_tx_qfail;
    static const uint8_t Z[6] = { 0x02, 0x00, 0x00, 0x00, 0x06, 0x77 };
    fresh();
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/true);
    g_warthog_mesh_pmf = 1;
#ifdef WARTHOG_MESH_HOST_CCMP
    g_warthog_host_ccmp_on = 1;
#endif
    keyed_(A, true);
    simnode_tick();
    simnode_advance_run(10000u);
    simnode_outbox_clear();
    const uint32_t tx0 = g_warthog_fwd_preq_tx, h0 = g_warthog_fwd_hold, hd0 = g_warthog_fwd_hold_drop,
                   qf0 = g_warthog_hwmp_tx_qfail;
    const uint32_t t0 = mmosal_get_time_ms();
    CHECK(rx_relayed_(A, Z, 4242) && g_warthog_fwd_hold - h0 == 1 && preqs_for_(Z) == 1,
          "a relayed unicast for unknown Z is held, its first PREQ sent at once");
    /* The tick reaches the 400 ms step before the ladder's own timeout does. */
    simnode_advance_ms(UMAC_MESH_RELAY_DISC_FIRST_MS);
    simnode_set_tx_alloc_hook(fail_tx_alloc_);
    simnode_tick();
    simnode_set_tx_alloc_hook(NULL);
    CHECK(preqs_for_(Z) == 1 && g_warthog_fwd_preq_tx - tx0 == 1 && g_warthog_hwmp_tx_qfail == qf0,
          "the tick's PREQ for the step gets no buffer: nothing on air, nothing counted (preq_tx +%u)",
          g_warthog_fwd_preq_tx - tx0);
    simnode_advance_run(t0 + 6799u - mmosal_get_time_ms());
    const unsigned on_air = preqs_for_(Z);
    CHECK(on_air == 5u && g_warthog_fwd_preq_tx - tx0 == on_air,
          "five PREQs on air by 6.8 s, as the ladder promises, and preq_tx counts exactly those (%u, +%u)",
          on_air, g_warthog_fwd_preq_tx - tx0);
    simnode_advance_run(t0 + 7200u - mmosal_get_time_ms());
    CHECK(g_warthog_fwd_hold_drop - hd0 == 1, "then given up");
    g_warthog_mesh_pmf = 0;
}

/* A group PREQ from @p ta, in the clear, originated by @p orig, asking for @p target. */
static void rx_group_preq_(const uint8_t *ta, const uint8_t *orig, uint32_t sn, const uint8_t *target)
{
    uint8_t f[128];
    hdr_(f, BC, ta, false);
    uint16_t bl = umac_mesh_hwmp_build_preq(f + 24, 64, orig, sn, sn, target, 5000);
    (void)simnode_rx(f, (uint16_t)(24 + bl), -50);
}

/* Does the path table dump (AT+MESHPATH?) contain @p needle? */
static bool paths_have_(const char *needle)
{
    char buf[1024];
    int n = simnode_render_paths(buf, sizeof(buf) - 1u);
    if (n <= 0) { return false; }
    buf[(n < (int)sizeof(buf) - 1) ? n : (int)sizeof(buf) - 1] = '\0';
    return strstr(buf, needle) != NULL;
}

static void t_candidate_path_selection(void)
{
    printf("--- (8) path selection from a candidate AMPE has not keyed is refused ---\n");
    fresh();
    (void)simnode_add_peer(E); /* hostap's sta_add: a slot before SAE runs */
    struct snap s = snap_();
    uint32_t u0 = g_warthog_hwmp_unestab;
    rx_preq_plain_(E, BC, 100);
    CHECK(hwmp_(HWMP_EID_PREP, 0) == NULL && g_warthog_hwmp_unestab - u0 == 1 &&
              g_warthog_hwmp_nommie == s.nommie,
          "leaf: E's group PREQ for us draws no PREP, counted once, as unestab (+%u)",
          g_warthog_hwmp_unestab - u0);

    fresh();
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/true);
    g_warthog_mesh_pmf = 1;
    keyed_(A, true);
    (void)simnode_add_peer(E);
    simnode_outbox_clear();
    uint32_t r0 = g_warthog_hwmp_relay_preq;
    u0 = g_warthog_hwmp_unestab;
    rx_group_preq_(E, R2, 0x7fff0000u, R);
    CHECK(g_warthog_hwmp_relay_preq == r0 && preq_for_(R, 0) == NULL && !paths_have_("dst=000078"),
          "relay, AT+MESHPMF=1: E's PREQ in R2's name is not rebroadcast under our MGTK and "
          "installs no path to R2 (unestab +%u)", g_warthog_hwmp_unestab - u0);

    uint8_t f[128];
    struct hwmp_preq pq;
    memset(&pq, 0, sizeof(pq));
    pq.ttl = HWMP_DEFAULT_TTL;
    pq.preq_id = 1;
    memcpy(pq.orig_addr, W, 6);
    pq.orig_sn = 1;
    pq.lifetime = 5000;
    pq.target_count = 1;
    pq.target_flags = HWMP_TGT_FLAG_TO;
    memcpy(pq.target_addr, R2, 6);
    hdr_(f, W, E, false);
    uint16_t bl = umac_mesh_hwmp_build_prep(f + 24, 64, &pq, R2, 1000);
    (void)simnode_rx(f, (uint16_t)(24 + bl), -50);
    CHECK(!paths_have_("dst=000078") && g_warthog_hwmp_unestab - u0 == 2,
          "nor does its PREP install R2 through it (unestab +%u)", g_warthog_hwmp_unestab - u0);

    uint8_t body[64];
    uint32_t sn = 0;
    bl = umac_mesh_hwmp_build_preq(body, sizeof(body), R2, 0x7fff0001u, 0x7fff0001u, R, 5000);
    const uint32_t b0 = g_warthog_fwd_drop_bad;
    r0 = g_warthog_hwmp_relay_preq;
    umac_mesh_fwd_glue_hwmp_rx(body, bl, E, &sn, true, true);
    CHECK(g_warthog_fwd_drop_bad - b0 == 1 && g_warthog_hwmp_relay_preq == r0 && !paths_have_("dst=000078"),
          "the forwarding glue's own gate refuses it too, Protected as the datapath hands one up "
          "(bad +%u)", g_warthog_fwd_drop_bad - b0);
    extern volatile uint32_t g_warthog_fwd_preq_tx;
    const uint32_t p0 = g_warthog_fwd_preq_tx;
    umac_mesh_fwd_glue_hwmp_rx(body, bl, A, &sn, true, true);
    CHECK(g_warthog_hwmp_relay_preq - r0 == 1 && g_warthog_fwd_drop_bad - b0 == 1,
          "(control) the same PREQ from keyed A passes that gate and is relayed");
    simnode_pump();
    CHECK(preq_for_(R, 0) != NULL && g_warthog_fwd_preq_tx == p0,
          "relayed from another task, it goes out from the loop and is not counted as ours (preq_tx +%u)",
          g_warthog_fwd_preq_tx - p0);

    g_warthog_mesh_pmf = 0;
    (void)simnode_set_key(E, K_MTK, 0, /*pairwise=*/true);
    mgtk_(E, K_MGTK_B);
    r0 = g_warthog_hwmp_relay_preq;
    u0 = g_warthog_hwmp_unestab;
    const uint32_t n0 = g_warthog_hwmp_unprotected;
    rx_group_preq_(E, R2, 0x7fff0002u, R);
    CHECK(g_warthog_hwmp_relay_preq == r0 && g_warthog_hwmp_unestab == u0 &&
              g_warthog_hwmp_unprotected - n0 == 1,
          "once AMPE keys E, its PREQ is judged as any keyed peer's: in the clear it is refused as "
          "unprotected, not unestab");
#ifdef WARTHOG_MESH_HOST_CCMP
    g_warthog_host_ccmp_on = 1;
    bl = umac_mesh_hwmp_build_preq(body, sizeof(body), R2, 0x7fff0003u, 0x7fff0003u, R, 5000);
    rx_gp_body_(E, K_MGTK_B, 1, 6, body, bl);
    CHECK(g_warthog_hwmp_relay_preq - r0 == 1 && g_warthog_hwmp_unestab == u0,
          "and Protected under its MGTK it is relayed");
    g_warthog_host_ccmp_on = 0;
#endif
}

static void t_relay_no_launder(void)
{
    printf("--- (6) a relay takes no group path selection in the clear, so re-sends none under our MGTK ---\n");
    extern volatile uint32_t g_warthog_hwmp_relay_perr;
    static const uint8_t VICTIM[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x5a };
    static const uint8_t VICTIM2[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x5c };
    static const uint8_t TGT[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x5b };
    fresh();
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/true);
    keyed_(A, true);  /* a wizard node: MFP, opens anything under our MGTK */
    keyed_(B, false); /* a Warthog at AT+MESHPMF=0: no IGTK */
    mgtk_(A, K_MGTK_A);
    mgtk_(B, K_MGTK_B);
    struct snap s = snap_();
    uint32_t r0 = g_warthog_hwmp_relay_preq;
    rx_group_preq_(B, VICTIM, 0x7fff1000u, TGT); /* from anyone on the channel, in B's name */
    simnode_pump();
    CHECK(g_warthog_hwmp_relay_preq == r0 && preq_for_(TGT, 0) == NULL && !paths_have_("dst=00005a") &&
              g_warthog_hwmp_unprotected - s.unprot == 1,
          "a group PREQ in the clear in B's name installs no path to its originator and is not "
          "rebroadcast Protected under our MGTK, which A would open (unprotected %u)",
          g_warthog_hwmp_unprotected - s.unprot);

    /* A path to VICTIM through B, from B's PREQ as host CCMP hands a Protected one up. */
    uint8_t body[64];
    uint32_t sn = 0;
    uint16_t bl = umac_mesh_hwmp_build_preq(body, sizeof(body), VICTIM, 0x10u, 0x10u, TGT, 5000);
    umac_mesh_fwd_glue_hwmp_rx(body, bl, B, &sn, true, true);
    simnode_pump();
    CHECK(paths_have_("dst=00005a via=00000b"), "(control) B's Protected PREQ installs VICTIM via B");
    simnode_outbox_clear();
    const uint32_t p0 = g_warthog_hwmp_relay_perr;
    s = snap_();
    uint8_t f[128];
    hdr_(f, BC, B, false);
    bl = umac_mesh_hwmp_build_perr(f + 24, 64, HWMP_DEFAULT_TTL, VICTIM, 0x11u, 0);
    (void)simnode_rx(f, (uint16_t)(24 + bl), -50);
    simnode_pump();
    CHECK(g_warthog_hwmp_relay_perr == p0 && hwmp_(HWMP_EID_PERR, 0) == NULL &&
              g_warthog_hwmp_unprotected - s.unprot == 1,
          "a PERR for it in the clear in B's name is refused, not forwarded under our MGTK");

    /* The forwarding glue's own gate, for a caller that bypasses the datapath's. */
    r0 = g_warthog_hwmp_relay_preq;
    s = snap_();
    bl = umac_mesh_hwmp_build_preq(body, sizeof(body), VICTIM2, 0x20u, 0x20u, TGT, 5000);
    umac_mesh_fwd_glue_hwmp_rx(body, bl, B, &sn, false, true);
    bl = umac_mesh_hwmp_build_perr(body, sizeof(body), HWMP_DEFAULT_TTL, VICTIM, 0x12u, 0);
    umac_mesh_fwd_glue_hwmp_rx(body, bl, B, &sn, false, true);
    simnode_pump();
    CHECK(g_warthog_hwmp_relay_preq == r0 && g_warthog_hwmp_relay_perr == p0 &&
              !paths_have_("dst=00005c") && simnode_outbox_count() == 0 &&
              g_warthog_hwmp_unprotected - s.unprot == 2,
          "the forwarding glue refuses a group PREQ and PERR that arrived in the clear too "
          "(unprotected %u)", g_warthog_hwmp_unprotected - s.unprot);
}

/* Our MGTK stored, but its chip install at the first peer failed: the group slot is empty. */
static void t_tx_group_no_chip_slot(void)
{
    printf("--- (3) while our MGTK is not in the chip, group path selection goes in the clear ---\n");
    fresh();
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/true);
    simnode_fail_next_install_key();
    keyed_(A, true);
    struct snap s = snap_();
    const unsigned d0 = simnode_group_pn_draws();
    static const uint8_t pay[16] = { 5 };
    (void)simnode_host_tx(R, W, pay, sizeof(pay));
    const struct simnode_frame *q = preq_for_(R, 0);
    CHECK(to_(q, BC) && !protected_(q) && !hw_enc_(q) && simnode_group_pn_draws() == d0 &&
              g_warthog_hwmp_tx_plain - s.tplain == 1 && g_warthog_hwmp_tx_gp == s.tgp,
          "the PREQ is not handed to the chip to seal under a key it lacks: in the clear, "
          "counted tx plain (%u)", g_warthog_hwmp_tx_plain - s.tplain);
    keyed_(B, false); /* the next peer retries the install */
    simnode_advance_ms(600);
    simnode_outbox_clear();
    (void)simnode_host_tx(R2, W, pay, sizeof(pay));
    CHECK(under_own_mgtk_(preq_for_(R2, 0)), "once it is in the chip, Protected under our MGTK again");
}

/* Warthog to Warthog on one image: B, a relay, sends a group PREQ as this build does; W,
 * a Warthog peer at AT+MESHPMF=0, takes it only where host CCMP opens it. */
static void t_w2w_group(void)
{
    printf("--- (6) a Warthog relay's group PREQ, as a Warthog on the same image takes it ---\n");
    static const uint8_t pay[16] = { 6 };
    simnode_del_peer(NULL);
    (void)simnode_start_sae(B);
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/true);
    g_warthog_mesh_pmf = 0;
    g_warthog_host_ccmp_on = 0;
    (void)simnode_set_igtk(BC, NULL, 4, NULL);
    (void)simnode_set_key(BC, K_MGTK_B, 1, /*pairwise=*/false);
    (void)simnode_add_peer(W);
    (void)simnode_set_key(W, K_MTK, 0, /*pairwise=*/true);
    uint8_t rsc[6] = { 0 };
    (void)simnode_own_group_rsc(1, rsc); /* the Key RSC B's AMPE Open to W carries */
    simnode_outbox_clear();
    const unsigned d0 = simnode_group_pn_draws();
    (void)simnode_host_tx(R, B, pay, sizeof(pay));
    const struct simnode_frame *q = preq_for_(R, 0);
    uint64_t pn = 0;
    (void)simnode_group_pn_top(&pn);
    uint8_t body[120];
    const uint16_t bl = (q != NULL && q->len - 24u <= sizeof(body)) ? (uint16_t)(q->len - 24u) : 0u;
    if (bl != 0u) { memcpy(body, q->bytes + 24, bl); }
    CHECK(under_own_mgtk_(q) && simnode_group_pn_draws() - d0 == 1u && bl != 0u,
          "B sends it Protected under its MGTK, sealed by its chip (PN %llu)", (unsigned long long)pn);

    fresh();
    keyed_(B, false);
    (void)simnode_set_key_rsc(B, K_MGTK_B, 1, /*pairwise=*/false, rsc);
    struct snap s = snap_();
    rx_gp_body_(B, K_MGTK_B, 1, pn, body, bl);
#ifdef WARTHOG_MESH_HOST_CCMP
    CHECK(g_warthog_mgmt_gp_nodec - s.gnodec == 1 && g_warthog_hwmp_gp == s.gp,
          "(pin) with host CCMP off, the boot default, W cannot open it (gp nodec %u)",
          g_warthog_mgmt_gp_nodec - s.gnodec);
    g_warthog_host_ccmp_on = 1;
    s = snap_();
    rx_gp_body_(B, K_MGTK_B, 1, pn, body, bl);
    CHECK(g_warthog_hwmp_gp - s.gp == 1 && g_warthog_mgmt_gp_replay == s.greplay,
          "with host CCMP on W opens it under B's MGTK, above the RSC B's AMPE carried, and takes it");
    g_warthog_host_ccmp_on = 0;
#else
    CHECK(g_warthog_mgmt_gp_nodec - s.gnodec == 1 && g_warthog_hwmp_gp == s.gp,
          "(pin) W cannot open it: its chip holds only W's own MGTK. On this image Warthogs "
          "exchange no group path selection under SAE, whatever AT+MESHPMF (gp nodec %u)",
          g_warthog_mgmt_gp_nodec - s.gnodec);
#endif
}

static void t_open_mesh_untouched(void)
{
    printf("--- (pin) an open mesh and a keyed non-SAE mesh: no change ---\n");
    simnode_del_peer(NULL);
    (void)simnode_start(W);
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_set_igtk(BC, K_OWN, 4, NULL);
    simnode_outbox_clear();
    struct snap s = snap_();
    static const uint8_t pay[8] = { 9 };
    (void)simnode_host_tx_nopump(R, W, pay, sizeof(pay));
    CHECK(hwmp_(HWMP_EID_PREQ, 0) == NULL && simnode_evt_pending() == 1u,
          "an open mesh's broadcast PREQ from the netif task is queued for the event loop too");
    simnode_pump();
    const struct simnode_frame *q = hwmp_(HWMP_EID_PREQ, 0);
    CHECK(q != NULL && !umac_mesh_bip_parse(q->bytes + 24, q->len - 24u, NULL, NULL) && !hw_enc_(q) &&
              !protected_(q),
          "(pin) an open mesh's broadcast PREQ goes in the clear, even with an SAE mesh's MGTK and "
          "an IGTK stored");
    CHECK(g_warthog_hwmp_tx_gp == s.tgp && g_warthog_hwmp_tx_plain == s.tplain,
          "(pin) and no SAE counter moves");
    (void)simnode_set_igtk(BC, NULL, 4, NULL);
    {
        const uint32_t r0 = g_warthog_hwmp_relay_preq, u0 = g_warthog_hwmp_unestab,
                       b0 = g_warthog_fwd_drop_bad;
        rx_group_preq_(A, R2, 500, R);
        uint8_t body[64];
        uint32_t sn = 0;
        const uint16_t bl = umac_mesh_hwmp_build_preq(body, sizeof(body), R2, 501, 501, R, 5000);
        umac_mesh_fwd_glue_hwmp_rx(body, bl, A, &sn, false, true);
        CHECK(g_warthog_hwmp_relay_preq - r0 == 2 && paths_have_("dst=000078 via=00000a") &&
                  g_warthog_hwmp_unestab == u0 && g_warthog_fwd_drop_bad == b0,
              "(pin) an open-mesh relay takes path selection from peer A, which holds no key (8)");
    }

    simnode_del_peer(NULL);
    (void)simnode_start(W);
    simnode_set_gates(/*fwd=*/false, /*bridge=*/false, /*grp_std=*/false, /*secure=*/true);
    (void)simnode_add_peer(A);
    rx_open_(A, 0x00c0);
    simnode_outbox_clear();
    rx_preq_plain_(A, W, 40);
    const struct simnode_frame *p = hwmp_(HWMP_EID_PREP, 0);
    CHECK(p != NULL && !protected_(p) && !hw_enc_(p),
          "(pin) a keyed non-SAE mesh answers plaintext path selection in the clear");
    simnode_outbox_clear();
    const uint32_t g0 = g_warthog_mgmt_prot_grpkey;
    rx_preq_chipdec_kid_(A, W, 41, 1, 1);
    CHECK(hwmp_(HWMP_EID_PREP, 0) != NULL && g_warthog_mgmt_prot_grpkey == g0,
          "(pin) and takes a chip-decrypted unicast under its shared group key id, as before");
}

#ifdef WARTHOG_MESH_HOST_CCMP
static void t_host_ccmp(void)
{
    printf("--- (4) swccmp: path selection under host CCMP, on the data path's PN counter ---\n");
    fresh();
    g_warthog_host_ccmp_on = 1;
    keyed_(A, true);
    struct snap s = snap_();
    rx_preq_sealed_(A, 50, 1, false);
    CHECK(g_warthog_mgmt_prot_host - s.host == 1 && g_warthog_hwmp_prot - s.prot == 1,
          "A's sealed PREQ, which the chip could not open, is decrypted by the host");
    const struct simnode_frame *p = NULL;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (f->is_mgmt && f->bytes[0] == 0xd0 && to_(f, A)) { p = f; }
    }
    CHECK(p != NULL && protected_(p) && !hw_enc_(p), "the PREP goes Protected and host-sealed, not HW_ENC");
    uint8_t pn[6] = { 0 }, kid = 9;
    CHECK(p != NULL && umac_ccmp_parse_header(p->bytes + 24, pn, &kid) && kid == 0,
          "behind a CCMP header for key 0");
    if (p != NULL && p->len > 40)
    {
        uint8_t c[512];
        memcpy(c, p->bytes, p->len);
        uint8_t aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
        uint32_t al = umac_ccmp_build_aad(c, aad);
        umac_ccmp_build_nonce(c, pn, nonce);
        const uint32_t pl = p->len - 40u;
        bool ok = warthog_ccm_ad(K_MTK, nonce, 8, aad, al, c + 32, pl, c + 32 + pl) == 0;
        struct hwmp_prep q;
        CHECK(ok && umac_mesh_hwmp_parse_prep(c + 32, (uint16_t)pl, &q) && memcmp(q.target_addr, W, 6) == 0,
              "which A opens with the MTK to the PREP");
    }
    uint64_t hw = 0;
    for (int i = 0; i < 6; i++) { hw = (hw << 8) | pn[i]; }
    simnode_outbox_clear();
    static const uint8_t pay[8] = { 7 };
    (void)simnode_host_tx(A, W, pay, sizeof(pay));
    const struct simnode_frame *d = simnode_outbox_count() ? simnode_outbox_get(0) : NULL;
    uint8_t dpn[6] = { 0 };
    uint64_t dv = 0;
    if (d != NULL && !d->is_mgmt && d->len > 40 && umac_ccmp_parse_header(d->bytes + 32, dpn, NULL))
    {
        for (int i = 0; i < 6; i++) { dv = (dv << 8) | dpn[i]; }
    }
    CHECK(d != NULL && !hw_enc_(d) && dv != 0,
          "that data frame is host-sealed: TX buffers now carry tailroom for its MIC");
    CHECK(hw != 0 && dv == hw + 1, "the next data frame to A takes the next PN (%llu after %llu)",
          (unsigned long long)dv, (unsigned long long)hw);
    simnode_outbox_clear();
    (void)simnode_host_tx(A, W, pay, sizeof(pay));
    d = simnode_outbox_count() ? simnode_outbox_get(0) : NULL;
    uint64_t dv2 = 0;
    if (d != NULL && !d->is_mgmt && d->len > 40 && umac_ccmp_parse_header(d->bytes + 32, dpn, NULL))
    {
        for (int i = 0; i < 6; i++) { dv2 = (dv2 << 8) | dpn[i]; }
    }
    CHECK(dv != 0 && dv2 == dv + 1, "and the one after it the next again (%llu)",
          (unsigned long long)dv2);
    {
        /* A buffer with no tailroom is refused before a byte is encrypted. */
        extern volatile uint32_t g_warthog_swccmp_tx_fail;
        bool umac_mesh_tx_host_ccmp(struct umac_sta_data *stad, uint8_t key_id,
                                    const uint8_t *mac_hdr, const uint8_t *qos,
                                    const uint8_t pn[6], struct mmpktview *view);
        uint8_t buf[256], hdr[30] = { 0x88, 0x43 }, qos[2] = { 0 };
        const uint8_t tpn[6] = { 0, 0, 0, 0, 1, 0 };
        struct mmpkt *pkt = mmpkt_init_buf(buf, sizeof(buf), 16, 32, 0, NULL);
        struct mmpktview *v = mmpkt_open(pkt);
        uint8_t plain[32];
        memset(plain, 0x3c, sizeof(plain));
        mmpkt_append_data(v, plain, sizeof(plain));
        uint32_t f0 = g_warthog_swccmp_tx_fail;
        bool ok = umac_mesh_tx_host_ccmp(umac_datapath_mesh_find_peer(A), 0, hdr, qos, tpn, v);
        CHECK(!ok && g_warthog_swccmp_tx_fail - f0 == 1 &&
                  memcmp(mmpkt_get_data_start(v), plain, sizeof(plain)) == 0,
              "a frame with no room for the MIC is refused, still plaintext, and counted");
        mmpkt_close(&v);
    }

    simnode_outbox_clear();
    s = snap_();
    rx_preq_sealed_(A, 51, 2, true);
    CHECK(hwmp_(HWMP_EID_PREP, 0) == NULL && simnode_outbox_count() == 0 &&
              g_warthog_mgmt_prot_nodec - s.nodec == 1,
          "a sealed PREQ that fails its MIC is dropped as undecryptable");

    g_warthog_host_ccmp_on = 0;
    simnode_outbox_clear();
    rx_preq_chipdec_(A, W, 52, 3);
    const struct simnode_frame *c = hwmp_(HWMP_EID_PREP, 0);
    CHECK(protected_(c) && hw_enc_(c), "with host CCMP disarmed it falls back to the chip, as data does");
}
#else
static void t_chip_undecrypted(void)
{
    printf("--- (5) chip build: a Protected frame the chip did not open is dropped, counted ---\n");
    fresh();
    keyed_(A, true);
    struct snap s = snap_();
    uint8_t f[96];
    hdr_(f, W, A, true);
    memset(f + 24, 0x5a, 60);
    (void)simnode_rx(f, 84, -50);
    CHECK(simnode_outbox_count() == 0 && g_warthog_mgmt_prot_nodec - s.nodec == 1,
          "no answer, nodec %u", g_warthog_mgmt_prot_nodec - s.nodec);
}
#endif

/* ---- (9) Block Ack ------------------------------------------------------ */

extern volatile uint32_t g_warthog_swccmp_tx_fail;
extern volatile uint32_t g_warthog_mgmt_tx_chip, g_warthog_mgmt_tx_host, g_warthog_mgmt_tx_drop;

/* AT+MESHFWDSTAT?'s mgmt tx counters. */
struct mgmt_tx_ { uint32_t chip, host, drop; };
static struct mgmt_tx_ mgmt_tx_(void)
{
    return (struct mgmt_tx_){ g_warthog_mgmt_tx_chip, g_warthog_mgmt_tx_host, g_warthog_mgmt_tx_drop };
}
/* Since @p m: @p prot frames protected by this build's route (the chip's CCMP, or host CCMP
 * on swccmp), none by the other, @p drop dropped. */
static bool mgmt_tx_moved_(struct mgmt_tx_ m, uint32_t prot, uint32_t drop)
{
    const struct mgmt_tx_ n = mgmt_tx_();
#ifdef WARTHOG_MESH_HOST_CCMP
    const uint32_t ours = n.host - m.host, other = n.chip - m.chip;
#else
    const uint32_t ours = n.chip - m.chip, other = n.host - m.host;
#endif
    return ours == prot && other == 0u && n.drop - m.drop == drop;
}

/* The first Block Ack frame (category 3) with action @p act we sent to @p da: its plaintext
 * action body into @p body (a host-sealed one opened under K_MTK, as the peer would). */
static const struct simnode_frame *ba_(const uint8_t *da, uint8_t act, uint8_t body[16])
{
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        uint8_t c[128];
        if (!f->is_mgmt || f->len < 27u || f->len > sizeof(c) || f->bytes[0] != 0xd0 || !to_(f, da))
        {
            continue;
        }
        memcpy(c, f->bytes, f->len);
        uint8_t *b = c + 24;
        uint32_t bl = f->len - 24u;
        uint8_t pn[6], kid = 0;
        if (protected_(f) && !hw_enc_(f))
        {
            uint8_t aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
            if (f->len < 24u + 16u + 3u || !umac_ccmp_parse_header(c + 24, pn, &kid) || kid != 0)
            {
                continue;
            }
            const uint32_t al = umac_ccmp_build_aad(c, aad);
            umac_ccmp_build_nonce(c, pn, nonce);
            bl = f->len - 40u;
            b = c + 32;
            if (warthog_ccm_ad(K_MTK, nonce, 8, aad, al, b, bl, b + bl) != 0) { continue; }
        }
        if (b[0] == 3u && b[1] == act)
        {
            if (body != NULL) { memcpy(body, b, bl < 16u ? bl : 16u); }
            return f;
        }
    }
    return NULL;
}

/* A frame's CCMP PN, at @p off, as one number. */
static uint64_t pn_at_(const struct simnode_frame *f, unsigned off)
{
    uint8_t pn[6] = { 0 };
    uint64_t v = 0;
    if (f != NULL && f->len > off + 8u && umac_ccmp_parse_header(f->bytes + off, pn, NULL))
    {
        for (int i = 0; i < 6; i++) { v = (v << 8) | pn[i]; }
    }
    return v;
}

/* @p ta's NDP ADDBA Request for TID 0 under the MTK at PN @p pn0: chip-decrypted, or sealed
 * for host CCMP on the swccmp build. */
static void rx_addba_req_prot_(const uint8_t *ta, uint8_t pn0)
{
    uint8_t f[64];
    hdr_(f, W, ta, true);
    const uint8_t pn[6] = { 0, 0, 0, 0, 0, pn0 };
    umac_ccmp_write_header(f + 24, pn, 0);
    const uint16_t ps = (uint16_t)((1u << 1) | (16u << 6)); /* immediate, TID 0, 16 frames */
    const uint8_t req[9] = { 3, 128, 7, (uint8_t)ps, (uint8_t)(ps >> 8), 0, 0, 0, 0 };
    memcpy(f + 32, req, sizeof(req));
#ifdef WARTHOG_MESH_HOST_CCMP
    uint8_t aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
    const uint32_t al = umac_ccmp_build_aad(f, aad);
    umac_ccmp_build_nonce(f, pn, nonce);
    (void)warthog_ccm_ae(K_MTK, nonce, 8, aad, al, f + 32, sizeof(req), f + 32 + sizeof(req));
    (void)simnode_rx(f, (uint16_t)(32 + sizeof(req) + 8), -50);
#else
    memset(f + 32 + sizeof(req), 0, 8);
    (void)simnode_rx_flags(f, (uint16_t)(32 + sizeof(req) + 8), -50, MMDRV_RX_FLAG_DECRYPTED);
#endif
}

#ifdef WARTHOG_MESH_HOST_CCMP
/* Fail the TX packet allocation @p s_fail_in allocations from now. */
static unsigned s_fail_in;
static void fail_nth_tx_alloc_(void)
{
    if (s_fail_in-- == 0u) { simnode_fail_next_alloc(); }
    else                   { simnode_set_tx_alloc_hook(fail_nth_tx_alloc_); }
}
#endif

static void t_block_ack(void)
{
    printf("--- (9) Block Ack to a peer that runs MFP is protected as its path selection is ---\n");
    fresh();
#ifdef WARTHOG_MESH_HOST_CCMP
    g_warthog_host_ccmp_on = 1;
#endif
    simnode_set_ampdu(true);
    keyed_(A, true);
    keyed_(B, false);
    static const uint8_t pay[8] = { 9 };
    uint8_t body[16] = { 0 };

    simnode_outbox_clear();
    struct mgmt_tx_ m = mgmt_tx_();
    (void)simnode_host_tx(A, W, pay, sizeof(pay));
    const struct simnode_frame *q = ba_(A, 128, body);
    CHECK(protected_(q), "the ADDBA request a unicast to A starts goes out Protected");
    CHECK(mgmt_tx_moved_(m, 1u, 0u), "and counts once in AT+MESHFWDSTAT? mgmt tx (chip %u host %u drop %u)",
          g_warthog_mgmt_tx_chip - m.chip, g_warthog_mgmt_tx_host - m.host, g_warthog_mgmt_tx_drop - m.drop);
#ifdef WARTHOG_MESH_HOST_CCMP
    const struct simnode_frame *d = NULL;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        if (!simnode_outbox_get(i)->is_mgmt) { d = simnode_outbox_get(i); }
    }
    CHECK(protected_(q) && !hw_enc_(q) && body[2] != 0u,
          "host-sealed under A's MTK, key 0: A opens it to the request (token %u)", body[2]);
    CHECK(pn_at_(q, 24) != 0u && pn_at_(d, 32) == pn_at_(q, 24) + 1u,
          "on the PN counter A's data frames use (request %llu, then data %llu)",
          (unsigned long long)pn_at_(q, 24), (unsigned long long)pn_at_(d, 32));
#else
    CHECK(hw_enc_(q) && q->key_idx == 0 && q->bytes[24] == 3u && q->bytes[25] == 128u,
          "the chip encrypts it under key 0, the MTK, from the plaintext request (flags 0x%02x)",
          q != NULL ? q->tx_flags : 0u);
#endif
    simnode_outbox_clear();
    m = mgmt_tx_();
    simnode_advance_run(150);
    CHECK(protected_(ba_(A, 130, NULL)) && mgmt_tx_moved_(m, 1u, 0u),
          "A never answers: the DELBA that ends the attempt is Protected, and counted");

    simnode_outbox_clear();
    m = mgmt_tx_();
    (void)simnode_host_tx(B, W, pay, sizeof(pay));
    q = ba_(B, 128, NULL);
    CHECK(q != NULL && !protected_(q) && !hw_enc_(q) && mgmt_tx_moved_(m, 0u, 0u),
          "(pin) B, which runs no MFP, gets its ADDBA in the clear, as before, and uncounted");

    simnode_outbox_clear();
    m = mgmt_tx_();
    rx_addba_req_prot_(A, 1);
    memset(body, 0xff, sizeof(body));
    q = ba_(A, 129, body);
    CHECK(protected_(q) && body[3] == 0u && body[4] == 0u && mgmt_tx_moved_(m, 1u, 0u),
          "A's protected ADDBA request is accepted with a Protected ADDBA response, counted");

#ifdef WARTHOG_MESH_HOST_CCMP
    const uint32_t f0 = g_warthog_swccmp_tx_fail;
    m = mgmt_tx_();
    s_fail_in = 2u; /* the data frame, the request's build, then its room for CCMP */
    simnode_set_tx_alloc_hook(fail_nth_tx_alloc_);
    simnode_outbox_clear();
    (void)simnode_host_tx(A, W, pay, sizeof(pay));
    simnode_set_tx_alloc_hook(NULL);
    bool plain = false;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        plain = plain || (f->is_mgmt && !protected_(f));
    }
    CHECK(simnode_outbox_count() == 1u && !simnode_outbox_get(0)->is_mgmt && !plain &&
              g_warthog_swccmp_tx_fail - f0 == 1u && mgmt_tx_moved_(m, 0u, 1u),
          "a request that cannot be sealed is dropped and counted, never sent in the clear "
          "(%u frames, fail %u)", simnode_outbox_count(), g_warthog_swccmp_tx_fail - f0);
    simnode_advance_run(250); /* that attempt's retry expires with nothing to end */
#endif

    struct umac_sta_data *rec = umac_datapath_mesh_find_peer(A);
    simnode_outbox_clear();
    (void)simnode_host_tx(A, W, pay, sizeof(pay));
    CHECK(simnode_timeouts_holding(rec) == 1u, "another request to A: its retry is pending on A's record");
    simnode_del_peer(A);
    CHECK(simnode_timeouts_holding(rec) == 0u, "del_peer(A) leaves no Block Ack timeout on the freed record");

    simnode_del_peer(NULL);
    (void)simnode_start(W);
    simnode_set_gates(/*fwd=*/false, /*bridge=*/false, /*grp_std=*/false, /*secure=*/true);
    (void)simnode_add_peer(A);
    rx_open_(A, 0x00c0);
    simnode_outbox_clear();
    m = mgmt_tx_();
    (void)simnode_host_tx(A, W, pay, sizeof(pay));
    q = ba_(A, 128, NULL);
    CHECK(q != NULL && !protected_(q) && !hw_enc_(q) && mgmt_tx_moved_(m, 0u, 0u),
          "(pin) a keyed non-SAE mesh sends its ADDBA in the clear, as before, and uncounted");
    simnode_del_peer(NULL);
    simnode_set_ampdu(false);
}

int main(void)
{
    printf("=== simnode mfp: path selection against a peer that runs MFP ===\n");
    t_tx_group_no_mgtk(); /* first: every later start delivers our MGTK, and it is never forgotten */
    t_tx_unicast_to_mfp_peer();
    t_rx_unicast_plain_refused();
    t_latch_on_evidence();
    t_pmf_on_marks_every_keyed_peer();
    t_rx_group();
    t_tx_group();
    t_relay_mode();
    t_igtk_api();
    t_mfp_from_igtk();
    t_rx_unicast_group_key();
    t_rx_pins();
    t_tx_group_on_loop();
    t_ladder_step_from_tick();
#ifdef WARTHOG_MESH_HOST_CCMP
    t_host_ccmp();
#else
    t_chip_undecrypted();
#endif
    t_candidate_path_selection();
    t_relay_no_launder();
    t_tx_group_no_chip_slot();
    t_w2w_group();
    t_open_mesh_untouched();
    t_block_ack();

    simnode_del_peer(NULL);
    CHECK(simnode_live_allocs() == 0, "(pin) no packet buffer was orphaned (%u live)",
          simnode_live_allocs());
    simnode_stop();
    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_simnode_mfp: all passed\n");
    return 0;
}
