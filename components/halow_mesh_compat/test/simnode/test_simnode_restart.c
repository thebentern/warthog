/*
 * A chip hardware restart under a running mesh, through the real umac_mmdrv_shim.c handler,
 * umac_interface.c, umac_mesh.c and the datapath, against a fake chip that loses everything it
 * holds when it boots again (fake_chip.c, simnode_chipstate).
 *
 * morselib restarts the chip when a health check fails (driver_health.c: every 90 s, and on
 * demand after bus failures): the health task pauses TX and posts HW_RESTARTED at the head of
 * the event queue, and the handler reloads the firmware (mmdrv_deinit, mmdrv_init).
 * simnode_chip_restart runs that path from the health task's two calls on. Upstream puts a STA
 * interface back (umac_connection_handle_hw_restarted) and asserts on an AP one; it asserted on
 * a mesh too, so the board reset and the mesh came back only after a full boot and re-peering.
 * Linux (mac80211's reconfig, morse_driver's morse_mesh_reconfig) re-adds the interface, its
 * stations and keys and sends MESH_CONFIG again, and the peer links stay up.
 *
 * Each case marked (red) failed on the tree before the change, where the first restart resets the
 * board (an assertion) and, with that assertion removed, the chip comes back empty. A (pin) passes
 * on the tree with the assertion; the (16) pins pin that reset itself, so they fail once it is
 * removed. A check that passes there only because the assertion fires before the chip reloads (so
 * it keeps what it held) is red, not pin. The checks added with the review fixes ((2)'s interface
 * settings, (12)'s and (14)'s event loop, (16)'s channel and fallback, (17)'s refusals and
 * retries, (18)'s probe, (19)-(21)) also fail on the restart recovery as first written, (2)'s on it
 * with umac_interface_init_vif's call removed.
 * Built four times, as the SAE envs build their keys and chip interface:
 *   test_simnode_restart                  as warthog-mesh-sae (AMPE keys in the chip, STA VIF)
 *   test_simnode_restart_meshvif          as warthog-mesh-sae-meshvif (and a MESH VIF)
 *   test_simnode_restart_swccmp           as warthog-mesh-sae-swccmp (host-only AMPE keys)
 *   test_simnode_restart_swccmp_meshvif   as warthog-mesh-sae-swccmp-meshvif
 *  (1) (red) a restart under a running mesh no longer resets the board: the chip boots again,
 *      the TX pause ends, counted restarts and mesh;
 *  (2) (red) the chip interface comes back as the mesh had it: its type (MESH 5 on -meshvif,
 *      else STA 1) and VIF id, a MESH one's capabilities read again; the TX status flush
 *      watermark, the dynamic power-save timeout and the power-save mode are set again
 *      (umac_interface_init_vif); the health check interval too (pin: the shim also sets it);
 *  (3) (red) the channel goes back: the interface forgets the channel the chip lost, so
 *      set_channel_from_regdb sends it again;
 *  (4) (red) the four QoS queues, BSS_CONFIG, the host beacon timer, MESH_CONFIG(START) with
 *      the same beaconing flag, and on a STA VIF BSS_BEACON_CONFIG and BSSID_SET (on a MESH VIF
 *      neither, pin): the BSS commands in the start's order, the chip holding what it held;
 *  (5) (pin) the beacon template keeps its RSN element: the restart does not re-initialise it;
 *  (6) (red) every peer's station record goes back at its AID, AUTHORIZED, an SAE candidate
 *      AMPE has not keyed included;
 *  (7) (red) our own MGTK goes back at AID 0, at a TX PN above every PN it used (on every
 *      build: at 0 a peer drops our group frames as replays until the PN passes what it saw);
 *      on a PN-base build the Key RSC AMPE advertises is one below the chip's next PN; one that
 *      was not in the chip yet still waits for the first peer (pin);
 *  (8) (red) each peer's MTK goes back at its AID at a TX PN above every one used (chip-key
 *      builds); host-key builds install none (pin) and host CCMP carries on;
 *  (9) (red) each peer's MGTK goes back at its AID where the build keeps it there (-meshvif with
 *      chip keys): its group frames read after it are opened and delivered as the peer's;
 *  (10) (red) a keyed non-SAE mesh: the shared MTK at each AID and the shared group key at AID 0,
 *      above every PN used, the host keys untouched (a replayed group frame is still refused;
 *      STA-VIF builds only, a MESH VIF taking no group frame the chip opened at AID 0 from a
 *      peer); an open mesh gets its stations back and no key (pin);
 *  (11) (red) AT+FRAG's threshold goes back into the chip; off stays off;
 *  (12) (red) AT+CRYPTOHOST's last setting goes back; never set, nothing is sent; the setting
 *      reaches the chip from the event loop, which a restart runs on;
 *  (13) (red) frames queued while the restart is pending are sent after it, under the keys it
 *      put back;
 *  (14) (red) the probe burst only posts its probe to the event loop: while a restart is pending
 *      nothing reaches the driver from the burst's task, and the probe goes out once the restart
 *      is done; it sends again after (pin);
 *  (15) (red) a second restart restores the same; a peer added after one keys as usual and does
 *      not install our MGTK again;
 *  (16) (pin) an interface that cannot come back at its VIF id (refused, or another id) resets
 *      the board as before; (red) so does a channel the chip will not take back, as upstream's
 *      STA restore asserts on its channel; (red) a MESH one refused falls back to STA
 *      (-meshvif), counted, its capabilities read, and the mesh carries on with the STA VIF's
 *      commands, not counted as a full restore;
 *  (17) (red) counted: stations, keys and what the chip refused (stafail, keyfail); a key the chip
 *      refuses at the restore leaves the rest restored and the restart is not counted as a full
 *      restore; our MGTK refused then is not taken as in the chip; meanwhile our group frames are
 *      not handed to the chip under the missing key (counted nokey); it goes back, at a TX PN above
 *      every one it drew, at the next service tick (counted retried), with the next peer, or at a
 *      second restart; so does a keyed mesh's group key, with the next peer or when a peer goes; a
 *      refused MTK is retried the same way, frames to that peer held from the chip meanwhile; a
 *      refused station record is counted stafail and walked to AUTHORIZED at the next tick; a
 *      refused peer MGTK goes back at the next tick; pending counts what still waits;
 *  (18) (red) a restart posted while another runs (the health check right after the reload fails
 *      too): both run and the mesh comes back after the second; a probe posted as the first ends
 *      goes out only after the second;
 *  (19) (red) AT+CHIPRESTART: the request is posted to the event loop, which fails the next health
 *      check through the driver; nothing reaches the driver from the calling task; the chip then
 *      restarts as after a real failure, counted forced; one posted while a restart is pending
 *      runs after it, on the reloaded driver (pin);
 *  (20) (red) a restore command the chip answers otherwise than it did at the start on the same
 *      interface type (MESH_CONFIG, BSS_BEACON_CONFIG) is counted cmdfail and the restart not as a
 *      full restore; the start's own refusals repeated (the STA VIF's -17s) count nothing (pin);
 *  (21) (red) host fragments' TX statuses queued before a restart and read after it count no
 *      failed MSDU (AT+HOSTFRAG?): the next MSDU is counted ok (chip-key builds; host CCMP cuts
 *      nothing);
 *  (22) (red) chip-key builds: a management frame the chip seals (path selection under MFP), held
 *      behind a fragment run when the chip restarts, is dropped (counted nokey) rather than handed
 *      to the reloaded chip while a peer's key is still owed, and so is a new one; (pin) a peer
 *      whose key went back, and that peer once the service tick puts its key back, get theirs.
 *  (23) (red) a frame AT+HOSTFRAG cut, waiting for the DELBA that ended its Block Ack session when
 *      the chip restarts (the restart purges that DELBA unreported): it goes once the chip is back,
 *      not at the wait's 500 ms limit; chip-key builds: dropped, counted nokey, if the restore
 *      could not put the peer's MTK back (host CCMP cuts nothing).
 *  (24) AT+ASSERTTEST=loop: the request only posts; the loop sets a timeout and asserts on the loop
 *      when it fires, MMWLAN_ASSERT_TEST_DELAY_MS later, not before; a post that fails says why, for
 *      it and AT+CHIPRESTART alike: NO_MEM with the loop's queue full, UNAVAILABLE with the loop
 *      stopping or down.
 *  (19) also: (red) a request posted while the driver is stopped is dropped by the loop, counted.
 */
#include <stdio.h>
#include <string.h>

#include "simnode.h"
#include "mmdrv.h"
#include "mmwlan.h"
#include "mmwlan_mesh.h"
#include "mmpkt.h"
#include "common/morse_commands.h"
#include "umac/core/umac_core_data.h"
#include "umac/data/umac_data.h"
#include "umac/datapath/umac_datapath.h"
#include "umac/datapath/umac_datapath_private.h"
#include "umac/interface/umac_interface.h"
#include "umac/interface/umac_interface_data.h"
#include "umac/mesh/umac_mesh_ccm.h"
#include "umac/mesh/umac_mesh_ccmp_hdr.h"
#include "umac/mesh/umac_mesh_ctrl.h"
#include "umac/mesh/umac_mesh_ies.h"

#ifndef WARTHOG_MESH_CHIP_VIF_MESH
#define WARTHOG_MESH_CHIP_VIF_MESH 0
#endif
#ifdef WARTHOG_MESH_AMPE_NO_CHIP_KEY
#define CHIPKEY 0
#else
#define CHIPKEY 1
#endif
#define PER_STA (WARTHOG_MESH_CHIP_VIF_MESH && CHIPKEY)
#ifdef WARTHOG_MESH_MGTK_PN_BASE
#define PNBASE 1
#else
#define PNBASE 0
#endif
#ifdef WARTHOG_MESH_HOST_CCMP
#define HOST_SEALS 1 /* a host-key build seals pairwise frames in the host */
#else
#define HOST_SEALS 0
#endif

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

extern volatile uint32_t g_warthog_chiprestart_dropped;
extern volatile uint32_t g_warthog_hostfrag_mgmt;
int umac_mesh_hwmp_send_preq(const uint8_t *da);
extern volatile uint32_t g_warthog_chiprestart_n, g_warthog_chiprestart_forced,
    g_warthog_chiprestart_mesh, g_warthog_chiprestart_sta, g_warthog_chiprestart_stafail,
    g_warthog_chiprestart_keys, g_warthog_chiprestart_keyfail, g_warthog_chiprestart_cmdfail,
    g_warthog_chiprestart_retried, g_warthog_chiprestart_pending, g_warthog_chiprestart_ms;
extern volatile uint32_t g_warthog_host_ccmp_on, g_warthog_peer_gtk_mode, g_warthog_rxdrop_reason;
extern volatile uint32_t g_warthog_chipvif_fallback, g_warthog_cryptohost_req;
extern volatile uint32_t g_warthog_rx_grp_mic_armed, g_warthog_tx_nokey, g_warthog_mesh_pmf;
extern volatile uint32_t g_warthog_hostfrag, g_warthog_hostfrag_ok, g_warthog_hostfrag_fail,
    g_warthog_hostfrag_acked;
struct mmpkt *umac_mesh_get_beacon(struct umac_data *umacd);
void umac_mesh_beacon_set_rsn(const uint8_t *rsn, uint16_t len);
int umac_mesh_tx_broadcast_probe(void);
enum mmwlan_status umac_chip_restart_request(struct umac_data *umacd); /* umac_mmdrv_shim.c */
enum mmwlan_status umac_assert_test_request(struct umac_data *umacd);  /* umac_mmdrv_shim.c */
unsigned simnode_asserts_caught(void);                                  /* fake_rtos.c */
void simnode_set_identity(const uint8_t mac[6], uint16_t vif_id); /* fake_config.c */
void simnode_evt_discard(void);
bool simnode_evt_dispatch_one(struct umac_data *umacd); /* fake_app.c */
void simnode_loop_enter(void);
void simnode_loop_leave(void);

static const uint8_t W[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 }; /* us */
static const uint8_t A[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0a };
static const uint8_t C[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0c };
static const uint8_t D[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0d }; /* an SAE candidate */
static const uint8_t E[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0e };
static const uint8_t BC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static const uint8_t K_OWN[16]    = { 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
                                      0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f };
static const uint8_t K_A_MTK[16]  = { 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
                                      0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf };
static const uint8_t K_A_MGTK[16] = { 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
                                      0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf };
static const uint8_t K_C_MTK[16]  = { 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
                                      0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf };
static const uint8_t K_C_MGTK[16] = { 0x3c, 0x4c, 0x5c, 0x6c, 0x7c, 0x8c, 0x9c, 0xac,
                                      0xbc, 0xcc, 0xdc, 0xec, 0xfc, 0x0c, 0x1c, 0x2c };
static const uint8_t K_E_MTK[16]  = { 0x0e, 0x1e, 0x2e, 0x3e, 0x4e, 0x5e, 0x6e, 0x7e,
                                      0x8e, 0x9e, 0xae, 0xbe, 0xce, 0xde, 0xee, 0xfe };
/* umac_datapath_mesh.c's shared keys for a keyed non-SAE mesh (AT+MESHSEC=1): not secrets. */
static const uint8_t K_P1_MTK[16]  = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                       0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
static const uint8_t K_P1_MGTK[16] = { 0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78,
                                       0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0 };
static const uint8_t RSC5[6] = { 5, 0, 0, 0, 0, 0 };
/* hostap's wpa_write_rsn_ie() for the mesh: CCMP-128 group and pairwise, AKM SAE. */
static const uint8_t RSN[] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00,
                               0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x08, 0x00, 0x00 };
static const uint8_t PAY[32] = { 0x45, 0x00, 0x00, 0x20 };
static const uint8_t SNAP[8] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00 };

enum { STA_T = MORSE_CMD_INTERFACE_TYPE_STA, MESH_T = MORSE_CMD_INTERFACE_TYPE_MESH };
#define WANT_VIF_T (WARTHOG_MESH_CHIP_VIF_MESH ? MESH_T : STA_T)

/* ---- setup -------------------------------------------------------------- */

static void reset_counters_(void)
{
    g_warthog_chiprestart_n = 0;
    g_warthog_chiprestart_forced = 0;
    g_warthog_chiprestart_mesh = 0;
    g_warthog_chiprestart_sta = 0;
    g_warthog_chiprestart_stafail = 0;
    g_warthog_chiprestart_keys = 0;
    g_warthog_chiprestart_keyfail = 0;
    g_warthog_chiprestart_cmdfail = 0;
    g_warthog_chiprestart_retried = 0;
    g_warthog_chiprestart_ms = 0;
    g_warthog_chipvif_fallback = 0;
}

/* Peers outlive a stop (the datapath's table is static), and so would an armed refusal. */
static void stop_(void)
{
    simnode_del_peer(NULL);
    simnode_stop();
    simnode_chip_refusals_clear();
    simnode_evt_discard();
}

/* Run once at the next start, after the stop: arms what the chip refuses at that start. */
static void (*s_at_start)(void);

/* An SAE start with our own MGTK delivered first, as hostap does; the chip log starts here. */
static bool start_sae_(void)
{
    stop_();
    reset_counters_();
    if (s_at_start != NULL) { s_at_start(); s_at_start = NULL; }
    g_warthog_peer_gtk_mode = 1;
    g_warthog_rx_grp_mic_armed = 0;
    g_warthog_host_ccmp_on = 0;
    simnode_set_chip_frag_threshold(0);
    simnode_chipcmd_clear();
    if (!simnode_start_sae(W)) { return false; }
    /* Standard group frames, so a broadcast goes once under our MGTK (AT+MESHGRP=1). */
    simnode_set_gates(/*fwd=*/false, /*bridge=*/false, /*grp_std=*/true, /*secure=*/true);
    (void)simnode_set_key(BC, K_OWN, 1, /*pairwise=*/false);
    return true;
}

static bool start_open_(bool keyed)
{
    stop_();
    reset_counters_();
    simnode_set_chip_frag_threshold(0);
    simnode_chipcmd_clear();
    if (!simnode_start(W)) { return false; }
    simnode_set_gates(false, false, /*grp_std=*/true, keyed);
    return true;
}

/* A peer as AMPE leaves it: its MTK, then (unless NULL) its MGTK, key id 1, RSC 5. */
static void keyed_(const uint8_t *p, const uint8_t mtk[16], const uint8_t mgtk[16])
{
    (void)simnode_add_peer(p);
    (void)simnode_set_key(p, mtk, 0, /*pairwise=*/true);
    if (mgtk != NULL) { (void)simnode_set_key_rsc(p, mgtk, 1, /*pairwise=*/false, RSC5); }
}

/* The SAE mesh most cases restart: A and C keyed, D a candidate AMPE has not keyed. */
static bool sae_mesh_(void)
{
    if (!start_sae_()) { return false; }
    keyed_(A, K_A_MTK, K_A_MGTK);
    keyed_(C, K_C_MTK, K_C_MGTK);
    return simnode_add_peer(D);
}

static uint16_t aid_(const uint8_t *p)
{
    struct umac_sta_data *s = umac_datapath_mesh_find_peer(p);
    return s != NULL ? umac_sta_data_get_aid(s) : 0u;
}

static bool held_(uint16_t aid, bool pairwise, uint8_t idx, const uint8_t key[16])
{
    uint8_t k[16];
    return simnode_chip_key_held(aid, pairwise, idx, k) && memcmp(k, key, 16) == 0;
}

static bool restart_ok_(void)
{
    return !simnode_expect_assert(simnode_chip_restart);
}

/* Probe requests in the outbox. */
static unsigned probes_(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        n += f->is_mgmt && f->len >= 24u && f->bytes[0] == 0x40u;
    }
    return n;
}

/* The last data frame to @p da in the outbox, or NULL. */
static const struct simnode_frame *last_data_to_(const uint8_t *da)
{
    const struct simnode_frame *hit = NULL;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (!f->is_mgmt && f->len >= 24u && (f->bytes[0] & 0x0cu) == 0x08u &&
            memcmp(f->bytes + 4, da, 6) == 0)
        {
            hit = f;
        }
    }
    return hit;
}

/* The highest PN the chip drew for a data frame to @p da in the outbox, 0 with none. */
static uint64_t top_pn_to_(const uint8_t *da)
{
    uint64_t top = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (!f->is_mgmt && f->air_len != 0u && memcmp(f->bytes + 4, da, 6) == 0 && f->pn > top)
        {
            top = f->pn + (f->pn_draws > 1u ? f->pn_draws - 1u : 0u);
        }
    }
    return top;
}

/* The INSTALL_KEY of @p key since the log was cleared, or NULL. */
static const struct simnode_keyinst *inst_of_(const uint8_t key[16], bool pairwise)
{
    for (unsigned i = 0; i < simnode_keyinst_count(); i++)
    {
        const struct simnode_keyinst *k = simnode_keyinst_get(i);
        if (k->pairwise == pairwise && memcmp(k->key, key, 16) == 0) { return k; }
    }
    return NULL;
}

/* The BSS commands in the log, in order, as "BB BS BC MC". */
static void bss_sequence_(char *buf, size_t len)
{
    static const struct { uint16_t id; const char *tag; } tags[] = {
        { MORSE_CMD_ID_BSS_BEACON_CONFIG, "BB" }, { MORSE_CMD_ID_BSSID_SET, "BS" },
        { MORSE_CMD_ID_BSS_CONFIG, "BC" }, { MORSE_CMD_ID_MESH_CONFIG, "MC" },
    };
    size_t off = 0;
    buf[0] = '\0';
    for (unsigned i = 0; i < simnode_chipcmd_count(); i++)
    {
        const struct simnode_chipcmd *c = simnode_chipcmd_get(i);
        for (unsigned t = 0; t < sizeof(tags) / sizeof(tags[0]); t++)
        {
            if (c->id != tags[t].id) { continue; }
            int n = snprintf(buf + off, len - off, "%s%s%u", off ? " " : "", tags[t].tag,
                             (unsigned)c->arg);
            if (n > 0 && off + (size_t)n < len) { off += (size_t)n; }
        }
    }
}

static bool beacon_has_rsn_(void)
{
    struct mmpkt *b = umac_mesh_get_beacon(umac_data_get_umacd());
    if (b == NULL) { return false; }
    struct mmpktview *v = mmpkt_open(b);
    const uint8_t *p = mmpkt_get_data_start(v);
    const uint32_t n = mmpkt_get_data_length(v);
    bool hit = false;
    for (uint32_t i = 0; !hit && i + sizeof(RSN) <= n; i++) { hit = memcmp(p + i, RSN, sizeof(RSN)) == 0; }
    mmpkt_close(&v);
    mmpkt_release(b);
    return hit;
}

/* ---- frames a peer sends -------------------------------------------------- */

static void pn6_(uint8_t out[6], uint64_t pn)
{
    for (int i = 0; i < 6; i++) { out[i] = (uint8_t)(pn >> (8 * (5 - i))); }
}

/* @p ta's group data frame (3-address QoS data, Mesh Control, IPv4), CCMP under @p key. */
static uint16_t grp_(uint8_t *f, const uint8_t *ta, const uint8_t key[16], uint8_t kid, uint64_t pn)
{
    static uint32_t mseq = 100;
    uint8_t p6[6], aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
    pn6_(p6, pn);
    uint16_t n = umac_mesh_ies_build_data_hdr3_group(f, BC, ta, ta);
    f[1] |= 0x40u;
    f[n++] = 0;
    f[n++] = 0x01;
    umac_ccmp_write_header(&f[n], p6, kid);
    n = (uint16_t)(n + UMAC_CCMP_HDR_LEN);
    const uint16_t body = n;
    struct umac_mesh_ctrl mc = { .flags = 0, .ttl = 31, .seq = ++mseq };
    n = (uint16_t)(n + umac_mesh_ctrl_build(&f[n], 18u, &mc));
    memcpy(&f[n], SNAP, sizeof(SNAP));
    n = (uint16_t)(n + sizeof(SNAP));
    memcpy(&f[n], PAY, 8);
    n = (uint16_t)(n + 8u + 8u);
    const uint32_t al = umac_ccmp_build_aad(f, aad);
    umac_ccmp_build_nonce(f, p6, nonce);
    (void)warthog_ccm_ae(key, nonce, 8, aad, al, f + body, (size_t)(n - body - 8u), f + n - 8u);
    return n;
}

/* @p f off the air through the chip: delivered to the host, and the drop reason. */
static unsigned air_(const uint8_t *f, uint16_t n, uint32_t *reason)
{
    simnode_host_rx_clear();
    g_warthog_rxdrop_reason = 0;
    (void)simnode_rx_air(f, n, -60);
    if (reason != NULL) { *reason = g_warthog_rxdrop_reason; }
    return simnode_host_rx_count();
}

/* ---- cases -------------------------------------------------------------- */

static void t_recovers(void)
{
    printf("--- (1)-(4) the restart: no reset; the interface, channel and BSS as before ---\n");
    CHECK(sae_mesh_(), "the mesh starts with A and C keyed and D a candidate");
    umac_mesh_beacon_set_rsn(RSN, sizeof(RSN));
    char seq0[160], seq1[160];
    bss_sequence_(seq0, sizeof(seq0));
    struct simnode_chipstate before = *simnode_chipstate();
    struct umac_interface_data *ifd = umac_data_get_interface(umac_data_get_umacd());
    ifd->current_s1g_operation.operating_channel_index = 5; /* what set_channel_from_regdb stores */
    const unsigned ch0 = simnode_stub_hits("umac_interface_set_channel_from_regdb");
    const unsigned ps0 = simnode_stub_hits("umac_ps_update_mode");
    simnode_chipcmd_clear();

    CHECK(restart_ok_(), "(1) (red) a chip restart under the mesh does not reset the board");
    const struct simnode_chipstate *s = simnode_chipstate();
    CHECK(s->boots == before.boots + 1u && !s->down && s->restarts_done == before.restarts_done + 1u,
          "(1) (red) the chip booted again and the restart completed (boots %u -> %u)", before.boots,
          s->boots);
    CHECK(g_warthog_chiprestart_n == 1u && g_warthog_chiprestart_mesh == 1u &&
              g_warthog_chiprestart_cmdfail == 0u && g_warthog_chiprestart_pending == 0u,
          "(1) (red) counted restarts 1, mesh 1, nothing failed or pending (%lu, %lu)",
          (unsigned long)g_warthog_chiprestart_n, (unsigned long)g_warthog_chiprestart_mesh);
    static const uint8_t pay[16] = { 0x45 };
    simnode_outbox_clear();
    CHECK(simnode_host_tx(BC, W, pay, sizeof(pay)) && simnode_outbox_count() == 1u,
          "(1) (red) the TX pause has ended: a broadcast reaches the chip");

    CHECK(s->vif && s->vif_type == (uint32_t)WANT_VIF_T && s->vif_id == before.vif_id &&
              simnode_chipcmd_n(MORSE_CMD_ID_ADD_INTERFACE, WANT_VIF_T) == 1u &&
              simnode_chipcmd_n(MORSE_CMD_ID_ADD_INTERFACE, UINT32_MAX) == 1u,
          "(2) (red) one ADD_INTERFACE: type %u, VIF %u, as the mesh had it", s->vif_type, s->vif_id);
    CHECK(simnode_chipcmd_n(MORSE_CMD_ID_GET_CAPABILITIES, UINT32_MAX) == (WARTHOG_MESH_CHIP_VIF_MESH ? 1u : 0u),
          "(2) %s", WARTHOG_MESH_CHIP_VIF_MESH ? "(red) the MESH VIF's capabilities read again, as at the start"
                                               : "(pin) no capabilities read: the STA VIF's are the boot VIF's");
    CHECK(simnode_chipcmd_n(MORSE_CMD_ID_REMOVE_INTERFACE, UINT32_MAX) == 0u, "(2) (pin) and nothing removed");
    /* umac_interface.c's MM_TX_STATUS_BUFFER_FLUSH_WATERMARK. */
    CHECK(s->flush_wm == 10u && s->dyn_ps_sets == 1u && simnode_stub_hits("umac_ps_update_mode") == ps0 + 1u,
          "(2) (red) the TX status flush watermark (%lu), the dynamic power-save timeout (%u) and the "
          "power-save mode (%u) are set again", (unsigned long)s->flush_wm, s->dyn_ps_sets,
          simnode_stub_hits("umac_ps_update_mode") - ps0);
    CHECK(s->health_ms == MMWLAN_DEFAULT_MIN_HEALTH_CHECK_INTERVAL_MS,
          "(2) (pin) the health check interval is set again (%lu; the shim sets it too)",
          (unsigned long)s->health_ms);
    CHECK(simnode_stub_hits("umac_interface_set_channel_from_regdb") == ch0 + 1u &&
              ifd->current_s1g_operation.operating_channel_index == 0u,
          "(3) (red) the channel is sent again (%u), its cached setting forgotten first",
          simnode_stub_hits("umac_interface_set_channel_from_regdb") - ch0);
    CHECK(s->qos == 4u, "(4) (red) the four QoS queues (%u)", s->qos);
    CHECK(s->beacon_int == before.beacon_int && before.beacon_int == 100u,
          "(4) (red) BSS_CONFIG with the beacon interval (%u)", s->beacon_int);
    CHECK(s->beacon_period_ms == before.beacon_period_ms && before.beacon_period_ms != 0u,
          "(4) (red) the host beacon timer at the same period (%lu ms)", (unsigned long)s->beacon_period_ms);
    CHECK(s->mesh_started && s->mesh_beaconing == before.mesh_beaconing &&
              s->mesh_beaconing == !WARTHOG_MESH_CHIP_VIF_MESH,
          "(4) (red) MESH_CONFIG(START), %s as at the start", s->mesh_beaconing ? "beaconing" : "beaconless");
    CHECK(s->bss_beacon == before.bss_beacon && s->bssid_set == before.bssid_set &&
              memcmp(s->bssid, before.bssid, 6) == 0 && s->bss_beacon == !WARTHOG_MESH_CHIP_VIF_MESH,
          "(4) %s", WARTHOG_MESH_CHIP_VIF_MESH ? "(pin) no BSS_BEACON_CONFIG or BSSID_SET on the MESH VIF"
                                               : "(red) BSS_BEACON_CONFIG and the derived BSSID_SET on the STA VIF");
    bss_sequence_(seq1, sizeof(seq1));
    CHECK(strcmp(seq0, seq1) == 0, "(4) (red) the BSS commands in the start's order (\"%s\" / \"%s\")", seq0, seq1);
    CHECK(beacon_has_rsn_(), "(5) (pin) the beacon template keeps its RSN element");
}

static void t_stations_and_keys(void)
{
    printf("--- (6)-(9) stations and keys ---\n");
    CHECK(sae_mesh_(), "the mesh starts with A and C keyed and D a candidate");
    g_warthog_host_ccmp_on = CHIPKEY ? 0u : 1u;
    static const uint8_t pay[48] = { 0x45 };
    simnode_outbox_clear();
    for (int i = 0; i < 3; i++)
    {
        (void)simnode_host_tx(BC, W, pay, sizeof(pay));
        (void)simnode_host_tx(A, W, pay, sizeof(pay));
    }
    (void)simnode_host_tx(C, W, pay, sizeof(pay));
    uint64_t gtop = 0;
    const bool gused = simnode_group_pn_top(&gtop);
#if CHIPKEY
    const uint64_t atop = top_pn_to_(A), ctop = top_pn_to_(C);
#endif
    CHECK(gused, "our broadcasts drew PNs under our MGTK before the restart (top 0x%llx)",
          (unsigned long long)gtop);
    const uint16_t aa = aid_(A), ac = aid_(C), ad = aid_(D);
    simnode_keyinst_clear();
    CHECK(restart_ok_(), "(1) (red) the restart does not reset the board");

    uint8_t addr[6];
    CHECK(simnode_chip_sta_state(aa, addr) == MORSE_STA_AUTHORIZED && memcmp(addr, A, 6) == 0 &&
              simnode_chip_sta_state(ac, addr) == MORSE_STA_AUTHORIZED && memcmp(addr, C, 6) == 0,
          "(6) (red) A and C are AUTHORIZED at their AIDs %u and %u again", aa, ac);
    CHECK(simnode_chip_sta_state(ad, addr) == MORSE_STA_AUTHORIZED && memcmp(addr, D, 6) == 0,
          "(6) (red) and D, which AMPE has not keyed, at AID %u", ad);
    CHECK(simnode_chipstate()->stas == 3u, "(6) (red) three station records, no more (%u)", simnode_chipstate()->stas);

    const struct simnode_keyinst *own = inst_of_(K_OWN, false);
    CHECK(held_(0, false, 1, K_OWN) && own != NULL && own->aid == 0u && own->key_idx == 1u,
          "(7) (red) our MGTK is in the chip again at AID 0, key id 1");
    CHECK(own != NULL && own->tx_pn > gtop && (own->tx_pn & 0xfffffull) == 0u,
          "(7) (red) at a fresh TX PN epoch above every PN it used (0x%llx > 0x%llx)",
          own != NULL ? (unsigned long long)own->tx_pn : 0ull, (unsigned long long)gtop);
    simnode_outbox_clear();
    (void)simnode_host_tx(BC, W, pay, sizeof(pay));
    const struct simnode_frame *g = last_data_to_(BC);
    CHECK(g != NULL && g->air_len != 0u && g->pn > gtop,
          "(7) (red) our next broadcast is sealed under it above the old top (PN 0x%llx)",
          g != NULL ? (unsigned long long)g->pn : 0ull);
    uint8_t rsc[6];
    (void)simnode_own_group_rsc(1, rsc);
    uint64_t r = 0;
    for (int i = 5; i >= 0; i--) { r = (r << 8) | rsc[i]; }
#if PNBASE
    uint64_t next = 0;
    CHECK(g != NULL && r >= g->pn && r >= gtop && simnode_chip_key_next_pn(0, false, 1, &next) && next == r + 1u,
          "(7) (red) the Key RSC AMPE advertises is one below the chip's next PN, above every PN used (0x%llx)",
          (unsigned long long)r);
#else
    CHECK(r == 0u, "(7) (pin) without the PN base the Key RSC AMPE advertises stays 0");
#endif

#if CHIPKEY
    const struct simnode_keyinst *ka = inst_of_(K_A_MTK, true), *kc = inst_of_(K_C_MTK, true);
    CHECK(held_(aa, true, 0, K_A_MTK) && held_(ac, true, 0, K_C_MTK) && ka != NULL && kc != NULL &&
              ka->aid == aa && kc->aid == ac,
          "(8) (red) A's and C's MTKs are in the chip again at their AIDs");
    CHECK(ka != NULL && kc != NULL && ka->tx_pn > atop && kc->tx_pn > ctop && ka->tx_pn > gtop,
          "(8) (red) each at a TX PN above every one used (A 0x%llx > 0x%llx)",
          ka != NULL ? (unsigned long long)ka->tx_pn : 0ull, (unsigned long long)atop);
    simnode_outbox_clear();
    (void)simnode_host_tx(A, W, pay, sizeof(pay));
    const struct simnode_frame *u = last_data_to_(A);
    CHECK(u != NULL && u->air_len != 0u && u->pn > atop,
          "(8) (red) a frame to A is sealed under its MTK above the old top (PN 0x%llx)",
          u != NULL ? (unsigned long long)u->pn : 0ull);
#else
    CHECK(inst_of_(K_A_MTK, true) == NULL && inst_of_(K_C_MTK, true) == NULL,
          "(8) (pin) host-key build: no MTK goes into the chip");
    simnode_outbox_clear();
    (void)simnode_host_tx(A, W, pay, sizeof(pay));
    const struct simnode_frame *u = last_data_to_(A);
    CHECK(u != NULL && (u->tx_flags & MMDRV_TX_FLAG_HW_ENC) == 0u && (u->bytes[1] & 0x40u) != 0u,
          "(8) (red) host CCMP still seals a frame to A");
#endif

#if PER_STA
    CHECK(held_(aa, false, 1, K_A_MGTK) && held_(ac, false, 1, K_C_MGTK),
          "(9) (red) A's and C's MGTKs are in the chip again at their AIDs");
    uint8_t f[160];
    uint32_t why = 0;
    const uint16_t n = grp_(f, A, K_A_MGTK, 1, 50);
    CHECK(air_(f, n, &why) == 1u && memcmp(simnode_host_rx_get(0)->sa, A, 6) == 0,
          "(9) (red) A's group frame read after it is opened and delivered as A's (reason %lu)",
          (unsigned long)why);
#else
    CHECK(!simnode_chip_key_held(aa, false, 1, NULL) && !simnode_chip_key_held(ac, false, 1, NULL),
          "(9) (pin) peers' MGTKs stay host-only on this build");
#endif
    CHECK(simnode_chipstate()->keys == 1u + (CHIPKEY ? 2u : 0u) + (PER_STA ? 2u : 0u),
          "(7)-(9) the chip holds exactly what it held (%u keys)", simnode_chipstate()->keys);
    const uint32_t want_keys = 1u + (CHIPKEY ? 2u : 0u) + (PER_STA ? 2u : 0u);
    CHECK(g_warthog_chiprestart_sta == 3u && g_warthog_chiprestart_keys == want_keys &&
              g_warthog_chiprestart_keyfail == 0u && g_warthog_chiprestart_stafail == 0u &&
              g_warthog_chiprestart_pending == 0u,
          "(17) (red) counted sta 3, keys %lu, nothing failed or pending (%lu, %lu, %lu)",
          (unsigned long)want_keys, (unsigned long)g_warthog_chiprestart_sta,
          (unsigned long)g_warthog_chiprestart_keys, (unsigned long)g_warthog_chiprestart_keyfail);
}

static void t_own_mgtk_waits(void)
{
    printf("--- (7) an MGTK not in the chip yet still waits for the first peer ---\n");
    CHECK(start_sae_(), "the mesh starts, our MGTK delivered, no peer");
    simnode_keyinst_clear();
    CHECK(restart_ok_(), "(1) (red) a restart with no peer does not reset the board");
    CHECK(simnode_keyinst_count() == 0u && simnode_chipstate()->keys == 0u,
          "(7) (pin) nothing is installed: our MGTK was not in the chip");
    CHECK(simnode_chipstate()->mesh_started, "(4) (red) the mesh is started in the chip again");
    CHECK(simnode_add_peer(A) && held_(0, false, 1, K_OWN),
          "(7) (pin) the first peer takes it into the chip, as before");
}

static void t_non_sae(void)
{
    printf("--- (10) a keyed non-SAE mesh, and an open one ---\n");
    CHECK(start_open_(true), "a keyed non-SAE mesh starts");
    CHECK(simnode_add_peer(A) && simnode_add_peer(C), "with A and C");
    static const uint8_t pay[48] = { 0x45 };
    simnode_outbox_clear();
    for (int i = 0; i < 3; i++) { (void)simnode_host_tx(BC, W, pay, sizeof(pay)); }
    (void)simnode_host_tx(A, W, pay, sizeof(pay));
    uint64_t gtop = 0;
    (void)simnode_group_pn_top(&gtop);
    const uint64_t atop = top_pn_to_(A);
#if !WARTHOG_MESH_CHIP_VIF_MESH
    /* A's broadcast under the shared group key, at PN 40: its replay floor moves there. (A MESH
     * VIF takes no group frame the chip opened at AID 0 from a peer, so the floor is untested there.) */
    uint8_t f[160];
    uint32_t why = 0;
    uint16_t n = grp_(f, A, K_P1_MGTK, 1, 40);
    CHECK(air_(f, n, &why) == 1u, "A's group frame at PN 40 is taken before the restart");
#endif
    const uint16_t aa = aid_(A), ac = aid_(C);
    simnode_keyinst_clear();
    CHECK(restart_ok_(), "(1) (red) the restart does not reset the board");
    const struct simnode_keyinst *ka = NULL, *g = NULL;
    unsigned mtks = 0;
    for (unsigned i = 0; i < simnode_keyinst_count(); i++)
    {
        const struct simnode_keyinst *k = simnode_keyinst_get(i);
        if (k->pairwise && memcmp(k->key, K_P1_MTK, 16) == 0) { mtks++; if (k->aid == aa) { ka = k; } }
        if (!k->pairwise && k->aid == 0u && memcmp(k->key, K_P1_MGTK, 16) == 0) { g = k; }
    }
    CHECK(held_(aa, true, 0, K_P1_MTK) && held_(ac, true, 0, K_P1_MTK) && mtks == 2u,
          "(10) (red) the shared MTK is in the chip again at A's and C's AIDs (%u installs)", mtks);
    CHECK(ka != NULL && ka->tx_pn > atop, "(10) (red) above every PN it used (0x%llx > 0x%llx)",
          ka != NULL ? (unsigned long long)ka->tx_pn : 0ull, (unsigned long long)atop);
    CHECK(held_(0, false, 1, K_P1_MGTK) && g != NULL && g->tx_pn > gtop,
          "(10) (red) the shared group key at AID 0, once, above every PN it used (0x%llx > 0x%llx)",
          g != NULL ? (unsigned long long)g->tx_pn : 0ull, (unsigned long long)gtop);
#if !WARTHOG_MESH_CHIP_VIF_MESH
    n = grp_(f, A, K_P1_MGTK, 1, 40);
    CHECK(air_(f, n, &why) == 0u && why == 5u,
          "(10) (red) the host keys are untouched: A's group frame at PN 40 again is a replay (%lu)",
          (unsigned long)why);
    n = grp_(f, A, K_P1_MGTK, 1, 41);
    CHECK(air_(f, n, &why) == 1u, "(10) (red) and its PN 41 is taken");
#endif

    CHECK(start_open_(false), "an open mesh starts");
    CHECK(simnode_add_peer(A), "with A");
    const uint16_t oa = aid_(A);
    simnode_keyinst_clear();
    CHECK(restart_ok_(), "(1) (red) the restart does not reset the board");
    CHECK(simnode_chip_sta_state(oa, NULL) == MORSE_STA_AUTHORIZED,
          "(10) (red) A's station record is back at AID %u", oa);
    CHECK(simnode_keyinst_count() == 0u && simnode_chipstate()->keys == 0u, "(10) (pin) and no key");
}

static void t_settings(void)
{
    printf("--- (11)-(12) AT+FRAG and AT+CRYPTOHOST ---\n");
    CHECK(sae_mesh_(), "the mesh starts");
    simnode_set_chip_frag_threshold(500);
    const unsigned off0 = simnode_chip_calls_off_loop();
    g_warthog_cryptohost_req = 1;
    simnode_tick();
    CHECK(simnode_chipstate()->frag_threshold == 500u && simnode_chipstate()->crypto_in_host_set &&
              simnode_chipstate()->crypto_in_host,
          "AT+FRAG=500 and AT+CRYPTOHOST=1 reached the chip");
    CHECK(simnode_chip_calls_off_loop() == off0,
          "(12) (red) AT+CRYPTOHOST reached it from the event loop, which a restart runs on (%u off it)",
          simnode_chip_calls_off_loop() - off0);
    CHECK(restart_ok_(), "(1) (red) the restart does not reset the board");
    CHECK(simnode_chipstate()->frag_threshold == 500u,
          "(11) (red) the chip's fragmentation threshold is 500 again (%lu)",
          (unsigned long)simnode_chipstate()->frag_threshold);
    CHECK(simnode_chipstate()->crypto_in_host_set && simnode_chipstate()->crypto_in_host,
          "(12) (red) crypto in host is set again as last asked");
    g_warthog_cryptohost_req = 2;
    simnode_tick();
    CHECK(restart_ok_(), "(1) (red) a second restart");
    CHECK(simnode_chipstate()->crypto_in_host_set && !simnode_chipstate()->crypto_in_host,
          "(12) (red) AT+CRYPTOHOST=0 goes back as 0");

    CHECK(sae_mesh_(), "a fresh start, neither set");
    CHECK(restart_ok_(), "(1) (red) the restart does not reset the board");
    CHECK(simnode_chipstate()->frag_threshold == 0u && !simnode_chipstate()->crypto_in_host_set,
          "(11)-(12) (pin) off stays off and crypto in host is never sent");
}

static unsigned s_probes_at_done = 99;
static void probes_at_done_(void) { s_probes_at_done = probes_(); }

static void t_queued(void)
{
    printf("--- (13)-(14) while the restart is pending ---\n");
    CHECK(sae_mesh_(), "the mesh starts");
    g_warthog_host_ccmp_on = CHIPKEY ? 0u : 1u;
    static const uint8_t pay[48] = { 0x45 };
    simnode_outbox_clear();
    (void)simnode_host_tx(A, W, pay, sizeof(pay));
    (void)simnode_host_tx(BC, W, pay, sizeof(pay));
    const uint64_t atop = top_pn_to_(A);
    uint64_t gtop = 0;
    (void)simnode_group_pn_top(&gtop);

    simnode_chip_restart_queued();
    simnode_outbox_clear();
    const int pr = umac_mesh_tx_broadcast_probe();
    CHECK(pr == 0 && simnode_outbox_count() == 0u,
          "(14) (red) the probe burst only posts its probe while the restart is pending: nothing reaches "
          "the driver from its task (ret %d)", pr);
    CHECK(simnode_host_tx_nopump(A, W, pay, sizeof(pay)) && simnode_host_tx_nopump(BC, W, pay, sizeof(pay)),
          "(13) (pin) two frames are queued meanwhile");
    simnode_keyinst_clear();
    s_probes_at_done = 99;
    simnode_chip_on_restart_done(probes_at_done_);
    bool asserted = simnode_expect_assert(simnode_pump);
    CHECK(!asserted, "(1) (red) the loop runs the restart without a reset");
    CHECK(s_probes_at_done == 0u && probes_() == 1u,
          "(14) (red) the probe goes out once the restart is done, from the event loop (%u at done, %u after)",
          s_probes_at_done, probes_());
    const struct simnode_frame *u = last_data_to_(A), *g = last_data_to_(BC);
    const struct simnode_keyinst *ka = inst_of_(K_A_MTK, true), *ko = inst_of_(K_OWN, false);
    CHECK(g != NULL && g->air_len != 0u && ko != NULL && g->pn >= ko->tx_pn && g->pn > gtop,
          "(13) (red) the queued broadcast goes after the restart, under our MGTK as put back");
#if CHIPKEY
    CHECK(u != NULL && u->air_len != 0u && ka != NULL && u->pn >= ka->tx_pn && u->pn > atop,
          "(13) (red) the queued frame to A goes after it, under A's MTK as put back");
#else
    (void)ka; (void)atop;
    CHECK(u != NULL && (u->bytes[1] & 0x40u) != 0u, "(13) (red) the queued frame to A goes, host-sealed");
#endif
    simnode_outbox_clear();
    CHECK(umac_mesh_tx_broadcast_probe() == 0 && (simnode_pump(), probes_() == 1u),
          "(14) (pin) after it the probe burst sends again");
}

static void t_twice(void)
{
    printf("--- (15) twice, then a new peer ---\n");
    CHECK(sae_mesh_(), "the mesh starts");
    CHECK(restart_ok_() && restart_ok_(), "(1) (red) two restarts, no reset");
    CHECK(held_(0, false, 1, K_OWN) && simnode_chip_sta_state(aid_(A), NULL) == MORSE_STA_AUTHORIZED,
          "(15) (red) after the second our MGTK and A's station are in the chip again");
#if CHIPKEY
    CHECK(held_(aid_(A), true, 0, K_A_MTK), "(15) (red) and A's MTK");
#endif
    CHECK(g_warthog_chiprestart_n == 2u && g_warthog_chiprestart_mesh == 2u, "(15) (red) counted 2");
    simnode_keyinst_clear();
    keyed_(E, K_E_MTK, NULL);
    unsigned own = 0;
    for (unsigned i = 0; i < simnode_keyinst_count(); i++)
    {
        const struct simnode_keyinst *k = simnode_keyinst_get(i);
        own += !k->pairwise && k->aid == 0u;
    }
    CHECK(own == 0u, "(15) (red) a peer added after it does not install our MGTK again (%u)", own);
#if CHIPKEY
    CHECK(held_(aid_(E), true, 0, K_E_MTK), "(15) (pin) and its MTK goes in as usual");
#endif
}

static int s_probe_mid = 99;
static unsigned s_outbox_mid;
static void probe_mid_(void)
{
    s_outbox_mid = simnode_outbox_count();
    s_probe_mid = umac_mesh_tx_broadcast_probe();
    s_outbox_mid = simnode_outbox_count() - s_outbox_mid;
    simnode_chip_on_restart_done(probes_at_done_); /* the second restart's end */
}

static void t_double(void)
{
    printf("--- (18) a restart posted while another runs ---\n");
    CHECK(sae_mesh_(), "the mesh starts");
    const unsigned b0 = simnode_chipstate()->boots;
    simnode_chip_fail_next_health_check();
    simnode_chip_on_restart_done(probe_mid_);
    simnode_outbox_clear();
    s_probes_at_done = 99;
    CHECK(restart_ok_(), "(18) (red) the check right after the reload fails too: no reset");
    CHECK(s_probe_mid == 0 && s_outbox_mid == 0u,
          "(18) (red) as the first ends, the second still pending: the probe burst only posts its probe (%d)",
          s_probe_mid);
    CHECK(s_probes_at_done == 0u && probes_() == 1u,
          "(18) (red) which goes out after the second, from the event loop (%u at its end, %u after)",
          s_probes_at_done, probes_());
    CHECK(g_warthog_chiprestart_n == 2u && g_warthog_chiprestart_mesh == 2u &&
              simnode_chipstate()->boots == b0 + 2u,
          "(18) (red) the chip restarts twice and the mesh comes back after each (%lu, %lu)",
          (unsigned long)g_warthog_chiprestart_n, (unsigned long)g_warthog_chiprestart_mesh);
    CHECK(held_(0, false, 1, K_OWN) && simnode_chip_sta_state(aid_(A), NULL) == MORSE_STA_AUTHORIZED &&
              simnode_chipstate()->mesh_started,
          "(18) (red) holding our MGTK, A's station and the mesh");
    simnode_outbox_clear();
    CHECK(umac_mesh_tx_broadcast_probe() == 0 && (simnode_pump(), probes_() == 1u),
          "(18) (pin) after both the probe burst sends again");
}

static void restart_refused_(void) { simnode_chip_restart(); }

/* Three broadcasts and three frames to A under the keys; the group and A's top TX PN. */
static void traffic_(uint64_t *gtop, uint64_t *atop)
{
    static const uint8_t pay[48] = { 0x45 };
    simnode_outbox_clear();
    for (int i = 0; i < 3; i++)
    {
        (void)simnode_host_tx(BC, W, pay, sizeof(pay));
        (void)simnode_host_tx(A, W, pay, sizeof(pay));
    }
    *gtop = 0;
    (void)simnode_group_pn_top(gtop);
    *atop = top_pn_to_(A);
}

/* One frame to @p da: the chip's copy of it, or NULL when none reached the chip. */
static const struct simnode_frame *send_(const uint8_t *da)
{
    static const uint8_t pay[48] = { 0x45 };
    simnode_outbox_clear();
    (void)simnode_host_tx(da, W, pay, sizeof(pay));
    return last_data_to_(da);
}

static void t_failures(void)
{
    printf("--- (16)-(17) what cannot come back, and what the chip refuses ---\n");
    CHECK(sae_mesh_(), "the mesh starts");
    simnode_chip_refuse_add_if(0, -95, 0);
    simnode_chip_refuse_add_if(0, -95, 0);
    CHECK(simnode_expect_assert(restart_refused_),
          "(16) (pin) an interface the chip refuses (MESH and STA) resets the board, as before");

    CHECK(sae_mesh_(), "the mesh starts again");
    simnode_set_identity(W, 2);
    CHECK(simnode_expect_assert(restart_refused_),
          "(16) (pin) an interface back at another VIF id (2) resets the board");
    simnode_set_identity(W, 0);

    CHECK(sae_mesh_(), "the mesh starts again");
    simnode_set_channel_status(MMWLAN_ERROR);
    CHECK(simnode_expect_assert(restart_refused_),
          "(16) (red) a channel the chip will not take back resets the board, as upstream's STA restore does");
    simnode_set_channel_status(0);

#if WARTHOG_MESH_CHIP_VIF_MESH
    CHECK(sae_mesh_(), "the mesh starts again");
    simnode_chip_refuse_add_if(MESH_T, -95, 0);
    simnode_chipcmd_clear();
    CHECK(restart_ok_(), "(16) (red) a MESH interface the chip now refuses: no reset");
    const struct simnode_chipstate *s = simnode_chipstate();
    CHECK(s->vif && s->vif_type == STA_T && s->vif_id == 0u && g_warthog_chipvif_fallback == 1u,
          "(16) (red) the mesh falls back to a STA VIF 0, counted (%lu)", (unsigned long)g_warthog_chipvif_fallback);
    CHECK(simnode_chipcmd_n(MORSE_CMD_ID_GET_CAPABILITIES, UINT32_MAX) == 1u,
          "(16) (red) whose capabilities are read, as those of the VIF the mesh now runs on");
    CHECK(s->mesh_beaconing && s->bss_beacon && s->bssid_set,
          "(16) (red) and carries on with the STA VIF's commands: beaconing MESH_CONFIG, BSS_BEACON_CONFIG, BSSID_SET");
    CHECK(held_(0, false, 1, K_OWN) && !simnode_chip_key_held(aid_(A), false, 1, NULL),
          "(16) (red) our MGTK at AID 0; on the STA VIF no peer MGTK");
    CHECK(g_warthog_chiprestart_n == 1u && g_warthog_chiprestart_mesh == 0u,
          "(16) (red) not counted as a full restore: the interface is not the type the mesh started on (mesh %lu)",
          (unsigned long)g_warthog_chiprestart_mesh);
#endif

    uint64_t gtop = 0, atop = 0;
    CHECK(sae_mesh_(), "the mesh starts again");
    g_warthog_host_ccmp_on = HOST_SEALS;
    traffic_(&gtop, &atop);
    const uint16_t aa = aid_(A);
    simnode_chip_refuse_next_arg(MORSE_CMD_ID_INSTALL_KEY, aa, -95);
    simnode_keyinst_clear();
    CHECK(restart_ok_(), "(17) (red) a key refused at the restore: no reset");
#if CHIPKEY
    CHECK(!held_(aa, true, 0, K_A_MTK) && held_(aid_(C), true, 0, K_C_MTK) && held_(0, false, 1, K_OWN),
          "(17) (red) A's refused MTK is not held, C's and our MGTK are");
    CHECK(g_warthog_chiprestart_keyfail == 1u && g_warthog_chiprestart_pending == 1u &&
              g_warthog_chiprestart_mesh == 0u,
          "(17) (red) counted keyfail 1, pending 1, and not as a full restore (%lu, %lu, mesh %lu)",
          (unsigned long)g_warthog_chiprestart_keyfail, (unsigned long)g_warthog_chiprestart_pending,
          (unsigned long)g_warthog_chiprestart_mesh);
    const uint32_t nokey_a = g_warthog_tx_nokey;
    CHECK(send_(A) == NULL && g_warthog_tx_nokey == nokey_a + 1u,
          "(17) (red) a frame to A meanwhile is not handed to the chip under the missing key (nokey)");
    simnode_keyinst_clear();
    simnode_tick();
    const struct simnode_keyinst *ka = inst_of_(K_A_MTK, true);
    CHECK(held_(aa, true, 0, K_A_MTK) && ka != NULL && ka->tx_pn > atop && ka->tx_pn > gtop &&
              g_warthog_chiprestart_retried == 1u && g_warthog_chiprestart_pending == 0u,
          "(17) (red) the service tick puts it back above every PN used (0x%llx > 0x%llx), counted retried",
          ka != NULL ? (unsigned long long)ka->tx_pn : 0ull, (unsigned long long)atop);
    const struct simnode_frame *u = send_(A);
    CHECK(u != NULL && u->air_len != 0u && u->pn > atop,
          "(17) (red) and a frame to A is sealed under it again (PN 0x%llx)", u != NULL ? (unsigned long long)u->pn : 0ull);
#else
    (void)aa;
    CHECK(g_warthog_chiprestart_keyfail == 0u && held_(0, false, 1, K_OWN),
          "(17) (red) host-key build: no peer key goes in, our MGTK goes back");
#endif
    simnode_chip_refusals_clear();

#if PER_STA
    CHECK(sae_mesh_(), "the mesh starts again");
    simnode_chip_refuse_next_arg(MORSE_CMD_ID_INSTALL_KEY, aid_(A), -95);
    simnode_chip_refuse_next_arg(MORSE_CMD_ID_INSTALL_KEY, aid_(A), -95);
    CHECK(restart_ok_(), "(17) (red) A's MTK and MGTK refused at the restore: no reset");
    CHECK(!held_(aid_(A), false, 1, K_A_MGTK) && g_warthog_chiprestart_keyfail == 2u &&
              g_warthog_chiprestart_pending == 2u,
          "(17) (red) A's MGTK is not held either, counted keyfail 2, pending 2");
    simnode_tick();
    CHECK(held_(aid_(A), true, 0, K_A_MTK) && held_(aid_(A), false, 1, K_A_MGTK) &&
              g_warthog_chiprestart_retried == 2u && g_warthog_chiprestart_pending == 0u,
          "(17) (red) the service tick puts both back, counted retried 2");
    simnode_chip_refusals_clear();
#endif

    CHECK(sae_mesh_(), "the mesh starts again");
    simnode_chip_refuse_next_arg(MORSE_CMD_ID_SET_STA_STATE, MORSE_STA_AUTHORIZED, -95);
    CHECK(restart_ok_(), "(17) (red) a station record refused at the restore: no reset");
    CHECK(simnode_chip_sta_state(aid_(A), NULL) == MORSE_STA_ASSOCIATED && g_warthog_chiprestart_sta == 2u &&
              g_warthog_chiprestart_stafail == 1u && g_warthog_chiprestart_pending == 1u &&
              g_warthog_chiprestart_mesh == 0u,
          "(17) (red) A stays ASSOCIATED: counted sta 2, stafail 1, pending 1, not a full restore (%lu, %lu)",
          (unsigned long)g_warthog_chiprestart_sta, (unsigned long)g_warthog_chiprestart_stafail);
    simnode_tick();
    CHECK(simnode_chip_sta_state(aid_(A), NULL) == MORSE_STA_AUTHORIZED &&
              g_warthog_chiprestart_retried == 1u && g_warthog_chiprestart_pending == 0u,
          "(17) (red) the service tick walks A to AUTHORIZED, counted retried");
    simnode_chip_refusals_clear();

    /* Our MGTK refused at the restore: held from the chip, then back above every PN it drew. */
    CHECK(sae_mesh_(), "the mesh starts again");
    g_warthog_host_ccmp_on = HOST_SEALS;
    traffic_(&gtop, &atop);
    simnode_chip_refuse_next_arg(MORSE_CMD_ID_INSTALL_KEY, 0, -95);
    CHECK(restart_ok_(), "(17) (red) our MGTK refused at the restore: no reset");
    CHECK(!simnode_chip_key_held(0, false, 1, NULL) && g_warthog_chiprestart_keyfail == 1u &&
              g_warthog_chiprestart_pending == 1u,
          "(17) (red) it is not held, counted keyfail 1, pending 1 (%lu)", (unsigned long)g_warthog_chiprestart_keyfail);
    const uint32_t nokey = g_warthog_tx_nokey;
    CHECK(send_(BC) == NULL && g_warthog_tx_nokey == nokey + 1u,
          "(17) (red) a broadcast meanwhile is not handed to the chip under the missing key (nokey)");
    simnode_keyinst_clear();
    simnode_tick();
    const struct simnode_keyinst *own = inst_of_(K_OWN, false);
    CHECK(held_(0, false, 1, K_OWN) && own != NULL && own->tx_pn > gtop &&
              g_warthog_chiprestart_retried == 1u && g_warthog_chiprestart_pending == 0u,
          "(17) (red) the service tick puts it back above every PN it drew (0x%llx > 0x%llx), counted retried",
          own != NULL ? (unsigned long long)own->tx_pn : 0ull, (unsigned long long)gtop);
    const struct simnode_frame *g = send_(BC);
    CHECK(g != NULL && g->air_len != 0u && g->pn > gtop,
          "(17) (red) our next broadcast is sealed above the old top (PN 0x%llx)", g != NULL ? (unsigned long long)g->pn : 0ull);

    CHECK(sae_mesh_(), "the mesh starts again");
    traffic_(&gtop, &atop);
    simnode_chip_refuse_next_arg(MORSE_CMD_ID_INSTALL_KEY, 0, -95);
    CHECK(restart_ok_(), "(17) (red) our MGTK refused at the restore again");
    simnode_keyinst_clear();
    keyed_(E, K_E_MTK, NULL);
    own = inst_of_(K_OWN, false);
    CHECK(held_(0, false, 1, K_OWN) && own != NULL && own->tx_pn > gtop,
          "(17) (red) the next peer puts it in, above every PN it drew (0x%llx > 0x%llx)",
          own != NULL ? (unsigned long long)own->tx_pn : 0ull, (unsigned long long)gtop);

    CHECK(sae_mesh_(), "the mesh starts again");
    traffic_(&gtop, &atop);
    simnode_chip_refuse_next_arg(MORSE_CMD_ID_INSTALL_KEY, 0, -95);
    CHECK(restart_ok_(), "(17) (red) our MGTK refused at the restore again");
    simnode_keyinst_clear();
    CHECK(restart_ok_(), "(17) (red) a second restart before the tick");
    own = inst_of_(K_OWN, false);
    CHECK(held_(0, false, 1, K_OWN) && own != NULL && own->tx_pn > gtop && g_warthog_chiprestart_pending == 0u,
          "(17) (red) puts it back, above every PN it drew (0x%llx > 0x%llx)",
          own != NULL ? (unsigned long long)own->tx_pn : 0ull, (unsigned long long)gtop);
    simnode_chip_refusals_clear();

    /* A keyed non-SAE mesh's group key, refused at the restore. */
    for (int how = 0; how < 3; how++)
    {
        CHECK(start_open_(true), "a keyed non-SAE mesh starts");
        CHECK(simnode_add_peer(A) && simnode_add_peer(C), "with A and C");
        g_warthog_host_ccmp_on = 0;
        traffic_(&gtop, &atop);
        simnode_chip_refuse_next_arg(MORSE_CMD_ID_INSTALL_KEY, 0, -95);
        CHECK(restart_ok_() && !simnode_chip_key_held(0, false, 1, NULL) && g_warthog_chiprestart_pending == 1u,
              "(17) (red) its group key refused at the restore: not held, pending 1");
        simnode_keyinst_clear();
        static const char *const by[] = { "the service tick", "the next peer", "a peer leaving" };
        if (how == 0) { simnode_tick(); }
        else if (how == 1) { (void)simnode_add_peer(E); }
        else { simnode_del_peer(C); }
        const struct simnode_keyinst *k = inst_of_(K_P1_MGTK, false);
        CHECK(held_(0, false, 1, K_P1_MGTK) && k != NULL && k->aid == 0u && k->tx_pn > gtop &&
                  g_warthog_chiprestart_pending == 0u,
              "(17) (red) %s puts it back at AID 0 above every PN it drew (0x%llx > 0x%llx)", by[how],
              k != NULL ? (unsigned long long)k->tx_pn : 0ull, (unsigned long long)gtop);
        simnode_chip_refusals_clear();
    }
}

static void t_forced(void)
{
    printf("--- (19) AT+CHIPRESTART goes through the event loop ---\n");
    CHECK(sae_mesh_(), "the mesh starts");
    const unsigned off0 = simnode_chip_calls_off_loop(), b0 = simnode_chipstate()->boots;
    const enum mmwlan_status st = umac_chip_restart_request(umac_data_get_umacd());
    CHECK(st == MMWLAN_SUCCESS && g_warthog_chiprestart_forced == 0u && simnode_chipstate()->boots == b0 &&
              simnode_chip_calls_off_loop() == off0,
          "(19) (red) from the AT task the request is only posted: nothing reaches the driver there (%d)", (int)st);
    simnode_pump();
    CHECK(g_warthog_chiprestart_forced == 1u && g_warthog_chiprestart_n == 1u && g_warthog_chiprestart_mesh == 1u &&
              simnode_chipstate()->boots == b0 + 1u && simnode_chip_calls_off_loop() == off0,
          "(19) (red) the loop fails the health check through the driver and the chip restarts as after a "
          "real failure, the mesh back (forced %lu, restarts %lu)", (unsigned long)g_warthog_chiprestart_forced,
          (unsigned long)g_warthog_chiprestart_n);
    simnode_chip_restart_queued();
    (void)umac_chip_restart_request(umac_data_get_umacd());
    CHECK(!simnode_expect_assert(simnode_pump) && g_warthog_chiprestart_n == 3u && g_warthog_chiprestart_forced == 2u &&
              simnode_chipstate()->boots == b0 + 3u,
          "(19) (pin) one posted while a restart is pending runs after it, on the reloaded driver (restarts %lu)",
          (unsigned long)g_warthog_chiprestart_n);
    const uint32_t dr0 = g_warthog_chiprestart_dropped;
    mmdrv_deinit();
    const enum mmwlan_status st2 = umac_chip_restart_request(umac_data_get_umacd());
    simnode_pump();
    CHECK(st2 == MMWLAN_SUCCESS && g_warthog_chiprestart_dropped == dr0 + 1u && g_warthog_chiprestart_n == 3u &&
              g_warthog_chiprestart_forced == 2u && simnode_chipstate()->boots == b0 + 3u,
          "(19) (red) the driver stopped: the request is posted (%d) but the loop drops it, counted dropped (%lu)",
          (int)st2, (unsigned long)(g_warthog_chiprestart_dropped - dr0));
    (void)mmdrv_init(NULL, "US");
}

/* The STA VIF's refusals, measured on air at every start: BSS_BEACON_CONFIG and MESH_CONFIG, -17. */
static void refuse_sta_vif_(void)
{
    simnode_chip_refuse_next(MORSE_CMD_ID_BSS_BEACON_CONFIG, -17);
    simnode_chip_refuse_next(MORSE_CMD_ID_MESH_CONFIG, -17);
}

static void t_cmdfail(void)
{
    printf("--- (20) restore commands the chip answers otherwise than at the start ---\n");
    CHECK(sae_mesh_(), "the mesh starts");
    simnode_chip_refuse_next(MORSE_CMD_ID_MESH_CONFIG, -5);
    CHECK(restart_ok_(), "(20) (pin) a MESH_CONFIG the chip refuses at the restore: no reset, as at the start");
    CHECK(g_warthog_chiprestart_cmdfail == 1u && g_warthog_chiprestart_n == 1u && g_warthog_chiprestart_mesh == 0u,
          "(20) (red) counted cmdfail 1, and not as a full restore (mesh %lu)", (unsigned long)g_warthog_chiprestart_mesh);
    simnode_chip_refusals_clear();

    s_at_start = refuse_sta_vif_;
    CHECK(sae_mesh_(), "the mesh starts with the chip refusing as on air with a STA VIF");
    refuse_sta_vif_();
    CHECK(restart_ok_() && g_warthog_chiprestart_cmdfail == 0u && g_warthog_chiprestart_mesh == 1u,
          "(20) (pin) the same refusals at the restore count nothing: a full restore (cmdfail %lu)",
          (unsigned long)g_warthog_chiprestart_cmdfail);
    simnode_chip_refusals_clear();
    simnode_chip_refuse_next(MORSE_CMD_ID_MESH_CONFIG, -5);
#if !WARTHOG_MESH_CHIP_VIF_MESH
    simnode_chip_refuse_next(MORSE_CMD_ID_BSS_BEACON_CONFIG, -5);
#endif
    CHECK(restart_ok_() && g_warthog_chiprestart_cmdfail == (WARTHOG_MESH_CHIP_VIF_MESH ? 1u : 2u) &&
              g_warthog_chiprestart_mesh == 1u,
          "(20) (red) another status than the start's is counted (cmdfail %lu)",
          (unsigned long)g_warthog_chiprestart_cmdfail);
    simnode_chip_refusals_clear();
}

static void t_hostfrag_statuses(void)
{
    printf("--- (21) host fragments' TX statuses across a restart ---\n");
#if HOST_SEALS
    CHECK(umac_datapath_mesh_hostfrag_mode() == UMAC_MESH_FRAG_OFF,
          "(21) (pin) host CCMP build: AT+HOSTFRAG cuts nothing");
    return;
#endif
    static uint8_t pay[1000];
    static const struct simnode_rate R_1M0[] = { { 1, 0, 2 } };
    for (unsigned i = 0; i < sizeof(pay); i++) { pay[i] = (uint8_t)(i * 7u); }
    CHECK(start_sae_(), "the mesh starts");
    keyed_(A, K_A_MTK, NULL);
    g_warthog_host_ccmp_on = HOST_SEALS;
    g_warthog_mesh_pmf = 0;
    simnode_set_rate_chain(R_1M0, 1);
    g_warthog_hostfrag = 1;
    simnode_tx_hold(true);
    const uint32_t ok0 = g_warthog_hostfrag_ok, fail0 = g_warthog_hostfrag_fail, acked0 = g_warthog_hostfrag_acked;
    (void)simnode_host_tx(A, W, pay, sizeof(pay));
    CHECK(simnode_tx_held() == 2u, "a 1000-octet MSDU to A at 1 MHz MCS0 goes to the chip as 2 fragments (%u)",
          simnode_tx_held());
    (void)simnode_tx_send_held(0);
    simnode_pump();
    /* The loop's datapath half runs, then the chip reports f1, then its event half runs the
     * restart: its status is read after it. */
    struct umac_data *umacd = umac_data_get_umacd();
    simnode_chip_restart_queued();
    simnode_loop_enter();
    (void)umac_datapath_process(umacd);
    simnode_loop_leave();
    (void)simnode_tx_send_held(0);
    simnode_loop_enter();
    (void)simnode_evt_dispatch_one(umacd);
    simnode_loop_leave();
    simnode_pump();
    simnode_tx_hold(false);
    (void)simnode_host_tx(A, W, pay, sizeof(pay));
    simnode_pump();
    CHECK(g_warthog_chiprestart_n == 1u && g_warthog_hostfrag_acked - acked0 == 4u,
          "the restart ran and every fragment of both MSDUs was acked (%lu)",
          (unsigned long)(g_warthog_hostfrag_acked - acked0));
    CHECK(g_warthog_hostfrag_fail == fail0 && g_warthog_hostfrag_ok == ok0 + 1u,
          "(21) (red) no failed MSDU; the next one counted ok (ok +%lu, fail +%lu)",
          (unsigned long)(g_warthog_hostfrag_ok - ok0), (unsigned long)(g_warthog_hostfrag_fail - fail0));
    g_warthog_hostfrag = 0;
    simnode_set_rate_chain(NULL, 0);
}

#if CHIPKEY
/* Management frames to @p p handed to the chip in the outbox. */
static unsigned mgmt_to_(const uint8_t *p)
{
    unsigned n = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        n += f->is_mgmt && f->len >= 24u && memcmp(f->bytes + 4, p, 6) == 0;
    }
    return n;
}
#endif

static void t_held_mgmt(void)
{
    printf("--- (22) chip-sealed management frames held behind a fragment run, across a restart ---\n");
#if CHIPKEY
    static uint8_t pay[1000];
    static const struct simnode_rate R_1M0[] = { { 1, 0, 2 } };
    CHECK(start_sae_(), "the mesh starts");
    keyed_(A, K_A_MTK, NULL);
    keyed_(C, K_C_MTK, NULL);
    g_warthog_mesh_pmf = 1;
    simnode_set_rate_chain(R_1M0, 1);
    g_warthog_hostfrag = 1;
    simnode_tx_hold(true);
    (void)simnode_host_tx(A, W, pay, sizeof(pay));
    (void)simnode_tx_send_held(0);
    const uint32_t m0 = g_warthog_hostfrag_mgmt;
    simnode_outbox_clear();
    (void)umac_mesh_hwmp_send_preq(A);
    CHECK(g_warthog_hostfrag_mgmt == m0 + 1u && mgmt_to_(A) == 0u,
          "a PREQ to A the chip would seal waits behind A's fragment run (mgmt %lu)",
          (unsigned long)(g_warthog_hostfrag_mgmt - m0));
    simnode_tx_hold(false);
    const uint32_t nk0 = g_warthog_tx_nokey;
    simnode_chip_refuse_next_arg(MORSE_CMD_ID_INSTALL_KEY, aid_(A), -95);
    CHECK(restart_ok_() && !held_(aid_(A), true, 0, K_A_MTK) && g_warthog_chiprestart_keyfail == 1u,
          "a restart whose restore the chip refuses A's MTK");
    simnode_pump();
    CHECK(mgmt_to_(A) == 0u && g_warthog_tx_nokey == nk0 + 1u,
          "(22) (red) the held PREQ is not handed to the reloaded chip, which holds no key for A: dropped, "
          "counted nokey (%u handed, nokey +%lu)", mgmt_to_(A), (unsigned long)(g_warthog_tx_nokey - nk0));
    (void)umac_mesh_hwmp_send_preq(A);
    CHECK(mgmt_to_(A) == 0u && g_warthog_tx_nokey == nk0 + 2u,
          "(22) (red) nor is a new one while the key is owed (nokey +%lu)", (unsigned long)(g_warthog_tx_nokey - nk0));
    (void)umac_mesh_hwmp_send_preq(C);
    CHECK(mgmt_to_(C) == 1u, "(22) (pin) a PREQ to C, whose key went back, is handed");
    simnode_tick();
    simnode_outbox_clear();
    (void)umac_mesh_hwmp_send_preq(A);
    CHECK(held_(aid_(A), true, 0, K_A_MTK) && mgmt_to_(A) == 1u,
          "(22) (pin) once the service tick puts A's MTK back, a PREQ to A is handed (%u)", mgmt_to_(A));
    g_warthog_hostfrag = 0;
    g_warthog_mesh_pmf = 0;
    simnode_set_rate_chain(NULL, 0);
#else
    CHECK(true, "(22) (pin) host-key build: the chip seals no peer's management frame");
#endif
}

/* A's NDP ADDBA Response to us for TID 0: success, immediate policy, 16 frames. */
static uint16_t addba_resp_a_(uint8_t *f, uint8_t token)
{
    memset(f, 0, 33);
    f[0] = 0xd0;
    memcpy(&f[4], W, 6);
    memcpy(&f[10], A, 6);
    memcpy(&f[16], A, 6);
    const uint16_t ps = (uint16_t)((1u << 1) | (16u << 6));
    f[24] = 3;   /* Block Ack */
    f[25] = 129; /* NDP ADDBA Response */
    f[26] = token;
    f[29] = (uint8_t)ps;
    f[30] = (uint8_t)(ps >> 8);
    return 33u;
}

/* The dialog token of the last ADDBA Request to A in the outbox, 0 if none. */
static uint8_t addba_token_a_(void)
{
    uint8_t tok = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (f->is_mgmt && f->len >= 27u && f->bytes[24] == 3u && f->bytes[25] == 128u &&
            memcmp(f->bytes + 4, A, 6) == 0)
        {
            tok = f->bytes[26];
        }
    }
    return tok;
}

/* Data frames to @p da in the outbox. */
static unsigned data_to_(const uint8_t *da)
{
    unsigned n = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        n += !f->is_mgmt && f->len >= 24u && (f->bytes[0] & 0x0cu) == 0x08u && memcmp(f->bytes + 4, da, 6) == 0;
    }
    return n;
}

static void t_ba_wait_restart(void)
{
    printf("--- (23) a cut frame waiting for its DELBA when the chip restarts ---\n");
#if HOST_SEALS
    CHECK(umac_datapath_mesh_hostfrag_mode() == UMAC_MESH_FRAG_OFF,
          "(23) (pin) host CCMP build: AT+HOSTFRAG cuts nothing");
    return;
#endif
    static uint8_t pay[1000], f[64];
    static const struct simnode_rate R_1M0[] = { { 1, 0, 2 } };
    for (unsigned i = 0; i < sizeof(pay); i++) { pay[i] = (uint8_t)(i * 5u); }
    for (unsigned k = 0; k < (CHIPKEY && !HOST_SEALS ? 2u : 1u); k++)
    {
        CHECK(start_sae_(), "the mesh starts");
        keyed_(A, K_A_MTK, NULL);
        g_warthog_host_ccmp_on = HOST_SEALS;
        g_warthog_mesh_pmf = 0;
        simnode_set_ampdu(true);
        (void)simnode_host_tx(A, W, pay, 64);
        (void)simnode_rx(f, addba_resp_a_(f, addba_token_a_()), -60);
        simnode_set_rate_chain(R_1M0, 1);
        g_warthog_hostfrag = 1;
        simnode_tx_hold(true);
        simnode_outbox_clear();
        (void)simnode_host_tx(A, W, pay, sizeof(pay));
        const unsigned held = simnode_tx_held();
        const unsigned before = data_to_(A);
        simnode_tx_hold(false);
        const uint32_t nk0 = g_warthog_tx_nokey;
        if (k == 1u) { simnode_chip_refuse_next_arg(MORSE_CMD_ID_INSTALL_KEY, aid_(A), -95); }
        const bool ok = restart_ok_();
        simnode_pump();
        if (k == 0u)
        {
            CHECK(ok && held == 1u && before == 0u && data_to_(A) == 2u && g_warthog_tx_nokey == nk0,
                  "(23) (red) 1000 octets to A wait for their DELBA, which the restart purges unreported: "
                  "they go once the chip is back, as 2 fragments, not at the 500 ms limit (%u held, %u, then %u)",
                  held, before, data_to_(A));
        }
        else
        {
            CHECK(ok && held == 1u && data_to_(A) == 0u && g_warthog_tx_nokey == nk0 + 1u,
                  "(23) (red) the restored chip lacks A's MTK: the waiting frame is dropped, counted nokey, "
                  "not handed (%u, nokey +%lu)", data_to_(A), (unsigned long)(g_warthog_tx_nokey - nk0));
        }
        simnode_set_ampdu(false);
        g_warthog_hostfrag = 0;
        simnode_set_rate_chain(NULL, 0);
    }
}

static void assert_test_early_(void) { simnode_advance_run(MMWLAN_ASSERT_TEST_DELAY_MS - 1u); }
static void assert_test_due_(void) { simnode_advance_run(1u); }

static void t_loop_requests(void)
{
    printf("--- (24) AT+ASSERTTEST=loop asserts on the event loop; a post that fails says why ---\n");
    CHECK(sae_mesh_(), "the mesh starts");
    struct umac_data *umacd = umac_data_get_umacd();
    const unsigned a0 = simnode_asserts_caught(), t0 = simnode_timeouts_pending();
    const enum mmwlan_status st = umac_assert_test_request(umacd);
    CHECK(st == MMWLAN_SUCCESS && simnode_evt_pending() == 1u && simnode_timeouts_pending() == t0,
          "(24) from the AT task the request is only posted (%d)", (int)st);
    CHECK(!simnode_expect_assert(simnode_pump) && simnode_timeouts_pending() == t0 + 1u,
          "(24) the loop takes it and sets a timeout; nothing asserts yet");
    CHECK(!simnode_expect_assert(assert_test_early_) && simnode_asserts_caught() == a0,
          "(24) nothing asserts in the first %u ms", (unsigned)MMWLAN_ASSERT_TEST_DELAY_MS - 1u);
    CHECK(simnode_expect_assert(assert_test_due_) && simnode_asserts_caught() == a0 + 1u,
          "(24) at %u ms the timeout asserts on the loop (the board resets)",
          (unsigned)MMWLAN_ASSERT_TEST_DELAY_MS);

    /* The simulator's loop is a stand-in: give the core the running loop the firmware has. */
    struct umac_core_data *core = umac_data_get_core(umacd);
    struct mmosal_task *const task0 = core->evtloop_task;
    const bool down0 = core->evtloop_shutting_down;
    core->evtloop_task = (struct mmosal_task *)core;
    core->evtloop_shutting_down = false;
    simnode_evt_discard();
    (void)simnode_evt_fill();
    const enum mmwlan_status full_a = umac_assert_test_request(umacd);
    const enum mmwlan_status full_c = umac_chip_restart_request(umacd);
    core->evtloop_shutting_down = true;
    const enum mmwlan_status stop_a = umac_assert_test_request(umacd);
    const enum mmwlan_status stop_c = umac_chip_restart_request(umacd);
    core->evtloop_shutting_down = false;
    core->evtloop_task = NULL;
    const enum mmwlan_status down_a = umac_assert_test_request(umacd);
    const enum mmwlan_status down_c = umac_chip_restart_request(umacd);
    core->evtloop_task = task0;
    core->evtloop_shutting_down = down0;
    simnode_evt_discard();
    CHECK(full_a == MMWLAN_NO_MEM && full_c == MMWLAN_NO_MEM,
          "(24) the loop's queue full: NO_MEM, AT's \"event queue full\" (%d, %d)", (int)full_a, (int)full_c);
    CHECK(stop_a == MMWLAN_UNAVAILABLE && stop_c == MMWLAN_UNAVAILABLE && down_a == MMWLAN_UNAVAILABLE &&
              down_c == MMWLAN_UNAVAILABLE,
          "(24) the loop stopping or down: UNAVAILABLE, AT's \"chip not running\" (%d, %d, %d, %d)",
          (int)stop_a, (int)stop_c, (int)down_a, (int)down_c);
}

int main(void)
{
    printf("=== chip restart under the mesh (MESH VIF %d, chip keys %d, PN base %d) ===\n",
           (int)WARTHOG_MESH_CHIP_VIF_MESH, (int)CHIPKEY, (int)PNBASE);
    t_recovers();
    t_stations_and_keys();
    t_own_mgtk_waits();
    t_non_sae();
    t_settings();
    t_queued();
    t_twice();
    t_double();
    t_failures();
    t_forced();
    t_cmdfail();
    t_hostfrag_statuses();
    t_held_mgmt();
    t_ba_wait_restart();
    t_loop_requests();
    stop_();
    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_simnode_restart: all passed\n");
    return 0;
}
