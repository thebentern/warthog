/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bat_crc32c.h"

uint32_t bat_crc32c(uint32_t crc, const uint8_t *p, size_t n)
{
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

uint32_t bat_crc32c_tt(uint16_t vid, uint8_t flags, const uint8_t mac[6])
{
    uint8_t s[9] = { (uint8_t)(vid >> 8), (uint8_t)vid, (uint8_t)(flags & 0xF0),
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5] };
    return bat_crc32c(0, s, sizeof(s));
}
