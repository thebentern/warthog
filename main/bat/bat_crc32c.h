/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WARTHOG_BAT_CRC32C_H
#define WARTHOG_BAT_CRC32C_H
/* CRC-32C, reflected poly 0x82F63B78, caller-supplied start register, no final xor (TT CRC). */
#include <stddef.h>
#include <stdint.h>

uint32_t bat_crc32c(uint32_t crc, const uint8_t *p, size_t n);
/* One TT entry: CRC-32C from 0 over VID (BE16) || flags & 0xF0 || MAC. */
uint32_t bat_crc32c_tt(uint16_t vid, uint8_t flags, const uint8_t mac[6]);

#endif
