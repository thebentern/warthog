/* usbprobe [reset <serial-substring>]: list 303a:* devices, whether each answers a control request, and
 * optionally re-enumerate one (a USB port reset, VBUS stays on, so the board does not reboot). */
#include <libusb.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    libusb_context *ctx = NULL;
    libusb_device **list;
    if (libusb_init(&ctx) != 0) { printf("libusb_init failed\n"); return 2; }
    ssize_t n = libusb_get_device_list(ctx, &list);
    const char *want = (argc >= 3 && strcmp(argv[1], "reset") == 0) ? argv[2] : NULL;
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) != 0 || d.idVendor != 0x303a) continue;
        uint8_t ports[8];
        int np = libusb_get_port_numbers(list[i], ports, 8);
        char loc[40] = "";
        for (int k = 0; k < np; k++) { char t[8]; snprintf(t, sizeof t, "%s%d", k ? "." : "", ports[k]); strcat(loc, t); }
        libusb_device_handle *h = NULL;
        int rc = libusb_open(list[i], &h);
        char serial[64] = "?";
        int sr = -99, st = -99;
        uint8_t status[2] = {0, 0};
        if (rc == 0) {
            sr = libusb_get_string_descriptor_ascii(h, d.iSerialNumber, (unsigned char *)serial, sizeof serial);
            /* GET_STATUS (device): the smallest control transfer there is */
            st = libusb_control_transfer(h, 0x80, 0x00, 0, 0, status, 2, 700);
        }
        printf("%04x:%04x bus %d port %s speed %d open=%d serial_rc=%d serial=%s get_status_rc=%d\n", d.idVendor, d.idProduct,
               libusb_get_bus_number(list[i]), loc, libusb_get_device_speed(list[i]), rc, sr, sr > 0 ? serial : "-", st);
        if (h && want && ((sr > 0 && strstr(serial, want)) || strcmp(want, loc) == 0)) {
            int rr = libusb_reset_device(h);
            printf("  reset_device(%s) -> %d (%s)\n", want, rr, rr == 0 ? "ok" : libusb_error_name(rr));
        }
        if (h) libusb_close(h);
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return 0;
}
