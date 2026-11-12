/* usbctl <serial-substring> <mode> <count> <gap_us>
 * Sends CDC class requests on endpoint 0 of one board, in a loop, without touching the console port:
 *   coding    SET_LINE_CODING (control OUT with a 7-byte data stage), 115200 8N1 and 57600 8N1 alternately
 *   state     SET_CONTROL_LINE_STATE wValue=3 (control OUT, no data stage)
 *   getcoding GET_LINE_CODING (control IN, 7 bytes)
 *   dtr       SET_CONTROL_LINE_STATE with DTR/RTS up and down alternately
 *   open      what a port open and close send: coding, DTR up, 20 ms, DTR down
 *   bad       GET_DESCRIPTOR for string index 0xEE, which the board refuses (STALL): the H6 stress
 *             (usb_stress.py). A STALL is the expected answer and is counted as ok.
 * Stops at the first request that fails and says how many went through. Build: make (needs libusb-1.0). */
#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

static double now(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec + tv.tv_usec / 1e6; }

int main(int argc, char **argv)
{
    if (argc < 5) { printf("usage\n"); return 2; }
    const char *want = argv[1], *mode = argv[2];
    long count = atol(argv[3]), gap = atol(argv[4]);
    libusb_context *ctx = NULL;
    libusb_device **list;
    if (libusb_init(&ctx) != 0) return 2;
    ssize_t n = libusb_get_device_list(ctx, &list);
    libusb_device_handle *h = NULL;
    int itf = -1;
    for (ssize_t i = 0; i < n && !h; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) != 0 || d.idVendor != 0x303a) continue;
        libusb_device_handle *t = NULL;
        if (libusb_open(list[i], &t) != 0) continue;
        char serial[64] = "";
        if (libusb_get_string_descriptor_ascii(t, d.iSerialNumber, (unsigned char *)serial, sizeof serial) > 0 && strstr(serial, want)) {
            struct libusb_config_descriptor *cfg;
            if (libusb_get_active_config_descriptor(list[i], &cfg) == 0) {
                for (int k = 0; k < cfg->bNumInterfaces && itf < 0; k++) {
                    const struct libusb_interface_descriptor *a = &cfg->interface[k].altsetting[0];
                    if (a->bInterfaceClass == 2 && a->bInterfaceSubClass == 2) itf = a->bInterfaceNumber;
                }
                libusb_free_config_descriptor(cfg);
            }
            h = t;
        } else {
            libusb_close(t);
        }
    }
    if (!h || itf < 0) { printf("device or CDC ACM interface not found (itf=%d)\n", itf); return 3; }
    unsigned char lc[2][7] = { {0x00, 0xc2, 0x01, 0x00, 0, 0, 8}, {0x00, 0xe1, 0x00, 0x00, 0, 0, 8} };
    unsigned char buf[8];
    long ok = 0;
    int rc = 0;
    double t0 = now(), worst = 0;
    for (long i = 0; i < count; i++) {
        double t1 = now();
        if (strcmp(mode, "coding") == 0) rc = libusb_control_transfer(h, 0x21, 0x20, 0, (uint16_t)itf, lc[i & 1], 7, 1500);
        else if (strcmp(mode, "bad") == 0) rc = libusb_control_transfer(h, 0x80, 0x06, 0x03EE, 0x0409, lc[0], 7, 1500); /* no such string: STALL */
        else if (strcmp(mode, "state") == 0) rc = libusb_control_transfer(h, 0x21, 0x22, 3, (uint16_t)itf, NULL, 0, 1500);
        else if (strcmp(mode, "dtr") == 0) rc = libusb_control_transfer(h, 0x21, 0x22, (i & 1) ? 0 : 3, (uint16_t)itf, NULL, 0, 1500); /* DTR/RTS up, down, up ... */
        else if (strcmp(mode, "open") == 0) { /* what a port open and close send: coding, DTR up, (pause), DTR down */
            rc = libusb_control_transfer(h, 0x21, 0x20, 0, (uint16_t)itf, lc[0], 7, 1500);
            if (rc >= 0) rc = libusb_control_transfer(h, 0x21, 0x22, 3, (uint16_t)itf, NULL, 0, 1500);
            if (rc >= 0) { usleep(20000); rc = libusb_control_transfer(h, 0x21, 0x22, 0, (uint16_t)itf, NULL, 0, 1500); }
        }
        else rc = libusb_control_transfer(h, 0xa1, 0x21, 0, (uint16_t)itf, buf, 7, 1500);
        double dt = now() - t1;
        if (dt > worst) worst = dt;
        if (rc == LIBUSB_ERROR_PIPE && strcmp(mode, "bad") == 0) rc = 0;
        if (rc < 0) break;
        ok++;
        if (gap > 0) usleep((useconds_t)gap);
    }
    unsigned char st[2];
    int gs = libusb_control_transfer(h, 0x80, 0x00, 0, 0, st, 2, 1000);
    printf("mode=%s itf=%d: %ld of %ld requests ok in %.1f s, slowest %.0f ms, last rc=%d (%s); GET_STATUS afterwards rc=%d\n", mode, itf, ok, count,
           now() - t0, worst * 1000, rc, rc < 0 ? libusb_error_name(rc) : "ok", gs);
    libusb_close(h);
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return rc < 0 ? 1 : 0;
}
