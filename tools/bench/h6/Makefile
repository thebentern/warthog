# Host tools for the H6 USB tests (tools/bench/README.md, "The H6 tests"). Needs libusb-1.0
# (macOS: brew install libusb pkg-config; Debian: apt install libusb-1.0-0-dev pkg-config).
CFLAGS ?= -O2 -Wall
USB := $(shell pkg-config --cflags --libs libusb-1.0)

all: usbctl usbprobe

usbctl: usbctl.c
	$(CC) $(CFLAGS) -o $@ $< $(USB)

usbprobe: usbprobe.c
	$(CC) $(CFLAGS) -o $@ $< $(USB)

clean:
	rm -f usbctl usbprobe

.PHONY: all clean
