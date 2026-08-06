/* SPDX-License-Identifier: GPL-2.0-or-later
 * <linux/jiffies.h> compat — monotonic tick counter. In firmware the value is
 * driven from the FreeRTOS tick; in the host test it stays 0 (the dot11ah codec
 * only uses it to stamp a relative beacon timestamp). */
#ifndef _COMPAT_LINUX_JIFFIES_H_
#define _COMPAT_LINUX_JIFFIES_H_
#include <linux/types.h>
#ifndef HZ
#define HZ 100
#endif
extern volatile u64 __compat_jiffies_64;
#define jiffies (__compat_jiffies_64)
static inline u64 get_jiffies_64(void) { return __compat_jiffies_64; }
static inline u64 jiffies_to_usecs(u64 j) { return j * (1000000ULL / HZ); }
static inline u64 jiffies_to_msecs(u64 j) { return j * (1000ULL / HZ); }
static inline u64 usecs_to_jiffies(u64 u) { return u / (1000000ULL / HZ); }
static inline u64 msecs_to_jiffies(u64 m) { return m / (1000ULL / HZ); }

/* Tick comparison macros — signed-difference form handles wraparound. */
#define time_after(a, b)     ((long)((b) - (a)) < 0)
#define time_before(a, b)    time_after(b, a)
#define time_after_eq(a, b)  ((long)((a) - (b)) >= 0)
#define time_before_eq(a, b) time_after_eq(b, a)
#endif
