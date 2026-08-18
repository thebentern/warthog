/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * <endian.h> for the host test build only.
 *
 * hostap's hostap_morse_common.h includes <endian.h>, which is glibc-specific.
 * Defined directly with __builtin_bswap rather than by pulling in macOS's
 * <libkern/OSByteOrder.h>: that header expands to __builtin_bswap32 too, and
 * hostap_morse_common.h defines a bswap_32 macro of its own, so including both
 * makes the two collide. Not part of any firmware build.
 */
#pragma once

#include <stdint.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define WARTHOG_HOST_BE 1
#else
#define WARTHOG_HOST_BE 0
#endif

#if WARTHOG_HOST_BE
#define htobe16(x) ((uint16_t)(x))
#define htole16(x) ((uint16_t)__builtin_bswap16((uint16_t)(x)))
#define htobe32(x) ((uint32_t)(x))
#define htole32(x) ((uint32_t)__builtin_bswap32((uint32_t)(x)))
#define htobe64(x) ((uint64_t)(x))
#define htole64(x) ((uint64_t)__builtin_bswap64((uint64_t)(x)))
#else
#define htobe16(x) ((uint16_t)__builtin_bswap16((uint16_t)(x)))
#define htole16(x) ((uint16_t)(x))
#define htobe32(x) ((uint32_t)__builtin_bswap32((uint32_t)(x)))
#define htole32(x) ((uint32_t)(x))
#define htobe64(x) ((uint64_t)__builtin_bswap64((uint64_t)(x)))
#define htole64(x) ((uint64_t)(x))
#endif

#define be16toh(x) htobe16(x)
#define le16toh(x) htole16(x)
#define be32toh(x) htobe32(x)
#define le32toh(x) htole32(x)
#define be64toh(x) htobe64(x)
#define le64toh(x) htole64(x)
