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
 *
 * The Close(MESH_MAX_PEERS) hold-off lives here too: a refused address stays
 * held off for exactly holdoff_ms of unsigned elapsed time (so a wrapped
 * millisecond clock is safe), a lapsed entry is freed on the next look, the
 * peer's own Open clears it, and init (AT+MESHRELINK) clears them all. Nothing
 * is evicted: with all four entries live, every other address is held off too,
 * and a fifth refusal holds every address without an entry off for its own
 * full holdoff_ms, even after the four lapse. A repeat refusal of a held
 * address restarts its own entry and nothing else. A link's re-announce stamp
 * starts at 0 ("never") on create and after release.
 *
 * So does the answer stamp for a neighbour we will not open toward (it holds
 * no link, so each of its beacons looks like a first sighting): answered at
 * once, then not again until period_ms of unsigned elapsed time has passed,
 * per address, independent of the hold-off, cleared by init. With
 * MPM_MAX_QUIET live a newcomer is not answered and nobody is evicted, so no
 * address is ever answered twice within period_ms.
 *
 * The SAE hold-off (a neighbour whose handshake timed out) is a third stamp set:
 * held for exactly hold_ms of unsigned elapsed time, a repeat restarts its own
 * entry, release (its own Commit) and init clear it. Unlike the Close(53) one it
 * never holds off an address that did not fail: with every entry live, the
 * oldest is replaced, because holding everyone off would keep a working
 * neighbour out of a table the failing ones had just been evicted from.
 *
 * An Open that finds every link taken first takes the link with no answer
 * (not ESTAB, plid 0) that has sent the most Opens; an ESTAB link or one whose
 * peer has named its llid is never taken, nor any link while one is free or
 * the Open's sender already holds one.
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

    /* ---- Close(MESH_MAX_PEERS) hold-off -------------------------------- */
    {
        const uint32_t H = 30000;
        mpm_table_init(&t);
        CHECK(!mpm_table_refused(&t, A, 5000, H), "nobody is held off in a fresh table");
        mpm_table_refuse(&t, A, 5000, H);
        CHECK(mpm_table_refused(&t, A, 5000, H), "refused: held off at once");
        CHECK(mpm_table_refused(&t, A, 5000 + H - 1, H), "still held off 1 ms before the hold-off ends");
        CHECK(!mpm_table_refused(&t, B, 5001, H), "a refusal is per address");
        CHECK(!mpm_table_refused(&t, A, 5000 + H, H), "lapsed exactly at holdoff_ms");
        CHECK(!t.refused[0].used && !t.refused[1].used, "and the lapsed entry was freed");

        /* Across the 32-bit wrap: elapsed is unsigned, so 0xffffff00 -> 0x100
         * is 512 ms, not four billion. */
        mpm_table_refuse(&t, A, 0xffffff00u, H);
        CHECK(mpm_table_refused(&t, A, 0xffffff05u, H), "held off just before the clock wraps");
        CHECK(mpm_table_refused(&t, A, 0x100u, H), "held off across the clock wrap");
        CHECK(!mpm_table_refused(&t, A, 0xffffff00u + H, H), "and lapses on time after it");
        mpm_table_refuse(&t, A, 0xffff0000u, H);
        CHECK(!mpm_table_refused(&t, A, 0x100u, H), "a refusal 65 s before the wrap has lapsed after it");

        mpm_table_refuse(&t, A, 1000, H);
        mpm_table_refuse(&t, A, 9000, H);
        int n = 0;
        for (int i = 0; i < MPM_MAX_LINKS; i++) { n += t.refused[i].used; }
        CHECK(n == 1, "a repeated refusal reuses the address's entry (%d used)", n);
        CHECK(mpm_table_refused(&t, A, 9000 + H - 1, H), "and restarts the hold-off");
        mpm_table_unrefuse(&t, A);
        CHECK(!mpm_table_refused(&t, A, 9001, H), "unrefuse clears it (the peer's own Open)");

        /* Four live: nobody evicted, and nobody new asked. */
        static const uint8_t F[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x03 };
        const uint8_t *who[5] = { A, B, C, D, E };
        mpm_table_init(&t);
        for (int i = 0; i < 4; i++) { mpm_table_refuse(&t, who[i], 100000u + 1000u * (uint32_t)i, H); }
        CHECK(mpm_table_refused(&t, E, 103500, H), "four live: an address that never refused is held off too");
        mpm_table_unrefuse(&t, B);
        CHECK(!mpm_table_refused(&t, E, 103500, H), "until one entry frees (B's own Open)");
        mpm_table_refuse(&t, B, 103600, H);
        mpm_table_refuse(&t, E, 104000, H);
        CHECK(mpm_table_refused(&t, A, 104500, H) && mpm_table_refused(&t, B, 104500, H) &&
              mpm_table_refused(&t, C, 104500, H) && mpm_table_refused(&t, D, 104500, H),
              "a fifth refusal evicts none of the four");
        CHECK(mpm_table_refused(&t, E, 104500, H) && mpm_table_refused(&t, F, 104500, H),
              "and holds off the fifth and any other address");
        CHECK(mpm_table_refused(&t, A, 130000, H), "A's own term ends at 130 s; the fifth's still holds it");
        CHECK(mpm_table_refused(&t, F, 133999, H) && mpm_table_refused(&t, E, 133999, H),
              "the fifth's term outlives the four: all held until 1 ms before 30 s after it");
        CHECK(!mpm_table_refused(&t, E, 134000, H) && !mpm_table_refused(&t, F, 134000, H),
              "and nobody after");

        /* A repeat from a held address restarts its own entry, nothing else. */
        mpm_table_init(&t);
        for (int i = 0; i < 4; i++) { mpm_table_refuse(&t, who[i], 200000u + 1000u * (uint32_t)i, H); }
        mpm_table_refuse(&t, A, 210000, H);
        CHECK(!mpm_table_refused(&t, F, 233000, H),
              "a repeat refusal of a held address holds nobody else past the others' terms");
        CHECK(mpm_table_refused(&t, A, 239999, H) && !mpm_table_refused(&t, A, 240000, H),
              "and restarts its own");

        for (int i = 0; i < 5; i++) { mpm_table_refuse(&t, who[i], 250000u + (uint32_t)i, H); }
        mpm_table_init(&t);
        CHECK(!mpm_table_refused(&t, E, 250010, H) && !mpm_table_refused(&t, F, 250010, H),
              "init clears every hold-off, the fifth's included");

        mpm_table_refuse(NULL, A, 0, H);
        mpm_table_unrefuse(NULL, A);
        CHECK(!mpm_table_refused(NULL, A, 0, H) && !mpm_table_refused(&t, NULL, 0, H),
              "hold-off entry points are NULL-safe");
    }

    /* ---- answer stamp for a neighbour we hold no link to ------------ */
    {
        const uint32_t P = 10000;
        mpm_table_init(&t);
        CHECK(mpm_table_quiet_due(&t, A, 7000, P), "first beacon: answered");
        CHECK(!mpm_table_quiet_due(&t, A, 7000, P), "the next at the same instant: not");
        CHECK(!mpm_table_quiet_due(&t, A, 7000 + P - 1, P), "nor 1 ms before the period ends");
        CHECK(mpm_table_quiet_due(&t, B, 7001, P), "per address: another neighbour is answered");
        CHECK(mpm_table_quiet_due(&t, A, 7000 + P, P), "answered again at exactly period_ms");
        CHECK(!mpm_table_quiet_due(&t, A, 7000 + P + 1, P), "which restamps it");
        CHECK(!mpm_table_refused(&t, A, 7000 + P + 1, 30000), "an answer stamp is not a hold-off");
        mpm_table_refuse(&t, C, 7000, 30000);
        CHECK(mpm_table_quiet_due(&t, C, 7001, P), "nor a hold-off an answer stamp");

        mpm_table_init(&t);
        CHECK(mpm_table_quiet_due(&t, A, 0xfffff000u, P), "answered just before the clock wraps");
        CHECK(!mpm_table_quiet_due(&t, A, 0x100u, P), "quiet across the wrap (4.4 s later)");
        CHECK(mpm_table_quiet_due(&t, A, 0xfffff000u + P, P), "answered on time after it");

        /* Full: a newcomer waits for a stamp to lapse; nobody is answered early. */
        mpm_table_init(&t);
        uint8_t who[MPM_MAX_QUIET + 1][MPM_ADDR_LEN];
        bool all = true;
        for (int i = 0; i <= MPM_MAX_QUIET; i++)
        {
            memcpy(who[i], E, MPM_ADDR_LEN);
            who[i][0] = (uint8_t)(0x40 + i);
            bool due = mpm_table_quiet_due(&t, who[i], 50000u + 10u * (uint32_t)i, P);
            all = all && (i < MPM_MAX_QUIET ? due : true);
        }
        CHECK(all, "MPM_MAX_QUIET (%d) neighbours are answered", MPM_MAX_QUIET);
        CHECK(!mpm_table_quiet_due(&t, who[MPM_MAX_QUIET], 50100, P),
              "with every stamp live, a newcomer is not answered");
        CHECK(!mpm_table_quiet_due(&t, who[0], 50100, P) && !mpm_table_quiet_due(&t, who[2], 50100, P),
              "and nobody is evicted to be answered early");
        CHECK(mpm_table_quiet_due(&t, who[MPM_MAX_QUIET], 60000, P),
              "the newcomer takes the first stamp to lapse");
        CHECK(!mpm_table_quiet_due(&t, who[0], 60000, P),
              "and its owner, due again, waits in turn (every stamp live)");
        CHECK(mpm_table_quiet_due(&t, who[0], 60010, P), "until the next lapses");
        mpm_table_init(&t);
        CHECK(mpm_table_quiet_due(&t, who[2], 50100, P), "init clears every stamp");
        CHECK(!mpm_table_quiet_due(NULL, A, 0, P) && !mpm_table_quiet_due(&t, NULL, 0, P),
              "NULL table or address: false");
    }

    /* ---- an Open makes room: the link nobody answered goes ---------- */
    {
        static const uint8_t F[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x03 };
        uint8_t out[MPM_ADDR_LEN] = { 0 };
        uint16_t out_llid = 0;
        mpm_table_init(&t);
        struct mpm_link *a = mpm_table_get_or_create(&t, A, 0x0a0a, 1000);
        struct mpm_link *b = mpm_table_get_or_create(&t, B, 0x0b0b, 1000);
        struct mpm_link *c = mpm_table_get_or_create(&t, C, 0x0c0c, 1000);
        a->estab = true;
        a->plid = 0x1a1a;
        b->plid = 0x1b1b; /* mid-handshake: B named its llid */
        c->opens = 2;
        CHECK(!mpm_table_make_room(&t, E, out, &out_llid) && mpm_table_find(&t, C) == c,
              "a free link: nothing is released");
        struct mpm_link *d = mpm_table_get_or_create(&t, D, 0x0d0d, 1000);
        d->opens = 5;
        CHECK(!mpm_table_make_room(&t, D, out, &out_llid) && mpm_table_find(&t, D) == d,
              "an Open from a link-holder releases nothing");
        CHECK(mpm_table_make_room(&t, E, out, &out_llid), "full: an Open from E makes room");
        CHECK(memcmp(out, D, MPM_ADDR_LEN) == 0 && out_llid == 0x0d0d && mpm_table_find(&t, D) == NULL,
              "by releasing the unanswered link with the most Opens (D, 5 > 2), reported for the Close");
        CHECK(mpm_table_find(&t, A) == a && mpm_table_find(&t, B) == b && mpm_table_find(&t, C) == c,
              "and nothing else");
        uint32_t ns = t.no_slot;
        struct mpm_link *e = mpm_table_get_or_create(&t, E, 0x0e0e, 2000);
        CHECK(e != NULL && t.no_slot == ns, "E then gets the freed link, not counted as no_slot");

        c->plid = 0x1c1c; /* C answers, and E: every link is ESTAB or named */
        if (e != NULL) { e->plid = 0x1e1e; }
        CHECK(!mpm_table_make_room(&t, F, NULL, NULL) && mpm_table_get_or_create(&t, F, 6, 3000) == NULL,
              "no unanswered link: nothing released, and F is refused");
        a->plid = 0; /* ESTAB alone protects a link */
        CHECK(!mpm_table_make_room(&t, F, NULL, NULL) && mpm_table_find(&t, A) == a,
              "an ESTAB link is never taken, even with plid 0");
        a->estab = false;
        CHECK(mpm_table_make_room(&t, F, NULL, NULL) && mpm_table_find(&t, A) == NULL,
              "NULL outputs are allowed");
        CHECK(!mpm_table_make_room(NULL, F, out, &out_llid) && !mpm_table_make_room(&t, NULL, out, &out_llid),
              "NULL table or address: false");
    }

    /* ---- re-announce stamp ------------------------------------------ */
    {
        mpm_table_init(&t);
        struct mpm_link *l = mpm_table_get_or_create(&t, A, 0x1111, 1000);
        CHECK(l != NULL && l->reannounce_ms == 0, "a new link has never been re-announced to");
        l->reannounce_ms = 4242;
        mpm_table_release(&t, l);
        l = mpm_table_get_or_create(&t, B, 0x2222, 2000);
        CHECK(l != NULL && l->reannounce_ms == 0, "a reused slot does not inherit the stamp");
    }

    /* ---- SAE hold-off after a failed handshake ----------------------- */
    {
        const uint32_t H = 30000;
        mpm_table_init(&t);
        CHECK(!mpm_table_sae_held(&t, A, 1000, H), "nobody is SAE-held in a fresh table");
        mpm_table_sae_hold(&t, A, 1000, H);
        CHECK(mpm_table_sae_held(&t, A, 1000, H) && mpm_table_sae_held(&t, A, 1000 + H - 1, H),
              "held at once and 1 ms before holdoff_ms");
        CHECK(!mpm_table_sae_held(&t, A, 1000 + H, H), "lapsed exactly at holdoff_ms");
        CHECK(!t.sae_held[0].used, "and the lapsed entry was freed");
        CHECK(!mpm_table_refused(&t, A, 2000, H) && mpm_table_quiet_due(&t, A, 2000, H),
              "independent of the Close(53) hold-off and the answer stamps");
        mpm_table_sae_hold(&t, B, 0xffffff00u, H);
        CHECK(mpm_table_sae_held(&t, B, 0x100u, H) && !mpm_table_sae_held(&t, B, 0xffffff00u + H, H),
              "held across the clock wrap, and lapses on time after it");
        mpm_table_sae_hold(&t, C, 5000, H);
        mpm_table_sae_hold(&t, C, 9000, H);
        int n = 0;
        for (int i = 0; i < MPM_MAX_SAE_HELD; i++) { n += t.sae_held[i].used; }
        CHECK(n == 1 && mpm_table_sae_held(&t, C, 9000 + H - 1, H),
              "a repeat failure restarts its own entry (%d entries)", n);
        mpm_table_sae_release(&t, C);
        CHECK(!mpm_table_sae_held(&t, C, 9001, H), "release clears it (the peer's own Commit)");

        /* One more failing neighbour than entries: the oldest gives way, and an
         * address that never failed is never held off. */
        mpm_table_init(&t);
        uint8_t nb[MPM_MAX_SAE_HELD + 1][6];
        for (int i = 0; i <= MPM_MAX_SAE_HELD; i++)
        {
            memcpy(nb[i], E, 6);
            nb[i][5] = (uint8_t)(0x40 + i);
            mpm_table_sae_hold(&t, nb[i], 100000u + 1000u * (uint32_t)i, H);
        }
        bool rest = true;
        for (int i = 1; i <= MPM_MAX_SAE_HELD; i++) { rest &= mpm_table_sae_held(&t, nb[i], 109000, H); }
        CHECK(!mpm_table_sae_held(&t, nb[0], 109000, H) && rest,
              "%d failures in %d entries: the oldest is replaced, the rest stay held",
              MPM_MAX_SAE_HELD + 1, MPM_MAX_SAE_HELD);
        CHECK(!mpm_table_sae_held(&t, A, 109000, H) && !mpm_table_refused(&t, A, 109000, H),
              "every entry live: an address that never failed is not held off");
        {
            /* Entry 0 freed and refilled by the newest: overflow must still take the oldest. */
            mpm_table_init(&t);
            uint8_t v[MPM_MAX_SAE_HELD + 2][6];
            for (int i = 0; i < MPM_MAX_SAE_HELD + 2; i++)
            {
                memcpy(v[i], E, 6);
                v[i][4] = 0x77;
                v[i][5] = (uint8_t)i;
            }
            for (int i = 0; i < MPM_MAX_SAE_HELD; i++)
            {
                mpm_table_sae_hold(&t, v[i], 200000u + 1000u * (uint32_t)i, H);
            }
            mpm_table_sae_release(&t, v[0]);
            mpm_table_sae_hold(&t, v[MPM_MAX_SAE_HELD], 200000u + 1000u * MPM_MAX_SAE_HELD, H);
            mpm_table_sae_hold(&t, v[MPM_MAX_SAE_HELD + 1], 200000u + 1000u * (MPM_MAX_SAE_HELD + 1), H);
            const uint32_t now = 200000u + 1000u * (MPM_MAX_SAE_HELD + 1);
            CHECK(mpm_table_sae_held(&t, v[MPM_MAX_SAE_HELD], now, H) && !mpm_table_sae_held(&t, v[1], now, H) &&
                      mpm_table_sae_held(&t, v[MPM_MAX_SAE_HELD + 1], now, H),
                  "overflow replaces the oldest entry wherever it sits, not the newest in entry 0");
        }
        CHECK(MPM_MAX_SAE_HELD > MPM_MAX_LINKS, "more entries than peer slots (%d > %d)",
              MPM_MAX_SAE_HELD, MPM_MAX_LINKS);
        mpm_table_init(&t);
        CHECK(!mpm_table_sae_held(&t, nb[3], 109000, H), "init (AT+MESHRELINK) clears them all");
        mpm_table_sae_hold(NULL, A, 0, H);
        mpm_table_sae_hold(&t, NULL, 0, H);
        mpm_table_sae_release(NULL, A);
        mpm_table_sae_release(&t, NULL);
        CHECK(!mpm_table_sae_held(NULL, A, 0, H) && !mpm_table_sae_held(&t, NULL, 0, H),
              "NULL table or address: nothing held, nothing written");
    }

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
