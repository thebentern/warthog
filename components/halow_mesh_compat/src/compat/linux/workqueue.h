/* SPDX-License-Identifier: GPL-2.0-or-later
 * <linux/workqueue.h> compat — minimal. The dot11ah/pv1 structs embed
 * work_struct members; the dot11ah codec never schedules them (that lives in
 * the driver core), so an opaque-but-complete type is all that is needed. */
#ifndef _COMPAT_LINUX_WORKQUEUE_H_
#define _COMPAT_LINUX_WORKQUEUE_H_
#include <linux/types.h>
struct work_struct { void (*func)(struct work_struct *); };
struct delayed_work { struct work_struct work; };
typedef void (*work_func_t)(struct work_struct *);
#define INIT_WORK(w, f)         do { (w)->func = (f); } while (0)
#define INIT_DELAYED_WORK(w, f) do { (w)->work.func = (f); } while (0)
static inline bool schedule_work(struct work_struct *w) { (void)w; return true; }
static inline bool cancel_work_sync(struct work_struct *w) { (void)w; return false; }
#endif
