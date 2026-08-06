/* SPDX-License-Identifier: GPL-2.0-or-later
 * <linux/slab.h> compat — kmalloc/kfree onto the host/ESP heap. */
#ifndef _COMPAT_LINUX_SLAB_H_
#define _COMPAT_LINUX_SLAB_H_
#include <linux/types.h>
#include <stdlib.h>
#include <string.h>
#define GFP_KERNEL 0
#define GFP_ATOMIC 0
static inline void *kmalloc(size_t n, int f) { (void)f; return malloc(n); }
static inline void *kzalloc(size_t n, int f) { (void)f; return calloc(1, n); }
static inline void *kcalloc(size_t c, size_t n, int f) { (void)f; return calloc(c, n); }
static inline void  kfree(const void *p) { free((void *)p); }
static inline void *kmemdup(const void *src, size_t len, int f)
{ (void)f; void *d = malloc(len); if (d) memcpy(d, src, len); return d; }
#endif
