/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Recent Multicast Cache (morselib src/umac/mesh/umac_mesh_rmc.c).
 *
 * This is the one structure that decides whether turning on forwarding
 * produces a mesh or a broadcast storm. The properties that matter: a repeat
 * is caught, a fresh one is not, the key is (source, seq) and not either
 * alone, memory is bounded and eviction is oldest-first, entries expire, and
 * the clock arithmetic survives a 32-bit millisecond wrap -- a relay that has
 * been up 49 days must not suddenly start looping or suddenly start dropping.
 */
#include "umac_mesh_rmc.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static const uint8_t A[6] = { 0x02, 0, 0, 0, 0, 0x01 };
static const uint8_t B[6] = { 0x02, 0, 0, 0, 0, 0x02 };
/* Same last octet as A, differs in a high octet: must NOT collide as a key. */
static const uint8_t A2[6] = { 0x02, 0xff, 0, 0, 0, 0x01 };

static struct umac_mesh_rmc rmc;

int main(void)
{
    umac_mesh_rmc_init(&rmc);
    uint32_t now = 1000;

    CHECK(!umac_mesh_rmc_check(&rmc, A, 1, now), "first sight of (A,1) is not a duplicate");
    CHECK( umac_mesh_rmc_check(&rmc, A, 1, now), "second sight of (A,1) IS a duplicate");
    CHECK( umac_mesh_rmc_check(&rmc, A, 1, now + 5), "still a duplicate a moment later");
    CHECK(!umac_mesh_rmc_check(&rmc, A, 2, now), "(A,2) is new");
    CHECK(!umac_mesh_rmc_check(&rmc, B, 1, now), "(B,1) is new: the key is source AND seq");
    CHECK(!umac_mesh_rmc_check(&rmc, A2, 1, now), "(A2,1) is new even though A2 shares A's last octet");
    CHECK( umac_mesh_rmc_check(&rmc, A2, 1, now), "and (A2,1) is then a duplicate on its own");
    CHECK(umac_mesh_rmc_count(&rmc, now) == 4, "four live entries (got %u)", (unsigned)umac_mesh_rmc_count(&rmc, now));

    /* Expiry. */
    now += UMAC_MESH_RMC_TIMEOUT_MS - 1;
    CHECK( umac_mesh_rmc_check(&rmc, A, 1, now), "one ms before timeout: still remembered");
    now += 1;
    CHECK(!umac_mesh_rmc_check(&rmc, A, 1, now), "at timeout: forgotten, so (A,1) is new again");
    CHECK( umac_mesh_rmc_check(&rmc, A, 1, now), "and remembered afresh");

    /* Bounded memory: the (QUEUE+1)th distinct seq from one source evicts the
     * oldest, and only the oldest. */
    umac_mesh_rmc_init(&rmc);
    now = 5000;
    for (uint32_t s = 0; s < UMAC_MESH_RMC_QUEUE; s++)
    {
        umac_mesh_rmc_check(&rmc, A, 100 + s, now + s); /* seq 100.. with rising time */
    }
    for (uint32_t s = 0; s < UMAC_MESH_RMC_QUEUE; s++)
    {
        CHECK(umac_mesh_rmc_check(&rmc, A, 100 + s, now + 10), "bucket holds seq %u", (unsigned)(100 + s));
    }
    CHECK(!umac_mesh_rmc_check(&rmc, A, 200, now + 10), "one more distinct seq is new");
    /* Probe survivors BEFORE probing the victim: a miss inserts, and inserting
     * into a full bucket evicts again. */
    CHECK( umac_mesh_rmc_check(&rmc, A, 101, now + 10), "seq 101 survived");
    CHECK( umac_mesh_rmc_check(&rmc, A, 102, now + 10), "seq 102 survived");
    CHECK( umac_mesh_rmc_check(&rmc, A, 103, now + 10), "seq 103 survived");
    CHECK( umac_mesh_rmc_check(&rmc, A, 200, now + 10), "seq 200 is remembered");
    CHECK(!umac_mesh_rmc_check(&rmc, A, 100, now + 10), "the OLDEST (seq 100) was the one evicted");

    /* A duplicate does not refresh the entry's expiry -- otherwise a chatty
     * repeater could keep an entry alive forever and pin the bucket. */
    umac_mesh_rmc_init(&rmc);
    now = 10000;
    umac_mesh_rmc_check(&rmc, A, 7, now);
    umac_mesh_rmc_check(&rmc, A, 7, now + UMAC_MESH_RMC_TIMEOUT_MS - 10); /* dup, near expiry */
    CHECK(!umac_mesh_rmc_check(&rmc, A, 7, now + UMAC_MESH_RMC_TIMEOUT_MS), "a duplicate did not extend the entry");

    /* 32-bit millisecond wrap: 0xfffffff0 -> 0x00000010 is 32 ms, not 49 days. */
    umac_mesh_rmc_init(&rmc);
    now = 0xfffffff0u;
    CHECK(!umac_mesh_rmc_check(&rmc, A, 9, now), "entry made just before the clock wraps");
    CHECK( umac_mesh_rmc_check(&rmc, A, 9, 0x00000010u), "still a duplicate 32 ms later, across the wrap");
    CHECK(!umac_mesh_rmc_check(&rmc, A, 9, 0x00000010u + UMAC_MESH_RMC_TIMEOUT_MS), "and expired after the timeout, across the wrap");

    /* A different source's traffic never evicts A's: buckets are per-source. */
    umac_mesh_rmc_init(&rmc);
    now = 20000;
    umac_mesh_rmc_check(&rmc, A, 1, now);
    for (uint32_t s = 0; s < 3 * UMAC_MESH_RMC_QUEUE; s++)
    {
        umac_mesh_rmc_check(&rmc, B, 500 + s, now);
    }
    CHECK(umac_mesh_rmc_check(&rmc, A, 1, now), "B's flood did not evict (A,1)");

    /* Fail open. */
    CHECK(!umac_mesh_rmc_check(NULL, A, 1, now), "NULL cache is not a duplicate");
    CHECK(!umac_mesh_rmc_check(&rmc, NULL, 1, now), "NULL source is not a duplicate");
    CHECK(umac_mesh_rmc_count(NULL, now) == 0, "NULL cache counts zero");

    printf("sizeof(struct umac_mesh_rmc) = %u bytes\n", (unsigned)sizeof(struct umac_mesh_rmc));
    CHECK(sizeof(struct umac_mesh_rmc) <= 8192, "cache fits in 8 KiB");

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_mesh_rmc: all passed\n");
    return 0;
}
