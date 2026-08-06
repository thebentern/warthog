/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Functional test for the dot11ah IE codec (ie.c) — the core parse/find/insert
 * machinery the S1G<->11n transforms are built on. Runs on the host with no
 * hardware: builds a real 802.11 management-frame IE buffer, parses it into a
 * dot11ah_ies_mask, and verifies the codec located every element correctly,
 * detected the mesh network, and can round-trip an element back out.
 */
#include <linux/types.h>
#include <linux/ieee80211.h>
#include <net/mac80211.h>
#include "dot11ah/dot11ah.h"
#include "mesh.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures;
#define CHECK(cond, msg, ...) do { \
    if (cond) { printf("ok   " msg "\n", ##__VA_ARGS__); } \
    else      { printf("FAIL " msg "\n", ##__VA_ARGS__); failures++; } \
} while (0)

/* Append one IE (eid, len, body) to buf at *pos, advancing *pos. */
static void put_ie(u8 *buf, size_t *pos, u8 eid, u8 len, const u8 *body)
{
    buf[(*pos)++] = eid;
    buf[(*pos)++] = len;
    memcpy(buf + *pos, body, len);
    *pos += len;
}

int main(void)
{
    u8 frame[256];
    size_t flen = 0;

    /* Build a mesh-beacon-like IE sequence:
     *   SSID (0)        = "warthog"
     *   Supported Rates (1)
     *   DS Params (3)   = channel 6
     *   RSN (48)        = a 4-byte stub
     *   Mesh ID (114)   = "mesh0"
     *   Mesh Config (113)
     */
    const u8 ssid[]   = { 'w','a','r','t','h','o','g' };
    const u8 rates[]  = { 0x82, 0x84, 0x8b, 0x96 };
    const u8 dsparm[] = { 6 };
    const u8 rsn[]    = { 0x01, 0x00, 0x00, 0x0f };
    const u8 meshid[] = { 'm','e','s','h','0' };
    const u8 meshcfg[]= { 0x00,0x07,0x10,0x01,0x01,0x00,0x01 };

    put_ie(frame, &flen, WLAN_EID_SSID,            sizeof(ssid),   ssid);
    put_ie(frame, &flen, WLAN_EID_SUPP_RATES,      sizeof(rates),  rates);
    put_ie(frame, &flen, WLAN_EID_DS_PARAMS,       sizeof(dsparm), dsparm);
    put_ie(frame, &flen, WLAN_EID_RSN,             sizeof(rsn),    rsn);
    put_ie(frame, &flen, WLAN_EID_MESH_ID,         sizeof(meshid), meshid);
    put_ie(frame, &flen, WLAN_EID_MESH_CONFIG,     sizeof(meshcfg),meshcfg);

    /* --- parse --- */
    struct dot11ah_ies_mask *mask = morse_dot11ah_ies_mask_alloc();
    CHECK(mask != NULL, "ies_mask_alloc returns non-NULL");
    if (!mask)
        return 1;

    int ret = morse_dot11ah_parse_ies(frame, flen, mask);
    CHECK(ret == 0, "parse_ies returns 0 (ret=%d)", ret);

    /* --- each element located, with the right length --- */
    CHECK(mask->ies[WLAN_EID_SSID].ptr && mask->ies[WLAN_EID_SSID].len == sizeof(ssid),
          "SSID parsed (len=%u)", mask->ies[WLAN_EID_SSID].len);
    CHECK(mask->ies[WLAN_EID_SSID].ptr &&
          memcmp(mask->ies[WLAN_EID_SSID].ptr, ssid, sizeof(ssid)) == 0,
          "SSID body == \"warthog\"");
    CHECK(mask->ies[WLAN_EID_SUPP_RATES].ptr &&
          mask->ies[WLAN_EID_SUPP_RATES].len == sizeof(rates),
          "Supported Rates parsed (len=%u)", mask->ies[WLAN_EID_SUPP_RATES].len);
    CHECK(mask->ies[WLAN_EID_DS_PARAMS].ptr &&
          mask->ies[WLAN_EID_DS_PARAMS].ptr[0] == 6,
          "DS Params channel == 6");
    CHECK(mask->ies[WLAN_EID_RSN].ptr && mask->ies[WLAN_EID_RSN].len == sizeof(rsn),
          "RSN parsed (len=%u)", mask->ies[WLAN_EID_RSN].len);
    CHECK(mask->ies[WLAN_EID_MESH_ID].ptr &&
          mask->ies[WLAN_EID_MESH_ID].len == sizeof(meshid) &&
          memcmp(mask->ies[WLAN_EID_MESH_ID].ptr, meshid, sizeof(meshid)) == 0,
          "Mesh ID parsed == \"mesh0\"");
    CHECK(mask->ies[WLAN_EID_MESH_CONFIG].ptr &&
          mask->ies[WLAN_EID_MESH_CONFIG].len == sizeof(meshcfg),
          "Mesh Config parsed (len=%u)", mask->ies[WLAN_EID_MESH_CONFIG].len);

    /* --- the mesh-network predicate fires on a Mesh ID IE --- */
    CHECK(morse_is_mesh_network(mask) == true,
          "morse_is_mesh_network() detects the mesh");

    /* --- a non-present element reads back empty --- */
    CHECK(mask->ies[WLAN_EID_HT_CAPABILITY].ptr == NULL,
          "absent IE (HT Cap) has NULL ptr");

    /* --- raw finder agrees with the parsed view --- */
    const u8 *found = morse_dot11_find_ie(WLAN_EID_MESH_ID, frame, (int)flen);
    CHECK(found != NULL && found[0] == WLAN_EID_MESH_ID && found[1] == sizeof(meshid),
          "morse_dot11_find_ie locates Mesh ID");
    const u8 *absent = morse_dot11_find_ie(WLAN_EID_HT_CAPABILITY, frame, (int)flen);
    CHECK(absent == NULL, "morse_dot11_find_ie returns NULL for absent IE");

    /* --- insert round-trips an element back into a buffer --- */
    u8 out[64];
    u8 *end = morse_dot11_insert_ie(out, meshid, WLAN_EID_MESH_ID, sizeof(meshid));
    CHECK(end == out + 2 + sizeof(meshid), "insert_ie advances by 2+len");
    CHECK(out[0] == WLAN_EID_MESH_ID && out[1] == sizeof(meshid) &&
          memcmp(out + 2, meshid, sizeof(meshid)) == 0,
          "insert_ie writes correct eid/len/body");

    morse_dot11ah_ies_mask_free(mask);

    printf("\n%s\n", failures ? "SOME CHECKS FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}
