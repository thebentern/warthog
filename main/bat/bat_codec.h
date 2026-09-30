/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WARTHOG_BAT_CODEC_H
#define WARTHOG_BAT_CODEC_H
/* BATMAN_V wire layout: packet types, header offsets (from the batman byte 0), BE helpers. */
#include <stddef.h>
#include <stdint.h>

/* packet types (byte 0) */
#define BAT_PT_BCAST       0x01
#define BAT_PT_CODED       0x02
#define BAT_PT_ELP         0x03
#define BAT_PT_OGM2        0x04
#define BAT_PT_MCAST       0x05
#define BAT_PT_UNICAST     0x40
#define BAT_PT_FRAG        0x41
#define BAT_PT_4ADDR       0x42
#define BAT_PT_ICMP        0x43
#define BAT_PT_UTVLV       0x44
#define BAT_PT_UNI_FIRST   0x40
#define BAT_PT_UNI_LAST    0x7F

/* common prefix */
#define BAT_OFF_TYPE       0
#define BAT_OFF_VERSION    1
#define BAT_OFF_TTL        2       /* every type except ELP */
#define BAT_OFF_UNI_DEST   4       /* destination originator of every 0x40..0x7F type */

/* link header (frame offsets) */
#define BAT_LINK_DST       0
#define BAT_LINK_SRC       6
#define BAT_LINK_TYPE      12

/* ELP 0x03 */
#define BAT_ELP_ORIG       2
#define BAT_ELP_SEQ        8
#define BAT_ELP_INTERVAL   12
#define BAT_ELP_HLEN       16      /* receivers need this much */
#define BAT_ELP_LEN        20      /* senders emit this much (4 zero bytes) */

/* OGM2 0x04 (one record) */
#define BAT_OGM_TTL        2
#define BAT_OGM_FLAGS      3
#define BAT_OGM_SEQ        4
#define BAT_OGM_ORIG       8
#define BAT_OGM_TVLV_LEN   14
#define BAT_OGM_TPUT       16
#define BAT_OGM_HLEN       20
#define BAT_OGM_TTL_INIT   50
#define BAT_OGM_MAX_OWN    512     /* own record and aggregate cap */

/* BCAST 0x01 */
#define BAT_BC_TTL         2
#define BAT_BC_RSVD        3
#define BAT_BC_SEQ         4
#define BAT_BC_ORIG        8
#define BAT_BC_HLEN        14
#define BAT_BC_TTL_INIT    49

/* UNICAST 0x40 / UNICAST_4ADDR 0x42 */
#define BAT_UC_TTL         2
#define BAT_UC_TTVN        3
#define BAT_UC_DEST        4
#define BAT_UC_HLEN        10
#define BAT_4A_SRC         10
#define BAT_4A_SUBTYPE     16
#define BAT_4A_RSVD        17
#define BAT_4A_HLEN        18
#define BAT_4A_ST_DATA     1
#define BAT_4A_ST_DAT_GET  2
#define BAT_4A_ST_DAT_PUT  3
#define BAT_4A_ST_DAT_REPLY 4
#define BAT_UC_TTL_INIT    50

/* UNICAST_FRAG 0x41 */
#define BAT_FR_TTL         2
#define BAT_FR_NUMPRIO     3       /* number << 4 | priority << 1 */
#define BAT_FR_DEST        4
#define BAT_FR_ORIG        10
#define BAT_FR_SEQ         16
#define BAT_FR_TOTAL       18
#define BAT_FR_HLEN        20
#define BAT_FR_MAX_FRAME   1280    /* fragment size cap, header included */
#define BAT_FR_MAX_N       16

/* ICMP 0x43 */
#define BAT_IC_TTL         2
#define BAT_IC_MSGTYPE     3
#define BAT_IC_DEST        4
#define BAT_IC_ORIG        10
#define BAT_IC_UID         16
#define BAT_IC_RRCOUNT     17
#define BAT_IC_SEQ         18
#define BAT_IC_HLEN        20
#define BAT_IC_RR_SLOTS    16
#define BAT_IC_RR_LEN      116     /* 20 + 16 * 6 */
#define BAT_IC_TP_HLEN     28
#define BAT_IC_ECHO_REPLY  0
#define BAT_IC_ECHO_REQ    8
#define BAT_IC_TTL_EXC     11
#define BAT_IC_TP          15

/* UNICAST_TVLV 0x44 */
#define BAT_UT_TTL         2
#define BAT_UT_RSVD        3
#define BAT_UT_DEST        4
#define BAT_UT_SRC         10
#define BAT_UT_TVLV_LEN    16
#define BAT_UT_ALIGN       18
#define BAT_UT_HLEN        20

/* MCAST 0x05 (never handled; length for completeness) */
#define BAT_MC_HLEN        6

/* TVLV containers: type, version, BE16 length, value */
#define BAT_TVLV_HLEN      4
#define BAT_TVLV_GW        0x01
#define BAT_TVLV_DAT       0x02
#define BAT_TVLV_NC        0x03
#define BAT_TVLV_TT        0x04
#define BAT_TVLV_ROAM      0x05
#define BAT_TVLV_MCAST     0x06
#define BAT_TVLV_MCAST_TRACKER 0x07
#define BAT_TVLV_GW_LEN    8

static inline uint16_t bat_get16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}
static inline uint32_t bat_get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void bat_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static inline void bat_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* One TVLV container; val points into the walked area. */
struct bat_tvlv {
    uint8_t type, version;
    uint16_t len;
    const uint8_t *val;
};
enum { BAT_TVLV_END = 0, BAT_TVLV_OK = 1, BAT_TVLV_OVERRUN = -1 };
/* Next container at *off: OK and *off advanced; END when fewer than 4 bytes remain;
 * OVERRUN when a length runs past the area (the walk must stop). */
int bat_tvlv_next(const uint8_t *area, size_t len, size_t *off, struct bat_tvlv *t);

/* Byte length of the fixed header for type @pt, 0 if the type has none we know. */
size_t bat_hdr_len(uint8_t pt);

#endif
