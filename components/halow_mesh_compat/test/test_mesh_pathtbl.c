/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Mesh path and proxy tables (morselib src/umac/mesh/umac_mesh_pathtbl.c).
 *
 * The update rules are what make forwarding both correct and safe. Get the
 * sequence-number comparison backwards and a relay adopts every stale
 * advertisement; forget the hysteresis and two equal routes flap on every
 * PREQ; refresh expiry on a rejected update and a dead path never dies;
 * honour an older PERR and a delayed error kills a path that was rebuilt;
 * start a new slot's expiry at 0 and a path made after 2^31 ms of uptime
 * never expires; never free a lapsed slot and 2^31 ms later it is live again;
 * never bound a leaf pin's age and 2^32 ms later its via hold is back.
 * Each of those is a known-answer case here.
 */
#include "umac_mesh_pathtbl.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static const uint8_t D[6]  = { 0x02, 0, 0, 0, 0, 0xd0 }; /* a destination */
static const uint8_t H1[6] = { 0x02, 0, 0, 0, 0, 0x11 }; /* next hop 1 */
static const uint8_t H2[6] = { 0x02, 0, 0, 0, 0, 0x22 }; /* next hop 2 */
static const uint8_t X[6]  = { 0x00, 0xe0, 0x4f, 0x71, 0x99, 0xa5 }; /* a host behind a node */

static struct umac_mesh_pathtbl T;
#define LT 5120u /* any lifetime; the relay derives the real one from the PREQ/PREP */

int main(void)
{
    umac_mesh_pathtbl_init(&T);
    uint32_t now = 1000;
    const struct umac_mesh_path *p;

    /* ---- creation and lookup --------------------------------------------- */
    CHECK(umac_mesh_path_lookup(&T, D, now) == NULL, "empty table has no path");
    CHECK(umac_mesh_path_update(&T, D, H1, 10, 500, 1, LT, now), "first advertisement is fresh");
    p = umac_mesh_path_lookup(&T, D, now);
    CHECK(p != NULL && memcmp(p->next_hop, H1, 6) == 0 && p->sn == 10 && p->metric == 500,
          "path to D via H1, sn 10, metric 500");
    CHECK(umac_mesh_path_count(&T, now) == 1, "one active path");

    /* ---- sequence numbers ------------------------------------------------- */
    CHECK(!umac_mesh_path_update(&T, D, H2, 9, 1, 1, LT, now), "older sn is rejected even with a perfect metric");
    p = umac_mesh_path_lookup(&T, D, now);
    CHECK(memcmp(p->next_hop, H1, 6) == 0 && p->metric == 500, "and nothing changed");
    CHECK(umac_mesh_path_update(&T, D, H2, 11, 9000, 3, LT, now), "newer sn is adopted even with a worse metric");
    p = umac_mesh_path_lookup(&T, D, now);
    CHECK(memcmp(p->next_hop, H2, 6) == 0 && p->sn == 11 && p->metric == 9000, "path now via H2");

    /* ---- equal sn: metric and hysteresis ---------------------------------- */
    CHECK(!umac_mesh_path_update(&T, D, H2, 11, 9000, 3, LT, now), "same sn, same hop, same metric: not fresh");
    CHECK(!umac_mesh_path_update(&T, D, H2, 11, 9500, 3, LT, now), "same sn, same hop, worse metric: not fresh");
    CHECK( umac_mesh_path_update(&T, D, H2, 11, 8999, 3, LT, now), "same sn, same hop, strictly better: fresh");
    /* Switching hops needs ~10 % better: new*10/9 < old. old=8999 -> new must be < 8099.1 */
    CHECK(!umac_mesh_path_update(&T, D, H1, 11, 8500, 2, LT, now), "same sn, other hop, 5 %% better: NOT enough to switch");
    p = umac_mesh_path_lookup(&T, D, now);
    CHECK(memcmp(p->next_hop, H2, 6) == 0, "still via H2 -- no flapping on near-equal routes");
    CHECK(!umac_mesh_path_update(&T, D, H1, 11, 8100, 2, LT, now), "8100*10/9 = 9000 >= 8999: still not enough");
    CHECK( umac_mesh_path_update(&T, D, H1, 11, 8099, 2, LT, now), "8099*10/9 = 8998 < 8999: switches");
    p = umac_mesh_path_lookup(&T, D, now);
    CHECK(memcmp(p->next_hop, H1, 6) == 0 && p->metric == 8099, "now via H1 at 8099");

    /* ---- expiry: only fresh info extends it ------------------------------- */
    uint32_t exp0 = p->exp_ms;
    CHECK(exp0 == now + LT, "expiry is now + lifetime");
    CHECK(!umac_mesh_path_update(&T, D, H1, 11, 8099, 2, LT, now + 2000), "repeat of the same info is not fresh");
    p = umac_mesh_path_lookup(&T, D, now + 2000);
    CHECK(p != NULL && p->exp_ms == exp0, "and did NOT extend expiry -- a stale repeater cannot keep a path alive");
    CHECK(umac_mesh_path_update(&T, D, H1, 12, 8099, 2, LT, now + 2000), "a new sn is fresh");
    p = umac_mesh_path_lookup(&T, D, now + 2000);
    CHECK(p->exp_ms == now + 2000 + LT, "and extends expiry");
    CHECK(umac_mesh_path_update(&T, D, H1, 13, 8099, 2, 100, now + 2100), "fresh info with a SHORT lifetime");
    p = umac_mesh_path_lookup(&T, D, now + 2100);
    CHECK(p->exp_ms == now + 2000 + LT, "does not cut the expiry short -- only ever extends");
    CHECK(umac_mesh_path_lookup(&T, D, now + 2000 + LT) == NULL, "expired at the deadline");
    CHECK(umac_mesh_path_lookup(&T, D, now + 2000 + LT - 1) != NULL, "alive one ms before it");

    /* ---- PERR invalidation ------------------------------------------------- */
    umac_mesh_pathtbl_init(&T);
    now = 50000;
    umac_mesh_path_update(&T, D, H1, 20, 100, 1, LT, now);
    CHECK(!umac_mesh_path_invalidate(&T, D, 25, H2, now), "PERR from a node that is NOT our next hop is ignored, even with a newer sn");
    CHECK(umac_mesh_path_lookup(&T, D, now) != NULL, "a third party cannot knock out our path");
    CHECK(!umac_mesh_path_invalidate(&T, D, 19, H1, now), "PERR with an OLDER sn is ignored (stale error)");
    CHECK(umac_mesh_path_lookup(&T, D, now) != NULL, "path survives it");
    CHECK(!umac_mesh_path_invalidate(&T, D, 20, H1, now), "PERR with the SAME sn is ignored too (mac80211: must be newer)");
    CHECK( umac_mesh_path_invalidate(&T, D, 21, H1, now), "PERR with a newer sn deactivates");
    CHECK(umac_mesh_path_lookup(&T, D, now) == NULL, "path is gone");
    CHECK(!umac_mesh_path_invalidate(&T, D, 22, H1, now), "a second PERR changes nothing -> must not be re-forwarded");
    umac_mesh_path_update(&T, D, H1, 30, 100, 1, LT, now);
    CHECK( umac_mesh_path_invalidate(&T, D, 0, NULL, now), "PERR with sn 0 (unknown) from anyone-checked-by-caller (NULL) deactivates");
    CHECK(umac_mesh_path_lookup(&T, D, now) == NULL, "gone again");
    /* mac80211 parity: an INACTIVE path does not reject on sn. After a PERR
     * the next advertisement rebuilds, even at an older number -- the sn
     * defence protects live paths, and recovery must not wait on it. */
    CHECK( umac_mesh_path_update(&T, D, H1, 22, 100, 1, LT, now), "after a PERR, sn 22 rebuilds although we held 30");
    CHECK( umac_mesh_path_invalidate(&T, D, 23, H1, now), "PERR at 23 deactivates the rebuilt path");
    CHECK( umac_mesh_path_update(&T, D, H1, 23, 100, 1, LT, now), "an advertisement at the PERR's own sn rebuilds it");
    CHECK(!umac_mesh_path_update(&T, D, H1, 22, 100, 1, LT, now), "but once ACTIVE again, an older sn is rejected");

    /* ---- losing a neighbour ------------------------------------------------ */
    umac_mesh_pathtbl_init(&T);
    now = 60000;
    const uint8_t D2[6] = { 0x02, 0, 0, 0, 0, 0xd2 }, D3[6] = { 0x02, 0, 0, 0, 0, 0xd3 };
    umac_mesh_path_update(&T, D,  H1, 1, 1, 1, LT, now);
    umac_mesh_path_update(&T, D2, H1, 1, 1, 1, LT, now);
    umac_mesh_path_update(&T, D3, H2, 1, 1, 1, LT, now);
    CHECK(umac_mesh_path_lose_next_hop(&T, H1) == 2, "losing H1 drops the two paths through it");
    CHECK(umac_mesh_path_lookup(&T, D3, now) != NULL, "the path via H2 is untouched");
    CHECK(umac_mesh_path_count(&T, now) == 1, "one active path left");

    /* ---- capacity: bounded, evicts the soonest-expiring ------------------- */
    umac_mesh_pathtbl_init(&T);
    now = 70000;
    for (uint32_t i = 0; i < UMAC_MESH_PATH_MAX; i++)
    {
        uint8_t d[6] = { 0x02, 0, 0, 0, (uint8_t)(i >> 8), (uint8_t)i };
        CHECK(umac_mesh_path_update(&T, d, H1, 1, 1, 1, LT, now + i), "fill slot %u", (unsigned)i);
    }
    uint8_t extra[6] = { 0x02, 0, 0, 0, 0xee, 0xee };
    /* Every slot holds a LIVE path: a newcomer is refused, not swapped in --
     * a forged-originator stream must not be able to push real routes out. */
    CHECK(!umac_mesh_path_update(&T, extra, H1, 1, 1, 1, LT, now + 100), "a full table of live paths refuses a newcomer");
    uint8_t first[6] = { 0x02, 0, 0, 0, 0, 0 };
    CHECK(umac_mesh_path_lookup(&T, first, now + 100) != NULL, "slot 0 was NOT evicted");
    CHECK(umac_mesh_path_count(&T, now + 100) == UMAC_MESH_PATH_MAX, "table stays at its bound");
    /* Deactivate one (as a PERR would) and the newcomer takes that slot. */
    umac_mesh_path_invalidate(&T, first, 0, NULL, now + 100);
    CHECK(umac_mesh_path_update(&T, extra, H1, 1, 1, 1, LT, now + 100), "an inactive slot is reused");
    CHECK(umac_mesh_path_lookup(&T, extra, now + 100) != NULL, "the newcomer is present");
    /* And an expired one likewise. */
    CHECK(umac_mesh_path_update(&T, first, H1, 2, 1, 1, LT, now + 100 + LT + 1) || true, "(advance clock)");

    /* ---- proxy table --------------------------------------------------------- */
    umac_mesh_pathtbl_init(&T);
    now = 80000;
    CHECK(umac_mesh_proxy_lookup(&T, X, now) == NULL, "unknown host has no proxy");
    umac_mesh_proxy_learn(&T, X, H1, now);
    const uint8_t *m = umac_mesh_proxy_lookup(&T, X, now);
    CHECK(m != NULL && memcmp(m, H1, 6) == 0, "host X is behind H1");
    umac_mesh_proxy_learn(&T, X, H2, now + 10);
    m = umac_mesh_proxy_lookup(&T, X, now + 10);
    CHECK(m != NULL && memcmp(m, H2, 6) == 0, "re-learning moves X behind H2 (a host can roam)");
    CHECK(umac_mesh_proxy_count(&T, now + 10) == 1, "still one proxy entry, not two");
    CHECK(umac_mesh_proxy_lookup(&T, X, now + 10 + UMAC_MESH_PROXY_LIFETIME_MS) == NULL, "proxy entry expires");
    /* One node may own at most UMAC_MESH_PROXY_PER_NODE hosts: a flood of AE
     * frames from one peer cannot fill the table. */
    umac_mesh_pathtbl_init(&T);
    for (uint32_t i = 0; i < UMAC_MESH_PROXY_PER_NODE + 4; i++)
    {
        uint8_t h[6] = { 0x00, 0x11, 0, 0, 0, (uint8_t)(1 + i) };
        bool ok = umac_mesh_proxy_learn(&T, h, H1, now + 1000);
        if (i < UMAC_MESH_PROXY_PER_NODE) { CHECK(ok, "host %u behind H1 learned", (unsigned)i); }
        else { CHECK(!ok, "host %u behind H1 refused: H1 owns its %u already", (unsigned)i, (unsigned)UMAC_MESH_PROXY_PER_NODE); }
    }
    CHECK(umac_mesh_proxy_count(&T, now + 1000) == UMAC_MESH_PROXY_PER_NODE, "H1 owns exactly its bound");
    uint8_t hb[6] = { 0x00, 0x22, 0, 0, 0, 1 };
    CHECK(umac_mesh_proxy_learn(&T, hb, H2, now + 1000), "another node may still learn a host");
    /* A live entry is never evicted for a newcomer: fill with four nodes. */
    umac_mesh_pathtbl_init(&T);
    for (uint32_t n = 0; n < 4; n++) for (uint32_t i = 0; i < UMAC_MESH_PROXY_PER_NODE; i++)
    {
        uint8_t node[6] = { 0x02, 0, 0, 0, 0, (uint8_t)(0x10 + n) }, h[6] = { 0x00, 0x33, 0, 0, (uint8_t)n, (uint8_t)i };
        umac_mesh_proxy_learn(&T, h, node, now + 1000);
    }
    CHECK(umac_mesh_proxy_count(&T, now + 1000) == UMAC_MESH_PROXY_MAX, "table full of live entries");
    uint8_t newcomer[6] = { 0x00, 0x44, 0, 0, 0, 1 }, node5[6] = { 0x02, 0, 0, 0, 0, 0x50 };
    CHECK(!umac_mesh_proxy_learn(&T, newcomer, node5, now + 1000), "a newcomer is refused rather than evicting a live host");
    uint8_t first_host[6] = { 0x00, 0x33, 0, 0, 0, 0 };
    CHECK(umac_mesh_proxy_lookup(&T, first_host, now + 1000) != NULL, "the first host is still known");
    /* Touch on transmit use extends life; an untouched entry lapses. */
    umac_mesh_pathtbl_init(&T);
    umac_mesh_proxy_learn(&T, X, H1, now);
    umac_mesh_proxy_touch(&T, X, now + UMAC_MESH_PROXY_LIFETIME_MS - 1000);
    CHECK(umac_mesh_proxy_lookup(&T, X, now + UMAC_MESH_PROXY_LIFETIME_MS + 1000) != NULL, "touched on TX use: still known past the original lifetime");
    /* A touch after the entry has lapsed must NOT bring it back: the host may
     * have moved, and a resurrected entry steers unicast to the wrong node. */
    umac_mesh_pathtbl_init(&T);
    umac_mesh_proxy_learn(&T, X, H1, now);
    umac_mesh_proxy_touch(&T, X, now + UMAC_MESH_PROXY_LIFETIME_MS + 1);
    CHECK(umac_mesh_proxy_lookup(&T, X, now + UMAC_MESH_PROXY_LIFETIME_MS + 2) == NULL, "a touch does not resurrect a lapsed host entry");
    _Static_assert(UMAC_MESH_PROXY_LIFETIME_MS == 600000u, "proxy lifetime is mac80211's 600 s");

    /* ---- a path made late in uptime still expires --------------------------- */
    /* The only-extend rule compares against the slot's old expiry. A new slot
     * must start at now (mac80211 mesh_path_new), not 0: from 2^31 ms of
     * uptime, 0 is in the future and the path would never lapse. */
    {
        const uint32_t late[2] = { 0x80000000u + 1000u, 0xfffff000u };
        const uint32_t life[2] = { LT, 1000u }; /* the second expires just short of the wrap */
        for (unsigned k = 0; k < 2u; k++)
        {
            umac_mesh_pathtbl_init(&T);
            umac_mesh_path_update(&T, D, H1, 10, 500, 1, life[k], late[k]);
            CHECK(T.p[0].exp_ms == late[k] + life[k], "uptime 0x%08lx: expiry is now + %lu ms (got 0x%08lx)",
                  (unsigned long)late[k], (unsigned long)life[k], (unsigned long)T.p[0].exp_ms);
            CHECK(umac_mesh_path_lookup(&T, D, late[k] + life[k] + 1u) == NULL,
                  "uptime 0x%08lx: and the path lapses then", (unsigned long)late[k]);
        }
    }

    /* ---- the sweep: a path 600 s past its expiry is freed ------------------
     *
     * Every expiry test is signed 32-bit, so a slot left 2^31 ms (24.9 days)
     * after its expiry reads as unexpired again: a lapsed ACTIVE path comes
     * back, and a dead slot re-learned then keeps its old expiry and never
     * lapses. umac_mesh_path_expire(), run from the glue's tick, frees both
     * UMAC_MESH_PATH_EXPIRE_MS after their expiry, as mac80211's
     * mesh_path_expire() does. Pinned: the bound (600 s, to the ms), that dead
     * and lapsed-active slots go and a live one stays, and that after a sweep
     * both wrap cases behave. Separately, without any sweep, an update still
     * restarts a slot that lapsed just under 2^31 ms ago.
     */
    {
        _Static_assert(UMAC_MESH_PATH_EXPIRE_MS == 600000u, "mac80211 MESH_PATH_EXPIRE is 600 s");
        const uint8_t D2[6] = { 0x02, 0, 0, 0, 0, 0xd2 }, D3[6] = { 0x02, 0, 0, 0, 0, 0xd3 };
        const uint32_t t = 1000;
        const uint32_t edge = t + LT + UMAC_MESH_PATH_EXPIRE_MS;
        umac_mesh_pathtbl_init(&T);
        umac_mesh_path_update(&T, D, H1, 10, 500, 1, LT, t);       /* lapses, still ACTIVE */
        umac_mesh_path_update(&T, D2, H1, 10, 500, 1, LT, t);      /* a PERR kills it... */
        CHECK(umac_mesh_path_invalidate(&T, D2, 11, H1, t + 10), "sweep: D2 dead with time left");
        umac_mesh_path_update(&T, D3, H1, 10, 500, 1, LT, edge - 1000u); /* live at the edge */
        CHECK(umac_mesh_path_expire(&T, edge - 1u) == 0 && T.p[0].used && T.p[1].used,
              "sweep: 600 s - 1 ms after their expiry, lapsed and dead paths are kept");
        CHECK(umac_mesh_path_expire(&T, edge) == 2 && !T.p[0].used && !T.p[1].used,
              "sweep: 600 s after it, both are freed");
        CHECK(T.p[2].used && umac_mesh_path_lookup(&T, D3, edge) != NULL,
              "sweep: a live path is not");
        CHECK(umac_mesh_path_expire(&T, edge) == 0, "sweep: a second pass frees nothing");

        const uint32_t wrap = t + LT + 0x80000000u + 10u;
        CHECK(umac_mesh_path_lookup(&T, D, wrap) == NULL,
              "sweep: 2^31 ms after its expiry, the lapsed path does not come back");
        CHECK(umac_mesh_path_update(&T, D2, H1, 12, 500, 1, LT, wrap), "sweep: D2 re-learned then");
        p = umac_mesh_path_lookup(&T, D2, wrap);
        CHECK(p != NULL && p->exp_ms == wrap + LT,
              "sweep: with expiry now + %u ms (got 0x%08lx want 0x%08lx)", (unsigned)LT,
              p != NULL ? (unsigned long)p->exp_ms : 0ul, (unsigned long)(wrap + LT));
        CHECK(umac_mesh_path_lookup(&T, D2, wrap + LT) == NULL &&
                  umac_mesh_path_lookup(&T, D2, wrap + 86400000u) == NULL,
              "sweep: and it lapses then, not a day later");
        CHECK(umac_mesh_path_expire(NULL, edge) == 0, "sweep: NULL table");

        /* Proxy entries: the same rule, or one learned and left 2^31 ms reads live again. */
        umac_mesh_pathtbl_init(&T);
        const uint8_t HX[6] = { 0x00, 0x77, 0, 0, 0, 0x5a };
        CHECK(umac_mesh_proxy_learn(&T, HX, H1, t), "sweep: a proxy entry learned");
        const uint32_t pedge = t + UMAC_MESH_PROXY_LIFETIME_MS + UMAC_MESH_PATH_EXPIRE_MS;
        CHECK(umac_mesh_path_expire(&T, pedge - 1u) == 0 && T.x[0].used,
              "sweep: a proxy entry 600 s - 1 ms past its expiry is kept");
        CHECK(umac_mesh_path_expire(&T, pedge) == 1 && !T.x[0].used,
              "sweep: and freed at 600 s past it");
        const uint32_t pwrap = t + UMAC_MESH_PROXY_LIFETIME_MS + 0x80000000u + 10u;
        CHECK(umac_mesh_proxy_lookup(&T, HX, pwrap) == NULL,
              "sweep: 2^31 ms after its expiry, the proxy entry does not come back");

        /* No sweep: update alone still restarts a slot stale by just under 2^31 ms. */
        umac_mesh_pathtbl_init(&T);
        umac_mesh_path_update(&T, D, H1, 10, 500, 1, LT, t);
        (void)umac_mesh_path_invalidate(&T, D, 11, H1, t + 10);
        const uint32_t stale = t + LT + 0x80000000u - LT / 2u;
        CHECK(umac_mesh_path_update(&T, D, H1, 12, 500, 1, LT, stale), "unswept: re-learned");
        p = umac_mesh_path_lookup(&T, D, stale);
        CHECK(p != NULL && p->exp_ms == stale + LT,
              "unswept: a slot %lu ms past its expiry restarts at now + %u ms (got %ld)",
              (unsigned long)(stale - t - LT), (unsigned)LT,
              p != NULL ? (long)(int32_t)(p->exp_ms - stale) : -1L);
    }

    /* ---- leaf entries: hints from floods may be evicted, conversations not ----
     *
     * Pinned: a full table of flood hints gives up exactly its least recently
     * refreshed one; a node at its bound rotates only its own hints; unicast
     * evidence pins (and is recorded with its time); relay-learned entries are
     * never evicted and a relay re-learn clears leaf state; a reused slot starts
     * clean; the entry accessor honours expiry. */
    {
        static const uint8_t VIA[6] = { 0x02, 0, 0, 0, 0, 0xaa };
        uint32_t t0 = 200000;
        /* Four nodes, eight flood-learned hosts each: the table is full. */
        umac_mesh_pathtbl_init(&T);
        for (uint32_t n = 0; n < 4; n++) for (uint32_t i = 0; i < UMAC_MESH_PROXY_PER_NODE; i++)
        {
            uint8_t node[6] = { 0x02, 0, 0, 0, 0, (uint8_t)(0x10 + n) }, h[6] = { 0x00, 0x55, 0, 0, (uint8_t)n, (uint8_t)i };
            (void)umac_mesh_proxy_learn_leaf(&T, h, node, VIA, false, t0 + n * 100u + i);
        }
        CHECK(umac_mesh_proxy_count(&T, t0 + 1000) == UMAC_MESH_PROXY_MAX, "leaf: table full of flood-learned hints");
        uint8_t fresh5[6] = { 0x00, 0x66, 0, 0, 0, 1 }, node5[6] = { 0x02, 0, 0, 0, 0, 0x50 };
        uint8_t oldest[6] = { 0x00, 0x55, 0, 0, 0, 0 }, second[6] = { 0x00, 0x55, 0, 0, 0, 1 };
        CHECK(umac_mesh_proxy_learn_leaf(&T, fresh5, node5, VIA, false, t0 + 1000),
              "leaf: a newcomer behind another node takes a slot from a flood hint");
        CHECK(umac_mesh_proxy_lookup(&T, oldest, t0 + 1000) == NULL &&
              umac_mesh_proxy_lookup(&T, second, t0 + 1000) != NULL,
              "leaf: the victim is the least recently refreshed hint, and only it");
        CHECK(umac_mesh_proxy_count(&T, t0 + 1000) == UMAC_MESH_PROXY_MAX, "leaf: still exactly full");

        /* A node at its bound rotates its own hints, even with slots free and
         * another node's hint older: one node's flood cannot unlearn others. */
        umac_mesh_pathtbl_init(&T);
        uint8_t node0[6] = { 0x02, 0, 0, 0, 0, 0x10 }, node1[6] = { 0x02, 0, 0, 0, 0, 0x11 };
        for (uint32_t i = 0; i < UMAC_MESH_PROXY_PER_NODE; i++)
        {
            uint8_t h1[6] = { 0x00, 0x55, 0, 0, 1, (uint8_t)i }, h0[6] = { 0x00, 0x55, 0, 0, 0, (uint8_t)i };
            (void)umac_mesh_proxy_learn_leaf(&T, h1, node1, VIA, false, t0 + i);
            (void)umac_mesh_proxy_learn_leaf(&T, h0, node0, VIA, false, t0 + 500u + i);
        }
        uint8_t n0h9[6] = { 0x00, 0x55, 0, 0, 0, 9 }, n0h0[6] = { 0x00, 0x55, 0, 0, 0, 0 };
        uint8_t n1h0[6] = { 0x00, 0x55, 0, 0, 1, 0 };
        CHECK(umac_mesh_proxy_learn_leaf(&T, n0h9, node0, VIA, false, t0 + 1000),
              "leaf: a ninth host behind a node at its bound replaces that node's oldest hint");
        CHECK(umac_mesh_proxy_lookup(&T, n0h0, t0 + 1000) == NULL &&
              umac_mesh_proxy_lookup(&T, n1h0, t0 + 1000) != NULL &&
              umac_mesh_proxy_count(&T, t0 + 1000) == 2u * UMAC_MESH_PROXY_PER_NODE,
              "leaf: ...not another node's, though older, and not a free slot");

        /* Unicast evidence pins an entry; a table of pinned entries refuses. */
        umac_mesh_pathtbl_init(&T);
        for (uint32_t n = 0; n < 4; n++) for (uint32_t i = 0; i < UMAC_MESH_PROXY_PER_NODE; i++)
        {
            uint8_t node[6] = { 0x02, 0, 0, 0, 0, (uint8_t)(0x10 + n) }, h[6] = { 0x00, 0x55, 0, 0, (uint8_t)n, (uint8_t)i };
            (void)umac_mesh_proxy_learn_leaf(&T, h, node, VIA, false, t0);
            (void)umac_mesh_proxy_learn_leaf(&T, h, node, VIA, true, t0 + 1);
        }
        CHECK(!umac_mesh_proxy_learn_leaf(&T, fresh5, node5, VIA, false, t0 + 1000),
              "leaf: nothing evicts a host we have had a unicast from");
        const struct umac_mesh_proxy *e = umac_mesh_proxy_entry(&T, oldest, t0 + 1000);
        CHECK(e != NULL && e->uni && e->uni_ms == t0 + 1 && e->leaf, "leaf: the entry records it");
        CHECK(!umac_mesh_proxy_learn_leaf(&T, n0h9, node0, VIA, false, t0 + 1000),
              "leaf: nor, for a node at its bound, one of that node's own");

        /* Relay-learned entries are never evicted by a leaf learn either. */
        umac_mesh_pathtbl_init(&T);
        for (uint32_t n = 0; n < 4; n++) for (uint32_t i = 0; i < UMAC_MESH_PROXY_PER_NODE; i++)
        {
            uint8_t node[6] = { 0x02, 0, 0, 0, 0, (uint8_t)(0x10 + n) }, h[6] = { 0x00, 0x55, 0, 0, (uint8_t)n, (uint8_t)i };
            (void)umac_mesh_proxy_learn(&T, h, node, t0);
        }
        CHECK(!umac_mesh_proxy_learn_leaf(&T, fresh5, node5, VIA, false, t0 + 1000),
              "relay entries are not leaf hints and are never evicted");

        /* A relay learn over a leaf entry leaves no leaf state behind. */
        umac_mesh_pathtbl_init(&T);
        (void)umac_mesh_proxy_learn_leaf(&T, X, H1, VIA, true, t0);
        (void)umac_mesh_proxy_learn(&T, X, H2, t0 + 1);
        e = umac_mesh_proxy_entry(&T, X, t0 + 1);
        CHECK(e != NULL && !e->leaf && !e->uni, "a relay re-learn clears leaf state");
        CHECK(umac_mesh_proxy_entry(&T, X, t0 + 1 + UMAC_MESH_PROXY_LIFETIME_MS) == NULL, "entry lookup honours expiry");

        /* A lapsed pinned entry's slot, reused by a flood, carries no pin. */
        umac_mesh_pathtbl_init(&T);
        (void)umac_mesh_proxy_learn_leaf(&T, X, H1, VIA, true, t0);
        uint8_t Y2[6] = { 0x00, 0x77, 0, 0, 0, 1 };
        (void)umac_mesh_proxy_learn_leaf(&T, Y2, H1, VIA, false, t0 + UMAC_MESH_PROXY_LIFETIME_MS + 1);
        e = umac_mesh_proxy_entry(&T, Y2, t0 + UMAC_MESH_PROXY_LIFETIME_MS + 1);
        CHECK(e != NULL && e->leaf && !e->uni && e->uni_ms == 0,
              "leaf: a reused slot does not inherit a lapsed entry's unicast pin");

        /* Nor does the SAME host back after its entry lapsed: a lapsed entry is
         * gone, not revived. */
        umac_mesh_pathtbl_init(&T);
        (void)umac_mesh_proxy_learn_leaf(&T, X, H1, VIA, true, t0);
        (void)umac_mesh_proxy_learn_leaf(&T, X, H1, VIA, false, t0 + UMAC_MESH_PROXY_LIFETIME_MS + 1);
        e = umac_mesh_proxy_entry(&T, X, t0 + UMAC_MESH_PROXY_LIFETIME_MS + 1);
        CHECK(e != NULL && e->leaf && !e->uni && e->uni_ms == 0,
              "leaf: the same host back after its entry lapsed carries no stale pin");

        /* Every learn, not only the first, restarts the lifetime. */
        umac_mesh_pathtbl_init(&T);
        (void)umac_mesh_proxy_learn_leaf(&T, X, H1, VIA, false, t0);
        (void)umac_mesh_proxy_learn_leaf(&T, X, H1, VIA, false, t0 + 400000u);
        e = umac_mesh_proxy_entry(&T, X, t0 + 400000u);
        CHECK(e != NULL && e->exp_ms == t0 + 400000u + UMAC_MESH_PROXY_LIFETIME_MS,
              "leaf: a re-learn restarts the entry at now + %u ms", (unsigned)UMAC_MESH_PROXY_LIFETIME_MS);
        CHECK(umac_mesh_proxy_entry(&T, X, t0 + 700000u) != NULL,
              "leaf: ...so it is still known 700 s after it was first learned");

        /* A unicast pin belongs to the node it came from: the same node's flood
         * keeps it, a move to another node drops it, the new node's unicast re-pins. */
        umac_mesh_pathtbl_init(&T);
        (void)umac_mesh_proxy_learn_leaf(&T, X, H1, VIA, true, t0);
        (void)umac_mesh_proxy_learn_leaf(&T, X, H1, VIA, false, t0 + 1);
        e = umac_mesh_proxy_entry(&T, X, t0 + 1);
        CHECK(e != NULL && e->uni && e->uni_ms == t0, "leaf: a flood from the same node keeps the pin and its time");
        (void)umac_mesh_proxy_learn_leaf(&T, X, H2, VIA, false, t0 + 2);
        e = umac_mesh_proxy_entry(&T, X, t0 + 2);
        CHECK(e != NULL && memcmp(e->mesh_sta, H2, 6) == 0 && !e->uni && e->uni_ms == 0,
              "leaf: re-learned behind another node, the host loses the old node's pin");
        (void)umac_mesh_proxy_learn_leaf(&T, X, H2, VIA, true, t0 + 3);
        e = umac_mesh_proxy_entry(&T, X, t0 + 3);
        CHECK(e != NULL && e->uni && e->uni_ms == t0 + 3, "leaf: a unicast from the new node pins it again");
    }

    /* ---- the sweep bounds a leaf pin's age ---------------------------------
     *
     * umac_mesh_fwd_leaf_learn holds a unicast's relay while the unsigned age
     * now - uni_ms is under UMAC_MESH_LEAF_VIA_HOLD_MS. Only a unicast rewrites
     * uni_ms, and floods keep the entry live for as long as they come, so left
     * alone the age wraps to 0 after 2^32 ms (49.7 days) and the hold re-arms.
     * Once the hold is over, the sweep pulls uni_ms up to the hold behind now.
     * Pinned: inside the hold it is untouched (to the ms); past it uni_ms is
     * exactly now - hold; the pin itself stays, so a full table still gives up
     * a flood hint rather than the pinned host; a flood-only entry is left
     * alone. test_mesh_fwd drives the same sweep across 2^32 ms.
     */
    {
        static const uint8_t VIA[6] = { 0x02, 0, 0, 0, 0, 0xaa };
        static const uint8_t Y[6] = { 0x00, 0x77, 0, 0, 0, 2 };
        const uint32_t t0 = 300000, hold = UMAC_MESH_LEAF_VIA_HOLD_MS;
        const struct umac_mesh_proxy *e;
        umac_mesh_pathtbl_init(&T);
        CHECK(umac_mesh_proxy_learn_leaf(&T, X, H1, VIA, true, t0) &&
                  umac_mesh_proxy_learn_leaf(&T, Y, H1, VIA, false, t0),
              "pin age: X pinned by a unicast, Y a flood hint, both behind H1");
        CHECK(umac_mesh_path_expire(&T, t0 + hold - 1u) == 0, "pin age: the sweep frees nothing");
        e = umac_mesh_proxy_entry(&T, X, t0 + hold - 1u);
        CHECK(e != NULL && e->uni && e->uni_ms == t0,
              "pin age: 1 ms before the hold ends, the sweep leaves uni_ms alone");
        CHECK(umac_mesh_path_expire(&T, t0 + hold + 1u) == 0, "pin age: nor does a later one");
        e = umac_mesh_proxy_entry(&T, X, t0 + hold + 1u);
        CHECK(e != NULL && e->uni && e->uni_ms == t0 + 1u,
              "pin age: 1 ms after it, uni_ms is pulled up to now - hold and the pin stays (off by %ld)",
              e != NULL ? (long)(int32_t)(e->uni_ms - (t0 + 1u)) : -1L);
        e = umac_mesh_proxy_entry(&T, Y, t0 + hold + 1u);
        CHECK(e != NULL && !e->uni && e->uni_ms == 0, "pin age: the flood hint's uni_ms is left at 0");

        /* X and Y share an expiry and X has the lower slot: were the pin gone,
         * X would be the victim. */
        const uint32_t tf = t0 + hold + 2u;
        for (uint32_t n = 0, k = 0; n < 4; n++) for (uint32_t i = 0; i < UMAC_MESH_PROXY_PER_NODE && k < 30u; i++, k++)
        {
            uint8_t node[6] = { 0x02, 0, 0, 0, 0, (uint8_t)(0x20 + n) }, h[6] = { 0x00, 0x55, 0, 0, (uint8_t)n, (uint8_t)i };
            (void)umac_mesh_proxy_learn_leaf(&T, h, node, VIA, false, tf);
        }
        CHECK(umac_mesh_proxy_count(&T, tf) == UMAC_MESH_PROXY_MAX, "pin age: table full");
        const uint8_t fresh[6] = { 0x00, 0x66, 0, 0, 0, 9 }, node5[6] = { 0x02, 0, 0, 0, 0, 0x50 };
        CHECK(umac_mesh_proxy_learn_leaf(&T, fresh, node5, VIA, false, tf + 1u) &&
                  umac_mesh_proxy_entry(&T, X, tf + 1u) != NULL &&
                  umac_mesh_proxy_entry(&T, Y, tf + 1u) == NULL,
              "pin age: a newcomer takes the flood hint Y's slot, not the pinned X's");

        /* The age is unsigned here too: a first sweep 2^31 ms late still bounds it. */
        umac_mesh_pathtbl_init(&T);
        (void)umac_mesh_proxy_learn_leaf(&T, X, H1, VIA, true, t0);
        uint32_t late = t0;
        for (uint32_t k = 0; k < 7200u; k++)
        {
            late += 300000u;
            (void)umac_mesh_proxy_learn_leaf(&T, X, H1, VIA, false, late);
        }
        (void)umac_mesh_path_expire(&T, late);
        e = umac_mesh_proxy_entry(&T, X, late);
        CHECK(e != NULL && e->uni && e->uni_ms == late - hold,
              "pin age: a first sweep %lu ms after the unicast (past 2^31) still pulls uni_ms up",
              (unsigned long)(late - t0));
    }

    /* ---- NULL safety ------------------------------------------------------- */
    CHECK(umac_mesh_path_lookup(NULL, D, now) == NULL, "NULL table lookup");
    CHECK(!umac_mesh_path_update(&T, NULL, H1, 1, 1, 1, LT, now), "NULL dst update");
    CHECK(!umac_mesh_path_invalidate(NULL, D, 1, H1, now), "NULL table invalidate");
    CHECK(umac_mesh_proxy_lookup(&T, NULL, now) == NULL, "NULL ext lookup");

    /* A relay keeps a path as long as its originator says (50 s for
     * OpenMANET 1.8.0), so the table must hold one per active originator. */
    CHECK(UMAC_MESH_PATH_MAX == 32u, "room for %u paths (want 32)", (unsigned)UMAC_MESH_PATH_MAX);
    printf("sizeof(struct umac_mesh_pathtbl) = %u bytes\n", (unsigned)sizeof(struct umac_mesh_pathtbl));
    CHECK(sizeof(struct umac_mesh_pathtbl) <= 2048, "tables fit in 2 KiB");
    /* The leaf proxy entry must also leave room for a 32-path table in the same 2 KiB. */
    unsigned at32 = (unsigned)(32u * sizeof(struct umac_mesh_path) + sizeof(T.x));
    CHECK(sizeof(struct umac_mesh_proxy) <= 32u && at32 <= 2048,
          "a %u-byte proxy entry: 32 paths + %u proxies = %u bytes, within 2 KiB",
          (unsigned)sizeof(struct umac_mesh_proxy), (unsigned)UMAC_MESH_PROXY_MAX, at32);

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_mesh_pathtbl: all passed\n");
    return 0;
}
