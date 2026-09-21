/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mesh_diag.h"

enum warthog_mesh_diag warthog_mesh_diagnose(const struct warthog_mesh_diag_in *in)
{
    if (in == NULL) {
        return WARTHOG_MESH_DIAG_PEERED;
    }
    if (in->peers > 0) {
        return WARTHOG_MESH_DIAG_PEERED;
    }
    /* Order matters. A refused channel makes the applied channel and the
     * beacon count both meaningless, and an unpinned radio makes the beacon
     * count mean "something was audible somewhere", not "on our channel". */
    if (in->chan_pin_status != 0) {
        return WARTHOG_MESH_DIAG_CHAN_REJECTED;
    }
    if (in->applied_chan == 0) {
        return WARTHOG_MESH_DIAG_CHAN_UNPINNED;
    }
    if (in->beacons_heard == 0) {
        return WARTHOG_MESH_DIAG_NOTHING_AUDIBLE;
    }
    return WARTHOG_MESH_DIAG_AUDIBLE_NOT_JOINING;
}

const char *warthog_mesh_diag_text(enum warthog_mesh_diag d)
{
    switch (d) {
    case WARTHOG_MESH_DIAG_PEERED:
        return "peered";
    case WARTHOG_MESH_DIAG_CHAN_REJECTED:
        return "the channel list was REJECTED, so the radio is NOT on the channel "
               "above -- fix that before reading anything else here";
    case WARTHOG_MESH_DIAG_CHAN_UNPINNED:
        return "no channel is pinned: the radio holds the whole country list, so "
               "its operating channel is neither chosen nor observable. Set "
               "AT+MESHCHAN= to match the peer. An unpinned radio meeting a mesh "
               "is luck, not configuration";
    case WARTHOG_MESH_DIAG_NOTHING_AUDIBLE:
        return "0 beacons heard: nothing is audible. Wrong channel or bandwidth, "
               "or out of range. Check the channel first";
    case WARTHOG_MESH_DIAG_AUDIBLE_NOT_JOINING:
        return "beacons heard but 0 peers: the mesh is audible and we will not "
               "join it. Check mesh ID, operating class and security";
    }
    return "unknown";
}
