/*
 * warthog mesh-support fork -- umac datapath ops for 802.11s mesh mode.
 *
 * Mirrors umac_datapath_ap.c structurally, with a mesh peer table in place of
 * the AP's STA list: the AP dequeue derefs an AP-STA struct that does not
 * exist in mesh mode.
 *
 * The RX dispatch is the substantive part, and it owns two things the vendor
 * path cannot do:
 *   - PROBE_REQ is answered here: a peer in beaconless mode discovers by
 *     probing. On an open mesh receiving a probe also triggers our own peering
 *     Open, as a beacon does.
 *   - ACTION category 15 (SELF_PROTECTED, i.e. Mesh Peering) is handled
 *     directly rather than through umac_datapath_process_rx_action_frame's
 *     supplicant fan-out, which expects hostap's mesh_mpm_action_rx and is
 *     never reached in this build. Peering frames were arriving and being
 *     silently discarded.
 *
 * Peers live in a fixed table of MESH_MAX_PEERS. Data frames go out 4-address,
 * or as 3-address group frames with AT+MESHGRP=1 (umac_mesh_fwd_tx_header).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 */

#include "mmdrv.h"
#include "mmlog.h"
#include "mmpkt.h"
#include "mmwlan.h"
#include "mmwlan_internal.h"
#include "umac/datapath/umac_datapath_data.h"
#include "umac/datapath/umac_datapath_private.h"
#include "umac/mesh/umac_mesh_fwd.h"
#include "umac/data/umac_data.h"
#include "umac/interface/umac_interface.h" /* umac_interface_chip_vif_is_mesh */
#include "umac/supplicant_shim/umac_supp_shim.h"
#include "dot11/dot11.h"
#include "dot11/dot11_utils.h"
#include "common/mac_address.h"
#include "umac/stats/umac_stats.h"
#include "umac/rc/umac_rc.h"
#include "umac/ba/umac_ba.h"
#include "umac/keys/umac_keys.h"
#include "umac/keys/umac_keys_data.h"
#include "umac/keys/connection_keys.h"
#include "mmosal.h"
#include "common/morse_commands.h"  /* enum morse_sta_state */
#include <string.h>
#include "umac/mesh/umac_mesh.h"
#include "umac/mesh/umac_mesh_bip.h"
#include "umac/mesh/umac_mesh_ccmp_hdr.h"
#include "umac/mesh/umac_mesh_ies.h"
#include "umac/frames/frames_common.h" /* frame_is_robust_mgmt */
#include "mmwlan_mesh.h"

/* Data-plane counters (storage in main/at.c; AT+DATASTAT?). */
extern volatile uint32_t g_warthog_tx_data_enq;
extern volatile uint32_t g_warthog_tx_bcast_dup;
extern volatile uint32_t g_warthog_tx_bcast_copy_fail;
extern volatile uint32_t g_warthog_tx_data_deq;
extern volatile uint32_t g_warthog_tx_data_hdr;
extern volatile uint32_t g_warthog_mesh_chip_sta_fail;
extern volatile uint32_t g_warthog_mesh_key_fail;
/* SET_STA_STATE (by state) and INSTALL_KEY the chip refused, and the last status (AT+MESHCFG?). */
extern volatile uint32_t g_warthog_chipcmd_sta_refused[5], g_warthog_chipcmd_key_refused;
extern volatile int32_t g_warthog_chipcmd_sta_status, g_warthog_chipcmd_key_status;
extern volatile uint32_t g_warthog_ampe_mtk_installed, g_warthog_ampe_mgtk_installed;
extern volatile uint32_t g_warthog_mgtk_reinst, g_warthog_mgtk_rsc_fail;
extern volatile uint32_t g_warthog_mesh_secure;
extern volatile uint8_t g_warthog_mesh_self_addr[6];
extern volatile uint32_t g_warthog_peer_fp[4];
extern volatile uint32_t g_warthog_peer_mac[4];
extern volatile uint32_t g_warthog_rekey_req, g_warthog_rekey_done, g_warthog_rekey_aid;
extern volatile uint32_t g_warthog_mesh_repeer_req;

/* Probe-response counters (storage in main/at.c; see AT+PRSPSTAT?). */
extern volatile uint32_t g_warthog_prsp_rx;
extern volatile uint32_t g_warthog_bcn_peer_rx;
bool umac_mesh_beacon_is_our_mesh(const uint8_t *body, uint32_t len);
bool umac_mesh_s1g_beacon_is_our_mesh(const uint8_t *frame, uint32_t len);
extern volatile uint32_t g_warthog_s1g_bcn_rx, g_warthog_s1g_bcn_ours, g_warthog_s1g_bcn_new;
extern volatile uint32_t g_warthog_s1g_bcn_retry;
bool umac_mesh_note_s1g_beacon(const uint8_t *sa);
bool umac_mesh_peering_incomplete(const uint8_t *sa);
extern volatile uint8_t g_warthog_s1g_bcn_sa[6];
int umac_mesh_tx_probe_response(const uint8_t *da);
void umac_mesh_handle_mpm(const uint8_t *ta, const uint8_t *body, uint32_t len);
void umac_mesh_maybe_initiate_mpm(const uint8_t *ta, int16_t rssi, bool floored);
void umac_mesh_handle_probe_req_discovery(const uint8_t *ta, const uint8_t *ies,
                                          uint32_t ies_len, int16_t rssi);

/* --- RX management frame dispatch ---------------------------------------- */

extern volatile uint32_t g_warthog_rx_auth;
extern volatile unsigned int g_warthog_rx_selfprot, g_warthog_rx_action_any;

static void process_rx_mgmt_frame_mesh(struct umac_data *umacd,
                                       struct umac_sta_data *stad,
                                       struct mmpktview *rxbufview)
{
    (void)stad;  /* the established peer the frame came from, or NULL */
    const struct dot11_hdr *header = (struct dot11_hdr *)mmpkt_get_data_start(rxbufview);
    uint16_t frame_control_le = header->frame_control;
    /* Under SAE every path that can start a peering applies the candidate RSSI floor. */
    const int16_t rssi = mmdrv_get_rx_metadata(mmpkt_from_view(rxbufview))->rssi;

    uint16_t subtype = dot11_frame_control_get_subtype(frame_control_le);

    switch (subtype)
    {
        case DOT11_FC_SUBTYPE_ACTION:
        {
            /* Mesh peering (Open/Confirm/Close) is category 15,
             * SELF_PROTECTED. Handle it here rather than via
             * umac_datapath_process_rx_action_frame -> supplicant fan-out:
             * that path expects hostap's mesh_mpm_action_rx, which this build
             * does not reach, so peering frames were being received and
             * silently dropped while the peer sat in OPN_SNT waiting. */
            const uint8_t *frame = (const uint8_t *)mmpkt_get_data_start(rxbufview);
            uint32_t frame_len = mmpkt_get_data_length(rxbufview);
            const uint32_t hdr_len = sizeof(struct dot11_hdr);

            /* One shared minimum. A 24-byte ACTION frame is the smallest the
             * upstream filter admits, and both this handler and the vendor's
             * read the category byte at hdr_len -- so anything shorter than
             * hdr_len + 4 (category, action, and the smallest useful body) is
             * a runt and must not reach either path. */
            if (frame_len < hdr_len + 4)
            {
                break;
            }
            /* Peering frames go to hostap's MPM when SAE/AMPE is running --
             * it owns the handshake then, because the AMPE elements that carry
             * the keys are built and verified inside mesh_mpm/mesh_rsn.
             * warthog's own MPM keeps the open mesh, where there are no keys
             * to exchange and it is the simpler, working path. */
            g_warthog_rx_action_any++;
            if (frame[hdr_len] == DOT11_ACTION_CATEGORY_SELF_PROTECTED)
            {
                g_warthog_rx_selfprot++;
                /* AMPE interop forensics: raw peering frame body exactly as
                 * it arrived, before hostap verification (AT+PLINKRX?). */
                {
                    extern volatile uint8_t g_warthog_plink_rx[192];
                    extern volatile uint16_t g_warthog_plink_rx_len;
                    extern volatile uint16_t g_warthog_plink_rx_full;
                    uint32_t bl = frame_len - hdr_len;
                    uint32_t cp = bl > 192u ? 192u : bl;
                    memcpy((void *)g_warthog_plink_rx, frame + hdr_len, cp);
                    g_warthog_plink_rx_len = (uint16_t)cp;
                    g_warthog_plink_rx_full = (uint16_t)bl;
                }
            }
            if (frame[hdr_len] == DOT11_ACTION_CATEGORY_SELF_PROTECTED &&
                !umac_mesh_sae_active())
            {
                umac_mesh_handle_mpm(dot11_get_ta(header), frame + hdr_len,
                                     frame_len - hdr_len);
                break;
            }
            if (frame[hdr_len] == DOT11_ACTION_CATEGORY_SELF_PROTECTED &&
                frame[hdr_len + 1u] == UMAC_MESH_MPM_ACTION_OPEN &&
                umac_mesh_sae_open_refused(dot11_get_ta(header), frame + hdr_len,
                                           frame_len - hdr_len, rssi))
            {
                break;
            }
            if (frame[hdr_len] == 13u && !umac_datapath_mesh_hwmp_rx_ok(frame, frame_len))
            {
                break;
            }

            umac_datapath_process_rx_action_frame(umacd, stad, rxbufview);
            break;
        }

        case DOT11_FC_SUBTYPE_PROBE_REQ:
        {
            /* Answer it: a peer in beaconless mesh mode discovers us by
             * probing, and an unanswered probe tells it nothing. */
            const uint8_t *ta = dot11_get_ta(header);
            g_warthog_prsp_rx++;
            (void)umac_mesh_tx_probe_response(ta);
            /* Also OPEN toward them: warthog<->warthog has no other initiator.
             * Not under SAE -- there hostap's MPM owns the link, and two state
             * machines opening the same peering race each other's link ids.
             * Not floored: a weak warthog-only link forms from these probes. */
            if (!umac_mesh_sae_active())
            {
                umac_mesh_maybe_initiate_mpm(ta, rssi, false);
            }
            /* Under SAE, a probe request naming our mesh is a beaconless
             * peer announcing itself (its only advertisement) -- offer it to
             * hostap as a candidate. See umac_mesh_handle_probe_req_discovery. */
            {
                const uint8_t *pframe = (const uint8_t *)mmpkt_get_data_start(rxbufview);
                uint32_t plen = mmpkt_get_data_length(rxbufview);
                const uint32_t phdr = sizeof(struct dot11_hdr);
                if (plen > phdr)
                {
                    umac_mesh_handle_probe_req_discovery(ta, pframe + phdr, plen - phdr, rssi);
                }
            }
            break;
        }

        case DOT11_FC_SUBTYPE_BEACON:
        {
            /* A beaconing peer -- mac80211 and OpenMANET nodes beacon rather
             * than probe, so this is the only way we ever hear about one.
             * Treat it exactly like a probe request from that address: answer
             * with a DIRECTED probe response (mac80211 ignores one whose DA is
             * not itself) and open a peering. Gated on the Mesh ID so we do not
             * answer every mesh in range. */
            const uint8_t *ta = dot11_get_ta(header);
            const uint8_t *bframe = (const uint8_t *)mmpkt_get_data_start(rxbufview);
            const uint32_t blen = mmpkt_get_data_length(rxbufview);
            const uint32_t bhdr = sizeof(struct dot11_hdr);
            if (blen > bhdr && umac_mesh_beacon_is_our_mesh(bframe + bhdr, blen - bhdr))
            {
                g_warthog_bcn_peer_rx++;
                (void)umac_mesh_tx_probe_response(ta);
                /* Open only toward a neighbour that says it would accept us
                 * (the length check above covers the 12 fixed octets). */
                if (!umac_mesh_sae_active() &&
                    umac_mesh_ies_peer_openable(bframe + bhdr + 12u, blen - bhdr - 12u, false))
                {
                    umac_mesh_maybe_initiate_mpm(ta, rssi, true);
                }
            }
            break;
        }

        case DOT11_FC_SUBTYPE_PROBE_RSP:
        {
            /* SAE peer discovery from probe responses, and from S1G beacons in
             * umac_mesh_handle_s1g_beacon -- not from legacy beacons.
             *
             * The S1G beacon's address fields survive the chip's
             * S1G->legacy conversion badly (this file already warns that A2/A3
             * arrive zeroed), so the address taken from a beacon is not
             * dependable. Measured consequence: every candidate we offered
             * hostap carried the same address -- a real but unrelated HaLow
             * node that happened to be beaconing our Mesh ID -- so hostap
             * opened exactly one SAE session, with the wrong station, and the
             * warthog peer we could actually key with was never offered at
             * all (offers=96, addp=1, all 95 rejects "already exists").
             *
             * A probe response is addressed to us and carries a genuine TA,
             * plus the same Mesh ID / Mesh Configuration elements
             * mesh_mpm_add_peer() needs. Fixed fields are 12 bytes
             * (timestamp 8, beacon interval 2, capability 2) before the IEs. */
            if (umac_mesh_sae_active())
            {
                const uint8_t *pf = (const uint8_t *)mmpkt_get_data_start(rxbufview);
                uint32_t pflen = mmpkt_get_data_length(rxbufview);
                const uint32_t ie_off = sizeof(struct dot11_hdr) + 12u;
                if (pflen > ie_off)
                {
                    umac_mesh_offer_sae_candidate(dot11_get_ta(header), pf + ie_off,
                                                  (size_t)(pflen - ie_off), rssi);
                }
            }
            /* Forward to supplicant — bss.c uses these for IE caching. */
            umac_supp_process_mgmt_frame(umacd, rxbufview);
            break;
        }

        default:
        {
            /* Other mgmt subtypes (auth/assoc/disassoc/deauth/etc.) — route
             * to supplicant. Anything mesh-specific that needs handling will
             * surface as a WRN here. */
            if (subtype == DOT11_FC_SUBTYPE_AUTH)
            {
                g_warthog_rx_auth++;
                const uint8_t *af = (const uint8_t *)mmpkt_get_data_start(rxbufview);
                const uint32_t alen = mmpkt_get_data_length(rxbufview);
                if (alen > sizeof(struct dot11_hdr))
                {
                    umac_mesh_note_sae_auth(dot11_get_ta(header), af + sizeof(struct dot11_hdr),
                                            alen - sizeof(struct dot11_hdr), rssi);
                }
            }
            MMLOG_ERR("mesh: rx mgmt subtype=%u (fc=0x%04x) -> supplicant fan-out\n",
                      subtype, frame_control_le);
            umac_supp_process_mgmt_frame(umacd, rxbufview);
            break;
        }
    }
}

/* --- Peer table ---------------------------------------------------------- *
 *
 * One umac_sta_data per established mesh peer. This is the core of the
 * data plane: every generic path in umac_datapath.c -- RX filter, reorder,
 * 802.3 conversion, the TX enqueue/dequeue cycle -- resolves the far end
 * through lookup_stad_by_peer_addr()/lookup_stad_by_tx_dest_addr() and does
 * nothing useful while those return NULL. Give it a record and the vendor's
 * data path carries the frames.
 *
 * Fixed-size: every established link takes a slot, so warthog's own MPM table
 * (MPM_MAX_LINKS) must not exceed it, and past MESH_MAX_PEERS add_peer returns
 * MMWLAN_UNAVAILABLE. umac_datapath_mesh_add_peer() fills a slot at ESTAB on an
 * open mesh, and under SAE when hostap adds the station (.sta_add), before
 * SAE/AMPE run; umac_datapath_mesh_del_peer() empties it on Close.
 */
#define MESH_MAX_PEERS UMAC_DATAPATH_MESH_MAX_PEERS

static struct umac_sta_data *s_peers[MESH_MAX_PEERS];
/* Per slot: the link is established (open: from add; SAE: from the AMPE MTK). */
static bool s_peer_estab[MESH_MAX_PEERS];
/* Per slot: the peer runs MFP. Set by its IGTK (AMPE carries one only from a peer with
 * ieee80211w != 0) or a protected unicast path-selection frame; cleared with the slot. */
static bool s_peer_mfp[MESH_MAX_PEERS];
/* Per slot: the record del_peer unlinked from it, until no reader on another task can hold
 * it (see read_begin); the slot is not reused before. old: every reader that can hold it is
 * counted on the side new readers no longer join. */
static struct umac_sta_data *s_dying[MESH_MAX_PEERS];
static bool s_dying_old[MESH_MAX_PEERS];
static uint8_t s_own_addr[6];
static uint16_t s_num_pkts_queued;

bool umac_datapath_mesh_has_free_slot(void)
{
    for (int i = 0; i < MESH_MAX_PEERS; i++)
    {
        if (s_peers[i] == NULL && s_dying[i] == NULL)
        {
            return true;
        }
    }
    return false;
}

/* The Mesh Configuration's view of the table. Reads the slot array only, never
 * a station: the beacon is built on the chip driver's task, not the evtloop. */
static void mesh_capacity_(struct umac_mesh_ies_capacity *out)
{
    uint8_t estab = 0;
    for (int i = 0; i < MESH_MAX_PEERS; i++)
    {
        estab += (s_peers[i] != NULL && s_peer_estab[i]);
    }
    out->accepting = umac_datapath_mesh_has_free_slot();
    out->peerings = estab;
}

/* Each slot is loaded once: off the event loop del_peer may empty it between two loads. */
static struct umac_sta_data *mesh_find_peer_(const uint8_t *addr)
{
    if (addr == NULL)
    {
        return NULL;
    }
    for (int i = 0; i < MESH_MAX_PEERS; i++)
    {
        struct umac_sta_data *p = s_peers[i];
        if (p != NULL && umac_sta_data_matches_peer_addr(p, addr))
        {
            return p;
        }
    }
    return NULL;
}

static int mesh_slot_of_(const uint8_t *addr)
{
    for (int i = 0; addr != NULL && i < MESH_MAX_PEERS; i++)
    {
        struct umac_sta_data *p = s_peers[i];
        if (p != NULL && umac_sta_data_matches_peer_addr(p, addr))
        {
            return i;
        }
    }
    return -1;
}

/* ---- Readers off the event loop ------------------------------------------- *
 *
 * Peers are added and deleted only on the umac event loop, but two paths resolve peer
 * records on other tasks: the TX entry (lwIP's tcpip thread, the batman engine task) and
 * the RX filter (the chip driver task). Each runs between read_begin and read_end.
 * del_peer unlinks a record at once and frees it only after every reader inside at that
 * moment has left, so a record a reader found stays allocated, and no new record takes its
 * address, until the reader is done. Its slot is not reused until then. Nobody waits on
 * anybody. New readers move to the other count once a record is waiting, so a steady
 * stream of readers cannot hold one forever.
 */
static uint16_t s_readers[2];
static uint8_t s_readers_cur; /* the count new readers join */

/* In the critical section: detach into @p out every record no reader can hold. */
static unsigned mesh_reclaim_locked_(struct umac_sta_data *out[MESH_MAX_PEERS])
{
    unsigned n = 0;
    while (s_readers[s_readers_cur ^ 1u] == 0u)
    {
        bool waiting = false;
        for (int i = 0; i < MESH_MAX_PEERS; i++)
        {
            if (s_dying[i] != NULL && s_dying_old[i])
            {
                out[n++] = s_dying[i];
                s_dying[i] = NULL;
            }
            else if (s_dying[i] != NULL)
            {
                s_dying_old[i] = true;
                waiting = true;
            }
        }
        if (!waiting)
        {
            break;
        }
        s_readers_cur ^= 1u; /* every reader that can hold those is on the other side now */
    }
    return n;
}

uint8_t umac_datapath_mesh_read_begin(void)
{
    MMOSAL_TASK_ENTER_CRITICAL();
    const uint8_t side = s_readers_cur;
    s_readers[side]++;
    MMOSAL_TASK_EXIT_CRITICAL();
    return side;
}

static void mesh_free_retired_(struct umac_sta_data *const done[], unsigned n)
{
    for (unsigned i = 0; i < n; i++)
    {
        mmosal_free(done[i]);
    }
}

void umac_datapath_mesh_read_end(uint8_t side)
{
    struct umac_sta_data *done[MESH_MAX_PEERS];
    MMOSAL_TASK_ENTER_CRITICAL();
    s_readers[side & 1u]--;
    const unsigned n = mesh_reclaim_locked_(done);
    MMOSAL_TASK_EXIT_CRITICAL();
    mesh_free_retired_(done, n);
}

/* Event loop, once it is done with a record it unlinked from @p slot: freed now, or by the
 * last reader that may hold it. */
static void mesh_retire_(int slot, struct umac_sta_data *stad)
{
    struct umac_sta_data *done[MESH_MAX_PEERS];
    MMOSAL_TASK_ENTER_CRITICAL();
    s_dying[slot] = stad;
    s_dying_old[slot] = false;
    const unsigned n = mesh_reclaim_locked_(done);
    MMOSAL_TASK_EXIT_CRITICAL();
    mesh_free_retired_(done, n);
}

/* ---- Management frame protection for path selection (SAE only) ---------- */

extern volatile uint32_t g_warthog_mesh_pmf;
extern volatile uint32_t g_warthog_hwmp_prot, g_warthog_hwmp_unprotected;
extern volatile uint32_t g_warthog_hwmp_gp, g_warthog_hwmp_mmie, g_warthog_hwmp_nommie;
extern volatile uint32_t g_warthog_hwmp_unestab;
extern volatile uint32_t g_warthog_ampe_igtk_installed;

/* Our own IGTK, generated by hostap only with AT+MESHPMF=1. Nothing of ours carries an
 * MMIE (group path selection uses our MGTK), so its IPN stays where AMPE reports it. */
static struct
{
    bool valid;
    uint8_t id;
    uint8_t key[UMAC_KEY_AES_128_LEN];
    uint64_t ipn; /* last IPN used; the next frame takes ipn + 1 */
} s_own_igtk;

static bool mesh_slot_mfp_(int slot)
{
    struct umac_sta_data *stad = (slot >= 0) ? s_peers[slot] : NULL;
    if (stad == NULL || !umac_mesh_sae_active() ||
        umac_sta_data_get_security_type(stad) == MMWLAN_OPEN ||
        umac_keys_get_active_key_id(stad, UMAC_KEY_TYPE_PAIRWISE) < 0)
    {
        return false;
    }
    return g_warthog_mesh_pmf != 0u || s_peer_mfp[slot];
}

bool umac_datapath_mesh_peer_mfp(const uint8_t *addr)
{
    return mesh_slot_mfp_(mesh_slot_of_(addr));
}

/* Phase-1 shared keys for a keyed non-SAE mesh (AT+MESHSEC=1). Under SAE the
 * AMPE per-link MTK and the MGTKs are used instead. 16 bytes = CCMP-128. */
/* NOT A SECRET. A counting sequence compiled into every warthog image: the
 * pairwise key (k_mesh_p1_mgtk below is the group key) on a keyed non-SAE mesh.
 *
 * It exists so the data plane can be exercised with CCMP on, not to protect
 * anything: anyone with the firmware has it. It also means "keyed" only
 * interoperates with another warthog carrying the same constant -- a mesh peer
 * that derives real keys (OpenMANET with SAE/AMPE, or any standard secured
 * 802.11s node) cannot decrypt a frame encrypted with it, and warthog cannot
 * decrypt theirs.
 *
 * Peering on such a mesh is unauthenticated: main/mesh.c requests MMWLAN_OPEN
 * unless built with WARTHOG_MESH_SAE (warthog-mesh-sae), where SAE and AMPE
 * (hostap's mesh_rsn.c) derive real per-link keys. Treat a non-SAE mesh as an
 * untrusted transport and protect traffic above it. */
static const uint8_t k_mesh_p1_mtk[UMAC_KEY_AES_128_LEN] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
};
/* The group key goes into the chip once, VIF-wide. */
static bool s_group_key_in_chip;
static uint32_t s_mesh_key_epoch; /* written only by mesh_next_pn_base_ */

/* Our own TX MGTK under SAE. hostap delivers it once, at mesh start and before
 * any peer exists, and this file only learns the mesh VIF from a peer -- so it
 * waits here and goes into the chip with the first one. */
static struct
{
    bool valid;
    uint8_t id;
    uint8_t key[UMAC_KEY_AES_128_LEN];
    uint16_t vif_id; /* VIF of the last successful chip install */
#ifdef WARTHOG_MESH_MGTK_PN_BASE
    /* Read and written only on the umac event loop; the critical sections are defensive. */
    bool based;    /* base assigned: installed, or advertised before the first install */
    uint64_t base; /* TX PN of the last install: every PN used under it is >= base */
    uint64_t sent; /* frames queued at that install or handed to the chip since:
                    * every PN used is <= base + sent */
#endif
} s_own_mgtk;

#ifdef WARTHOG_MESH_MGTK_PN_BASE
/* Group frames under our MGTK with no TX status yet. The chip draws each one's PN when
 * it sends it, from whatever install is in the slot then; one never reported stays counted. */
static uint64_t s_own_group_inflight;
#endif

/* The one TX PN allocator for every chip key install, pairwise and group alike:
 * a fresh 2^20 epoch strictly above @p above and every earlier install. */
static uint64_t mesh_next_pn_base_(uint64_t above)
{
    MMOSAL_TASK_ENTER_CRITICAL();
    uint64_t e = (uint64_t)s_mesh_key_epoch + 1u;
    if (e <= (above >> 20))
    {
        e = (above >> 20) + 1u;
    }
    s_mesh_key_epoch = (uint32_t)e;
    MMOSAL_TASK_EXIT_CRITICAL();
    return e << 20;
}

/* The highest PN our own MGTK can have used, or 0 when it has no base. */
static uint64_t mesh_own_group_pn_top_(void)
{
    uint64_t top = 0;
#ifdef WARTHOG_MESH_MGTK_PN_BASE
    MMOSAL_TASK_ENTER_CRITICAL();
    if (s_own_mgtk.based)
    {
        top = s_own_mgtk.base + s_own_mgtk.sent;
    }
    MMOSAL_TASK_EXIT_CRITICAL();
#endif
    return top;
}

#ifdef WARTHOG_MESH_MGTK_PN_BASE
static void mesh_own_pn_store_(uint64_t base)
{
    MMOSAL_TASK_ENTER_CRITICAL();
    s_own_mgtk.based = true;
    s_own_mgtk.base = base;
    s_own_mgtk.sent = s_own_group_inflight; /* still queued: they draw from this base */
    MMOSAL_TASK_EXIT_CRITICAL();
}
#endif

/* A key into the chip, true if it took it. A refusal (its status, which morse_cmd_tx does
 * not return) is counted and fails the install, as Linux's does (morse_driver command.c:214). */
static bool mesh_chip_install_key_(uint16_t vif_id, uint16_t aid, struct mmdrv_key_conf *kc)
{
    int32_t st = 0;
    if (mmdrv_install_key_status(vif_id, aid, kc, &st) != 0)
    {
        return false;
    }
    if (st != 0)
    {
        g_warthog_chipcmd_key_refused++;
        g_warthog_chipcmd_key_status = st;
        return false;
    }
    return true;
}

/* Our own MGTK into the chip's group slot: at TX PN 0, or with WARTHOG_MESH_MGTK_PN_BASE
 * at a fresh base above every earlier install and every PN our MGTK has used. */
static enum mmwlan_status mesh_put_own_mgtk_(uint16_t vif_id)
{
    uint64_t pn = 0;
#ifdef WARTHOG_MESH_MGTK_PN_BASE
    pn = mesh_next_pn_base_(mesh_own_group_pn_top_());
#endif
    struct mmdrv_key_conf kc = { .is_pairwise = false, .key_idx = s_own_mgtk.id,
                                 .length = UMAC_KEY_AES_128_LEN, .tx_pn = pn };
    memcpy(kc.key, s_own_mgtk.key, sizeof(s_own_mgtk.key));
    if (!mesh_chip_install_key_(vif_id, 0, &kc))
    {
        MMLOG_WRN("mesh: own MGTK chip install failed\n");
        return MMWLAN_ERROR;
    }
#ifdef WARTHOG_MESH_MGTK_PN_BASE
    mesh_own_pn_store_(pn);
#endif
    s_own_mgtk.vif_id = vif_id;
    s_group_key_in_chip = true;
    return MMWLAN_SUCCESS;
}

static enum mmwlan_status mesh_install_own_mgtk_(uint16_t vif_id)
{
    if (mesh_put_own_mgtk_(vif_id) != MMWLAN_SUCCESS)
    {
        return MMWLAN_ERROR;
    }
    g_warthog_ampe_mgtk_installed++;
    MMLOG_INF("mesh: own TX MGTK installed in chip group slot (key_id %u)\n",
              (unsigned)s_own_mgtk.id);
    return MMWLAN_SUCCESS;
}

int umac_datapath_mesh_own_group_key_id(void)
{
    return s_own_mgtk.valid ? (int)s_own_mgtk.id : -1;
}

void umac_datapath_mesh_own_group_tx_note(void)
{
#ifdef WARTHOG_MESH_MGTK_PN_BASE
    MMOSAL_TASK_ENTER_CRITICAL();
    s_own_mgtk.sent++;
    s_own_group_inflight++;
    MMOSAL_TASK_EXIT_CRITICAL();
#endif
}

void umac_datapath_mesh_own_group_tx_done(void)
{
#ifdef WARTHOG_MESH_MGTK_PN_BASE
    MMOSAL_TASK_ENTER_CRITICAL();
    if (s_own_group_inflight != 0)
    {
        s_own_group_inflight--;
    }
    MMOSAL_TASK_EXIT_CRITICAL();
#endif
}

/* The Key RSC AMPE advertises for our own MGTK: one below the chip's next PN. The chip's PN
 * cannot be read, so if a frame may have drawn one since the last install it is re-installed
 * at a fresh base; if that fails, the old base still lies below every PN it will use. */
enum mmwlan_status umac_datapath_mesh_own_group_rsc(uint8_t key_id, uint8_t rsc[6])
{
    if (rsc == NULL)
    {
        return MMWLAN_INVALID_ARGUMENT;
    }
    memset(rsc, 0, 6);
    {
        MMOSAL_TASK_ENTER_CRITICAL();
        const bool igtk = s_own_igtk.valid && key_id == s_own_igtk.id;
        const uint64_t ipn = s_own_igtk.ipn;
        MMOSAL_TASK_EXIT_CRITICAL();
        if (igtk)
        {
            for (int i = 0; i < 6; i++)
            {
                rsc[i] = (uint8_t)(ipn >> (8 * i));
            }
            return MMWLAN_SUCCESS;
        }
    }
#ifdef WARTHOG_MESH_MGTK_PN_BASE
    if (!s_own_mgtk.valid || key_id != s_own_mgtk.id)
    {
        return MMWLAN_SUCCESS; /* never installed, never used: 0 is exact */
    }
    if (!s_own_mgtk.based)
    {
        mesh_own_pn_store_(mesh_next_pn_base_(0)); /* not in the chip yet: installs go above */
    }
    else if (s_group_key_in_chip && s_own_mgtk.sent != 0)
    {
        if (mesh_put_own_mgtk_(s_own_mgtk.vif_id) == MMWLAN_SUCCESS)
        {
            g_warthog_mgtk_reinst++;
        }
        else
        {
            g_warthog_mgtk_rsc_fail++;
        }
    }
    uint64_t v = s_own_mgtk.base - 1u;
    for (int i = 0; i < 6; i++)
    {
        rsc[i] = (uint8_t)(v >> (8 * i));
    }
#else
    (void)key_id;
#endif
    return MMWLAN_SUCCESS;
}

static const uint8_t k_mesh_p1_mgtk[UMAC_KEY_AES_128_LEN] = {
    0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78,
    0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0
};

/* Every peer shares ONE pairwise key, because this chip firmware's mesh
 * interface (mm6108.mbin rel_1_17_6) holds exactly one.
 *
 * Settled by experiment, not inference. Built with WARTHOG_MESH_PERLINK_MTK so
 * each link had its own key -- verified genuinely distinct and symmetric via
 * AT+KEYFP? (an FNV-1a over the whole key; all three links matched end to end
 * and no two links shared a value) -- then re-installed one peer's key at a
 * time with AT+REKEY=<n> on a three-board mesh:
 *
 *     baseline            F8738D->F8823D 3/3   F8738D->81BA51 0/3
 *     REKEY=0 (aid 1)     F8738D->F8823D 0/3   F8738D->81BA51 0/3
 *     REKEY=1 (aid 2)     F8738D->F8823D 3/3   F8738D->81BA51 0/3
 *
 * Connectivity follows the most recent install exactly: re-installing peer 0's
 * key breaks peer 1's link, re-installing peer 1's restores it. One slot,
 * last write wins, even though each peer has its own AID and the AIDs are
 * passed correctly. A link then only works when BOTH ends happen to hold each
 * other's key -- which is why one pair works while a link to a third board
 * fails. (The `hw` index INSTALL_KEY returns is only an echo and proves
 * nothing about slot allocation; the re-install experiment above is what
 * establishes the single-slot behavior.)
 *
 * The consequence is the important part: AMPE derives a DISTINCT MTK per link,
 * so SAE/AMPE -- and therefore OpenMANET interop -- cannot use chip crypto with
 * more than one peer as this SDK drives it. Host software CCMP is not an
 * optimisation for that path, it is the prerequisite. mesh_derive_mtk_() below
 * stays behind the flag as the seam where a real per-link key lands once that
 * exists.
 */
/* One SET_STA_STATE; a refusal (its status, which morse_cmd_tx does not return) is counted
 * by state, and the station carries on as before. Returns the transport result. */
static int mesh_chip_sta_state_(uint16_t vif_id, uint16_t aid, const uint8_t *peer_addr,
                                enum morse_sta_state state)
{
    int32_t st = 0;
    int r = mmdrv_update_sta_state_status(vif_id, aid, peer_addr, state, &st);
    if (r == 0 && st != 0)
    {
        if ((unsigned)state < sizeof(g_warthog_chipcmd_sta_refused) / sizeof(g_warthog_chipcmd_sta_refused[0]))
        {
            g_warthog_chipcmd_sta_refused[state]++;
        }
        g_warthog_chipcmd_sta_status = st;
        MMLOG_WRN("mesh: chip refused sta_state %d for " MM_MAC_ADDR_FMT " (status %ld)\n",
                  (int)state, MM_MAC_ADDR_VAL(peer_addr), (long)st);
    }
    return r;
}

/* Walk a station up to AUTHORIZED in the chip. Same sequence umac_ap_update_sta()
 * uses; AID is 1-based (0 means "no station" to the chip). */
static void mesh_chip_register_sta_(uint16_t vif_id, uint16_t aid, const uint8_t *peer_addr)
{
    static const enum morse_sta_state seq[] = {
        MORSE_STA_AUTHENTICATED, MORSE_STA_ASSOCIATED, MORSE_STA_AUTHORIZED
    };
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++)
    {
        int r = mesh_chip_sta_state_(vif_id, aid, peer_addr, seq[i]);
        if (r != 0)
        {
            MMLOG_WRN("mesh: chip sta_state %d for " MM_MAC_ADDR_FMT " -> %d\n",
                      (int)seq[i], MM_MAC_ADDR_VAL(peer_addr), r);
            g_warthog_mesh_chip_sta_fail++;
        }
    }
}

static void mesh_derive_mtk_(uint8_t out[UMAC_KEY_AES_128_LEN], const uint8_t *a, const uint8_t *b)
{
    const bool a_first = memcmp(a, b, MMWLAN_MAC_ADDR_LEN) <= 0;
    const uint8_t *lo = a_first ? a : b;
    const uint8_t *hi = a_first ? b : a;
    for (int i = 0; i < UMAC_KEY_AES_128_LEN; i++)
    {
        out[i] = (uint8_t)(k_mesh_p1_mtk[i] ^ lo[i % MMWLAN_MAC_ADDR_LEN] ^
                           (uint8_t)(hi[i % MMWLAN_MAC_ADDR_LEN] * 31u + (unsigned)i));
    }
}

static enum mmwlan_status umac_datapath_mesh_install_peer_keys(struct umac_sta_data *stad,
                                                               uint16_t vif_id)
{
    struct umac_key mtk = { .key_id = 0, .key_type = UMAC_KEY_TYPE_PAIRWISE,
                            .key_len = UMAC_KEY_AES_128_LEN };
#ifdef WARTHOG_MESH_PERLINK_MTK
    {
        uint8_t peer[MMWLAN_MAC_ADDR_LEN];
        umac_sta_data_get_peer_addr(stad, peer);
        mesh_derive_mtk_(mtk.key_data, s_own_addr, peer);
    }
#else
    memcpy(mtk.key_data, k_mesh_p1_mtk, sizeof(k_mesh_p1_mtk));
#endif

    /* Re-installed for every peer, with a PN that only ever moves FORWARD.
     *
     * Two hardware behaviours pull in opposite directions here. The key slot
     * is VIF-wide, and removing the station it was installed against takes the
     * key with it -- expire one dead peer and every surviving link goes silent
     * -- so the key has to be re-installed whenever a peer is added. But a
     * plain re-install restarts the chip's TX packet number at zero, and every
     * existing peer then rejects our frames as replays because its RX window
     * has already passed that PN (measured: rxdrop reason=5, replay=11,
     * pn=23, on both survivors after a third board rejoined).
     *
     * Starting each install a wide margin above the last satisfies both: the
     * key is always present, and no receiver ever sees the PN go backwards.
     * 2^20 per epoch is far more frames than a link will carry between two
     * peer changes, and 48-bit CCMP PN space allows 2^28 (~2.7e8) epochs. */
    mtk.tx_seq = mesh_next_pn_base_(mesh_own_group_pn_top_());

    /* Publish peer -> key fingerprint. A per-link key must derive IDENTICALLY
     * on both ends; comparing the two boards' fingerprints for the same link
     * settles that in one command instead of by inspection. */
    {
        uint8_t peer[MMWLAN_MAC_ADDR_LEN];
        umac_sta_data_get_peer_addr(stad, peer);
        for (int i = 0; i < MESH_MAX_PEERS; i++)
        {
            if (s_peers[i] == stad || s_peers[i] == NULL)
            {
                /* FNV-1a over the WHOLE key. Sampling a few bytes is not
                 * enough: bytes 0-2 and 15 depend only on the shared MAC
                 * prefix, so two different keys can print the same sample. */
                uint32_t h = 2166136261u;
                for (int b = 0; b < UMAC_KEY_AES_128_LEN; b++)
                {
                    h = (h ^ mtk.key_data[b]) * 16777619u;
                }
                g_warthog_peer_fp[i] = h;
                g_warthog_peer_mac[i] = ((uint32_t)peer[3] << 16) | ((uint32_t)peer[4] << 8) |
                                        (uint32_t)peer[5];
                break;
            }
        }
    }

#ifdef WARTHOG_MESH_NO_CHIP_KEY
    /* Deliberately withhold the pairwise key from the CHIP, keeping it only in
     * the host keychain.
     *
     * The feasibility test for host software CCMP: with no key, the chip cannot
     * decrypt this peer's frames, and the question is whether it DELIVERS them
     * undecrypted (MMDRV_RX_FLAG_DECRYPTED clear, which our RX tags as
     * rxdrop reason=4 with the ciphertext intact) or discards them silently. If
     * it delivers, that gate is where software decryption hooks -- and no
     * firmware parameter is needed, which matters because
     * MORSE_CMD_PARAM_ID_CRYPTO_IN_HOST is accepted and ignored by this build.
     *
     * TX is expected to break on a board built this way: with no chip key the
     * frame goes out unprotected and the peer drops it as plaintext
     * (reason=3). That is fine -- this flag exists to answer the RX question. */
    if (!connection_keys_install_key(&umac_sta_data_get_keys(stad)->keys, &mtk))
    {
        MMLOG_WRN("mesh: MTK keychain install failed\n");
        return MMWLAN_ERROR;
    }
    enum mmwlan_status st = MMWLAN_SUCCESS;
#else
    enum mmwlan_status st = umac_keys_install_key(stad, vif_id, &mtk);
    if (st != MMWLAN_SUCCESS)
    {
        MMLOG_WRN("mesh: MTK install failed %d\n", (int)st);
        return st;
    }
#endif

    /* On this firmware a second GROUP key install on the mesh interface breaks
     * group decryption; the Linux order (own MGTK at aid 0 first) is untested.
     *
     * umac_keys_install_key() pushes to the chip at the stad's AID, so
     * installing the MGTK on every peer stad handed the chip the same group
     * key under aid 1, aid 2, ... With two peers the chip then failed to
     * decrypt group frames entirely: measured across three boards,
     * "no HW decryption" (rxdrop reason=4) on 100% of group frames --
     * key id 1, fc=0x4288 -- and ZERO unicast failures. That kills ARP, so
     * unicast never resolves either and the link looks dead end to end.
     *
     * Install it to the CHIP exactly once, at aid 0 (the VIF-wide slot the
     * AP path uses for its GTK), and add it to each peer's HOST keychain so
     * ccmp_is_valid() can still do the per-sender replay check on RX. */
    struct umac_key mgtk = { .key_id = 1, .key_type = UMAC_KEY_TYPE_GROUP,
                             .key_len = UMAC_KEY_AES_128_LEN };
    memcpy(mgtk.key_data, k_mesh_p1_mgtk, sizeof(k_mesh_p1_mgtk));

    if (!connection_keys_install_key(&umac_sta_data_get_keys(stad)->keys, &mgtk))
    {
        MMLOG_WRN("mesh: MGTK keychain install failed\n");
        return MMWLAN_ERROR;
    }

    if (!s_group_key_in_chip)
    {
        struct mmdrv_key_conf kc = { .is_pairwise = false, .key_idx = mgtk.key_id,
                                     .length = mgtk.key_len, .tx_pn = 0 };
        memcpy(kc.key, mgtk.key_data, mgtk.key_len);
        if (!mesh_chip_install_key_(vif_id, 0, &kc))
        {
            MMLOG_WRN("mesh: MGTK chip install failed\n");
            return MMWLAN_ERROR;
        }
        s_group_key_in_chip = true;
    }
    return st;
}

/* Put a peer's own pairwise key back in the chip: its AMPE MTK under SAE, the
 * constant on a keyed non-SAE mesh, nothing on an open one. True if one went in. */
static bool mesh_restore_peer_key_(struct umac_sta_data *stad, uint16_t vif_id)
{
    if (umac_mesh_sae_active())
    {
#ifndef WARTHOG_MESH_AMPE_NO_CHIP_KEY
        /* An unkeyed candidate has no MTK yet. The chip's PN counter is shared by
         * every link, so the reinstall starts a fresh epoch, never below it. */
        int kid = umac_keys_get_active_key_id(stad, UMAC_KEY_TYPE_PAIRWISE);
        if (kid >= 0)
        {
            return umac_keys_reinstall_key(stad, vif_id, (uint8_t)kid,
                                           mesh_next_pn_base_(mesh_own_group_pn_top_())) ==
                   MMWLAN_SUCCESS;
        }
#endif
        return false;
    }
    if (g_warthog_mesh_secure)
    {
        return umac_datapath_mesh_install_peer_keys(stad, vif_id) == MMWLAN_SUCCESS;
    }
    return false;
}

enum mmwlan_status umac_datapath_mesh_add_peer(struct umac_data *umacd, uint16_t vif_id,
                                               const uint8_t *own_addr,
                                               const uint8_t *peer_addr, bool sae)
{
    if (umacd == NULL || own_addr == NULL || peer_addr == NULL ||
        mm_mac_addr_is_multicast(peer_addr))
    {
        return MMWLAN_INVALID_ARGUMENT;
    }
    if (mesh_find_peer_(peer_addr) != NULL)
    {
        return MMWLAN_SUCCESS; /* re-peering the same node: nothing to do */
    }

    int slot = -1;
    for (int i = 0; i < MESH_MAX_PEERS; i++)
    {
        if (s_peers[i] == NULL && s_dying[i] == NULL)
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
    {
        return MMWLAN_UNAVAILABLE;
    }

    struct umac_sta_data *stad = umac_sta_data_alloc(umacd);
    if (stad == NULL)
    {
        return MMWLAN_NO_MEM;
    }

    /* umac_sta_data_alloc() callocs the record but does NOT initialise the
     * keychain: connection_keys_init() sets active_key_lookup[*] = -1, and a
     * zeroed one reads as "key 0 is active for every type" before any key is
     * installed. The AP path calls this separately after alloc; the reference
     * mesh port does too. Without it the CCMP RX check sees a BLANK key. */
    umac_keys_init(stad);

    memcpy(s_own_addr, own_addr, sizeof(s_own_addr));
    /* Publish the address the KEY DERIVATION uses. It must equal the TA our
     * peers see, or a symmetric per-link key derives differently on each end
     * and only the sender can decrypt. */
    memcpy((void *)g_warthog_mesh_self_addr, own_addr, MMWLAN_MAC_ADDR_LEN);

    /* Same recipe umac_ap_add_sta() uses to stand up a station, minus the AID
     * (mesh has none; the AP path only uses it for the TIM bitmap).
     *
     * BSSID = OUR address. In 802.11s the mesh has no BSSID; mac80211 uses
     * vif->addr, and the datapath's construct_80211_data_header() reads it
     * back via umac_sta_data_get_bssid() to fill addr2 (TA). */
    umac_sta_data_set_vif_id(stad, vif_id);
    umac_sta_data_set_bssid(stad, own_addr);
    umac_sta_data_set_peer_addr(stad, peer_addr);
    /* Keyed when AT+MESHSEC=1 (the default) on a non-SAE mesh, open when
     * AT+MESHSEC=0. Open data is delivered as long as it carries a Mesh Control
     * field (umac_datapath_process_tx_frame adds one to every mesh frame).
     *
     * Keyed: mark the stad as secured (this makes the TX path set Protected +
     * HW_ENC and pick a key) and install a pairwise + group key on it. The
     * chip encrypts on TX and decrypts+delivers on RX; the vendor RX path
     * already strips the CCMP header and replay-checks against the stad key.
     * The RX-side check "security != OPEN => drop plaintext non-EAPOL" is also
     * what we want: a keyed mesh must not accept clear-text data. */
    /* Under SAE the station starts UNPROTECTED and is promoted when AMPE
     * delivers the MTK (umac_datapath_mesh_set_peer_key).
     *
     * Marking it secured here instead looks harmless and is fatal: the TX path
     * then sets Protected and asks the chip to encrypt with a key that does
     * not exist yet, so the MPM peering frames that AMPE needs in order to
     * derive that key are the ones destroyed (measured: peering frames
     * transmitted on both ends, zero received on either, while SAE
     * Authentication -- a different, unprotected path -- crossed normally).
     * 802.11s peering is in the clear until the link is keyed; AMPE protects
     * its own elements. */
    umac_sta_data_set_security(stad,
                               (!sae && g_warthog_mesh_secure) ? MMWLAN_SAE : MMWLAN_OPEN,
                               MMWLAN_PMF_DISABLED);

    /* Rate control, otherwise every frame goes out at the base rate. */
    umac_rc_start(stad, /*sgi_flags=*/0, /*max_mcs=*/7);

    /* Register the peer with the CHIP, not just the host. This is what makes
     * data frames come out the far side. Measured without it: the chip ACKs
     * every one of our data frames at the MAC layer (txst acked=137 noack=0)
     * and the receiving host still sees rx_data=0 -- the firmware silently
     * drops data from a station it does not know. Management frames were
     * unaffected (peering worked), which is why this looked like a data-plane
     * bug rather than a station-table one. Same sequence umac_ap_update_sta()
     * walks; AID is 1-based and per-peer (0 means "no station" to the chip). */
    uint16_t aid = (uint16_t)(slot + 1);
    umac_sta_data_set_aid(stad, aid);
    mesh_chip_register_sta_(vif_id, aid, peer_addr);

    /* Keys on a keyed non-SAE mesh: a fixed shared MTK (pairwise, key 0) and
     * MGTK (group, key 1), identical on every node -- the same "static/shared"
     * phase the reference port shipped before SAE. The MTK goes on the PEER's
     * stad, keyed to the peer's AID: that is how the chip selects the key for
     * frames to/from that station. The MGTK goes to the chip once, on AID 0
     * (the chip's "no station" slot), so our own broadcast TX has a key. */
    /* NOT under SAE. The hardcoded MTK/MGTK below is a shared constant with no
     * secrecy value (it is in this source file); installing it on a SAE link
     * would overwrite the AMPE-derived per-link key that .set_key installs a
     * moment later, silently downgrading a real handshake to a known key.
     * Under SAE the station is left unkeyed here and keyed by AMPE. */
    if (!sae && g_warthog_mesh_secure &&
        umac_datapath_mesh_install_peer_keys(stad, vif_id) != MMWLAN_SUCCESS)
    {
        g_warthog_mesh_key_fail++;
    }
    /* Our own TX MGTK arrived at mesh start, before this VIF had a peer. */
    if (sae && s_own_mgtk.valid && !s_group_key_in_chip)
    {
        (void)mesh_install_own_mgtk_(vif_id);
    }

    s_peer_estab[slot] = !sae;
    s_peer_mfp[slot] = false;
    s_peers[slot] = stad;
    MMLOG_INF("mesh: peer " MM_MAC_ADDR_FMT " added (slot %d)\n", MM_MAC_ADDR_VAL(peer_addr), slot);
    return MMWLAN_SUCCESS;
}

/* Install a key that came from somewhere else -- in practice AMPE, via the
 * supplicant shim's .set_key driver op.
 *
 * This is the same install the hardcoded path does, with the same two
 * constraints, which are properties of this chip firmware rather than of the key:
 *
 *  - the pairwise TX packet number must only ever move FORWARD across
 *    installs, or existing peers reject our frames as replays; and
 *  - one group key goes into the chip, at aid 0 (a second install broke group
 *    RX on this firmware). It is our own TX MGTK; each peer's MGTK stays in
 *    that peer's host keychain.
 *
 * @param peer_addr  the peer the key belongs to, or the broadcast address for
 *                   our own TX MGTK.
 * @param pairwise   true for the MTK, false for the MGTK.
 * @param rsc        a peer MGTK's Key RSC from AMPE (little-endian, @p rsc_len
 *                   octets), or NULL. Ignored for every other key.
 */
enum mmwlan_status umac_datapath_mesh_set_peer_key(const uint8_t *peer_addr, const uint8_t *key,
                                                   uint8_t key_len, uint8_t key_id, bool pairwise,
                                                   const uint8_t *rsc, size_t rsc_len)
{
    if (peer_addr == NULL || key == NULL || key_len != UMAC_KEY_AES_128_LEN)
    {
        return MMWLAN_INVALID_ARGUMENT;
    }
    /* Our own TX MGTK arrives against the broadcast address, not a peer, so the
     * peer lookup below would reject it and leave our broadcasts keyed wrong. */
    if (!pairwise && (peer_addr[0] & 0x01) != 0)
    {
        s_own_mgtk.valid = true;
        s_own_mgtk.id = key_id;
        memcpy(s_own_mgtk.key, key, key_len);
#ifdef WARTHOG_MESH_MGTK_PN_BASE
        /* hostap may deliver the same key again: move the allocator past every PN the
         * last install used, or the next install could reuse them under that key. */
        (void)mesh_next_pn_base_(mesh_own_group_pn_top_());
        MMOSAL_TASK_ENTER_CRITICAL();
        s_own_mgtk.based = false; /* its next install takes a fresh base */
        s_own_mgtk.sent = 0;
        MMOSAL_TASK_EXIT_CRITICAL();
#endif
        s_group_key_in_chip = false;
        for (int i = 0; i < MESH_MAX_PEERS; i++)
        {
            if (s_peers[i] != NULL)
            {
                return mesh_install_own_mgtk_(umac_sta_data_get_vif_id(s_peers[i]));
            }
        }
        /* hostap's normal order: the mesh starts before any peer does. */
        MMLOG_INF("mesh: own TX MGTK stored; it goes into the chip with the first peer\n");
        return MMWLAN_SUCCESS;
    }

    struct umac_sta_data *stad = mesh_find_peer_(peer_addr);
    if (stad == NULL)
    {
        MMLOG_WRN("mesh: set_key for unknown peer " MM_MAC_ADDR_FMT "\n",
                  MM_MAC_ADDR_VAL(peer_addr));
        return MMWLAN_ERROR;
    }
    uint16_t vif_id = umac_sta_data_get_vif_id(stad);

    struct umac_key k = { .key_id = key_id, .key_len = key_len,
                          .key_type = pairwise ? UMAC_KEY_TYPE_PAIRWISE : UMAC_KEY_TYPE_GROUP };
    memcpy(k.key_data, key, key_len);

    if (pairwise)
    {
        k.tx_seq = mesh_next_pn_base_(mesh_own_group_pn_top_());
#ifdef WARTHOG_MESH_AMPE_NO_CHIP_KEY
        /* Same feasibility probe as WARTHOG_MESH_NO_CHIP_KEY above, on the
         * path that actually matters: AMPE. Keep the MTK in the host keychain
         * and withhold it from the chip, so the chip cannot decrypt this peer
         * and we learn whether it hands the frame up undecrypted -- which is
         * where software CCMP would hook -- or eats it in firmware, which
         * would end the idea. Unlike CRYPTO_IN_HOST, which this build accepts
         * and ignores, withholding the key needs nothing from the firmware.
         *
         * Expect TX to this peer to stop working on such a build. That is the
         * cost of asking the RX question on its own. */
        enum mmwlan_status st =
            connection_keys_install_key(&umac_sta_data_get_keys(stad)->keys, &k)
                ? MMWLAN_SUCCESS
                : MMWLAN_ERROR;
#else
        enum mmwlan_status st = umac_keys_install_key(stad, vif_id, &k);
#endif
        if (st != MMWLAN_SUCCESS)
        {
            MMLOG_WRN("mesh: AMPE MTK install failed %d\n", (int)st);
            g_warthog_mesh_key_fail++;
            return st;
        }
        /* Now -- and only now -- the link is keyed, so the TX path may set
         * Protected and the RX path may insist on it. */
        umac_sta_data_set_security(stad, MMWLAN_SAE, MMWLAN_PMF_DISABLED);
        uint16_t aid = umac_sta_data_get_aid(stad);
        if (aid >= 1u && aid <= MESH_MAX_PEERS)
        {
            s_peer_estab[aid - 1u] = true; /* AMPE done: counts in Formation Info */
        }
        g_warthog_ampe_mtk_installed++;
        MMLOG_INF("mesh: AMPE MTK installed for " MM_MAC_ADDR_FMT "\n",
                  MM_MAC_ADDR_VAL(peer_addr));
        return MMWLAN_SUCCESS;
    }

    /* Replay floor: the peer's advertised RSC, little-endian as mac80211 reads it.
     * The same key installed again on a live link keeps the counter it has. */
    uint64_t floor = 0;
    if (rsc != NULL)
    {
        for (size_t i = (rsc_len < 6u) ? rsc_len : 6u; i > 0; i--)
        {
            floor = (floor << 8) | rsc[i - 1];
        }
    }
    struct connection_keys_data *kd = &umac_sta_data_get_keys(stad)->keys;
    for (int i = 0; i < UMAC_KEY_RX_COUNTER_NUM; i++)
    {
        k.rx_seq[i] = floor;
    }
    if (key_id < UMAC_KEYS_NUM_KEY_IDS)
    {
        MMOSAL_TASK_ENTER_CRITICAL();
        const struct umac_key *old = kd->keys[key_id];
        if (old != NULL && old->key_type == UMAC_KEY_TYPE_GROUP && old->key_len == key_len &&
            memcmp(old->key_data, key, key_len) == 0)
        {
            for (int i = 0; i < UMAC_KEY_RX_COUNTER_NUM; i++)
            {
                if (old->rx_seq[i] > k.rx_seq[i])
                {
                    k.rx_seq[i] = old->rx_seq[i];
                }
            }
        }
        MMOSAL_TASK_EXIT_CRITICAL();
    }

    if (!connection_keys_install_key(kd, &k))
    {
        MMLOG_WRN("mesh: AMPE MGTK keychain install failed\n");
        return MMWLAN_ERROR;
    }
    /* A peer's RX MGTK stays host-only: every peer generates its own, the chip
     * has one VIF-wide slot, and that slot belongs to our TX key. */
    (void)vif_id;
    g_warthog_ampe_mgtk_installed++;
    MMLOG_INF("mesh: AMPE MGTK installed (key_id %u, rx floor 0x%08lx)\n", (unsigned)k.key_id,
              (unsigned long)(k.rx_seq[UMAC_KEY_RX_COUNTER_SPACE_DEFAULT] & 0xffffffffu));
    return MMWLAN_SUCCESS;
}

enum mmwlan_status umac_datapath_mesh_set_igtk(const uint8_t *addr, const uint8_t *key,
                                               uint8_t key_len, uint16_t key_id,
                                               const uint8_t *rsc, size_t rsc_len)
{
    if (addr == NULL || key_id < UMAC_MESH_IGTK_ID_MIN || key_id > UMAC_MESH_IGTK_ID_MAX ||
        (key != NULL && key_len != UMAC_KEY_AES_128_LEN))
    {
        return MMWLAN_INVALID_ARGUMENT;
    }
    if ((addr[0] & 0x01) != 0)
    {
        /* The same key delivered again keeps its IPN; a new one starts from 0. */
        MMOSAL_TASK_ENTER_CRITICAL();
        if (key == NULL)
        {
            s_own_igtk.valid = false;
        }
        else
        {
            if (!s_own_igtk.valid || s_own_igtk.id != key_id ||
                memcmp(s_own_igtk.key, key, UMAC_KEY_AES_128_LEN) != 0)
            {
                s_own_igtk.ipn = 0;
            }
            s_own_igtk.id = (uint8_t)key_id;
            memcpy(s_own_igtk.key, key, UMAC_KEY_AES_128_LEN);
            s_own_igtk.valid = true;
        }
        MMOSAL_TASK_EXIT_CRITICAL();
        return MMWLAN_SUCCESS;
    }
    struct umac_sta_data *stad = mesh_find_peer_(addr);
    if (key == NULL || stad == NULL)
    {
        return MMWLAN_ERROR;
    }
    struct umac_key k = { .key_id = (uint8_t)key_id, .key_type = UMAC_KEY_TYPE_IGTK,
                          .key_len = UMAC_KEY_AES_128_LEN };
    memcpy(k.key_data, key, UMAC_KEY_AES_128_LEN);
    uint64_t floor = 0;
    for (size_t i = (rsc != NULL) ? ((rsc_len < 6u) ? rsc_len : 6u) : 0u; i > 0; i--)
    {
        floor = (floor << 8) | rsc[i - 1];
    }
    struct connection_keys_data *kd = &umac_sta_data_get_keys(stad)->keys;
    MMOSAL_TASK_ENTER_CRITICAL();
    const struct umac_key *old = kd->keys[key_id];
    if (old != NULL && old->key_type == UMAC_KEY_TYPE_IGTK &&
        memcmp(old->key_data, key, UMAC_KEY_AES_128_LEN) == 0 &&
        old->rx_seq[UMAC_KEY_RX_COUNTER_SPACE_DEFAULT] > floor)
    {
        floor = old->rx_seq[UMAC_KEY_RX_COUNTER_SPACE_DEFAULT];
    }
    MMOSAL_TASK_EXIT_CRITICAL();
    for (int i = 0; i < UMAC_KEY_RX_COUNTER_NUM; i++)
    {
        k.rx_seq[i] = floor;
    }
    if (!connection_keys_install_key(kd, &k))
    {
        return MMWLAN_ERROR;
    }
    /* Authenticated by AMPE, and sent exactly when hostap marks all its peers MFP. */
    const int slot = mesh_slot_of_(addr);
    if (slot >= 0)
    {
        s_peer_mfp[slot] = true;
    }
    g_warthog_ampe_igtk_installed++;
    return MMWLAN_SUCCESS;
}

uint64_t umac_datapath_mesh_take_tx_pn(struct umac_sta_data *stad, uint8_t key_id)
{
    uint64_t pn = 0;
    if (stad == NULL || key_id >= UMAC_KEYS_NUM_KEY_IDS)
    {
        return 0;
    }
    struct connection_keys_data *kd = &umac_sta_data_get_keys(stad)->keys;
    MMOSAL_TASK_ENTER_CRITICAL();
    if (kd->keys[key_id] != NULL)
    {
        pn = kd->keys[key_id]->tx_seq++;
    }
    MMOSAL_TASK_EXIT_CRITICAL();
    return pn;
}

void umac_datapath_mesh_hwmp_tx_key(const uint8_t *da, struct umac_mesh_hwmp_txkey *out)
{
    memset(out, 0, sizeof(*out));
    if (da == NULL || !umac_mesh_sae_active())
    {
        return;
    }
    if ((da[0] & 0x01) != 0)
    {
        /* Group-addressed privacy, as mac80211 (tx.c ieee80211_select_link_key): our own
         * MGTK whenever we hold one, whatever each peer's MFP, and by the chip as our group
         * data. An MFP peer drops it in the clear, and drops it with an MMIE. Not while the
         * chip's group slot is empty: nobody could open what it sent under that key id. */
        const int kid = umac_datapath_mesh_own_group_key_id();
        if (kid >= 0 && s_group_key_in_chip)
        {
            out->how = UMAC_MESH_HWMP_PROT_GROUP;
            out->key_id = (uint8_t)kid;
        }
        return;
    }
    const int slot = mesh_slot_of_(da);
    if (!mesh_slot_mfp_(slot))
    {
        return;
    }
    struct umac_sta_data *stad = s_peers[slot];
    const int kid = umac_keys_get_active_key_id(stad, UMAC_KEY_TYPE_PAIRWISE);
    if (kid < 0)
    {
        return;
    }
    out->key_id = (uint8_t)kid;
#ifdef WARTHOG_MESH_HOST_CCMP
    /* The data path's choice: host CCMP when armed and the key is here, else the chip. */
    extern volatile uint32_t g_warthog_host_ccmp_on;
    const uint8_t *key = umac_keys_get_key_data(stad, (uint8_t)kid);
    if (g_warthog_host_ccmp_on && key != NULL &&
        umac_keys_get_key_len(stad, (uint8_t)kid) == UMAC_KEY_AES_128_LEN)
    {
        memcpy(out->key, key, UMAC_KEY_AES_128_LEN);
        out->pn = umac_datapath_mesh_take_tx_pn(stad, (uint8_t)kid);
        out->how = UMAC_MESH_HWMP_PROT_HOST;
        return;
    }
#endif
    umac_keys_increment_tx_seq(stad, (uint8_t)kid);
    out->how = UMAC_MESH_HWMP_PROT_CHIP;
}

extern volatile uint32_t g_warthog_swccmp_tx_fail;
/* AT+MESHFWDSTAT? mgmt tx: protected by the chip, sealed by host CCMP, dropped unsealed. */
extern volatile uint32_t g_warthog_mgmt_tx_chip, g_warthog_mgmt_tx_host, g_warthog_mgmt_tx_drop;
bool umac_mesh_tx_host_ccmp_mgmt(const uint8_t key[16], uint8_t key_id, uint64_t pn,
                                 uint8_t *frame, uint32_t len);

/* umac_ba.c's Block Ack frames: mac80211 drops a robust unicast one from an MFP peer
 * unprotected (rx.c ieee80211_drop_unencrypted_mgmt), as it does path selection. */
struct mmpkt *umac_datapath_mesh_protect_mgmt(struct mmpkt *txbuf, int *key_id)
{
    const uint32_t hdr = sizeof(struct dot11_hdr);
    struct umac_mesh_hwmp_txkey k = { .how = UMAC_MESH_HWMP_PROT_NONE };
    *key_id = -1;
    struct mmpktview *v = mmpkt_open(txbuf);
    uint8_t *f = mmpkt_get_data_start(v);
    const uint32_t len = mmpkt_get_data_length(v);
    if (len > hdr && !mm_mac_addr_is_multicast(dot11_get_da((struct dot11_hdr *)f)) &&
        frame_is_robust_mgmt(v))
    {
        umac_datapath_mesh_hwmp_tx_key(dot11_get_da((struct dot11_hdr *)f), &k);
    }
    if (k.how == UMAC_MESH_HWMP_PROT_CHIP)
    {
        f[1] |= 0x40u; /* the chip adds CCMP, as for a PMF station's robust frame */
        *key_id = k.key_id;
        g_warthog_mgmt_tx_chip++;
    }
    if (k.how != UMAC_MESH_HWMP_PROT_HOST)
    {
        mmpkt_close(&v);
        return txbuf;
    }
    /* build_mgmt_frame leaves no room for the CCMP header and MIC: copy into a frame that has it. */
    const uint32_t n = len + UMAC_CCMP_HDR_LEN + UMAC_CCMP_MIC_LEN;
    struct mmpkt *out = umac_datapath_alloc_raw_tx_mmpkt(MMDRV_PKT_CLASS_MGMT, 0, n);
    bool ok = false;
    if (out != NULL)
    {
        struct mmpktview *ov = mmpkt_open(out);
        uint8_t *o = mmpkt_append(ov, n);
        memcpy(o, f, hdr);
        memset(o + hdr, 0, UMAC_CCMP_HDR_LEN);
        memcpy(o + hdr + UMAC_CCMP_HDR_LEN, f + hdr, len - hdr);
        memset(o + n - UMAC_CCMP_MIC_LEN, 0, UMAC_CCMP_MIC_LEN);
        ok = umac_mesh_tx_host_ccmp_mgmt(k.key, k.key_id, k.pn, o, n);
        mmpkt_close(&ov);
        *mmdrv_get_tx_metadata(out) = *mmdrv_get_tx_metadata(txbuf);
    }
    else
    {
        g_warthog_swccmp_tx_fail++;
    }
    mmpkt_close(&v);
    mmpkt_release(txbuf);
    if (!ok && out != NULL)
    {
        mmpkt_release(out);
    }
    if (ok)
    {
        g_warthog_mgmt_tx_host++;
    }
    else
    {
        g_warthog_mgmt_tx_drop++;
    }
    return ok ? out : NULL;
}

/* mac80211 parity (rx.c ieee80211_rx_h_decrypt, ieee80211_drop_unencrypted_mgmt): only from
 * an ESTAB peer (mesh_rx_path_sel_frame); unicast from one that runs MFP must be protected.
 * Group path selection is group-addressed privacy: Protected, opened under the sender's MGTK
 * and replay-checked on the way here. Stricter than mac80211, never in the clear or with an
 * MMIE: every ESTAB peer's AMPE delivered its MGTK and it protects with it, and a relay
 * re-sends what it takes under our MGTK, which every MFP peer opens. */
bool umac_datapath_mesh_hwmp_rx_ok(const uint8_t *frame, uint32_t len)
{
    if (frame == NULL || len < UMAC_MESH_BIP_HDR_LEN || !umac_mesh_sae_active())
    {
        return true;
    }
    const bool group = (frame[4] & 0x01u) != 0u;
    const bool prot = (frame[1] & 0x40u) != 0u;
    const int slot = mesh_slot_of_(frame + 10);
    if (slot < 0 || !s_peer_estab[slot])
    {
        g_warthog_hwmp_unestab++;
        return false;
    }
    if (!group)
    {
        if (!prot)
        {
            if (mesh_slot_mfp_(slot))
            {
                g_warthog_hwmp_unprotected++;
                return false;
            }
            return true;
        }
        /* Decrypted and replay-checked on the way here: this peer runs MFP. */
        g_warthog_hwmp_prot++;
        if (slot >= 0)
        {
            s_peer_mfp[slot] = true;
        }
        return true;
    }
    if (prot)
    {
        /* No MFP latch: mac80211 protects every group Mesh Action frame when it has an MGTK. */
        g_warthog_hwmp_gp++;
        return true;
    }
    if (umac_mesh_bip_has_mmie(frame + UMAC_MESH_BIP_HDR_LEN, len - UMAC_MESH_BIP_HDR_LEN))
    {
        g_warthog_hwmp_mmie++;
        return false;
    }
    g_warthog_hwmp_nommie++;
    g_warthog_hwmp_unprotected++;
    return false;
}

struct umac_sta_data *umac_datapath_mesh_find_peer(const uint8_t *addr)
{
    return addr != NULL ? mesh_find_peer_(addr) : NULL;
}

bool umac_datapath_mesh_peer_estab(const uint8_t *addr)
{
    const int slot = mesh_slot_of_(addr);
    return slot >= 0 && s_peer_estab[slot];
}

/* Any established peer other than @p excl, for the original copy of a relayed
 * group frame; NULL when the sender is our only neighbour. Under SAE, keyed only. */
struct umac_sta_data *umac_datapath_mesh_first_peer_except(const uint8_t *excl)
{
    for (int i = 0; i < MESH_MAX_PEERS; i++)
    {
        struct umac_sta_data *p = s_peers[i];
        if (p != NULL && s_peer_estab[i] &&
            (excl == NULL || !umac_sta_data_matches_peer_addr(p, excl)))
        {
            return p;
        }
    }
    return NULL;
}

void umac_datapath_mesh_del_peer(const uint8_t *peer_addr)
{
    for (int i = 0; i < MESH_MAX_PEERS; i++)
    {
        if (s_peers[i] != NULL &&
            (peer_addr == NULL || umac_sta_data_matches_peer_addr(s_peers[i], peer_addr)))
        {
            struct umac_sta_data *stad = s_peers[i];
            /* Paths through this neighbour die with it; the relay announces them. */
            {
                extern volatile uint32_t g_warthog_mesh_fwd, g_warthog_mesh_bridge;
                extern void umac_mesh_fwd_glue_peer_lost(const uint8_t *peer);
                if (g_warthog_mesh_fwd || g_warthog_mesh_bridge)
                {
                    umac_mesh_fwd_glue_peer_lost(umac_sta_data_peek_peer_addr(stad));
                }
            }
            /* Unlink and take the queue in one critical section: a TX on another task that
             * looked this stad up re-checks the table under it before queueing
             * (mesh_queue_one_), so nothing lands in the queue once it is taken. */
            struct mmpkt *pkt;
            struct mmpkt_list gone = MMPKT_LIST_INIT;
            MMOSAL_TASK_ENTER_CRITICAL();
            s_peers[i] = NULL;
            s_peer_mfp[i] = false;
            while ((pkt = umac_sta_data_pop_pkt(stad)) != NULL)
            {
                s_num_pkts_queued--;
                mmpkt_list_append(&gone, pkt);
            }
            MMOSAL_TASK_EXIT_CRITICAL();
            mmpkt_list_clear(&gone);
            /* Tell the chip the station is gone. A MESH chip VIF is walked down one state at
             * a time to NONE, as mac80211 does (morse_driver skips NONE -> NOTEXIST). */
            static const enum morse_sta_state mesh_down[] = {
                MORSE_STA_ASSOCIATED, MORSE_STA_AUTHENTICATED, MORSE_STA_NONE
            };
            static const enum morse_sta_state sta_down[] = { MORSE_STA_NOTEXIST };
            const bool mesh_vif = umac_interface_chip_vif_is_mesh(umac_sta_data_get_umacd(stad));
            const enum morse_sta_state *down = mesh_vif ? mesh_down : sta_down;
            const size_t n_down = mesh_vif ? sizeof(mesh_down) / sizeof(mesh_down[0]) : 1u;
            for (size_t k = 0; k < n_down; k++)
            {
                (void)mesh_chip_sta_state_(umac_sta_data_get_vif_id(stad), umac_sta_data_get_aid(stad),
                                           umac_sta_data_peek_peer_addr(stad), down[k]);
            }
            umac_rc_stop(stad);
            /* Loop timeouts point into the record (ADDBA retry, RX reorder, defrag): stop them. */
            umac_ba_deinit(stad);
            umac_datapath_stad_teardown(umac_sta_data_get_umacd(stad), stad);
            uint16_t vif_id = umac_sta_data_get_vif_id(stad);
            mesh_retire_(i, stad); /* a reader on another task may still hold it */

            /* The key slot is VIF-wide but it lives against the station it was
             * installed for, so removing that station takes the key with it
             * and every SURVIVING link goes silent. Measured: expire one dead
             * peer and the other two boards stop passing traffic entirely,
             * even though their own link is healthy. Re-install against a peer
             * that is still here; the epoch bump keeps the TX PN moving
             * forward so nobody sees a replay. */
            /* ...and removing a station also disturbs the ones that remain:
             * with no restore, the surviving peers stopped receiving entirely
             * -- sender enq=4/drv_ok=4, receiver rx_data=0 with rxdrop=0, i.e.
             * discarded by the chip before it ever reached the host. Put every
             * survivor back: station state first, then its key if the link is keyed.
             * Measured on the STA chip VIF. A MESH VIF keeps its stations, as Linux relies on,
             * but its key slots are unmeasured, so the keys still go back. */
            for (int j = 0; j < MESH_MAX_PEERS; j++)
            {
                if (s_peers[j] == NULL)
                {
                    continue;
                }
                if (!mesh_vif)
                {
                    uint8_t survivor[MMWLAN_MAC_ADDR_LEN];
                    umac_sta_data_get_peer_addr(s_peers[j], survivor);
                    mesh_chip_register_sta_(vif_id, umac_sta_data_get_aid(s_peers[j]), survivor);
                }
                (void)mesh_restore_peer_key_(s_peers[j], vif_id);
            }
        }
    }
}

uint8_t umac_datapath_mesh_peer_links(struct mmwlan_mesh_peer_link *out, uint8_t max)
{
    uint8_t n = 0;
    for (int i = 0; out != NULL && i < MESH_MAX_PEERS && n < max; i++)
    {
        if (s_peers[i] == NULL)
        {
            continue;
        }
        struct mmwlan_mesh_peer_link *l = &out[n++];
        memset(l, 0, sizeof(*l));
        umac_sta_data_get_peer_addr(s_peers[i], l->addr);
        l->estab = s_peer_estab[i] ? 1u : 0u;
        l->expected_tput_kbps = umac_rc_get_expected_tput_kbps(s_peers[i]);
        l->rc_valid = l->expected_tput_kbps != 0 ? 1u : 0u;
    }
    return n;
}

uint8_t umac_datapath_mesh_peer_count(void)
{
    uint8_t n = 0;
    for (int i = 0; i < MESH_MAX_PEERS; i++)
    {
        n += (s_peers[i] != NULL);
    }
    return n;
}

static struct umac_sta_data *mesh_lookup_stad_by_peer_addr(struct umac_data *umacd,
                                                           const uint8_t *peer_addr)
{
    (void)umacd;
    return mesh_find_peer_(peer_addr);
}

/* TX destination -> next hop.
 *
 * Unicast to a known peer: that peer. With AT+MESHFWD or AT+MESHBRIDGE on, any
 * other unicast takes its HWMP path's next hop, or is dropped until one exists.
 * In leaf mode a learned host (Address Extension) goes to its node if that is
 * a peer, else to the peer it was learned through. Group frames, and a leaf's
 * other unicast, go to the FIRST established peer;
 * mesh_enqueue_tx_frame decides how a group frame reaches the rest. */
static struct umac_sta_data *mesh_lookup_stad_by_tx_dest_addr(struct umac_data *umacd,
                                                              const uint8_t *dest_addr)
{
    (void)umacd;
    struct umac_sta_data *stad = mesh_find_peer_(dest_addr);
    if (stad != NULL)
    {
        return stad;
    }
    /* Relay or bridge on: a destination that is not a neighbour goes to the
     * path's next hop. No path yet: a PREQ goes out and the caller holds the
     * frame for the PREP (umac_mesh_fwd_glue_tx_pending). */
    {
        extern volatile uint32_t g_warthog_mesh_fwd, g_warthog_mesh_bridge;
        extern bool umac_mesh_fwd_glue_next_hop(const uint8_t *dest, uint8_t out[6]);
        extern bool umac_mesh_fwd_glue_proxy_via_peer(const uint8_t *da, uint8_t out[6]);
        if (g_warthog_mesh_fwd || g_warthog_mesh_bridge)
        {
            uint8_t nh[6];
            if (umac_mesh_fwd_glue_next_hop(dest_addr, nh))
            {
                return mesh_find_peer_(nh);
            }
            if (!mm_mac_addr_is_multicast(dest_addr))
            {
                return NULL;
            }
        }
        else
        {
            /* Leaf mode: a learned host goes to its node if that is a peer, else
             * to the peer its traffic arrived through; failing both, the first. */
            uint8_t via[6];
            if (umac_mesh_fwd_glue_proxy_via_peer(dest_addr, via))
            {
                stad = mesh_find_peer_(via);
                if (stad != NULL)
                {
                    return stad;
                }
            }
        }
    }
    /* The first established peer: under SAE one AMPE has keyed, since a frame
     * queued to a candidate could only be dropped at TX. */
    for (int i = 0; i < MESH_MAX_PEERS; i++)
    {
        struct umac_sta_data *p = s_peers[i];
        if (p != NULL && s_peer_estab[i])
        {
            return p;
        }
    }
    return NULL;
}

static struct umac_sta_data *mesh_lookup_stad_by_aid(struct umac_data *umacd, uint16_t aid)
{
    (void)umacd;
    /* AIDs are slot+1 (see add_peer). Resolving here is what routes the chip's
     * TX-status reports back to the peer for rate-control feedback. */
    if (aid == 0 || aid > MESH_MAX_PEERS)
    {
        return NULL;
    }
    return s_peers[aid - 1];
}

/* Mesh peers do not power-save through us; treat every peer as awake. */
static bool mesh_set_stad_sleep_state(struct umac_sta_data *stad, bool asleep)
{
    (void)stad; (void)asleep;
    return true;
}

static bool mesh_is_stad_tx_paused(struct umac_sta_data *stad)
{
    return umac_sta_data_is_paused(stad);
}

static enum mmwlan_sta_state mesh_get_sta_state(struct umac_sta_data *stad)
{
    /* Anything in the table is an established peer. The datapath treats
     * CONNECTED as "ok to send data". */
    return (stad != NULL) ? MMWLAN_STA_CONNECTED : MMWLAN_STA_DISABLED;
}

/* --- TX queue: per-peer, mirroring umac_ap_queue_pkt/tx_dequeue_frame ---- */

/* @p stad was looked up off the event loop (the netif or batman engine task), so del_peer
 * may have unlinked it since: queue only while it is still in the table, checked by pointer
 * under the critical section del_peer unlinks in. Before read_end it is not freed, so no
 * new record can have its address. */
static void mesh_queue_one_(struct umac_data *umacd, struct umac_sta_data *stad,
                            struct mmpkt *txbuf)
{
    bool live = false;
    MMOSAL_TASK_ENTER_CRITICAL();
    for (int i = 0; i < MESH_MAX_PEERS && !live; i++)
    {
        live = (s_peers[i] == stad);
    }
    if (live)
    {
        umac_sta_data_queue_pkt(stad, txbuf);
        umac_stats_update_datapath_txq_high_water_mark(umacd, ++s_num_pkts_queued);
    }
    MMOSAL_TASK_EXIT_CRITICAL();
    if (!live)
    {
        mmpkt_release(txbuf); /* as del_peer's drain would have */
        return;
    }
    g_warthog_tx_data_enq++;
}

/* By default (AT+MESHGRP=0) group-addressed traffic is REPLICATED as unicast,
 * one copy per peer.
 *
 * The obvious implementation -- one real 802.11 group frame, encrypted with a
 * group key -- does not survive this chip once there is more than one peer.
 * Measured across three boards: 100% of group frames were dropped by the
 * receiver as "no HW decryption" (rxdrop reason=4, key id 1) while unicast
 * decryption never failed once (uni=0 on every board). Installing the MGTK on
 * each peer's AID, and installing it once VIF-wide at AID 0, both failed the
 * same way; with a single peer it works, so the chip cannot key group RX
 * across several mesh peers. Since ARP is group-addressed, that alone made
 * every unicast flow fail too -- nothing could resolve.
 *
 * Replication costs one transmission per peer (<= MESH_MAX_PEERS) and uses the
 * pairwise key, which is the path the hardware handles reliably. It also makes
 * broadcast ACKed rather than best-effort. The receiver sees a frame addressed
 * to itself carrying an unchanged IP payload, so a multicast/broadcast IP
 * datagram is still accepted: lwIP dispatches on the IP destination and its
 * IGMP membership, not on the Ethernet destination, and a unicast ARP request
 * is answered exactly like a broadcast one.
 *
 * This is forced, not chosen, and it is the same limit as the pairwise one:
 * the chip holds a single key per key-index for this VIF, so it cannot hold
 * one peer's group key alongside another's any more than it can hold two
 * pairwise keys (see umac_datapath_mesh_install_peer_keys). Replication costs
 * one transmission per peer -- N x airtime on a 2 MHz channel, and NOT the
 * shape a standard 802.11s peer emits or expects.
 *
 * Both halves are unblocked by the same thing: host software CCMP, which keys
 * off the transmitter address in software and so is not bound by the chip's
 * single slot.
 *
 * Host CCMP now EXISTS (umac_mesh_rx_host_ccmp, and the RX gate has no
 * multicast exclusion -- it resolves the stad from the TA, so group frames are
 * in scope). It is not a reason to change this path yet, for two reasons that
 * have to be cleared in order:
 *
 *   1. It compiles only in warthog-mesh-sae-swccmp{,-on}. The region images
 *      and warthog-mesh-sae do not have it.
 *   2. `swccmp ok` has never been observed above zero on air, for unicast or
 *      group. Until it has, host CCMP is a compiled hypothesis.
 *
 * The experiment that decides this, in order: bring up two swccmp-on boards,
 * confirm AT+SWCCMP? shows ok > 0 on ordinary unicast, and only then try a
 * real group frame. Emitting standard group frames before step one trades a
 * path measured to work for one that is merely argued to.
 *
 * AT+MESHGRP=1 sends the standard shape instead: one 3-address group frame
 * (umac_mesh_ies_build_data_hdr3_group, via umac_mesh_fwd_tx_header) with a
 * Mesh Control field, under the group key when keyed (our own MGTK under SAE). */
static void mesh_enqueue_tx_frame(struct umac_data *umacd,
                                  struct umac_sta_data *stad,
                                  struct mmpkt *txbuf)
{
    if (stad == NULL)
    {
        mmpkt_release(txbuf);
        return;
    }

    bool group = false;
    {
        struct mmpktview *v = mmpkt_open(txbuf);
        if (v != NULL)
        {
            if (mmpkt_get_data_length(v) >= sizeof(struct umac_8023_hdr))
            {
                const struct umac_8023_hdr *h =
                    (const struct umac_8023_hdr *)mmpkt_get_data_start(v);
                group = mm_mac_addr_is_multicast(h->dest_addr);
            }
            mmpkt_close(&v);
        }
    }

    extern volatile uint32_t g_warthog_mesh_grp;
    if (group && g_warthog_mesh_grp)
    {
        /* Standard group frame: one transmission, the chip broadcasts it.
         * The handed-over stad only supplies rate control and a queue. */
        mesh_queue_one_(umacd, stad, txbuf);
        return;
    }
    if (group)
    {
        /* Copy to every peer except the one we were handed, which takes the
         * original. A failed copy drops that peer's replica only. A relayed
         * group frame also skips the neighbour it came from.
         * Under SAE an unkeyed candidate gets none: TX would drop it, and a failed SAE frees
         * such a slot. A peer del_peer unlinks during this walk gets nothing queued
         * (mesh_queue_one_). */
        const struct mmdrv_tx_metadata *md0 = mmdrv_get_tx_metadata(txbuf);
        for (int i = 0; i < MESH_MAX_PEERS; i++)
        {
            struct umac_sta_data *p = s_peers[i];
            if (p == NULL || p == stad || !s_peer_estab[i])
            {
                continue;
            }
            if (md0->mesh.exclude_valid &&
                umac_sta_data_matches_peer_addr(p, md0->mesh.exclude_ta))
            {
                continue;
            }
            struct mmpkt *dup = umac_datapath_copy_tx_mmpkt(txbuf, MMDRV_PKT_CLASS_DATA_TID0);
            if (dup == NULL)
            {
                g_warthog_tx_bcast_copy_fail++;
                continue;
            }
            g_warthog_tx_bcast_dup++;
            mesh_queue_one_(umacd, p, dup);
        }
    }

    mesh_queue_one_(umacd, stad, txbuf);
}

static bool mesh_dequeue_tx_frame(struct umac_data *umacd,
                                  struct umac_sta_data **stad_ptr,
                                  struct mmpkt **txbuf_ptr)
{
    (void)umacd;
    *stad_ptr = NULL;
    *txbuf_ptr = NULL;

    /* Round-robin across peers so one busy link cannot starve another. */
    static int rr = 0;
    bool has_more = false;
    MMOSAL_TASK_ENTER_CRITICAL();
    for (int n = 0; n < MESH_MAX_PEERS; n++)
    {
        int i = (rr + n) % MESH_MAX_PEERS;
        struct umac_sta_data *stad = s_peers[i];
        if (stad == NULL || umac_sta_data_is_paused(stad))
        {
            continue;
        }
        struct mmpkt *txbuf = umac_sta_data_pop_pkt(stad);
        if (txbuf != NULL)
        {
            has_more = (--s_num_pkts_queued) != 0;
            *stad_ptr = stad;
            *txbuf_ptr = txbuf;
            g_warthog_tx_data_deq++;
            rr = (i + 1) % MESH_MAX_PEERS;
            break;
        }
    }
    MMOSAL_TASK_EXIT_CRITICAL();
    return has_more;
}

/* --- 4-address mesh data header ------------------------------------------ *
 *
 * 802.11s mesh data frames set both ToDS and FromDS and carry four addresses
 * (IEEE 802.11-2020 s9.3.2.1, table 9-30):
 *   addr1 = RA   next-hop receiver (the peer)
 *   addr2 = TA   transmitter (us)
 *   addr3 = DA   final destination (the 802.3 dest)
 *   addr4 = SA   original source (the 802.3 src)
 * For a single hop RA==DA and TA==SA, but the fields are still all four --
 * that is what lets a receiver's dot11_get_da()/dot11_get_sa_data() recover
 * the 802.3 header, and what a forwarding node needs to relay without
 * losing the endpoints. The vendor RX path is already 4-address aware
 * (dot11_is_4addr_hdr / dot11_get_sa_data read addr4), so nothing changes on
 * receive.
 *
 * The Mesh Control field (s9.2.4.7.3) is NOT built here: the generic TX
 * path in umac_datapath.c prepends one to every mesh-mode frame -- 6 bytes
 * (flags 0, TTL, seq), or a relayed or proxied frame's own, with Address
 * Extension -- and the RX side strips it. This builder is the MAC
 * header only.
 */
static const struct mmdrv_tx_metadata *s_cur_tx_md;
void umac_datapath_mesh_set_cur_tx_md(const struct mmdrv_tx_metadata *md) { s_cur_tx_md = md; }
const struct mmdrv_tx_metadata *umac_datapath_mesh_cur_tx_md(void) { return s_cur_tx_md; }

static void mesh_construct_80211_data_header(struct umac_sta_data *stad,
                                             const struct umac_8023_hdr *hdr_8023,
                                             struct dot11_data_hdr *data_hdr)
{
    /* Byte layout lives in the freestanding umac_mesh_ies.c so the host tests
     * pin it against a golden header. Build into a scratch buffer and copy the
     * fields into the SDK struct -- the struct is packed identically, but
     * copying keeps this correct even if that ever changes. */
    uint8_t ta[6];
    umac_sta_data_get_bssid(stad, ta); /* us (mesh BSSID == own addr) */
    g_warthog_tx_data_hdr++;

    uint8_t ra[6], hdr[UMAC_MESH_DATA_HDR4_LEN];
    umac_sta_data_get_peer_addr(stad, ra); /* next hop = the peer */

    /* With AT+MESHGRP=0 a group-addressed frame reaches here once per peer (see
     * mesh_enqueue_tx_frame). Address each replica TO that peer -- RA and DA
     * both the peer -- so it is an ordinary unicast on air and is protected
     * with the pairwise key, the only crypto path this chip handles across
     * several peers. The IP payload is untouched, so a multicast datagram is
     * still delivered by the receiver's IP layer. */
    /* The shaping decision lives in the freestanding engine so the host
     * simulator emits the same bytes; this only copies them into the SDK
     * struct. A relayed or proxied frame's mesh endpoints ride in the sidecar. */
    extern volatile uint32_t g_warthog_mesh_grp;
    const struct mmdrv_tx_metadata *md = umac_datapath_mesh_cur_tx_md();
    struct umac_mesh_tx_hdr_in in = {
        .ra = ra, .own = ta, .dst8023 = hdr_8023->dest_addr, .src8023 = hdr_8023->src_addr,
        .sidecar_valid = (md != NULL && md->mesh.addr_valid),
        .mesh_da = md != NULL ? md->mesh.mesh_da : NULL,
        .mesh_sa = md != NULL ? md->mesh.mesh_sa : NULL,
        .grp_std = g_warthog_mesh_grp != 0,
    };
    uint16_t n = umac_mesh_fwd_tx_header(&in, hdr);
    memcpy(&data_hdr->base.frame_control, &hdr[0], 2);
    mac_addr_copy(data_hdr->base.addr1, &hdr[4]);
    mac_addr_copy(data_hdr->base.addr2, &hdr[10]);
    mac_addr_copy(data_hdr->base.addr3, &hdr[16]);
    if (n == UMAC_MESH_DATA_HDR4_LEN)
    {
        mac_addr_copy(data_hdr->addr4, &hdr[24]);
    }
    else
    {
        memset(data_hdr->addr4, 0, 6);
    }
}

/* The freestanding builder hard-codes the FC bit positions; pin them to the
 * SDK's so a header change there breaks the build here, not on air. */
MM_STATIC_ASSERT(DOT11_MASK_FC_TO_DS == 0x0100, "ToDS bit drift");
MM_STATIC_ASSERT(DOT11_MASK_FC_FROM_DS == 0x0200, "FromDS bit drift");
MM_STATIC_ASSERT(DOT11_SHIFT_FC_TYPE == 2 && DOT11_SHIFT_FC_SUBTYPE == 4, "FC shift drift");
MM_STATIC_ASSERT(sizeof(struct dot11_data_hdr) == UMAC_MESH_DATA_HDR4_LEN, "4-addr hdr size drift");
MM_STATIC_ASSERT(UMAC_MESH_DATA_HDR3_LEN == 24, "3-addr hdr size drift");

/* --- Frames allowed before "association" -------------------------------- */

/* In mesh mode there is no association in the AP/STA sense — peers come and
 * go through PLINK. The filter at umac_datapath_rx_frame_allowed_pre_association
 * always treats us as "pre-association" because no STA-mode connection
 * happens. Let through what mesh actually uses on the wire:
 *
 *   • S1G_BEACON (EXT type) — peer discovery (skipped by line 1204 check
 *     in umac_datapath.c too, but include for completeness)
 *   • PROBE_REQ / PROBE_RESP — active peer discovery
 *   • ACTION — PLINK Open/Confirm/Close + (later) mesh routing frames
 *   • AUTH — SAE; harmless for open mesh, needed for SAE mesh
 */
const uint16_t frames_allowed_pre_association_mesh_mode[] = {
    DOT11_VER_TYPE_SUBTYPE(0, MGMT, PROBE_REQ),
    DOT11_VER_TYPE_SUBTYPE(0, MGMT, PROBE_RSP),
    DOT11_VER_TYPE_SUBTYPE(0, MGMT, AUTH),
    DOT11_VER_TYPE_SUBTYPE(0, MGMT, ACTION),
    DOT11_VER_TYPE_SUBTYPE(0, EXT,  S1G_BEACON),
    /* LEGACY BEACON (type 0, subtype 8) is REQUIRED, not redundant.
     *
     * The MM6108 firmware converts S1G beacons to legacy MGMT/BEACON on the
     * way up to the host -- it does NOT hand us DOT11_FC_TYPE_EXT/S1G_BEACON.
     * Allowing only the EXT form means a peer's beacon can never pass this
     * gate, so beacon-driven peer discovery is dead on arrival even when the
     * chip does deliver the frame. (This is also why earlier hunts that
     * filtered for S1G_BEACON found nothing: the frame exists, wearing the
     * legacy hat.) Note peer beacons arrive with A2/A3 zeroed by the S1G
     * address-compression conversion, so identify peers from Action/data
     * frames, not from the beacon's addresses. */
    DOT11_VER_TYPE_SUBTYPE(0, MGMT, BEACON),
    UINT16_MAX,
};

/* --- Ops table ---------------------------------------------------------- */

const struct umac_datapath_ops datapath_ops_mesh = {
    .process_rx_mgmt_frame       = process_rx_mgmt_frame_mesh,
    .lookup_stad_by_peer_addr    = mesh_lookup_stad_by_peer_addr,
    .lookup_stad_by_tx_dest_addr = mesh_lookup_stad_by_tx_dest_addr,
    .lookup_stad_by_aid          = mesh_lookup_stad_by_aid,
    .set_stad_sleep_state        = mesh_set_stad_sleep_state,
    .is_stad_tx_paused           = mesh_is_stad_tx_paused,
    .enqueue_tx_frame            = mesh_enqueue_tx_frame,
    .dequeue_tx_frame            = mesh_dequeue_tx_frame,
    .construct_80211_data_header = mesh_construct_80211_data_header,
    .get_sta_state               = mesh_get_sta_state,
    .frames_allowed_pre_association = frames_allowed_pre_association_mesh_mode,
};

void umac_datapath_configure_mesh_mode(struct umac_data *umacd)
{
    struct umac_datapath_data *data = umac_data_get_datapath(umacd);
    data->ops = &datapath_ops_mesh;
    umac_mesh_ies_capacity_fn = mesh_capacity_;
    MMLOG_INF("Datapath configured for mesh mode\n");
}

/* AT+REKEY=<n>: re-push peer slot n's own pairwise key (mesh_restore_peer_key_)
 * and publish its AID, or 0xffffffff when no key went in. The one-slot probe:
 * whichever link the chip decrypts afterwards follows the last install. */
void umac_datapath_mesh_service_rekey(void)
{
    /* Run on the umac event loop, posted from the mesh-probe tick, rather than called
     * from main: morselib is a static archive and the linker will not extract an object
     * to satisfy a call from main -- the same reason every warthog counter is defined in
     * main/at.c and extern'd here. AT+REKEY=<n> sets the request; this runs within one
     * probe interval (~2 s). */
    if (g_warthog_mesh_repeer_req)
    {
        /* Tear every link down, host and chip, and re-arm the peering. Used to
         * A/B the data-plane protection setting without a reflash. */
        g_warthog_mesh_repeer_req = 0;
        umac_datapath_mesh_del_peer(NULL);
        umac_mesh_reset_links();
    }

    uint32_t req = g_warthog_rekey_req;
    if (req == 0)
    {
        return;
    }
    g_warthog_rekey_req = 0;
    int idx = (int)req - 1;
    if (idx < 0 || idx >= MESH_MAX_PEERS || s_peers[idx] == NULL)
    {
        g_warthog_rekey_aid = 0xffffffffu;
        return;
    }
    uint16_t vif_id = umac_sta_data_get_vif_id(s_peers[idx]);
    if (!mesh_restore_peer_key_(s_peers[idx], vif_id))
    {
        g_warthog_rekey_aid = 0xffffffffu; /* unkeyed, open, or a no-chip-key build */
        return;
    }
    g_warthog_rekey_aid = umac_sta_data_get_aid(s_peers[idx]);
    g_warthog_rekey_done++;
}

/* Act on an S1G Beacon: answer a peer advertising our Mesh ID.
 *
 * Called from the extension-frame path in umac_datapath.c, which is where S1G
 * beacons actually arrive -- they are type 3, not management, so the mesh
 * management dispatch never sees them. Offsets are pinned by
 * test_s1g_beacon.c against a frame captured from a real OpenMANET node.
 */
void umac_mesh_handle_s1g_beacon(struct mmpktview *rxbufview)
{
    const uint8_t *f = (const uint8_t *)mmpkt_get_data_start(rxbufview);
    uint32_t flen = mmpkt_get_data_length(rxbufview);
    uint8_t sa[MMWLAN_MAC_ADDR_LEN];

    if (!umac_mesh_ies_is_s1g_beacon(f, flen))
    {
        return;
    }
    g_warthog_s1g_bcn_rx++;
    {
        extern volatile uint8_t g_warthog_bcn_rx_frame[160];
        extern volatile uint16_t g_warthog_bcn_rx_len;
        uint32_t l = flen > 160 ? 160 : flen;
        memcpy((void *)g_warthog_bcn_rx_frame, f, l);
        g_warthog_bcn_rx_len = (uint16_t)flen;
    }
    if (!umac_mesh_ies_s1g_beacon_sa(f, flen, sa) || !umac_mesh_s1g_beacon_is_our_mesh(f, flen))
    {
        return;
    }
    g_warthog_s1g_bcn_ours++;
    memcpy((void *)g_warthog_s1g_bcn_sa, sa, MMWLAN_MAC_ADDR_LEN);
    const int16_t rssi = mmdrv_get_rx_metadata(mmpkt_from_view(rxbufview))->rssi;

    /* Announce ourselves once per peer link. Answering every beacon restarts
     * the peering each time the peer frees its station -- see
     * umac_mesh_note_s1g_beacon(). */
    if (umac_mesh_note_s1g_beacon(sa))
    {
        const bool sae = umac_mesh_sae_active();
        const uint32_t boff = umac_mesh_ies_s1g_beacon_ie_offset(
            (uint16_t)(f[0] | ((uint16_t)f[1] << 8)));
        /* Open only toward a neighbour whose Mesh Configuration would accept
         * us, as mac80211 does (mesh_plink.c:635-640), heard above the floor. */
        const bool accepts =
            sae || flen <= boff || umac_mesh_ies_peer_openable(f + boff, flen - boff, false);
        const bool openable = accepts && !umac_mesh_rssi_below_floor(rssi);
        /* One we will not open toward gets no link, so each of its beacons is
         * a first sighting: answer it at most once per re-announce period. */
        if (!openable && !umac_mesh_unopenable_answer_due(sa))
        {
            return;
        }
        /* First sight: announce ourselves so the peer learns we exist. */
        g_warthog_s1g_bcn_new++;
        (void)umac_mesh_tx_probe_response(sa);
        if (sae)
        {
            /* Under SAE, beacons are the discovery path for mac80211-style
             * peers (OpenMANET): they beacon and never probe, so the
             * probe-response path cannot see them. The S1G beacon's SA reads
             * true from the chip (verified against a live node), and the
             * candidate still passes umac_supp_mesh_new_peer's auth-protocol
             * gate, so an open node sharing the Mesh ID is not SAE'd at.
             * Warthog peers are discovered by the probe-response path; both
             * feed the same gate. */
            if (flen > boff)
            {
                umac_mesh_offer_sae_candidate(sa, f + boff, (size_t)(flen - boff), rssi);
            }
        }
        else if (accepts)
        {
            umac_mesh_maybe_initiate_mpm(sa, rssi, true); /* refuses, and counts, a weak one */
        }
    }
    else if (umac_mesh_reannounce_due(sa))
    {
        /* Full: our beacon says "not accepting", so tell this established
         * peer directly that we accept IT, in case it lost its half. */
        (void)umac_mesh_tx_probe_response(sa);
    }
    else if (!umac_mesh_sae_active() && umac_mesh_peering_incomplete(sa))
    {
        /* Known peer, handshake unfinished: retransmit the Open only. NOT the
         * probe response -- that is what restarts the peer's FSM. */
        g_warthog_s1g_bcn_retry++;
        umac_mesh_maybe_initiate_mpm(sa, rssi, true);
    }
}
