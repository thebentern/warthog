// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * mm8108 - native macOS userspace access to the Morse Micro MM8108
 *          Wi-Fi HaLow (802.11ah) USB adapter (USB ID 325b:8100).
 *
 * The Morse Micro USB interface is NOT a command protocol. It is a raw
 * memory/register access bus into the chip's address space. Everything the
 * Linux driver does on top (firmware load, the YAPS/pager mailbox, morse_cmd
 * request/response) is built out of these four primitives:
 *
 *     dm_read(addr, len)   reg32_read(addr)
 *     dm_write(addr, len)  reg32_write(addr, val)
 *
 * Wire protocol, reverse-engineered from the GPL driver's usb.c
 * (struct morse_usb_command / morse_usb_mem_read / morse_usb_mem_write):
 *
 *   1. Write a 12-byte little-endian command to bulk OUT ep 0x02:
 *        __le32 dir      0x00 = write, 0x80 = read, 0x02 = non-destructive reset
 *        __le32 address  chip address for the following bulk transfer
 *        __le32 length   byte count of the following bulk transfer
 *   2. Then move `length` bytes:
 *        read  -> bulk IN  0x82
 *        write -> bulk OUT 0x02
 *
 * The device exposes exactly three endpoints on a single vendor-specific
 * interface (class 255 / subclass 255 / protocol 41):
 *   0x02 bulk OUT (512)  - command + memory write
 *   0x82 bulk IN  (512)  - memory read
 *   0x81 int  IN  (8)    - "YAPS STAT" doorbell from chip to host
 *
 * macOS attaches only AppleUSBHostCompositeDevice to the device and leaves the
 * interface nub unclaimed, so libusb can take interface 0 without any kext
 * detach and without root.
 */

#include <libusb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MORSE_VID 0x325b
#define MORSE_PID 0x8100

#define EP_OUT 0x02 /* bulk OUT: command header + memory write payload */
#define EP_IN 0x82  /* bulk IN : memory read payload */
#define EP_INT 0x81 /* interrupt IN: 8-byte YAPS status */

/* enum morse_usb_command_direction, usb.c */
#define DIR_WRITE 0x00
#define DIR_READ 0x80
#define DIR_RESET 0x02

/* USB_MAX_TRANSFER_SIZE, usb.c */
#define MAX_XFER (16 * 1024)
/* URB_TIMEOUT_MS in the driver is 250; be a little more forgiving. */
#define TIMEOUT_MS 1000

/* Register addresses from mm8108.c */
#define MM8108_REG_CHIP_ID 0x00002d20
#define MM8108_REG_MANIFEST_PTR_ADDRESS 0x00002d40 /* SW manifest pointer */

/* MM8108_REG_HOST_MAGIC_VALUE, mm8108.c - written by firmware into host_table */
#define HOST_TABLE_MAGIC 0xdeadbeef

/* MORSE_SEMVER_GET_*, morse.h */
#define SEMVER_MAJOR(x) (((x) >> 22) & 0x3ff)
#define SEMVER_MINOR(x) (((x) >> 10) & 0xfff)
#define SEMVER_PATCH(x) ((x) & 0x3ff)

/* MORSE_DEVICE_ID(chip, rev, type) == chip | rev<<8 | type<<12, morse.h */
#define DEV_CHIP(id) ((id) & 0xff)
#define DEV_REV(id) (((id) >> 8) & 0xf)
#define DEV_TYPE(id) (((id) >> 12) & 0xf)
#define MM8108XX_ID 0x9
#define MM6108XX_ID 0x6

static libusb_context *ctx;
static libusb_device_handle *dev;
static bool verbose;

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Step 1 of every transaction: the 12-byte header on the OUT endpoint. */
static int mm_cmd(uint32_t dir, uint32_t address, uint32_t length)
{
	uint8_t hdr[12];
	int transferred = 0;
	int rc;

	put_le32(&hdr[0], dir);
	put_le32(&hdr[4], address);
	put_le32(&hdr[8], length);

	if (verbose)
		fprintf(stderr, "[cmd ] dir=0x%02x addr=0x%08x len=%u\n", dir, address, length);

	rc = libusb_bulk_transfer(dev, EP_OUT, hdr, sizeof(hdr), &transferred, TIMEOUT_MS);
	if (rc != 0) {
		fprintf(stderr, "cmd header failed: %s\n", libusb_error_name(rc));
		return rc;
	}
	if (transferred != (int)sizeof(hdr)) {
		fprintf(stderr, "cmd header short write: %d/%zu\n", transferred, sizeof(hdr));
		return LIBUSB_ERROR_IO;
	}
	return 0;
}

static int mm_mem_read(uint32_t address, uint8_t *data, int len)
{
	int off = 0;

	while (off < len) {
		int chunk = len - off > MAX_XFER ? MAX_XFER : len - off;
		int transferred = 0;
		int rc;

		rc = mm_cmd(DIR_READ, address + off, chunk);
		if (rc)
			return rc;

		rc = libusb_bulk_transfer(dev, EP_IN, data + off, chunk, &transferred, TIMEOUT_MS);
		if (rc != 0) {
			fprintf(stderr, "mem read failed @0x%08x: %s\n", address + off,
				libusb_error_name(rc));
			return rc;
		}
		if (transferred != chunk) {
			fprintf(stderr, "mem read short @0x%08x: %d/%d\n", address + off, transferred,
				chunk);
			return LIBUSB_ERROR_IO;
		}
		off += chunk;
	}
	return 0;
}

static int mm_mem_write(uint32_t address, const uint8_t *data, int len)
{
	int off = 0;

	while (off < len) {
		int chunk = len - off > MAX_XFER ? MAX_XFER : len - off;
		int transferred = 0;
		int rc;

		rc = mm_cmd(DIR_WRITE, address + off, chunk);
		if (rc)
			return rc;

		rc = libusb_bulk_transfer(dev, EP_OUT, (uint8_t *)data + off, chunk, &transferred,
					  TIMEOUT_MS);
		if (rc != 0) {
			fprintf(stderr, "mem write failed @0x%08x: %s\n", address + off,
				libusb_error_name(rc));
			return rc;
		}
		if (transferred != chunk) {
			fprintf(stderr, "mem write short @0x%08x: %d/%d\n", address + off,
				transferred, chunk);
			return LIBUSB_ERROR_IO;
		}
		off += chunk;
	}
	return 0;
}

static int mm_reg32_read(uint32_t address, uint32_t *val)
{
	uint8_t buf[4];
	int rc = mm_mem_read(address, buf, sizeof(buf));

	if (rc)
		return rc;
	*val = get_le32(buf);
	return 0;
}

static int mm_reg32_write(uint32_t address, uint32_t val)
{
	uint8_t buf[4];

	put_le32(buf, val);
	return mm_mem_write(address, buf, sizeof(buf));
}

static const char *chip_name(uint32_t id)
{
	static char buf[32];
	const char *fam;

	switch (DEV_CHIP(id)) {
	case MM8108XX_ID:
		fam = "MM8108";
		break;
	case MM6108XX_ID:
		fam = "MM6108";
		break;
	default:
		return "unknown";
	}
	/* MM8108 revs 0x6..0x9 map to B0..B3; MM6108 revs 2..4 map to A0..A2. */
	if (DEV_CHIP(id) == MM8108XX_ID && DEV_REV(id) >= 0x6 && DEV_REV(id) <= 0x9)
		snprintf(buf, sizeof(buf), "%sB%u%s", fam, DEV_REV(id) - 0x6,
			 DEV_TYPE(id) == 1 ? "-FPGA" : "");
	else if (DEV_CHIP(id) == MM6108XX_ID && DEV_REV(id) >= 2 && DEV_REV(id) <= 4)
		snprintf(buf, sizeof(buf), "%sA%u%s", fam, DEV_REV(id) - 2,
			 DEV_TYPE(id) == 1 ? "-FPGA" : "");
	else
		snprintf(buf, sizeof(buf), "%s rev 0x%x%s", fam, DEV_REV(id),
			 DEV_TYPE(id) == 1 ? "-FPGA" : "");
	return buf;
}

static void hexdump(uint32_t base, const uint8_t *p, int len)
{
	for (int i = 0; i < len; i += 16) {
		int n = len - i > 16 ? 16 : len - i;

		printf("%08x  ", base + i);
		for (int j = 0; j < 16; j++) {
			if (j < n)
				printf("%02x ", p[i + j]);
			else
				printf("   ");
			if (j == 7)
				printf(" ");
		}
		printf(" |");
		for (int j = 0; j < n; j++) {
			uint8_t c = p[i + j];

			putchar(c >= 0x20 && c < 0x7f ? c : '.');
		}
		printf("|\n");
	}
}

static int open_device(void)
{
	int rc = libusb_init(&ctx);

	if (rc != 0) {
		fprintf(stderr, "libusb_init: %s\n", libusb_error_name(rc));
		return rc;
	}

	dev = libusb_open_device_with_vid_pid(ctx, MORSE_VID, MORSE_PID);
	if (!dev) {
		fprintf(stderr, "no Morse Micro device %04x:%04x found (is QEMU/a VM holding it?)\n",
			MORSE_VID, MORSE_PID);
		libusb_exit(ctx);
		return LIBUSB_ERROR_NO_DEVICE;
	}

	rc = libusb_claim_interface(dev, 0);
	if (rc != 0) {
		fprintf(stderr, "claim interface 0: %s\n", libusb_error_name(rc));
		libusb_close(dev);
		libusb_exit(ctx);
		return rc;
	}
	return 0;
}

static void close_device(void)
{
	if (dev) {
		libusb_release_interface(dev, 0);
		libusb_close(dev);
	}
	if (ctx)
		libusb_exit(ctx);
}

static int cmd_info(void)
{
	uint32_t id = 0;
	int rc = mm_reg32_read(MM8108_REG_CHIP_ID, &id);

	if (rc)
		return 1;

	printf("chip id register (0x%08x) = 0x%08x\n", MM8108_REG_CHIP_ID, id);
	printf("  chip    0x%x\n", DEV_CHIP(id));
	printf("  rev     0x%x\n", DEV_REV(id));
	printf("  type    %s\n", DEV_TYPE(id) == 1 ? "FPGA" : "silicon");
	printf("  decoded %s\n", chip_name(id));
	return 0;
}

static int cmd_readreg(uint32_t addr)
{
	uint32_t val = 0;

	if (mm_reg32_read(addr, &val))
		return 1;
	printf("0x%08x = 0x%08x\n", addr, val);
	return 0;
}

static int cmd_writereg(uint32_t addr, uint32_t val)
{
	if (mm_reg32_write(addr, val))
		return 1;
	printf("wrote 0x%08x = 0x%08x\n", addr, val);
	return 0;
}

static int cmd_readmem(uint32_t addr, int len)
{
	uint8_t *buf;
	int rc;

	if (len <= 0 || len > 1024 * 1024) {
		fprintf(stderr, "bad length %d\n", len);
		return 1;
	}
	buf = calloc(1, (size_t)len);
	if (!buf)
		return 1;

	rc = mm_mem_read(addr, buf, len);
	if (!rc)
		hexdump(addr, buf, len);
	free(buf);
	return rc ? 1 : 0;
}

/* Dump raw chip memory to stdout, for piping into strings/grep/xxd. */
static int cmd_dumpraw(uint32_t addr, int len)
{
	uint8_t *buf;
	int rc;

	if (len <= 0 || len > 8 * 1024 * 1024) {
		fprintf(stderr, "bad length %d\n", len);
		return 1;
	}
	buf = calloc(1, (size_t)len);
	if (!buf)
		return 1;

	rc = mm_mem_read(addr, buf, len);
	if (!rc)
		fwrite(buf, 1, (size_t)len, stdout);
	free(buf);
	return rc ? 1 : 0;
}

/*
 * Read the firmware host table (struct host_table, hw.h). Its address comes
 * from the SW manifest pointer register, which the firmware populates at boot.
 * A zero pointer or a magic other than 0xdeadbeef means no firmware is running.
 */
static int cmd_hosttable(void)
{
	uint32_t ptr = 0;
	uint8_t t[28];
	uint32_t magic, ver, host_flags, fw_flags, memcmd_cmd, memcmd_resp, ext;

	if (mm_reg32_read(MM8108_REG_MANIFEST_PTR_ADDRESS, &ptr))
		return 1;

	printf("manifest ptr (0x%08x) = 0x%08x\n", MM8108_REG_MANIFEST_PTR_ADDRESS, ptr);
	if (ptr == 0) {
		printf("  no firmware loaded (null host table pointer)\n");
		return 0;
	}

	if (mm_mem_read(ptr, t, sizeof(t)))
		return 1;

	magic = get_le32(&t[0]);
	ver = get_le32(&t[4]);
	host_flags = get_le32(&t[8]);
	fw_flags = get_le32(&t[12]);
	memcmd_cmd = get_le32(&t[16]);
	memcmd_resp = get_le32(&t[20]);
	ext = get_le32(&t[24]);

	printf("host table @ 0x%08x\n", ptr);
	printf("  magic            0x%08x%s\n", magic,
	       magic == HOST_TABLE_MAGIC ? " (valid, firmware running)" : " (INVALID)");
	printf("  fw_version       0x%08x -> command API semver %u.%u.%u\n", ver,
	       SEMVER_MAJOR(ver), SEMVER_MINOR(ver), SEMVER_PATCH(ver));
	printf("  host_flags       0x%08x\n", host_flags);
	printf("  firmware_flags   0x%08x\n", fw_flags);
	printf("  memcmd_cmd_addr  0x%08x\n", memcmd_cmd);
	printf("  memcmd_resp_addr 0x%08x\n", memcmd_resp);
	printf("  ext_host_table   0x%08x\n", ext);
	return 0;
}

/* MM8108_APPS_MAC_DMEM_ADDR_START (DTCM) in mm8108.c, and its observed extent. */
#define DTCM_START 0x00100000
#define DTCM_LEN 0x20000

/* Print the printable run surrounding buf[i]. */
static void print_string_at(const uint8_t *buf, int len, int i)
{
	int s = i, e = i;

	while (s > 0 && buf[s - 1] >= 0x20 && buf[s - 1] < 0x7f)
		s--;
	while (e < len && buf[e] >= 0x20 && buf[e] < 0x7f)
		e++;
	printf("  %.*s\n", e - s, &buf[s]);
}

/*
 * Report firmware version.
 *
 * Two independent sources:
 *  - host_table.fw_version_number, a packed semver of the *command API*. This is
 *    what the kernel driver checks for compatibility. Authoritative and stable.
 *  - the firmware boot banner, scraped out of the firmware's log ring buffer in
 *    DTCM. This carries the human-readable build string that the MORSE_CMD_ID_
 *    GET_VERSION (0x0002) command would return. Scraping it is NOT the same as
 *    issuing that command - see README. The banner is only present until the log
 *    ring wraps, so treat a miss as inconclusive rather than as "no firmware".
 */
static int cmd_version(void)
{
	uint32_t ptr = 0, ver = 0, magic = 0;
	uint8_t hdr[8];
	uint8_t *buf;
	int hits = 0;

	if (mm_reg32_read(MM8108_REG_MANIFEST_PTR_ADDRESS, &ptr))
		return 1;
	if (ptr == 0) {
		printf("no firmware running (null host table pointer)\n");
		return 0;
	}
	if (mm_mem_read(ptr, hdr, sizeof(hdr)))
		return 1;
	magic = get_le32(&hdr[0]);
	ver = get_le32(&hdr[4]);

	if (magic != HOST_TABLE_MAGIC) {
		printf("host table magic invalid (0x%08x) - firmware not running\n", magic);
		return 1;
	}

	printf("command API semver : %u.%u.%u  (host_table.fw_version_number = 0x%08x)\n",
	       SEMVER_MAJOR(ver), SEMVER_MINOR(ver), SEMVER_PATCH(ver), ver);

	buf = calloc(1, DTCM_LEN);
	if (!buf)
		return 1;
	if (mm_mem_read(DTCM_START, buf, DTCM_LEN)) {
		free(buf);
		return 1;
	}

	printf("firmware log banner:\n");
	for (int i = 0; i + 8 < DTCM_LEN; i++) {
		if (memcmp(&buf[i], "version:", 8) == 0) {
			print_string_at(buf, DTCM_LEN, i);
			hits++;
		}
	}
	if (!hits)
		printf("  (boot banner not found - log ring has probably wrapped)\n");

	free(buf);
	return 0;
}

/* ---------------------------------------------------------------------------
 * YAPS command mailbox
 *
 * MM8108 carries morse_cmd request/response over "YAPS", which is built
 * entirely on top of the memory primitives above - there is no doorbell
 * register. Two streaming ports and one status block, all discovered at run
 * time from the firmware host table:
 *
 *   yds_addr    write port: dm_write here enqueues a to-chip packet
 *   ysl_addr    read port : dm_read here pops from-chip packets
 *   status_addr 18 x u32 counters, including the chip-side spinlock
 *
 * Each packet on the wire is a 4-byte delimiter followed by the payload:
 *
 *   bits 0-13  pkt_size + yaps_reserved_page_size
 *   bits 14-16 pool id  (to-chip: TX=0 CMD=1 BEACON=2 MGMT=3;
 *                        from-chip: RX=4 CMD_RESP=5 TX_STATUS=6 AUX=7)
 *   bits 17-18 padding to a 4-byte boundary
 *   bit  19    IRQ - this bit IS the doorbell
 *   bits 20-24 reserved, zero
 *   bits 25-31 CRC-7 over bits 0-24
 *
 * The payload is a 40-byte morse_buff_skb_header followed by the command.
 * ------------------------------------------------------------------------- */

#define YAPS_POOL_CMD 1      /* MORSE_YAPS_CMD_Q */
#define YAPS_POOL_CMD_RESP 5 /* MORSE_YAPS_CMD_RESP_Q */

#define SKB_HDR_LEN 40 /* sizeof(struct morse_buff_skb_header) */
#define SKB_SYNC 0xaa  /* MORSE_SKB_HEADER_SYNC */
#define SKB_CHAN_COMMAND 0xfe

#define CMD_HDR_LEN 12 /* sizeof(struct morse_cmd_header) */
#define CMD_TYPE_REQ 0x0001
#define CMD_TYPE_RESP 0x0002
#define CMD_ID_GET_VERSION 0x0002

#define STATUS_REGS_WORDS 18
#define STATUS_IDX_FC_RX_BYTES 14
#define STATUS_IDX_LOCK 17

#define TLV_TAG_YAPS_TABLE 3

/* Hostsync interrupt block, base MM8108_REG_INT_BASE = 0x3c50 (mm8108.c). */
#define REG_INT1_STS 0x00003c50
#define REG_INT1_CLR 0x00003c58
/* MORSE_INT_YAPS_FC_PKT_WAITING_IRQN 0, FC_PACKET_FREED_UP 1 (yaps_hw.h) */
#define YAPS_IRQ_BITS 0x3

struct yaps_ctx {
	uint32_t ysl_addr;
	uint32_t yds_addr;
	uint32_t status_addr;
	uint16_t reserved_page_size;
};

/* Linux crc7_be: poly x^7+x^3+1 (0x89), MSB-first, left-aligned in a u8. */
static uint8_t crc7_be_byte(uint8_t crc, uint8_t data)
{
	crc ^= data;
	for (int i = 0; i < 8; i++)
		crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x12) : (uint8_t)(crc << 1);
	return crc;
}

/* morse_yaps_crc(), yaps_hw.c */
static uint8_t yaps_crc(uint32_t word)
{
	uint8_t crc = 0;

	word &= 0x1ffffff;
	for (int i = 0; i < 4; i++) {
		crc = crc7_be_byte(crc, (uint8_t)((word >> 24) & 0xff));
		word <<= 8;
	}
	return (uint8_t)(crc >> 1);
}

static uint32_t yaps_delimiter(const struct yaps_ctx *y, unsigned int size, uint8_t pool, bool irq)
{
	unsigned int pad = (size & 3) ? 4 - (size & 3) : 0;
	uint32_t d = 0;

	d |= ((size + y->reserved_page_size) & 0x3fff);
	d |= (uint32_t)(pad & 3) << 17;
	d |= (uint32_t)(pool & 7) << 14;
	d |= (uint32_t)(irq ? 1 : 0) << 19;
	d |= (uint32_t)yaps_crc(d) << 25;
	return d;
}

/*
 * Walk manifest pointer -> host table -> extended host table -> TLV tag 3 to
 * recover the YAPS stream addresses. Everything here is little-endian and the
 * TLVs deliberately start at offset 10, so parse byte-wise, never by casting.
 */
static int yaps_discover(struct yaps_ctx *y)
{
	uint32_t ht = 0, magic, ext = 0, ext_len = 0;
	uint8_t hdr[28];
	uint8_t *tbl;
	int off;
	bool found = false;

	if (mm_reg32_read(MM8108_REG_MANIFEST_PTR_ADDRESS, &ht))
		return -1;
	if (ht == 0) {
		fprintf(stderr, "no firmware running (null host table pointer)\n");
		return -1;
	}
	if (mm_mem_read(ht, hdr, sizeof(hdr)))
		return -1;

	magic = get_le32(&hdr[0]);
	if (magic != HOST_TABLE_MAGIC) {
		fprintf(stderr, "host table magic invalid (0x%08x)\n", magic);
		return -1;
	}
	/* MORSE_FW_FLAGS_FAILSAFE_MODE is BIT(11); commands are unreliable there. */
	if (get_le32(&hdr[12]) & (1u << 11))
		fprintf(stderr, "warning: firmware is in failsafe mode\n");

	ext = get_le32(&hdr[24]);
	if (ext == 0) {
		fprintf(stderr, "no extended host table\n");
		return -1;
	}
	if (mm_mem_read(ext, (uint8_t *)&ext_len, 4))
		return -1;
	ext_len = get_le32((uint8_t *)&ext_len);
	if (ext_len < 10 || ext_len > 4096) {
		fprintf(stderr, "bogus extended host table length %u\n", ext_len);
		return -1;
	}

	tbl = calloc(1, ext_len);
	if (!tbl)
		return -1;
	if (mm_mem_read(ext, tbl, (int)ext_len)) {
		free(tbl);
		return -1;
	}

	if (verbose)
		fprintf(stderr, "[yaps] host_table=0x%08x ext=0x%08x len=%u mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
			ht, ext, ext_len, tbl[4], tbl[5], tbl[6], tbl[7], tbl[8], tbl[9]);

	/* TLVs start at offset 10; each length includes its own 4-byte header. */
	off = 10;
	while (off + 4 <= (int)ext_len) {
		uint16_t tag = (uint16_t)(tbl[off] | (tbl[off + 1] << 8));
		uint16_t len = (uint16_t)(tbl[off + 2] | (tbl[off + 3] << 8));

		if (len < 4 || off + len > (int)ext_len)
			break;
		if (verbose)
			fprintf(stderr, "[yaps] tlv tag=%u len=%u @+%d\n", tag, len, off);

		if (tag == TLV_TAG_YAPS_TABLE && len >= 4 + 36) {
			const uint8_t *t = &tbl[off + 4];

			y->ysl_addr = get_le32(&t[4]);
			y->yds_addr = get_le32(&t[8]);
			y->status_addr = get_le32(&t[12]);
			y->reserved_page_size = (uint16_t)(t[32] | (t[33] << 8));
			found = true;
		}
		off += len;
	}
	free(tbl);

	if (!found) {
		fprintf(stderr, "YAPS table (TLV tag 3) not found in extended host table\n");
		return -1;
	}
	if (verbose)
		fprintf(stderr, "[yaps] ysl=0x%08x yds=0x%08x status=0x%08x reserved_page=%u\n",
			y->ysl_addr, y->yds_addr, y->status_addr, y->reserved_page_size);
	return 0;
}

/* Clear any latched YAPS interrupt bits, as morse_hw_irq_handle() does. */
static void yaps_ack_irq(void)
{
	uint32_t sts = 0;

	if (mm_reg32_read(REG_INT1_STS, &sts))
		return;
	if (sts & YAPS_IRQ_BITS) {
		if (verbose)
			fprintf(stderr, "[yaps] acking INT1_STS=0x%08x\n", sts);
		mm_reg32_write(REG_INT1_CLR, sts & YAPS_IRQ_BITS);
	}
}

/* Read the 72-byte status block, spinning until the chip-side lock clears. */
static int yaps_status(const struct yaps_ctx *y, uint32_t *words)
{
	uint8_t raw[STATUS_REGS_WORDS * 4];

	for (int tries = 0; tries < 2000; tries++) {
		if (mm_mem_read(y->status_addr, raw, sizeof(raw)))
			return -1;
		for (int i = 0; i < STATUS_REGS_WORDS; i++)
			words[i] = get_le32(&raw[i * 4]);
		if (words[STATUS_IDX_LOCK] == 0)
			return 0;
	}
	fprintf(stderr, "timed out waiting for YAPS status lock to clear\n");
	return -1;
}

/* Build [delim][40-byte skb header][12-byte command header] and push it. */
static int yaps_send_cmd(const struct yaps_ctx *y, uint16_t message_id, uint16_t host_id)
{
	uint8_t frame[4 + SKB_HDR_LEN + CMD_HDR_LEN];
	uint8_t *skb = &frame[4];
	uint8_t *cmd = &frame[4 + SKB_HDR_LEN];
	uint32_t delim;

	memset(frame, 0, sizeof(frame));

	delim = yaps_delimiter(y, SKB_HDR_LEN + CMD_HDR_LEN, YAPS_POOL_CMD, true);
	put_le32(&frame[0], delim);

	skb[0] = SKB_SYNC;
	skb[1] = SKB_CHAN_COMMAND;
	skb[2] = (uint8_t)(CMD_HDR_LEN & 0xff); /* len */
	skb[3] = (uint8_t)(CMD_HDR_LEN >> 8);
	skb[4] = 0; /* offset */
	/* checksum_lower/upper stay zero, as the driver does */

	cmd[0] = CMD_TYPE_REQ & 0xff;
	cmd[1] = CMD_TYPE_REQ >> 8;
	cmd[2] = message_id & 0xff;
	cmd[3] = message_id >> 8;
	/* len (body bytes after the header) = 0, vif_id = 0, pad = 0 */
	cmd[6] = host_id & 0xff;
	cmd[7] = host_id >> 8;

	if (verbose) {
		fprintf(stderr, "[yaps] delim=0x%08x frame:", delim);
		for (size_t i = 0; i < sizeof(frame); i++)
			fprintf(stderr, " %02x", frame[i]);
		fprintf(stderr, "\n");
	}
	return mm_mem_write(y->yds_addr, frame, (int)sizeof(frame));
}

static int cmd_getversion(uint16_t host_id)
{
	struct yaps_ctx y = { 0 };
	uint32_t st[STATUS_REGS_WORDS];
	uint8_t *rx;
	uint32_t avail = 0;
	int rc = 1;

	if (yaps_discover(&y))
		return 1;

	/*
	 * Acknowledge any latched YAPS interrupt before we start. The driver does
	 * this in morse_hw_irq_handle() (read INT1_STS, write INT1_CLR); if the
	 * latch is left set by a host that went away mid-flight, the chip stops
	 * signalling and command responses appear to hang.
	 */
	yaps_ack_irq();

	if (yaps_status(&y, st))
		return 1;
	if (verbose)
		fprintf(stderr, "[yaps] cmd_pool_pages=%u cmd_pkts=%u fc_rx_bytes=%u\n", st[1],
			st[9], st[STATUS_IDX_FC_RX_BYTES]);

	/* Drain anything already queued so we don't mistake it for our response. */
	if (st[STATUS_IDX_FC_RX_BYTES] > 0) {
		uint32_t stale = st[STATUS_IDX_FC_RX_BYTES];
		uint8_t *junk = calloc(1, stale);

		if (junk) {
			mm_mem_read(y.ysl_addr, junk, (int)stale);
			free(junk);
		}
		if (verbose)
			fprintf(stderr, "[yaps] drained %u stale bytes\n", stale);
	}

	if (yaps_send_cmd(&y, CMD_ID_GET_VERSION, host_id))
		return 1;

	/* Poll for the response. The driver's own command timeout is 600 ms. */
	for (int i = 0; i < 600; i++) {
		if (yaps_status(&y, st))
			return 1;
		avail = st[STATUS_IDX_FC_RX_BYTES];
		if (avail > 0)
			break;
	}
	if (avail == 0) {
		fprintf(stderr, "no response from chip (fc_rx_bytes_in_queue stayed 0)\n");
		return 1;
	}
	if (avail > 32768)
		avail = 32768;

	rx = calloc(1, avail);
	if (!rx)
		return 1;
	if (mm_mem_read(y.ysl_addr, rx, (int)avail)) {
		free(rx);
		return 1;
	}
	if (verbose)
		fprintf(stderr, "[yaps] read %u bytes from ysl\n", avail);

	/* Walk the returned stream: [delim][payload][pad] ... */
	uint32_t off = 0;

	while (off + 4 <= avail) {
		uint32_t delim = get_le32(&rx[off]);
		uint32_t sz, pool, pad, pkt;
		const uint8_t *skb, *cmd;

		off += 4;
		if (delim == 0)
			break;
		if (yaps_crc(delim) != ((delim >> 25) & 0x7f)) {
			fprintf(stderr, "invalid delimiter 0x%08x\n", delim);
			break;
		}
		sz = delim & 0x3fff;
		pool = (delim >> 14) & 7;
		pad = (delim >> 17) & 3;
		pkt = sz - y.reserved_page_size;

		if (verbose)
			fprintf(stderr, "[yaps] rx delim=0x%08x pool=%u pkt=%u pad=%u\n", delim,
				pool, pkt, pad);
		if (off + pkt > avail)
			break;

		skb = &rx[off];
		if (pool == YAPS_POOL_CMD_RESP && pkt >= SKB_HDR_LEN + 20 && skb[0] == SKB_SYNC) {
			cmd = skb + SKB_HDR_LEN + skb[4] /* offset */;
			uint16_t flags = (uint16_t)(cmd[0] | (cmd[1] << 8));
			uint16_t mid = (uint16_t)(cmd[2] | (cmd[3] << 8));
			uint16_t hid = (uint16_t)(cmd[6] | (cmd[7] << 8));
			int32_t status = (int32_t)get_le32(&cmd[12]);
			int32_t vlen = (int32_t)get_le32(&cmd[16]);

			if (mid == CMD_ID_GET_VERSION && (flags & CMD_TYPE_RESP)) {
				int max = (int)(pkt - SKB_HDR_LEN - 20);

				if (vlen < 0 || vlen > max)
					vlen = max;
				/* The field is a fixed 128-byte buffer; the string
				 * inside it is NUL-terminated, so trim to it.
				 */
				vlen = (int)strnlen((const char *)&cmd[20], (size_t)vlen);
				printf("GET_VERSION (0x%04x) response\n", mid);
				printf("  host_id  0x%04x (sent 0x%04x)\n", hid, host_id);
				printf("  status   %d\n", status);
				printf("  length   %d\n", vlen);
				printf("  version  %.*s\n", vlen, (const char *)&cmd[20]);
				rc = (status == 0) ? 0 : 1;
			}
		}
		off += pkt + pad;
	}

	free(rx);
	yaps_ack_irq();
	if (rc)
		fprintf(stderr, "no GET_VERSION response found in the returned stream\n");
	return rc;
}

/*
 * Non-destructive USB reset (morse_usb_ndr_reset). This is a bus-level reset of
 * the USB bridge state machine, not a chip reset - it does not reload firmware.
 */
static int cmd_reset(void)
{
	if (mm_cmd(DIR_RESET, 0, 0))
		return 1;
	printf("sent non-destructive reset\n");
	return 0;
}

/* Drain the 8-byte YAPS status doorbell for `count` events. */
static int cmd_irq(int count)
{
	for (int i = 0; i < count; i++) {
		uint8_t buf[8] = { 0 };
		int transferred = 0;
		int rc = libusb_interrupt_transfer(dev, EP_INT, buf, sizeof(buf), &transferred,
						   TIMEOUT_MS);

		if (rc == LIBUSB_ERROR_TIMEOUT) {
			printf("[%d] timeout (no interrupt pending)\n", i);
			continue;
		}
		if (rc != 0) {
			fprintf(stderr, "interrupt transfer: %s\n", libusb_error_name(rc));
			return 1;
		}
		printf("[%d] YAPS STAT (%d):", i, transferred);
		for (int j = 0; j < transferred; j++)
			printf(" %02x", buf[j]);
		printf("\n");
	}
	return 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"mm8108 - native macOS userspace access to the Morse Micro MM8108 (325b:8100)\n"
		"\n"
		"usage: %s [-v] <command>\n"
		"\n"
		"commands:\n"
		"  info                     read and decode the chip ID register\n"
		"  readreg <addr>           read one 32-bit register\n"
		"  writereg <addr> <val>    write one 32-bit register\n"
		"  readmem <addr> <len>     hexdump chip memory\n"
		"  dumpraw <addr> <len>     dump chip memory as binary to stdout\n"
		"  hosttable                decode the firmware host table (proves FW is running)\n"
		"  version                  firmware command-API semver + boot banner\n"
		"  getversion [host_id]     issue a real GET_VERSION (0x0002) over the YAPS mailbox\n"
		"  irq [count]              read the 8-byte YAPS status doorbell (default 1)\n"
		"  reset                    non-destructive USB bridge reset\n"
		"\n"
		"addresses and values are parsed with strtoul(base 0): use 0x... for hex.\n"
		"-v traces every 12-byte command header on stderr.\n",
		argv0);
}

int main(int argc, char **argv)
{
	int argi = 1;
	int rc;

	if (argc > 1 && strcmp(argv[1], "-v") == 0) {
		verbose = true;
		argi++;
	}
	if (argi >= argc) {
		usage(argv[0]);
		return 2;
	}

	rc = open_device();
	if (rc)
		return 1;

	const char *cmd = argv[argi];

	if (strcmp(cmd, "info") == 0) {
		rc = cmd_info();
	} else if (strcmp(cmd, "readreg") == 0 && argc > argi + 1) {
		rc = cmd_readreg((uint32_t)strtoul(argv[argi + 1], NULL, 0));
	} else if (strcmp(cmd, "writereg") == 0 && argc > argi + 2) {
		rc = cmd_writereg((uint32_t)strtoul(argv[argi + 1], NULL, 0),
				  (uint32_t)strtoul(argv[argi + 2], NULL, 0));
	} else if (strcmp(cmd, "readmem") == 0 && argc > argi + 2) {
		rc = cmd_readmem((uint32_t)strtoul(argv[argi + 1], NULL, 0),
				 (int)strtol(argv[argi + 2], NULL, 0));
	} else if (strcmp(cmd, "dumpraw") == 0 && argc > argi + 2) {
		rc = cmd_dumpraw((uint32_t)strtoul(argv[argi + 1], NULL, 0),
				 (int)strtol(argv[argi + 2], NULL, 0));
	} else if (strcmp(cmd, "hosttable") == 0) {
		rc = cmd_hosttable();
	} else if (strcmp(cmd, "version") == 0) {
		rc = cmd_version();
	} else if (strcmp(cmd, "getversion") == 0) {
		/* host_id = seq<<4 | retry; default to a distinctive seq so a stale
		 * queued response from a previous host cannot be mistaken for ours.
		 */
		uint16_t hid = (uint16_t)((argc > argi + 1) ?
						  strtoul(argv[argi + 1], NULL, 0) :
						  (((unsigned)getpid() & 0xfff) << 4));
		rc = cmd_getversion(hid);
	} else if (strcmp(cmd, "irq") == 0) {
		rc = cmd_irq(argc > argi + 1 ? (int)strtol(argv[argi + 1], NULL, 0) : 1);
	} else if (strcmp(cmd, "reset") == 0) {
		rc = cmd_reset();
	} else {
		usage(argv[0]);
		rc = 2;
	}

	close_device();
	return rc;
}
