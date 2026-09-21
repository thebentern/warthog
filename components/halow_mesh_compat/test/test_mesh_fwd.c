/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Mesh data-plane decisions (morselib src/umac/mesh/umac_mesh_fwd.c).
 *
 * Receive: a group frame is delivered AND rebroadcast with ttl - 1, once (the
 * cache catches the echo); a unicast for us is delivered with the proxied
 * endpoints if AE 2 carried them; a unicast for a third party is forwarded to
 * the path's next hop with ttl - 1, or a PERR goes back to the transmitter
 * when there is no path; nothing is forwarded with forwarding off or at
 * ttl <= 1; our own frame coming back is dropped.
 *
 * Transmit: a frame from us to a mesh node is plain 4-address; to a host
 * behind a node it goes to that node with AE 2; from a host behind us it
 * carries AE 2 (unicast) or AE 1 (group); a destination with no path and no
 * peer asks for a PREQ.
 */
#include "umac_mesh_fwd.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static const uint8_t A[6]  = { 0x02, 0, 0, 0, 0, 0xaa }; /* peer */
static const uint8_t W[6]  = { 0x02, 0, 0, 0, 0, 0x77 }; /* us */
static const uint8_t B[6]  = { 0x02, 0, 0, 0, 0, 0xbb }; /* peer */
static const uint8_t C[6]  = { 0x02, 0, 0, 0, 0, 0xcc }; /* two hops away, via B */
static const uint8_t HA[6] = { 0x00, 0xe0, 0x4f, 0x71, 0x99, 0xa5 }; /* host behind A */
static const uint8_t HW[6] = { 0x00, 0xe0, 0x4f, 0x00, 0x00, 0x01 }; /* host behind us */
static const uint8_t BC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t MC[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x45 }; /* 239.0.0.69 */

static struct umac_mesh_pathtbl T;
static struct umac_mesh_rmc R;

static bool is_peer(const uint8_t *a, void *arg) { (void)arg; return memcmp(a, A, 6) == 0 || memcmp(a, B, 6) == 0; }

static struct umac_mesh_fwd_ctx ctx(bool fwd, uint32_t now)
{
    struct umac_mesh_fwd_ctx c = { .own_addr = W, .tbl = &T, .rmc = &R, .forwarding = fwd,
                                   .element_ttl = 31, .now_ms = now, .is_peer = is_peer };
    return c;
}

static struct umac_mesh_rx_frame group_from(const uint8_t *sa, const uint8_t *ta, uint32_t seq, uint8_t ttl, uint8_t ae, const uint8_t *e1)
{
    struct umac_mesh_rx_frame f; memset(&f, 0, sizeof(f));
    f.group = true; memcpy(f.addr1, BC, 6); memcpy(f.addr2, ta, 6); memcpy(f.addr3, sa, 6);
    f.mc.flags = ae; f.mc.ttl = ttl; f.mc.seq = seq; if (e1) memcpy(f.mc.eaddr1, e1, 6);
    return f;
}

static struct umac_mesh_rx_frame uni(const uint8_t *ra, const uint8_t *ta, const uint8_t *mda, const uint8_t *msa, uint32_t seq, uint8_t ttl, uint8_t ae, const uint8_t *e1, const uint8_t *e2)
{
    struct umac_mesh_rx_frame f; memset(&f, 0, sizeof(f));
    f.group = false; memcpy(f.addr1, ra, 6); memcpy(f.addr2, ta, 6); memcpy(f.addr3, mda, 6); memcpy(f.addr4, msa, 6);
    f.mc.flags = ae; f.mc.ttl = ttl; f.mc.seq = seq; if (e1) memcpy(f.mc.eaddr1, e1, 6); if (e2) memcpy(f.mc.eaddr2, e2, 6);
    return f;
}

int main(void)
{
    struct umac_mesh_fwd_rx_result r; struct umac_mesh_fwd_tx_result t;
    uint32_t now = 1000;
    umac_mesh_pathtbl_init(&T); umac_mesh_rmc_init(&R);
    struct umac_mesh_fwd_ctx c = ctx(true, now), leaf = ctx(false, now);

    /* ---- group receive --------------------------------------------------- */
    struct umac_mesh_rx_frame g = group_from(A, A, 10, 31, 0, NULL);
    umac_mesh_fwd_rx(&c, &g, &r);
    CHECK(r.verdict == UMAC_MESH_FWD_DELIVER_AND_FORWARD, "group from A: deliver AND forward");
    CHECK(memcmp(r.mesh_sa, A, 6) == 0 && memcmp(r.mesh_da, BC, 6) == 0, "group result carries mesh SA=A (the source, not the hop)");
    CHECK(memcmp(r.deliver_da, BC, 6) == 0 && memcmp(r.deliver_sa, A, 6) == 0, "delivered as bcast from A");
    CHECK(memcmp(r.fwd_ra, BC, 6) == 0 && r.fwd_mc.ttl == 30 && r.fwd_mc.seq == 10, "rebroadcast to bcast, ttl 30, same seq");
    umac_mesh_fwd_rx(&c, &g, &r);
    CHECK(r.verdict == UMAC_MESH_FWD_DROP && r.drop == UMAC_MESH_FWD_DROP_DUP, "the same group frame again is a duplicate");
    /* Our own rebroadcast heard back from B carries our SA: dropped as own. */
    struct umac_mesh_rx_frame echo = group_from(W, B, 99, 29, 0, NULL);
    umac_mesh_fwd_rx(&c, &echo, &r);
    CHECK(r.verdict == UMAC_MESH_FWD_DROP && r.drop == UMAC_MESH_FWD_DROP_OWN, "our own frame echoed back is dropped");
    /* A's frame relayed by B reaches us with SA=A, TA=B, same seq: duplicate. */
    struct umac_mesh_rx_frame viaB = group_from(A, B, 10, 30, 0, NULL);
    umac_mesh_fwd_rx(&c, &viaB, &r);
    CHECK(r.verdict == UMAC_MESH_FWD_DROP && r.drop == UMAC_MESH_FWD_DROP_DUP, "A's frame via B with the same seq is a duplicate -- no loop");
    /* ttl 0: dropped outright, as mac80211 does. */
    struct umac_mesh_rx_frame zero = group_from(A, A, 15, 0, 0, NULL);
    umac_mesh_fwd_rx(&c, &zero, &r);
    CHECK(r.verdict == UMAC_MESH_FWD_DROP && r.drop == UMAC_MESH_FWD_DROP_TTL0, "group at ttl 0 is dropped, not delivered");
    struct umac_mesh_rx_frame zero_u = uni(W, A, W, A, 16, 0, 0, NULL, NULL);
    umac_mesh_fwd_rx(&c, &zero_u, &r);
    CHECK(r.verdict == UMAC_MESH_FWD_DROP && r.drop == UMAC_MESH_FWD_DROP_TTL0, "unicast at ttl 0 is dropped too");
    /* ttl 1: delivered, not forwarded. */
    struct umac_mesh_rx_frame low = group_from(A, A, 11, 1, 0, NULL);
    umac_mesh_fwd_rx(&c, &low, &r);
    CHECK(r.verdict == UMAC_MESH_FWD_DELIVER && r.drop == UMAC_MESH_FWD_DROP_TTL, "group at ttl 1 is delivered but not forwarded");
    /* leaf: delivered, not forwarded. */
    struct umac_mesh_rx_frame g2 = group_from(A, A, 12, 31, 0, NULL);
    umac_mesh_fwd_rx(&leaf, &g2, &r);
    CHECK(r.verdict == UMAC_MESH_FWD_DELIVER && r.drop == UMAC_MESH_FWD_DROP_NO_FWD, "a leaf delivers a group frame and forwards nothing");
    /* AE 1: proxied source learned and delivered as the real source. */
    struct umac_mesh_rx_frame gp = group_from(A, A, 13, 31, UMAC_MESH_CTRL_AE_A4, HA);
    umac_mesh_fwd_rx(&c, &gp, &r);
    CHECK(r.verdict == UMAC_MESH_FWD_DELIVER_AND_FORWARD && memcmp(r.deliver_sa, HA, 6) == 0, "AE 1 group: delivered from the host behind A");
    const uint8_t *px = umac_mesh_proxy_lookup(&T, HA, now);
    CHECK(px != NULL && memcmp(px, A, 6) == 0, "and HA is now known to sit behind A");
    CHECK(umac_mesh_ctrl_ae(&r.fwd_mc) == UMAC_MESH_CTRL_AE_A4 && memcmp(r.fwd_mc.eaddr1, HA, 6) == 0, "rebroadcast keeps AE 1 and the proxied source");
    struct umac_mesh_rx_frame gbad = group_from(A, A, 14, 31, UMAC_MESH_CTRL_AE_A5A6, HA);
    umac_mesh_fwd_rx(&c, &gbad, &r);
    CHECK(r.verdict == UMAC_MESH_FWD_DROP && r.drop == UMAC_MESH_FWD_DROP_BAD_AE, "AE 2 on a group frame is refused");

    /* ---- unicast receive: for us ------------------------------------------ */
    struct umac_mesh_rx_frame u = uni(W, A, W, A, 20, 31, 0, NULL, NULL);
    umac_mesh_fwd_rx(&c, &u, &r);
    CHECK(r.verdict == UMAC_MESH_FWD_DELIVER && memcmp(r.deliver_da, W, 6) == 0 && memcmp(r.deliver_sa, A, 6) == 0, "unicast for us from A: delivered W<-A");
    struct umac_mesh_rx_frame up = uni(W, A, W, A, 21, 31, UMAC_MESH_CTRL_AE_A5A6, HW, HA);
    umac_mesh_fwd_rx(&c, &up, &r);
    CHECK(r.verdict == UMAC_MESH_FWD_DELIVER && memcmp(r.deliver_da, HW, 6) == 0 && memcmp(r.deliver_sa, HA, 6) == 0, "AE 2 unicast: delivered HW<-HA, the real ends");
    struct umac_mesh_rx_frame ubad = uni(W, A, W, A, 22, 31, UMAC_MESH_CTRL_AE_A4, HA, NULL);
    umac_mesh_fwd_rx(&c, &ubad, &r);
    CHECK(r.verdict == UMAC_MESH_FWD_DROP && r.drop == UMAC_MESH_FWD_DROP_BAD_AE, "AE 1 on a unicast is refused");
    struct umac_mesh_rx_frame notus = uni(B, A, B, A, 23, 31, 0, NULL, NULL);
    umac_mesh_fwd_rx(&c, &notus, &r);
    CHECK(r.verdict == UMAC_MESH_FWD_DROP && r.drop == UMAC_MESH_FWD_DROP_NOT_FOR_US, "unicast with RA != us is not ours to act on");

    /* ---- unicast receive: for a third party ------------------------------- */
    /* A -> C via us, and we know C via B. */
    umac_mesh_path_update(&T, C, B, 5, 200, 1, 5120, now);
    struct umac_mesh_rx_frame f3 = uni(W, A, C, A, 30, 31, 0, NULL, NULL);
    umac_mesh_fwd_rx(&c, &f3, &r);
    CHECK(r.verdict == UMAC_MESH_FWD_FORWARD, "unicast for C is forwarded");
    CHECK(memcmp(r.fwd_ra, B, 6) == 0, "to B, the next hop for C");
    CHECK(memcmp(r.mesh_da, C, 6) == 0 && memcmp(r.mesh_sa, A, 6) == 0, "result carries mesh DA=C, SA=A for the relay to keep");
    CHECK(r.fwd_mc.ttl == 30 && r.fwd_mc.seq == 30 && umac_mesh_ctrl_ae(&r.fwd_mc) == 0, "ttl-1, seq and AE carried");
    /* Direct peer without a path entry: still forwardable. */
    struct umac_mesh_rx_frame f4 = uni(W, A, B, A, 31, 31, 0, NULL, NULL);
    umac_mesh_fwd_rx(&c, &f4, &r);
    CHECK(r.verdict == UMAC_MESH_FWD_FORWARD && memcmp(r.fwd_ra, B, 6) == 0, "unicast for peer B (no path entry) goes straight to B");
    /* No path, not a peer: PERR back to the transmitter. */
    const uint8_t X[6] = { 0x02, 0, 0, 0, 0, 0xee };
    struct umac_mesh_rx_frame f5 = uni(W, A, X, A, 32, 31, 0, NULL, NULL);
    umac_mesh_fwd_rx(&c, &f5, &r);
    CHECK(r.verdict == UMAC_MESH_FWD_DROP && r.drop == UMAC_MESH_FWD_DROP_NO_PATH, "no path to X: dropped");
    CHECK(r.send_perr && memcmp(r.perr_to, A, 6) == 0, "and a PERR goes back to A, the transmitter");
    { struct hwmp_perr e; CHECK(umac_mesh_hwmp_parse_perr(r.perr_body, r.perr_len, &e) && memcmp(e.dest_addr, X, 6) == 0 && e.dest_sn == 0 && e.reason == HWMP_REASON_MESH_PATH_ERROR_NO_FORWARDING, "PERR names X, sn 0, reason no-forwarding"); }
    /* Forwarding off: nothing, no PERR. */
    umac_mesh_fwd_rx(&leaf, &f3, &r);
    CHECK(r.verdict == UMAC_MESH_FWD_DROP && r.drop == UMAC_MESH_FWD_DROP_NO_FWD && !r.send_perr, "a leaf drops a frame for C and sends no PERR");
    /* ttl 1: dropped. */
    struct umac_mesh_rx_frame f6 = uni(W, A, C, A, 33, 1, 0, NULL, NULL);
    umac_mesh_fwd_rx(&c, &f6, &r);
    CHECK(r.verdict == UMAC_MESH_FWD_DROP && r.drop == UMAC_MESH_FWD_DROP_TTL, "unicast at ttl 1 is not forwarded");
    /* AE 2 through us: proxied source learned behind the mesh SA, endpoints carried. */
    struct umac_mesh_rx_frame f7 = uni(W, A, C, A, 34, 31, UMAC_MESH_CTRL_AE_A5A6, HW, HA);
    umac_mesh_fwd_rx(&c, &f7, &r);
    CHECK(r.verdict == UMAC_MESH_FWD_FORWARD && umac_mesh_ctrl_ae(&r.fwd_mc) == UMAC_MESH_CTRL_AE_A5A6 && memcmp(r.fwd_mc.eaddr2, HA, 6) == 0, "AE 2 forwarded intact");

    /* ---- transmit ------------------------------------------------------------- */
    umac_mesh_pathtbl_init(&T);
    umac_mesh_fwd_tx(&c, A, W, 100, &t);
    CHECK(t.ok && t.shape == UMAC_MESH_TX_UNICAST_4ADDR && memcmp(t.ra, A, 6) == 0 && memcmp(t.addr3, A, 6) == 0 && memcmp(t.addr4, W, 6) == 0, "W->A: plain 4-addr to the peer");
    CHECK(umac_mesh_ctrl_ae(&t.mc) == 0 && t.mc.ttl == 31 && t.mc.seq == 100, "no AE, ttl 31, seq 100");
    umac_mesh_path_update(&T, C, B, 5, 200, 1, 5120, now);
    umac_mesh_fwd_tx(&c, C, W, 101, &t);
    CHECK(t.ok && memcmp(t.ra, B, 6) == 0 && memcmp(t.addr3, C, 6) == 0, "W->C: RA is B (next hop), mesh DA is C");
    umac_mesh_fwd_tx(&c, X, W, 102, &t);
    CHECK(!t.ok && t.need_path && memcmp(t.path_target, X, 6) == 0, "W->X with no path: ask for a PREQ to X");
    umac_mesh_proxy_learn(&T, HA, A, now);
    umac_mesh_fwd_tx(&c, HA, W, 103, &t);
    CHECK(t.ok && memcmp(t.ra, A, 6) == 0 && memcmp(t.addr3, A, 6) == 0, "W->HA: goes to A, the node HA sits behind");
    CHECK(umac_mesh_ctrl_ae(&t.mc) == UMAC_MESH_CTRL_AE_A5A6 && memcmp(t.mc.eaddr1, HA, 6) == 0 && memcmp(t.mc.eaddr2, W, 6) == 0, "with AE 2: proxied DA=HA, SA=W");
    umac_mesh_fwd_tx(&c, A, HW, 104, &t);
    CHECK(t.ok && memcmp(t.ra, A, 6) == 0 && umac_mesh_ctrl_ae(&t.mc) == UMAC_MESH_CTRL_AE_A5A6 && memcmp(t.mc.eaddr2, HW, 6) == 0, "HW->A (bridged host originates): AE 2 carries HW as SA");
    umac_mesh_fwd_tx(&c, HA, HW, 105, &t);
    CHECK(t.ok && memcmp(t.ra, A, 6) == 0 && memcmp(t.mc.eaddr1, HA, 6) == 0 && memcmp(t.mc.eaddr2, HW, 6) == 0, "HW->HA host to host: to A with both ends in AE 2");
    umac_mesh_fwd_tx(&c, BC, W, 106, &t);
    CHECK(t.ok && t.shape == UMAC_MESH_TX_GROUP_3ADDR && memcmp(t.ra, BC, 6) == 0 && memcmp(t.addr3, W, 6) == 0 && umac_mesh_ctrl_ae(&t.mc) == 0, "W->bcast: 3-addr group, SA=W, no AE");
    umac_mesh_fwd_tx(&c, MC, HW, 107, &t);
    CHECK(t.ok && t.shape == UMAC_MESH_TX_GROUP_3ADDR && memcmp(t.ra, MC, 6) == 0 && umac_mesh_ctrl_ae(&t.mc) == UMAC_MESH_CTRL_AE_A4 && memcmp(t.mc.eaddr1, HW, 6) == 0, "HW->multicast: 3-addr group with AE 1 carrying HW");

    /* ---- round trip: what we shape, we would deliver correctly -------------- */
    {
        umac_mesh_fwd_tx(&c, HA, HW, 108, &t);
        struct umac_mesh_rx_frame back = uni(A, W, t.addr3, t.addr4, t.mc.seq, t.mc.ttl, t.mc.flags, t.mc.eaddr1, t.mc.eaddr2);
        /* Pretend we are A receiving it. */
        struct umac_mesh_fwd_ctx as_a = ctx(true, now); as_a.own_addr = A;
        umac_mesh_fwd_rx(&as_a, &back, &r);
        CHECK(r.verdict == UMAC_MESH_FWD_DELIVER && memcmp(r.deliver_da, HA, 6) == 0 && memcmp(r.deliver_sa, HW, 6) == 0, "A delivers HW->HA to its host with the real endpoints");
        CHECK(memcmp(umac_mesh_proxy_lookup(&T, HW, now), W, 6) == 0, "and A learned HW sits behind W");
    }

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_mesh_fwd: all passed\n");
    return 0;
}
