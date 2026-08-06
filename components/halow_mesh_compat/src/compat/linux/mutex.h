/* SPDX-License-Identifier: GPL-2.0-or-later
 * <linux/mutex.h> compat — no-op locks (port runs on one FreeRTOS task). */
#ifndef _COMPAT_LINUX_MUTEX_H_
#define _COMPAT_LINUX_MUTEX_H_
struct mutex { int placeholder; };  /* not '__unused' — that's a libc macro */
static inline void mutex_init(struct mutex *m) { (void)m; }
static inline void mutex_lock(struct mutex *m) { (void)m; }
static inline void mutex_unlock(struct mutex *m) { (void)m; }
static inline void mutex_destroy(struct mutex *m) { (void)m; }
#endif
