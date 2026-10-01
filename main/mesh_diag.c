/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mesh_diag.h"

#include <stdio.h>

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
    /* A floor skip or a probe naming our mesh: a would-be peer is audible here. */
    if (in->floor_skips > 0 && in->floor_passes == 0) {
        return WARTHOG_MESH_DIAG_BELOW_FLOOR;
    }
    if (in->beacons_heard == 0 && in->mesh_probes > 0) {
        return WARTHOG_MESH_DIAG_BEACONLESS_PEER;
    }
    if (in->beacons_heard == 0) {
        return WARTHOG_MESH_DIAG_NOTHING_AUDIBLE;
    }
    return WARTHOG_MESH_DIAG_AUDIBLE_NOT_JOINING;
}

void warthog_mesh_diag_window_reset(struct warthog_mesh_diag_window *w,
                                    const struct warthog_mesh_diag_counts *now)
{
    if (w != NULL && now != NULL) {
        w->start = *now;
        w->last = *now;
    }
}

void warthog_mesh_diag_window_roll(struct warthog_mesh_diag_window *w,
                                   const struct warthog_mesh_diag_counts *now)
{
    if (w != NULL && now != NULL) {
        w->start = w->last;
        w->last = *now;
    }
}

void warthog_mesh_diag_window_fill(const struct warthog_mesh_diag_window *w,
                                   const struct warthog_mesh_diag_counts *now,
                                   struct warthog_mesh_diag_in *in)
{
    if (w == NULL || now == NULL || in == NULL) {
        return;
    }
    /* Unsigned: a counter that wrapped since the start still subtracts right. */
    in->mesh_probes = now->mesh_probes - w->start.mesh_probes;
    in->floor_skips = now->floor_skips - w->start.floor_skips;
    in->floor_passes = now->floor_passes - w->start.floor_passes;
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
    case WARTHOG_MESH_DIAG_BELOW_FLOOR:
        return "every neighbour we would peer with was heard at or below the RSSI floor "
               "(AT+MESHRSSI?), so no peering was started. Move the nodes closer, or "
               "lower the floor (AT+MESHRSSI=0 is off). OpenMANET ignores us at or "
               "below -80 dBm too";
    case WARTHOG_MESH_DIAG_BEACONLESS_PEER:
        return "0 beacons heard, but probe requests name our mesh: a peer that sends "
               "no beacons (beaconless mode) is on this channel. Check security and "
               "passphrase, and that it hears us above its RSSI threshold";
    case WARTHOG_MESH_DIAG_NOTHING_AUDIBLE:
        return "0 beacons heard: nothing is audible. Wrong channel or bandwidth, "
               "or out of range. Check the channel first";
    case WARTHOG_MESH_DIAG_AUDIBLE_NOT_JOINING:
        return "beacons heard but 0 peers: the mesh is audible and we will not "
               "join it. Check mesh ID, operating class and security";
    }
    return "unknown";
}

static const char *mesh_start_vif_name_(uint32_t type)
{
    switch (type) {
    case 0: return "none";
    case 1: return "sta";
    case 2: return "ap";
    case 5: return "mesh";
    default: return "other";
    }
}

enum warthog_mesh_start_verdict warthog_mesh_start_result(const struct warthog_mesh_start_in *in,
                                                          char *buf, size_t len)
{
    const char *cfg = in->meshcfg_mode == 0 ? "none"
                    : in->meshcfg_refused != 0 ? "REFUSED" : "accepted";
    const char *mode = in->meshcfg_mode == 1 ? ",beaconing" : in->meshcfg_mode == 2 ? ",beaconless" : "";
    enum warthog_mesh_start_verdict v = WARTHOG_MESH_START_FAIL;
    if (in->status == 0) {
        /* The chip interface this build asks for: MESH on -meshvif, the boot STA one elsewhere. */
        const uint32_t want = in->built_mesh ? 5u : 1u;
        v = (in->chip_vif == want && in->meshcfg_refused == 0) ? WARTHOG_MESH_START_PASS
                                                               : WARTHOG_MESH_START_WARN;
    }
    int n = (v == WARTHOG_MESH_START_FAIL)
                ? snprintf(buf, len, "RESULT: FAIL -- mmwlan_mesh_enable() returned status=%d;", in->status)
                : snprintf(buf, len, "RESULT: %s -- mesh up on",
                           v == WARTHOG_MESH_START_PASS ? "PASS" : "WARN");
    if (n > 0 && (size_t)n < len) {
        snprintf(buf + n, len - (size_t)n,
                 " chip_vif=%s(%lu) fallback=%lu(add_st=%ld) mesh_config=%s%s(st=%ld)",
                 mesh_start_vif_name_(in->chip_vif), (unsigned long)in->chip_vif,
                 (unsigned long)in->fallback, (long)in->add_status, cfg, mode,
                 (long)in->meshcfg_status);
    }
    return v;
}
