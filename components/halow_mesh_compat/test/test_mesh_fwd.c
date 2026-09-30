/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Mesh data-plane decisions (morselib src/umac/mesh/umac_mesh_fwd.c).
 *
 * Receive: a group frame is delivered AND rebroadcast with ttl - 1, once (the
 * cache catches the echo); a unicast for us is delivered with the proxied
 * endpoints if AE 2 carried them; a unicast for a third party is forwarded to
 * the path's next hop with ttl - 1, or held for discovery when there is no
 * path (no PERR) unless its mesh DA is a group address, which is dropped;
 * nothing is forwarded or held with forwarding off or at
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

/* B alone: A has gone away. */
static bool only_b(const uint8_t *a, void *arg) { (void)arg; return memcmp(a, B, 6) == 0; }

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
    /* No path, not a peer: held while we discover X (OpenMANET 999-0027), no PERR. */
    const uint8_t X[6] = { 0x02, 0, 0, 0, 0, 0xee };
    struct umac_mesh_rx_frame f5 = uni(W, A, X, A, 32, 31, UMAC_MESH_CTRL_AE_A5A6, HW, HA);
    umac_mesh_fwd_rx(&c, &f5, &r);
    CHECK(r.verdict == UMAC_MESH_FWD_HOLD && r.drop == UMAC_MESH_FWD_DROP_NO_PATH,
          "no path to X: HELD for discovery, reason no-path (verdict %d)", (int)r.verdict);
    CHECK(r.verdict == UMAC_MESH_FWD_HOLD && memcmp(r.mesh_da, X, 6) == 0 && memcmp(r.mesh_sa, A, 6) == 0 &&
          r.fwd_mc.ttl == 30 && r.fwd_mc.seq == 32 && umac_mesh_ctrl_ae(&r.fwd_mc) == UMAC_MESH_CTRL_AE_A5A6 &&
          memcmp(r.fwd_mc.eaddr1, HW, 6) == 0 && memcmp(r.fwd_mc.eaddr2, HA, 6) == 0,
          "the hold carries mesh DA X, SA A, ttl-1, seq and AE 2 unchanged -- the copy the PREP releases");
    /* A group mesh DA on a unicast is never discovered: mac80211's mesh_path_add
     * refuses a multicast destination, and a broadcast Target Only PREQ reads as a
     * root announcement that even forwarding-off nodes re-flood. */
    {
        struct umac_mesh_rx_frame fm = uni(W, A, MC, A, 35, 31, 0, NULL, NULL);
        umac_mesh_fwd_rx(&c, &fm, &r);
        const bool mc_dropped = r.verdict == UMAC_MESH_FWD_DROP && r.drop == UMAC_MESH_FWD_DROP_NO_PATH;
        struct umac_mesh_rx_frame fb = uni(W, A, BC, A, 36, 31, 0, NULL, NULL);
        umac_mesh_fwd_rx(&c, &fb, &r);
        CHECK(mc_dropped && r.verdict == UMAC_MESH_FWD_DROP && r.drop == UMAC_MESH_FWD_DROP_NO_PATH,
              "a unicast whose mesh DA is a group address (multicast, broadcast) is dropped as no-path, "
              "never held (verdict %d)", (int)r.verdict);
    }
    /* Forwarding off: nothing, never held. */
    umac_mesh_fwd_rx(&leaf, &f3, &r);
    CHECK(r.verdict == UMAC_MESH_FWD_DROP && r.drop == UMAC_MESH_FWD_DROP_NO_FWD, "a leaf drops a frame for C");
    umac_mesh_fwd_rx(&leaf, &f5, &r);
    CHECK(r.verdict == UMAC_MESH_FWD_DROP && r.drop == UMAC_MESH_FWD_DROP_NO_FWD,
          "and one for unroutable X: a leaf never holds or discovers (verdict %d)", (int)r.verdict);
    { struct umac_mesh_rx_frame f5t = f5; f5t.mc.ttl = 1; umac_mesh_fwd_rx(&c, &f5t, &r);
      CHECK(r.verdict == UMAC_MESH_FWD_DROP && r.drop == UMAC_MESH_FWD_DROP_TTL,
            "unroutable X at ttl 1 is dropped for TTL, not held"); }
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

    /* ---- replica normalisation: the per-peer unicast form of a group frame --- */
    {
        /* Arrives 4-addr, RA=us, TA=A, addr3=us (the chip's addr3 filter), addr4=A,
         * AE 2 with eaddr1 = the group, eaddr2 = the host behind A. */
        struct umac_mesh_rx_frame rep = uni(W, A, W, A, 40, 31, UMAC_MESH_CTRL_AE_A5A6, BC, HA);
        CHECK(umac_mesh_fwd_normalise_replica(&rep), "AE 2 with a group extension DA is a replica");
        CHECK(rep.group && memcmp(rep.addr1, BC, 6) == 0, "rewritten as a group frame to bcast");
        CHECK(memcmp(rep.addr3, A, 6) == 0, "mesh SA is A (from addr4)");
        CHECK(umac_mesh_ctrl_ae(&rep.mc) == UMAC_MESH_CTRL_AE_A4 && memcmp(rep.mc.eaddr1, HA, 6) == 0, "AE 1 now carries the host behind A");
        umac_mesh_fwd_rx(&c, &rep, &r);
        CHECK(r.verdict == UMAC_MESH_FWD_DELIVER_AND_FORWARD && memcmp(r.deliver_da, BC, 6) == 0 && memcmp(r.deliver_sa, HA, 6) == 0,
              "and the engine delivers it as a broadcast from HA and forwards it");
        CHECK(memcmp(r.mesh_sa, A, 6) == 0, "with mesh SA = A, so the next hop's cache sees the same identity");
        struct umac_mesh_rx_frame notrep = uni(W, A, W, A, 41, 31, UMAC_MESH_CTRL_AE_A5A6, HW, HA);
        CHECK(!umac_mesh_fwd_normalise_replica(&notrep) && !notrep.group, "AE 2 with a unicast extension DA is left alone");
        struct umac_mesh_rx_frame plain = uni(W, A, W, A, 42, 31, 0, NULL, NULL);
        CHECK(!umac_mesh_fwd_normalise_replica(&plain), "no AE is left alone");
    }

    /* ---- on-air shaping: the header bytes the firmware emits ---------------- */
    {
        uint8_t h[UMAC_MESH_DATA_HDR4_LEN];
        /* Plain unicast W -> A: 4-address, addr3 = A, addr4 = W. */
        struct umac_mesh_tx_hdr_in in = { .ra = A, .own = W, .dst8023 = A, .src8023 = W };
        CHECK(umac_mesh_fwd_tx_header(&in, h) == 30, "unicast header is 30 octets");
        CHECK(h[0] == 0x88 && h[1] == 0x03, "FC = QoS data, ToDS+FromDS (88 03)");
        CHECK(memcmp(&h[4], A, 6) == 0 && memcmp(&h[10], W, 6) == 0 && memcmp(&h[16], A, 6) == 0 && memcmp(&h[24], W, 6) == 0,
              "addr1=A addr2=W addr3=A addr4=W");
        /* Replicated group frame to peer A: addr3 is the PEER (the chip filters on it). */
        struct umac_mesh_tx_hdr_in g = { .ra = A, .own = W, .dst8023 = BC, .src8023 = W };
        CHECK(umac_mesh_fwd_tx_header(&g, h) == 30 && memcmp(&h[16], A, 6) == 0 && memcmp(&h[24], W, 6) == 0,
              "replicated group frame: 4-addr, addr3 = the peer, addr4 = us");
        /* Standard group frame: 3-address, addr1 = group, addr3 = us. */
        g.grp_std = true;
        CHECK(umac_mesh_fwd_tx_header(&g, h) == 24, "grp_std group header is 24 octets");
        CHECK(h[0] == 0x88 && h[1] == 0x02, "FC = QoS data, FromDS only (88 02)");
        CHECK(memcmp(&h[4], BC, 6) == 0 && memcmp(&h[10], W, 6) == 0 && memcmp(&h[16], W, 6) == 0, "addr1=bcast addr2=W addr3=W");
        /* Relayed STANDARD group frame from A through W: addr3 stays A, TA is W. */
        struct umac_mesh_tx_hdr_in rs = { .ra = BC, .own = W, .dst8023 = BC, .src8023 = A,
                                          .sidecar_valid = true, .mesh_da = BC, .mesh_sa = A, .grp_std = true };
        CHECK(umac_mesh_fwd_tx_header(&rs, h) == 24 && memcmp(&h[10], W, 6) == 0 && memcmp(&h[16], A, 6) == 0,
              "relayed 3-addr group frame: addr2 = W (TA), addr3 = A (the original mesh source)");
        /* Relayed unicast A -> C through W: sidecar keeps the ORIGINAL endpoints. */
        struct umac_mesh_tx_hdr_in rl = { .ra = B, .own = W, .dst8023 = C, .src8023 = A,
                                          .sidecar_valid = true, .mesh_da = C, .mesh_sa = A };
        CHECK(umac_mesh_fwd_tx_header(&rl, h) == 30 && memcmp(&h[4], B, 6) == 0 && memcmp(&h[10], W, 6) == 0 &&
              memcmp(&h[16], C, 6) == 0 && memcmp(&h[24], A, 6) == 0, "relayed: RA=B TA=W addr3=C addr4=A (not us)");
        /* Relayed group replica: sidecar mesh_da is the group, so addr3 stays the peer. */
        struct umac_mesh_tx_hdr_in rg = { .ra = B, .own = W, .dst8023 = BC, .src8023 = A,
                                          .sidecar_valid = true, .mesh_da = BC, .mesh_sa = A };
        CHECK(umac_mesh_fwd_tx_header(&rg, h) == 30 && memcmp(&h[16], B, 6) == 0 && memcmp(&h[24], A, 6) == 0,
              "relayed group replica: addr3 = the peer, addr4 = the original source A");
        CHECK(umac_mesh_fwd_tx_header(NULL, h) == 0, "NULL input refused");
    }
    /* ---- replica Mesh Control rule ------------------------------------------ */
    {
        struct umac_mesh_ctrl nat = { .flags = 0, .ttl = 30, .seq = 77 }, rep;
        umac_mesh_fwd_replica_ctrl(&nat, BC, HW, &rep);
        CHECK(umac_mesh_ctrl_ae(&rep) == UMAC_MESH_CTRL_AE_A5A6 && rep.ttl == 30 && rep.seq == 77, "replica keeps ttl/seq, gains AE 2");
        CHECK(memcmp(rep.eaddr1, BC, 6) == 0 && memcmp(rep.eaddr2, HW, 6) == 0, "AE 2 = group DA, real source");
    }
    /* ---- frame parser: bytes -> rx_frame, the receive side of the same layout - */
    {
        uint8_t frame[80]; struct umac_mesh_rx_frame pf; uint16_t n = 0, used;
        struct umac_mesh_tx_hdr_in in = { .ra = W, .own = A, .dst8023 = W, .src8023 = A };
        n = umac_mesh_fwd_tx_header(&in, frame);
        frame[n] = 0x00; frame[n + 1] = 0x01; n += 2;             /* QoS: tid 0, Mesh Control Present */
        struct umac_mesh_ctrl mc = { .flags = UMAC_MESH_CTRL_AE_A5A6, .ttl = 9, .seq = 5 };
        memcpy(mc.eaddr1, HW, 6); memcpy(mc.eaddr2, HA, 6);
        n += umac_mesh_ctrl_build(&frame[n], 32, &mc);
        used = umac_mesh_fwd_parse_frame(frame, n, &pf);
        CHECK(used == 30 + 2 + 18, "4-addr + QoS + AE2 parses to 50 octets (got %u)", used);
        CHECK(!pf.group && memcmp(pf.addr1, W, 6) == 0 && memcmp(pf.addr2, A, 6) == 0 && memcmp(pf.addr3, W, 6) == 0 && memcmp(pf.addr4, A, 6) == 0,
              "addresses recovered");
        CHECK(pf.mc.ttl == 9 && pf.mc.seq == 5 && memcmp(pf.mc.eaddr2, HA, 6) == 0, "Mesh Control recovered");
        umac_mesh_fwd_rx(&c, &pf, &r);
        CHECK(r.verdict == UMAC_MESH_FWD_DELIVER && memcmp(r.deliver_da, HW, 6) == 0 && memcmp(r.deliver_sa, HA, 6) == 0,
              "and the parsed frame is delivered HW<-HA");
        /* group form */
        struct umac_mesh_tx_hdr_in gi = { .ra = BC, .own = A, .dst8023 = BC, .src8023 = A, .grp_std = true };
        n = umac_mesh_fwd_tx_header(&gi, frame); frame[n] = 0x00; frame[n + 1] = 0x01; n += 2;
        struct umac_mesh_ctrl gm = { .flags = 0, .ttl = 31, .seq = 6 };
        n += umac_mesh_ctrl_build(&frame[n], 32, &gm);
        CHECK(umac_mesh_fwd_parse_frame(frame, n, &pf) == 24 + 2 + 6 && pf.group && memcmp(pf.addr3, A, 6) == 0, "3-addr group parses: 32 octets, SA = A");
        /* refusals */
        CHECK(umac_mesh_fwd_parse_frame(frame, 31, &pf) == 0, "short frame refused");
        frame[25] = 0x00; /* clear Mesh Control Present */
        CHECK(umac_mesh_fwd_parse_frame(frame, n, &pf) == 0, "no Mesh Control Present bit: refused");
        frame[25] = 0x01; frame[1] = 0x01; /* ToDS only */
        CHECK(umac_mesh_fwd_parse_frame(frame, n, &pf) == 0, "ToDS-only is not a mesh shape");
        frame[1] = 0x02; frame[4] = 0x02; /* 3-addr with a unicast addr1 */
        CHECK(umac_mesh_fwd_parse_frame(frame, n, &pf) == 0, "3-addr with unicast addr1 refused");
    }

    /* ---- discovery rate limit ------------------------------------------------ */
    {
        struct umac_mesh_preq_gate g; umac_mesh_preq_gate_init(&g);
        uint32_t t0 = 5000;
        CHECK( umac_mesh_preq_gate_allow(&g, X, t0), "first PREQ for X allowed");
        CHECK(!umac_mesh_preq_gate_allow(&g, X, t0 + 100), "X again 100 ms later: suppressed");
        CHECK(!umac_mesh_preq_gate_allow(&g, X, t0 + UMAC_MESH_PREQ_MIN_INTERVAL_MS - 1), "one ms short of the interval: suppressed");
        CHECK(!umac_mesh_preq_gate_allow(&g, C, t0 + 10), "a DIFFERENT target 10 ms later: the global floor suppresses it");
        CHECK( umac_mesh_preq_gate_allow(&g, C, t0 + UMAC_MESH_PREQ_GLOBAL_MIN_MS), "and allows it at the floor");
        CHECK( umac_mesh_preq_gate_allow(&g, X, t0 + UMAC_MESH_PREQ_MIN_INTERVAL_MS), "X allowed again after its interval");
        /* What the relay ladder asks: when did the last PREQ for this target go out?
         * An allowed PREQ the radio refused is not reported, so it is not "last". */
        const uint32_t t1 = t0 + UMAC_MESH_PREQ_MIN_INTERVAL_MS;
        uint32_t last = 0;
        CHECK(!umac_mesh_preq_gate_last(&g, X, &last), "allowed but never reported sent: no last PREQ for X");
        umac_mesh_preq_gate_sent(&g, X, t1);
        CHECK(umac_mesh_preq_gate_last(&g, X, &last) && last == t1 && !umac_mesh_preq_gate_last(&g, A, &last) &&
                  !umac_mesh_preq_gate_last(NULL, X, &last) && !umac_mesh_preq_gate_last(&g, X, NULL),
              "reported sent: X's last PREQ is at %lu; never one for an unasked target", (unsigned long)last);
        CHECK(umac_mesh_preq_gate_allow(&g, X, t1 + UMAC_MESH_PREQ_MIN_INTERVAL_MS) &&
                  umac_mesh_preq_gate_last(&g, X, &last) && last == t1,
              "a later PREQ allowed but not sent leaves the last one at %lu", (unsigned long)last);
        umac_mesh_preq_gate_sent(&g, A, t1); /* A holds no slot: nothing recorded */
        CHECK(!umac_mesh_preq_gate_last(&g, A, &last), "a send for a target the gate never allowed is not recorded");
        CHECK(UMAC_MESH_PREQ_MIN_INTERVAL_MS <= UMAC_MESH_RELAY_DISC_FIRST_MS,
              "the per-target interval (%u ms) leaves room for the ladder's first re-ask (%u ms)",
              (unsigned)UMAC_MESH_PREQ_MIN_INTERVAL_MS, (unsigned)UMAC_MESH_RELAY_DISC_FIRST_MS);
        /* LRU across the slots, stepped at the global floor: a target that no longer fits
         * evicts the least recently asked, whose interval has always run out by then. */
        enum { NT = UMAC_MESH_PREQ_TARGETS + 2 };
        uint8_t T[NT][6];
        for (int i = 0; i < NT; i++) { const uint8_t a[6] = { 2, 0, 0, 0, 0, (uint8_t)(i + 1) }; memcpy(T[i], a, 6); }
        umac_mesh_preq_gate_init(&g);
        const uint32_t step = UMAC_MESH_PREQ_GLOBAL_MIN_MS;
        uint32_t t = 10000;
        bool filled = true;
        for (int i = 0; i < (int)UMAC_MESH_PREQ_TARGETS; i++) { filled = filled && umac_mesh_preq_gate_allow(&g, T[i], t); t += step; }
        CHECK(filled, "%u targets fill the %u slots, one per floor", (unsigned)UMAC_MESH_PREQ_TARGETS,
              (unsigned)UMAC_MESH_PREQ_TARGETS);
        const int over = (int)UMAC_MESH_PREQ_TARGETS;
        CHECK(!umac_mesh_preq_gate_allow(&g, T[1], t) && umac_mesh_preq_gate_allow(&g, T[over], t),
              "T1 is still remembered and suppressed; one target more evicts the least recently used, T0");
        t += step;
        CHECK(t - 10000u >= UMAC_MESH_PREQ_MIN_INTERVAL_MS && umac_mesh_preq_gate_allow(&g, T[0], t),
              "the evicted T0 is asked again %lu ms after its last PREQ: never inside its %u ms interval",
              (unsigned long)(t - 10000u), (unsigned)UMAC_MESH_PREQ_MIN_INTERVAL_MS);
        {
            uint32_t last = 0;
            umac_mesh_preq_gate_sent(&g, T[2], 10000u + 2u * step);
            CHECK(umac_mesh_preq_gate_last(&g, T[2], &last) && last == 10000u + 2u * step, "T2 went out");
            t += step;
            CHECK(umac_mesh_preq_gate_allow(&g, T[over + 1], t) && !umac_mesh_preq_gate_last(&g, T[over + 1], &last) &&
                      !umac_mesh_preq_gate_last(&g, T[2], &last),
                  "a new target takes the least recently used slot, T2's, without inheriting its send");
        }
        /* A scanner cycling k targets, one attempt per floor: every attempt is allowed exactly
         * when that target has none in the last interval, for every k, the table's size included. */
        {
            bool exact = true;
            for (uint32_t k = 1; k <= 2u * UMAC_MESH_PREQ_TARGETS && exact; k++)
            {
                uint32_t prev[2 * UMAC_MESH_PREQ_TARGETS] = { 0 };
                bool seen[2 * UMAC_MESH_PREQ_TARGETS] = { false };
                umac_mesh_preq_gate_init(&g);
                for (uint32_t n = 0; n < 8u * k + 16u && exact; n++)
                {
                    const uint32_t now = 40000u + n * UMAC_MESH_PREQ_GLOBAL_MIN_MS, i = n % k;
                    const uint8_t tg[6] = { 2, 0, 0, 0, 7, (uint8_t)i };
                    const bool want = !seen[i] || now - prev[i] >= UMAC_MESH_PREQ_MIN_INTERVAL_MS;
                    if (umac_mesh_preq_gate_allow(&g, tg, now) != want)
                    {
                        printf("     k=%lu: target %lu at +%lu ms %s\n", (unsigned long)k, (unsigned long)i,
                               (unsigned long)(seen[i] ? now - prev[i] : 0u), want ? "suppressed" : "allowed");
                        exact = false;
                    }
                    else if (want)
                    {
                        seen[i] = true;
                        prev[i] = now;
                    }
                }
            }
            CHECK(exact, "cycling 1..%u targets: at most one PREQ per target per %u ms, and one as soon as it is due",
                  2u * (unsigned)UMAC_MESH_PREQ_TARGETS, (unsigned)UMAC_MESH_PREQ_MIN_INTERVAL_MS);
        }
        /* The bound: a flood alternating eight unknown targets for one second
         * yields at most 1000 / floor PREQs, however many targets it cycles. */
        umac_mesh_preq_gate_init(&g);
        int sent = 0;
        for (uint32_t ms = 0; ms < 1000; ms += 5) { if (umac_mesh_preq_gate_allow(&g, T[(ms / 5) % 6], 20000 + ms)) sent++; }
        CHECK(sent <= 1000 / UMAC_MESH_PREQ_GLOBAL_MIN_MS && sent >= 10, "alternating-target flood for 1 s: %d PREQs (bound %u)", sent, 1000 / UMAC_MESH_PREQ_GLOBAL_MIN_MS);
        /* Wrap-safe clock. */
        umac_mesh_preq_gate_init(&g);
        CHECK( umac_mesh_preq_gate_allow(&g, X, 0xfffffff0u), "PREQ just before the clock wraps");
        CHECK(!umac_mesh_preq_gate_allow(&g, X, 0x00000010u), "32 ms later across the wrap: still suppressed");
        CHECK( umac_mesh_preq_gate_allow(&g, X, 0x00000010u + UMAC_MESH_PREQ_MIN_INTERVAL_MS), "and allowed after the interval, across the wrap");
        CHECK(!umac_mesh_preq_gate_allow(NULL, X, 1) && !umac_mesh_preq_gate_allow(&g, NULL, 1), "NULLs suppressed");
    }

    /* ---- precedence: a mesh node is never treated as a host behind another --- */
    {
        umac_mesh_pathtbl_init(&T);
        /* A peer claims via AE 1 that our direct neighbour B is a host behind it. */
        struct umac_mesh_rx_frame hij = group_from(A, A, 60, 31, UMAC_MESH_CTRL_AE_A4, B);
        umac_mesh_fwd_rx(&c, &hij, &r);
        CHECK(umac_mesh_proxy_lookup(&T, B, now) == NULL, "a claim that peer B is a host behind A is NOT learned");
        umac_mesh_fwd_tx(&c, B, W, 200, &t);
        CHECK(t.ok && memcmp(t.ra, B, 6) == 0 && umac_mesh_ctrl_ae(&t.mc) == 0, "and W->B still goes straight to B, no AE");
        /* Nor ourselves, nor a node we hold a path to, nor a group address. */
        struct umac_mesh_rx_frame self = group_from(A, A, 61, 31, UMAC_MESH_CTRL_AE_A4, W);
        umac_mesh_fwd_rx(&c, &self, &r);
        CHECK(umac_mesh_proxy_lookup(&T, W, now) == NULL, "a claim that WE are a host behind A is not learned");
        umac_mesh_path_update(&T, C, B, 5, 200, 1, 5120, now);
        struct umac_mesh_rx_frame viaA = group_from(A, A, 62, 31, UMAC_MESH_CTRL_AE_A4, C);
        umac_mesh_fwd_rx(&c, &viaA, &r);
        CHECK(umac_mesh_proxy_lookup(&T, C, now) == NULL, "a node we hold a path to is not learned as a host");
        umac_mesh_fwd_tx(&c, C, W, 201, &t);
        CHECK(t.ok && memcmp(t.ra, B, 6) == 0 && memcmp(t.addr3, C, 6) == 0 && umac_mesh_ctrl_ae(&t.mc) == 0, "W->C uses the path, not a proxy");
        struct umac_mesh_rx_frame grp = group_from(A, A, 63, 31, UMAC_MESH_CTRL_AE_A4, MC);
        umac_mesh_fwd_rx(&c, &grp, &r);
        CHECK(umac_mesh_proxy_lookup(&T, MC, now) == NULL, "a group address is not learned as a host");
        /* A real host is still learned and still wins for a non-mesh address. */
        struct umac_mesh_rx_frame real = group_from(A, A, 64, 31, UMAC_MESH_CTRL_AE_A4, HA);
        umac_mesh_fwd_rx(&c, &real, &r);
        CHECK(umac_mesh_proxy_lookup(&T, HA, now) != NULL, "a genuine host behind A is learned");
    }
    /* ---- path refresh ------------------------------------------------------- */
    {
        umac_mesh_pathtbl_init(&T);
        umac_mesh_path_update(&T, C, B, 5, 200, 1, 5120, now);
        struct umac_mesh_fwd_ctx late = c; late.now_ms = now + 5120 - 500;
        umac_mesh_fwd_tx(&late, C, W, 300, &t);
        CHECK(t.ok && memcmp(t.ra, B, 6) == 0, "a path 500 ms from expiry is still used");
        CHECK(t.refresh && memcmp(t.path_target, C, 6) == 0, "and the caller is told to refresh it");
        struct umac_mesh_fwd_ctx early = c; early.now_ms = now + 1000;
        umac_mesh_fwd_tx(&early, C, W, 301, &t);
        CHECK(t.ok && !t.refresh, "a path with 4 s left is not refreshed");
        /* For a proxied destination the PREQ must name the mesh NODE, never
         * the host: a PREQ for a host address can never be answered. */
        umac_mesh_pathtbl_init(&T);
        umac_mesh_proxy_learn(&T, HA, A, now);
        umac_mesh_path_update(&T, A, B, 5, 200, 1, 5120, now);
        struct umac_mesh_fwd_ctx plate = c; plate.now_ms = now + 5120 - 500;
        umac_mesh_fwd_tx(&plate, HA, W, 302, &t);
        CHECK(t.ok && t.refresh && memcmp(t.path_target, A, 6) == 0, "refreshing a proxied destination asks for the NODE A, not the host HA");
    }
    /* ---- originating a PREQ: always broadcast ------------------------------- */
    {
        uint8_t body[HWMP_PREQ_BODY_LEN], ra[6]; uint32_t sn = 10, id = 0; struct hwmp_preq q;
        uint16_t n = umac_mesh_fwd_originate_preq(body, sizeof(body), W, &sn, &id, C, 4882, ra);
        CHECK(n == HWMP_PREQ_BODY_LEN, "PREQ originated");
        CHECK(ra[0] == 0xff && ra[5] == 0xff, "to BROADCAST -- a unicast to a non-neighbour target reaches nobody");
        CHECK(umac_mesh_hwmp_parse_preq(body, n, &q) && memcmp(q.target_addr, C, 6) == 0 && memcmp(q.orig_addr, W, 6) == 0, "target C, originator us");
        CHECK(sn == 11 && id == 1 && q.orig_sn == 11 && q.preq_id == 1, "our sn and preq id advanced and are in the frame");
    }

    /* ---- protection latch: trust on first protected frame ------------------ */
    {
        /* A protected frame is never refused, so asserting the return value of
         * a protected check proves nothing. Every assertion below reads the
         * latch back through a PLAINTEXT check, which is the only call that
         * can return false. */
        struct umac_mesh_prot_latch L; umac_mesh_prot_latch_init(&L);
        CHECK(umac_mesh_prot_latch_check(&L, A, false), "plaintext from A accepted while A has never protected (PMF off: today's only case)");
        (void)umac_mesh_prot_latch_check(&L, A, true);
        CHECK(!umac_mesh_prot_latch_check(&L, A, false), "one protected frame latches A: plaintext claiming to be A is now refused");
        CHECK(umac_mesh_prot_latch_check(&L, B, false), "B is judged on its own");
        umac_mesh_prot_latch_forget(&L, A);
        CHECK(umac_mesh_prot_latch_check(&L, A, false), "forgotten on peer loss: a re-peered A starts over");
        /* Fill the table exactly, then overflow it one peer at a time and name
         * the victim each time -- that pins the round-robin, which an
         * always-evict-the-same-slot mutant would otherwise pass. */
        umac_mesh_prot_latch_init(&L);
        uint8_t pr[UMAC_MESH_PROT_LATCH_MAX + 2][6];
        for (unsigned i = 0; i < UMAC_MESH_PROT_LATCH_MAX + 2; i++)
        {
            pr[i][0] = 2; pr[i][1] = 0; pr[i][2] = 0; pr[i][3] = 0; pr[i][4] = 0; pr[i][5] = (uint8_t)i;
        }
        for (unsigned i = 0; i < UMAC_MESH_PROT_LATCH_MAX; i++)
        {
            (void)umac_mesh_prot_latch_check(&L, pr[i], true);
        }
        for (unsigned i = 0; i < UMAC_MESH_PROT_LATCH_MAX; i++)
        {
            CHECK(!umac_mesh_prot_latch_check(&L, pr[i], false), "peer %u is latched: the table holds all %u", i, (unsigned)UMAC_MESH_PROT_LATCH_MAX);
        }
        (void)umac_mesh_prot_latch_check(&L, pr[UMAC_MESH_PROT_LATCH_MAX], true);
        CHECK(umac_mesh_prot_latch_check(&L, pr[0], false), "overflow evicted peer 0, the first slot");
        CHECK(!umac_mesh_prot_latch_check(&L, pr[1], false), "and left peer 1 alone");
        CHECK(!umac_mesh_prot_latch_check(&L, pr[UMAC_MESH_PROT_LATCH_MAX], false), "the newcomer is latched");
        (void)umac_mesh_prot_latch_check(&L, pr[UMAC_MESH_PROT_LATCH_MAX + 1], true);
        CHECK(umac_mesh_prot_latch_check(&L, pr[1], false), "the next overflow takes peer 1: the victim ROTATES, it is not a fixed slot");
        CHECK(!umac_mesh_prot_latch_check(&L, pr[2], false), "peer 2 is still latched");
        CHECK(umac_mesh_prot_latch_check(NULL, A, false) && umac_mesh_prot_latch_check(&L, NULL, false), "NULL fails open");
    }
    /* ---- pending: frames held for discovery ---------------------------------- */
    {
        umac_mesh_pathtbl_init(&T);
        struct umac_mesh_pending P; umac_mesh_pending_init(&P);
        int h[8];
        static const uint8_t D2[6] = { 0x02, 0xd2, 0, 0, 0, 1 }, D3[6] = { 0x02, 0xd3, 0, 0, 0, 1 }, D4[6] = { 0x02, 0xd4, 0, 0, 0, 1 };
        CHECK(umac_mesh_pending_push(&P, C, &h[0], now) == NULL, "first frame for C held");
        CHECK(umac_mesh_pending_push(&P, C, &h[1], now) == NULL, "second held");
        CHECK(umac_mesh_pending_push(&P, C, &h[2], now) == &h[0], "a third for C evicts the OLDEST for C (%u per target)", (unsigned)UMAC_MESH_PENDING_PER_TARGET);
        CHECK(umac_mesh_pending_push(&P, D2, &h[3], now) == NULL && umac_mesh_pending_push(&P, D3, &h[4], now) == NULL, "other targets fill the table");
        CHECK(umac_mesh_pending_push(&P, D4, &h[5], now) == &h[1], "table full: the oldest overall goes");
        CHECK(umac_mesh_pending_count(&P) == UMAC_MESH_PENDING_MAX, "%u held", (unsigned)UMAC_MESH_PENDING_MAX);
        struct umac_mesh_pending_out out[UMAC_MESH_PENDING_MAX];
        CHECK(umac_mesh_pending_take(&P, &c, out, UMAC_MESH_PENDING_MAX) == 0, "nothing is released without a path");
        umac_mesh_path_update(&T, C, B, 5, 200, 1, 5120, now);
        uint32_t k = umac_mesh_pending_take(&P, &c, out, UMAC_MESH_PENDING_MAX);
        CHECK(k == 1 && out[0].handle == &h[2] && out[0].ok && memcmp(out[0].ra, B, 6) == 0, "C's frame released to next hop B once the path exists");
        CHECK(umac_mesh_pending_count(&P) == 3, "the others still wait");
        struct umac_mesh_fwd_ctx late = c; late.now_ms = now + UMAC_MESH_PENDING_MS;
        k = umac_mesh_pending_take(&P, &late, out, UMAC_MESH_PENDING_MAX);
        CHECK(k == 3 && !out[0].ok && !out[1].ok && !out[2].ok, "after %u ms the rest are handed back to drop", (unsigned)UMAC_MESH_PENDING_MS);
        CHECK(umac_mesh_pending_count(&P) == 0, "store empty");
        CHECK(umac_mesh_pending_push(&P, B, &h[6], now) == NULL, "held for direct peer B (the caller had no STA yet)");
        k = umac_mesh_pending_take(&P, &c, out, UMAC_MESH_PENDING_MAX);
        CHECK(k == 1 && out[0].ok && memcmp(out[0].ra, B, 6) == 0, "a direct peer resolves at once");
        CHECK(umac_mesh_pending_push(NULL, C, &h[7], now) == &h[7], "NULL store hands the frame straight back");
        /* The targets still waiting, so the caller can re-ask: distinct, and
         * an expired entry is not worth another PREQ. */
        umac_mesh_pending_init(&P);
        uint8_t tg[UMAC_MESH_PENDING_MAX][6];
        static const uint8_t D9[6] = { 0x02, 0xd9, 0, 0, 0, 1 };
        (void)umac_mesh_pending_push(&P, C, &h[0], now);
        (void)umac_mesh_pending_push(&P, C, &h[1], now);
        (void)umac_mesh_pending_push(&P, D9, &h[2], now);
        uint32_t m = umac_mesh_pending_targets(&P, now, tg, UMAC_MESH_PENDING_MAX);
        CHECK(m == 2, "two frames for C and one for D report TWO targets, not three");
        CHECK((memcmp(tg[0], C, 6) == 0 && memcmp(tg[1], D9, 6) == 0) ||
              (memcmp(tg[1], C, 6) == 0 && memcmp(tg[0], D9, 6) == 0), "and they are C and D");
        CHECK(umac_mesh_pending_targets(&P, now + UMAC_MESH_PENDING_MS, tg, UMAC_MESH_PENDING_MAX) == 0,
              "nothing is re-asked once the frames have lapsed");
        CHECK(umac_mesh_pending_targets(NULL, now, tg, UMAC_MESH_PENDING_MAX) == 0, "NULL store reports nothing");
        /* A frame held for a HOST address is released through the proxy entry
         * that arrives later: the next hop is the path to the node, not the host. */
        umac_mesh_pathtbl_init(&T);
        umac_mesh_pending_init(&P);
        CHECK(umac_mesh_pending_push(&P, HA, &h[0], now) == NULL, "frame for host HA held (nothing known about HA yet)");
        CHECK(umac_mesh_pending_take(&P, &c, out, UMAC_MESH_PENDING_MAX) == 0, "still held: a host with no proxy has no route");
        umac_mesh_proxy_learn(&T, HA, A, now);
        umac_mesh_path_update(&T, A, B, 7, 300, 2, 5120, now);
        k = umac_mesh_pending_take(&P, &c, out, UMAC_MESH_PENDING_MAX);
        CHECK(k == 1 && out[0].ok && memcmp(out[0].ra, B, 6) == 0, "released via the proxy: HA sits behind A, whose next hop is B");
    }

    /* ---- relayed frames held for discovery: own cap, own ladder ----------------
     * The relay's store sits beside ours in the same struct; neither kind may
     * evict the other, and a relayed frame is resolved on its mesh DA only. */
    {
        umac_mesh_pathtbl_init(&T);
        struct umac_mesh_pending P; umac_mesh_pending_init(&P);
        int h[16];
        static const uint8_t O1[6] = { 0x02, 0x0a, 0, 0, 0, 1 }, O2[6] = { 0x02, 0x0a, 0, 0, 0, 2 },
                             O3[6] = { 0x02, 0x0a, 0, 0, 0, 3 }, O4[6] = { 0x02, 0x0a, 0, 0, 0, 4 },
                             O5[6] = { 0x02, 0x0a, 0, 0, 0, 5 };
        static const uint8_t X1[6] = { 0x02, 0x0e, 0, 0, 0, 1 }, X2[6] = { 0x02, 0x0e, 0, 0, 0, 2 },
                             X3[6] = { 0x02, 0x0e, 0, 0, 0, 3 };
        CHECK(umac_mesh_relay_ask_ms(0) == 0 && umac_mesh_relay_ask_ms(1) == 400 &&
              umac_mesh_relay_ask_ms(2) == 1200 && umac_mesh_relay_ask_ms(3) == 2800 &&
              umac_mesh_relay_ask_ms(4) == 4800 && umac_mesh_relay_ask_ms(5) == 6800 &&
              umac_mesh_relay_ask_ms(9) == 6800,
              "relay ladder: PREQs at 0, 0.4, 1.2, 2.8, 4.8 s, give up at 6.8 s (%u %u %u %u %u %u)",
              umac_mesh_relay_ask_ms(1), umac_mesh_relay_ask_ms(2), umac_mesh_relay_ask_ms(3),
              umac_mesh_relay_ask_ms(4), umac_mesh_relay_ask_ms(5), umac_mesh_relay_ask_ms(9));

        (void)umac_mesh_pending_push(&P, O1, &h[0], now);
        (void)umac_mesh_pending_push(&P, O2, &h[1], now);
        (void)umac_mesh_pending_push(&P, O3, &h[2], now);
        (void)umac_mesh_pending_push(&P, O4, &h[3], now);
        bool none = umac_mesh_pending_push_relayed(&P, X1, &h[4], now) == NULL &&
                    umac_mesh_pending_push_relayed(&P, X1, &h[5], now) == NULL &&
                    umac_mesh_pending_push_relayed(&P, X2, &h[6], now) == NULL &&
                    umac_mesh_pending_push_relayed(&P, X2, &h[7], now) == NULL;
        CHECK(none && umac_mesh_pending_count(&P) == UMAC_MESH_PENDING_MAX + UMAC_MESH_PENDING_RELAY_MAX &&
              umac_mesh_pending_count_relayed(&P) == UMAC_MESH_PENDING_RELAY_MAX,
              "a full store of ours still takes %u relayed frames beside it (%u held, %u relayed)",
              (unsigned)UMAC_MESH_PENDING_RELAY_MAX, umac_mesh_pending_count(&P),
              umac_mesh_pending_count_relayed(&P));
        CHECK(umac_mesh_pending_push_relayed(&P, X3, &h[8], now) == &h[4],
              "one more relayed frame evicts the oldest RELAYED one, never ours");
        CHECK(umac_mesh_pending_push(&P, O5, &h[9], now) == &h[0],
              "one more of ours evicts our oldest, never a relayed one");
        CHECK(umac_mesh_pending_push_relayed(&P, X2, &h[10], now) == &h[6],
              "a third relayed frame for X2 evicts X2's oldest: %u per target",
              (unsigned)UMAC_MESH_PENDING_PER_TARGET);
        uint8_t tg[UMAC_MESH_PENDING_SLOTS][6];
        uint32_t m = umac_mesh_pending_targets(&P, now, tg, UMAC_MESH_PENDING_SLOTS);
        bool relayed_listed = false;
        for (uint32_t i = 0; i < m; i++) { relayed_listed |= tg[i][1] == 0x0e; }
        CHECK(umac_mesh_pending_count_relayed(&P) == UMAC_MESH_PENDING_RELAY_MAX && m == 4 && !relayed_listed,
              "the own re-ask list names our 4 targets and no relayed one (%u listed)", m);

        /* The ladder: one discovery per target, joined by later frames. */
        umac_mesh_pending_init(&P);
        uint8_t due[UMAC_MESH_PENDING_RELAY_MAX][6];
        uint32_t d = 0;
        const uint32_t t0 = now + 1000u;
        (void)umac_mesh_pending_push_relayed(&P, X1, &h[0], t0);
        CHECK(umac_mesh_pending_ask_due(&P, t0, due, UMAC_MESH_PENDING_RELAY_MAX) == 1 &&
              memcmp(due[0], X1, 6) == 0, "a new relayed hold owes its first PREQ at once");
        umac_mesh_pending_asked(&P, X1, t0);
        CHECK(umac_mesh_pending_ask_due(&P, t0, due, UMAC_MESH_PENDING_RELAY_MAX) == 0 &&
              umac_mesh_pending_next_ms(&P, t0, &d) && d == 400,
              "asked: nothing more is due, and the next step is 400 ms away (%u)", d);
        (void)umac_mesh_pending_push_relayed(&P, X1, &h[1], t0 + 100u);
        CHECK(umac_mesh_pending_ask_due(&P, t0 + 100u, due, UMAC_MESH_PENDING_RELAY_MAX) == 0 &&
              umac_mesh_pending_next_ms(&P, t0 + 100u, &d) && d == 300,
              "a second frame for X1 joins the running discovery: no new PREQ, same ladder (%u)", d);
        static const uint32_t at[] = { 400u, 1200u, 2800u, 4800u };
        bool exact = true;
        for (unsigned k = 0; k < 4u; k++)
        {
            exact &= umac_mesh_pending_ask_due(&P, t0 + at[k] - 1u, due, UMAC_MESH_PENDING_RELAY_MAX) == 0;
            exact &= umac_mesh_pending_ask_due(&P, t0 + at[k], due, UMAC_MESH_PENDING_RELAY_MAX) == 1;
            umac_mesh_pending_asked(&P, X1, t0 + at[k]);
        }
        CHECK(exact, "re-asks fall due at exactly +400, +1200, +2800 and +4800 ms, not a ms early");
        CHECK(umac_mesh_pending_ask_due(&P, t0 + 6000u, due, UMAC_MESH_PENDING_RELAY_MAX) == 0 &&
              umac_mesh_pending_next_ms(&P, t0 + 6000u, &d) && d == 800,
              "after the fifth PREQ only the give-up at 6.8 s is left (%u ms away)", d);
        struct umac_mesh_pending_out out2[UMAC_MESH_PENDING_SLOTS];
        struct umac_mesh_fwd_ctx cg = c;
        cg.now_ms = t0 + 6799u;
        CHECK(umac_mesh_pending_take(&P, &cg, out2, UMAC_MESH_PENDING_SLOTS) == 0,
              "1 ms before the give-up both frames are still held");
        cg.now_ms = t0 + 6800u;
        uint32_t k2 = umac_mesh_pending_take(&P, &cg, out2, UMAC_MESH_PENDING_SLOTS);
        CHECK(k2 == 2 && !out2[0].ok && !out2[1].ok && out2[0].relayed && out2[1].relayed &&
              !umac_mesh_pending_next_ms(&P, cg.now_ms, &d),
              "at 6.8 s both are handed back to drop together, the later one too (%u)", k2);

        /* A step is taken only by a PREQ that went out once it was due, one step per PREQ:
         * one held back by the gate or refused by the radio leaves the step due. */
        {
            umac_mesh_pending_init(&P);
            const uint32_t t2 = now + 20000u;
            (void)umac_mesh_pending_push_relayed(&P, X2, &h[2], t2);
            umac_mesh_pending_asked(&P, X2, t2 - 10u);
            CHECK(umac_mesh_pending_ask_due(&P, t2, due, UMAC_MESH_PENDING_RELAY_MAX) == 1,
                  "a PREQ for X2 sent 10 ms before the hold does not take its first step");
            umac_mesh_pending_asked(&P, X2, t2 + 50u);
            CHECK(umac_mesh_pending_ask_due(&P, t2 + 50u, due, UMAC_MESH_PENDING_RELAY_MAX) == 0 &&
                      umac_mesh_pending_ask_due(&P, t2 + 400u, due, UMAC_MESH_PENDING_RELAY_MAX) == 1,
                  "the first, sent late at +50 ms, takes it; the next is still due at +400");
            umac_mesh_pending_asked(&P, X2, t2 + 399u);
            CHECK(umac_mesh_pending_ask_due(&P, t2 + 400u, due, UMAC_MESH_PENDING_RELAY_MAX) == 1,
                  "a PREQ 1 ms before the second step does not take it");
            (void)umac_mesh_pending_push_relayed(&P, X2, &h[3], t2 + 450u);
            umac_mesh_pending_asked(&P, X2, t2 + 400u);
            CHECK(umac_mesh_pending_ask_due(&P, t2 + 450u, due, UMAC_MESH_PENDING_RELAY_MAX) == 0 &&
                      umac_mesh_pending_next_ms(&P, t2 + 450u, &d) && d == 750u,
                  "a PREQ sent at +400 and reported after a frame joined at +450 takes the second "
                  "step for both (next step %u ms away)", d);
            umac_mesh_pending_asked(&P, X2, t2 + 2900u);
            umac_mesh_pending_asked(&P, X2, t2 + 2900u);
            CHECK(umac_mesh_pending_ask_due(&P, t2 + 2900u, due, UMAC_MESH_PENDING_RELAY_MAX) == 1,
                  "one PREQ sent late, past the fourth step's time too, reported twice, takes only the third");
            /* The gate's record of a target can be days old; past 2^31 ms it reads as later. */
            umac_mesh_pending_init(&P);
            (void)umac_mesh_pending_push_relayed(&P, X3, &h[4], t2);
            umac_mesh_pending_asked(&P, X3, t2 - 0x80000000u - 1000u);
            CHECK(umac_mesh_pending_ask_due(&P, t2, due, UMAC_MESH_PENDING_RELAY_MAX) == 1,
                  "a PREQ sent 2^31 ms before the hold, later across the clock wrap, takes no step");
        }

        /* Resolution: a path or a peer for the mesh DA, never the proxy table. */
        umac_mesh_pending_init(&P);
        (void)umac_mesh_pending_push_relayed(&P, C, &h[0], now);
        CHECK(umac_mesh_pending_take(&P, &c, out2, UMAC_MESH_PENDING_SLOTS) == 0,
              "a relayed frame for C waits while there is no path");
        umac_mesh_path_update(&T, C, B, 5, 200, 1, 5120, now);
        k2 = umac_mesh_pending_take(&P, &c, out2, UMAC_MESH_PENDING_SLOTS);
        CHECK(k2 == 1 && out2[0].ok && out2[0].relayed && out2[0].handle == &h[0] &&
              memcmp(out2[0].ra, B, 6) == 0, "and is released to next hop B once the PREP installs it");
        static const uint8_t HX[6] = { 0x00, 0xe0, 0x4f, 0x71, 0x99, 0xa6 };
        umac_mesh_proxy_learn(&T, HX, B, now);
        (void)umac_mesh_pending_push(&P, HX, &h[1], now);
        (void)umac_mesh_pending_push_relayed(&P, HX, &h[2], now);
        k2 = umac_mesh_pending_take(&P, &c, out2, UMAC_MESH_PENDING_SLOTS);
        CHECK(k2 == 1 && out2[0].handle == &h[1] && !out2[0].relayed && umac_mesh_pending_count_relayed(&P) == 1,
              "a proxy entry releases OUR frame for HX but not a relayed one: a relay never re-addresses (%u)", k2);
        CHECK(umac_mesh_pending_push_relayed(NULL, C, &h[3], now) == &h[3],
              "NULL store hands a relayed frame straight back");
        /* A third frame reuses the evicted first one's slot; the flow still leaves in order. */
        umac_mesh_pending_init(&P);
        (void)umac_mesh_pending_push_relayed(&P, C, &h[4], now);
        (void)umac_mesh_pending_push_relayed(&P, C, &h[5], now);
        void *ev = umac_mesh_pending_push_relayed(&P, C, &h[6], now);
        k2 = umac_mesh_pending_take(&P, &c, out2, UMAC_MESH_PENDING_SLOTS);
        CHECK(ev == &h[4] && k2 == 2 && out2[0].handle == &h[5] && out2[1].handle == &h[6],
              "released oldest first even when the newest sits in a lower slot (%u)", k2);
    }

    /* ---- round trip: what we shape, we would deliver correctly -------------- */
    {
        /* The blocks above re-initialised the shared table; seed it as at the top. */
        umac_mesh_pathtbl_init(&T);
        umac_mesh_proxy_learn(&T, HA, A, now);
        umac_mesh_fwd_tx(&c, HA, HW, 108, &t);
        struct umac_mesh_rx_frame back = uni(A, W, t.addr3, t.addr4, t.mc.seq, t.mc.ttl, t.mc.flags, t.mc.eaddr1, t.mc.eaddr2);
        /* Pretend we are A receiving it. */
        struct umac_mesh_fwd_ctx as_a = ctx(true, now); as_a.own_addr = A;
        umac_mesh_fwd_rx(&as_a, &back, &r);
        CHECK(r.verdict == UMAC_MESH_FWD_DELIVER && memcmp(r.deliver_da, HA, 6) == 0 && memcmp(r.deliver_sa, HW, 6) == 0, "A delivers HW->HA to its host with the real endpoints");
        CHECK(memcmp(umac_mesh_proxy_lookup(&T, HW, now), W, 6) == 0, "and A learned HW sits behind W");
    }

    /* ---- leaf mode: a host behind a node we do not hear --------------------
     *
     * Leaf W hears peers A and B; host H3 sits behind M, which W does not hear.
     * Pinned: what teaches (AE 1 on a group frame, AE 2 on a unicast, a
     * replica) and what does not (the wrong AE for the shape, a non-peer TA, a
     * group or own mesh SA, a peer or the mesh SA itself as the host, ttl 0, a
     * second copy of a flood); which peer a reply goes to (the host's node when
     * it is a peer, else the relay of the latest evidence, with a unicast's
     * relay held against floods for UMAC_MESH_LEAF_VIA_HOLD_MS unless the host
     * moved or that relay is gone); only a unicast to us pins; and the leaf TX
     * branch never applies to a relay-learned entry. */
    {
        static const uint8_t M[6]  = { 0x02, 0, 0, 0, 0, 0x4d };  /* not a peer, no path */
        static const uint8_t M2[6] = { 0x02, 0, 0, 0, 0, 0x4e };  /* another node we do not hear */
        static const uint8_t H3[6] = { 0x00, 0xe0, 0x4f, 0, 0, 0x93 };
        static const uint8_t H5[6] = { 0x00, 0xe0, 0x4f, 0, 0, 0x95 };
        static const uint8_t X[6]  = { 0x02, 0, 0, 0, 0, 0x99 };  /* not a peer */
        uint32_t t0 = 900000;
        umac_mesh_pathtbl_init(&T);
        umac_mesh_rmc_init(&R);
        struct umac_mesh_fwd_ctx lf = ctx(false, t0);
        const struct umac_mesh_proxy *e;
        uint8_t via[6] = { 0 };

        struct umac_mesh_rx_frame u = uni(W, A, W, M, 1, 30, UMAC_MESH_CTRL_AE_A5A6, W, H3);
        CHECK(umac_mesh_fwd_leaf_learn(&lf, &u), "leaf: H3 behind non-neighbour M is learned from A's unicast");
        e = umac_mesh_proxy_entry(&T, H3, t0);
        CHECK(e != NULL && memcmp(e->mesh_sta, M, 6) == 0 && memcmp(e->via, A, 6) == 0 && e->leaf && e->uni,
              "leaf: behind M, via A, with unicast evidence");

        umac_mesh_fwd_tx(&lf, H3, W, 77, &t);
        CHECK(t.ok && !t.need_path && memcmp(t.ra, A, 6) == 0, "leaf TX: RA = A, the relay, and no path request");
        CHECK(memcmp(t.addr3, M, 6) == 0 && memcmp(t.addr4, W, 6) == 0, "leaf TX: mesh DA = M, mesh SA = us");
        CHECK(umac_mesh_ctrl_ae(&t.mc) == UMAC_MESH_CTRL_AE_A5A6 && memcmp(t.mc.eaddr1, H3, 6) == 0 &&
              memcmp(t.mc.eaddr2, W, 6) == 0, "leaf TX: AE 2 = (H3, us), mac80211's own shape for a proxied host");
        CHECK(umac_mesh_fwd_proxy_via_peer(&lf, H3, via) && memcmp(via, A, 6) == 0, "leaf: H3 is sent through A");
        CHECK(umac_mesh_fwd_leaf_proxied(&lf, H3) && !umac_mesh_fwd_leaf_proxied(&lf, M) &&
              !umac_mesh_fwd_leaf_proxied(&lf, A) && !umac_mesh_fwd_leaf_proxied(&lf, BC),
              "leaf: only the learned host is proxied -- not its node, a peer or broadcast");

        /* Shape rules, as umac_mesh_fwd_rx applies them, and the refusals. */
        umac_mesh_pathtbl_init(&T);
        struct umac_mesh_rx_frame bad = uni(W, A, W, M, 2, 30, UMAC_MESH_CTRL_AE_A4, H3, NULL);
        CHECK(!umac_mesh_fwd_leaf_learn(&lf, &bad), "leaf: AE 1 on a unicast teaches nothing");
        struct umac_mesh_rx_frame g2 = group_from(M, A, 3, 30, UMAC_MESH_CTRL_AE_A5A6, H3);
        CHECK(!umac_mesh_fwd_leaf_learn(&lf, &g2), "leaf: AE 2 on a group frame teaches nothing");
        struct umac_mesh_rx_frame nx = uni(W, X, W, M, 4, 30, UMAC_MESH_CTRL_AE_A5A6, W, H3);
        CHECK(!umac_mesh_fwd_leaf_learn(&lf, &nx), "leaf: a transmitter that is not a peer teaches nothing");
        struct umac_mesh_rx_frame gm = uni(W, A, W, MC, 5, 30, UMAC_MESH_CTRL_AE_A5A6, W, H3);
        CHECK(!umac_mesh_fwd_leaf_learn(&lf, &gm), "leaf: a group mesh SA is refused");
        struct umac_mesh_rx_frame pb = uni(W, A, W, M, 6, 30, UMAC_MESH_CTRL_AE_A5A6, W, B);
        CHECK(!umac_mesh_fwd_leaf_learn(&lf, &pb), "leaf: peer B is never a host behind M");
        struct umac_mesh_rx_frame z1 = group_from(M, A, 9, 0, UMAC_MESH_CTRL_AE_A4, H3);
        struct umac_mesh_rx_frame z2 = uni(W, A, W, M, 10, 0, UMAC_MESH_CTRL_AE_A5A6, W, H3);
        CHECK(!umac_mesh_fwd_leaf_learn(&lf, &z1) && !umac_mesh_fwd_leaf_learn(&lf, &z2),
              "leaf: ttl 0 teaches nothing, flood or unicast (mac80211 drops it before learning)");
        /* A warthog replica of M's own broadcast carries e2 = M = mesh SA. */
        struct umac_mesh_rx_frame selfrep = uni(W, A, W, M, 12, 30, UMAC_MESH_CTRL_AE_A5A6, MC, M);
        struct umac_mesh_rx_frame selfuni = uni(W, A, W, M, 13, 30, UMAC_MESH_CTRL_AE_A5A6, W, M);
        CHECK(!umac_mesh_fwd_leaf_learn(&lf, &selfrep) && !umac_mesh_fwd_leaf_learn(&lf, &selfuni),
              "leaf: a node is never learned as a host behind itself (replica or unicast)");
        CHECK(umac_mesh_proxy_count(&T, t0) == 0, "leaf: and none of those left an entry");

        /* A flood, and the replica form of one, both teach -- weakly. */
        struct umac_mesh_rx_frame fl = group_from(M, B, 7, 30, UMAC_MESH_CTRL_AE_A4, H3);
        CHECK(umac_mesh_fwd_leaf_learn(&lf, &fl), "leaf: a flood from H3 re-sent by B teaches");
        e = umac_mesh_proxy_entry(&T, H3, t0);
        CHECK(e != NULL && memcmp(e->via, B, 6) == 0 && !e->uni, "leaf: via B, without unicast evidence");
        struct umac_mesh_rx_frame rp = uni(W, A, W, M, 8, 30, UMAC_MESH_CTRL_AE_A5A6, MC, H5);
        CHECK(umac_mesh_fwd_leaf_learn(&lf, &rp), "leaf: a warthog replica of a flood teaches too");
        e = umac_mesh_proxy_entry(&T, H5, t0);
        CHECK(e != NULL && memcmp(e->mesh_sta, M, 6) == 0 && memcmp(e->via, A, 6) == 0 && !e->uni,
              "leaf: ...as the flood it is: behind M, via A, no unicast evidence");
        /* Only a unicast to US is a conversation: one to another host behind us pins nothing. */
        struct umac_mesh_rx_frame other = uni(W, A, W, M, 14, 30, UMAC_MESH_CTRL_AE_A5A6, HW, H5);
        (void)umac_mesh_fwd_leaf_learn(&lf, &other);
        e = umac_mesh_proxy_entry(&T, H5, t0);
        CHECK(e != NULL && !e->uni, "leaf: an AE 2 unicast whose e1 is not us is no unicast evidence");

        /* The second copy of a flood, through another relay, does not move via:
         * mac80211's RMC drops it before learning, so the first copy wins. */
        umac_mesh_pathtbl_init(&T);
        struct umac_mesh_rx_frame d1 = group_from(M, A, 20, 30, UMAC_MESH_CTRL_AE_A4, H3);
        struct umac_mesh_rx_frame d2 = group_from(M, B, 20, 29, UMAC_MESH_CTRL_AE_A4, H3);
        CHECK(umac_mesh_fwd_leaf_learn(&lf, &d1) && !umac_mesh_fwd_leaf_learn(&lf, &d2),
              "leaf: flood (M, seq 20) via A teaches; its copy via B is a duplicate");
        e = umac_mesh_proxy_entry(&T, H3, t0);
        CHECK(e != NULL && memcmp(e->via, A, 6) == 0, "leaf: ...and via stays A");
        struct umac_mesh_rx_frame r1 = uni(W, A, W, M, 21, 30, UMAC_MESH_CTRL_AE_A5A6, MC, H5);
        struct umac_mesh_rx_frame r2 = uni(W, B, W, M, 21, 29, UMAC_MESH_CTRL_AE_A5A6, MC, H5);
        (void)umac_mesh_fwd_leaf_learn(&lf, &r1);
        CHECK(!umac_mesh_fwd_leaf_learn(&lf, &r2), "leaf: a replica's copy via B is a duplicate too");
        e = umac_mesh_proxy_entry(&T, H5, t0);
        CHECK(e != NULL && memcmp(e->via, A, 6) == 0, "leaf: ...and H5's via stays A");
        struct umac_mesh_rx_frame d3 = group_from(M, B, 22, 30, UMAC_MESH_CTRL_AE_A4, H3);
        CHECK(umac_mesh_fwd_leaf_learn(&lf, &d3), "leaf: the NEXT flood via B is new evidence");
        e = umac_mesh_proxy_entry(&T, H3, t0);
        CHECK(e != NULL && memcmp(e->via, B, 6) == 0, "leaf: ...and moves a flood-only hint to B");

        /* Unicast via A outranks a later flood via B until A's path can have lapsed. */
        umac_mesh_pathtbl_init(&T);
        (void)umac_mesh_fwd_leaf_learn(&lf, &u);
        struct umac_mesh_rx_frame h1 = group_from(M, B, 30, 30, UMAC_MESH_CTRL_AE_A4, H3);
        (void)umac_mesh_fwd_leaf_learn(&lf, &h1);
        e = umac_mesh_proxy_entry(&T, H3, t0);
        CHECK(e != NULL && memcmp(e->via, A, 6) == 0, "leaf: a flood via B does not displace A");
        struct umac_mesh_fwd_ctx held = ctx(false, t0 + UMAC_MESH_LEAF_VIA_HOLD_MS - 1);
        struct umac_mesh_rx_frame h2 = group_from(M, B, 31, 30, UMAC_MESH_CTRL_AE_A4, H3);
        (void)umac_mesh_fwd_leaf_learn(&held, &h2);
        e = umac_mesh_proxy_entry(&T, H3, held.now_ms);
        CHECK(e != NULL && memcmp(e->via, A, 6) == 0, "leaf: nor does one 1 ms before the hold ends");
        struct umac_mesh_fwd_ctx later = ctx(false, t0 + UMAC_MESH_LEAF_VIA_HOLD_MS);
        struct umac_mesh_rx_frame h3 = group_from(M, B, 32, 30, UMAC_MESH_CTRL_AE_A4, H3);
        (void)umac_mesh_fwd_leaf_learn(&later, &h3);
        e = umac_mesh_proxy_entry(&T, H3, later.now_ms);
        CHECK(e != NULL && memcmp(e->via, B, 6) == 0, "leaf: after the hold it does");

        /* The host moves to another node inside the hold: A's path was to M, not M2. */
        umac_mesh_pathtbl_init(&T);
        (void)umac_mesh_fwd_leaf_learn(&lf, &u);
        struct umac_mesh_rx_frame mv = group_from(M2, B, 33, 30, UMAC_MESH_CTRL_AE_A4, H3);
        struct umac_mesh_fwd_ctx soon = ctx(false, t0 + 1);
        CHECK(umac_mesh_fwd_leaf_learn(&soon, &mv), "leaf: H3 reappears behind M2, flooded by B");
        e = umac_mesh_proxy_entry(&T, H3, t0 + 1);
        CHECK(e != NULL && memcmp(e->mesh_sta, M2, 6) == 0 && memcmp(e->via, B, 6) == 0,
              "leaf: ...behind M2 via B: the hold is for A's path to M only");

        /* The relay goes. Its hint is not used, and a flood through the peer
         * that is left may take over at once, hold or no hold. */
        umac_mesh_pathtbl_init(&T);
        (void)umac_mesh_fwd_leaf_learn(&lf, &u);
        struct umac_mesh_fwd_ctx gone = ctx(false, t0 + 1);
        gone.is_peer = only_b;
        umac_mesh_fwd_tx(&gone, H3, W, 79, &t);
        CHECK(!t.ok && t.need_path, "leaf: with A gone its hint is not a next hop");
        CHECK(!umac_mesh_fwd_proxy_via_peer(&gone, H3, via) && umac_mesh_fwd_leaf_proxied(&gone, H3),
              "leaf: no via, but still a proxied host (so the caller keeps the AE shape)");
        struct umac_mesh_rx_frame g3 = group_from(M, B, 34, 30, UMAC_MESH_CTRL_AE_A4, H3);
        (void)umac_mesh_fwd_leaf_learn(&gone, &g3);
        e = umac_mesh_proxy_entry(&T, H3, t0 + 1);
        CHECK(e != NULL && memcmp(e->via, B, 6) == 0, "leaf: a held via that is no peer yields to B's flood");

        /* A relay-learned host never takes the leaf branch, even with stale via bytes. */
        umac_mesh_pathtbl_init(&T);
        (void)umac_mesh_fwd_leaf_learn(&lf, &u);
        (void)umac_mesh_proxy_learn(&T, H3, M, t0);
        umac_mesh_fwd_tx(&lf, H3, W, 78, &t);
        CHECK(!t.ok && t.need_path && memcmp(t.path_target, M, 6) == 0,
              "a relay-learned host still needs a path; the leaf branch is leaf-only");
        CHECK(!umac_mesh_fwd_leaf_proxied(&lf, H3), "and it is not a leaf host");

        /* A host behind a DIRECT peer goes to that peer, not to whichever relay
         * carried the flood that taught it. */
        umac_mesh_pathtbl_init(&T);
        struct umac_mesh_rx_frame viaA = group_from(B, A, 40, 30, UMAC_MESH_CTRL_AE_A4, H3);
        CHECK(umac_mesh_fwd_leaf_learn(&lf, &viaA), "leaf: H3 behind peer B, its flood carried by A");
        umac_mesh_fwd_tx(&lf, H3, W, 81, &t);
        CHECK(t.ok && memcmp(t.ra, B, 6) == 0 && memcmp(t.addr3, B, 6) == 0,
              "leaf: a host behind direct peer B goes to B, not to the flood's relay A");
        CHECK(umac_mesh_fwd_proxy_via_peer(&lf, H3, via) && memcmp(via, B, 6) == 0,
              "leaf: and the datapath's next hop for it is B");

        /* The hold is vanilla dot11MeshHWMPactivePathTimeout, not whatever path
         * lifetime this build uses. */
        _Static_assert(UMAC_MESH_LEAF_VIA_HOLD_MS == 5120u, "leaf via hold is 5120 ms");

        /* A frame naming us as its mesh SA is our own, echoed: it names no host. */
        umac_mesh_pathtbl_init(&T);
        umac_mesh_rmc_init(&R);
        struct umac_mesh_rx_frame own1 = uni(W, A, W, W, 50, 30, UMAC_MESH_CTRL_AE_A5A6, W, H3);
        struct umac_mesh_rx_frame own2 = group_from(W, A, 51, 30, UMAC_MESH_CTRL_AE_A4, H3);
        CHECK(!umac_mesh_fwd_leaf_learn(&lf, &own1) && !umac_mesh_fwd_leaf_learn(&lf, &own2) &&
              umac_mesh_proxy_count(&T, t0) == 0,
              "leaf: our own address as mesh SA teaches nothing, unicast or flood");

        /* The duplicate check runs after every refusal: a copy refused for its
         * transmitter or its ttl must not make the peer's copy a duplicate. */
        umac_mesh_pathtbl_init(&T);
        umac_mesh_rmc_init(&R);
        struct umac_mesh_rx_frame nx1 = group_from(M, X, 52, 30, UMAC_MESH_CTRL_AE_A4, H3);
        struct umac_mesh_rx_frame nx2 = group_from(M, A, 52, 29, UMAC_MESH_CTRL_AE_A4, H3);
        CHECK(!umac_mesh_fwd_leaf_learn(&lf, &nx1) && umac_mesh_fwd_leaf_learn(&lf, &nx2),
              "leaf: non-peer X's copy of flood (M, seq 52) is refused and A's copy still teaches");
        struct umac_mesh_rx_frame zt1 = group_from(M, A, 53, 0, UMAC_MESH_CTRL_AE_A4, H3);
        struct umac_mesh_rx_frame zt2 = group_from(M, B, 53, 30, UMAC_MESH_CTRL_AE_A4, H3);
        CHECK(!umac_mesh_fwd_leaf_learn(&lf, &zt1) && umac_mesh_fwd_leaf_learn(&lf, &zt2),
              "leaf: a ttl-0 copy of flood (M, seq 53) is refused and B's copy still teaches");

        /* ttl 1 is live: mac80211 learns from it and only declines to forward. */
        umac_mesh_pathtbl_init(&T);
        umac_mesh_rmc_init(&R);
        struct umac_mesh_rx_frame one1 = group_from(M, A, 54, 1, UMAC_MESH_CTRL_AE_A4, H3);
        struct umac_mesh_rx_frame one2 = uni(W, A, W, M, 55, 1, UMAC_MESH_CTRL_AE_A5A6, W, H5);
        CHECK(umac_mesh_fwd_leaf_learn(&lf, &one1) && umac_mesh_fwd_leaf_learn(&lf, &one2),
              "leaf: a ttl-1 frame teaches, flood or unicast");

        /* The hold protects unicast evidence against floods only: fresher unicast
         * evidence through another relay takes over at once. */
        umac_mesh_pathtbl_init(&T);
        umac_mesh_rmc_init(&R);
        (void)umac_mesh_fwd_leaf_learn(&lf, &u);
        struct umac_mesh_fwd_ctx next = ctx(false, t0 + 1);
        struct umac_mesh_rx_frame ub = uni(W, B, W, M, 56, 30, UMAC_MESH_CTRL_AE_A5A6, W, H3);
        CHECK(umac_mesh_fwd_leaf_learn(&next, &ub), "leaf: H3's next unicast arrives through B");
        e = umac_mesh_proxy_entry(&T, H3, t0 + 1);
        CHECK(e != NULL && memcmp(e->via, B, 6) == 0 && e->uni && e->uni_ms == t0 + 1,
              "leaf: ...inside A's hold, and via moves to B (via=%02x)", e != NULL ? e->via[5] : 0);

        /* After a move, the old node's pin neither holds the new node's floods
         * to their first relay nor keeps the entry pinned. */
        umac_mesh_pathtbl_init(&T);
        umac_mesh_rmc_init(&R);
        (void)umac_mesh_fwd_leaf_learn(&lf, &u);
        struct umac_mesh_rx_frame m2b = group_from(M2, B, 57, 30, UMAC_MESH_CTRL_AE_A4, H3);
        struct umac_mesh_rx_frame m2a = group_from(M2, A, 58, 30, UMAC_MESH_CTRL_AE_A4, H3);
        (void)umac_mesh_fwd_leaf_learn(&next, &m2b);
        struct umac_mesh_fwd_ctx then = ctx(false, t0 + 2);
        (void)umac_mesh_fwd_leaf_learn(&then, &m2a);
        e = umac_mesh_proxy_entry(&T, H3, t0 + 2);
        CHECK(e != NULL && memcmp(e->mesh_sta, M2, 6) == 0 && memcmp(e->via, A, 6) == 0,
              "leaf: after H3 moves to M2, M2's next flood via A moves via to A (via=%02x)",
              e != NULL ? e->via[5] : 0);
        CHECK(e != NULL && !e->uni && e->uni_ms == 0, "leaf: ...and M's unicast pin did not follow H3 to M2");
    }

    /* ---- the leaf via hold across 2^31 and 2^32 ms ---------------------------
     *
     * H3, behind non-neighbour M, sends W one unicast through A; after that only
     * its floods arrive, through B, every 300 s. They keep the entry live and
     * pinned indefinitely, and move via to B once the hold is over. The hold is
     * the 5.12 s after that one unicast, never again: a signed age reads as
     * fresh once it is 2^31 ms old (24.9 days), and an unsigned one wraps to 0
     * at 2^32 ms (49.7 days) unless the sweep bounds it. Either way the next
     * flood, through A, would leave via on B, so every reply to H3 goes to a
     * relay that may hold no path to M. Pinned: without a sweep, a flood exactly
     * 2^31 ms after the unicast moves via; with the glue's sweep run at every
     * flood, so does one exactly 2^32 ms after it.
     */
    {
        static const uint8_t M[6]  = { 0x02, 0, 0, 0, 0, 0x4d };
        static const uint8_t H3[6] = { 0x00, 0xe0, 0x4f, 0, 0, 0x93 };
        const uint32_t t0 = 900000, step = 300000;
        for (int swept = 0; swept < 2; swept++)
        {
            const uint64_t span = swept ? (1ull << 32) : (1ull << 31);
            const char *when = swept ? "2^32 ms, swept" : "2^31 ms, unswept";
            umac_mesh_pathtbl_init(&T);
            umac_mesh_rmc_init(&R);
            struct umac_mesh_fwd_ctx lf = ctx(false, t0);
            struct umac_mesh_rx_frame u = uni(W, A, W, M, 1, 30, UMAC_MESH_CTRL_AE_A5A6, W, H3);
            CHECK(umac_mesh_fwd_leaf_learn(&lf, &u), "wrap %s: H3's one unicast arrives through A", when);
            uint32_t seq = 2, taught = 0, floods = 0;
            for (uint64_t at = step; at < span; at += step, floods++)
            {
                lf.now_ms = (uint32_t)(t0 + at);
                struct umac_mesh_rx_frame fb = group_from(M, B, seq++, 30, UMAC_MESH_CTRL_AE_A4, H3);
                taught += umac_mesh_fwd_leaf_learn(&lf, &fb) ? 1u : 0u;
                if (swept) { (void)umac_mesh_path_expire(&T, lf.now_ms); }
            }
            lf.now_ms = (uint32_t)(t0 + span);
            const struct umac_mesh_proxy *e = umac_mesh_proxy_entry(&T, H3, lf.now_ms);
            CHECK(taught == floods && e != NULL && e->uni && memcmp(e->via, B, 6) == 0,
                  "wrap %s: %u floods through B kept H3 live and pinned, via B", when, (unsigned)floods);
            struct umac_mesh_rx_frame fa = group_from(M, A, seq++, 30, UMAC_MESH_CTRL_AE_A4, H3);
            CHECK(umac_mesh_fwd_leaf_learn(&lf, &fa), "wrap %s: then a flood through A", when);
            e = umac_mesh_proxy_entry(&T, H3, lf.now_ms);
            CHECK(e != NULL && memcmp(e->via, A, 6) == 0,
                  "wrap %s: moves via to A; the hold does not re-arm (via=%02x)", when,
                  e != NULL ? e->via[5] : 0);
            umac_mesh_fwd_tx(&lf, H3, W, 90, &t);
            CHECK(t.ok && memcmp(t.ra, A, 6) == 0, "wrap %s: and a reply to H3 goes to A (ra=%02x)",
                  when, t.ra[5]);
        }
    }

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_mesh_fwd: all passed\n");
    return 0;
}
