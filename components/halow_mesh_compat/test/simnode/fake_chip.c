/*
 * The chip, replaced. THIS FILE IS THE PHY CUT LINE.
 *
 * mmdrv_tx_frame() is where the firmware hands a finished 802.11 frame to the
 * MM6108 over SDIO. Here it appends that frame to an outbox instead, so a test
 * can assert on the exact bytes that would have gone on the air -- addresses,
 * QoS control, Mesh Control, Address Extension, the lot.
 *
 * Everything above this line is the real firmware. Below it only two things
 * the host sees back are modelled: a TX status per data frame, and the group key
 * slot's TX PN, drawn when a frame is sent. No modulation, no timing, no
 * interference. What the radio does with these bytes is exactly what the
 * simulator cannot tell you.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mmpkt.h"
#include "mmdrv.h"
#include "simnode.h" /* the one definition of struct simnode_frame the tests read */
#include "umac/data/umac_data.h"
#include "umac/datapath/umac_datapath.h"

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

/* The group key slot (aid 0) and its TX PN. Only the chip advances it, one PN per frame
 * it encrypts there, from the install PN up; the host cannot read it. */
static struct {
    bool     valid;
    uint8_t  key_idx;
    uint64_t next_pn;
    bool     used;
    uint64_t top;
} s_grp;

bool simnode_group_pn_top(uint64_t *top)
{
    if (top != NULL) { *top = s_grp.top; }
    return s_grp.used;
}

/* The chip sends @p pkt, or hands it back untried, then reports its TX status up the
 * real path; the datapath releases it once the event loop takes that status. */
static void chip_finish_(struct mmpkt *pkt, bool sent)
{
    struct mmdrv_tx_metadata *md = mmdrv_get_tx_metadata(pkt);
    struct mmpktview *v = mmpkt_open(pkt);
    const uint8_t *d = mmpkt_get_data_start(v);
    const bool group_ra = mmpkt_get_data_length(v) >= 10u && (d[4] & 0x01u) != 0u;
    mmpkt_close(&v);
    if (sent && (md->flags & MMDRV_TX_FLAG_HW_ENC) != 0u && group_ra && s_grp.valid &&
        md->key_idx == s_grp.key_idx)
    {
        uint64_t pn = s_grp.next_pn++;
        if (!s_grp.used || pn > s_grp.top) { s_grp.top = pn; }
        s_grp.used = true;
    }
    md->attempts = sent ? 1u : 0u;
    md->status_flags = !sent ? MMDRV_TX_STATUS_DUTY_CYCLE_CANT_SEND
                     : (md->flags & MMDRV_TX_FLAG_NO_ACK) != 0u ? MMDRV_TX_STATUS_FLAG_NO_ACK : 0u;
    umac_datapath_handle_tx_status(umac_data_get_umacd(), pkt);
}

/* Frames waiting in the chip's queue while simnode_tx_hold is on, oldest first. */
static struct mmpkt *s_held[SIMNODE_OUTBOX_MAX];
static unsigned s_held_n;
static bool s_hold;

void simnode_tx_hold(bool on) { s_hold = on; }
unsigned simnode_tx_held(void) { return s_held_n; }

static struct mmpkt *take_held_(unsigned i)
{
    if (i >= s_held_n) { return NULL; }
    struct mmpkt *pkt = s_held[i];
    memmove(&s_held[i], &s_held[i + 1u], (s_held_n - i - 1u) * sizeof(s_held[0]));
    s_held_n--;
    return pkt;
}

bool simnode_tx_send_held(unsigned i)
{
    struct mmpkt *pkt = take_held_(i);
    if (pkt == NULL) { return false; }
    chip_finish_(pkt, true);
    return true;
}

bool simnode_tx_return_held(unsigned i)
{
    struct mmpkt *pkt = take_held_(i);
    if (pkt == NULL) { return false; }
    chip_finish_(pkt, false);
    return true;
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
    /* Management frames' status feeds hostap and rate control, neither linked: freed as sent. */
    if (is_mgmt)
    {
        mmpkt_release(pkt);
        return 0;
    }
    /* The driver owns it now; the chip sends it and reports its status, at once unless held. */
    if (s_hold && s_held_n < SIMNODE_OUTBOX_MAX)
    {
        s_held[s_held_n++] = pkt;
        return 0;
    }
    chip_finish_(pkt, true);
    return 0;
}

/* The driver's packet allocators. The real ones come from a driver-owned pool;
 * heap-backed ones with the same headroom contract are the honest stand-in,
 * because what the firmware depends on is the headroom, not the pool. */
static void (*s_tx_alloc_hook)(void);
void simnode_set_tx_alloc_hook(void (*cb)(void)) { s_tx_alloc_hook = cb; }

struct mmpkt *mmdrv_alloc_mmpkt_for_tx(uint8_t pkt_class, uint32_t space_at_start,
                                       uint32_t space_at_end)
{
    (void)pkt_class;
    void (*cb)(void) = s_tx_alloc_hook;
    s_tx_alloc_hook = NULL;
    if (cb != NULL) { cb(); }
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

#define SIMNODE_KEYINST_MAX 32u
static struct simnode_keyinst s_keyinst[SIMNODE_KEYINST_MAX];
static unsigned s_keyinst_n;

unsigned simnode_keyinst_count(void) { return s_keyinst_n; }
const struct simnode_keyinst *simnode_keyinst_get(unsigned i)
{
    return (i < s_keyinst_n) ? &s_keyinst[i] : NULL;
}
void simnode_keyinst_clear(void) { s_keyinst_n = 0; }

static unsigned s_install_fail_next;
void simnode_fail_next_install_key(void) { s_install_fail_next++; }

int mmdrv_install_key(uint16_t vif_id, uint16_t aid, struct mmdrv_key_conf *key_conf)
{
    if (s_install_fail_next != 0u)
    {
        s_install_fail_next--;
        return -5;
    }
    s_cfg.keys_installed++;
    if (key_conf != NULL && !key_conf->is_pairwise)
    {
        s_grp.valid = true;
        s_grp.key_idx = key_conf->key_idx;
        s_grp.next_pn = key_conf->tx_pn;
    }
    if (key_conf != NULL && s_keyinst_n < SIMNODE_KEYINST_MAX)
    {
        struct simnode_keyinst *k = &s_keyinst[s_keyinst_n++];
        k->vif_id = vif_id;
        k->aid = aid;
        k->pairwise = key_conf->is_pairwise;
        k->key_idx = key_conf->key_idx;
        k->tx_pn = key_conf->tx_pn;
        memcpy(k->key, key_conf->key, sizeof(k->key));
    }
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
