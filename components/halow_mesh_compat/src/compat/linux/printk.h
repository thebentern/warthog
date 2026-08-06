/* SPDX-License-Identifier: GPL-2.0-or-later
 * <linux/printk.h> compat — pr_* and hex-dump become no-ops; the dot11ah codec
 * routes real logging through dot11ah/debug.h. */
#ifndef _COMPAT_LINUX_PRINTK_H_
#define _COMPAT_LINUX_PRINTK_H_
#include <stdarg.h>

#define pr_emerg(...)   do { } while (0)
#define pr_alert(...)   do { } while (0)
#define pr_crit(...)    do { } while (0)
#define pr_err(...)     do { } while (0)
#define pr_warn(...)    do { } while (0)
#define pr_warning(...) do { } while (0)
#define pr_notice(...)  do { } while (0)
#define pr_info(...)    do { } while (0)
#define pr_debug(...)   do { } while (0)
#define pr_cont(...)    do { } while (0)
#define pr_warn_ratelimited(...)  do { } while (0)
#define pr_err_ratelimited(...)   do { } while (0)
#define pr_info_ratelimited(...)  do { } while (0)
enum { DUMP_PREFIX_NONE, DUMP_PREFIX_ADDRESS, DUMP_PREFIX_OFFSET };
#define print_hex_dump(...)        do { } while (0)
#define print_hex_dump_bytes(...)  do { } while (0)
#define print_hex_dump_debug(...)  do { } while (0)

/* struct va_format — kernel wrapper letting "%pV" recursively format a
 * (fmt, va_list) pair. The dot11ah debug.c builds one per log call. */
struct va_format {
    const char *fmt;
    va_list *va;
};
#endif
