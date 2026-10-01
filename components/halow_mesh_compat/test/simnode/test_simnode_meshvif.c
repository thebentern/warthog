/*
 * The mesh's chip VIF type, through the real umac_interface.c, umac_mesh.c and datapath.
 *
 * The board boots the chip with a STA-type VIF (mmwlan_boot adds a NONE interface) and,
 * before WARTHOG_MESH_CHIP_VIF_MESH, the mesh ran on it: umac_interface_add(MESH) had no
 * re-add branch for MESH, so no ADD_INTERFACE(MESH) ever reached the chip. The MM6108
 * builds an NDP CTS from per-VIF BSSID/AID state on a STA VIF and from the RTS's TA on a
 * MESH VIF, so on air only the most recently registered peer accepted our CTS.
 *
 * The fake chip logs every ADD_INTERFACE, REMOVE_INTERFACE, GET_CAPABILITIES, BSSID_SET,
 * BSS_CONFIG, BSS_BEACON_CONFIG, MESH_CONFIG and SET_STA_STATE with the status it answered;
 * these assertions read that log. Built twice, each as warthog-mesh-sae-swccmp builds its
 * crypto:
 *
 * test_simnode_meshvif (-DWARTHOG_MESH_CHIP_VIF_MESH=1, the -meshvif env):
 *  (1) the mesh removes the boot VIF and adds a MESH one (type 5), accepted, then reads
 *      that VIF's capabilities, as Linux's add_interface does; after the scan probe too;
 *      the beacon template is tagged for the VIF the mesh runs on;
 *  (2) a MESH VIF the chip accepts with an id other than 0 is removed and the mesh falls
 *      back to a STA VIF, counted (add_st -1000 - id): beacons and data are tagged VIF 0
 *      (upstream's single-VIF datapath), so a MESH VIF 2 would carry only our management
 *      frames; a failed remove of it fails the start;
 *  (3) a MESH_CONFIG the chip refuses is counted with its status and the mesh still
 *      comes up (as before: its status was never read);
 *  (4) an ADD_INTERFACE(MESH) the chip refuses, or whose transport fails, falls back to
 *      a STA VIF, counted with the status; the node comes up as the baseline does,
 *      derived BSSID included; a REMOVE_INTERFACE that fails keeps the STA VIF;
 *  (5) a fallback that also fails leaves no VIF claimed and the start fails cleanly;
 *  (6) a peer leaving a MESH VIF walks 3 -> 2 -> 1 as mac80211 + morse_driver do, and
 *      the survivors' stations are left alone; on the fallback STA VIF the old behaviour
 *      stands;
 *  (7) the MESH VIF gets no BSS_BEACON_CONFIG: Linux sends it only when beaconing restarts
 *      (morse_driver mac.c:4221-4232), never on a VIF's first start, so BSS_CONFIG and then
 *      MESH_CONFIG(START); the fallback STA VIF keeps its order;
 *  (8) MESH_CONFIG(START) on the MESH VIF is beaconless, once, as the OpenMANET Pis send it
 *      (mmdrv_mesh_config makes that MBCA and its timers 0 too, test_chipvif_glue): on air on
 *      2026-09-30 the beaconing tuple on a MESH VIF hung the board about 2 s after the
 *      firmware started (USB gone, no reboot, no coredump). The fallback STA VIF keeps the
 *      beaconing tuple every STA-VIF build sends;
 *  (9) a SET_STA_STATE the chip refuses, registering a peer or walking one down, is counted
 *      by state with its status, and the node carries on;
 *  (10) our own MGTK refused by the chip is counted, is not taken as installed, and goes in
 *      at the next peer.
 * test_simnode_meshvif_off (no flag, as every other env ships): (11), (12) and (13) pin the
 * behaviour before the change:
 *  (11) the boot STA VIF carries the mesh: one ADD_INTERFACE(1), no REMOVE, type 1;
 *  (12) BSSID_SET goes out with the derived BSSID, and a refusal is counted;
 *  (13) a peer leaving: NOTEXIST, then every survivor re-walked 2 -> 3 -> 4;
 *  (14) a refused MESH_CONFIG is counted and the mesh still comes up; it beacons, the
 *      tuple unchanged by the MESH-VIF work;
 *  (15) BSS_BEACON_CONFIG, BSSID_SET, BSS_CONFIG, MESH_CONFIG in that order, and a refused
 *      BSS_BEACON_CONFIG is counted with its status;
 *  (16) refused SET_STA_STATEs (NOTEXIST, a survivor's AUTHORIZED) and a refused own MGTK
 *      are counted.
 */
#include <stdio.h>
#include <string.h>

#include "simnode.h"
#include "mmdrv.h"
#include "mmpkt.h"
#include "common/morse_commands.h"
#include "umac/data/umac_data.h"
#include "umac/interface/umac_interface.h"
#include "umac/mesh/umac_mesh_beacon.h"

#ifndef WARTHOG_MESH_CHIP_VIF_MESH
#define WARTHOG_MESH_CHIP_VIF_MESH 0
#endif

extern volatile uint32_t g_warthog_chipvif_type, g_warthog_chipvif_id, g_warthog_chipvif_fallback;
extern volatile int32_t g_warthog_chipvif_add_status;
extern volatile uint32_t g_warthog_chipcmd_bssid_refused, g_warthog_chipcmd_meshcfg_refused;
extern volatile int32_t g_warthog_chipcmd_bssid_status, g_warthog_chipcmd_meshcfg_status;
extern volatile uint32_t g_warthog_chipcmd_beacon_refused, g_warthog_chipcmd_sta_refused[5];
extern volatile int32_t g_warthog_chipcmd_beacon_status, g_warthog_chipcmd_sta_status;
extern volatile uint32_t g_warthog_chipcmd_key_refused, g_warthog_chipcmd_meshcfg_mode;
extern volatile int32_t g_warthog_chipcmd_key_status;

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static const uint8_t W[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 }; /* us */
static const uint8_t A[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0a };
static const uint8_t B[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0b };
static const uint8_t BC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t OWN_MGTK[16] = { 0x6d, 0x67, 0x74, 0x6b };

enum { STA = MORSE_CMD_INTERFACE_TYPE_STA, MESH = MORSE_CMD_INTERFACE_TYPE_MESH };

static void reset_counters_(void)
{
    g_warthog_chipvif_type = 0;
    g_warthog_chipvif_id = 0;
    g_warthog_chipvif_fallback = 0;
    g_warthog_chipvif_add_status = 0;
    g_warthog_chipcmd_bssid_refused = 0;
    g_warthog_chipcmd_bssid_status = 0;
    g_warthog_chipcmd_meshcfg_refused = 0;
    g_warthog_chipcmd_meshcfg_status = 0;
    g_warthog_chipcmd_beacon_refused = 0;
    g_warthog_chipcmd_beacon_status = 0;
    for (unsigned i = 0; i < 5; i++) { g_warthog_chipcmd_sta_refused[i] = 0; }
    g_warthog_chipcmd_sta_status = 0;
    g_warthog_chipcmd_key_refused = 0;
    g_warthog_chipcmd_key_status = 0;
    g_warthog_chipcmd_meshcfg_mode = 0;
}

/* A clean start: empty chip log, zeroed counters, VIF 0, no boot scan. */
static bool start_(bool scan)
{
    simnode_chipcmd_clear();
    reset_counters_();
    simnode_set_boot_scan(scan);
    return simnode_start_sae(W);
}

/* Peers outlive a stop (the datapath's table is static), and a refusal a case armed for a
 * command the firmware never sent must not reach the next case either. */
static void stop_(void)
{
    simnode_del_peer(NULL);
    simnode_stop();
    simnode_chip_refusals_clear();
}

static unsigned chip_vif_type_(void)
{
    return umac_interface_get_chip_vif_type(umac_data_get_umacd());
}

/* The log's commands with @p id, in order, into @p out (at most @p max); returns how many. */
static unsigned cmds_(uint16_t id, const struct simnode_chipcmd **out, unsigned max)
{
    unsigned n = 0;
    for (unsigned i = 0; i < simnode_chipcmd_count() && n < max; i++)
    {
        const struct simnode_chipcmd *c = simnode_chipcmd_get(i);
        if (c->id == id) { out[n++] = c; }
    }
    return n;
}

/* The order of the interface commands in the log, as "A1 R A5 C" (Add type, Remove, Caps). */
static void if_sequence_(char *buf, size_t len)
{
    size_t off = 0;
    buf[0] = '\0';
    for (unsigned i = 0; i < simnode_chipcmd_count(); i++)
    {
        const struct simnode_chipcmd *c = simnode_chipcmd_get(i);
        int n = 0;
        if (c->id == MORSE_CMD_ID_ADD_INTERFACE)
        {
            n = snprintf(buf + off, len - off, "%sA%u%s", off ? " " : "", (unsigned)c->arg,
                         (c->status != 0 || c->ret != 0) ? "!" : "");
        }
        else if (c->id == MORSE_CMD_ID_REMOVE_INTERFACE)
        {
            n = snprintf(buf + off, len - off, "%sR%s", off ? " " : "", c->ret != 0 ? "!" : "");
        }
        else if (c->id == MORSE_CMD_ID_GET_CAPABILITIES)
        {
            n = snprintf(buf + off, len - off, "%sC", off ? " " : "");
        }
        if (n > 0 && off + (size_t)n < len) { off += (size_t)n; }
    }
}

/* The mesh start's BSS commands in the log, in order, as "BB BS BC MC" (BSS_BEACON_CONFIG,
 * BSSID_SET, BSS_CONFIG, MESH_CONFIG), each marked ! when the chip refused it. */
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
            int n = snprintf(buf + off, len - off, "%s%s%s", off ? " " : "", tags[t].tag,
                             c->status != 0 ? "!" : "");
            if (n > 0 && off + (size_t)n < len) { off += (size_t)n; }
        }
    }
}

/* The SET_STA_STATE states the chip saw for @p mac, in order, as digits. */
static void sta_states_(const uint8_t mac[6], char *buf, size_t len)
{
    size_t off = 0;
    buf[0] = '\0';
    for (unsigned i = 0; i < simnode_chipcmd_count() && off + 2 < len; i++)
    {
        const struct simnode_chipcmd *c = simnode_chipcmd_get(i);
        if (c->id == MORSE_CMD_ID_SET_STA_STATE && memcmp(c->addr, mac, 6) == 0)
        {
            buf[off++] = (char)('0' + c->arg);
            buf[off] = '\0';
        }
    }
}

/* AMPE keys peer A (an SAE peer carries no data before), then the host sends it a frame:
 * true if it reached the chip tagged VIF @p want_vif. */
static bool host_tx_vif_(uint8_t want_vif)
{
    static const uint8_t payload[32] = { 0x45 };
    static const uint8_t mtk[16] = { 0x11, 0x22, 0x33 };
    if (simnode_set_key(A, mtk, 0, /*pairwise=*/true) != 0) { return false; }
    simnode_outbox_clear();
    if (!simnode_host_tx(A, W, payload, sizeof(payload))) { return false; }
    const struct simnode_frame *f = simnode_outbox_get(0);
    return f != NULL && f->vif_id == want_vif;
}

/* The VIF the beacon template the chip asks for is tagged with, or -1 with none. */
static int beacon_vif_(void)
{
    struct mmpkt *b = umac_mesh_get_beacon(umac_data_get_umacd());
    if (b == NULL) { return -1; }
    const int vif = mmdrv_get_tx_metadata(b)->vif_id;
    mmpkt_release(b);
    return vif;
}

/* The mesh's own VIF id, as umac_interface_add handed it to umac_mesh. */
static int mesh_vif_(void)
{
    return umac_interface_get_vif_id(umac_data_get_umacd(), UMAC_INTERFACE_MESH);
}

/* An own MGTK the chip refuses (INSTALL_KEY at aid 0) is counted with its status, not taken
 * as installed, and goes in at the next peer. */
static void own_mgtk_refused_(const char *n)
{
    CHECK(start_(false), "(%s) the mesh starts", n);
    (void)simnode_set_key(BC, OWN_MGTK, 1, /*pairwise=*/false);
    simnode_keyinst_clear();
    simnode_chip_refuse_next_arg(MORSE_CMD_ID_INSTALL_KEY, 0, -95);
    CHECK(simnode_add_peer(A), "(%s) the first peer comes up though the chip refused our MGTK", n);
    CHECK(simnode_keyinst_count() == 0 && simnode_chip_refusals_armed() == 0,
          "(%s) the refused MGTK is not recorded as installed (%u)", n, simnode_keyinst_count());
    CHECK(g_warthog_chipcmd_key_refused == 1 && g_warthog_chipcmd_key_status == -95,
          "(%s) the refusal is counted with its status (%lu, %ld)", n,
          (unsigned long)g_warthog_chipcmd_key_refused, (long)g_warthog_chipcmd_key_status);
    CHECK(simnode_add_peer(B), "(%s) a second peer", n);
    const struct simnode_keyinst *k = simnode_keyinst_count() == 1 ? simnode_keyinst_get(0) : NULL;
    CHECK(k != NULL && !k->pairwise && k->aid == 0 && k->key_idx == 1 &&
              memcmp(k->key, OWN_MGTK, 16) == 0,
          "(%s) which installs our MGTK, not taken as in the chip before (%u installs)", n,
          simnode_keyinst_count());
    CHECK(host_tx_vif_(0), "(%s) and the node carries data", n);
    stop_();
}

#if WARTHOG_MESH_CHIP_VIF_MESH

static void t_mesh_vif_added(void)
{
    char seq[96];
    CHECK(start_(false), "(1) the mesh starts");
    if_sequence_(seq, sizeof(seq));
    CHECK(strcmp(seq, "A1 C R A5 C") == 0,
          "(1) boot STA VIF, then REMOVE and ADD_INTERFACE(MESH) with its capabilities (%s)", seq);
    const struct simnode_chipcmd *add[4];
    unsigned n = cmds_(MORSE_CMD_ID_ADD_INTERFACE, add, 4);
    CHECK(n == 2 && add[1]->status == 0 && add[1]->ret == 0 && memcmp(add[1]->addr, W, 6) == 0,
          "(1) the chip accepted the MESH VIF, on our own MAC");
    CHECK(chip_vif_type_() == MESH && g_warthog_chipvif_type == MESH,
          "(1) the chip VIF in use is MESH (getter %u, AT %lu)", chip_vif_type_(),
          (unsigned long)g_warthog_chipvif_type);
    CHECK(g_warthog_chipvif_fallback == 0 && g_warthog_chipvif_add_status == 0,
          "(1) no fallback counted");
    CHECK(beacon_vif_() == mesh_vif_(), "(1) the beacon template is tagged for the mesh's VIF (%d, %d)",
          beacon_vif_(), mesh_vif_());
    stop_();

    CHECK(start_(true), "(1) the mesh starts after the boot scan probe");
    if_sequence_(seq, sizeof(seq));
    CHECK(strcmp(seq, "A1 C R A1 R A5 C") == 0,
          "(1) boot STA, scan's STA re-add, then the MESH VIF (%s)", seq);
    CHECK(chip_vif_type_() == MESH, "(1) and the mesh runs on the MESH VIF");
    stop_();
}

static void t_mesh_vif_nonzero_id(void)
{
    char seq[96];
    simnode_chip_set_mesh_vif_id(2);
    simnode_outbox_clear();
    CHECK(start_(false), "(2) the mesh starts with the chip handing the MESH VIF id 2");
    if_sequence_(seq, sizeof(seq));
    CHECK(strcmp(seq, "A1 C R A5 R A1") == 0,
          "(2) the MESH VIF is removed again and a STA VIF added (%s)", seq);
    const struct simnode_chipcmd *rm[4];
    CHECK(cmds_(MORSE_CMD_ID_REMOVE_INTERFACE, rm, 4) == 2 && rm[1]->vif_id == 2,
          "(2) the remove is for VIF 2");
    CHECK(chip_vif_type_() == STA && g_warthog_chipvif_fallback == 1 &&
              g_warthog_chipvif_add_status == -1002 && g_warthog_chipvif_id == 0,
          "(2) the STA fallback is counted with add_st -1000 - id (%lu, %ld, VIF %lu)",
          (unsigned long)g_warthog_chipvif_fallback, (long)g_warthog_chipvif_add_status,
          (unsigned long)g_warthog_chipvif_id);
    const struct simnode_frame *probe = simnode_outbox_get(0);
    CHECK(probe != NULL && probe->is_mgmt && probe->vif_id == 0,
          "(2) the start-up probe request goes out on VIF 0");
    const struct simnode_chipcmd *mc[2], *bs[2];
    CHECK(cmds_(MORSE_CMD_ID_MESH_CONFIG, mc, 2) == 1 && mc[0]->vif_id == 0 && mc[0]->beaconing,
          "(2) MESH_CONFIG(START) goes to VIF 0, beaconing as on the STA baseline");
    CHECK(cmds_(MORSE_CMD_ID_BSSID_SET, bs, 2) == 1 && bs[0]->vif_id == 0 && bs[0]->addr[5] == 0x5a,
          "(2) and the STA VIF gets its derived BSSID");
    CHECK(beacon_vif_() == 0 && mesh_vif_() == 0, "(2) the beacon template and the mesh share VIF 0");
    CHECK(simnode_add_peer(A), "(2) a peer is added");
    const struct simnode_chipcmd *ss[4];
    unsigned n = cmds_(MORSE_CMD_ID_SET_STA_STATE, ss, 4);
    CHECK(n == 3 && ss[0]->vif_id == 0 && ss[2]->vif_id == 0 && ss[2]->arg == MORSE_STA_AUTHORIZED,
          "(2) its station walks to AUTHORIZED on VIF 0 (%u commands)", n);
    CHECK(host_tx_vif_(0), "(2) data frames are tagged VIF 0");
    stop_();

    simnode_chip_refuse_next_arg(MORSE_CMD_ID_REMOVE_INTERFACE, 2, -5);
    CHECK(!start_(false), "(2) a remove of the MESH VIF 2 that fails fails the start");
    CHECK(chip_vif_type_() == 0 && g_warthog_chipvif_type == 0 && g_warthog_chipvif_fallback == 0 &&
              g_warthog_chipvif_add_status == -1002,
          "(2) claiming no chip VIF, the id recorded (type %u, fallback %lu, add_st %ld)",
          chip_vif_type_(), (unsigned long)g_warthog_chipvif_fallback,
          (long)g_warthog_chipvif_add_status);
    CHECK(mesh_vif_() == UMAC_INTERFACE_VIF_ID_INVALID, "(2) nor a mesh interface");
    stop_();
    simnode_chip_set_mesh_vif_id(0);
}

static void t_mesh_config_refused(void)
{
    simnode_chip_refuse_next(MORSE_CMD_ID_MESH_CONFIG, -22);
    CHECK(start_(false), "(3) the mesh comes up though the chip refused MESH_CONFIG");
    CHECK(g_warthog_chipcmd_meshcfg_refused == 1 && g_warthog_chipcmd_meshcfg_status == -22,
          "(3) the refusal is counted with its status (%lu, %ld)",
          (unsigned long)g_warthog_chipcmd_meshcfg_refused, (long)g_warthog_chipcmd_meshcfg_status);
    CHECK(simnode_add_peer(A) && host_tx_vif_(0), "(3) and it still carries data");
    stop_();
}

static void t_fallback(void)
{
    char seq[96];
    simnode_chip_refuse_add_if(MESH, -1, 0);
    CHECK(start_(false), "(4) the mesh comes up though the chip refused a MESH VIF");
    if_sequence_(seq, sizeof(seq));
    CHECK(strcmp(seq, "A1 C R A5! A1") == 0, "(4) refused MESH add, then a STA VIF again (%s)", seq);
    CHECK(chip_vif_type_() == STA && g_warthog_chipvif_type == STA,
          "(4) the chip VIF in use is STA (getter %u, AT %lu)", chip_vif_type_(),
          (unsigned long)g_warthog_chipvif_type);
    CHECK(g_warthog_chipvif_fallback == 1 && g_warthog_chipvif_add_status == -1,
          "(4) the fallback is counted with the chip's status (%lu, %ld)",
          (unsigned long)g_warthog_chipvif_fallback, (long)g_warthog_chipvif_add_status);
    const struct simnode_chipcmd *bs[2];
    CHECK(cmds_(MORSE_CMD_ID_BSSID_SET, bs, 2) == 1 && bs[0]->addr[0] == 0x02 && bs[0]->addr[5] == 0x5a,
          "(4) the STA VIF gets the derived BSSID, as before");
    CHECK(simnode_add_peer(A) && host_tx_vif_(0), "(4) and carries data");
    stop_();

    simnode_chip_refuse_add_if(MESH, 0, -110);
    CHECK(start_(false), "(4) the mesh comes up though ADD_INTERFACE(MESH) timed out");
    CHECK(chip_vif_type_() == STA && g_warthog_chipvif_fallback == 1 &&
              g_warthog_chipvif_add_status == -110,
          "(4) STA VIF, the fallback counted with the transport error (%ld)",
          (long)g_warthog_chipvif_add_status);
    stop_();

    simnode_chip_refuse_rm_if(-5);
    CHECK(start_(false), "(4) the mesh comes up though REMOVE_INTERFACE failed");
    if_sequence_(seq, sizeof(seq));
    CHECK(strcmp(seq, "A1 C R!") == 0, "(4) no ADD_INTERFACE after the failed remove (%s)", seq);
    CHECK(chip_vif_type_() == STA && g_warthog_chipvif_fallback == 1 && g_warthog_chipvif_add_status == -5,
          "(4) the boot STA VIF carries the mesh, counted (%ld)", (long)g_warthog_chipvif_add_status);
    stop_();
}

static void t_fallback_fails(void)
{
    /* The MESH add times out, and so does the STA add that falls back from it. */
    simnode_chip_refuse_add_if(MESH, 0, -110);
    simnode_chip_refuse_add_if(0, 0, -110);
    CHECK(!start_(false), "(5) with the fallback failing too the start fails");
    CHECK(chip_vif_type_() == 0 && g_warthog_chipvif_type == 0 && g_warthog_chipvif_fallback == 1,
          "(5) and no chip VIF is claimed (getter %u, AT %lu)", chip_vif_type_(),
          (unsigned long)g_warthog_chipvif_type);
    CHECK(mesh_vif_() == UMAC_INTERFACE_VIF_ID_INVALID, "(5) nor a mesh interface");
    stop_();
}

static void t_peer_leaves(void)
{
    char a[16], b[16];
    CHECK(start_(false), "(6) the mesh starts on a MESH VIF");
    CHECK(simnode_add_peer(A) && simnode_add_peer(B), "(6) two peers");
    simnode_chipcmd_clear();
    simnode_del_peer(A);
    sta_states_(A, a, sizeof(a));
    sta_states_(B, b, sizeof(b));
    CHECK(strcmp(a, "321") == 0, "(6) the leaving peer walks 3 -> 2 -> 1, never NOTEXIST (%s)", a);
    CHECK(b[0] == '\0', "(6) the survivor's station is left alone (%s)", b);
    stop_();

    simnode_chip_refuse_add_if(MESH, -1, 0);
    CHECK(start_(false), "(6) on the fallback STA VIF");
    CHECK(simnode_add_peer(A) && simnode_add_peer(B), "(6) two peers");
    simnode_chipcmd_clear();
    simnode_del_peer(A);
    sta_states_(A, a, sizeof(a));
    sta_states_(B, b, sizeof(b));
    CHECK(strcmp(a, "0") == 0 && strcmp(b, "234") == 0,
          "(6) NOTEXIST and the survivor re-walked, as before (%s / %s)", a, b);
    stop_();
}

static void t_bss_commands(void)
{
    char seq[64];
    CHECK(start_(false), "(7) the mesh starts on a MESH VIF");
    bss_sequence_(seq, sizeof(seq));
    CHECK(strcmp(seq, "BC MC") == 0,
          "(7) BSS_CONFIG, then MESH_CONFIG(START): no BSS_BEACON_CONFIG, no BSSID_SET (%s)", seq);
    CHECK(simnode_chipcmd_n(MORSE_CMD_ID_BSS_BEACON_CONFIG, UINT32_MAX) == 0 &&
              g_warthog_chipcmd_beacon_refused == 0,
          "(7) nothing reached the new VIF before its first BSS_CONFIG");
    stop_();

    simnode_chip_refuse_add_if(MESH, -1, 0);
    simnode_chip_refuse_next(MORSE_CMD_ID_BSS_BEACON_CONFIG, -7);
    CHECK(start_(false), "(7) on the fallback STA VIF");
    bss_sequence_(seq, sizeof(seq));
    CHECK(strcmp(seq, "BB! BS BC MC") == 0, "(7) the STA VIF's order stands (%s)", seq);
    CHECK(g_warthog_chipcmd_beacon_refused == 1 && g_warthog_chipcmd_beacon_status == -7,
          "(7) and its refused BSS_BEACON_CONFIG is counted (%lu, %ld)",
          (unsigned long)g_warthog_chipcmd_beacon_refused, (long)g_warthog_chipcmd_beacon_status);
    stop_();
}

static void t_mesh_config_bytes(void)
{
    const struct simnode_chipcmd *mc[2];
    CHECK(start_(false), "(8) the mesh starts on a MESH VIF");
    const unsigned n = cmds_(MORSE_CMD_ID_MESH_CONFIG, mc, 2);
    CHECK(n == 1 && mc[0]->arg == 1u && !mc[0]->beaconing && mc[0]->vif_id == (uint16_t)mesh_vif_(),
          "(8) one MESH_CONFIG(START) to the MESH VIF, beaconless, as the OpenMANET Pis send (%u sent%s)",
          n, n == 1 && mc[0]->beaconing ? ", beaconing" : "");
    CHECK(g_warthog_chipcmd_meshcfg_mode == 2u,
          "(8) AT+MESHCFG? reports it beaconless (mode %lu)", (unsigned long)g_warthog_chipcmd_meshcfg_mode);
    stop_();

    simnode_chip_refuse_add_if(MESH, -1, 0);
    CHECK(start_(false), "(8) on the fallback STA VIF");
    CHECK(cmds_(MORSE_CMD_ID_MESH_CONFIG, mc, 2) == 1 && mc[0]->beaconing &&
              g_warthog_chipcmd_meshcfg_mode == 1,
          "(8) MESH_CONFIG(START) beacons, as the STA baseline");
    stop_();
}

static void t_sta_state_refused(void)
{
    CHECK(start_(false), "(9) the mesh starts on a MESH VIF");
    simnode_chip_refuse_next_arg(MORSE_CMD_ID_SET_STA_STATE, MORSE_STA_AUTHENTICATED, -3);
    CHECK(simnode_add_peer(A) && simnode_add_peer(B), "(9) two peers, the first's AUTHENTICATED refused");
    CHECK(g_warthog_chipcmd_sta_refused[MORSE_STA_AUTHENTICATED] == 1 &&
              g_warthog_chipcmd_sta_status == -3,
          "(9) counted under AUTHENTICATED with its status (%lu, %ld)",
          (unsigned long)g_warthog_chipcmd_sta_refused[MORSE_STA_AUTHENTICATED],
          (long)g_warthog_chipcmd_sta_status);
    simnode_chip_refuse_next_arg(MORSE_CMD_ID_SET_STA_STATE, MORSE_STA_NONE, -4);
    simnode_del_peer(B);
    CHECK(g_warthog_chipcmd_sta_refused[MORSE_STA_NONE] == 1 && g_warthog_chipcmd_sta_status == -4,
          "(9) a refused walk-down to NONE is counted under NONE (%lu, %ld)",
          (unsigned long)g_warthog_chipcmd_sta_refused[MORSE_STA_NONE],
          (long)g_warthog_chipcmd_sta_status);
    CHECK(host_tx_vif_(0), "(9) and the node carries data");
    stop_();
}

#else /* !WARTHOG_MESH_CHIP_VIF_MESH */

static void t_baseline_vif(void)
{
    char seq[96];
    CHECK(start_(false), "(11) the mesh starts");
    if_sequence_(seq, sizeof(seq));
    CHECK(strcmp(seq, "A1 C") == 0, "(11) the boot STA VIF carries the mesh, nothing re-added (%s)", seq);
    CHECK(chip_vif_type_() == STA && g_warthog_chipvif_type == STA && g_warthog_chipvif_fallback == 0,
          "(11) the chip VIF in use is STA");
    CHECK(beacon_vif_() == 0 && mesh_vif_() == 0, "(11) the beacon template and the mesh share VIF 0");
    stop_();
    CHECK(start_(true), "(11) the mesh starts after the boot scan probe");
    if_sequence_(seq, sizeof(seq));
    CHECK(strcmp(seq, "A1 C R A1") == 0, "(11) the scan's STA re-add only (%s)", seq);
    stop_();
}

static void t_baseline_bssid(void)
{
    simnode_chip_refuse_next(MORSE_CMD_ID_BSSID_SET, -13);
    CHECK(start_(false), "(12) the mesh starts though the chip refused BSSID_SET");
    const struct simnode_chipcmd *bs[2];
    CHECK(cmds_(MORSE_CMD_ID_BSSID_SET, bs, 2) == 1 && bs[0]->addr[0] == 0x02 && bs[0]->addr[5] == 0x5a,
          "(12) BSSID_SET carries the derived BSSID");
    CHECK(g_warthog_chipcmd_bssid_refused == 1 && g_warthog_chipcmd_bssid_status == -13,
          "(12) the refusal is counted with its status (%lu, %ld)",
          (unsigned long)g_warthog_chipcmd_bssid_refused, (long)g_warthog_chipcmd_bssid_status);
    stop_();
}

static void t_baseline_peer_leaves(void)
{
    char a[16], b[16];
    CHECK(start_(false), "(13) the mesh starts");
    CHECK(simnode_add_peer(A) && simnode_add_peer(B), "(13) two peers");
    simnode_chipcmd_clear();
    simnode_del_peer(A);
    sta_states_(A, a, sizeof(a));
    sta_states_(B, b, sizeof(b));
    CHECK(strcmp(a, "0") == 0 && strcmp(b, "234") == 0,
          "(13) NOTEXIST, and the survivor re-walked (%s / %s)", a, b);
    stop_();
}

static void t_baseline_mesh_config(void)
{
    simnode_chip_refuse_next(MORSE_CMD_ID_MESH_CONFIG, -22);
    CHECK(start_(false), "(14) the mesh comes up though the chip refused MESH_CONFIG");
    CHECK(g_warthog_chipcmd_meshcfg_refused == 1 && g_warthog_chipcmd_meshcfg_status == -22,
          "(14) the refusal is counted with its status");
    const struct simnode_chipcmd *mc[2];
    CHECK(cmds_(MORSE_CMD_ID_MESH_CONFIG, mc, 2) == 1 && mc[0]->beaconing &&
              g_warthog_chipcmd_meshcfg_mode == 1,
          "(14) MESH_CONFIG(START) beacons (mode %lu)", (unsigned long)g_warthog_chipcmd_meshcfg_mode);
    stop_();
}

static void t_baseline_bss_beacon(void)
{
    char seq[64];
    simnode_chip_refuse_next(MORSE_CMD_ID_BSS_BEACON_CONFIG, -7);
    CHECK(start_(false), "(15) the mesh starts though the chip refused BSS_BEACON_CONFIG");
    bss_sequence_(seq, sizeof(seq));
    CHECK(strcmp(seq, "BB! BS BC MC") == 0, "(15) the STA VIF's order is unchanged (%s)", seq);
    CHECK(g_warthog_chipcmd_beacon_refused == 1 && g_warthog_chipcmd_beacon_status == -7,
          "(15) the refusal is counted with its status (%lu, %ld)",
          (unsigned long)g_warthog_chipcmd_beacon_refused, (long)g_warthog_chipcmd_beacon_status);
    stop_();
}

static void t_baseline_sta_state_refused(void)
{
    CHECK(start_(false), "(16) the mesh starts");
    CHECK(simnode_add_peer(A) && simnode_add_peer(B), "(16) two peers");
    simnode_chip_refuse_next_arg(MORSE_CMD_ID_SET_STA_STATE, MORSE_STA_NOTEXIST, -6);
    simnode_chip_refuse_next_arg(MORSE_CMD_ID_SET_STA_STATE, MORSE_STA_AUTHORIZED, -8);
    simnode_del_peer(A);
    CHECK(g_warthog_chipcmd_sta_refused[MORSE_STA_NOTEXIST] == 1 &&
              g_warthog_chipcmd_sta_refused[MORSE_STA_AUTHORIZED] == 1 &&
              g_warthog_chipcmd_sta_status == -8,
          "(16) a refused NOTEXIST and a survivor's refused AUTHORIZED are counted (%lu, %lu, %ld)",
          (unsigned long)g_warthog_chipcmd_sta_refused[MORSE_STA_NOTEXIST],
          (unsigned long)g_warthog_chipcmd_sta_refused[MORSE_STA_AUTHORIZED],
          (long)g_warthog_chipcmd_sta_status);
    stop_();
}

#endif

int main(void)
{
    printf("=== simnode: the mesh's chip VIF type (WARTHOG_MESH_CHIP_VIF_MESH=%d) ===\n",
           (int)WARTHOG_MESH_CHIP_VIF_MESH);
#if WARTHOG_MESH_CHIP_VIF_MESH
    t_mesh_vif_added();
    t_mesh_vif_nonzero_id();
    t_mesh_config_refused();
    t_fallback();
    t_fallback_fails();
    t_peer_leaves();
    t_bss_commands();
    t_mesh_config_bytes();
    t_sta_state_refused();
    own_mgtk_refused_("10");
#else
    t_baseline_vif();
    t_baseline_bssid();
    t_baseline_peer_leaves();
    t_baseline_mesh_config();
    t_baseline_bss_beacon();
    t_baseline_sta_state_refused();
    own_mgtk_refused_("16");
#endif
    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_simnode_meshvif%s: all passed\n", WARTHOG_MESH_CHIP_VIF_MESH ? "" : "_off");
    return 0;
}
