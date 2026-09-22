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

void simnode_set_identity(const uint8_t mac[6], uint16_t vif_id);

/* Not in a header: umac_mesh.c exports it for the probe task. */
void umac_mesh_service_tick(void);

extern volatile uint32_t g_warthog_mesh_fwd, g_warthog_mesh_bridge,
                         g_warthog_mesh_grp, g_warthog_mesh_secure;

static struct umac_data *s_umacd;
static uint8_t s_mac[6];
static bool s_up;

bool simnode_start(const uint8_t mac[6])
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
    args.security_type = MMWLAN_OPEN;
    args.max_peers = 4;
    args.beacon_interval_tu = 100;

    /* The real bring-up: interface add, mesh config, beaconing, and the
     * forwarding glue's own init. */
    if (umac_mesh_enable_mesh(s_umacd, &args) != MMWLAN_SUCCESS) { return false; }
    umac_mesh_fwd_glue_init();
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
    /* The glue advertises the Forwarding capability from the gate, so it has
     * to be re-read after a change, exactly as mmwlan_mesh_enable does. */
    umac_mesh_fwd_glue_init();
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
    if (!s_up || frame == NULL || len == 0) { return false; }
    struct mmpkt *pkt = mmpkt_alloc_on_heap(0, len, sizeof(struct mmdrv_rx_metadata));
    if (pkt == NULL) { return false; }
    struct mmdrv_rx_metadata *md = mmpkt_get_metadata(pkt).rx;
    memset(md, 0, sizeof(*md));
    md->rssi = rssi;
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
