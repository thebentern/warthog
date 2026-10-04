/*
 * The chip, replaced. THIS FILE IS THE PHY CUT LINE.
 *
 * mmdrv_tx_frame() is where the firmware hands a finished 802.11 frame to the
 * MM6108 over SDIO. Here it appends that frame to an outbox instead, so a test
 * can assert on the exact bytes that would have gone on the air -- addresses,
 * QoS control, Mesh Control, Address Extension, the lot.
 *
 * Everything above this line is the real firmware. Below it only four things
 * the host sees back are modelled: a TX status per data frame (and per management
 * frame under our group key, and per DELBA a cut MSDU waits on), the TX PN of each key the
 * chip holds, drawn when it sends a frame it seals (kept as the frame's air copy), and, for
 * a frame a test passes through simnode_rx_air, which key the chip opens a Protected frame
 * under. No modulation, no timing, no interference. What the radio does with these bytes is
 * exactly what the simulator cannot tell you.
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
#include "umac/core/umac_core.h"
#include "common/morse_commands.h" /* MORSE_CMD_ID_*: the command log's ids */
#include "mmhal_wlan.h"
#include "umac_mesh_ccm.h"      /* the receive model opens frames with the firmware's own CCM */
#include "umac_mesh_ccmp_hdr.h"

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
    unsigned draws;
} s_grp;

bool simnode_group_pn_top(uint64_t *top)
{
    if (top != NULL) { *top = s_grp.top; }
    return s_grp.used;
}

unsigned simnode_group_pn_draws(void) { return s_grp.draws; }

/* The chip's keys (below). Sealing a frame draws its key's TX PN, @p draws of them for a frame
 * it cuts into that many fragments. */
static bool chip_key_seal_key_(uint16_t aid, bool pairwise, uint8_t idx, uint8_t key[16],
                               uint64_t *pn, unsigned draws);
static uint16_t chip_sta_aid_(const uint8_t *addr);

static unsigned s_noack_next, s_retry_next, s_agg_next, s_noack_mgmt_next;
void simnode_tx_noack_next(unsigned n) { s_noack_next = n; }
void simnode_tx_noack_mgmt_next(unsigned n) { s_noack_mgmt_next = n; }
void simnode_tx_retry_next(unsigned attempts) { s_retry_next = attempts; }
void simnode_tx_aggregated_next(unsigned n) { s_agg_next = n; }
static bool s_agg_params;
void simnode_chip_agg_on_baparams(bool on) { s_agg_params = on; }

/* What the chip holds and loses when it boots (simnode_chipstate), its TX fragmentation
 * threshold among it. */
static struct simnode_chipstate s_now;

/* The fragments the chip cuts a unicast frame of @p len octets (MAC header @p hl, CCMP and FCS
 * on top) into under its own threshold (AT+FRAG), each fragment's header, CCMP and FCS counted. */
static unsigned chip_frags_(uint32_t len, uint32_t hl)
{
    const uint32_t thr = s_now.frag_threshold;
    if (thr == 0u || len + 16u + 4u <= thr || thr <= hl + 16u + 4u + 1u) { return 1u; }
    const uint32_t chunk = (thr - hl - 16u - 4u) & ~1u;
    const uint32_t n = (len - hl + chunk - 1u) / chunk;
    return n > 16u ? 16u : (unsigned)n;
}

/* Outbox entry @p idx as the chip put it on the air: a HW_ENC frame sealed under the group
 * slot's key at @p grp_pn, or a unicast one under its station's pairwise key at that key's
 * next TX PN; anything else as handed. */
static void chip_air_(unsigned idx, const struct mmdrv_tx_metadata *md, bool group_ra,
                      bool have_grp_pn, uint64_t grp_pn)
{
    if (idx >= s_outbox_n) { return; }
    struct simnode_frame *f = &s_outbox[idx];
    f->sent = true;
    if ((md->flags & MMDRV_TX_FLAG_HW_ENC) == 0u)
    {
        memcpy(f->air, f->bytes, f->len);
        f->air_len = f->len;
        return;
    }
    uint8_t key[16];
    uint64_t pn = grp_pn;
    bool keyed;
    const uint32_t hl = umac_ccmp_hdr_len(f->bytes);
    if (group_ra)
    {
        keyed = have_grp_pn && chip_key_seal_key_(0, false, md->key_idx, key, NULL, 1u);
    }
    else
    {
        const uint16_t aid = md->aid != 0u ? md->aid : chip_sta_aid_(f->bytes + 4);
        const unsigned draws = chip_frags_(f->len, hl);
        keyed = aid != 0u && chip_key_seal_key_(aid, true, md->key_idx, key, &pn, draws);
        f->pn_draws = keyed ? (uint8_t)draws : 0u;
    }
    if (!keyed || f->len < hl || (uint32_t)f->len + 16u > sizeof(f->air)) { return; }
    uint8_t pn6[6], aad[UMAC_CCMP_AAD_MAXLEN], nonce[13];
    for (int i = 0; i < 6; i++) { pn6[i] = (uint8_t)(pn >> (8 * (5 - i))); }
    memcpy(f->air, f->bytes, hl);
    umac_ccmp_write_header(f->air + hl, pn6, md->key_idx);
    const uint32_t body = (uint32_t)f->len - hl;
    uint8_t *b = f->air + hl + UMAC_CCMP_HDR_LEN;
    memcpy(b, f->bytes + hl, body);
    const uint32_t al = umac_ccmp_build_aad(f->air, aad);
    umac_ccmp_build_nonce(f->air, pn6, nonce);
    (void)warthog_ccm_ae(key, nonce, UMAC_CCMP_MIC_LEN, aad, al, b, body, b + body);
    f->air_len = (uint16_t)(hl + UMAC_CCMP_HDR_LEN + body + UMAC_CCMP_MIC_LEN);
    f->pn = pn;
}

/* The chip sends @p pkt (outbox entry @p idx), or hands it back untried, then reports its TX
 * status up the real path; the datapath releases it once the event loop takes that status. */
static void chip_finish_(struct mmpkt *pkt, bool sent, unsigned idx)
{
    struct mmdrv_tx_metadata *md = mmdrv_get_tx_metadata(pkt);
    struct mmpktview *v = mmpkt_open(pkt);
    const uint8_t *d = mmpkt_get_data_start(v);
    const bool group_ra = mmpkt_get_data_length(v) >= 10u && (d[4] & 0x01u) != 0u;
    const bool data = mmpkt_get_data_length(v) >= 1u && (d[0] & 0x0cu) == 0x08u;
    mmpkt_close(&v);
    bool have_grp_pn = false;
    uint64_t grp_pn = 0;
    if (sent && (md->flags & MMDRV_TX_FLAG_HW_ENC) != 0u && group_ra && s_grp.valid &&
        md->key_idx == s_grp.key_idx)
    {
        uint64_t pn = s_grp.next_pn++;
        if (!s_grp.used || pn > s_grp.top) { s_grp.top = pn; }
        s_grp.used = true;
        s_grp.draws++;
        have_grp_pn = true;
        grp_pn = pn;
    }
    if (sent)
    {
        chip_air_(idx, md, group_ra, have_grp_pn, grp_pn);
    }
    md->attempts = sent ? 1u : 0u;
    md->status_flags = !sent ? MMDRV_TX_STATUS_DUTY_CYCLE_CANT_SEND
                     : (md->flags & MMDRV_TX_FLAG_NO_ACK) != 0u ? MMDRV_TX_STATUS_FLAG_NO_ACK : 0u;
    if (sent && data && !group_ra && (md->flags & MMDRV_TX_FLAG_NO_ACK) == 0u && s_noack_next != 0u)
    {
        /* Every attempt its chain allows, then given up. */
        unsigned tries = 0;
        for (unsigned i = 0; i < MMRC_MAX_CHAIN_LENGTH; i++)
        {
            tries += md->rc_data.rates[i].rate != MMRC_MCS_UNUSED ? md->rc_data.rates[i].attempts : 0u;
        }
        s_noack_next--;
        md->attempts = (uint8_t)(tries != 0u ? tries : 1u);
        md->status_flags = MMDRV_TX_STATUS_FLAG_NO_ACK;
    }
    else if (sent && !data && !group_ra && s_noack_mgmt_next != 0u)
    {
        unsigned tries = 0;
        for (unsigned i = 0; i < MMRC_MAX_CHAIN_LENGTH; i++)
        {
            tries += md->rc_data.rates[i].rate != MMRC_MCS_UNUSED ? md->rc_data.rates[i].attempts : 0u;
        }
        s_noack_mgmt_next--;
        md->attempts = (uint8_t)(tries != 0u ? tries : 1u);
        md->status_flags = MMDRV_TX_STATUS_FLAG_NO_ACK;
    }
    else if (sent && data && !group_ra && (md->flags & MMDRV_TX_FLAG_NO_ACK) == 0u && s_retry_next != 0u)
    {
        unsigned tries = 0;
        for (unsigned i = 0; i < MMRC_MAX_CHAIN_LENGTH; i++)
        {
            tries += md->rc_data.rates[i].rate != MMRC_MCS_UNUSED ? md->rc_data.rates[i].attempts : 0u;
        }
        const bool acked = tries >= s_retry_next;
        md->attempts = (uint8_t)(acked ? s_retry_next : (tries != 0u ? tries : 1u));
        md->status_flags = acked ? 0u : MMDRV_TX_STATUS_FLAG_NO_ACK;
        s_retry_next = 0;
    }
    if (sent && data && !group_ra && s_agg_next != 0u)
    {
        s_agg_next--;
        md->status_flags |= MMDRV_TX_STATUS_WAS_AGGREGATED;
    }
    else if (sent && data && !group_ra && s_agg_params &&
             ((md->flags & MMDRV_TX_FLAG_AMPDU_ENABLED) != 0u || md->tid_max_reorder_buf_size != 0u))
    {
        md->status_flags |= MMDRV_TX_STATUS_WAS_AGGREGATED;
    }
    if (idx < s_outbox_n)
    {
        s_outbox[idx].status_flags = md->status_flags;
        s_outbox[idx].attempts = md->attempts;
    }
    umac_datapath_handle_tx_status(umac_data_get_umacd(), pkt);
}

/* Frames waiting in the chip's queue while simnode_tx_hold is on, oldest first, and the
 * outbox entry each is (SIMNODE_OUTBOX_MAX: the outbox was full). */
static struct mmpkt *s_held[SIMNODE_OUTBOX_MAX];
static unsigned s_held_idx[SIMNODE_OUTBOX_MAX];
static unsigned s_held_n;
static bool s_hold;

void simnode_tx_hold(bool on) { s_hold = on; }
unsigned simnode_tx_held(void) { return s_held_n; }
int simnode_tx_held_tid(unsigned i)
{
    return (i < s_held_n && s_held_idx[i] < s_outbox_n) ? (int)s_outbox[s_held_idx[i]].tid : -1;
}

static struct mmpkt *take_held_(unsigned i, unsigned *idx)
{
    if (i >= s_held_n) { return NULL; }
    struct mmpkt *pkt = s_held[i];
    *idx = s_held_idx[i];
    memmove(&s_held[i], &s_held[i + 1], (s_held_n - i - 1u) * sizeof(s_held[0]));
    memmove(&s_held_idx[i], &s_held_idx[i + 1], (s_held_n - i - 1u) * sizeof(s_held_idx[0]));
    s_held_n--;
    return pkt;
}

bool simnode_tx_send_held(unsigned i)
{
    unsigned idx = 0;
    struct mmpkt *pkt = take_held_(i, &idx);
    if (pkt == NULL) { return false; }
    chip_finish_(pkt, true, idx);
    return true;
}

bool simnode_tx_return_held(unsigned i)
{
    unsigned idx = 0;
    struct mmpkt *pkt = take_held_(i, &idx);
    if (pkt == NULL) { return false; }
    chip_finish_(pkt, false, idx);
    return true;
}

bool simnode_tx_drop_held(unsigned i)
{
    unsigned idx = 0;
    struct mmpkt *pkt = take_held_(i, &idx);
    if (pkt == NULL) { return false; }
    struct mmdrv_tx_metadata *md = mmdrv_get_tx_metadata(pkt);
    if (md->mesh.host_frag == 0u && md->mesh.ba_wait == 0u)
    {
        mmpkt_release(pkt);
        return true;
    }
    md->attempts = 0;
    md->status_flags = 0;
    umac_datapath_handle_tx_status(umac_data_get_umacd(), pkt);
    return true;
}

bool simnode_tx_forget_held(unsigned i)
{
    unsigned idx = 0;
    struct mmpkt *pkt = take_held_(i, &idx);
    if (pkt == NULL) { return false; }
    mmpkt_release(pkt);
    return true;
}

int mmdrv_tx_frame(struct mmpkt *pkt, bool is_mgmt)
{
    if (pkt == NULL) { return -1; }
    struct mmpktview *v = mmpkt_open(pkt);
    uint32_t len = mmpkt_get_data_length(v);
    const uint8_t *data = mmpkt_get_data_start(v);
    const bool group_ra = len >= 10u && (data[4] & 0x01u) != 0u;
    unsigned idx = SIMNODE_OUTBOX_MAX;
    if (s_outbox_n < SIMNODE_OUTBOX_MAX && len <= sizeof(s_outbox[0].bytes))
    {
        idx = s_outbox_n;
        struct simnode_frame *f = &s_outbox[s_outbox_n++];
        memset(f, 0, sizeof(*f));
        memcpy(f->bytes, data, len);
        f->len = (uint16_t)len;
        f->is_mgmt = is_mgmt;
        struct mmdrv_tx_metadata *md = mmdrv_get_tx_metadata(pkt);
        f->vif_id = md->vif_id;
        f->tid = md->tid;
        f->tx_flags = md->flags;
        f->key_idx = md->key_idx;
        f->aid = (uint8_t)md->aid;
        f->host_frag = md->mesh.host_frag != 0u;
        f->ba_wait = md->mesh.ba_wait != 0u;
        f->reorder = md->tid_max_reorder_buf_size;
        for (unsigned r = 0; r < 4u; r++)
        {
            const struct mmrc_rate *rt = &md->rc_data.rates[r];
            if (rt->rate == MMRC_MCS_UNUSED || rt->attempts == 0u) { continue; }
            f->chain[r].bw_mhz = (uint8_t)(1u << rt->bw);
            f->chain[r].mcs = (uint8_t)rt->rate;
            f->chain[r].attempts = (uint8_t)rt->attempts;
            f->chain_rts |= (uint8_t)((rt->flags & MMRC_MASK(MMRC_FLAGS_CTS_RTS)) != 0u ? 1u << r : 0u);
        }
    }
    else
    {
        s_outbox_dropped++;
    }
    mmpkt_close(&v);
    /* Management frames' status feeds hostap and rate control, neither linked: freed as sent,
     * and sealed now if HW_ENC. Not one under our group key (group path selection): it draws
     * that slot's PN, as data does; nor a DELBA a cut waits on, whose status the host reads. */
    if (is_mgmt && mmdrv_get_tx_metadata(pkt)->mesh.own_group == 0u &&
        mmdrv_get_tx_metadata(pkt)->mesh.ba_wait == 0u)
    {
        chip_air_(idx, mmdrv_get_tx_metadata(pkt), group_ra, false, 0);
        mmpkt_release(pkt);
        return 0;
    }
    /* The driver owns it now; the chip sends it and reports its status, at once unless held. */
    if (s_hold && s_held_n < SIMNODE_OUTBOX_MAX)
    {
        s_held_idx[s_held_n] = idx;
        s_held[s_held_n++] = pkt;
        return 0;
    }
    chip_finish_(pkt, true, idx);
    return 0;
}

/* The driver's packet allocators. The real ones come from a driver-owned pool;
 * heap-backed ones with the same headroom contract are the honest stand-in,
 * because what the firmware depends on is the headroom, not the pool. */
static void (*s_tx_alloc_hook)(void);
void simnode_set_tx_alloc_hook(void (*cb)(void)) { s_tx_alloc_hook = cb; }

static unsigned s_tx_alloc_fail_at, s_tx_allocs;
void simnode_tx_alloc_fail_at(unsigned k) { s_tx_alloc_fail_at = k; }
unsigned simnode_tx_allocs(void) { return s_tx_allocs; }

/* The firmware's own TX pool (mmpktmem_heap.c, MMPKTMEM_TX_POOL_N_BLOCKS from the Makefile),
 * initialised once: its count must outlive every packet it handed out. */
static bool s_pool_on, s_pool_init;
static uint32_t s_pool_reserve;
static void pool_flow_cb_(void) {}
void simnode_tx_pool(bool on)
{
    if (on && !s_pool_init)
    {
        struct mmhal_wlan_pktmem_init_args a = { .tx_flow_control_cb = pool_flow_cb_ };
        mmhal_wlan_pktmem_init(&a);
        s_pool_init = true;
        mmhal_wlan_pktmem_set_tx_reserve(s_pool_reserve);
    }
    s_pool_on = on;
}
bool simnode_tx_pool_paused(void)
{
    return s_pool_init && mmhal_wlan_pktmem_tx_flow_control_state() == MMWLAN_TX_PAUSED;
}
uint32_t simnode_tx_pool_free(void) { return s_pool_init ? mmhal_wlan_pktmem_tx_free() : 0u; }
uint32_t simnode_tx_pool_reserve(void) { return s_pool_reserve; }

struct mmpkt *mmdrv_alloc_mmpkt_for_tx(uint8_t pkt_class, uint32_t space_at_start,
                                       uint32_t space_at_end)
{
    (void)pkt_class;
    void (*cb)(void) = s_tx_alloc_hook;
    s_tx_alloc_hook = NULL;
    if (cb != NULL) { cb(); }
    if (s_tx_alloc_fail_at != 0u && --s_tx_alloc_fail_at == 0u)
    {
        return NULL; /* the pool is out */
    }
    if (s_pool_on)
    {
        struct mmpkt *p = mmhal_wlan_alloc_mmpkt_for_tx(pkt_class, space_at_start, space_at_end,
                                                       sizeof(struct mmdrv_tx_metadata));
        s_tx_allocs += p != NULL;
        return p;
    }
    s_tx_allocs++;
    return mmpkt_alloc_on_heap(space_at_start, space_at_end,
                               sizeof(struct mmdrv_tx_metadata));
}

uint32_t mmdrv_tx_pool_free(void) { return s_pool_on ? mmhal_wlan_pktmem_tx_free() : UINT32_MAX; }

void mmdrv_set_tx_pool_reserve(uint32_t blocks)
{
    s_pool_reserve = blocks;
    if (s_pool_init) { mmhal_wlan_pktmem_set_tx_reserve(blocks); }
}

struct mmpkt *mmdrv_alloc_mmpkt_for_defrag(uint32_t min_capacity, uint32_t max_capacity)
{
    (void)min_capacity;
    return mmpkt_alloc_on_heap(0, max_capacity, sizeof(struct mmdrv_rx_metadata));
}

/* Interface, BSS and station commands, in the order the firmware sent them, each with
 * the status this chip answered. A refusal is armed per command, as a firmware that does
 * not support something answers it. */
#define SIMNODE_CHIPCMD_MAX 256u
static struct simnode_chipcmd s_cmd[SIMNODE_CHIPCMD_MAX];
static unsigned s_cmd_n;
static struct { uint32_t type; int32_t status; int ret; } s_refuse_add[4];
static unsigned s_refuse_add_n;
/* Other commands' refusals: the first command with this id (and arg, unless UINT32_MAX).
 * REMOVE_INTERFACE has no status in its response: its "status" is its transport result. */
static struct { uint16_t id; uint32_t arg; int32_t status; } s_refuse[8];
static unsigned s_refuse_n;
static struct { bool set; uint16_t id; } s_mesh_vif;

unsigned simnode_chipcmd_count(void) { return s_cmd_n; }
const struct simnode_chipcmd *simnode_chipcmd_get(unsigned i)
{
    return (i < s_cmd_n) ? &s_cmd[i] : NULL;
}
void simnode_chipcmd_clear(void) { s_cmd_n = 0; }

unsigned simnode_chipcmd_n(uint16_t id, uint32_t arg)
{
    unsigned n = 0;
    for (unsigned i = 0; i < s_cmd_n; i++)
    {
        n += (s_cmd[i].id == id && (arg == UINT32_MAX || s_cmd[i].arg == arg));
    }
    return n;
}

void simnode_chip_refuse_add_if(uint32_t type, int32_t status, int ret)
{
    if (s_refuse_add_n < 4u)
    {
        s_refuse_add[s_refuse_add_n].type = type;
        s_refuse_add[s_refuse_add_n].status = status;
        s_refuse_add[s_refuse_add_n].ret = ret;
        s_refuse_add_n++;
    }
}

void simnode_chip_refuse_next_arg(uint16_t id, uint32_t arg, int32_t status);
void simnode_chip_refuse_rm_if(int ret)
{
    simnode_chip_refuse_next_arg(MORSE_CMD_ID_REMOVE_INTERFACE, UINT32_MAX, ret);
}

void simnode_chip_refuse_next_arg(uint16_t id, uint32_t arg, int32_t status)
{
    if (s_refuse_n < sizeof(s_refuse) / sizeof(s_refuse[0]))
    {
        s_refuse[s_refuse_n].id = id;
        s_refuse[s_refuse_n].arg = arg;
        s_refuse[s_refuse_n].status = status;
        s_refuse_n++;
    }
}

void simnode_chip_refuse_next(uint16_t id, int32_t status)
{
    simnode_chip_refuse_next_arg(id, UINT32_MAX, status);
}

unsigned simnode_chip_refusals_armed(void) { return s_refuse_n + s_refuse_add_n; }

void simnode_chip_refusals_clear(void)
{
    s_refuse_n = 0;
    s_refuse_add_n = 0;
}

/* The status this chip answers a command with: an armed refusal's, taken, else 0. */
static int32_t chip_answer_(uint16_t id, uint32_t arg)
{
    for (unsigned i = 0; i < s_refuse_n; i++)
    {
        if (s_refuse[i].id == id && (s_refuse[i].arg == UINT32_MAX || s_refuse[i].arg == arg))
        {
            const int32_t status = s_refuse[i].status;
            s_refuse_n--;
            memmove(&s_refuse[i], &s_refuse[i + 1], (s_refuse_n - i) * sizeof(s_refuse[0]));
            return status;
        }
    }
    return 0;
}

void simnode_chip_set_mesh_vif_id(uint16_t vif_id)
{
    s_mesh_vif.set = true;
    s_mesh_vif.id = vif_id;
}

static struct simnode_chipcmd *chipcmd_log_(uint16_t id, uint16_t vif_id, uint32_t arg)
{
    static struct simnode_chipcmd spill;
    struct simnode_chipcmd *c = s_cmd_n < SIMNODE_CHIPCMD_MAX ? &s_cmd[s_cmd_n++] : &spill;
    memset(c, 0, sizeof(*c));
    c->id = id;
    c->vif_id = vif_id;
    c->arg = arg;
    return c;
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

int mmdrv_mesh_config(uint16_t vif_id, bool start, bool enable_beaconing, int32_t *chip_status)
{
    struct simnode_chipcmd *c = chipcmd_log_(MORSE_CMD_ID_MESH_CONFIG, vif_id, start ? 1u : 0u);
    c->beaconing = enable_beaconing;
    c->status = chip_answer_(MORSE_CMD_ID_MESH_CONFIG, c->arg);
    if (chip_status != NULL) { *chip_status = c->status; }
    if (c->status == 0)
    {
        s_now.mesh_started = start;
        s_now.mesh_beaconing = enable_beaconing;
    }
    s_cfg.mesh_config++;
    s_cfg.mesh_started = start;
    s_cfg.beaconing_enabled = enable_beaconing;
    return 0;
}

int mmdrv_cfg_bss(uint16_t vif_id, uint16_t beacon_int, uint16_t dtim_period, uint32_t cssid)
{
    (void)dtim_period; (void)cssid;
    (void)chipcmd_log_(MORSE_CMD_ID_BSS_CONFIG, vif_id, beacon_int);
    s_now.beacon_int = beacon_int;
    s_cfg.bss_cfg++;
    return 0;
}

int mmdrv_cfg_bss_beacon(uint16_t vif_id, bool enable, int32_t *chip_status)
{
    struct simnode_chipcmd *c = chipcmd_log_(MORSE_CMD_ID_BSS_BEACON_CONFIG, vif_id, enable ? 1u : 0u);
    c->status = chip_answer_(MORSE_CMD_ID_BSS_BEACON_CONFIG, c->arg);
    if (chip_status != NULL) { *chip_status = c->status; }
    if (c->status == 0) { s_now.bss_beacon = enable; }
    s_cfg.bss_beacon++;
    return 0;
}

int mmdrv_cfg_qos_queue(const struct mmwlan_qos_queue_params *params)
{
    (void)params;
    s_now.qos++;
    s_cfg.qos_queue++;
    return 0;
}

int mmdrv_start_beaconing_period(uint16_t vif_id, uint32_t host_timer_period_ms)
{
    (void)vif_id;
    s_now.beacon_period_ms = host_timer_period_ms;
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

/* ---- the chip's stations and keys ---------------------------------------------------
 *
 * What the receive model (simnode_rx_air) opens frames with. A station is what
 * SET_STA_STATE told the chip: an AID and an address, known from AUTHENTICATED up. A key
 * is held per (AID, pairwise or group, index) from the INSTALL_KEY the chip accepted until
 * a DISABLE_KEY for it or an install over it. Removing a station is NOT assumed to take its
 * keys: whether the MM6108 does is not measured, so the firmware has to say so itself.
 * Cleared when the chip boots. */
#define SIMNODE_CHIP_STA_MAX 16u
static struct { bool used; uint16_t aid; uint8_t addr[6]; uint32_t state; } s_sta[SIMNODE_CHIP_STA_MAX];
#define SIMNODE_CHIP_KEY_MAX 32u
static struct { bool used; uint16_t aid; bool pairwise; uint8_t idx; uint8_t key[16]; uint64_t next_pn; } s_key[SIMNODE_CHIP_KEY_MAX];
static uint32_t s_vif_type;   /* the type of the interface the chip added last */
static bool s_grp_fallback;   /* MESH VIF: a group frame no station key opens is tried at AID 0 */
static bool s_grp_fallback_mic; /* ... and one whose MIC fails under its station's key */
static unsigned s_rx_opened;

static void chip_tables_clear_(void)
{
    memset(s_sta, 0, sizeof(s_sta));
    memset(s_key, 0, sizeof(s_key));
    s_vif_type = 0;
}

static void chip_sta_note_(uint16_t aid, const uint8_t *addr, uint32_t state)
{
    unsigned free_i = SIMNODE_CHIP_STA_MAX;
    for (unsigned i = 0; i < SIMNODE_CHIP_STA_MAX; i++)
    {
        if (s_sta[i].used && s_sta[i].aid == aid) { free_i = i; break; }
        if (!s_sta[i].used && free_i == SIMNODE_CHIP_STA_MAX) { free_i = i; }
    }
    if (free_i == SIMNODE_CHIP_STA_MAX) { return; }
    s_sta[free_i].used = true;
    s_sta[free_i].aid = aid;
    s_sta[free_i].state = state;
    if (addr != NULL) { memcpy(s_sta[free_i].addr, addr, 6); }
}

/* The AID of the station the chip knows at @p addr, or 0. */
static uint16_t chip_sta_aid_(const uint8_t *addr)
{
    for (unsigned i = 0; i < SIMNODE_CHIP_STA_MAX; i++)
    {
        if (s_sta[i].used && s_sta[i].state >= (uint32_t)MORSE_STA_AUTHENTICATED &&
            memcmp(s_sta[i].addr, addr, 6) == 0)
        {
            return s_sta[i].aid;
        }
    }
    return 0;
}

/* STA VIF, shared mode: one pairwise TX PN counter, set by each pairwise install. */
static bool s_shared_pn_on;
static uint64_t s_shared_pn;
void simnode_chip_shared_pairwise_pn(bool on) { s_shared_pn_on = on; }
static bool chip_shared_pn_(bool pairwise)
{
    return s_shared_pn_on && pairwise && s_vif_type != MMDRV_INTERFACE_TYPE_MESH;
}

static int chip_key_find_(uint16_t aid, bool pairwise, uint8_t idx)
{
    for (unsigned i = 0; i < SIMNODE_CHIP_KEY_MAX; i++)
    {
        if (s_key[i].used && s_key[i].aid == aid && s_key[i].pairwise == pairwise &&
            s_key[i].idx == idx)
        {
            return (int)i;
        }
    }
    return -1;
}

static void chip_key_put_(uint16_t aid, bool pairwise, uint8_t idx, const uint8_t key[16],
                          uint64_t tx_pn)
{
    int i = chip_key_find_(aid, pairwise, idx);
    for (unsigned j = 0; i < 0 && j < SIMNODE_CHIP_KEY_MAX; j++)
    {
        if (!s_key[j].used) { i = (int)j; }
    }
    if (i < 0) { return; }
    s_key[i].used = true;
    s_key[i].aid = aid;
    s_key[i].pairwise = pairwise;
    s_key[i].idx = idx;
    memcpy(s_key[i].key, key, 16);
    s_key[i].next_pn = tx_pn;
    if (pairwise) { s_shared_pn = tx_pn; }
}

/* The key a frame is sealed under, and for a pairwise one its next TX PN: from the install's
 * tx_pn up, one per frame the chip seals under it, data or management. */
static bool chip_key_seal_key_(uint16_t aid, bool pairwise, uint8_t idx, uint8_t key[16],
                               uint64_t *pn, unsigned draws)
{
    const int k = chip_key_find_(aid, pairwise, idx);
    if (k < 0) { return false; }
    memcpy(key, s_key[k].key, 16);
    uint64_t *ctr = chip_shared_pn_(pairwise) ? &s_shared_pn : &s_key[k].next_pn;
    if (pn != NULL) { *pn = *ctr; *ctr += draws; }
    return true;
}

bool simnode_chip_key_next_pn(uint16_t aid, bool pairwise, uint8_t idx, uint64_t *pn)
{
    const int k = chip_key_find_(aid, pairwise, idx);
    if (k < 0) { return false; }
    if (pn != NULL) { *pn = chip_shared_pn_(pairwise) ? s_shared_pn : s_key[k].next_pn; }
    return true;
}

bool simnode_chip_key_held(uint16_t aid, bool pairwise, uint8_t idx, uint8_t key[16])
{
    const int i = chip_key_find_(aid, pairwise, idx);
    if (i >= 0 && key != NULL) { memcpy(key, s_key[i].key, 16); }
    return i >= 0;
}

void simnode_chip_group_fallback(bool on) { s_grp_fallback = on; }
void simnode_chip_group_fallback_mic(bool on) { s_grp_fallback_mic = on; }
unsigned simnode_chip_rx_opened(void) { return s_rx_opened; }

/* ---- the chip's receive crypto ------------------------------------------------------
 *
 * What the chip does with a Protected frame before the host sees it, as Linux relies on
 * it (morse_driver installs every key that has a station at that station's AID): it opens
 * the frame under the key it holds for the transmitter's station with the frame's key id,
 * pairwise for a unicast and that station's group key for a group frame. On a STA chip VIF
 * a group frame opens only under the VIF's group key at AID 0 (measured: our own MGTK there
 * opened a frame sealed under it). On a MESH VIF it does not, unless simnode_chip_group_fallback
 * says the chip tries AID 0 when the station holds no group key under that id, or
 * simnode_chip_group_fallback_mic when the station's key fails the MIC. A frame it
 * cannot open goes up as it came, ciphertext and MIC intact (measured: rxdrop 4). It checks
 * no replay. A frame it opens goes up with the MIC octets still in place. */
static bool chip_rx_open_(uint8_t *f, uint16_t len)
{
    if (len < 24u || (f[1] & 0x40u) == 0u) { return false; }
    const uint8_t type = (uint8_t)((f[0] >> 2) & 0x3u);
    if (type != 0u && type != 2u) { return false; }
    const uint32_t hl = umac_ccmp_hdr_len(f);
    if (len < hl + UMAC_CCMP_HDR_LEN + UMAC_CCMP_MIC_LEN) { return false; }
    uint8_t pn[6], kid = 0;
    if (!umac_ccmp_parse_header(f + hl, pn, &kid)) { return false; }
    const bool group = (f[4] & 0x01u) != 0u;
    const bool mesh = s_vif_type == MMDRV_INTERFACE_TYPE_MESH;
    const uint16_t aid = chip_sta_aid_(f + 10);
    int k = -1;
    if (aid != 0u && (!group || mesh))
    {
        k = chip_key_find_(aid, !group, kid);
    }
    uint8_t aad[UMAC_CCMP_AAD_MAXLEN], nonce[13], plain[1600];
    const uint32_t al = umac_ccmp_build_aad(f, aad);
    umac_ccmp_build_nonce(f, pn, nonce);
    const uint32_t body = hl + UMAC_CCMP_HDR_LEN;
    const uint32_t n = len - body - UMAC_CCMP_MIC_LEN;
    bool ok = false;
    if (k >= 0)
    {
        memcpy(plain, f + body, n);
        ok = warthog_ccm_ad(s_key[k].key, nonce, UMAC_CCMP_MIC_LEN, aad, al, plain, n,
                            f + len - UMAC_CCMP_MIC_LEN) == 0;
    }
    if (!ok && group && (!mesh || (k < 0 && s_grp_fallback) || (k >= 0 && s_grp_fallback_mic)))
    {
        k = chip_key_find_(0, false, kid);
        if (k >= 0)
        {
            memcpy(plain, f + body, n);
            ok = warthog_ccm_ad(s_key[k].key, nonce, UMAC_CCMP_MIC_LEN, aad, al, plain, n,
                                f + len - UMAC_CCMP_MIC_LEN) == 0;
        }
    }
    if (!ok) { return false; }
    memcpy(f + body, plain, n);
    s_rx_opened++;
    return true;
}

static bool rx_air_(const uint8_t *frame, uint16_t len, int16_t rssi, bool queued)
{
    static uint8_t f[1600];
    if (frame == NULL || len == 0u || len > sizeof(f)) { return false; }
    memcpy(f, frame, len);
    const uint8_t flags = chip_rx_open_(f, len) ? (uint8_t)MMDRV_RX_FLAG_DECRYPTED : 0u;
    return queued ? simnode_rx_flags_queued(f, len, rssi, flags) : simnode_rx_flags(f, len, rssi, flags);
}

bool simnode_rx_air(const uint8_t *frame, uint16_t len, int16_t rssi)
{
    return rx_air_(frame, len, rssi, false);
}

bool simnode_rx_air_queued(const uint8_t *frame, uint16_t len, int16_t rssi)
{
    return rx_air_(frame, len, rssi, true);
}

/* A key the chip refuses (simnode_chip_refuse_next_arg(INSTALL_KEY, aid, status)) is not
 * installed and leaves @p key_conf as it was, as driver.c's status read does. */
int mmdrv_install_key_status(uint16_t vif_id, uint16_t aid, struct mmdrv_key_conf *key_conf,
                             int32_t *chip_status)
{
    if (chip_status != NULL) { *chip_status = 0; }
    if (s_install_fail_next != 0u)
    {
        s_install_fail_next--;
        return -5;
    }
    const int32_t refused = chip_answer_(MORSE_CMD_ID_INSTALL_KEY, aid);
    if (refused != 0)
    {
        if (chip_status != NULL) { *chip_status = refused; }
        return 0;
    }
    s_cfg.keys_installed++;
    if (key_conf != NULL)
    {
        chip_key_put_(aid, key_conf->is_pairwise, key_conf->key_idx, key_conf->key,
                          key_conf->tx_pn);
    }
    /* The TX PN model is the VIF's group key, at AID 0: a station's group key only receives. */
    if (key_conf != NULL && !key_conf->is_pairwise && aid == 0u)
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

/* As driver.c: a refusal is returned as the chip's status. */
int mmdrv_install_key(uint16_t vif_id, uint16_t aid, struct mmdrv_key_conf *key_conf)
{
    int32_t st = 0;
    int ret = mmdrv_install_key_status(vif_id, aid, key_conf, &st);
    return ret != 0 ? ret : (int)st;
}

/* As driver.c: nothing goes to the chip for AID 0. Its response carries no status read, so a
 * refusal (simnode_chip_refuse_next_arg(DISABLE_KEY, aid, ret)) fails the transport instead,
 * and the key stays. */
int mmdrv_disable_key(uint16_t vif_id, uint16_t aid, uint8_t hw_key_idx, bool is_pairwise)
{
    if (aid == 0u) { return 0; }
    struct simnode_chipcmd *c = chipcmd_log_(MORSE_CMD_ID_DISABLE_KEY, vif_id, hw_key_idx);
    c->aid = aid;
    c->pairwise = is_pairwise;
    c->ret = (int)chip_answer_(MORSE_CMD_ID_DISABLE_KEY, aid);
    if (c->ret != 0) { return c->ret; }
    s_cfg.keys_disabled++;
    const int i = chip_key_find_(aid, is_pairwise, hw_key_idx);
    if (i >= 0) { s_key[i].used = false; }
    return 0;
}

int mmdrv_set_bssid(uint16_t vif_id, const uint8_t bssid[6], int32_t *chip_status)
{
    struct simnode_chipcmd *c = chipcmd_log_(MORSE_CMD_ID_BSSID_SET, vif_id, 0);
    memcpy(c->addr, bssid, 6);
    c->status = chip_answer_(MORSE_CMD_ID_BSSID_SET, c->arg);
    if (chip_status != NULL) { *chip_status = c->status; }
    if (c->status == 0)
    {
        s_now.bssid_set = true;
        memcpy(s_now.bssid, bssid, 6);
    }
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

int mmdrv_update_sta_state_status(uint16_t vif_id, uint16_t aid, const uint8_t *addr,
                                  enum morse_sta_state state, int32_t *chip_status)
{
    struct simnode_chipcmd *c = chipcmd_log_(MORSE_CMD_ID_SET_STA_STATE, vif_id, (uint32_t)state);
    c->aid = aid;
    if (addr != NULL) { memcpy(c->addr, addr, 6); }
    c->status = chip_answer_(MORSE_CMD_ID_SET_STA_STATE, c->arg);
    if (chip_status != NULL) { *chip_status = c->status; }
    if (c->status == 0) { chip_sta_note_(aid, addr, (uint32_t)state); }
    s_cfg.sta_state_updates++;
    return 0;
}

/* As driver.c: without a status out, a refusal still returns 0. */
int mmdrv_update_sta_state(uint16_t vif_id, uint16_t aid, const uint8_t *addr,
                           enum morse_sta_state state)
{
    return mmdrv_update_sta_state_status(vif_id, aid, addr, state, NULL);
}

/* Driver calls a test asserts reach the chip only from the umac event loop, which runs a restart. */
static unsigned s_off_loop;
unsigned simnode_chip_calls_off_loop(void) { return s_off_loop; }
static void on_loop_only_(void) { s_off_loop += umac_core_evtloop_is_active(NULL) ? 0u : 1u; }

/* Host CCMP: the chip's one group-key slot is why warthog can do crypto in the
 * host at all, so the setting is honoured rather than ignored. */
int mmdrv_set_crypto_in_host(uint16_t vif_id, bool enable, uint32_t *out_value)
{
    (void)vif_id;
    on_loop_only_();
    s_cfg.crypto_in_host = enable;
    s_now.crypto_in_host_set = true;
    s_now.crypto_in_host = enable;
    if (out_value != NULL) { *out_value = enable ? 1u : 0u; }
    return 0;
}

int mmdrv_get_crypto_in_host(uint16_t vif_id, uint32_t *out_value)
{
    (void)vif_id;
    on_loop_only_();
    if (out_value != NULL) { *out_value = s_now.crypto_in_host ? 1u : 0u; }
    return 0;
}

/* ---- the chip VIF: what umac_interface.c sends (interface_seam.c) -------- */

const uint8_t *simnode_own_mac(void);   /* fake_config.c */
uint16_t simnode_own_vif_id(void);      /* fake_config.c */

/* A chip that boots holds nothing: no interface, channel setting, BSS, beaconing, mesh, station,
 * key (its group slot's PN included), fragmentation threshold or parameter. The firmware
 * reloads it the same way at a hardware restart (mmdrv_deinit, then mmdrv_init with no
 * chip_info), so a restart loses all of it too. */
static unsigned s_boots, s_restart_done;
static bool s_down;

int mmdrv_init(struct mmdrv_chip_info *chip_info, const char *country_code)
{
    (void)country_code;
    chip_tables_clear_();
    memset(&s_grp, 0, sizeof(s_grp));
    memset(&s_now, 0, sizeof(s_now));
    s_boots++;
    s_down = false;
    if (chip_info == NULL) { return 0; }
    memset(chip_info, 0, sizeof(*chip_info));
    memcpy(chip_info->mac_addr, simnode_own_mac(), 6);
    chip_info->fw_version.major = 1;
    chip_info->fw_version.minor = 17;
    chip_info->fw_version.patch = 6;
    chip_info->morse_chip_id = 0x0306;
    chip_info->morse_chip_id_string = "MM6108-A2";
    return 0;
}

/* As driver.c: its queues are purged with no TX status (morse_skbq_finish). */
void mmdrv_deinit(void)
{
    while (s_held_n != 0u)
    {
        unsigned idx = 0;
        mmpkt_release(take_held_(0, &idx));
    }
    s_down = true;
}

/* As driver.c: the TX pause the health check set (MMDRV_PAUSE_SOURCE_MASK_HW_RESTART) ends. */
static void (*s_restart_done_hook)(void);
void simnode_chip_on_restart_done(void (*cb)(void)) { s_restart_done_hook = cb; }

void mmdrv_hw_restart_completed(void)
{
    s_restart_done++;
    mmdrv_host_set_tx_paused(MMDRV_PAUSE_SOURCE_MASK_HW_RESTART, false);
    void (*cb)(void) = s_restart_done_hook;
    s_restart_done_hook = NULL;
    if (cb != NULL) { cb(); }
}

/* As driver.c sends it: 0 is off. */
int mmdrv_set_frag_threshold(uint32_t frag_threshold)
{
    s_now.frag_threshold = frag_threshold;
    return 0;
}

const struct simnode_chipstate *simnode_chipstate(void)
{
    s_now.boots = s_boots;
    s_now.restarts_done = s_restart_done;
    s_now.down = s_down;
    s_now.stas = 0;
    s_now.keys = 0;
    for (unsigned i = 0; i < SIMNODE_CHIP_STA_MAX; i++) { s_now.stas += s_sta[i].used; }
    for (unsigned i = 0; i < SIMNODE_CHIP_KEY_MAX; i++) { s_now.keys += s_key[i].used; }
    return &s_now;
}

int simnode_chip_sta_state(uint16_t aid, uint8_t addr[6])
{
    for (unsigned i = 0; i < SIMNODE_CHIP_STA_MAX; i++)
    {
        if (s_sta[i].used && s_sta[i].aid == aid)
        {
            if (addr != NULL) { memcpy(addr, s_sta[i].addr, 6); }
            return (int)s_sta[i].state;
        }
    }
    return -1;
}

unsigned simnode_chip_keys(struct simnode_keyinst *out, unsigned max)
{
    unsigned n = 0;
    for (unsigned i = 0; i < SIMNODE_CHIP_KEY_MAX && n < max; i++)
    {
        if (!s_key[i].used) { continue; }
        memset(&out[n], 0, sizeof(out[n]));
        out[n].aid = s_key[i].aid;
        out[n].pairwise = s_key[i].pairwise;
        out[n].key_idx = s_key[i].idx;
        memcpy(out[n].key, s_key[i].key, 16);
        out[n].tx_pn = s_key[i].next_pn;
        n++;
    }
    return n;
}

/* The board's HAL has no MAC override: the chip's own address stands. */
void mmhal_read_mac_addr(uint8_t *mac_addr) { (void)mac_addr; }

/* As driver.c: the transport result, and the VIF id from the response header, which a
 * refusing chip leaves at the request's UNKNOWN_VIF_ID. */
static int chip_add_if_(uint16_t *vif_id, const uint8_t *addr, enum mmdrv_interface_type type,
                        int32_t *status)
{
    struct simnode_chipcmd *c = chipcmd_log_(MORSE_CMD_ID_ADD_INTERFACE, UINT16_MAX, (uint32_t)type);
    memcpy(c->addr, addr, 6);
    if (s_refuse_add_n != 0u && (s_refuse_add[0].type == 0u || s_refuse_add[0].type == (uint32_t)type))
    {
        c->ret = s_refuse_add[0].ret;
        c->status = s_refuse_add[0].ret == 0 ? s_refuse_add[0].status : 0;
        memmove(&s_refuse_add[0], &s_refuse_add[1], (--s_refuse_add_n) * sizeof(s_refuse_add[0]));
    }
    if (status != NULL) { *status = c->status; }
    if (c->ret != 0) { return c->ret; }
    if (c->status != 0)
    {
        c->vif_id = UINT16_MAX;
        return 0;
    }
    c->vif_id = (type == MMDRV_INTERFACE_TYPE_MESH && s_mesh_vif.set) ? s_mesh_vif.id
                                                                       : simnode_own_vif_id();
    *vif_id = c->vif_id;
    s_vif_type = (uint32_t)type;
    s_now.vif = true;
    s_now.vif_type = (uint32_t)type;
    s_now.vif_id = c->vif_id;
    return 0;
}

int mmdrv_add_if(uint16_t *vif_id, const uint8_t *addr, enum mmdrv_interface_type type)
{
    /* As driver.c: without a status out, a refused add still reads the response's VIF id. */
    int32_t st = 0;
    int ret = chip_add_if_(vif_id, addr, type, &st);
    if (ret == 0 && st != 0) { *vif_id = UINT16_MAX; }
    return ret;
}

int mmdrv_add_if_status(uint16_t *vif_id, const uint8_t *addr, enum mmdrv_interface_type type,
                        int32_t *chip_status)
{
    return chip_add_if_(vif_id, addr, type, chip_status);
}

int mmdrv_rm_if(uint16_t vif_id)
{
    struct simnode_chipcmd *c = chipcmd_log_(MORSE_CMD_ID_REMOVE_INTERFACE, vif_id, vif_id);
    c->ret = (int)chip_answer_(MORSE_CMD_ID_REMOVE_INTERFACE, c->arg);
    if (c->ret == 0 && s_now.vif && s_now.vif_id == vif_id) { s_now.vif = false; }
    return c->ret;
}

int mmdrv_get_capabilities_status(uint16_t vif_id, struct morse_caps *caps, int32_t *chip_status)
{
    (void)chipcmd_log_(MORSE_CMD_ID_GET_CAPABILITIES, vif_id, 0);
    if (chip_status != NULL) { *chip_status = 0; }
    memset(caps, 0, sizeof(*caps));
    return 0;
}

int mmdrv_get_capabilities(uint16_t vif_id, struct morse_caps *caps)
{
    return mmdrv_get_capabilities_status(vif_id, caps, NULL);
}

/* Per-VIF and global settings umac_interface.c sends at every add: accepted; the TX status flush
 * watermark is kept until the chip boots. */
int mmdrv_set_param(uint16_t vif_id, enum morse_param_id param_id, uint32_t value)
{
    (void)vif_id;
    if (param_id == MORSE_PARAM_ID_TX_STATUS_FLUSH_WATERMARK) { s_now.flush_wm = value; }
    return 0;
}

/* Setting the interval runs a check at once (driver_health_request_check); an armed failure fails
 * it, and the health task posts a restart as morse_reset_chip does. */
static bool s_fail_health;
void simnode_chip_fail_next_health_check(void) { s_fail_health = true; }
void mmdrv_host_hw_restart_required(void);
void mmdrv_host_set_tx_paused(uint16_t sources_mask, bool paused);

int mmdrv_set_health_check_interval(uint32_t min_interval_ms, uint32_t max_interval_ms)
{
    (void)max_interval_ms;
    s_now.health_ms = min_interval_ms;
    if (s_fail_health && !s_down)
    {
        s_fail_health = false;
        mmdrv_host_set_tx_paused(MMDRV_PAUSE_SOURCE_MASK_HW_RESTART, true);
        mmdrv_host_hw_restart_required();
    }
    return 0;
}

int mmdrv_set_dynamic_ps_timeout(uint32_t timeout_ms)
{
    s_now.dyn_ps_sets++;
    s_now.dyn_ps_ms = timeout_ms;
    return 0;
}

/* AT+CHIPRESTART (driver.c, then driver_health.c's task): the next check fails without asking the
 * chip, counted forced, and the task restarts it as after a real failure; refused while stopped. */
extern volatile uint32_t g_warthog_chiprestart_forced;
int mmdrv_force_health_check_fail(void)
{
    on_loop_only_();
    if (s_down) { return -19; /* -ENODEV */ }
    g_warthog_chiprestart_forced++;
    mmdrv_host_set_tx_paused(MMDRV_PAUSE_SOURCE_MASK_HW_RESTART, true);
    mmdrv_host_hw_restart_required();
    return 0;
}

int mmdrv_set_ndp_probe(uint16_t vif_id, bool enabled)
{
    (void)vif_id; (void)enabled;
    return 0;
}

int mmdrv_set_listen_interval_sleep(uint16_t vif, uint16_t listen_interval)
{
    (void)vif; (void)listen_interval;
    return 0;
}

int mmdrv_set_control_response_bw(uint16_t vif, enum mmdrv_direction direction, bool cr_1mhz_en)
{
    (void)vif; (void)direction; (void)cr_1mhz_en;
    return 0;
}

int mmdrv_cfg_scan(bool enabled)
{
    (void)enabled;
    return 0;
}

/* The channel path is stubbed above the chip (the generated
 * umac_interface_set_channel_from_regdb); these only satisfy the real file's link. */
int mmdrv_set_channel(uint32_t op_chan_freq_hz, uint8_t pri_1mhz_chan_idx, uint8_t op_bw_mhz,
                      uint8_t pri_bw_mhz, bool is_off_channel)
{
    (void)op_chan_freq_hz; (void)pri_1mhz_chan_idx; (void)op_bw_mhz; (void)pri_bw_mhz;
    (void)is_off_channel;
    return 0;
}

int mmdrv_set_txpower(int32_t *out_power_dbm, int txpower_dbm)
{
    if (out_power_dbm != NULL) { *out_power_dbm = txpower_dbm; }
    return 0;
}

int mmdrv_set_duty_cycle(uint32_t duty_cycle, bool duty_cycle_omit_ctrl_resp,
                         enum mmwlan_duty_cycle_mode mode)
{
    (void)duty_cycle; (void)duty_cycle_omit_ctrl_resp; (void)mode;
    return 0;
}

int mmdrv_cfg_mpsw(uint32_t airtime_min_us, uint32_t airtime_max_us,
                   uint32_t packet_space_window_length_us)
{
    (void)airtime_min_us; (void)airtime_max_us; (void)packet_space_window_length_us;
    return 0;
}
