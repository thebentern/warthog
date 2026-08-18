/* SPDX-License-Identifier: GPL-2.0-or-later
 * <linux/string.h> compat — forward to libc <string.h> plus the kernel-only
 * helpers the dot11ah sources use. */
#ifndef _COMPAT_LINUX_STRING_H_
#define _COMPAT_LINUX_STRING_H_

/* ssize_t. Reached transitively on macOS but not through glibc's headers, so
 * without this the compat layer compiles on a developer laptop and fails on a
 * Linux CI runner. */
#include <sys/types.h>
#include <string.h>
#include <linux/types.h>

/* strscpy — bounded string copy that always NUL-terminates. Returns the number
 * of bytes copied (excluding NUL), or -E2BIG on truncation. */
static inline ssize_t strscpy(char *dst, const char *src, size_t size)
{
    size_t len;
    if (size == 0)
        return -7 /* -E2BIG */;
    len = strlen(src);
    if (len >= size) {
        memcpy(dst, src, size - 1);
        dst[size - 1] = '\0';
        return -7 /* -E2BIG */;
    }
    memcpy(dst, src, len + 1);
    return (ssize_t)len;
}
#endif
