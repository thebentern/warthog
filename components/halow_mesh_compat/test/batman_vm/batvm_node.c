/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * batvm_node -- the warthog BATMAN_V engine (every main/bat source, unmodified) as a Linux
 * host program, for the VM interop suite in this directory (see run_all.sh).
 *
 * Hard side: an AF_PACKET socket for ethertype 0x4305 on one interface (a veth
 * toward batman-adv peers, or an 802.11s mesh point). Frames the host itself
 * sent (PACKET_OUTGOING) are ignored. Engine link frames leave through that
 * socket, or -- with --inject -- as hand-built 802.11s data frames on a monitor
 * interface, in the shapes warthog's radio sends (design 4.3):
 *   ae2      group frame -> one 4-address copy per peer, Mesh Control AE mode 2
 *            (addr5 = ff:ff:ff:ff:ff:ff, addr6 = own)          AT+MESHGRP=0
 *   group    group frame -> one 3-address group frame (FromDS)  AT+MESHGRP=1
 *   rewrite  group frame -> one 4-address copy per peer, no AE, DA = the peer
 *            (warthog's pre-batman default; batman-adv must reject it)
 *   unicast  always one 4-address frame to the peer, no AE.
 * Soft side: a TAP (IFF_TAP|IFF_NO_PI) whose MAC and MTU are the soft MAC / MTU.
 * Frames read from it go to bat_tx_soft(); frames the engine delivers are written
 * to it.
 *
 * Every second the status file (--status) is rewritten atomically with all five
 * engine renders (NEIGH, ORIG, TT_GLOBAL, TT_LOCAL, STAT; "\r" stripped; each paged
 * with bat_render_from through a buffer of the firmware port's size, BAT_RENDER_BUF), one
 * "+NODE:" line of harness counters and "+QRY:" lines: the best gateway, and with
 * --watch-file the client MAC on that file's first line and what TT resolves it to. SIGTERM/SIGINT exit cleanly after a last
 * status write. --seq-file maps a file as the engine's struct bat_seq_keep (MAP_SHARED, so a SIGKILL
 * keeps what the engine last wrote): the firmware port's RTC no-init record across a reset. SIGUSR1 / SIGUSR2 add / delete --extra-client in the local TT
 * (engine test hooks), to drive TT diffs.
 *
 * The protocol lives only in main/bat (clean room); this file is plumbing and
 * includes no batman-adv header.
 */
#define _GNU_SOURCE
#include "bat_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_packet.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_PEERS 8
enum shape { SH_AE2, SH_GROUP, SH_REWRITE };

struct node {
    int hard_fd, hard_ifidx, inj_fd, inj_ifidx, tap_fd;
    char hard_if[IFNAMSIZ];
    enum shape shape;
    uint8_t own[6], peers[MAX_PEERS][6];
    unsigned npeers;
    uint16_t seq80211;
    uint32_t meshseq;
    uint32_t rng;
    bool seeded;
    uint32_t tput;
    bool tput_auto;
    uint32_t clock_base;
    bool clock_set;
    struct timespec t0;
    uint8_t extra[6];
    bool have_extra;
    const char *watch_file;
    bool seq_carried;
    struct bat *b;
    /* harness counters */
    unsigned long rx_frames, rx_outgoing, rx_trunc, tx_frames, tx_err, tx_nopeer, air_frames;
    unsigned long tap_rx, tap_rx_drop, tap_tx, tap_tx_err;
};

static volatile sig_atomic_t g_stop, g_add, g_del;

static void on_sig(int s)
{
    if (s == SIGTERM || s == SIGINT) {
        g_stop = 1;
    } else if (s == SIGUSR1) {
        g_add = 1;
    } else if (s == SIGUSR2) {
        g_del = 1;
    }
}

static int parse_mac(const char *s, uint8_t m[6])
{
    unsigned v[6];
    char tail;
    if (sscanf(s, "%x:%x:%x:%x:%x:%x%c", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &tail) != 6) {
        return -1;
    }
    for (int i = 0; i < 6; i++) {
        if (v[i] > 255) {
            return -1;
        }
        m[i] = (uint8_t)v[i];
    }
    return 0;
}

static bool is_bcast(const uint8_t *m)
{
    static const uint8_t ff[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    return memcmp(m, ff, 6) == 0;
}

/* ---- engine ops ---------------------------------------------------------- */

static int send_hard(struct node *n, const uint8_t *f, size_t l)
{
    struct sockaddr_ll a = { .sll_family = AF_PACKET, .sll_ifindex = n->hard_ifidx, .sll_halen = 6 };
    memcpy(a.sll_addr, f, 6);
    if (sendto(n->hard_fd, f, l, 0, (struct sockaddr *)&a, sizeof(a)) != (ssize_t)l) {
        n->tx_err++;
        return errno == ENOBUFS || errno == EAGAIN ? BAT_TX_BUSY : BAT_TX_FAIL;
    }
    return BAT_TX_OK;
}

/* One 802.11s QoS data frame on the monitor interface; body = batman bytes (f + 14). */
static int send_air(struct node *n, int fromds_only, const uint8_t *a1, const uint8_t *a3,
                    int ae2, const uint8_t *f, size_t l)
{
    uint8_t p[2048];
    size_t o = 0;
    static const uint8_t rt[8] = { 0, 0, 8, 0, 0, 0, 0, 0 };   /* radiotap, no fields */
    static const uint8_t llc[8] = { 0xaa, 0xaa, 0x03, 0, 0, 0, 0x43, 0x05 };
    if (l < 14 || l - 14 + 80 > sizeof(p)) {
        return BAT_TX_FAIL;
    }
    memcpy(p + o, rt, 8); o += 8;
    p[o++] = 0x88;                                  /* QoS data */
    p[o++] = fromds_only ? 0x02 : 0x03;             /* FromDS / ToDS|FromDS */
    p[o++] = 0; p[o++] = 0;                         /* duration */
    memcpy(p + o, a1, 6); o += 6;
    memcpy(p + o, n->own, 6); o += 6;               /* addr2 = TA */
    memcpy(p + o, a3, 6); o += 6;
    n->seq80211 = (uint16_t)(n->seq80211 + 1);
    p[o++] = (uint8_t)((n->seq80211 << 4) & 0xf0);
    p[o++] = (uint8_t)(n->seq80211 >> 4);
    if (!fromds_only) {
        memcpy(p + o, n->own, 6); o += 6;           /* addr4 = mesh SA */
    }
    p[o++] = 0x00; p[o++] = 0x01;                   /* QoS: TID 0, Mesh Control present */
    p[o++] = ae2 ? 0x02 : 0x00;                     /* Mesh Control flags: AE mode */
    p[o++] = 31;                                    /* mesh TTL */
    n->meshseq++;
    p[o++] = (uint8_t)n->meshseq; p[o++] = (uint8_t)(n->meshseq >> 8);
    p[o++] = (uint8_t)(n->meshseq >> 16); p[o++] = (uint8_t)(n->meshseq >> 24);
    if (ae2) {
        memset(p + o, 0xff, 6); o += 6;             /* addr5 = mesh DA = broadcast */
        memcpy(p + o, n->own, 6); o += 6;           /* addr6 = mesh SA = own */
    }
    memcpy(p + o, llc, 8); o += 8;
    memcpy(p + o, f + 14, l - 14); o += l - 14;
    if (send(n->inj_fd, p, o, 0) != (ssize_t)o) {
        n->tx_err++;
        return BAT_TX_FAIL;
    }
    n->air_frames++;
    return BAT_TX_OK;
}

static int find_peer(const struct node *n, const uint8_t *m)
{
    for (unsigned i = 0; i < n->npeers; i++) {
        if (memcmp(n->peers[i], m, 6) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int op_tx(void *u, const uint8_t *f, size_t l)
{
    struct node *n = u;
    n->tx_frames++;
    if (n->inj_fd < 0) {
        return send_hard(n, f, l);
    }
    if (!is_bcast(f)) {
        if (find_peer(n, f) < 0) {
            n->tx_nopeer++;
            return BAT_TX_NOPEER;
        }
        return send_air(n, 0, f, f, 0, f, l);
    }
    if (n->shape == SH_GROUP) {
        return send_air(n, 1, f, n->own, 0, f, l);
    }
    if (n->npeers == 0) {
        n->tx_nopeer++;
        return BAT_TX_NOPEER;
    }
    int rc = BAT_TX_OK;
    for (unsigned i = 0; i < n->npeers; i++) {
        int r = send_air(n, 0, n->peers[i], n->peers[i], n->shape == SH_AE2, f, l);
        if (r != BAT_TX_OK) {
            rc = r;
        }
    }
    return rc;
}

static void op_deliver(void *u, const uint8_t *f, size_t l)
{
    struct node *n = u;
    if (write(n->tap_fd, f, l) == (ssize_t)l) {
        n->tap_tx++;
    } else {
        n->tap_tx_err++;
    }
}

static uint32_t op_now(void *u)
{
    struct node *n = u;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (!n->clock_set) {
        return (uint32_t)((uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u);
    }
    int64_t ms = (int64_t)(t.tv_sec - n->t0.tv_sec) * 1000 + (t.tv_nsec - n->t0.tv_nsec) / 1000000;
    return n->clock_base + (uint32_t)ms;
}

static uint32_t op_rand(void *u)
{
    struct node *n = u;
    uint32_t v;
    if (!n->seeded) {
        if (getrandom(&v, sizeof(v), 0) == sizeof(v)) {
            return v;
        }
        n->rng ^= (uint32_t)time(NULL) | 1u;
    }
    v = n->rng;
    v ^= v << 13;
    v ^= v >> 17;
    v ^= v << 5;
    n->rng = v;
    return v;
}

static uint32_t op_link_tput(void *u, const uint8_t hard[BAT_ALEN])
{
    struct node *n = u;
    (void)hard;
    if (n->tput_auto) {
        char path[128];
        long speed = 0;
        snprintf(path, sizeof(path), "/sys/class/net/%s/speed", n->hard_if);
        FILE *f = fopen(path, "r");
        if (f) {
            if (fscanf(f, "%ld", &speed) != 1) {
                speed = 0;
            }
            fclose(f);
        }
        return speed > 0 ? (uint32_t)speed * 10u : BAT_TPUT_UNKNOWN;
    }
    return n->tput;
}

/* ---- setup ---------------------------------------------------------------- */

static int open_packet(const char *ifname, uint16_t proto, int *ifidx)
{
    *ifidx = (int)if_nametoindex(ifname);
    if (*ifidx == 0) {
        fprintf(stderr, "batvm_node: no interface %s\n", ifname);
        return -1;
    }
    int fd = socket(AF_PACKET, SOCK_RAW, htons(proto));
    struct sockaddr_ll a = { .sll_family = AF_PACKET, .sll_protocol = htons(proto), .sll_ifindex = *ifidx };
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        perror("batvm_node: packet socket");
        return -1;
    }
    int big = 1 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
    return fd;
}

static int if_mac(const char *ifname, uint8_t m[6])
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq q;
    memset(&q, 0, sizeof(q));
    snprintf(q.ifr_name, IFNAMSIZ, "%s", ifname);
    int rc = s >= 0 ? ioctl(s, SIOCGIFHWADDR, &q) : -1;
    if (s >= 0) {
        close(s);
    }
    if (rc < 0) {
        return -1;
    }
    memcpy(m, q.ifr_hwaddr.sa_data, 6);
    return 0;
}

static int open_tap(const char *name, const uint8_t mac[6], int mtu)
{
    int fd = open("/dev/net/tun", O_RDWR);
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
    if (fd < 0 || ioctl(fd, TUNSETIFF, &ifr) < 0) {
        perror("batvm_node: tap");
        return -1;
    }
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq q;
    memset(&q, 0, sizeof(q));
    snprintf(q.ifr_name, IFNAMSIZ, "%s", name);
    q.ifr_hwaddr.sa_family = ARPHRD_ETHER;
    memcpy(q.ifr_hwaddr.sa_data, mac, 6);
    int bad = ioctl(s, SIOCSIFHWADDR, &q) < 0;
    q.ifr_mtu = mtu;
    bad |= ioctl(s, SIOCSIFMTU, &q) < 0;
    close(s);
    if (bad) {
        perror("batvm_node: tap mac/mtu");
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

static void write_status(struct node *n, const char *path)
{
    static char r[BAT_RENDER_BUF];   /* the firmware's chunk: every listing is paged through it */
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *o = fopen(tmp, "w");
    if (!o) {
        return;
    }
    const enum bat_render_kind k[5] = { BAT_RENDER_NEIGH, BAT_RENDER_ORIG, BAT_RENDER_TT_GLOBAL,
                                        BAT_RENDER_TT_LOCAL, BAT_RENDER_STAT };
    for (int i = 0; i < 5; i++) {
        uint32_t cur = 0;
        while (cur != BAT_RENDER_DONE) {
            size_t len = bat_render_from(n->b, k[i], NULL, &cur, r, sizeof(r));
            for (size_t j = 0; j < len; j++) {
                if (r[j] != '\r') {
                    fputc(r[j], o);
                }
            }
        }
    }
    struct bat_gw g;
    const unsigned gws = bat_gw_best(n->b, &g);
    fprintf(o, "+QRY: gws=%u\n", gws);
    if (gws) {
        fprintf(o, "+QRY: gw=%02x:%02x:%02x:%02x:%02x:%02x down=%lu up=%lu tput=%lu age=%lu\n", g.orig[0], g.orig[1],
                g.orig[2], g.orig[3], g.orig[4], g.orig[5], (unsigned long)g.down, (unsigned long)g.up,
                (unsigned long)g.tput, (unsigned long)g.ogm_age_ms);
    } else {
        fprintf(o, "+QRY: gw=none\n");
    }
    FILE *wf = n->watch_file ? fopen(n->watch_file, "r") : NULL;
    char wl[64], *tok = NULL;
    uint8_t wm[6];
    if (wf && fgets(wl, sizeof(wl), wf) && (tok = strtok(wl, " \r\n")) != NULL && !parse_mac(tok, wm)) {
        struct bat_client_route cr;
        const bool routed = bat_client_route(n->b, wm, &cr);
        fprintf(o, "+QRY: watch=%02x:%02x:%02x:%02x:%02x:%02x routed=%d orig=%02x:%02x:%02x:%02x:%02x:%02x "
                   "tput=%lu age=%lu\n", wm[0], wm[1], wm[2], wm[3], wm[4], wm[5], routed ? 1 : 0, cr.orig[0],
                cr.orig[1], cr.orig[2], cr.orig[3], cr.orig[4], cr.orig[5], (unsigned long)cr.tput,
                (unsigned long)cr.ogm_age_ms);
    }
    if (wf) {
        fclose(wf);
    }
    fprintf(o, "+NODE: pid=%ld now=%lu rx_frames=%lu rx_outgoing=%lu rx_trunc=%lu tx_frames=%lu "
               "tx_err=%lu tx_nopeer=%lu air_frames=%lu tap_rx=%lu tap_rx_drop=%lu tap_tx=%lu "
               "tap_tx_err=%lu seq_carried=%d\n",
            (long)getpid(), (unsigned long)op_now(n), n->rx_frames, n->rx_outgoing, n->rx_trunc,
            n->tx_frames, n->tx_err, n->tx_nopeer, n->air_frames, n->tap_rx, n->tap_rx_drop,
            n->tap_tx, n->tap_tx_err, n->seq_carried ? 1 : 0);
    fclose(o);
    rename(tmp, path);
}

static void usage(void)
{
    fprintf(stderr,
            "usage: batvm_node --hard <if> --tap <name> --hard-mac <mac> --soft-mac <mac>\n"
            "  [--hard-mtu 1500] [--soft-mtu 1460] [--tput <units>|auto] [--bcast-copies 1]\n"
            "  [--no-aggr] [--wired] [--status <file>] [--seed <n>] [--clock-base <u32>]\n"
            "  [--extra-client <mac>] [--watch-file <file>] [--seq-file <file>]\n"
            "  [--inject <monitor-if> --shape ae2|group|rewrite --peer <mac>...]\n");
}

/* @path as a struct bat_seq_keep shared with every later process that maps it; NULL on error. */
static struct bat_seq_keep *map_seq_file(const char *path)
{
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0 || ftruncate(fd, sizeof(struct bat_seq_keep)) < 0) {
        perror("batvm_node: seq file");
        if (fd >= 0) {
            close(fd);
        }
        return NULL;
    }
    void *m = mmap(NULL, sizeof(struct bat_seq_keep), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        perror("batvm_node: seq file mmap");
        return NULL;
    }
    return m;
}

int main(int argc, char **argv)
{
    static struct node n;
    struct bat_config c;
    const char *hard = NULL, *tap = NULL, *status = NULL, *inject = NULL, *seq_file = NULL;
    int soft_mtu = BAT_SOFT_MTU_DEFAULT;
    bool have_hard = false, have_soft = false;

    bat_config_defaults(&c);
    n.hard_fd = n.inj_fd = n.tap_fd = -1;
    n.tput = 100;
    n.shape = SH_AE2;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define NEEDV() do { if (!v) { usage(); return 2; } i++; } while (0)
        if (!strcmp(a, "--hard")) { NEEDV(); hard = v; }
        else if (!strcmp(a, "--tap")) { NEEDV(); tap = v; }
        else if (!strcmp(a, "--hard-mac")) { NEEDV(); if (parse_mac(v, c.hard_addr)) { usage(); return 2; } have_hard = true; }
        else if (!strcmp(a, "--soft-mac")) { NEEDV(); if (parse_mac(v, c.soft_addr)) { usage(); return 2; } have_soft = true; }
        else if (!strcmp(a, "--hard-mtu")) { NEEDV(); c.hard_mtu = (uint16_t)atoi(v); }
        else if (!strcmp(a, "--soft-mtu")) { NEEDV(); soft_mtu = atoi(v); }
        else if (!strcmp(a, "--tput")) { NEEDV(); if (!strcmp(v, "auto")) n.tput_auto = true; else n.tput = (uint32_t)strtoul(v, NULL, 0); }
        else if (!strcmp(a, "--bcast-copies")) { NEEDV(); c.bcast_copies = (uint8_t)atoi(v); }
        else if (!strcmp(a, "--no-aggr")) { c.aggregate_ogm = false; }
        else if (!strcmp(a, "--wired")) { c.half_duplex = false; }
        else if (!strcmp(a, "--status")) { NEEDV(); status = v; }
        else if (!strcmp(a, "--seed")) { NEEDV(); n.rng = (uint32_t)strtoul(v, NULL, 0) | 1u; n.seeded = true; }
        else if (!strcmp(a, "--clock-base")) { NEEDV(); n.clock_base = (uint32_t)strtoul(v, NULL, 0); n.clock_set = true; }
        else if (!strcmp(a, "--extra-client")) { NEEDV(); if (parse_mac(v, n.extra)) { usage(); return 2; } n.have_extra = true; }
        else if (!strcmp(a, "--watch-file")) { NEEDV(); n.watch_file = v; }
        else if (!strcmp(a, "--seq-file")) { NEEDV(); seq_file = v; }
        else if (!strcmp(a, "--inject")) { NEEDV(); inject = v; }
        else if (!strcmp(a, "--shape")) {
            NEEDV();
            if (!strcmp(v, "ae2")) n.shape = SH_AE2;
            else if (!strcmp(v, "group")) n.shape = SH_GROUP;
            else if (!strcmp(v, "rewrite")) n.shape = SH_REWRITE;
            else { usage(); return 2; }
        }
        else if (!strcmp(a, "--peer")) {
            NEEDV();
            if (n.npeers == MAX_PEERS || parse_mac(v, n.peers[n.npeers])) { usage(); return 2; }
            n.npeers++;
        }
        else { usage(); return 2; }
#undef NEEDV
    }
    if (!hard || !tap || !have_hard || !have_soft) {
        usage();
        return 2;
    }
    snprintf(n.hard_if, sizeof(n.hard_if), "%s", hard);
    memcpy(n.own, c.hard_addr, 6);
    clock_gettime(CLOCK_MONOTONIC, &n.t0);

    uint8_t ifm[6];
    if (if_mac(hard, ifm) || memcmp(ifm, c.hard_addr, 6) != 0) {
        fprintf(stderr, "batvm_node: %s does not carry --hard-mac\n", hard);
        return 1;
    }
    n.hard_fd = open_packet(hard, BAT_ETHERTYPE, &n.hard_ifidx);
    if (n.hard_fd < 0) {
        return 1;
    }
    if (inject) {
        n.inj_fd = open_packet(inject, 0x0003 /* ETH_P_ALL */, &n.inj_ifidx);
        if (n.inj_fd < 0) {
            return 1;
        }
    }
    n.tap_fd = open_tap(tap, c.soft_addr, soft_mtu);
    if (n.tap_fd < 0) {
        return 1;
    }
    if (seq_file && !(c.seq_keep = map_seq_file(seq_file))) {
        return 1;
    }
    n.b = calloc(1, bat_ctx_size());
    const struct bat_ops ops = { .tx = op_tx, .deliver = op_deliver, .now_ms = op_now, .rand32 = op_rand,
                                 .link_tput = op_link_tput };
    const int seq = n.b ? bat_init(n.b, &c, &ops, &n) : -1;
    if (seq < 0) {
        fprintf(stderr, "batvm_node: bat_init refused the configuration\n");
        return 1;
    }
    n.seq_carried = seq == 1;
    fprintf(stderr, "batvm_node: seq=%s elp %u ogm %u bcast %u\n", seq ? "carried" : "random",
            (unsigned)n.b->elp_seq, (unsigned)n.b->ogm_seq, (unsigned)n.b->bcast_seq + 1u);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sig;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    static uint8_t buf[65536];
    uint32_t wait = bat_tick(n.b);
    uint32_t next_status = op_now(&n);
    while (!g_stop) {
        struct pollfd p[2] = { { .fd = n.hard_fd, .events = POLLIN }, { .fd = n.tap_fd, .events = POLLIN } };
        uint32_t tnow = op_now(&n);
        int32_t to_status = (int32_t)(next_status - tnow);
        int timeout = (int)wait;
        if (status && to_status < timeout) {
            timeout = to_status < 0 ? 0 : to_status;
        }
        int pr = poll(p, 2, timeout);
        if (pr < 0 && errno != EINTR) {
            perror("batvm_node: poll");
            break;
        }
        if (pr > 0 && (p[0].revents & POLLIN)) {
            for (int k = 0; k < 64; k++) {
                struct sockaddr_ll from;
                socklen_t fl = sizeof(from);
                ssize_t r = recvfrom(n.hard_fd, buf, sizeof(buf), MSG_DONTWAIT | MSG_TRUNC,
                                     (struct sockaddr *)&from, &fl);
                if (r < 0) {
                    break;
                }
                if (from.sll_pkttype == PACKET_OUTGOING) {
                    n.rx_outgoing++;
                    continue;
                }
                if ((size_t)r > sizeof(buf)) {
                    n.rx_trunc++;
                    continue;
                }
                n.rx_frames++;
                bat_rx_hard(n.b, buf, (size_t)r);
                wait = bat_tick(n.b);
            }
        }
        if (pr > 0 && (p[1].revents & POLLIN)) {
            for (int k = 0; k < 64; k++) {
                ssize_t r = read(n.tap_fd, buf, sizeof(buf));
                if (r <= 0) {
                    break;
                }
                n.tap_rx++;
                if (bat_tx_soft(n.b, buf, (size_t)r) != 0) {
                    n.tap_rx_drop++;
                }
                wait = bat_tick(n.b);
            }
        }
        if (g_add) {
            g_add = 0;
            if (n.have_extra && bat_tt_local_add(n.b, n.extra, 0, 0) != 0) {
                fprintf(stderr, "batvm_node: extra client add refused\n");
            }
        }
        if (g_del) {
            g_del = 0;
            if (n.have_extra && bat_tt_local_del(n.b, n.extra, 0) != 0) {
                fprintf(stderr, "batvm_node: extra client delete refused\n");
            }
        }
        wait = bat_tick(n.b);
        if (status && (int32_t)(op_now(&n) - next_status) >= 0) {
            write_status(&n, status);
            next_status = op_now(&n) + 1000;
        }
    }
    if (status) {
        write_status(&n, status);
    }
    free(n.b);
    return 0;
}
