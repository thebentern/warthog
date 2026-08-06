/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * "../mesh.h" shim — the mesh helpers dot11ah's IE/transform code calls.
 *
 * The MPM (Mesh Peering Management) frame predicates and the mesh-network test
 * are `static inline` in the upstream morse mesh.h and depend only on the
 * kernel 802.11 definitions (provided by the compat <linux/ieee80211.h>) and
 * struct dot11ah_ies_mask (dot11ah.h), so they are reproduced verbatim here.
 * Functions that genuinely live in mesh.c (the state machine, ported later)
 * are left as plain declarations.
 */
#ifndef _COMPAT_MORSE_MESH_H_
#define _COMPAT_MORSE_MESH_H_

#include <linux/types.h>
#include <linux/ieee80211.h>
#include <net/mac80211.h>
#include "dot11ah/dot11ah.h"

/* AMPE (Authenticated Mesh Peering Exchange) block sizes carried through the
 * S1G transform unchanged (the codec only needs their lengths). */
#define AMPE_BLOCK_SIZE_OPEN_FRAME      98
#define AMPE_BLOCK_IGTK_DATA_LEN        24  /* KeyID 2 + IPN 6 + Key 16 */
#define AMPE_BLOCK_SIZE_CONFIRM_FRAME   70

/* RSN IE: cipher/AKM selector length and the MFP capability bits. */
#define RSN_SELECTOR_LEN     4
#define RSN_CAPABILITY_MFPR  BIT(6)
#define RSN_CAPABILITY_MFPC  BIT(7)

/* Category code of self-protected action frames. */
#define WLAN_ACTION_SELF_PROTECTED 15

/* Mesh neighbour-entry validity window, in TUs (1024 us), used to age out
 * MBCA neighbour records. Matches the upstream default (~536 ms × 1000). */
#define MESH_CONFIG_NEIGHBOR_ENTRY_VALIDITY_IN_TU 524288

/* action codes of mesh peer link action frames */
enum plink_action_field {
    PLINK_OPEN = 1,
    PLINK_CONFIRM,
    PLINK_CLOSE
};

/* True if the frame is a mesh peering management (MPM) open frame. */
static inline bool morse_dot11_is_mpm_open_frame(const struct ieee80211_mgmt *mesh_mpm_frm)
{
    return (mesh_mpm_frm->u.action.u.self_prot.action_code == WLAN_SP_MESH_PEERING_OPEN);
}

/* True if the frame is a mesh peering management (MPM) confirm frame. */
static inline bool morse_dot11_is_mpm_confirm_frame(struct ieee80211_mgmt *mesh_mpm_frm)
{
    return (mesh_mpm_frm->u.action.u.self_prot.action_code == WLAN_SP_MESH_PEERING_CONFIRM);
}

/* Start address of the IEs in a mesh peering management (MPM) frame. The
 * action-frame `variable` field begins at the capability info, so skip 2 bytes
 * of capability plus 2 more for the AID present in peering-confirm frames. */
static inline u8 *morse_dot11_mpm_frame_ies(struct ieee80211_mgmt *mesh_mpm_frm)
{
    return (mesh_mpm_frm->u.action.u.self_prot.variable + 2 +
            (morse_dot11_is_mpm_confirm_frame(mesh_mpm_frm) ? 2 : 0));
}

/* True if the action frame is a Mesh Peering Management (MPM) frame carrying
 * S1G IEs (open or confirm; close carries none). MPM frames are unprotected. */
static inline bool morse_dot11_is_mpm_frame(struct ieee80211_mgmt *mgmt)
{
    if (ieee80211_has_protected(mgmt->frame_control))
        return false;
    if (mgmt->u.action.category == WLAN_CATEGORY_SELF_PROTECTED &&
        (mgmt->u.action.u.self_prot.action_code == WLAN_SP_MESH_PEERING_OPEN ||
         mgmt->u.action.u.self_prot.action_code == WLAN_SP_MESH_PEERING_CONFIRM))
        return true;
    return false;
}

/* True if a Mesh ID element is present in the parsed IE set. */
static inline bool morse_is_mesh_network(struct dot11ah_ies_mask *ies_mask)
{
    return ies_mask->ies[WLAN_EID_MESH_ID].ptr ? true : false;
}

/* Length of the AMPE (Authenticated Mesh Peering Exchange) element in an MPM
 * frame — lives in mesh.c (ported with the mesh state machine). */
int morse_dot11_get_mpm_ampe_len(struct sk_buff *skb);

#endif /* _COMPAT_MORSE_MESH_H_ */
