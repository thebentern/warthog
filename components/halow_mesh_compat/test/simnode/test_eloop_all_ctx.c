/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * hostap's ELOOP_ALL_CTX wildcard through Morse's eloop shim
 * (supplicant_shim/eloop.c) and umac's timeout queue (umac/core/umac_timeout.c),
 * and the mesh station free that relies on it, through hostap's sta_info.c
 * (ap_sta_add, ap_free_sta) and mesh_mpm.c (mesh_mpm_auth_peer,
 * mesh_mpm_free_sta, plink_timer). All four are the shipping sources.
 *
 * ap_free_sta() runs mesh_sta_free_cb (mesh_mpm_free_sta) just before it frees
 * the station, and that cancels plink_timer and mesh_auth_timer as
 * (handler, ELOOP_ALL_CTX, sta), while every registration passes (wpa_s, sta).
 * hostap's own eloop.c matches ELOOP_ALL_CTX against any context. A shim that
 * compares it by value cancels nothing, so a peer freed in the AMPE window (a
 * new SAE Commit for a station in SAE_ACCEPTED, ieee802_11.c) or during SAE
 * (mesh_mpm_kickout_peer) leaves its timer on freed memory: plink_timer then
 * sends Open for the dead station and, after its retries and HOLDING,
 * ap_free_sta()s it a second time.
 *
 * Asserted:
 *  - the shim's eloop_cancel_timeout() honours ELOOP_ALL_CTX in either
 *    argument or both, at the head of the queue and behind it, removes only
 *    that handler's timeouts, and returns how many it removed, as hostap's
 *    eloop.c does. NULL is not a wildcard (umac's own callers pass it), and
 *    eloop_cancel_timeout_one(), eloop_is_timeout_registered() and
 *    eloop_deplete_timeout() stay exact matches, as hostap's are.
 *  - a station freed after mesh_mpm_auth_peer() (Open sent, plink_timer armed),
 *    or after an SAE start armed mesh_auth_timer, is named by no timeout, and
 *    30 s of clock afterwards sends nothing for it, runs no handler on it and
 *    frees nothing twice.
 *
 * os_free() keeps each freed block, poisoned under ASan, so a timer that fires
 * on a freed station is a use-after-poison report under SAN=1 and a counted
 * send or double free without it, never undefined behaviour. The clock is the
 * test's. Everything else the sources reference is a stub; the ones the driven
 * paths must not reach print their name and count.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "umac/data/umac_data.h"
#include "umac/core/umac_core_private.h"
#include "umac/supplicant_shim/umac_supp_shim_private.h"
#include "hostap/src/utils/eloop.h"
#include "hostap/src/ap/hostapd.h"
#include "hostap/src/ap/ap_config.h"
#include "hostap/src/ap/sta_info.h"
#include "hostap/src/ap/ieee802_11.h"
#include "hostap/src/ap/ieee802_1x.h"
#include "hostap/src/ap/ap_drv_ops.h"
#include "hostap/src/ap/ap_mlme.h"
#include "hostap/src/ap/beacon.h"
#include "hostap/src/ap/hw_features.h"
#include "hostap/src/ap/wpa_auth.h"
#include "hostap/src/common/hw_features_common.h"
#include "hostap/src/common/sae.h"
#include "hostap/src/crypto/crypto.h"
#include "hostap/wpa_supplicant/config_ssid.h"
#include "hostap/wpa_supplicant/driver_i.h"
#include "hostap/wpa_supplicant/notify.h"
#include "hostap/wpa_supplicant/mesh_mpm.h"
#include "hostap/wpa_supplicant/mesh_rsn.h"
#pragma GCC diagnostic pop

#if defined(__SANITIZE_ADDRESS__)
#define TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TEST_ASAN 1
#endif
#endif
#ifdef TEST_ASAN
#include <sanitizer/asan_interface.h>
#define POISON(p, n)   ASAN_POISON_MEMORY_REGION(p, n)
#define UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION(p, n)
#else
#define POISON(p, n)   ((void)(p), (void)(n))
#define UNPOISON(p, n) ((void)(p), (void)(n))
#endif

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static unsigned s_unexpected;
#define UNEXPECTED() (printf("unexpected call: %s\n", __func__), s_unexpected++)

/* ---- an os_* allocator that keeps what it frees ------------------------- */

#define MAX_BLOCKS 256
struct block
{
    unsigned char *p;
    size_t size;
    bool dead;
};
static struct block s_blocks[MAX_BLOCKS];
static unsigned s_nblocks, s_double_frees, s_foreign_frees;

/* The block p points into, live or freed; NULL if p is not os_* memory. */
static struct block *block_of(const void *p)
{
    const unsigned char *c = p;
    for (unsigned i = 0; i < s_nblocks; i++)
    {
        if (c >= s_blocks[i].p && c < s_blocks[i].p + s_blocks[i].size) { return &s_blocks[i]; }
    }
    return NULL;
}

static bool is_dead(const void *p)
{
    const struct block *b = block_of(p);
    return b != NULL && b->dead;
}

void *os_malloc(size_t size)
{
    if (s_nblocks == MAX_BLOCKS) { fprintf(stderr, "block table full\n"); abort(); }
    struct block *b = &s_blocks[s_nblocks++];
    b->size = size ? size : 1;
    b->p = malloc(b->size);
    b->dead = false;
    if (b->p == NULL) { abort(); }
    return b->p;
}

void *os_zalloc(size_t size)
{
    void *p = os_malloc(size);
    memset(p, 0, size);
    return p;
}

void os_free(void *ptr)
{
    if (ptr == NULL) { return; }
    struct block *b = block_of(ptr);
    if (b == NULL || b->p != ptr) { s_foreign_frees++; return; }
    if (b->dead) { s_double_frees++; return; }
    b->dead = true;
    POISON(b->p, b->size);
}

void *os_realloc(void *ptr, size_t size)
{
    void *n = os_malloc(size);
    struct block *b = block_of(ptr);
    if (b != NULL && !b->dead) { memcpy(n, ptr, b->size < size ? b->size : size); os_free(ptr); }
    return n;
}

static void release_all(void)
{
    for (unsigned i = 0; i < s_nblocks; i++)
    {
        UNPOISON(s_blocks[i].p, s_blocks[i].size);
        free(s_blocks[i].p);
    }
    s_nblocks = 0;
}

void bin_clear_free(void *bin, size_t len)
{
    if (bin != NULL) { memset(bin, 0, len); os_free(bin); }
}

void *os_memcpy(void *dest, const void *src, size_t n) { return memcpy(dest, src, n); }
void *os_memset(void *s, int c, size_t n) { return memset(s, c, n); }
int os_memcmp(const void *s1, const void *s2, size_t n) { return memcmp(s1, s2, n); }
size_t os_strlen(const char *s) { return strlen(s); }

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

int os_snprintf(char *str, size_t size, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(str, size, format, ap);
    va_end(ap);
    return n;
}

/* ---- the clock and the umac core around the timeout queue -------------- */

static uint32_t s_now_ms = 100000;
static struct umac_core_data s_core;

uint32_t mmosal_get_time_ms(void) { return s_now_ms; }
void mmosal_task_enter_critical(void) {}
void mmosal_task_exit_critical(void) {}
void *mmosal_calloc_(size_t nitems, size_t size) { return calloc(nitems, size); }
void mmosal_free(void *p) { free(p); }
void mmosal_log_failure_info(const struct mmosal_failure_info *info) { (void)info; }
void mmosal_impl_assert(void) { fprintf(stderr, "MMOSAL_ASSERT\n"); abort(); }

struct umac_data *umac_data_get_umacd(void) { return (struct umac_data *)&s_core; }
struct umac_core_data *umac_data_get_core(struct umac_data *umacd) { return &s_core; }
bool umac_core_is_running(struct umac_data *umacd) { return true; }
void umac_core_evt_wake(struct umac_data *umacd) {}

int os_get_reltime(struct os_reltime *t)
{
    t->sec = s_now_ms / 1000;
    t->usec = (s_now_ms % 1000) * 1000;
    return 0;
}

int os_get_random(unsigned char *buf, size_t len)
{
    memset(buf, 0x5a, len);
    return 0;
}

unsigned long os_random(void) { return 0x5a5a; }

/* Timeouts whose context or user data is p. */
static unsigned queued_for(const void *p)
{
    unsigned n = 0;
    for (const struct umac_core_timeout *t = s_core.toq.head; t != NULL; t = t->next)
    {
        n += t->arg1 == p || t->arg2 == p;
    }
    return n;
}

static unsigned queued(void)
{
    unsigned n = 0;
    for (const struct umac_core_timeout *t = s_core.toq.head; t != NULL; t = t->next) { n++; }
    return n;
}

/* The clock ms on, in 10 ms steps, dispatching whatever falls due as the event loop does. */
static void run_clock(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 10)
    {
        s_now_ms += 10;
        while (umac_timeoutq_dispatch(&s_core) != 0) {}
    }
}

/* ---- the mesh interface ------------------------------------------------- */

static struct wpa_supplicant s_wpa_s;
static struct hostapd_iface s_ifmsh;
static struct hostapd_data s_hapd;
static struct hostapd_data *s_bss[1];
static struct hostapd_config s_iconf;
static struct hostapd_bss_config s_conf;
static struct mesh_conf s_mconf;
static struct mesh_rsn s_rsn;
static struct wpa_driver_ops s_drv;
static unsigned s_sends, s_sends_dead, s_auth_timer_runs;

static const u8 k_own[ETH_ALEN] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const u8 k_peer[ETH_ALEN] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };

static int drv_sta_add(void *priv, struct hostapd_sta_add_params *params) { return 0; }

int wpa_drv_send_action(struct wpa_supplicant *wpa_s, unsigned int freq, unsigned int wait,
                        const u8 *dst, const u8 *src, const u8 *bssid, const u8 *data,
                        size_t data_len, int no_cck)
{
    s_sends++;
    s_sends_dead += is_dead(dst);
    return 0;
}

/* mesh_rsn.c's, which this test does not link: it reads the station as that one does. */
void mesh_auth_timer(void *eloop_ctx, void *user_data)
{
    struct sta_info *sta = user_data;
    s_auth_timer_runs++;
    if (sta->sae->state == SAE_ACCEPTED) { return; }
}

static void fresh_queue(void)
{
    memset(&s_core, 0, sizeof(s_core));
    umac_timeoutq_init(&s_core.toq);
}

/* A fresh queue, allocator and mesh interface; the configuration is a mesh SAE build's. */
static void setup(void)
{
    fresh_queue();
    release_all();
    s_double_frees = s_foreign_frees = s_unexpected = 0;
    s_sends = s_sends_dead = s_auth_timer_runs = 0;

    memset(&s_wpa_s, 0, sizeof(s_wpa_s));
    memset(&s_ifmsh, 0, sizeof(s_ifmsh));
    memset(&s_hapd, 0, sizeof(s_hapd));
    memset(&s_iconf, 0, sizeof(s_iconf));
    memset(&s_conf, 0, sizeof(s_conf));
    memset(&s_mconf, 0, sizeof(s_mconf));
    memset(&s_drv, 0, sizeof(s_drv));

    s_drv.sta_add = drv_sta_add;
    s_conf.mesh = MESH_ENABLED;
    s_conf.max_num_sta = 4;
    s_conf.ap_max_inactivity = AP_MAX_INACTIVITY;
    s_mconf.security = MESH_CONF_SEC_AUTH | MESH_CONF_SEC_AMPE;
    s_mconf.dot11MeshMaxRetries = DEFAULT_MESH_MAX_RETRIES;
    s_mconf.dot11MeshRetryTimeout = DEFAULT_MESH_RETRY_TIMEOUT;
    s_mconf.dot11MeshConfirmTimeout = DEFAULT_MESH_CONFIRM_TIMEOUT;
    s_mconf.dot11MeshHoldingTimeout = DEFAULT_MESH_HOLDING_TIMEOUT;

    s_bss[0] = &s_hapd;
    s_ifmsh.bss = s_bss;
    s_ifmsh.num_bss = 1;
    s_ifmsh.conf = &s_iconf;
    s_ifmsh.mconf = &s_mconf;
    s_ifmsh.owner = &s_wpa_s;
    s_hapd.iface = &s_ifmsh;
    s_hapd.iconf = &s_iconf;
    s_hapd.conf = &s_conf;
    s_hapd.max_plinks = 4;
    /* As supplicant_core_mesh.c's passive bring-up sets it. */
    s_hapd.mesh_sta_free_cb = mesh_mpm_free_sta;
    s_wpa_s.ifmsh = &s_ifmsh;
    s_wpa_s.driver = &s_drv;
    s_wpa_s.mesh_rsn = &s_rsn;
    memcpy(s_wpa_s.own_addr, k_own, ETH_ALEN);
}

/* ---- the shim's wildcard ------------------------------------------------ */

static unsigned s_h1_runs, s_h2_runs;
static void h1(void *eloop_ctx, void *user_data) { s_h1_runs++; }
static void h2(void *eloop_ctx, void *user_data) { s_h2_runs++; }
static int s_ctx_a, s_ctx_b, s_data_x, s_data_y;
#define A ((void *)&s_ctx_a)
#define B ((void *)&s_ctx_b)
#define X ((void *)&s_data_x)
#define Y ((void *)&s_data_y)

static void test_shim_wildcard(void)
{
    printf("--- eloop_cancel_timeout() and ELOOP_ALL_CTX ---\n");
    setup();
    s_h1_runs = s_h2_runs = 0;

    /* Due in this order: two matches at the head, a non-match, then a match behind it. */
    eloop_register_timeout(0, 1000, h1, A, X);
    eloop_register_timeout(0, 2000, h1, B, X);
    eloop_register_timeout(0, 3000, h1, A, Y);
    eloop_register_timeout(0, 4000, h2, A, X);
    eloop_register_timeout(0, 5000, h1, B, X);
    CHECK(queued() == 5, "five timeouts queued (%u)", queued());
    int n = eloop_cancel_timeout(h1, ELOOP_ALL_CTX, X);
    CHECK(n == 3, "(h1, ELOOP_ALL_CTX, x) cancels h1 for x under every context (%d of 3)", n);
    CHECK(queued() == 2 && eloop_is_timeout_registered(h1, A, Y) && eloop_is_timeout_registered(h2, A, X),
          "it leaves h1's other user data and h2 (%u queued)", queued());
    n = eloop_cancel_timeout(h1, A, ELOOP_ALL_CTX);
    CHECK(n == 1 && !eloop_is_timeout_registered(h1, A, Y),
          "(h1, a, ELOOP_ALL_CTX) cancels h1 under a for any user data (%d of 1)", n);

    /* Each block below starts from an empty queue, so one failure does not carry into the next. */
    fresh_queue();
    eloop_register_timeout(0, 1000, h2, A, X);
    eloop_register_timeout(0, 2000, h2, B, Y);
    n = eloop_cancel_timeout(h2, ELOOP_ALL_CTX, ELOOP_ALL_CTX);
    CHECK(n == 2 && queued() == 0, "(h2, ELOOP_ALL_CTX, ELOOP_ALL_CTX) cancels every h2 (%d of 2)", n);

    fresh_queue();
    eloop_register_timeout(0, 1000, h1, A, NULL);
    eloop_register_timeout(0, 2000, h1, A, X);
    n = eloop_cancel_timeout(h1, A, NULL);
    CHECK(n == 1 && eloop_is_timeout_registered(h1, A, X), "NULL is not a wildcard: (h1, a, NULL) leaves (h1, a, x)");
    n = eloop_cancel_timeout(h1, A, X);
    CHECK(n == 1 && eloop_cancel_timeout(h1, A, X) == 0, "an exact cancel removes its one and then finds none");

    fresh_queue();
    eloop_register_timeout(0, 1000, h1, A, X);
    struct os_reltime rem;
    CHECK(!eloop_is_timeout_registered(h1, ELOOP_ALL_CTX, X) && !eloop_is_timeout_registered(h1, A, ELOOP_ALL_CTX),
          "eloop_is_timeout_registered() matches exactly, as hostap's does");
    CHECK(eloop_cancel_timeout_one(h1, ELOOP_ALL_CTX, X, &rem) == 0 &&
          eloop_deplete_timeout(0, 0, h1, ELOOP_ALL_CTX, X) == -1 && eloop_is_timeout_registered(h1, A, X),
          "eloop_cancel_timeout_one() and eloop_deplete_timeout() match exactly, as hostap's do");

    fresh_queue();
    eloop_register_timeout(0, 1000, h1, A, X);
    eloop_register_timeout(0, 1000, h2, A, X);
    n = eloop_cancel_timeout(h1, ELOOP_ALL_CTX, ELOOP_ALL_CTX);
    run_clock(20);
    CHECK(n == 1 && s_h1_runs == 0 && s_h2_runs == 1, "a cancelled timeout never runs, the other handler's does");
    CHECK(s_unexpected == 0, "no stub the shim must not reach was called (%u)", s_unexpected);
}

/* ---- a mesh station freed with its timers armed ------------------------ */

static void after_free(struct sta_info *sta, const char *what)
{
    CHECK(is_dead(sta), "ap_free_sta() freed the station %s", what);
    unsigned n = queued_for(sta);
    CHECK(n == 0, "no timeout names the freed station (%u do)", n);
    run_clock(30000);
    CHECK(s_sends_dead == 0, "30 s on, nothing was sent for the freed station (%u frames)", s_sends_dead);
    CHECK(s_double_frees == 0, "nothing was freed twice (%u)", s_double_frees);
    CHECK(s_hapd.num_sta == 0 && s_hapd.sta_list == NULL, "the interface holds no station (num_sta %d)",
          s_hapd.num_sta);
    CHECK(s_unexpected == 0 && s_foreign_frees == 0, "no unexpected stub (%u) and no foreign free (%u)",
          s_unexpected, s_foreign_frees);
}

static void test_free_in_ampe_window(void)
{
    printf("--- a peer in SAE_ACCEPTED freed while its Open is out ---\n");
    setup();
    /* mesh_mpm_add_peer(), then SAE to SAE_ACCEPTED (sae_accept_sta()). */
    struct sta_info *sta = ap_sta_add(&s_hapd, k_peer);
    CHECK(sta != NULL, "ap_sta_add() made the station");
    if (sta == NULL) { return; }
    sta->sae = os_zalloc(sizeof(*sta->sae));
    sta->sae->state = SAE_ACCEPTED;
    /* auth_start_ampe(). */
    mesh_mpm_auth_peer(&s_wpa_s, k_peer);
    CHECK(sta->plink_state == PLINK_OPN_SNT && s_sends == 1, "mesh_mpm_auth_peer() sent Open (%u sent)", s_sends);
    CHECK(queued_for(sta) == 2, "plink_timer and ap_handle_timer name the station (%u)", queued_for(sta));
    /* ieee802_11.c sae_sm_step(): a new Commit from the peer, SAE_ACCEPTED, mesh. */
    ap_free_sta(&s_hapd, sta);
    CHECK(s_sends == 2 && s_sends_dead == 0, "mesh_mpm_free_sta() sent Close before the free (%u sent)", s_sends);
    after_free(sta, "in the AMPE window");
}

static void test_free_during_sae(void)
{
    printf("--- a peer freed while SAE's mesh_auth_timer is armed ---\n");
    setup();
    struct sta_info *sta = ap_sta_add(&s_hapd, k_peer);
    CHECK(sta != NULL, "ap_sta_add() made the station");
    if (sta == NULL) { return; }
    sta->sae = os_zalloc(sizeof(*sta->sae));
    sta->sae->state = SAE_COMMITTED;
    /* mesh_rsn_auth_sae_sta() after its Commit: MESH_AUTH_TIMEOUT + rnd. */
    eloop_register_timeout(10, 0, mesh_auth_timer, &s_wpa_s, sta);
    CHECK(queued_for(sta) == 2, "mesh_auth_timer and ap_handle_timer name the station (%u)", queued_for(sta));
    /* mesh_mpm_kickout_peer(). */
    ap_free_sta(&s_hapd, sta);
    after_free(sta, "during SAE");
    CHECK(s_auth_timer_runs == 0, "mesh_auth_timer never ran on it (%u runs)", s_auth_timer_runs);
}

int main(void)
{
    test_shim_wildcard();
    test_free_in_ampe_window();
    test_free_during_sae();
    release_all();
    printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}

/* ---- reached on the paths driven: the no-op the real one is here -------- */

void mesh_rsn_init_ampe_sta(struct wpa_supplicant *wpa_s, struct sta_info *sta) {}
void mesh_rsn_get_pmkid(struct mesh_rsn *rsn, struct sta_info *sta, u8 *pmkid) { memset(pmkid, 0, PMKID_LEN); }
int mesh_rsn_protect_frame(struct mesh_rsn *rsn, struct sta_info *sta, const u8 *cat, struct wpabuf *buf)
{
    return 0;
}
u8 *hostapd_eid_supp_rates(struct hostapd_data *hapd, u8 *eid) { return eid; }
u8 *hostapd_eid_ext_supp_rates(struct hostapd_data *hapd, u8 *eid) { return eid; }
int hostapd_set_sta_flags(struct hostapd_data *hapd, struct sta_info *sta) { return 0; }
void ht40_intolerant_remove(struct hostapd_iface *iface, struct sta_info *sta) {}
int hostapd_ht_operation_update(struct hostapd_iface *iface) { return 0; }
int ieee802_11_update_beacons(struct hostapd_iface *iface) { return 0; }
void sae_clear_retransmit_timer(struct hostapd_data *hapd, struct sta_info *sta) {}
void sae_clear_data(struct sae_data *sae) {}
void ieee802_1x_free_station(struct hostapd_data *hapd, struct sta_info *sta) {}
void wpa_auth_sta_deinit(struct wpa_state_machine *sm) {}
void hostapd_free_psk_list(struct hostapd_sta_wpa_psk_short *psk) {}
void crypto_ecdh_deinit(struct crypto_ecdh *ecdh) {}
void wpa_auth_pmksa_remove(struct wpa_authenticator *wpa_auth, const u8 *sta_addr) {}
void umac_mesh_plink_failed(const u8 *addr) {}
void warthog_sae_trace(unsigned int n) {}
void wpas_notify_mesh_peer_disconnected(struct wpa_supplicant *wpa_s, const u8 *peer_addr, u16 reason_code) {}

/* Logging: silent. */
void wpa_printf(int level, const char *fmt, ...) {}
void wpa_msg(void *ctx, int level, const char *fmt, ...) {}
void wpa_msg_no_global(void *ctx, int level, const char *fmt, ...) {}
void hostapd_logger(void *ctx, const u8 *addr, unsigned int module, int level, const char *fmt, ...) {}
void wpa_hexdump_key(int level, const char *title, const void *buf, size_t len) {}
int wpa_snprintf_hex(char *buf, size_t buf_size, const u8 *data, size_t len)
{
    if (buf_size != 0) { buf[0] = '\0'; }
    return 0;
}

/* ---- not reached on the paths driven ------------------------------------ */

int eloop_replenish_timeout(unsigned int req_secs, unsigned int req_usecs, eloop_timeout_handler handler,
                            void *eloop_data, void *user_data) { UNEXPECTED(); return -1; }
u8 *hostapd_eid_ht_capabilities(struct hostapd_data *hapd, u8 *eid) { UNEXPECTED(); return eid; }
u8 *hostapd_eid_ht_operation(struct hostapd_data *hapd, u8 *eid) { UNEXPECTED(); return eid; }
int hostapd_get_aid(struct hostapd_data *hapd, struct sta_info *sta) { UNEXPECTED(); return -1; }
u16 copy_sta_ht_capab(struct hostapd_data *hapd, struct sta_info *sta, const u8 *ht_capab)
{
    UNEXPECTED();
    return 0;
}
int update_ht_state(struct hostapd_data *hapd, struct sta_info *sta) { UNEXPECTED(); return 0; }
void set_disable_ht40(struct ieee80211_ht_capabilities *htcaps, int disabled) { UNEXPECTED(); }
int ieee802_11_mgmt(struct hostapd_data *hapd, const u8 *buf, size_t len, struct hostapd_frame_info *fi)
{
    UNEXPECTED();
    return 0;
}
ParseRes ieee802_11_parse_elems(const u8 *start, size_t len, struct ieee802_11_elems *elems, int show_errors)
{
    UNEXPECTED();
    return ParseFailed;
}
void ieee802_11_send_sa_query_req(struct hostapd_data *hapd, const u8 *addr, const u8 *trans_id) { UNEXPECTED(); }
void hostapd_prune_associations(struct hostapd_data *hapd, const u8 *addr, int mld_assoc_link_id) { UNEXPECTED(); }
int hostapd_sta_add(struct hostapd_data *hapd, const u8 *addr, u16 aid, u16 capability, const u8 *supp_rates,
                    size_t supp_rates_len, u16 listen_interval,
                    const struct ieee80211_ht_capabilities *ht_capab,
                    const struct ieee80211_vht_capabilities *vht_capab,
                    const struct ieee80211_he_capabilities *he_capab, size_t he_capab_len,
                    const struct ieee80211_eht_capabilities *eht_capab, size_t eht_capab_len,
                    const struct ieee80211_he_6ghz_band_cap *he_6ghz_capab, u32 flags, u8 qosinfo,
                    u8 vht_opmode, int supp_p2p_ps, int set, const u8 *link_addr, bool mld_link_sta,
                    u16 eml_cap)
{
    UNEXPECTED();
    return -1;
}
int hostapd_set_wds_sta(struct hostapd_data *hapd, char *ifname_wds, const u8 *addr, int aid, int val)
{
    UNEXPECTED();
    return -1;
}
int hostapd_drv_sta_deauth(struct hostapd_data *hapd, const u8 *addr, int reason) { UNEXPECTED(); return -1; }
int hostapd_drv_sta_disassoc(struct hostapd_data *hapd, const u8 *addr, int reason) { UNEXPECTED(); return -1; }
void ieee802_1x_notify_port_enabled(struct eapol_state_machine *sm, bool enabled) { UNEXPECTED(); }
void mlme_deauthenticate_indication(struct hostapd_data *hapd, struct sta_info *sta, u16 reason_code)
{
    UNEXPECTED();
}
void mlme_disassociate_indication(struct hostapd_data *hapd, struct sta_info *sta, u16 reason_code)
{
    UNEXPECTED();
}
const u8 *wpa_auth_get_dpp_pkhash(struct wpa_state_machine *sm) { UNEXPECTED(); return NULL; }
const u8 *wpa_auth_get_pmk(struct wpa_state_machine *sm, int *len) { UNEXPECTED(); return NULL; }
struct rsn_pmksa_cache_entry *wpa_auth_pmksa_get(struct wpa_authenticator *wpa_auth, const u8 *sta_addr,
                                                 const u8 *pmkid)
{
    UNEXPECTED();
    return NULL;
}
int wpa_auth_sm_event(struct wpa_state_machine *sm, enum wpa_event event) { UNEXPECTED(); return -1; }
enum wpa_alg wpa_cipher_to_alg(int cipher) { UNEXPECTED(); return WPA_ALG_NONE; }
size_t merge_byte_arrays(u8 *res, size_t res_len, const u8 *src1, size_t src1_len, const u8 *src2,
                         size_t src2_len)
{
    UNEXPECTED();
    return 0;
}
int hexstr2bin(const char *hex, u8 *buf, size_t len) { UNEXPECTED(); return -1; }
int mesh_rsn_auth_sae_sta(struct wpa_supplicant *wpa_s, struct sta_info *sta) { UNEXPECTED(); return -1; }
int mesh_rsn_derive_mtk(struct wpa_supplicant *wpa_s, struct sta_info *sta) { UNEXPECTED(); return -1; }
int mesh_rsn_process_ampe(struct wpa_supplicant *wpa_s, struct sta_info *sta, struct ieee802_11_elems *elems,
                          const u8 *cat, const u8 *chosen_pmk, const u8 *start, size_t elems_len)
{
    UNEXPECTED();
    return -1;
}
void wpas_notify_mesh_peer_connected(struct wpa_supplicant *wpa_s, const u8 *peer_addr) { UNEXPECTED(); }
