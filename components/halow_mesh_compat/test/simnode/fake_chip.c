/*
 * The chip, replaced. THIS FILE IS THE PHY CUT LINE.
 *
 * mmdrv_tx_frame() is where the firmware hands a finished 802.11 frame to the
 * MM6108 over SDIO. Here it appends that frame to an outbox instead, so a test
 * can assert on the exact bytes that would have gone on the air -- addresses,
 * QoS control, Mesh Control, Address Extension, the lot.
 *
 * Everything above this line is the real firmware. Nothing below it is
 * simulated at all: no modulation, no timing, no interference, no chip
 * behaviour. What the radio does with these bytes is exactly what the
 * simulator cannot tell you.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mmpkt.h"
#include "mmdrv.h"
#include "simnode.h" /* the one definition of struct simnode_frame the tests read */

#ifndef SIMNODE_OUTBOX_MAX
#define SIMNODE_OUTBOX_MAX 64u
#endif

static struct simnode_frame s_outbox[SIMNODE_OUTBOX_MAX];
static unsigned s_outbox_n;
static unsigned s_outbox_dropped;

unsigned simnode_outbox_count(void) { return s_outbox_n; }
unsigned simnode_outbox_dropped(void) { return s_outbox_dropped; }
void simnode_outbox_clear(void) { s_outbox_n = 0; s_outbox_dropped = 0; }

const struct simnode_frame *simnode_outbox_get(unsigned i)
{
    return (i < s_outbox_n) ? &s_outbox[i] : NULL;
}

int mmdrv_tx_frame(struct mmpkt *pkt, bool is_mgmt)
{
    if (pkt == NULL) { return -1; }
    struct mmpktview *v = mmpkt_open(pkt);
    uint32_t len = mmpkt_get_data_length(v);
    const uint8_t *data = mmpkt_get_data_start(v);
    if (s_outbox_n < SIMNODE_OUTBOX_MAX && len <= sizeof(s_outbox[0].bytes))
    {
        struct simnode_frame *f = &s_outbox[s_outbox_n++];
        memcpy(f->bytes, data, len);
        f->len = (uint16_t)len;
        f->is_mgmt = is_mgmt;
        struct mmdrv_tx_metadata *md = mmdrv_get_tx_metadata(pkt);
        f->vif_id = md->vif_id;
        f->tid = md->tid;
        f->tx_flags = md->flags;
        f->key_idx = md->key_idx;
    }
    else
    {
        s_outbox_dropped++;
    }
    mmpkt_close(&v);
    /* The real driver takes ownership and releases on TX completion. */
    mmpkt_release(pkt);
    return 0;
}

/* The driver's packet allocators. The real ones come from a driver-owned pool;
 * heap-backed ones with the same headroom contract are the honest stand-in,
 * because what the firmware depends on is the headroom, not the pool. */
struct mmpkt *mmdrv_alloc_mmpkt_for_tx(uint8_t pkt_class, uint32_t space_at_start,
                                       uint32_t space_at_end)
{
    (void)pkt_class;
    return mmpkt_alloc_on_heap(space_at_start, space_at_end,
                               sizeof(struct mmdrv_tx_metadata));
}

struct mmpkt *mmdrv_alloc_mmpkt_for_defrag(uint32_t min_capacity, uint32_t max_capacity)
{
    (void)min_capacity;
    return mmpkt_alloc_on_heap(0, max_capacity, sizeof(struct mmdrv_rx_metadata));
}

/* Chip configuration: recorded, not performed. A test can assert the firmware
 * asked for the right thing (mesh started, beaconing on, a key installed at a
 * given index) without a chip to ask. */
struct simnode_chipcfg {
    unsigned mesh_config, bss_cfg, bss_beacon, qos_queue, beaconing_period;
    unsigned keys_installed, keys_disabled, sta_state_updates, seq_num_spaces, bssid_set;
    bool     mesh_started, beaconing_enabled, crypto_in_host;
};
static struct simnode_chipcfg s_cfg;
const struct simnode_chipcfg *simnode_chipcfg(void) { return &s_cfg; }
void simnode_chipcfg_clear(void) { memset(&s_cfg, 0, sizeof(s_cfg)); }

int mmdrv_mesh_config(uint16_t vif_id, bool start, bool enable_beaconing)
{
    (void)vif_id;
    s_cfg.mesh_config++;
    s_cfg.mesh_started = start;
    s_cfg.beaconing_enabled = enable_beaconing;
    return 0;
}

int mmdrv_cfg_bss(uint16_t vif_id, uint16_t beacon_int, uint16_t dtim_period, uint32_t cssid)
{
    (void)vif_id; (void)beacon_int; (void)dtim_period; (void)cssid;
    s_cfg.bss_cfg++;
    return 0;
}

int mmdrv_cfg_bss_beacon(uint16_t vif_id, bool enable)
{
    (void)vif_id; (void)enable;
    s_cfg.bss_beacon++;
    return 0;
}

int mmdrv_cfg_qos_queue(const struct mmwlan_qos_queue_params *params)
{
    (void)params;
    s_cfg.qos_queue++;
    return 0;
}

int mmdrv_start_beaconing_period(uint16_t vif_id, uint32_t host_timer_period_ms)
{
    (void)vif_id; (void)host_timer_period_ms;
    s_cfg.beaconing_period++;
    return 0;
}

int mmdrv_install_key(uint16_t vif_id, uint16_t aid, struct mmdrv_key_conf *key_conf)
{
    (void)vif_id; (void)aid; (void)key_conf;
    s_cfg.keys_installed++;
    return 0;
}

int mmdrv_disable_key(uint16_t vif_id, uint16_t aid, uint8_t hw_key_idx, bool is_pairwise)
{
    (void)vif_id; (void)aid; (void)hw_key_idx; (void)is_pairwise;
    s_cfg.keys_disabled++;
    return 0;
}

int mmdrv_set_bssid(uint16_t vif_id, const uint8_t bssid[6])
{
    (void)vif_id; (void)bssid;
    s_cfg.bssid_set++;
    return 0;
}

int mmdrv_set_seq_num_spaces(uint16_t vif_id, const uint16_t *tx_seq_num_spaces,
                             const uint8_t *addr)
{
    (void)vif_id; (void)tx_seq_num_spaces; (void)addr;
    s_cfg.seq_num_spaces++;
    return 0;
}

int mmdrv_update_sta_state(uint16_t vif_id, uint16_t aid, const uint8_t *addr,
                           enum morse_sta_state state)
{
    (void)vif_id; (void)aid; (void)addr; (void)state;
    s_cfg.sta_state_updates++;
    return 0;
}

/* Host CCMP: the chip's one group-key slot is why warthog can do crypto in the
 * host at all, so the setting is honoured rather than ignored. */
int mmdrv_set_crypto_in_host(uint16_t vif_id, bool enable, uint32_t *out_value)
{
    (void)vif_id;
    s_cfg.crypto_in_host = enable;
    if (out_value != NULL) { *out_value = enable ? 1u : 0u; }
    return 0;
}

int mmdrv_get_crypto_in_host(uint16_t vif_id, uint32_t *out_value)
{
    (void)vif_id;
    if (out_value != NULL) { *out_value = s_cfg.crypto_in_host ? 1u : 0u; }
    return 0;
}
