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
#include "umac/mesh/umac_mesh.h"
#include "umac/mesh/umac_mesh_fwd_glue.h"
#include "umac/mesh/umac_mesh_ies.h"

void simnode_set_identity(const uint8_t mac[6], uint16_t vif_id);

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

static bool simnode_start_(const uint8_t mac[6], bool sae);

bool simnode_start(const uint8_t mac[6])
{
    return simnode_start_(mac, false);
}

bool simnode_start_sae(const uint8_t mac[6])
{
    return simnode_start_(mac, true);
}

int simnode_set_key(const uint8_t addr[6], const uint8_t key[16], uint8_t key_id, bool pairwise)
{
    return (int)umac_datapath_mesh_set_peer_key(addr, key, 16, key_id, pairwise);
}

static bool simnode_start_(const uint8_t mac[6], bool sae)
{
    if (s_up) { simnode_stop(); }
    memcpy(s_mac, mac, 6);
    simnode_set_identity(mac, 0);
    umac_data_init();
    s_umacd = umac_data_get_umacd();
    if (s_umacd == NULL) { return false; }

    struct mmwlan_mesh_args args;
    memset(&args, 0, sizeof(args));
    memcpy(args.mesh_id, "simnode", 7);
    args.mesh_id_len = 7;
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

bool simnode_add_peer(const uint8_t mac[6])
{
    return umac_mesh_add_datapath_peer(mac) == MMWLAN_SUCCESS;
}

void simnode_del_peer(const uint8_t mac[6])
{
    umac_datapath_mesh_del_peer(mac);
}

bool simnode_host_tx(const uint8_t da[6], const uint8_t sa[6],
                     const uint8_t *payload, uint16_t payload_len)
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
    simnode_pump(); /* the frame is queued; the event loop is what sends it */
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
    /* The event loop's datapath work. On the firmware the umac core task runs
     * this whenever umac_core_evt_wake() fires; here the test drives it, which
     * is what keeps a run deterministic. Bounded so a firmware bug that never
     * drains cannot hang the suite. */
    if (s_umacd == NULL) { return; }
    for (unsigned i = 0; i < 64u && umac_datapath_process(s_umacd); i++)
    {
    }
}

int simnode_render_paths(char *buf, uint32_t len)
{
    return umac_mesh_fwd_glue_render(buf, len);
}
