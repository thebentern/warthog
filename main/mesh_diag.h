/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WARTHOG_MESH_DIAG_H
#define WARTHOG_MESH_DIAG_H

/* Why a mesh node is not peering.
 *
 * Split out from the reporting so the decision can be tested on the host: the
 * branch that picks the cause is the part that can be wrong, and it is the
 * part nobody can see without a radio and a second node to mismatch against.
 * Freestanding on purpose -- no SDK, no ESP-IDF, libc only. */

#include <stddef.h>
#include <stdint.h>

enum warthog_mesh_diag {
    /* Has at least one peer; nothing to report. */
    WARTHOG_MESH_DIAG_PEERED = 0,
    /* The channel list was refused, so the radio is not where we say it is.
     * Checked before everything else: every other reading is then misleading. */
    WARTHOG_MESH_DIAG_CHAN_REJECTED,
    /* No channel pinned -- the radio holds the whole country list, so its
     * operating channel is neither chosen nor observable. */
    WARTHOG_MESH_DIAG_CHAN_UNPINNED,
    /* Pinned and applied, but nothing is audible on it. */
    WARTHOG_MESH_DIAG_NOTHING_AUDIBLE,
    /* Beacons are arriving and we still will not peer. */
    WARTHOG_MESH_DIAG_AUDIBLE_NOT_JOINING,
};

struct warthog_mesh_diag_in {
    unsigned int peers;
    /* mmwlan_set_channel_list() status; 0 is success. */
    int          chan_pin_status;
    /* S1G channel actually in force, 0 when nothing was pinned. */
    unsigned int applied_chan;
    /* Bus-level beacon pages; this is what separates the last two causes. */
    uint32_t     beacons_heard;
};

enum warthog_mesh_diag warthog_mesh_diagnose(const struct warthog_mesh_diag_in *in);

/* One line naming the cause and what to do about it. Never NULL. */
const char *warthog_mesh_diag_text(enum warthog_mesh_diag d);

#endif /* WARTHOG_MESH_DIAG_H */
