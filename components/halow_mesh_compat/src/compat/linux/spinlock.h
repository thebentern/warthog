/* SPDX-License-Identifier: GPL-2.0-or-later
 * <linux/spinlock.h> compat — single-threaded-from-the-port's-view locking.
 * The mesh port runs on one FreeRTOS task; locks are no-ops at host-test time
 * and become FreeRTOS mutexes in the firmware build (see shim). */
#ifndef _COMPAT_LINUX_SPINLOCK_H_
#define _COMPAT_LINUX_SPINLOCK_H_
typedef struct { int x; } spinlock_t;
static inline void spin_lock_init(spinlock_t *l) { (void)l; }
static inline void spin_lock(spinlock_t *l) { (void)l; }
static inline void spin_unlock(spinlock_t *l) { (void)l; }
static inline void spin_lock_bh(spinlock_t *l) { (void)l; }
static inline void spin_unlock_bh(spinlock_t *l) { (void)l; }
#endif
