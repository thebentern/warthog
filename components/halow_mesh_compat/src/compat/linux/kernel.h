/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * <linux/kernel.h> + <linux/bits.h> compat — common macros the morse_driver
 * dot11ah/mesh sources rely on.
 */
#ifndef _COMPAT_LINUX_KERNEL_H_
#define _COMPAT_LINUX_KERNEL_H_

#include <linux/types.h>
#include <string.h>

#ifndef BIT
#define BIT(n)      (1UL << (n))
#endif
#ifndef BIT_ULL
#define BIT_ULL(n)  (1ULL << (n))
#endif

/* GENMASK(h, l): contiguous bitmask from bit l to bit h inclusive. */
#ifndef GENMASK
#define GENMASK(h, l) \
    (((~0UL) - (1UL << (l)) + 1) & (~0UL >> (31 - (h))))
#endif
#ifndef GENMASK_ULL
#define GENMASK_ULL(h, l) \
    (((~0ULL) - (1ULL << (l)) + 1) & (~0ULL >> (63 - (h))))
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef min_t
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#endif
#ifndef max_t
#define max_t(t, a, b) ((t)(a) > (t)(b) ? (t)(a) : (t)(b))
#endif
#ifndef clamp
#define clamp(v, lo, hi) max((lo), min((v), (hi)))
#endif

#ifndef ALIGN
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#endif
#ifndef DIV_ROUND_UP
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#endif

#ifndef offsetof
#define offsetof(t, m) __builtin_offsetof(t, m)
#endif
#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

/* Upper/lower 32 bits of a 64-bit value. */
#ifndef UPPER_32_BITS
#define UPPER_32_BITS(n) ((u32)(((n) >> 16) >> 16))
#endif
#ifndef LOWER_32_BITS
#define LOWER_32_BITS(n) ((u32)((n) & 0xffffffff))
#endif

#ifndef likely
#define likely(x)   __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

/* Endian conversion — host is little-endian on both ESP32-S3 and MM6108. */
#ifndef cpu_to_le16
#define cpu_to_le16(x) ((__le16)(u16)(x))
#define le16_to_cpu(x) ((u16)(__le16)(x))
#define cpu_to_le32(x) ((__le32)(u32)(x))
#define le32_to_cpu(x) ((u32)(__le32)(x))
#define cpu_to_le64(x) ((__le64)(u64)(x))
#define le64_to_cpu(x) ((u64)(__le64)(x))
#endif
#ifndef cpu_to_be16
#define cpu_to_be16(x) ((__be16)__builtin_bswap16((u16)(x)))
#define be16_to_cpu(x) ((u16)__builtin_bswap16((__be16)(x)))
#define cpu_to_be32(x) ((__be32)__builtin_bswap32((u32)(x)))
#define be32_to_cpu(x) ((u32)__builtin_bswap32((__be32)(x)))
#endif

/* get/put unaligned helpers live in <asm/unaligned.h> (compat). */

/* struct_group() — kernel macro (<linux/stddef.h>) that overlays a named
 * sub-struct onto a run of members, used by ieee80211_hdr for the address
 * block. Provide the no-tag/no-attrs form the header needs. */
#ifndef struct_group
#define __struct_group(TAG, NAME, ATTRS, MEMBERS...) \
    union { \
        struct { MEMBERS } ATTRS; \
        struct TAG { MEMBERS } ATTRS NAME; \
    }
#define struct_group(NAME, MEMBERS...) \
    __struct_group(/* no tag */, NAME, /* no attrs */, MEMBERS)
#define struct_group_tagged(TAG, NAME, MEMBERS...) \
    __struct_group(TAG, NAME, /* no attrs */, MEMBERS)
#endif

/* Population count + offsetofend, used by the vendored ieee80211.h inlines. */
static inline unsigned int hweight8(u8 v)  { return __builtin_popcount(v); }
static inline unsigned int hweight16(u16 v){ return __builtin_popcount(v); }
static inline unsigned int hweight32(u32 v){ return __builtin_popcount(v); }
#ifndef offsetofend
#define offsetofend(t, m) (offsetof(t, m) + sizeof(((t *)0)->m))
#endif

/* DECLARE_FLEX_ARRAY — kernel macro for a flexible array inside a union. */
#ifndef DECLARE_FLEX_ARRAY
#define DECLARE_FLEX_ARRAY(TYPE, NAME) \
    struct { struct { } __empty_##NAME; TYPE NAME[]; }
#endif

#ifndef WARN_ON
#define WARN_ON(c)      ({ int __c = !!(c); __c; })
#define WARN_ON_ONCE(c) WARN_ON(c)
#endif
#ifndef WARN_ONCE
#define WARN_ONCE(c, fmt, ...)  ({ int __c = !!(c); (void)(__c); __c; })
#endif
#ifndef WARN
#define WARN(c, fmt, ...)       ({ int __c = !!(c); (void)(__c); __c; })
#endif

/* Module export markers — no-ops in a single-binary firmware build. */
#ifndef EXPORT_SYMBOL
#define EXPORT_SYMBOL(sym)
#define EXPORT_SYMBOL_GPL(sym)
#endif
#ifndef MODULE_LICENSE
#define MODULE_LICENSE(s)
#define MODULE_AUTHOR(s)
#define MODULE_DESCRIPTION(s)
#define MODULE_VERSION(s)
#endif

/* Section attributes + module init/exit registration. The firmware calls the
 * dot11ah init/exit functions directly (they are not kernel module hooks), so
 * the section markers vanish and the (un)register macros are no-ops. */
#ifndef __init
#define __init
#define __exit
#define __devinit
#endif
#ifndef __cold        /* the host libc <sys/cdefs.h> may already define this */
#define __cold
#endif
#ifndef module_init
#define module_init(fn)
#define module_exit(fn)
#endif
/* Module parameters — the firmware has no insmod-time params; drop them. */
#ifndef module_param
#define module_param(name, type, perm)
#define module_param_array(name, type, nump, perm)
#define module_param_named(alias, name, type, perm)
#define MODULE_PARM_DESC(name, desc)
#endif

/* errno codes — the dot11ah/mesh sources return -EINVAL/-ENOMEM/etc. */
#include <linux/errno.h>

/* jiffies / tick-time helpers (beacon timestamping in the TX transform). */
#include <linux/jiffies.h>

/* Single-word bitmap ops (set_bit/test_bit/for_each_set_bit) — pulled here so
 * any source reaching kernel.h transitively gets them, matching the kernel's
 * habit of surfacing bitops through common headers. */
#include <linux/bitops.h>

#endif /* _COMPAT_LINUX_KERNEL_H_ */
