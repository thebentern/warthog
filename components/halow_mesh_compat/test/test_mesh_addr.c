/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Plain mesh addressing (batman off): what mesh_addr_step decides per 2 s probe tick.
 *
 * The first ESTAB starts DHCP with no address for MESH_ADDR_DHCP_WAIT_MS (an offer being taken
 * at the deadline gets MESH_ADDR_BIND_GRACE_MS more); then the static 10.77 address goes on and
 * DHCP is asked again beside it for MESH_ADDR_ATTEMPT_MS (on air, OpenWrt's dnsmasq offered ~3 s
 * after a DISCOVER, and a new peer's first copies were lost), 30 s later doubling to 600 s, or at
 * once when a peer joins. On
 * the bench the first ESTAB is usually the other warthog, seconds before a bridged OpenMANET node,
 * so a peer joining during the first window must still get an attempt right after it, and one
 * joining mid-attempt gets a whole attempt. A lease lost later (NAK, expiry) puts the static
 * address back while lwIP asks again. AT+MESHDHCP=0 takes the static address at the first ESTAB
 * and never asks.
 */
#include <stdio.h>
#include <string.h>

#include "mesh_addr.h"

static int failures;

#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

#define TICK 2000u

static struct mesh_addr S;
static struct mesh_addr_in I;
static int t;

static void reset(bool dhcp)
{
    memset(&S, 0, sizeof(S));
    memset(&I, 0, sizeof(I));
    I.elapsed_ms = TICK;
    I.dhcp = dhcp;
}
static enum mesh_addr_act tick(void) { return mesh_addr_step(&S, &I); }

/* Steps until @p want (at most @p max); returns the step it came on (1-based), or -1. */
static int until(enum mesh_addr_act want, int max)
{
    for (int i = 1; i <= max; i++) {
        if (tick() == want) {
            return i;
        }
    }
    return -1;
}

/* The client as mesh.c reads it after each action: running from START_DHCP/RETRY to STATIC/RETRY_FAIL. */
static enum mesh_addr_act act_tick(void)
{
    const enum mesh_addr_act a = tick();
    if (a == MESH_ADDR_START_DHCP || a == MESH_ADDR_RETRY) I.dhcp_running = true;
    if (a == MESH_ADDR_STATIC || a == MESH_ADDR_RETRY_FAIL) I.dhcp_running = false;
    if (a == MESH_ADDR_STATIC || a == MESH_ADDR_HOLD) I.have_ip = true;
    return a;
}

/* To the static address after a first window with no server; returns the ticks it took. */
static int to_static(void)
{
    I.estab = true;
    I.peers = 1;
    int t = 0;
    while (t < 20 && act_tick() != MESH_ADDR_STATIC) t++;
    return t;
}

/* Ticks until the next RETRY (the client then running), or -1. */
static int next_retry(int max)
{
    for (int i = 1; i <= max; i++) {
        if (act_tick() == MESH_ADDR_RETRY) return i;
    }
    return -1;
}

static void t_first_window(void)
{
    reset(true);
    I.peers = 1; /* a candidate's slot, not ESTAB yet */
    CHECK(until(MESH_ADDR_START_DHCP, 10) == -1, "no ESTAB yet: nothing, however long");
    I.estab = true;
    CHECK(tick() == MESH_ADDR_START_DHCP, "the first ESTAB starts DHCP, with no address");
    I.dhcp_running = true;
    CHECK(tick() == MESH_ADDR_NONE && tick() == MESH_ADDR_NONE && tick() == MESH_ADDR_STATIC,
          "no offer: the static address at 6 s, not sooner");

    reset(true);
    I.estab = true;
    I.peers = 1;
    (void)tick();
    I.dhcp_running = true;
    (void)tick();
    I.leased = true;
    CHECK(tick() == MESH_ADDR_LEASED, "an offer at 4 s: leased");
    CHECK(until(MESH_ADDR_STATIC, 400) == -1, "and kept: no static address, no attempt, over 13 min");

    reset(true);
    I.estab = true;
    I.peers = 1;
    (void)tick();
    I.dhcp_running = true;
    (void)tick();
    (void)tick();
    I.binding = true; /* requesting or ARP-checking an offer at the deadline */
    t = until(MESH_ADDR_STATIC, 10);
    CHECK(t == 6, "an offer being taken at 6 s gets the 10 s grace: static at 16 s (tick %d)", t);
    reset(true);
    I.estab = true;
    I.peers = 1;
    (void)tick();
    I.dhcp_running = true;
    (void)tick();
    (void)tick();
    I.binding = true;
    (void)tick();
    I.binding = false;
    I.leased = true;
    CHECK(tick() == MESH_ADDR_LEASED, "and it binds within the grace: leased");

    reset(true);
    I.estab = true;
    I.peers = 1;
    (void)tick(); /* the client did not start */
    CHECK(tick() == MESH_ADDR_STATIC, "a DHCP client that would not start: static on the next tick, not after 6 s");
}

static void t_retries(void)
{
    reset(true);
    (void)to_static();
    int w[7];
    for (int i = 0; i < 7; i++) {
        w[i] = next_retry(400);
        const int len = until(MESH_ADDR_RETRY_FAIL, 20);
        I.dhcp_running = false;
        if (i == 0) CHECK(len == 6, "an attempt beside the static address lasts 12 s (%d ticks)", len);
    }
    CHECK(w[0] == 15 && w[1] == 30 && w[2] == 60 && w[3] == 120 && w[4] == 240 && w[5] == 300 && w[6] == 300,
          "no server: attempts 30, 60, 120, 240, 480, 600, 600 s apart (%d %d %d %d %d %d %d ticks)",
          w[0], w[1], w[2], w[3], w[4], w[5], w[6]);
    CHECK(S.retries == 7, "seven attempts counted (%lu)", (unsigned long)S.retries);

    /* The grace is the attempt deadline's, not the first window's: an offer
     * under ARP check when a RETRY attempt lapses gets the same 10 s. */
    reset(true);
    (void)to_static();
    (void)next_retry(400);
    for (int i = 0; i < 5; i++) (void)act_tick(); /* 10 s into the 12 s attempt */
    I.binding = true;
    t = until(MESH_ADDR_RETRY_FAIL, 20);
    CHECK(t == 6, "an offer being taken at the RETRY deadline gets the 10 s grace too (tick %d)", t);

    reset(true);
    (void)to_static();
    (void)next_retry(400);
    for (int i = 0; i < 5; i++) (void)act_tick();
    I.binding = true;
    (void)act_tick();
    (void)act_tick(); /* inside the grace, past the deadline */
    I.binding = false;
    I.leased = true;
    CHECK(act_tick() == MESH_ADDR_LEASED, "and one that binds inside that grace is a lease, not a failure");

    /* The level<255 guard: 300 failed attempts must never ramp back down. */
    reset(true);
    (void)to_static();
    int early = 0;
    for (int i = 0; i < 300; i++) {
        const int w2 = next_retry(400);
        if (i >= 8 && w2 >= 0 && w2 < 300) early++;
        (void)until(MESH_ADDR_RETRY_FAIL, 20);
        I.dhcp_running = false;
    }
    CHECK(early == 0, "300 attempts with no server: the wait never falls back below the 600 s cap (%d did)", early);
    CHECK(S.level == 255, "and the level saturates rather than wrapping (%u)", (unsigned)S.level);

    reset(true);
    (void)to_static();
    for (int i = 0; i < 3; i++) { (void)next_retry(400); (void)until(MESH_ADDR_RETRY_FAIL, 20); I.dhcp_running = false; }
    for (int i = 0; i < 10; i++) (void)act_tick();
    I.peers = 2;
    CHECK(act_tick() == MESH_ADDR_RETRY, "a peer joins while static: an attempt at once, not 240 s on");
    (void)until(MESH_ADDR_RETRY_FAIL, 20);
    I.dhcp_running = false;
    t = next_retry(400);
    CHECK(t == 15, "and none answers: the next 30 s later, the backoff restarted (tick %d)", t);
}

static void t_joins(void)
{
    reset(true);
    I.estab = true;
    I.peers = 1; /* the other warthog */
    (void)act_tick();
    (void)act_tick();
    I.peers = 2; /* a bridged OpenMANET node, 4 s later */
    CHECK(act_tick() == MESH_ADDR_NONE, "a peer joining in the first window does not stretch it");
    CHECK(act_tick() == MESH_ADDR_STATIC && act_tick() == MESH_ADDR_RETRY,
          "static at 6 s, then an attempt for the new peer on the next tick, not 30 s on");

    reset(true);
    (void)to_static();
    (void)next_retry(400);
    (void)act_tick();
    (void)act_tick(); /* 4 s into the attempt */
    I.peers = 2;
    t = until(MESH_ADDR_RETRY_FAIL, 20);
    CHECK(t == 6, "a peer joining mid-attempt gets a whole attempt: it ends 12 s after the join (tick %d)", t);

    reset(true);
    (void)to_static();
    I.peers = 0;
    CHECK(until(MESH_ADDR_RETRY, 400) == -1, "no peer: no attempt, however long");
    I.peers = 1;
    CHECK(act_tick() == MESH_ADDR_RETRY, "a peer back after 13 min: an attempt at once");

    /* A rise is a JOIN, not any change: a peer LEAVING must not ask. */
    reset(true);
    (void)to_static();
    for (int i = 0; i < 3; i++) { (void)next_retry(400); (void)until(MESH_ADDR_RETRY_FAIL, 20); I.dhcp_running = false; }
    I.peers = 3;
    CHECK(act_tick() == MESH_ADDR_RETRY, "(setup) a join asks at once");
    (void)until(MESH_ADDR_RETRY_FAIL, 20);
    I.dhcp_running = false;
    const uint8_t lvl = S.level;
    const uint32_t rt = S.retries;
    I.peers = 2; /* one of the three left */
    CHECK(act_tick() == MESH_ADDR_NONE, "a peer LEAVING does not ask: a rise is a join, not any change");
    CHECK(S.level == lvl && S.retries == rt, "and neither the backoff nor the count moves");

    reset(true);
    (void)to_static();
    I.dhcp = false; /* weeks with AT+MESHDHCP=0, two huge steps: a wrapped wait would read 2 s */
    I.elapsed_ms = 0x80000000u;
    (void)tick();
    (void)tick();
    I.elapsed_ms = TICK;
    I.dhcp = true;
    CHECK(act_tick() == MESH_ADDR_RETRY, "the wait saturates rather than wrapping");
}

/* A peer whose link flaps raises the count over and over. Each rise restarts
 * the attempt beside the static address -- which must still END, or lwIP is
 * left broadcasting DISCOVER for as long as the flapping lasts, the backoff
 * never climbs, and AT+MESHDHCP=0 never takes hold. */
static void t_peer_churn(void)
{
    reset(true);
    (void)to_static();
    int ended = 0;
    for (int i = 0; i < 200; i++) {
        I.peers = (uint8_t)(1 + (i & 1)); /* 1,2,1,2: a rise every other tick */
        if (act_tick() == MESH_ADDR_RETRY_FAIL) ended++;
    }
    CHECK(ended >= 5, "a peer flapping every other tick: attempts still end (%d in 400 s)", ended);
    CHECK(S.retries >= 5, "each one counts (%lu)", (unsigned long)S.retries);
    /* Mid-flap the client is legitimately up for the attempt in flight; what
     * matters is that it is stopped once the flapping stops. */
    I.peers = 2;
    int stopped = 0;
    for (int i = 0; i < 15 && !stopped; i++) stopped = act_tick() == MESH_ADDR_RETRY_FAIL;
    CHECK(stopped && !I.dhcp_running, "and when the flapping stops the client is stopped, not left running");

    /* One join still buys a whole attempt -- the stretch is once, not none. */
    reset(true);
    (void)to_static();
    (void)next_retry(400);
    (void)act_tick();
    (void)act_tick(); /* 4 s in */
    I.peers = 2;
    t = until(MESH_ADDR_RETRY_FAIL, 20);
    CHECK(t == 6, "a single join still gets a whole attempt from the join (tick %d)", t);
    I.dhcp_running = false;

    /* AT+MESHDHCP=0 during a flap: the attempt in flight ends, then nothing. */
    reset(true);
    (void)to_static();
    (void)next_retry(400);
    I.dhcp = false;
    int asked = 0;
    for (int i = 0; i < 300; i++) {
        I.peers = (uint8_t)(1 + (i & 1));
        const enum mesh_addr_act a = act_tick();
        if (a == MESH_ADDR_RETRY || a == MESH_ADDR_START_DHCP) asked++;
    }
    CHECK(asked == 0, "set to 0 while a peer flaps: no further attempt (%d)", asked);
    CHECK(!I.dhcp_running, "and the client was stopped");
}

static void t_lease_lost(void)
{
    reset(true);
    (void)to_static();
    (void)next_retry(400);
    I.have_ip = false; /* a NAK took the static address mid-attempt */
    CHECK(act_tick() == MESH_ADDR_HOLD && act_tick() == MESH_ADDR_NONE, "an address gone mid-attempt is re-applied, once");
    I.leased = true;
    CHECK(act_tick() == MESH_ADDR_LEASED, "an attempt beside the static address that binds: leased");
    I.leased = false;
    I.have_ip = false; /* expiry: lwIP released it and asks again */
    CHECK(act_tick() == MESH_ADDR_HOLD, "a lease lost: the static address back at once");
    I.binding = false;
    t = until(MESH_ADDR_RETRY_FAIL, 20);
    CHECK(t == 6, "lwIP's own asking counts as an attempt: it ends 12 s on (tick %d)", t);
    I.dhcp_running = false;
    t = next_retry(400);
    CHECK(t == 15, "then 30 s to the next (tick %d)", t);

    reset(true);
    (void)to_static();
    (void)next_retry(400);
    I.have_ip = false;
    I.binding = true; /* the address went while an offer is being ARP-checked */
    CHECK(act_tick() == MESH_ADDR_HOLD, "HOLD is asked for; mesh.c's tcpip op refuses it during the check");
}

static void t_dhcp_off(void)
{
    reset(false);
    I.estab = true;
    I.peers = 1;
    CHECK(act_tick() == MESH_ADDR_STATIC, "AT+MESHDHCP=0: the static address at the first ESTAB");
    int asked = 0;
    for (int i = 0; i < 600; i++) { I.peers = (uint8_t)(1 + (i / 7) % 3); asked += act_tick() != MESH_ADDR_NONE; }
    CHECK(asked == 0, "and never asks, peers coming and going (%d actions)", asked);
    I.dhcp = true;
    I.peers = 3;
    CHECK(act_tick() == MESH_ADDR_RETRY, "AT+MESHDHCP=1 at run time: asked at once, the wait long passed");
    I.dhcp = false;
    t = until(MESH_ADDR_RETRY_FAIL, 20);
    I.dhcp_running = false;
    CHECK(t == 6 && until(MESH_ADDR_RETRY, 600) == -1, "set to 0 mid-attempt: that attempt ends, no more (tick %d)", t);
}

int main(void)
{
    t_first_window();
    t_retries();
    t_joins();
    t_peer_churn();
    t_lease_lost();
    t_dhcp_off();
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
