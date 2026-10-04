/*
 * What chip firmware 1.17.6 delivers of a frame cut in pieces, as the transmit path now obeys it,
 * read off what the firmware hands the chip (fake_chip.c seals a HW_ENC frame at the next TX PN of
 * the key it holds).
 *
 * Measured on air 2026-10-03, AT+TXCAP on the sender and AT+RXCAP on the receiver (the two rings
 * agree byte for byte but Duration), the sender at 1 MHz MCS0 (AT+TXRATE=0,1), AT+HOSTFRAG=auto:
 *  - host CCMP (no HW_ENC): the sending chip re-encapsulates every host fragment numbered 1 or
 *    more, the last included: an outer header (a copy of ours, the chip's Duration, QoS 0x0120)
 *    and our 32-octet header and QoS again inside the body. The receiver finds no CCMP header, or
 *    a MIC failure. Fragment 0 goes out unchanged. A sealed frame the chip cuts itself fails too
 *    (2026-10-01/02). So host fragmentation is off on these builds whatever AT+HOSTFRAG says, and
 *    a sealed frame goes only at rates that carry it whole (AT+SEALFIT=1, the default).
 *  - chip keys (HW_ENC): a frame in 2 fragments arrives intact and is delivered; in 3, fragment 1
 *    arrives with More Fragments cleared and 32 octets more inside its encrypted body, fragment 2
 *    with 32 more: lost. So a frame is never cut in more than 2: a chain whose first two rates
 *    would need more keeps only the rates where it needs at most 2 (their attempts to the slowest
 *    kept), and with none such its rates are replaced by the slowest rate at the chain's last
 *    bandwidth that does (MCS1 for a full-size frame at 1 MHz), AT+TXRATE notwithstanding:
 *    whole at that bandwidth needs MCS2, and the frame is lost at every rate the chain had.
 *
 * Each case marked (red) failed before this rule (the tree with every host fragment count up to
 * 16, host CCMP cutting too, and no AT+SEALFIT); (pin) marks behaviour that must not change.
 *  (R1) (red) chip keys, auto, rate control's chain for a 1 MHz MCS1 link (MCS1, then MCS0 three
 *       times): 1500 octets go as 2 fragments within MCS1's cap (were 3 within MCS0's), each with
 *       all four attempts at MCS1 (cap_trim); mac80211's rules met, the Warthog delivers it once;
 *       (pin) a 1 MHz MCS2 link's chain cuts in 2 sized for MCS1 as before, nothing moved;
 *  (R2) (red) chip keys, every rate 1 MHz MCS0 (AT+TXRATE=0,1; one entry, and four): 1000 octets
 *       still go as 2 fragments at MCS0 (pin); 1500 octets as 2 at 1 MHz MCS1 carrying every
 *       attempt (cap_sub), reassembled; a 1 MHz MCS10 chain sends 1000 octets in 2 at MCS0;
 *  (R3) (red) chip keys, a 2 MHz MCS0 link's chain (2 MHz MCS0, then 1 MHz MCS0): 1500 octets
 *       whole at 2 MHz MCS0 with all four attempts, no Block Ack session ended;
 *  (R4) (red) chip keys, AT+HOSTFRAG=512 at 2 MHz MCS7 (Linux's rule: 4 fragments): 2 fragments
 *       of 758 and 756 octets (clamp), reassembled; =1000 cuts in 2 as Linux does (pin);
 *  (R5) (red) chip keys, AT+FRAG=512 (3 fragments under it at any rate): sent whole, counted many;
 *       AT+SEALFIT runs on it, as on any whole frame, and finds no rate (seal_nofit; red before
 *       AT+SEALFIT fitted frames AT+HOSTFRAG planned to cut);
 *  (R6) (red) chip keys, AT+HOSTFRAG=0, no Block Ack session, AT+SEALFIT=1: a whole frame the chip seals
 *       goes only at rates where the chip cuts it in at most 2: 1500 octets on the 1 MHz MCS1 link's
 *       chain keep MCS1 alone (seal_trim), on 1 MHz MCS0 alone go at MCS1 (seal_sub), on the 2 MHz
 *       MCS0 link's chain at 2 MHz MCS0; (pin) 1000 octets keep 1 MHz MCS0 (2 there); AT+SEALFIT=0
 *       keeps the chain; (red) under AT+FRAG=512 nothing helps (seal_nofit);
 *  (R7) (red) every build: a head the rule puts in (a substitute, or a fallback promoted) asks for
 *       RTS/CTS only if rate control's head did; mmrc sets it on every fallback, and on a STA chip
 *       interface the chip addresses its CTS from one peer only;
 *  (R8) (red, also on the rule before AT+SEALFIT fitted these) chip keys, AT+HOSTFRAG=auto, no Block
 *       Ack session, a frame cut in 2 at its chain's first two rates but sent whole (no buffer for a
 *       fragment, or no pool block: pool) goes only at rates where the chip cuts it in at most 2, as
 *       any whole frame does:
 *       1500 octets on the 1 MHz MCS2 link's chain lose the MCS0 retries (3 there) to MCS1
 *       (seal_trim); (pin) AT+SEALFIT=0 keeps it;
 *  (H1) (red) host CCMP, AT+HOSTFRAG=auto: nothing is cut (msdu 0), and the effective mode is off;
 *  (H2) (red) host CCMP, AT+SEALFIT=1: a 1000-octet frame at the 1 MHz MCS1 link's chain keeps
 *       only MCS1, all four attempts (seal_trim); at 1 MHz MCS0 alone it goes at MCS1 and 1500
 *       octets at MCS2 (seal_sub); a batman unicast too; a 300-octet frame keeps its chain (pin);
 *  (H3) (pin) host CCMP, AT+SEALFIT=0: the chain rate control gave, counters still;
 *  (H4) (red) host CCMP, AT+FRAG=512 under a 1000-octet frame: chain kept, counted seal_nofit;
 *  (G1) (red) every build, AT+MESHGRP=1 on a 1 MHz primary channel: a group frame goes at the
 *       slowest MCS that carries it whole (1000 octets MCS1, 1500 MCS2; grp_sub), as receivers drop
 *       group fragments; (pin) 600 octets and a 2 MHz primary keep MCS0, AT+SEALFIT=0 keeps it, and
 *       with AT+MESHGRP=0 the per-peer copies are unicast, under the rules above;
 *  (P1) (red) chip keys: with AT+HOSTFRAG in force the TX pool keeps 5 blocks back (a cut's one extra
 *       fragment for each of 4 peers and a DELBA), 0 off and on host-CCMP builds; (pin) a producer
 *       that waits for the pool has 60 MSDUs cut in 2 and acked.
 *
 * Measured on air 2026-10-03 at AT+TXRATE=0,1, an OpenMANET 1.8.0 Pi pinging the Warthog with
 * 1000- and 1472-byte ICMP: chip keys, AT+HOSTFRAG=0: 0/8 and 0/8, also with AT+SEALFIT=1: the chip
 * cut each reply in 2 while the Warthog held an originator Block Ack session with the Pi, and the
 * Pi drops fragments under its session (mac80211 rx.c ieee80211_rx_reorder_ampdu); auto: 8/8 and
 * 8/8 (the session ended by DELBA, 2 host fragments). Warthog to Warthog UDP, 1000 and 1400 bytes:
 * 5/5 each with auto and with 0 (a Warthog recipient reassembles the chip's fragments under a session).
 *  (D1) a node that never stored AT+HOSTFRAG (the storage at.c boots with, the value cfg.c's NVS
 *       getter falls back to) runs auto on an SAE build with chip keys (red), off with host CCMP
 *       (pin); these builds define WARTHOG_MESH_SAE, and the glue guard runs every env's flags. Under
 *       A's TID 0 session at 1 MHz MCS0, chip keys: a 1028-octet IP packet (ping -s 1000) ends the
 *       session (one DELBA) and goes as 2 host fragments at MCS0, 1500 octets (-s 1472) as 2 at MCS1
 *       (cap_sub), mac80211's rules met, reassembled; (pin) host CCMP: whole at MCS1 and MCS2
 *       (seal_sub, not seal_ba), no DELBA;
 *  (B1) (red) chip keys, AT+HOSTFRAG=0, AT+SEALFIT=1, while W holds an originator Block Ack session,
 *       agreed or requested, with the frame's peer on its TID: the frame goes only at rates where
 *       the chip sends it whole, as a host-sealed one does, counted seal_ba (not seal_trim or
 *       seal_sub): 1028 octets on 1 MHz MCS0 alone at MCS1 and 1500 at MCS2, the session kept, no
 *       DELBA; a session requested and not answered too; (pin) a refused session, TID 6 (never a
 *       session), AT+AMPDU=0 and AT+SEALFIT=0 keep 1 MHz MCS0 (2 fragments there), counted none;
 *       under AT+FRAG=512 seal_nofit; (red) under AT+FRAG=1000 no rate sends 1500 octets whole,
 *       so they go in at most 2 as with no session, at MCS1 (seal_nofit, then seal_sub; they
 *       stayed at MCS0, 3 fragments there); (pin) AT+FRAG=810 keeps 1028 octets at MCS0 (2 there);
 *  (B2) (red) chip keys, AT+HOSTFRAG=auto, A's TID 0 session agreed, 1 MHz MCS0 alone, TX statuses
 *       held: 1028 octets end the session (one DELBA, its status pending) but go whole, with no
 *       buffer for fragment 1 or no pool block for it (pool): before that DELBA is through they go
 *       only at a rate where the chip sends them whole, 1 MHz MCS1 (seal_ba), not at MCS0, where
 *       the chip cuts them in 2 under a session the recipient may still hold.
 *
 * Builds: test_simnode_hostfrag_rule (warthog-mesh-sae) and _meshvif run (R*) and (B*), _swccmp and
 * _swccmp_meshvif run (H*); all four run (D1), (R7), (G1) and (P1); none defines
 * WARTHOG_MESH_HOSTFRAG_ANY.
 */
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

#if defined(WARTHOG_MESH_HOSTFRAG_ANY)
#error "this suite tests the shipping rule"
#endif
#if defined(WARTHOG_MESH_HOST_CCMP)
#define HOST_SEALS 1
#else
#define HOST_SEALS 0
#endif

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

extern volatile uint32_t g_warthog_hostfrag, g_warthog_hostfrag_msdu, g_warthog_hostfrag_frags,
    g_warthog_hostfrag_many, g_warthog_hostfrag_ba_end, g_warthog_hostfrag_cap_trim,
    g_warthog_hostfrag_cap_sub, g_warthog_hostfrag_clamp;
extern volatile uint32_t g_warthog_sealfit, g_warthog_sealfit_trim, g_warthog_sealfit_sub,
    g_warthog_sealfit_nofit, g_warthog_sealfit_ba;
extern volatile uint32_t g_warthog_grpfit_trim, g_warthog_grpfit_sub, g_warthog_grpfit_nofit;
extern volatile uint32_t g_warthog_hostfrag_ok, g_warthog_hostfrag_pool, g_warthog_hostfrag_ba_wait;
extern volatile uint32_t g_warthog_host_ccmp_on, g_warthog_mesh_pmf, g_warthog_ampdu;
extern volatile uint32_t g_warthog_defrag_in, g_warthog_defrag_ok, g_warthog_rxdrop_reason;
extern volatile uint32_t g_warthog_ba_delba_end;

/* AT+HOSTFRAG as this node booted, read before any test sets it. */
static uint32_t s_boot_hostfrag;

static const uint8_t W[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t A[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0a };
static const uint8_t C[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0c };
static const uint8_t BC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t M[6]  = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x45 };
static const uint8_t K_OWN[16] = { 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
                                   0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f };
static const uint8_t K_RX[16]  = { 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
                                   0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f };
static const uint8_t K_A[16]   = { 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
                                   0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf };
static const uint8_t K_C[16]   = { 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
                                   0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf };

#define HDR 32u
#define MAC_EQ(p, m) (memcmp((p), (m), 6) == 0)

/* The longest MPDU the chip sends whole at a rate, re-derived (test_mesh_frag pins it). */
static uint32_t cap_(uint8_t bw_mhz, uint8_t mcs)
{
    static const uint8_t k6[10] = { 3, 6, 9, 12, 18, 24, 27, 30, 36, 40 };
    const uint32_t nsd = bw_mhz == 1u ? 24u : bw_mhz == 2u ? 52u : bw_mhz == 4u ? 108u : 234u;
    const uint32_t ndbps = mcs == 10u ? nsd / 4u : nsd * k6[mcs % 10u] / 6u;
    return ((((511u * ndbps - 22u) / 8u) & ~3u) - 4u) - 36u;
}

static const struct simnode_rate R_1M0[]    = { { 1, 0, 2 } };
static const struct simnode_rate R_RC_1M1[] = { { 1, 1, 1 }, { 1, 0, 1 }, { 1, 0, 1 }, { 1, 0, 1 } };
static const struct simnode_rate R_1M0X4[]  = { { 1, 0, 1 }, { 1, 0, 1 }, { 1, 0, 1 }, { 1, 0, 1 } };
/* A lookaround head below the best rate: rate control probes 1 MHz MCS0, then MCS2, MCS1, MCS0. */
static const struct simnode_rate R_PROBE0[] = { { 1, 0, 1 }, { 1, 2, 1 }, { 1, 1, 1 }, { 1, 0, 1 } };
#if !HOST_SEALS
static const struct simnode_rate R_1M10[]   = { { 1, 10, 2 } };
static const struct simnode_rate R_2M7[]    = { { 2, 7, 2 }, { 2, 5, 2 }, { 2, 2, 2 }, { 2, 0, 2 } };
static const struct simnode_rate R_RC_2M0[] = { { 2, 0, 1 }, { 1, 0, 1 }, { 1, 0, 1 }, { 1, 0, 1 } };
static const struct simnode_rate R_RC_1M2[] = { { 1, 2, 1 }, { 1, 1, 1 }, { 1, 0, 1 }, { 1, 0, 1 } };
#endif
#define SET_RATES(r) simnode_set_rate_chain((r), sizeof(r) / sizeof((r)[0]))

/* ---- the nodes --------------------------------------------------------------------------- */

/* W on an SAE mesh with keyed peers A and C, AT+HOSTFRAG off, AT+SEALFIT on. */
static void up_(void)
{
    while (simnode_tx_held() != 0u) { (void)simnode_tx_forget_held(0); }
    simnode_tx_pool(false);
    simnode_del_peer(NULL);
    simnode_set_batman(false);
    simnode_set_ampdu(false);
    g_warthog_ampdu = 1;
    simnode_tx_hold(false);
    (void)simnode_start_sae(W);
    simnode_set_gates(false, false, false, true);
    g_warthog_host_ccmp_on = HOST_SEALS;
    g_warthog_mesh_pmf = 0;
    simnode_set_rx_ext_cb(true);
    (void)simnode_set_key(BC, K_OWN, 1, false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    (void)simnode_set_key(A, K_A, 0, true);
    (void)simnode_set_key(C, K_C, 0, true);
    g_warthog_hostfrag = 0;
    g_warthog_sealfit = 1;
    simnode_set_rate_chain(NULL, 0);
    simnode_set_mgmt_rate_bw(0);
    simnode_set_chip_frag_threshold(0);
    simnode_outbox_clear();
    simnode_ext_rx_clear();
    simnode_rc_clear();
}

/* A receiving from W over K_A, a leaf. */
static void as_a_(void)
{
    while (simnode_tx_held() != 0u) { (void)simnode_tx_forget_held(0); }
    simnode_del_peer(NULL);
    simnode_set_batman(false);
    (void)simnode_start_sae(A);
    simnode_set_gates(false, false, false, true);
    g_warthog_host_ccmp_on = HOST_SEALS;
    simnode_set_rx_ext_cb(true);
    (void)simnode_set_key(BC, K_RX, 1, false);
    (void)simnode_add_peer(W);
    (void)simnode_set_key(W, K_A, 0, true);
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
    uint8_t air[1700];
    uint16_t len;
    uint8_t tx_flags;
    struct simnode_rate chain[4];
    uint8_t rts; /* bit r: entry r asks for RTS/CTS */
};
#define CAP_MAX 8u
struct caps { struct cap f[CAP_MAX]; unsigned n; };

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
        e->tx_flags = f->tx_flags;
        memcpy(e->chain, f->chain, sizeof(e->chain));
        e->rts = f->chain_rts;
    }
}

static void send_(struct caps *c, const uint8_t *ra, uint16_t n, uint8_t salt, uint8_t *pay)
{
    const unsigned from = simnode_outbox_count();
    pay_(pay, n, salt);
    (void)simnode_host_tx(ra, W, pay, n);
    capture_(c, ra, from);
}

/* Frame @p i's chain is exactly @p want (@p n entries), the rest unused. */
static bool chain_is_(const struct cap *e, const struct simnode_rate *want, unsigned n)
{
    for (unsigned r = 0; r < 4u; r++)
    {
        const struct simnode_rate *g = &e->chain[r];
        if (r < n ? (g->bw_mhz != want[r].bw_mhz || g->mcs != want[r].mcs || g->attempts != want[r].attempts)
                  : g->attempts != 0u)
        {
            return false;
        }
    }
    return true;
}

static const char *chain_str_(const struct cap *e)
{
    static char b[96];
    int w = 0;
    for (unsigned r = 0; r < 4u && w < (int)sizeof(b) - 16; r++)
    {
        const struct simnode_rate *g = &e->chain[r];
        if (g->attempts != 0u)
        {
            w += snprintf(b + w, sizeof(b) - (size_t)w, "%s%u@%uMx%u", w ? "," : "", g->mcs, g->bw_mhz, g->attempts);
        }
    }
    if (w == 0) { snprintf(b, sizeof(b), "none"); }
    return b;
}

/* Every rate in each frame's chain carries it whole (FCS included). */
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

/* mac80211's reassembly (rx.c ieee80211_rx_h_defragment) of @p c sealed under @p key: NULL and the
 * body (Mesh Control onward) in @p body if they make one MSDU, else why not. */
static const char *mac80211_rx_(const struct caps *c, const uint8_t key[16], uint8_t *body,
                                uint16_t *blen)
{
    static char why[96];
    uint16_t off = 0;
    uint64_t prev = 0;
    for (unsigned i = 0; i < c->n; i++)
    {
        const uint8_t *h = c->f[i].air;
        const uint16_t len = c->f[i].len;
        const uint16_t fc = (uint16_t)(h[0] | (h[1] << 8));
        const uint16_t sc = (uint16_t)(h[22] | (h[23] << 8));
        const uint16_t sc0 = (uint16_t)(c->f[0].air[22] | (c->f[0].air[23] << 8));
        if (len < HDR + 17u) { snprintf(why, sizeof(why), "fragment %u: %u octets", i, len); return why; }
        if ((sc & 0x0fu) != i || (sc >> 4) != (sc0 >> 4)) { snprintf(why, sizeof(why), "fragment %u: sequence control 0x%04x", i, sc); return why; }
        if (((fc & 0x0400u) != 0u) != (i + 1u < c->n)) { snprintf(why, sizeof(why), "fragment %u: More Fragments", i); return why; }
        if ((fc & 0x4000u) == 0u) { snprintf(why, sizeof(why), "fragment %u not Protected", i); return why; }
        uint8_t pn[6], kid = 0, aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
        if (!umac_ccmp_parse_header(h + HDR, pn, &kid)) { snprintf(why, sizeof(why), "fragment %u: no CCMP header", i); return why; }
        uint64_t pv = 0;
        for (int k = 0; k < 6; k++) { pv = (pv << 8) | pn[k]; }
        if (i != 0u && pv != prev + 1u) { snprintf(why, sizeof(why), "fragment %u: PN %llu after %llu", i, (unsigned long long)pv, (unsigned long long)prev); return why; }
        prev = pv;
        const uint32_t al = umac_ccmp_build_aad(h, aad);
        umac_ccmp_build_nonce(h, pn, nonce);
        const uint16_t pl = (uint16_t)(len - HDR - 16u);
        memcpy(body + off, h + HDR + 8u, pl);
        if (warthog_ccm_ad(key, nonce, 8, aad, al, body + off, pl, h + len - 8u) != 0)
        {
            snprintf(why, sizeof(why), "fragment %u: MIC fails", i);
            return why;
        }
        off = (uint16_t)(off + pl);
    }
    *blen = off;
    return NULL;
}

static bool body_is_(const uint8_t *b, uint16_t blen, const uint8_t *pay, uint16_t n)
{
    static const uint8_t snap[8] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00 };
    return blen == 6u + 8u + n && memcmp(b + 6, snap, 8) == 0 && memcmp(b + 14, pay, n) == 0;
}

/* @p c from W to A reassembles by mac80211's rules and through A's own receive path, once. */
static void reassembles_(const char *what, const struct caps *c, const uint8_t *pay, uint16_t n)
{
    static uint8_t body[3000];
    uint16_t blen = 0;
    const char *why = mac80211_rx_(c, K_A, body, &blen);
    CHECK(why == NULL && body_is_(body, blen, pay, n), "  %s: mac80211's rules met, the body intact (%s)",
          what, why ? why : "ok");
    as_a_();
    const uint32_t in0 = g_warthog_defrag_in, ok0 = g_warthog_defrag_ok;
    for (unsigned i = 0; i < c->n; i++) { (void)simnode_rx_air(c->f[i].air, c->f[i].len, -60); }
    const struct simnode_extrx *x = simnode_ext_rx_get(0);
    CHECK(simnode_ext_rx_count() == 1u && x != NULL && x->len == 14u + n && memcmp(x->frame + 14, pay, n) == 0 &&
              g_warthog_defrag_in - in0 == c->n && g_warthog_defrag_ok - ok0 == 1u,
          "  %s: the Warthog's receive path delivers it once, byte-identical (%u delivered, defrag "
          "in %u ok %u, rxdrop %lu)", what, simnode_ext_rx_count(), (unsigned)(g_warthog_defrag_in - in0),
          (unsigned)(g_warthog_defrag_ok - ok0), (unsigned long)g_warthog_rxdrop_reason);
}

/* ---- Block Ack, as test_simnode_hostfrag.c drives it ------------------------------------------ */

static bool is_ba_(const struct simnode_frame *f, uint8_t act)
{
    return f->is_mgmt && f->len >= 27u && f->bytes[24] == 3u && f->bytes[25] == act;
}

/* Block Ack frames with action @p act (128 ADDBA Request, 130 DELBA) to @p ra from outbox entry @p from on. */
static unsigned ba_n_(const uint8_t *ra, uint8_t act, unsigned from)
{
    unsigned n = 0;
    for (unsigned i = from; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        n += (is_ba_(f, act) && MAC_EQ(f->bytes + 4, ra)) ? 1u : 0u;
    }
    return n;
}

/* A 64-octet frame to @p peer on @p tid sends an ADDBA Request; @p peer answers the last one with
 * @p status (0 accepts: immediate policy, 16 frames), or not at all with @p answer false. */
static void session_(const uint8_t *peer, uint8_t tid, bool answer, uint16_t status)
{
    static uint8_t sm[64];
    pay_(sm, 64, 0x5a);
    (void)simnode_host_tx_tid(peer, W, sm, 64, tid);
    uint8_t tok = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (is_ba_(f, 128u) && f->len >= 29u && MAC_EQ(f->bytes + 4, peer) && ((f->bytes[27] >> 2) & 0x0fu) == tid)
        {
            tok = f->bytes[26];
        }
    }
    if (!answer || tok == 0u) { return; }
    uint8_t r[33] = { 0 };
    const uint16_t ps = (uint16_t)((1u << 1) | ((uint16_t)tid << 2) | (16u << 6));
    r[0] = 0xd0;
    memcpy(&r[4], W, 6);
    memcpy(&r[10], peer, 6);
    memcpy(&r[16], peer, 6);
    r[24] = 3;   /* Block Ack */
    r[25] = 129; /* NDP ADDBA Response */
    r[26] = tok;
    r[27] = (uint8_t)status;
    r[28] = (uint8_t)(status >> 8);
    r[29] = (uint8_t)ps;
    r[30] = (uint8_t)(ps >> 8);
    (void)simnode_rx(r, sizeof(r), -60);
}

/* W's originator session with @p peer on @p tid: 0 none, 1 requested or refused, 2 agreed. */
static int orig_(const uint8_t *peer, uint8_t tid)
{
    struct umac_sta_data *stad = umac_datapath_mesh_find_peer(peer);
    if (stad == NULL) { return -1; }
    return umac_ba_is_ampdu_permitted(stad, tid) ? 2 : umac_ba_originator_idle(stad, tid) ? 0 : 1;
}

/* ---- the default -------------------------------------------------------------------------- */

static void t_default(void)
{
    printf("--- (D1) a node that never stored AT+HOSTFRAG: auto with chip keys, off with host CCMP ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    const uint32_t want = HOST_SEALS ? UMAC_MESH_FRAG_OFF : UMAC_MESH_FRAG_AUTO;
    CHECK(s_boot_hostfrag == want, "%s booted with AT+HOSTFRAG %lu (want %lu)", HOST_SEALS ? "(pin)" : "(red)",
          (unsigned long)s_boot_hostfrag, (unsigned long)want);
    /* The measured case: A's session on the replies' TID, every rate 1 MHz MCS0 (AT+TXRATE=0,1). */
    static const uint16_t sizes[] = { 1028, 1500 };
    for (unsigned k = 0; k < 2u; k++)
    {
        const uint16_t n = sizes[k];
        up_();
        g_warthog_hostfrag = s_boot_hostfrag;
        simnode_set_ampdu(true);
        session_(A, 0, true, 0);
        const bool agreed = orig_(A, 0) == 2;
        SET_RATES(R_1M0);
#if HOST_SEALS
        const uint32_t s0 = g_warthog_sealfit_sub, b0 = g_warthog_sealfit_ba;
#else
        const uint32_t c0 = g_warthog_hostfrag_cap_sub, d0 = g_warthog_ba_delba_end;
#endif
        const unsigned from = simnode_outbox_count();
        pay_(pay, n, (uint8_t)(0xc1 + k));
        (void)simnode_host_tx(A, W, pay, n);
        simnode_advance_run(UMAC_MESH_FRAG_BA_GUARD_MS);
        capture_(&c, A, from);
        const unsigned delbas = ba_n_(A, 130u, from);
#if HOST_SEALS
        static const struct simnode_rate w[2][1] = { { { 1, 1, 2 } }, { { 1, 2, 2 } } };
        CHECK(agreed && c.n == 1u && chain_is_(&c.f[0], w[k], 1) && carries_(&c) && delbas == 0u &&
                  orig_(A, 0) == 2 && g_warthog_sealfit_sub - s0 == 1u && g_warthog_sealfit_ba == b0,
              "(pin) %u octets under A's session: %u frame at %s (want %u@1Mx2), whole; the session kept, "
              "no DELBA (%u); seal_sub +%lu, seal_ba +%lu", n, c.n, c.n ? chain_str_(&c.f[0]) : "-",
              w[k][0].mcs, delbas, (unsigned long)(g_warthog_sealfit_sub - s0),
              (unsigned long)(g_warthog_sealfit_ba - b0));
#else
        static const struct simnode_rate w[2][1] = { { { 1, 0, 2 } }, { { 1, 1, 2 } } };
        CHECK(agreed && delbas == 1u && g_warthog_ba_delba_end - d0 == 1u && orig_(A, 0) == 0 && c.n == 2u &&
                  chain_is_(&c.f[0], w[k], 1) && chain_is_(&c.f[1], w[k], 1) && carries_(&c) &&
                  g_warthog_hostfrag_cap_sub - c0 == (k == 1u ? 1u : 0u),
              "(red) %u octets under A's session: the session ends (%u DELBA), then %u fragments (want 2) "
              "at %s (want %u@1Mx2), cap_sub +%lu", n, delbas, c.n, c.n ? chain_str_(&c.f[0]) : "-",
              w[k][0].mcs, (unsigned long)(g_warthog_hostfrag_cap_sub - c0));
        reassembles_(k == 0u ? "1028 octets in 2" : "1500 octets in 2", &c, pay, n);
#endif
    }
}

/* ---- chip keys ----------------------------------------------------------------------------- */

#if !HOST_SEALS
static void t_trim(void)
{
    printf("--- (R1) chip keys, a 1 MHz MCS1 link: 2 fragments at MCS1, not 3 at MCS0 ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    up_();
    SET_RATES(R_RC_1M1);
    g_warthog_hostfrag = UMAC_MESH_FRAG_AUTO;
    const uint32_t m0 = g_warthog_hostfrag_msdu, t0 = g_warthog_hostfrag_cap_trim, s0 = g_warthog_hostfrag_cap_sub;
    send_(&c, A, 1500, 0x21, pay);
    static const struct simnode_rate want[] = { { 1, 1, 4 } };
    CHECK(c.n == 2u && chain_is_(&c.f[0], want, 1) && chain_is_(&c.f[1], want, 1) && carries_(&c) &&
              (uint32_t)c.f[0].len + 4u <= cap_(1, 1) && (c.f[0].tx_flags & MMDRV_TX_FLAG_HW_ENC) != 0u,
          "(red) 1500 octets: %u fragments (want 2), chains %s and %s (want 1@1Mx4), each within "
          "%lu, sealed by the chip", c.n, chain_str_(&c.f[0]), c.n > 1u ? chain_str_(&c.f[1]) : "-",
          (unsigned long)cap_(1, 1));
    CHECK(g_warthog_hostfrag_msdu - m0 == 1u && g_warthog_hostfrag_cap_trim - t0 == 1u &&
              g_warthog_hostfrag_cap_sub == s0,
          "(red)   counted cap_trim %lu, cap_sub %lu, msdu %lu",
          (unsigned long)(g_warthog_hostfrag_cap_trim - t0), (unsigned long)(g_warthog_hostfrag_cap_sub - s0),
          (unsigned long)(g_warthog_hostfrag_msdu - m0));
    reassembles_("1500 octets in 2", &c, pay, 1500);

    up_();
    SET_RATES(R_RC_1M2);
    g_warthog_hostfrag = UMAC_MESH_FRAG_AUTO;
    const uint32_t t1 = g_warthog_hostfrag_cap_trim;
    send_(&c, A, 1500, 0x22, pay);
    static const struct simnode_rate want2[] = { { 1, 2, 1 }, { 1, 1, 3 } };
    CHECK(c.n == 2u && chain_is_(&c.f[0], want2, 2) && carries_(&c) && g_warthog_hostfrag_cap_trim == t1,
          "(pin) a 1 MHz MCS2 link: 2 fragments sized for MCS1, the first at %s (want 2@1Mx1,1@1Mx3), "
          "no cap_trim", c.n ? chain_str_(&c.f[0]) : "-");
}

static void t_sub(void)
{
    printf("--- (R2) chip keys, only 1 MHz MCS0 in the chain (AT+TXRATE=0,1) ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    up_();
    SET_RATES(R_1M0);
    g_warthog_hostfrag = UMAC_MESH_FRAG_AUTO;
    uint32_t t0 = g_warthog_hostfrag_cap_trim, s0 = g_warthog_hostfrag_cap_sub;
    send_(&c, A, 1000, 0x31, pay);
    CHECK(c.n == 2u && chain_is_(&c.f[0], R_1M0, 1) && chain_is_(&c.f[1], R_1M0, 1) &&
              g_warthog_hostfrag_cap_trim == t0 && g_warthog_hostfrag_cap_sub == s0,
          "(pin) 1000 octets: %u fragments at MCS0 as before (chain %s), nothing trimmed", c.n,
          chain_str_(&c.f[0]));
    reassembles_("1000 octets in 2", &c, pay, 1000);

    up_();
    SET_RATES(R_1M0);
    g_warthog_hostfrag = UMAC_MESH_FRAG_AUTO;
    t0 = g_warthog_hostfrag_cap_trim;
    s0 = g_warthog_hostfrag_cap_sub;
    send_(&c, A, 1500, 0x32, pay);
    static const struct simnode_rate want2[] = { { 1, 1, 2 } };
    CHECK(c.n == 2u && chain_is_(&c.f[0], want2, 1) && chain_is_(&c.f[1], want2, 1) && carries_(&c),
          "(red) 1500 octets, no rate in the chain needs 2 or fewer: %u fragments (want 2) at %s "
          "(want 1@1Mx2, the slowest that does)", c.n, c.n ? chain_str_(&c.f[0]) : "-");
    CHECK(g_warthog_hostfrag_cap_sub - s0 == 1u && g_warthog_hostfrag_cap_trim == t0,
          "(red)   counted cap_sub %lu", (unsigned long)(g_warthog_hostfrag_cap_sub - s0));
    reassembles_("1500 octets at MCS1", &c, pay, 1500);

    up_();
    SET_RATES(R_1M0X4);
    g_warthog_hostfrag = UMAC_MESH_FRAG_AUTO;
    send_(&c, A, 1500, 0x33, pay);
    static const struct simnode_rate want4[] = { { 1, 1, 4 } };
    CHECK(c.n == 2u && chain_is_(&c.f[0], want4, 1) && chain_is_(&c.f[1], want4, 1),
          "(red) four entries of 1 MHz MCS0: one at MCS1 with all four attempts (%s, %u fragments)",
          c.n ? chain_str_(&c.f[0]) : "-", c.n);
    CHECK(c.n == 2u && (c.f[0].rts & 1u) == 0u && (c.f[1].rts & 1u) == 0u,
          "(R7 red)   the substitute asks for no RTS, as rate control's head did not (rts 0x%x 0x%x)",
          c.n ? c.f[0].rts : 0xffu, c.n > 1u ? c.f[1].rts : 0xffu);

    up_();
    SET_RATES(R_1M10);
    g_warthog_hostfrag = UMAC_MESH_FRAG_AUTO;
    send_(&c, A, 1000, 0x34, pay);
    static const struct simnode_rate want10[] = { { 1, 0, 2 } };
    CHECK(c.n == 2u && chain_is_(&c.f[0], want10, 1) && carries_(&c),
          "(red) 1 MHz MCS10 (4 fragments): 1000 octets in 2 at MCS0 (%s, %u fragments)",
          c.n ? chain_str_(&c.f[0]) : "-", c.n);
}

static void t_whole(void)
{
    printf("--- (R3) chip keys, a 2 MHz MCS0 link: whole at 2 MHz MCS0 ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    up_();
    SET_RATES(R_RC_2M0);
    g_warthog_hostfrag = UMAC_MESH_FRAG_AUTO;
    const uint32_t m0 = g_warthog_hostfrag_msdu, t0 = g_warthog_hostfrag_cap_trim, b0 = g_warthog_hostfrag_ba_end;
    send_(&c, A, 1500, 0x41, pay);
    static const struct simnode_rate want[] = { { 2, 0, 4 } };
    CHECK(c.n == 1u && chain_is_(&c.f[0], want, 1) && carries_(&c) && g_warthog_hostfrag_msdu == m0,
          "(red) 1500 octets: %u frame (want 1) at %s (want 0@2Mx4), not cut", c.n,
          c.n ? chain_str_(&c.f[0]) : "-");
    CHECK(g_warthog_hostfrag_cap_trim - t0 == 1u && g_warthog_hostfrag_ba_end == b0,
          "(red)   counted cap_trim %lu; no Block Ack session ended",
          (unsigned long)(g_warthog_hostfrag_cap_trim - t0));
}

static void t_clamp(void)
{
    printf("--- (R4) chip keys, AT+HOSTFRAG=<n>: never more than 2 ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    up_();
    SET_RATES(R_2M7);
    g_warthog_hostfrag = 512;
    const uint32_t k0 = g_warthog_hostfrag_clamp;
    send_(&c, A, 1500, 0x51, pay);
    CHECK(c.n == 2u && c.f[0].len == HDR + 16u + 758u && c.f[1].len == HDR + 16u + 756u && carries_(&c),
          "(red) =512 at 2 MHz MCS7 (Linux's rule: 4): %u fragments (want 2) of %u and %u body octets "
          "(want 758, 756)", c.n, c.n ? c.f[0].len - HDR - 16u : 0u, c.n > 1u ? c.f[1].len - HDR - 16u : 0u);
    CHECK(g_warthog_hostfrag_clamp - k0 == 1u, "(red)   counted clamp %lu",
          (unsigned long)(g_warthog_hostfrag_clamp - k0));
    reassembles_("=512 in 2", &c, pay, 1500);

    up_();
    SET_RATES(R_2M7);
    g_warthog_hostfrag = 1000;
    const uint32_t k1 = g_warthog_hostfrag_clamp;
    send_(&c, A, 1500, 0x52, pay);
    CHECK(c.n == 2u && c.f[0].len == HDR + 16u + 964u && g_warthog_hostfrag_clamp == k1,
          "(pin) =1000: 2 fragments by Linux's rule, the first 964 body octets, not counted (%u, %u)",
          c.n, c.n ? c.f[0].len - HDR - 16u : 0u);
}

static void t_atfrag(void)
{
    printf("--- (R5) chip keys, AT+FRAG=512: 3 under it whatever the rate ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    up_();
    SET_RATES(R_2M7);
    simnode_set_chip_frag_threshold(512);
    g_warthog_hostfrag = UMAC_MESH_FRAG_AUTO;
    const uint32_t m0 = g_warthog_hostfrag_msdu, y0 = g_warthog_hostfrag_many, x0 = g_warthog_sealfit_nofit;
    send_(&c, A, 1000, 0x61, pay);
    CHECK(c.n == 1u && g_warthog_hostfrag_msdu == m0 && g_warthog_hostfrag_many - y0 == 1u,
          "(red) 1000 octets: %u frame (want 1, whole), counted many %lu", c.n,
          (unsigned long)(g_warthog_hostfrag_many - y0));
    CHECK(g_warthog_sealfit_nofit - x0 == 1u, "(red)   AT+SEALFIT ran on it and found no rate: seal_nofit +%lu (want 1)",
          (unsigned long)(g_warthog_sealfit_nofit - x0));
}

static void t_off(void)
{
    printf("--- (R6) chip keys, AT+HOSTFRAG=0: whole, at rates where the chip cuts it in at most 2 ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    up_();
    SET_RATES(R_RC_1M1);
    uint32_t t0 = g_warthog_sealfit_trim, s0 = g_warthog_sealfit_sub, x0 = g_warthog_sealfit_nofit;
    const uint32_t m0 = g_warthog_hostfrag_msdu, k0 = g_warthog_hostfrag_cap_trim + g_warthog_hostfrag_cap_sub;
    send_(&c, A, 1500, 0x71, pay);
    static const struct simnode_rate want1[] = { { 1, 1, 4 } };
    CHECK(c.n == 1u && chain_is_(&c.f[0], want1, 1) && (c.f[0].tx_flags & MMDRV_TX_FLAG_HW_ENC) != 0u &&
              g_warthog_sealfit_trim - t0 == 1u && g_warthog_sealfit_sub == s0,
          "(red) 1500 octets on the 1 MHz MCS1 link's chain: whole to the chip, at %s (want 1@1Mx4: "
          "2 at MCS1, 3 at MCS0), seal_trim +%lu", c.n ? chain_str_(&c.f[0]) : "-",
          (unsigned long)(g_warthog_sealfit_trim - t0));
    CHECK(g_warthog_hostfrag_msdu == m0 && g_warthog_hostfrag_cap_trim + g_warthog_hostfrag_cap_sub == k0,
          "  the host cut nothing and counted no cap_trim or cap_sub");

    up_();
    SET_RATES(R_1M0X4);
    s0 = g_warthog_sealfit_sub;
    send_(&c, A, 1500, 0x72, pay);
    CHECK(c.n == 1u && chain_is_(&c.f[0], want1, 1) && g_warthog_sealfit_sub - s0 == 1u,
          "(red) 1500 octets, four entries of 1 MHz MCS0 (AT+TXRATE=0,1): at %s (want 1@1Mx4), "
          "seal_sub +%lu", c.n ? chain_str_(&c.f[0]) : "-", (unsigned long)(g_warthog_sealfit_sub - s0));

    up_();
    SET_RATES(R_RC_2M0);
    t0 = g_warthog_sealfit_trim;
    send_(&c, A, 1500, 0x73, pay);
    static const struct simnode_rate want2[] = { { 2, 0, 4 } };
    CHECK(c.n == 1u && chain_is_(&c.f[0], want2, 1) && g_warthog_sealfit_trim - t0 == 1u,
          "(red) 1500 octets on the 2 MHz MCS0 link's chain: at %s (want 0@2Mx4, its 1 MHz MCS0 "
          "retries cut it in 3)", c.n ? chain_str_(&c.f[0]) : "-");

    up_();
    SET_RATES(R_1M0);
    t0 = g_warthog_sealfit_trim;
    s0 = g_warthog_sealfit_sub;
    send_(&c, A, 1000, 0x74, pay);
    CHECK(c.n == 1u && chain_is_(&c.f[0], R_1M0, 1) && g_warthog_sealfit_trim == t0 && g_warthog_sealfit_sub == s0,
          "(pin) 1000 octets at 1 MHz MCS0 alone keep it (2 there): %s", c.n ? chain_str_(&c.f[0]) : "-");

    up_();
    SET_RATES(R_RC_1M1);
    g_warthog_sealfit = 0;
    const uint32_t n0 = g_warthog_sealfit_trim + g_warthog_sealfit_sub + g_warthog_sealfit_nofit;
    send_(&c, A, 1500, 0x75, pay);
    CHECK(c.n == 1u && chain_is_(&c.f[0], R_RC_1M1, 4) &&
              g_warthog_sealfit_trim + g_warthog_sealfit_sub + g_warthog_sealfit_nofit == n0,
          "(pin) AT+SEALFIT=0: 1500 octets with the chain rate control gave (%s), nothing counted",
          c.n ? chain_str_(&c.f[0]) : "-");
    g_warthog_sealfit = 1;

    up_();
    SET_RATES(R_RC_1M1);
    simnode_set_chip_frag_threshold(512);
    x0 = g_warthog_sealfit_nofit;
    t0 = g_warthog_sealfit_trim;
    send_(&c, A, 1000, 0x76, pay);
    CHECK(c.n == 1u && chain_is_(&c.f[0], R_RC_1M1, 4) && g_warthog_sealfit_nofit - x0 == 1u &&
              g_warthog_sealfit_trim == t0,
          "(red) AT+FRAG=512 (3 under it at any rate): chain kept (%s), seal_nofit +%lu",
          c.n ? chain_str_(&c.f[0]) : "-", (unsigned long)(g_warthog_sealfit_nofit - x0));
}

static void t_cut_whole(void)
{
    printf("--- (R8) chip keys, AT+HOSTFRAG=auto, a cut planned but sent whole: the same rate fit ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    /* The 1 MHz MCS2 link's chain: its first two rates cut 1500 octets in 2, its MCS0 retries in 3. */
    static const struct simnode_rate want[] = { { 1, 2, 1 }, { 1, 1, 3 } };
    up_();
    SET_RATES(R_RC_1M2);
    g_warthog_hostfrag = UMAC_MESH_FRAG_AUTO;
    uint32_t p0 = g_warthog_hostfrag_pool, t0 = g_warthog_sealfit_trim;
    const uint32_t m0 = g_warthog_hostfrag_msdu;
    simnode_tx_alloc_fail_at(2); /* the frame itself, then its second fragment */
    send_(&c, A, 1500, 0x77, pay);
    simnode_tx_alloc_fail_at(0);
    CHECK(c.n == 1u && g_warthog_hostfrag_pool - p0 == 1u && g_warthog_hostfrag_msdu == m0 &&
              (c.f[0].tx_flags & MMDRV_TX_FLAG_HW_ENC) != 0u,
          "  no buffer for the second fragment: %u frame (want 1, whole), pool +%lu, sealed by the chip",
          c.n, (unsigned long)(g_warthog_hostfrag_pool - p0));
    CHECK(c.n == 1u && chain_is_(&c.f[0], want, 2) && g_warthog_sealfit_trim - t0 == 1u,
          "(red)   at %s (want 2@1Mx1,1@1Mx3: MCS0 cuts it in 3), seal_trim +%lu",
          c.n ? chain_str_(&c.f[0]) : "-", (unsigned long)(g_warthog_sealfit_trim - t0));

    up_();
    SET_RATES(R_RC_1M2);
    simnode_tx_pool(true);
    g_warthog_hostfrag = UMAC_MESH_FRAG_AUTO;
    simnode_tx_hold(true);
    pay_(pay, 64, 0x78);
    while (simnode_tx_pool_free() > 1u && simnode_tx_held() < 32u) { (void)simnode_host_tx(C, W, pay, 64); }
    const unsigned held = simnode_tx_held();
    p0 = g_warthog_hostfrag_pool;
    t0 = g_warthog_sealfit_trim;
    send_(&c, A, 1500, 0x79, pay);
    CHECK(c.n == 1u && g_warthog_hostfrag_pool - p0 == 1u,
          "  %u frames in the chip, no block for the second fragment: %u frame (want 1), pool +%lu", held,
          c.n, (unsigned long)(g_warthog_hostfrag_pool - p0));
    CHECK(c.n == 1u && chain_is_(&c.f[0], want, 2) && g_warthog_sealfit_trim - t0 == 1u,
          "(red)   at %s (want 2@1Mx1,1@1Mx3), seal_trim +%lu", c.n ? chain_str_(&c.f[0]) : "-",
          (unsigned long)(g_warthog_sealfit_trim - t0));
    while (simnode_tx_held() != 0u) { (void)simnode_tx_send_held(0); simnode_pump(); }
    simnode_tx_hold(false);
    simnode_tx_pool(false);

    up_();
    SET_RATES(R_RC_1M2);
    g_warthog_hostfrag = UMAC_MESH_FRAG_AUTO;
    g_warthog_sealfit = 0;
    p0 = g_warthog_hostfrag_pool;
    const uint32_t n0 = g_warthog_sealfit_trim + g_warthog_sealfit_sub + g_warthog_sealfit_nofit;
    simnode_tx_alloc_fail_at(2);
    send_(&c, A, 1500, 0x7a, pay);
    simnode_tx_alloc_fail_at(0);
    CHECK(c.n == 1u && g_warthog_hostfrag_pool - p0 == 1u && chain_is_(&c.f[0], R_RC_1M2, 4) &&
              g_warthog_sealfit_trim + g_warthog_sealfit_sub + g_warthog_sealfit_nofit == n0,
          "(pin) AT+SEALFIT=0: whole with the chain rate control gave (%s), nothing counted",
          c.n ? chain_str_(&c.f[0]) : "-");
    g_warthog_sealfit = 1;
}

/* W with A-MPDU on, its rates 1 MHz MCS0 alone (AT+TXRATE=0,1), AT+HOSTFRAG=0: one of @p n octets
 * to A on @p tid captured in @p c; the ADDBA Requests and DELBAs it sent to A in @p addba, @p delba. */
static void send_ba_(struct caps *c, uint16_t n, uint8_t tid, uint8_t salt, uint8_t *pay,
                     unsigned *addba, unsigned *delba)
{
    const unsigned from = simnode_outbox_count();
    pay_(pay, n, salt);
    (void)simnode_host_tx_tid(A, W, pay, n, tid);
    capture_(c, A, from);
    *addba = ba_n_(A, 128u, from);
    *delba = ba_n_(A, 130u, from);
}

static void up_ba_(void)
{
    up_();
    simnode_set_ampdu(true);
    SET_RATES(R_1M0);
}

static void t_seal_ba(void)
{
    printf("--- (B1) chip keys, AT+HOSTFRAG=0 under the Warthog's Block Ack session: whole ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    static const struct simnode_rate w1[] = { { 1, 1, 2 } }, w2[] = { { 1, 2, 2 } };
    unsigned addba = 0, delba = 0;
    up_ba_();
    session_(A, 0, true, 0);
    const bool agreed = orig_(A, 0) == 2;
    uint32_t b0 = g_warthog_sealfit_ba, t0 = g_warthog_sealfit_trim, s0 = g_warthog_sealfit_sub;
    send_ba_(&c, 1028, 0, 0xb1, pay, &addba, &delba);
    CHECK(agreed && c.n == 1u && chain_is_(&c.f[0], w1, 1) && carries_(&c) &&
              (c.f[0].tx_flags & MMDRV_TX_FLAG_HW_ENC) != 0u && g_warthog_sealfit_ba - b0 == 1u &&
              g_warthog_sealfit_trim == t0 && g_warthog_sealfit_sub == s0,
          "(red) 1028 octets under A's agreed TID 0 session: whole to the chip at %s (want 1@1Mx2; MCS0 "
          "cuts it in 2), seal_ba +%lu, seal_trim +%lu, seal_sub +%lu", c.n ? chain_str_(&c.f[0]) : "-",
          (unsigned long)(g_warthog_sealfit_ba - b0), (unsigned long)(g_warthog_sealfit_trim - t0),
          (unsigned long)(g_warthog_sealfit_sub - s0));
    CHECK(delba == 0u && orig_(A, 0) == 2, "  the session kept, no DELBA (%u)", delba);
    b0 = g_warthog_sealfit_ba;
    send_ba_(&c, 1500, 0, 0xb2, pay, &addba, &delba);
    CHECK(c.n == 1u && chain_is_(&c.f[0], w2, 1) && carries_(&c) && g_warthog_sealfit_ba - b0 == 1u,
          "(red) 1500 octets: whole at %s (want 2@1Mx2), seal_ba +%lu", c.n ? chain_str_(&c.f[0]) : "-",
          (unsigned long)(g_warthog_sealfit_ba - b0));

    up_ba_();
    b0 = g_warthog_sealfit_ba;
    send_ba_(&c, 1028, 0, 0xb3, pay, &addba, &delba);
    CHECK(addba == 1u && orig_(A, 0) == 1 && c.n == 1u && chain_is_(&c.f[0], w1, 1) &&
              g_warthog_sealfit_ba - b0 == 1u,
          "(red) the first frame to A requests a session (%u ADDBA), unanswered: 1028 octets whole at %s "
          "(want 1@1Mx2), seal_ba +%lu", addba, c.n ? chain_str_(&c.f[0]) : "-",
          (unsigned long)(g_warthog_sealfit_ba - b0));

    /* No session for the frame: the chip may cut it in 2, which a recipient without one delivers. */
    struct { const char *what; uint8_t tid; uint16_t refuse; uint32_t ampdu, sealfit; } pins[] = {
        { "A refused the session", 0, 37, 1, 1 },
        { "TID 6, which has no sessions", 6, 0, 1, 1 },
        { "AT+AMPDU=0", 0, 0xffff, 0, 1 },
        { "AT+SEALFIT=0 under the agreed session", 0, 0, 1, 0 },
    };
    for (unsigned k = 0; k < sizeof(pins) / sizeof(pins[0]); k++)
    {
        up_ba_();
        g_warthog_ampdu = pins[k].ampdu;
        if (pins[k].refuse != 0xffffu)
        {
            session_(A, 0, true, pins[k].refuse);
        }
        g_warthog_sealfit = pins[k].sealfit;
        const uint32_t n0 = g_warthog_sealfit_ba + g_warthog_sealfit_trim + g_warthog_sealfit_sub +
                            g_warthog_sealfit_nofit;
        send_ba_(&c, 1028, pins[k].tid, (uint8_t)(0xb4 + k), pay, &addba, &delba);
        CHECK(c.n == 1u && chain_is_(&c.f[0], R_1M0, 1) && !carries_(&c) && delba == 0u &&
                  g_warthog_sealfit_ba + g_warthog_sealfit_trim + g_warthog_sealfit_sub + g_warthog_sealfit_nofit == n0,
              "(pin) %s: 1028 octets keep 1 MHz MCS0 (%s), nothing counted", pins[k].what,
              c.n ? chain_str_(&c.f[0]) : "-");
        g_warthog_sealfit = 1;
        g_warthog_ampdu = 1;
    }

    up_ba_();
    session_(A, 0, true, 0);
    SET_RATES(R_RC_1M1);
    simnode_set_chip_frag_threshold(512);
    b0 = g_warthog_sealfit_ba;
    const uint32_t x0 = g_warthog_sealfit_nofit;
    send_ba_(&c, 1028, 0, 0xb9, pay, &addba, &delba);
    CHECK(c.n == 1u && chain_is_(&c.f[0], R_RC_1M1, 4) && g_warthog_sealfit_nofit - x0 == 1u &&
              g_warthog_sealfit_ba == b0,
          "(pin) under AT+FRAG=512 no rate sends it whole: chain kept (%s), seal_nofit +%lu, no seal_ba",
          c.n ? chain_str_(&c.f[0]) : "-", (unsigned long)(g_warthog_sealfit_nofit - x0));

    /* AT+FRAG under the frame at every rate: whole is out of reach, so at most 2 as with no session. */
    static const struct {
        uint32_t thr; uint16_t n; const struct simnode_rate *want; uint32_t sub; const char *tag, *why;
    } frags[] = {
        { 1000, 1500, w1, 1, "(red)", "2 under it; MCS0 cuts it in 3" },
        { 810, 1028, R_1M0, 0, "(pin)", "2 under it at MCS0" },
    };
    for (unsigned k = 0; k < sizeof(frags) / sizeof(frags[0]); k++)
    {
        up_ba_();
        session_(A, 0, true, 0);
        simnode_set_chip_frag_threshold(frags[k].thr);
        b0 = g_warthog_sealfit_ba;
        s0 = g_warthog_sealfit_sub;
        t0 = g_warthog_sealfit_trim;
        const uint32_t x1 = g_warthog_sealfit_nofit;
        send_ba_(&c, frags[k].n, 0, (uint8_t)(0xba + k), pay, &addba, &delba);
        CHECK(c.n == 1u && chain_is_(&c.f[0], frags[k].want, 1) && g_warthog_sealfit_nofit - x1 == 1u &&
                  g_warthog_sealfit_sub - s0 == frags[k].sub && g_warthog_sealfit_trim == t0 &&
                  g_warthog_sealfit_ba == b0 && delba == 0u && orig_(A, 0) == 2,
              "%s AT+FRAG=%lu under A's session, %u octets: at %s (want %u@1Mx2, %s), seal_nofit +%lu, "
              "seal_sub +%lu, seal_ba +%lu, the session kept", frags[k].tag, (unsigned long)frags[k].thr,
              frags[k].n, c.n ? chain_str_(&c.f[0]) : "-", frags[k].want[0].mcs, frags[k].why,
              (unsigned long)(g_warthog_sealfit_nofit - x1), (unsigned long)(g_warthog_sealfit_sub - s0),
              (unsigned long)(g_warthog_sealfit_ba - b0));
    }
}

/* AT+HOSTFRAG=auto, A's TID 0 session agreed, 1 MHz MCS0 alone, TX statuses held: 1028 octets
 * planned in 2 end the session, then go whole (no fragment buffer, then no pool block). */
static void t_ba_wait(void)
{
    printf("--- (B2) chip keys, auto: a frame whose cut ended the session but goes whole: whole ---\n");
    static uint8_t pay[1500], sm[64];
    static struct caps c;
    static const struct simnode_rate w1[] = { { 1, 1, 2 } };
    for (unsigned k = 0; k < 2u; k++)
    {
        const bool pool = k == 1u;
        up_ba_();
        g_warthog_hostfrag = UMAC_MESH_FRAG_AUTO;
        session_(A, 0, true, 0);
        const bool agreed = orig_(A, 0) == 2;
        simnode_tx_pool(pool);
        simnode_tx_hold(true);
        pay_(sm, 64, 0x33);
        while (pool && simnode_tx_pool_free() > 2u && simnode_tx_held() < 40u) { (void)simnode_host_tx(C, W, sm, 64); }
        const uint32_t p0 = g_warthog_hostfrag_pool, e0 = g_warthog_hostfrag_ba_end,
                       w0 = g_warthog_hostfrag_ba_wait, b0 = g_warthog_sealfit_ba;
        const unsigned from = simnode_outbox_count();
        pay_(pay, 1028, (uint8_t)(0xd1 + k));
        simnode_tx_alloc_fail_at(pool ? 0u : 3u); /* the frame, the DELBA, then fragment 1 */
        (void)simnode_host_tx(A, W, pay, 1028);
        simnode_tx_alloc_fail_at(0);
        capture_(&c, A, from);
        const unsigned delbas = ba_n_(A, 130u, from);
        CHECK(agreed && delbas == 1u && g_warthog_hostfrag_ba_end - e0 == 1u && g_warthog_hostfrag_pool - p0 == 1u &&
                  g_warthog_hostfrag_ba_wait == w0 && orig_(A, 0) == 0,
              "  %s: the session ended (%u DELBA, its status pending), then the frame whole (pool +%lu)",
              pool ? "no pool block for fragment 1" : "no buffer for fragment 1", delbas,
              (unsigned long)(g_warthog_hostfrag_pool - p0));
        CHECK(c.n == 1u && chain_is_(&c.f[0], w1, 1) && carries_(&c) &&
                  (c.f[0].tx_flags & MMDRV_TX_FLAG_HW_ENC) != 0u && g_warthog_sealfit_ba - b0 == 1u,
              "(red)   before A's session is known to be down: at %s (want 1@1Mx2, whole; MCS0 cuts it in 2), "
              "seal_ba +%lu", c.n ? chain_str_(&c.f[0]) : "-", (unsigned long)(g_warthog_sealfit_ba - b0));
        while (simnode_tx_held() != 0u) { (void)simnode_tx_send_held(0); simnode_pump(); }
        simnode_advance_run(UMAC_MESH_FRAG_BA_GUARD_MS);
        simnode_tx_hold(false);
        simnode_tx_pool(false);
    }
}
#endif

/* ---- host CCMP --------------------------------------------------------------------------- */

#if HOST_SEALS
static void t_hc_nocut(void)
{
    printf("--- (H1) host CCMP: AT+HOSTFRAG=auto cuts nothing ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    up_();
    SET_RATES(R_1M0);
    g_warthog_hostfrag = UMAC_MESH_FRAG_AUTO;
    const uint32_t m0 = g_warthog_hostfrag_msdu, f0 = g_warthog_hostfrag_frags;
    send_(&c, A, 1500, 0x81, pay);
    CHECK(c.n == 1u && g_warthog_hostfrag_msdu == m0 && g_warthog_hostfrag_frags == f0 &&
              (c.f[0].tx_flags & MMDRV_TX_FLAG_HW_ENC) == 0u &&
              umac_datapath_mesh_hostfrag_mode() == UMAC_MESH_FRAG_OFF,
          "(red) 1500 octets at 1 MHz MCS0: %u frame (want 1), msdu +%lu, host-sealed; the mode in "
          "force is off", c.n, (unsigned long)(g_warthog_hostfrag_msdu - m0));
    g_warthog_hostfrag = 512;
    send_(&c, A, 1500, 0x82, pay);
    CHECK(c.n == 1u && g_warthog_hostfrag_msdu == m0, "(red) =512: %u frame (want 1)", c.n);
}

static void t_hc_fit(void)
{
    printf("--- (H2) host CCMP, AT+SEALFIT=1: only rates that carry the sealed frame whole ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    up_();
    SET_RATES(R_RC_1M1);
    uint32_t t0 = g_warthog_sealfit_trim, s0 = g_warthog_sealfit_sub;
    send_(&c, A, 1000, 0x91, pay);
    static const struct simnode_rate want1[] = { { 1, 1, 4 } };
    CHECK(c.n == 1u && chain_is_(&c.f[0], want1, 1) && carries_(&c) && g_warthog_sealfit_trim - t0 == 1u &&
              g_warthog_sealfit_sub == s0,
          "(red) 1000 octets at the 1 MHz MCS1 link's chain: %s (want 1@1Mx4), seal_trim +%lu",
          c.n ? chain_str_(&c.f[0]) : "-", (unsigned long)(g_warthog_sealfit_trim - t0));
    up_();
    SET_RATES(R_1M0);
    t0 = g_warthog_sealfit_trim;
    s0 = g_warthog_sealfit_sub;
    send_(&c, A, 1000, 0x92, pay);
    static const struct simnode_rate want2[] = { { 1, 1, 2 } };
    CHECK(c.n == 1u && chain_is_(&c.f[0], want2, 1) && carries_(&c),
          "(red) 1000 octets, 1 MHz MCS0 alone: %s (want 1@1Mx2)", c.n ? chain_str_(&c.f[0]) : "-");
    send_(&c, A, 1500, 0x93, pay);
    static const struct simnode_rate want3[] = { { 1, 2, 2 } };
    CHECK(c.n == 1u && chain_is_(&c.f[0], want3, 1) && carries_(&c) && g_warthog_sealfit_sub - s0 == 2u &&
              g_warthog_sealfit_trim == t0,
          "(red) 1500 octets: %s (want 2@1Mx2), seal_sub +%lu (want 2)", c.n ? chain_str_(&c.f[0]) : "-",
          (unsigned long)(g_warthog_sealfit_sub - s0));
    simnode_set_batman(true);
    const unsigned from = simnode_outbox_count();
    pay_(pay, 1200, 0x94);
    (void)simnode_host_tx_eth(A, A, W, 0x4305, pay, 1200);
    capture_(&c, A, from);
    CHECK(c.n == 1u && chain_is_(&c.f[0], want2, 1) && carries_(&c),
          "(red) a batman unicast of 1200 octets: whole at %s (want 1@1Mx2)", c.n ? chain_str_(&c.f[0]) : "-");
    simnode_set_batman(false);
    up_();
    SET_RATES(R_1M0X4);
    s0 = g_warthog_sealfit_sub;
    send_(&c, A, 1000, 0x96, pay);
    static const struct simnode_rate want4[] = { { 1, 1, 4 } };
    CHECK(c.n == 1u && chain_is_(&c.f[0], want4, 1) && g_warthog_sealfit_sub - s0 == 1u &&
              (c.f[0].rts & 1u) == 0u,
          "(R7 red) 1000 octets, four entries of 1 MHz MCS0: %s (want 1@1Mx4), no RTS as rate control's "
          "head had none (rts 0x%x)", c.n ? chain_str_(&c.f[0]) : "-", c.n ? c.f[0].rts : 0xffu);
    up_();
    SET_RATES(R_RC_1M1);
    t0 = g_warthog_sealfit_trim;
    s0 = g_warthog_sealfit_sub;
    send_(&c, A, 300, 0x95, pay);
    CHECK(c.n == 1u && chain_is_(&c.f[0], R_RC_1M1, 4) && g_warthog_sealfit_trim == t0 &&
              g_warthog_sealfit_sub == s0,
          "(pin) 300 octets keep the chain (%s)", c.n ? chain_str_(&c.f[0]) : "-");
}

static void t_hc_fit_off(void)
{
    printf("--- (H3) host CCMP, AT+SEALFIT=0: rate control's chain ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    up_();
    SET_RATES(R_RC_1M1);
    g_warthog_sealfit = 0;
    const uint32_t n0 = g_warthog_sealfit_trim + g_warthog_sealfit_sub + g_warthog_sealfit_nofit;
    send_(&c, A, 1000, 0xa1, pay);
    CHECK(c.n == 1u && chain_is_(&c.f[0], R_RC_1M1, 4) &&
              g_warthog_sealfit_trim + g_warthog_sealfit_sub + g_warthog_sealfit_nofit == n0,
          "(pin) 1000 octets keep the chain rate control gave (%s), nothing counted",
          c.n ? chain_str_(&c.f[0]) : "-");
    g_warthog_sealfit = 1;
}

static void t_hc_atfrag(void)
{
    printf("--- (H4) host CCMP, AT+FRAG=512: no rate helps ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    up_();
    SET_RATES(R_RC_1M1);
    simnode_set_chip_frag_threshold(512);
    const uint32_t x0 = g_warthog_sealfit_nofit, t0 = g_warthog_sealfit_trim;
    send_(&c, A, 1000, 0xb1, pay);
    CHECK(c.n == 1u && chain_is_(&c.f[0], R_RC_1M1, 4) && g_warthog_sealfit_nofit - x0 == 1u &&
              g_warthog_sealfit_trim == t0,
          "(red) 1000 octets over AT+FRAG: chain kept (%s), seal_nofit +%lu", c.n ? chain_str_(&c.f[0]) : "-",
          (unsigned long)(g_warthog_sealfit_nofit - x0));
}
#endif

/* ---- every build ------------------------------------------------------------------------- */

static void t_rts(void)
{
    printf("--- (R7) a head the rule puts in asks for RTS only if rate control's did ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    up_();
    SET_RATES(R_PROBE0);
    /* Host CCMP whole: MCS0 cannot carry 1000 octets; chip keys in 2: MCS0 cuts 1500 in 3. */
    const uint16_t n = HOST_SEALS ? 1000u : 1500u;
    const uint32_t t0 = g_warthog_sealfit_trim;
    send_(&c, A, n, 0xd1, pay);
    static const struct simnode_rate want[] = { { 1, 2, 1 }, { 1, 1, 3 } };
    CHECK(c.n == 1u && chain_is_(&c.f[0], want, 2) && g_warthog_sealfit_trim - t0 == 1u,
          "(red) %u octets on a chain probing 1 MHz MCS0 first: %s (want 2@1Mx1,1@1Mx3), seal_trim +%lu", n,
          c.n ? chain_str_(&c.f[0]) : "-", (unsigned long)(g_warthog_sealfit_trim - t0));
    CHECK(c.n == 1u && (c.f[0].rts & 1u) == 0u && (c.f[0].rts & 2u) != 0u,
          "(red) the promoted fallback heads it without RTS, the next keeps mmrc's (rts 0x%x, want 0x2)",
          c.n ? c.f[0].rts : 0xffu);
}

static void t_group(void)
{
    printf("--- (G1) AT+MESHGRP=1: a group frame at a rate that carries it whole ---\n");
    static uint8_t pay[1500];
    static struct caps c;
    up_();
    simnode_set_gates(false, false, true, true);
    simnode_set_mgmt_rate_bw(1);
    uint32_t t0 = g_warthog_grpfit_trim, s0 = g_warthog_grpfit_sub, x0 = g_warthog_grpfit_nofit;
    send_(&c, M, 1000, 0xe1, pay);
    static const struct simnode_rate w1[] = { { 1, 1, 5 } };
    CHECK(c.n == 1u && chain_is_(&c.f[0], w1, 1) && carries_(&c) && (c.f[0].rts & 1u) == 0u &&
              (c.f[0].tx_flags & MMDRV_TX_FLAG_NO_ACK) != 0u && g_warthog_grpfit_sub - s0 == 1u,
          "(red) 1000 octets on a 1 MHz primary: one frame (%u) at %s (want 1@1Mx5: MCS0 carries 720), "
          "no RTS, grp_sub +%lu", c.n, c.n ? chain_str_(&c.f[0]) : "-", (unsigned long)(g_warthog_grpfit_sub - s0));
    send_(&c, M, 1500, 0xe2, pay);
    static const struct simnode_rate w2[] = { { 1, 2, 5 } };
    CHECK(c.n == 1u && chain_is_(&c.f[0], w2, 1) && carries_(&c) && g_warthog_grpfit_sub - s0 == 2u,
          "(red) 1500 octets: %s (want 2@1Mx5)", c.n ? chain_str_(&c.f[0]) : "-");
    s0 = g_warthog_grpfit_sub;
    send_(&c, M, 600, 0xe3, pay);
    static const struct simnode_rate w0[] = { { 1, 0, 5 } };
    CHECK(c.n == 1u && chain_is_(&c.f[0], w0, 1) && g_warthog_grpfit_sub == s0 && g_warthog_grpfit_trim == t0 &&
              g_warthog_grpfit_nofit == x0,
          "(pin) 600 octets keep 1 MHz MCS0 (%s), nothing counted", c.n ? chain_str_(&c.f[0]) : "-");
    simnode_set_mgmt_rate_bw(2);
    send_(&c, M, 1500, 0xe4, pay);
    static const struct simnode_rate w20[] = { { 2, 0, 5 } };
    CHECK(c.n == 1u && chain_is_(&c.f[0], w20, 1) && g_warthog_grpfit_sub == s0,
          "(pin) a 2 MHz primary: 1500 octets keep 2 MHz MCS0 (%s)", c.n ? chain_str_(&c.f[0]) : "-");
    simnode_set_mgmt_rate_bw(1);
    g_warthog_sealfit = 0;
    send_(&c, M, 1000, 0xe5, pay);
    CHECK(c.n == 1u && chain_is_(&c.f[0], w0, 1) && g_warthog_grpfit_sub == s0,
          "(pin) AT+SEALFIT=0: 1000 octets keep 1 MHz MCS0 (%s)", c.n ? chain_str_(&c.f[0]) : "-");
    g_warthog_sealfit = 1;

    up_();
    simnode_set_mgmt_rate_bw(1);
    SET_RATES(R_1M0);
    s0 = g_warthog_grpfit_sub;
    t0 = g_warthog_grpfit_trim;
    static struct caps ca, cc;
    const unsigned from = simnode_outbox_count();
    pay_(pay, 1000, 0xe6);
    (void)simnode_host_tx(M, W, pay, 1000);
    capture_(&c, M, from);
    capture_(&ca, A, from);
    capture_(&cc, C, from);
    CHECK(c.n == 0u && ca.n >= 1u && cc.n >= 1u && g_warthog_grpfit_sub == s0 && g_warthog_grpfit_trim == t0,
          "(pin) AT+MESHGRP=0: no group frame (%u), a unicast copy each to A (%u) and C (%u), nothing "
          "counted grp", c.n, ca.n, cc.n);
}

static void t_reserve(void)
{
    printf("--- (P1) the TX pool's reserve while AT+HOSTFRAG is in force ---\n");
    up_();
    simnode_tx_pool(true);
    g_warthog_hostfrag = UMAC_MESH_FRAG_AUTO;
    simnode_pump();
    const uint32_t want = HOST_SEALS ? 0u : 5u;
    CHECK(simnode_tx_pool_reserve() == want, "(red) AT+HOSTFRAG=auto: %lu blocks kept back (want %lu)",
          (unsigned long)simnode_tx_pool_reserve(), (unsigned long)want);
    g_warthog_hostfrag = 0;
    simnode_pump();
    CHECK(simnode_tx_pool_reserve() == 0u, "(pin) off: none (%lu)", (unsigned long)simnode_tx_pool_reserve());
#if !HOST_SEALS
    static uint8_t pay[1500];
    up_();
    SET_RATES(R_1M0);
    simnode_tx_pool(true);
    g_warthog_hostfrag = UMAC_MESH_FRAG_AUTO;
    simnode_tx_hold(true);
    const uint32_t m0 = g_warthog_hostfrag_msdu, ok0 = g_warthog_hostfrag_ok, p0 = g_warthog_hostfrag_pool;
    pay_(pay, 1000, 0xf1);
    unsigned offered = 0;
    for (unsigned t = 0; t < 600u; t++)
    {
        while (!simnode_tx_pool_paused() && offered < 60u)
        {
            (void)simnode_host_tx(A, W, pay, 1000);
            offered++;
        }
        (void)simnode_tx_send_held(0);
        simnode_pump();
    }
    CHECK(offered == 60u && g_warthog_hostfrag_msdu - m0 == 60u && g_warthog_hostfrag_ok - ok0 == 60u &&
              g_warthog_hostfrag_pool == p0,
          "(pin) a producer that waits for the pool: 60 MSDUs of 1000 octets cut in 2 and acked (msdu %lu, "
          "ok %lu, pool %lu)", (unsigned long)(g_warthog_hostfrag_msdu - m0),
          (unsigned long)(g_warthog_hostfrag_ok - ok0), (unsigned long)(g_warthog_hostfrag_pool - p0));
    simnode_tx_hold(false);
#endif
    g_warthog_hostfrag = 0;
    simnode_pump();
    up_();
}

int main(void)
{
    s_boot_hostfrag = g_warthog_hostfrag;
    printf("=== test_simnode_hostfrag_rule (%s) ===\n", HOST_SEALS ? "host CCMP" : "chip keys");
    t_default();
#if HOST_SEALS
    t_hc_nocut();
    t_hc_fit();
    t_hc_fit_off();
    t_hc_atfrag();
#else
    t_trim();
    t_sub();
    t_whole();
    t_clamp();
    t_atfrag();
    t_off();
    t_cut_whole();
    t_seal_ba();
    t_ba_wait();
#endif
    t_rts();
    t_group();
    t_reserve();
    simnode_stop();
    printf(failures ? "test_simnode_hostfrag_rule: %d FAILED\n" : "test_simnode_hostfrag_rule: all passed\n",
           failures);
    return failures != 0;
}
