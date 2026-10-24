#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mmwlan.h"

/* The region a board with none stored runs. */
#define WARTHOG_DEFAULT_REGION "US"
/* Rows a plan can hold: more than any regdb country has in the module's band. */
#define WARTHOG_PLAN_MAX_ROWS 64

/* AT+MESHCHAN's set, stored and read as one blob (cfg.c). */
struct warthog_mesh_chan {
    uint32_t freq_hz;
    uint16_t chan;
    uint8_t  global_op_class;
    uint8_t  op_class;
    uint8_t  bw_mhz;
};

enum warthog_rg_state { WARTHOG_RG_ABSENT, WARTHOG_RG_VALID, WARTHOG_RG_INVALID };

/* The stored region as cfg.c read it; raw is a printable copy of an invalid value. */
struct warthog_rg_stored {
    enum warthog_rg_state state;
    char cc[3];
    char raw[16];
    bool unreadable;   /* NVS refused regnvs (reads invalid); only AT+REGION= reformats it */
    bool no_partition; /* the partition table has no regnvs */
};

/* What the allowed list is decided from: the regdb, the linked BCF, the HW list, the rated band. */
struct warthog_region_ctx {
    const struct mmwlan_regulatory_db *db;
    const uint8_t *bcf;
    size_t bcf_len;
    const char *hw;
    uint32_t band_lo_hz, band_hi_hz;
};

enum warthog_cc_src { WARTHOG_CC_NONE, WARTHOG_CC_STORED, WARTHOG_CC_DEFAULT };
enum warthog_hold {
    WARTHOG_HOLD_NONE,
    WARTHOG_HOLD_REGION_INVALID,
    WARTHOG_HOLD_REGION_REFUSED,
    WARTHOG_HOLD_DEFAULT_REFUSED,
    WARTHOG_HOLD_NO_MESH_DEFAULT,
};
enum warthog_chan_src { WARTHOG_CHAN_HELD, WARTHOG_CHAN_FULL_LIST, WARTHOG_CHAN_STORED, WARTHOG_CHAN_DEFAULT };
#define WARTHOG_CHAN_F_STORED_REFUSED 1u
#define WARTHOG_CHAN_F_CLASSES_FIXED  2u
#define WARTHOG_CHAN_F_WARNING_ROW    4u

/* The boot's decision. list points at out (rows copied from the regdb) or is NULL when held. */
struct warthog_region_plan {
    char cc[3];
    enum warthog_cc_src cc_src;
    enum warthog_hold hold;
    enum warthog_chan_src chan_src;
    unsigned chan_flags;
    const struct mmwlan_s1g_channel_list *country;
    const struct mmwlan_s1g_channel_list *list;
    struct mmwlan_s1g_channel_list out;
    struct mmwlan_s1g_channel rows[WARTHOG_PLAN_MAX_ROWS];
};

/* Two ASCII letters and nothing else, upper-cased into out; out may alias in. */
bool warthog_region_normalize(const char *in, char out[3]);
/* The BCF has a regdom section for cc that the driver reaches, walking it as firmware_mbin.c does. */
bool warthog_region_bcf_has(const uint8_t *bcf, size_t len, const char *cc);
/* The row's occupied band (centre +/- bw/2) lies inside the module's rated band. */
bool warthog_region_in_band(const struct warthog_region_ctx *ctx, const struct mmwlan_s1g_channel *row);
/* The in-band rows of list, in order: all are counted, the first max copied to out (may be NULL). */
unsigned warthog_region_band_rows(const struct warthog_region_ctx *ctx, const struct mmwlan_s1g_channel_list *list,
                                  struct mmwlan_s1g_channel *out, unsigned max);
/* Offered: in the HW list, the regdb and the BCF, with 1..WARTHOG_PLAN_MAX_ROWS in-band rows. */
bool warthog_region_allowed(const struct warthog_region_ctx *ctx, const char *cc);
/* The offered codes in regdb order joined by sep; always terminated, returns the length needed. */
size_t warthog_region_allowed_list(const struct warthog_region_ctx *ctx, char sep, char *out, size_t out_len);
/* The region a boot with this stored state runs, into cc (empty when it holds). */
enum warthog_hold warthog_region_resolve(const struct warthog_region_ctx *ctx, const struct warthog_rg_stored *st,
                                         char cc[3], enum warthog_cc_src *src);
/* Boot's rule: the in-band row equal in frequency, channel and bandwidth; flags say what differed. */
const struct mmwlan_s1g_channel *warthog_region_match(const struct warthog_region_ctx *ctx,
                                                      const struct mmwlan_s1g_channel_list *list,
                                                      const struct warthog_mesh_chan *mc, unsigned *flags);
/* AT+MESHCHAN='s rule: all five equal an in-band row Morse does not flag; why says otherwise. */
bool warthog_region_check_set(const struct warthog_region_ctx *ctx, const struct mmwlan_s1g_channel_list *list,
                              const struct warthog_mesh_chan *mc, char *why, size_t why_len);
/* A row mmregdb.c marks "regulatory requirements may not be met". */
bool warthog_region_is_warning_row(const uint8_t *cc, unsigned chan, unsigned bw);
/* The country's default mesh row inside list, or NULL when it has none in band. */
const struct mmwlan_s1g_channel *warthog_region_default_row(const struct warthog_region_ctx *ctx,
                                                            const struct mmwlan_s1g_channel_list *list);
/* The whole decision: country, then the channel list the chip gets, or a hold. mc may be NULL. */
void warthog_region_plan(const struct warthog_region_ctx *ctx, const struct warthog_rg_stored *st, bool mesh_on,
                         const struct warthog_mesh_chan *mc, struct warthog_region_plan *plan);
/* A row of list carries a duty cycle under 100% or a packet spacing window (JP, KR). */
bool warthog_region_restricted(const struct mmwlan_s1g_channel_list *list);
/* warthog_region_plan over the four hooks below. */
void warthog_region_boot_plan(struct warthog_region_plan *plan);
/* Why a hold holds, in words, and as one token. */
const char *warthog_region_hold_text(enum warthog_hold hold);
const char *warthog_region_hold_token(enum warthog_hold hold);
/* The channel's source with its flags, e.g. "stored+classes-fixed"; returns the length needed. */
size_t warthog_region_chan_src_text(const struct warthog_region_plan *plan, char *out, size_t len);
/* The NOT PEERED report's region ("AU (stored)", "none (held: ...)") and, only when the stored set
 * was refused, the line saying so (else ""). plan may be NULL. */
void warthog_region_report_text(const struct warthog_region_plan *plan, char *region, size_t rlen, char *refused,
                                size_t flen);

/* Hooks region.c calls: cfg.c and halow.c on the board, fakes on the host. */
void warthog_cfg_get_region(struct warthog_rg_stored *out);
uint8_t warthog_cfg_get_mesh_enable(void);
bool warthog_cfg_get_mesh_chan(struct warthog_mesh_chan *out);
struct warthog_region_ctx warthog_region_ctx_get(void);
