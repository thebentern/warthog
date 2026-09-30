/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The mesh supplicant shim's hostapd configuration, built from the shipping
 * sources: supplicant_core_mesh.c (the passive mesh bring-up) and hostap's
 * ap_config.c (hostapd_config_defaults / hostapd_config_free), both unmodified.
 *
 * What is asserted is the SAE group list the shim hands hostap:
 *  - its contents and order. mesh_rsn_sae_group() commits with index 0, and
 *    ieee802_11.c accepts a peer's commit only for a group in this list, so
 *    it must be 19 first, then 20 and 21 (OpenMANET's mesh default list is
 *    19,20,21,25,26; its LuCI lets an operator pick 20 or 21 alone).
 *  - who owns it. bss->conf is a pointer to conf->bss[0], and
 *    hostapd_config_free_bss() os_free()s sae_groups, so the list must be a
 *    heap allocation the config free can release exactly once.
 *  - what happens when that allocation fails. mesh_rsn_sae_group() indexes the
 *    list without a NULL check, so SAE must never be armed without one; the
 *    hostapd_config must stay owned by ifmsh so the teardown still frees all of
 *    it; and g_warthog_sae_init must read 2 (SAE requested, not running) when
 *    SAE was asked for, 0 when it was not.
 *
 * The os_* allocator here records every live allocation, so a free of
 * anything it did not hand out (static storage) or a second free is counted
 * rather than crashing the run. It numbers allocations from the start of each
 * bring-up and can fail one chosen number, which is how the OOM case finds the
 * group list's allocation without knowing the code above it. The teardown is
 * the real hostapd_config_free() on ifmsh->conf -- what hostapd_interface_free()
 * -> hostapd_cleanup_iface() runs when umac_supp_remove_mesh_interface() removes
 * the interface; the layers in between (mesh_mpm/hostapd.c interface teardown)
 * are modelled by the fake wpa_supplicant_remove_iface() below, which frees
 * only containers.
 *
 * Everything else the two files reference -- MPM, RSN, the chip, logging --
 * is a stub; the ones the bring-up must not reach count their calls.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "mmwlan.h"
#include "umac/data/umac_data.h"
#include "umac/supplicant_shim/umac_supp_shim.h"
#include "umac/supplicant_shim/umac_supp_shim_private.h"
#include "umac/mesh/umac_mesh.h"
#include "umac/mesh/umac_mesh_ies.h"
#include "hostap/src/ap/hostapd.h"
#include "hostap/src/ap/ap_config.h"
#include "hostap/src/common/ieee802_11_common.h"
#include "hostap/src/common/sae.h"
#include "hostap/src/common/wpa_common.h"
#include "hostap/src/crypto/sha1.h"
#include "hostap/wpa_supplicant/config_ssid.h"
#include "hostap/wpa_supplicant/mesh.h"
#include "hostap/wpa_supplicant/mesh_mpm.h"
#include "hostap/wpa_supplicant/mesh_rsn.h"
#pragma GCC diagnostic pop

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* ---- an os_* allocator that knows what it handed out ------------------- */

#define MAX_LIVE 512
static void *s_live[MAX_LIVE];
static size_t s_live_size[MAX_LIVE];
static void *s_dead[MAX_LIVE];
static unsigned s_dead_n, s_foreign_frees, s_double_frees;
static const void *s_last_foreign;
/* s_alloc_n numbers os_malloc/os_zalloc calls from 1; the one numbered s_fail_at (0 = none)
 * returns NULL. s_live_idx is the number each live allocation was handed out under. */
static unsigned s_alloc_n, s_fail_at;
static unsigned s_live_idx[MAX_LIVE];

static void track_(void *p, size_t size)
{
    if (p == NULL) { return; }
    for (unsigned i = 0; i < MAX_LIVE; i++)
    {
        if (s_live[i] == NULL) { s_live[i] = p; s_live_size[i] = size; s_live_idx[i] = s_alloc_n; return; }
    }
    fprintf(stderr, "allocation table full\n");
    abort();
}

/* Bytes in the live os_* allocation starting at p; 0 if p is not one. */
static size_t live_size(const void *p)
{
    for (unsigned i = 0; i < MAX_LIVE; i++)
    {
        if (p != NULL && s_live[i] == p) { return s_live_size[i]; }
    }
    return 0;
}

static bool is_live(const void *p) { return live_size(p) != 0; }

static bool was_freed(const void *p)
{
    for (unsigned i = 0; i < s_dead_n; i++)
    {
        if (p != NULL && s_dead[i] == p && !is_live(p)) { return true; }
    }
    return false;
}

static bool inject_fail_(void) { s_alloc_n++; return s_fail_at != 0 && s_alloc_n == s_fail_at; }
void *os_malloc(size_t size)
{
    if (inject_fail_()) { return NULL; }
    void *p = malloc(size ? size : 1);
    track_(p, size ? size : 1);
    return p;
}

void *os_zalloc(size_t size)
{
    if (inject_fail_()) { return NULL; }
    void *p = calloc(1, size ? size : 1);
    track_(p, size ? size : 1);
    return p;
}

static unsigned live_count(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < MAX_LIVE; i++) { n += s_live[i] != NULL; }
    return n;
}

/* The allocation number p was handed out under; 0 if p is not a live allocation. */
static unsigned live_index(const void *p)
{
    for (unsigned i = 0; i < MAX_LIVE; i++)
    {
        if (p != NULL && s_live[i] == p) { return s_live_idx[i]; }
    }
    return 0;
}

void os_free(void *ptr)
{
    if (ptr == NULL) { return; }
    for (unsigned i = 0; i < MAX_LIVE; i++)
    {
        if (s_live[i] == ptr)
        {
            s_live[i] = NULL;
            if (s_dead_n < MAX_LIVE) { s_dead[s_dead_n++] = ptr; }
            free(ptr);
            return;
        }
    }
    if (was_freed(ptr)) { s_double_frees++; return; }
    /* Not ours: static or stack storage. Counted, never passed to free(). */
    s_foreign_frees++;
    s_last_foreign = ptr;
}

void *os_realloc(void *ptr, size_t size)
{
    size_t old = live_size(ptr);
    void *n = os_malloc(size);
    if (n != NULL && ptr != NULL) { memcpy(n, ptr, old < size ? old : size); os_free(ptr); }
    return n;
}

void *os_memdup(const void *src, size_t len)
{
    void *p = os_malloc(len);
    if (p != NULL && src != NULL) { memcpy(p, src, len); }
    return p;
}

char *os_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    return os_memdup(s, n);
}

void *os_memcpy(void *dest, const void *src, size_t n) { return memcpy(dest, src, n); }
void *os_memmove(void *dest, const void *src, size_t n) { return memmove(dest, src, n); }
void *os_memset(void *s, int c, size_t n) { return memset(s, c, n); }
int os_memcmp(const void *s1, const void *s2, size_t n) { return memcmp(s1, s2, n); }
size_t os_strlen(const char *s) { return strlen(s); }
int os_strcmp(const char *s1, const char *s2) { return strcmp(s1, s2); }
char *os_strchr(const char *s, int c) { return strchr(s, c); }

size_t os_strlcpy(char *dest, const char *src, size_t siz)
{
    size_t n = strlen(src);
    if (siz != 0)
    {
        size_t k = n < siz - 1 ? n : siz - 1;
        memcpy(dest, src, k);
        dest[k] = '\0';
    }
    return n;
}

void str_clear_free(char *str)
{
    if (str != NULL) { memset(str, 0, strlen(str)); os_free(str); }
}

void bin_clear_free(void *bin, size_t len)
{
    if (bin != NULL) { memset(bin, 0, len); os_free(bin); }
}

void wpabuf_free(struct wpabuf *buf) { os_free(buf); }

/* ---- the supplicant around the shim ------------------------------------ */

static struct umac_supp_shim_data s_shim;
static struct wpa_supplicant *s_wpa_s;
static unsigned s_unexpected;

struct umac_supp_shim_data *umac_data_get_supp_shim(struct umac_data *umacd) { return &s_shim; }
enum mmwlan_status umac_supp_start_supp(struct umac_data *umacd) { return MMWLAN_SUCCESS; }

struct wpa_supplicant *wpa_supplicant_add_iface(struct wpa_global *global,
                                                struct wpa_interface *iface,
                                                struct wpa_supplicant *parent)
{
    return s_wpa_s;
}

/* hostapd_cleanup_iface() is where hostap frees ifmsh->conf; the rest here is
 * the containers around it, freed the same way hostapd_interface_free() does. */
int wpa_supplicant_remove_iface(struct wpa_global *global, struct wpa_supplicant *wpa_s,
                                int terminate)
{
    struct hostapd_iface *ifmsh = wpa_s->ifmsh;
    if (ifmsh != NULL)
    {
        os_free(ifmsh->mconf);
        os_free(ifmsh->current_rates);
        os_free(ifmsh->basic_rates);
        for (size_t j = 0; j < ifmsh->num_bss; j++) { os_free(ifmsh->bss[j]); }
        hostapd_config_free(ifmsh->conf);
        os_free(ifmsh->bss);
        os_free(ifmsh);
        wpa_s->ifmsh = NULL;
    }
    return 0;
}

struct hostapd_iface *hostapd_alloc_iface(void)
{
    struct hostapd_iface *iface = os_zalloc(sizeof(*iface));
    if (iface != NULL) { dl_list_init(&iface->sta_seen); }
    return iface;
}

struct hostapd_data *hostapd_alloc_bss_data(struct hostapd_iface *hapd_iface,
                                            struct hostapd_config *conf,
                                            struct hostapd_bss_config *bss)
{
    struct hostapd_data *hapd = os_zalloc(sizeof(*hapd));
    if (hapd != NULL)
    {
        hapd->iface = hapd_iface;
        hapd->iconf = conf;
        hapd->conf = bss;
    }
    return hapd;
}

struct mesh_conf *mesh_config_create(struct wpa_supplicant *wpa_s, struct wpa_ssid *ssid)
{
    struct mesh_conf *m = os_zalloc(sizeof(*m));
    if (m != NULL)
    {
        /* As mesh.c's mesh_config_create(). */
        m->security = (ssid->key_mgmt & WPA_KEY_MGMT_SAE) ? MESH_CONF_SEC_AUTH | MESH_CONF_SEC_AMPE
                                                          : MESH_CONF_SEC_NONE;
    }
    return m;
}

static struct mesh_rsn s_rsn;
struct mesh_rsn *mesh_rsn_auth_init(struct wpa_supplicant *wpa_s, struct mesh_conf *conf)
{
    return &s_rsn;
}

enum mmwlan_status mmwlan_get_vif_mac_addr(enum mmwlan_vif vif, uint8_t *mac_addr)
{
    static const uint8_t mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
    memcpy(mac_addr, mac, sizeof(mac));
    return MMWLAN_SUCCESS;
}

/* Reached only from paths this test does not drive. */
void mesh_mpm_free_sta(struct hostapd_data *hapd, struct sta_info *sta) { s_unexpected++; }
void wpa_mesh_new_mesh_peer(struct wpa_supplicant *wpa_s, const u8 *addr,
                            struct ieee802_11_elems *elems) { s_unexpected++; }
ParseRes ieee802_11_parse_elems(const u8 *start, size_t len, struct ieee802_11_elems *elems,
                                int show_errors) { s_unexpected++; return ParseFailed; }
bool umac_mesh_sae_active(void) { return true; }
void umac_mesh_beacon_set_rsn(const uint8_t *rsn, uint16_t len) { (void)rsn; (void)len; }
const uint8_t *umac_mesh_ies_supported_rates(uint8_t *len_out) { *len_out = 0; return NULL; }
void warthog_sae_trace(uint32_t n) { (void)n; }
struct sae_pt *sae_derive_pt(const int *groups, const u8 *ssid, size_t ssid_len,
                             const u8 *password, size_t password_len,
                             const char *identifier) { s_unexpected++; return NULL; }
void sae_deinit_pt(struct sae_pt *pt) { if (pt != NULL) { s_unexpected++; } }
int pbkdf2_sha1(const char *passphrase, const u8 *ssid, size_t ssid_len, int iterations,
                u8 *buf, size_t buflen) { s_unexpected++; return -1; }
int wpa_select_ap_group_cipher(int wpa, int wpa_pairwise, int rsn_pairwise) { s_unexpected++; return 0; }
bool is_6ghz_op_class(u8 op_class) { return false; }
int hwaddr_aton(const char *txt, u8 *addr) { s_unexpected++; return -1; }
int hexstr2bin(const char *hex, u8 *buf, size_t len) { s_unexpected++; return -1; }
char *str_token(char *str, const char *delim, char **context) { s_unexpected++; return NULL; }
struct wpabuf *wpabuf_alloc(size_t len) { s_unexpected++; return NULL; }
struct wpabuf *wpabuf_alloc_copy(const void *data, size_t len) { s_unexpected++; return NULL; }
void *wpabuf_put(struct wpabuf *buf, size_t len) { s_unexpected++; return NULL; }

/* Logging: silent. */
void wpa_printf(int level, const char *fmt, ...) { (void)level; (void)fmt; }
void wpa_hexdump_key(int level, const char *title, const void *buf, size_t len) {}
void wpa_hexdump_ascii(int level, const char *title, const void *buf, size_t len) {}
void wpa_hexdump_ascii_key(int level, const char *title, const void *buf, size_t len) {}
int mmosal_printf(const char *format, ...) { (void)format; return 0; }
const char *mmosal_task_name(void) { return "test"; }
uint32_t mmosal_get_time_ms(void) { return 0; }

/* ---- the test ----------------------------------------------------------- */

extern volatile uint32_t g_warthog_sae_init;

/* A mesh wpa_supplicant as wpa_config_read_mesh leaves it: SAE, or an open mesh, which
 * keeps the proto wpa_config_set_network_defaults() gave it. */
static struct wpa_supplicant *make_wpa_s(bool sae)
{
    struct wpa_supplicant *wpa_s = os_zalloc(sizeof(*wpa_s));
    wpa_s->conf = os_zalloc(sizeof(*wpa_s->conf));
    struct wpa_ssid *ssid = os_zalloc(sizeof(*ssid));
    ssid->mode = WPAS_MODE_MESH;
    ssid->key_mgmt = sae ? WPA_KEY_MGMT_SAE : WPA_KEY_MGMT_NONE;
    ssid->proto = sae ? WPA_PROTO_RSN : DEFAULT_PROTO;
    ssid->sae_password = sae ? os_strdup("warthog-mesh") : NULL;
    wpa_s->conf->ssid = ssid;
    wpa_s->conf->max_peer_links = 8;
    wpa_s->conf->dot11RSNASAERetransPeriod = 1000;
    os_strlcpy(wpa_s->ifname, "mesh", sizeof(wpa_s->ifname));
    return wpa_s;
}

static void free_wpa_s(struct wpa_supplicant *wpa_s)
{
    os_free(wpa_s->conf->ssid->sae_password);
    os_free(wpa_s->conf->ssid);
    os_free(wpa_s->conf);
    os_free(wpa_s);
}

/* The allocation number, counted from the start of a bring-up, that became the group list. */
static unsigned s_groups_alloc_idx;

static void bring_up_and_down(int cycle)
{
    memset(&s_shim, 0, sizeof(s_shim));
    s_shim.is_started = true;
    s_wpa_s = make_wpa_s(true);
    s_foreign_frees = s_double_frees = 0;
    s_last_foreign = NULL;
    g_warthog_sae_init = 0;
    const unsigned live_before = live_count();
    s_alloc_n = 0;

    enum mmwlan_status st = umac_supp_add_mesh_interface(NULL);
    struct hostapd_iface *ifmsh = s_wpa_s->ifmsh;
    if (st != MMWLAN_SUCCESS || ifmsh == NULL || ifmsh->bss == NULL || ifmsh->bss[0] == NULL ||
        ifmsh->conf == NULL || ifmsh->bss[0]->conf != ifmsh->conf->bss[0])
    {
        printf("FAIL cycle %d: the passive bring-up did not build ifmsh (status %d)\n", cycle, (int)st);
        failures++;
        return;
    }

    /* hostap walks the list to its <= 0 terminator. A heap list is read only as far as its
     * allocation goes; anything else (the ownership checks below catch it) short-circuits. */
    const int *groups = ifmsh->bss[0]->conf->sae_groups;
    const size_t bytes = live_size(groups);
    const bool whole = groups != NULL && !was_freed(groups) && (bytes == 0 || bytes >= 4 * sizeof(int));
    CHECK(whole && groups[0] == 19 && groups[1] == 20 && groups[2] == 21 && groups[3] <= 0,
          "cycle %d: the SAE group list is 19, 20, 21, terminated -- 19 first, the group the "
          "initiator commits with", cycle);
    CHECK(groups != NULL && is_live(groups),
          "cycle %d: the SAE group list is a heap allocation the config teardown can free", cycle);
    /* hostap reads the interface config through hapd->iconf on the auth and MPM paths. */
    CHECK(ifmsh->bss[0]->iconf == ifmsh->conf, "cycle %d: bss->iconf is ifmsh->conf", cycle);
    CHECK(g_warthog_sae_init == 1 && s_wpa_s->mesh_rsn != NULL,
          "cycle %d: SAE is armed and g_warthog_sae_init is 1 (got %u)", cycle,
          (unsigned)g_warthog_sae_init);
    s_groups_alloc_idx = live_index(groups);

    const void *groups_at = groups;
    st = umac_supp_remove_mesh_interface(NULL);
    CHECK(st == MMWLAN_SUCCESS && s_foreign_frees == 0 && s_double_frees == 0 && !is_live(groups_at),
          "cycle %d: hostapd_config_free releases the group list once and frees nothing it did not "
          "allocate (foreign %u%s, double %u)",
          cycle, s_foreign_frees, s_last_foreign == groups_at ? " = sae_groups" : "", s_double_frees);
    CHECK(live_count() == live_before, "cycle %d: the teardown leaves no allocation behind (%u -> %u)",
          cycle, live_before, live_count());

    free_wpa_s(s_wpa_s);
    s_wpa_s = NULL;
}

/* The group list's allocation fails (it is the allocation cycle 2 recorded; the bring-up
 * makes the same calls up to it whether or not SAE is configured). sae_init_before is what
 * the counter held going in: the shim writes it only when SAE was asked for. */
static void oom_on_group_list(bool sae, uint32_t sae_init_before)
{
    const char *mode = sae ? "SAE" : "open";
    memset(&s_shim, 0, sizeof(s_shim));
    s_shim.is_started = true;
    s_wpa_s = make_wpa_s(sae);
    s_foreign_frees = s_double_frees = 0;
    s_last_foreign = NULL;
    g_warthog_sae_init = sae_init_before;
    const unsigned live_before = live_count();
    s_alloc_n = 0;
    s_fail_at = s_groups_alloc_idx;
    enum mmwlan_status st = umac_supp_add_mesh_interface(NULL);
    s_fail_at = 0;

    struct hostapd_iface *ifmsh = s_wpa_s->ifmsh;
    if (s_groups_alloc_idx == 0 || st != MMWLAN_SUCCESS || ifmsh == NULL || ifmsh->bss == NULL ||
        ifmsh->bss[0] == NULL)
    {
        printf("FAIL %s OOM: the bring-up did not get as far as the group list (allocation %u, "
               "status %d)\n", mode, s_groups_alloc_idx, (int)st);
        failures++;
        return;
    }
    struct hostapd_data *hapd = ifmsh->bss[0];
    CHECK(hapd->conf != NULL && hapd->conf->sae_groups == NULL,
          "%s OOM: the failed allocation was the group list", mode);
    /* mesh_rsn_sae_group() and index_within_array() would dereference the NULL list. */
    CHECK(s_wpa_s->mesh_rsn == NULL && ifmsh->mconf == NULL,
          "%s OOM: SAE is never armed without a group list (mesh_rsn %p, mconf %p)", mode,
          (void *)s_wpa_s->mesh_rsn, (void *)ifmsh->mconf);
    CHECK(ifmsh->conf != NULL && hapd->iconf == ifmsh->conf && hapd->conf == ifmsh->conf->bss[0],
          "%s OOM: the hostapd_config stays owned by ifmsh", mode);
    CHECK(g_warthog_sae_init == (sae ? 2u : 0u),
          "%s OOM: g_warthog_sae_init is %u, %s (got %u)", mode, sae ? 2u : 0u,
          sae ? "SAE requested, not running" : "open mesh", (unsigned)g_warthog_sae_init);

    st = umac_supp_remove_mesh_interface(NULL);
    CHECK(st == MMWLAN_SUCCESS && s_foreign_frees == 0 && s_double_frees == 0 && live_count() == live_before,
          "%s OOM: the teardown frees everything once (foreign %u, double %u, live %u -> %u)", mode,
          s_foreign_frees, s_double_frees, live_before, live_count());
    free_wpa_s(s_wpa_s);
    s_wpa_s = NULL;
}

int main(void)
{
    printf("=== mesh supplicant shim: the SAE group list hostap is handed ===\n");
    bring_up_and_down(1);
    /* A second bring-up in the same image: nothing the first teardown freed is reused. */
    bring_up_and_down(2);
    /* SAE after an earlier bring-up left the counter at 1; an open mesh from boot. */
    oom_on_group_list(true, 1);
    oom_on_group_list(false, 0);
    if (s_unexpected != 0)
    {
        printf("FAIL the bring-up reached %u stub(s) it should never call\n", s_unexpected);
        failures++;
    }
    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("all mesh shim configuration checks passed\n");
    return 0;
}
