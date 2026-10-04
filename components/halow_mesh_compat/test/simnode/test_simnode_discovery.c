/*
 * What a mac80211 / OpenMANET peer needs to see before it will take us as a
 * mesh candidate, read off the frames the firmware hands the chip.
 *
 * Linux mesh.c (ieee80211_mesh_rx_bcn_presp) drops a beacon or probe response
 * that carries no RSN element when its own mesh is secured, and one that
 * carries an RSN element when its mesh is open. It answers a mesh probe
 * request only when the SSID is zero-length and a Mesh ID element is present
 * (ieee80211_mesh_rx_probe_req); the Morse beaconless path also keys on the
 * Mesh ID element. So:
 *
 *  1. every probe request we send -- the one-shot at mesh start and the
 *     periodic one -- is a wildcard SSID, then our Mesh ID element, then S1G
 *     Capabilities, open and SAE alike;
 *  2. open mesh: the beacon and the probe response carry no RSN element, even
 *     after an RSN element is handed over, and the beacon's Security bit is clear;
 *  3. SAE: once the shim hands over hostap's rsn_ie, the beacon carries exactly
 *     those bytes between SSID and Mesh ID with the S1G Security bit set, and
 *     the probe response carries them between Supported Rates and Mesh ID;
 *  4. a new mesh start forgets the RSN element until it is handed over again,
 *     and bytes that are not an RSN element clear it;
 *  5. our own probe request, heard by another warthog on an SAE mesh, is
 *     answered and offered to hostap by its Mesh ID element;
 *  6. the shim runs on another task: an RSN hand-over or clear landing between
 *     the beacon's size and fill passes changes the next beacon, not that one;
 *  7. a well-formed RSN element longer than the 64-byte store is refused and
 *     clears the stored one;
 *  8. with the longest Mesh ID (32) under SAE, the probe requests, the probe
 *     response and the beacon still carry every element: the firmware's own
 *     IE buffers are sized for it, not for the simulator's 7-byte ID.
 *
 * hostap is not linked: the test hands over the bytes hostap's
 * wpa_write_rsn_ie() produces for this mesh config, as the shim does.
 */
#include <stdio.h>
#include <string.h>

#include "simnode.h"
#include "mmpkt.h"
#include "umac_mesh_ies.h"

struct umac_data;
struct umac_data *umac_data_get_umacd(void);
struct mmpkt *umac_mesh_get_beacon(struct umac_data *umacd);
int umac_mesh_tx_broadcast_probe(void);
void umac_mesh_beacon_set_rsn(const uint8_t *rsn, uint16_t len);

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static const uint8_t W[6] = { 0x02, 0, 0, 0, 0, 0x01 }; /* us */
static const uint8_t P[6] = { 0x02, 0, 0, 0, 0, 0x0a }; /* a probing neighbour */
static const uint8_t Q[6] = { 0x02, 0, 0, 0, 0, 0x0b }; /* another warthog */
static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* hostap's wpa_write_rsn_ie() for warthog's mesh (mesh_rsn.c): RSN v1, group
 * CCMP-128, one pairwise CCMP-128, one AKM SAE 00-0F-AC:8, capabilities 0. */
static const uint8_t RSN[] = {
    0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f,
    0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x08, 0x00, 0x00,
};

#define HDR 24u
#define FIXED 12u /* timestamp, beacon interval, capability */
#define FC_PROBE_REQ 0x40u
#define FC_PROBE_RSP 0x50u
#define S1G_BCN_IES 15u /* no optional fields: we set none */

/* The Mesh ID the node under test started with; the simulator's by default. */
static const uint8_t *s_id = (const uint8_t *)"simnode";
static uint8_t s_id_len = 7;

/* The probe-request IEs before S1G Capabilities: wildcard SSID, then our Mesh ID element. */
static uint16_t preq_ies_(uint8_t out[4 + UMAC_MESH_IES_MESH_ID_MAXLEN])
{
    out[0] = 0x00;
    out[1] = 0x00;
    out[2] = 0x72;
    out[3] = s_id_len;
    memcpy(out + 4, s_id, s_id_len);
    return (uint16_t)(4u + s_id_len);
}

/* Rates, [RSN], Mesh ID, Mesh Configuration as our probe response carries them. */
static const uint8_t RATES[] = { 0x01, 0x08, 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c };
static const uint8_t MESHID_IE[] = { 0x72, 0x07, 's', 'i', 'm', 'n', 'o', 'd', 'e' };
static const uint8_t CFG_OPEN[] = { 0x71, 0x07, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01 };
static const uint8_t CFG_SAE[] = { 0x71, 0x07, 0x01, 0x01, 0x00, 0x01, 0x01, 0x00, 0x01 };

static uint16_t cat_(uint8_t *out, const uint8_t *const *parts, const size_t *lens, int n)
{
    uint16_t at = 0;
    for (int i = 0; i < n; i++) { memcpy(out + at, parts[i], lens[i]); at = (uint16_t)(at + lens[i]); }
    return at;
}

/* The element ids of [p, p+n) in order, as a string of decimal ids; "!" on a bad walk. */
static const char *eids_(const uint8_t *p, uint32_t n)
{
    static char s[256];
    size_t at = 0;
    uint32_t off = 0;
    s[0] = '\0';
    while (off + 2u <= n && at + 8u < sizeof(s))
    {
        if (off + 2u + p[off + 1] > n) { strcpy(s + at, "!"); return s; }
        at += (size_t)snprintf(s + at, sizeof(s) - at, "%s%u", at ? "," : "", p[off]);
        off += 2u + p[off + 1];
    }
    if (off != n) { strcpy(s + at, "!"); }
    return s;
}

/* Where element @p eid sits in [p, p+n), or NULL. */
static const uint8_t *find_(const uint8_t *p, uint32_t n, uint8_t eid)
{
    uint32_t off = 0;
    while (off + 2u <= n && off + 2u + p[off + 1] <= n)
    {
        if (p[off] == eid) { return p + off; }
        off += 2u + p[off + 1];
    }
    return NULL;
}

static uint16_t our_beacon_(uint8_t *out, uint16_t cap)
{
    struct mmpkt *b = umac_mesh_get_beacon(umac_data_get_umacd());
    if (b == NULL) { return 0; }
    struct mmpktview *v = mmpkt_open(b);
    uint32_t n = mmpkt_get_data_length(v);
    uint16_t got = 0;
    if (n <= cap) { memcpy(out, mmpkt_get_data_start(v), n); got = (uint16_t)n; }
    mmpkt_close(&v);
    mmpkt_release(b);
    return got;
}

static const struct simnode_frame *last_(uint8_t fc0, const uint8_t *da)
{
    const struct simnode_frame *hit = NULL;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (f->len >= HDR && f->bytes[0] == fc0 && memcmp(f->bytes + 4, da, 6) == 0) { hit = f; }
    }
    return hit;
}

/* A probe request from @p sa in the shape we now send ourselves. */
static bool rx_probe_req_(const uint8_t *sa)
{
    uint8_t f[HDR + 4 + UMAC_MESH_IES_MESH_ID_MAXLEN];
    memset(f, 0, HDR);
    f[0] = FC_PROBE_REQ;
    memcpy(f + 4, BCAST, 6);
    memcpy(f + 10, sa, 6);
    memcpy(f + 16, BCAST, 6);
    const uint16_t n = preq_ies_(f + HDR);
    return simnode_rx(f, (uint16_t)(HDR + n), -60);
}

/* Our probe response to @p sa's probe request: its IEs after SSID, or 0. */
static uint16_t prsp_ies_(const uint8_t *sa, uint8_t *out, uint16_t cap)
{
    simnode_outbox_clear();
    rx_probe_req_(sa);
    const struct simnode_frame *f = last_(FC_PROBE_RSP, sa);
    const uint32_t off = HDR + FIXED + 2u; /* SSID(0) is always first, from frame_probe_response_build */
    if (f == NULL || f->len < off || f->bytes[HDR + FIXED] != 0 || f->bytes[HDR + FIXED + 1] != 0 ||
        f->len - off > cap)
    {
        return 0;
    }
    memcpy(out, f->bytes + off, f->len - off);
    return (uint16_t)(f->len - off);
}

/* A probe request frame the firmware sent: broadcast, from us, wildcard SSID
 * then our Mesh ID element, then S1G Capabilities and nothing else. */
static bool probe_req_shape_(const struct simnode_frame *f)
{
    uint8_t want[4 + UMAC_MESH_IES_MESH_ID_MAXLEN];
    const uint16_t wn = preq_ies_(want);
    if (f == NULL || f->len < HDR + wn || f->bytes[0] != FC_PROBE_REQ ||
        memcmp(f->bytes + 4, BCAST, 6) != 0 || memcmp(f->bytes + 10, W, 6) != 0 ||
        memcmp(f->bytes + 16, BCAST, 6) != 0)
    {
        return false;
    }
    return memcmp(f->bytes + HDR, want, wn) == 0 &&
           strcmp(eids_(f->bytes + HDR, f->len - HDR), "0,114,217") == 0;
}

/* The one-shot probe request still in the outbox from simnode_start, then a
 * periodic one: both in the new shape? */
static void probes_(bool *first_ok, bool *periodic_ok)
{
    *first_ok = probe_req_shape_(last_(FC_PROBE_REQ, BCAST));
    simnode_outbox_clear();
    const bool posted = umac_mesh_tx_broadcast_probe() >= 0;
    simnode_pump(); /* the event loop sends it */
    *periodic_ok = posted && probe_req_shape_(last_(FC_PROBE_REQ, BCAST));
}

static bool bytes_are_(const uint8_t *got, uint16_t n, const uint8_t *const *parts,
                       const size_t *lens, int count)
{
    uint8_t want[256];
    uint16_t wn = cat_(want, parts, lens, count);
    return n == wn && memcmp(got, want, wn) == 0;
}

/* Hand-overs from "the supplicant task", run by the TX-allocation hook. */
static void clear_rsn_(void) { umac_mesh_beacon_set_rsn(NULL, 0); }
static void give_rsn_(void) { umac_mesh_beacon_set_rsn(RSN, sizeof(RSN)); }

int main(void)
{
    printf("=== simnode discovery: RSN in beacon and probe response, Mesh ID in probe request ===\n");
    uint8_t b[512];
    uint16_t n;
    static const uint8_t SSID0[] = { 0x00, 0x00 };
    const uint8_t *const open_prsp[] = { RATES, MESHID_IE, CFG_OPEN };
    const size_t open_prsp_len[] = { sizeof(RATES), sizeof(MESHID_IE), sizeof(CFG_OPEN) };
    const uint8_t *const sae_prsp[] = { RATES, RSN, MESHID_IE, CFG_SAE };
    const size_t sae_prsp_len[] = { sizeof(RATES), sizeof(RSN), sizeof(MESHID_IE), sizeof(CFG_SAE) };
    const uint8_t *const sae_bcn[] = { SSID0, RSN, MESHID_IE, CFG_SAE };
    const size_t sae_bcn_len[] = { sizeof(SSID0), sizeof(RSN), sizeof(MESHID_IE), sizeof(CFG_SAE) };
    char open_eids[64], open_eids_after[64];

    /* ---- open mesh, before and after an RSN element is handed over -------- */
    const bool open_up = simnode_start(W);
    bool open_first, open_periodic;
    probes_(&open_first, &open_periodic);
    const uint16_t open_bcn = our_beacon_(b, sizeof(b));
    const uint8_t open_fc = open_bcn > 1u ? b[1] : 0xffu;
    snprintf(open_eids, sizeof(open_eids), "%s", eids_(b + S1G_BCN_IES, open_bcn - S1G_BCN_IES));
    n = prsp_ies_(P, b, sizeof(b));
    const bool open_prsp_ok = bytes_are_(b, n, open_prsp, open_prsp_len, 3);
    umac_mesh_beacon_set_rsn(RSN, sizeof(RSN));
    n = our_beacon_(b, sizeof(b));
    const uint8_t open_fc_after = n > 1u ? b[1] : 0xffu;
    snprintf(open_eids_after, sizeof(open_eids_after), "%s", eids_(b + S1G_BCN_IES, n - S1G_BCN_IES));
    n = prsp_ies_(P, b, sizeof(b));
    const bool open_prsp_after_ok = bytes_are_(b, n, open_prsp, open_prsp_len, 3);

    /* ---- SAE -------------------------------------------------------------- */
    const bool sae_up = simnode_start_sae(W);
    bool sae_first, sae_periodic;
    probes_(&sae_first, &sae_periodic);
    CHECK(open_up && sae_up && open_first && sae_first,
          "open and SAE: the probe request sent at mesh start is SSID(0), Mesh ID, S1G Capabilities");
    CHECK(open_periodic && sae_periodic, "open and SAE: so is every periodic one");

    n = our_beacon_(b, sizeof(b));
    const bool bare = n == open_bcn && find_(b + S1G_BCN_IES, n - S1G_BCN_IES, 48) == NULL;
    umac_mesh_beacon_set_rsn(RSN, sizeof(RSN));
    n = our_beacon_(b, sizeof(b));
    CHECK(bare && n == open_bcn + sizeof(RSN),
          "SAE: no RSN before the shim hands it over, then the beacon grows by exactly its %u bytes "
          "(%u -> %u)", (unsigned)sizeof(RSN), (unsigned)open_bcn, (unsigned)n);
    CHECK(n > 1 && b[0] == 0x1c && b[1] == 0x40 && open_fc == 0x00 && open_fc_after == 0x00,
          "beacon frame control 0x401c under SAE -- Security Supported (B14) -- and 0x001c on an "
          "open mesh, before and after a hand-over (got %02x%02x)", b[1], b[0]);
    CHECK(strcmp(eids_(b + S1G_BCN_IES, n - S1G_BCN_IES), "0,48,114,113,213,217,232,214") == 0 &&
          strcmp(open_eids, "0,114,113,213,217,232,214") == 0 && strcmp(open_eids, open_eids_after) == 0,
          "beacon elements: SSID, RSN, Mesh ID, Mesh Config, S1G x4 under SAE (%s); no RSN on an "
          "open mesh, even after a hand-over (%s)", eids_(b + S1G_BCN_IES, n - S1G_BCN_IES), open_eids_after);
    const uint16_t pre = (uint16_t)(sizeof(SSID0) + sizeof(RSN) + sizeof(MESHID_IE) + sizeof(CFG_SAE));
    CHECK(n >= S1G_BCN_IES + pre && memcmp(b + 4, W, 6) == 0 &&
          bytes_are_(b + S1G_BCN_IES, pre, sae_bcn, sae_bcn_len, 4),
          "SAE: beacon bytes after the header are SSID(0), hostap's RSN verbatim, Mesh ID, "
          "Mesh Config with auth 1");

    n = prsp_ies_(P, b, sizeof(b));
    CHECK(bytes_are_(b, n, sae_prsp, sae_prsp_len, 4) && open_prsp_ok && open_prsp_after_ok,
          "probe response IEs: rates, hostap's RSN verbatim, Mesh ID, Mesh Config under SAE (%s); "
          "no RSN on an open mesh, before or after a hand-over", eids_(b, n));

    /* Bytes that are not an RSN element (id 49) clear it rather than go on air. */
    uint8_t bad[sizeof(RSN)];
    memcpy(bad, RSN, sizeof(RSN));
    bad[0] = 0x31;
    const bool had = our_beacon_(b, sizeof(b)) == open_bcn + sizeof(RSN);
    umac_mesh_beacon_set_rsn(bad, sizeof(bad));
    n = our_beacon_(b, sizeof(b));
    CHECK(had && n == open_bcn && b[1] == 0x00 && find_(b + S1G_BCN_IES, n - S1G_BCN_IES, 49) == NULL,
          "SAE: a non-RSN element handed over clears the RSN element and is not sent itself");

    /* A new start forgets it until the shim hands it over again. */
    umac_mesh_beacon_set_rsn(RSN, sizeof(RSN));
    const bool restarted = simnode_start_sae(W);
    n = our_beacon_(b, sizeof(b));
    const bool forgot = n == open_bcn && prsp_ies_(P, b, sizeof(b)) == sizeof(RATES) +
                        sizeof(MESHID_IE) + sizeof(CFG_SAE);
    umac_mesh_beacon_set_rsn(RSN, sizeof(RSN));
    n = our_beacon_(b, sizeof(b));
    CHECK(restarted && forgot && n == open_bcn + sizeof(RSN),
          "SAE: a restart drops the previous RSN from beacon and probe response; the next hand-over restores it");

    /* Another warthog hears our probe request: it answers, and offers us to
     * hostap because the Mesh ID ELEMENT names its mesh -- the SSID is empty. */
    simnode_stub_reset();
    simnode_outbox_clear();
    const bool posted = umac_mesh_tx_broadcast_probe() >= 0;
    simnode_pump(); /* the event loop sends it */
    const struct simnode_frame *ours = posted ? last_(FC_PROBE_REQ, BCAST) : NULL;
    uint8_t heard[512];
    uint16_t hn = 0;
    if (ours != NULL)
    {
        hn = ours->len;
        memcpy(heard, ours->bytes, hn);
        memcpy(heard + 10, Q, 6); /* as if Q had sent it */
    }
    const uint8_t *ssid = find_(heard + HDR, hn > HDR ? hn - HDR : 0, 0);
    const bool empty_ssid = hn > HDR && ssid != NULL && ssid[1] == 0;
    simnode_outbox_clear();
    CHECK(empty_ssid && simnode_rx(heard, hn, -60) && last_(FC_PROBE_RSP, Q) != NULL,
          "SAE: a warthog probe request with an empty SSID is still answered");
    uint8_t who[6] = { 0 };
    const uint8_t *offered = NULL;
    size_t on = simnode_last_new_peer(who, &offered);
    CHECK(empty_ssid && simnode_stub_hits("umac_supp_mesh_new_peer") == 1 && memcmp(who, Q, 6) == 0 &&
          on > 0,
          "SAE: and its sender is offered to hostap, named by the Mesh ID element alone");

    /* ---- a hand-over racing the beacon's two build passes ----------------- */
    static const char SAE_EIDS[] = "0,48,114,113,213,217,232,214";
    static const char BARE_EIDS[] = "0,114,113,213,217,232,214";
    const bool race_up = simnode_start_sae(W);
    give_rsn_();
    simnode_set_tx_alloc_hook(clear_rsn_);
    n = our_beacon_(b, sizeof(b));
    const bool whole = n == open_bcn + sizeof(RSN) && b[1] == 0x40 &&
                       strcmp(eids_(b + S1G_BCN_IES, n - S1G_BCN_IES), SAE_EIDS) == 0;
    n = our_beacon_(b, sizeof(b));
    CHECK(race_up && whole && n == open_bcn && b[1] == 0x00,
          "SAE: an RSN clear between the beacon's size and fill passes leaves that beacon whole; "
          "the next has no RSN");
    simnode_set_tx_alloc_hook(give_rsn_);
    n = our_beacon_(b, sizeof(b));
    const bool stayed_bare = n == open_bcn && b[1] == 0x00 &&
                             strcmp(eids_(b + S1G_BCN_IES, n - S1G_BCN_IES), BARE_EIDS) == 0;
    n = our_beacon_(b, sizeof(b));
    CHECK(stayed_bare && n == open_bcn + sizeof(RSN) && b[1] == 0x40,
          "SAE: a hand-over between the passes does not grow that beacon; the next carries it");

    /* ---- a hand-over longer than the store -------------------------------- */
    uint8_t big[UMAC_MESH_IES_RSN_MAXLEN + 2];
    memset(big, 0xee, sizeof(big));
    big[0] = UMAC_MESH_EID_RSN;
    big[1] = (uint8_t)(sizeof(big) - 2u);
    give_rsn_();
    umac_mesh_beacon_set_rsn(big, (uint16_t)sizeof(big));
    n = our_beacon_(b, sizeof(b));
    CHECK(n == open_bcn && b[1] == 0x00 && prsp_ies_(P, b, sizeof(b)) == sizeof(RATES) +
          sizeof(MESHID_IE) + sizeof(CFG_SAE),
          "SAE: a well-formed %u-byte RSN element is refused and clears the stored one "
          "(beacon and probe response)", (unsigned)sizeof(big));

    /* ---- the longest Mesh ID under SAE ------------------------------------ */
    uint8_t id[UMAC_MESH_IES_MESH_ID_MAXLEN];
    memset(id, 'L', sizeof(id));
    uint8_t long_id_ie[2 + sizeof(id)] = { 0x72, (uint8_t)sizeof(id) };
    memcpy(long_id_ie + 2, id, sizeof(id));
    simnode_set_mesh_id(id, sizeof(id));
    s_id = id;
    s_id_len = sizeof(id);
    const bool long_up = simnode_start_sae(W);
    bool long_first, long_periodic;
    probes_(&long_first, &long_periodic);
    CHECK(long_up && long_first && long_periodic,
          "SAE, 32-byte Mesh ID: the probe requests at mesh start and after are SSID(0), "
          "Mesh ID(32), S1G Capabilities");
    give_rsn_();
    const uint8_t *const long_prsp[] = { RATES, RSN, long_id_ie, CFG_SAE };
    const size_t long_prsp_len[] = { sizeof(RATES), sizeof(RSN), sizeof(long_id_ie), sizeof(CFG_SAE) };
    n = prsp_ies_(P, b, sizeof(b));
    CHECK(bytes_are_(b, n, long_prsp, long_prsp_len, 4),
          "SAE, 32-byte Mesh ID: the probe response carries rates, RSN, Mesh ID(32), Mesh Config "
          "(%u IE bytes)", (unsigned)n);
    const uint8_t *const long_bcn[] = { SSID0, RSN, long_id_ie, CFG_SAE };
    const size_t long_bcn_len[] = { sizeof(SSID0), sizeof(RSN), sizeof(long_id_ie), sizeof(CFG_SAE) };
    const uint16_t long_pre = (uint16_t)(sizeof(SSID0) + sizeof(RSN) + sizeof(long_id_ie) + sizeof(CFG_SAE));
    n = our_beacon_(b, sizeof(b));
    CHECK(n == open_bcn + sizeof(RSN) + sizeof(long_id_ie) - sizeof(MESHID_IE) &&
          strcmp(eids_(b + S1G_BCN_IES, n - S1G_BCN_IES), SAE_EIDS) == 0 &&
          bytes_are_(b + S1G_BCN_IES, long_pre, long_bcn, long_bcn_len, 4),
          "SAE, 32-byte Mesh ID: the beacon is SSID(0), RSN, Mesh ID(32), Mesh Config, S1G x4 "
          "(%u bytes)", (unsigned)n);
    simnode_set_mesh_id(NULL, 0);

    simnode_stop();

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_simnode_discovery: all passed\n");
    return 0;
}
