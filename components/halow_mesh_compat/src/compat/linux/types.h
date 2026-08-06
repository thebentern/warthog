/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * <linux/types.h> compat shim for the softmac mesh port.
 * Maps Linux kernel fixed-width + endian types onto <stdint.h>.
 */
#ifndef _COMPAT_LINUX_TYPES_H_
#define _COMPAT_LINUX_TYPES_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t  u8;
typedef int8_t   s8;
typedef uint16_t u16;
typedef int16_t  s16;
typedef uint32_t u32;
typedef int32_t  s32;
typedef uint64_t u64;
typedef int64_t  s64;

/* Kernel spells `unsigned int` as `uint` in a few helper signatures. */
#ifndef _UINT_DEFINED
#define _UINT_DEFINED
typedef unsigned int uint;
#endif

/* Endian-annotated types. ESP32-S3 (xtensa) and the MM6108 are both
 * little-endian, so these are storage-compatible with the native types;
 * the __le/__be annotations are documentation only here. */
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef uint16_t __be16;
typedef uint32_t __be32;
typedef uint64_t __be64;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint8_t  __u8;
typedef int8_t   __s8;
typedef int16_t  __s16;
typedef int32_t  __s32;
typedef int64_t  __s64;
typedef uint64_t __u64;

#ifndef __packed
#define __packed __attribute__((packed))
#endif
#ifndef __aligned
#define __aligned(n) __attribute__((aligned(n)))
#endif
#ifndef __must_check
#define __must_check
#endif

/* Compiler attribute macros the morse_driver logging prototypes carry. */
#ifndef __printf
#define __printf(a, b) __attribute__((format(printf, a, b)))
#endif
#ifndef __maybe_unused
#define __maybe_unused __attribute__((unused))
#endif
#ifndef __always_unused
#define __always_unused __attribute__((unused))
#endif

/* Bitmap storage — DECLARE_BITMAP lives in <linux/types.h> in the kernel.
 * BITS_PER_LONG is 32 on the ESP32-S3 target, 64 on the host test build;
 * the bitmap ops in <linux/bitops.h> use the same value, so it stays
 * self-consistent either way. */
#ifndef BITS_PER_LONG
#define BITS_PER_LONG (__SIZEOF_LONG__ * 8)
#endif
#ifndef BITS_TO_LONGS
#define BITS_TO_LONGS(nr) (((nr) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#endif
#ifndef DECLARE_BITMAP
#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]
#endif

/* Fixed-width integer limits (kernel <linux/limits.h> spellings). */
#ifndef U8_MAX
#define U8_MAX   ((u8)~0U)
#define S8_MAX   ((s8)(U8_MAX >> 1))
#define S8_MIN   ((s8)(-S8_MAX - 1))
#define U16_MAX  ((u16)~0U)
#define S16_MAX  ((s16)(U16_MAX >> 1))
#define S16_MIN  ((s16)(-S16_MAX - 1))
#define U32_MAX  ((u32)~0U)
#define S32_MAX  ((s32)(U32_MAX >> 1))
#define S32_MIN  ((s32)(-S32_MAX - 1))
#define U64_MAX  ((u64)~0ULL)
#endif

#endif /* _COMPAT_LINUX_TYPES_H_ */
