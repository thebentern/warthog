/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Which netif the Meshtastic repeater (main/mudp.c) attributes a multicast
 * datagram to. Getting it wrong either drops a Meshtastic packet ("unknown")
 * or sends it back where it came from.
 *
 * Each socket is bound to one netif, so the arrival netif is known; the
 * classifier must go by it and never by the source address. Three cases that
 * source-subnet classification got wrong:
 *  - a mesh with mixed addressing (a DHCP lease in 10.41/16 next to a static
 *    10.77/16 fallback): matching only our own mesh subnet dropped the other;
 *  - a local client with an off-subnet address (a static 10.0.0.50 on the
 *    softAP, a 169.254/16 USB host): calling "anything else" the mesh echoed
 *    it back onto its own link and never sent it to the mesh;
 *  - a mesh sender numbered inside our USB or AP /24: matching those subnets
 *    first called it local and re-injected it into the mesh.
 */
#include "mudp_classify.h"

#include <stdio.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

#define IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

static const char *name(int r)
{
    switch (r) {
    case MUDP_NIF_USB: return "USB";
    case MUDP_NIF_AP: return "AP";
    case MUDP_NIF_HALOW: return "HaLow";
    case MUDP_FROM_SELF: return "self";
    default: return "unknown";
    }
}

int main(void)
{
    printf("=== mudp_classify: which netif a Meshtastic multicast is attributed to ===\n");

    /* This warthog: USB and AP on their fixed /24s, HaLow on a DHCP lease from
     * an OpenMANET node's 10.41/16. */
    struct mudp_nif dhcp[MUDP_NIF_COUNT] = {
        [MUDP_NIF_USB]   = { IP(192, 168, 4, 1), IP(255, 255, 255, 0) },
        [MUDP_NIF_AP]    = { IP(192, 168, 5, 1), IP(255, 255, 255, 0) },
        [MUDP_NIF_HALOW] = { IP(10, 41, 12, 34), IP(255, 255, 0, 0) },
    };
    int r;

    printf("--- the local links ---\n");
    r = mudp_classify(IP(192, 168, 4, 2), MUDP_NIF_USB, dhcp);
    CHECK(r == MUDP_NIF_USB, "a host on USB is USB (%s)", name(r));
    r = mudp_classify(IP(192, 168, 5, 7), MUDP_NIF_AP, dhcp);
    CHECK(r == MUDP_NIF_AP, "a Meshtastic node on the softAP is AP (%s)", name(r));
    r = mudp_classify(IP(10, 0, 0, 50), MUDP_NIF_AP, dhcp);
    CHECK(r == MUDP_NIF_AP, "a softAP client with a static off-subnet address is still AP (%s)",
          name(r));
    r = mudp_classify(IP(169, 254, 3, 4), MUDP_NIF_USB, dhcp);
    CHECK(r == MUDP_NIF_USB, "a link-local USB host is still USB (%s)", name(r));

    printf("--- our own repeats never come back in ---\n");
    for (int i = 0; i < MUDP_NIF_COUNT; i++) {
        for (int on = 0; on < MUDP_NIF_COUNT; on++) {
            r = mudp_classify(dhcp[i].ip, on, dhcp);
            CHECK(r == MUDP_FROM_SELF, "our own %s address arriving on %s is self (%s)", name(i),
                  name(on), name(r));
        }
    }

    printf("--- the mesh side, whatever addressing it uses ---\n");
    r = mudp_classify(IP(10, 41, 254, 1), MUDP_NIF_HALOW, dhcp);
    CHECK(r == MUDP_NIF_HALOW, "an OpenMANET node on our own mesh subnet is HaLow (%s)", name(r));
    r = mudp_classify(IP(10, 77, 199, 248), MUDP_NIF_HALOW, dhcp);
    CHECK(r == MUDP_NIF_HALOW,
          "a warthog on its static 10.77/16 fallback is HaLow too, not unknown (%s)", name(r));
    r = mudp_classify(IP(192, 168, 4, 50), MUDP_NIF_HALOW, dhcp);
    CHECK(r == MUDP_NIF_HALOW,
          "a mesh sender numbered inside our USB /24 is HaLow, not USB (%s)", name(r));

    /* The other way round: this warthog on its static fallback, the sender on
     * a DHCP lease. */
    struct mudp_nif fallback[MUDP_NIF_COUNT] = {
        [MUDP_NIF_USB]   = { IP(192, 168, 4, 1), IP(255, 255, 255, 0) },
        [MUDP_NIF_AP]    = { IP(192, 168, 5, 1), IP(255, 255, 255, 0) },
        [MUDP_NIF_HALOW] = { IP(10, 77, 13, 182), IP(255, 255, 0, 0) },
    };
    r = mudp_classify(IP(10, 41, 12, 34), MUDP_NIF_HALOW, fallback);
    CHECK(r == MUDP_NIF_HALOW, "and a DHCP-leased warthog reaching a fallback one is HaLow (%s)",
          name(r));

    printf("--- a netif that is not joined attributes nothing ---\n");
    struct mudp_nif no_mesh[MUDP_NIF_COUNT] = {
        [MUDP_NIF_USB]   = { IP(192, 168, 4, 1), IP(255, 255, 255, 0) },
        [MUDP_NIF_AP]    = { IP(192, 168, 5, 1), IP(255, 255, 255, 0) },
        [MUDP_NIF_HALOW] = { 0, 0 },
    };
    r = mudp_classify(IP(10, 77, 1, 2), MUDP_NIF_HALOW, no_mesh);
    CHECK(r == MUDP_FROM_UNKNOWN, "HaLow not joined yet: unknown (%s)", name(r));
    r = mudp_classify(IP(192, 168, 4, 9), MUDP_NIF_USB, no_mesh);
    CHECK(r == MUDP_NIF_USB, "while USB still classifies (%s)", name(r));
    r = mudp_classify(IP(192, 168, 4, 9), -1, dhcp);
    CHECK(r == MUDP_FROM_UNKNOWN, "an out-of-range slot is unknown (%s)", name(r));
    r = mudp_classify(IP(192, 168, 4, 9), MUDP_NIF_COUNT, dhcp);
    CHECK(r == MUDP_FROM_UNKNOWN, "and so is one past the end (%s)", name(r));

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_mudp_classify: all passed\n");
    return 0;
}
