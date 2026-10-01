/*
 * warthog mesh -- software CCMP decryption for received data frames.
 *
 * The chip holds one pairwise key. AMPE derives a distinct MTK per link, so on
 * a mesh with more than one peer the chip's slot always holds the wrong key for
 * somebody: every peer's frames are acknowledged at the MAC (an ACK precedes
 * decryption) and then dropped, which reads as a working link that carries no
 * traffic. Decrypting in the host is what makes per-link keys possible.
 *
 * The chip's own behaviour makes this cheap. A frame it could not decrypt is
 * still delivered, with the CCMP header, the ciphertext and the MIC intact and
 * the Protected bit set -- measured on hardware as rxdrop reason=4 with
 * nodec uni climbing. So this only has to decrypt in place: afterwards the
 * buffer is byte-identical in shape to one the chip decrypted, and the existing
 * strip-and-replay path handles it unchanged.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 */

#include "umac_mesh_ccm.h"
#include "umac_mesh_ccmp_hdr.h"
#include "umac/data/umac_data.h"
#include "umac/keys/umac_keys.h"
#include "umac/keys/connection_keys.h"
#include "dot11/dot11.h"
#include "dot11/dot11_frames.h"
#include "mmpkt.h"
#include <string.h>

/* Storage in main/at.c (AT+SWCCMP?). */
/* Runtime switch, default OFF. Software decryption runs on the RX task, and a
 * fault there is a reboot loop -- which is the one state where the console
 * never stays up long enough to say why. Booting with it off means the board
 * is always reachable, and AT+SWCCMP=1 turns it on with traffic already
 * flowing, so a crash is attributable to the moment it was switched. */
extern volatile uint32_t g_warthog_host_ccmp_on;
extern volatile uint32_t g_warthog_swccmp_ok;
extern volatile uint32_t g_warthog_swccmp_micfail;
extern volatile uint32_t g_warthog_swccmp_nokey;
extern volatile uint32_t g_warthog_swccmp_grpkey;
extern volatile uint32_t g_warthog_swccmp_badhdr;
extern volatile uint32_t g_warthog_swccmp_short;
extern volatile uint32_t g_warthog_swccmp_tried;
extern volatile uint32_t g_warthog_swccmp_last_keyid;
extern volatile uint32_t g_warthog_swccmp_last_aadlen;
extern volatile uint8_t g_warthog_swccmp_last_aad[32];
extern volatile uint32_t g_warthog_swccmp_fail_len;
extern volatile uint32_t g_warthog_swccmp_fail_keyid;
extern volatile uint8_t g_warthog_swccmp_fail_pn[6];
extern volatile uint8_t g_warthog_swccmp_fail_hdr[32];

#define SWCCMP_MIC_LEN 8u

/* Decrypt a Protected mesh data frame in place.
 *
 * @param stad       the transmitting peer, already looked up -- its keychain
 *                   holds the per-link MTK even when the chip does not.
 * @param header     the 802.11 header, still addressing frame-control byte 0.
 *                   mmpkt_remove_from_start() only advances an offset, so the
 *                   header bytes the AAD covers are still in the buffer.
 * @param rxbufview  positioned at the CCMP header.
 *
 * @returns true if the MIC verified and the body is now plaintext. False
 *          leaves the frame undecryptable and the caller must drop it -- CCM
 *          decrypts before it authenticates, so on failure the body has
 *          already been overwritten with garbage.
 */
bool umac_mesh_rx_host_ccmp(struct umac_sta_data *stad, const struct dot11_hdr *header,
                            struct mmpktview *rxbufview)
{
    if (!g_warthog_host_ccmp_on || stad == NULL || header == NULL || rxbufview == NULL)
    {
        return false;
    }
    g_warthog_swccmp_tried++;

    uint8_t *body = (uint8_t *)mmpkt_get_data_start(rxbufview);
    uint32_t len = mmpkt_get_data_length(rxbufview);
    if (body == NULL || len <= UMAC_CCMP_HDR_LEN + SWCCMP_MIC_LEN)
    {
        g_warthog_swccmp_short++;
        return false;
    }

    uint8_t pn[6];
    uint8_t key_id = 0;
    if (!umac_ccmp_parse_header(body, pn, &key_id))
    {
        /* No ExtIV: not a CCMP frame, so there is nothing here to decrypt. */
        g_warthog_swccmp_badhdr++;
        return false;
    }
    g_warthog_swccmp_last_keyid = key_id;

    /* The key id comes off the air, so bound it before handing it to the
     * keychain: connection_keys_get_key_data() opens with an assert on the
     * range, and an assert in the RX path is a reboot triggered by whatever
     * a peer chose to transmit. Two bits cannot exceed the keychain today,
     * which is exactly why this is worth pinning down before someone widens
     * either end. */
    if (key_id >= UMAC_KEYS_NUM_KEY_IDS)
    {
        g_warthog_swccmp_badhdr++;
        return false;
    }

    const uint8_t *key = umac_keys_get_key_data(stad, key_id);
    if (key == NULL || umac_keys_get_key_len(stad, key_id) != UMAC_KEY_AES_128_LEN)
    {
        /* The peer keyed the frame with something we were never given. */
        g_warthog_swccmp_nokey++;
        return false;
    }

    /* Individually addressed means pairwise. Every member of the mesh holds a
     * peer's MGTK, so a unicast keyed with it could have been forged by any of
     * them in that peer's name. */
    if ((header->addr1[0] & 0x01u) == 0u &&
        umac_keys_get_key_type(stad, key_id) != UMAC_KEY_TYPE_PAIRWISE)
    {
        g_warthog_swccmp_grpkey++;
        return false;
    }

    uint8_t aad[UMAC_CCMP_AAD_MAXLEN];
    uint8_t nonce[13];
    uint32_t aad_len = umac_ccmp_build_aad((const uint8_t *)header, aad);
    umac_ccmp_build_nonce((const uint8_t *)header, pn, nonce);

    /* Snapshot the AAD. If a peer's frames start failing the MIC this is the
     * first thing worth comparing against what the sender authenticated --
     * the two ends build this from independent implementations of the same
     * S1G/11n conversion, which is the likeliest place for them to disagree. */
    g_warthog_swccmp_last_aadlen = aad_len;
    memcpy((void *)g_warthog_swccmp_last_aad, aad, aad_len > 32u ? 32u : aad_len);

    uint8_t *cipher = body + UMAC_CCMP_HDR_LEN;
    uint32_t cipher_len = len - UMAC_CCMP_HDR_LEN - SWCCMP_MIC_LEN;
    const uint8_t *mic = body + len - SWCCMP_MIC_LEN;

    if (warthog_ccm_ad(key, nonce, SWCCMP_MIC_LEN, aad, aad_len, cipher, cipher_len, mic) != 0)
    {
        g_warthog_swccmp_micfail++;
        /* The last failure only, so a passing frame cannot overwrite what went wrong. */
        g_warthog_swccmp_fail_len = len;
        g_warthog_swccmp_fail_keyid = key_id;
        memcpy((void *)g_warthog_swccmp_fail_pn, pn, sizeof(pn));
        memcpy((void *)g_warthog_swccmp_fail_hdr, header, sizeof(g_warthog_swccmp_fail_hdr));
        return false;
    }

    /* Deliberately no replay check here. The caller runs ccmp_is_valid() next,
     * and that must not happen until the MIC has verified: it updates the
     * stored receive counter on success, so a forged frame carrying a large PN
     * would advance it and lock out the real peer for good. */
    g_warthog_swccmp_ok++;
    return true;
}

/* A fresh group frame the chip opened from @p stad and the host takes as that peer's: do the MIC
 * octets the chip left verify under the peer's key @p key_id? Read-only. mic ok means the chip
 * opened it under that key and kept the MIC, and arms the check for frame class @p cls (0 data,
 * 1 management): a match by chance is 2^-64, so it proves the chip leaves the MIC. mic bad means
 * it did not keep the MIC, or opened the frame under another key. False -- drop it -- for a mic
 * bad once @p cls is armed; until then the check only counts.
 *
 * @param header  the 802.11 header (the AAD and nonce come from it).
 * @param ccmp    the CCMP header, followed by the plaintext and the 8 MIC octets.
 * @param len     octets from @p ccmp to the end of the MIC. */
extern volatile uint32_t g_warthog_rx_grp_mic_ok, g_warthog_rx_grp_mic_bad, g_warthog_rx_grp_mic_armed;
bool umac_mesh_rx_chip_mic_note(struct umac_sta_data *stad, uint8_t key_id, const uint8_t *header,
                                const uint8_t *ccmp, uint32_t len, unsigned cls)
{
    uint8_t pn[6], kid = 0, mic[SWCCMP_MIC_LEN], aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
    const uint8_t *key = (stad != NULL && key_id < UMAC_KEYS_NUM_KEY_IDS)
                             ? umac_keys_get_key_data(stad, key_id) : NULL;
    bool ok = key != NULL && umac_keys_get_key_len(stad, key_id) == UMAC_KEY_AES_128_LEN &&
              header != NULL && ccmp != NULL && len >= UMAC_CCMP_HDR_LEN + SWCCMP_MIC_LEN &&
              umac_ccmp_parse_header(ccmp, pn, &kid) && kid == key_id;
    if (ok)
    {
        const uint32_t aad_len = umac_ccmp_build_aad(header, aad);
        umac_ccmp_build_nonce(header, pn, nonce);
        ok = warthog_ccm_mic(key, nonce, SWCCMP_MIC_LEN, aad, aad_len, ccmp + UMAC_CCMP_HDR_LEN,
                             len - UMAC_CCMP_HDR_LEN - SWCCMP_MIC_LEN, mic) == 0 &&
             memcmp(mic, ccmp + len - SWCCMP_MIC_LEN, SWCCMP_MIC_LEN) == 0;
    }
    const uint32_t bit = 1u << (cls & 1u);
    if (ok)
    {
        g_warthog_rx_grp_mic_ok++;
        g_warthog_rx_grp_mic_armed |= bit;
        return true;
    }
    g_warthog_rx_grp_mic_bad++;
    return (g_warthog_rx_grp_mic_armed & bit) == 0u;
}

/* ---- TX ----------------------------------------------------------------- */

extern volatile uint32_t g_warthog_swccmp_tx_ok;
extern volatile uint32_t g_warthog_swccmp_tx_fail;

/* Encrypt a mesh data frame's body in place and wrap it in CCMP.
 *
 * Called with the buffer holding only what gets encrypted -- the Mesh Control
 * field and the payload -- and BEFORE the MAC header and QoS Control are
 * prepended. That ordering is deliberate: CCMP sits between the QoS Control
 * and the encrypted body, so doing this first turns the CCMP header into one
 * more prepend instead of an insert into the middle of a buffer.
 *
 * The header those two fields will form is not in the buffer yet, so the AAD
 * is built from a scratch copy of them. The builders index QoS at hdr+30 for a
 * 4-address frame, which is exactly a 30-byte MAC header followed by the
 * 2-byte QoS Control.
 *
 * @param stad     the peer this frame is addressed to. The caller hands frames
 *                 under our own MGTK (standard group frames under SAE) to the
 *                 chip instead, so the key here is the link's pairwise one.
 * @param key_id   the key to encrypt under.
 * @param mac_hdr  the 30-byte 4-address header about to be prepended.
 * @param qos      the 2-byte QoS Control about to be prepended.
 * @param pn       the packet number to use; must never repeat for this key.
 *
 * @returns true if the body is now ciphertext with the CCMP header prepended
 *          and the MIC appended. On false the buffer is untouched and the
 *          caller must not claim the frame is protected.
 */
bool umac_mesh_tx_host_ccmp(struct umac_sta_data *stad, uint8_t key_id,
                            const uint8_t *mac_hdr, const uint8_t *qos,
                            const uint8_t pn[6], struct mmpktview *txbufview)
{
    if (!g_warthog_host_ccmp_on || stad == NULL || mac_hdr == NULL || qos == NULL ||
        txbufview == NULL)
    {
        return false;
    }
    if (key_id >= UMAC_KEYS_NUM_KEY_IDS)
    {
        g_warthog_swccmp_tx_fail++;
        return false;
    }

    const uint8_t *key = umac_keys_get_key_data(stad, key_id);
    if (key == NULL || umac_keys_get_key_len(stad, key_id) != UMAC_KEY_AES_128_LEN)
    {
        g_warthog_swccmp_tx_fail++;
        return false;
    }

    uint8_t *body = (uint8_t *)mmpkt_get_data_start(txbufview);
    uint32_t body_len = mmpkt_get_data_length(txbufview);
    /* Checked before encrypting in place: a frame refused here is still plaintext. */
    if (body == NULL || body_len == 0u ||
        mmpkt_available_space_at_end(txbufview) < SWCCMP_MIC_LEN ||
        mmpkt_available_space_at_start(txbufview) < UMAC_CCMP_HDR_LEN)
    {
        g_warthog_swccmp_tx_fail++;
        return false;
    }

    /* The header the receiver will authenticate, assembled before it exists. */
    uint8_t scratch[32];
    memcpy(scratch, mac_hdr, 30u);
    memcpy(scratch + 30u, qos, 2u);

    uint8_t aad[UMAC_CCMP_AAD_MAXLEN];
    uint8_t nonce[13];
    uint32_t aad_len = umac_ccmp_build_aad(scratch, aad);
    umac_ccmp_build_nonce(scratch, pn, nonce);

    uint8_t mic[SWCCMP_MIC_LEN];
    if (warthog_ccm_ae(key, nonce, SWCCMP_MIC_LEN, aad, aad_len, body, body_len, mic) != 0)
    {
        /* warthog_ccm_ae encrypts in place, so a failure here has already
         * scribbled on the body. Nothing may transmit it. */
        g_warthog_swccmp_tx_fail++;
        return false;
    }

    uint8_t ccmp_hdr[UMAC_CCMP_HDR_LEN];
    umac_ccmp_write_header(ccmp_hdr, pn, key_id);

    mmpkt_append_data(txbufview, mic, sizeof(mic));
    mmpkt_prepend_data(txbufview, ccmp_hdr, sizeof(ccmp_hdr));

    g_warthog_swccmp_tx_ok++;
    return true;
}

/* Protect a built management frame in place: 24-byte header, UMAC_CCMP_HDR_LEN
 * reserved, the plaintext body, SWCCMP_MIC_LEN reserved. @p pn must be this key's
 * next unused PN (umac_datapath_mesh_take_tx_pn), shared with the data path. */
bool umac_mesh_tx_host_ccmp_mgmt(const uint8_t key[16], uint8_t key_id, uint64_t pn64,
                                 uint8_t *frame, uint32_t len)
{
    const uint32_t hdr = 24u;
    if (key == NULL || frame == NULL || key_id >= UMAC_KEYS_NUM_KEY_IDS ||
        len <= hdr + UMAC_CCMP_HDR_LEN + SWCCMP_MIC_LEN)
    {
        g_warthog_swccmp_tx_fail++;
        return false;
    }
    const uint8_t pn[6] = { (uint8_t)(pn64 >> 40), (uint8_t)(pn64 >> 32), (uint8_t)(pn64 >> 24),
                            (uint8_t)(pn64 >> 16), (uint8_t)(pn64 >> 8),  (uint8_t)pn64 };
    frame[1] |= 0x40u; /* Protected: the AAD forces it on anyway */
    umac_ccmp_write_header(frame + hdr, pn, key_id);

    uint8_t aad[UMAC_CCMP_AAD_MAXLEN];
    uint8_t nonce[13];
    uint32_t aad_len = umac_ccmp_build_aad(frame, aad);
    umac_ccmp_build_nonce(frame, pn, nonce);
    uint8_t *plain = frame + hdr + UMAC_CCMP_HDR_LEN;
    const uint32_t plain_len = len - hdr - UMAC_CCMP_HDR_LEN - SWCCMP_MIC_LEN;
    if (warthog_ccm_ae(key, nonce, SWCCMP_MIC_LEN, aad, aad_len, plain, plain_len,
                       plain + plain_len) != 0)
    {
        g_warthog_swccmp_tx_fail++;
        return false;
    }
    g_warthog_swccmp_tx_ok++;
    return true;
}
