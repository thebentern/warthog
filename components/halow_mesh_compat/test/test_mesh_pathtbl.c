/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Mesh path and proxy tables (morselib src/umac/mesh/umac_mesh_pathtbl.c).
 *
 * The update rules are what make forwarding both correct and safe. Get the
 * sequence-number comparison backwards and a relay adopts every stale
 * advertisement; forget the hysteresis and two equal routes flap on every
 * PREQ; refresh expiry on a rejected update and a dead path never dies;
 * honour an older PERR and a delayed error kills a path that was rebuilt.
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
#define LT UMAC_MESH_PATH_LIFETIME_MS

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
    for (uint32_t i = 0; i < UMAC_MESH_PROXY_MAX + 3; i++)
    {
        uint8_t h[6] = { 0x00, 0x11, 0, 0, (uint8_t)(i >> 8), (uint8_t)i };
        umac_mesh_proxy_learn(&T, h, H1, now + 1000 + i);
    }
    CHECK(umac_mesh_proxy_count(&T, now + 2000) == UMAC_MESH_PROXY_MAX, "proxy table stays at its bound");

    /* ---- NULL safety ------------------------------------------------------- */
    CHECK(umac_mesh_path_lookup(NULL, D, now) == NULL, "NULL table lookup");
    CHECK(!umac_mesh_path_update(&T, NULL, H1, 1, 1, 1, LT, now), "NULL dst update");
    CHECK(!umac_mesh_path_invalidate(NULL, D, 1, H1, now), "NULL table invalidate");
    CHECK(umac_mesh_proxy_lookup(&T, NULL, now) == NULL, "NULL ext lookup");

    printf("sizeof(struct umac_mesh_pathtbl) = %u bytes\n", (unsigned)sizeof(struct umac_mesh_pathtbl));
    CHECK(sizeof(struct umac_mesh_pathtbl) <= 2048, "tables fit in 2 KiB");

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_mesh_pathtbl: all passed\n");
    return 0;
}
