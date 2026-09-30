/*
 * Peer capacity: what a node says and does at and around a full peer table.
 *
 * The datapath holds four stations. Every Mesh Configuration element we emit
 * (beacon, probe response, MPM frame) carries "Accepting Additional Mesh
 * Peerings" (capability bit 0) and Formation Info's Number of Peerings (bits
 * 1..6). mac80211 raises NEW_PEER_CANDIDATE, and auto-opens, only on a 1
 * (net/mac80211/mesh_plink.c:549-563 and :635-640, v6.6), so the bit must read
 * 0 exactly while no slot is free, and a peer already holding a slot must
 * still be told 1 or it can never come back after losing its half of the link.
 *
 * What each section pins:
 *  1. OPEN: the bit and Number of Peerings follow the datapath in the beacon,
 *     probe responses and Confirms; an in-progress peer holding the last MPM
 *     link is still told 1 in our Open (a wpa_supplicant peer would otherwise
 *     drop it as "crowded"); a slot-holder's probe response says 1 when full;
 *     while full, each established peer's own S1G beacon earns it one probe
 *     response saying 1, at most once per 10 s per link (none at 9.999 s, one
 *     at 10.000 s), and none when not full; a fifth Open is still answered
 *     Close(53).
 *  2. OPEN: we do not Open toward a neighbour whose Mesh Configuration clears
 *     its accepting bit or names another auth protocol, from an S1G beacon or
 *     a legacy beacon (whose fixed fields carry a live TSF, so the element
 *     walk must start after them), and its beacons give it none of our MPM
 *     links -- so with three peers up they cannot take the last link from a
 *     fourth that would accept us. Holding no link, each of its S1G beacons
 *     would be a first sighting: it is answered at most once per 10 s per
 *     address, and answered and opened at once when it starts accepting.
 *  3. OPEN, full: a slot-holder that rebooted (fresh llid) is re-peered and our
 *     Open and Confirm to it say 1; a fifth Open meanwhile still gets
 *     Close(53); once it is back the beacon says 0 again.
 *  4. OPEN: a Confirm we cannot give a station (datapath full behind the MPM
 *     table's back, standing in for NO_MEM) is refused with Close(53) citing
 *     both link ids -- the peer's llid taken from its Confirm when its Open
 *     never reached us -- counted in close_tx; ESTAB is not latched and the
 *     link is released.
 *  5. OPEN: after Close(53) we do not re-open toward that peer for 30 s (still
 *     held at 29 s, open again after 30 s); the peer's own Open is answered
 *     and clears the hold-off; Close(52), (55), (56) and (57) -- mac80211's
 *     ordinary churn -- do not hold off.
 *  6. OPEN: a probe request still draws an Open toward an SAE neighbour, and
 *     its beacon repeats it, so it holds the last MPM link while our beacon
 *     says 1; a fourth peer's Open then takes that link (Confirm, not
 *     Close(53), no no_slot), the SAE neighbour is sent Close(52) citing our
 *     llid, and once full its probe draws nothing. A link whose peer sent us
 *     its llid is not taken: the fourth's Open gets Close(53).
 *  7. OPEN: 9 and 12 neighbours we will not open toward, beaconing 1/s for
 *     30 s: none answered twice within 10 s, at most 8 answers per 10 s. With
 *     8 stamps live a ninth is not answered, an accepting neighbour is still
 *     answered and opened, and the ninth is answered once a stamp lapses.
 *  8. OPEN: 5 and 8 full neighbours probing every 2 s for 28 s draw 4 Opens
 *     in all, none within 30 s of that neighbour's Close(53); a stranger's own
 *     Open is still answered. With 4 hold-offs live nobody new is opened, and
 *     a Close(53) that no entry can hold is held off its full 30 s (not at
 *     29.999 s, even after the older three lapse; opened at 30 s).
 *  9. SAE: while full, a new candidate reaches hostap from none of the three
 *     discovery paths (probe response, probe request, S1G beacon carrying
 *     accepting=1 so only OUR capacity can explain it), each refusal counted
 *     in g_warthog_sae_offer_full; a slot-holder is still offered; a
 *     slot-holder's S1G beacon earns one probe response saying 1 and a
 *     stranger's one saying 0; the synthetic IEs offered for a slot-holder's
 *     probe request say 1; a candidate that has not finished AMPE does not
 *     count in Number of Peerings, a keyed one does -- in whichever slot it
 *     sits, the fourth included, and only while that station is there; a
 *     freed slot re-admits.
 * 10. SAE: five neighbours hostap adds and whose SAE never completes. With a
 *     keyed peer K in one slot, three take the rest and G finds the table
 *     full; the first mesh_auth_timer failure (umac_mesh_sae_failed) frees each
 *     slot while K keeps its slot and key; the failed three are held off on all
 *     three discovery paths, counted in `held`; the other two and a genuine G
 *     then get slots and G keys; with all five failed and beaconing every
 *     second for 17 s none is re-offered and a second genuine neighbour is;
 *     each is re-offered at exactly 30 s (not 1 ms before); a held-off
 *     neighbour's own SAE Commit ends its hold-off and offers it at once with
 *     SAE IEs saying accepting, while its Confirm, or a Commit from a
 *     neighbour never held, offers nothing.
 * 11. The candidate RSSI floor, default -80 dBm. SAE: a neighbour at -80, -85
 *     or -95 dBm is offered from none of the three discovery paths, each
 *     refusal counted, and at -79 it is; a weak neighbour's S1G beacons are
 *     answered once per 10 s; a slot-holder is not floored; a held-off
 *     neighbour's Commit below the floor ends the hold-off but is not offered;
 *     0 and -255 turn the floor off, even for -255 dBm; a floor of -90 admits
 *     -89 and not -90. OPEN: a probe request naming our mesh is counted (the
 *     no-peers diagnosis reads it); at -81 dBm an S1G beacon and a legacy
 *     beacon draw no Open and no link, each counted, while a probe request
 *     draws one uncounted (two weak warthogs peer through their probes); the
 *     S1G beacons are answered once per 10 s and never opened; a weak
 *     neighbour's own Open is still answered; a handshake started at -70 is
 *     still retransmitted after the signal drops to -90; a scanner's wildcard
 *     probe requests, weak or strong, count nowhere and each still draws an Open.
 * 12. SAE: an Open System Authentication (seq 1) from a held-off neighbour
 *     neither offers it nor ends its hold-off; umac_mesh_sae_failed(NULL) frees
 *     nothing; floors of -1 and -254 are on at their own value and admit the
 *     next dBm up. A peering that failed after SAE (hostap's FSM restarting
 *     before ESTAB, umac_mesh_plink_failed) frees the slot, is counted apart
 *     from SAE failures, is held off on all three discovery paths until exactly
 *     30 s, and its own Commit ends that; NULL frees nothing. The floor counters
 *     take only frames naming our mesh: a probe response naming another mesh
 *     and a held-off neighbour's Commit count neither way, while probe
 *     responses naming ours count each way.
 * 13. SAE: hostap creates a station from an Open only on a cached PMKSA, and only
 *     for a sender it holds no station for, so an Open from a neighbour with no
 *     slot is refused before hostap while it is held off (plink_fail or sae_fail,
 *     counted in `held`, until exactly 30 s) or heard at or below the floor
 *     (counted in `skipped`; one above it reaches hostap uncounted); its Confirm
 *     and Close still reach hostap, and a slot-holder's Open is not floored.
 *
 * Everything goes through simnode_rx(), the real RX path; our frames are read
 * back from the outbox. hostap is stubbed, so the SAE assertions are on
 * whether a candidate reaches umac_supp_mesh_new_peer() and with which IEs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "simnode.h"
#include "mmpkt.h"
#include "umac_mesh_ies.h"
#include "umac_mesh_plink_tbl.h"

struct umac_data;
struct umac_data *umac_data_get_umacd(void);
struct mmpkt *umac_mesh_get_beacon(struct umac_data *umacd);
void umac_mesh_reset_links(void);
void umac_datapath_mesh_del_peer(const uint8_t *peer_addr);
uint8_t umac_datapath_mesh_peer_count(void);

extern volatile uint32_t g_warthog_mpm_estab, g_warthog_mpm_close_tx, g_warthog_mpm_no_slot;
extern volatile uint32_t g_warthog_mesh_peer_add_fail;
extern volatile uint32_t g_warthog_sae_offer_full;
extern volatile char g_warthog_mpm_links[256];
extern volatile uint32_t g_warthog_sae_fail, g_warthog_sae_offer_held;
extern volatile int32_t g_warthog_mesh_rssi_floor;
extern volatile uint32_t g_warthog_mesh_rssi_skip, g_warthog_mesh_rssi_pass;
extern volatile uint32_t g_warthog_prq_named;
/* What hostap's mesh_auth_timer calls when SAE with a station times out. */
void umac_mesh_sae_failed(const uint8_t *addr);
/* What hostap's mesh_mpm_fsm_restart calls for a station that never reached ESTAB. */
void umac_mesh_plink_failed(const uint8_t *addr);
extern volatile uint32_t g_warthog_plink_fail;

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static const uint8_t W[6] = { 0x02, 0, 0, 0, 0, 0x01 }; /* us */
static const uint8_t A[6] = { 0x02, 0, 0, 0, 0, 0x0a };
static const uint8_t B[6] = { 0x02, 0, 0, 0, 0, 0x0b };
static const uint8_t C[6] = { 0x02, 0, 0, 0, 0, 0x0c };
static const uint8_t D[6] = { 0x02, 0, 0, 0, 0, 0x0d };
static const uint8_t E[6] = { 0x02, 0, 0, 0, 0, 0x0e };
static const uint8_t F[6] = { 0x02, 0, 0, 0, 0, 0x0f };
static const uint8_t G[6] = { 0x02, 0, 0, 0, 0, 0x10 };
static const uint8_t Y[6] = { 0x02, 0, 0, 0, 0, 0x19 }; /* an SAE warthog: probes, never answers */
static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t MESHID[] = { 's', 'i', 'm', 'n', 'o', 'd', 'e' };
static const uint8_t K_MTK[16] = { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                   0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f };

#define HDR 24u
#define FIXED 12u /* timestamp, beacon interval, capability */
#define FC_ACTION 0xd0u
#define FC_PROBE_REQ 0x40u
#define FC_PROBE_RSP 0x50u
#define FC_BEACON 0x80u
#define EID_MESH_CONFIG 113u
#define CFG_AUTH 4 /* octets of the Mesh Configuration payload */
#define CFG_FORM 5
#define CFG_CAP 6
#define AUTH_NONE 0x00u
#define AUTH_SAE 0x01u

/* One of many neighbours, told apart by its last octet. */
static void nbr_(uint8_t out[6], uint8_t id)
{
    static const uint8_t base[6] = { 0x02, 0, 0, 0, 0x01, 0 };
    memcpy(out, base, 6);
    out[5] = id;
}

/* ---- frames in ---------------------------------------------------------- */

/* The RSSI every injected frame arrives at: above the -80 dBm default floor
 * unless a test lowers it. */
static int16_t s_rssi = -60;

static bool rx_mgmt_da(uint8_t fc0, const uint8_t *da, const uint8_t *sa, const uint8_t *body,
                       uint16_t n)
{
    uint8_t f[512];
    if ((uint32_t)n + HDR > sizeof(f)) { return false; }
    memset(f, 0, HDR);
    f[0] = fc0;
    memcpy(f + 4, da, 6);
    memcpy(f + 10, sa, 6);
    memcpy(f + 16, sa, 6);
    memcpy(f + HDR, body, n);
    return simnode_rx(f, (uint16_t)(HDR + n), s_rssi);
}

static bool rx_mgmt(uint8_t fc0, const uint8_t *sa, const uint8_t *body, uint16_t n)
{
    return rx_mgmt_da(fc0, W, sa, body, n);
}

static bool rx_mpm(const uint8_t *sa, uint8_t action, uint16_t llid, uint16_t plid, uint16_t reason,
                   uint16_t aid)
{
    uint8_t body[UMAC_MESH_MPM_BODY_MAXLEN];
    uint16_t n = umac_mesh_ies_build_mpm_body(body, sizeof(body), action, llid, plid, reason, aid,
                                              MESHID, sizeof(MESHID), false, NULL, 0);
    return n != 0 && rx_mgmt(FC_ACTION, sa, body, n);
}

/* A probe request naming our mesh: answered with a probe response; on an open
 * mesh it also drives our Open, under SAE the beaconless-peer offer. */
static bool prod_(const uint8_t *sa)
{
    uint8_t ies[2 + sizeof(MESHID)] = { 0, (uint8_t)sizeof(MESHID) };
    memcpy(ies + 2, MESHID, sizeof(MESHID));
    return rx_mgmt(FC_PROBE_REQ, sa, ies, (uint16_t)sizeof(ies));
}

#define FC_AUTH 0xb0u

/* An Authentication frame with algorithm @p alg and sequence @p seq, status 0. */
static bool rx_auth(const uint8_t *sa, uint16_t alg, uint16_t seq)
{
    uint8_t body[6] = { (uint8_t)alg, (uint8_t)(alg >> 8), (uint8_t)seq, (uint8_t)(seq >> 8), 0, 0 };
    return rx_mgmt(FC_AUTH, sa, body, (uint16_t)sizeof(body));
}

/* An SAE Authentication frame (algorithm 3) from @p sa: seq 1 Commit, 2 Confirm. */
static bool rx_sae_auth(const uint8_t *sa, uint16_t seq)
{
    uint8_t body[8 + 32] = { 3, 0, (uint8_t)seq, 0, 0, 0, 19, 0 }; /* status 0, group 19 */
    for (unsigned i = 8; i < sizeof(body); i++) { body[i] = (uint8_t)(0xa0 + i); }
    return rx_mgmt(FC_AUTH, sa, body, (uint16_t)sizeof(body));
}

/* The Mesh Configuration payload in [p, p+n), or NULL. */
static uint8_t *meshcfg_in(uint8_t *p, uint32_t n)
{
    uint32_t off = 0;
    while (off + 2u <= n)
    {
        uint8_t eid = p[off], len = p[off + 1];
        if (off + 2u + len > n) { return NULL; }
        if (eid == EID_MESH_CONFIG && len >= 7u) { return p + off + 2u; }
        off += 2u + len;
    }
    return NULL;
}

/* A peer's own discovery IEs, with its capability and auth octets set here
 * rather than taken from our builder. */
static uint16_t peer_ies(uint8_t *out, uint16_t cap, uint8_t cfg_cap, uint8_t auth)
{
    uint16_t n = umac_mesh_ies_build_discovery(out, cap, MESHID, sizeof(MESHID), auth == AUTH_SAE);
    uint8_t *cfg = meshcfg_in(out, n);
    if (cfg == NULL) { return 0; }
    cfg[CFG_AUTH] = auth;
    cfg[CFG_FORM] = 0;
    cfg[CFG_CAP] = cfg_cap;
    return n;
}

/* A peer's probe response: the SAE discovery path for warthog peers. */
static bool rx_probe_rsp(const uint8_t *sa, uint8_t cfg_cap)
{
    uint8_t body[FIXED + UMAC_MESH_DISCOVERY_IES_MAXLEN];
    memset(body, 0, FIXED);
    uint16_t n = peer_ies(body + FIXED, UMAC_MESH_DISCOVERY_IES_MAXLEN, cfg_cap, AUTH_SAE);
    return n != 0 && rx_mgmt(FC_PROBE_RSP, sa, body, (uint16_t)(FIXED + n));
}

/* A probe response from @p sa for the mesh @p id: another mesh's, when it is not ours. */
static bool rx_probe_rsp_id(const uint8_t *sa, const uint8_t *id, uint8_t id_len)
{
    uint8_t body[FIXED + UMAC_MESH_DISCOVERY_IES_MAXLEN];
    memset(body, 0, FIXED);
    uint16_t n = umac_mesh_ies_build_discovery(body + FIXED, UMAC_MESH_DISCOVERY_IES_MAXLEN, id,
                                               id_len, true);
    return n != 0 && rx_mgmt(FC_PROBE_RSP, sa, body, (uint16_t)(FIXED + n));
}

/* A wildcard probe request (empty SSID): a scanner, naming no mesh. */
static bool rx_wildcard_probe(const uint8_t *sa)
{
    static const uint8_t ies[2] = { 0, 0 };
    return rx_mgmt(FC_PROBE_REQ, sa, ies, (uint16_t)sizeof(ies));
}

/* A legacy (PV0) mesh beacon from @p sa. Its fixed fields carry a live TSF and
 * a 100 TU interval: zeroes there would parse as six empty elements and hide
 * an element walk that starts 12 octets early. */
static bool rx_legacy_beacon(const uint8_t *sa, uint8_t cfg_cap, uint8_t auth)
{
    static const uint8_t fixed[FIXED] = { 0xef, 0xcd, 0xab, 0x89, 0x67, 0x45,
                                          0x23, 0x01, 0x64, 0x00, 0x00, 0x00 };
    uint8_t body[FIXED + UMAC_MESH_DISCOVERY_IES_MAXLEN];
    memcpy(body, fixed, FIXED);
    uint16_t n = peer_ies(body + FIXED, UMAC_MESH_DISCOVERY_IES_MAXLEN, cfg_cap, auth);
    return n != 0 && rx_mgmt_da(FC_BEACON, BCAST, sa, body, (uint16_t)(FIXED + n));
}

/* Our beacon template, as the chip would fetch it. Returns its length. */
static uint16_t our_beacon(uint8_t *out, uint16_t cap)
{
    struct mmpkt *b = umac_mesh_get_beacon(umac_data_get_umacd());
    if (b == NULL) { return 0; }
    struct mmpktview *v = mmpkt_open(b);
    uint32_t n = mmpkt_get_data_length(v);
    uint16_t got = 0;
    if (n <= cap)
    {
        memcpy(out, mmpkt_get_data_start(v), n);
        got = (uint16_t)n;
    }
    mmpkt_close(&v);
    mmpkt_release(b);
    return got;
}

static uint8_t *beacon_cfg(uint8_t *bcn, uint16_t n)
{
    if (n < 2u) { return NULL; }
    uint32_t off = umac_mesh_ies_s1g_beacon_ie_offset((uint16_t)(bcn[0] | (bcn[1] << 8)));
    return off < n ? meshcfg_in(bcn + off, n - off) : NULL;
}

/* One octet of our beacon's Mesh Configuration, or -1. */
static int beacon_octet(int which)
{
    uint8_t bcn[512];
    uint16_t n = our_beacon(bcn, sizeof(bcn));
    uint8_t *cfg = beacon_cfg(bcn, n);
    return cfg ? cfg[which] : -1;
}

/* An S1G beacon from @p sa: our own template re-addressed, carrying the
 * neighbour's capability and auth octets. */
static bool rx_s1g_beacon(const uint8_t *sa, uint8_t cfg_cap, uint8_t auth)
{
    uint8_t bcn[512];
    uint16_t n = our_beacon(bcn, sizeof(bcn));
    uint8_t *cfg = beacon_cfg(bcn, n);
    if (cfg == NULL || n < 10u) { return false; }
    memcpy(bcn + 4, sa, 6); /* S1G beacon SA */
    cfg[CFG_AUTH] = auth;
    cfg[CFG_FORM] = 0;
    cfg[CFG_CAP] = cfg_cap;
    return simnode_rx(bcn, n, s_rssi);
}

/* ---- frames out --------------------------------------------------------- */

static bool frame_is(const struct simnode_frame *f, uint8_t fc0, const uint8_t *da, int action)
{
    if (f->len < HDR || f->bytes[0] != fc0 || memcmp(f->bytes + 4, da, 6) != 0) { return false; }
    return action < 0 || (f->len >= HDR + 2u && f->bytes[HDR] == 15 && f->bytes[HDR + 1] == action);
}

static const struct simnode_frame *last_to(uint8_t fc0, const uint8_t *da, int action)
{
    const struct simnode_frame *hit = NULL;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        if (frame_is(simnode_outbox_get(i), fc0, da, action)) { hit = simnode_outbox_get(i); }
    }
    return hit;
}

static unsigned count_to(uint8_t fc0, const uint8_t *da, int action)
{
    unsigned c = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        c += frame_is(simnode_outbox_get(i), fc0, da, action);
    }
    return c;
}

/* One octet of the Mesh Configuration in an outbox frame whose IEs start at @p ie_off. */
static int frame_octet(const struct simnode_frame *f, uint32_t ie_off, int which)
{
    static uint8_t copy[512];
    if (f == NULL || f->len <= ie_off) { return -1; }
    memcpy(copy, f->bytes, f->len);
    uint8_t *cfg = meshcfg_in(copy + ie_off, f->len - ie_off);
    return cfg ? cfg[which] : -1;
}

#define PRSP_IES (HDR + FIXED)
#define OPEN_IES (HDR + 4u)    /* category, action, capability */
#define CONFIRM_IES (HDR + 6u) /* ... and AID */

/* Capability octet of our probe response to @p to, provoked by its probe request. */
static int prsp_cap(const uint8_t *to)
{
    simnode_outbox_clear();
    prod_(to);
    return frame_octet(last_to(FC_PROBE_RSP, to, -1), PRSP_IES, CFG_CAP);
}

/* The peer's side of an open handshake; returns our llid for the link. */
static uint16_t peer_up(const uint8_t *peer, uint16_t their)
{
    simnode_outbox_clear();
    rx_mpm(peer, UMAC_MESH_MPM_ACTION_OPEN, their, 0, 0, 0);
    const struct simnode_frame *cf = last_to(FC_ACTION, peer, UMAC_MESH_MPM_ACTION_CONFIRM);
    uint16_t ours = 0;
    if (cf == NULL || !umac_mesh_ies_get_peer_llid(cf->bytes + HDR, cf->len - HDR, &ours)) { return 0; }
    rx_mpm(peer, UMAC_MESH_MPM_ACTION_CONFIRM, their, ours, 0, 1);
    return ours;
}

/* Our llid in the last Open we sent @p to, or 0. */
static uint16_t our_open_llid(const uint8_t *to)
{
    const struct simnode_frame *op = last_to(FC_ACTION, to, UMAC_MESH_MPM_ACTION_OPEN);
    uint16_t ours = 0;
    if (op == NULL || !umac_mesh_ies_get_peer_llid(op->bytes + HDR, op->len - HDR, &ours)) { return 0; }
    return ours;
}

static void reset_node(void)
{
    umac_datapath_mesh_del_peer(NULL);
    umac_mesh_reset_links();
    simnode_outbox_clear();
}

/* Beacons from @p n established peers; how many got a probe response, and how
 * many of those said accepting. */
static void beacons_from(const uint8_t *const *peers, int n, unsigned *answered, unsigned *said_1)
{
    simnode_outbox_clear();
    for (int i = 0; i < n; i++) { rx_s1g_beacon(peers[i], 0x01, AUTH_NONE); }
    *answered = 0;
    *said_1 = 0;
    for (int i = 0; i < n; i++)
    {
        unsigned c = count_to(FC_PROBE_RSP, peers[i], -1);
        *answered += c;
        *said_1 += (c == 1 &&
                    (frame_octet(last_to(FC_PROBE_RSP, peers[i], -1), PRSP_IES, CFG_CAP) & 1) == 1);
    }
}

int main(void)
{
    printf("=== simnode capacity: a full peer table, on the wire ===\n");

    /* ---- 1. OPEN: the bit and Number of Peerings track the datapath ----- */
    printf("--- 1. open mesh: the accepting bit tracks the datapath ---\n");
    CHECK(simnode_start(W), "node starts on an open mesh");
    simnode_set_gates(false, false, false, false);
    reset_node();
    simnode_set_time_ms(1000);
    CHECK((prsp_cap(E) & 1) == 1, "empty table: probe response says accepting");
    reset_node();
    CHECK(peer_up(A, 0x100) && peer_up(B, 0x200) && peer_up(C, 0x300), "A, B, C reach ESTAB");
    CHECK(umac_datapath_mesh_peer_count() == 3, "three datapath peers");
    CHECK((prsp_cap(E) & 1) == 1, "3 of 4: probe response to a stranger says accepting");
    rx_mpm(E, UMAC_MESH_MPM_ACTION_CLOSE, 0, our_open_llid(E), 52, 0); /* E's prodded link goes */
    CHECK((beacon_octet(CFG_CAP) & 1) == 1, "3 of 4: beacon says accepting");
    CHECK(beacon_octet(CFG_FORM) == 0x06, "3 of 4: beacon Formation Info = 3 << 1 (got 0x%02x)",
          beacon_octet(CFG_FORM));

    /* D in progress: the MPM table is full (A, B, C, D), the datapath is not. */
    simnode_outbox_clear();
    rx_mpm(D, UMAC_MESH_MPM_ACTION_OPEN, 0x400, 0, 0, 0);
    CHECK((frame_octet(last_to(FC_ACTION, D, UMAC_MESH_MPM_ACTION_OPEN), OPEN_IES, CFG_CAP) & 1) == 1,
          "an in-progress peer holding the last MPM link is told accepting in our Open");
    const struct simnode_frame *cf = last_to(FC_ACTION, D, UMAC_MESH_MPM_ACTION_CONFIRM);
    uint16_t ours_d = 0;
    CHECK(cf != NULL && umac_mesh_ies_get_peer_llid(cf->bytes + HDR, cf->len - HDR, &ours_d),
          "and answered with a Confirm");
    rx_mpm(D, UMAC_MESH_MPM_ACTION_CONFIRM, 0x400, ours_d, 0, 1);
    CHECK(umac_datapath_mesh_peer_count() == 4, "D reaches ESTAB: the table is full");

    CHECK((prsp_cap(E) & 1) == 0, "full: probe response to a stranger clears accepting");
    CHECK(beacon_octet(CFG_CAP) >= 0 && (beacon_octet(CFG_CAP) & 1) == 0,
          "full: the beacon clears it too");
    CHECK(beacon_octet(CFG_FORM) == 0x08, "full: beacon Formation Info = 4 << 1 (got 0x%02x)",
          beacon_octet(CFG_FORM));
    CHECK((prsp_cap(A) & 1) == 1, "full: a probe response to a slot-holder still says accepting");
    simnode_outbox_clear();
    rx_mpm(A, UMAC_MESH_MPM_ACTION_OPEN, 0x100, 0, 0, 0); /* A retransmits its Open */
    CHECK(frame_octet(last_to(FC_ACTION, A, UMAC_MESH_MPM_ACTION_CONFIRM), CONFIRM_IES, CFG_CAP) == 0x00,
          "full: a Confirm to an established peer carries 0, as mac80211's does");

    /* Full, so a peer that lost its half of a link would never raise us again:
     * each established peer's own beacon earns it a "we accept you". */
    const uint8_t *est[4] = { A, B, C, D };
    unsigned answered = 0, said_1 = 0;
    beacons_from(est, 4, &answered, &said_1);
    CHECK(answered == 4 && said_1 == 4,
          "full: each established peer's beacon gets one probe response saying accepting (%u/%u)",
          answered, said_1);
    simnode_advance_ms(2000);
    beacons_from(est, 4, &answered, &said_1);
    CHECK(answered == 0, "rate-limited: none 2 s later (%u)", answered);
    simnode_advance_ms(7999);
    beacons_from(est, 4, &answered, &said_1);
    CHECK(answered == 0, "nor at 9.999 s (%u)", answered);
    simnode_advance_ms(1);
    beacons_from(est, 4, &answered, &said_1);
    CHECK(answered == 4 && said_1 == 4, "and again at exactly 10 s (%u/%u)", answered, said_1);

    /* A fifth that ignores the bit is still refused on the wire. */
    simnode_outbox_clear();
    uint32_t ct0 = g_warthog_mpm_close_tx;
    rx_mpm(E, UMAC_MESH_MPM_ACTION_OPEN, 0x500, 0, 0, 0);
    const struct simnode_frame *cl = last_to(FC_ACTION, E, UMAC_MESH_MPM_ACTION_CLOSE);
    uint16_t reason = 0;
    CHECK(cl != NULL && umac_mesh_ies_get_close_reason(cl->bytes + HDR, cl->len - HDR, &reason) &&
          reason == 53u && g_warthog_mpm_close_tx == ct0 + 1, "a fifth Open is answered Close(53)");

    /* A slot frees: the bit comes back with nothing else happening. */
    rx_mpm(B, UMAC_MESH_MPM_ACTION_CLOSE, 0x200, 0, 52, 0);
    CHECK(umac_datapath_mesh_peer_count() == 3, "B leaves");
    CHECK((beacon_octet(CFG_CAP) & 1) == 1 && beacon_octet(CFG_FORM) == 0x06,
          "a slot frees: the beacon says accepting, 3 peerings");
    CHECK((prsp_cap(E) & 1) == 1, "and so does a probe response to a stranger");
    simnode_advance_ms(10001);
    const uint8_t *est3[3] = { A, C, D };
    beacons_from(est3, 3, &answered, &said_1);
    CHECK(answered == 0, "not full: established peers' beacons are not answered (%u)", answered);
    umac_datapath_mesh_del_peer(NULL);
    CHECK((beacon_octet(CFG_CAP) & 1) == 1 && beacon_octet(CFG_FORM) == 0x00,
          "bulk teardown (AT+MESHRELINK): accepting, 0 peerings");

    /* ---- 2. OPEN: the neighbour's own Mesh Configuration ---------------- */
    printf("--- 2. open mesh: a neighbour that would refuse us is not opened ---\n");
    reset_node();
    simnode_outbox_clear();
    for (int i = 0; i < 3; i++) { rx_s1g_beacon(F, 0x00, AUTH_NONE); }
    CHECK(count_to(FC_ACTION, F, UMAC_MESH_MPM_ACTION_OPEN) == 0,
          "S1G beacons saying not-accepting: no Open");
    CHECK(strstr((const char *)g_warthog_mpm_links, "00000f") == NULL, "and it holds no MPM link");
    simnode_outbox_clear();
    rx_s1g_beacon(F, 0x01, AUTH_SAE);
    CHECK(count_to(FC_ACTION, F, UMAC_MESH_MPM_ACTION_OPEN) == 0,
          "an S1G beacon naming another auth protocol: no Open");
    simnode_outbox_clear();
    rx_s1g_beacon(F, 0x01, AUTH_NONE);
    CHECK(count_to(FC_ACTION, F, UMAC_MESH_MPM_ACTION_OPEN) == 1, "an accepting open neighbour: one Open");
    simnode_outbox_clear();
    for (int i = 0; i < 3; i++) { rx_legacy_beacon(G, 0x00, AUTH_NONE); }
    CHECK(count_to(FC_ACTION, G, UMAC_MESH_MPM_ACTION_OPEN) == 0,
          "legacy beacons saying not-accepting: no Open");
    simnode_outbox_clear();
    rx_legacy_beacon(G, 0x01, AUTH_NONE);
    CHECK(count_to(FC_ACTION, G, UMAC_MESH_MPM_ACTION_OPEN) == 1, "an accepting legacy beacon: one Open");

    /* A refused neighbour holds no link, so every S1G beacon of it would be a
     * first sighting; a probe response to each is what announce-once forbids. */
    reset_node();
    simnode_set_time_ms(40000);
    simnode_outbox_clear();
    for (int i = 0; i < 3; i++) { rx_s1g_beacon(F, 0x00, AUTH_NONE); }
    rx_s1g_beacon(E, 0x01, AUTH_SAE);
    CHECK(count_to(FC_PROBE_RSP, F, -1) == 1 && count_to(FC_PROBE_RSP, E, -1) == 1,
          "a not-accepting and an SAE neighbour: each answered once (%u, %u)",
          count_to(FC_PROBE_RSP, F, -1), count_to(FC_PROBE_RSP, E, -1));
    simnode_advance_ms(9999);
    simnode_outbox_clear();
    rx_s1g_beacon(F, 0x00, AUTH_NONE);
    rx_s1g_beacon(E, 0x01, AUTH_SAE);
    CHECK(count_to(FC_PROBE_RSP, F, -1) == 0 && count_to(FC_PROBE_RSP, E, -1) == 0,
          "neither again at 9.999 s");
    simnode_advance_ms(1);
    simnode_outbox_clear();
    rx_s1g_beacon(F, 0x00, AUTH_NONE);
    rx_s1g_beacon(E, 0x01, AUTH_SAE);
    CHECK(count_to(FC_PROBE_RSP, F, -1) == 1 && count_to(FC_PROBE_RSP, E, -1) == 1,
          "both at exactly 10 s");
    simnode_outbox_clear();
    for (int i = 0; i < 20; i++)
    {
        simnode_advance_ms(1000);
        rx_s1g_beacon(F, 0x00, AUTH_NONE);
    }
    CHECK(count_to(FC_PROBE_RSP, F, -1) == 2 && count_to(FC_ACTION, F, -1) == 0,
          "20 beacons 1 s apart: 2 probe responses, no peering frame (%u)",
          count_to(FC_PROBE_RSP, F, -1));
    simnode_advance_ms(1000);
    simnode_outbox_clear();
    rx_s1g_beacon(F, 0x01, AUTH_NONE);
    CHECK(count_to(FC_PROBE_RSP, F, -1) == 1 && count_to(FC_ACTION, F, UMAC_MESH_MPM_ACTION_OPEN) == 1,
          "once it accepts, its next beacon is answered and opened at once");

    reset_node();
    CHECK(peer_up(A, 0x100) && peer_up(B, 0x200) && peer_up(C, 0x300), "three up again");
    for (int i = 0; i < 3; i++) { rx_s1g_beacon(F, 0x00, AUTH_NONE); }
    simnode_outbox_clear();
    rx_mpm(E, UMAC_MESH_MPM_ACTION_OPEN, 0x500, 0, 0, 0);
    CHECK(count_to(FC_ACTION, E, UMAC_MESH_MPM_ACTION_CONFIRM) == 1 &&
          count_to(FC_ACTION, E, UMAC_MESH_MPM_ACTION_CLOSE) == 0,
          "with a not-accepting neighbour in range, a fourth peer's Open still gets a Confirm");

    /* ---- 3. OPEN, full: a slot-holder reboots ------------------------------ */
    printf("--- 3. open mesh, full: a slot-holder that rebooted comes back ---\n");
    reset_node();
    CHECK(peer_up(A, 0x100) && peer_up(B, 0x200) && peer_up(C, 0x300) && peer_up(D, 0x400),
          "four up");
    CHECK((beacon_octet(CFG_CAP) & 1) == 0, "full: beacon says not accepting");
    simnode_outbox_clear();
    rx_mpm(A, UMAC_MESH_MPM_ACTION_OPEN, 0x1a1a, 0, 0, 0); /* fresh llid */
    CHECK((frame_octet(last_to(FC_ACTION, A, UMAC_MESH_MPM_ACTION_OPEN), OPEN_IES, CFG_CAP) & 1) == 1,
          "rebooted A: our Open says accepting (its old slot was released first)");
    cf = last_to(FC_ACTION, A, UMAC_MESH_MPM_ACTION_CONFIRM);
    CHECK((frame_octet(cf, CONFIRM_IES, CFG_CAP) & 1) == 1, "and so does our Confirm");
    uint16_t ours_a = 0;
    (void)(cf != NULL && umac_mesh_ies_get_peer_llid(cf->bytes + HDR, cf->len - HDR, &ours_a));
    simnode_outbox_clear();
    rx_mpm(E, UMAC_MESH_MPM_ACTION_OPEN, 0x500, 0, 0, 0);
    reason = 0;
    cl = last_to(FC_ACTION, E, UMAC_MESH_MPM_ACTION_CLOSE);
    CHECK(cl != NULL && umac_mesh_ies_get_close_reason(cl->bytes + HDR, cl->len - HDR, &reason) &&
          reason == 53u, "a fifth Open during A's re-handshake still gets Close(53)");
    rx_mpm(A, UMAC_MESH_MPM_ACTION_CONFIRM, 0x1a1a, ours_a, 0, 1);
    CHECK(umac_datapath_mesh_peer_count() == 4, "A is back: 4/4");
    CHECK((beacon_octet(CFG_CAP) & 1) == 0, "and the beacon says not accepting again");

    /* ---- 4. OPEN: a Confirm we cannot give a station ---------------------- */
    printf("--- 4. open mesh: no station at ESTAB is refused, not latched ---\n");
    reset_node();
    CHECK(simnode_add_peer(A) && simnode_add_peer(B) && simnode_add_peer(C) && simnode_add_peer(D),
          "datapath filled behind the MPM table's back (the NO_MEM stand-in)");
    simnode_outbox_clear();
    rx_mpm(E, UMAC_MESH_MPM_ACTION_OPEN, 0x0e0e, 0, 0, 0);
    uint16_t ours_e = 0;
    cf = last_to(FC_ACTION, E, UMAC_MESH_MPM_ACTION_CONFIRM);
    CHECK(cf != NULL && umac_mesh_ies_get_peer_llid(cf->bytes + HDR, cf->len - HDR, &ours_e),
          "E is answered while the MPM table has room");
    simnode_outbox_clear();
    uint32_t af0 = g_warthog_mesh_peer_add_fail;
    rx_mpm(E, UMAC_MESH_MPM_ACTION_CONFIRM, 0x0e0e, ours_e, 0, 1);
    CHECK(g_warthog_mesh_peer_add_fail == af0 + 1, "the datapath add failed");
    cl = last_to(FC_ACTION, E, UMAC_MESH_MPM_ACTION_CLOSE);
    reason = 0;
    CHECK(cl != NULL && umac_mesh_ies_get_close_reason(cl->bytes + HDR, cl->len - HDR, &reason) &&
          reason == 53u, "E gets Close(53) instead of a silent ESTAB (reason %u)", reason);
    uint16_t cl_llid = 0, cl_plid = 0;
    if (cl != NULL)
    {
        (void)umac_mesh_ies_get_peer_llid(cl->bytes + HDR, cl->len - HDR, &cl_llid);
        (void)umac_mesh_ies_get_peer_plid(cl->bytes + HDR, cl->len - HDR, &cl_plid);
    }
    CHECK(cl_llid == ours_e && cl_plid == 0x0e0e,
          "citing both link ids, so E's FSM accepts it (llid %04x plid %04x)", cl_llid, cl_plid);
    CHECK(g_warthog_mpm_estab == 0, "ESTAB was not latched for E (estab=%u)", (unsigned)g_warthog_mpm_estab);
    CHECK(strstr((const char *)g_warthog_mpm_links, "00000e") == NULL, "and E's link is released");

    /* We opened and E's Open was lost: our link has no plid yet, so only E's
     * Confirm names the llid E's FSM will accept a Close for. */
    reset_node();
    CHECK(simnode_add_peer(A) && simnode_add_peer(B) && simnode_add_peer(C) && simnode_add_peer(D),
          "datapath full again");
    simnode_outbox_clear();
    prod_(E);
    ours_e = our_open_llid(E);
    CHECK(ours_e != 0, "we open toward E");
    simnode_outbox_clear();
    uint32_t ct1 = g_warthog_mpm_close_tx;
    rx_mpm(E, UMAC_MESH_MPM_ACTION_CONFIRM, 0x0e1e, ours_e, 0, 1);
    cl = last_to(FC_ACTION, E, UMAC_MESH_MPM_ACTION_CLOSE);
    reason = 0;
    cl_llid = 0;
    cl_plid = 0;
    if (cl != NULL)
    {
        (void)umac_mesh_ies_get_close_reason(cl->bytes + HDR, cl->len - HDR, &reason);
        (void)umac_mesh_ies_get_peer_llid(cl->bytes + HDR, cl->len - HDR, &cl_llid);
        (void)umac_mesh_ies_get_peer_plid(cl->bytes + HDR, cl->len - HDR, &cl_plid);
    }
    CHECK(reason == 53u && cl_llid == ours_e && cl_plid == 0x0e1e,
          "E's Confirm without its Open: Close(53) cites the llid in that Confirm (llid %04x plid %04x)",
          cl_llid, cl_plid);
    CHECK(g_warthog_mpm_close_tx == ct1 + 1, "and is counted in close_tx (+%u)",
          (unsigned)(g_warthog_mpm_close_tx - ct1));

    /* ---- 5. OPEN: Close(53) holds off our next Open ------------------------ */
    printf("--- 5. open mesh: a peer that says it is full is not re-opened at once ---\n");
    reset_node();
    simnode_set_time_ms(100000);
    prod_(F);
    CHECK(count_to(FC_ACTION, F, UMAC_MESH_MPM_ACTION_OPEN) == 1, "we open toward F");
    rx_mpm(F, UMAC_MESH_MPM_ACTION_CLOSE, 0, our_open_llid(F), 53, 0);
    simnode_outbox_clear();
    prod_(F);
    CHECK(count_to(FC_ACTION, F, UMAC_MESH_MPM_ACTION_OPEN) == 0,
          "after Close(53) its next probe does not re-open (was a 2 s Open/Close loop)");
    simnode_advance_ms(29000);
    simnode_outbox_clear();
    prod_(F);
    CHECK(count_to(FC_ACTION, F, UMAC_MESH_MPM_ACTION_OPEN) == 0, "still held off 29 s later");
    simnode_outbox_clear();
    rx_mpm(F, UMAC_MESH_MPM_ACTION_OPEN, 0x2222, 0, 0, 0);
    CHECK(count_to(FC_ACTION, F, UMAC_MESH_MPM_ACTION_CONFIRM) == 1, "but F's own Open is answered");
    rx_mpm(F, UMAC_MESH_MPM_ACTION_CLOSE, 0x2222, 0, 52, 0);
    simnode_outbox_clear();
    prod_(F);
    CHECK(count_to(FC_ACTION, F, UMAC_MESH_MPM_ACTION_OPEN) == 1,
          "and clears the hold-off: after its later Close(52) we re-open at once");

    reset_node();
    simnode_set_time_ms(200000);
    prod_(F);
    rx_mpm(F, UMAC_MESH_MPM_ACTION_CLOSE, 0, our_open_llid(F), 53, 0);
    simnode_advance_ms(30001);
    simnode_outbox_clear();
    prod_(F);
    CHECK(count_to(FC_ACTION, F, UMAC_MESH_MPM_ACTION_OPEN) == 1, "after 30 s we try again");

    reset_node();
    simnode_set_time_ms(300000);
    prod_(F);
    rx_mpm(F, UMAC_MESH_MPM_ACTION_CLOSE, 0, our_open_llid(F), 52, 0);
    simnode_outbox_clear();
    prod_(F);
    CHECK(count_to(FC_ACTION, F, UMAC_MESH_MPM_ACTION_OPEN) == 1, "Close(52) re-opens at once, as before");

    /* CLOSE_RCVD, MAX_RETRIES and CONFIRM_TIMEOUT are mac80211's ordinary churn. */
    static const uint16_t churn[3] = { 55, 56, 57 };
    for (int i = 0; i < 3; i++)
    {
        reset_node();
        simnode_set_time_ms(400000u + 100000u * (uint32_t)i);
        simnode_outbox_clear();
        prod_(F);
        rx_mpm(F, UMAC_MESH_MPM_ACTION_CLOSE, 0, our_open_llid(F), churn[i], 0);
        simnode_outbox_clear();
        prod_(F);
        CHECK(count_to(FC_ACTION, F, UMAC_MESH_MPM_ACTION_OPEN) == 1, "Close(%u) re-opens at once",
              (unsigned)churn[i]);
    }

    /* ---- 6. OPEN: an unanswered link gives way to an Open ----------------- */
    printf("--- 6. open mesh: a neighbour that never answers cannot hold the last link ---\n");
    reset_node();
    simnode_set_time_ms(1000000);
    CHECK(peer_up(A, 0x100) && peer_up(B, 0x200) && peer_up(C, 0x300), "A, B, C reach ESTAB");
    simnode_outbox_clear();
    prod_(Y);
    simnode_advance_ms(1000);
    rx_s1g_beacon(Y, 0x01, AUTH_SAE);
    CHECK(count_to(FC_ACTION, Y, UMAC_MESH_MPM_ACTION_OPEN) == 2,
          "an SAE neighbour's probe request draws an Open, which its beacon retransmits (%u)",
          count_to(FC_ACTION, Y, UMAC_MESH_MPM_ACTION_OPEN));
    const uint16_t ours_y = our_open_llid(Y);
    CHECK(strstr((const char *)g_warthog_mpm_links, "000019") != NULL && (beacon_octet(CFG_CAP) & 1) == 1,
          "so Y holds the last MPM link while our beacon says accepting");
    simnode_outbox_clear();
    uint32_t ns0 = g_warthog_mpm_no_slot;
    rx_mpm(D, UMAC_MESH_MPM_ACTION_OPEN, 0x400, 0, 0, 0);
    CHECK(count_to(FC_ACTION, D, UMAC_MESH_MPM_ACTION_CONFIRM) == 1 &&
          count_to(FC_ACTION, D, UMAC_MESH_MPM_ACTION_CLOSE) == 0 && g_warthog_mpm_no_slot == ns0,
          "a fourth peer's Open takes Y's link: a Confirm, not Close(53), and no no_slot");
    cl = last_to(FC_ACTION, Y, UMAC_MESH_MPM_ACTION_CLOSE);
    reason = 0;
    cl_plid = 0;
    if (cl != NULL)
    {
        (void)umac_mesh_ies_get_close_reason(cl->bytes + HDR, cl->len - HDR, &reason);
        (void)umac_mesh_ies_get_peer_plid(cl->bytes + HDR, cl->len - HDR, &cl_plid);
    }
    CHECK(reason == 52u && cl_plid == ours_y && ours_y != 0,
          "Y is sent Close(52) citing the llid of our Opens (reason %u plid %04x)", reason, cl_plid);
    CHECK(strstr((const char *)g_warthog_mpm_links, "000019") == NULL, "and Y's link is released");
    cf = last_to(FC_ACTION, D, UMAC_MESH_MPM_ACTION_CONFIRM);
    ours_d = 0;
    (void)(cf != NULL && umac_mesh_ies_get_peer_llid(cf->bytes + HDR, cf->len - HDR, &ours_d));
    rx_mpm(D, UMAC_MESH_MPM_ACTION_CONFIRM, 0x400, ours_d, 0, 1);
    CHECK(umac_datapath_mesh_peer_count() == 4, "D reaches ESTAB: 4/4");
    simnode_outbox_clear();
    prod_(Y);
    CHECK(count_to(FC_ACTION, Y, -1) == 0, "full: Y's next probe request draws nothing");

    /* Only a link with no answer yields: a neighbour mid-handshake keeps its. */
    reset_node();
    CHECK(peer_up(A, 0x100) && peer_up(B, 0x200) && peer_up(C, 0x300), "three up again");
    rx_mpm(E, UMAC_MESH_MPM_ACTION_OPEN, 0x500, 0, 0, 0);
    simnode_outbox_clear();
    rx_mpm(D, UMAC_MESH_MPM_ACTION_OPEN, 0x400, 0, 0, 0);
    reason = 0;
    cl = last_to(FC_ACTION, D, UMAC_MESH_MPM_ACTION_CLOSE);
    CHECK(cl != NULL && umac_mesh_ies_get_close_reason(cl->bytes + HDR, cl->len - HDR, &reason) &&
          reason == 53u && count_to(FC_ACTION, E, -1) == 0,
          "E sent us an Open: D's Open gets Close(53) and E is left alone");
    CHECK(strstr((const char *)g_warthog_mpm_links, "00000e") != NULL, "E keeps its link");

    /* ---- 7. OPEN: more neighbours we will not open toward than stamps ----- */
    printf("--- 7. open mesh: 9 and 12 neighbours we will not open toward ---\n");
    static const int n_quiet[2] = { MPM_MAX_QUIET + 1, 12 };
    for (int k = 0; k < 2; k++)
    {
        const int n = n_quiet[k];
        reset_node();
        simnode_set_time_ms(2000000u + 100000u * (uint32_t)k);
        uint32_t last[12] = { 0 };
        bool seen[12] = { false };
        unsigned total = 0, early = 0;
        for (int s = 0; s < 30; s++)
        {
            for (int i = 0; i < n; i++)
            {
                uint8_t sa[6];
                nbr_(sa, (uint8_t)(0x40 + i));
                simnode_advance_ms(1000u / (uint32_t)n);
                simnode_outbox_clear();
                rx_s1g_beacon(sa, 0x01, AUTH_SAE);
                if (count_to(FC_PROBE_RSP, sa, -1) == 0) { continue; }
                const uint32_t now = mmosal_get_time_ms();
                early += (seen[i] && (uint32_t)(now - last[i]) < 10000u);
                seen[i] = true;
                last[i] = now;
                total++;
            }
        }
        CHECK(early == 0, "%d neighbours beaconing 1/s for 30 s: none answered twice within 10 s (%u)",
              n, early);
        CHECK(total >= MPM_MAX_QUIET && total <= 3u * MPM_MAX_QUIET,
              "and %u answers in all, at most %u per 10 s", total, (unsigned)MPM_MAX_QUIET);
    }

    reset_node();
    simnode_set_time_ms(2500000);
    for (int i = 0; i < MPM_MAX_QUIET; i++)
    {
        uint8_t sa[6];
        nbr_(sa, (uint8_t)(0x40 + i));
        rx_s1g_beacon(sa, 0x00, AUTH_NONE);
    }
    uint8_t ninth[6];
    nbr_(ninth, (uint8_t)(0x40 + MPM_MAX_QUIET));
    simnode_outbox_clear();
    rx_s1g_beacon(ninth, 0x00, AUTH_NONE);
    CHECK(count_to(FC_PROBE_RSP, ninth, -1) == 0, "every stamp live: a ninth is not answered");
    rx_s1g_beacon(G, 0x01, AUTH_NONE);
    CHECK(count_to(FC_PROBE_RSP, G, -1) == 1 && count_to(FC_ACTION, G, UMAC_MESH_MPM_ACTION_OPEN) == 1,
          "an accepting neighbour is still answered and opened at once");
    simnode_advance_ms(10000);
    simnode_outbox_clear();
    rx_s1g_beacon(ninth, 0x00, AUTH_NONE);
    CHECK(count_to(FC_PROBE_RSP, ninth, -1) == 1, "the ninth is answered once a stamp lapses");

    /* ---- 8. OPEN: more refusers than hold-off entries --------------------- */
    printf("--- 8. open mesh: 5 and 8 full neighbours that refuse us ---\n");
    static const int n_refuse[2] = { MPM_MAX_LINKS + 1, 8 };
    for (int k = 0; k < 2; k++)
    {
        const int n = n_refuse[k];
        reset_node();
        simnode_set_time_ms(3000000u + 100000u * (uint32_t)k);
        CHECK(peer_up(A, 0x100) && peer_up(B, 0x200) && peer_up(C, 0x300), "three up");
        uint32_t closed[8] = { 0 };
        bool was[8] = { false };
        unsigned opens = 0, early = 0;
        for (int r = 0; r < 14; r++)
        {
            for (int i = 0; i < n; i++)
            {
                uint8_t sa[6];
                nbr_(sa, (uint8_t)(0x60 + i));
                simnode_advance_ms(2000u / (uint32_t)n);
                simnode_outbox_clear();
                prod_(sa);
                if (count_to(FC_ACTION, sa, UMAC_MESH_MPM_ACTION_OPEN) == 0) { continue; }
                const uint32_t now = mmosal_get_time_ms();
                early += (was[i] && (uint32_t)(now - closed[i]) < 30000u);
                opens++;
                rx_mpm(sa, UMAC_MESH_MPM_ACTION_CLOSE, 0, our_open_llid(sa), 53, 0);
                was[i] = true;
                closed[i] = now;
            }
            rx_s1g_beacon(A, 0x01, AUTH_NONE);
            rx_s1g_beacon(B, 0x01, AUTH_NONE);
            rx_s1g_beacon(C, 0x01, AUTH_NONE);
        }
        CHECK(early == 0 && opens == MPM_MAX_LINKS,
              "%d refusers probing every 2 s for 28 s: %u Opens, none within 30 s of a Close (%u)",
              n, opens, early);
        simnode_outbox_clear();
        rx_mpm(E, UMAC_MESH_MPM_ACTION_OPEN, 0x500, 0, 0, 0);
        CHECK(count_to(FC_ACTION, E, UMAC_MESH_MPM_ACTION_CONFIRM) == 1,
              "a stranger's own Open is still answered");
    }

    /* A refusal that finds every entry live still holds off for its full 30 s. */
    reset_node();
    simnode_set_time_ms(4000000);
    uint8_t rf[5][6];
    for (int i = 0; i < 5; i++) { nbr_(rf[i], (uint8_t)(0x70 + i)); }
    for (int i = 0; i < 3; i++)
    {
        simnode_outbox_clear();
        prod_(rf[i]);
        rx_mpm(rf[i], UMAC_MESH_MPM_ACTION_CLOSE, 0, our_open_llid(rf[i]), 53, 0);
    }
    simnode_advance_ms(20000);
    simnode_outbox_clear();
    prod_(rf[3]);
    prod_(rf[4]);
    const uint16_t l3 = our_open_llid(rf[3]), l4 = our_open_llid(rf[4]);
    CHECK(l3 != 0 && l4 != 0, "two Opens out before either is refused");
    rx_mpm(rf[3], UMAC_MESH_MPM_ACTION_CLOSE, 0, l3, 53, 0);
    rx_mpm(rf[4], UMAC_MESH_MPM_ACTION_CLOSE, 0, l4, 53, 0);
    simnode_outbox_clear();
    prod_(F);
    CHECK(count_to(FC_ACTION, F, UMAC_MESH_MPM_ACTION_OPEN) == 0,
          "every hold-off entry live: a neighbour that never refused is not opened either");
    simnode_advance_ms(10001);
    simnode_outbox_clear();
    prod_(rf[4]);
    CHECK(count_to(FC_ACTION, rf[4], UMAC_MESH_MPM_ACTION_OPEN) == 0,
          "the refusal no entry could hold: still held off after the older three lapse");
    simnode_advance_ms(19998);
    simnode_outbox_clear();
    prod_(rf[4]);
    CHECK(count_to(FC_ACTION, rf[4], UMAC_MESH_MPM_ACTION_OPEN) == 0, "and 1 ms before its 30 s");
    simnode_advance_ms(1);
    simnode_outbox_clear();
    prod_(rf[4]);
    CHECK(count_to(FC_ACTION, rf[4], UMAC_MESH_MPM_ACTION_OPEN) == 1, "opened again at 30 s");

    /* ---- 9. SAE: the offer gate ------------------------------------------- */
    printf("--- 9. SAE: a new candidate is not offered to hostap while full ---\n");
    CHECK(simnode_start_sae(W), "node restarts as a SAE mesh");
    reset_node();
    simnode_add_peer(A);
    simnode_add_peer(B);
    simnode_add_peer(C);
    simnode_stub_reset();
    rx_probe_rsp(E, 0x01);
    CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 1, "3 of 4: E is offered");
    simnode_add_peer(D);
    CHECK((prsp_cap(F) & 1) == 0, "full: probe response to a stranger says not accepting");
    CHECK((prsp_cap(D) & 1) == 1, "but one to a slot-holder says accepting");
    CHECK((beacon_octet(CFG_CAP) & 1) == 0, "full: the beacon says not accepting");
    CHECK(beacon_octet(CFG_FORM) == 0x00, "stations still in SAE are not peerings (got 0x%02x)",
          beacon_octet(CFG_FORM));
    CHECK(simnode_set_key(A, K_MTK, 0, true) == 0, "A finishes AMPE (its MTK arrives)");
    CHECK(beacon_octet(CFG_FORM) == 0x02, "a keyed station is: Formation Info = 1 << 1 (got 0x%02x)",
          beacon_octet(CFG_FORM));

    uint32_t of0 = g_warthog_sae_offer_full;
    simnode_stub_reset();
    rx_probe_rsp(F, 0x01);
    CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 0, "full: F's probe response is not offered");
    simnode_stub_reset();
    prod_(F);
    CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 0, "nor F's probe request");
    simnode_stub_reset();
    CHECK(rx_s1g_beacon(F, 0x01, AUTH_SAE) && simnode_stub_hits("umac_supp_mesh_new_peer") == 0,
          "nor F's S1G beacon, though F itself says accepting");
    CHECK(g_warthog_sae_offer_full == of0 + 3, "each refusal is counted (%u)",
          (unsigned)(g_warthog_sae_offer_full - of0));

    simnode_outbox_clear();
    simnode_stub_reset();
    rx_s1g_beacon(A, 0x01, AUTH_SAE);
    CHECK(count_to(FC_PROBE_RSP, A, -1) == 1 &&
          (frame_octet(last_to(FC_PROBE_RSP, A, -1), PRSP_IES, CFG_CAP) & 1) == 1,
          "full: a slot-holder's S1G beacon gets one probe response saying accepting");
    CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 1, "and the slot-holder is still offered");
    simnode_outbox_clear();
    rx_s1g_beacon(F, 0x01, AUTH_SAE);
    CHECK(count_to(FC_PROBE_RSP, F, -1) == 1 &&
          (frame_octet(last_to(FC_PROBE_RSP, F, -1), PRSP_IES, CFG_CAP) & 1) == 0,
          "a stranger's S1G beacon gets one saying not accepting");
    simnode_stub_reset();
    rx_probe_rsp(A, 0x01);
    CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 1, "a slot-holder's probe response still reaches hostap");

    simnode_stub_reset();
    prod_(B);
    uint8_t who[6] = { 0 };
    const uint8_t *ies = NULL;
    size_t ies_len = simnode_last_new_peer(who, &ies);
    static uint8_t ies_copy[256];
    if (ies != NULL && ies_len <= sizeof(ies_copy)) { memcpy(ies_copy, ies, ies_len); }
    uint8_t *ocfg = (ies != NULL && ies_len <= sizeof(ies_copy)) ? meshcfg_in(ies_copy, (uint32_t)ies_len) : NULL;
    CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 1 && memcmp(who, B, 6) == 0,
          "full: a slot-holder's probe request is offered");
    CHECK(ocfg != NULL && (ocfg[CFG_CAP] & 1) == 1,
          "with synthetic IEs that say accepting, not our own capacity");

    simnode_del_peer(B);
    simnode_stub_reset();
    rx_probe_rsp(F, 0x01);
    CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 1, "a slot frees: F is offered again");

    /* Number of Peerings counts the keyed slot itself: AID n is slot n - 1. */
    reset_node();
    CHECK(simnode_add_peer(A) && simnode_add_peer(B) && simnode_add_peer(C) && simnode_add_peer(D),
          "four SAE candidates, none keyed");
    CHECK(simnode_set_key(D, K_MTK, 0, true) == 0, "D, in the fourth slot (AID 4), finishes AMPE");
    CHECK(beacon_octet(CFG_FORM) == 0x02, "the fourth slot's key counts (0x%02x)", beacon_octet(CFG_FORM));
    simnode_del_peer(A);
    CHECK(beacon_octet(CFG_FORM) == 0x02, "and still counts after unkeyed A leaves (0x%02x)",
          beacon_octet(CFG_FORM));
    simnode_add_peer(G);
    simnode_del_peer(D);
    simnode_add_peer(E);
    CHECK(beacon_octet(CFG_FORM) == 0x00, "a new candidate in D's old slot is not a peering (0x%02x)",
          beacon_octet(CFG_FORM));

    /* ---- 10. SAE: failed handshakes give their slots back ------------------ */
    printf("--- 10. SAE: five neighbours that never finish SAE do not starve the table ---\n");
    reset_node();
    simnode_set_time_ms(6000000);
    static const uint8_t K[6] = { 0x02, 0, 0, 0, 0, 0x2a }; /* keyed, before any of this */
    static const uint8_t G2[6] = { 0x02, 0, 0, 0, 0, 0x2b };
    CHECK(simnode_add_peer(K) && simnode_set_key(K, K_MTK, 0, true) == 0, "K is keyed in one slot");
    uint8_t fl[5][6];
    for (int i = 0; i < 5; i++) { nbr_(fl[i], (uint8_t)(0x80 + i)); }

    /* hostap adds each offered candidate (.sta_add takes the slot); none ever answers. */
    unsigned offered = 0;
    uint32_t full0 = g_warthog_sae_offer_full;
    for (int i = 0; i < 5; i++)
    {
        simnode_stub_reset();
        rx_probe_rsp(fl[i], 0x01);
        if (simnode_stub_hits("umac_supp_mesh_new_peer") == 1 && simnode_add_peer(fl[i])) { offered++; }
    }
    CHECK(offered == 3 && umac_datapath_mesh_peer_count() == 4 && g_warthog_sae_offer_full == full0 + 2,
          "three take the free slots, two are refused as full (%u offered)", offered);
    simnode_stub_reset();
    rx_probe_rsp(G, 0x01);
    CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 0, "a genuine neighbour G finds the table full");

    /* 10-19 s later hostap's mesh_auth_timer fires for each without SAE_ACCEPTED. */
    simnode_advance_ms(15000);
    const uint32_t t_fail = mmosal_get_time_ms();
    uint32_t sf0 = g_warthog_sae_fail;
    for (int i = 0; i < 3; i++) { umac_mesh_sae_failed(fl[i]); }
    CHECK(umac_datapath_mesh_peer_count() == 1 && g_warthog_sae_fail == sf0 + 3,
          "the first failure frees each slot: only K is left (%u)", (unsigned)umac_datapath_mesh_peer_count());
    CHECK(beacon_octet(CFG_FORM) == 0x02 && (beacon_octet(CFG_CAP) & 1) == 1,
          "K keeps its slot and its key; the beacon says accepting again");

    uint32_t held0 = g_warthog_sae_offer_held;
    simnode_stub_reset();
    for (int i = 0; i < 3; i++)
    {
        rx_probe_rsp(fl[i], 0x01);
        rx_s1g_beacon(fl[i], 0x01, AUTH_SAE);
        prod_(fl[i]);
    }
    CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 0 && g_warthog_sae_offer_held == held0 + 9,
          "the three that failed are held off on all three discovery paths (+%u held)",
          (unsigned)(g_warthog_sae_offer_held - held0));

    /* The two never tried now get slots; G gets the last one while they run SAE. */
    offered = 0;
    for (int i = 3; i < 5; i++)
    {
        simnode_stub_reset();
        rx_probe_rsp(fl[i], 0x01);
        if (simnode_stub_hits("umac_supp_mesh_new_peer") == 1 && simnode_add_peer(fl[i])) { offered++; }
    }
    simnode_stub_reset();
    rx_probe_rsp(G, 0x01);
    CHECK(offered == 2 && simnode_stub_hits("umac_supp_mesh_new_peer") == 1 && simnode_add_peer(G) &&
          simnode_set_key(G, K_MTK, 0, true) == 0,
          "the other two are offered, and G is offered and keys beside them");
    simnode_advance_ms(12000);
    const uint32_t t_fail2 = mmosal_get_time_ms();
    umac_mesh_sae_failed(fl[3]);
    umac_mesh_sae_failed(fl[4]);
    CHECK(umac_datapath_mesh_peer_count() == 2 && beacon_octet(CFG_FORM) == 0x04,
          "all five failed: K and G hold the only slots, both keyed (0x%02x)", beacon_octet(CFG_FORM));

    /* All five keep beaconing once a second: none gets a slot back during its
     * hold-off, and a second genuine neighbour is still offered. */
    unsigned leaked = 0;
    for (int s = 0; s < 17; s++)
    {
        simnode_advance_ms(1000);
        simnode_stub_reset();
        for (int i = 0; i < 5; i++)
        {
            rx_s1g_beacon(fl[i], 0x01, AUTH_SAE);
            rx_probe_rsp(fl[i], 0x01);
        }
        leaked += simnode_stub_hits("umac_supp_mesh_new_peer");
    }
    CHECK(leaked == 0 && umac_datapath_mesh_peer_count() == 2,
          "17 s of beacons from all five: none offered (%u)", leaked);
    simnode_stub_reset();
    rx_probe_rsp(G2, 0x01);
    uint8_t who2[6] = { 0 };
    (void)simnode_last_new_peer(who2, NULL);
    CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 1 && memcmp(who2, G2, 6) == 0,
          "a second genuine neighbour is offered meanwhile");

    /* The hold-off ends at 30 s: re-offered then, not before. */
    simnode_set_time_ms(t_fail + 29999);
    simnode_stub_reset();
    rx_probe_rsp(fl[0], 0x01);
    CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 0, "held 1 ms before its 30 s");
    simnode_set_time_ms(t_fail + 30000);
    simnode_stub_reset();
    rx_probe_rsp(fl[0], 0x01);
    CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 1, "and offered again at 30 s");

    /* A held-off neighbour's own Commit ends its hold-off at once, with IEs that
     * say accepting; its Confirm, or a Commit from one never held, does not. */
    simnode_set_time_ms(t_fail2 + 29000);
    simnode_stub_reset();
    rx_sae_auth(fl[3], 2);
    rx_sae_auth(A, 1);
    CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 0,
          "a held-off neighbour's Confirm, and an unknown one's Commit, offer nothing");
    rx_sae_auth(fl[3], 1);
    uint8_t who3[6] = { 0 };
    const uint8_t *cies = NULL;
    size_t cies_len = simnode_last_new_peer(who3, &cies);
    static uint8_t cies_copy[256];
    uint8_t *ccfg = NULL;
    if (cies != NULL && cies_len <= sizeof(cies_copy))
    {
        memcpy(cies_copy, cies, cies_len);
        ccfg = meshcfg_in(cies_copy, (uint32_t)cies_len);
    }
    CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 1 && memcmp(who3, fl[3], 6) == 0 &&
          ccfg != NULL && (ccfg[CFG_CAP] & 1) == 1 && ccfg[CFG_AUTH] == AUTH_SAE,
          "its Commit offers it at once, with SAE IEs that say accepting");
    simnode_stub_reset();
    rx_probe_rsp(fl[3], 0x01);
    CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 1, "and its hold-off is gone");
    simnode_stub_reset();
    rx_probe_rsp(fl[4], 0x01);
    CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 0, "the other one's is not");

    /* ---- 11. the candidate RSSI floor --------------------------------------- */
    printf("--- 11. a neighbour heard only at or below the RSSI floor is not peered with ---\n");
    reset_node();
    simnode_set_time_ms(7000000);
    CHECK(g_warthog_mesh_rssi_floor == -80, "the floor defaults to -80 dBm, OpenMANET's mesh_rssi_threshold");
    uint32_t sk0 = g_warthog_mesh_rssi_skip, ps0 = g_warthog_mesh_rssi_pass;
    simnode_stub_reset();
    s_rssi = -80;
    rx_probe_rsp(F, 0x01);
    s_rssi = -85;
    rx_s1g_beacon(F, 0x01, AUTH_SAE);
    s_rssi = -95;
    prod_(F);
    CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 0 && g_warthog_mesh_rssi_skip == sk0 + 3,
          "SAE: at -80, -85 and -95 dBm, from all three discovery paths: not offered, each counted (+%u)",
          (unsigned)(g_warthog_mesh_rssi_skip - sk0));
    s_rssi = -79;
    rx_probe_rsp(F, 0x01);
    CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 1 && g_warthog_mesh_rssi_pass == ps0 + 1,
          "at -79 dBm it is offered, and counted as passing");
    s_rssi = -85;
    simnode_outbox_clear();
    for (int i = 0; i < 3; i++)
    {
        simnode_advance_ms(1000);
        rx_s1g_beacon(E, 0x01, AUTH_SAE);
    }
    CHECK(count_to(FC_PROBE_RSP, E, -1) == 1, "a weak SAE neighbour's beacons are answered once per 10 s (%u)",
          count_to(FC_PROBE_RSP, E, -1));
    CHECK(simnode_add_peer(A), "A holds a slot");
    simnode_stub_reset();
    s_rssi = -95;
    rx_probe_rsp(A, 0x01);
    CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 1, "a slot-holder is not subject to the floor");
    simnode_set_time_ms(7100000);
    umac_mesh_sae_failed(F);
    simnode_stub_reset();
    rx_sae_auth(F, 1);
    CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 0,
          "a held-off neighbour's Commit at -95 dBm is released but not offered");
    s_rssi = -70;
    rx_probe_rsp(F, 0x01);
    CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 1, "and is offered once heard above it");

    static const int32_t offs[2] = { 0, -255 };
    for (int k = 0; k < 2; k++)
    {
        g_warthog_mesh_rssi_floor = offs[k];
        simnode_stub_reset();
        s_rssi = -255;
        rx_probe_rsp(E, 0x01);
        CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 1, "floor %d is off: even -255 dBm is offered",
              (int)offs[k]);
    }
    g_warthog_mesh_rssi_floor = -90;
    simnode_stub_reset();
    s_rssi = -89;
    rx_probe_rsp(G, 0x01);
    s_rssi = -90;
    rx_probe_rsp(E, 0x01);
    CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 1, "floor -90: -89 is offered, -90 is not");
    g_warthog_mesh_rssi_floor = -80;

    CHECK(simnode_start(W), "node restarts as an open mesh");
    simnode_set_gates(false, false, false, false);
    reset_node();
    simnode_set_time_ms(8000000);
    sk0 = g_warthog_mesh_rssi_skip;
    ps0 = g_warthog_mesh_rssi_pass;
    const uint32_t named0 = g_warthog_prq_named;
    s_rssi = -81;
    simnode_outbox_clear();
    rx_s1g_beacon(F, 0x01, AUTH_NONE);
    rx_legacy_beacon(G, 0x01, AUTH_NONE);
    prod_(E);
    CHECK(g_warthog_prq_named == named0 + 1,
          "open: a probe request naming our mesh is counted, as the no-peers diagnosis needs");
    CHECK(count_to(FC_ACTION, F, UMAC_MESH_MPM_ACTION_OPEN) == 0 &&
          count_to(FC_ACTION, G, UMAC_MESH_MPM_ACTION_OPEN) == 0 && g_warthog_mesh_rssi_skip == sk0 + 2,
          "open: at -81 dBm neither an S1G beacon nor a legacy beacon draws an Open, each counted (+%u)",
          (unsigned)(g_warthog_mesh_rssi_skip - sk0));
    CHECK(count_to(FC_ACTION, E, UMAC_MESH_MPM_ACTION_OPEN) == 1 && g_warthog_mesh_rssi_pass == ps0,
          "but a probe request does, uncounted: two weak warthogs still peer through their probes");
    CHECK(strstr((const char *)g_warthog_mpm_links, "00000f") == NULL, "and the weak one holds no MPM link");
    for (int i = 0; i < 3; i++)
    {
        simnode_advance_ms(1000);
        rx_s1g_beacon(F, 0x01, AUTH_NONE);
    }
    CHECK(count_to(FC_PROBE_RSP, F, -1) == 1 && count_to(FC_ACTION, F, UMAC_MESH_MPM_ACTION_OPEN) == 0,
          "its beacons are answered once per 10 s, not every second, and never opened (%u)",
          count_to(FC_PROBE_RSP, F, -1));
    simnode_outbox_clear();
    s_rssi = -95;
    rx_mpm(D, UMAC_MESH_MPM_ACTION_OPEN, 0x0d0d, 0, 0, 0);
    CHECK(count_to(FC_ACTION, D, UMAC_MESH_MPM_ACTION_CONFIRM) == 1,
          "a weak neighbour's own Open is still answered");
    s_rssi = -70;
    simnode_outbox_clear();
    rx_s1g_beacon(C, 0x01, AUTH_NONE);
    CHECK(count_to(FC_ACTION, C, UMAC_MESH_MPM_ACTION_OPEN) == 1, "at -70 dBm the beacon draws an Open");
    s_rssi = -90;
    simnode_advance_ms(1000);
    simnode_outbox_clear();
    rx_s1g_beacon(C, 0x01, AUTH_NONE);
    CHECK(count_to(FC_ACTION, C, UMAC_MESH_MPM_ACTION_OPEN) == 1,
          "and a handshake under way is still retransmitted after the signal drops to -90");
    s_rssi = -60;

    /* An open mesh counts no wildcard probe request, weak or strong; each draws an Open. */
    reset_node();
    sk0 = g_warthog_mesh_rssi_skip;
    ps0 = g_warthog_mesh_rssi_pass;
    uint32_t named1 = g_warthog_prq_named;
    uint8_t sc1[6], sc2[6];
    nbr_(sc1, 0xd1);
    nbr_(sc2, 0xd2);
    simnode_outbox_clear();
    s_rssi = -85;
    rx_wildcard_probe(sc1);
    s_rssi = -50;
    rx_wildcard_probe(sc2);
    s_rssi = -60;
    CHECK(g_warthog_mesh_rssi_skip == sk0 && g_warthog_mesh_rssi_pass == ps0 && g_warthog_prq_named == named1,
          "open: a scanner's wildcard probes, at -85 and -50 dBm, count nowhere (+%u/+%u/+%u)",
          (unsigned)(g_warthog_mesh_rssi_skip - sk0), (unsigned)(g_warthog_mesh_rssi_pass - ps0),
          (unsigned)(g_warthog_prq_named - named1));
    CHECK(count_to(FC_ACTION, sc1, UMAC_MESH_MPM_ACTION_OPEN) == 1 &&
          count_to(FC_ACTION, sc2, UMAC_MESH_MPM_ACTION_OPEN) == 1,
          "and each still draws an Open, as before the floor");

    /* ---- 12. SAE hold-offs and the floor at their edges ------------------- */
    printf("--- 12. SAE hold-offs, the floor's range ends, and what the floor counts ---\n");
    CHECK(simnode_start_sae(W), "node restarts as a SAE mesh (12)");
    reset_node();
    simnode_set_time_ms(9000000);
    {
        uint8_t n1[6], p1[6], w1[6], w2[6], w3[6], w4[6];
        nbr_(n1, 0xc1); nbr_(p1, 0xc2); nbr_(w1, 0xc3); nbr_(w2, 0xc4); nbr_(w3, 0xc5); nbr_(w4, 0xc6);
        umac_mesh_sae_failed(n1);
        simnode_stub_reset();
        rx_auth(n1, 0 /* Open System */, 1);
        CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 0,
              "an Open System Authentication (seq 1) from a held-off neighbour offers nothing");
        uint32_t h0 = g_warthog_sae_offer_held;
        rx_probe_rsp(n1, 0x01);
        CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 0 && g_warthog_sae_offer_held == h0 + 1,
              "and it is still held off afterwards");

        CHECK(simnode_add_peer(p1), "P1 holds a slot");
        unsigned before = umac_datapath_mesh_peer_count();
        uint32_t sf0 = g_warthog_sae_fail, pf0 = g_warthog_plink_fail;
        umac_mesh_sae_failed(NULL);
        umac_mesh_plink_failed(NULL);
        CHECK(umac_datapath_mesh_peer_count() == before && g_warthog_sae_fail == sf0 &&
              g_warthog_plink_fail == pf0,
              "a NULL failure of either kind frees no slot and counts nothing (%u -> %u)", before,
              (unsigned)umac_datapath_mesh_peer_count());

        g_warthog_mesh_rssi_floor = -1;
        simnode_stub_reset();
        s_rssi = -1;
        rx_probe_rsp(w1, 0x01);
        CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 0, "floor -1 is on: -1 dBm is refused");
        s_rssi = 0;
        rx_probe_rsp(w2, 0x01);
        CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 1, "floor -1 admits 0 dBm");
        g_warthog_mesh_rssi_floor = -254;
        simnode_stub_reset();
        s_rssi = -254;
        rx_probe_rsp(w3, 0x01);
        CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 0, "floor -254 is on: -254 dBm is refused");
        s_rssi = -253;
        rx_probe_rsp(w4, 0x01);
        CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 1, "floor -254 admits -253 dBm");
        g_warthog_mesh_rssi_floor = -80;
        s_rssi = -60;
    }

    /* SAE finished, peering did not: hostap frees the station; it is held off too. */
    reset_node();
    simnode_set_time_ms(9100000);
    {
        uint8_t pk[6], q1[6];
        nbr_(pk, 0xe1);
        nbr_(q1, 0xe2);
        simnode_stub_reset();
        rx_probe_rsp(pk, 0x01);
        CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 1 && simnode_add_peer(pk) &&
              umac_datapath_mesh_peer_count() == 1,
              "PK is offered and holds a slot while its peering runs");
        const uint32_t t_pf = mmosal_get_time_ms();
        uint32_t sf0 = g_warthog_sae_fail, pf0 = g_warthog_plink_fail, h0 = g_warthog_sae_offer_held;
        umac_mesh_plink_failed(pk);
        CHECK(umac_datapath_mesh_peer_count() == 0 && g_warthog_plink_fail == pf0 + 1 &&
              g_warthog_sae_fail == sf0,
              "its FSM restarting before ESTAB frees the slot, counted as plink_fail, not sae_fail");
        simnode_stub_reset();
        rx_probe_rsp(pk, 0x01);
        rx_s1g_beacon(pk, 0x01, AUTH_SAE);
        prod_(pk);
        CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 0 && g_warthog_sae_offer_held == h0 + 3,
              "it is held off on all three discovery paths (+%u held)",
              (unsigned)(g_warthog_sae_offer_held - h0));
        simnode_stub_reset();
        rx_probe_rsp(q1, 0x01);
        CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 1, "another neighbour takes the slot meanwhile");
        simnode_set_time_ms(t_pf + 29999);
        simnode_stub_reset();
        rx_probe_rsp(pk, 0x01);
        CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 0, "held 1 ms before its 30 s");
        simnode_set_time_ms(t_pf + 30000);
        simnode_stub_reset();
        rx_probe_rsp(pk, 0x01);
        CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 1, "and offered again at 30 s");
        umac_mesh_plink_failed(pk);
        simnode_stub_reset();
        rx_sae_auth(pk, 1);
        uint8_t who[6] = { 0 };
        (void)simnode_last_new_peer(who, NULL);
        CHECK(simnode_stub_hits("umac_supp_mesh_new_peer") == 1 && memcmp(who, pk, 6) == 0,
              "failed again, its own Commit ends the hold-off at once");
    }

    /* The floor counts only frames naming our mesh. */
    reset_node();
    simnode_set_time_ms(9200000);
    {
        static const uint8_t OTHER[] = { 'o', 't', 'h', 'e', 'r', 'm', 'e', 's', 'h' };
        uint8_t o1[6], o2[6], m1[6], m2[6], h1[6];
        nbr_(o1, 0xf1); nbr_(o2, 0xf2); nbr_(m1, 0xf3); nbr_(m2, 0xf4); nbr_(h1, 0xf5);
        sk0 = g_warthog_mesh_rssi_skip;
        ps0 = g_warthog_mesh_rssi_pass;
        s_rssi = -85;
        rx_probe_rsp_id(o1, OTHER, (uint8_t)sizeof(OTHER));
        s_rssi = -60;
        rx_probe_rsp_id(o2, OTHER, (uint8_t)sizeof(OTHER));
        CHECK(g_warthog_mesh_rssi_skip == sk0 && g_warthog_mesh_rssi_pass == ps0,
              "SAE: another mesh's probe responses, weak or strong, count neither way (+%u/+%u)",
              (unsigned)(g_warthog_mesh_rssi_skip - sk0), (unsigned)(g_warthog_mesh_rssi_pass - ps0));
        umac_mesh_sae_failed(h1);
        rx_sae_auth(h1, 1);
        umac_mesh_sae_failed(h1);
        s_rssi = -95;
        rx_sae_auth(h1, 1);
        CHECK(g_warthog_mesh_rssi_skip == sk0 && g_warthog_mesh_rssi_pass == ps0,
              "nor does a held-off neighbour's Commit, which names no mesh");
        rx_probe_rsp(m1, 0x01);
        s_rssi = -60;
        rx_probe_rsp(m2, 0x01);
        CHECK(g_warthog_mesh_rssi_skip == sk0 + 1 && g_warthog_mesh_rssi_pass == ps0 + 1,
              "while ours count each way (+%u/+%u)", (unsigned)(g_warthog_mesh_rssi_skip - sk0),
              (unsigned)(g_warthog_mesh_rssi_pass - ps0));
    }

    /* ---- 13. an Open from a station with no slot: hostap's PMKSA-cached path ---- */
    printf("--- 13. SAE: an Open from a neighbour with no slot meets the hold-off and the floor ---\n");
    reset_node();
    simnode_set_time_ms(9300000);
    {
        uint8_t hp[6], hs[6], wk[6], sh[6];
        nbr_(hp, 0xd1); nbr_(hs, 0xd2); nbr_(wk, 0xd3); nbr_(sh, 0xd4);
        const uint32_t t_h = mmosal_get_time_ms();
        CHECK(simnode_add_peer(hp), "HP holds a slot while its peering runs (13)");
        umac_mesh_plink_failed(hp);
        umac_mesh_sae_failed(hs);
        uint32_t h0 = g_warthog_sae_offer_held;
        simnode_stub_reset();
        rx_mpm(hp, UMAC_MESH_MPM_ACTION_OPEN, 0x1111, 0, 0, 0);
        rx_mpm(hs, UMAC_MESH_MPM_ACTION_OPEN, 0x2222, 0, 0, 0);
        CHECK(simnode_stub_hits("umac_supp_process_mgmt_frame") == 0 && g_warthog_sae_offer_held == h0 + 2 &&
                  umac_datapath_mesh_peer_count() == 0,
              "Opens from neighbours held off after plink_fail and sae_fail never reach hostap, "
              "which would take them on a cached PMKSA (+%u held)", (unsigned)(g_warthog_sae_offer_held - h0));
        simnode_stub_reset();
        rx_mpm(hp, UMAC_MESH_MPM_ACTION_CONFIRM, 0x1111, 0x3333, 0, 1);
        rx_mpm(hp, UMAC_MESH_MPM_ACTION_CLOSE, 0x1111, 0x3333, 55, 0);
        CHECK(simnode_stub_hits("umac_supp_process_mgmt_frame") == 2,
              "(pin) its Confirm and Close still do: without a station hostap creates none from them");

        uint32_t sk = g_warthog_mesh_rssi_skip, ps = g_warthog_mesh_rssi_pass;
        simnode_stub_reset();
        s_rssi = -85;
        rx_mpm(wk, UMAC_MESH_MPM_ACTION_OPEN, 0x4444, 0, 0, 0);
        CHECK(simnode_stub_hits("umac_supp_process_mgmt_frame") == 0 &&
                  g_warthog_mesh_rssi_skip == sk + 1 && g_warthog_mesh_rssi_pass == ps,
              "an Open at -85 dBm from a neighbour with no slot is refused below the floor (+%u skipped)",
              (unsigned)(g_warthog_mesh_rssi_skip - sk));
        {
            static const uint8_t OTHER[] = { 'o', 't', 'h', 'e', 'r', 'm', 'e', 's', 'h' };
            uint8_t body[UMAC_MESH_MPM_BODY_MAXLEN];
            const uint16_t n = umac_mesh_ies_build_mpm_body(body, sizeof(body), UMAC_MESH_MPM_ACTION_OPEN,
                                                            0x4445, 0, 0, 0, OTHER, sizeof(OTHER), false,
                                                            NULL, 0);
            rx_mgmt(FC_ACTION, wk, body, n);
            CHECK(simnode_stub_hits("umac_supp_process_mgmt_frame") == 0 && g_warthog_mesh_rssi_skip == sk + 1,
                  "one naming another mesh is refused too, and not counted: skipped counts ours only");
        }
        simnode_stub_reset();
        s_rssi = -79;
        rx_mpm(wk, UMAC_MESH_MPM_ACTION_OPEN, 0x4444, 0, 0, 0);
        CHECK(simnode_stub_hits("umac_supp_process_mgmt_frame") == 1 && g_warthog_mesh_rssi_pass == ps,
              "(pin) at -79 dBm it reaches hostap, uncounted: hostap may still drop it");
        simnode_stub_reset();
        g_warthog_mesh_rssi_floor = 0;
        s_rssi = -95;
        rx_mpm(wk, UMAC_MESH_MPM_ACTION_OPEN, 0x4444, 0, 0, 0);
        CHECK(simnode_stub_hits("umac_supp_process_mgmt_frame") == 1, "(pin) with the floor off, at -95 dBm too");
        g_warthog_mesh_rssi_floor = -80;
        CHECK(simnode_add_peer(sh), "SH holds a slot");
        simnode_stub_reset();
        rx_mpm(sh, UMAC_MESH_MPM_ACTION_OPEN, 0x5555, 0, 0, 0);
        CHECK(simnode_stub_hits("umac_supp_process_mgmt_frame") == 1,
              "(pin) a slot-holder's Open at -95 dBm is not floored: a peering under way continues");
        s_rssi = -60;

        simnode_set_time_ms(t_h + 29999);
        simnode_stub_reset();
        rx_mpm(hp, UMAC_MESH_MPM_ACTION_OPEN, 0x1111, 0, 0, 0);
        CHECK(simnode_stub_hits("umac_supp_process_mgmt_frame") == 0, "HP is still held 1 ms before its 30 s");
        simnode_set_time_ms(t_h + 30000);
        simnode_stub_reset();
        rx_mpm(hp, UMAC_MESH_MPM_ACTION_OPEN, 0x1111, 0, 0, 0);
        CHECK(simnode_stub_hits("umac_supp_process_mgmt_frame") == 1, "and its Open reaches hostap at 30 s");
    }

    simnode_stop();
    if (failures == 0)
    {
        printf("test_simnode_capacity: all passed\n");
        return 0;
    }
    printf("test_simnode_capacity: %d FAILED\n", failures);
    return 1;
}
