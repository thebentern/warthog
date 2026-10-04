/*
 * One simulated warthog node: the glue that lets a test drive the real mesh
 * stack. See simnode.h for what is real and what is not.
 */
#include "simnode.h"

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "mmpkt.h"
#include "mmwlan.h"
#include "mmwlan_mesh.h"
#include "mmdrv.h"
#include "umac/data/umac_data.h"
#include "umac/data/umac_data_private.h" /* the station record's size, for simnode_recycle_peer_record */
#include "umac/datapath/umac_datapath.h"
#include "umac/datapath/umac_datapath_private.h"
#include "umac/datapath/umac_datapath_data.h"
#include "umac/mesh/umac_mesh.h"
#include "umac/mesh/umac_mesh_fwd_glue.h"
#include "umac/mesh/umac_mesh_ies.h"
#include "umac/interface/umac_interface.h"
#include "umac/rc/umac_rc.h"

void simnode_set_identity(const uint8_t mac[6], uint16_t vif_id);
bool simnode_evt_dispatch_one(struct umac_data *umacd); /* fake_app.c */
unsigned simnode_fire_timeouts(void);                   /* fake_app.c */
bool simnode_timeout_next_due(uint32_t *due);           /* fake_app.c */
void simnode_loop_enter(void);                          /* fake_app.c */
void simnode_loop_leave(void);                          /* fake_app.c */

/* Not in a header: umac_mesh.c exports it for the probe task. */
void umac_mesh_service_tick(void);

extern volatile uint32_t g_warthog_mesh_fwd, g_warthog_mesh_bridge,
                         g_warthog_mesh_grp, g_warthog_mesh_secure, g_warthog_mesh_batman;

static struct umac_data *s_umacd;
static uint8_t s_mac[6];
static bool s_up;

/* ---- the host netif, captured ------------------------------------------
 *
 * A real node has a network stack registered; without one the datapath drops
 * every validated mesh frame one line before delivery ("No RX callback
 * registered"). Registering the real callback is both more faithful and the
 * only way to assert that a node's APPLICATION received something, rather
 * than that bytes arrived on its air interface. */
#define SIMNODE_HOSTRX_MAX 32u
static struct simnode_hostrx s_hostrx[SIMNODE_HOSTRX_MAX];
static unsigned s_hostrx_n;

unsigned simnode_host_rx_count(void) { return s_hostrx_n; }
void simnode_host_rx_clear(void) { s_hostrx_n = 0; }
const struct simnode_hostrx *simnode_host_rx_get(unsigned i)
{
    return (i < s_hostrx_n) ? &s_hostrx[i] : NULL;
}

static void simnode_netif_rx_(uint8_t *header, unsigned header_len,
                              uint8_t *payload, unsigned payload_len, void *arg)
{
    (void)arg;
    if (s_hostrx_n >= SIMNODE_HOSTRX_MAX) { return; }
    struct simnode_hostrx *e = &s_hostrx[s_hostrx_n++];
    memset(e, 0, sizeof(*e));
    if (header != NULL && header_len >= sizeof(struct umac_8023_hdr))
    {
        const struct umac_8023_hdr *h = (const struct umac_8023_hdr *)header;
        memcpy(e->da, h->dest_addr, 6);
        memcpy(e->sa, h->src_addr, 6);
    }
    unsigned n = (payload_len < sizeof(e->payload)) ? payload_len : (unsigned)sizeof(e->payload);
    if (payload != NULL) { memcpy(e->payload, payload, n); }
    e->len = (uint16_t)payload_len;
}

static bool simnode_start_(const uint8_t mac[6], bool sae, uint16_t vif_id);

/* ---- the extended RX callback --------------------------------------------
 *
 * The radio-stack stub generator used to supply these two as no-ops, which made
 * the ext path unreachable. Here they keep one callback per VIF as umac_interface.c
 * does, UNSPECIFIED meaning both: the datapath's register calls (raw and pkt clear it,
 * ext sets it) and its per-frame lookup, for the VIF it names, go through them. */
static struct { mmwlan_rx_pkt_ext_cb_t cb; void *arg; } s_ext[2]; /* STA, AP */
static enum mmwlan_vif s_ext_vif;
static bool s_ext_on;

enum mmwlan_status umac_interface_register_rx_pkt_ext_cb(struct umac_data *umacd,
                                                         enum mmwlan_vif vif,
                                                         mmwlan_rx_pkt_ext_cb_t callback,
                                                         void *arg)
{
    if (vif == MMWLAN_VIF_UNSPECIFIED)
    {
        (void)umac_interface_register_rx_pkt_ext_cb(umacd, MMWLAN_VIF_STA, callback, arg);
        return umac_interface_register_rx_pkt_ext_cb(umacd, MMWLAN_VIF_AP, callback, arg);
    }
    if (vif != MMWLAN_VIF_STA && vif != MMWLAN_VIF_AP) { return MMWLAN_INVALID_ARGUMENT; }
    s_ext[vif == MMWLAN_VIF_AP].cb = callback;
    s_ext[vif == MMWLAN_VIF_AP].arg = arg;
    return MMWLAN_SUCCESS;
}

mmwlan_rx_pkt_ext_cb_t umac_interface_get_rx_pkt_ext_cb(struct umac_data *umacd,
                                                        enum mmwlan_vif vif, void **arg)
{
    (void)umacd;
    const bool known = (vif == MMWLAN_VIF_STA || vif == MMWLAN_VIF_AP);
    if (arg != NULL) { *arg = known ? s_ext[vif == MMWLAN_VIF_AP].arg : NULL; }
    return known ? s_ext[vif == MMWLAN_VIF_AP].cb : NULL;
}

#define SIMNODE_EXTRX_MAX 16u
static struct simnode_extrx s_extrx[SIMNODE_EXTRX_MAX];
static unsigned s_extrx_n;

unsigned simnode_ext_rx_count(void) { return s_extrx_n; }
void simnode_ext_rx_clear(void) { s_extrx_n = 0; }
const struct simnode_extrx *simnode_ext_rx_get(unsigned i)
{
    return (i < s_extrx_n) ? &s_extrx[i] : NULL;
}

/* Consumes the packet, as the callback contract requires. */
static void simnode_ext_rx_(struct mmpkt *pkt, const struct mmwlan_rx_metadata *md, void *arg)
{
    (void)arg;
    struct mmpktview *v = mmpkt_open(pkt);
    if (s_extrx_n < SIMNODE_EXTRX_MAX)
    {
        struct simnode_extrx *e = &s_extrx[s_extrx_n++];
        memset(e, 0, sizeof(*e));
        uint32_t n = mmpkt_get_data_length(v);
        memcpy(e->frame, mmpkt_get_data_start(v), n < sizeof(e->frame) ? n : sizeof(e->frame));
        e->len = (uint16_t)n;
        if (md != NULL && md->ta != NULL)
        {
            memcpy(e->ta, md->ta, 6);
            e->have_ta = true;
        }
    }
    mmpkt_close(&v);
    mmpkt_release(pkt);
}

static void simnode_rx_register_(void)
{
    if (s_ext_on)
    {
        (void)umac_datapath_register_rx_pkt_ext_cb(s_umacd, s_ext_vif, simnode_ext_rx_, NULL);
    }
    else
    {
        (void)umac_datapath_register_rx_cb(s_umacd, simnode_netif_rx_, NULL);
    }
}

void simnode_set_rx_ext_cb_vif(bool on, unsigned vif)
{
    s_ext_on = on;
    s_ext_vif = (enum mmwlan_vif)vif;
    s_extrx_n = 0;
    if (s_umacd != NULL) { simnode_rx_register_(); }
}

void simnode_set_rx_ext_cb(bool on)
{
    simnode_set_rx_ext_cb_vif(on, MMWLAN_VIF_UNSPECIFIED);
}

/* ---- rate control's expected throughput ---------------------------------
 *
 * umac_rc.c is not linked (the radio stack below the mesh is stubbed), so the
 * helper umac_datapath_mesh_peer_links calls is supplied here, settable per peer. */
#define SIMNODE_TPUT_MAX 8u
static struct { uint8_t mac[6]; uint32_t kbps; bool valid, used; } s_tput[SIMNODE_TPUT_MAX];

void simnode_set_peer_tput(const uint8_t mac[6], uint32_t kbps, bool valid)
{
    unsigned free_i = SIMNODE_TPUT_MAX;
    for (unsigned i = 0; i < SIMNODE_TPUT_MAX; i++)
    {
        if (s_tput[i].used && memcmp(s_tput[i].mac, mac, 6) == 0) { free_i = i; break; }
        if (!s_tput[i].used && free_i == SIMNODE_TPUT_MAX) { free_i = i; }
    }
    if (free_i == SIMNODE_TPUT_MAX) { return; }
    memcpy(s_tput[free_i].mac, mac, 6);
    s_tput[free_i].kbps = kbps;
    s_tput[free_i].valid = valid;
    s_tput[free_i].used = true;
}

uint32_t umac_rc_get_expected_tput_kbps(struct umac_sta_data *stad)
{
    if (stad == NULL) { return 0; }
    for (unsigned i = 0; i < SIMNODE_TPUT_MAX; i++)
    {
        if (s_tput[i].used && umac_sta_data_matches_peer_addr(stad, s_tput[i].mac))
        {
            return s_tput[i].valid ? s_tput[i].kbps : 0u;
        }
    }
    return 0;
}

/* ---- rate control's table and feedback -------------------------------------
 *
 * The chain a test sets is what rate control hands each data frame; without one the frame
 * keeps the zeroed table its metadata starts with, as the generated stub left it. Feedback
 * is recorded per call, in the order the chip reported. */
static struct simnode_rate s_chain[4];
static unsigned s_chain_n;
static unsigned s_rc_table_calls;
static uint32_t s_rc_last_size;
#define SIMNODE_RCFB_MAX 64u
static struct simnode_rcfb s_rcfb[SIMNODE_RCFB_MAX];
static unsigned s_rcfb_n;

void simnode_set_rate_chain(const struct simnode_rate *chain, unsigned n)
{
    s_chain_n = (chain != NULL && n <= 4u) ? n : 0u;
    if (s_chain_n != 0u) { memcpy(s_chain, chain, s_chain_n * sizeof(s_chain[0])); }
}

unsigned simnode_rc_table_calls(void) { return s_rc_table_calls; }
uint32_t simnode_rc_last_size(void) { return s_rc_last_size; }
unsigned simnode_rcfb_count(void) { return s_rcfb_n; }
const struct simnode_rcfb *simnode_rcfb_get(unsigned i) { return i < s_rcfb_n ? &s_rcfb[i] : NULL; }
void simnode_rc_clear(void) { s_rc_table_calls = 0; s_rc_last_size = 0; s_rcfb_n = 0; }

static uint8_t rc_bw_enum_(uint8_t mhz)
{
    return mhz >= 16u ? MMRC_BW_16MHZ : mhz >= 8u ? MMRC_BW_8MHZ : mhz >= 4u ? MMRC_BW_4MHZ
         : mhz >= 2u ? MMRC_BW_2MHZ : MMRC_BW_1MHZ;
}

void umac_rc_init_rate_table_data(struct umac_sta_data *stad, struct mmrc_rate_table *table,
                                  bool rts_required, uint32_t frame_size)
{
    (void)stad;
    s_rc_table_calls++;
    s_rc_last_size = frame_size;
    if (s_chain_n == 0u || table == NULL) { return; }
    memset(table, 0, sizeof(*table));
    for (unsigned i = 0; i < MMRC_MAX_CHAIN_LENGTH; i++)
    {
        struct mmrc_rate *r = &table->rates[i];
        if (i >= s_chain_n)
        {
            r->rate = MMRC_MCS_UNUSED;
            continue;
        }
        r->rate = s_chain[i].mcs;
        r->bw = rc_bw_enum_(s_chain[i].bw_mhz);
        r->attempts = s_chain[i].attempts;
        r->flags = (rts_required || i != 0u) ? MMRC_MASK(MMRC_FLAGS_CTS_RTS) : 0u;
    }
}

static uint8_t s_mgmt_bw;
void simnode_set_mgmt_rate_bw(uint8_t mhz) { s_mgmt_bw = (mhz == 1u || mhz == 2u) ? mhz : 0u; }

void umac_rc_init_rate_table_mgmt(struct umac_data *umacd, struct mmrc_rate_table *table, bool rts_required)
{
    (void)umacd;
    if (s_mgmt_bw == 0u || table == NULL) { return; }
    memset(table, 0, sizeof(*table));
    table->rates[0].attempts = 5;
    table->rates[0].rate = MMRC_MCS0;
    table->rates[0].bw = rc_bw_enum_(s_mgmt_bw);
    table->rates[0].flags = rts_required ? MMRC_MASK(MMRC_FLAGS_CTS_RTS) : 0u;
    for (unsigned i = 1; i < MMRC_MAX_CHAIN_LENGTH; i++) { table->rates[i].rate = MMRC_MCS_UNUSED; }
}

void umac_rc_feedback(struct umac_sta_data *stad, struct mmdrv_tx_metadata *tx_metadata)
{
    if (tx_metadata == NULL || s_rcfb_n >= SIMNODE_RCFB_MAX) { return; }
    struct simnode_rcfb *e = &s_rcfb[s_rcfb_n++];
    memset(e, 0, sizeof(*e));
    e->aid = umac_sta_data_get_aid(stad);
    e->attempts = tx_metadata->attempts;
    e->status_flags = tx_metadata->status_flags;
    for (unsigned i = 0; i < 4u; i++)
    {
        const struct mmrc_rate *r = &tx_metadata->rc_data.rates[i];
        if (r->rate == MMRC_MCS_UNUSED || r->attempts == 0u) { continue; }
        e->chain[i].bw_mhz = (uint8_t)(1u << r->bw);
        e->chain[i].mcs = r->rate;
        e->chain[i].attempts = r->attempts;
    }
}

int simnode_peer_links_query(struct mmwlan_mesh_peer_link *out, uint8_t max, uint8_t *count,
                             bool on_loop)
{
    if (on_loop) { simnode_loop_enter(); }
    int st = (int)umac_mesh_peer_links_snapshot(s_umacd, out, max, count);
    if (on_loop) { simnode_loop_leave(); }
    return st;
}

uint8_t simnode_peer_links(struct mmwlan_mesh_peer_link *out, uint8_t max)
{
    uint8_t n = 0;
    return simnode_peer_links_query(out, max, &n, true) == (int)MMWLAN_SUCCESS ? n : 0u;
}

void simnode_set_batman(bool on)
{
    g_warthog_mesh_batman = on ? 1u : 0u;
}

bool simnode_start(const uint8_t mac[6])
{
    return simnode_start_(mac, false, 0);
}

bool simnode_start_sae(const uint8_t mac[6])
{
    return simnode_start_(mac, true, 0);
}

bool simnode_start_sae_vif(const uint8_t mac[6], uint16_t vif_id)
{
    return simnode_start_(mac, true, vif_id);
}

int simnode_set_key(const uint8_t addr[6], const uint8_t key[16], uint8_t key_id, bool pairwise)
{
    return simnode_set_key_rsc(addr, key, key_id, pairwise, NULL);
}

/* hostap's driver ops run on the umac event loop. */
int simnode_set_key_rsc(const uint8_t addr[6], const uint8_t key[16], uint8_t key_id,
                        bool pairwise, const uint8_t rsc[6])
{
    simnode_loop_enter();
    int st = (int)umac_datapath_mesh_set_peer_key(addr, key, 16, key_id, pairwise, rsc,
                                                  rsc != NULL ? 6u : 0u);
    simnode_loop_leave();
    return st;
}

int simnode_own_group_rsc(uint8_t key_id, uint8_t rsc[6])
{
    return (int)umac_datapath_mesh_own_group_rsc(key_id, rsc);
}

int simnode_set_igtk(const uint8_t addr[6], const uint8_t key[16], uint16_t key_id,
                     const uint8_t rsc[6])
{
    simnode_loop_enter();
    int st = (int)umac_datapath_mesh_set_igtk(addr, key, key != NULL ? 16u : 0u, key_id, rsc,
                                              rsc != NULL ? 6u : 0u);
    simnode_loop_leave();
    return st;
}

static uint8_t s_mesh_id[MMWLAN_MESH_ID_MAXLEN] = "simnode";
static uint8_t s_mesh_id_len = 7;

void simnode_set_mesh_id(const uint8_t *id, uint8_t len)
{
    const bool ok = id != NULL && len != 0 && len <= sizeof(s_mesh_id);
    memcpy(s_mesh_id, ok ? id : (const uint8_t *)"simnode", ok ? len : 7u);
    s_mesh_id_len = ok ? len : 7u;
}

static bool s_boot_scan;
void simnode_set_boot_scan(bool on) { s_boot_scan = on; }

static bool simnode_start_(const uint8_t mac[6], bool sae, uint16_t vif_id)
{
    if (s_up) { simnode_stop(); }
    memcpy(s_mac, mac, 6);
    simnode_set_identity(mac, vif_id);
    umac_data_init();
    s_umacd = umac_data_get_umacd();
    if (s_umacd == NULL) { return false; }

    struct mmwlan_mesh_args args;
    memset(&args, 0, sizeof(args));
    memcpy(args.mesh_id, s_mesh_id, s_mesh_id_len);
    args.mesh_id_len = s_mesh_id_len;
    args.security_type = sae ? MMWLAN_SAE : MMWLAN_OPEN;
    if (sae)
    {
        memcpy(args.passphrase, "simnode-pass", 12);
        args.passphrase_len = 12;
    }
    args.max_peers = 4;
    args.beacon_interval_tu = 100;

    /* The board boots the chip before the mesh exists (mmhalow_init -> mmwlan_boot adds a
     * NONE interface), and every env but -swccmp-on then runs the scan probe. */
    if (umac_interface_add(s_umacd, UMAC_INTERFACE_NONE, NULL, NULL) != MMWLAN_SUCCESS)
    {
        return false;
    }
    if (s_boot_scan)
    {
        uint16_t scan_vif = UMAC_INTERFACE_VIF_ID_INVALID;
        if (umac_interface_add(s_umacd, UMAC_INTERFACE_SCAN, NULL, &scan_vif) != MMWLAN_SUCCESS)
        {
            return false;
        }
        umac_interface_remove(s_umacd, UMAC_INTERFACE_SCAN);
    }

    /* The real bring-up: interface add, mesh config, beaconing, and the
     * forwarding glue's own init. */
    if (umac_mesh_enable_mesh(s_umacd, &args) != MMWLAN_SUCCESS) { return false; }
    umac_mesh_fwd_glue_init();
    simnode_rx_register_();
    s_hostrx_n = 0;
    s_extrx_n = 0;
    s_up = true;
    return true;
}

void simnode_stop(void)
{
    if (s_up && s_umacd != NULL)
    {
        /* The loop takes the TX statuses already queued for it (a PERR sent by del_peer, under
         * our MGTK); the next start's umac_data_init would drop them unreleased. */
        struct umac_datapath_data *dp = umac_data_get_datapath(s_umacd);
        simnode_loop_enter();
        for (unsigned i = 0; i < 64u && !mmpkt_list_is_empty(&dp->tx_status_q); i++)
        {
            (void)umac_datapath_process(s_umacd);
        }
        simnode_loop_leave();
        (void)umac_mesh_disable_mesh(s_umacd);
    }
    s_up = false;
    s_umacd = NULL;
    simnode_outbox_clear();
}

void simnode_set_gates(bool fwd, bool bridge, bool grp_std, bool secure)
{
    g_warthog_mesh_fwd = fwd ? 1u : 0u;
    g_warthog_mesh_bridge = bridge ? 1u : 0u;
    g_warthog_mesh_grp = grp_std ? 1u : 0u;
    g_warthog_mesh_secure = secure ? 1u : 0u;
    /* Only the advertised capability is derived from a gate; everything else
     * reads g_warthog_mesh_fwd live, every frame.
     *
     * Deliberately NOT umac_mesh_fwd_glue_init() here. On the device the AT
     * commands only write NVS ("takes effect on next boot"), the gates load
     * once at boot, and glue_init runs once after that -- so a device can
     * never clear the duplicate ring, the two rate gates, the protection
     * latch, the path table and the held-frame store by changing a gate. A
     * harness that did would hide any bug where that state wrongly survives
     * (or wrongly fails to survive), and would drop held packets without
     * releasing them, corrupting the leak oracle. */
    umac_mesh_ies_cap_forwarding = fwd ? 1u : 0u;
}

/* Peers come and go on the umac event loop: hostap's sta_add/sta_remove and the MPM. */
bool simnode_add_peer(const uint8_t mac[6])
{
    simnode_loop_enter();
    bool ok = umac_mesh_add_datapath_peer(mac) == MMWLAN_SUCCESS;
    simnode_loop_leave();
    return ok;
}

void simnode_del_peer(const uint8_t mac[6])
{
    simnode_loop_enter();
    umac_datapath_mesh_del_peer(mac);
    simnode_loop_leave();
}

void simnode_recycle_arm(const void *block, size_t size); /* fake_rtos.c */

bool simnode_recycle_peer_record(const uint8_t *mac)
{
    if (mac == NULL)
    {
        simnode_recycle_arm(NULL, 0);
        return true;
    }
    simnode_loop_enter();
    const struct umac_sta_data *stad = umac_datapath_mesh_find_peer(mac);
    simnode_loop_leave();
    if (stad == NULL) { return false; }
    simnode_recycle_arm(stad, sizeof(*stad));
    return true;
}

static bool simnode_host_tx_(const uint8_t da[6], const uint8_t sa[6],
                             const uint8_t *payload, uint16_t payload_len, bool pump);
static bool simnode_host_tx_on_(const uint8_t da[6], const uint8_t sa[6],
                                const uint8_t *payload, uint16_t payload_len, bool pump,
                                uint8_t tid);

int simnode_host_tx_eth(const uint8_t *ra, const uint8_t da[6], const uint8_t sa[6],
                        uint16_t ethertype, const uint8_t *payload, uint16_t payload_len)
{
    if (!s_up) { return (int)MMWLAN_NOT_RUNNING; }
    struct mmpkt *pkt = umac_datapath_alloc_mmpkt_for_qos_data_tx(
        (uint32_t)payload_len + sizeof(struct umac_8023_hdr), MMDRV_PKT_CLASS_DATA_TID0);
    if (pkt == NULL) { return (int)MMWLAN_NO_MEM; }
    struct umac_8023_hdr h;
    memcpy(h.dest_addr, da, 6);
    memcpy(h.src_addr, sa, 6);
    h.ethertype_be = htobe16(ethertype);
    struct mmpktview *v = mmpkt_open(pkt);
    mmpkt_append_data(v, (const uint8_t *)&h, sizeof(h));
    if (payload != NULL && payload_len != 0) { mmpkt_append_data(v, payload, payload_len); }
    mmpkt_close(&v);
    int st = (int)umac_datapath_tx_frame(s_umacd, pkt, ENCRYPTION_ENABLED, ra);
    simnode_pump();
    return st;
}

bool simnode_host_tx(const uint8_t da[6], const uint8_t sa[6],
                     const uint8_t *payload, uint16_t payload_len)
{
    return simnode_host_tx_(da, sa, payload, payload_len, true);
}

bool simnode_host_tx_nopump(const uint8_t da[6], const uint8_t sa[6],
                            const uint8_t *payload, uint16_t payload_len)
{
    return simnode_host_tx_(da, sa, payload, payload_len, false);
}

bool simnode_host_tx_tid(const uint8_t da[6], const uint8_t sa[6],
                         const uint8_t *payload, uint16_t payload_len, uint8_t tid)
{
    return simnode_host_tx_on_(da, sa, payload, payload_len, true, tid);
}

static bool simnode_host_tx_(const uint8_t da[6], const uint8_t sa[6],
                             const uint8_t *payload, uint16_t payload_len, bool pump)
{
    return simnode_host_tx_on_(da, sa, payload, payload_len, pump, 0);
}

static bool simnode_host_tx_on_(const uint8_t da[6], const uint8_t sa[6],
                                const uint8_t *payload, uint16_t payload_len, bool pump,
                                uint8_t tid)
{
    if (!s_up) { return false; }
    /* An 802.3 frame, the shape the netif hands down. */
    struct mmpkt *pkt = umac_datapath_alloc_mmpkt_for_qos_data_tx(
        (uint32_t)payload_len + sizeof(struct umac_8023_hdr), MMDRV_PKT_CLASS_DATA_TID0 + tid);
    if (pkt == NULL) { return false; }
    mmdrv_get_tx_metadata(pkt)->tid = tid; /* as mmwlan_tx_pkt sets it */
    struct umac_8023_hdr h;
    memcpy(h.dest_addr, da, 6);
    memcpy(h.src_addr, sa, 6);
    h.ethertype_be = htobe16(0x0800);
    struct mmpktview *v = mmpkt_open(pkt);
    mmpkt_append_data(v, (const uint8_t *)&h, sizeof(h));
    if (payload != NULL && payload_len != 0) { mmpkt_append_data(v, payload, payload_len); }
    mmpkt_close(&v);
    if (umac_datapath_tx_frame(s_umacd, pkt, ENCRYPTION_ENABLED, NULL) != MMWLAN_SUCCESS)
    {
        return false;
    }
    if (pump)
    {
        simnode_pump(); /* the frame is queued; the event loop is what sends it */
    }
    return true;
}

bool simnode_rx(const uint8_t *frame, uint16_t len, int16_t rssi)
{
    return simnode_rx_flags(frame, len, rssi, 0);
}

extern volatile uint32_t g_warthog_rx_read_seq;

static bool rx_flags_(const uint8_t *frame, uint16_t len, int16_t rssi, uint8_t rx_flags, bool pump)
{
    if (!s_up || frame == NULL || len == 0) { return false; }
    struct mmpkt *pkt = mmpkt_alloc_on_heap(0, len, sizeof(struct mmdrv_rx_metadata));
    if (pkt == NULL) { return false; }
    struct mmdrv_rx_metadata *md = mmpkt_get_metadata(pkt).rx;
    memset(md, 0, sizeof(*md));
    md->rssi = rssi;
    md->flags = rx_flags;
    md->read_timestamp_ms = mmosal_get_time_ms(); /* as pageset.c stamps it; the reorder timeout reads it */
    md->read_seq = ++g_warthog_rx_read_seq;       /* and its read order */
    struct mmpktview *v = mmpkt_open(pkt);
    mmpkt_append_data(v, frame, len);
    mmpkt_close(&v);
    umac_datapath_rx_frame(s_umacd, pkt);
    if (pump)
    {
        simnode_pump();
    }
    return true;
}

bool simnode_rx_flags(const uint8_t *frame, uint16_t len, int16_t rssi, uint8_t rx_flags)
{
    return rx_flags_(frame, len, rssi, rx_flags, true);
}

bool simnode_rx_flags_queued(const uint8_t *frame, uint16_t len, int16_t rssi, uint8_t rx_flags)
{
    return rx_flags_(frame, len, rssi, rx_flags, false);
}

void simnode_tick(void)
{
    /* What the 2 s probe task runs on the firmware. */
    umac_mesh_service_tick();
    simnode_pump();
}

void simnode_pump(void)
{
    /* The event loop: datapath work, then queued events, as evtloop_iteration
     * runs them. On the firmware the umac core task runs this whenever
     * umac_core_evt_wake() fires; here the test drives it, which is what keeps
     * a run deterministic. Bounded so a firmware bug that never drains cannot
     * hang the suite. */
    if (s_umacd == NULL) { return; }
    struct umac_datapath_data *dp = umac_data_get_datapath(s_umacd);
    simnode_loop_enter();
    for (unsigned i = 0; i < 64u; i++)
    {
        bool more = umac_datapath_process(s_umacd);
        /* A frame sent in this pass reports its TX status for the next one to take. */
        more = !mmpkt_list_is_empty(&dp->tx_status_q) || more;
        if (!simnode_evt_dispatch_one(s_umacd) && !more) { break; }
    }
    simnode_loop_leave();
}

unsigned simnode_run_timeouts(void)
{
    unsigned n = simnode_fire_timeouts();
    simnode_pump();
    return n;
}

void simnode_advance_run(uint32_t ms)
{
    const uint32_t end = mmosal_get_time_ms() + ms;
    (void)simnode_run_timeouts();
    for (unsigned guard = 0; guard < 100000u; guard++) /* a timeout that re-arms at 0 must not hang the suite */
    {
        uint32_t due;
        if (!simnode_timeout_next_due(&due) || (int32_t)(due - end) > 0) { break; }
        if ((int32_t)(due - mmosal_get_time_ms()) > 0) { simnode_set_time_ms(due); }
        if (simnode_run_timeouts() == 0) { break; } /* nothing due after all: no spin */
    }
    simnode_set_time_ms(end);
    (void)simnode_run_timeouts();
}

int simnode_render_paths(char *buf, uint32_t len)
{
    return umac_mesh_fwd_glue_render(buf, len);
}

/* driver_health.c's failure path (morse_reset_chip), on the health task. */
void simnode_chip_restart_queued(void)
{
    mmdrv_host_set_tx_paused(MMDRV_PAUSE_SOURCE_MASK_HW_RESTART, true);
    mmdrv_host_hw_restart_required();
}

void simnode_chip_restart(void)
{
    simnode_chip_restart_queued();
    simnode_pump();
}

void simnode_assert_catch(jmp_buf *jb); /* fake_rtos.c */
void simnode_crit_reset(void);          /* fake_rtos.c */
void simnode_loop_reset(void);          /* fake_app.c */

bool simnode_expect_assert(void (*fn)(void))
{
    static jmp_buf jb;
    if (setjmp(jb) == 0)
    {
        simnode_assert_catch(&jb);
        fn();
        simnode_assert_catch(NULL);
        return false;
    }
    /* Left mid-way, as a reset leaves it: no task is in the loop or a critical section. */
    simnode_loop_reset();
    simnode_crit_reset();
    return true;
}
