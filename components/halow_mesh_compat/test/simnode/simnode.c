/*
 * One simulated warthog node: the glue that lets a test drive the real mesh
 * stack. See simnode.h for what is real and what is not.
 */
#include "simnode.h"

#include <stdio.h>
#include <string.h>

#include "mmpkt.h"
#include "mmwlan.h"
#include "mmwlan_mesh.h"
#include "mmdrv.h"
#include "umac/data/umac_data.h"
#include "umac/datapath/umac_datapath.h"
#include "umac/datapath/umac_datapath_private.h"
#include "umac/datapath/umac_datapath_data.h"
#include "umac/mesh/umac_mesh.h"
#include "umac/mesh/umac_mesh_fwd_glue.h"
#include "umac/mesh/umac_mesh_ies.h"

void simnode_set_identity(const uint8_t mac[6], uint16_t vif_id);
bool simnode_evt_dispatch_one(struct umac_data *umacd); /* fake_app.c */
unsigned simnode_fire_timeouts(void);                   /* fake_app.c */
bool simnode_timeout_next_due(uint32_t *due);           /* fake_app.c */
void simnode_loop_enter(void);                          /* fake_app.c */
void simnode_loop_leave(void);                          /* fake_app.c */

/* Not in a header: umac_mesh.c exports it for the probe task. */
void umac_mesh_service_tick(void);

extern volatile uint32_t g_warthog_mesh_fwd, g_warthog_mesh_bridge,
                         g_warthog_mesh_grp, g_warthog_mesh_secure;

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

    /* The real bring-up: interface add, mesh config, beaconing, and the
     * forwarding glue's own init. */
    if (umac_mesh_enable_mesh(s_umacd, &args) != MMWLAN_SUCCESS) { return false; }
    umac_mesh_fwd_glue_init();
    (void)umac_datapath_register_rx_cb(s_umacd, simnode_netif_rx_, NULL);
    s_hostrx_n = 0;
    s_up = true;
    return true;
}

void simnode_stop(void)
{
    if (s_up && s_umacd != NULL) { (void)umac_mesh_disable_mesh(s_umacd); }
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

static bool simnode_host_tx_(const uint8_t da[6], const uint8_t sa[6],
                             const uint8_t *payload, uint16_t payload_len, bool pump);

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

static bool simnode_host_tx_(const uint8_t da[6], const uint8_t sa[6],
                             const uint8_t *payload, uint16_t payload_len, bool pump)
{
    if (!s_up) { return false; }
    /* An 802.3 frame, the shape the netif hands down. */
    struct mmpkt *pkt = umac_datapath_alloc_mmpkt_for_qos_data_tx(
        (uint32_t)payload_len + sizeof(struct umac_8023_hdr), MMDRV_PKT_CLASS_DATA_TID0);
    if (pkt == NULL) { return false; }
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

bool simnode_rx_flags(const uint8_t *frame, uint16_t len, int16_t rssi, uint8_t rx_flags)
{
    if (!s_up || frame == NULL || len == 0) { return false; }
    struct mmpkt *pkt = mmpkt_alloc_on_heap(0, len, sizeof(struct mmdrv_rx_metadata));
    if (pkt == NULL) { return false; }
    struct mmdrv_rx_metadata *md = mmpkt_get_metadata(pkt).rx;
    memset(md, 0, sizeof(*md));
    md->rssi = rssi;
    md->flags = rx_flags;
    struct mmpktview *v = mmpkt_open(pkt);
    mmpkt_append_data(v, frame, len);
    mmpkt_close(&v);
    umac_datapath_rx_frame(s_umacd, pkt);
    simnode_pump();
    return true;
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
