/*
 * A peer's MGTK in the chip at that peer's AID, as Linux installs it, and the group frames
 * the chip then opens -- through the real datapath and the fake chip's receive model
 * (simnode_rx_air), which opens a Protected frame only under a key it holds for the
 * transmitter's station (fake_chip.c says what it assumes and why).
 *
 * On air on 2026-09-30, warthog-mesh-sae-meshvif (AMPE keys in the chip, MESH chip VIF)
 * against OpenMANET 1.8.0 Pis: AT+KEYINST? showed our own MGTK at AID 0 and each peer's MTK
 * at its AID, and every Pi group frame was undecryptable (AT+RXCHAN? nodec grp, rxdrop 4):
 * its broadcast ARP and its group PREQs, so the Pi built no path to the Warthog. morse_driver
 * (mac.c morse_mac_ops_set_key) installs every key that has a station at that station's AID,
 * the peer's group key included, with its own key index; that is how the Pis' chips open each
 * other's group frames.
 *
 * Each case marked (red) failed on the tree before the change; (pin) marks behaviour that
 * must not change and passes before it too.
 *
 * test_simnode_peergtk_meshvif (-DWARTHOG_MESH_CHIP_VIF_MESH=1, as warthog-mesh-sae-meshvif):
 *  (1) (red) once AMPE delivers a peer's MGTK it goes into the chip at the peer's AID with
 *      its key id, a group key at a fresh TX PN epoch (it only receives); our own MGTK keeps
 *      AID 0; counted, and shown per slot for AT+GTKSTAT?;
 *  (2) (red) a Linux node's group data frame under its MGTK is opened by the chip and
 *      delivered as that node's, counted rx grp; the MIC octets the chip left are checked
 *      under that MGTK: until one verifies (mic ok) the check only counts, so one whose MIC
 *      the chip zeroed is still delivered (mic bad); after it, such a frame is dropped (95,
 *      micdrop) before any counter moves; replayed, or at or below the RSC its AMPE carried,
 *      it is dropped (5) with no MIC work and nothing counted rx grp or mic; another TID has
 *      its own counter; a unicast under the MTK is still delivered (pin);
 *  (3) one sealed under OUR MGTK in the node's name is not opened (4) and moves no replay
 *      floor (red: its real frames were not delivered at all); on a chip that falls back to
 *      AID 0 for a key id the station lacks, one the chip opens under a key id that is not
 *      the node's MGTK's is dropped (95), counted rx grp forged, as is one from a peer with
 *      no MGTK in the chip;
 *  (4) (red) from a slot AMPE has not keyed (its MGTK arrived before its MTK) a group frame
 *      the chip opened is dropped (95); once the MTK is in, the next is delivered;
 *  (5) (red) group path selection: the node's group PREQ under its MGTK is opened by the
 *      chip, counted mgmt gp chip and mic ok (which arms the check for management), taken
 *      (hwmp gp) and answered with a PREP; replayed, or at its RSC, it is refused (gp replay),
 *      counted neither mgmt gp chip nor mic; under our MGTK in its name it is not opened
 *      (gp nodec); opened by a fallback chip under another key id, refused (gp own);
 *  (6) (red) a peer that leaves has its MGTK disabled at its AID where mac80211 frees a
 *      station's keys (after AUTHORIZED -> ASSOCIATED, before the rest of the walk down),
 *      counted; our own stays; a new peer in the same slot opens nothing under it;
 *  (7) (red) re-delivery: a new MGTK replaces the old at the AID, so frames under the old no
 *      longer open; the same key again keeps its replay floor; a new key id disables the old;
 *  (8) (red) an install the chip refuses (or whose transport fails) is counted and the old key
 *      disabled, so nothing from that peer opens; if the disable fails too, whatever the chip
 *      still opens for that peer is dropped (95); the next delivery installs it again;
 *  (9) (red) a survivor's MGTK goes back in with its MTK when another peer leaves, and
 *      AT+REKEY re-pushes it;
 *  (10) (pin) a MESH VIF the chip refused (STA fallback) keeps the STA-VIF behaviour: no peer
 *      MGTK in the chip, a group frame the chip opened under our MGTK dropped (95);
 *  (11) (pin) our own broadcast still goes out under our own MGTK, from the AID 0 slot;
 *  (12) (red) the fence: a group frame the chip opened under a peer's OLD MGTK and still
 *      queued when that peer's new MGTK goes in is refused (95, gtk fence; a group PREQ on
 *      the management queue: gp own), whether the new key comes on a live link or after the
 *      peer left and re-peered with the same MAC;
 *      frames read after the install are taken; the same key again, or a survivor's
 *      re-install, moves no fence; with the loop run between the leave and the new keys
 *      nothing is delivered either (pin);
 *  (13) (red) the taint: a DISABLE_KEY the chip refuses when a peer leaves marks its AID;
 *      the next install there first retries it, and once it succeeds the taint is gone and
 *      a frame the chip opened under the old key before the new one went in is still refused
 *      (fence); if the retry is refused too, every group frame the chip opens for that AID is
 *      refused (95, gtk taint) until a later DISABLE there succeeds (the next install's retry,
 *      AT+REKEY here, or the peer leaving); a chip that boots clears it;
 *  (14) (red) a fence retires a minute after its install (the service tick), so 2^31 frames
 *      read since cannot make the read order wrap into a refusal;
 *  (15) (red) on a chip that, when a station's group key fails the MIC, tries our own MGTK at
 *      AID 0: while the MIC check is unarmed a member's forgery under our MGTK in A's name is
 *      taken and pushes A's replay counter (the residual); once armed it is dropped (95,
 *      micdrop) and A's next frame is still taken; the same for a group PREQ; a forgery under
 *      A's own MGTK (by another of A's peers) still verifies and is taken (the residual);
 *  (16) (red) each install of a peer's MGTK takes a TX PN epoch above every earlier one --
 *      a survivor's re-install and AT+REKEY too; AT+GTKPERSTA=2 installs at TX PN 0, as Linux;
 *  (17) (red) AT+GTKPERSTA=0 refuses at once every group frame the chip opens for a peer,
 *      then (service tick) takes every peer's MGTK out of the chip; a peer keyed while off
 *      gets none in; =1 puts them back and their frames are taken again.
 * test_simnode_peergtk (no flag, as warthog-mesh-sae): (1)-(9) and (12)-(17) pinned as they
 *  were, the peer's MGTK host-only, its group frames unopened (4), one the chip opened under
 *  our MGTK dropped (95), no DISABLE_KEY ever; (3)'s new counter moves (red).
 * test_simnode_peergtk_swccmp_meshvif (as warthog-mesh-sae-swccmp-meshvif): the same pins on
 *  the MESH VIF, with host CCMP opening the node's group frames once armed (pin).
 */
#include <stdio.h>
#include <string.h>

#include "simnode.h"
#include "mmdrv.h"
#include "mmwlan.h"
#include "mmpkt.h"
#include "common/morse_commands.h"
#include "umac/data/umac_data.h"
#include "umac/datapath/umac_datapath.h"
#include "umac/datapath/umac_datapath_private.h"
#include "umac/mesh/umac_mesh_ccm.h"
#include "umac/mesh/umac_mesh_ccmp_hdr.h"
#include "umac/mesh/umac_mesh_ctrl.h"
#include "umac/mesh/umac_mesh_hwmp.h"
#include "umac/mesh/umac_mesh_ies.h"

#ifndef WARTHOG_MESH_CHIP_VIF_MESH
#define WARTHOG_MESH_CHIP_VIF_MESH 0
#endif
/* This build puts a peer's MGTK into the chip at the peer's AID. */
#if WARTHOG_MESH_CHIP_VIF_MESH && !defined(WARTHOG_MESH_AMPE_NO_CHIP_KEY)
#define PER_STA 1
#else
#define PER_STA 0
#endif

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

extern volatile uint32_t g_warthog_peer_gtk_inst, g_warthog_peer_gtk_fail;
extern volatile uint32_t g_warthog_peer_gtk_del, g_warthog_peer_gtk_delfail;
extern volatile uint32_t g_warthog_peer_gtk[4], g_warthog_peer_gtk_mac[4];
extern volatile uint32_t g_warthog_rx_grp_chip, g_warthog_mgmt_gp_chip, g_warthog_rx_grp_forged;
extern volatile uint32_t g_warthog_rx_grp_mic_ok, g_warthog_rx_grp_mic_bad;
extern volatile uint32_t g_warthog_rxdrop_reason, g_warthog_nodec_group_n;
extern volatile uint32_t g_warthog_mgmt_gp_nodec, g_warthog_mgmt_gp_own, g_warthog_mgmt_gp_key,
    g_warthog_mgmt_gp_replay, g_warthog_hwmp_gp, g_warthog_mgmt_prot_host;
extern volatile uint32_t g_warthog_host_ccmp_on, g_warthog_swccmp_ok;
extern volatile uint32_t g_warthog_rekey_req, g_warthog_chipvif_type;
extern volatile uint32_t g_warthog_rx_grp_mic_armed, g_warthog_rx_grp_micdrop, g_warthog_mgmt_gp_micdrop;
extern volatile uint32_t g_warthog_peer_gtk_mode, g_warthog_peer_gtk_fence, g_warthog_peer_gtk_taint;
extern volatile uint32_t g_warthog_peer_gtk_tainted, g_warthog_rx_read_seq;
void umac_datapath_mesh_service_rekey(void);
void umac_datapath_mesh_service_peer_gtk(void);

static const uint8_t W[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 }; /* us */
static const uint8_t A[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0a }; /* a Linux node */
static const uint8_t C[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0c };
static const uint8_t D[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0d };
static const uint8_t E[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0e };
static const uint8_t BC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static const uint8_t K_OWN[16]     = { 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
                                       0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f };
static const uint8_t K_A_MTK[16]   = { 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
                                       0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf };
static const uint8_t K_A_MGTK[16]  = { 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
                                       0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf };
#if PER_STA
static const uint8_t K_A_MGTK2[16] = { 0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
                                       0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef };
static const uint8_t K_A_MGTK3[16] = { 0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
                                       0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff };
static const uint8_t K_E_MTK[16]   = { 0x0e, 0x1e, 0x2e, 0x3e, 0x4e, 0x5e, 0x6e, 0x7e,
                                       0x8e, 0x9e, 0xae, 0xbe, 0xce, 0xde, 0xee, 0xfe };
static const uint8_t K_A_MTK2[16]  = { 0x1b, 0x2b, 0x3b, 0x4b, 0x5b, 0x6b, 0x7b, 0x8b,
                                       0x9b, 0xab, 0xbb, 0xcb, 0xdb, 0xeb, 0xfb, 0x0b };
#endif
static const uint8_t K_C_MTK[16]   = { 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
                                       0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf };
static const uint8_t K_C_MGTK[16]  = { 0x3c, 0x4c, 0x5c, 0x6c, 0x7c, 0x8c, 0x9c, 0xac,
                                       0xbc, 0xcc, 0xdc, 0xec, 0xfc, 0x0c, 0x1c, 0x2c };
static const uint8_t K_D_MTK[16]   = { 0x0d, 0x1d, 0x2d, 0x3d, 0x4d, 0x5d, 0x6d, 0x7d,
                                       0x8d, 0x9d, 0xad, 0xbd, 0xcd, 0xdd, 0xed, 0xfd };
static const uint8_t K_E_MGTK[16]  = { 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8,
                                       0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, 0xe0 };
static const uint8_t RSC5[6] = { 5, 0, 0, 0, 0, 0 };
static const uint8_t PAY[8] = { 0xc0, 0xff, 0xee, 0x11, 0x22, 0x33, 0x44, 0x55 };
static const uint8_t SNAP[8] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00 };

/* ---- setup -------------------------------------------------------------- */

/* A mesh start with our own MGTK (key id @p own_id) delivered first, as hostap does; the
 * chip refusals armed are kept with @p keep_refusals, else dropped. */
static void start_(uint8_t own_id, bool keep_refusals)
{
    simnode_del_peer(NULL);
    simnode_chip_group_fallback(false);
    simnode_chip_group_fallback_mic(false);
    g_warthog_rx_grp_mic_armed = 0;  /* sticky until reboot on the firmware */
    g_warthog_peer_gtk_mode = 1;     /* AT+GTKPERSTA's default */
    if (!keep_refusals)
    {
        simnode_chip_refusals_clear();
    }
    (void)simnode_start_sae(W);
    simnode_set_gates(/*fwd=*/false, /*bridge=*/false, /*grp_std=*/false, /*secure=*/true);
    g_warthog_host_ccmp_on = 0;
    (void)simnode_set_key(BC, K_OWN, own_id, /*pairwise=*/false);
    simnode_outbox_clear();
    simnode_host_rx_clear();
    simnode_keyinst_clear();
    simnode_chipcmd_clear();
}

static void fresh_(uint8_t own_id) { start_(own_id, false); }

/* A peer as AMPE leaves it: its MTK, then (unless NULL) its MGTK, key id 1, RSC 5. */
static void keyed_(const uint8_t *p, const uint8_t mtk[16], const uint8_t mgtk[16])
{
    (void)simnode_add_peer(p);
    (void)simnode_set_key(p, mtk, 0, /*pairwise=*/true);
    if (mgtk != NULL)
    {
        (void)simnode_set_key_rsc(p, mgtk, 1, /*pairwise=*/false, RSC5);
    }
}

static uint16_t aid_(const uint8_t *p)
{
    struct umac_sta_data *s = umac_datapath_mesh_find_peer(p);
    return s != NULL ? umac_sta_data_get_aid(s) : 0u;
}

/* The INSTALL_KEY that carried @p key as a group key, or NULL. */
static const struct simnode_keyinst *grp_install_(const uint8_t key[16])
{
    for (unsigned i = 0; i < simnode_keyinst_count(); i++)
    {
        const struct simnode_keyinst *k = simnode_keyinst_get(i);
        if (k != NULL && !k->pairwise && memcmp(k->key, key, 16) == 0) { return k; }
    }
    return NULL;
}

static bool held_(uint16_t aid, uint8_t idx, const uint8_t key[16])
{
    uint8_t k[16];
    return simnode_chip_key_held(aid, false, idx, k) && memcmp(k, key, 16) == 0;
}

/* Index in the chip log of the first command @p id at @p aid (with @p arg unless UINT32_MAX),
 * or -1. */
static int cmd_at_arg_(uint16_t id, uint16_t aid, uint32_t arg)
{
    for (unsigned i = 0; i < simnode_chipcmd_count(); i++)
    {
        const struct simnode_chipcmd *c = simnode_chipcmd_get(i);
        if (c->id == id && c->aid == aid && (arg == UINT32_MAX || c->arg == arg)) { return (int)i; }
    }
    return -1;
}

static int cmd_at_(uint16_t id, uint16_t aid) { return cmd_at_arg_(id, aid, UINT32_MAX); }

/* ---- frames a peer sends, sealed as its chip seals them ------------------- */

static void pn6_(uint8_t out[6], uint64_t pn)
{
    for (int i = 0; i < 6; i++) { out[i] = (uint8_t)(pn >> (8 * (5 - i))); }
}

static void seal_(uint8_t *f, uint16_t n, uint16_t body, const uint8_t key[16], const uint8_t pn[6])
{
    uint8_t aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
    const uint32_t al = umac_ccmp_build_aad(f, aad);
    umac_ccmp_build_nonce(f, pn, nonce);
    (void)warthog_ccm_ae(key, nonce, 8, aad, al, f + body, (size_t)(n - body - 8u), f + n - 8u);
}

/* A Linux node's group data frame from @p ta (3-address QoS data, Mesh Control, IPv4) on
 * @p tid, CCMP under @p key with key id @p kid at @p pn, as it goes on the air. */
static uint16_t grp_(uint8_t *f, const uint8_t *ta, const uint8_t key[16], uint8_t kid,
                     uint64_t pn, uint8_t tid)
{
    static uint32_t mseq = 100;
    uint8_t p6[6];
    pn6_(p6, pn);
    uint16_t n = umac_mesh_ies_build_data_hdr3_group(f, BC, ta, ta);
    f[1] |= 0x40u;
    f[n++] = tid;
    f[n++] = 0x01; /* Mesh Control present */
    umac_ccmp_write_header(&f[n], p6, kid);
    n = (uint16_t)(n + UMAC_CCMP_HDR_LEN);
    const uint16_t body = n;
    struct umac_mesh_ctrl mc = { .flags = 0, .ttl = 31, .seq = ++mseq };
    n = (uint16_t)(n + umac_mesh_ctrl_build(&f[n], 18u, &mc));
    memcpy(&f[n], SNAP, sizeof(SNAP));
    n = (uint16_t)(n + sizeof(SNAP));
    memcpy(&f[n], PAY, sizeof(PAY));
    n = (uint16_t)(n + sizeof(PAY) + 8u);
    seal_(f, n, body, key, p6);
    return n;
}

/* Its unicast data frame to us (4-address) under @p key, key id @p kid, at @p pn. */
static uint16_t uni_(uint8_t *f, const uint8_t *ta, const uint8_t key[16], uint8_t kid, uint64_t pn)
{
    static uint32_t mseq = 9000;
    uint8_t p6[6];
    pn6_(p6, pn);
    uint16_t n = umac_mesh_ies_build_data_hdr4(f, W, ta, W, ta);
    f[1] |= 0x40u;
    f[n++] = 0x00;
    f[n++] = 0x01;
    umac_ccmp_write_header(&f[n], p6, kid);
    n = (uint16_t)(n + UMAC_CCMP_HDR_LEN);
    const uint16_t body = n;
    struct umac_mesh_ctrl mc = { .flags = 0, .ttl = 31, .seq = ++mseq };
    n = (uint16_t)(n + umac_mesh_ctrl_build(&f[n], 18u, &mc));
    memcpy(&f[n], SNAP, sizeof(SNAP));
    n = (uint16_t)(n + sizeof(SNAP));
    memcpy(&f[n], PAY, sizeof(PAY));
    n = (uint16_t)(n + sizeof(PAY) + 8u);
    seal_(f, n, body, key, p6);
    return n;
}

/* @p f as a chip that opened it under @p key hands it up, its MIC octets zeroed: true if
 * it opened. */
static bool chip_opened_zero_mic_(uint8_t *f, uint16_t n, const uint8_t key[16])
{
    uint8_t pn[6], kid = 0, aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
    const uint32_t hl = umac_ccmp_hdr_len(f);
    if (!umac_ccmp_parse_header(f + hl, pn, &kid)) { return false; }
    const uint32_t al = umac_ccmp_build_aad(f, aad);
    umac_ccmp_build_nonce(f, pn, nonce);
    const uint32_t body = hl + UMAC_CCMP_HDR_LEN;
    if (warthog_ccm_ad(key, nonce, 8, aad, al, f + body, n - body - 8u, f + n - 8u) != 0) { return false; }
    memset(f + n - 8u, 0, 8);
    return true;
}

struct rx_out { unsigned delivered; uint32_t reason; bool opened; bool from_ta; };

/* @p f off the air through the chip: what reached the host and why not. */
static struct rx_out air_(const uint8_t *f, uint16_t n)
{
    simnode_host_rx_clear();
    g_warthog_rxdrop_reason = 0;
    const unsigned o0 = simnode_chip_rx_opened();
    (void)simnode_rx_air(f, n, -60);
    struct rx_out r = { simnode_host_rx_count(), g_warthog_rxdrop_reason,
                        simnode_chip_rx_opened() != o0, false };
    const struct simnode_hostrx *h = simnode_host_rx_get(0);
    r.from_ta = h != NULL && memcmp(h->sa, f + 10, 6) == 0;
    return r;
}

/* A Linux node's group PREQ for us: group-addressed privacy, CCMP under @p key, in @p f. */
static uint16_t preq_(uint8_t *f, const uint8_t *ta, const uint8_t key[16], uint8_t kid,
                      uint64_t pn, uint32_t sn)
{
    memset(f, 0, 24);
    f[0] = 0xd0;
    f[1] = 0x40;
    memcpy(f + 4, BC, 6);
    memcpy(f + 10, ta, 6);
    memcpy(f + 16, ta, 6);
    uint8_t p6[6];
    pn6_(p6, pn);
    umac_ccmp_write_header(f + 24, p6, kid);
    const uint16_t bl = umac_mesh_hwmp_build_preq(f + 32, 64, ta, sn, sn, W, 5000);
    const uint16_t n = (uint16_t)(32u + bl + 8u);
    seal_(f, n, 32, key, p6);
    return n;
}

/* ...sent off the air through the chip. */
static void preq_air_(const uint8_t *ta, const uint8_t key[16], uint8_t kid, uint64_t pn,
                      uint32_t sn)
{
    uint8_t f[160];
    const uint16_t n = preq_(f, ta, key, kid, pn, sn);
    (void)simnode_rx_air(f, n, -50);
}

/* A PREP we sent to @p da. */
static bool prep_to_(const uint8_t *da)
{
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (f->is_mgmt && f->len > 27u && f->bytes[0] == 0xd0 && memcmp(f->bytes + 4, da, 6) == 0 &&
            f->bytes[24] == 13 && f->bytes[26] == HWMP_EID_PREP)
        {
            return true;
        }
    }
    return false;
}

/* ---- cases -------------------------------------------------------------- */

static void t_install(void)
{
    printf("--- (1) a peer's MGTK goes into the chip at the peer's AID, as Linux installs it ---\n");
    fresh_(1);
    (void)simnode_add_peer(A);
    CHECK(held_(0, 1, K_OWN), "(pin) our own MGTK is in the chip at AID 0 under key id 1");
    const uint32_t i0 = g_warthog_peer_gtk_inst;
    (void)simnode_set_key(A, K_A_MTK, 0, true);
    const int st = simnode_set_key_rsc(A, K_A_MGTK, 1, false, RSC5);
    const uint16_t aid = aid_(A);
    const struct simnode_keyinst *k = grp_install_(K_A_MGTK);
    CHECK(st == (int)MMWLAN_SUCCESS, "(pin) set_key for A's MGTK succeeds (%d)", st);
#if PER_STA
    CHECK(aid != 0u && held_(aid, 1, K_A_MGTK),
          "(red) A's MGTK is in the chip at A's AID %u, a group key under key id 1", aid);
    CHECK(k != NULL && k->aid == aid && k->key_idx == 1u && k->tx_pn >= (1ull << 20) &&
              (k->tx_pn & 0xfffffull) == 0u,
          "(red) by one INSTALL_KEY: group, AID %u, key id 1, at a fresh TX PN epoch (0x%llx)", aid,
          k != NULL ? (unsigned long long)k->tx_pn : 0ull);
    CHECK(g_warthog_peer_gtk_inst - i0 == 1u, "(red) counted gtk inst (%lu)",
          (unsigned long)(g_warthog_peer_gtk_inst - i0));
    const uint32_t want = 0x80000000u | ((uint32_t)aid << 16) | (1u << 8) | 1u;
    CHECK(g_warthog_peer_gtk[aid - 1u] == want && g_warthog_peer_gtk_mac[aid - 1u] == 0x00000au,
          "(red) AT+GTKSTAT?'s slot reads A, AID %u, key id 1, chip index 1 (0x%08lx, mac %06lx)",
          aid, (unsigned long)g_warthog_peer_gtk[aid - 1u],
          (unsigned long)g_warthog_peer_gtk_mac[aid - 1u]);
#else
    CHECK(k == NULL && !held_(aid, 1, K_A_MGTK), "(pin) A's MGTK stays host-only");
    CHECK(g_warthog_peer_gtk_inst == i0 && g_warthog_peer_gtk[0] == 0u, "(pin) nothing counted");
#endif
    CHECK(held_(0, 1, K_OWN), "(pin) and our own MGTK keeps AID 0");
}

static void t_group_data(void)
{
    printf("--- (2) a Linux node's group data under its MGTK ---\n");
    fresh_(1);
    keyed_(A, K_A_MTK, K_A_MGTK);
    uint8_t f[200];
    const uint32_t c0 = g_warthog_rx_grp_chip, g0 = g_warthog_nodec_group_n;
    const uint32_t mok0 = g_warthog_rx_grp_mic_ok, mbad0 = g_warthog_rx_grp_mic_bad;
    uint16_t n = grp_(f, A, K_A_MGTK, 1, 10, 0);
    struct rx_out r;
#if PER_STA
    /* A chip that does not leave the MIC octets: until one verifies, the check only counts. */
    n = grp_(f, A, K_A_MGTK, 1, 8, 0);
    simnode_host_rx_clear();
    CHECK(chip_opened_zero_mic_(f, n, K_A_MGTK) &&
              simnode_rx_flags(f, n, -60, MMDRV_RX_FLAG_DECRYPTED) && simnode_host_rx_count() == 1u &&
              g_warthog_rx_grp_mic_bad - mbad0 == 1u && g_warthog_rx_grp_mic_armed == 0u,
          "(red) unarmed: one the chip opened with its MIC octets zeroed is delivered, counted mic bad");
    n = grp_(f, A, K_A_MGTK, 1, 10, 0);
    r = air_(f, n);
    CHECK(r.opened && r.delivered == 1u && r.from_ta,
          "(red) the chip opens it under A's MGTK at A's AID; delivered as A's (%u, reason %lu)",
          r.delivered, (unsigned long)r.reason);
    CHECK(g_warthog_rx_grp_chip - c0 == 2u && g_warthog_nodec_group_n == g0,
          "(red) counted rx grp chip, not nodec grp");
    CHECK(g_warthog_rx_grp_mic_ok - mok0 == 1u && g_warthog_rx_grp_mic_bad - mbad0 == 1u &&
              (g_warthog_rx_grp_mic_armed & 1u) != 0u,
          "(red) the MIC octets the chip left verify under A's MGTK: mic ok, which arms the check "
          "for data (armed %lu)", (unsigned long)g_warthog_rx_grp_mic_armed);
    const uint32_t c1 = g_warthog_rx_grp_chip, mok1 = g_warthog_rx_grp_mic_ok;
    r = air_(f, n);
    CHECK(r.delivered == 0u && r.reason == 5u, "(red) the same frame again is a replay (5, got %lu)",
          (unsigned long)r.reason);
    CHECK(g_warthog_rx_grp_chip == c1 && g_warthog_rx_grp_mic_ok == mok1,
          "(red) a replay is not counted rx grp chip and costs no MIC check (rx grp %lu, mic ok %lu)",
          (unsigned long)(g_warthog_rx_grp_chip - c1), (unsigned long)(g_warthog_rx_grp_mic_ok - mok1));
    n = grp_(f, A, K_A_MGTK, 1, 5, 0);
    r = air_(f, n);
    CHECK(r.opened && r.delivered == 0u && r.reason == 5u,
          "(red) PN 5, the RSC A's AMPE carried, is a replay too (reason %lu)", (unsigned long)r.reason);
    n = grp_(f, A, K_A_MGTK, 1, 9, 6);
    r = air_(f, n);
    CHECK(r.delivered == 1u, "(red) PN 9 on TID 6 is taken: each TID has its own counter (reason %lu)",
          (unsigned long)r.reason);
    n = grp_(f, A, K_A_MGTK, 1, 11, 0);
    r = air_(f, n);
    CHECK(r.delivered == 1u, "(red) and PN 11 on TID 0");
    /* Armed: a frame whose MIC octets do not verify is dropped before any counter moves. */
    n = grp_(f, A, K_A_MGTK, 1, 12, 0);
    const uint32_t mbad1 = g_warthog_rx_grp_mic_bad, md0 = g_warthog_rx_grp_micdrop;
    simnode_host_rx_clear();
    g_warthog_rxdrop_reason = 0;
    CHECK(chip_opened_zero_mic_(f, n, K_A_MGTK) &&
              simnode_rx_flags(f, n, -60, MMDRV_RX_FLAG_DECRYPTED) && simnode_host_rx_count() == 0u &&
              g_warthog_rxdrop_reason == 95u && g_warthog_rx_grp_mic_bad - mbad1 == 1u &&
              g_warthog_rx_grp_micdrop - md0 == 1u,
          "(red) armed: one with its MIC octets zeroed is dropped, 95, counted mic bad and micdrop "
          "(reason %lu)", (unsigned long)g_warthog_rxdrop_reason);
    n = grp_(f, A, K_A_MGTK, 1, 12, 0);
    r = air_(f, n);
    CHECK(r.delivered == 1u, "(red) and A's real PN 12 is still taken: the dropped one moved no counter");
#else
    r = air_(f, n);
    CHECK(!r.opened && r.delivered == 0u && r.reason == 4u && g_warthog_nodec_group_n - g0 == 1u,
          "(pin) the chip cannot open it: nodec grp, rxdrop 4 (reason %lu)", (unsigned long)r.reason);
    CHECK(g_warthog_rx_grp_chip == c0 && g_warthog_rx_grp_mic_ok == mok0 &&
              g_warthog_rx_grp_mic_bad == mbad0, "(pin) rx grp chip and the MIC check do not move");
#ifdef WARTHOG_MESH_HOST_CCMP
    g_warthog_host_ccmp_on = 1;
    const uint32_t ok0 = g_warthog_swccmp_ok;
    n = grp_(f, A, K_A_MGTK, 1, 11, 0);
    r = air_(f, n);
    CHECK(r.delivered == 1u && g_warthog_swccmp_ok - ok0 == 1u,
          "(pin) host CCMP armed opens it instead and it is delivered");
#endif
#endif
#ifndef WARTHOG_MESH_AMPE_NO_CHIP_KEY
    n = uni_(f, A, K_A_MTK, 0, 20);
    r = air_(f, n);
    CHECK(r.opened && r.delivered == 1u, "(pin) A's unicast under its MTK: opened by the chip, delivered");
#endif
}

static void t_forged(void)
{
    printf("--- (3) a group frame sealed under a key that is not the sender's MGTK ---\n");
    fresh_(1);
    keyed_(A, K_A_MTK, K_A_MGTK);
    uint8_t f[200];
    uint32_t x0 = g_warthog_rx_grp_forged;
    /* C peers with us and so holds our MGTK (key id 1, as A's); it seals in A's name. */
    uint16_t n = grp_(f, A, K_OWN, 1, 0x500000u, 0);
    struct rx_out r = air_(f, n);
#if PER_STA
    CHECK(!r.opened && r.delivered == 0u && r.reason == 4u,
          "(pin) under our MGTK in A's name: the chip tries A's key only and cannot open it (%lu)",
          (unsigned long)r.reason);
    n = grp_(f, A, K_A_MGTK, 1, 12, 0);
    r = air_(f, n);
    CHECK(r.delivered == 1u, "(red) and A's own at PN 12 is still taken: the forged PN moved nothing");
    simnode_chip_group_fallback(true);
    n = grp_(f, A, K_OWN, 1, 0x500001u, 0);
    r = air_(f, n);
    CHECK(!r.opened && r.reason == 4u,
          "(red) a chip that falls back to AID 0 does not, while A's AID holds key id 1");
#elif WARTHOG_MESH_CHIP_VIF_MESH
    CHECK(!r.opened && r.reason == 4u, "(pin) on a MESH VIF with no peer key the chip opens nothing (%lu)",
          (unsigned long)r.reason);
#else
    CHECK(r.opened && r.delivered == 0u && r.reason == 95u,
          "(pin) a STA VIF opens it under our MGTK at AID 0: dropped as forged, 95 (%lu)",
          (unsigned long)r.reason);
    CHECK(g_warthog_rx_grp_forged - x0 == 1u, "(red) counted rx grp forged");
#endif

    /* Our MGTK under key id 2, A's under 1: a fallback chip opens one under 2 in A's name. */
    fresh_(2);
    keyed_(A, K_A_MTK, K_A_MGTK);
    keyed_(C, K_C_MTK, NULL);
    simnode_chip_group_fallback(true);
    x0 = g_warthog_rx_grp_forged;
    n = grp_(f, A, K_OWN, 2, 0x600000u, 0);
    r = air_(f, n);
    CHECK(r.opened && r.delivered == 0u && r.reason == 95u,
          "the fallback chip opens one under key id 2 in A's name; not A's MGTK's id: 95 (%lu)",
          (unsigned long)r.reason);
    CHECK(g_warthog_rx_grp_forged - x0 == 1u, "(red) counted rx grp forged (%lu)",
          (unsigned long)(g_warthog_rx_grp_forged - x0));
    n = grp_(f, C, K_OWN, 2, 0x600001u, 0);
    r = air_(f, n);
    CHECK(r.opened && r.delivered == 0u && r.reason == 95u,
          "and one in C's name, C having no MGTK in the chip: 95 (%lu)", (unsigned long)r.reason);
    simnode_chip_group_fallback(false);
}

static void t_unkeyed_slot(void)
{
    printf("--- (4) a slot AMPE has not keyed: its MGTK arrived before its MTK ---\n");
    fresh_(1);
    (void)simnode_add_peer(E);
    (void)simnode_set_key_rsc(E, K_E_MGTK, 1, false, RSC5);
    uint8_t f[200];
    uint16_t n = grp_(f, E, K_E_MGTK, 1, 10, 0);
    struct rx_out r = air_(f, n);
#if PER_STA
    CHECK(r.opened && r.delivered == 0u && r.reason == 95u,
          "(red) the chip opens E's group frame but E is not keyed: dropped, 95 (%lu)",
          (unsigned long)r.reason);
    (void)simnode_set_key(E, K_E_MTK, 0, true);
    n = grp_(f, E, K_E_MGTK, 1, 11, 0);
    r = air_(f, n);
    CHECK(r.delivered == 1u, "(red) once E's MTK is in, its next one is delivered (%lu)",
          (unsigned long)r.reason);
#else
    CHECK(!r.opened && r.delivered == 0u && r.reason == 4u, "(pin) not opened: 4 (%lu)",
          (unsigned long)r.reason);
#endif
}

static void t_group_path_selection(void)
{
    printf("--- (5) a Linux node's group PREQ (group-addressed privacy) under its MGTK ---\n");
    fresh_(1);
    keyed_(A, K_A_MTK, K_A_MGTK);
    uint32_t chip0 = g_warthog_mgmt_gp_chip, gp0 = g_warthog_hwmp_gp, nodec0 = g_warthog_mgmt_gp_nodec;
    const uint32_t mok0 = g_warthog_rx_grp_mic_ok;
    uint32_t rep0 = g_warthog_mgmt_gp_replay, own0 = g_warthog_mgmt_gp_own;
    preq_air_(A, K_A_MGTK, 1, 6, 30);
#if PER_STA
    CHECK(g_warthog_mgmt_gp_chip - chip0 == 1u && g_warthog_hwmp_gp - gp0 == 1u,
          "(red) opened by the chip under A's MGTK (mgmt gp chip %lu) and taken (hwmp gp %lu)",
          (unsigned long)(g_warthog_mgmt_gp_chip - chip0), (unsigned long)(g_warthog_hwmp_gp - gp0));
    CHECK(prep_to_(A), "(red) and answered with a PREP to A, so A gets its path to us");
    CHECK(g_warthog_rx_grp_mic_ok - mok0 == 1u && (g_warthog_rx_grp_mic_armed & 2u) != 0u,
          "(red) its MIC octets verify under A's MGTK: mic ok, which arms the check for management");
    simnode_outbox_clear();
    chip0 = g_warthog_mgmt_gp_chip;
    const uint32_t mok1 = g_warthog_rx_grp_mic_ok;
    preq_air_(A, K_A_MGTK, 1, 6, 31);
    CHECK(g_warthog_mgmt_gp_replay - rep0 == 1u && !prep_to_(A),
          "(red) the same PN again: refused as a replay (gp replay)");
    CHECK(g_warthog_mgmt_gp_chip == chip0 && g_warthog_rx_grp_mic_ok == mok1,
          "(red) and counted neither mgmt gp chip nor mic ok");
    preq_air_(A, K_A_MGTK, 1, 5, 32);
    CHECK(g_warthog_mgmt_gp_replay - rep0 == 2u && !prep_to_(A),
          "(red) PN 5, A's RSC: refused too");
    preq_air_(A, K_OWN, 1, 50, 33);
    CHECK(g_warthog_mgmt_gp_nodec - nodec0 == 1u && !prep_to_(A),
          "(red) under our MGTK in A's name the chip cannot open it (gp nodec)");
    preq_air_(A, K_A_MGTK, 1, 7, 34);
    CHECK(prep_to_(A), "(red) PN 7 is taken: nothing refused moved the counter");

    fresh_(2);
    keyed_(A, K_A_MTK, K_A_MGTK);
    simnode_chip_group_fallback(true);
    own0 = g_warthog_mgmt_gp_own;
    gp0 = g_warthog_hwmp_gp;
    preq_air_(A, K_OWN, 2, 60, 40);
    CHECK(g_warthog_mgmt_gp_own - own0 == 1u && g_warthog_hwmp_gp == gp0 && !prep_to_(A),
          "a fallback chip opens one under key id 2 (ours) in A's name: refused (gp own)");
    simnode_chip_group_fallback(false);
#else
    CHECK(g_warthog_mgmt_gp_chip == chip0 && g_warthog_mgmt_gp_nodec - nodec0 == 1u &&
              g_warthog_hwmp_gp == gp0 && !prep_to_(A) && g_warthog_rx_grp_mic_ok == mok0,
          "(pin) the chip cannot open it: gp nodec, not taken");
#ifdef WARTHOG_MESH_HOST_CCMP
    g_warthog_host_ccmp_on = 1;
    const uint32_t h0 = g_warthog_mgmt_prot_host;
    preq_air_(A, K_A_MGTK, 1, 7, 35);
    CHECK(g_warthog_mgmt_prot_host - h0 == 1u && g_warthog_hwmp_gp - gp0 == 1u,
          "(pin) host CCMP armed opens and takes it");
#endif
    (void)rep0;
    (void)own0;
#endif
}

static void t_leave(void)
{
    printf("--- (6) a peer that leaves takes its MGTK out of the chip ---\n");
    fresh_(1);
    keyed_(A, K_A_MTK, K_A_MGTK);
    const uint16_t aid = aid_(A);
    simnode_chipcmd_clear();
    const uint32_t d0 = g_warthog_peer_gtk_del;
    simnode_del_peer(A);
    const int dis = cmd_at_(MORSE_CMD_ID_DISABLE_KEY, aid);
    const int assoc = cmd_at_arg_(MORSE_CMD_ID_SET_STA_STATE, aid, MORSE_STA_ASSOCIATED);
    const int auth = cmd_at_arg_(MORSE_CMD_ID_SET_STA_STATE, aid, MORSE_STA_AUTHENTICATED);
#if PER_STA
    const struct simnode_chipcmd *c = dis >= 0 ? simnode_chipcmd_get((unsigned)dis) : NULL;
    CHECK(c != NULL && !c->pairwise && c->arg == 1u && c->ret == 0,
          "(red) DISABLE_KEY for A's group key, chip index 1, at A's AID %u", aid);
    CHECK(assoc >= 0 && dis > assoc && auth > dis,
          "(red) where mac80211 frees a station's keys: after AUTHORIZED -> ASSOCIATED, before the "
          "rest of the walk down (%d < %d < %d)", assoc, dis, auth);
    CHECK(!simnode_chip_key_held(aid, false, 1, NULL) && g_warthog_peer_gtk_del - d0 == 1u &&
              g_warthog_peer_gtk[aid - 1u] == 0u,
          "(red) the chip no longer holds it; counted gtk del, the slot cleared");
#else
    CHECK(dis < 0 && g_warthog_peer_gtk_del == d0, "(pin) no DISABLE_KEY: nothing to take out");
    (void)assoc;
    (void)auth;
#endif
    CHECK(held_(0, 1, K_OWN), "(pin) our own MGTK stays at AID 0");

    keyed_(D, K_D_MTK, NULL);
    CHECK(aid_(D) == aid, "D takes A's slot, AID %u", aid_(D));
    uint8_t f[200];
    const uint16_t n = grp_(f, D, K_A_MGTK, 1, 40, 0);
    const struct rx_out r = air_(f, n);
    CHECK(!r.opened && r.delivered == 0u,
          "a group frame in D's name under A's old MGTK is not opened (reason %lu)",
          (unsigned long)r.reason);
}

static void t_redelivery(void)
{
    printf("--- (7) a peer's MGTK delivered again ---\n");
#if PER_STA
    fresh_(1);
    keyed_(A, K_A_MTK, K_A_MGTK);
    const uint16_t aid = aid_(A);
    uint8_t f[200];
    (void)simnode_set_key_rsc(A, K_A_MGTK2, 1, false, RSC5);
    CHECK(held_(aid, 1, K_A_MGTK2), "(red) a new MGTK under the same id replaces the old at A's AID");
    uint16_t n = grp_(f, A, K_A_MGTK, 1, 20, 0);
    struct rx_out r = air_(f, n);
    CHECK(!r.opened && r.delivered == 0u, "(red) a frame under the old one no longer opens (%lu)",
          (unsigned long)r.reason);
    n = grp_(f, A, K_A_MGTK2, 1, 6, 0);
    r = air_(f, n);
    CHECK(r.delivered == 1u, "(red) one under the new one at PN 6, above its RSC, is delivered");

    (void)simnode_set_key_rsc(A, K_A_MGTK2, 1, false, RSC5);
    r = air_(f, n);
    CHECK(held_(aid, 1, K_A_MGTK2) && r.opened && r.reason == 5u,
          "(red) the same key again: still opened, and PN 6 is still a replay (%lu)",
          (unsigned long)r.reason);
    n = grp_(f, A, K_A_MGTK2, 1, 7, 0);
    r = air_(f, n);
    CHECK(r.delivered == 1u, "(red) PN 7 is taken");

    simnode_chipcmd_clear();
    (void)simnode_set_key_rsc(A, K_A_MGTK3, 2, false, RSC5);
    const int dis = cmd_at_(MORSE_CMD_ID_DISABLE_KEY, aid);
    const struct simnode_chipcmd *c = dis >= 0 ? simnode_chipcmd_get((unsigned)dis) : NULL;
    CHECK(c != NULL && !c->pairwise && c->arg == 1u && held_(aid, 2, K_A_MGTK3) &&
              !simnode_chip_key_held(aid, false, 1, NULL),
          "(red) a new key id: id 1 disabled at A's AID, id 2 installed");
    n = grp_(f, A, K_A_MGTK2, 1, 8, 0);
    r = air_(f, n);
    CHECK(!r.opened && r.delivered == 0u, "(red) so a frame under id 1 no longer opens");
    n = grp_(f, A, K_A_MGTK3, 2, 6, 0);
    r = air_(f, n);
    CHECK(r.delivered == 1u && ((g_warthog_peer_gtk[aid - 1u] >> 8) & 0xffu) == 2u,
          "(red) one under id 2 is delivered, and the slot reads key id 2 (reason %lu)",
          (unsigned long)r.reason);
#else
    printf("ok   (pin) nothing to replace: no peer MGTK reaches the chip on this build\n");
#endif
}

static void t_refused(void)
{
    printf("--- (8) an install the chip refuses ---\n");
#if PER_STA
    fresh_(1);
    keyed_(A, K_A_MTK, K_A_MGTK);
    const uint16_t aid = aid_(A);
    uint8_t f[200];
    const uint32_t f0 = g_warthog_peer_gtk_fail;
    simnode_chipcmd_clear();
    simnode_chip_refuse_next_arg(MORSE_CMD_ID_INSTALL_KEY, aid, -22);
    const int st = simnode_set_key_rsc(A, K_A_MGTK2, 1, false, RSC5);
    CHECK(st == (int)MMWLAN_SUCCESS, "the link stands: the key is in the host keychain (%d)", st);
    CHECK(g_warthog_peer_gtk_fail - f0 == 1u && cmd_at_(MORSE_CMD_ID_DISABLE_KEY, aid) >= 0 &&
              !simnode_chip_key_held(aid, false, 1, NULL) && g_warthog_peer_gtk[aid - 1u] == 0u,
          "(red) counted gtk fail; the old key disabled at A's AID, the slot cleared");
    uint16_t n = grp_(f, A, K_A_MGTK, 1, 20, 0);
    struct rx_out r = air_(f, n);
    CHECK(!r.opened && r.delivered == 0u, "(red) nothing under the old key opens");
    n = grp_(f, A, K_A_MGTK2, 1, 21, 0);
    r = air_(f, n);
    CHECK(!r.opened && r.delivered == 0u && r.reason == 4u, "(red) nor under the new: 4 (%lu)",
          (unsigned long)r.reason);

    simnode_fail_next_install_key();
    (void)simnode_set_key_rsc(A, K_A_MGTK2, 1, false, RSC5);
    CHECK(g_warthog_peer_gtk_fail - f0 == 2u, "(red) a failed transport is counted the same way");

    (void)simnode_set_key_rsc(A, K_A_MGTK2, 1, false, RSC5);
    n = grp_(f, A, K_A_MGTK2, 1, 22, 0);
    r = air_(f, n);
    CHECK(held_(aid, 1, K_A_MGTK2) && r.delivered == 1u, "(red) the next delivery installs it again");

    const uint32_t df0 = g_warthog_peer_gtk_delfail, x0 = g_warthog_rx_grp_forged;
    simnode_chip_refuse_next_arg(MORSE_CMD_ID_INSTALL_KEY, aid, -22);
    simnode_chip_refuse_next_arg(MORSE_CMD_ID_DISABLE_KEY, aid, -110);
    (void)simnode_set_key_rsc(A, K_A_MGTK3, 1, false, RSC5);
    CHECK(g_warthog_peer_gtk_delfail - df0 == 1u && held_(aid, 1, K_A_MGTK2),
          "(red) a disable that fails too is counted gtk delfail; the chip still holds the old key");
    n = grp_(f, A, K_A_MGTK2, 1, 23, 0);
    r = air_(f, n);
    CHECK(r.opened && r.delivered == 0u && r.reason == 95u && g_warthog_rx_grp_forged - x0 == 1u,
          "(red) what the chip opens under it is no longer taken as A's: 95 (%lu)",
          (unsigned long)r.reason);
#else
    printf("ok   (pin) no peer MGTK is installed on this build, so none is refused\n");
#endif
}

static void t_survivor_and_rekey(void)
{
    printf("--- (9) a survivor's MGTK goes back with its MTK; AT+REKEY re-pushes it ---\n");
    fresh_(1);
    keyed_(A, K_A_MTK, K_A_MGTK);
    keyed_(C, K_C_MTK, K_C_MGTK);
    const uint16_t aid = aid_(A);
    simnode_keyinst_clear();
    simnode_del_peer(C);
    const struct simnode_keyinst *k = grp_install_(K_A_MGTK);
#if PER_STA
    CHECK(k != NULL && k->aid == aid && k->key_idx == 1u, "(red) A's MGTK re-installed at AID %u", aid);
    uint8_t f[200];
    const uint16_t n = grp_(f, A, K_A_MGTK, 1, 30, 0);
    const struct rx_out r = air_(f, n);
    CHECK(r.delivered == 1u, "(red) and A's group frames still open (%lu)", (unsigned long)r.reason);
#else
    CHECK(k == NULL, "(pin) no group key re-installed");
#endif
    simnode_keyinst_clear();
    g_warthog_rekey_req = aid;      /* AT+REKEY=<slot>: slot + 1 */
    umac_datapath_mesh_service_rekey();
    k = grp_install_(K_A_MGTK);
#if PER_STA
    CHECK(k != NULL && k->aid == aid, "(red) AT+REKEY re-pushes A's MGTK at its AID with its MTK");
#else
    CHECK(k == NULL, "(pin) AT+REKEY re-pushes no group key");
#endif
}

static void t_sta_fallback(void)
{
#if WARTHOG_MESH_CHIP_VIF_MESH
    printf("--- (10) a MESH VIF the chip refused: the STA fallback keeps the STA-VIF rules ---\n");
    simnode_del_peer(NULL);
    simnode_chip_refusals_clear();
    simnode_chip_refuse_add_if(MORSE_CMD_INTERFACE_TYPE_MESH, -1, 0);
    start_(1, /*keep_refusals=*/true);
    CHECK(g_warthog_chipvif_type == MORSE_CMD_INTERFACE_TYPE_STA, "on a STA VIF (%lu)",
          (unsigned long)g_warthog_chipvif_type);
    keyed_(A, K_A_MTK, K_A_MGTK);
    CHECK(!held_(aid_(A), 1, K_A_MGTK) && grp_install_(K_A_MGTK) == NULL,
          "(pin) A's MGTK stays host-only");
    uint8_t f[200];
    uint16_t n = grp_(f, A, K_OWN, 1, 0x700000u, 0);
    struct rx_out r = air_(f, n);
    CHECK(r.opened && r.delivered == 0u && r.reason == 95u,
          "(pin) a group frame the chip opened under our MGTK in A's name: 95 (%lu)",
          (unsigned long)r.reason);
    n = grp_(f, A, K_A_MGTK, 1, 10, 0);
    r = air_(f, n);
    CHECK(!r.opened && r.reason == 4u, "(pin) A's own is not opened: 4 (%lu)", (unsigned long)r.reason);
    simnode_del_peer(NULL);
    simnode_stop();
#endif
}

static void t_own_tx(void)
{
    printf("--- (11) our own broadcast still goes out under our own MGTK ---\n");
    fresh_(1);
    simnode_set_gates(false, false, /*grp_std=*/true, true);
    keyed_(A, K_A_MTK, K_A_MGTK);
    simnode_outbox_clear();
    const unsigned d0 = simnode_group_pn_draws();
    (void)simnode_host_tx(BC, W, PAY, sizeof(PAY));
    const struct simnode_frame *f = NULL;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *o = simnode_outbox_get(i);
        if (!o->is_mgmt && (o->bytes[4] & 0x01u) != 0u) { f = o; }
    }
    CHECK(f != NULL && (f->tx_flags & MMDRV_TX_FLAG_HW_ENC) != 0u && f->key_idx == 1u &&
              simnode_group_pn_draws() - d0 == 1u && held_(0, 1, K_OWN),
          "(pin) one group frame, HW_ENC under key id 1, drawing its PN from the AID 0 slot");
}

/* @p f, sealed under @p key, as the chip opens it and the driver reads it off: queued for the
 * event loop, which has not run. True if the chip opened it. */
static bool queued_(const uint8_t *f, uint16_t n)
{
    const unsigned o0 = simnode_chip_rx_opened();
    (void)simnode_rx_air_queued(f, n, -60);
    return simnode_chip_rx_opened() != o0;
}

/* Run the event loop over what is queued: what reached the host and why not. */
static struct rx_out pump_(void)
{
    simnode_host_rx_clear();
    g_warthog_rxdrop_reason = 0;
    simnode_pump();
    struct rx_out r = { simnode_host_rx_count(), g_warthog_rxdrop_reason, false, false };
    return r;
}

static void t_fence(void)
{
    printf("--- (12) a frame the chip opened under a peer's old MGTK, queued when the new one lands ---\n");
#if PER_STA
    fresh_(1);
    keyed_(A, K_A_MTK, K_A_MGTK);
    uint8_t f[200];
    uint16_t n = grp_(f, A, K_A_MGTK, 1, 1000, 0);
    struct rx_out r = air_(f, n);
    CHECK(r.delivered == 1u, "A's PN 1000 under its MGTK is taken (its counter is now 1000)");
    n = grp_(f, A, K_A_MGTK, 1, 600, 0);
    r = air_(f, n);
    CHECK(r.delivered == 0u && r.reason == 5u, "a replay at PN 600 processed at once: 5 (%lu)",
          (unsigned long)r.reason);

    /* On a live link: an outsider's replay the chip opened under A's MGTK, still queued. */
    n = grp_(f, A, K_A_MGTK, 1, 600, 0);
    CHECK(queued_(f, n), "the same replay, opened by the chip under A's old MGTK and queued");
    uint32_t fe0 = g_warthog_peer_gtk_fence;
    (void)simnode_set_key_rsc(A, K_A_MGTK2, 1, false, RSC5);
    r = pump_();
    CHECK(r.delivered == 0u && r.reason == 95u && g_warthog_peer_gtk_fence - fe0 == 1u,
          "(red) A's new MGTK lands before it is processed: refused, 95, counted gtk fence "
          "(delivered %u, reason %lu)", r.delivered, (unsigned long)r.reason);
    n = grp_(f, A, K_A_MGTK2, 1, 6, 0);
    r = air_(f, n);
    CHECK(r.delivered == 1u, "(red) a frame read after the new key went in is taken (reason %lu)",
          (unsigned long)r.reason);

    /* The same for group path selection, on the management queue. */
    preq_air_(A, K_A_MGTK2, 1, 900, 60);
    uint8_t pf[160];
    const uint16_t pn_ = preq_(pf, A, K_A_MGTK2, 1, 500, 61);
    CHECK(queued_(pf, pn_), "a replay of A's group PREQ at PN 500, opened by the chip under A's "
          "MGTK and queued");
    fe0 = g_warthog_peer_gtk_fence;
    const uint32_t own0 = g_warthog_mgmt_gp_own, gp0 = g_warthog_hwmp_gp;
    simnode_outbox_clear();
    (void)simnode_set_key_rsc(A, K_A_MGTK3, 1, false, RSC5);
    (void)pump_();
    CHECK(g_warthog_mgmt_gp_own - own0 == 1u && g_warthog_peer_gtk_fence - fe0 == 1u &&
              g_warthog_hwmp_gp == gp0 && !prep_to_(A),
          "(red) A's next MGTK lands before it is processed: refused (gp own, gtk fence), not answered");
    preq_air_(A, K_A_MGTK3, 1, 6, 62);
    CHECK(g_warthog_hwmp_gp - gp0 == 1u && prep_to_(A),
          "(red) A's PREQ read after it went in is taken and answered");
    n = grp_(f, A, K_A_MGTK3, 1, 6, 0);
    CHECK(air_(f, n).delivered == 1u, "and A's group data under it");

    /* The same key again, and a survivor's re-install: the chip held that key throughout. */
    n = grp_(f, A, K_A_MGTK3, 1, 7, 0);
    CHECK(queued_(f, n), "A's PN 7 under its MGTK, queued");
    (void)simnode_set_key_rsc(A, K_A_MGTK3, 1, false, RSC5);
    r = pump_();
    CHECK(r.delivered == 1u, "(red) the same MGTK delivered again moves no fence: taken (reason %lu)",
          (unsigned long)r.reason);
    keyed_(C, K_C_MTK, K_C_MGTK);
    n = grp_(f, A, K_A_MGTK3, 1, 8, 0);
    CHECK(queued_(f, n), "A's PN 8, queued");
    simnode_del_peer(C);
    r = pump_();
    CHECK(r.delivered == 1u, "(red) nor does A's re-install when C leaves: taken (reason %lu)",
          (unsigned long)r.reason);

    /* A leaves and re-peers with the same MAC while the frame waits (management first). */
    fresh_(1);
    keyed_(A, K_A_MTK, K_A_MGTK);
    n = grp_(f, A, K_A_MGTK, 1, 1000, 0);
    CHECK(air_(f, n).delivered == 1u, "A's PN 1000 is taken");
    n = grp_(f, A, K_A_MGTK, 1, 700, 0);
    CHECK(queued_(f, n), "a replay at PN 700, opened by the chip under A's MGTK and queued");
    fe0 = g_warthog_peer_gtk_fence;
    simnode_del_peer(A);
    keyed_(A, K_A_MTK2, K_A_MGTK2);
    r = pump_();
    CHECK(r.delivered == 0u && r.reason == 95u && g_warthog_peer_gtk_fence - fe0 == 1u,
          "(red) A re-peered and keyed before it is processed: refused, 95, gtk fence "
          "(delivered %u, reason %lu)", r.delivered, (unsigned long)r.reason);
    n = grp_(f, A, K_A_MGTK2, 1, 6, 0);
    CHECK(air_(f, n).delivered == 1u, "(red) and A's next frame under its new MGTK is taken");

    /* The loop runs while A is away (the handshake takes round trips): nothing either. */
    fresh_(1);
    keyed_(A, K_A_MTK, K_A_MGTK);
    n = grp_(f, A, K_A_MGTK, 1, 1000, 0);
    CHECK(air_(f, n).delivered == 1u, "A's PN 1000 is taken");
    n = grp_(f, A, K_A_MGTK, 1, 700, 0);
    CHECK(queued_(f, n), "a replay at PN 700, queued");
    simnode_del_peer(A);
    r = pump_();
    CHECK(r.delivered == 0u, "(pin) processed while A is gone: not delivered (reason %lu)",
          (unsigned long)r.reason);
    keyed_(A, K_A_MTK2, K_A_MGTK2);
    r = pump_();
    CHECK(r.delivered == 0u, "(pin) nor after A's new keys");
#else
    printf("ok   (pin) no peer MGTK reaches the chip on this build: nothing to fence\n");
#endif
}

static void t_taint(void)
{
    printf("--- (13) a DISABLE_KEY the chip refuses leaves a stale key at the AID ---\n");
#if PER_STA
    fresh_(1);
    keyed_(A, K_A_MTK, K_A_MGTK);
    const uint16_t aid = aid_(A);
    const uint32_t bit = 1u << (aid - 1u);
    uint8_t f[200];
    uint16_t n = grp_(f, A, K_A_MGTK, 1, 1000, 0);
    CHECK(air_(f, n).delivered == 1u, "A's PN 1000 is taken");
    uint32_t df0 = g_warthog_peer_gtk_delfail;
    simnode_chip_refuse_next_arg(MORSE_CMD_ID_DISABLE_KEY, aid, -110);
    simnode_del_peer(A);
    CHECK(g_warthog_peer_gtk_delfail - df0 == 1u && held_(aid, 1, K_A_MGTK) &&
              (g_warthog_peer_gtk_tainted & bit) != 0u,
          "(red) A leaves, the chip refuses the DISABLE_KEY: delfail, the chip still holds A's MGTK, "
          "AID %u tainted (0x%lx)", aid, (unsigned long)g_warthog_peer_gtk_tainted);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK2, 0, true);
    CHECK(aid_(A) == aid, "A re-peers at AID %u", aid_(A));
    n = grp_(f, A, K_A_MGTK, 1, 700, 0);
    struct rx_out r = air_(f, n);
    CHECK(r.opened && r.delivered == 0u && r.reason == 95u,
          "during its handshake the chip opens an old-key replay: refused, 95 (%lu)",
          (unsigned long)r.reason);
    n = grp_(f, A, K_A_MGTK, 1, 701, 0);
    CHECK(queued_(f, n), "one the chip opens just before A's new MGTK goes in, queued");
    simnode_chipcmd_clear();
    const uint32_t fe0 = g_warthog_peer_gtk_fence;
    (void)simnode_set_key_rsc(A, K_A_MGTK2, 1, false, RSC5);
    const int dis = cmd_at_(MORSE_CMD_ID_DISABLE_KEY, aid);
    CHECK(dis >= 0 && (g_warthog_peer_gtk_tainted & bit) == 0u && held_(aid, 1, K_A_MGTK2),
          "(red) the install first retries the DISABLE_KEY; it succeeds, the taint is gone, the new "
          "MGTK is in");
    r = pump_();
    CHECK(r.delivered == 0u && r.reason == 95u && g_warthog_peer_gtk_fence - fe0 == 1u,
          "(red) the queued old-key frame is refused all the same: read before the install (95, %lu)",
          (unsigned long)r.reason);
    n = grp_(f, A, K_A_MGTK2, 1, 6, 0);
    CHECK(air_(f, n).delivered == 1u, "(red) A's next frame under its new MGTK is taken");

    /* A chip that boots holds no stale key. */
    df0 = g_warthog_peer_gtk_delfail;
    simnode_chip_refuse_next_arg(MORSE_CMD_ID_DISABLE_KEY, aid, -110);
    simnode_del_peer(A);
    CHECK(g_warthog_peer_gtk_delfail - df0 == 1u && (g_warthog_peer_gtk_tainted & bit) != 0u,
          "A leaves, the DISABLE_KEY refused: AID %u tainted", aid);
    fresh_(1);
    CHECK(g_warthog_peer_gtk_tainted == 0u, "(pin) the chip boots: no taint (0x%lx)",
          (unsigned long)g_warthog_peer_gtk_tainted);

    /* The retry refused too: the new MGTK goes in over the stale one, and nothing is taken. */
    keyed_(A, K_A_MTK, K_A_MGTK);
    df0 = g_warthog_peer_gtk_delfail;
    simnode_chip_refuse_next_arg(MORSE_CMD_ID_DISABLE_KEY, aid, -110);
    simnode_chip_refuse_next_arg(MORSE_CMD_ID_DISABLE_KEY, aid, -110);
    simnode_del_peer(A);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK2, 0, true);
    (void)simnode_set_key_rsc(A, K_A_MGTK2, 1, false, RSC5);
    CHECK(g_warthog_peer_gtk_delfail - df0 == 2u && held_(aid, 1, K_A_MGTK2) &&
              (g_warthog_peer_gtk_tainted & bit) != 0u,
          "(red) both DISABLE_KEYs refused: the new MGTK went in over the stale one, AID %u still "
          "tainted", aid);
    uint32_t t0 = g_warthog_peer_gtk_taint;
    n = grp_(f, A, K_A_MGTK2, 1, 6, 0);
    r = air_(f, n);
    CHECK(r.opened && r.delivered == 0u && r.reason == 95u && g_warthog_peer_gtk_taint - t0 == 1u,
          "(red) what the chip opens for A is refused while tainted: 95, counted gtk taint (%lu)",
          (unsigned long)r.reason);
    /* Its next install (AT+REKEY here; a survivor's re-install the same) retries: DISABLE_KEY,
     * which the chip takes, then the same key back in. */
    simnode_chipcmd_clear();
    g_warthog_rekey_req = aid;
    umac_datapath_mesh_service_rekey();
    CHECK(cmd_at_(MORSE_CMD_ID_DISABLE_KEY, aid) >= 0 && (g_warthog_peer_gtk_tainted & bit) == 0u &&
              held_(aid, 1, K_A_MGTK2),
          "(red) AT+REKEY retries the DISABLE_KEY first; taken, the taint is gone and A's MGTK is "
          "back in");
    n = grp_(f, A, K_A_MGTK2, 1, 7, 0);
    CHECK(air_(f, n).delivered == 1u, "A's frames are taken again");

    /* Or the peer leaving with a DISABLE_KEY the chip takes. */
    fresh_(1);
    keyed_(A, K_A_MTK, K_A_MGTK);
    simnode_chip_refuse_next_arg(MORSE_CMD_ID_DISABLE_KEY, aid, -110);
    simnode_chip_refuse_next_arg(MORSE_CMD_ID_DISABLE_KEY, aid, -110);
    simnode_del_peer(A);
    (void)simnode_add_peer(A);
    (void)simnode_set_key(A, K_A_MTK2, 0, true);
    (void)simnode_set_key_rsc(A, K_A_MGTK2, 1, false, RSC5);
    t0 = g_warthog_peer_gtk_taint;
    n = grp_(f, A, K_A_MGTK2, 1, 6, 0);
    r = air_(f, n);
    CHECK(r.delivered == 0u && g_warthog_peer_gtk_taint - t0 == 1u, "tainted again: refused");
    const uint32_t d0 = g_warthog_peer_gtk_del;
    simnode_del_peer(A);
    CHECK(g_warthog_peer_gtk_del - d0 == 1u && (g_warthog_peer_gtk_tainted & bit) == 0u &&
              !simnode_chip_key_held(aid, false, 1, NULL),
          "(red) A leaves and that DISABLE_KEY succeeds: the AID holds nothing, the taint is gone");
    keyed_(A, K_A_MTK, K_A_MGTK3);
    n = grp_(f, A, K_A_MGTK3, 1, 6, 0);
    CHECK(air_(f, n).delivered == 1u, "(red) A re-peers: its frames are taken again");
#else
    printf("ok   (pin) no peer MGTK reaches the chip on this build: nothing to taint\n");
#endif
}

static void t_fence_retires(void)
{
    printf("--- (14) a fence retires a minute after its install ---\n");
#if PER_STA
    fresh_(1);
    g_warthog_rx_read_seq = 0x7ffffff0u;
    keyed_(A, K_A_MTK, K_A_MGTK);
    uint8_t f[200];
    uint16_t n = grp_(f, A, K_A_MGTK, 1, 10, 0);
    CHECK(air_(f, n).delivered == 1u, "A's PN 10 is taken");
    simnode_advance_ms(61000u);
    umac_datapath_mesh_service_peer_gtk();
    g_warthog_rx_read_seq += 0x80000000u; /* 2^31 frames read since the install */
    n = grp_(f, A, K_A_MGTK, 1, 11, 0);
    const struct rx_out r = air_(f, n);
    CHECK(r.delivered == 1u, "(red) 2^31 frames later the read order has wrapped; the fence has "
          "retired, so A's PN 11 is still taken (reason %lu)", (unsigned long)r.reason);
#else
    printf("ok   (pin) no fence on this build\n");
#endif
}

static void t_mic_fallback(void)
{
    printf("--- (15) a chip that tries our MGTK at AID 0 when a station's key fails the MIC ---\n");
#if PER_STA
    fresh_(1);
    keyed_(A, K_A_MTK, K_A_MGTK);
    keyed_(C, K_C_MTK, K_C_MGTK);
    simnode_chip_group_fallback_mic(true);
    uint8_t f[200];
    /* C holds our MGTK (key id 1, as A's): it forges in A's name at a PN near the top. */
    uint16_t n = grp_(f, A, K_OWN, 1, 0xfffffffffff0ull, 0);
    uint32_t mbad0 = g_warthog_rx_grp_mic_bad;
    struct rx_out r = air_(f, n);
    CHECK(r.opened && r.delivered == 1u && g_warthog_rx_grp_mic_bad - mbad0 == 1u,
          "the residual while unarmed: the chip opens C's forgery under our MGTK and it is taken "
          "as A's, mic bad");
    n = grp_(f, A, K_A_MGTK, 1, 11, 0);
    r = air_(f, n);
    CHECK(r.delivered == 0u && r.reason == 5u,
          "and A's real PN 11 is then a replay on this node (5, %lu)", (unsigned long)r.reason);

    fresh_(1);
    keyed_(A, K_A_MTK, K_A_MGTK);
    keyed_(C, K_C_MTK, K_C_MGTK);
    simnode_chip_group_fallback_mic(true);
    n = grp_(f, A, K_A_MGTK, 1, 10, 0);
    r = air_(f, n);
    CHECK(r.delivered == 1u && (g_warthog_rx_grp_mic_armed & 1u) != 0u,
          "(red) A's PN 10 verifies: mic ok arms the check for data");
    n = grp_(f, A, K_OWN, 1, 0xfffffffffff0ull, 0);
    mbad0 = g_warthog_rx_grp_mic_bad;
    const uint32_t md0 = g_warthog_rx_grp_micdrop;
    r = air_(f, n);
    CHECK(r.opened && r.delivered == 0u && r.reason == 95u && g_warthog_rx_grp_mic_bad - mbad0 == 1u &&
              g_warthog_rx_grp_micdrop - md0 == 1u,
          "(red) armed: C's forgery is dropped, 95, mic bad and micdrop (%lu)", (unsigned long)r.reason);
    n = grp_(f, A, K_A_MGTK, 1, 11, 0);
    r = air_(f, n);
    CHECK(r.delivered == 1u, "(red) and A's real PN 11 is still taken (%lu)", (unsigned long)r.reason);

    const uint32_t gmd0 = g_warthog_mgmt_gp_micdrop, gp0 = g_warthog_hwmp_gp;
    preq_air_(A, K_A_MGTK, 1, 6, 50);
    CHECK(g_warthog_hwmp_gp - gp0 == 1u && (g_warthog_rx_grp_mic_armed & 2u) != 0u,
          "(red) A's group PREQ verifies: taken, mic ok arms the check for management");
    simnode_outbox_clear();
    preq_air_(A, K_OWN, 1, 0xfffffffffff0ull, 51);
    CHECK(g_warthog_mgmt_gp_micdrop - gmd0 == 1u && g_warthog_hwmp_gp - gp0 == 1u && !prep_to_(A),
          "(red) armed: C's PREQ under our MGTK in A's name is refused (gp micdrop), not answered");
    preq_air_(A, K_A_MGTK, 1, 7, 52);
    CHECK(g_warthog_hwmp_gp - gp0 == 2u && prep_to_(A),
          "(red) A's PREQ at PN 7 is still taken and answered");

    /* Another of A's peers holds A's MGTK itself: that forgery verifies (the residual). */
    n = grp_(f, A, K_A_MGTK, 1, 0xfffff0ull, 0);
    const uint32_t mok0 = g_warthog_rx_grp_mic_ok;
    r = air_(f, n);
    CHECK(r.delivered == 1u && g_warthog_rx_grp_mic_ok - mok0 == 1u,
          "the residual: a forgery under A's own MGTK verifies (mic ok) and is taken");
    simnode_chip_group_fallback_mic(false);
#else
    printf("ok   (pin) no chip-opened peer group frame is taken on this build\n");
#endif
}

static void t_pn_epochs(void)
{
    printf("--- (16) each install of a peer's MGTK takes a fresh TX PN epoch ---\n");
#if PER_STA
    fresh_(1);
    keyed_(A, K_A_MTK, K_A_MGTK);
    keyed_(C, K_C_MTK, K_C_MGTK);
    const uint16_t aid = aid_(A);
    const struct simnode_keyinst *k = grp_install_(K_A_MGTK);
    const uint64_t pn0 = k != NULL ? k->tx_pn : 0u;
    CHECK(k != NULL && pn0 >= (1ull << 20), "(red) A's MGTK went in at a fresh epoch (0x%llx)",
          (unsigned long long)pn0);
    simnode_keyinst_clear();
    simnode_del_peer(C);
    k = grp_install_(K_A_MGTK);
    const uint64_t pn1 = k != NULL ? k->tx_pn : 0u;
    CHECK(k != NULL && pn1 > pn0 && (pn1 & 0xfffffull) == 0u,
          "(red) re-installed when C leaves at an epoch above it (0x%llx > 0x%llx)",
          (unsigned long long)pn1, (unsigned long long)pn0);
    simnode_keyinst_clear();
    g_warthog_rekey_req = aid;
    umac_datapath_mesh_service_rekey();
    k = grp_install_(K_A_MGTK);
    CHECK(k != NULL && k->tx_pn > pn1, "(red) and again on AT+REKEY (0x%llx)",
          k != NULL ? (unsigned long long)k->tx_pn : 0ull);
    simnode_keyinst_clear();
    g_warthog_peer_gtk_mode = 2; /* AT+GTKPERSTA=2 */
    (void)simnode_set_key_rsc(A, K_A_MGTK2, 1, false, RSC5);
    k = grp_install_(K_A_MGTK2);
    CHECK(k != NULL && k->tx_pn == 0u && held_(aid, 1, K_A_MGTK2),
          "(red) AT+GTKPERSTA=2: installed at TX PN 0, as Linux");
    g_warthog_peer_gtk_mode = 1;
#else
    printf("ok   (pin) no peer MGTK is installed on this build\n");
#endif
}

static void t_toggle(void)
{
    printf("--- (17) AT+GTKPERSTA=0 takes peers' MGTKs out at run time; =1 puts them back ---\n");
    fresh_(1);
    keyed_(A, K_A_MTK, K_A_MGTK);
    keyed_(C, K_C_MTK, K_C_MGTK);
    const uint16_t aa = aid_(A), ac = aid_(C);
    uint8_t f[200];
    simnode_chipcmd_clear();
    const uint32_t d0 = g_warthog_peer_gtk_del, x0 = g_warthog_rx_grp_forged;
    g_warthog_peer_gtk_mode = 0; /* AT+GTKPERSTA=0 */
    uint16_t n = grp_(f, A, K_A_MGTK, 1, 10, 0);
    struct rx_out r = air_(f, n);
#if PER_STA
    CHECK(r.opened && r.delivered == 0u && r.reason == 95u && g_warthog_rx_grp_forged - x0 == 1u,
          "(red) at once, before the tick: what the chip still opens for A is refused, 95 (%lu)",
          (unsigned long)r.reason);
#else
    CHECK(!r.opened && r.reason == 4u, "(pin) the chip opens nothing for A (%lu)", (unsigned long)r.reason);
#endif
    simnode_tick();
    const int da = cmd_at_(MORSE_CMD_ID_DISABLE_KEY, aa), dc = cmd_at_(MORSE_CMD_ID_DISABLE_KEY, ac);
#if PER_STA
    CHECK(da >= 0 && dc >= 0 && g_warthog_peer_gtk_del - d0 == 2u && !held_(aa, 1, K_A_MGTK) &&
              !held_(ac, 1, K_C_MGTK) && g_warthog_peer_gtk[aa - 1u] == 0u && g_warthog_peer_gtk[ac - 1u] == 0u,
          "(red) the tick takes A's and C's MGTKs out of the chip: DISABLE_KEY at AIDs %u and %u, gtk "
          "del, the slots cleared", aa, ac);
#else
    CHECK(da < 0 && dc < 0 && g_warthog_peer_gtk_del == d0, "(pin) nothing to take out");
#endif
    CHECK(aid_(A) == aa && aid_(C) == ac && held_(0, 1, K_OWN),
          "(pin) the links stand and our own MGTK keeps AID 0");
    n = grp_(f, A, K_A_MGTK, 1, 11, 0);
    r = air_(f, n);
    CHECK(!r.opened && r.delivered == 0u && r.reason == 4u, "A's group frame is not opened: 4 (%lu)",
          (unsigned long)r.reason);
    simnode_keyinst_clear();
    keyed_(D, K_D_MTK, K_E_MGTK);
    CHECK(grp_install_(K_E_MGTK) == NULL, "(red) a peer keyed while off gets no MGTK in the chip");
    simnode_set_gates(false, false, /*grp_std=*/true, true);
    simnode_outbox_clear();
    const unsigned g0 = simnode_group_pn_draws();
    (void)simnode_host_tx(BC, W, PAY, sizeof(PAY));
    CHECK(simnode_group_pn_draws() - g0 == 1u, "(pin) our own broadcast still goes out under our MGTK");

    const uint32_t i0 = g_warthog_peer_gtk_inst;
    g_warthog_peer_gtk_mode = 1; /* AT+GTKPERSTA=1 */
    simnode_tick();
#if PER_STA
    CHECK(g_warthog_peer_gtk_inst - i0 == 3u && held_(aa, 1, K_A_MGTK) && held_(ac, 1, K_C_MGTK) &&
              held_(aid_(D), 1, K_E_MGTK),
          "(red) =1: the tick puts A's, C's and D's MGTKs in (gtk inst %lu)",
          (unsigned long)(g_warthog_peer_gtk_inst - i0));
    n = grp_(f, A, K_A_MGTK, 1, 12, 0);
    r = air_(f, n);
    CHECK(r.delivered == 1u, "(red) and A's group frames are taken again (%lu)", (unsigned long)r.reason);
#else
    CHECK(g_warthog_peer_gtk_inst == i0, "(pin) nothing to put in on this build");
#endif
}

int main(void)
{
    printf("=== simnode peergtk: a peer's MGTK in the chip at its AID (PER_STA=%d) ===\n", PER_STA);
    t_install();
    t_group_data();
    t_forged();
    t_unkeyed_slot();
    t_group_path_selection();
    t_leave();
    t_redelivery();
    t_refused();
    t_survivor_and_rekey();
    t_own_tx();
    t_fence();
    t_taint();
    t_fence_retires();
    t_mic_fallback();
    t_pn_epochs();
    t_toggle();
    t_sta_fallback();

    simnode_del_peer(NULL);
    CHECK(simnode_live_allocs() == 0, "no packet buffer was orphaned (%u live)", simnode_live_allocs());
    simnode_stop();
    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_simnode_peergtk: all passed\n");
    return 0;
}
