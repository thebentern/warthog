/*
 * The chip, replaced. THIS FILE IS THE PHY CUT LINE.
 *
 * mmdrv_tx_frame() is where the firmware hands a finished 802.11 frame to the
 * MM6108 over SDIO. Here it appends that frame to an outbox instead, so a test
 * can assert on the exact bytes that would have gone on the air -- addresses,
 * QoS control, Mesh Control, Address Extension, the lot.
 *
 * Everything above this line is the real firmware. Below it only three things
 * the host sees back are modelled: a TX status per data frame (and per management
 * frame under our group key), the group key slot's TX PN, drawn when a frame is sent,
 * and, for a frame a test passes through simnode_rx_air, which key the chip opens a
 * Protected frame under. No modulation, no timing, no interference. What the radio
 * does with these bytes is exactly what the simulator cannot tell you.
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
        s_grp.draws++;
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
    /* Management frames' status feeds hostap and rate control, neither linked: freed as sent.
     * Not one under our group key (group path selection): it draws that slot's PN, as data does. */
    if (is_mgmt && mmdrv_get_tx_metadata(pkt)->mesh.own_group == 0u)
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

/* Interface, BSS and station commands, in the order the firmware sent them, each with
 * the status this chip answered. A refusal is armed per command, as a firmware that does
 * not support something answers it. */
#define SIMNODE_CHIPCMD_MAX 64u
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
    s_cfg.mesh_config++;
    s_cfg.mesh_started = start;
    s_cfg.beaconing_enabled = enable_beaconing;
    return 0;
}

int mmdrv_cfg_bss(uint16_t vif_id, uint16_t beacon_int, uint16_t dtim_period, uint32_t cssid)
{
    (void)dtim_period; (void)cssid;
    (void)chipcmd_log_(MORSE_CMD_ID_BSS_CONFIG, vif_id, beacon_int);
    s_cfg.bss_cfg++;
    return 0;
}

int mmdrv_cfg_bss_beacon(uint16_t vif_id, bool enable, int32_t *chip_status)
{
    struct simnode_chipcmd *c = chipcmd_log_(MORSE_CMD_ID_BSS_BEACON_CONFIG, vif_id, enable ? 1u : 0u);
    c->status = chip_answer_(MORSE_CMD_ID_BSS_BEACON_CONFIG, c->arg);
    if (chip_status != NULL) { *chip_status = c->status; }
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
static struct { bool used; uint16_t aid; bool pairwise; uint8_t idx; uint8_t key[16]; } s_key[SIMNODE_CHIP_KEY_MAX];
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

static void chip_key_put_(uint16_t aid, bool pairwise, uint8_t idx, const uint8_t key[16])
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
        chip_key_put_(aid, key_conf->is_pairwise, key_conf->key_idx, key_conf->key);
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

/* ---- the chip VIF: what umac_interface.c sends (interface_seam.c) -------- */

const uint8_t *simnode_own_mac(void);   /* fake_config.c */
uint16_t simnode_own_vif_id(void);      /* fake_config.c */

int mmdrv_init(struct mmdrv_chip_info *chip_info, const char *country_code)
{
    (void)country_code;
    chip_tables_clear_(); /* a chip that boots holds no station and no key */
    memset(chip_info, 0, sizeof(*chip_info));
    memcpy(chip_info->mac_addr, simnode_own_mac(), 6);
    chip_info->fw_version.major = 1;
    chip_info->fw_version.minor = 17;
    chip_info->fw_version.patch = 6;
    chip_info->morse_chip_id = 0x0306;
    chip_info->morse_chip_id_string = "MM6108-A2";
    return 0;
}

void mmdrv_deinit(void) {}

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

/* Per-VIF and global settings umac_interface.c sends at every add: accepted, not modelled. */
int mmdrv_set_param(uint16_t vif_id, enum morse_param_id param_id, uint32_t value)
{
    (void)vif_id; (void)param_id; (void)value;
    return 0;
}

int mmdrv_set_health_check_interval(uint32_t min_interval_ms, uint32_t max_interval_ms)
{
    (void)min_interval_ms; (void)max_interval_ms;
    return 0;
}

int mmdrv_set_dynamic_ps_timeout(uint32_t timeout_ms)
{
    (void)timeout_ms;
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
