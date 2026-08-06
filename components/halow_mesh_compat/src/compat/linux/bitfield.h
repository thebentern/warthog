/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * <linux/bitfield.h> compat — FIELD_GET / FIELD_PREP used by the S1G IE
 * builders/parsers in dot11ah.
 */
#ifndef _COMPAT_LINUX_BITFIELD_H_
#define _COMPAT_LINUX_BITFIELD_H_

#include <linux/types.h>
#include <linux/kernel.h>

/* __ffs of a compile-time-constant mask: index of the lowest set bit. */
static inline unsigned int __compat_mask_shift(u64 mask)
{
    unsigned int s = 0;
    if (!mask)
        return 0;
    while (!(mask & 1)) {
        mask >>= 1;
        s++;
    }
    return s;
}

/* Extract the field described by _mask from _reg. */
#define FIELD_GET(_mask, _reg) \
    ((typeof(_mask))(((_reg) & (_mask)) >> __compat_mask_shift(_mask)))

/* Build a register value placing _val into the field described by _mask. */
#define FIELD_PREP(_mask, _val) \
    (((typeof(_mask))(_val) << __compat_mask_shift(_mask)) & (_mask))

/* u{8,16,32}_get_bits / _encode_bits — typed FIELD_GET/PREP the kernel
 * ieee80211.h uses for S1G capability/operation field extraction. */
#define u8_get_bits(v, mask)   ((u8) FIELD_GET((u8)(mask),  (v)))
#define u16_get_bits(v, mask)  ((u16)FIELD_GET((u16)(mask), (v)))
#define u32_get_bits(v, mask)  ((u32)FIELD_GET((u32)(mask), (v)))
#define u8_encode_bits(v, mask)  ((u8) FIELD_PREP((u8)(mask),  (v)))
#define u16_encode_bits(v, mask) ((u16)FIELD_PREP((u16)(mask), (v)))
#define u32_encode_bits(v, mask) ((u32)FIELD_PREP((u32)(mask), (v)))
#define le16_get_bits(v, mask) u16_get_bits(le16_to_cpu(v), mask)
#define le32_get_bits(v, mask) u32_get_bits(le32_to_cpu(v), mask)

#endif /* _COMPAT_LINUX_BITFIELD_H_ */
