/* SPDX-License-Identifier: GPL-2.0-or-later
 * <linux/timer.h> compat — the morse mesh probe timer. In firmware these map
 * to FreeRTOS timers; the host build keeps them inert (callbacks fire only
 * when explicitly driven by a test). */
#ifndef _COMPAT_LINUX_TIMER_H_
#define _COMPAT_LINUX_TIMER_H_
#include <linux/types.h>
#include <linux/kernel.h>
struct timer_list {
    unsigned long expires;
    void (*function)(struct timer_list *);
    unsigned long data;
};
#define timer_setup(t, fn, flags) do { (t)->function = (fn); } while (0)
#define from_timer(var, t, field) container_of(t, typeof(*(var)), field)
#define timer_container_of(var, t, field) from_timer(var, t, field)
static inline void mod_timer(struct timer_list *t, unsigned long e) { if (t) t->expires = e; }
static inline void del_timer(struct timer_list *t) { (void)t; }
static inline void del_timer_sync(struct timer_list *t) { (void)t; }
static inline void timer_delete_sync(struct timer_list *t) { (void)t; }
static inline int  timer_pending(const struct timer_list *t) { (void)t; return 0; }
static inline void add_timer(struct timer_list *t) { (void)t; }
#endif
