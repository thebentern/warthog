/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The 802.11s peer-link table
 * (morselib src/umac/mesh/umac_mesh_plink_tbl.c).
 *
 * Three separate three-board failures came out of this logic, and every one of
 * them cost a flash-and-measure cycle to find:
 *   - one global link-id pair, so a third node bound the only slot;
 *   - a peer that lost power and was never expired, because MPM Close only
 *     arrives when a peer leaves politely;
 *   - a rejoining node handed back a slot whose llid had not changed, so its
 *     peers could not tell the link had restarted.
 * All three are pure table logic and none of them needs a radio to catch.
 */
#include "umac_mesh_plink_tbl.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static const uint8_t A[6] = { 0x3c,0x1a,0xcc,0x4c,0x83,0xa5 };
static const uint8_t B[6] = { 0x3c,0x1a,0xcc,0x4c,0x81,0x9d };
static const uint8_t C[6] = { 0xa8,0xdd,0x9f,0x4d,0xc7,0xf8 };
static const uint8_t D[6] = { 0x02,0x00,0x00,0x00,0x00,0x01 };
static const uint8_t E[6] = { 0x02,0x00,0x00,0x00,0x00,0x02 };

int main(void)
{
    struct mpm_table t;

    /* ---- basics ---------------------------------------------------- */
    mpm_table_init(&t);
    CHECK(mpm_table_find(&t, A) == NULL, "empty table finds nothing");
    CHECK(mpm_table_estab_count(&t) == 0, "empty table has no established links");

    struct mpm_link *la = mpm_table_get_or_create(&t, A, 0x1111, 1000);
    CHECK(la != NULL && la->llid == 0x1111, "create assigns the given llid");
    CHECK(la->last_heard_ms == 1000, "create stamps last_heard");
    CHECK(la->used && !la->estab, "new link is used but not established");
    CHECK(mpm_table_find(&t, A) == la, "find returns the created link");
    CHECK(mpm_table_find(&t, B) == NULL, "find does not confuse two addresses");

    /* An existing link must KEEP its llid. Re-minting mid-handshake is what
     * makes a peer answer CNF_IGNR, and the caller mints on every frame. */
    struct mpm_link *again = mpm_table_get_or_create(&t, A, 0x9999, 2000);
    CHECK(again == la, "get_or_create returns the existing link");
    CHECK(again->llid == 0x1111, "existing link keeps its llid (CNF_IGNR trap)");
    CHECK(again->last_heard_ms == 1000, "get_or_create does not restamp an existing link");

    /* Distinct peers get distinct entries. */
    struct mpm_link *lb = mpm_table_get_or_create(&t, B, 0x2222, 1000);
    CHECK(lb != NULL && lb != la, "second peer gets its own entry");
    lb->estab = true;
    CHECK(mpm_table_estab_count(&t) == 1, "estab_count counts only established links");
    la->estab = true;
    CHECK(mpm_table_estab_count(&t) == 2, "estab_count tracks both links");

    /* ---- capacity --------------------------------------------------- */
    CHECK(mpm_table_get_or_create(&t, C, 3, 1000) != NULL, "third peer fits");
    CHECK(mpm_table_get_or_create(&t, D, 4, 1000) != NULL, "fourth peer fits");
    CHECK(mpm_table_get_or_create(&t, E, 5, 1000) == NULL, "fifth peer is refused");
    CHECK(t.no_slot == 1, "refusal is counted (a real mesh answers Close(MESH_MAX_PEERS))");
    CHECK(mpm_table_find(&t, E) == NULL, "refused peer was not stored");
    /* A full table must still serve the peers it already has. */
    CHECK(mpm_table_find(&t, A) == la, "full table still finds existing peers");

    /* ---- release and slot reuse ------------------------------------- */
    mpm_table_release(&t, la);
    CHECK(mpm_table_find(&t, A) == NULL, "released link is gone");
    CHECK(mpm_table_estab_count(&t) == 1, "release drops its established count");
    struct mpm_link *le = mpm_table_get_or_create(&t, E, 0x5555, 1000);
    CHECK(le != NULL, "a freed slot is reused");
    CHECK(le->llid == 0x5555 && !le->estab && le->plid == 0,
          "reused slot is fully reset, not inherited from the previous peer");
    mpm_table_release(&t, NULL); /* must not crash */
    CHECK(1, "release(NULL) is a no-op");

    /* ---- expiry ----------------------------------------------------- */
    mpm_table_init(&t);
    mpm_table_get_or_create(&t, A, 1, 10000);
    mpm_table_get_or_create(&t, B, 2, 10000);
    uint8_t gone[MPM_MAX_LINKS][MPM_ADDR_LEN];

    int n = mpm_table_expire(&t, 39999, 30000, gone, MPM_MAX_LINKS);
    CHECK(n == 0 && mpm_table_find(&t, A) != NULL, "not expired one ms before the timeout");

    /* Refresh only A; B must be the one that goes. */
    mpm_table_find(&t, A)->last_heard_ms = 39000;
    n = mpm_table_expire(&t, 40000, 30000, gone, MPM_MAX_LINKS);
    CHECK(n == 1, "exactly one link expired at the boundary");
    CHECK(memcmp(gone[0], B, 6) == 0, "the expired address is reported for teardown");
    CHECK(mpm_table_find(&t, B) == NULL, "expired link is removed");
    CHECK(mpm_table_find(&t, A) != NULL, "a peer still being heard survives");
    CHECK(t.expired == 1, "expiry is counted");

    /* last_heard 0 means "never stamped" and must never expire. */
    mpm_table_init(&t);
    struct mpm_link *l0 = mpm_table_get_or_create(&t, A, 1, 0);
    l0->last_heard_ms = 0;
    CHECK(mpm_table_expire(&t, 0xffffffffu, 30000, gone, MPM_MAX_LINKS) == 0,
          "an unstamped link is never expired");

    /* A wrapped millisecond clock must measure the true interval, not a huge
     * one -- otherwise every link is expired the moment the clock rolls over. */
    mpm_table_init(&t);
    mpm_table_get_or_create(&t, A, 1, 0xfffffff0u);
    CHECK(mpm_table_expire(&t, 0x00000010u, 30000, gone, MPM_MAX_LINKS) == 0,
          "clock wrap does not expire a link that was just heard");
    CHECK(mpm_table_expire(&t, 0x00007540u, 30000, gone, MPM_MAX_LINKS) == 1,
          "clock wrap still expires a genuinely silent link");

    /* Expiry must not depend on the caller providing an output buffer. */
    mpm_table_init(&t);
    mpm_table_get_or_create(&t, A, 1, 1000);
    CHECK(mpm_table_expire(&t, 100000, 30000, NULL, 0) == 0, "no output buffer reports nothing");
    CHECK(mpm_table_find(&t, A) == NULL, "...but the link is still expired");

    /* More expiries than the caller can hold: the extras must still be removed,
     * or a dead peer lingers forever with no way to notice. */
    mpm_table_init(&t);
    mpm_table_get_or_create(&t, A, 1, 1000);
    mpm_table_get_or_create(&t, B, 2, 1000);
    mpm_table_get_or_create(&t, C, 3, 1000);
    n = mpm_table_expire(&t, 100000, 30000, gone, 1);
    CHECK(n == 1, "reports only what fits in the caller's buffer");
    CHECK(mpm_table_estab_count(&t) == 0 && mpm_table_find(&t, A) == NULL &&
          mpm_table_find(&t, B) == NULL && mpm_table_find(&t, C) == NULL,
          "every stale link is removed even when only one is reported");

    /* ---- render ------------------------------------------------------ */
    char buf[256];
    mpm_table_init(&t);
    mpm_table_render(&t, buf, sizeof(buf));
    CHECK(strcmp(buf, "(none) ") == 0, "empty table renders as (none)");

    struct mpm_link *lr = mpm_table_get_or_create(&t, A, 0xabcd, 1000);
    CHECK(lr->opens == 0, "a new link starts with no Opens sent");
    lr->plid = 0x1234;
    lr->estab = true;
    lr->opens = 7;
    mpm_table_render(&t, buf, sizeof(buf));
    CHECK(strstr(buf, "4c83a5") && strstr(buf, "llid=43981") && strstr(buf, "plid=4660") &&
          strstr(buf, "estab=1"),
          "render shows address, both link ids and estab");
    /* The unanswered-Open count drives the Close that recovers a peer holding
     * a stale link, so it has to be visible from AT+MPMPEERS? to be diagnosed. */
    CHECK(strstr(buf, "opens=7"), "render shows the unanswered-Open count");

    /* A slot reused for a different peer must not inherit the old Open count,
     * or a rejoining node is Closed before it has been given a chance to
     * answer. get_or_create zeroes the entry; this pins that it stays that way. */
    mpm_table_release(&t, lr);
    struct mpm_link *lr2 = mpm_table_get_or_create(&t, B, 0x1111, 2000);
    CHECK(lr2 != NULL && lr2->opens == 0, "a reused slot starts the Open count over");

    /* A short buffer must be truncated safely, not overrun. Under ASan a
     * regression here is a hard failure rather than a judgement call. */
    char tiny[8];
    memset(tiny, 0x7f, sizeof(tiny));
    mpm_table_render(&t, tiny, sizeof(tiny));
    CHECK(memchr(tiny, '\0', sizeof(tiny)) != NULL, "short buffer is still NUL-terminated");
    mpm_table_render(&t, buf, 0);
    CHECK(1, "zero-length buffer is a no-op");
    mpm_table_render(NULL, buf, sizeof(buf));
    CHECK(buf[0] == '\0', "NULL table renders an empty string");

    /* ---- NULL-safety on every entry point ---------------------------- */
    CHECK(mpm_table_find(NULL, A) == NULL, "find(NULL table)");
    CHECK(mpm_table_find(&t, NULL) == NULL, "find(NULL addr)");
    CHECK(mpm_table_get_or_create(NULL, A, 1, 0) == NULL, "get_or_create(NULL table)");
    CHECK(mpm_table_get_or_create(&t, NULL, 1, 0) == NULL, "get_or_create(NULL addr)");
    CHECK(mpm_table_estab_count(NULL) == 0, "estab_count(NULL)");
    CHECK(mpm_table_expire(NULL, 0, 0, gone, MPM_MAX_LINKS) == 0, "expire(NULL table)");
    mpm_table_init(NULL);
    CHECK(1, "init(NULL) is a no-op");

    printf(failures ? "\nFAILED (%d)\n" : "\nALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
