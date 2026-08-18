/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _COMPAT_LINUX_ERRNO_H_
#define _COMPAT_LINUX_ERRNO_H_

/* This header shadows the kernel's <linux/errno.h>, and on Linux that creates
 * a loop worth understanding before touching it.
 *
 * glibc reaches the E* values through <errno.h> -> <bits/errno.h>, and
 * <bits/errno.h> itself does #include <linux/errno.h>. Our -I path resolves
 * that back to THIS file, whose include guard is already set, so the nested
 * visit expands to nothing and the real definitions never load. The symptom is
 * every E* constant undeclared in files that clearly included errno -- which
 * is exactly how it presented in CI while compiling fine on macOS, where no
 * linux/ headers exist and <errno.h> is reached directly.
 *
 * So: on Linux hand straight through to the system header; elsewhere the plain
 * C one is both correct and all that exists.
 */
#if defined(__linux__)
#include_next <linux/errno.h>
#else
#include <errno.h>
#endif

#endif /* _COMPAT_LINUX_ERRNO_H_ */
