/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Host TX fragmentation arithmetic (morselib src/umac/mesh/umac_mesh_frag.c): the S1G data
 * bits per symbol, the longest MPDU the chip can send unfragmented at a rate, and how an
 * MSDU is cut for AT+HOSTFRAG=auto and =<n>.
 *
 * Why a symbol count: an S1G SIG field's Length is 9 bits, in symbols for an A-MPDU, so a
 * PPDU's data field holds at most 511 OFDM symbols: 764 octets at 1 MHz MCS0 (12 data bits a
 * symbol, an 8-bit SERVICE and a 6-bit tail). Morse's Linux driver (beacon.c) refuses a 1 MHz
 * beacon of DOT11AH_1MHZ_MCS0_MAX_BEACON_LENGTH = 764 - FRAGMENTATION_OVERHEAD (36) = 728
 * octets or more, FCS not counted, because one larger "may get fragmented by the FW". The cap
 * here assumes the worse case at each step (a 16-bit SERVICE, an A-MPDU delimiter and its
 * padding) and then takes Morse's 36 octets off too, so at 1 MHz MCS0 it is 720 with the FCS,
 * 716 without: under Morse's 728. That the chip uses a symbol rule at all is derived, not
 * measured.
 *
 * A frame the host hands the chip whole may still be cut by the chip itself (its AT+FRAG
 * threshold, or a rate that cannot carry it), each fragment under its own PN. The host's count
 * of a chip-sealed key's PNs is the floor for re-installing it, so it counts such a frame as
 * the worst case: one PN for an attempt sent whole and one for every fragment of the most the
 * chip could cut it into (umac_mesh_frag_chip_pns).
 *
 * AT+HOSTFRAG=<n> is Linux's threshold (net/mac80211/tx.c ieee80211_tx_h_fragment and
 * ieee80211_fragment): a unicast MSDU is cut when MAC header, body and FCS exceed n, each
 * fragment but the last carrying n - header - FCS body octets, before encryption adds its
 * CCMP header and MIC; cfg80211 makes n even. ieee80211_fragment is replayed below and
 * compared fragment by fragment.
 *
 * The cap on fragments (max_frags). Measured on air 2026-10-03 with AT+TXCAP on the sender and
 * AT+RXCAP on the receiver, chip firmware 1.17.6 at 1 MHz MCS0: of an MSDU the host cut and the
 * chip sealed (HW_ENC), 2 fragments arrive intact and 3 do not (fragment 1 loses More Fragments
 * and gains 32 octets, fragment 2 gains 32). So the plan takes a cap: a threshold n that would cut
 * more is raised to the least that cuts it in max_frags (clamped); more under the chip's own
 * threshold or the rate is not cut (n 0, lim names which), and the datapath picks rates where
 * it needs no more (umac_mesh_frag_count, umac_mesh_frag_slowest_mcs). (red) marks the cases
 * that failed before the cap existed.
 */
#include "umac_mesh_frag.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* 802.11-2020 Tables 23-41 to 23-50, N_SS = 1: N_DBPS by MCS 0-9 at 1, 2, 4, 8, 16 MHz. */
static const unsigned NDBPS[5][10] = {
    { 12, 24, 36, 48, 72, 96, 108, 120, 144, 160 },
    { 26, 52, 78, 104, 156, 208, 234, 260, 312, 0 },     /* MCS9 is not valid at 2 MHz */
    { 54, 108, 162, 216, 324, 432, 486, 540, 648, 720 },
    { 117, 234, 351, 468, 702, 936, 1053, 1170, 1404, 1560 },
    { 234, 468, 702, 936, 1404, 1872, 2106, 2340, 2808, 3120 },
};
static const uint8_t BW[5] = { 1, 2, 4, 8, 16 };

/* Symbols a PSDU of @p octets takes with @p extra_bits of SERVICE and tail. */
static unsigned symbols(unsigned octets, unsigned ndbps, unsigned extra_bits)
{
    return (octets * 8u + extra_bits + ndbps - 1u) / ndbps;
}

/* mac80211's ieee80211_fragment: the body octets of each fragment for threshold @p thr. */
static unsigned mac80211_frags(unsigned hdrlen, unsigned body, unsigned thr, unsigned out[16])
{
    thr &= ~1u;
    if (hdrlen + body + 4u <= thr) { out[0] = body; return 1; }
    const unsigned per = thr - hdrlen - 4u;
    unsigned n = 0, rem = body;
    while (rem != 0u && n < 16u)
    {
        const unsigned f = rem < per ? rem : per;
        out[n++] = f;
        rem -= f;
    }
    return n;
}

static struct umac_mesh_frag_plan plan_max(uint32_t mode, uint32_t chip, uint32_t cap, uint16_t hdr,
                                           uint16_t sec, uint16_t body, uint8_t max)
{
    struct umac_mesh_frag_req r = { .mode = mode, .chip_thresh = chip, .rate_cap = cap,
                                    .hdr_len = hdr, .sec_len = sec, .body_len = body,
                                    .max_frags = max };
    struct umac_mesh_frag_plan p;
    umac_mesh_frag_plan(&r, &p);
    return p;
}

static struct umac_mesh_frag_plan plan(uint32_t mode, uint32_t chip, uint32_t cap, uint16_t hdr,
                                       uint16_t sec, uint16_t body)
{
    return plan_max(mode, chip, cap, hdr, sec, body, 0);
}

/* A 1000- and a 1472-byte ping's MSDU body: Mesh Control, SNAP and the IP packet. */
#define BODY_1000 1042u
#define BODY_1472 1514u

int main(void)
{
    /* ---- data bits per symbol ---------------------------------------------- */
    bool all = true;
    for (unsigned b = 0; b < 5; b++)
    {
        for (uint8_t m = 0; m < 10; m++)
        {
            if (umac_mesh_frag_ndbps(BW[b], m) != NDBPS[b][m])
            {
                printf("     %u MHz MCS%u: %u, want %u\n", BW[b], m, umac_mesh_frag_ndbps(BW[b], m),
                       NDBPS[b][m]);
                all = false;
            }
        }
    }
    CHECK(all, "N_DBPS for MCS0-9 at 1, 2, 4, 8 and 16 MHz is the standard's (MCS9 at 2 MHz is none)");
    CHECK(umac_mesh_frag_ndbps(1, 10) == 6u && umac_mesh_frag_ndbps(2, 10) == 0u,
          "MCS10 is MCS0 repeated twice, at 1 MHz only (%u, %u)", umac_mesh_frag_ndbps(1, 10),
          umac_mesh_frag_ndbps(2, 10));
    CHECK(umac_mesh_frag_ndbps(3, 0) == 0u && umac_mesh_frag_ndbps(0, 0) == 0u &&
              umac_mesh_frag_ndbps(1, 11) == 0u && umac_mesh_frag_ndbps(32, 0) == 0u,
          "no bandwidth but 1, 2, 4, 8 and 16 MHz and no MCS above 10");

    /* ---- the longest MPDU the chip sends whole ----------------------------- */
    CHECK(umac_mesh_frag_mpdu_cap(1, 0) == 720u && umac_mesh_frag_mpdu_cap(1, 0) - 4u < 728u,
          "1 MHz MCS0: 720 octets with the FCS, %u without, under the 728 Morse's driver refuses a "
          "beacon at (%u)", umac_mesh_frag_mpdu_cap(1, 0) - 4u, umac_mesh_frag_mpdu_cap(1, 0));
    CHECK(symbols(764u, 12u, 14u) == 511u && symbols(765u, 12u, 14u) == 512u &&
              UMAC_MESH_FRAG_MORSE_MARGIN == 36u,
          "  764 is the most that fits 511 symbols at 12 bits with an 8-bit SERVICE and a 6-bit "
          "tail; Morse takes 36 off it");
    CHECK(umac_mesh_frag_mpdu_cap(1, 1) == 1488u && umac_mesh_frag_mpdu_cap(1, 2) == 2256u &&
              umac_mesh_frag_mpdu_cap(1, 10) == 340u && umac_mesh_frag_mpdu_cap(2, 0) == 1616u &&
              umac_mesh_frag_mpdu_cap(2, 1) == 3276u,
          "1 MHz MCS1 1488, MCS2 2256, MCS10 340; 2 MHz MCS0 1616, MCS1 3276 (%u %u %u %u %u)",
          umac_mesh_frag_mpdu_cap(1, 1), umac_mesh_frag_mpdu_cap(1, 2),
          umac_mesh_frag_mpdu_cap(1, 10), umac_mesh_frag_mpdu_cap(2, 0),
          umac_mesh_frag_mpdu_cap(2, 1));
    /* A full-size frame: 1500 octets of IP, SNAP 8, Mesh Control 6, 4-address QoS header 32,
     * CCMP 16, FCS 4 = 1566. It needs cutting at 1 MHz MCS0, MCS1 and MCS10 and nowhere else. */
    bool mtu = true;
    for (unsigned b = 0; b < 5; b++)
    {
        for (uint8_t m = 0; m <= 10; m++)
        {
            const uint32_t cap = umac_mesh_frag_mpdu_cap(BW[b], m);
            const bool cut = cap != 0u && 1566u > cap;
            const bool want = BW[b] == 1u && (m == 0u || m == 1u || m == 10u);
            if (cut != want) { printf("     %u MHz MCS%u cap %u\n", BW[b], m, cap); mtu = false; }
        }
    }
    CHECK(mtu, "a 1566-octet MPDU is over the cap at 1 MHz MCS0, MCS1 and MCS10 only");
    /* Every cap, with Morse's 36 back on, is the largest MPDU whose subframe (delimiter + MPDU,
     * padded to 4) fits 511 symbols with 22 bits of SERVICE and tail; 4 octets more do not. */
    bool tight = true;
    for (unsigned b = 0; b < 5; b++)
    {
        for (uint8_t m = 0; m <= 10; m++)
        {
            const unsigned nd = umac_mesh_frag_ndbps(BW[b], m);
            const uint32_t cap = umac_mesh_frag_mpdu_cap(BW[b], m);
            if (nd == 0u) { tight = tight && cap == 0u; continue; }
            const unsigned sub = ((cap + 36u + 4u + 3u) & ~3u);
            if (symbols(sub, nd, 22u) > 511u || symbols(sub + 4u, nd, 22u) <= 511u)
            {
                printf("     %u MHz MCS%u cap %u: %u symbols, next %u\n", BW[b], m, cap,
                       symbols(sub, nd, 22u), symbols(sub + 4u, nd, 22u));
                tight = false;
            }
        }
    }
    CHECK(tight, "every cap is 36 under the most that fits 511 symbols as an A-MPDU subframe");

    /* ---- off ----------------------------------------------------------------- */
    struct umac_mesh_frag_plan p = plan(UMAC_MESH_FRAG_OFF, 256, 300, 32, 16, 2000);
    CHECK(p.n == 1u && p.lim == UMAC_MESH_FRAG_LIM_NONE && p.mpdu_max == 0u,
          "off: whole, whatever the chip's threshold and the rate (n %u)", p.n);

    /* ---- auto ---------------------------------------------------------------- */
    const uint32_t cap0 = umac_mesh_frag_mpdu_cap(1, 0);
    p = plan(UMAC_MESH_FRAG_AUTO, 0, cap0, 32, 16, (uint16_t)(cap0 - 52u));
    CHECK(p.n == 1u, "auto: an MPDU of exactly the rate's cap goes whole (n %u)", p.n);
    p = plan(UMAC_MESH_FRAG_AUTO, 0, cap0, 32, 16, (uint16_t)(cap0 - 51u));
    CHECK(p.n == 2u && p.lim == UMAC_MESH_FRAG_LIM_RATE && p.mpdu_max == cap0 &&
              p.chunk == cap0 - 52u && p.last == 1u,
          "  one octet more is cut in two at the rate's cap: %u + %u body octets (lim %d, max %lu)",
          p.chunk, p.last, (int)p.lim, (unsigned long)p.mpdu_max);
    p = plan(UMAC_MESH_FRAG_AUTO, 0, 0, 32, 16, 2300);
    CHECK(p.n == 1u, "  with no rate known and no chip threshold, whole (n %u)", p.n);
    p = plan(UMAC_MESH_FRAG_AUTO, 0, umac_mesh_frag_mpdu_cap(2, 0), 32, 16, 1514);
    CHECK(p.n == 1u, "  a full-size frame at 2 MHz MCS0 goes whole (n %u)", p.n);
    p = plan(UMAC_MESH_FRAG_AUTO, 0, umac_mesh_frag_mpdu_cap(1, 1), 32, 16, 1514);
    CHECK(p.n == 2u && 32u + 16u + p.chunk + 4u <= 1488u && (p.chunk & 1u) == 0u,
          "  at 1 MHz MCS1 it is cut in two, each MPDU within 1488 (%u + %u)", p.chunk, p.last);
    p = plan(UMAC_MESH_FRAG_AUTO, 0, cap0, 32, 16, 1514);
    CHECK(p.n == 3u && p.chunk == 668u && p.last == 178u,
          "  at 1 MHz MCS0 in three: 668 + 668 + 178 body octets (%u x %u + %u)", p.n, p.chunk,
          p.last);
    p = plan(UMAC_MESH_FRAG_AUTO, 0, umac_mesh_frag_mpdu_cap(1, 10), 32, 16, 1514);
    CHECK(p.n == 6u && 32u + 16u + p.chunk + 4u <= 340u && p.chunk == 288u,
          "  at 1 MHz MCS10 in six of 288 body octets (%u x %u)", p.n, p.chunk);
    p = plan(UMAC_MESH_FRAG_AUTO, 512, umac_mesh_frag_mpdu_cap(2, 7), 32, 16, 1000);
    CHECK(p.n == 3u && p.lim == UMAC_MESH_FRAG_LIM_CHIP && p.mpdu_max == 512u &&
              32u + 16u + p.chunk + 4u <= 512u,
          "  the chip's threshold binds where it is the smaller: every MPDU, CCMP and FCS "
          "included, within 512 (%u x %u, lim %d)", p.n, p.chunk, (int)p.lim);
    p = plan(UMAC_MESH_FRAG_AUTO, 1000, cap0, 32, 16, 1000);
    CHECK(p.lim == UMAC_MESH_FRAG_LIM_RATE && p.mpdu_max == cap0,
          "  and the rate where it is (lim %d, max %lu)", (int)p.lim, (unsigned long)p.mpdu_max);
    p = plan(UMAC_MESH_FRAG_AUTO, 0, 757u, 32, 16, 1000);
    CHECK((p.chunk & 1u) == 0u && p.chunk == 704u,
          "  an odd cap leaves an even body in every fragment but the last (%u)", p.chunk);
    p = plan(UMAC_MESH_FRAG_AUTO, 0, 120u, 32, 16, 2300);
    CHECK(p.n == 0u && p.lim == UMAC_MESH_FRAG_LIM_RATE,
          "  an MSDU that would need more than 16 fragments is not cut (n %u)", p.n);
    p = plan(UMAC_MESH_FRAG_AUTO, 0, 53u, 32, 16, 100);
    CHECK(p.n == 0u, "  nor one with room for less than 2 octets in a fragment (n %u)", p.n);
    p = plan(7u, 0, cap0, 32, 16, 1000);
    CHECK(p.n == 2u && p.lim == UMAC_MESH_FRAG_LIM_RATE,
          "  a mode under 256 other than off has no threshold of its own (n %u)", p.n);

    /* ---- a threshold, as Linux takes it ------------------------------------- */
    static const unsigned thr[] = { 256, 257, 512, 700, 1000, 1499, 2346 };
    static const unsigned hdrs[] = { 26, 32 };
    static const unsigned bodies[] = { 100, 199, 200, 476, 477, 1014, 1514, 1526, 2304 };
    bool lin = true;
    unsigned cases = 0;
    for (unsigned t = 0; t < sizeof(thr) / sizeof(thr[0]); t++)
    {
        for (unsigned h = 0; h < 2; h++)
        {
            for (unsigned b = 0; b < sizeof(bodies) / sizeof(bodies[0]); b++)
            {
                for (unsigned sec = 0; sec <= 16u; sec += 16u)
                {
                    unsigned want[16];
                    const unsigned n = mac80211_frags(hdrs[h], bodies[b], thr[t], want);
                    p = plan(thr[t], 0, 0, (uint16_t)hdrs[h], (uint16_t)sec, (uint16_t)bodies[b]);
                    unsigned got_n = p.n == 0u ? 99u : p.n;
                    bool same = got_n == n;
                    for (unsigned i = 0; same && i + 1u < n; i++) { same = want[i] == p.chunk; }
                    same = same && (n == 1u || want[n - 1u] == p.last);
                    if (!same)
                    {
                        printf("     thr %u hdr %u body %u sec %u: mac80211 %u, plan %u x %u + %u\n",
                               thr[t], hdrs[h], bodies[b], sec, n, p.n, p.chunk, p.last);
                        lin = false;
                    }
                    cases++;
                }
            }
        }
    }
    CHECK(lin, "=<n>: the same cut as mac80211's ieee80211_fragment, CCMP not counted (%u cases)",
          cases);
    p = plan(512, 0, cap0, 32, 16, 1000);
    CHECK(p.lim == UMAC_MESH_FRAG_LIM_THRESH && p.mpdu_max == 528u && p.n == 3u,
          "=512 binds below the rate's cap: each MPDU at most 512 + 16 (max %lu, n %u)",
          (unsigned long)p.mpdu_max, p.n);
    p = plan(2346, 0, cap0, 32, 16, 1000);
    CHECK(p.lim == UMAC_MESH_FRAG_LIM_RATE && p.mpdu_max == cap0,
          "=2346 still cuts what the rate cannot carry (lim %d)", (int)p.lim);
    p = plan(2346, 300, 0, 32, 16, 1000);
    CHECK(p.lim == UMAC_MESH_FRAG_LIM_CHIP && p.mpdu_max == 300u,
          "  and what the chip's threshold would (lim %d)", (int)p.lim);
    CHECK(UMAC_MESH_FRAG_THRESH_MIN == 256u && UMAC_MESH_FRAG_THRESH_MAX == 2346u &&
              UMAC_MESH_FRAG_MAX == 16u,
          "thresholds 256..2346 as cfg80211 accepts them; 16 fragments, a 4-bit number");
    umac_mesh_frag_plan(NULL, &p);
    CHECK(p.n == 1u, "no request: whole");

    /* ---- the fragment cap ----------------------------------------------------- */
    CHECK(UMAC_MESH_FRAG_CHIP_MAX == 2u, "chip-sealed fragments: at most 2 (1.17.6, measured)");
    CHECK(umac_mesh_frag_count(52u, cap0 - 52u, cap0) == 1u && umac_mesh_frag_count(52u, cap0 - 51u, cap0) == 2u &&
              umac_mesh_frag_count(52u, BODY_1472, cap0) == 3u &&
              umac_mesh_frag_count(52u, BODY_1472, umac_mesh_frag_mpdu_cap(1, 1)) == 2u &&
              umac_mesh_frag_count(52u, BODY_1472, umac_mesh_frag_mpdu_cap(2, 0)) == 1u &&
              umac_mesh_frag_count(52u, BODY_1472, umac_mesh_frag_mpdu_cap(1, 10)) == 6u &&
              umac_mesh_frag_count(52u, BODY_1000, cap0) == 2u,
          "(red) count: whole at the cap, 2 one octet over; a 1472-byte ping 3 at 1 MHz MCS0, 2 at "
          "MCS1, whole at 2 MHz MCS0, 6 at MCS10; a 1000-byte ping 2 at 1 MHz MCS0 (%u %u %u %u)",
          umac_mesh_frag_count(52u, cap0 - 52u, cap0), umac_mesh_frag_count(52u, BODY_1472, cap0),
          umac_mesh_frag_count(52u, BODY_1472, umac_mesh_frag_mpdu_cap(1, 1)),
          umac_mesh_frag_count(52u, BODY_1000, cap0));
    CHECK(umac_mesh_frag_count(52u, 2300u, 120u) == 0u && umac_mesh_frag_count(52u, 100u, 53u) == 0u,
          "  0 for more than 16, or no room for 2 octets (%u, %u)", umac_mesh_frag_count(52u, 2300u, 120u),
          umac_mesh_frag_count(52u, 100u, 53u));
    bool same_n = true;
    for (uint32_t lim = 60u; lim <= 2400u; lim += 37u)
    {
        for (uint16_t b = 1u; b <= 2304u; b = (uint16_t)(b + 61u))
        {
            p = plan(UMAC_MESH_FRAG_AUTO, 0, lim, 32, 16, b);
            if (umac_mesh_frag_count(52u, b, lim) != p.n) { same_n = false; }
        }
    }
    CHECK(same_n, "(red) count agrees with the uncapped plan everywhere it is defined");

    p = plan_max(UMAC_MESH_FRAG_AUTO, 0, cap0, 32, 16, BODY_1000, 2);
    CHECK(p.n == 2u && p.lim == UMAC_MESH_FRAG_LIM_RATE && !p.clamped,
          "max 2: a 1000-byte ping at 1 MHz MCS0 is cut in 2 as before (n %u)", p.n);
    p = plan_max(UMAC_MESH_FRAG_AUTO, 0, cap0, 32, 16, BODY_1472, 2);
    CHECK(p.n == 0u && p.lim == UMAC_MESH_FRAG_LIM_RATE && p.mpdu_max == cap0 && p.thr == 0u,
          "(red) max 2: a 1472-byte ping at 1 MHz MCS0 (3 fragments) is not cut; the rate binds "
          "(n %u, lim %d, thr %lu)", p.n, (int)p.lim, (unsigned long)p.thr);
    p = plan_max(UMAC_MESH_FRAG_AUTO, 512, umac_mesh_frag_mpdu_cap(2, 7), 32, 16, 1000, 2);
    CHECK(p.n == 0u && p.lim == UMAC_MESH_FRAG_LIM_CHIP && p.thr == 512u,
          "(red) max 2: 3 under the chip's threshold is not cut; the chip's threshold binds (n %u, "
          "lim %d, thr %lu)", p.n, (int)p.lim, (unsigned long)p.thr);
    p = plan_max(512, 0, umac_mesh_frag_mpdu_cap(2, 7), 32, 16, BODY_1472, 2);
    CHECK(p.n == 2u && p.clamped && p.lim == UMAC_MESH_FRAG_LIM_THRESH && p.chunk == 758u &&
              p.last == 756u && p.mpdu_max == 810u && p.thr == 810u,
          "(red) max 2, =512 at 2 MHz MCS7 (Linux would cut 4): raised to 810, the least that cuts "
          "it in 2: %u + %u (n %u, clamped %d, max %lu)", p.chunk, p.last, p.n, (int)p.clamped,
          (unsigned long)p.mpdu_max);
    p = plan_max(512, 0, cap0, 32, 16, BODY_1472, 2);
    CHECK(p.n == 0u && p.clamped && p.lim == UMAC_MESH_FRAG_LIM_RATE && p.thr == 810u,
          "(red)   at 1 MHz MCS0 the rate still needs 3: not cut, thr 810 for the rate choice "
          "(n %u, lim %d, thr %lu)", p.n, (int)p.lim, (unsigned long)p.thr);
    p = plan_max(1000, 0, umac_mesh_frag_mpdu_cap(2, 7), 32, 16, BODY_1472, 2);
    CHECK(p.n == 2u && !p.clamped && p.chunk == 964u && p.thr == 1016u,
          "(red) =1000 cuts a 1472-byte ping in 2 by Linux's rule: not raised, thr 1016 (n %u, %u, thr %lu)", p.n,
          p.chunk, (unsigned long)p.thr);
    p = plan_max(UMAC_MESH_FRAG_AUTO, 0, cap0, 32, 16, BODY_1472, 0);
    CHECK(p.n == 3u && !p.clamped, "max 0 is 16: uncapped as before (n %u)", p.n);
    bool capped = true;
    for (unsigned t = 0; t < sizeof(thr) / sizeof(thr[0]); t++)
    {
        for (unsigned b = 0; b < sizeof(bodies) / sizeof(bodies[0]); b++)
        {
            for (uint8_t bw = 1; bw <= 2u; bw++)
            {
                for (uint8_t m = 0; m <= 2u; m++)
                {
                    const uint32_t rc = umac_mesh_frag_mpdu_cap(bw, m);
                    p = plan_max(thr[t], 0, rc, 32, 16, (uint16_t)bodies[b], 2);
                    const bool ok = p.n <= 2u && (p.n != 2u || (p.chunk + p.last == bodies[b] &&
                                                                52u + p.chunk <= rc && 52u + p.chunk <= p.thr)) &&
                                    (p.n != 0u || p.lim == UMAC_MESH_FRAG_LIM_RATE);
                    if (!ok)
                    {
                        printf("     thr %u body %u at %u MHz MCS%u: n %u, %u + %u, lim %d, thr %lu\n", thr[t],
                               bodies[b], bw, m, p.n, p.chunk, p.last, (int)p.lim, (unsigned long)p.thr);
                        capped = false;
                    }
                }
            }
        }
    }
    CHECK(capped, "(red) max 2 under every threshold: at most 2, each within the rate and the raised "
          "threshold, or not cut with the rate named");

    CHECK(umac_mesh_frag_slowest_mcs(1, 52u, BODY_1472, 0, 2) == 1 &&
              umac_mesh_frag_slowest_mcs(1, 52u, BODY_1000, 0, 2) == 0 &&
              umac_mesh_frag_slowest_mcs(1, 52u, BODY_1472, 0, 1) == 2 &&
              umac_mesh_frag_slowest_mcs(2, 52u, BODY_1472, 0, 1) == 0 &&
              umac_mesh_frag_slowest_mcs(1, 52u, BODY_1000, 0, 1) == 1,
          "(red) slowest rate: a 1472-byte ping in 2 at 1 MHz MCS1, a 1000-byte at MCS0; whole, "
          "1 MHz MCS2, 2 MHz MCS0 and (1000) 1 MHz MCS1 (%d %d %d %d %d)",
          umac_mesh_frag_slowest_mcs(1, 52u, BODY_1472, 0, 2), umac_mesh_frag_slowest_mcs(1, 52u, BODY_1000, 0, 2),
          umac_mesh_frag_slowest_mcs(1, 52u, BODY_1472, 0, 1), umac_mesh_frag_slowest_mcs(2, 52u, BODY_1472, 0, 1),
          umac_mesh_frag_slowest_mcs(1, 52u, BODY_1000, 0, 1));
    CHECK(umac_mesh_frag_slowest_mcs(1, 52u, BODY_1472, 512u, 2) == -1 &&
              umac_mesh_frag_slowest_mcs(1, 52u, BODY_1472, 810u, 2) == 1 &&
              umac_mesh_frag_slowest_mcs(3, 52u, 100u, 0, 1) == -1 &&
              umac_mesh_frag_slowest_mcs(1, 52u, 100u, 0, 1) == 0,
          "(red)   none under a threshold that needs 3 at any rate; 810 allows MCS1; no 3 MHz; small "
          "frames at MCS0 (%d %d %d %d)", umac_mesh_frag_slowest_mcs(1, 52u, BODY_1472, 512u, 2),
          umac_mesh_frag_slowest_mcs(1, 52u, BODY_1472, 810u, 2), umac_mesh_frag_slowest_mcs(3, 52u, 100u, 0, 1),
          umac_mesh_frag_slowest_mcs(1, 52u, 100u, 0, 1));

    /* The least AT+FRAG under which a full-size frame (1500-octet IP packet) still goes in 2, as the
     * AT reference and Troubleshooting give it: 810, and 816 with Address Extension (12 octets more).
     * Under it the plan cuts nothing and the datapath counts the frame many. */
    const struct umac_mesh_frag_plan f808 = plan_max(UMAC_MESH_FRAG_AUTO, 808u, 0, 32, 16, BODY_1472, 2);
    const struct umac_mesh_frag_plan f810 = plan_max(UMAC_MESH_FRAG_AUTO, 810u, 0, 32, 16, BODY_1472, 2);
    const struct umac_mesh_frag_plan a814 = plan_max(UMAC_MESH_FRAG_AUTO, 814u, 0, 32, 16, BODY_1472 + 12u, 2);
    const struct umac_mesh_frag_plan a816 = plan_max(UMAC_MESH_FRAG_AUTO, 816u, 0, 32, 16, BODY_1472 + 12u, 2);
    CHECK(f808.n == 0u && f808.lim == UMAC_MESH_FRAG_LIM_CHIP && f810.n == 2u && f810.chunk == 758u &&
              a814.n == 0u && a816.n == 2u && a816.chunk == 764u &&
              umac_mesh_frag_slowest_mcs(1, 52u, BODY_1472, 808u, 2) == -1,
          "(pin) AT+FRAG floor for a full-size frame in 2: 808 none (n %u, lim %d), 810 (n %u, %u + %u); "
          "with Address Extension 814 none (n %u), 816 (n %u, %u + %u)", f808.n, (int)f808.lim, f810.n,
          f810.chunk, f810.last, a814.n, a816.n, a816.chunk, a816.last);

    /* ---- PNs the chip may draw for a frame handed whole ---------------------- */
    CHECK(umac_mesh_frag_chip_pns(720u, 52u, cap0) == 1u &&
              umac_mesh_frag_chip_pns(340u, 52u, 0u) == 1u,
          "a frame every limit carries draws one PN; with none known, the floor (1 MHz MCS10, "
          "%u) is the limit", umac_mesh_frag_mpdu_cap(1, 10));
    CHECK(umac_mesh_frag_chip_pns(1066u, 52u, 512u) == 4u &&
              umac_mesh_frag_chip_pns(1566u, 52u, cap0) == 4u &&
              umac_mesh_frag_chip_pns(1566u, 52u, 0u) == 7u,
          "  over it: one sent whole and every fragment of the most it could be cut into (%u, %u, %u)",
          umac_mesh_frag_chip_pns(1066u, 52u, 512u), umac_mesh_frag_chip_pns(1566u, 52u, cap0),
          umac_mesh_frag_chip_pns(1566u, 52u, 0u));
    CHECK(umac_mesh_frag_chip_pns(2400u, 52u, 60u) == 17u &&
              umac_mesh_frag_chip_pns(2400u, 52u, 53u) == 17u,
          "  at most 16 fragments, a 4-bit number (%u, %u)", umac_mesh_frag_chip_pns(2400u, 52u, 60u),
          umac_mesh_frag_chip_pns(2400u, 52u, 53u));
    /* Whatever rule the chip cuts by at a limit -- mac80211's, CCMP counted or not -- it makes
     * no more fragments than the count allows for. */
    bool over = true;
    for (unsigned t = 0; t < sizeof(thr) / sizeof(thr[0]); t++)
    {
        for (unsigned b = 0; b < sizeof(bodies) / sizeof(bodies[0]); b++)
        {
            for (unsigned sec = 0; sec <= 16u; sec += 16u)
            {
                unsigned f[16];
                const unsigned mpdu = 32u + bodies[b] + 16u + 4u;
                const unsigned lim = thr[t] & ~1u;
                const unsigned n = mac80211_frags(32u + sec, bodies[b], lim, f);
                const uint32_t pns = umac_mesh_frag_chip_pns(mpdu, 52u, lim);
                if (pns < (mpdu > lim ? n + 1u : 1u)) { printf("     %u in %u: %u\n", mpdu, lim, pns); over = false; }
            }
        }
    }
    CHECK(over, "  never fewer than a mac80211-style cut at that limit makes, plus the whole attempt");

    printf(failures ? "test_mesh_frag: %d FAILED\n" : "test_mesh_frag: all passed\n", failures);
    return failures != 0;
}
