/*
 * The real TX and RX datapath: an 802.3 frame in, a complete 802.11 frame out.
 *
 * Everything asserted here is built by the shipping code -- umac_datapath.c's
 * TX entry, umac_datapath_mesh.c's header construction and per-peer fan-out,
 * umac_mesh_fwd.c's shaping and verdicts, umac_mesh_fwd_glue.c's relay -- and
 * read back either with the firmware's own parser (umac_mesh_fwd_parse_frame)
 * or at absolute byte offsets, where the on-air layout is the thing that
 * matters and a parser agreeing with a builder would prove nothing.
 *
 * The two shapes this firmware emits are not interchangeable:
 *
 *   MESHGRP=1  a standard 802.11s 3-address group frame, one transmission,
 *              addr3 the MESH SOURCE (which a relay must preserve).
 *   MESHGRP=0  the default on this chip: one 4-address unicast replica per
 *              peer, the group DA and the real source carried in Address
 *              Extension mode 2, because the MM6108 cannot key group RX
 *              across several mesh peers.
 *
 * What this file cannot tell you: anything about the air. It asserts that the
 * firmware's own logic is self-consistent and that the bytes are the ones
 * 802.11-2020 s9.2.4.7.3 and mac80211 describe -- not that a mac80211 peer
 * accepts them.
 *
 * One engine verdict is deliberately absent. UMAC_MESH_FWD_DROP_OWN cannot be
 * reached through the real receive path: umac_datapath_rx_frame_filter drops a
 * frame whose SA is our own address (addr4 on a 4-address frame, addr3 on a
 * 3-address one) before the Mesh Control is ever parsed. Measured here -- both
 * shapes are refused upstream with filt_reason 7 -- so the engine's own check
 * is defence in depth, and a test that claimed to exercise it would be
 * exercising the filter instead.
 */
#include <stdio.h>
#include <string.h>

#include "simnode.h"
#include "mmdrv.h"
#include "mmpkt.h"
#include "umac_mesh_ctrl.h"
#include "umac_mesh_fwd.h"
#include "umac_mesh_fwd_glue.h"
#include "umac_mesh_ies.h"

/* Defined in warthog_globals.c, exactly as main/at.c defines them on the
 * firmware. rxdrop_reason is how the datapath records WHICH rule dropped a
 * frame: 100 + enum umac_mesh_fwd_drop for a forwarding-engine verdict. */
extern volatile uint32_t g_warthog_rxdrop_reason, g_warthog_rxdrop_count;
extern volatile uint32_t g_warthog_tx_bcast_dup;

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static const uint8_t W[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 }; /* us */
static const uint8_t A[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0a }; /* peer A */
static const uint8_t C[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0c }; /* peer C */
static const uint8_t E[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0e }; /* not a peer */
static const uint8_t H[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x77 }; /* host behind us */
static const uint8_t H2[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x88 }; /* host behind A */
static const uint8_t BC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static const uint8_t PAY[8] = { 0xc0, 0xff, 0xee, 0x11, 0x22, 0x33, 0x44, 0x55 };

/* SNAP/802.1h, the eight octets umac_datapath.c puts between Mesh Control and
 * the payload. Spelled out here because the offsets below count through it. */
static const uint8_t SNAP_IPV4[8] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00 };
/* 802.1X / EAPOL. umac_datapath.c recognises EAPOL by this ethertype alone. */
static const uint8_t SNAP_EAPOL[8] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E };

#define MAC_EQ(p, m) (memcmp((p), (m), 6) == 0)

static void hexdump(const char *tag, const uint8_t *b, unsigned n)
{
    printf("     %s (%u):", tag, n);
    for (unsigned i = 0; i < n; i++) { printf("%s%02x", (i % 6) ? "" : " ", b[i]); }
    printf("\n");
}

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- frame injection ---------------------------------------------------
 *
 * Built with the SHIPPING builders (umac_mesh_ies_build_data_hdr*,
 * umac_mesh_ctrl_build), so an injected frame is the same byte layout a
 * warthog or a mac80211 peer puts on air rather than a second hand-rolled
 * encoder that could agree with a wrong decoder. */

static uint16_t put_tail_(uint8_t *f, uint16_t n, const struct umac_mesh_ctrl *mc,
                          const uint8_t *pay, uint16_t pay_len)
{
    f[n++] = 0x00;  /* QoS Control: TID 0 ... */
    f[n++] = 0x01;  /* ... | Mesh Control Present (bit 8) */
    n = (uint16_t)(n + umac_mesh_ctrl_build(&f[n], 18u, mc));
    memcpy(&f[n], SNAP_IPV4, sizeof(SNAP_IPV4));
    n = (uint16_t)(n + sizeof(SNAP_IPV4));
    memcpy(&f[n], pay, pay_len);
    return (uint16_t)(n + pay_len);
}

static uint16_t mk_uni(uint8_t *f, const uint8_t *ra, const uint8_t *ta, const uint8_t *da,
                       const uint8_t *sa, const struct umac_mesh_ctrl *mc, const uint8_t *pay,
                       uint16_t pay_len)
{
    uint16_t n = umac_mesh_ies_build_data_hdr4(f, ra, ta, da, sa);
    return put_tail_(f, n, mc, pay, pay_len);
}

static uint16_t mk_grp(uint8_t *f, const uint8_t *gda, const uint8_t *ta, const uint8_t *msa,
                       const struct umac_mesh_ctrl *mc, const uint8_t *pay, uint16_t pay_len)
{
    uint16_t n = umac_mesh_ies_build_data_hdr3_group(f, gda, ta, msa);
    return put_tail_(f, n, mc, pay, pay_len);
}

/* The outbox is filled in round-robin peer order, which is deliberate and not
 * something a test should pin; find a copy by who it is addressed to. */
static const struct simnode_frame *frame_to(const uint8_t *addr1)
{
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *f = simnode_outbox_get(i);
        if (f != NULL && f->len >= 10u && MAC_EQ(&f->bytes[4], addr1)) { return f; }
    }
    return NULL;
}

/* Bring a node up clean: fresh tables, fresh outbox, fresh host netif.
 *
 * The peer table has to be emptied explicitly. umac_mesh_disable_mesh() is a
 * stub on this build (it returns MMWLAN_UNAVAILABLE and does nothing), so a
 * restart alone leaves the previous scenario's peers established -- and how
 * many peers exist is exactly what the group fan-out counts. del_peer with a
 * NULL address is the shipping "every peer" form. */
static void fresh(bool fwd, bool bridge, bool grp_std)
{
    simnode_del_peer(NULL);
    (void)simnode_start(W);
    simnode_set_gates(fwd, bridge, grp_std, /*secure=*/false);
    simnode_outbox_clear();
    simnode_host_rx_clear();
    simnode_stub_reset();
    g_warthog_rxdrop_count = 0;
    g_warthog_rxdrop_reason = 0;
}

/* ---- 1. unicast to a direct peer: the whole header, byte for byte -------- */

static void t_tx_unicast_4addr(void)
{
    printf("--- TX: an 802.3 unicast becomes a 4-address mesh data frame ---\n");
    fresh(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false);
    (void)simnode_add_peer(A);

    CHECK(simnode_host_tx(A, W, PAY, sizeof(PAY)), "host hands down DA=A SA=us");
    CHECK(simnode_outbox_count() == 1, "one frame on air (got %u)", simnode_outbox_count());
    const struct simnode_frame *f = simnode_outbox_get(0);
    if (f == NULL) { return; }
    if (f->len != 46u + sizeof(PAY)) { hexdump("frame", f->bytes, f->len); }
    CHECK(f->len == 46u + sizeof(PAY),
          "30 MAC + 2 QoS + 6 Mesh Control + 8 SNAP + %u payload = %u bytes (got %u)",
          (unsigned)sizeof(PAY), (unsigned)(46u + sizeof(PAY)), f->len);
    if (f->len < 46u + sizeof(PAY)) { return; }

    /* Frame Control: type Data (2), subtype QoS Data (8), ToDS and FromDS
     * both set -- the 4-address form 802.11-2020 table 9-30 gives mesh. */
    CHECK(f->bytes[0] == 0x88 && f->bytes[1] == 0x03,
          "[0..1] FC = 0x0388: QoS Data, ToDS|FromDS (got %02x%02x)", f->bytes[1], f->bytes[0]);
    CHECK(MAC_EQ(&f->bytes[4], A),  "[4]  addr1 = RA   = the peer");
    CHECK(MAC_EQ(&f->bytes[10], W), "[10] addr2 = TA   = us");
    CHECK(MAC_EQ(&f->bytes[16], A), "[16] addr3 = mesh DA");
    CHECK(MAC_EQ(&f->bytes[24], W), "[24] addr4 = mesh SA");
    CHECK(f->bytes[30] == 0x00 && f->bytes[31] == 0x01,
          "[30..31] QoS Control = TID 0 with Mesh Control Present (got %02x%02x)",
          f->bytes[31], f->bytes[30]);
    CHECK(f->bytes[32] == 0x00,
          "[32] Mesh Control flags = 0: no Address Extension (got %02x)", f->bytes[32]);
    CHECK(f->bytes[33] == UMAC_MESH_CTRL_TTL_DEFAULT,
          "[33] TTL = %u (got %u)", UMAC_MESH_CTRL_TTL_DEFAULT, f->bytes[33]);
    CHECK(memcmp(&f->bytes[38], SNAP_IPV4, sizeof(SNAP_IPV4)) == 0,
          "[38] SNAP/802.1h + ethertype 0x0800 follows the Mesh Control");
    CHECK(memcmp(&f->bytes[46], PAY, sizeof(PAY)) == 0, "[46] the payload, unchanged");

    /* And the shipping parser agrees with the shipping builder. */
    struct umac_mesh_rx_frame pf;
    uint16_t used = umac_mesh_fwd_parse_frame(f->bytes, f->len, &pf);
    CHECK(used == 38u, "the firmware's parser consumes exactly 38 octets to the body (got %u)",
          used);
    CHECK(used != 0 && !pf.group, "and reads it back as individually addressed");
    CHECK(used != 0 && umac_mesh_ctrl_ae(&pf.mc) == UMAC_MESH_CTRL_AE_NONE,
          "with AE mode 0");

    /* Two frames must not share a Mesh Control sequence number: the first
     * relay's duplicate cache is keyed on (source, seq) and would eat one. */
    uint32_t seq0 = rd32le(&f->bytes[34]);
    simnode_outbox_clear();
    (void)simnode_host_tx(A, W, PAY, sizeof(PAY));
    const struct simnode_frame *g = simnode_outbox_get(0);
    CHECK(g != NULL && g->len > 38u && rd32le(&g->bytes[34]) != seq0,
          "the next frame gets a different sequence number (%lu then %lu)",
          (unsigned long)seq0,
          (unsigned long)(g != NULL && g->len > 38u ? rd32le(&g->bytes[34]) : seq0));

    CHECK(simnode_stub_hits("umac_connection_get_state") == 0,
          "nothing fell through into STA-mode connection code");
}

/* ---- 2. MESHGRP=1: one standard 3-address group frame ------------------- */

static void t_tx_group_3addr_standard(void)
{
    printf("--- TX: MESHGRP=1 emits ONE standard 3-address group frame ---\n");
    fresh(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/true);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);

    CHECK(simnode_host_tx(BC, W, PAY, sizeof(PAY)), "host broadcasts");
    CHECK(simnode_outbox_count() == 1,
          "ONE transmission even with two peers -- the chip broadcasts it (got %u)",
          simnode_outbox_count());
    const struct simnode_frame *f = simnode_outbox_get(0);
    if (f == NULL) { return; }
    if (f->len != 40u + sizeof(PAY)) { hexdump("frame", f->bytes, f->len); }
    CHECK(f->len == 40u + sizeof(PAY),
          "24 MAC + 2 QoS + 6 Mesh Control + 8 SNAP + %u payload = %u bytes (got %u)",
          (unsigned)sizeof(PAY), (unsigned)(40u + sizeof(PAY)), f->len);
    if (f->len < 40u + sizeof(PAY)) { return; }

    CHECK(f->bytes[0] == 0x88 && f->bytes[1] == 0x02,
          "[0..1] FC = 0x0288: QoS Data, FromDS only -- 3 addresses (got %02x%02x)",
          f->bytes[1], f->bytes[0]);
    CHECK(MAC_EQ(&f->bytes[4], BC), "[4]  addr1 = the group address");
    CHECK(MAC_EQ(&f->bytes[10], W), "[10] addr2 = TA = us");
    CHECK(MAC_EQ(&f->bytes[16], W), "[16] addr3 = mesh SA = us (we originated it)");
    CHECK(f->bytes[24] == 0x00 && f->bytes[25] == 0x01,
          "[24..25] QoS Control sits at 24, not 30 (got %02x%02x)", f->bytes[25], f->bytes[24]);
    CHECK(f->bytes[26] == 0x00,
          "[26] Mesh Control flags = 0: a group frame we originated needs no AE (got %02x)",
          f->bytes[26]);
    CHECK(f->bytes[27] == UMAC_MESH_CTRL_TTL_DEFAULT, "[27] TTL = %u (got %u)",
          UMAC_MESH_CTRL_TTL_DEFAULT, f->bytes[27]);
    CHECK(memcmp(&f->bytes[32], SNAP_IPV4, sizeof(SNAP_IPV4)) == 0, "[32] SNAP follows");
    CHECK(memcmp(&f->bytes[40], PAY, sizeof(PAY)) == 0, "[40] the payload, unchanged");

    struct umac_mesh_rx_frame pf;
    uint16_t used = umac_mesh_fwd_parse_frame(f->bytes, f->len, &pf);
    CHECK(used == 32u, "the parser consumes 32 octets to the body (got %u)", used);
    CHECK(used != 0 && pf.group, "and reads it back as group-addressed");
}

/* ---- 3. MESHGRP=0: one AE-2 unicast replica per peer -------------------- */

static void t_tx_group_replica_ae2(void)
{
    printf("--- TX: MESHGRP=0 replicates a group frame as one AE-2 unicast per peer ---\n");
    fresh(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    g_warthog_tx_bcast_dup = 0;

    CHECK(simnode_host_tx(BC, W, PAY, sizeof(PAY)), "host broadcasts");
    CHECK(simnode_outbox_count() == 2, "two peers, two transmissions (got %u)",
          simnode_outbox_count());
    CHECK(g_warthog_tx_bcast_dup == 1,
          "one copy was made and the original took the other peer (dup=%lu)",
          (unsigned long)g_warthog_tx_bcast_dup);

    const struct simnode_frame *fa = frame_to(A);
    const struct simnode_frame *fc = frame_to(C);
    CHECK(fa != NULL && fc != NULL, "one copy addressed to each peer");
    if (fa == NULL || fc == NULL) { return; }

    if (fa->len != 58u + sizeof(PAY)) { hexdump("replica", fa->bytes, fa->len); }
    CHECK(fa->len == 58u + sizeof(PAY),
          "30 MAC + 2 QoS + 18 Mesh Control (AE 2) + 8 SNAP + %u = %u bytes (got %u)",
          (unsigned)sizeof(PAY), (unsigned)(58u + sizeof(PAY)), fa->len);
    if (fa->len < 58u + sizeof(PAY)) { return; }

    CHECK(fa->bytes[0] == 0x88 && fa->bytes[1] == 0x03,
          "[0..1] each replica is an ordinary 4-address unicast (got %02x%02x)",
          fa->bytes[1], fa->bytes[0]);
    CHECK(MAC_EQ(&fa->bytes[4], A) && MAC_EQ(&fa->bytes[16], A),
          "[4]/[16] addr1 and addr3 are BOTH the peer -- a unicast on air, pairwise-keyed");
    CHECK(MAC_EQ(&fa->bytes[24], W), "[24] addr4 = us");
    CHECK(fa->bytes[32] == UMAC_MESH_CTRL_AE_A5A6,
          "[32] Mesh Control flags = AE mode 2 (got %02x)", fa->bytes[32]);
    CHECK(MAC_EQ(&fa->bytes[38], BC),
          "[38] AE address 1 = the GROUP destination the 802.3 frame really had");
    CHECK(MAC_EQ(&fa->bytes[44], W), "[44] AE address 2 = the real source");
    CHECK(memcmp(&fa->bytes[50], SNAP_IPV4, sizeof(SNAP_IPV4)) == 0, "[50] SNAP follows");
    CHECK(memcmp(&fa->bytes[58], PAY, sizeof(PAY)) == 0, "[58] the payload, unchanged");

    /* Both replicas are the same frame, so they must carry the same identity:
     * a relay's RMC is keyed on (source, seq) and two seq numbers would make
     * one datagram look like two. */
    CHECK(fc->len == fa->len && rd32le(&fc->bytes[34]) == rd32le(&fa->bytes[34]),
          "both replicas carry ONE sequence number (%lu / %lu)",
          (unsigned long)rd32le(&fa->bytes[34]), (unsigned long)rd32le(&fc->bytes[34]));
    CHECK(MAC_EQ(&fc->bytes[4], C) && MAC_EQ(&fc->bytes[16], C) && MAC_EQ(&fc->bytes[38], BC),
          "the second replica is the same frame addressed to the other peer");

    /* The receive side must recognise the shape as a group frame again. */
    struct umac_mesh_rx_frame pf;
    CHECK(umac_mesh_fwd_parse_frame(fa->bytes, fa->len, &pf) != 0, "the parser accepts it");
    CHECK(umac_mesh_fwd_normalise_replica(&pf),
          "and the engine recognises it as a replicated group frame");
    CHECK(pf.group && MAC_EQ(pf.addr1, BC) && MAC_EQ(pf.addr3, W),
          "normalised: addr1 back to the group, addr3 back to the mesh source");
}

/* ---- 4. proxied source: a host behind us (bridge mode) ------------------ */

static void t_tx_proxied_source(void)
{
    printf("--- TX: a host behind us rides in Address Extension, addr4 stays the mesh node ---\n");
    fresh(/*fwd=*/false, /*bridge=*/true, /*grp_std=*/false);
    (void)simnode_add_peer(A);

    CHECK(simnode_host_tx(A, H, PAY, sizeof(PAY)), "a bridged host H sends to peer A");
    CHECK(simnode_outbox_count() == 1, "one frame (got %u)", simnode_outbox_count());
    const struct simnode_frame *f = simnode_outbox_get(0);
    if (f == NULL) { return; }
    if (f->len != 58u + sizeof(PAY)) { hexdump("frame", f->bytes, f->len); }
    CHECK(f->len == 58u + sizeof(PAY), "18-octet Mesh Control: %u bytes (got %u)",
          (unsigned)(58u + sizeof(PAY)), f->len);
    if (f->len < 58u + sizeof(PAY)) { return; }

    CHECK(MAC_EQ(&f->bytes[16], A), "[16] addr3 = the peer, the MESH destination");
    CHECK(MAC_EQ(&f->bytes[24], W),
          "[24] addr4 = US, not H -- the mesh source is the node, not the host behind it");
    CHECK(f->bytes[32] == UMAC_MESH_CTRL_AE_A5A6, "[32] AE mode 2 (got %02x)", f->bytes[32]);
    CHECK(MAC_EQ(&f->bytes[38], A), "[38] AE address 1 = the real 802.3 destination");
    CHECK(MAC_EQ(&f->bytes[44], H),
          "[44] AE address 2 = H, the proxied source -- this is what stops replies "
          "going to the wrong host");
}

/* ---- 5. proxied destination: a host behind a peer ----------------------- */

static void t_tx_proxied_destination(void)
{
    printf("--- TX: a destination learned as proxied is addressed to its mesh node ---\n");
    fresh(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/true);
    (void)simnode_add_peer(A);

    /* Learn it the way a real node does: A group-broadcasts on behalf of H2,
     * which is AE 1 on a 3-address frame. */
    uint8_t frame[256];
    struct umac_mesh_ctrl mc = { .flags = UMAC_MESH_CTRL_AE_A4, .ttl = 1, .seq = 5000 };
    memcpy(mc.eaddr1, H2, 6);
    uint16_t n = mk_grp(frame, BC, A, A, &mc, PAY, sizeof(PAY));
    CHECK(simnode_rx(frame, n, -50), "peer A broadcasts for a host H2 behind it");

    char paths[512] = { 0 };
    (void)simnode_render_paths(paths, sizeof(paths));
    /* Anchored: a relay-learned entry prints no relay= and no uni. */
    CHECK(strstr(paths, "host=000088 behind=00000a\r\n") != NULL,
          "the proxy table learned H2 behind A%s", strstr(paths, "host=") ? "" : " (no host= line)");

    simnode_outbox_clear();
    CHECK(simnode_host_tx(H2, W, PAY, sizeof(PAY)), "we now send to H2 directly");
    CHECK(simnode_outbox_count() == 1, "one frame (got %u)", simnode_outbox_count());
    const struct simnode_frame *f = simnode_outbox_get(0);
    if (f == NULL) { return; }
    if (f->len != 58u + sizeof(PAY)) { hexdump("frame", f->bytes, f->len); }
    CHECK(f->len == 58u + sizeof(PAY), "18-octet Mesh Control: %u bytes (got %u)",
          (unsigned)(58u + sizeof(PAY)), f->len);
    if (f->len < 58u + sizeof(PAY)) { return; }

    CHECK(MAC_EQ(&f->bytes[4], A), "[4]  addr1 = A: the next hop is the mesh node");
    CHECK(MAC_EQ(&f->bytes[16], A),
          "[16] addr3 = A, NOT H2 -- H2 is not a mesh node and cannot be a mesh DA");
    CHECK(MAC_EQ(&f->bytes[24], W), "[24] addr4 = us");
    CHECK(f->bytes[32] == UMAC_MESH_CTRL_AE_A5A6, "[32] AE mode 2 (got %02x)", f->bytes[32]);
    CHECK(MAC_EQ(&f->bytes[38], H2), "[38] AE address 1 = H2, the real destination");
    CHECK(MAC_EQ(&f->bytes[44], W), "[44] AE address 2 = us, the real source");
}

/* ---- 6. receive: deliver ------------------------------------------------ */

static void t_rx_deliver(void)
{
    printf("--- RX: a frame addressed to us reaches the host netif ---\n");
    fresh(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false);
    (void)simnode_add_peer(A);

    uint8_t frame[256];
    struct umac_mesh_ctrl mc = { .flags = 0, .ttl = 31, .seq = 100 };
    uint16_t n = mk_uni(frame, W, A, W, A, &mc, PAY, sizeof(PAY));
    CHECK(simnode_rx(frame, n, -60), "A sends us a 4-address mesh frame");

    CHECK(simnode_outbox_count() == 0, "nothing was relayed (got %u)", simnode_outbox_count());
    CHECK(simnode_host_rx_count() == 1, "one frame reached the host (got %u)",
          simnode_host_rx_count());
    const struct simnode_hostrx *r = simnode_host_rx_get(0);
    if (r == NULL) { return; }
    CHECK(MAC_EQ(r->da, W), "the rebuilt 802.3 destination is us");
    CHECK(MAC_EQ(r->sa, A), "and the source is A");
    CHECK(r->len == sizeof(PAY) && memcmp(r->payload, PAY, sizeof(PAY)) == 0,
          "with the payload intact (%u bytes)", r->len);

    /* The per-peer RSSI the datapath reports is fed from this same path. */
    int16_t rssi = 0;
    CHECK(simnode_rssi_for(A, &rssi) && rssi == -60,
          "and the datapath reported A's RSSI (%d)", rssi);
}

/* ---- 7. receive: relay, with the TTL decremented ------------------------ */

static void t_rx_forward_unicast(void)
{
    printf("--- RX: a frame for a third node is relayed, TTL down by one ---\n");
    fresh(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);

    uint8_t frame[256];
    struct umac_mesh_ctrl mc = { .flags = 0, .ttl = 31, .seq = 200 };
    uint16_t n = mk_uni(frame, W, A, C, A, &mc, PAY, sizeof(PAY));
    CHECK(simnode_rx(frame, n, -60), "A sends a frame whose mesh DA is C");

    CHECK(simnode_host_rx_count() == 0,
          "it is NOT delivered to our host: it was never for us (got %u)",
          simnode_host_rx_count());
    CHECK(simnode_outbox_count() == 1, "exactly one relayed copy (got %u)",
          simnode_outbox_count());
    const struct simnode_frame *f = simnode_outbox_get(0);
    if (f == NULL) { return; }
    if (f->len != 46u + sizeof(PAY)) { hexdump("relayed", f->bytes, f->len); }
    CHECK(f->len == 46u + sizeof(PAY), "a 4-address frame with a 6-octet Mesh Control (got %u)",
          f->len);
    if (f->len < 46u + sizeof(PAY)) { return; }

    CHECK(MAC_EQ(&f->bytes[4], C),  "[4]  addr1 = C: the next hop");
    CHECK(MAC_EQ(&f->bytes[10], W), "[10] addr2 = us: a relay replaces the transmitter address");
    CHECK(MAC_EQ(&f->bytes[16], C), "[16] addr3 = C: the mesh destination is carried through");
    CHECK(MAC_EQ(&f->bytes[24], A),
          "[24] addr4 = A, NOT us -- a relay must not claim the frame as its own");
    CHECK(f->bytes[33] == 30u, "[33] TTL 31 -> 30 (got %u)", f->bytes[33]);
    CHECK(rd32le(&f->bytes[34]) == 200u,
          "[34] the originator's sequence number survives the hop (got %lu)",
          (unsigned long)rd32le(&f->bytes[34]));
    CHECK(memcmp(&f->bytes[46], PAY, sizeof(PAY)) == 0, "[46] the payload is untouched");

    /* One more hop's worth, to show the decrement is per-relay and not a
     * constant: the same frame arriving at TTL 3 leaves at 2. */
    simnode_outbox_clear();
    mc.ttl = 3; mc.seq = 201;
    n = mk_uni(frame, W, A, C, A, &mc, PAY, sizeof(PAY));
    (void)simnode_rx(frame, n, -60);
    const struct simnode_frame *g = simnode_outbox_get(0);
    CHECK(g != NULL && g->len > 34u && g->bytes[33] == 2u,
          "a frame arriving at TTL 3 leaves at 2 (got %u)",
          (g != NULL && g->len > 34u) ? g->bytes[33] : 0u);
}

/* ---- 7b. receive: a relayed EAPOL frame is relayed, not swallowed --------
 *
 * The forwarding engine decides a frame's fate while its Mesh Control is still
 * in place, but the relay itself happens further down the RX path -- and the
 * EAPOL gate sits between the two. That gate drops every 4-address EAPOL frame
 * as "not supported", which is right for EAPOL addressed to us and wrong for
 * EAPOL we were only asked to carry: a relay must not care what it carries.
 * The observable failure is a silent blackhole with rxdrop reason 9. */

static void t_rx_forward_eapol(void)
{
    printf("--- RX: an EAPOL frame for a third node is relayed, not dropped ---\n");
    fresh(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);

    uint8_t frame[256];
    struct umac_mesh_ctrl mc = { .flags = 0, .ttl = 31, .seq = 400 };
    uint16_t n = umac_mesh_ies_build_data_hdr4(frame, W, A, C, A);
    frame[n++] = 0x00;
    frame[n++] = 0x01;
    n = (uint16_t)(n + umac_mesh_ctrl_build(&frame[n], 18u, &mc));
    memcpy(&frame[n], SNAP_EAPOL, sizeof(SNAP_EAPOL));
    n = (uint16_t)(n + sizeof(SNAP_EAPOL));
    memcpy(&frame[n], PAY, sizeof(PAY));
    n = (uint16_t)(n + sizeof(PAY));
    CHECK(simnode_rx(frame, n, -60), "A sends an EAPOL frame whose mesh DA is C");

    CHECK(g_warthog_rxdrop_reason != 9u,
          "it is not dropped as 'unsupported 4-address EAPOL' (rxdrop reason %lu)",
          (unsigned long)g_warthog_rxdrop_reason);
    CHECK(simnode_host_rx_count() == 0, "it is NOT delivered to our host (got %u)",
          simnode_host_rx_count());
    CHECK(simnode_outbox_count() == 1, "exactly one relayed copy (got %u)",
          simnode_outbox_count());
    const struct simnode_frame *f = simnode_outbox_get(0);
    if (f != NULL && f->len >= 46u + sizeof(PAY))
    {
        CHECK(MAC_EQ(&f->bytes[4], C), "[4]  addr1 = C: the next hop");
        CHECK(MAC_EQ(&f->bytes[16], C), "[16] addr3 = C: the mesh destination is carried through");
        CHECK(f->bytes[44] == 0x88u && f->bytes[45] == 0x8Eu,
              "[44] the ethertype is still EAPOL (got %02x%02x)", f->bytes[44], f->bytes[45]);
        CHECK(memcmp(&f->bytes[46], PAY, sizeof(PAY)) == 0, "[46] the payload is untouched");
    }

    /* The same for one held while we discover its mesh DA: carried, not judged. */
    simnode_outbox_clear();
    mc.seq = 403;
    n = umac_mesh_ies_build_data_hdr4(frame, W, A, E, A);
    frame[n++] = 0x00;
    frame[n++] = 0x01;
    n = (uint16_t)(n + umac_mesh_ctrl_build(&frame[n], 18u, &mc));
    memcpy(&frame[n], SNAP_EAPOL, sizeof(SNAP_EAPOL));
    n = (uint16_t)(n + sizeof(SNAP_EAPOL));
    memcpy(&frame[n], PAY, sizeof(PAY));
    n = (uint16_t)(n + sizeof(PAY));
    (void)simnode_rx(frame, n, -60);
    const struct simnode_frame *q = simnode_outbox_get(0);
    CHECK(g_warthog_rxdrop_reason == 100u + UMAC_MESH_FWD_DROP_NO_PATH && simnode_outbox_count() == 1 &&
              q != NULL && q->is_mgmt && q->len >= 27u && q->bytes[26] == HWMP_EID_PREQ &&
              simnode_host_rx_count() == 0,
          "an EAPOL frame for unroutable E is held and asked for, not refused as ours "
          "(reason %lu, %u out)", (unsigned long)g_warthog_rxdrop_reason, simnode_outbox_count());

    /* The gate itself stays: 4-address EAPOL addressed to US is still refused. */
    fresh(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false);
    (void)simnode_add_peer(A);
    n = umac_mesh_ies_build_data_hdr4(frame, W, A, W, A);
    frame[n++] = 0x00;
    frame[n++] = 0x01;
    mc.seq = 401;
    n = (uint16_t)(n + umac_mesh_ctrl_build(&frame[n], 18u, &mc));
    memcpy(&frame[n], SNAP_EAPOL, sizeof(SNAP_EAPOL));
    n = (uint16_t)(n + sizeof(SNAP_EAPOL));
    memcpy(&frame[n], PAY, sizeof(PAY));
    n = (uint16_t)(n + sizeof(PAY));
    (void)simnode_rx(frame, n, -60);
    CHECK(g_warthog_rxdrop_reason == 9u && simnode_outbox_count() == 0,
          "4-address EAPOL addressed to us is still refused (reason %lu, %u sent)",
          (unsigned long)g_warthog_rxdrop_reason, simnode_outbox_count());

    /* Group-addressed EAPOL is DELIVER_AND_FORWARD: it must be re-flooded like
     * any other group frame, and reach our stack as an ordinary 802.3 frame. */
    fresh(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/true);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    mc.seq = 402;
    n = umac_mesh_ies_build_data_hdr3_group(frame, BC, A, A);
    frame[n++] = 0x00;
    frame[n++] = 0x01;
    n = (uint16_t)(n + umac_mesh_ctrl_build(&frame[n], 18u, &mc));
    memcpy(&frame[n], SNAP_EAPOL, sizeof(SNAP_EAPOL));
    n = (uint16_t)(n + sizeof(SNAP_EAPOL));
    memcpy(&frame[n], PAY, sizeof(PAY));
    n = (uint16_t)(n + sizeof(PAY));
    CHECK(simnode_rx(frame, n, -60), "A broadcasts a group EAPOL frame");
    CHECK(simnode_outbox_count() == 1, "it is re-flooded once (got %u)", simnode_outbox_count());
    CHECK(simnode_host_rx_count() == 1, "and delivered to our stack (got %u)",
          simnode_host_rx_count());
    CHECK(g_warthog_rxdrop_reason != 9u && g_warthog_rxdrop_reason != 10u,
          "not swallowed by the EAPOL gate (rxdrop reason %lu)",
          (unsigned long)g_warthog_rxdrop_reason);
}

/* ---- 7c. receive: a KEYED relay ------------------------------------------
 *
 * The shipping default is a keyed mesh, and until this case every relay test
 * ran open -- so the plaintext refusal, the not-decrypted drop, the CCMP replay
 * check and the MIC strip had never executed for a frame we forward, and
 * nothing showed that the forwarded copy is itself sent encrypted.
 *
 * On RX the chip decrypts in place and hands up header + CCMP header +
 * plaintext + MIC octets with MMDRV_RX_FLAG_DECRYPTED. On TX the host sets the
 * Protected bit, HW_ENC and the key index, and the chip adds CCMP header and
 * MIC -- so the relayed copy's bytes carry neither, and tx_flags/key_idx are
 * the record that it went out keyed. */

static uint16_t mk_keyed_(uint8_t *f, const uint8_t *ra, const uint8_t *ta, const uint8_t *da,
                          const uint8_t *sa, uint8_t pn, bool is_protected, uint16_t seq)
{
    uint16_t n = umac_mesh_ies_build_data_hdr4(f, ra, ta, da, sa);
    if (is_protected) { f[1] |= 0x40; } /* FC Protected (bit 14) */
    f[n++] = 0x00;
    f[n++] = 0x01;
    if (is_protected)
    {
        /* CCMP header: PN0 PN1 rsvd KeyID|ExtIV PN2..PN5, key id 0. */
        const uint8_t ccmp[8] = { pn, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00 };
        memcpy(&f[n], ccmp, sizeof(ccmp));
        n = (uint16_t)(n + sizeof(ccmp));
    }
    struct umac_mesh_ctrl mc = { .flags = 0, .ttl = 31, .seq = seq };
    n = (uint16_t)(n + umac_mesh_ctrl_build(&f[n], 18u, &mc));
    memcpy(&f[n], SNAP_IPV4, sizeof(SNAP_IPV4));
    n = (uint16_t)(n + sizeof(SNAP_IPV4));
    memcpy(&f[n], PAY, sizeof(PAY));
    n = (uint16_t)(n + sizeof(PAY));
    if (is_protected)
    {
        memset(&f[n], 0xA5, 8); /* MIC octets: present, value already checked by the chip */
        n = (uint16_t)(n + 8u);
    }
    return n;
}

static void t_rx_forward_keyed(void)
{
    printf("--- RX: a KEYED relay -- every crypto gate runs, and the copy goes out keyed ---\n");
    fresh(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false);
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/true);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);

    uint8_t frame[256];
    uint16_t n;

    /* Baseline: keyed and addressed to us, it is delivered. */
    n = mk_keyed_(frame, W, A, W, A, /*pn=*/1, true, 500);
    (void)simnode_rx_flags(frame, n, -60, MMDRV_RX_FLAG_DECRYPTED);
    CHECK(simnode_host_rx_count() == 1,
          "a decrypted frame addressed to us is delivered (%u delivered, rxdrop reason %lu)",
          simnode_host_rx_count(), (unsigned long)g_warthog_rxdrop_reason);

    /* The relay. */
    simnode_host_rx_clear();
    simnode_outbox_clear();
    n = mk_keyed_(frame, W, A, C, A, /*pn=*/2, true, 501);
    CHECK(simnode_rx_flags(frame, n, -60, MMDRV_RX_FLAG_DECRYPTED),
          "A sends a decrypted frame whose mesh DA is C");
    CHECK(simnode_host_rx_count() == 0, "it is not delivered to our host (got %u)",
          simnode_host_rx_count());
    CHECK(g_warthog_rxdrop_reason == 99u, "it passes every crypto gate and is forwarded (reason %lu)",
          (unsigned long)g_warthog_rxdrop_reason);
    CHECK(simnode_outbox_count() == 1, "exactly one relayed copy (got %u)", simnode_outbox_count());
    const struct simnode_frame *f = simnode_outbox_get(0);
    if (f != NULL && f->len >= 46u + sizeof(PAY))
    {
        if (f->len != 46u + sizeof(PAY)) { hexdump("relayed", f->bytes, f->len); }
        CHECK(MAC_EQ(&f->bytes[4], C), "[4]  addr1 = C: the next hop");
        CHECK((f->bytes[1] & 0x40u) != 0u, "the relayed copy has the Protected bit set");
        CHECK((f->tx_flags & MMDRV_TX_FLAG_HW_ENC) != 0u,
              "and the chip is told to encrypt it (tx_flags 0x%02x)", f->tx_flags);
        CHECK(f->key_idx == 0u, "with the pairwise key, id 0 (got 0x%02x)", f->key_idx);
        CHECK(f->len == 46u + sizeof(PAY),
              "the host added no CCMP header or MIC of its own -- the chip does (len %u)", f->len);
        CHECK(f->len >= 46u + sizeof(PAY) && f->bytes[33] == 30u, "[33] TTL 31 -> 30 (got %u)",
              f->bytes[33]);
        CHECK(f->len >= 46u + sizeof(PAY) && memcmp(&f->bytes[46], PAY, sizeof(PAY)) == 0,
              "[46] the payload is untouched");
    }

    /* The same packet number again is a replay: it must not be relayed. */
    simnode_outbox_clear();
    n = mk_keyed_(frame, W, A, C, A, /*pn=*/2, true, 502);
    (void)simnode_rx_flags(frame, n, -60, MMDRV_RX_FLAG_DECRYPTED);
    CHECK(simnode_outbox_count() == 0 && g_warthog_rxdrop_reason == 5u,
          "a replayed packet number is dropped by the CCMP check, not relayed (reason %lu, %u sent)",
          (unsigned long)g_warthog_rxdrop_reason, simnode_outbox_count());

    /* A frame the chip could not decrypt is not relayed either. */
    simnode_outbox_clear();
    n = mk_keyed_(frame, W, A, C, A, /*pn=*/3, true, 503);
    (void)simnode_rx_flags(frame, n, -60, 0);
    CHECK(simnode_outbox_count() == 0 && g_warthog_rxdrop_reason == 4u,
          "a frame the chip did not decrypt is dropped, not relayed (reason %lu, %u sent)",
          (unsigned long)g_warthog_rxdrop_reason, simnode_outbox_count());

    /* Plaintext on a keyed link is refused before it could be relayed. */
    simnode_outbox_clear();
    n = mk_keyed_(frame, W, A, C, A, /*pn=*/0, false, 504);
    (void)simnode_rx_flags(frame, n, -60, 0);
    CHECK(simnode_outbox_count() == 0 && g_warthog_rxdrop_reason == 3u,
          "plaintext on a keyed link is refused, not relayed (reason %lu, %u sent)",
          (unsigned long)g_warthog_rxdrop_reason, simnode_outbox_count());
}

/* ---- 7d. leaf mode: a host behind a peer ---------------------------------
 *
 * The default node forwards nothing, but it still has to answer a host that
 * sits behind a bridged peer -- a laptop on an OpenMANET node's LAN, say. That
 * host is not a mesh node, so a frame to it must be addressed to the node it
 * sits behind, with the host in Address Extension; addressed to the host
 * directly, as leaf mode used to do, no mesh node accepts it. Everything else
 * a leaf sends must keep the shape measured on air. */

static void t_leaf_proxied_host(void)
{
    printf("--- leaf mode: a reply to a host behind a peer is addressed to that peer ---\n");
    fresh(/*fwd=*/false, /*bridge=*/false, /*grp_std=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);

    /* A delivers a frame to us on behalf of H2, a host behind it. */
    uint8_t frame[256];
    struct umac_mesh_ctrl mc = { .flags = UMAC_MESH_CTRL_AE_A5A6, .ttl = 31, .seq = 6000 };
    memcpy(mc.eaddr1, W, 6);
    memcpy(mc.eaddr2, H2, 6);
    uint16_t n = mk_uni(frame, W, A, W, A, &mc, PAY, sizeof(PAY));
    CHECK(simnode_rx(frame, n, -60), "A delivers a frame from H2, a host behind it");
    const struct simnode_hostrx *r = simnode_host_rx_get(0);
    CHECK(r != NULL && MAC_EQ(r->sa, H2), "it reaches our host as coming from H2");

    simnode_outbox_clear();
    CHECK(simnode_host_tx(H2, W, PAY, sizeof(PAY)), "our host replies to H2");
    CHECK(simnode_outbox_count() == 1, "one frame (got %u)", simnode_outbox_count());
    const struct simnode_frame *f = simnode_outbox_get(0);
    if (f != NULL && f->len != 58u + sizeof(PAY)) { hexdump("reply", f->bytes, f->len); }
    if (f != NULL && f->len == 58u + sizeof(PAY))
    {
        CHECK(MAC_EQ(&f->bytes[4], A), "[4]  addr1 = A, the node H2 sits behind");
        CHECK(MAC_EQ(&f->bytes[16], A), "[16] addr3 = A, not H2 -- a host cannot be a mesh DA");
        CHECK(MAC_EQ(&f->bytes[24], W), "[24] addr4 = us");
        CHECK(f->bytes[32] == UMAC_MESH_CTRL_AE_A5A6, "[32] AE mode 2 (got %02x)", f->bytes[32]);
        CHECK(MAC_EQ(&f->bytes[38], H2), "[38] AE address 1 = H2, the real destination");
        CHECK(MAC_EQ(&f->bytes[44], W), "[44] AE address 2 = us");
    }
    else
    {
        CHECK(false, "a proxied reply is 30 + 2 + 18 Mesh Control + 8 SNAP + payload = %u bytes "
                     "(got %u)", (unsigned)(58u + sizeof(PAY)), f ? f->len : 0u);
    }

    /* Nothing else changes shape. A broadcast is still one plain replica per
     * peer, and an unknown destination still goes to the first peer as-is --
     * no Address Extension, and no path request from a leaf. */
    simnode_outbox_clear();
    (void)simnode_host_tx(BC, W, PAY, sizeof(PAY));
    int plain = 0, mgmt = 0;
    for (unsigned i = 0; i < simnode_outbox_count(); i++)
    {
        const struct simnode_frame *g = simnode_outbox_get(i);
        if (g == NULL) { continue; }
        if (g->is_mgmt) { mgmt++; continue; }
        if (g->len == 46u + sizeof(PAY) && g->bytes[32] == 0x00u) { plain++; }
    }
    CHECK(plain == 2 && mgmt == 0,
          "a broadcast is still one plain replica per peer (%d plain of %u, %d mgmt)", plain,
          simnode_outbox_count(), mgmt);

    simnode_outbox_clear();
    (void)simnode_host_tx(E, W, PAY, sizeof(PAY));
    const struct simnode_frame *u = simnode_outbox_get(0);
    CHECK(simnode_outbox_count() == 1 && u != NULL && !u->is_mgmt && u->len == 46u + sizeof(PAY) &&
              MAC_EQ(&u->bytes[16], E) && u->bytes[32] == 0x00u,
          "an unknown destination still goes to the first peer unchanged, and no PREQ (%u frames)",
          simnode_outbox_count());

    /* A peer is never learned as a host behind someone else. */
    fresh(/*fwd=*/false, /*bridge=*/false, /*grp_std=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    memcpy(mc.eaddr2, C, 6);
    mc.seq = 6001;
    n = mk_uni(frame, W, A, W, A, &mc, PAY, sizeof(PAY));
    (void)simnode_rx(frame, n, -60);
    simnode_outbox_clear();
    (void)simnode_host_tx(C, W, PAY, sizeof(PAY));
    const struct simnode_frame *d = simnode_outbox_get(0);
    CHECK(d != NULL && MAC_EQ(&d->bytes[4], C) && MAC_EQ(&d->bytes[16], C) &&
              d->bytes[32] == 0x00u,
          "an AE frame naming peer C as a host behind A does not redirect traffic for C");

    /* The TX gate and classify are separate lock sections. If the proxy's peer
     * goes between them, a leaf still must not originate a PREQ. */
    {
        extern volatile uint32_t g_warthog_fwd_preq_tx;
        fresh(/*fwd=*/false, /*bridge=*/false, /*grp_std=*/false);
        (void)simnode_add_peer(A);
        (void)simnode_add_peer(C);
        memcpy(mc.eaddr2, H2, 6);
        mc.seq = 6003;
        n = mk_uni(frame, W, A, W, A, &mc, PAY, sizeof(PAY));
        (void)simnode_rx(frame, n, -60);
        uint8_t via[6];
        CHECK(umac_mesh_fwd_glue_proxy_via_peer(H2, via) && MAC_EQ(via, A), "gate: H2 via A");
        simnode_del_peer(A);
        simnode_outbox_clear();
        uint32_t preq0 = g_warthog_fwd_preq_tx;
        struct mmpkt *pkt = mmpkt_alloc_on_heap(64, 64, sizeof(struct mmdrv_tx_metadata));
        umac_mesh_fwd_glue_tx_classify(pkt, H2, W);
        mmpkt_release(pkt);
        simnode_pump();
        CHECK(g_warthog_fwd_preq_tx == preq0 && simnode_outbox_count() == 0u,
              "A gone before classify: no PREQ (%u sent, %u frames)",
              (unsigned)(g_warthog_fwd_preq_tx - preq0), simnode_outbox_count());
    }
}

/* ---- 7e. leaf mode: a host behind a node we do not hear ------------------
 *
 * Leaf W hears only A (and C where stated). Laptop H3 sits on bridge M's LAN;
 * A relays H3's frames to W. W's answer must be the shape mac80211 itself
 * emits from its own stack to a proxied host (tx.c ieee80211_build_hdr, the
 * DS -> MBSS branch): mesh DA = M, mesh SA = W, QoS Mesh Control Present, AE 2
 * carrying H3 and W, handed to A -- the peer that carried H3's traffic -- and
 * never a PREQ, because a leaf does not discover.
 *
 * Also pinned, through the shipping datapath: a flood alone teaches; the
 * unicast's relay is held against another relay's flood for
 * UMAC_MESH_LEAF_VIA_HOLD_MS and yields when it goes; the second copy of a
 * flood through another relay does not move the reply; a host behind a direct
 * peer goes to that peer although a relay carried its flood; a frame for a
 * third party (reason 93) teaches nothing; a peer named as a host redirects
 * nothing. AT+MESHPATH? shows each leaf entry's relay and its unicast pin. */

static const uint8_t M[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x4d }; /* bridge, not a peer */
static const uint8_t H3[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x93 }; /* host behind M */
static const uint8_t H4[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x94 }; /* another host behind M */
static const uint8_t H5[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x95 }; /* host behind peer C */
static const uint8_t H9[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x99 }; /* host in a third party's frame */

/* A unicast from @p host behind M, relayed to us by @p ta. */
static uint16_t mk_from_m_(uint8_t *f, const uint8_t *ta, const uint8_t *host, uint32_t seq)
{
    struct umac_mesh_ctrl mc = { .flags = UMAC_MESH_CTRL_AE_A5A6, .ttl = 30, .seq = seq };
    memcpy(mc.eaddr1, W, 6);
    memcpy(mc.eaddr2, host, 6);
    return mk_uni(f, W, ta, W, M, &mc, PAY, sizeof(PAY));
}

/* A broadcast from @p host behind @p node (an ARP request, say), re-flooded to us by @p ta. */
static uint16_t mk_flood_(uint8_t *f, const uint8_t *ta, const uint8_t *node, const uint8_t *host,
                          uint32_t seq, uint8_t ttl)
{
    struct umac_mesh_ctrl mc = { .flags = UMAC_MESH_CTRL_AE_A4, .ttl = ttl, .seq = seq };
    memcpy(mc.eaddr1, host, 6);
    return mk_grp(f, BC, ta, node, &mc, PAY, sizeof(PAY));
}

static uint16_t mk_flood_from_m_(uint8_t *f, const uint8_t *ta, const uint8_t *host, uint32_t seq)
{
    return mk_flood_(f, ta, M, host, seq, 30);
}

/* The one data frame our host's reply to @p host produced, checked byte for byte:
 * mesh DA @p node, handed to @p ra. */
static void expect_reply_to_(const uint8_t *host, const uint8_t *node, const uint8_t *ra, const char *what)
{
    extern volatile uint32_t g_warthog_fwd_preq_tx;
    uint32_t preq0 = g_warthog_fwd_preq_tx;
    simnode_outbox_clear();
    (void)simnode_host_tx(host, W, PAY, sizeof(PAY));
    const struct simnode_frame *f = simnode_outbox_get(0);
    bool one = simnode_outbox_count() == 1u && f != NULL && !f->is_mgmt;
    CHECK(one && f->len == 58u + sizeof(PAY), "%s: one data frame with an 18-octet Mesh Control "
          "(%u frames, %u bytes)", what, simnode_outbox_count(), f ? f->len : 0u);
    if (!one || f->len != 58u + sizeof(PAY)) { if (f) { hexdump("reply", f->bytes, f->len); } return; }
    CHECK(MAC_EQ(&f->bytes[4], ra), "  [4]  addr1 = %02x, the peer it is handed to", ra[5]);
    CHECK(MAC_EQ(&f->bytes[10], W), "  [10] addr2 = us");
    CHECK(MAC_EQ(&f->bytes[16], node), "  [16] addr3 = %02x, the node it sits behind -- not the host",
          node[5]);
    CHECK(MAC_EQ(&f->bytes[24], W), "  [24] addr4 = us");
    CHECK(f->bytes[30] == 0x00 && f->bytes[31] == 0x01,
          "  [30..31] QoS Control = TID 0 with Mesh Control Present (got %02x%02x)",
          f->bytes[31], f->bytes[30]);
    CHECK(f->bytes[32] == UMAC_MESH_CTRL_AE_A5A6, "  [32] AE mode 2 (got %02x)", f->bytes[32]);
    CHECK(MAC_EQ(&f->bytes[38], host), "  [38] AE address 1 = the host");
    CHECK(MAC_EQ(&f->bytes[44], W), "  [44] AE address 2 = us");
    CHECK(g_warthog_fwd_preq_tx == preq0, "  and no PREQ: a leaf never discovers (%u sent)",
          (unsigned)(g_warthog_fwd_preq_tx - preq0));
}

static void expect_reply_(const uint8_t *host, const uint8_t *ra, const char *what)
{
    expect_reply_to_(host, M, ra, what);
}

static void t_leaf_nonneighbour_host(void)
{
    printf("--- leaf mode: a reply to a host behind a non-neighbour goes through the relay ---\n");
    uint8_t frame[256];
    uint16_t n;
    char paths[512];

    fresh(/*fwd=*/false, /*bridge=*/false, /*grp_std=*/false);
    (void)simnode_add_peer(A);
    n = mk_from_m_(frame, A, H3, 7000);
    (void)simnode_rx(frame, n, -60);
    const struct simnode_hostrx *r = simnode_host_rx_get(0);
    CHECK(simnode_host_rx_count() == 1u && r != NULL && MAC_EQ(r->sa, H3),
          "A relays a frame from H3, behind non-neighbour M: delivered as from H3");
    (void)simnode_render_paths(paths, sizeof(paths));
    CHECK(strstr(paths, "host=000093 behind=00004d relay=00000a uni\r\n") != NULL,
          "H3 is learned behind M, through relay A, from a unicast to us");
    expect_reply_(H3, A, "reply to H3");

    /* An ARP request arrives as a flood before any unicast: it must be enough. */
    n = mk_flood_from_m_(frame, A, H4, 7001);
    (void)simnode_rx(frame, n, -60);
    (void)simnode_render_paths(paths, sizeof(paths));
    CHECK(strstr(paths, "host=000094 behind=00004d relay=00000a\r\n") != NULL,
          "H4 is learned from a flood alone, without unicast evidence");
    expect_reply_(H4, A, "ARP reply to H4");

    /* Two relays. A carried H3's unicast; C re-floods H3's broadcast. A holds a
     * path to M, C need not: C must not take over while A's path can be live. */
    fresh(/*fwd=*/false, /*bridge=*/false, /*grp_std=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    n = mk_from_m_(frame, A, H3, 7002);
    (void)simnode_rx(frame, n, -60);
    n = mk_flood_from_m_(frame, C, H3, 7003);
    (void)simnode_rx(frame, n, -60);
    expect_reply_(H3, A, "unicast evidence outranks a flood's transmitter");
    simnode_advance_ms(UMAC_MESH_LEAF_VIA_HOLD_MS);
    n = mk_flood_from_m_(frame, C, H3, 7004);
    (void)simnode_rx(frame, n, -60);
    expect_reply_(H3, C, "once A's path to M may have lapsed, the flood's relay");

    /* The relay goes: the host is still answered in its proper shape, via the
     * only peer left, and still without discovery. */
    simnode_del_peer(C);
    expect_reply_(H3, A, "with C gone, the remaining peer (A)");

    /* One flood, heard through both relays: the first copy decides. */
    fresh(/*fwd=*/false, /*bridge=*/false, /*grp_std=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    n = mk_flood_(frame, A, M, H4, 7010, 30);
    (void)simnode_rx(frame, n, -60);
    n = mk_flood_(frame, C, M, H4, 7010, 29);
    (void)simnode_rx(frame, n, -60);
    (void)simnode_render_paths(paths, sizeof(paths));
    CHECK(strstr(paths, "host=000094 behind=00004d relay=00000a\r\n") != NULL,
          "the same flood (M, seq 7010) again through C is a duplicate: H4 stays with A");
    expect_reply_(H4, A, "reply to H4 after the duplicate");

    /* H5 sits behind peer C; A re-floods its broadcast. C is the node: the reply
     * goes straight to C, whoever carried the flood. */
    fresh(/*fwd=*/false, /*bridge=*/false, /*grp_std=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    n = mk_flood_(frame, A, C, H5, 7011, 30);
    (void)simnode_rx(frame, n, -60);
    expect_reply_to_(H5, C, C, "a host behind direct peer C, flooded by A, goes to C");

    /* A frame for a third party is dropped at the reason-93 gate, before
     * anything is learned from its Address Extension. */
    fresh(/*fwd=*/false, /*bridge=*/false, /*grp_std=*/false);
    (void)simnode_add_peer(A);
    {
        struct umac_mesh_ctrl mt = { .flags = UMAC_MESH_CTRL_AE_A5A6, .ttl = 30, .seq = 7012 };
        memcpy(mt.eaddr1, E, 6);
        memcpy(mt.eaddr2, H9, 6);
        n = mk_uni(frame, W, A, E, M, &mt, PAY, sizeof(PAY));
    }
    (void)simnode_rx(frame, n, -60);
    (void)simnode_render_paths(paths, sizeof(paths));
    CHECK(g_warthog_rxdrop_reason == 93u && simnode_host_rx_count() == 0u &&
              strstr(paths, "proxies=0") != NULL,
          "a frame for third party E is dropped (reason %lu) and teaches nothing",
          (unsigned long)g_warthog_rxdrop_reason);

    /* Refusals. A peer, ourselves and a group address are never hosts. */
    fresh(/*fwd=*/false, /*bridge=*/false, /*grp_std=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    n = mk_from_m_(frame, A, C, 7005);
    (void)simnode_rx(frame, n, -60);
    simnode_outbox_clear();
    (void)simnode_host_tx(C, W, PAY, sizeof(PAY));
    const struct simnode_frame *d = simnode_outbox_get(0);
    CHECK(d != NULL && MAC_EQ(&d->bytes[4], C) && MAC_EQ(&d->bytes[16], C) && d->bytes[32] == 0u,
          "a claim that peer C sits behind M does not redirect traffic for C");
    (void)simnode_render_paths(paths, sizeof(paths));
    CHECK(strstr(paths, "proxies=0") != NULL, "and nothing was learned");
}

/* ---- 8. receive: group frame delivered AND re-flooded ------------------- */

static void t_rx_group_deliver_and_forward(void)
{
    printf("--- RX: a group frame is delivered and re-flooded, mesh source preserved ---\n");
    fresh(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/true);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);

    uint8_t frame[256];
    struct umac_mesh_ctrl mc = { .flags = 0, .ttl = 31, .seq = 300 };
    uint16_t n = mk_grp(frame, BC, A, A, &mc, PAY, sizeof(PAY));
    CHECK(simnode_rx(frame, n, -60), "A broadcasts a standard 3-address group frame");

    CHECK(simnode_host_rx_count() == 1, "delivered to our host (got %u)",
          simnode_host_rx_count());
    const struct simnode_hostrx *r = simnode_host_rx_get(0);
    CHECK(r != NULL && MAC_EQ(r->da, BC) && MAC_EQ(r->sa, A),
          "as a broadcast from A");

    CHECK(simnode_outbox_count() == 1, "and re-flooded once (got %u)", simnode_outbox_count());
    const struct simnode_frame *f = simnode_outbox_get(0);
    if (f == NULL) { return; }
    if (f->len != 40u + sizeof(PAY)) { hexdump("reflooded", f->bytes, f->len); }
    CHECK(f->len == 40u + sizeof(PAY), "still a 3-address group frame (got %u)", f->len);
    if (f->len < 40u + sizeof(PAY)) { return; }

    CHECK(MAC_EQ(&f->bytes[4], BC),  "[4]  addr1 = still the group address");
    CHECK(MAC_EQ(&f->bytes[10], W),  "[10] addr2 = us: we are the transmitter now");
    CHECK(MAC_EQ(&f->bytes[16], A),
          "[16] addr3 = A, the ORIGINAL mesh source -- every duplicate cache "
          "downstream is keyed on it");
    CHECK(f->bytes[27] == 30u, "[27] TTL 31 -> 30 (got %u)", f->bytes[27]);
    CHECK(rd32le(&f->bytes[28]) == 300u, "[28] sequence number 300 carried through (got %lu)",
          (unsigned long)rd32le(&f->bytes[28]));
    CHECK(memcmp(&f->bytes[40], PAY, sizeof(PAY)) == 0, "[40] payload untouched");

    /* ---- the duplicate cache ---- */
    simnode_outbox_clear();
    simnode_host_rx_clear();
    uint32_t drops = g_warthog_rxdrop_count;
    (void)simnode_rx(frame, n, -60);
    CHECK(g_warthog_rxdrop_count == drops + 1 &&
          g_warthog_rxdrop_reason == 100u + UMAC_MESH_FWD_DROP_DUP,
          "the same (source, seq) again is dropped as a duplicate (reason %lu)",
          (unsigned long)g_warthog_rxdrop_reason);
    CHECK(simnode_outbox_count() == 0, "not re-flooded a second time (got %u)",
          simnode_outbox_count());
    CHECK(simnode_host_rx_count() == 0, "and not delivered twice (got %u)",
          simnode_host_rx_count());

    /* A different sequence number from the same source is NOT a duplicate --
     * otherwise the cache would simply be eating that source's traffic. */
    mc.seq = 301;
    n = mk_grp(frame, BC, A, A, &mc, PAY, sizeof(PAY));
    (void)simnode_rx(frame, n, -60);
    CHECK(simnode_host_rx_count() == 1 && simnode_outbox_count() == 1,
          "the next sequence number from A goes through (delivered %u, flooded %u)",
          simnode_host_rx_count(), simnode_outbox_count());
}

/* ---- 9. receive: a replicated group frame is recognised and re-flooded --- */

static void t_rx_replica_normalise(void)
{
    printf("--- RX: an AE-2 unicast replica is a group frame, and is re-flooded onward ---\n");
    fresh(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);

    /* Exactly what another warthog emits for a broadcast from a host behind
     * it: 4-address to us, AE 2 holding the group DA and the real source. */
    uint8_t frame[256];
    struct umac_mesh_ctrl mc = { .flags = UMAC_MESH_CTRL_AE_A5A6, .ttl = 31, .seq = 400 };
    memcpy(mc.eaddr1, BC, 6);
    memcpy(mc.eaddr2, H2, 6);
    uint16_t n = mk_uni(frame, W, A, W, A, &mc, PAY, sizeof(PAY));
    CHECK(simnode_rx(frame, n, -60), "A sends us a replicated broadcast");

    CHECK(simnode_host_rx_count() == 1, "delivered to our host (got %u)",
          simnode_host_rx_count());
    const struct simnode_hostrx *r = simnode_host_rx_get(0);
    CHECK(r != NULL && MAC_EQ(r->da, BC),
          "as a BROADCAST, not as a unicast to us -- the AE told us what it really was");
    CHECK(r != NULL && MAC_EQ(r->sa, H2),
          "from H2, the host behind A, not from A itself");

    CHECK(simnode_outbox_count() == 1,
          "re-flooded to the OTHER peer only: one copy, not two (got %u)",
          simnode_outbox_count());
    const struct simnode_frame *f = frame_to(C);
    CHECK(f != NULL, "and it goes to C");
    CHECK(frame_to(A) == NULL, "never back to A, which is where it came from");
    if (f == NULL) { return; }
    if (f->len != 58u + sizeof(PAY)) { hexdump("reflooded", f->bytes, f->len); }
    CHECK(f->len == 58u + sizeof(PAY), "as another AE-2 replica (got %u)", f->len);
    if (f->len < 58u + sizeof(PAY)) { return; }

    CHECK(MAC_EQ(&f->bytes[16], C), "[16] addr3 = C: this copy is addressed to C");
    CHECK(MAC_EQ(&f->bytes[24], A),
          "[24] addr4 = A: the mesh source of the flood, not us");
    CHECK(f->bytes[32] == UMAC_MESH_CTRL_AE_A5A6, "[32] AE mode 2 again (got %02x)",
          f->bytes[32]);
    CHECK(f->bytes[33] == 30u, "[33] TTL 31 -> 30 (got %u)", f->bytes[33]);
    CHECK(rd32le(&f->bytes[34]) == 400u, "[34] sequence number 400 preserved (got %lu)",
          (unsigned long)rd32le(&f->bytes[34]));
    CHECK(MAC_EQ(&f->bytes[38], BC), "[38] AE address 1 = still the group destination");
    CHECK(MAC_EQ(&f->bytes[44], H2),
          "[44] AE address 2 = still H2 -- the originating host survives the relay");
}

/* ---- 10. receive: proxied endpoints on a unicast to us ------------------ */

static void t_rx_proxied_endpoints(void)
{
    printf("--- RX: AE 2 on a unicast to us names the real endpoints ---\n");
    fresh(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false);
    (void)simnode_add_peer(A);

    uint8_t frame[256];
    struct umac_mesh_ctrl mc = { .flags = UMAC_MESH_CTRL_AE_A5A6, .ttl = 31, .seq = 500 };
    memcpy(mc.eaddr1, H, 6);   /* proxied destination: a host behind US */
    memcpy(mc.eaddr2, H2, 6);  /* proxied source: a host behind A */
    uint16_t n = mk_uni(frame, W, A, W, A, &mc, PAY, sizeof(PAY));
    CHECK(simnode_rx(frame, n, -60), "A relays for H2, addressed to H behind us");

    CHECK(simnode_outbox_count() == 0, "nothing relayed: the mesh DA is us (got %u)",
          simnode_outbox_count());
    CHECK(simnode_host_rx_count() == 1, "delivered once (got %u)", simnode_host_rx_count());
    const struct simnode_hostrx *r = simnode_host_rx_get(0);
    if (r == NULL) { return; }
    CHECK(MAC_EQ(r->da, H),
          "the 802.3 destination is H, the host behind us -- not our own address");
    CHECK(MAC_EQ(r->sa, H2), "and the source is H2, the host behind A");

    char paths[512] = { 0 };
    (void)simnode_render_paths(paths, sizeof(paths));
    CHECK(strstr(paths, "host=000088 behind=00000a\r\n") != NULL,
          "and H2 was learned as a host behind A, as a relay entry");
}

/* ---- 11. receive: the drop verdicts ------------------------------------- */

static void rx_expect_drop_(const char *what, const uint8_t *frame, uint16_t n,
                            enum umac_mesh_fwd_drop why)
{
    simnode_outbox_clear();
    simnode_host_rx_clear();
    uint32_t before = g_warthog_rxdrop_count;
    (void)simnode_rx(frame, n, -60);
    CHECK(g_warthog_rxdrop_count > before && g_warthog_rxdrop_reason == 100u + (uint32_t)why,
          "%s -> dropped, reason %u (got %lu, %lu drops)", what, 100u + (unsigned)why,
          (unsigned long)g_warthog_rxdrop_reason,
          (unsigned long)(g_warthog_rxdrop_count - before));
    CHECK(simnode_outbox_count() == 0 && simnode_host_rx_count() == 0,
          "   and nothing was relayed or delivered (%u on air, %u to the host)",
          simnode_outbox_count(), simnode_host_rx_count());
}

static void t_rx_drops(void)
{
    printf("--- RX: the verdicts that refuse a frame ---\n");
    uint8_t frame[256];
    struct umac_mesh_ctrl mc;

    fresh(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/true);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);

    memset(&mc, 0, sizeof(mc)); mc.ttl = 0; mc.seq = 600;
    rx_expect_drop_("a group frame that arrived at TTL 0",
                    frame, mk_grp(frame, BC, A, A, &mc, PAY, sizeof(PAY)),
                    UMAC_MESH_FWD_DROP_TTL0);

    memset(&mc, 0, sizeof(mc)); mc.ttl = 1; mc.seq = 601;
    rx_expect_drop_("a relay frame with one hop left",
                    frame, mk_uni(frame, W, A, C, A, &mc, PAY, sizeof(PAY)),
                    UMAC_MESH_FWD_DROP_TTL);

    memset(&mc, 0, sizeof(mc)); mc.ttl = 31; mc.seq = 602;
    rx_expect_drop_("a unicast whose RA is somebody else",
                    frame, mk_uni(frame, C, A, E, A, &mc, PAY, sizeof(PAY)),
                    UMAC_MESH_FWD_DROP_NOT_FOR_US);

    /* AE 1 is the group form; on a 4-address unicast it is meaningless, and
     * guessing at what the sender meant is how an endpoint gets rewritten. */
    memset(&mc, 0, sizeof(mc)); mc.flags = UMAC_MESH_CTRL_AE_A4; mc.ttl = 31; mc.seq = 603;
    memcpy(mc.eaddr1, H2, 6);
    rx_expect_drop_("AE mode 1 on a 4-address unicast",
                    frame, mk_uni(frame, W, A, W, A, &mc, PAY, sizeof(PAY)),
                    UMAC_MESH_FWD_DROP_BAD_AE);

    /* AE 2 has no meaning on a real group frame either. (A replica is a
     * 4-address frame; this is a genuine 3-address one.) */
    memset(&mc, 0, sizeof(mc)); mc.flags = UMAC_MESH_CTRL_AE_A5A6; mc.ttl = 31; mc.seq = 604;
    memcpy(mc.eaddr1, H, 6); memcpy(mc.eaddr2, H2, 6);
    rx_expect_drop_("AE mode 2 on a 3-address group frame",
                    frame, mk_grp(frame, BC, A, A, &mc, PAY, sizeof(PAY)),
                    UMAC_MESH_FWD_DROP_BAD_AE);

    /* With forwarding off but bridging on, a frame for somebody else stops
     * here rather than being quietly delivered to our own host.
     *
     * Not asserted, because it does not hold: with BOTH AT+MESHFWD and
     * AT+MESHBRIDGE off, umac_datapath.c never calls the engine at all (the
     * gate at the Mesh Control strip is `if (g_warthog_mesh_fwd ||
     * g_warthog_mesh_bridge)`), and the frame is delivered to our own netif
     * with the 802.3 destination of the node it was really for. Measured on
     * this harness: one frame to the host, DA = the third node. An assertion
     * for the right behaviour would leave this suite red, so it is reported
     * rather than written down as if it passed. */
    fresh(/*fwd=*/false, /*bridge=*/true, /*grp_std=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    memset(&mc, 0, sizeof(mc)); mc.ttl = 31; mc.seq = 605;
    rx_expect_drop_("a relay frame with AT+MESHFWD off",
                    frame, mk_uni(frame, W, A, C, A, &mc, PAY, sizeof(PAY)),
                    UMAC_MESH_FWD_DROP_NO_FWD);

    /* No peer and no path to the mesh DA: held while we PREQ for it, as
     * OpenMANET's mac80211 (999-0027) does; the receive buffer itself is done. */
    fresh(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false);
    (void)simnode_add_peer(A);
    memset(&mc, 0, sizeof(mc)); mc.ttl = 31; mc.seq = 606;
    simnode_outbox_clear();
    uint32_t before = g_warthog_rxdrop_count;
    uint16_t n = mk_uni(frame, W, A, E, A, &mc, PAY, sizeof(PAY));
    (void)simnode_rx(frame, n, -60);
    CHECK(g_warthog_rxdrop_count > before &&
          g_warthog_rxdrop_reason == 100u + UMAC_MESH_FWD_DROP_NO_PATH,
          "a relay frame for an unreachable node -> its RX buffer done, reason %u (got %lu)",
          100u + (unsigned)UMAC_MESH_FWD_DROP_NO_PATH, (unsigned long)g_warthog_rxdrop_reason);
    const struct simnode_frame *preq = simnode_outbox_get(0);
    CHECK(simnode_outbox_count() == 1 && preq != NULL && preq->is_mgmt && preq->len >= 27u &&
              preq->bytes[4] == 0xff && preq->bytes[26] == HWMP_EID_PREQ,
          "and the one frame out is a broadcast PREQ, not a PERR to the sender (%u frames out)",
          simnode_outbox_count());
    CHECK(simnode_host_rx_count() == 0, "nothing reached our own host (%u)", simnode_host_rx_count());

    /* A LEAF must not hand a third party's traffic to its own IP stack.
     * With both gates off the forwarding engine never runs, so nothing else
     * compares the frame's mesh destination with our address and the 802.3
     * header would be built from addr3 -- another node. This was a real leak
     * in the shipping default until the datapath learned to drop it. */
    fresh(/*fwd=*/false, /*bridge=*/false, /*grp_std=*/false);
    (void)simnode_add_peer(A);
    (void)simnode_add_peer(C);
    memset(&mc, 0, sizeof(mc)); mc.ttl = 31; mc.seq = 607;
    simnode_host_rx_clear();
    simnode_outbox_clear();
    before = g_warthog_rxdrop_count;
    n = mk_uni(frame, W, A, C, A, &mc, PAY, sizeof(PAY));
    (void)simnode_rx(frame, n, -60);
    CHECK(simnode_host_rx_count() == 0,
          "leaf mode: a frame whose mesh DA is another node is NOT delivered to our host (%u delivered)",
          simnode_host_rx_count());
    CHECK(g_warthog_rxdrop_count > before && g_warthog_rxdrop_reason == 93u,
          "it is dropped with the leaf reason 93 (got %lu)", (unsigned long)g_warthog_rxdrop_reason);
    CHECK(simnode_outbox_count() == 0, "and nothing is relayed, because we do not forward");

    /* The same frame addressed to US is still delivered: the drop is aimed at
     * the mesh destination, not at 4-address frames in general. */
    memset(&mc, 0, sizeof(mc)); mc.ttl = 31; mc.seq = 608;
    simnode_host_rx_clear();
    n = mk_uni(frame, W, A, W, A, &mc, PAY, sizeof(PAY));
    (void)simnode_rx(frame, n, -60);
    CHECK(simnode_host_rx_count() == 1,
          "a leaf still delivers what is addressed to it (%u delivered)", simnode_host_rx_count());

    /* The same leak, without a Mesh Control field. The destination check above
     * lives inside the Mesh-Control branch, so a 4-address frame with the QoS
     * Mesh Control Present bit clear skipped it and was delivered with addr3 --
     * a third party -- as its destination. A conforming peer never sends that
     * shape, and whether one can reach the host over the air at all is not
     * measured (the MM6108 may discard it first), so this is defence in depth.
     * With no Mesh Control there is no TTL, so it cannot be relayed either,
     * whatever the forwarding gate says. */
    for (int fwd = 0; fwd <= 1; fwd++)
    {
        fresh(fwd != 0, /*bridge=*/false, /*grp_std=*/false);
        (void)simnode_add_peer(A);
        (void)simnode_add_peer(C);
        n = umac_mesh_ies_build_data_hdr4(frame, W, A, C, A);
        frame[n++] = 0x00; /* QoS Control: TID 0 ... */
        frame[n++] = 0x00; /* ... Mesh Control Present CLEAR */
        memcpy(&frame[n], SNAP_IPV4, sizeof(SNAP_IPV4));
        n = (uint16_t)(n + sizeof(SNAP_IPV4));
        memcpy(&frame[n], PAY, sizeof(PAY));
        n = (uint16_t)(n + sizeof(PAY));
        (void)simnode_rx(frame, n, -60);
        CHECK(simnode_host_rx_count() == 0,
              "forwarding %s: a 4-address frame for another node with NO Mesh Control is "
              "not delivered either (%u delivered)",
              fwd ? "on" : "off", simnode_host_rx_count());
        CHECK(simnode_outbox_count() == 0, "  and it is not relayed (%u sent)",
              simnode_outbox_count());
        CHECK(g_warthog_rxdrop_reason == 94u, "  dropped with reason 94 (got %lu)",
              (unsigned long)g_warthog_rxdrop_reason);
    }

    /* Only the destination is judged: addressed to us, no Mesh Control, it is
     * still delivered exactly as before. */
    fresh(/*fwd=*/false, /*bridge=*/false, /*grp_std=*/false);
    (void)simnode_add_peer(A);
    n = umac_mesh_ies_build_data_hdr4(frame, W, A, W, A);
    frame[n++] = 0x00;
    frame[n++] = 0x00;
    memcpy(&frame[n], SNAP_IPV4, sizeof(SNAP_IPV4));
    n = (uint16_t)(n + sizeof(SNAP_IPV4));
    memcpy(&frame[n], PAY, sizeof(PAY));
    n = (uint16_t)(n + sizeof(PAY));
    (void)simnode_rx(frame, n, -60);
    CHECK(simnode_host_rx_count() == 1,
          "addressed to us with no Mesh Control, it is still delivered (%u delivered)",
          simnode_host_rx_count());
}

/* ---- leaf mode: the service tick frees lapsed proxy entries ------------ */

static void t_leaf_proxy_sweep(void)
{
    printf("--- leaf mode: a lapsed proxy entry is freed by the tick, and stays gone past 2^31 ms ---\n");
    fresh(/*fwd=*/false, /*bridge=*/false, /*grp_std=*/false);
    (void)simnode_add_peer(A);
    uint8_t frame[256];
    struct umac_mesh_ctrl mc = { .flags = UMAC_MESH_CTRL_AE_A5A6, .ttl = 31, .seq = 6500 };
    memcpy(mc.eaddr1, W, 6);
    memcpy(mc.eaddr2, H2, 6);
    uint16_t n = mk_uni(frame, W, A, W, A, &mc, PAY, sizeof(PAY));
    (void)simnode_rx(frame, n, -60);
    uint8_t via[6];
    CHECK(umac_mesh_fwd_glue_proxy_via_peer(H2, via) && MAC_EQ(via, A), "H2 learned behind A");

    const uint32_t t0 = mmosal_get_time_ms();
    simnode_set_time_ms(t0 + UMAC_MESH_PROXY_LIFETIME_MS + UMAC_MESH_PATH_EXPIRE_MS + 10u);
    simnode_tick();
    simnode_pump();
    char paths[512];
    (void)simnode_render_paths(paths, sizeof(paths));
    CHECK(strstr(paths, "proxies=0") != NULL, "600 s past its expiry, the leaf's tick freed it");
    simnode_set_time_ms(t0 + UMAC_MESH_PROXY_LIFETIME_MS + 0x80000000u + 10u);
    CHECK(!umac_mesh_fwd_glue_proxy_via_peer(H2, via),
          "2^31 ms after its expiry, H2 does not come back behind A");
}

int main(void)
{
    printf("=== simnode datapath: the real TX and RX path, header construction "
           "and Mesh Control ===\n");

    t_tx_unicast_4addr();
    t_tx_group_3addr_standard();
    t_tx_group_replica_ae2();
    t_tx_proxied_source();
    t_tx_proxied_destination();
    t_rx_deliver();
    t_rx_forward_unicast();
    t_rx_forward_eapol();
    t_rx_forward_keyed();
    t_leaf_proxied_host();
    t_leaf_nonneighbour_host();
    t_leaf_proxy_sweep();
    t_rx_group_deliver_and_forward();
    t_rx_replica_normalise();
    t_rx_proxied_endpoints();
    t_rx_drops();

    /* Peer records are the only other thing on this heap, and del_peer frees
     * them (and drains their queues). With every peer gone, anything still
     * allocated is a packet buffer the datapath orphaned. */
    simnode_del_peer(NULL);
    CHECK(simnode_live_allocs() == 0,
          "no packet buffer was orphaned across the whole run (%u live)",
          simnode_live_allocs());
    simnode_stop();

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_simnode_datapath: all passed\n");
    return 0;
}
