/*
 * warthog mesh-support fork -- umac datapath ops for 802.11s mesh mode.
 *
 * Mirrors umac_datapath_ap.c structurally, but every TX-side op is a safe
 * "no peer, no frame" stub: the chip polls for TX autonomously, and the AP
 * dequeue derefs an AP-STA struct that does not exist in mesh mode (that is
 * what crashed mesh boards before these ops existed).
 *
 * The RX dispatch is the substantive part, and it owns two things the vendor
 * path cannot do:
 *   - PROBE_REQ is answered here. A peer in beaconless mode discovers by
 *     probing, and this chip never beacons in mesh mode, so probe/response is
 *     the only discovery path that works. Receiving a probe also triggers our
 *     own peering Open -- warthog<->warthog has no other initiator.
 *   - ACTION category 15 (SELF_PROTECTED, i.e. Mesh Peering) is handled
 *     directly rather than through umac_datapath_process_rx_action_frame's
 *     supplicant fan-out, which expects hostap's mesh_mpm_action_rx and is
 *     never reached in this build. Peering frames were arriving and being
 *     silently discarded.
 *
 * No peer table yet: the lookup ops return NULL and mmwlan_mesh_get_peer_count
 * stays 0, so exactly one peer per board is supported. 4-address mesh data
 * frames are not implemented.
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
#include "umac/data/umac_data.h"
#include "umac/supplicant_shim/umac_supp_shim.h"
#include "dot11/dot11.h"
#include "dot11/dot11_utils.h"
#include "common/mac_address.h"
#include "umac/stats/umac_stats.h"
#include "umac/rc/umac_rc.h"
#include "umac/keys/umac_keys.h"
#include "umac/keys/umac_keys_data.h"
#include "umac/keys/connection_keys.h"
#include "mmosal.h"
#include "common/morse_commands.h"  /* enum morse_sta_state */
#include <string.h>
#include "umac/mesh/umac_mesh.h"
#include "umac/mesh/umac_mesh_ies.h"

/* Data-plane counters (storage in main/at.c; AT+DATASTAT?). */
extern volatile uint32_t g_warthog_tx_data_enq;
extern volatile uint32_t g_warthog_tx_bcast_dup;
extern volatile uint32_t g_warthog_tx_bcast_copy_fail;
extern volatile uint32_t g_warthog_tx_data_deq;
extern volatile uint32_t g_warthog_tx_data_hdr;
extern volatile uint32_t g_warthog_mesh_chip_sta_fail;
extern volatile uint32_t g_warthog_mesh_key_fail;
extern volatile uint32_t g_warthog_ampe_mtk_installed, g_warthog_ampe_mgtk_installed;
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
void umac_mesh_maybe_initiate_mpm(const uint8_t *ta);

/* --- RX management frame dispatch ---------------------------------------- */

extern volatile uint32_t g_warthog_rx_auth;
extern volatile unsigned int g_warthog_rx_selfprot, g_warthog_rx_action_any;

static void process_rx_mgmt_frame_mesh(struct umac_data *umacd,
                                       struct umac_sta_data *stad,
                                       struct mmpktview *rxbufview)
{
    (void)stad;  /* No mesh peer table yet; stad is always NULL here */
    const struct dot11_hdr *header = (struct dot11_hdr *)mmpkt_get_data_start(rxbufview);
    uint16_t frame_control_le = header->frame_control;

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
            }
            if (frame[hdr_len] == DOT11_ACTION_CATEGORY_SELF_PROTECTED &&
                !umac_mesh_sae_active())
            {
                umac_mesh_handle_mpm(dot11_get_ta(header), frame + hdr_len,
                                     frame_len - hdr_len);
                break;
            }

            umac_datapath_process_rx_action_frame(umacd, stad, rxbufview);
            break;
        }

        case DOT11_FC_SUBTYPE_PROBE_REQ:
        {
            /* Answer it. A peer in beaconless mesh mode discovers us by probing,
             * not by hearing a beacon -- and warthog's chip does not currently
             * beacon in mesh mode at all (see AT+BCNSTAT?), so this is the only
             * discovery path that can work. Dropping these was why a probing
             * peer never learned warthog existed. */
            const uint8_t *ta = dot11_get_ta(header);
            g_warthog_prsp_rx++;
            (void)umac_mesh_tx_probe_response(ta);
            /* Also OPEN toward them: warthog<->warthog has no other initiator.
             * Not under SAE -- there hostap's MPM owns the link, and two state
             * machines opening the same peering race each other's link ids. */
            if (!umac_mesh_sae_active())
            {
                umac_mesh_maybe_initiate_mpm(ta);
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
                if (!umac_mesh_sae_active())
                {
                    umac_mesh_maybe_initiate_mpm(ta);
                }
            }
            break;
        }

        case DOT11_FC_SUBTYPE_PROBE_RSP:
        {
            /* Peer discovery for SAE happens HERE, not from beacons.
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
                    umac_supp_mesh_new_peer(dot11_get_ta(header), pf + ie_off,
                                            (size_t)(pflen - ie_off));
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
 * Small and fixed-size on purpose: MPM today holds a single link-id pair, so a
 * larger table would advertise capacity the peering layer cannot deliver. The
 * table is populated by umac_datapath_mesh_add_peer() when a peering reaches
 * ESTAB and emptied by umac_datapath_mesh_del_peer() on Close.
 */
#define MESH_MAX_PEERS 4

static struct umac_sta_data *s_peers[MESH_MAX_PEERS];
static uint8_t s_own_addr[6];
static uint16_t s_num_pkts_queued;

static struct umac_sta_data *mesh_find_peer_(const uint8_t *addr)
{
    if (addr == NULL)
    {
        return NULL;
    }
    for (int i = 0; i < MESH_MAX_PEERS; i++)
    {
        if (s_peers[i] != NULL && umac_sta_data_matches_peer_addr(s_peers[i], addr))
        {
            return s_peers[i];
        }
    }
    return NULL;
}

/* Phase-1 shared keys. Every warthog node uses these; a per-pair SAE-derived
 * MTK replaces the pairwise one later. 16 bytes = CCMP-128. */
/* NOT A SECRET. A counting sequence compiled into every warthog image, used as
 * both the pairwise and the group key when AT+MESHSEC=1.
 *
 * It exists so the data plane can be exercised with CCMP on, not to protect
 * anything: anyone with the firmware has it. It also means "keyed" only
 * interoperates with another warthog carrying the same constant -- a mesh peer
 * that derives real keys (OpenMANET with SAE/AMPE, or any standard secured
 * 802.11s node) cannot decrypt a frame encrypted with it, and warthog cannot
 * decrypt theirs.
 *
 * Peering itself is unauthenticated regardless: main/mesh.c requests
 * MMWLAN_OPEN, so no SAE handshake runs at all. Real per-link keys need
 * SAE authentication plus AMPE key exchange; hostap's mesh_rsn.c is compiled
 * in but nothing in this port drives it. Until that exists, treat the mesh as
 * an untrusted transport and protect traffic above it. */
static const uint8_t k_mesh_p1_mtk[UMAC_KEY_AES_128_LEN] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
};
/* The group key goes into the chip once, VIF-wide. */
static bool s_group_key_in_chip;
static uint32_t s_mesh_key_epoch; /* bumps the TX PN forward on every key install */

static const uint8_t k_mesh_p1_mgtk[UMAC_KEY_AES_128_LEN] = {
    0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78,
    0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0
};

/* Every peer shares ONE pairwise key, because the chip holds exactly one.
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
 * so SAE/AMPE -- and therefore OpenMANET interop -- cannot use chip crypto as
 * this SDK drives it. Host software CCMP is not an optimisation for that path,
 * it is the prerequisite. mesh_derive_mtk_() below stays behind the flag as
 * the seam where a real per-link key lands once that exists.
 */
/* Walk a station up to AUTHORIZED in the chip. Same sequence umac_ap_update_sta()
 * uses; AID is 1-based (0 means "no station" to the chip). */
static void mesh_chip_register_sta_(uint16_t vif_id, uint16_t aid, const uint8_t *peer_addr)
{
    static const enum morse_sta_state seq[] = {
        MORSE_STA_AUTHENTICATED, MORSE_STA_ASSOCIATED, MORSE_STA_AUTHORIZED
    };
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++)
    {
        int r = mmdrv_update_sta_state(vif_id, aid, peer_addr, seq[i]);
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
     * peer changes, and 48-bit CCMP PN space allows ~2.7e11 epochs. */
    s_mesh_key_epoch++;
    mtk.tx_seq = (uint64_t)s_mesh_key_epoch << 20;

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

    /* The GROUP key is a per-VIF resource on this chip, not a per-STA one.
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
        if (mmdrv_install_key(vif_id, 0, &kc) != 0)
        {
            MMLOG_WRN("mesh: MGTK chip install failed\n");
            return MMWLAN_ERROR;
        }
        s_group_key_in_chip = true;
    }
    return st;
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
        if (s_peers[i] == NULL)
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
    /* KEYED, never open. Measured on hardware: with MMWLAN_OPEN every data
     * frame we send is ACKed by the peer's chip (txst acked=137 noack=0) and
     * NEVER delivered to its host (rx_data=0) -- the MM6108 firmware will not
     * deliver unprotected data on a mesh vif, only management. Ruled out by
     * A/B before landing here: 4-addr vs 3-addr, chip STA registration (accepted),
     * BSSID filter (wildcard made no difference). The known-working ESP32 mesh
     * port is a keyed mesh for exactly this reason (its authors call the
     * behaviour a "delivery gate" and route around it with keys).
     *
     * So: mark the stad as secured (this makes the TX path set Protected +
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

    /* Keys. For now, a fixed shared MTK (pairwise, key 0) and MGTK (group,
     * key 1), identical on every node -- the same "static/shared" phase the
     * reference port shipped before SAE. Installed on the PEER's stad, so
     * they are keyed to the peer's AID: that is how the chip selects the key
     * for frames to/from that station. Group key also goes on AID 0 (the
     * chip's "no station" slot) so our own broadcast TX has a key.
     * Deriving per-pair keys from SAE is the follow-up; the install mechanics
     * are unchanged by it. */
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

    s_peers[slot] = stad;
    MMLOG_INF("mesh: peer " MM_MAC_ADDR_FMT " added (slot %d)\n", MM_MAC_ADDR_VAL(peer_addr), slot);
    return MMWLAN_SUCCESS;
}

/* Install a key that came from somewhere else -- in practice AMPE, via the
 * supplicant shim's .set_key driver op.
 *
 * This is the same install the hardcoded path does, with the same two
 * constraints, which are properties of the chip rather than of the key:
 *
 *  - the pairwise TX packet number must only ever move FORWARD across
 *    installs, or existing peers reject our frames as replays; and
 *  - the group key is a VIF-wide resource and must go to the chip exactly
 *    once, at aid 0, while every peer keeps a host-keychain copy for the
 *    per-sender replay check.
 *
 * @param peer_addr  peer the key belongs to; for a group key, any established
 *                   peer (the chip slot is VIF-wide).
 * @param pairwise   true for the MTK, false for the MGTK.
 */
enum mmwlan_status umac_datapath_mesh_set_peer_key(const uint8_t *peer_addr, const uint8_t *key,
                                                   uint8_t key_len, uint8_t key_id, bool pairwise)
{
    if (peer_addr == NULL || key == NULL || key_len != UMAC_KEY_AES_128_LEN)
    {
        return MMWLAN_INVALID_ARGUMENT;
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
        s_mesh_key_epoch++;
        k.tx_seq = (uint64_t)s_mesh_key_epoch << 20;
        enum mmwlan_status st = umac_keys_install_key(stad, vif_id, &k);
        if (st != MMWLAN_SUCCESS)
        {
            MMLOG_WRN("mesh: AMPE MTK install failed %d\n", (int)st);
            g_warthog_mesh_key_fail++;
            return st;
        }
        /* Now -- and only now -- the link is keyed, so the TX path may set
         * Protected and the RX path may insist on it. */
        umac_sta_data_set_security(stad, MMWLAN_SAE, MMWLAN_PMF_DISABLED);
        g_warthog_ampe_mtk_installed++;
        MMLOG_INF("mesh: AMPE MTK installed for " MM_MAC_ADDR_FMT "\n",
                  MM_MAC_ADDR_VAL(peer_addr));
        return MMWLAN_SUCCESS;
    }

    if (!connection_keys_install_key(&umac_sta_data_get_keys(stad)->keys, &k))
    {
        MMLOG_WRN("mesh: AMPE MGTK keychain install failed\n");
        return MMWLAN_ERROR;
    }
    if (!s_group_key_in_chip)
    {
        struct mmdrv_key_conf kc = { .is_pairwise = false, .key_idx = k.key_id,
                                     .length = k.key_len, .tx_pn = 0 };
        memcpy(kc.key, k.key_data, k.key_len);
        if (mmdrv_install_key(vif_id, 0, &kc) != 0)
        {
            MMLOG_WRN("mesh: AMPE MGTK chip install failed\n");
            return MMWLAN_ERROR;
        }
        s_group_key_in_chip = true;
    }
    g_warthog_ampe_mgtk_installed++;
    MMLOG_INF("mesh: AMPE MGTK installed (key_id %u)\n", (unsigned)k.key_id);
    return MMWLAN_SUCCESS;
}

void umac_datapath_mesh_del_peer(const uint8_t *peer_addr)
{
    for (int i = 0; i < MESH_MAX_PEERS; i++)
    {
        if (s_peers[i] != NULL &&
            (peer_addr == NULL || umac_sta_data_matches_peer_addr(s_peers[i], peer_addr)))
        {
            struct umac_sta_data *stad = s_peers[i];
            s_peers[i] = NULL;
            /* Tell the chip the station is gone, then drain the queue. */
            (void)mmdrv_update_sta_state(umac_sta_data_get_vif_id(stad),
                                         umac_sta_data_get_aid(stad),
                                         umac_sta_data_peek_peer_addr(stad),
                                         MORSE_STA_NOTEXIST);
            /* Drain anything still queued for the dead link. */
            struct mmpkt *pkt;
            MMOSAL_TASK_ENTER_CRITICAL();
            while ((pkt = umac_sta_data_pop_pkt(stad)) != NULL)
            {
                s_num_pkts_queued--;
                mmpkt_release(pkt);
            }
            MMOSAL_TASK_EXIT_CRITICAL();
            umac_rc_stop(stad);
            uint16_t vif_id = umac_sta_data_get_vif_id(stad);
            mmosal_free(stad);

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
             * survivor back: station state first, then the key. */
            for (int j = 0; j < MESH_MAX_PEERS; j++)
            {
                if (s_peers[j] == NULL)
                {
                    continue;
                }
                uint8_t survivor[MMWLAN_MAC_ADDR_LEN];
                umac_sta_data_get_peer_addr(s_peers[j], survivor);
                mesh_chip_register_sta_(vif_id, umac_sta_data_get_aid(s_peers[j]), survivor);
                (void)umac_datapath_mesh_install_peer_keys(s_peers[j], vif_id);
            }
        }
    }
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
 * Unicast to a known peer: that peer. Anything else -- broadcast, multicast,
 * or a unicast we have no route for -- goes to the FIRST peer as a
 * "default gateway". That is what makes IP over this link work today: ARP,
 * DHCP, mDNS and Meshtastic's UDP multicast are all group-addressed, and with
 * a two-node link the only place they can usefully go is the other node.
 *
 * This is deliberately NOT mesh forwarding / HWMP path selection. With one
 * peer there is nothing to select; a multi-hop mesh needs a real path table
 * here, and that is the next layer up. */
static struct umac_sta_data *mesh_lookup_stad_by_tx_dest_addr(struct umac_data *umacd,
                                                              const uint8_t *dest_addr)
{
    (void)umacd;
    struct umac_sta_data *stad = mesh_find_peer_(dest_addr);
    if (stad != NULL)
    {
        return stad;
    }
    for (int i = 0; i < MESH_MAX_PEERS; i++)
    {
        if (s_peers[i] != NULL)
        {
            return s_peers[i];
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

static void mesh_queue_one_(struct umac_data *umacd, struct umac_sta_data *stad,
                            struct mmpkt *txbuf)
{
    MMOSAL_TASK_ENTER_CRITICAL();
    umac_sta_data_queue_pkt(stad, txbuf);
    umac_stats_update_datapath_txq_high_water_mark(umacd, ++s_num_pkts_queued);
    MMOSAL_TASK_EXIT_CRITICAL();
    g_warthog_tx_data_enq++;
}

/* Group-addressed traffic is REPLICATED as unicast, one copy per peer.
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
 * single slot. Until that exists, an OpenMANET peer's genuine group frames
 * would fail on our side exactly as our own did. */
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

    if (group)
    {
        /* Copy to every peer except the one we were handed, which takes the
         * original. A failed copy drops that peer's replica only. */
        for (int i = 0; i < MESH_MAX_PEERS; i++)
        {
            if (s_peers[i] == NULL || s_peers[i] == stad)
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
            mesh_queue_one_(umacd, s_peers[i], dup);
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
 * the 802.3 header, and what a future forwarding node needs to relay without
 * losing the endpoints. The vendor RX path is already 4-address aware
 * (dot11_is_4addr_hdr / dot11_get_sa_data read addr4), so nothing changes on
 * receive.
 *
 * A Mesh Control field (s9.2.4.7.3) is NOT inserted. It is required for
 * multi-hop forwarding (TTL, sequence number, address extension) and would
 * need the RX side to strip it. Single-hop peers do not need it, and adding
 * it is the first job of the forwarding layer.
 */
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

    /* A group-addressed frame reaches here once per peer (see
     * mesh_enqueue_tx_frame). Address each replica TO that peer -- RA and DA
     * both the peer -- so it is an ordinary unicast on air and is protected
     * with the pairwise key, the only crypto path this chip handles across
     * several peers. The IP payload is untouched, so a multicast datagram is
     * still delivered by the receiver's IP layer. */
    const uint8_t *da = mm_mac_addr_is_multicast(hdr_8023->dest_addr) ? ra : hdr_8023->dest_addr;
    umac_mesh_ies_build_data_hdr4(hdr, ra, ta, da, hdr_8023->src_addr);
    memcpy(&data_hdr->base.frame_control, &hdr[0], 2);
    mac_addr_copy(data_hdr->base.addr1, &hdr[4]);
    mac_addr_copy(data_hdr->base.addr2, &hdr[10]);
    mac_addr_copy(data_hdr->base.addr3, &hdr[16]);
    mac_addr_copy(data_hdr->addr4, &hdr[24]);
}

/* The freestanding builder hard-codes the FC bit positions; pin them to the
 * SDK's so a header change there breaks the build here, not on air. */
MM_STATIC_ASSERT(DOT11_MASK_FC_TO_DS == 0x0100, "ToDS bit drift");
MM_STATIC_ASSERT(DOT11_MASK_FC_FROM_DS == 0x0200, "FromDS bit drift");
MM_STATIC_ASSERT(DOT11_SHIFT_FC_TYPE == 2 && DOT11_SHIFT_FC_SUBTYPE == 4, "FC shift drift");
MM_STATIC_ASSERT(sizeof(struct dot11_data_hdr) == UMAC_MESH_DATA_HDR4_LEN, "4-addr hdr size drift");

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
    MMLOG_INF("Datapath configured for mesh mode\n");
}

/* Re-install peer @p idx's pairwise key, and nothing else.
 *
 * The decisive probe for "does this chip key per station, or per VIF?": with
 * distinct per-link keys, re-install one peer's key and see whether that
 * link starts working while the OTHER link stops. If connectivity follows the
 * most recent install, there is one slot; if both links keep working, keys are
 * per-station and SAE/AMPE is reachable. Returns the peer's AID, or -1.
 */
void umac_datapath_mesh_service_rekey(void)
{
    /* Serviced from the probe path rather than called from main: morselib is a
     * static archive and the linker will not extract an object to satisfy a
     * call from main -- the same reason every warthog counter is defined in
     * main/at.c and extern'd here. AT+REKEY=<n> sets the request; this runs
     * within one probe interval (~2 s). */
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
    (void)umac_datapath_mesh_install_peer_keys(s_peers[idx], vif_id);
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
    if (!umac_mesh_ies_s1g_beacon_sa(f, flen, sa) || !umac_mesh_s1g_beacon_is_our_mesh(f, flen))
    {
        return;
    }
    g_warthog_s1g_bcn_ours++;
    memcpy((void *)g_warthog_s1g_bcn_sa, sa, MMWLAN_MAC_ADDR_LEN);

    /* Announce ourselves ONCE per peer. Answering every beacon makes mac80211
     * raise NEW_PEER_CANDIDATE repeatedly, and wpa_supplicant restarts the
     * peering each time -- see umac_mesh_note_s1g_beacon(). */
    if (umac_mesh_note_s1g_beacon(sa))
    {
        /* First sight: announce ourselves so the peer learns we exist. */
        g_warthog_s1g_bcn_new++;
        (void)umac_mesh_tx_probe_response(sa);
        if (umac_mesh_sae_active())
        {
            /* Under SAE, beacons are the discovery path for mac80211-style
             * peers (OpenMANET): they beacon and never probe, so the
             * probe-response path cannot see them. The S1G beacon's SA reads
             * true from the chip (verified against a live node), and the
             * candidate still passes umac_supp_mesh_new_peer's auth-protocol
             * gate, so an open node sharing the Mesh ID is not SAE'd at.
             * Warthog peers are discovered by the probe-response path; both
             * feed the same gate. */
            uint32_t boff = umac_mesh_ies_s1g_beacon_ie_offset(
                (uint16_t)(f[0] | ((uint16_t)f[1] << 8)));
            if (flen > boff)
            {
                umac_supp_mesh_new_peer(sa, f + boff, (size_t)(flen - boff));
            }
        }
        else
        {
            umac_mesh_maybe_initiate_mpm(sa);
        }
    }
    else if (!umac_mesh_sae_active() && umac_mesh_peering_incomplete(sa))
    {
        /* Known peer, handshake unfinished: retransmit the Open only. NOT the
         * probe response -- that is what restarts the peer's FSM. */
        g_warthog_s1g_bcn_retry++;
        umac_mesh_maybe_initiate_mpm(sa);
    }
}
