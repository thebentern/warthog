/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WARTHOG_BAT_H
#define WARTHOG_BAT_H
/* Clean-room BATMAN_V (compat 15) member engine: libc only, all state in struct bat. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BAT_ALEN              6
#define BAT_ETH_HLEN          14
#define BAT_ETHERTYPE         0x4305
#define BAT_COMPAT            15
/* Measure on the radio before raising either: OpenMANET's MM6108 path lost every group frame
 * above 1524 batman bytes (Pi to Pi); an AE-2 copy adds 12 bytes more and is untested there. */
#define BAT_HARD_MTU_DEFAULT  1500   /* batman bytes after the link header, sent unfragmented */
#define BAT_SOFT_MTU_DEFAULT  1460   /* soft-interface IP MTU (OpenMANET bat0) */
#define BAT_MAX_LINK_FRAME    1600   /* largest link frame bat_rx_hard accepts, header included */
#define BAT_RENDER_BUF        4096   /* the port's render buffer: one bat_render_from chunk */
#define BAT_TPUT_UNKNOWN      0xFFFFFFFFu

enum bat_tx_result { BAT_TX_OK = 0, BAT_TX_FAIL = -1, BAT_TX_NOPEER = -2, BAT_TX_BUSY = -3 };

struct bat;

/* Sequence numbers kept where a reset that keeps power leaves them (the port: RTC no-init memory),
 * accessed 32 bits at a time; any content is safe, bat_init takes only an intact one for hard_addr. */
#define BAT_SEQ_KEEP_MAGIC    0xBA75E001u   /* layout version in the low byte */
#define BAT_SEQ_MARGIN        256u          /* resumed this far ahead (membership §7.4.3) */
struct bat_seq_keep {
    uint32_t magic;
    uint32_t addr_hi, addr_lo;  /* hard_addr bytes 0-3 and 4-5, big-endian */
    uint32_t elp, ogm, bcast;   /* the next number each counter sends */
    uint32_t crc;               /* CRC-32C of the words before it, each big-endian */
};

struct bat_config {
    uint8_t  hard_addr[BAT_ALEN]; /* mesh MAC = originator address */
    uint8_t  soft_addr[BAT_ALEN]; /* soft-interface MAC, announced in TT */
    uint16_t hard_mtu;            /* 100..1586; default BAT_HARD_MTU_DEFAULT */
    uint16_t elp_interval_ms;     /* default 500, minimum 100 */
    uint16_t ogm_interval_ms;     /* default 1000, minimum 40 */
    uint8_t  hop_penalty;         /* mesh hop_penalty, default 30 */
    uint8_t  bcast_copies;        /* 1..3, default 1; the port sets 3 with standard group frames.
                                   * With 2 or 3 a group frame goes to ops->tx at least 5 ms
                                   * after ops->tx accepted the one before */
    uint32_t tput_override;       /* 100 kbit/s units; 0 = ask ops->link_tput */
    bool     aggregate_ogm;       /* default true */
    bool     half_duplex;         /* default true (802.11s) */
    /* NULL (default), or a record that outlives the engine: bat_init resumes the ELP, OGM and BCAST
     * counters BAT_SEQ_MARGIN past an intact one, else draws them; the engine keeps it current. */
    struct bat_seq_keep *seq_keep;
};

struct bat_ops {
    /* One link frame: [dst 6][src 6][43 05][batman...]; dst ff:ff:ff:ff:ff:ff = broadcast.
     * Returns enum bat_tx_result. The engine never keeps @frame. */
    int      (*tx)(void *user, const uint8_t *frame, size_t len);
    /* One decapsulated client Ethernet frame for the soft interface; copy it. */
    void     (*deliver)(void *user, const uint8_t *frame, size_t len);
    uint32_t (*now_ms)(void *user);      /* monotonic ms, wraps at 2^32 */
    uint32_t (*rand32)(void *user);
    /* Our TX throughput toward a neighbour, 100 kbit/s units: 0 = no 802.11s station,
     * BAT_TPUT_UNKNOWN = station without an estimate, else the estimate (>= 1). */
    uint32_t (*link_tput)(void *user, const uint8_t hard_addr[BAT_ALEN]);
};

void     bat_config_defaults(struct bat_config *cfg); /* all fields; addresses zeroed */
size_t   bat_ctx_size(void);
/* Zeroes *b, validates cfg, draws random sequence numbers, arms timers. 0; 1 when the sequence
 * numbers were resumed from cfg->seq_keep; -1 for a refused cfg (cfg->seq_keep untouched). */
int      bat_init(struct bat *b, const struct bat_config *cfg, const struct bat_ops *ops,
                  void *user);
/* One received link frame, 14-byte Ethernet header included. The engine may rewrite it
 * in place and never keeps the pointer. */
void     bat_rx_hard(struct bat *b, uint8_t *frame, size_t len);
/* One Ethernet frame from the soft interface. 0 = sent or queued, -1 = dropped (counted). */
int      bat_tx_soft(struct bat *b, const uint8_t *frame, size_t len);
/* Runs every due timer; returns ms until it wants to run again (1..1000). */
uint32_t bat_tick(struct bat *b);
unsigned bat_route_count(const struct bat *b);  /* originators with a default-table router */
unsigned bat_neigh_count(const struct bat *b);

/* Where an untagged client MAC goes: the routed originator unicast to it would use. */
struct bat_client_route {
    uint8_t  orig[BAT_ALEN];
    uint32_t tput;          /* its default-table route, 100 kbit/s units */
    uint32_t ogm_age_ms;    /* since its last accepted OGM */
};
/* true if TT resolves @mac to an originator with a route; false zeroes *out. @out may be NULL. */
bool     bat_client_route(struct bat *b, const uint8_t mac[BAT_ALEN], struct bat_client_route *out);
/* A gateway: a routed originator whose last OGM carried a GW TVLV with both bandwidths set. */
struct bat_gw {
    uint8_t  orig[BAT_ALEN];
    uint32_t down, up;      /* announced, 100 kbit/s units */
    uint32_t tput;          /* route throughput to it */
    uint32_t ogm_age_ms;
};
/* Returns how many gateways there are; *out = the one with the highest min(route tput, down), the
 * first on a tie (zeroed when none). @out may be NULL. */
unsigned bat_gw_best(struct bat *b, struct bat_gw *out);

enum bat_render_kind { BAT_RENDER_NEIGH, BAT_RENDER_ORIG, BAT_RENDER_TT_GLOBAL,
                       BAT_RENDER_TT_LOCAL, BAT_RENDER_STAT };
/* "\r\n"-terminated "+BATx: ..." lines (5.3). Returns bytes written (NUL excluded).
 * A listing longer than len - 32 bytes stops at a whole entry and ends "+BATx: (truncated)". */
size_t   bat_render(struct bat *b, enum bat_render_kind kind, char *buf, size_t len);
#define BAT_RENDER_DONE       0xFFFFFFFFu
/* The same listing in chunks (same kind and @mac each call): *cursor 0 starts it (the summary
 * line comes only then); on return it is where the next chunk starts, or BAT_RENDER_DONE. A
 * chunk holds whole entries (ORIG: an originator with its candidates; else one line) below
 * len - 32 bytes and no marker, so the chunks joined are the listing; an entry added or moved
 * between chunks can be missed or shown twice. An entry longer than len - 33 bytes ends the
 * listing with "+BATx: (truncated)". @mac (NULL = all): ORIG keeps that originator; TT_GLOBAL
 * the rows whose client or originator it is, after the CRC lines of each originator shown;
 * other kinds ignore it. */
size_t   bat_render_from(struct bat *b, enum bat_render_kind kind, const uint8_t *mac,
                         uint32_t *cursor, char *buf, size_t len);

/* Counters, X(ID, "name") in render-group order: rx elp ogm bc uc ut fr ic st lk. */
#define BAT_COUNTERS(X) \
    X(RX, "rx") X(RX_SHORT, "rx_short") X(RX_TOOBIG, "rx_toobig") \
    X(RX_VERSION, "rx_version") X(RX_TYPE, "rx_type") X(RX_HDR, "rx_hdr") \
    X(RX_MGMT_DST, "rx_mgmt_dst") X(RX_SRC_BAD, "rx_src_bad") X(RX_SRC_OWN, "rx_src_own") \
    X(RX_UNI_DST, "rx_uni_dst") \
    X(ELP_RX, "elp_rx") X(ELP_TX, "elp_tx") X(ELP_PROBE, "elp_probe") X(ELP_DUP, "elp_dup") \
    X(ELP_OWN_ORIG, "elp_own_orig") X(NEIGH_NEW, "neigh_new") X(NEIGH_FULL, "neigh_full") \
    X(NEIGH_PURGED, "neigh_purged") \
    X(OGM_RX, "ogm_rx") X(OGM_REC, "ogm_rec") X(OGM_BADREC, "ogm_badrec") \
    X(OGM_OVERRUN, "ogm_overrun") X(OGM_OWN, "ogm_own") X(OGM_TPUT0, "ogm_tput0") \
    X(OGM_NOT_NEIGH, "ogm_not_neigh") X(OGM_OLD, "ogm_old") X(OGM_RESTART, "ogm_restart") \
    X(OGM_RESTART_BLOCKED, "ogm_restart_blocked") X(OGM_NEW, "ogm_new") X(OGM_SAME, "ogm_same") \
    X(OGM_TX_OWN, "ogm_tx_own") X(OGM_SUPP_OWN, "ogm_supp_own") X(OGM_TX_FWD, "ogm_tx_fwd") \
    X(OGM_FWD_TTL, "ogm_fwd_ttl") X(OGM_FWD_TPUT0, "ogm_fwd_tput0") \
    X(OGM_TX_FRAMES, "ogm_tx_frames") X(OGM_TOOBIG, "ogm_toobig") X(ORIG_NEW, "orig_new") \
    X(ORIG_FULL, "orig_full") X(ORIG_PURGED, "orig_purged") X(CAND_FULL, "cand_full") \
    X(ROUTE_LOST, "route_lost") \
    X(BC_RX, "bc_rx") X(BC_TTL, "bc_ttl") X(BC_OWN, "bc_own") X(BC_UNKNOWN, "bc_unknown") \
    X(BC_DUP, "bc_dup") X(BC_RESTART_BLOCKED, "bc_restart_blocked") X(BC_DELIVER, "bc_deliver") \
    X(BC_FWD, "bc_fwd") X(BC_SUPP, "bc_supp") X(BC_TOOBIG, "bc_toobig") X(BC_TX_OWN, "bc_tx_own") \
    X(BC_COPY_DROP, "bc_copy_drop") X(BC_QUEUE_FULL, "bc_queue_full") \
    X(UC_RX, "uc_rx") X(UC_DELIVER, "uc_deliver") X(UC_FWD, "uc_fwd") X(UC_TTL, "uc_ttl") \
    X(UC_NOROUTE, "uc_noroute") X(UC_STALE_DROP, "uc_stale_drop") X(UC_REROUTE, "uc_reroute") \
    X(UC_INNER_BAD, "uc_inner_bad") X(UC_4A_DAT, "uc_4a_dat") X(UNK_FWD, "unk_fwd") \
    X(UNK_SELF, "unk_self") \
    X(UT_RX, "ut_rx") X(UT_LEN, "ut_len") X(UT_FWD, "ut_fwd") X(UT_CONSUMED, "ut_consumed") \
    X(TT_REQ_RX, "tt_req_rx") X(TT_REQ_PACED, "tt_req_paced") X(TT_RESP_TX, "tt_resp_tx") \
    X(TT_REQ_TX, "tt_req_tx") X(TT_REQ_NOROUTE, "tt_req_noroute") X(TT_REQ_STALL, "tt_req_stall") \
    X(TT_RESP_RX, "tt_resp_rx") \
    X(TT_RESP_UNKNOWN, "tt_resp_unknown") X(TT_DIFF, "tt_diff") X(TT_FULL, "tt_full") \
    X(TT_CRC_FAIL, "tt_crc_fail") X(TT_ROWS_FULL, "tt_rows_full") X(TT_TEMP_NEW, "tt_temp_new") \
    X(TT_ROAM_RX, "tt_roam_rx") X(TT_BAD, "tt_bad") \
    X(FR_RX, "fr_rx") X(FR_FWD, "fr_fwd") X(FR_TTL, "fr_ttl") X(FR_UNKNOWN, "fr_unknown") \
    X(FR_NOROUTE, "fr_noroute") X(FR_DUP, "fr_dup") X(FR_BAD, "fr_bad") X(FR_TOOBIG, "fr_toobig") \
    X(FR_EVICT, "fr_evict") X(FR_FULL, "fr_full") X(FR_TIMEOUT, "fr_timeout") X(FR_DONE, "fr_done") \
    X(FR_NESTED, "fr_nested") X(FR_TX, "fr_tx") X(FR_TX_TOOMANY, "fr_tx_toomany") \
    X(IC_RX, "ic_rx") X(IC_REPLY, "ic_reply") X(IC_TTLX, "ic_ttlx") X(IC_FWD, "ic_fwd") \
    X(IC_RR_FULL, "ic_rr_full") X(IC_TP, "ic_tp") X(IC_UNKNOWN_SRC, "ic_unknown_src") \
    X(IC_OTHER, "ic_other") \
    X(ST_TX, "st_tx") X(ST_SHORT, "st_short") X(ST_LOOP, "st_loop") X(ST_SRC, "st_src") \
    X(ST_VLAN, "st_vlan") X(ST_CTRL, "st_ctrl") X(ST_TOOBIG, "st_toobig") \
    X(ST_NOROUTE, "st_noroute") X(ST_UNICAST, "st_unicast") X(ST_BCAST, "st_bcast") \
    X(LK_TX, "lk_tx") X(LK_NOPEER, "lk_nopeer") X(LK_BUSY, "lk_busy") X(LK_FAIL, "lk_fail") \
    X(DELIVER, "deliver") X(DELIVER_BAD, "deliver_bad")

enum bat_counter {
#define BAT_X_ENUM(id, name) BAT_C_##id,
    BAT_COUNTERS(BAT_X_ENUM)
#undef BAT_X_ENUM
    BAT_C__COUNT
};
uint32_t    bat_counter(const struct bat *b, enum bat_counter c);
const char *bat_counter_name(enum bat_counter c);
#endif
