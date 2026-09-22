/*
 * Does the simulator actually run the real stack?
 *
 * This is the harness's own smoke test. It drives one node through the real
 * TX path and asserts on the exact bytes the firmware handed to the chip,
 * parsed back with the firmware's own parser -- not by hand, because a
 * hand-rolled parser in a test can agree with a hand-rolled builder while both
 * are wrong about the air.
 */
#include <stdio.h>
#include <string.h>

#include "simnode.h"
#include "umac_mesh_fwd.h"
#include "umac_mesh_ctrl.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static const uint8_t W[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 }; /* us */
static const uint8_t A[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0a }; /* peer A */
static const uint8_t B[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0b }; /* peer B */

int main(void)
{
    printf("=== simnode smoke: the real stack, with only the chip and the RTOS replaced ===\n");

    CHECK(simnode_start(W), "node starts: interface up, mesh configured, glue initialised");
    simnode_set_gates(/*fwd=*/true, /*bridge=*/false, /*grp_std=*/false, /*secure=*/false);

    CHECK(simnode_add_peer(A), "peer A added");
    CHECK(simnode_add_peer(B), "peer B added");

    /* The virtual clock is the test's, not the wall's. */
    uint32_t t0 = mmosal_get_time_ms();
    simnode_advance_ms(5000);
    CHECK(mmosal_get_time_ms() == t0 + 5000, "time is virtual: advanced 5 s without waiting");

    simnode_outbox_clear();
    static const uint8_t payload[32] = { 0xde, 0xad, 0xbe, 0xef };
    CHECK(simnode_host_tx(A, W, payload, sizeof(payload)),
          "an 802.3 frame from the host side is accepted by the real TX path");

    CHECK(simnode_outbox_count() == 1, "exactly one frame reached the chip (got %u)",
          simnode_outbox_count());

    const struct simnode_frame *f = simnode_outbox_get(0);
    CHECK(f != NULL && f->len > 24, "and it is a full 802.11 frame (%u bytes)", f ? f->len : 0);

    if (f != NULL)
    {
        /* Parsed by the SHIPPING parser, so this asserts the real layout. */
        struct umac_mesh_rx_frame pf;
        CHECK(umac_mesh_fwd_parse_frame(f->bytes, f->len, &pf) != 0,
              "the firmware's own parser accepts what the firmware built");
        CHECK(memcmp(pf.addr1, A, 6) == 0, "addr1 is the peer we addressed");
        CHECK(memcmp(pf.addr2, W, 6) == 0, "addr2 is us");
        CHECK(pf.mc.ttl > 0, "a Mesh Control field is present, ttl=%u", pf.mc.ttl);
        CHECK(!f->is_mgmt, "and it went out as a data frame");
    }

    /* Nothing on a mesh data path should have fallen through into the radio
     * stack the simulator does not model. */
    CHECK(simnode_stub_hits("umac_connection_get_state") == 0,
          "the mesh TX path never entered STA-mode connection code");

    /* The tick is what the 2 s probe task runs. */
    unsigned before = simnode_outbox_count();
    simnode_advance_ms(2000);
    simnode_tick();
    CHECK(simnode_outbox_count() >= before, "the service tick runs without faulting");

    char paths[512];
    int n = simnode_render_paths(paths, sizeof(paths));
    CHECK(n > 0 && strstr(paths, "+MESHPATH:") != NULL,
          "the real table dump renders (%d bytes)", n);

    simnode_stop();

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_simnode_smoke: all passed\n");
    return 0;
}
