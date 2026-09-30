/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WARTHOG_BAT_MODE_H
#define WARTHOG_BAT_MODE_H

/* BATMAN_V member mode decisions shared by the AT setters, boot, the port and the RX hook.
 * Freestanding (libc and the engine's libc-only bat.h) so the host tests build it. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct bat_config;

enum bat_mode_reason { BAT_MODE_OK, BAT_MODE_OFF, BAT_MODE_MESH_OFF, BAT_MODE_FWD,
                       BAT_MODE_BRIDGE, BAT_MODE_NO_GROUP_RX, BAT_MODE_NOMEM,
                       BAT_MODE_INIT_FAIL, BAT_MODE_MESH_FAILED };

/* Whether batman may run: batman 0 -> OFF, mesh off -> MESH_OFF, SAE without host CCMP
 * -> NO_GROUP_RX, 802.11s forwarding -> FWD, bridge -> BRIDGE, else OK. */
enum bat_mode_reason bat_mode_check(uint8_t batman, uint8_t mesh_en, uint8_t fwd, uint8_t bridge,
                                    bool sae_build, bool host_ccmp_build);
/* ok off mesh-off fwd bridge sae-no-host-ccmp nomem init-failed mesh-failed; "?" out of range. */
const char *bat_mode_reason_text(enum bat_mode_reason r);

enum bat_mode_rx { BAT_MODE_RX_BATMAN, BAT_MODE_RX_NONBAT, BAT_MODE_RX_SHORT,
                   BAT_MODE_RX_TOOBIG, BAT_MODE_RX_RELAYED, BAT_MODE_RX_PROBE };
#define BAT_MODE_RX_MAX 1600 /* largest link frame, 14-byte header included (<= BAT_MAX_LINK_FRAME) */

/* One 802.3 frame from the mesh, @p ta its 802.11 transmitter (NULL: not checked). */
enum bat_mode_rx bat_mode_rx_classify(const uint8_t *frame, size_t len, const uint8_t *ta);
/* The RA for mmwlan_tx_pkt: the destination of a unicast frame, NULL (the group shape) otherwise. */
const uint8_t *bat_mode_tx_ra(const uint8_t *frame);

/* AT+BATO= / AT+BATTG=<mac>: exactly "xx:xx:xx:xx:xx:xx", hex in either case; false leaves @p out alone. */
bool bat_mode_parse_mac(const char *s, uint8_t out[6]);

/* Soft-interface MAC: @p factory with byte 0 = 0x06, never equal to @p mesh, never BA:BE. */
void bat_mode_soft_mac(const uint8_t factory[6], const uint8_t mesh[6], uint8_t out[6]);
/* The port's part of the engine config (the rest is bat_config_defaults): originator = mesh
 * MAC, soft = bat_mode_soft_mac, tput override, and 3 copies with standard group frames. */
void bat_mode_port_config(struct bat_config *cfg, const uint8_t mesh[6], const uint8_t factory[6],
                          uint32_t tput_override, bool grp_std, uint8_t copies_override);

/* Static fallback candidate @p attempt: 10.41.253.x, x = 2 + ((soft[3..5] >> 2) + attempt) % 252,
 * so boards whose factory MACs step by 4 get distinct addresses. */
void bat_mode_static_ip(const uint8_t soft[6], unsigned attempt, uint8_t ip[4]);
/* Its gateway: 10.41.0.1, which openmanetd gives only a gate. */
void bat_mode_static_gw(uint8_t gw[4]);

/* bat0 addressing, one step per probe tick: DHCP from the first route, then ARP-probed static
 * candidates; a lease whose router stops resolving, or a held address, asks DHCP again beside it.
 * Peers hold a rebooted or newly started node's broadcasts up to ~31 s, so every attempt lasts
 * BAT_MODE_DHCP_WAIT_MS. */
#define BAT_MODE_DHCP_WAIT_MS   45000
#define BAT_MODE_STATIC_TRIES   8
#define BAT_MODE_PROBE_TICKS    2      /* quiet ticks after a candidate's first ARP probe */
#define BAT_MODE_ROUTER_OGM_MS  10000  /* the router's originator counts as heard this long */
#define BAT_MODE_ROUTER_LOSS_MS 60000  /* unreachable this long: DHCP again, doubling per restart to RETRY_MAX */
#define BAT_MODE_RETRY_FIRST_MS 30000  /* held address: first attempt, doubling per attempt */
#define BAT_MODE_RETRY_MAX_MS   600000
#define BAT_MODE_BIND_GRACE_MS  10000  /* an offer being taken at the deadline: lwIP re-requests ~11 s */

/* The lease's router: ok = TT resolves its MAC to a routed originator heard within
 * BAT_MODE_ROUTER_OGM_MS; unknown = its MAC or the engine's answer is not in yet. */
enum bat_mode_router { BAT_MODE_ROUTER_NONE, BAT_MODE_ROUTER_OK, BAT_MODE_ROUTER_LOST, BAT_MODE_ROUTER_UNKNOWN };
/* @p answer: the engine's 1 routed / 0 not / -1 not yet; @p ogm_age_ms with a 1. */
enum bat_mode_router bat_mode_router_state(bool have_router, bool mac_known, int answer, uint32_t ogm_age_ms);
const char *bat_mode_router_text(enum bat_mode_router r); /* none ok lost unknown; "?" out of range */

enum bat_mode_bat0_act {
    BAT0_NONE,
    BAT0_START_DHCP,  /* first route: bat0 up, DHCP client started, no address yet */
    BAT0_LEASED,      /* lwIP holds a lease */
    BAT0_PROBE,       /* client stopped, no address: ARP-probe candidate s->attempt */
    BAT0_STATIC,      /* take candidate s->attempt */
    BAT0_RETRY,       /* start the client beside the held address, which stays */
    BAT0_HOLD,        /* the held address went mid-attempt (NAK): re-apply it unless lwIP bound or checks an offer */
    BAT0_RETRY_FAIL,  /* stop the client; re-apply the held address with the static gateway */
    BAT0_ROUTER_LOST, /* the lease's router is unreachable: its address becomes the held one, as RETRY */
};
struct bat_mode_bat0_in {
    uint32_t elapsed_ms;   /* since the previous step */
    unsigned routes;       /* originators with a route */
    bool     dhcp;         /* AT+MESHDHCP=1 */
    bool     dhcp_running; /* lwIP's client is started */
    bool     leased;       /* it holds a lease (bound, renewing or rebinding) */
    bool     have_ip;      /* bat0 has an address */
    bool     taken;        /* candidate s->attempt answered an ARP probe */
    uint8_t  gws;          /* gateways the engine knows with a route */
    bool     binding;      /* the client is requesting or ARP-checking an offer */
    uint8_t  router;       /* enum bat_mode_router, while leased */
};
struct bat_mode_bat0 {     /* zero-initialised; outside bat_mode.c only attempt, ticks and lost_ms are read */
    uint8_t  phase, attempt, level, lost_level;
    bool     held_lease, prev_routes;
    uint8_t  prev_gws, ticks;
    uint32_t ms, lost_ms, retries, restarts;
};
enum bat_mode_bat0_act bat_mode_bat0_step(struct bat_mode_bat0 *s, const struct bat_mode_bat0_in *in);
/* Whether the next step reads @p taken for candidate s->attempt. */
bool bat_mode_bat0_probing(const struct bat_mode_bat0 *s);
/* Whether the lease's router is watched (the step reads @p router). */
bool bat_mode_bat0_leased(const struct bat_mode_bat0 *s);

/* The AT+MESHBATMAN? bat0 line, CRLF included; returns snprintf's count. */
struct bat_mode_bat0_view {
    const struct bat_mode_bat0 *s;
    uint8_t  ip[4], router_ip[4];
    uint8_t  router;       /* enum bat_mode_router */
    bool     dhcp;         /* AT+MESHDHCP=1: a held address is retried */
    bool     gw;
    uint8_t  gw_orig[6];
    uint32_t gw_down, gw_up;
};
#define BAT_MODE_BAT0_LINE 224     /* holds the line at its longest */
int bat_mode_bat0_line(const struct bat_mode_bat0_view *v, char *buf, size_t len);

/* Link throughput in 100 kbit/s units: 0 = no station, 0xFFFFFFFF = no estimate, else >= 1. */
uint32_t bat_mode_kbps_to_units(bool present, bool estab, bool rc_valid, uint32_t kbps);

/* The engine's link_tput over a cached peer-link snapshot, refreshed at most every 250 ms. */
#define BAT_MODE_LINKS_MAX      8
#define BAT_MODE_LINKS_MS       250
#define BAT_MODE_LINKS_STALE_MS 2000 /* a failed refresh falls back to a snapshot this young */
struct bat_mode_link { /* the layout of struct mmwlan_mesh_peer_link */
    uint8_t  addr[6];
    uint8_t  estab;
    uint8_t  rc_valid;
    uint32_t expected_tput_kbps;
};
struct bat_mode_links {
    struct bat_mode_link l[BAT_MODE_LINKS_MAX];
    uint8_t  n;
    bool     valid;
    uint32_t ts, fails;
};
/* true: @p out and *@p n hold a snapshot; false: the query failed and @p out is untouched. */
typedef bool (*bat_mode_links_query_t)(void *ctx, struct bat_mode_link *out, uint8_t max, uint8_t *n);
uint32_t bat_mode_link_tput(struct bat_mode_links *c, uint32_t now, const uint8_t addr[6],
                            bat_mode_links_query_t query, void *ctx);

#endif
