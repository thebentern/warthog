/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The BATMAN_V member-mode decisions in main/bat_mode.c, compiled straight out of
 * main/ -- the shipped functions, not copies:
 *
 *  - bat_mode_check: whether batman may run this boot, and why not. The AT
 *    setters (AT+MESHBATMAN=, AT+MESHFWD=, AT+MESHBRIDGE=) and the boot path use
 *    the same function, so a refusal at the console and a refusal at boot cannot
 *    disagree. Checked against the whole 64-row truth table.
 *  - bat_mode_rx_classify: what the umac event loop's RX hook does with each
 *    802.3 frame the datapath hands up, before anything is copied. Only BATMAN
 *    takes one of the six RX slots; a Linux neighbour's unicast ELP probes (two
 *    per ELP interval each) and frames another 802.11s node relayed are dropped
 *    there and counted.
 *  - the soft-interface MAC, the engine config the port builds, the RA each
 *    transmitted frame gets, and the mapping from rate control's kbit/s to the
 *    engine's 100 kbit/s throughput units over the port's cached peer snapshot.
 *  - the <mac> of AT+BATO= and AT+BATTG=: exactly six colon-separated hex pairs.
 *  - bat0 addressing: DHCP from the first route for 45 s (longer than the ~31 s a
 *    peer holds a new or rebooted node's broadcasts), then ARP-probed static
 *    candidates in 10.41.253.0/24 with 10.41.0.1 as gateway; an offer still being
 *    requested or ARP-checked at the deadline gets up to 10 s more. A held static address
 *    asks DHCP again beside itself 30 s later, the wait doubling to 10 min, sooner
 *    when a gateway not known before (a second one too) or the first route after none
 *    appears, even mid-attempt. A lease whose router has not resolved to a routed,
 *    recently heard originator at any check for 60 s asks again, keeping its address;
 *    while each next lease's router fails too, that wait doubles to 10 min, until a
 *    check passes or a gateway appears. A lease lwIP drops falls back through every
 *    candidate again. And the AT+MESHBATMAN? bat0 line.
 *
 * What this cannot tell you: whether the port wires them up. That is the glue
 * guard's job (test_glue_guard.sh) and the firmware build's.
 */
#include "bat_mode.h"

#include <stdio.h>
#include <string.h>

#include "bat.h"

/* The hook copies what bat_mode_rx_classify keeps into a BAT_MAX_LINK_FRAME slot. */
_Static_assert(BAT_MODE_RX_MAX <= BAT_MAX_LINK_FRAME, "an RX slot holds every frame the hook keeps");

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

#define UNKNOWN 0xFFFFFFFFu

static const uint8_t HARD[6] = { 0x02, 0x00, 0x00, 0x00, 0x0a, 0x01 };
static const uint8_t PEER[6] = { 0x02, 0x00, 0x00, 0x00, 0x0b, 0x01 };
static const uint8_t BC[6]   = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* [dst][src][type] + one payload byte (the batman packet type) + padding. */
static size_t mk(uint8_t *f, size_t len, const uint8_t *dst, const uint8_t *src, uint16_t type,
                 uint8_t first)
{
    memset(f, 0, len);
    memcpy(f, dst, 6);
    memcpy(f + 6, src, 6);
    f[12] = (uint8_t)(type >> 8);
    f[13] = (uint8_t)type;
    if (len > 14) {
        f[14] = first;
    }
    return len;
}

static enum bat_mode_reason expect_check(int batman, int mesh, int fwd, int bridge, int sae, int ccmp)
{
    if (!batman) return BAT_MODE_OFF;
    if (!mesh) return BAT_MODE_MESH_OFF;
    if (sae && !ccmp) return BAT_MODE_NO_GROUP_RX;
    if (fwd) return BAT_MODE_FWD;
    if (bridge) return BAT_MODE_BRIDGE;
    return BAT_MODE_OK;
}

static void t_check(void)
{
    printf("--- bat_mode_check: the full truth table ---\n");
    int bad = 0, rows = 0;
    for (int m = 0; m < 64; m++) {
        int b = m & 1, e = (m >> 1) & 1, f = (m >> 2) & 1, br = (m >> 3) & 1, s = (m >> 4) & 1,
            c = (m >> 5) & 1;
        enum bat_mode_reason got = bat_mode_check((uint8_t)b, (uint8_t)e, (uint8_t)f, (uint8_t)br,
                                                  s != 0, c != 0);
        enum bat_mode_reason want = expect_check(b, e, f, br, s, c);
        rows++;
        if (got != want) {
            printf("     row batman=%d mesh=%d fwd=%d bridge=%d sae=%d ccmp=%d: got %s want %s\n",
                   b, e, f, br, s, c, bat_mode_reason_text(got), bat_mode_reason_text(want));
            bad++;
        }
    }
    CHECK(bad == 0, "all %d rows follow off > mesh-off > sae-no-host-ccmp > fwd > bridge > ok", rows);

    CHECK(bat_mode_check(1, 1, 0, 0, false, false) == BAT_MODE_OK, "open build, leaf: ok");
    CHECK(bat_mode_check(1, 1, 0, 0, true, true) == BAT_MODE_OK, "swccmp build (SAE + host CCMP): ok");
    CHECK(bat_mode_check(1, 1, 0, 0, true, false) == BAT_MODE_NO_GROUP_RX,
          "warthog-mesh-sae (chip crypto only): refused, it cannot hear peers' group frames");
    CHECK(bat_mode_check(1, 1, 1, 1, true, false) == BAT_MODE_NO_GROUP_RX,
          "and that reason wins over fwd/bridge, the one the operator cannot fix by a setting");
    CHECK(bat_mode_check(1, 0, 1, 1, true, false) == BAT_MODE_MESH_OFF,
          "mesh off wins over everything but batman off");
    CHECK(bat_mode_check(0, 0, 1, 1, true, false) == BAT_MODE_OFF, "batman off is always off");
    CHECK(bat_mode_check(1, 1, 1, 0, false, false) == BAT_MODE_FWD, "802.11s forwarding on: refused");
    CHECK(bat_mode_check(1, 1, 0, 1, false, false) == BAT_MODE_BRIDGE, "bridge on: refused");
    CHECK(bat_mode_check(2, 1, 0, 0, false, false) == BAT_MODE_OK,
          "any nonzero stored value counts as on (the getter already bounds it to 0/1)");
}

static void t_reason_text(void)
{
    printf("--- bat_mode_reason_text: the words AT+MESHBATMAN? prints ---\n");
    static const struct { enum bat_mode_reason r; const char *t; } k[] = {
        { BAT_MODE_OK, "ok" }, { BAT_MODE_OFF, "off" }, { BAT_MODE_MESH_OFF, "mesh-off" },
        { BAT_MODE_FWD, "fwd" }, { BAT_MODE_BRIDGE, "bridge" },
        { BAT_MODE_NO_GROUP_RX, "sae-no-host-ccmp" }, { BAT_MODE_NOMEM, "nomem" },
        { BAT_MODE_INIT_FAIL, "init-failed" }, { BAT_MODE_MESH_FAILED, "mesh-failed" },
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        CHECK(strcmp(bat_mode_reason_text(k[i].r), k[i].t) == 0, "reason %d reads '%s' (got '%s')",
              (int)k[i].r, k[i].t, bat_mode_reason_text(k[i].r));
    }
    CHECK(strcmp(bat_mode_reason_text((enum bat_mode_reason)99), "?") == 0,
          "an out-of-range reason reads '?', never NULL");
}

static void t_classify(void)
{
    printf("--- bat_mode_rx_classify: what the event-loop hook keeps ---\n");
    uint8_t f[1700];
    size_t n;

    CHECK(bat_mode_rx_classify(NULL, 60, PEER) == BAT_MODE_RX_SHORT, "no frame: short");
    n = mk(f, 13, BC, PEER, 0x4305, 0);
    CHECK(bat_mode_rx_classify(f, n, PEER) == BAT_MODE_RX_SHORT, "13 bytes: short");
    n = mk(f, 14, BC, PEER, 0x4305, 0);
    CHECK(bat_mode_rx_classify(f, n, PEER) == BAT_MODE_RX_BATMAN,
          "a bare 14-byte 0x4305 header goes to the engine, which counts rx_short");
    n = mk(f, 1601, BC, PEER, 0x4305, 0x03);
    CHECK(bat_mode_rx_classify(f, n, PEER) == BAT_MODE_RX_TOOBIG, "1601 bytes: too big for a slot");
    n = mk(f, 1600, PEER, PEER, 0x4305, 0x40);
    CHECK(bat_mode_rx_classify(f, n, PEER) == BAT_MODE_RX_BATMAN, "1600 bytes (BAT_MAX_LINK_FRAME): kept");
    n = mk(f, 60, BC, PEER, 0x0800, 0x45);
    CHECK(bat_mode_rx_classify(f, n, PEER) == BAT_MODE_RX_NONBAT, "IPv4 in batman mode: dropped");
    n = mk(f, 60, BC, PEER, 0x0806, 0x00);
    CHECK(bat_mode_rx_classify(f, n, PEER) == BAT_MODE_RX_NONBAT, "ARP in batman mode: dropped");
    n = mk(f, 64, BC, PEER, 0x8100, 0x00);
    f[16] = 0x43;
    f[17] = 0x05;
    CHECK(bat_mode_rx_classify(f, n, PEER) == BAT_MODE_RX_NONBAT,
          "802.1Q-tagged 0x4305: not batman on a hard interface");
    n = mk(f, 34, BC, PEER, 0x4306, 0x03);
    CHECK(bat_mode_rx_classify(f, n, PEER) == BAT_MODE_RX_NONBAT, "0x4306 (high byte matches): dropped");
    n = mk(f, 34, BC, PEER, 0x0805, 0x03);
    CHECK(bat_mode_rx_classify(f, n, PEER) == BAT_MODE_RX_NONBAT, "0x0805 (low byte matches): dropped");
    n = mk(f, 34, BC, PEER, 0x4305, 0x03);
    CHECK(bat_mode_rx_classify(f, n, HARD) == BAT_MODE_RX_RELAYED,
          "Ethernet source is not the transmitter: relayed by another 802.11s node, dropped");
    CHECK(bat_mode_rx_classify(f, n, NULL) == BAT_MODE_RX_BATMAN,
          "no transmitter address given: the source is not checked");
    CHECK(bat_mode_rx_classify(f, n, PEER) == BAT_MODE_RX_BATMAN, "broadcast ELP from the neighbour: batman");
    n = mk(f, 214, HARD, PEER, 0x4305, 0x03);
    CHECK(bat_mode_rx_classify(f, n, PEER) == BAT_MODE_RX_PROBE,
          "200-byte unicast ELP (a Linux throughput probe): dropped as a probe");
    n = mk(f, 15, HARD, PEER, 0x4305, 0x03);
    CHECK(bat_mode_rx_classify(f, n, PEER) == BAT_MODE_RX_PROBE, "the probe test needs only the type byte");
    static const uint8_t MC[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x45 };
    n = mk(f, 34, MC, PEER, 0x4305, 0x03);
    CHECK(bat_mode_rx_classify(f, n, PEER) == BAT_MODE_RX_BATMAN,
          "ELP to a non-broadcast group: the engine's rx_mgmt_dst, not a probe");
    n = mk(f, 62, HARD, PEER, 0x4305, 0x04);
    CHECK(bat_mode_rx_classify(f, n, PEER) == BAT_MODE_RX_BATMAN,
          "unicast OGM2: to the engine, which counts rx_mgmt_dst");
    n = mk(f, 74, HARD, PEER, 0x4305, 0x40);
    CHECK(bat_mode_rx_classify(f, n, PEER) == BAT_MODE_RX_BATMAN, "UNICAST to us: batman");
    n = mk(f, 74, BC, PEER, 0x4305, 0x01);
    CHECK(bat_mode_rx_classify(f, n, PEER) == BAT_MODE_RX_BATMAN, "BCAST: batman");
}

static void t_soft_mac(void)
{
    printf("--- bat_mode_soft_mac: the bat0 MAC announced in TT ---\n");
    static const uint8_t FAC[6] = { 0x64, 0xe8, 0x33, 0x51, 0x6a, 0x10 };
    uint8_t out[6];
    bat_mode_soft_mac(FAC, HARD, out);
    CHECK(out[0] == 0x06 && memcmp(out + 1, FAC + 1, 5) == 0,
          "factory MAC with byte 0 = 0x06 (got %02x:%02x:%02x:%02x:%02x:%02x)",
          out[0], out[1], out[2], out[3], out[4], out[5]);
    CHECK((out[0] & 0x01) == 0 && (out[0] & 0x02) != 0, "unicast and locally administered");
    CHECK(memcmp(out, HARD, 6) != 0, "distinct from the mesh MAC");

    static const uint8_t MESH_SAME[6] = { 0x06, 0xe8, 0x33, 0x51, 0x6a, 0x10 };
    bat_mode_soft_mac(FAC, MESH_SAME, out);
    CHECK(out[0] == 0x02 && memcmp(out, MESH_SAME, 6) != 0,
          "would it equal the mesh MAC, byte 0 becomes 0x02 instead");

    static const uint8_t BABE[6] = { 0xba, 0xbe, 0x00, 0x00, 0x00, 0x01 };
    bat_mode_soft_mac(BABE, HARD, out);
    CHECK(!(out[0] == 0xba && out[1] == 0xbe), "never BA:BE (a loop-detect source must not enter TT)");

    uint8_t same[6];
    memcpy(same, FAC, 6);
    bat_mode_soft_mac(same, NULL, same);
    CHECK(same[0] == 0x06 && memcmp(same + 1, FAC + 1, 5) == 0, "in place, with no mesh MAC given");
}

static void t_static_ip(void)
{
    printf("--- bat_mode_static_ip / _gw: 10.41.253.x/16 candidates, 10.41.0.1 gateway ---\n");
    int bad = 0;
    for (unsigned v = 0; v < 1u << 16; v++) {
        uint8_t soft[6] = { 0x06, 0x21, 0xbf, 0x81, (uint8_t)(v >> 8), (uint8_t)v }, ip[4];
        bat_mode_static_ip(soft, v & 7, ip);
        if (ip[0] != 10 || ip[1] != 41 || ip[2] != 253 || ip[3] < 2 || ip[3] > 253) {
            bad++;
        }
    }
    CHECK(bad == 0, "every MAC and attempt maps into 10.41.253.2..253: never .0/.1/.254/.255");

    /* One batch: factory MACs 4 apart (Espressif gives each chip 4 universal MACs). */
    uint8_t seen[256] = { 0 };
    int dup = 0;
    for (unsigned k = 0; k < 252; k++) {
        uint32_t m = 0x72a1f8u + 4u * k;
        uint8_t soft[6] = { 0x06, 0x02, 0x72, (uint8_t)(m >> 16), (uint8_t)(m >> 8), (uint8_t)m }, ip[4];
        bat_mode_static_ip(soft, 0, ip);
        dup += seen[ip[3]]++ != 0;
    }
    CHECK(dup == 0, "252 boards from one batch (MACs 4 apart) get 252 distinct addresses (%d shared)", dup);

    static const uint8_t bench[5][3] = { { 0xf6, 0x81, 0x50 }, { 0xf8, 0x73, 0x8c }, { 0xf8, 0x82, 0x3c },
                                         { 0xc0, 0xe4, 0xb4 }, { 0x3c, 0x24, 0x28 } };
    uint8_t got[5];
    for (int i = 0; i < 5; i++) {
        uint8_t soft[6] = { 0x06, 0, 0, bench[i][0], bench[i][1], bench[i][2] }, ip[4];
        bat_mode_static_ip(soft, 0, ip);
        got[i] = ip[3];
    }
    int clash = 0;
    for (int i = 0; i < 5; i++) {
        for (int j = i + 1; j < 5; j++) {
            clash += got[i] == got[j];
        }
    }
    CHECK(clash == 0, "the five bench ESP32-S3s get five addresses (.%u .%u .%u .%u .%u)", got[0], got[1],
          got[2], got[3], got[4]);

    uint8_t a[6] = { 0x06, 1, 2, 0xf8, 0x74, 0x00 }, b[6] = { 0x06, 1, 2, 0xf8, 0x74, 0xfc }, ia[4], ib[4];
    bat_mode_static_ip(a, 0, ia);
    bat_mode_static_ip(b, 0, ib);
    CHECK(ia[3] != ib[3], "last bytes 0x00 and 0xfc no longer share an address (.%u, .%u)", ia[3], ib[3]);
    uint8_t c[6] = { 0x06, 1, 2, 0xf8, 0x73, 0x8c }, d[6] = { 0x06, 1, 2, 0xf8, 0x74, 0x8c }, ic[4], id[4];
    bat_mode_static_ip(c, 0, ic);
    bat_mode_static_ip(d, 0, id);
    CHECK(ic[3] != id[3], "equal last bytes on different boards: distinct (.%u, .%u)", ic[3], id[3]);

    int walk_bad = 0;
    uint8_t w0[4];
    bat_mode_static_ip(a, 0, w0);
    for (unsigned k = 1; k < BAT_MODE_STATIC_TRIES; k++) {
        uint8_t w[4];
        bat_mode_static_ip(a, k, w);
        walk_bad += w[3] != 2 + ((unsigned)(w0[3] - 2) + k) % 252;
    }
    CHECK(walk_bad == 0, "attempt k is the next slot along, wrapping inside .2..253");
    uint8_t top[6] = { 0x06, 0, 0, 0, 0, 251 * 4 % 256 }, t0[4], t1[4];
    top[4] = (uint8_t)((251 * 4) >> 8);
    bat_mode_static_ip(top, 0, t0);
    bat_mode_static_ip(top, 1, t1);
    CHECK(t0[3] == 253 && t1[3] == 2, "slot .253, one attempt on: wraps to .2 (got .%u then .%u)", t0[3], t1[3]);

    uint8_t gw[4];
    bat_mode_static_gw(gw);
    CHECK(gw[0] == 10 && gw[1] == 41 && gw[2] == 0 && gw[3] == 1,
          "gateway 10.41.0.1, the address openmanetd gives only a gate (got %u.%u.%u.%u)", gw[0], gw[1],
          gw[2], gw[3]);
    CHECK(gw[0] == ia[0] && gw[1] == ia[1], "and on-link in the static address's /16, never our own address");
}

/* ---- bat0 addressing -------------------------------------------------------- */

static const char *act_name(enum bat_mode_bat0_act a)
{
    static const char *n[] = { "none", "start-dhcp", "leased", "probe", "static", "retry", "hold",
                               "retry-fail", "router-lost" };
    return (unsigned)a < sizeof(n) / sizeof(n[0]) ? n[a] : "?";
}

#define TICK 2000 /* mesh.c's probe tick */
enum { R_NONE = BAT_MODE_ROUTER_NONE, R_OK = BAT_MODE_ROUTER_OK, R_LOST = BAT_MODE_ROUTER_LOST,
       R_UNK = BAT_MODE_ROUTER_UNKNOWN };

/* One 2 s step. f: 'd' AT+MESHDHCP=1, 'r' client running, 'l' leased, 'i' bat0 has an address,
 * 't' candidate taken, 'g' a gateway is known (one 'g' per gateway), 'b' the client is taking an offer. */
static enum bat_mode_bat0_act st(struct bat_mode_bat0 *s, unsigned routes, const char *f, int router)
{
    uint8_t gws = 0;
    for (const char *c = f; *c; c++) {
        gws += *c == 'g';
    }
    const struct bat_mode_bat0_in in = {
        .elapsed_ms = TICK, .routes = routes, .dhcp = strchr(f, 'd') != NULL,
        .dhcp_running = strchr(f, 'r') != NULL, .leased = strchr(f, 'l') != NULL,
        .have_ip = strchr(f, 'i') != NULL, .taken = strchr(f, 't') != NULL, .gws = gws,
        .binding = strchr(f, 'b') != NULL, .router = (uint8_t)router,
    };
    return bat_mode_bat0_step(s, &in);
}

/* Steps with @f until an action other than NONE; returns it, *ms = the time that took. */
static enum bat_mode_bat0_act until(struct bat_mode_bat0 *s, unsigned routes, const char *f, int router, int *ms)
{
    enum bat_mode_bat0_act a = BAT0_NONE;
    int t = 0;
    while (a == BAT0_NONE && t < 3600000) {
        t += TICK;
        a = st(s, routes, f, router);
    }
    *ms = t;
    return a;
}

/* A bat0 that took static candidate 0 after the first DHCP attempt. */
static void to_static(struct bat_mode_bat0 *s)
{
    int ms;
    memset(s, 0, sizeof(*s));
    (void)st(s, 1, "d", R_NONE);
    (void)until(s, 1, "dr", R_NONE, &ms);
    (void)st(s, 1, "d", R_NONE);
    (void)st(s, 1, "d", R_NONE);
}

/* A bat0 holding a lease. */
static void to_leased(struct bat_mode_bat0 *s)
{
    memset(s, 0, sizeof(*s));
    (void)st(s, 1, "d", R_NONE);
    (void)st(s, 1, "drli", R_UNK);
}

static void t_router_state(void)
{
    printf("--- bat_mode_router_state: is the lease's router still reachable ---\n");
    CHECK(bat_mode_router_state(false, true, 1, 0) == BAT_MODE_ROUTER_NONE, "a lease without a router: none");
    CHECK(bat_mode_router_state(true, false, -1, 0) == BAT_MODE_ROUTER_UNKNOWN, "router MAC not learned yet: unknown");
    CHECK(bat_mode_router_state(true, true, -1, 0) == BAT_MODE_ROUTER_UNKNOWN, "engine not asked yet: unknown");
    CHECK(bat_mode_router_state(true, true, 0, 0) == BAT_MODE_ROUTER_LOST,
          "TT does not resolve it to an originator with a route: lost");
    CHECK(bat_mode_router_state(true, true, 1, 0) == BAT_MODE_ROUTER_OK, "resolved, OGM just heard: ok");
    CHECK(bat_mode_router_state(true, true, 1, BAT_MODE_ROUTER_OGM_MS) == BAT_MODE_ROUTER_OK,
          "last OGM exactly %d s ago: ok", BAT_MODE_ROUTER_OGM_MS / 1000);
    CHECK(bat_mode_router_state(true, true, 1, BAT_MODE_ROUTER_OGM_MS + 1) == BAT_MODE_ROUTER_LOST,
          "resolved, but its originator silent longer (batman keeps the route 200 s): lost");
    CHECK(!strcmp(bat_mode_router_text(BAT_MODE_ROUTER_NONE), "none") &&
          !strcmp(bat_mode_router_text(BAT_MODE_ROUTER_OK), "ok") &&
          !strcmp(bat_mode_router_text(BAT_MODE_ROUTER_LOST), "lost") &&
          !strcmp(bat_mode_router_text(BAT_MODE_ROUTER_UNKNOWN), "unknown") &&
          !strcmp(bat_mode_router_text((enum bat_mode_router)9), "?"), "router texts");
}

static void t_bat0(void)
{
    printf("--- bat_mode_bat0_step: the first address (2 s ticks) ---\n");
    struct bat_mode_bat0 s = { 0 };
    int bad = 0, ms;
    for (int i = 0; i < 20; i++) {
        bad += st(&s, 0, "d", R_NONE) != BAT0_NONE;
    }
    CHECK(bad == 0 && !bat_mode_bat0_probing(&s), "no route for 40 s: nothing is started or probed");
    CHECK(st(&s, 1, "d", R_NONE) == BAT0_START_DHCP, "the first route starts DHCP on that tick");
    enum bat_mode_bat0_act a = until(&s, 3, "dr", R_NONE, &ms);
    CHECK(a == BAT0_PROBE && ms >= BAT_MODE_DHCP_WAIT_MS && ms < BAT_MODE_DHCP_WAIT_MS + TICK,
          "no lease: DHCP gives up after %d s, not before 45 s (want >= %d s; a node started or rebooted "
          "together with us holds our broadcasts ~31 s)", ms / 1000, BAT_MODE_DHCP_WAIT_MS / 1000);
    CHECK(BAT_MODE_DHCP_WAIT_MS >= 31000 + 4000 + 3000 + 1000,
          "the wait covers the 31 s hold, lwIP's 4 s DISCOVER spacing, dnsmasq's 3 s ping check and the ARP check");
    CHECK(s.attempt == 0 && bat_mode_bat0_probing(&s), "the first candidate probed is attempt 0");
    CHECK(st(&s, 3, "d", R_NONE) == BAT0_PROBE, "a quiet candidate is probed a second time");
    CHECK(st(&s, 3, "d", R_NONE) == BAT0_STATIC && s.attempt == 0 && !bat_mode_bat0_probing(&s),
          "two quiet ticks: candidate 0 is taken");

    struct bat_mode_bat0 l = { 0 };
    (void)st(&l, 1, "d", R_NONE);
    (void)st(&l, 1, "dr", R_NONE);
    (void)st(&l, 1, "dr", R_NONE);
    CHECK(st(&l, 1, "drli", R_UNK) == BAT0_LEASED && bat_mode_bat0_leased(&l), "a lease at 6 s is kept");
    bad = 0;
    for (int i = 0; i < 60; i++) {
        bad += st(&l, i < 30 ? 1 : 0, "drli", R_OK) != BAT0_NONE;
    }
    CHECK(bad == 0, "leased with its router reachable: nothing, even when routes go");

    struct bat_mode_bat0 late = { 0 };
    (void)st(&late, 1, "d", R_NONE);
    a = BAT0_NONE;
    for (int t = TICK; t < BAT_MODE_DHCP_WAIT_MS && a == BAT0_NONE; t += TICK) {
        a = st(&late, 1, t + TICK >= BAT_MODE_DHCP_WAIT_MS ? "drli" : "dr", R_UNK);
    }
    CHECK(a == BAT0_LEASED, "a lease on the last tick before the deadline is kept (%s)", act_name(a));

    struct bat_mode_bat0 f = { 0 };
    (void)st(&f, 1, "d", R_NONE);
    CHECK(st(&f, 1, "d", R_NONE) == BAT0_PROBE, "the DHCP client failed to start: straight to the static candidates");

    struct bat_mode_bat0 b = { 0 };
    (void)st(&b, 1, "d", R_NONE);
    for (int t = TICK; t < BAT_MODE_DHCP_WAIT_MS - TICK; t += TICK) {
        (void)st(&b, 1, "dr", R_NONE);
    }
    a = BAT0_NONE;
    for (int i = 0; i < 3 && a == BAT0_NONE; i++) {
        a = st(&b, 1, "drb", R_NONE);
    }
    CHECK(a == BAT0_NONE, "an offer being requested or ARP-checked at the deadline is not cut off (%s)", act_name(a));
    CHECK(st(&b, 1, "drli", R_UNK) == BAT0_LEASED, "and its lease is kept");
    memset(&b, 0, sizeof(b));
    (void)st(&b, 1, "d", R_NONE);
    a = until(&b, 1, "drb", R_NONE, &ms);
    CHECK(a == BAT0_PROBE && ms >= BAT_MODE_DHCP_WAIT_MS + BAT_MODE_BIND_GRACE_MS &&
          ms < BAT_MODE_DHCP_WAIT_MS + BAT_MODE_BIND_GRACE_MS + TICK,
          "but not waited for beyond %d s more (%d s)", BAT_MODE_BIND_GRACE_MS / 1000, ms / 1000);
    to_static(&b);
    (void)until(&b, 1, "di", R_NONE, &ms);
    for (int t = TICK; t < BAT_MODE_DHCP_WAIT_MS; t += TICK) {
        (void)st(&b, 1, "dri", R_NONE);
    }
    CHECK(st(&b, 1, "drib", R_NONE) == BAT0_NONE && st(&b, 1, "drli", R_UNK) == BAT0_LEASED,
          "the same for an attempt beside a held address");

    struct bat_mode_bat0 off = { 0 };
    CHECK(st(&off, 1, "", R_NONE) == BAT0_PROBE, "AT+MESHDHCP=0: the first route probes candidate 0 at once");
    (void)st(&off, 1, "", R_NONE);
    CHECK(st(&off, 1, "", R_NONE) == BAT0_STATIC, "and takes it after two quiet ticks");
    bad = 0;
    for (int i = 0; i < 1000; i++) {
        bad += st(&off, (unsigned)(i & 1), (i & 2) ? "gi" : "i", R_NONE) != BAT0_NONE;
    }
    CHECK(bad == 0, "AT+MESHDHCP=0 keeps its static address and never asks DHCP, whatever routes and gateways do");

    struct bat_mode_bat0 c = { 0 };
    (void)st(&c, 1, "", R_NONE);
    CHECK(st(&c, 1, "t", R_NONE) == BAT0_PROBE && c.attempt == 1, "candidate 0 answered the probe: candidate 1 is probed");
    (void)st(&c, 1, "", R_NONE);
    CHECK(st(&c, 1, "t", R_NONE) == BAT0_PROBE && c.attempt == 2, "an answer to the second probe also moves on");
    (void)st(&c, 1, "", R_NONE);
    CHECK(st(&c, 1, "", R_NONE) == BAT0_STATIC && c.attempt == 2, "candidate 2 stays quiet: taken");

    struct bat_mode_bat0 all = { 0 };
    (void)st(&all, 1, "", R_NONE);
    int probes = 0;
    a = BAT0_PROBE;
    while (a == BAT0_PROBE && probes < 50) {
        probes++;
        a = st(&all, 1, "t", R_NONE);
    }
    CHECK(a == BAT0_STATIC && all.attempt == BAT_MODE_STATIC_TRIES && probes == BAT_MODE_STATIC_TRIES,
          "every one of %d candidates answered: the next is taken unprobed rather than none (%s, attempt %u)",
          BAT_MODE_STATIC_TRIES, act_name(a), all.attempt);

    struct bat_mode_bat0 d = { 0 };
    to_leased(&d);
    CHECK(st(&d, 1, "dr", R_OK) == BAT0_NONE && !bat_mode_bat0_leased(&d),
          "lwIP dropped the lease (NAK, expiry) and asks again: waited for like the first");
    a = until(&d, 1, "dr", R_NONE, &ms);
    CHECK(a == BAT0_PROBE && ms >= BAT_MODE_DHCP_WAIT_MS && ms < BAT_MODE_DHCP_WAIT_MS + TICK,
          "no new lease within the wait: the static candidates (%s after %d s)", act_name(a), ms / 1000);

    struct bat_mode_bat0 e = { 0 };
    (void)st(&e, 1, "d", R_NONE);
    (void)until(&e, 1, "dr", R_NONE, &ms);
    for (int i = 0; i < 5; i++) {
        (void)st(&e, 1, "t", R_NONE);
    }
    (void)st(&e, 1, "", R_NONE);
    CHECK(st(&e, 1, "", R_NONE) == BAT0_STATIC && e.attempt == 5, "candidates 0..4 answered: 5 taken");
    CHECK(until(&e, 1, "di", R_NONE, &ms) == BAT0_RETRY, "DHCP again beside it");
    CHECK(st(&e, 1, "drli", R_UNK) == BAT0_LEASED, "and a lease");
    (void)st(&e, 1, "dr", R_OK);
    a = until(&e, 1, "dr", R_NONE, &ms);
    CHECK(a == BAT0_PROBE && e.attempt == 0, "a lease lost after static candidate 5: the probe starts over at 0 "
          "(attempt %u)", e.attempt);
    probes = 0;
    while (a == BAT0_PROBE && probes < 50) {
        probes++;
        a = st(&e, 1, "t", R_NONE);
    }
    CHECK(a == BAT0_STATIC && probes == BAT_MODE_STATIC_TRIES,
          "and takes one unprobed only after %d answers, like the first fallback (%d probed)", BAT_MODE_STATIC_TRIES,
          probes);
}

static void t_bat0_retry(void)
{
    printf("--- bat_mode_bat0_step: DHCP again beside a static address ---\n");
    struct bat_mode_bat0 s;
    int ms, bad;
    to_static(&s);
    enum bat_mode_bat0_act a = until(&s, 2, "di", R_NONE, &ms);
    CHECK(a == BAT0_RETRY && ms == BAT_MODE_RETRY_FIRST_MS && !bat_mode_bat0_probing(&s),
          "static with routes: the first background attempt %d s later (%s)", ms / 1000, act_name(a));
    a = until(&s, 2, "dri", R_NONE, &ms);
    CHECK(a == BAT0_RETRY_FAIL && ms >= BAT_MODE_DHCP_WAIT_MS && ms < BAT_MODE_DHCP_WAIT_MS + TICK,
          "an attempt keeps the address and lasts %d s like the first, then gives up (%s)", ms / 1000, act_name(a));
    uint32_t want = BAT_MODE_RETRY_FIRST_MS * 2;
    bad = 0;
    for (int k = 0; k < 8; k++) {
        a = until(&s, 2, "di", R_NONE, &ms);
        bad += a != BAT0_RETRY || (uint32_t)ms != want;
        if (a != BAT0_RETRY || (uint32_t)ms != want) {
            printf("     attempt %d after %d s, want %lu s (%s)\n", k + 2, ms / 1000, (unsigned long)want / 1000, act_name(a));
        }
        (void)until(&s, 2, "dri", R_NONE, &ms);
        want = want * 2 > BAT_MODE_RETRY_MAX_MS ? BAT_MODE_RETRY_MAX_MS : want * 2;
    }
    CHECK(bad == 0, "every failed attempt doubles the wait, 60 s up to %d min", BAT_MODE_RETRY_MAX_MS / 60000);
    CHECK(s.retries == 9 && s.restarts == 0, "9 attempts counted (%lu)", (unsigned long)s.retries);

    for (int i = 0; i < 5; i++) {
        (void)st(&s, 2, "di", R_NONE);
    }
    CHECK(st(&s, 2, "dig", R_NONE) == BAT0_NONE, "a gateway appears 12 s after an attempt: not at once");
    a = until(&s, 2, "dig", R_NONE, &ms);
    CHECK(a == BAT0_RETRY && ms + 12000 == BAT_MODE_RETRY_FIRST_MS,
          "but 30 s after the last attempt, not after 10 min (%s at %d s)", act_name(a), (ms + 12000) / 1000);
    (void)until(&s, 2, "drig", R_NONE, &ms);
    a = until(&s, 2, "dig", R_NONE, &ms);
    CHECK(a == BAT0_RETRY && ms == 2 * BAT_MODE_RETRY_FIRST_MS, "and doubling from there (%d s)", ms / 1000);
    (void)until(&s, 2, "drig", R_NONE, &ms);

    for (int i = 0; i < 20; i++) {
        (void)st(&s, 0, "dig", R_NONE);
    }
    bad = 0;
    for (int i = 0; i < 400; i++) {
        bad += st(&s, 0, "dig", R_NONE) != BAT0_NONE;
    }
    CHECK(bad == 0, "no route at all: nothing to ask, however long");
    CHECK(st(&s, 3, "dig", R_NONE) == BAT0_RETRY, "the first route after none: at once, the wait long over");
    (void)until(&s, 3, "drig", R_NONE, &ms);
    CHECK(s.level >= 1, "setup: the wait has doubled");
    for (int i = 0; i < 5; i++) {
        (void)st(&s, 3, "dig", R_NONE);
    }
    for (int i = 0; i < 10; i++) {
        (void)st(&s, 0, "dig", R_NONE);
    }
    a = until(&s, 1, "dig", R_NONE, &ms);
    CHECK(a == BAT0_RETRY && ms == TICK, "routes gone and back 30 s after an attempt: asked at once, not after the "
          "doubled wait (%d s)", ms / 1000);
    CHECK(st(&s, 3, "drig", R_NONE) == BAT0_NONE, "the attempt runs");
    CHECK(st(&s, 3, "dr", R_NONE) == BAT0_HOLD, "a NAK or a declined offer took the address: re-applied");
    CHECK(st(&s, 3, "dr", R_NONE) == BAT0_HOLD, "every tick it is gone");
    CHECK(st(&s, 3, "dri", R_NONE) == BAT0_NONE, "and left alone while it is there");
    CHECK(st(&s, 3, "drli", R_UNK) == BAT0_LEASED && bat_mode_bat0_leased(&s), "a lease replaces it");
    CHECK(s.level == 0 && !s.held_lease, "and the doubling starts over");

    to_static(&s);
    for (int k = 0; k < 5; k++) {
        (void)until(&s, 2, "di", R_NONE, &ms);
        (void)until(&s, 2, "dri", R_NONE, &ms);
    }
    (void)until(&s, 2, "di", R_NONE, &ms);
    for (int t = TICK; t < BAT_MODE_DHCP_WAIT_MS; t += TICK) {
        (void)st(&s, 2, "dri", R_NONE);
    }
    CHECK(st(&s, 2, "drig", R_NONE) == BAT0_RETRY_FAIL, "a gateway first heard on an attempt's last tick: it still ends");
    a = until(&s, 2, "dig", R_NONE, &ms);
    CHECK(a == BAT0_RETRY && ms == BAT_MODE_RETRY_FIRST_MS, "but the next comes 30 s after it, not 10 min (%s at %d s)",
          act_name(a), ms / 1000);
    (void)until(&s, 2, "drig", R_NONE, &ms);
    (void)until(&s, 2, "dig", R_NONE, &ms);
    for (int i = 0; i < 5; i++) {
        (void)st(&s, 0, "dri", R_NONE);
    }
    CHECK(until(&s, 2, "dri", R_NONE, &ms) == BAT0_RETRY_FAIL, "routes gone and back while an attempt runs");
    a = until(&s, 2, "di", R_NONE, &ms);
    CHECK(a == BAT0_RETRY && ms == BAT_MODE_RETRY_FIRST_MS, "the next attempt 30 s after it too (%s at %d s)",
          act_name(a), ms / 1000);

    to_static(&s);
    for (int k = 0; k < 6; k++) {
        (void)until(&s, 2, "dig", R_NONE, &ms);
        (void)until(&s, 2, "drig", R_NONE, &ms);
    }
    for (int i = 0; i < 5; i++) {
        (void)st(&s, 2, "dig", R_NONE);
    }
    CHECK(s.level >= 5, "setup: one gateway all along, the wait at its cap");
    CHECK(st(&s, 2, "digg", R_NONE) == BAT0_NONE, "a second gateway 12 s after an attempt: not at once");
    a = until(&s, 2, "digg", R_NONE, &ms);
    CHECK(a == BAT0_RETRY && ms + 12000 == BAT_MODE_RETRY_FIRST_MS,
          "but 30 s after the last attempt, not after 10 min: a gate joining another, or replacing one whose route "
          "lingers 200 s (%s at %d s)", act_name(a), (ms + 12000) / 1000);
    for (int k = 0; k < 6; k++) {
        (void)until(&s, 2, "drigg", R_NONE, &ms);
        (void)until(&s, 2, "digg", R_NONE, &ms);
    }
    (void)until(&s, 2, "drigg", R_NONE, &ms);
    a = until(&s, 2, "digg", R_NONE, &ms);
    CHECK(a == BAT0_RETRY && ms == BAT_MODE_RETRY_MAX_MS,
          "the same two gateways all along, whichever is best: the wait stays %d min (%d s)",
          BAT_MODE_RETRY_MAX_MS / 60000, ms / 1000);
    (void)until(&s, 2, "drigg", R_NONE, &ms);
    a = until(&s, 2, "dig", R_NONE, &ms);
    CHECK(a == BAT0_RETRY && ms == BAT_MODE_RETRY_MAX_MS, "one of them gone: still %d min (%d s)",
          BAT_MODE_RETRY_MAX_MS / 60000, ms / 1000);

    to_static(&s);
    (void)until(&s, 2, "di", R_NONE, &ms);
    CHECK(st(&s, 2, "di", R_NONE) == BAT0_RETRY_FAIL, "the client would not start: the attempt ends at once");

    to_static(&s);
    s.ms = 0xFFFFFFFFu - 1000;
    (void)st(&s, 0, "di", R_NONE);
    (void)st(&s, 0, "di", R_NONE);
    CHECK(st(&s, 1, "di", R_NONE) == BAT0_RETRY, "held 49.7 days without a route: the time saturates, due at once");

    to_static(&s);
    s.level = 250;
    a = until(&s, 2, "di", R_NONE, &ms);
    CHECK(a == BAT0_RETRY && ms == BAT_MODE_RETRY_MAX_MS, "the doubling does not overflow (%d s)", ms / 1000);
}

static void t_bat0_router(void)
{
    printf("--- bat_mode_bat0_step: the lease's router goes ---\n");
    struct bat_mode_bat0 s;
    int ms, bad;
    to_leased(&s);
    enum bat_mode_bat0_act a = until(&s, 2, "drli", R_LOST, &ms);
    CHECK(a == BAT0_ROUTER_LOST && ms == BAT_MODE_ROUTER_LOSS_MS && s.restarts == 1,
          "router unreachable at every check for %d s: DHCP again (%s)", ms / 1000, act_name(a));
    CHECK(st(&s, 2, "dri", R_NONE) == BAT0_NONE && !bat_mode_bat0_leased(&s),
          "the lease address is kept while it asks (lwIP's client restarted, no lease)");
    CHECK(st(&s, 2, "drli", R_UNK) == BAT0_LEASED && bat_mode_bat0_leased(&s), "another node's lease: leased again");

    to_leased(&s);
    bad = 0;
    for (int round = 0; round < 5; round++) {
        for (int t = 0; t < BAT_MODE_ROUTER_LOSS_MS - TICK; t += TICK) {
            bad += st(&s, 2, "drli", R_LOST) != BAT0_NONE;
        }
        bad += st(&s, 2, "drli", R_OK) != BAT0_NONE;
    }
    CHECK(bad == 0 && s.restarts == 0, "58 s unreachable, then heard once, five times over: no restart (hysteresis)");
    a = until(&s, 2, "drli", R_UNK, &ms);
    CHECK(a == BAT0_ROUTER_LOST && ms == BAT_MODE_ROUTER_LOSS_MS,
          "a router whose MAC never resolves counts as unreachable (%d s)", ms / 1000);

    to_leased(&s);
    bad = 0;
    for (int i = 0; i < 1000; i++) {
        bad += st(&s, (unsigned)(i & 1), "drli", R_NONE) != BAT0_NONE;
    }
    CHECK(bad == 0, "a lease without a router option is never restarted");
    to_leased(&s);
    bad = 0;
    for (int i = 0; i < 1000; i++) {
        bad += st(&s, 2, "rli", R_LOST) != BAT0_NONE;
    }
    CHECK(bad == 0, "AT+MESHDHCP=0 since: no restart");

    to_leased(&s);
    (void)until(&s, 0, "drli", R_LOST, &ms);
    a = until(&s, 0, "dri", R_NONE, &ms);
    CHECK(a == BAT0_RETRY_FAIL && ms >= BAT_MODE_DHCP_WAIT_MS && ms < BAT_MODE_DHCP_WAIT_MS + TICK && s.held_lease,
          "isolated: the restart gets nothing and the lease address is held (%s after %d s)", act_name(a), ms / 1000);
    CHECK(st(&s, 0, "di", R_NONE) == BAT0_NONE, "held, no route: nothing to ask");
    for (int i = 0; i < 30; i++) {
        (void)st(&s, 0, "di", R_NONE);
    }
    CHECK(st(&s, 1, "di", R_NONE) == BAT0_RETRY, "a route again: asked at once");

    /* A router the check can never pass (its TT row does not fit) while the same server hands
     * the same lease straight back: each restart's DHCP exchange takes 4 s. */
    to_leased(&s);
    int waits[8];
    for (int k = 0; k < 8; k++) {
        a = until(&s, 3, "drli", R_LOST, &ms);
        waits[k] = a == BAT0_ROUTER_LOST ? ms : -1;
        (void)st(&s, 3, "drbi", R_LOST);
        (void)st(&s, 3, "drli", R_UNK);
    }
    bad = 0;
    uint32_t want = BAT_MODE_ROUTER_LOSS_MS;
    for (int k = 0; k < 8; k++) {
        if ((uint32_t)waits[k] != want) {
            bad++;
            printf("     restart %d after %d s, want %lu s\n", k + 1, waits[k] / 1000, (unsigned long)want / 1000);
        }
        want = want * 2 > BAT_MODE_RETRY_MAX_MS ? BAT_MODE_RETRY_MAX_MS : want * 2;
    }
    CHECK(bad == 0 && s.restarts == 8, "the same router back each time: restarts after 60 s, then 120, 240, 480 s "
          "and every %d min (%lu restarts)", BAT_MODE_RETRY_MAX_MS / 60000, (unsigned long)s.restarts);
    to_leased(&s);
    unsigned day = 0;
    for (long t = 0; t < 86400000L; t += ms) {
        if (until(&s, 3, "drli", R_LOST, &ms) == BAT0_ROUTER_LOST) {
            day++;
            (void)st(&s, 3, "drbi", R_LOST);
            (void)st(&s, 3, "drli", R_UNK);
            t += 2 * TICK;
        }
    }
    CHECK(day <= 86400u / (BAT_MODE_RETRY_MAX_MS / 1000) + 4, "%u mesh-wide DHCP exchanges in 24 h, not one a minute",
          day);
    for (int k = 0; k < 3; k++) {
        (void)until(&s, 3, "drli", R_LOST, &ms);
        (void)st(&s, 3, "drli", R_UNK);
    }
    CHECK(st(&s, 3, "drli", R_OK) == BAT0_NONE, "setup: one check passes");
    a = until(&s, 3, "drli", R_LOST, &ms);
    CHECK(a == BAT0_ROUTER_LOST && ms == BAT_MODE_ROUTER_LOSS_MS,
          "a check that passed starts the waits over: the next loss restarts after 60 s (%d s)", ms / 1000);
    for (int k = 0; k < 3; k++) {
        (void)st(&s, 3, "drli", R_UNK);
        (void)until(&s, 3, "drli", R_LOST, &ms);
    }
    (void)st(&s, 3, "drli", R_UNK);
    CHECK(st(&s, 3, "drlig", R_LOST) == BAT0_NONE, "setup: a gateway appears 2 s into the fourth wait");
    a = until(&s, 3, "drlig", R_LOST, &ms);
    CHECK(a == BAT0_ROUTER_LOST && ms + TICK == BAT_MODE_ROUTER_LOSS_MS,
          "a new gateway (maybe a new DHCP server) starts them over too (%d s)", (ms + TICK) / 1000);
    to_leased(&s);
    for (int k = 0; k < 3; k++) {
        (void)until(&s, 3, "drli", R_LOST, &ms);
        (void)st(&s, 3, "drli", R_UNK);
    }
    (void)st(&s, 3, "drli", R_NONE);
    a = until(&s, 3, "drli", R_LOST, &ms);
    CHECK(a == BAT0_ROUTER_LOST && ms == BAT_MODE_ROUTER_LOSS_MS,
          "so does a lease with nothing to check (no router option) (%d s)", ms / 1000);
    to_leased(&s);
    s.lost_level = 255;
    a = until(&s, 3, "drli", R_LOST, &ms);
    (void)st(&s, 3, "drli", R_UNK);
    int ms2;
    enum bat_mode_bat0_act a2 = until(&s, 3, "drli", R_LOST, &ms2);
    CHECK(a == BAT0_ROUTER_LOST && a2 == BAT0_ROUTER_LOST && ms == BAT_MODE_RETRY_MAX_MS && ms2 == BAT_MODE_RETRY_MAX_MS,
          "the doubling neither overflows nor wraps (%d s, then %d s)", ms / 1000, ms2 / 1000);
}

static void t_bat0_line(void)
{
    printf("--- bat_mode_bat0_line: the AT+MESHBATMAN? bat0 line ---\n");
    char buf[512];
    struct bat_mode_bat0 s;
    to_leased(&s);
    for (int i = 0; i < 6; i++) {
        (void)st(&s, 1, "drli", R_LOST);
    }
    struct bat_mode_bat0_view v = { .s = &s, .ip = { 10, 41, 0, 124 }, .router_ip = { 10, 41, 0, 2 },
                                    .router = R_LOST, .gw = true, .gw_orig = { 2, 0xd4, 0x0a, 0, 0, 1 },
                                    .gw_down = 100, .gw_up = 20 };
    int n = bat_mode_bat0_line(&v, buf, sizeof(buf));
    const char *want = "+MESHBATMAN: bat0 addr=leased ip=10.41.0.124 router=10.41.0.2(lost 12s) retry_in=- "
                       "retries=0 restarts=0 gw=02:d4:0a:00:00:01(10.0/2.0)\r\n";
    CHECK(n == (int)strlen(want) && !strcmp(buf, want), "leased, router lost 12 s, a gateway: [%s]", buf);
    v.dhcp = true;
    (void)bat_mode_bat0_line(&v, buf, sizeof(buf));
    CHECK(strstr(buf, " router=10.41.0.2(lost 12s) retry_in=48s "), "with DHCP on, retry_in counts down to the "
          "restart at 60 s [%s]", buf);
    {
        struct bat_mode_bat0 r;
        int ms;
        to_leased(&r);
        (void)until(&r, 1, "drli", R_LOST, &ms);
        for (int i = 0; i < 6; i++) {
            (void)st(&r, 1, "drli", R_UNK); /* a new lease, its router not learned yet: 10 s */
        }
        struct bat_mode_bat0_view w = { .s = &r, .router_ip = { 10, 41, 0, 3 }, .router = R_UNK, .dhcp = true };
        (void)bat_mode_bat0_line(&w, buf, sizeof(buf));
        CHECK(strstr(buf, " router=10.41.0.3(unknown 10s) retry_in=110s retries=0 restarts=1 "),
              "after a restart the wait doubles: 120 s less 10 s [%s]", buf);
        (void)st(&r, 1, "drli", R_OK);
        w.router = R_OK;
        (void)bat_mode_bat0_line(&w, buf, sizeof(buf));
        CHECK(strstr(buf, "(ok) retry_in=- "), "a router that passes: nothing due [%s]", buf);
    }
    to_static(&s);
    (void)st(&s, 1, "di", R_NONE);
    v = (struct bat_mode_bat0_view){ .s = &s, .ip = { 10, 41, 253, 7 }, .router_ip = { 10, 41, 0, 1 }, .dhcp = true };
    (void)bat_mode_bat0_line(&v, buf, sizeof(buf));
    CHECK(!strcmp(buf, "+MESHBATMAN: bat0 addr=static ip=10.41.253.7 router=10.41.0.1 retry_in=28s retries=0 "
                       "restarts=0 gw=none\r\n"), "static, next attempt in 28 s: [%s]", buf);
    v.dhcp = false;
    (void)bat_mode_bat0_line(&v, buf, sizeof(buf));
    CHECK(strstr(buf, " addr=static ") && strstr(buf, " retry_in=- "), "AT+MESHDHCP=0: no attempt due [%s]", buf);
    v.dhcp = true;
    to_leased(&s);
    (void)until(&s, 1, "drli", R_LOST, &n);
    (void)st(&s, 1, "dri", R_NONE);
    v.s = &s;
    (void)bat_mode_bat0_line(&v, buf, sizeof(buf));
    CHECK(strstr(buf, " addr=retry ") && strstr(buf, " restarts=1 "), "asking again: addr=retry [%s]", buf);
    (void)until(&s, 1, "dri", R_NONE, &n);
    (void)bat_mode_bat0_line(&v, buf, sizeof(buf));
    CHECK(strstr(buf, " addr=held ") && strstr(buf, " retry_in=30s "), "a former lease held: addr=held [%s]", buf);
    struct bat_mode_bat0 z = { 0 };
    v = (struct bat_mode_bat0_view){ .s = &z, .dhcp = true };
    (void)bat_mode_bat0_line(&v, buf, sizeof(buf));
    CHECK(strstr(buf, " addr=idle ip=0.0.0.0 "), "before the first route: addr=idle [%s]", buf);

    struct bat_mode_bat0 big;
    to_static(&big);
    big.phase = s.phase; /* held */
    big.retries = big.restarts = 0xFFFFFFFFu;
    big.level = 255; /* retry_in=600s */
    big.ms = 0;
    struct bat_mode_bat0 bl;
    to_leased(&bl);
    bl.lost_ms = 0xFFFFFFFFu;
    bl.retries = bl.restarts = 0xFFFFFFFFu;
    int worst = 0;
    const struct bat_mode_bat0 *ss[2] = { &big, &bl };
    for (int k = 0; k < 2; k++) {
        v = (struct bat_mode_bat0_view){ .s = ss[k], .ip = { 255, 255, 255, 255 }, .router_ip = { 255, 255, 255, 255 },
                                         .router = R_UNK, .gw = true, .gw_orig = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff },
                                         .gw_down = 0xFFFFFFFFu, .gw_up = 0xFFFFFFFFu, .dhcp = true };
        n = bat_mode_bat0_line(&v, buf, sizeof(buf));
        worst = n > worst ? n : worst;
    }
    CHECK(worst < BAT_MODE_BAT0_LINE, "at its longest %d bytes, under BAT_MODE_BAT0_LINE %d", worst, BAT_MODE_BAT0_LINE);
    n = bat_mode_bat0_line(&v, buf, 20);
    CHECK(n > 20 && strlen(buf) == 19, "a short buffer is cut, NUL-terminated");
}

static void t_kbps(void)
{
    printf("--- bat_mode_kbps_to_units: rate control's kbit/s to the engine's 100 kbit/s ---\n");
    CHECK(bat_mode_kbps_to_units(false, true, true, 7200) == 0, "not in the peer snapshot: 0, no station");
    CHECK(bat_mode_kbps_to_units(true, false, true, 7200) == 0, "an SAE candidate not yet keyed: 0");
    CHECK(bat_mode_kbps_to_units(true, true, false, 0) == UNKNOWN,
          "ESTAB, rate control without a best rate yet: unknown (the engine samples 10)");
    CHECK(bat_mode_kbps_to_units(true, true, true, 0) == UNKNOWN, "a valid flag with 0 kbit/s is still unknown");
    CHECK(bat_mode_kbps_to_units(true, true, false, 7200) == UNKNOWN, "no valid rate: unknown whatever the kbit/s says");
    CHECK(bat_mode_kbps_to_units(true, true, true, 50) == 1, "50 kbit/s -> 1, never 0 for a live peer");
    CHECK(bat_mode_kbps_to_units(true, true, true, 99) == 1, "99 kbit/s -> 1");
    CHECK(bat_mode_kbps_to_units(true, true, true, 100) == 1, "100 kbit/s -> 1");
    CHECK(bat_mode_kbps_to_units(true, true, true, 199) == 1, "199 kbit/s -> 1 (truncated, like batman)");
    CHECK(bat_mode_kbps_to_units(true, true, true, 300) == 3, "300 kbit/s (MCS0 at 1 MHz) -> 3");
    CHECK(bat_mode_kbps_to_units(true, true, true, 7200) == 72, "7200 kbit/s -> 72");
    CHECK(bat_mode_kbps_to_units(true, true, true, 0xFFFFFFFFu) == 42949672u,
          "the largest estimate stays below the unknown marker");
}

/* ---- the port's link-throughput source over the peer snapshot --------------- */

static struct bat_mode_link q_links[BAT_MODE_LINKS_MAX];
static uint8_t q_n;
static int q_calls, q_fail;

static bool fake_query(void *ctx, struct bat_mode_link *out, uint8_t max, uint8_t *n)
{
    (void)ctx;
    q_calls++;
    if (q_fail) {
        return false;
    }
    uint8_t k = q_n < max ? q_n : max;
    memcpy(out, q_links, k * sizeof(*out));
    *n = k;
    return true;
}

static void set_link(int i, const uint8_t *addr, int estab, int rc, uint32_t kbps)
{
    memcpy(q_links[i].addr, addr, 6);
    q_links[i].estab = (uint8_t)estab;
    q_links[i].rc_valid = (uint8_t)rc;
    q_links[i].expected_tput_kbps = kbps;
}

static void t_link_tput(void)
{
    printf("--- bat_mode_link_tput: the engine's link_tput over the cached peer snapshot ---\n");
    static const uint8_t A[6] = { 2, 0, 0, 0, 0xa, 1 }, B[6] = { 2, 0, 0, 0, 0xb, 1 },
                         C[6] = { 2, 0, 0, 0, 0xc, 1 }, D[6] = { 2, 0, 0, 0, 0xd, 1 };
    struct bat_mode_links c;
    memset(&c, 0, sizeof(c));
    set_link(0, A, 1, 1, 7200);
    set_link(1, B, 0, 1, 7200);
    set_link(2, C, 1, 0, 0);
    q_n = 3;
    uint32_t t = 1000;
    CHECK(bat_mode_link_tput(&c, t, A, fake_query, NULL) == 72, "ESTAB, 7200 kbit/s: 72");
    CHECK(bat_mode_link_tput(&c, t, B, fake_query, NULL) == 0, "an unkeyed SAE candidate: 0");
    CHECK(bat_mode_link_tput(&c, t, C, fake_query, NULL) == UNKNOWN, "ESTAB, rate control not ready: unknown");
    CHECK(bat_mode_link_tput(&c, t, D, fake_query, NULL) == 0, "not in the snapshot: 0, no station");
    CHECK(q_calls == 1, "one snapshot serves every lookup of an ELP round (%d queries)", q_calls);

    set_link(0, A, 1, 1, 300);
    CHECK(bat_mode_link_tput(&c, t + 249, A, fake_query, NULL) == 72 && q_calls == 1, "+249 ms: the cached value");
    CHECK(bat_mode_link_tput(&c, t + 250, A, fake_query, NULL) == 3 && q_calls == 2,
          "+250 ms: refreshed, the new rate reads 3");

    q_n = 0;
    t += 500;
    CHECK(bat_mode_link_tput(&c, t, A, fake_query, NULL) == 0, "the peer left the snapshot: 0");

    q_n = 1;
    t += 500;
    CHECK(bat_mode_link_tput(&c, t, A, fake_query, NULL) == 3, "back: 3");
    q_fail = 1;
    int calls = q_calls;
    t += 500;
    CHECK(bat_mode_link_tput(&c, t, A, fake_query, NULL) == 3 && c.fails == 1,
          "a failed refresh keeps the last snapshot, not 'no station' (fails=%lu)", (unsigned long)c.fails);
    CHECK(bat_mode_link_tput(&c, t + 1, A, fake_query, NULL) == 3 && q_calls == calls + 2,
          "and the very next lookup retries instead of waiting 250 ms");
    q_fail = 0;
    CHECK(bat_mode_link_tput(&c, t + 2, A, fake_query, NULL) == 3 && c.ts == t + 2, "a retry that works is cached");
    q_fail = 1;
    CHECK(bat_mode_link_tput(&c, t + 2 + BAT_MODE_LINKS_STALE_MS, A, fake_query, NULL) == UNKNOWN,
          "failing for %d ms: unknown, the engine's default sample", BAT_MODE_LINKS_STALE_MS);
    q_fail = 0;

    struct bat_mode_links first;
    memset(&first, 0, sizeof(first));
    q_fail = 1;
    CHECK(bat_mode_link_tput(&first, 5, A, fake_query, NULL) == UNKNOWN && !first.valid,
          "the first query ever fails: unknown, and nothing is cached");
    q_fail = 0;
    CHECK(bat_mode_link_tput(&first, 6, A, fake_query, NULL) == 3 && first.valid, "the next lookup takes one");

    struct bat_mode_links w;
    memset(&w, 0, sizeof(w));
    q_n = 1;
    set_link(0, A, 1, 1, 7200);
    (void)bat_mode_link_tput(&w, 0xFFFFFF00u, A, fake_query, NULL);
    calls = q_calls;
    set_link(0, A, 1, 1, 300);
    CHECK(bat_mode_link_tput(&w, 0x10u, A, fake_query, NULL) == 3 && q_calls == calls + 1,
          "the ms clock wrapping counts as time passed");

    struct bat_mode_links many;
    memset(&many, 0, sizeof(many));
    for (int i = 0; i < BAT_MODE_LINKS_MAX; i++) {
        uint8_t m[6] = { 2, 0, 0, 1, 0, (uint8_t)i };
        set_link(i, m, 1, 1, 100u * (uint32_t)(i + 1));
    }
    q_n = BAT_MODE_LINKS_MAX;
    uint8_t last[6] = { 2, 0, 0, 1, 0, BAT_MODE_LINKS_MAX - 1 };
    CHECK(bat_mode_link_tput(&many, 1, last, fake_query, NULL) == BAT_MODE_LINKS_MAX,
          "the %dth peer of a full snapshot is found", BAT_MODE_LINKS_MAX);
}

static void t_tx_ra(void)
{
    printf("--- bat_mode_tx_ra: which frames go to one ESTAB peer ---\n");
    uint8_t f[64];
    size_t n = mk(f, 64, PEER, HARD, 0x4305, 0x40);
    CHECK(bat_mode_tx_ra(f) == f, "unicast: RA = the destination, that peer only (%zu bytes)", n);
    (void)mk(f, 64, BC, HARD, 0x4305, 0x03);
    CHECK(bat_mode_tx_ra(f) == NULL, "ff:ff (ELP, OGM, BCAST): NULL, the datapath's group shape");
    static const uint8_t MC4[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x45 }, MC6[6] = { 0x33, 0x33, 0, 0, 0, 1 };
    (void)mk(f, 64, MC4, HARD, 0x4305, 0x03);
    CHECK(bat_mode_tx_ra(f) == NULL, "an IPv4 group address: NULL");
    (void)mk(f, 64, MC6, HARD, 0x4305, 0x03);
    CHECK(bat_mode_tx_ra(f) == NULL, "an IPv6 group address: NULL");
}

static void t_parse_mac(void)
{
    printf("--- bat_mode_parse_mac: the <mac> of AT+BATO= and AT+BATTG= ---\n");
    static const uint8_t WANT[6] = { 0x0c, 0xbf, 0x74, 0x28, 0xbf, 0xcd };
    uint8_t m[6];
    memset(m, 0, 6);
    CHECK(bat_mode_parse_mac("0c:bf:74:28:bf:cd", m) && memcmp(m, WANT, 6) == 0, "0c:bf:74:28:bf:cd");
    memset(m, 0, 6);
    CHECK(bat_mode_parse_mac("0C:BF:74:28:Bf:cD", m) && memcmp(m, WANT, 6) == 0, "either case");
    static const char *const BAD[] = { "", "0c:bf:74:28:bf", "0c:bf:74:28:bf:cd:01", "0c:bf:74:28:bf:c",
                                       "c:bf:74:28:bf:cd0", "0c-bf-74-28-bf-cd", "0cbf7428bfcd", "0c:bf:74:28:bf:cg",
                                       "0c:bf:74:28:bf:cd ", " 0c:bf:74:28:bf:cd", "0c:bf:74:28:bf:", "0c::bf:74:28:bf" };
    int refused = 0;
    for (unsigned i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        memset(m, 0xa5, 6);
        const bool ok = bat_mode_parse_mac(BAD[i], m);
        refused += !ok && m[0] == 0xa5 && m[5] == 0xa5;
        if (ok) {
            printf("     accepted \"%s\"\n", BAD[i]);
        }
    }
    CHECK(refused == (int)(sizeof(BAD) / sizeof(BAD[0])),
          "short, long, one-digit, '-' or no separators, non-hex, spaces: refused, the output untouched (%d of %zu)",
          refused, sizeof(BAD) / sizeof(BAD[0]));
    CHECK(!bat_mode_parse_mac(NULL, m), "NULL: refused");
}

static void t_port_config(void)
{
    printf("--- bat_mode_port_config: the engine config the port builds (membership M2) ---\n");
    static const uint8_t MESH[6] = { 0x0c, 0xbf, 0x74, 0x28, 0xbf, 0xcd };
    static const uint8_t FAC[6] = { 0x64, 0xe8, 0x33, 0x51, 0x6a, 0x10 };
    struct bat_config cfg;
    memset(&cfg, 0xa5, sizeof(cfg));
    cfg.aggregate_ogm = false; /* sentinels; a bool may only hold 0 or 1 */
    cfg.half_duplex = true;
    const struct bat_config before = cfg;
    bat_mode_port_config(&cfg, MESH, FAC, 25, false, 0);
    uint8_t soft[6];
    bat_mode_soft_mac(FAC, MESH, soft);
    CHECK(memcmp(cfg.hard_addr, MESH, 6) == 0, "originator = the 802.11s mesh MAC (M2)");
    CHECK(memcmp(cfg.soft_addr, soft, 6) == 0 && cfg.soft_addr[0] == 0x06, "soft MAC = bat_mode_soft_mac");
    CHECK(memcmp(cfg.soft_addr, cfg.hard_addr, 6) != 0, "the two differ");
    CHECK(cfg.tput_override == 25, "AT+MESHBATTP passes through (25)");
    CHECK(cfg.bcast_copies == 1, "replicas (MESHGRP=0): one copy, each ACKed per peer");
    CHECK(cfg.hard_mtu == before.hard_mtu && cfg.elp_interval_ms == before.elp_interval_ms &&
          cfg.ogm_interval_ms == before.ogm_interval_ms && cfg.hop_penalty == before.hop_penalty &&
          cfg.aggregate_ogm == before.aggregate_ogm && cfg.half_duplex == before.half_duplex,
          "every other field is left to bat_config_defaults");
    bat_mode_port_config(&cfg, MESH, FAC, 0, true, 0);
    CHECK(cfg.bcast_copies == 3 && cfg.tput_override == 0, "standard group frames (MESHGRP=1): 3 copies; 0 = rate control");
    bat_mode_port_config(&cfg, MESH, FAC, 0xFFFFFFFFu, true, 2);
    CHECK(cfg.bcast_copies == 2 && cfg.tput_override == 0xFFFFFFFFu, "a build-time copy count wins");
    static const uint8_t SAME[6] = { 0x06, 0xe8, 0x33, 0x51, 0x6a, 0x10 };
    bat_mode_port_config(&cfg, SAME, FAC, 0, false, 0);
    CHECK(memcmp(cfg.hard_addr, SAME, 6) == 0 && memcmp(cfg.soft_addr, SAME, 6) != 0,
          "a factory MAC that would equal the mesh MAC still yields a distinct soft MAC");
}

int main(void)
{
    printf("=== bat_mode: BATMAN_V member-mode decisions (main/bat_mode.c) ===\n");
    t_check();
    t_reason_text();
    t_classify();
    t_soft_mac();
    t_port_config();
    t_tx_ra();
    t_parse_mac();
    t_static_ip();
    t_router_state();
    t_bat0();
    t_bat0_retry();
    t_bat0_router();
    t_bat0_line();
    t_kbps();
    t_link_tput();
    if (failures) {
        printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_bat_mode: all passed\n");
    return 0;
}
