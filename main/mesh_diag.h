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
    /* Every new peering we would start was refused by the candidate RSSI floor. */
    WARTHOG_MESH_DIAG_BELOW_FLOOR,
    /* No beacons, but probe requests name our mesh: a peer that does not beacon. */
    WARTHOG_MESH_DIAG_BEACONLESS_PEER,
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
    /* Probe requests naming our mesh: how a beaconless peer is heard. */
    uint32_t     mesh_probes;
    /* New peerings the RSSI floor refused, and ones it let through, from frames
     * naming our mesh. These three count over warthog_mesh_diag_window. */
    uint32_t     floor_skips;
    uint32_t     floor_passes;
};

enum warthog_mesh_diag warthog_mesh_diagnose(const struct warthog_mesh_diag_in *in);

/* The floor and probe inputs above as cumulative totals, as the counters hold them. */
struct warthog_mesh_diag_counts {
    uint32_t mesh_probes;
    uint32_t floor_skips;
    uint32_t floor_passes;
};

/* What the diagnosis counts over: from the no-peers report before last, or from
 * the last tick with a peer, to now. An old sighting stops masking a cause two
 * reports on, and every report spans at least one full report period. */
struct warthog_mesh_diag_window {
    struct warthog_mesh_diag_counts start; /* totals at the window's start */
    struct warthog_mesh_diag_counts last;  /* totals at the last report */
};

/* Peered: the window starts again at @p now. */
void warthog_mesh_diag_window_reset(struct warthog_mesh_diag_window *w,
                                    const struct warthog_mesh_diag_counts *now);

/* A no-peers report was made at @p now: the window now starts at the previous one. */
void warthog_mesh_diag_window_roll(struct warthog_mesh_diag_window *w,
                                   const struct warthog_mesh_diag_counts *now);

/* Set @p in's probe and floor inputs to what the totals @p now gained in the window. */
void warthog_mesh_diag_window_fill(const struct warthog_mesh_diag_window *w,
                                   const struct warthog_mesh_diag_counts *now,
                                   struct warthog_mesh_diag_in *in);

/* One line naming the cause and what to do about it. Never NULL. */
const char *warthog_mesh_diag_text(enum warthog_mesh_diag d);

/* The mesh start's verdict, from what the chip answered rather than what was asked. */
enum warthog_mesh_start_verdict {
    /* Up on the chip interface this build asks for, MESH_CONFIG(START) accepted. */
    WARTHOG_MESH_START_PASS = 0,
    /* Up, but on a fallback STA interface or with MESH_CONFIG(START) refused. */
    WARTHOG_MESH_START_WARN,
    /* mmwlan_mesh_enable() failed. */
    WARTHOG_MESH_START_FAIL,
};

struct warthog_mesh_start_in {
    int      status;          /* mmwlan_mesh_enable(); 0 is success */
    int      built_mesh;      /* WARTHOG_MESH_CHIP_VIF_MESH: the build asks for a MESH chip VIF */
    uint32_t chip_vif;        /* chip VIF type in use: 0 none, 1 STA, 2 AP, 5 MESH */
    uint32_t fallback;        /* MESH adds that fell back to STA */
    int32_t  add_status;      /* and the last one's status or error */
    uint32_t meshcfg_refused; /* MESH_CONFIG the chip refused */
    int32_t  meshcfg_status;  /* and the last refusal's status */
    uint32_t meshcfg_mode;    /* MESH_CONFIG(START) sent: 0 none, 1 beaconing, 2 beaconless */
};

/* A buffer this long holds any RESULT line whole. */
#define WARTHOG_MESH_START_RESULT_LEN 192

/* The RESULT line for @p in into @p buf (no line ending); returns the verdict. */
enum warthog_mesh_start_verdict warthog_mesh_start_result(const struct warthog_mesh_start_in *in,
                                                          char *buf, size_t len);

#endif /* WARTHOG_MESH_DIAG_H */
