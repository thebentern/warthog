/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * <linux/bitops.h> compat — the single-word bitmap operations the dot11ah IE
 * tracker uses (more_than_one_ie bitmap in struct dot11ah_ies_mask).
 * Non-atomic; the port runs the IE codec on one FreeRTOS task.
 */
#ifndef _COMPAT_LINUX_BITOPS_H_
#define _COMPAT_LINUX_BITOPS_H_

#include <linux/types.h>

static inline void set_bit(int nr, unsigned long *addr)
{
    addr[nr / BITS_PER_LONG] |= (1UL << (nr % BITS_PER_LONG));
}
static inline void clear_bit(int nr, unsigned long *addr)
{
    addr[nr / BITS_PER_LONG] &= ~(1UL << (nr % BITS_PER_LONG));
}
static inline int test_bit(int nr, const unsigned long *addr)
{
    return (addr[nr / BITS_PER_LONG] >> (nr % BITS_PER_LONG)) & 1UL;
}
#define __set_bit   set_bit
#define __clear_bit clear_bit

static inline unsigned long
find_next_bit(const unsigned long *addr, unsigned long size, unsigned long start)
{
    unsigned long i;
    for (i = start; i < size; i++)
        if (test_bit((int)i, addr))
            return i;
    return size;
}
#define find_first_bit(addr, size) find_next_bit((addr), (size), 0)

#define for_each_set_bit(bit, addr, size) \
    for ((bit) = find_first_bit((addr), (size)); \
         (bit) < (size); \
         (bit) = find_next_bit((addr), (size), (bit) + 1))

#endif /* _COMPAT_LINUX_BITOPS_H_ */
