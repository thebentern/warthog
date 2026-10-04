/*
 * AT+RXCAP / AT+TXCAP capture rings (morselib/src/umac/mesh/umac_mesh_cap.c), freestanding.
 *
 * The rings answer what a frame looked like where the chip and the host meet: the first 64 octets
 * of each received data frame before anything parses or decrypts it, and of each data frame the
 * host hands the chip with its descriptor fields and, once read, the chip's TX status. On air on
 * 2026-10-03 host-sealed fragments after the first reached two receivers without a parseable CCMP
 * header while the transmit code sets every field before sealing; these rings are how the run
 * tells the host's bytes from the air's.
 *
 * Covered: arming clears and disarming keeps; RX keeps data frames only, by TA when filtered; TX
 * mode 1 keeps host fragments only, mode 2 every unicast data frame, never group or management,
 * by RA when filtered; 16 kept, the newest, read oldest first and from a sequence number on; the
 * frame's length beside its first 64 octets; a TX status lands on its own frame (packet id and TID),
 * the newest one without a status; a status that finds the ring busy is counted (st_lost) and its
 * frame keeps st=none; a host fragment released with no chip status reads st=untried; the AT lines
 * exact, and never past the caller's buffer. The module is compiled in here, so a test can hold the
 * ring's lock as a writer on another core or the AT task would.
 */
#include <stdio.h>
#include <string.h>

#include "umac_mesh_cap.c"

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static const uint8_t W[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t A[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0a };
static const uint8_t C[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0c };
static const uint8_t BC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* A 4-address QoS data frame (or @p mgmt: a management one) from @p ta to @p ra, @p len octets,
 * fragment @p fn of sequence @p seq, its body octets counting up from @p salt. */
static const uint8_t *frame_(uint8_t *f, uint32_t len, const uint8_t *ra, const uint8_t *ta, uint8_t fn,
                             uint16_t seq, uint8_t salt, int mgmt)
{
    memset(f, 0, len);
    f[0] = mgmt ? 0xd0 : 0x88;
    f[1] = mgmt ? 0x00 : 0x43; /* ToDS FromDS Protected */
    memcpy(f + 4, ra, 6);
    memcpy(f + 10, ta, 6);
    f[22] = (uint8_t)((seq << 4) | fn);
    f[23] = (uint8_t)(seq >> 4);
    for (uint32_t i = 30; i < len; i++) { f[i] = (uint8_t)(salt + i); }
    return f;
}

static int read_all_(unsigned dir, struct mmwlan_cap_rec *out, uint32_t *seen, uint32_t *lost)
{
    return mmwlan_cap_read(dir, 0, out, MMWLAN_CAP_SLOTS, seen, lost);
}

static void t_rx(void)
{
    printf("--- RX ---\n");
    static uint8_t f[1600];
    static struct mmwlan_cap_rec r[MMWLAN_CAP_SLOTS];
    uint32_t seen = 0, lost = 0;
    CHECK(mmwlan_cap_read(MMWLAN_CAP_RX, 0, r, MMWLAN_CAP_SLOTS, &seen, &lost) == 0 && seen == 0,
          "never armed: nothing kept, and no ring taken");
    mmwlan_cap_rx(frame_(f, 100, W, A, 0, 7, 1, 0), 100, 0x2u, 0, 1, -60, 5);
    CHECK(read_all_(MMWLAN_CAP_RX, r, &seen, &lost) == 0, "a frame before arming is not kept");
    CHECK(mmwlan_cap_arm(MMWLAN_CAP_RX, MMWLAN_CAP_HOST_FRAG, NULL) == 1 &&
              mmwlan_cap_mode[MMWLAN_CAP_RX] == 1u, "armed (mode 1)");
    mmwlan_cap_rx(frame_(f, 1032, W, A, 1, 7, 1, 0), 1032, 0x12u, 2, 1, -71, 1000);
    static uint8_t g[64];
    mmwlan_cap_rx(frame_(g, 60, W, A, 0, 8, 2, 1), 60, 0, 0, 1, -71, 1001);
    int n = read_all_(MMWLAN_CAP_RX, r, &seen, &lost);
    CHECK(n == 1 && seen == 1 && r[0].seq == 1u && r[0].len == 1032u && r[0].n == 64u &&
              memcmp(r[0].b, f, 22) == 0 && r[0].b[22] == ((7u << 4) | 1u) && r[0].flags == 0x12u &&
              r[0].mcs == 2u && r[0].bw_mhz == 1u && r[0].rssi == -71 && r[0].t_ms == 1000u,
          "a data frame kept: its length (1032), its first 64 octets, flags, rate, signal and time; a "
          "management frame is not (%d kept)", n);
    char line[400];
    const int w = mmwlan_cap_line(line, sizeof(line), MMWLAN_CAP_RX, &r[0]);
    char want[400];
    int k = snprintf(want, sizeof(want), "+RXCAP: #1 t=1000 len=1032 fl=00000012 mcs=2 bw=1 rssi=-71 | ");
    for (unsigned i = 0; i < 64u; i++) { k += snprintf(want + k, sizeof(want) - (size_t)k, "%02x", r[0].b[i]); }
    snprintf(want + k, sizeof(want) - (size_t)k, "\r\n");
    CHECK(strcmp(line, want) == 0 && w == (int)strlen(want), "the AT line, exact (%d octets)", w);
    char small[40];
    memset(small, 'Z', sizeof(small));
    const int w2 = mmwlan_cap_line(small, 24, MMWLAN_CAP_RX, &r[0]);
    CHECK(w2 == w && small[23] == '\0' && small[24] == 'Z', "a short buffer: cut, terminated, nothing past it");

    CHECK(mmwlan_cap_arm(MMWLAN_CAP_RX, MMWLAN_CAP_HOST_FRAG, C) == 1, "re-armed with TA C");
    n = read_all_(MMWLAN_CAP_RX, r, &seen, &lost);
    CHECK(n == 0 && seen == 0, "arming clears the ring (%d kept, %lu seen)", n, (unsigned long)seen);
    for (unsigned i = 0; i < 20u; i++)
    {
        mmwlan_cap_rx(frame_(f, 200, W, (i & 1u) ? C : A, 0, (uint16_t)i, (uint8_t)i, 0), 200, 0, 0, 2, -50, i);
    }
    n = read_all_(MMWLAN_CAP_RX, r, &seen, &lost);
    bool order = n == 10;
    for (int i = 0; i < n; i++) { order = order && r[i].seq == (uint32_t)(i + 1) && memcmp(r[i].b + 10, C, 6) == 0; }
    CHECK(order && seen == 10u, "filtered by TA: the 10 frames from C, oldest first (%d)", n);
    for (unsigned i = 0; i < 20u; i++)
    {
        mmwlan_cap_rx(frame_(f, 200, W, C, 0, (uint16_t)(100 + i), (uint8_t)i, 0), 200, 0, 0, 2, -50, 100 + i);
    }
    n = read_all_(MMWLAN_CAP_RX, r, &seen, &lost);
    CHECK(n == 16 && r[0].seq == 15u && r[15].seq == 30u && seen == 30u,
          "the newest 16 are kept, oldest first (#%lu to #%lu of %lu)", (unsigned long)r[0].seq,
          (unsigned long)r[n - 1].seq, (unsigned long)seen);
    struct mmwlan_cap_rec one;
    n = mmwlan_cap_read(MMWLAN_CAP_RX, 27, &one, 1, NULL, NULL);
    CHECK(n == 1 && one.seq == 28u, "read on from #27: #28 next (paging one line at a time)");
    uint8_t mac[6];
    CHECK(mmwlan_cap_filter(MMWLAN_CAP_RX, mac) && memcmp(mac, C, 6) == 0, "the filter reads back");
    CHECK(mmwlan_cap_arm(MMWLAN_CAP_RX, MMWLAN_CAP_OFF, NULL) == 1 && mmwlan_cap_mode[MMWLAN_CAP_RX] == 0u,
          "disarmed");
    n = read_all_(MMWLAN_CAP_RX, r, &seen, &lost);
    CHECK(n == 16 && seen == 30u, "disarming keeps what it holds (%d)", n);
}

static void t_tx(void)
{
    printf("--- TX ---\n");
    static uint8_t f[1600];
    static struct mmwlan_cap_rec r[MMWLAN_CAP_SLOTS];
    uint32_t seen = 0, lost = 0;
    const uint16_t rate[4] = { 0x0200u | 0x10u | 1u, 0x0100u, 0, 0 }; /* MCS1 2 MHz x2, MCS0 1 MHz x1 */
    CHECK(mmwlan_cap_arm(MMWLAN_CAP_TX, MMWLAN_CAP_HOST_FRAG, NULL) == 1, "armed (host fragments)");
    mmwlan_cap_tx(frame_(f, 500, A, W, 0, 9, 3, 0), 500, 0x8u, 0, 0, rate, 41, false, 10);
    mmwlan_cap_tx(frame_(f, 520, A, W, 1, 9, 3, 0), 520, 0x0u, 0, 0x0f, rate, 42, true, 11);
    mmwlan_cap_tx(frame_(f, 300, BC, W, 0, 9, 3, 0), 300, 0x0u, 0, 0, rate, 43, true, 12);
    mmwlan_cap_tx(frame_(f, 40, A, W, 0, 9, 3, 1), 40, 0x0u, 0, 0, rate, 44, true, 13);
    int n = read_all_(MMWLAN_CAP_TX, r, &seen, &lost);
    CHECK(n == 1 && r[0].pkt_id == 42u && r[0].host_frag == 1u && r[0].tid_params == 0x0fu && r[0].len == 520u,
          "mode 1: only the host fragment, not a whole frame, a group frame or a management frame (%d)", n);
    mmwlan_cap_tx_status(42, 1, 0x800000u, 2, 0x1234);
    n = read_all_(MMWLAN_CAP_TX, r, &seen, &lost);
    CHECK(n == 1 && !r[0].st_done, "a status for another TID is not this frame's");
    mmwlan_cap_tx_status(42, 0, 0x800000u, 2, 0x1234);
    n = read_all_(MMWLAN_CAP_TX, r, &seen, &lost);
    CHECK(n == 1 && r[0].st_done && r[0].st_flags == 0x800000u && r[0].st_tries == 2u && r[0].st_ampdu == 0x1234u,
          "its status lands on it: flags (WAS_AGGREGATED), attempts and A-MPDU info");
    char line[400];
    mmwlan_cap_line(line, sizeof(line), MMWLAN_CAP_TX, &r[0]);
    CHECK(strncmp(line, "+TXCAP: #1 t=11 len=520 id=42 cf=00000000 tid=0 tp=0f hf=1 r=1@2Mx2,0@1Mx1,-,- "
                        "st=00800000/2/1234 | 88430000", 99) == 0,
          "the AT line names the descriptor, the chain and the status: %.99s", line);
    CHECK(mmwlan_cap_arm(MMWLAN_CAP_TX, MMWLAN_CAP_ALL_DATA, A) == 1, "re-armed (every unicast data frame to A)");
    mmwlan_cap_tx(frame_(f, 500, A, W, 0, 10, 4, 0), 500, 0x8u, 0, 0, rate, 50, false, 20);
    mmwlan_cap_tx(frame_(f, 500, C, W, 0, 10, 4, 0), 500, 0x8u, 0, 0, rate, 51, false, 21);
    mmwlan_cap_tx(frame_(f, 500, A, W, 0, 11, 4, 0), 500, 0x8u, 0, 0, rate, 50, false, 22);
    mmwlan_cap_tx_status(50, 0, 0, 1, 0);
    n = read_all_(MMWLAN_CAP_TX, r, &seen, &lost);
    CHECK(n == 2 && r[0].pkt_id == 50u && r[1].pkt_id == 50u && !r[0].st_done && r[1].st_done,
          "mode 2 to A: both frames to A, not C's; a repeated packet id's status goes to the newest (%d)", n);
    mmwlan_cap_line(line, sizeof(line), MMWLAN_CAP_TX, &r[0]);
    CHECK(strstr(line, " st=none | ") != NULL, "no status yet reads st=none");
    CHECK(mmwlan_cap_arm(2, 1, NULL) == 0 && mmwlan_cap_read(2, 0, r, 1, NULL, NULL) == 0,
          "an unknown direction is refused");

    /* A status while the ring is held (a capture on another core, AT+TXCAP? reading): counted. */
    CHECK(mmwlan_cap_arm(MMWLAN_CAP_TX, MMWLAN_CAP_ALL_DATA, NULL) == 1 && mmwlan_cap_st_lost() == 0u,
          "re-armed: st_lost from 0");
    mmwlan_cap_tx(frame_(f, 500, A, W, 0, 12, 5, 0), 500, 0x8u, 0, 0, rate, 60, false, 30);
    s_ring[MMWLAN_CAP_TX]->lock = 1u;
    mmwlan_cap_tx_status(60, 0, 0x800000u, 3, 0);
    s_ring[MMWLAN_CAP_TX]->lock = 0u;
    n = read_all_(MMWLAN_CAP_TX, r, &seen, &lost);
    mmwlan_cap_line(line, sizeof(line), MMWLAN_CAP_TX, &r[0]);
    CHECK(n == 1 && !r[0].st_done && strstr(line, " st=none | ") != NULL && mmwlan_cap_st_lost() == 1u &&
              lost == 0u,
          "a status that finds the ring busy: counted st_lost (%lu), its frame still st=none, lost unchanged",
          (unsigned long)mmwlan_cap_st_lost());
    /* A host fragment the driver releases with no chip status (untried): said so, not st=none. */
    mmwlan_cap_tx(frame_(f, 520, A, W, 1, 12, 5, 0), 520, 0x0u, 0, 0, rate, 61, true, 31);
    mmwlan_cap_tx_untried(61, 0);
    mmwlan_cap_tx_status(61, 0, 0, 1, 0);
    n = read_all_(MMWLAN_CAP_TX, r, &seen, &lost);
    mmwlan_cap_line(line, sizeof(line), MMWLAN_CAP_TX, &r[1]);
    CHECK(n == 2 && r[1].pkt_id == 61u && strstr(line, " st=untried | ") != NULL,
          "a fragment released untried reads st=untried, and a later status for its id is not its own: %.90s", line);
    CHECK(mmwlan_cap_arm(MMWLAN_CAP_TX, MMWLAN_CAP_ALL_DATA, NULL) == 1 && mmwlan_cap_st_lost() == 0u,
          "arming clears st_lost");
}

int main(void)
{
    t_rx();
    t_tx();
    printf(failures ? "test_mesh_cap: %d FAILED\n" : "test_mesh_cap: all passed\n", failures);
    return failures != 0;
}
