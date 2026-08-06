/* SPDX-License-Identifier: GPL-2.0-or-later
 * <linux/crc32.h> compat — IEEE 802.11 FCS/CSSID CRC32.
 * Linux's crc32(seed, buf, len) uses the reflected poly 0xEDB88320. */
#ifndef _COMPAT_LINUX_CRC32_H_
#define _COMPAT_LINUX_CRC32_H_
#include <linux/types.h>
u32 crc32(u32 seed, const void *data, size_t len);   /* impl in compat.c */
#define crc32_le crc32
#endif
