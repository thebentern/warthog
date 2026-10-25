/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The runtime region decision (main/region.c), compiled with the real regulatory database
 * (mmregdb.c), the real linked BCF (region_bcf.h, bcf_fgh100mhaamd.mbin byte for byte) and the
 * module's HW list and rated band as sdkconfig.defaults sets them (the Makefile passes them in).
 * The four hooks region.c calls on the board (the stored region, mesh on/off, the stored
 * AT+MESHCHAN, the context) are faked here; everything that decides is the shipped code.
 *
 * What is offered: a country is offered only when it is in the HW list, in the regdb, has a
 * regdom section in the BCF (a stub counts: the FGH100M-H BCF has populated AU and US sections
 * and 16-byte stubs for EU IN JP KR NZ SG; CA and GB have none, and firmware init fails without
 * one), and has rows inside the module's 902-928 MHz band. So EU and IN (863-868 MHz) and CA/GB
 * are refused; AU JP KR NZ US are offered, and the offered list must equal the HW list.
 *
 * The BCF walk mirrors morse_bcf_load_mbin (firmware_mbin.c): magic, board_config, then TLVs up
 * to EOF; any regdom no longer than its 4-byte header asserts in the driver, so it refuses; the
 * first section for the country decides; EOF_WITH_SIGNATURE and other TLVs are skipped. Every
 * prefix of the real BCF is walked out of an exact-size heap buffer, so a read past the end is an
 * ASan error under SAN=1, not luck.
 *
 * Every channel list is cut to the rows whose occupied band (centre +/- bw/2) lies inside the
 * rated band: KR loses 928.5 and 929.5 MHz @ 1 MHz and 929 MHz @ 2 MHz, the rest keep every row.
 *
 * Boot's AT+MESHCHAN rule matches frequency, channel and bandwidth against an in-band row and
 * takes the classes from the row; AT+MESHCHAN= needs all five, in band, and not a row Morse marks
 * "regulatory requirements may not be met". Mesh on with nothing usable stored pins the
 * country's default row (US ch42, AU ch41; JP KR NZ by the rule checked here: the 2 MHz row
 * nearest the middle of the country's in-band span, lower frequency on a tie, else 1 MHz), never
 * index 0 of a full list; a country with no default holds the chip (test_region_au2020 builds
 * this file over the 2020 AU channelisation, which has no ch41 @ 2 MHz). Every pinned row is the
 * regdb row byte for byte; no default is an entry of its country's BCF section.
 *
 * A stored region that is invalid or not offered holds the chip in reset; only nothing stored
 * means US. JP's and KR's rows carry a duty cycle or a packet spacing window that only a channel
 * set programs; the set of offered countries with such rows is pinned so a new one is noticed.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mmregdb.h"
#include "region.h"
#include "region_bcf.h"

#ifndef HW_REGIONS
#error "the Makefile passes CONFIG_WARTHOG_HW_REGIONS from sdkconfig.defaults as HW_REGIONS"
#endif
#ifndef HW_BAND_LO_KHZ
#error "the Makefile passes CONFIG_WARTHOG_HW_BAND_LO_KHZ as HW_BAND_LO_KHZ"
#endif
#ifndef HW_BAND_HI_KHZ
#error "the Makefile passes CONFIG_WARTHOG_HW_BAND_HI_KHZ as HW_BAND_HI_KHZ"
#endif

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* ---- the four hooks ----------------------------------------------------- */

static struct warthog_rg_stored f_st;
static uint8_t f_mesh;
static bool f_have_mc;
static struct warthog_mesh_chan f_mc;
static struct warthog_region_ctx f_ctx;

void warthog_cfg_get_region(struct warthog_rg_stored *out) { *out = f_st; }
uint8_t warthog_cfg_get_mesh_enable(void) { return f_mesh; }
bool warthog_cfg_get_mesh_chan(struct warthog_mesh_chan *out)
{
    if (f_have_mc) { *out = f_mc; }
    return f_have_mc;
}
struct warthog_region_ctx warthog_region_ctx_get(void) { return f_ctx; }

/* ---- helpers ------------------------------------------------------------- */

static struct warthog_region_ctx ctx_(const char *hw, const uint8_t *bcf, size_t bcf_len, uint32_t lo_khz,
                                      uint32_t hi_khz)
{
    struct warthog_region_ctx c = {
        .db = get_regulatory_db(), .bcf = bcf, .bcf_len = bcf_len, .hw = hw,
        .band_lo_hz = lo_khz * 1000u, .band_hi_hz = hi_khz * 1000u,
    };
    return c;
}

/* The shipped context: real regdb, real BCF, the HW list and band from sdkconfig.defaults. */
static struct warthog_region_ctx real_(void)
{
    return ctx_(HW_REGIONS, k_region_bcf, sizeof(k_region_bcf), HW_BAND_LO_KHZ, HW_BAND_HI_KHZ);
}

static const struct mmwlan_s1g_channel_list *dom_(const char *cc)
{
    return mmwlan_lookup_regulatory_domain(get_regulatory_db(), cc);
}

static const struct mmwlan_s1g_channel *row_(const char *cc, unsigned chan, unsigned bw)
{
    const struct mmwlan_s1g_channel_list *l = dom_(cc);
    for (unsigned i = 0; l != NULL && i < l->num_channels; i++) {
        if (l->channels[i].s1g_chan_num == chan && l->channels[i].bw_mhz == bw) { return &l->channels[i]; }
    }
    return NULL;
}

static struct warthog_mesh_chan mc_(unsigned chan, uint32_t hz, unsigned g, unsigned s, unsigned bw)
{
    struct warthog_mesh_chan m = { .freq_hz = hz, .chan = (uint16_t)chan, .global_op_class = (uint8_t)g,
                                   .op_class = (uint8_t)s, .bw_mhz = (uint8_t)bw };
    return m;
}

static struct warthog_rg_stored st_(enum warthog_rg_state s, const char *cc)
{
    struct warthog_rg_stored r;
    memset(&r, 0, sizeof(r));
    r.state = s;
    if (cc != NULL) { snprintf(r.cc, sizeof(r.cc), "%s", cc); }
    return r;
}

/* Bytes appended to a BCF under construction. */
struct buf { uint8_t b[1024]; size_t n; };
static void put_(struct buf *o, const void *p, size_t n) { memcpy(o->b + o->n, p, n); o->n += n; }
static void tlv_(struct buf *o, uint16_t type, const uint8_t *p, uint16_t len)
{
    const uint8_t h[4] = { (uint8_t)type, (uint8_t)(type >> 8), (uint8_t)len, (uint8_t)(len >> 8) };
    put_(o, h, 4);
    if (len) { put_(o, p, len); }
}

/* The offset of cc's regdom TLV in the real BCF, walked independently of region.c. */
static size_t real_off_(const char *cc, uint16_t *len)
{
    size_t off = 8 + 4 + (size_t)(k_region_bcf[10] | (k_region_bcf[11] << 8));
    while (off + 4 <= sizeof(k_region_bcf)) {
        const uint16_t t = (uint16_t)(k_region_bcf[off] | (k_region_bcf[off + 1] << 8));
        const uint16_t l = (uint16_t)(k_region_bcf[off + 2] | (k_region_bcf[off + 3] << 8));
        if (t == 0x8101 && k_region_bcf[off + 4] == (uint8_t)cc[0] && k_region_bcf[off + 5] == (uint8_t)cc[1]) {
            *len = l;
            return off;
        }
        off += 4u + l;
    }
    return 0;
}

/* The real BCF with @p p (n bytes) inserted before the US section. */
static void real_with_before_us_(struct buf *o, const void *p, size_t n)
{
    uint16_t l = 0;
    const size_t us = real_off_("US", &l);
    o->n = 0;
    put_(o, k_region_bcf, us);
    put_(o, p, n);
    put_(o, k_region_bcf + us, sizeof(k_region_bcf) - us);
}

/* ---- normalize ------------------------------------------------------------ */

static bool norm_heap_(const char *s, size_t n, char out[3])
{
    char *h = malloc(n);
    memcpy(h, s, n);
    const bool r = warthog_region_normalize(h, out);
    free(h);
    return r;
}

static void t_normalize(void)
{
    printf("--- normalize: two ASCII letters, read only as far as needed ---\n");
    char o[3];
    CHECK(warthog_region_normalize("US", o) && strcmp(o, "US") == 0, "'US' -> US");
    CHECK(warthog_region_normalize("us", o) && strcmp(o, "US") == 0, "'us' -> US");
    CHECK(warthog_region_normalize("Us", o) && strcmp(o, "US") == 0, "'Us' -> US");
    static const char *bad[] = { "USA", "U1", "1U", "U ", " U", "\xc3\x9c", "U\n" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(!warthog_region_normalize(bad[i], o), "refused: '%s'", bad[i]);
    }
    CHECK(!warthog_region_normalize(NULL, o), "refused: NULL");
    CHECK(!norm_heap_("", 1, o), "refused: '' in a 1-byte heap buffer (nothing read past it)");
    CHECK(!norm_heap_("U", 2, o), "refused: 'U' in a 2-byte heap buffer (nothing read past it)");
    CHECK(norm_heap_("au", 3, o) && strcmp(o, "AU") == 0, "'au' in a 3-byte heap buffer -> AU");
    char a[3] = "nz";
    CHECK(warthog_region_normalize(a, a) && strcmp(a, "NZ") == 0, "out may alias in ('nz' -> NZ in place)");
}

/* ---- the BCF walk ---------------------------------------------------------- */

static bool has_(const struct buf *o, const char *cc) { return warthog_region_bcf_has(o->b, o->n, cc); }

static void t_bcf_walk(void)
{
    printf("--- the BCF walk matches the driver's, on the real bcf_fgh100mhaamd.mbin ---\n");
    static const char *present[] = { "AU", "EU", "IN", "JP", "KR", "NZ", "SG", "US" };
    for (unsigned i = 0; i < 8; i++) {
        CHECK(warthog_region_bcf_has(k_region_bcf, sizeof(k_region_bcf), present[i]),
              "%s has a section (AU and US populated, the rest 16-byte stubs: a stub counts)", present[i]);
    }
    static const char *absent[] = { "CA", "GB", "XX" };
    for (unsigned i = 0; i < 3; i++) {
        CHECK(!warthog_region_bcf_has(k_region_bcf, sizeof(k_region_bcf), absent[i]),
              "%s has no section: refused (firmware init fails without one)", absent[i]);
    }
    uint16_t l = 0;
    CHECK(real_off_("AU", &l) == 124 && l == 88 && real_off_("US", &l) == 336 && l == 144 &&
          real_off_("EU", &l) == 216 && l == 16,
          "the real BCF is the one the layout was read from (AU @124 len 88, EU stub @216 len 16, US @336 len 144)");

    struct buf o;
    static const uint8_t eof[4] = { 0x00, 0x8f, 0x00, 0x00 };
    real_with_before_us_(&o, eof, 4);
    CHECK(!has_(&o, "US") && has_(&o, "AU") && has_(&o, "SG"), "a section after EOF is not reached: US refused, AU and SG kept");

    uint8_t sig[12] = { 0x01, 0x8f, 8, 0 };
    real_with_before_us_(&o, sig, sizeof(sig));
    CHECK(has_(&o, "US"), "EOF_WITH_SIGNATURE is skipped as the driver skips it: US after it still counts");

    static const uint8_t hdr4[8] = { 0x01, 0x81, 4, 0, 'Z', 'Z', 0, 0 };
    real_with_before_us_(&o, hdr4, sizeof(hdr4));
    CHECK(!has_(&o, "US") && has_(&o, "AU"),
          "a regdom no longer than its header (len 4) before US: the driver asserts, so US is refused (AU, before it, kept)");
    static const uint8_t hdr5[9] = { 0x01, 0x81, 5, 0, 'Z', 'Z', 0, 0, 0 };
    real_with_before_us_(&o, hdr5, sizeof(hdr5));
    CHECK(has_(&o, "US"), "a len-5 regdom before US passes the driver's assert: US kept");

    static const uint8_t stub[20] = { 0x01, 0x81, 16, 0, 'U', 'S', 0, 0 };
    real_with_before_us_(&o, stub, sizeof(stub));
    CHECK(has_(&o, "US"), "the first US section decides, a stub included");

    o.n = 0;
    put_(&o, k_region_bcf, sizeof(k_region_bcf));
    o.b[4] ^= 1;
    CHECK(!has_(&o, "US") && !has_(&o, "AU"), "a bad magic refuses everything");
    o.b[4] ^= 1;
    o.b[8] = 0x02; /* board_config (0x8100) becomes board_desc (0x8102) */
    CHECK(!has_(&o, "US") && !has_(&o, "AU"), "no board_config first refuses everything");
    o.b[8] = 0x00;
    tlv_(&o, 0x8101, (const uint8_t *)"CA\0\0abcdefghijkl", 16);
    CHECK(!has_(&o, "CA"), "a CA section appended after the real EOF is not reached");
    CHECK(!warthog_region_bcf_has(NULL, 0, "US") && !warthog_region_bcf_has(k_region_bcf, sizeof(k_region_bcf), NULL),
          "NULL BCF or code: refused");
}

static void t_bcf_bounds(void)
{
    printf("--- the BCF walk never reads past the buffer (every prefix, exact-size heap copies) ---\n");
    unsigned wrong = 0, first = 0;
    for (size_t n = 0; n <= sizeof(k_region_bcf); n++) {
        uint8_t *h = malloc(n ? n : 1);
        memcpy(h, k_region_bcf, n);
        const bool us = warthog_region_bcf_has(h, n, "US"), au = warthog_region_bcf_has(h, n, "AU");
        const bool ca = warthog_region_bcf_has(h, n, "CA");
        free(h);
        if (us != (n >= 484) || au != (n >= 216) || ca) {
            if (!wrong++) { first = (unsigned)n; }
        }
    }
    CHECK(wrong == 0, "every prefix 0..%zu: US only once its whole section is in (484), AU at 216, CA never (%u wrong, first %u)",
          sizeof(k_region_bcf), wrong, first);
    uint16_t l = 0;
    const size_t au = real_off_("AU", &l);
    uint8_t *h = malloc(au + 4 + 8);
    memcpy(h, k_region_bcf, au + 4 + 8);
    h[au + 2] = 0xff; /* AU's length now runs past the end */
    CHECK(!warthog_region_bcf_has(h, au + 12, "AU"), "a section whose length runs past the end is refused");
    free(h);
}

/* ---- the band ---------------------------------------------------------------- */

static void t_band(void)
{
    printf("--- every list is cut to the module's %u-%u kHz band ---\n", (unsigned)HW_BAND_LO_KHZ, (unsigned)HW_BAND_HI_KHZ);
    const struct warthog_region_ctx c = real_();
    CHECK(HW_BAND_LO_KHZ == 902000 && HW_BAND_HI_KHZ == 928000, "sdkconfig.defaults rates the FGH100M-H at 902-928 MHz");
    struct mmwlan_s1g_channel r = { .centre_freq_hz = 927500000, .bw_mhz = 1 };
    CHECK(warthog_region_in_band(&c, &r), "927.5 MHz @ 1 MHz (927-928) is in: the edges are inclusive");
    r.centre_freq_hz = 902500000;
    CHECK(warthog_region_in_band(&c, &r), "902.5 MHz @ 1 MHz (902-903) is in");
    r.centre_freq_hz = 928500000;
    CHECK(!warthog_region_in_band(&c, &r), "928.5 MHz @ 1 MHz (928-929) is out");
    r.centre_freq_hz = 902000000;
    CHECK(!warthog_region_in_band(&c, &r), "902.0 MHz @ 1 MHz (901.5-902.5) is out");
    r.centre_freq_hz = 927000000; r.bw_mhz = 2;
    CHECK(warthog_region_in_band(&c, &r), "927 MHz @ 2 MHz (926-928) is in");
    r.bw_mhz = 4;
    CHECK(!warthog_region_in_band(&c, &r), "927 MHz @ 4 MHz (925-929) is out");
    r.bw_mhz = 0;
    CHECK(!warthog_region_in_band(&c, &r), "a 0 MHz row is never in band");

    static const struct { const char *cc; unsigned all, in; } want[] = {
#ifdef WANT_AU2020
        { "AU", 23, 23 },
#else
        { "AU", 26, 26 },
#endif
        { "JP", 12, 12 }, { "KR", 16, 13 }, { "NZ", 23, 23 }, { "US", 48, 48 }, { "CA", 48, 48 },
        { "EU", 7, 0 }, { "IN", 3, 0 }, { "GB", 9, 2 },
    };
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        const struct mmwlan_s1g_channel_list *l = dom_(want[i].cc);
        CHECK(l != NULL && l->num_channels == want[i].all && warthog_region_band_rows(&c, l, NULL, 0) == want[i].in,
              "%s: %u of %u rows in band", want[i].cc, want[i].in, want[i].all);
    }
    struct mmwlan_s1g_channel rows[WARTHOG_PLAN_MAX_ROWS];
    const unsigned n = warthog_region_band_rows(&c, dom_("KR"), rows, WARTHOG_PLAN_MAX_ROWS);
    bool kr = n == 13;
    for (unsigned i = 0; kr && i < n; i++) {
        kr = memcmp(&rows[i], &dom_("KR")->channels[i < 12 ? i : 14], sizeof(rows[i])) == 0;
    }
    CHECK(kr, "KR keeps rows 0-11 and 14 in order (928.5, 929.5 @ 1 and 929 @ 2 MHz dropped)");
    struct mmwlan_s1g_channel *three = malloc(3 * sizeof(*three));
    const unsigned n3 = warthog_region_band_rows(&c, dom_("US"), three, 3);
    CHECK(n3 == 48 && memcmp(three, dom_("US")->channels, 3 * sizeof(*three)) == 0,
          "a short copy into an exact 3-row heap buffer counts every in-band row (%u) and holds US rows 0-2", n3);
    free(three);
    memset(rows, 0xa5, 4 * sizeof(rows[0]));
    (void)warthog_region_band_rows(&c, dom_("US"), rows, 3);
    unsigned char spare[sizeof(rows[0])];
    memset(spare, 0xa5, sizeof(spare));
    CHECK(memcmp(&rows[3], spare, sizeof(spare)) == 0, "max 3 writes nothing past the third row");
    unsigned most = 0;
    for (unsigned i = 0; i < get_regulatory_db()->num_domains; i++) {
        const unsigned m = warthog_region_band_rows(&c, get_regulatory_db()->domains[i], NULL, 0);
        most = m > most ? m : most;
    }
    CHECK(most <= WARTHOG_PLAN_MAX_ROWS, "every regdb country's in-band list fits a plan (%u <= %u)", most,
          (unsigned)WARTHOG_PLAN_MAX_ROWS);
}

/* ---- what is offered ------------------------------------------------------------ */

static void t_allowed(void)
{
    printf("--- offered = HW list, regdb, BCF section and the band, together ---\n");
    char out[64];
    struct warthog_region_ctx c = real_();
    size_t n = warthog_region_allowed_list(&c, ',', out, sizeof(out));
    CHECK(strcmp(out, HW_REGIONS) == 0 && n == strlen(HW_REGIONS),
          "the shipped config offers exactly its HW list: '%s' (HW list '%s')", out, HW_REGIONS);
    CHECK(strcmp(HW_REGIONS, "AU,JP,KR,NZ,US") == 0, "sdkconfig.defaults' HW list is AU,JP,KR,NZ,US");
    (void)warthog_region_allowed_list(&c, '|', out, sizeof(out));
    CHECK(strcmp(out, "AU|JP|KR|NZ|US") == 0, "with '|': '%s'", out);

    c = ctx_("US,SG,GB,CA,IN,EU,NZ,KR,JP,AU", k_region_bcf, sizeof(k_region_bcf), 902000, 928000);
    (void)warthog_region_allowed_list(&c, ',', out, sizeof(out));
    CHECK(strcmp(out, "AU,JP,KR,NZ,US") == 0,
          "every regdb country in the HW list: still '%s' (EU, IN out of band; CA, GB no section; SG no regdb), in regdb order",
          out);
    c = ctx_("US", k_region_bcf, sizeof(k_region_bcf), 902000, 928000);
    (void)warthog_region_allowed_list(&c, ',', out, sizeof(out));
    CHECK(strcmp(out, "US") == 0, "HW list 'US': '%s' (the HW list filters)", out);
    c = ctx_("EU,IN,US", k_region_bcf, sizeof(k_region_bcf), 863000, 868000);
    (void)warthog_region_allowed_list(&c, ',', out, sizeof(out));
    CHECK(strcmp(out, "EU,IN") == 0, "a module rated 863-868 MHz with HW list EU,IN,US: '%s' (the band filters)", out);
    c = ctx_("CA,US", k_region_bcf, sizeof(k_region_bcf), 902000, 928000);
    CHECK(!warthog_region_allowed(&c, "CA") && warthog_region_allowed(&c, "us"),
          "CA in the HW list and the regdb, in band, but no BCF section: refused; 'us' allowed");
    c = ctx_("AUS,U,USA,US", k_region_bcf, sizeof(k_region_bcf), 902000, 928000);
    CHECK(!warthog_region_allowed(&c, "AU") && warthog_region_allowed(&c, "US"), "the HW list matches whole tokens only");
    c = ctx_(NULL, k_region_bcf, sizeof(k_region_bcf), 902000, 928000);
    CHECK(!warthog_region_allowed(&c, "US"), "no HW list: nothing offered");
    c = real_();
    CHECK(!warthog_region_allowed(&c, " US") && !warthog_region_allowed(&c, "SG"), "' US' and SG (no regdb row) refused");
    n = warthog_region_allowed_list(&c, ',', out, 6);
    CHECK(strcmp(out, "AU,JP") == 0 && n == 14, "a 6-byte buffer gets 'AU,JP' terminated; the full length (14) is returned");
    n = warthog_region_allowed_list(&c, ',', NULL, 0);
    CHECK(n == 14, "no buffer: the length alone (14)");
}

static void t_restricted(void)
{
    printf("--- offered countries whose rows only a channel set restricts ---\n");
    const struct warthog_region_ctx c = real_();
    char got[64] = "";
    const struct mmwlan_regulatory_db *db = get_regulatory_db();
    for (unsigned i = 0; i < db->num_domains; i++) {
        const struct mmwlan_s1g_channel_list *l = db->domains[i];
        const char cc[3] = { (char)l->country_code[0], (char)l->country_code[1], 0 };
        if (!warthog_region_allowed(&c, cc)) { continue; }
        bool r = false;
        for (unsigned j = 0; j < l->num_channels; j++) {
            const struct mmwlan_s1g_channel *x = &l->channels[j];
            if (warthog_region_in_band(&c, x) &&
                (x->duty_cycle_sta != 10000 || x->pkt_spacing_us || x->airtime_min_us || x->airtime_max_us)) {
                r = true;
            }
        }
        if (r) { strcat(got, got[0] ? "," : ""); strcat(got, cc); }
    }
    CHECK(strcmp(got, "JP,KR") == 0,
          "exactly JP and KR carry a duty cycle or packet spacing (%s): a scan programs them first (test_hw_scan)", got);
    char flagged[64] = "";
    for (unsigned i = 0; i < db->num_domains; i++) {
        const char cc[3] = { (char)db->domains[i]->country_code[0], (char)db->domains[i]->country_code[1], 0 };
        if (!warthog_region_allowed(&c, cc)) { continue; }
        for (unsigned mesh = 0; mesh < 2; mesh++) {
            struct warthog_rg_stored s = st_(WARTHOG_RG_VALID, cc);
            struct warthog_region_plan p;
            warthog_region_plan(&c, &s, mesh != 0, NULL, &p);
            if (warthog_region_restricted(p.list)) {
                snprintf(flagged + strlen(flagged), sizeof(flagged) - strlen(flagged), "%s%s/%s", flagged[0] ? "," : "", cc,
                         mesh ? "mesh" : "station");
            }
        }
    }
    CHECK(strcmp(flagged, "JP/station,JP/mesh,KR/station,KR/mesh") == 0 && !warthog_region_restricted(NULL),
          "warthog_region_restricted: the plans whose list rations airtime, so the mesh scan probe is skipped (%s)", flagged);
}

/* ---- resolve ----------------------------------------------------------------- */

static void t_resolve(void)
{
    printf("--- resolve: only nothing stored means US; anything else not offered holds ---\n");
    const struct warthog_region_ctx c = real_();
    char cc[3];
    enum warthog_cc_src src;
    struct warthog_rg_stored s = st_(WARTHOG_RG_ABSENT, NULL);
    CHECK(warthog_region_resolve(&c, &s, cc, &src) == WARTHOG_HOLD_NONE && strcmp(cc, "US") == 0 &&
          src == WARTHOG_CC_DEFAULT, "nothing stored: US, the default");
    static const char *ok[] = { "AU", "JP", "KR", "NZ", "US" };
    for (unsigned i = 0; i < 5; i++) {
        s = st_(WARTHOG_RG_VALID, ok[i]);
        CHECK(warthog_region_resolve(&c, &s, cc, &src) == WARTHOG_HOLD_NONE && strcmp(cc, ok[i]) == 0 &&
              src == WARTHOG_CC_STORED, "stored %s: %s, stored", ok[i], ok[i]);
    }
    static const char *no[] = { "EU", "IN", "CA", "GB", "SG", "ZZ" };
    for (unsigned i = 0; i < 6; i++) {
        s = st_(WARTHOG_RG_VALID, no[i]);
        memcpy(cc, "QQ", 3);
        CHECK(warthog_region_resolve(&c, &s, cc, &src) == WARTHOG_HOLD_REGION_REFUSED && cc[0] == '\0' &&
              src == WARTHOG_CC_NONE, "stored %s, not offered: held (region-refused), no code", no[i]);
    }
    s = st_(WARTHOG_RG_INVALID, NULL);
    snprintf(s.raw, sizeof(s.raw), "u$");
    CHECK(warthog_region_resolve(&c, &s, cc, &src) == WARTHOG_HOLD_REGION_INVALID && cc[0] == '\0',
          "an invalid stored value: held (region-invalid), never US");
    s = st_(WARTHOG_RG_ABSENT, NULL);
    const struct warthog_region_ctx au = ctx_("AU", k_region_bcf, sizeof(k_region_bcf), 902000, 928000);
    CHECK(warthog_region_resolve(&au, &s, cc, &src) == WARTHOG_HOLD_DEFAULT_REFUSED && cc[0] == '\0',
          "nothing stored on a module that does not offer US: held (default-refused)");
    CHECK(warthog_region_resolve(&c, NULL, cc, &src) == WARTHOG_HOLD_REGION_INVALID, "no state at all: held");
}

/* ---- AT+MESHCHAN, at boot and at set time ------------------------------------- */

static void t_match(void)
{
    printf("--- boot: frequency, channel and bandwidth match an in-band row; the classes are the row's ---\n");
    const struct warthog_region_ctx c = real_();
    unsigned f = 0;
    struct warthog_mesh_chan m = mc_(42, 923000000, 69, 2, 2);
    CHECK(warthog_region_match(&c, dom_("US"), &m, &f) == row_("US", 42, 2) && f == 0, "US 42,923000000,69,2,2: the row, no flags");
    f = 0; m = mc_(42, 923000000, 1, 1, 2);
    CHECK(warthog_region_match(&c, dom_("US"), &m, &f) == row_("US", 42, 2) && f == WARTHOG_CHAN_F_CLASSES_FIXED,
          "classes 1,1: the same row, classes-fixed (an upgraded board stays on its channel)");
    f = 0; m = mc_(42, 923500000, 69, 2, 2);
    CHECK(warthog_region_match(&c, dom_("US"), &m, &f) == NULL, "923.5 MHz is not ch42's frequency: no row");
    m = mc_(42 + 256, 923000000, 69, 2, 2);
    CHECK(warthog_region_match(&c, dom_("US"), &m, &f) == NULL, "chan 298 is not ch42 (no 8-bit truncation)");
    m = mc_(42, 923000000, 69, 2, 4);
    CHECK(warthog_region_match(&c, dom_("US"), &m, &f) == NULL, "ch42 @ 4 MHz is not a row");
    f = 0; m = mc_(2, 903000000, 69, 2, 2);
    CHECK(warthog_region_match(&c, dom_("US"), &m, &f) == row_("US", 2, 2) && f == WARTHOG_CHAN_F_WARNING_ROW,
          "US ch2 (a Morse warning row) is used at boot and flagged warning-row");
    m = mc_(42, 923000000, 69, 2, 2);
#ifndef WANT_AU2020
    CHECK(warthog_region_match(&c, dom_("AU"), &m, &f) == NULL, "the US ch42 set under AU (REVMF has no 923.0 @ 2): no row");
#endif
    f = 0; m = mc_(22, 928500000, 74, 14, 1);
    CHECK(warthog_region_match(&c, dom_("KR"), &m, &f) == NULL, "KR ch22 (928.5 MHz, out of band): no row");
    f = 0; m = mc_(20, 927500000, 74, 14, 1);
    CHECK(warthog_region_match(&c, dom_("KR"), &m, &f) == row_("KR", 20, 1) && f == WARTHOG_CHAN_F_WARNING_ROW,
          "KR ch20 (927.5 MHz, in band, a warning row): used and flagged");
    CHECK(warthog_region_match(&c, NULL, &m, &f) == NULL && warthog_region_match(&c, dom_("US"), NULL, &f) == NULL,
          "no list or no set: no row");
}

static void t_check_set(void)
{
    printf("--- AT+MESHCHAN=: all five equal an in-band row Morse does not flag ---\n");
    const struct warthog_region_ctx c = real_();
    char why[128] = "";
    struct warthog_mesh_chan m = mc_(42, 923000000, 69, 2, 2);
    CHECK(warthog_region_check_set(&c, dom_("US"), &m, why, sizeof(why)), "US ch42 exact: accepted");
    m = mc_(42, 923000000, 1, 1, 2);
    CHECK(!warthog_region_check_set(&c, dom_("US"), &m, why, sizeof(why)) &&
          strcmp(why, "US ch42 @ 2 MHz has classes 69,2") == 0, "classes 1,1 refused: '%s'", why);
    static const struct { unsigned g, s; } one[] = { { 69, 27 }, { 1, 2 } };
    for (unsigned i = 0; i < 2; i++) {
        m = mc_(42, 923000000, one[i].g, one[i].s, 2);
        why[0] = '\0';
        CHECK(!warthog_region_check_set(&c, dom_("US"), &m, why, sizeof(why)) &&
              strcmp(why, "US ch42 @ 2 MHz has classes 69,2") == 0, "classes %u,%u (one of the two wrong) refused: '%s'", one[i].g,
              one[i].s, why);
    }
    m = mc_(42, 923000000, 69, 2, 2);
    why[0] = '\0';
    CHECK(!warthog_region_check_set(&c, dom_("NZ"), &m, why, sizeof(why)) &&
          strcmp(why, "NZ ch42 @ 2 MHz has classes 69,27") == 0, "the US ch42 set under NZ (s1g class 2, NZ's is 27) refused: '%s'", why);
    m = mc_(2, 903000000, 69, 2, 2);
    CHECK(!warthog_region_check_set(&c, dom_("US"), &m, why, sizeof(why)) &&
          strcmp(why, "US ch2 @ 2 MHz is flagged by Morse as possibly non-compliant") == 0, "US ch2 refused: '%s'", why);
    m = mc_(6, 905500000, 69, 2, 2);
    CHECK(!warthog_region_check_set(&c, dom_("US"), &m, why, sizeof(why)) && strcmp(why, "not a row of the US table") == 0,
          "6,905500000 refused: '%s'", why);
    m = mc_(42, 999000000, 69, 2, 2);
    CHECK(!warthog_region_check_set(&c, dom_("US"), &m, why, sizeof(why)), "the interop bench's 999 MHz: refused");
    m = mc_(42, 923000000, 69, 2, 3);
    CHECK(!warthog_region_check_set(&c, dom_("US"), &m, why, sizeof(why)), "the interop bench's bw 3: refused");
#ifndef WANT_AU2020
    m = mc_(41, 922500000, 51, 23, 2);
    CHECK(warthog_region_check_set(&c, dom_("AU"), &m, why, sizeof(why)), "AU ch41 exact: accepted");
#endif
    m = mc_(22, 928500000, 74, 14, 1);
    CHECK(!warthog_region_check_set(&c, dom_("KR"), &m, why, sizeof(why)) &&
          strcmp(why, "KR ch22 @ 1 MHz is outside this module's 902-928 MHz band") == 0, "KR ch22 refused: '%s'", why);
    m = mc_(10, 922500000, 75, 15, 2);
    CHECK(warthog_region_check_set(&c, dom_("KR"), &m, why, sizeof(why)), "KR ch10 exact: accepted");
    m = mc_(19, 927000000, 75, 15, 2);
    CHECK(!warthog_region_check_set(&c, dom_("KR"), &m, why, sizeof(why)) && strstr(why, "flagged by Morse") != NULL,
          "KR ch19 (in band, a warning row) refused: '%s'", why);
    m = mc_(42, 923000000, 69, 2, 2);
    CHECK(!warthog_region_check_set(&c, NULL, &m, why, sizeof(why)) && strcmp(why, "not a row of the ?? table") == 0,
          "no list: refused");
}

/* ---- defaults --------------------------------------------------------------------- */

/* The rule the JP, KR and NZ defaults were chosen by, independent of region.c. */
static const struct mmwlan_s1g_channel *rule_default_(const struct warthog_region_ctx *c,
                                                      const struct mmwlan_s1g_channel_list *l)
{
    uint64_t lo = UINT64_MAX, hi = 0;
    for (unsigned i = 0; i < l->num_channels; i++) {
        const struct mmwlan_s1g_channel *r = &l->channels[i];
        if (!warthog_region_in_band(c, r)) { continue; }
        const uint64_t h = (uint64_t)r->bw_mhz * 500000u;
        lo = r->centre_freq_hz - h < lo ? r->centre_freq_hz - h : lo;
        hi = r->centre_freq_hz + h > hi ? r->centre_freq_hz + h : hi;
    }
    for (unsigned bw = 2; bw >= 1; bw--) {
        const struct mmwlan_s1g_channel *best = NULL;
        uint64_t bd = UINT64_MAX;
        for (unsigned i = 0; i < l->num_channels; i++) {
            const struct mmwlan_s1g_channel *r = &l->channels[i];
            if (r->bw_mhz != bw || !warthog_region_in_band(c, r) ||
                warthog_region_is_warning_row(l->country_code, r->s1g_chan_num, r->bw_mhz)) { continue; }
            const uint64_t two = 2u * (uint64_t)r->centre_freq_hz;
            const uint64_t d = two > lo + hi ? two - (lo + hi) : (lo + hi) - two;
            if (d < bd || (d == bd && r->centre_freq_hz < best->centre_freq_hz)) { best = r; bd = d; }
        }
        if (best != NULL) { return best; }
    }
    return NULL;
}

/* Every (freq kHz, bw) entry of cc's populated BCF section; the entry layout is inferred from the bytes. */
static unsigned bcf_entries_(const char *cc, uint32_t khz[16], uint32_t bw[16])
{
    uint16_t l = 0;
    const size_t off = real_off_(cc, &l);
    const uint8_t *body = k_region_bcf + off + 8;
    const uint32_t inner = (uint32_t)(body[4] | (body[5] << 8) | (body[6] << 16) | ((uint32_t)body[7] << 24));
    unsigned n = 0;
    uint32_t f = 0;
    for (uint32_t p = 12; p + 2 <= 12 + inner && p + 2 <= (uint32_t)l - 4u; ) {
        const uint8_t t = body[p], len = body[p + 1];
        const uint32_t v = len == 4 ? (uint32_t)(body[p + 2] | (body[p + 3] << 8) | (body[p + 4] << 16) |
                                                 ((uint32_t)body[p + 5] << 24)) : 0;
        if (t == 0xc9) { f = v; }
        if (t == 0xca && n < 16) { khz[n] = f; bw[n] = v; n++; }
        p += 2u + len;
    }
    return n;
}

static void t_defaults(void)
{
    printf("--- the default mesh rows are regulator rows ---\n");
    const struct warthog_region_ctx c = real_();
    static const struct { const char *cc; unsigned chan, bw, g, s; uint32_t hz; int eirp; } want[] = {
        { "US", 42, 2, 69, 2, 923000000, 36 },
#ifndef WANT_AU2020
        { "AU", 41, 2, 51, 23, 922500000, 30 },
#endif
        { "JP", 2, 2, 64, 9, 923500000, 16 },
        { "KR", 10, 2, 75, 15, 922500000, 10 },
        { "NZ", 38, 2, 69, 27, 921000000, 36 },
    };
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        const struct mmwlan_s1g_channel_list *l = dom_(want[i].cc);
        const struct mmwlan_s1g_channel *d = warthog_region_default_row(&c, l);
        CHECK(d != NULL && d >= l->channels && d < l->channels + l->num_channels && d->s1g_chan_num == want[i].chan &&
              d->bw_mhz == want[i].bw && d->centre_freq_hz == want[i].hz && d->global_operating_class == (int)want[i].g &&
              d->s1g_operating_class == (int)want[i].s && d->max_tx_eirp_dbm == want[i].eirp &&
              warthog_region_in_band(&c, d) && !warthog_region_is_warning_row(l->country_code, d->s1g_chan_num, d->bw_mhz),
              "%s default: ch%u %lu Hz @ %u MHz, classes %u/%u, %d dBm, the regdb row itself, in band, no warning",
              want[i].cc, want[i].chan, (unsigned long)want[i].hz, want[i].bw, want[i].g, want[i].s, want[i].eirp);
    }
    const struct mmwlan_s1g_channel *us = warthog_region_default_row(&c, dom_("US"));
    CHECK(us != NULL && us->duty_cycle_sta == 10000, "US ch42 is the old build-time pin (duty 10000)");
    static const char *ruled[] = { "JP", "KR", "NZ" };
    for (unsigned i = 0; i < 3; i++) {
        const struct mmwlan_s1g_channel_list *l = dom_(ruled[i]);
        CHECK(warthog_region_default_row(&c, l) == rule_default_(&c, l),
              "%s's default is the 2 MHz row nearest the middle of its in-band span (lower on a tie)", ruled[i]);
    }
    static const char *none[] = { "CA", "EU", "GB", "IN" };
    for (unsigned i = 0; i < 4; i++) {
        CHECK(warthog_region_default_row(&c, dom_(none[i])) == NULL, "%s has no default (never offered)", none[i]);
    }
    CHECK(warthog_region_default_row(&c, NULL) == NULL, "no list: no default");
    const struct warthog_region_ctx narrow = ctx_("US", k_region_bcf, sizeof(k_region_bcf), 902000, 922000);
    CHECK(warthog_region_default_row(&narrow, dom_("US")) == NULL, "a default outside the band is no default");

    static const char *pop[] = { "US", "AU" };
#ifdef WANT_AU2020
    for (unsigned i = 0; i < 1; i++) {
#else
    for (unsigned i = 0; i < 2; i++) {
#endif
        uint32_t khz[16], bw[16];
        const unsigned n = bcf_entries_(pop[i], khz, bw);
        const struct mmwlan_s1g_channel *d = warthog_region_default_row(&c, dom_(pop[i]));
        bool hit = false;
        for (unsigned j = 0; d != NULL && j < n; j++) {
            hit |= khz[j] * 1000u == d->centre_freq_hz && bw[j] == d->bw_mhz;
        }
        CHECK(n == (i == 0 ? 7u : 4u) && d != NULL && !hit,
              "%s's default is none of the %u entries of its BCF section", pop[i], n);
    }
}

/* ---- the plan, through the hooks ------------------------------------------------------ */

enum mck { M_NONE, M_US42, M_US42_CLS, M_AU41, M_US2, M_KR22, M_KR10 };
static struct warthog_mesh_chan mck_(enum mck k)
{
    switch (k) {
    case M_US42: return mc_(42, 923000000, 69, 2, 2);
    case M_US42_CLS: return mc_(42, 923000000, 1, 1, 2);
    case M_AU41: return mc_(41, 922500000, 51, 23, 2);
    case M_US2: return mc_(2, 903000000, 69, 2, 2);
    case M_KR22: return mc_(22, 928500000, 74, 14, 1);
    case M_KR10: return mc_(10, 922500000, 75, 15, 2);
    default: return mc_(0, 0, 0, 0, 0);
    }
}

struct pcase {
    enum warthog_rg_state st; const char *stored; bool mesh; enum mck mc;
    const char *cc; enum warthog_cc_src cc_src; enum warthog_hold hold; enum warthog_chan_src src; unsigned flags;
    unsigned chan, bw; /* the pinned row; 0 for the full in-band list */
};

static void run_case_(const struct pcase *k)
{
    f_st = st_(k->st, k->stored);
    f_mesh = k->mesh;
    f_have_mc = k->mc != M_NONE;
    f_mc = mck_(k->mc);
    f_ctx = real_();
    struct warthog_region_plan p;
    memset(&p, 0x5a, sizeof(p));
    warthog_region_boot_plan(&p);
    char what[96];
    static const char *const mcn[] = { "none", "US ch42", "US ch42 classes 1,1", "AU ch41", "US ch2", "KR ch22",
                                       "KR ch10" };
    snprintf(what, sizeof(what), "%s%s, mesh %s, MESHCHAN %s", k->st == WARTHOG_RG_ABSENT ? "nothing stored" :
             k->st == WARTHOG_RG_INVALID ? "invalid" : "stored ", k->stored ? k->stored : "", k->mesh ? "on" : "off",
             mcn[k->mc]);
    bool ok = p.hold == k->hold && p.cc_src == k->cc_src && strcmp(p.cc, k->cc) == 0 && p.chan_src == k->src &&
              p.chan_flags == k->flags;
    if (k->hold != WARTHOG_HOLD_NONE) {
        ok = ok && p.list == NULL && p.country == NULL;
    } else if (ok) {
        const struct mmwlan_s1g_channel_list *l = dom_(k->cc);
        ok = p.country == l && p.list == &p.out && p.out.channels == p.rows && p.list->country_code[0] == (uint8_t)k->cc[0] &&
             p.list->country_code[1] == (uint8_t)k->cc[1] && p.list->country_code[2] == 0;
        if (ok && k->chan != 0) {
            const struct mmwlan_s1g_channel *r = row_(k->cc, k->chan, k->bw);
            ok = p.list->num_channels == 1 && r != NULL && memcmp(&p.rows[0], r, sizeof(*r)) == 0;
        } else if (ok) {
            struct mmwlan_s1g_channel rows[WARTHOG_PLAN_MAX_ROWS];
            const unsigned n = warthog_region_band_rows(&f_ctx, l, rows, WARTHOG_PLAN_MAX_ROWS);
            ok = p.list->num_channels == n && memcmp(p.rows, rows, n * sizeof(rows[0])) == 0;
        }
    }
    char src[64];
    (void)warthog_region_chan_src_text(&p, src, sizeof(src));
    CHECK(ok, "%s -> %s%s ch%u@%u flags %u (got hold %d cc '%s' src %s, %u row(s))", what,
          k->hold ? warthog_region_hold_token(k->hold) : k->cc, k->cc_src == WARTHOG_CC_DEFAULT ? " (default)" : "",
          k->chan, k->bw, k->flags, (int)p.hold, p.hold ? "" : p.cc, src, p.list ? p.list->num_channels : 0u);
}

static void t_plan(void)
{
    printf("--- the plan, through the hooks: country, then the one row or the in-band list, or a hold ---\n");
    enum { A = WARTHOG_RG_ABSENT, V = WARTHOG_RG_VALID, I = WARTHOG_RG_INVALID };
    enum { D = WARTHOG_CC_DEFAULT, S = WARTHOG_CC_STORED, N = WARTHOG_CC_NONE };
    enum { CF = WARTHOG_CHAN_FULL_LIST, CS = WARTHOG_CHAN_STORED, CD = WARTHOG_CHAN_DEFAULT, CH = WARTHOG_CHAN_HELD };
    const unsigned R = WARTHOG_CHAN_F_STORED_REFUSED, X = WARTHOG_CHAN_F_CLASSES_FIXED, W = WARTHOG_CHAN_F_WARNING_ROW;
    const struct pcase cases[] = {
        { A, NULL, false, M_NONE, "US", D, WARTHOG_HOLD_NONE, CF, 0, 0, 0 },
        { A, NULL, true, M_NONE, "US", D, WARTHOG_HOLD_NONE, CD, 0, 42, 2 },
        { A, NULL, false, M_US42, "US", D, WARTHOG_HOLD_NONE, CS, 0, 42, 2 },
        { A, NULL, true, M_US42, "US", D, WARTHOG_HOLD_NONE, CS, 0, 42, 2 },
        { A, NULL, true, M_US42_CLS, "US", D, WARTHOG_HOLD_NONE, CS, X, 42, 2 },
        { A, NULL, true, M_US2, "US", D, WARTHOG_HOLD_NONE, CS, W, 2, 2 },
        { A, NULL, true, M_AU41, "US", D, WARTHOG_HOLD_NONE, CD, R, 42, 2 },
        { A, NULL, false, M_AU41, "US", D, WARTHOG_HOLD_NONE, CF, R, 0, 0 },
        { V, "US", true, M_AU41, "US", S, WARTHOG_HOLD_NONE, CD, R, 42, 2 },
        { V, "US", false, M_NONE, "US", S, WARTHOG_HOLD_NONE, CF, 0, 0, 0 },
        { V, "AU", false, M_NONE, "AU", S, WARTHOG_HOLD_NONE, CF, 0, 0, 0 },
#ifndef WANT_AU2020
        { V, "AU", true, M_NONE, "AU", S, WARTHOG_HOLD_NONE, CD, 0, 41, 2 },
        { V, "AU", true, M_US42, "AU", S, WARTHOG_HOLD_NONE, CD, R, 41, 2 },
        { V, "AU", false, M_AU41, "AU", S, WARTHOG_HOLD_NONE, CS, 0, 41, 2 },
        { V, "AU", false, M_US42, "AU", S, WARTHOG_HOLD_NONE, CF, R, 0, 0 },
#else
        { V, "AU", true, M_NONE, "", N, WARTHOG_HOLD_NO_MESH_DEFAULT, CH, 0, 0, 0 },
        { V, "AU", true, M_US42, "AU", S, WARTHOG_HOLD_NONE, CS, X, 42, 2 }, /* 2020 has 923.0 @ 2, classes 69/23 */
        { V, "AU", true, M_AU41, "", N, WARTHOG_HOLD_NO_MESH_DEFAULT, CH, R, 0, 0 },
#endif
        { V, "KR", false, M_NONE, "KR", S, WARTHOG_HOLD_NONE, CF, 0, 0, 0 },
        { V, "KR", true, M_NONE, "KR", S, WARTHOG_HOLD_NONE, CD, 0, 10, 2 },
        { V, "KR", true, M_KR22, "KR", S, WARTHOG_HOLD_NONE, CD, R, 10, 2 },
        { V, "KR", false, M_KR10, "KR", S, WARTHOG_HOLD_NONE, CS, 0, 10, 2 },
        { V, "JP", true, M_NONE, "JP", S, WARTHOG_HOLD_NONE, CD, 0, 2, 2 },
        { V, "NZ", true, M_US42, "NZ", S, WARTHOG_HOLD_NONE, CS, X, 42, 2 },
        { V, "NZ", true, M_NONE, "NZ", S, WARTHOG_HOLD_NONE, CD, 0, 38, 2 },
        { V, "EU", false, M_NONE, "", N, WARTHOG_HOLD_REGION_REFUSED, CH, 0, 0, 0 },
        { V, "EU", true, M_US42, "", N, WARTHOG_HOLD_REGION_REFUSED, CH, 0, 0, 0 },
        { V, "SG", true, M_NONE, "", N, WARTHOG_HOLD_REGION_REFUSED, CH, 0, 0, 0 },
        { I, NULL, false, M_NONE, "", N, WARTHOG_HOLD_REGION_INVALID, CH, 0, 0, 0 },
        { I, NULL, true, M_US42, "", N, WARTHOG_HOLD_REGION_INVALID, CH, 0, 0, 0 },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        run_case_(&cases[i]);
    }
    f_st = st_(V, "KR"); f_mesh = 0; f_have_mc = false; f_ctx = real_();
    struct warthog_region_plan p;
    warthog_region_boot_plan(&p);
    bool in = p.list != NULL && p.list->num_channels == 13;
    for (unsigned i = 0; in && i < p.list->num_channels; i++) { in = warthog_region_in_band(&f_ctx, &p.list->channels[i]); }
    CHECK(in, "KR with mesh off gets its 13 in-band rows, none of 928.5 / 929.5 / 929 MHz");
    f_st = st_(A, NULL); f_mesh = 1;
    warthog_region_boot_plan(&p);
    CHECK(p.list != NULL && p.list->num_channels == 1 && p.list->channels[0].s1g_chan_num != dom_("US")->channels[0].s1g_chan_num,
          "mesh on with nothing stored is never index 0 of the full list (US ch1, a warning row)");

    /* regnvs that NVS refused at boot (cfg.c: never erased there) reads invalid; it holds, it is not US. */
    for (unsigned i = 0; i < 4; i++) {
        f_st = st_(I, NULL);
        snprintf(f_st.raw, sizeof(f_st.raw), "(unreadable)");
        f_st.unreadable = true;
        f_mesh = i & 1;
        f_have_mc = i >= 2;
        f_mc = mck_(M_US42);
        memset(&p, 0x5a, sizeof(p));
        warthog_region_boot_plan(&p);
        CHECK(p.hold == WARTHOG_HOLD_REGION_INVALID && p.list == NULL && p.country == NULL && p.cc[0] == '\0' &&
                  p.cc_src == WARTHOG_CC_NONE && p.chan_src == WARTHOG_CHAN_HELD,
              "regnvs unreadable, mesh %s, MESHCHAN %s: held (region-invalid), never the US default", (i & 1) ? "on" : "off",
              i >= 2 ? "US ch42" : "none");
    }

    /* v0.1.0's station pinned a stored set on channel and bandwidth alone; the rule now needs the
     * frequency too, so a set with ch38 @ 2 MHz but 920.0 MHz (ch38 is 921.0) no longer pins it. */
    f_st = st_(A, NULL); f_have_mc = true; f_mc = mc_(38, 920000000, 69, 2, 2);
    for (unsigned on = 0; on < 2; on++) {
        f_mesh = (uint8_t)on;
        warthog_region_boot_plan(&p);
        const bool want = on ? (p.chan_src == WARTHOG_CHAN_DEFAULT && p.list->num_channels == 1 && p.rows[0].s1g_chan_num == 42)
                             : (p.chan_src == WARTHOG_CHAN_FULL_LIST && p.list->num_channels == 48);
        CHECK(p.hold == WARTHOG_HOLD_NONE && want && p.chan_flags == WARTHOG_CHAN_F_STORED_REFUSED,
              "an upgraded US board with ch38 @ 2 MHz stored at 920.0 MHz, mesh %s: %s +stored-refused, not pinned to ch38",
              on ? "on" : "off", on ? "the ch42 default" : "the full list");
    }

    /* With mesh off the station's list keeps the in-band rows Morse flags (as v0.1.0 and Morse's own full
     * list did): an AP on one is still found; a mesh default or a new AT+MESHCHAN= never lands on one. */
    static const struct { const char *cc; unsigned n; const char *want; } flagged[] = {
        { "US", 48, "1@1,2@2" }, { "KR", 13, "18@1,20@1,19@2" },
    };
    for (unsigned i = 0; i < 2; i++) {
        f_st = st_(V, flagged[i].cc); f_mesh = 0; f_have_mc = false;
        warthog_region_boot_plan(&p);
        char got[64] = "";
        for (unsigned j = 0; p.list != NULL && j < p.list->num_channels; j++) {
            const struct mmwlan_s1g_channel *r = &p.list->channels[j];
            if (warthog_region_is_warning_row(p.list->country_code, r->s1g_chan_num, r->bw_mhz)) {
                snprintf(got + strlen(got), sizeof(got) - strlen(got), "%s%u@%u", got[0] ? "," : "", (unsigned)r->s1g_chan_num,
                         (unsigned)r->bw_mhz);
            }
        }
        CHECK(p.list != NULL && p.list->num_channels == flagged[i].n && strcmp(got, flagged[i].want) == 0,
              "%s with mesh off: the station's %u rows keep the flagged in-band rows %s (got %s)", flagged[i].cc, flagged[i].n,
              flagged[i].want, got);
    }
}

/* A country only a synthetic context offers: one row, no default. */
static const struct mmwlan_s1g_channel k_xx_rows[] = { { 915000000, 10000, false, 69, 2, 26, 2, 30, 0, 0, 0 } };
static const struct mmwlan_s1g_channel_list k_xx = { .country_code = "XX", .num_channels = 1, .channels = k_xx_rows };

static void t_holds(void)
{
    printf("--- the plan holds the chip rather than guess ---\n");
    const struct mmwlan_s1g_channel_list *doms[] = { dom_("US"), &k_xx };
    const struct mmwlan_regulatory_db db = { .num_domains = 2, .domains = doms };
    struct buf o;
    real_with_before_us_(&o, "\x01\x81\x10\x00XX\0\0abcdefghijkl", 20);
    struct warthog_region_ctx c = ctx_("US,XX", o.b, o.n, 902000, 928000);
    c.db = &db;
    struct warthog_rg_stored s = st_(WARTHOG_RG_VALID, "XX");
    struct warthog_region_plan p;
    warthog_region_plan(&c, &s, true, NULL, &p);
    CHECK(p.hold == WARTHOG_HOLD_NO_MESH_DEFAULT && p.list == NULL && p.cc[0] == '\0' && p.chan_src == WARTHOG_CHAN_HELD,
          "an offered country with no default row, mesh on: held (no-mesh-default), no list");
    warthog_region_plan(&c, &s, false, NULL, &p);
    CHECK(p.hold == WARTHOG_HOLD_NONE && p.list != NULL && p.list->num_channels == 1 && strcmp(p.cc, "XX") == 0,
          "the same country with mesh off: its one row");

    /* One in-band row more than a plan holds: refused, not cut to WARTHOG_PLAN_MAX_ROWS. */
    static struct mmwlan_s1g_channel big[WARTHOG_PLAN_MAX_ROWS + 1];
    for (unsigned i = 0; i <= WARTHOG_PLAN_MAX_ROWS; i++) {
        big[i] = k_xx_rows[0];
        big[i].centre_freq_hz = 903000000u + i * 250000u;
        big[i].s1g_chan_num = (uint8_t)(i + 1);
        big[i].bw_mhz = 1;
    }
    const struct mmwlan_s1g_channel_list yy = { .country_code = "XX", .num_channels = WARTHOG_PLAN_MAX_ROWS + 1, .channels = big };
    const struct mmwlan_s1g_channel_list *doms2[] = { dom_("US"), &yy };
    const struct mmwlan_regulatory_db db2 = { .num_domains = 2, .domains = doms2 };
    c.db = &db2;
    CHECK(warthog_region_band_rows(&c, &yy, NULL, 0) == WARTHOG_PLAN_MAX_ROWS + 1 && !warthog_region_allowed(&c, "XX") &&
          warthog_region_allowed(&c, "US"), "a country with %u in-band rows (a plan holds %u): not offered", WARTHOG_PLAN_MAX_ROWS + 1,
          (unsigned)WARTHOG_PLAN_MAX_ROWS);
    for (unsigned on = 0; on < 2; on++) {
        memset(&p, 0x5a, sizeof(p));
        warthog_region_plan(&c, &s, on != 0, NULL, &p);
        CHECK(p.hold == WARTHOG_HOLD_REGION_REFUSED && p.list == NULL && p.cc[0] == '\0',
              "stored XX with %u in-band rows, mesh %s: held (region-refused), no list", WARTHOG_PLAN_MAX_ROWS + 1, on ? "on" : "off");
    }
    c.db = &db;
    const enum warthog_hold all[] = { WARTHOG_HOLD_NONE, WARTHOG_HOLD_REGION_INVALID, WARTHOG_HOLD_REGION_REFUSED,
                                      WARTHOG_HOLD_DEFAULT_REFUSED, WARTHOG_HOLD_NO_MESH_DEFAULT };
    bool named = true;
    for (unsigned i = 0; i < 5; i++) {
        for (unsigned j = 0; j < i; j++) {
            named = named && strcmp(warthog_region_hold_text(all[i]), warthog_region_hold_text(all[j])) != 0 &&
                    strcmp(warthog_region_hold_token(all[i]), warthog_region_hold_token(all[j])) != 0;
        }
        named = named && strcmp(warthog_region_hold_text(all[i]), "unknown") != 0;
    }
    CHECK(named, "every hold has its own text and token");
    CHECK(strcmp(warthog_region_hold_token(WARTHOG_HOLD_REGION_INVALID), "region-invalid") == 0 &&
          strcmp(warthog_region_hold_token(WARTHOG_HOLD_NO_MESH_DEFAULT), "no-mesh-default") == 0, "tokens as AT+MESHCFG? prints them");
    char t[64];
    memset(&p, 0, sizeof(p));
    p.chan_src = WARTHOG_CHAN_STORED;
    p.chan_flags = WARTHOG_CHAN_F_CLASSES_FIXED | WARTHOG_CHAN_F_WARNING_ROW;
    size_t n = warthog_region_chan_src_text(&p, t, sizeof(t));
    CHECK(strcmp(t, "stored+classes-fixed+warning-row") == 0 && n == strlen(t), "chan src text: '%s'", t);
    p.chan_src = WARTHOG_CHAN_DEFAULT; p.chan_flags = WARTHOG_CHAN_F_STORED_REFUSED;
    (void)warthog_region_chan_src_text(&p, t, sizeof(t));
    CHECK(strcmp(t, "default+stored-refused") == 0, "chan src text: '%s'", t);
    (void)warthog_region_chan_src_text(NULL, t, sizeof(t));
    CHECK(strcmp(t, "held") == 0, "no plan (safe mode): 'held'");

    /* The NOT PEERED log's region lines (mesh.c), from the plan the boot made. */
    char rg[48], rf[112];
    f_st = st_(WARTHOG_RG_VALID, "AU"); f_mesh = 1; f_have_mc = true; f_mc = mck_(M_US42); f_ctx = real_();
    warthog_region_boot_plan(&p);
    warthog_region_report_text(&p, rg, sizeof(rg), rf, sizeof(rf));
#ifndef WANT_AU2020
    CHECK(strcmp(rg, "AU (stored)") == 0 &&
              strcmp(rf, "stored AT+MESHCHAN is not in the AU table; the AU default is in use") == 0,
          "NOT PEERED, AU stored, US ch42 refused: '%s' / '%s'", rg, rf);
#endif
    f_st = st_(WARTHOG_RG_ABSENT, NULL);
    warthog_region_boot_plan(&p);
    warthog_region_report_text(&p, rg, sizeof(rg), rf, sizeof(rf));
    CHECK(strcmp(rg, "US (not set: US default)") == 0 && rf[0] == '\0', "NOT PEERED, nothing stored, US ch42 used: '%s' / '%s'", rg, rf);
    f_mc = mck_(M_US42_CLS);
    warthog_region_boot_plan(&p);
    warthog_region_report_text(&p, rg, sizeof(rg), rf, sizeof(rf));
    CHECK(p.chan_flags == WARTHOG_CHAN_F_CLASSES_FIXED && rf[0] == '\0', "NOT PEERED, a set used with the row's classes: no refused line ('%s')", rf);
    f_mesh = 0; f_mc = mck_(M_AU41);
    warthog_region_boot_plan(&p);
    warthog_region_report_text(&p, rg, sizeof(rg), rf, sizeof(rf));
    CHECK(strcmp(rf, "stored AT+MESHCHAN is not in the US table; the US full list is in use") == 0, "station, AU ch41 refused: '%s'", rf);
    f_st = st_(WARTHOG_RG_INVALID, NULL);
    warthog_region_boot_plan(&p);
    warthog_region_report_text(&p, rg, sizeof(rg), rf, sizeof(rf));
    CHECK(strcmp(rg, "none (held: region-invalid)") == 0 && rf[0] == '\0', "held: '%s' / '%s'", rg, rf);
    warthog_region_report_text(NULL, rg, sizeof(rg), rf, sizeof(rf));
    CHECK(strcmp(rg, "none (no plan)") == 0 && rf[0] == '\0', "no plan: '%s' / '%s'", rg, rf);
    f_have_mc = false;
}

int main(void)
{
#ifdef WANT_AU2020
    printf("=== the region decision, AU channelisation 2020 ===\n");
#else
    printf("=== the region decision ===\n");
#endif
    t_normalize();
    t_bcf_walk();
    t_bcf_bounds();
    t_band();
    t_allowed();
    t_restricted();
    t_resolve();
    t_match();
    t_check_set();
    t_defaults();
    t_plan();
    t_holds();
#ifdef WANT_AU2020
    {
        const struct warthog_region_ctx c = real_();
        CHECK(dom_("AU")->num_channels == 23 && warthog_region_default_row(&c, dom_("AU")) == NULL,
              "2020 AU (23 rows) has no ch41 @ 2 MHz: no default, so a mesh holds instead of landing on index 0");
    }
#endif
    if (failures) {
        printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_region: all passed\n");
    return 0;
}
