#include "region.h"

#include <stdio.h>
#include <string.h>

#include "mbin.h"

/* Default mesh row per country (chan, bw): 2 MHz, in band, no warning row, no BCF edge entry. */
static const struct { char cc[3]; uint8_t chan, bw; } k_default[] = {
    { "AU", 41, 2 }, { "JP", 2, 2 }, { "KR", 10, 2 }, { "NZ", 38, 2 }, { "US", 42, 2 },
};

/* mmregdb.c's rows marked "Warning: regulatory requirements may not be met" (chan, bw). */
static const struct { char cc[3]; uint8_t chan, bw; } k_warning[] = {
    { "CA", 1, 1 },  { "CA", 2, 2 },  { "KR", 18, 1 }, { "KR", 20, 1 }, { "KR", 22, 1 },
    { "KR", 24, 1 }, { "KR", 19, 2 }, { "KR", 23, 2 }, { "US", 1, 1 },  { "US", 2, 2 },
};

static bool letter_(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static char upper_(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

bool warthog_region_normalize(const char *in, char out[3])
{
    if (in == NULL || out == NULL) {
        return false;
    }
    const char a = in[0];
    if (!letter_(a)) {
        return false;
    }
    const char b = in[1];
    if (!letter_(b) || in[2] != '\0') {
        return false;
    }
    out[0] = upper_(a);
    out[1] = upper_(b);
    out[2] = '\0';
    return true;
}

static uint16_t le16_(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

bool warthog_region_bcf_has(const uint8_t *bcf, size_t len, const char *cc)
{
    if (bcf == NULL || cc == NULL) {
        return false;
    }
    if (len < 8 || le16_(bcf) != FIELD_TYPE_MAGIC || le16_(bcf + 2) != 4 ||
        (uint32_t)(bcf[4] | (bcf[5] << 8) | (bcf[6] << 16) | ((uint32_t)bcf[7] << 24)) != MBIN_BCF_MAGIC_NUMBER) {
        return false;
    }
    size_t off = 8;
    if (len - off < 4 || le16_(bcf + off) != FIELD_TYPE_BCF_BOARD_CONFIG ||
        len - off - 4 < le16_(bcf + off + 2)) {
        return false;
    }
    off += 4u + le16_(bcf + off + 2);
    for (;;) {
        if (len - off < 4) {
            return false;
        }
        const uint16_t type = le16_(bcf + off), tlen = le16_(bcf + off + 2);
        if (len - off - 4 < tlen) {
            return false;
        }
        const uint8_t *p = bcf + off + 4;
        if (type == FIELD_TYPE_EOF) {
            return false;
        }
        if (type == FIELD_TYPE_BCF_REGDOM) {
            /* The driver asserts every regdom it walks is longer than its header. */
            if (tlen <= sizeof(struct mbin_regdom_hdr)) {
                return false;
            }
            if (p[0] == (uint8_t)cc[0] && p[1] == (uint8_t)cc[1]) {
                return true;
            }
        }
        off += 4u + tlen;
    }
}

bool warthog_region_in_band(const struct warthog_region_ctx *ctx, const struct mmwlan_s1g_channel *row)
{
    if (ctx == NULL || row == NULL || row->bw_mhz == 0) {
        return false;
    }
    const uint64_t half = (uint64_t)row->bw_mhz * 500000u;
    const uint64_t c = row->centre_freq_hz;
    return c >= half && c - half >= ctx->band_lo_hz && c + half <= ctx->band_hi_hz;
}

unsigned warthog_region_band_rows(const struct warthog_region_ctx *ctx, const struct mmwlan_s1g_channel_list *list,
                                  struct mmwlan_s1g_channel *out, unsigned max)
{
    unsigned n = 0;
    if (list == NULL || list->channels == NULL) {
        return 0;
    }
    for (unsigned i = 0; i < list->num_channels; i++) {
        if (!warthog_region_in_band(ctx, &list->channels[i])) {
            continue;
        }
        if (out != NULL && n < max) {
            out[n] = list->channels[i];
        }
        n++;
    }
    return n;
}

static bool hw_has_(const char *hw, const char *cc)
{
    if (hw == NULL) {
        return false;
    }
    for (const char *p = hw; *p != '\0';) {
        const char *e = strchr(p, ',');
        const size_t n = e != NULL ? (size_t)(e - p) : strlen(p);
        if (n == 2 && p[0] == cc[0] && p[1] == cc[1]) {
            return true;
        }
        if (e == NULL) {
            break;
        }
        p = e + 1;
    }
    return false;
}

bool warthog_region_allowed(const struct warthog_region_ctx *ctx, const char *cc)
{
    char n[3];
    if (ctx == NULL || !warthog_region_normalize(cc, n) || !hw_has_(ctx->hw, n)) {
        return false;
    }
    const struct mmwlan_s1g_channel_list *list = mmwlan_lookup_regulatory_domain(ctx->db, n);
    if (list == NULL || !warthog_region_bcf_has(ctx->bcf, ctx->bcf_len, n)) {
        return false;
    }
    const unsigned rows = warthog_region_band_rows(ctx, list, NULL, 0);
    return rows >= 1 && rows <= WARTHOG_PLAN_MAX_ROWS;
}

size_t warthog_region_allowed_list(const struct warthog_region_ctx *ctx, char sep, char *out, size_t out_len)
{
    size_t need = 0;
    if (out != NULL && out_len > 0) {
        out[0] = '\0';
    }
    if (ctx == NULL || ctx->db == NULL) {
        return 0;
    }
    for (unsigned i = 0; i < ctx->db->num_domains; i++) {
        const uint8_t *c = ctx->db->domains[i]->country_code;
        const char cc[3] = { (char)c[0], (char)c[1], '\0' };
        if (!warthog_region_allowed(ctx, cc)) {
            continue;
        }
        const char piece[4] = { sep, cc[0], cc[1], '\0' };
        const char *add = need == 0 ? piece + 1 : piece;
        for (const char *a = add; *a != '\0'; a++, need++) {
            if (out != NULL && need + 1 < out_len) {
                out[need] = *a;
                out[need + 1] = '\0';
            }
        }
    }
    return need;
}

enum warthog_hold warthog_region_resolve(const struct warthog_region_ctx *ctx, const struct warthog_rg_stored *st,
                                         char cc[3], enum warthog_cc_src *src)
{
    cc[0] = '\0';
    *src = WARTHOG_CC_NONE;
    if (st == NULL || st->state == WARTHOG_RG_INVALID) {
        return WARTHOG_HOLD_REGION_INVALID;
    }
    if (st->state == WARTHOG_RG_ABSENT) {
        if (!warthog_region_allowed(ctx, WARTHOG_DEFAULT_REGION)) {
            return WARTHOG_HOLD_DEFAULT_REFUSED;
        }
        memcpy(cc, WARTHOG_DEFAULT_REGION, 3);
        *src = WARTHOG_CC_DEFAULT;
        return WARTHOG_HOLD_NONE;
    }
    if (st->state != WARTHOG_RG_VALID) {
        return WARTHOG_HOLD_REGION_INVALID;
    }
    if (!warthog_region_allowed(ctx, st->cc)) {
        return WARTHOG_HOLD_REGION_REFUSED;
    }
    (void)warthog_region_normalize(st->cc, cc);
    *src = WARTHOG_CC_STORED;
    return WARTHOG_HOLD_NONE;
}

bool warthog_region_is_warning_row(const uint8_t *cc, unsigned chan, unsigned bw)
{
    if (cc == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(k_warning) / sizeof(k_warning[0]); i++) {
        if ((uint8_t)k_warning[i].cc[0] == cc[0] && (uint8_t)k_warning[i].cc[1] == cc[1] &&
            k_warning[i].chan == chan && k_warning[i].bw == bw) {
            return true;
        }
    }
    return false;
}

static const struct mmwlan_s1g_channel *find_(const struct mmwlan_s1g_channel_list *list,
                                              const struct warthog_mesh_chan *mc)
{
    if (list == NULL || mc == NULL) {
        return NULL;
    }
    for (unsigned i = 0; i < list->num_channels; i++) {
        const struct mmwlan_s1g_channel *r = &list->channels[i];
        if (r->centre_freq_hz == mc->freq_hz && r->s1g_chan_num == mc->chan && r->bw_mhz == mc->bw_mhz) {
            return r;
        }
    }
    return NULL;
}

const struct mmwlan_s1g_channel *warthog_region_match(const struct warthog_region_ctx *ctx,
                                                      const struct mmwlan_s1g_channel_list *list,
                                                      const struct warthog_mesh_chan *mc, unsigned *flags)
{
    const struct mmwlan_s1g_channel *r = find_(list, mc);
    if (r == NULL || !warthog_region_in_band(ctx, r)) {
        return NULL;
    }
    if (flags != NULL) {
        if (r->global_operating_class != mc->global_op_class || r->s1g_operating_class != mc->op_class) {
            *flags |= WARTHOG_CHAN_F_CLASSES_FIXED;
        }
        if (warthog_region_is_warning_row(list->country_code, r->s1g_chan_num, r->bw_mhz)) {
            *flags |= WARTHOG_CHAN_F_WARNING_ROW;
        }
    }
    return r;
}

bool warthog_region_check_set(const struct warthog_region_ctx *ctx, const struct mmwlan_s1g_channel_list *list,
                              const struct warthog_mesh_chan *mc, char *why, size_t why_len)
{
    char w[96];
    const struct mmwlan_s1g_channel *r = find_(list, mc);
    const char *cc = list != NULL ? (const char *)list->country_code : "??";
    if (r == NULL) {
        snprintf(w, sizeof(w), "not a row of the %.2s table", cc);
    } else if (!warthog_region_in_band(ctx, r)) {
        snprintf(w, sizeof(w), "%.2s ch%u @ %u MHz is outside this module's %lu-%lu MHz band", cc,
                 (unsigned)r->s1g_chan_num, (unsigned)r->bw_mhz, (unsigned long)(ctx->band_lo_hz / 1000000u),
                 (unsigned long)(ctx->band_hi_hz / 1000000u));
    } else if (r->global_operating_class != mc->global_op_class || r->s1g_operating_class != mc->op_class) {
        snprintf(w, sizeof(w), "%.2s ch%u @ %u MHz has classes %d,%d", cc, (unsigned)r->s1g_chan_num,
                 (unsigned)r->bw_mhz, (int)r->global_operating_class, (int)r->s1g_operating_class);
    } else if (warthog_region_is_warning_row(list->country_code, r->s1g_chan_num, r->bw_mhz)) {
        snprintf(w, sizeof(w), "%.2s ch%u @ %u MHz is flagged by Morse as possibly non-compliant", cc,
                 (unsigned)r->s1g_chan_num, (unsigned)r->bw_mhz);
    } else {
        return true;
    }
    if (why != NULL && why_len > 0) {
        snprintf(why, why_len, "%s", w);
    }
    return false;
}

const struct mmwlan_s1g_channel *warthog_region_default_row(const struct warthog_region_ctx *ctx,
                                                            const struct mmwlan_s1g_channel_list *list)
{
    if (list == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(k_default) / sizeof(k_default[0]); i++) {
        if ((uint8_t)k_default[i].cc[0] != list->country_code[0] ||
            (uint8_t)k_default[i].cc[1] != list->country_code[1]) {
            continue;
        }
        for (unsigned j = 0; j < list->num_channels; j++) {
            const struct mmwlan_s1g_channel *r = &list->channels[j];
            if (r->s1g_chan_num == k_default[i].chan && r->bw_mhz == k_default[i].bw) {
                return (warthog_region_in_band(ctx, r) &&
                        !warthog_region_is_warning_row(list->country_code, r->s1g_chan_num, r->bw_mhz))
                           ? r
                           : NULL;
            }
        }
        return NULL;
    }
    return NULL;
}

static void hold_(struct warthog_region_plan *plan, enum warthog_hold hold)
{
    plan->hold = hold;
    plan->cc[0] = '\0';
    plan->cc_src = WARTHOG_CC_NONE;
    plan->chan_src = WARTHOG_CHAN_HELD;
    plan->country = NULL;
    plan->list = NULL;
}

static void pin_(struct warthog_region_plan *plan, const struct mmwlan_s1g_channel *row, enum warthog_chan_src src)
{
    plan->rows[0] = *row;
    plan->out.num_channels = 1;
    plan->chan_src = src;
    plan->list = &plan->out;
}

void warthog_region_plan(const struct warthog_region_ctx *ctx, const struct warthog_rg_stored *st, bool mesh_on,
                         const struct warthog_mesh_chan *mc, struct warthog_region_plan *plan)
{
    memset(plan, 0, sizeof(*plan));
    const enum warthog_hold h = warthog_region_resolve(ctx, st, plan->cc, &plan->cc_src);
    if (h != WARTHOG_HOLD_NONE) {
        hold_(plan, h);
        return;
    }
    const struct mmwlan_s1g_channel_list *country = mmwlan_lookup_regulatory_domain(ctx->db, plan->cc);
    if (country == NULL) {
        hold_(plan, WARTHOG_HOLD_REGION_REFUSED);
        return;
    }
    plan->country = country;
    memcpy(plan->out.country_code, country->country_code, 2);
    plan->out.country_code[2] = '\0';
    plan->out.channels = plan->rows;
    if (mc != NULL) {
        unsigned f = 0;
        const struct mmwlan_s1g_channel *r = warthog_region_match(ctx, country, mc, &f);
        if (r != NULL) {
            plan->chan_flags = f;
            pin_(plan, r, WARTHOG_CHAN_STORED);
            return;
        }
        plan->chan_flags |= WARTHOG_CHAN_F_STORED_REFUSED;
    }
    if (mesh_on) {
        const struct mmwlan_s1g_channel *d = warthog_region_default_row(ctx, country);
        if (d == NULL) {
            hold_(plan, WARTHOG_HOLD_NO_MESH_DEFAULT);
            return;
        }
        pin_(plan, d, WARTHOG_CHAN_DEFAULT);
        return;
    }
    const unsigned n = warthog_region_band_rows(ctx, country, plan->rows, WARTHOG_PLAN_MAX_ROWS);
    plan->out.num_channels = n < WARTHOG_PLAN_MAX_ROWS ? n : WARTHOG_PLAN_MAX_ROWS;
    plan->chan_src = WARTHOG_CHAN_FULL_LIST;
    plan->list = &plan->out;
}

bool warthog_region_restricted(const struct mmwlan_s1g_channel_list *list)
{
    for (unsigned i = 0; list != NULL && i < list->num_channels; i++) {
        const struct mmwlan_s1g_channel *r = &list->channels[i];
        if (r->duty_cycle_sta < 10000 || r->pkt_spacing_us || r->airtime_min_us || r->airtime_max_us) {
            return true;
        }
    }
    return false;
}

void warthog_region_boot_plan(struct warthog_region_plan *plan)
{
    const struct warthog_region_ctx ctx = warthog_region_ctx_get();
    struct warthog_rg_stored st;
    warthog_cfg_get_region(&st);
    struct warthog_mesh_chan mc;
    const bool have = warthog_cfg_get_mesh_chan(&mc);
    warthog_region_plan(&ctx, &st, warthog_cfg_get_mesh_enable() != 0, have ? &mc : NULL, plan);
}

const char *warthog_region_hold_text(enum warthog_hold hold)
{
    switch (hold) {
    case WARTHOG_HOLD_NONE:
        return "not held";
    case WARTHOG_HOLD_REGION_INVALID:
        return "the stored region is not a valid code";
    case WARTHOG_HOLD_REGION_REFUSED:
        return "the stored region is not offered on this module";
    case WARTHOG_HOLD_DEFAULT_REFUSED:
        return "no region stored and the " WARTHOG_DEFAULT_REGION " default is not offered";
    case WARTHOG_HOLD_NO_MESH_DEFAULT:
        return "mesh is on and the region has no default mesh channel";
    }
    return "unknown";
}

const char *warthog_region_hold_token(enum warthog_hold hold)
{
    switch (hold) {
    case WARTHOG_HOLD_NONE:
        return "none";
    case WARTHOG_HOLD_REGION_INVALID:
        return "region-invalid";
    case WARTHOG_HOLD_REGION_REFUSED:
        return "region-refused";
    case WARTHOG_HOLD_DEFAULT_REFUSED:
        return "default-refused";
    case WARTHOG_HOLD_NO_MESH_DEFAULT:
        return "no-mesh-default";
    }
    return "unknown";
}

size_t warthog_region_chan_src_text(const struct warthog_region_plan *plan, char *out, size_t len)
{
    static const char *const base[] = { "held", "full-list", "stored", "default" };
    const unsigned s = plan != NULL ? (unsigned)plan->chan_src : 0u;
    const unsigned f = plan != NULL ? plan->chan_flags : 0u;
    const int n = snprintf(out, len, "%s%s%s%s", s < 4 ? base[s] : "unknown",
                           (f & WARTHOG_CHAN_F_STORED_REFUSED) ? "+stored-refused" : "",
                           (f & WARTHOG_CHAN_F_CLASSES_FIXED) ? "+classes-fixed" : "",
                           (f & WARTHOG_CHAN_F_WARNING_ROW) ? "+warning-row" : "");
    return n < 0 ? 0 : (size_t)n;
}

void warthog_region_report_text(const struct warthog_region_plan *plan, char *region, size_t rlen, char *refused,
                                size_t flen)
{
    const bool run = plan != NULL && plan->hold == WARTHOG_HOLD_NONE;
    if (!run) {
        snprintf(region, rlen, "none (%s%s)", plan != NULL ? "held: " : "no plan",
                 plan != NULL ? warthog_region_hold_token(plan->hold) : "");
    } else {
        snprintf(region, rlen, "%s (%s)", plan->cc,
                 plan->cc_src == WARTHOG_CC_STORED ? "stored" : "not set: " WARTHOG_DEFAULT_REGION " default");
    }
    if (run && (plan->chan_flags & WARTHOG_CHAN_F_STORED_REFUSED)) {
        snprintf(refused, flen, "stored AT+MESHCHAN is not in the %s table; the %s %s is in use", plan->cc, plan->cc,
                 plan->chan_src == WARTHOG_CHAN_DEFAULT ? "default" : "full list");
    } else if (flen > 0) {
        refused[0] = '\0';
    }
}
