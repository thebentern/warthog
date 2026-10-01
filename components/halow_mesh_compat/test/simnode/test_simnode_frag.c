/*
 * Fragmented unicast mesh data through the real receive path: decryption, the replay check,
 * the reorder window, reassembly (datapath_defrag.c), then Mesh Control, the forwarding
 * engine, leaf learning and delivery (umac_datapath.c), as mac80211 orders them (rx.c:
 * ieee80211_rx_h_decrypt, ieee80211_rx_h_defragment, then the mesh header on the MSDU).
 *
 * On air on 2026-10-01 an OpenMANET 1.8.0 node (MM6108, chip firmware 2.0.1) whose rate to a
 * Warthog fell to S1G MCS0 sent each 1000-byte ping as two fragments: its chip fragments on
 * its own at low rates (morse_cli stats 'TX fragment'). The Warthog decrypted both, then
 * parsed and stripped a Mesh Control off EVERY fragment before reassembling, so the second
 * fragment lost 6 to 18 payload octets (and its payload was read as Address Extension) and
 * the IP stack dropped the reassembled packet. Only the first fragment carries Mesh Control.
 *
 * Each case marked (red) failed on the tree before the change; (pin) marks behaviour that
 * must not change and passed before it too, though the counter it reads (AT+DEFRAG?) is new
 * and read 0 there. Delivery is read on the extended RX callback,
 * which holds the whole 802.3 frame, so "identical" means every octet of it.
 *
 * Every build (SAE: the chip opens a fragment when it holds the link's MTK, else host CCMP
 * does when built and armed; an open mesh; a non-SAE keyed mesh, chip-decrypted):
 *  (1) (red) a 2- and a 3-fragment unicast, with no Address Extension, AE 1 (A4) and AE 2
 *      (A5/A6), is delivered once, after its last fragment, byte-identical to the same MSDU
 *      sent whole; Mesh Control is parsed once (meshctrl stripped and ae counted once);
 *      counted in and ok;
 *  (2) (red) relay: a fragmented unicast for another peer is forwarded once, the relayed
 *      copy identical to the one relayed for the whole MSDU (but its sequence numbers);
 *  (3) (red) leaf: a fragmented AE 2 frame from a host behind A is delivered as the host's
 *      and the host is learned behind A; payload octets at the start of a later fragment
 *      that look like Mesh Control teach nothing; one for another node is dropped once (93);
 *  (4) (red) batman mode: a fragmented batman frame reaches the extended hook intact;
 * FragAttacks (Vanhoef 2021; mac80211's checks), each delivering nothing:
 *  (5) (red) a PN gap inside a chain (pn), and a key replaced under a chain: the mesh
 *      flushes a peer's chains when its pairwise key changes (flush, then nofirst), and a
 *      chain whose key changed by any other path is refused (key); fragments under two
 *      key ids (key, or 96 where the chip build takes only the link's key id);
 *  (6) (red) a plaintext fragment on a keyed link, even one that starts like EAPOL, is
 *      refused before anything reads it (plain, rxdrop 98) and never reaches the supplicant;
 *  (7) (red) a later fragment whose addresses differ from the first's (hdr); one with the
 *      A-MSDU Present bit (amsdu); a group-addressed fragment, last fragment included
 *      (mcast, rxdrop 7);
 * (7b) (red) mac80211's mesh frame shapes (ieee80211_rx_mesh_check): a 4-address frame with a
 *      group RA and DA us, whole or fragmented, is dropped before decryption and reassembly
 *      (rxdrop 89), on SAE sealed under the TA's MGTK, which every peer of the TA holds; so is a
 *      3-address unicast; a 4-address unicast-RA frame with a group DA is reassembled, as
 *      mac80211 decides "group" by addr1;
 *  (8) out of order: a later fragment before its first is dropped (nofirst, pin); one that
 *      skips a number is dropped and the chain kept, so the retransmission completes it
 *      (order, red); a missing first (nofirst, pin);
 *  (9) (red) a chain expires 1 s after its first fragment: by its timer (expired, then
 *      nofirst), and when the timer could not be armed, at its next fragment (expired);
 *      a chain larger than the buffer is dropped (oversize); a new first fragment
 *      replaces an incomplete chain (restart); a peer that leaves takes its chains (flush);
 * (10) the duplicate filter and Block Ack: fragments of one MSDU share a sequence number
 *      but are not duplicates (pin); a retransmitted fragment is (FILTSTAT dup, pin) and the
 *      MSDU is still delivered once, intact (red); under a Block Ack session the expected
 *      sequence control steps through the fragments, a fragment received early waits in
 *      the reorder window and the MSDU is delivered intact, the next one is not outdated.
 * Bounds (reassembly holds chip-RX-sized buffers, and its timeouts share the core's pool with
 * hostap's MPM and SAE timers):
 * (11) (red) a chain the core had no timeout for still goes 1 s after its first fragment,
 *      whatever comes next: whole frames from others within that second (which arm its
 *      expiry), the mesh service tick, or the next data frame from anyone; its peer stays;
 * (12) (red) a first fragment on each of TIDs 0-7 from two peers: 4 chains held on the node,
 *      each peer's 2 newest (evict), under one timeout; they complete; an evicted chain's
 *      later fragment joins nothing; a third peer's first fragment evicts the node's oldest;
 * (13) (red) QoS Control bit 8 is Mesh Control Present only on the mesh: a BSS uplink whose
 *      Queue Size changes between fragments is reassembled (datapath_defrag called directly,
 *      as the STA/AP datapaths call it; the simulator runs only the mesh).
 * Nothing leaks: every chain is freed by completion, a drop, expiry or the peer's removal.
 *
 * Builds: test_simnode_frag (as warthog-mesh-sae: AMPE keys in the chip),
 * test_simnode_frag_nochip (-nochipkey: SAE data never opens, so the SAE cases only pin that
 * nothing reaches reassembly), test_simnode_frag_swccmp (-swccmp, host CCMP armed),
 * test_simnode_frag_meshvif and test_simnode_frag_swccmp_meshvif (the build measured on air).
 */
#include <stdio.h>
#include <string.h>

#include "simnode.h"
#include "mmdrv.h"
#include "mmwlan.h"
#include "mmpkt.h"
#include "umac/data/umac_data.h"
#include "umac/datapath/datapath_defrag.h"
#include "umac/datapath/umac_datapath_private.h"
#include "umac/keys/connection_keys.h"
#include "umac/keys/umac_keys_data.h"
#include "umac/mesh/umac_mesh_ccm.h"
#include "umac/mesh/umac_mesh_ccmp_hdr.h"
#include "umac/mesh/umac_mesh_ctrl.h"
#include "umac/mesh/umac_mesh_ies.h"
#include "umac/mesh/umac_mesh_fwd_glue.h"

/* How a Protected unicast from a keyed SAE peer gets opened on this build. */
#if !defined(WARTHOG_MESH_AMPE_NO_CHIP_KEY)
#define SAE_CHIP 1 /* the chip holds the link's MTK and opens it */
#else
#define SAE_CHIP 0
#endif
#if defined(WARTHOG_MESH_HOST_CCMP)
#define SAE_HOST 1 /* the chip holds no MTK; host CCMP opens it once armed */
#else
#define SAE_HOST 0
#endif
#define SAE_OPENS (SAE_CHIP || SAE_HOST)

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

extern volatile uint32_t g_warthog_defrag_in, g_warthog_defrag_ok, g_warthog_defrag_nofirst,
    g_warthog_defrag_order, g_warthog_defrag_pn, g_warthog_defrag_key, g_warthog_defrag_prot,
    g_warthog_defrag_hdr, g_warthog_defrag_amsdu, g_warthog_defrag_oversize,
    g_warthog_defrag_nomem, g_warthog_defrag_mcast, g_warthog_defrag_plain, g_warthog_defrag_shape,
    g_warthog_defrag_expired, g_warthog_defrag_restart, g_warthog_defrag_flush,
    g_warthog_defrag_evict;
extern volatile uint32_t g_warthog_rxdrop_reason, g_warthog_rxdrop_count;
extern volatile uint32_t g_warthog_rx_meshctrl_stripped, g_warthog_rx_meshctrl_ae;
extern volatile uint32_t g_warthog_rx_data_delivered, g_warthog_filt_hist[10];
extern volatile uint32_t g_warthog_reord_outdated, g_warthog_reord_buffered;
extern volatile uint32_t g_warthog_host_ccmp_on, g_warthog_swccmp_ok;

static const uint8_t W[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 }; /* us */
static const uint8_t A[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0a }; /* the Linux node */
static const uint8_t C[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0c }; /* another peer */
static const uint8_t DD[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0d }; /* a third, when added */
static const uint8_t H2[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x88 }; /* a host behind A */
static const uint8_t H3[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x93 }; /* named only in payload */
static const uint8_t BC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static const uint8_t K_OWN[16] = { 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
                                   0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f };
static const uint8_t K_A[16]   = { 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
                                   0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf };
#if SAE_OPENS
static const uint8_t K_A2[16]  = { 0x1b, 0x2b, 0x3b, 0x4b, 0x5b, 0x6b, 0x7b, 0x8b,
                                   0x9b, 0xab, 0xbb, 0xcb, 0xdb, 0xeb, 0xfb, 0x0b };
static const uint8_t K_A3[16]  = { 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41,
                                   0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49 };
#endif
static const uint8_t K_C[16]   = { 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
                                   0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf };
static const uint8_t K_D[16]   = { 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
                                   0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf };
static const uint8_t K_AG[16]  = { 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad,
                                   0xae, 0xaf, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5 }; /* A's MGTK */
static const uint8_t SNAP_IPV4[8]  = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00 };
static const uint8_t SNAP_EAPOL[8] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E };

#define MAC_EQ(p, m) (memcmp((p), (m), 6) == 0)

/* ---- the link under test ------------------------------------------------- */

enum link { LINK_SAE, LINK_OPEN, LINK_STATIC };
static enum link s_link;
static const char *link_name_(void)
{
    return s_link == LINK_SAE ? "SAE" : s_link == LINK_OPEN ? "open" : "keyed non-SAE";
}

static uint16_t s_seq = 1;      /* 802.11 sequence number, one per MSDU */
static uint64_t s_pn = 1;       /* A's next PN on its pairwise key (one counter, every TID) */
static uint64_t s_pn_c = 1;     /* C's, and D's once added */
static uint64_t s_pn_d = 1;
static uint32_t s_mseq = 7000;  /* Mesh Control sequence number, one per MSDU */

/* A node with peers A and C on @p link; forwarding @p fwd; the extended RX callback on. */
static void up_(enum link link, bool fwd)
{
    s_link = link;
    simnode_del_peer(NULL);
    simnode_set_batman(false);
    simnode_set_ampdu(false);
    if (link == LINK_SAE)
    {
        (void)simnode_start_sae(W);
    }
    else
    {
        (void)simnode_start(W);
    }
    simnode_set_gates(fwd, /*bridge=*/false, /*grp_std=*/false, /*secure=*/link != LINK_OPEN);
    /* Armed for SAE only: a keyed non-SAE link's relayed copy then goes to the chip to seal
     * under the built-in key, which this file does not know. */
    g_warthog_host_ccmp_on = link == LINK_SAE ? SAE_HOST : 0u;
    simnode_set_rx_ext_cb(true);
    if (link == LINK_SAE)
    {
        (void)simnode_set_key(BC, K_OWN, 1, /*pairwise=*/false);
    }
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    if (link == LINK_SAE)
    {
        (void)simnode_set_key(A, K_A, 0, /*pairwise=*/true);
        (void)simnode_set_key(C, K_C, 0, /*pairwise=*/true);
    }
    s_pn = 1;
    s_pn_c = 1;
    simnode_outbox_clear();
    simnode_ext_rx_clear();
    simnode_host_rx_clear();
    simnode_stub_reset();
    g_warthog_rxdrop_reason = 0;
}

/* ---- the MSDU and its MPDUs ------------------------------------------------ */

struct msdu { uint8_t b[3200]; uint16_t len; };

/* Mesh Control @p mc, SNAP with @p ethertype, @p n payload octets patterned by @p salt. */
static void msdu_(struct msdu *m, const struct umac_mesh_ctrl *mc, uint16_t ethertype, uint16_t n,
                  uint8_t salt)
{
    uint16_t k = umac_mesh_ctrl_build(m->b, UMAC_MESH_CTRL_LEN_MAX, mc);
    memcpy(m->b + k, SNAP_IPV4, 6);
    m->b[k + 6] = (uint8_t)(ethertype >> 8);
    m->b[k + 7] = (uint8_t)ethertype;
    k = (uint16_t)(k + 8u);
    for (uint16_t i = 0; i < n; i++) { m->b[k + i] = (uint8_t)(i * 7u + salt); }
    m->len = (uint16_t)(k + n);
}

static struct umac_mesh_ctrl mc_(uint8_t ae, const uint8_t *e1, const uint8_t *e2)
{
    struct umac_mesh_ctrl mc = { .flags = ae, .ttl = 31, .seq = ++s_mseq };
    if (e1 != NULL) { memcpy(mc.eaddr1, e1, 6); }
    if (e2 != NULL) { memcpy(mc.eaddr2, e2, 6); }
    return mc;
}

/* One MPDU as A puts it on the air. */
struct mpdu {
    const uint8_t *ra, *ta, *da, *sa;
    uint16_t seq;
    uint8_t frag;
    bool more, retry;
    uint8_t qos0;        /* QoS Control octet 0: TID, A-MSDU Present (0x80) */
    bool prot;           /* CCMP-sealed */
    const uint8_t *key;  /* sealed under this */
    uint8_t kid;
    uint64_t pn;
};

static void pn6_(uint8_t out[6], uint64_t pn)
{
    for (int i = 0; i < 6; i++) { out[i] = (uint8_t)(pn >> (8 * (5 - i))); }
}

static uint16_t mpdu_(uint8_t *f, const struct mpdu *p, const uint8_t *body, uint16_t blen)
{
    uint16_t n = umac_mesh_ies_build_data_hdr4(f, p->ra, p->ta, p->da, p->sa);
    f[1] = (uint8_t)(f[1] | (p->more ? 0x04u : 0u) | (p->retry ? 0x08u : 0u) | (p->prot ? 0x40u : 0u));
    f[22] = (uint8_t)((p->seq << 4) | (p->frag & 0x0fu));
    f[23] = (uint8_t)(p->seq >> 4);
    f[n++] = p->qos0;
    f[n++] = 0x01; /* Mesh Control Present, on every fragment as the header is copied */
    if (!p->prot)
    {
        memcpy(f + n, body, blen);
        return (uint16_t)(n + blen);
    }
    uint8_t pn[6];
    pn6_(pn, p->pn);
    umac_ccmp_write_header(f + n, pn, p->kid);
    const uint16_t b0 = (uint16_t)(n + UMAC_CCMP_HDR_LEN);
    memcpy(f + b0, body, blen);
    const uint16_t total = (uint16_t)(b0 + blen + UMAC_CCMP_MIC_LEN);
    uint8_t aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
    const uint32_t al = umac_ccmp_build_aad(f, aad);
    umac_ccmp_build_nonce(f, pn, nonce);
    (void)warthog_ccm_ae(p->key, nonce, UMAC_CCMP_MIC_LEN, aad, al, f + b0, blen, f + total - 8u);
    return total;
}

/* Into the node. SAE: off the air through the chip, which opens it if it holds the key.
 * Keyed non-SAE: as the chip hands up one it opened (plaintext, its MIC octets in place). */
static void rx_(const uint8_t *f, uint16_t n, bool prot)
{
    static uint8_t g[3200];
    if (s_link == LINK_SAE || !prot)
    {
        (void)simnode_rx_air(f, n, -70);
        return;
    }
    /* Undo the seal: the chip hands the plaintext up. */
    memcpy(g, f, n);
    const uint32_t hl = umac_ccmp_hdr_len(g);
    uint8_t pn[6], kid = 0, aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
    (void)umac_ccmp_parse_header(g + hl, pn, &kid);
    const uint32_t al = umac_ccmp_build_aad(g, aad);
    umac_ccmp_build_nonce(g, pn, nonce);
    const uint32_t b0 = hl + UMAC_CCMP_HDR_LEN;
    (void)warthog_ccm_ad(K_A, nonce, 8, aad, al, g + b0, n - b0 - 8u, g + n - 8u);
    (void)simnode_rx_flags(g, n, -70, MMDRV_RX_FLAG_DECRYPTED);
}

/* A unicast from A for @p da (mesh SA A), protected unless the link is open. */
static struct mpdu tmpl_(const uint8_t *da)
{
    struct mpdu p = { .ra = W, .ta = A, .da = da, .sa = A, .qos0 = 0x00, .prot = s_link != LINK_OPEN,
                      .key = K_A, .kid = 0 };
    return p;
}

/* Send @p m from @p p as one MPDU, or cut at @p cuts (ascending body offsets) into
 * @p ncut + 1 fragments, each taking the next PN. @p after_each, when non-NULL, gets the
 * extended-callback count after each MPDU. */
static void send_(struct mpdu p, const struct msdu *m, const uint16_t *cuts, unsigned ncut,
                  unsigned *after_each)
{
    static uint8_t f[3200];
    p.seq = s_seq++;
    uint16_t from = 0;
    for (unsigned i = 0; i <= ncut; i++)
    {
        const uint16_t to = (i < ncut) ? cuts[i] : m->len;
        p.frag = (uint8_t)i;
        p.more = i < ncut;
        p.pn = s_pn++;
        const uint16_t n = mpdu_(f, &p, m->b + from, (uint16_t)(to - from));
        rx_(f, n, p.prot);
        if (after_each != NULL) { after_each[i] = simnode_ext_rx_count(); }
        from = to;
    }
}

/* Fragment @p i of @p m cut at @p cuts, built from @p p (frag, more and pn as given). */
static uint16_t frag_(uint8_t *f, const struct mpdu *p, const struct msdu *m, const uint16_t *cuts,
                      unsigned ncut, unsigned i)
{
    const uint16_t from = i == 0 ? 0 : cuts[i - 1];
    const uint16_t to = i < ncut ? cuts[i] : m->len;
    return mpdu_(f, p, m->b + from, (uint16_t)(to - from));
}

struct ext { uint8_t b[1600]; uint16_t len; };

static bool last_ext_(struct ext *e)
{
    const unsigned n = simnode_ext_rx_count();
    const struct simnode_extrx *x = n ? simnode_ext_rx_get(n - 1u) : NULL;
    if (x == NULL) { return false; }
    e->len = x->len;
    memcpy(e->b, x->frame, x->len < sizeof(e->b) ? x->len : sizeof(e->b));
    return true;
}

struct snap {
    uint32_t in, ok, nofirst, order, pn, key, prot, hdr, amsdu, oversize, nomem, mcast, plain,
             shape, expired, restart, flush, evict, stripped, ae, rxdrop, dup, outdated, buffered;
};
static struct snap snap_(void)
{
    struct snap s = { g_warthog_defrag_in, g_warthog_defrag_ok, g_warthog_defrag_nofirst,
                      g_warthog_defrag_order, g_warthog_defrag_pn, g_warthog_defrag_key,
                      g_warthog_defrag_prot, g_warthog_defrag_hdr, g_warthog_defrag_amsdu,
                      g_warthog_defrag_oversize, g_warthog_defrag_nomem, g_warthog_defrag_mcast,
                      g_warthog_defrag_plain, g_warthog_defrag_shape, g_warthog_defrag_expired,
                      g_warthog_defrag_restart,
                      g_warthog_defrag_flush, g_warthog_defrag_evict, g_warthog_rx_meshctrl_stripped,
                      g_warthog_rx_meshctrl_ae, g_warthog_rxdrop_count, g_warthog_filt_hist[8],
                      g_warthog_reord_outdated, g_warthog_reord_buffered };
    return s;
}
#define D(s, f) ((unsigned)(g_warthog_defrag_##f - (s).f))

/* ---- (1) identical to the whole MSDU ---------------------------------------- */

/* @p m as @p p sends it, whole and then cut at @p cuts: delivered once each, after the last
 * fragment, the two 802.3 frames identical; @p ae: its Mesh Control carries AE. */
static void same_as_whole_p_(const char *what, struct mpdu p, const struct msdu *m,
                             const uint16_t *cuts, unsigned ncut, bool ae)
{
    const unsigned live0 = simnode_live_allocs();
    struct ext whole, frag;
    simnode_ext_rx_clear();
    send_(p, m, NULL, 0, NULL);
    const bool got_whole = simnode_ext_rx_count() == 1u && last_ext_(&whole);
    simnode_ext_rx_clear();
    const struct snap s = snap_();
    unsigned after[4] = { 0 };
    send_(p, m, cuts, ncut, after);
    bool early = false;
    for (unsigned i = 0; i < ncut; i++) { early = early || after[i] != 0u; }
    const bool got = simnode_ext_rx_count() == 1u && last_ext_(&frag);
    CHECK(got_whole && got && !early && frag.len == whole.len &&
              memcmp(frag.b, whole.b, whole.len) == 0,
          "%s, %s, in %u fragments: delivered once, after the last, identical to it sent whole "
          "(%u octets; whole %s, fragmented %u delivered, early %d, rxdrop %lu)",
          link_name_(), what, ncut + 1u, whole.len, got_whole ? "ok" : "MISSING",
          simnode_ext_rx_count(), (int)early, (unsigned long)g_warthog_rxdrop_reason);
    CHECK(D(s, in) == ncut + 1u && D(s, ok) == 1u &&
              g_warthog_rx_meshctrl_stripped - s.stripped == 1u &&
              g_warthog_rx_meshctrl_ae - s.ae == (ae ? 1u : 0u),
          "  counted in %u ok %u; its Mesh Control parsed once (stripped %u, ae %u)",
          D(s, in), D(s, ok), (unsigned)(g_warthog_rx_meshctrl_stripped - s.stripped),
          (unsigned)(g_warthog_rx_meshctrl_ae - s.ae));
    CHECK(simnode_live_allocs() == live0, "  and nothing is left allocated (%u -> %u)", live0,
          simnode_live_allocs());
}

/* @p m from A to us. */
static void same_as_whole_(const char *what, const struct msdu *m, const uint16_t *cuts,
                           unsigned ncut, bool ae)
{
    same_as_whole_p_(what, tmpl_(W), m, cuts, ncut, ae);
}

static void t_identical(enum link link)
{
    up_(link, false);
    printf("--- (1) %s: a fragmented unicast is delivered byte-identical to it sent whole ---\n",
           link_name_());
    struct msdu m;
    struct umac_mesh_ctrl mc = mc_(UMAC_MESH_CTRL_AE_NONE, NULL, NULL);
    msdu_(&m, &mc, 0x0800, 1000, 0x11);
    static const uint16_t cut2[] = { 520 };
    same_as_whole_("no AE", &m, cut2, 1, false);

    mc = mc_(UMAC_MESH_CTRL_AE_A5A6, W, H2);
    msdu_(&m, &mc, 0x0800, 1000, 0x22);
    static const uint16_t cut3[] = { 350, 700 };
    same_as_whole_("AE 2 (A5/A6)", &m, cut3, 2, true);
    struct ext e;
    CHECK(last_ext_(&e) && MAC_EQ(e.b, W) && MAC_EQ(e.b + 6, H2),
          "  its 802.3 endpoints are the AE ones: to us, from H2");

    mc = mc_(UMAC_MESH_CTRL_AE_A4, H2, NULL);
    msdu_(&m, &mc, 0x0800, 1000, 0x33);
    same_as_whole_("AE 1 (A4)", &m, cut2, 1, true);

    /* Cut where the second fragment's first octets would read as an AE 2 Mesh Control. */
    mc = mc_(UMAC_MESH_CTRL_AE_NONE, NULL, NULL);
    msdu_(&m, &mc, 0x0800, 1000, 0x44);
    static const uint16_t cut_ae[] = { 400 };
    m.b[400] = UMAC_MESH_CTRL_AE_A5A6;
    same_as_whole_("a later fragment starting like AE 2", &m, cut_ae, 1, false);
}

/* ---- (2) relay ------------------------------------------------------------- */

/* @p f as C reads it once opened: header, QoS and body, no CCMP header or MIC. */
static bool opened_(const struct simnode_frame *f, uint8_t *out, uint16_t *len)
{
    if (f == NULL || f->is_mgmt) { return false; }
    if ((f->bytes[1] & 0x40u) == 0u || (f->tx_flags & MMDRV_TX_FLAG_HW_ENC) != 0u)
    {
        memcpy(out, f->bytes, f->len);
        *len = f->len;
        return true;
    }
    const uint32_t hl = umac_ccmp_hdr_len(f->bytes);
    uint8_t pn[6], kid = 0, aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
    if (f->len < hl + 16u || !umac_ccmp_parse_header(f->bytes + hl, pn, &kid)) { return false; }
    memcpy(out, f->bytes, hl);
    const uint32_t n = f->len - hl - 16u;
    memcpy(out + hl, f->bytes + hl + 8u, n);
    const uint32_t al = umac_ccmp_build_aad(f->bytes, aad);
    umac_ccmp_build_nonce(f->bytes, pn, nonce);
    if (warthog_ccm_ad(K_C, nonce, 8, aad, al, out + hl, n, f->bytes + f->len - 8u) != 0) { return false; }
    *len = (uint16_t)(hl + n);
    return true;
}

/* The data frame we sent C, opened, its 802.11 and Mesh Control sequence numbers zeroed. */
static bool relayed_(uint8_t *out, uint16_t *len)
{
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (f != NULL && !f->is_mgmt && f->len > 40u && MAC_EQ(f->bytes + 4, C) && opened_(f, out, len))
        {
            out[1] &= (uint8_t)~0x40u; /* a host-sealed copy keeps it; a chip-sealed one too */
            out[22] = out[23] = 0;
            memset(out + 34, 0, 4);    /* Mesh Control seq: 30 MAC + 2 QoS + flags + TTL */
            return true;
        }
    }
    return false;
}

static void t_relay(enum link link)
{
    up_(link, /*fwd=*/true);
    printf("--- (2) %s: a fragmented unicast for another peer is relayed as it was sent whole ---\n",
           link_name_());
    struct msdu m;
    struct umac_mesh_ctrl mc = mc_(UMAC_MESH_CTRL_AE_NONE, NULL, NULL);
    msdu_(&m, &mc, 0x0800, 1000, 0x55);
    static uint8_t r1[1600], r2[1600];
    uint16_t n1 = 0, n2 = 0;
    simnode_outbox_clear();
    send_(tmpl_(C), &m, NULL, 0, NULL);
    const bool whole = relayed_(r1, &n1);
    const unsigned out_whole = simnode_outbox_count();
    mc = mc_(UMAC_MESH_CTRL_AE_NONE, NULL, NULL);
    msdu_(&m, &mc, 0x0800, 1000, 0x55);
    simnode_outbox_clear();
    const struct snap s = snap_();
    static const uint16_t cut3[] = { 300, 640 };
    send_(tmpl_(C), &m, cut3, 2, NULL);
    const bool frag = relayed_(r2, &n2);
    CHECK(whole && frag && simnode_outbox_count() == out_whole && n1 == n2 && memcmp(r1, r2, n1) == 0,
          "relayed once, identical but for its sequence numbers (whole %d/%u, fragmented %d/%u, "
          "%u frames, rxdrop %lu)", (int)whole, n1, (int)frag, n2, simnode_outbox_count(),
          (unsigned long)g_warthog_rxdrop_reason);
    CHECK(D(s, in) == 3u && D(s, ok) == 1u && simnode_ext_rx_count() == 0u,
          "  reassembled first (in %u ok %u), delivered to nobody here", D(s, in), D(s, ok));
}

/* ---- (3) leaf ------------------------------------------------------------- */

static void t_leaf(enum link link)
{
    up_(link, /*fwd=*/false);
    printf("--- (3) %s leaf: AE learning and the not-for-us drop run once, on the MSDU ---\n",
           link_name_());
    struct msdu m;
    struct umac_mesh_ctrl mc = mc_(UMAC_MESH_CTRL_AE_A5A6, W, H2);
    msdu_(&m, &mc, 0x0800, 1000, 0x66);
    /* At the cut: octets that read as an AE 2 Mesh Control naming H3 behind A. */
    static const uint16_t cut[] = { 500 };
    const struct umac_mesh_ctrl fake = { .flags = UMAC_MESH_CTRL_AE_A5A6, .ttl = 9, .seq = 1 };
    struct umac_mesh_ctrl fk = fake;
    memcpy(fk.eaddr1, W, 6);
    memcpy(fk.eaddr2, H3, 6);
    (void)umac_mesh_ctrl_build(m.b + 500, 18u, &fk);
    simnode_ext_rx_clear();
    send_(tmpl_(W), &m, cut, 1, NULL);
    struct ext e;
    uint8_t via[6] = { 0 };
    CHECK(simnode_ext_rx_count() == 1u && last_ext_(&e) && MAC_EQ(e.b + 6, H2),
          "a fragmented AE 2 frame from H2 behind A is delivered as H2's (%u delivered)",
          simnode_ext_rx_count());
    CHECK(umac_mesh_fwd_glue_proxy_via_peer(H2, via) && MAC_EQ(via, A), "  and H2 is learned behind A");
    CHECK(!umac_mesh_fwd_glue_proxy_via_peer(H3, via),
          "  H3, named only by payload octets at the start of the second fragment, is not");

    mc = mc_(UMAC_MESH_CTRL_AE_NONE, NULL, NULL);
    msdu_(&m, &mc, 0x0800, 1000, 0x77);
    const struct snap s = snap_();
    simnode_ext_rx_clear();
    send_(tmpl_(C), &m, cut, 1, NULL);
    CHECK(simnode_ext_rx_count() == 0u && g_warthog_rxdrop_reason == 93u &&
              g_warthog_rxdrop_count - s.rxdrop == 1u && D(s, ok) == 1u,
          "one for C is reassembled, then dropped once as not ours (reason %lu, %u drops)",
          (unsigned long)g_warthog_rxdrop_reason, (unsigned)(g_warthog_rxdrop_count - s.rxdrop));
}

/* ---- (4) batman ------------------------------------------------------------ */

static void t_batman(enum link link)
{
    up_(link, /*fwd=*/false);
    simnode_set_batman(true);
    printf("--- (4) %s batman: a fragmented batman frame reaches the hook intact ---\n", link_name_());
    struct msdu m;
    struct umac_mesh_ctrl mc = mc_(UMAC_MESH_CTRL_AE_NONE, NULL, NULL);
    msdu_(&m, &mc, 0x4305, 900, 0x5b);
    static const uint16_t cut[] = { 450 };
    same_as_whole_("batman (0x4305)", &m, cut, 1, false);
    simnode_set_batman(false);
}

/* ---- (5)-(9) FragAttacks ----------------------------------------------------- */

static void nothing_(const char *what, const struct snap *s, unsigned live0)
{
    CHECK(simnode_ext_rx_count() == 0u && D(*s, ok) == 0u, "  %s: nothing delivered (%u)", what,
          simnode_ext_rx_count());
    simnode_advance_run(1100);
    CHECK(simnode_live_allocs() == live0, "  and nothing is left allocated once a chain would "
          "expire (%u -> %u)", live0, simnode_live_allocs());
}

/* After an attack, A's next MSDU still goes through whole and fragmented. */
static void still_works_(void)
{
    struct msdu m;
    struct umac_mesh_ctrl mc = mc_(UMAC_MESH_CTRL_AE_NONE, NULL, NULL);
    msdu_(&m, &mc, 0x0800, 600, 0x99);
    static const uint16_t cut[] = { 300 };
    simnode_ext_rx_clear();
    send_(tmpl_(W), &m, cut, 1, NULL);
    CHECK(simnode_ext_rx_count() == 1u, "  A's next fragmented MSDU is delivered (%u, rxdrop %lu)",
          simnode_ext_rx_count(), (unsigned long)g_warthog_rxdrop_reason);
}

static void t_crypto(void)
{
    printf("--- (5) SAE: PN gap, key replaced under a chain, two key ids ---\n");
#if !SAE_OPENS
    up_(LINK_SAE, false);
    {
        struct msdu m;
        struct umac_mesh_ctrl mc = mc_(UMAC_MESH_CTRL_AE_NONE, NULL, NULL);
        msdu_(&m, &mc, 0x0800, 600, 0x01);
        static const uint16_t cut[] = { 300 };
        const struct snap s = snap_();
        send_(tmpl_(W), &m, cut, 1, NULL);
        CHECK(simnode_ext_rx_count() == 0u && D(s, in) == 0u && g_warthog_rxdrop_reason == 4u,
              "(pin) this build opens no SAE unicast, fragments included: none reaches "
              "reassembly (in %u, reason %lu)", D(s, in), (unsigned long)g_warthog_rxdrop_reason);
    }
    return;
#else
    struct msdu m;
    struct umac_mesh_ctrl mc;
    static uint8_t f[3200];
    static const uint16_t cut[] = { 300 };
    uint16_t n;

    /* PN gap. */
    up_(LINK_SAE, false);
    unsigned live0 = simnode_live_allocs();
    mc = mc_(UMAC_MESH_CTRL_AE_NONE, NULL, NULL);
    msdu_(&m, &mc, 0x0800, 600, 0x02);
    struct snap s = snap_();
    struct mpdu p = tmpl_(W);
    p.seq = s_seq++;
    p.frag = 0; p.more = true; p.pn = s_pn;
    n = frag_(f, &p, &m, cut, 1, 0);
    rx_(f, n, true);
    p.frag = 1; p.more = false; p.pn = s_pn + 2u;
    n = frag_(f, &p, &m, cut, 1, 1);
    rx_(f, n, true);
    s_pn += 3u;
    CHECK(D(s, pn) == 1u && g_warthog_rxdrop_reason == 97u,
          "(red) a second fragment at PN+2 is refused (pn %u, reason %lu)", D(s, pn),
          (unsigned long)g_warthog_rxdrop_reason);
    nothing_("PN gap", &s, live0);
    still_works_();

    /* The pairwise key replaced between two fragments, as AMPE re-keying does. */
    up_(LINK_SAE, false);
    live0 = simnode_live_allocs();
    mc = mc_(UMAC_MESH_CTRL_AE_NONE, NULL, NULL);
    msdu_(&m, &mc, 0x0800, 600, 0x03);
    s = snap_();
    p = tmpl_(W);
    p.seq = s_seq++;
    p.frag = 0; p.more = true; p.pn = s_pn++;
    n = frag_(f, &p, &m, cut, 1, 0);
    rx_(f, n, true);
    (void)simnode_set_key(A, K_A2, 0, /*pairwise=*/true);
    p.frag = 1; p.more = false; p.pn = s_pn++; p.key = K_A2;
    n = frag_(f, &p, &m, cut, 1, 1);
    rx_(f, n, true);
    CHECK(D(s, flush) == 1u && D(s, nofirst) == 1u && simnode_ext_rx_count() == 0u,
          "(red) a new MTK for A flushes A's chain, so its next fragment under the new key has "
          "none to join (flush %u, nofirst %u)", D(s, flush), D(s, nofirst));
    nothing_("key replaced", &s, live0);

    /* The keychain's key replaced by a path that flushes nothing (STA/AP's supplicant installs
     * keys through umac_keys): the chain's key is not the fragment's. */
    up_(LINK_SAE, false);
    live0 = simnode_live_allocs();
    mc = mc_(UMAC_MESH_CTRL_AE_NONE, NULL, NULL);
    msdu_(&m, &mc, 0x0800, 600, 0x04);
    s = snap_();
    p = tmpl_(W);
    p.seq = s_seq++;
    p.frag = 0; p.more = true; p.pn = s_pn++;
    n = frag_(f, &p, &m, cut, 1, 0);
    rx_(f, n, true);
    {
        struct umac_key k = { .key_id = 0, .key_type = UMAC_KEY_TYPE_PAIRWISE, .key_len = 16 };
        memcpy(k.key_data, K_A3, 16);
        struct umac_sta_data *stad = umac_datapath_mesh_find_peer(A);
        CHECK(stad != NULL && connection_keys_install_key(&umac_sta_data_get_keys(stad)->keys, &k),
              "  A's pairwise key replaced on the keychain alone");
    }
    /* The chip build's chip still holds K_A, so it opens the next fragment under it. */
    p.frag = 1; p.more = false; p.pn = s_pn++; p.key = SAE_CHIP ? K_A : K_A3;
    n = frag_(f, &p, &m, cut, 1, 1);
    rx_(f, n, true);
    CHECK(D(s, key) == 1u && g_warthog_rxdrop_reason == 97u,
          "(red) the next fragment is refused: its chain began under another key (key %u, "
          "reason %lu)", D(s, key), (unsigned long)g_warthog_rxdrop_reason);
    nothing_("key colour", &s, live0);

    /* Two pairwise key ids at once: the fragments of one MSDU must share one. */
    up_(LINK_SAE, false);
    live0 = simnode_live_allocs();
    (void)simnode_set_key(A, K_A2, 1, /*pairwise=*/true);
    mc = mc_(UMAC_MESH_CTRL_AE_NONE, NULL, NULL);
    msdu_(&m, &mc, 0x0800, 600, 0x05);
    s = snap_();
    p = tmpl_(W);
    p.seq = s_seq++;
    p.frag = 0; p.more = true; p.pn = s_pn++; p.key = K_A2; p.kid = 1;
    n = frag_(f, &p, &m, cut, 1, 0);
    rx_(f, n, true);
    p.frag = 1; p.more = false; p.pn = s_pn++; p.key = K_A; p.kid = 0;
    n = frag_(f, &p, &m, cut, 1, 1);
    rx_(f, n, true);
#if SAE_CHIP
    CHECK(g_warthog_rxdrop_reason == 96u,
          "(pin) under key id 0 while the link's key is id 1, the chip build refuses it (96)");
#else
    CHECK(D(s, key) == 1u && g_warthog_rxdrop_reason == 97u,
          "(red) a second fragment under the other key id is refused (key %u, reason %lu)",
          D(s, key), (unsigned long)g_warthog_rxdrop_reason);
#endif
    nothing_("two key ids", &s, live0);
#endif
}

static void t_plain(void)
{
    printf("--- (6) SAE: a plaintext fragment on a keyed link ---\n");
    up_(LINK_SAE, false);
    const unsigned live0 = simnode_live_allocs();
    struct msdu m;
    struct umac_mesh_ctrl mc = mc_(UMAC_MESH_CTRL_AE_NONE, NULL, NULL);
    msdu_(&m, &mc, 0x0800, 600, 0x06);
    static const uint16_t cut[] = { 300 };
    memcpy(m.b + 300, SNAP_EAPOL, sizeof(SNAP_EAPOL)); /* the plaintext one starts like EAPOL */
    static uint8_t f[3200];
    struct snap s = snap_();
    struct mpdu p = tmpl_(W);
    p.seq = s_seq++;
    p.frag = 0; p.more = true; p.pn = s_pn++;
    uint16_t n = frag_(f, &p, &m, cut, 1, 0);
    rx_(f, n, true);
    p.frag = 1; p.more = false; p.prot = false;
    n = frag_(f, &p, &m, cut, 1, 1);
    rx_(f, n, false);
    CHECK(D(s, plain) == 1u && g_warthog_rxdrop_reason == 98u,
          "(red) a protected first fragment, then one in the clear that starts like EAPOL: "
          "refused (plain %u, reason %lu)", D(s, plain), (unsigned long)g_warthog_rxdrop_reason);
    CHECK(simnode_stub_hits("umac_supp_l2_sock_receive") == 0u, "  and the supplicant never sees it");
    nothing_("plaintext second", &s, live0);

    /* Plaintext first, starting like EAPOL; then a protected second. */
    memcpy(m.b, SNAP_EAPOL, sizeof(SNAP_EAPOL));
    s = snap_();
    p = tmpl_(W);
    p.seq = s_seq++;
    p.frag = 0; p.more = true; p.prot = false;
    n = frag_(f, &p, &m, cut, 1, 0);
    rx_(f, n, false);
    p.frag = 1; p.more = false; p.prot = true; p.pn = s_pn++;
    n = frag_(f, &p, &m, cut, 1, 1);
    rx_(f, n, true);
    CHECK(D(s, plain) == 1u && simnode_stub_hits("umac_supp_l2_sock_receive") == 0u,
          "(red) a plaintext first fragment that starts like EAPOL is refused (plain %u), and the "
          "supplicant never sees it", D(s, plain));
    CHECK(SAE_OPENS ? D(s, nofirst) == 1u : D(s, in) == 0u,
          "  the protected second has no chain to join (nofirst %u)", D(s, nofirst));
    nothing_("plaintext first", &s, live0);
#if SAE_OPENS
    still_works_();
#endif
}

static void t_header(enum link link)
{
    up_(link, false);
    printf("--- (7) %s: later fragments must match the first; group and A-MSDU fragments ---\n",
           link_name_());
    unsigned live0 = simnode_live_allocs();
    struct msdu m;
    struct umac_mesh_ctrl mc = mc_(UMAC_MESH_CTRL_AE_NONE, NULL, NULL);
    msdu_(&m, &mc, 0x0800, 600, 0x07);
    static const uint16_t cut[] = { 300 };
    static uint8_t f[3200];
    uint16_t n;

    /* addr4 (the mesh source) changed on the second fragment, sealed as such by its sender. */
    struct snap s = snap_();
    struct mpdu p = tmpl_(W);
    p.seq = s_seq++;
    p.frag = 0; p.more = true; p.pn = s_pn++;
    n = frag_(f, &p, &m, cut, 1, 0);
    rx_(f, n, p.prot);
    p.frag = 1; p.more = false; p.pn = s_pn++; p.sa = H3;
    n = frag_(f, &p, &m, cut, 1, 1);
    rx_(f, n, p.prot);
    CHECK(D(s, hdr) == 1u && g_warthog_rxdrop_reason == 97u,
          "(red) a second fragment naming another mesh source is refused (hdr %u, reason %lu)",
          D(s, hdr), (unsigned long)g_warthog_rxdrop_reason);
    nothing_("other mesh source", &s, live0);

    /* A-MSDU Present on a fragment. */
    s = snap_();
    p = tmpl_(W);
    p.seq = s_seq++;
    p.frag = 0; p.more = true; p.pn = s_pn++; p.qos0 = 0x80;
    n = frag_(f, &p, &m, cut, 1, 0);
    rx_(f, n, p.prot);
    p.frag = 1; p.more = false; p.pn = s_pn++;
    n = frag_(f, &p, &m, cut, 1, 1);
    rx_(f, n, p.prot);
    CHECK(D(s, amsdu) >= 1u, "(red) fragments with A-MSDU Present are refused (amsdu %u)", D(s, amsdu));
    nothing_("A-MSDU", &s, live0);

    if (link == LINK_OPEN)
    {
        /* Group-addressed fragments: the more-fragments one, and a last fragment alone. */
        s = snap_();
        uint16_t k = umac_mesh_ies_build_data_hdr3_group(f, BC, A, A);
        f[1] |= 0x04u;
        f[22] = (uint8_t)(s_seq << 4); f[23] = (uint8_t)(s_seq >> 4);
        s_seq++;
        f[k++] = 0x00; f[k++] = 0x01;
        memcpy(f + k, m.b, 300);
        (void)simnode_rx(f, (uint16_t)(k + 300u), -70);
        CHECK(D(s, mcast) == 1u && g_warthog_rxdrop_reason == 7u,
              "(pin) a group frame with More Fragments is dropped (mcast %u, reason %lu)",
              D(s, mcast), (unsigned long)g_warthog_rxdrop_reason);
        k = umac_mesh_ies_build_data_hdr3_group(f, BC, A, A);
        f[22] = (uint8_t)((s_seq << 4) | 1u); f[23] = (uint8_t)(s_seq >> 4);
        s_seq++;
        f[k++] = 0x00; f[k++] = 0x01;
        memcpy(f + k, m.b, 300);
        simnode_ext_rx_clear();
        (void)simnode_rx(f, (uint16_t)(k + 300u), -70);
        CHECK(D(s, mcast) == 2u && g_warthog_rxdrop_reason == 7u && simnode_ext_rx_count() == 0u,
              "(red) and so is a group frame with fragment number 1 and no More Fragments, "
              "never delivered as a whole frame (mcast %u, %u delivered)", D(s, mcast),
              simnode_ext_rx_count());
    }
    still_works_();
}

static void t_order(enum link link)
{
    up_(link, false);
    printf("--- (8) %s: fragments out of order, and a missing first ---\n", link_name_());
    const unsigned live0 = simnode_live_allocs();
    struct msdu m;
    struct umac_mesh_ctrl mc = mc_(UMAC_MESH_CTRL_AE_NONE, NULL, NULL);
    msdu_(&m, &mc, 0x0800, 900, 0x08);
    static const uint16_t cut[] = { 300 };
    static const uint16_t cut3[] = { 300, 600 };
    static uint8_t f[3200];
    uint16_t n;

    /* The second before the first. */
    struct snap s = snap_();
    struct mpdu p = tmpl_(W);
    p.seq = s_seq++;
    const uint64_t pn0 = s_pn;
    s_pn += 2u;
    p.frag = 1; p.more = false; p.pn = pn0 + 1u;
    n = frag_(f, &p, &m, cut, 1, 1);
    rx_(f, n, p.prot);
    p.frag = 0; p.more = true; p.pn = pn0;
    n = frag_(f, &p, &m, cut, 1, 0);
    rx_(f, n, p.prot);
    CHECK(D(s, nofirst) == 1u,
          "(pin) a second fragment before its first has no chain (nofirst %u)", D(s, nofirst));
    nothing_("second before first", &s, live0);

    if (link == LINK_OPEN)
    {
        /* 0, 2, 1, then 2 again: the skip is dropped, the chain kept, the resend completes it. */
        struct ext whole, got;
        simnode_ext_rx_clear();
        send_(tmpl_(W), &m, NULL, 0, NULL);
        const bool ok_whole = last_ext_(&whole);
        simnode_ext_rx_clear();
        s = snap_();
        p = tmpl_(W);
        p.seq = s_seq++;
        const unsigned order[] = { 0, 2, 1, 2 };
        for (unsigned i = 0; i < 4; i++)
        {
            p.frag = (uint8_t)order[i];
            p.more = order[i] < 2u;
            p.retry = i == 3u;
            n = frag_(f, &p, &m, cut3, 2, order[i]);
            rx_(f, n, false);
        }
        CHECK(D(s, order) == 1u && simnode_ext_rx_count() == 1u && ok_whole && last_ext_(&got) &&
                  got.len == whole.len && memcmp(got.b, whole.b, whole.len) == 0,
              "(red) fragments 0, 2, 1, 2: the skip is refused (order %u), the chain kept, and "
              "the resent 2 completes it, delivered intact (%u)", D(s, order), simnode_ext_rx_count());
    }
    else
    {
        /* Out of order under CCMP: the third fragment's PN moves the replay counter past the
         * second's, which is then a replay; nothing completes. */
        s = snap_();
        p = tmpl_(W);
        p.seq = s_seq++;
        const uint64_t q = s_pn;
        s_pn += 3u;
        const unsigned order[] = { 0, 2, 1 };
        for (unsigned i = 0; i < 3; i++)
        {
            p.frag = (uint8_t)order[i];
            p.more = order[i] < 2u;
            p.pn = q + order[i];
            n = frag_(f, &p, &m, cut3, 2, order[i]);
            rx_(f, n, true);
        }
        CHECK(D(s, order) == 1u && simnode_ext_rx_count() == 0u,
              "(red) keyed, fragments 0, 2, 1: the skip is refused (order %u), the late one is a "
              "replay (reason %lu)", D(s, order), (unsigned long)g_warthog_rxdrop_reason);
        nothing_("0, 2, 1", &s, live0);
    }

    /* A second fragment whose first never came. */
    s = snap_();
    p = tmpl_(W);
    p.seq = s_seq++;
    p.frag = 1; p.more = false; p.pn = s_pn++;
    n = frag_(f, &p, &m, cut, 1, 1);
    rx_(f, n, p.prot);
    CHECK(D(s, nofirst) == 1u && g_warthog_rxdrop_reason == 97u,
          "(pin) a lone second fragment is dropped (nofirst %u, reason %lu)", D(s, nofirst),
          (unsigned long)g_warthog_rxdrop_reason);
    still_works_();
}

static void t_chains(enum link link)
{
    up_(link, false);
    printf("--- (9) %s: expiry, oversize, restart and a peer that leaves ---\n", link_name_());
    unsigned live0 = simnode_live_allocs();
    struct msdu m;
    struct umac_mesh_ctrl mc = mc_(UMAC_MESH_CTRL_AE_NONE, NULL, NULL);
    msdu_(&m, &mc, 0x0800, 900, 0x09);
    static const uint16_t cut[] = { 450 };
    static uint8_t f[3200];
    uint16_t n;

    /* The timer. */
    struct snap s = snap_();
    struct mpdu p = tmpl_(W);
    p.seq = s_seq++;
    p.frag = 0; p.more = true; p.pn = s_pn++;
    n = frag_(f, &p, &m, cut, 1, 0);
    rx_(f, n, p.prot);
    simnode_advance_run(1100);
    CHECK(D(s, expired) == 1u && simnode_live_allocs() == live0,
          "a chain expires 1 s after its first fragment, freed (expired %u, %u -> %u live)",
          D(s, expired), live0, simnode_live_allocs());
    p.frag = 1; p.more = false; p.pn = s_pn++;
    n = frag_(f, &p, &m, cut, 1, 1);
    rx_(f, n, p.prot);
    CHECK(D(s, nofirst) == 1u && simnode_ext_rx_count() == 0u,
          "  its second fragment then has none to join (nofirst %u)", D(s, nofirst));

    /* No timer: the core's pool was empty when the chain began. */
    s = snap_();
    p = tmpl_(W);
    p.seq = s_seq++;
    p.frag = 0; p.more = true; p.pn = s_pn++;
    n = frag_(f, &p, &m, cut, 1, 0);
    simnode_fail_next_timeout();
    rx_(f, n, p.prot);
    simnode_advance_ms(1100);
    p.frag = 1; p.more = false; p.pn = s_pn++;
    n = frag_(f, &p, &m, cut, 1, 1);
    rx_(f, n, p.prot);
    CHECK(D(s, expired) == 1u && simnode_ext_rx_count() == 0u && simnode_live_allocs() == live0,
          "(red) with no timer armed, a second fragment 1.1 s after the first finds it expired "
          "(expired %u, %u delivered, %u -> %u live)", D(s, expired), simnode_ext_rx_count(),
          live0, simnode_live_allocs());

    /* Larger than the reassembly buffer. */
    struct msdu big;
    mc = mc_(UMAC_MESH_CTRL_AE_NONE, NULL, NULL);
    msdu_(&big, &mc, 0x0800, 2700, 0x0a);
    static const uint16_t cutb[] = { 900, 1800 };
    s = snap_();
    send_(tmpl_(W), &big, cutb, 2, NULL);
    CHECK(D(s, oversize) == 1u && simnode_ext_rx_count() == 0u,
          "(red) three fragments totalling %u octets are dropped (oversize %u)", big.len, D(s, oversize));
    nothing_("oversize", &s, live0);

    /* A new first fragment while a chain is incomplete. */
    s = snap_();
    p = tmpl_(W);
    p.seq = s_seq++;
    p.frag = 0; p.more = true; p.pn = s_pn++;
    n = frag_(f, &p, &m, cut, 1, 0);
    rx_(f, n, p.prot);
    struct ext whole, got;
    simnode_ext_rx_clear();
    send_(tmpl_(W), &m, NULL, 0, NULL);
    const bool ok_whole = last_ext_(&whole);
    simnode_ext_rx_clear();
    send_(tmpl_(W), &m, cut, 1, NULL);
    CHECK(D(s, restart) == 1u && ok_whole && simnode_ext_rx_count() == 1u && last_ext_(&got) &&
              got.len == whole.len && memcmp(got.b, whole.b, whole.len) == 0,
          "(red) a later MSDU's first fragment replaces an incomplete chain (restart %u) and is "
          "delivered intact", D(s, restart));

    /* A peer that leaves mid-chain. */
    simnode_ext_rx_clear();
    s = snap_();
    p = tmpl_(W);
    p.seq = s_seq++;
    p.frag = 0; p.more = true; p.pn = s_pn++;
    n = frag_(f, &p, &m, cut, 1, 0);
    rx_(f, n, p.prot);
    const unsigned live_chain = simnode_live_allocs();
    simnode_del_peer(A);
    CHECK(D(s, flush) == 1u && simnode_live_allocs() + 2u == live_chain,
          "(red) a peer that leaves takes its chain with it (flush %u; its record and the chain "
          "freed: %u -> %u live)", D(s, flush), live_chain, simnode_live_allocs());
    (void)simnode_add_peer(A);
    if (link == LINK_SAE)
    {
        (void)simnode_set_key(A, K_A, 0, /*pairwise=*/true);
        s_pn = 1;
    }
    p.frag = 1; p.more = false; p.pn = s_pn++;
    n = frag_(f, &p, &m, cut, 1, 1);
    rx_(f, n, p.prot);
    CHECK(simnode_ext_rx_count() == 0u && D(s, nofirst) == 1u,
          "  so A's second fragment after it re-peers joins nothing (nofirst %u, %u delivered)",
          D(s, nofirst), simnode_ext_rx_count());
    live0 = simnode_live_allocs();
    still_works_();
    simnode_advance_run(1100);
    CHECK(simnode_live_allocs() == live0, "  nothing left allocated (%u -> %u)", live0,
          simnode_live_allocs());
}

/* ---- (10) the duplicate filter and Block Ack ------------------------------------ */

/* A peer's NDP ADDBA Request for TID 0: immediate policy, 16 frames, starting sequence 0. */
static uint16_t mk_addba_req(uint8_t *f, const uint8_t *ta, uint16_t ssn)
{
    memset(f, 0, 33);
    f[0] = 0xd0;
    memcpy(&f[4], W, 6);
    memcpy(&f[10], ta, 6);
    memcpy(&f[16], ta, 6);
    const uint16_t ps = (uint16_t)((1u << 1) | (16u << 6));
    f[24] = 3;   /* Block Ack */
    f[25] = 128; /* NDP ADDBA Request */
    f[26] = 9;
    f[27] = (uint8_t)ps;
    f[28] = (uint8_t)(ps >> 8);
    f[31] = (uint8_t)(ssn << 4);
    f[32] = (uint8_t)(ssn >> 4);
    return 33u;
}

static void t_dup_ba(void)
{
    up_(LINK_OPEN, false);
    printf("--- (10) open: the duplicate filter and Block Ack ---\n");
    struct msdu m;
    struct umac_mesh_ctrl mc = mc_(UMAC_MESH_CTRL_AE_NONE, NULL, NULL);
    msdu_(&m, &mc, 0x0800, 900, 0x0b);
    static const uint16_t cut3[] = { 300, 600 };
    static uint8_t f[3200];
    uint16_t n;
    struct ext whole, got;
    simnode_ext_rx_clear();
    send_(tmpl_(W), &m, NULL, 0, NULL);
    const bool ok_whole = last_ext_(&whole);

    struct snap s = snap_();
    simnode_ext_rx_clear();
    send_(tmpl_(W), &m, cut3, 2, NULL);
    CHECK(g_warthog_filt_hist[8] == s.dup, "(pin) three fragments of one sequence number: no "
          "duplicate (dup %u)", (unsigned)(g_warthog_filt_hist[8] - s.dup));

    s = snap_();
    simnode_ext_rx_clear();
    struct mpdu p = tmpl_(W);
    p.seq = s_seq++;
    const unsigned order[] = { 0, 1, 1, 2 };
    for (unsigned i = 0; i < 4; i++)
    {
        p.frag = (uint8_t)order[i];
        p.more = order[i] < 2u;
        p.retry = i == 2u;
        n = frag_(f, &p, &m, cut3, 2, order[i]);
        rx_(f, n, false);
    }
    CHECK(g_warthog_filt_hist[8] - s.dup == 1u, "(pin) a retransmitted second fragment is a "
          "duplicate (dup %u)", (unsigned)(g_warthog_filt_hist[8] - s.dup));
    CHECK(ok_whole && simnode_ext_rx_count() == 1u && last_ext_(&got) && got.len == whole.len &&
              memcmp(got.b, whole.b, whole.len) == 0,
          "(red) and the MSDU is delivered once, intact (%u)", simnode_ext_rx_count());

    /* Block Ack: A opens a session at sequence 0. */
    up_(LINK_OPEN, false);
    simnode_set_ampdu(true);
    n = mk_addba_req(f, A, 0);
    (void)simnode_rx(f, n, -60);
    s_seq = 0;
    s = snap_();
    simnode_ext_rx_clear();
    send_(tmpl_(W), &m, cut3, 2, NULL); /* sequence 0 */
    const bool ba0 = simnode_ext_rx_count() == 1u && last_ext_(&got) && got.len == whole.len &&
                     memcmp(got.b, whole.b, whole.len) == 0;
    send_(tmpl_(W), &m, cut3, 2, NULL); /* sequence 1 */
    CHECK(ba0 && simnode_ext_rx_count() == 2u && g_warthog_reord_outdated == s.outdated,
          "(red) under Block Ack two fragmented MSDUs are delivered intact, the second not "
          "outdated (%u delivered, outdated %u)", simnode_ext_rx_count(),
          (unsigned)(g_warthog_reord_outdated - s.outdated));
    /* The second fragment of sequence 2 before its first: it waits in the reorder window. */
    s = snap_();
    simnode_ext_rx_clear();
    p = tmpl_(W);
    p.seq = s_seq++;
    const unsigned ba_order[] = { 1, 0, 2 };
    unsigned before = 0;
    for (unsigned i = 0; i < 3; i++)
    {
        p.frag = (uint8_t)ba_order[i];
        p.more = ba_order[i] < 2u;
        n = frag_(f, &p, &m, cut3, 2, ba_order[i]);
        rx_(f, n, false);
        if (i == 1) { before = simnode_ext_rx_count(); }
    }
    CHECK(g_warthog_reord_buffered - s.buffered >= 1u && before == 0u &&
              simnode_ext_rx_count() == 1u && last_ext_(&got) && got.len == whole.len &&
              memcmp(got.b, whole.b, whole.len) == 0 && D(s, nofirst) == 0u,
          "(red) a second fragment received before its first waits in the reorder window and "
          "the MSDU is delivered intact (buffered %u, nofirst %u)",
          (unsigned)(g_warthog_reord_buffered - s.buffered), D(s, nofirst));
    simnode_set_ampdu(false);
    s_seq = 100;
}

/* ---- (11)-(12) how many chains, and for how long ------------------------------------------ */

static const uint8_t *key_of_(const uint8_t *ta)
{
    return MAC_EQ(ta, A) ? K_A : MAC_EQ(ta, C) ? K_C : K_D;
}

static uint64_t next_pn_(const uint8_t *ta)
{
    return MAC_EQ(ta, A) ? s_pn++ : MAC_EQ(ta, C) ? s_pn_c++ : s_pn_d++;
}

/* Fragment 0 of @p m, cut once at @p cut, from peer @p ta to us on QoS TID @p tid; its MPDU,
 * for second_(). */
static struct mpdu first_(const uint8_t *ta, uint8_t tid, const struct msdu *m, const uint16_t *cut)
{
    static uint8_t f[3200];
    struct mpdu p = tmpl_(W);
    p.ta = ta;
    p.sa = ta;
    p.key = key_of_(ta);
    p.qos0 = tid;
    p.seq = s_seq++;
    p.frag = 0; p.more = true; p.pn = next_pn_(ta);
    rx_(f, frag_(f, &p, m, cut, 1, 0), p.prot);
    return p;
}

/* Its second fragment, at the next PN after the first's: the replay counter is per TID. */
static void second_(struct mpdu p, const struct msdu *m, const uint16_t *cut)
{
    static uint8_t f[3200];
    uint64_t *next = MAC_EQ(p.ta, A) ? &s_pn : MAC_EQ(p.ta, C) ? &s_pn_c : &s_pn_d;
    p.frag = 1; p.more = false; p.pn = p.pn + 1u;
    if (*next <= p.pn) { *next = p.pn + 1u; }
    rx_(f, frag_(f, &p, m, cut, 1, 1), p.prot);
}

/* A whole 200-octet MSDU from peer @p ta to us. */
static void whole_(const uint8_t *ta)
{
    static uint8_t f[3200];
    struct msdu m;
    struct umac_mesh_ctrl mc = mc_(UMAC_MESH_CTRL_AE_NONE, NULL, NULL);
    msdu_(&m, &mc, 0x0800, 200, 0x3c);
    struct mpdu p = tmpl_(W);
    p.ta = ta;
    p.sa = ta;
    p.key = key_of_(ta);
    p.seq = s_seq++;
    p.pn = next_pn_(ta);
    rx_(f, mpdu_(f, &p, m.b, m.len), p.prot);
}

static void t_timerless(enum link link)
{
    up_(link, false);
    printf("--- (11) %s: a chain the core had no timeout for still goes 1 s after its first ---\n",
           link_name_());
    struct msdu m;
    struct umac_mesh_ctrl mc = mc_(UMAC_MESH_CTRL_AE_NONE, NULL, NULL);
    msdu_(&m, &mc, 0x0800, 600, 0x1a);
    static const uint16_t cut[] = { 300 };
    const unsigned live0 = simnode_live_allocs();

    /* Whole frames within its second: the receive path arms the expiry it lacked. */
    struct snap s = snap_();
    simnode_fail_next_timeout();
    (void)first_(A, 0, &m, cut);
    const unsigned held = simnode_live_allocs();
    simnode_advance_ms(400);
    whole_(C);
    whole_(A);
    simnode_advance_run(700);
    CHECK(held == live0 + 1u && D(s, expired) == 1u && simnode_live_allocs() == live0 &&
              umac_datapath_mesh_find_peer(A) != NULL,
          "(red) no timeout for it, whole frames from C and A within its second, nothing after: "
          "freed 1 s after its first fragment, A still a peer (expired %u, %u -> %u -> %u live)",
          D(s, expired), live0, held, simnode_live_allocs());

    /* Nothing at all: the mesh service tick sweeps. */
    s = snap_();
    simnode_fail_next_timeout();
    (void)first_(A, 1, &m, cut);
    simnode_advance_ms(1100);
    simnode_tick();
    CHECK(D(s, expired) == 1u && simnode_live_allocs() == live0,
          "(red) no timeout and no traffic: the service tick after its second frees it (expired %u, "
          "%u -> %u live)", D(s, expired), live0, simnode_live_allocs());

    /* The next data frame from anyone, after its second. */
    s = snap_();
    simnode_fail_next_timeout();
    (void)first_(A, 2, &m, cut);
    simnode_advance_ms(1100);
    whole_(C);
    CHECK(D(s, expired) == 1u && simnode_live_allocs() == live0,
          "(red) no timeout: a whole frame from C after its second frees it (expired %u, %u -> %u live)",
          D(s, expired), live0, simnode_live_allocs());
    still_works_();
}

static void t_caps(enum link link)
{
    up_(link, false);
    printf("--- (12) %s: at most 2 chains a peer and 4 on the node, under one timeout ---\n",
           link_name_());
    struct msdu m;
    struct umac_mesh_ctrl mc = mc_(UMAC_MESH_CTRL_AE_NONE, NULL, NULL);
    msdu_(&m, &mc, 0x0800, 600, 0x1b);
    static const uint16_t cut[] = { 300 };
    const unsigned live0 = simnode_live_allocs();
    const unsigned pend0 = simnode_timeouts_pending();
    struct snap s = snap_();
    struct mpdu pa[8], pc[8];
    for (uint8_t t = 0; t < 8u; t++) { pa[t] = first_(A, t, &m, cut); simnode_advance_ms(1); }
    for (uint8_t t = 0; t < 8u; t++) { pc[t] = first_(C, t, &m, cut); simnode_advance_ms(1); }
    const unsigned pend = simnode_timeouts_pending();
    CHECK(D(s, in) == 16u && simnode_live_allocs() == live0 + 4u && pend <= pend0 + 1u &&
              D(s, evict) == 12u,
          "(red) a first fragment on each of TIDs 0-7 from A, then from C: 4 chains held (+%u "
          "live), %u evicted, timeouts pending %u -> %u", simnode_live_allocs() - live0,
          D(s, evict), pend0, pend);
    simnode_ext_rx_clear();
    second_(pa[7], &m, cut);
    second_(pc[6], &m, cut);
    const unsigned done = simnode_ext_rx_count();
    second_(pa[0], &m, cut);
    CHECK(done == 2u && simnode_ext_rx_count() == 2u && D(s, ok) == 2u && D(s, nofirst) == 1u,
          "  a peer's 2 newest complete and are delivered (%u); an evicted chain's second fragment "
          "joins nothing (nofirst %u)", done, D(s, nofirst));
    simnode_advance_run(1100);
    CHECK(simnode_live_allocs() == live0 && simnode_timeouts_pending() <= pend0 && D(s, expired) == 2u,
          "  the other 2 expire after 1 s: nothing left allocated or pending (expired %u, %u -> %u "
          "live, %u pending)", D(s, expired), live0, simnode_live_allocs(), simnode_timeouts_pending());

    /* A third peer's first fragment with the node's 4 taken: the oldest chain goes. */
    (void)simnode_add_peer(DD);
    if (link == LINK_SAE)
    {
        (void)simnode_set_key(DD, K_D, 0, /*pairwise=*/true);
    }
    s_pn_d = 1;
    const unsigned live1 = simnode_live_allocs(); /* D's record too */
    s = snap_();
    struct mpdu q[5];
    q[0] = first_(A, 0, &m, cut); simnode_advance_ms(1);
    q[1] = first_(A, 1, &m, cut); simnode_advance_ms(1);
    q[2] = first_(C, 0, &m, cut); simnode_advance_ms(1);
    q[3] = first_(C, 1, &m, cut); simnode_advance_ms(1);
    q[4] = first_(DD, 0, &m, cut);
    CHECK(D(s, evict) == 1u && simnode_live_allocs() == live1 + 4u,
          "(red) A and C hold 2 each; a first fragment from D evicts the node's oldest (evict %u, "
          "+%u live)", D(s, evict), simnode_live_allocs() - live1);
    simnode_ext_rx_clear();
    for (unsigned i = 0; i < 5u; i++) { second_(q[i], &m, cut); }
    CHECK(D(s, nofirst) == 1u && D(s, ok) == 4u && simnode_ext_rx_count() == 4u &&
              simnode_live_allocs() == live1,
          "  A's oldest joins nothing (nofirst %u); the other 4 complete (ok %u, %u delivered, "
          "%u -> %u live)", D(s, nofirst), D(s, ok), simnode_ext_rx_count(), live1,
          simnode_live_allocs());
    simnode_del_peer(DD);
}

/* ---- (13) QoS bits 8-15 in a BSS ------------------------------------------------------------ */

/* One QoS data fragment from A straight into datapath_defrag, as the receive path hands it over:
 * DS bits @p ds (0x01 ToDS: a BSS uplink; 0x03 the mesh), QoS Control @p qos, @p n body octets;
 * @p out_len gets a completed MSDU's length. */
static struct mmpkt *direct_(struct umac_data *umacd, struct datapath_defrag_data *dd, uint8_t ds,
                             uint16_t seq, uint8_t frag, bool more, uint16_t qos, bool mesh,
                             const uint8_t *body, uint16_t n, uint16_t *out_len)
{
    uint8_t h[32] = { 0 };
    const uint16_t hl = ds == 0x03u ? 30u : 24u;
    h[0] = 0x88;
    h[1] = (uint8_t)(ds | (more ? 0x04u : 0u));
    memcpy(h + 4, W, 6);
    memcpy(h + 10, A, 6);
    memcpy(h + 16, C, 6);
    if (hl == 30u) { memcpy(h + 24, A, 6); }
    h[22] = (uint8_t)((seq << 4) | frag);
    h[23] = (uint8_t)(seq >> 4);
    h[hl] = (uint8_t)qos;
    h[hl + 1u] = (uint8_t)(qos >> 8);
    struct mmpkt *pkt = mmpkt_alloc_on_heap(0, (uint32_t)hl + 2u + n, 0);
    struct mmpktview *v = mmpkt_open(pkt);
    mmpkt_append_data(v, h, (uint32_t)hl + 2u);
    mmpkt_append_data(v, body, n);
    const struct dot11_data_hdr *dh = (const struct dot11_data_hdr *)mmpkt_remove_from_start(v, hl);
    (void)mmpkt_remove_from_start(v, 2);
    const struct datapath_defrag_mpdu mp = { .qos = qos, .mesh = mesh };
    struct mmpkt *out = datapath_defrag(umacd, dd, &dh, &v, pkt, (uint8_t)(qos & 0x0fu), &mp);
    if (out != NULL)
    {
        if (out_len != NULL) { *out_len = (uint16_t)mmpkt_get_data_length(v); }
        mmpkt_close(&v);
    }
    return out;
}

static void t_bss_qos(void)
{
    up_(LINK_OPEN, false);
    printf("--- (13) QoS bits 8-15 between fragments: Mesh Control Present on the mesh only ---\n");
    struct umac_data *umacd = umac_sta_data_get_umacd(umac_datapath_mesh_find_peer(A));
    static struct datapath_defrag_data dd; /* a station of a BSS, as the AP datapath holds one */
    const unsigned live0 = simnode_live_allocs();
    uint8_t body[400];
    for (unsigned i = 0; i < sizeof(body); i++) { body[i] = (uint8_t)(i * 3u + 1u); }
    uint16_t len = 0;

    /* Queue Size 0 then 1 (bit 8), then 0 then 2 (bit 9). */
    struct snap s = snap_();
    (void)direct_(umacd, &dd, 0x01, 0x10, 0, true, 0x0010, false, body, 200, NULL);
    struct mmpkt *out = direct_(umacd, &dd, 0x01, 0x10, 1, false, 0x0110, false, body + 200, 200, &len);
    CHECK(out != NULL && len == 400u && D(s, ok) == 1u && D(s, hdr) == 0u,
          "(red) a BSS uplink whose Queue Size goes 0 -> 1 between fragments is reassembled "
          "(ok %u, hdr %u, %u octets)", D(s, ok), D(s, hdr), len);
    mmpkt_release(out);
    s = snap_();
    (void)direct_(umacd, &dd, 0x01, 0x11, 0, true, 0x0010, false, body, 200, NULL);
    out = direct_(umacd, &dd, 0x01, 0x11, 1, false, 0x0210, false, body + 200, 200, &len);
    CHECK(out != NULL && D(s, ok) == 1u, "(pin) and one whose Queue Size goes 0 -> 2 (ok %u)", D(s, ok));
    mmpkt_release(out);

    /* The mesh: bit 8 is Mesh Control Present, which only the first fragment's body follows. */
    s = snap_();
    (void)direct_(umacd, &dd, 0x03, 0x12, 0, true, 0x0110, true, body, 200, NULL);
    out = direct_(umacd, &dd, 0x03, 0x12, 1, false, 0x0010, true, body + 200, 200, NULL);
    CHECK(out == NULL && D(s, hdr) == 1u,
          "(pin) on the mesh a later fragment without Mesh Control Present is refused (hdr %u)",
          D(s, hdr));
    mmpkt_release(out);
    datapath_defrag_deinit(umacd, &dd);
    CHECK(simnode_live_allocs() == live0, "  nothing left allocated (%u -> %u)", live0,
          simnode_live_allocs());
}

/* ---- (7b) the frame shapes mac80211 takes on a mesh ------------------------------------------- */

static void t_group_ra(enum link link)
{
    up_(link, false);
    printf("--- (7b) %s: group RA only FromDS, unicast RA only 4-address (mac80211's mesh check) ---\n",
           link_name_());
    const unsigned live0 = simnode_live_allocs();
    struct msdu m;
    struct umac_mesh_ctrl mc = mc_(UMAC_MESH_CTRL_AE_NONE, NULL, NULL);
    msdu_(&m, &mc, 0x0800, 600, 0x7b);
    static const uint16_t cut[] = { 300 };

    /* 4-address, RA broadcast, DA us; on SAE sealed under A's MGTK, which every peer of A holds. */
    struct mpdu p = tmpl_(W);
    p.ra = BC;
    if (link == LINK_SAE)
    {
        (void)simnode_set_key(A, K_AG, 1, /*pairwise=*/false);
        p.key = K_AG;
        p.kid = 1;
    }
    struct snap s = snap_();
    simnode_ext_rx_clear();
    send_(p, &m, NULL, 0, NULL);
    send_(p, &m, cut, 1, NULL);
    CHECK(simnode_ext_rx_count() == 0u && D(s, in) == 0u && g_warthog_rxdrop_reason == 89u &&
              g_warthog_rxdrop_count - s.rxdrop == 3u && D(s, shape) == 3u,
          "(red) 4-address, group RA, DA us, whole and in 2 fragments: all 3 dropped before "
          "reassembly (reason %lu, %u drops, shape %u, in %u, %u delivered)",
          (unsigned long)g_warthog_rxdrop_reason, (unsigned)(g_warthog_rxdrop_count - s.rxdrop),
          D(s, shape), D(s, in), simnode_ext_rx_count());

    if (link == LINK_OPEN)
    {
        /* 3-address, FromDS only (an AP's downlink shape), RA us. */
        static uint8_t f[3200];
        uint16_t k = umac_mesh_ies_build_data_hdr3_group(f, W, A, A);
        f[22] = (uint8_t)(s_seq << 4); f[23] = (uint8_t)(s_seq >> 4);
        s_seq++;
        f[k++] = 0x00; f[k++] = 0x01;
        memcpy(f + k, m.b, m.len);
        s = snap_();
        simnode_ext_rx_clear();
        (void)simnode_rx(f, (uint16_t)(k + m.len), -70);
        CHECK(simnode_ext_rx_count() == 0u && g_warthog_rxdrop_reason == 89u && D(s, shape) == 1u,
              "(red) 3-address unicast from a peer is dropped (reason %lu, shape %u, %u delivered)",
              (unsigned long)g_warthog_rxdrop_reason, D(s, shape), simnode_ext_rx_count());

        /* 4-address, RA us, DA broadcast: mac80211 reassembles by addr1. */
        same_as_whole_p_("4-address with DA broadcast", tmpl_(BC), &m, cut, 1, false);
    }
    simnode_advance_run(1100);
    CHECK(simnode_live_allocs() == live0, "  nothing left allocated (%u -> %u)", live0,
          simnode_live_allocs());
    if (link == LINK_OPEN || SAE_OPENS)
    {
        still_works_();
    }
}

int main(void)
{
    printf("=== fragmented unicast: chip %d, host CCMP %d ===\n", SAE_CHIP, SAE_HOST);

#if SAE_OPENS
    t_identical(LINK_SAE);
    t_relay(LINK_SAE);
    t_leaf(LINK_SAE);
    t_batman(LINK_SAE);
#endif
    t_identical(LINK_OPEN);
    t_relay(LINK_OPEN);
    t_leaf(LINK_OPEN);
    t_batman(LINK_OPEN);
    t_identical(LINK_STATIC);
    t_relay(LINK_STATIC);

    t_crypto();
    t_plain();
#if SAE_OPENS
    t_header(LINK_SAE);
    t_order(LINK_SAE);
    t_chains(LINK_SAE);
#endif
    t_header(LINK_OPEN);
    t_order(LINK_OPEN);
    t_chains(LINK_OPEN);
    t_dup_ba();

    t_group_ra(LINK_SAE);
    t_group_ra(LINK_OPEN);
#if SAE_OPENS
    t_timerless(LINK_SAE);
    t_caps(LINK_SAE);
#endif
    t_timerless(LINK_OPEN);
    t_caps(LINK_OPEN);
    t_bss_qos();

    simnode_del_peer(NULL);
    simnode_stop();
    if (failures == 0) { printf("test_simnode_frag: all passed\n"); return 0; }
    printf("test_simnode_frag: %d FAILED\n", failures);
    return 1;
}
