/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Force-included ahead of the REAL hostap headers, for host tests that link
 * vendored hostap and shim sources unmodified (test_supp_mesh_conf).
 *
 * The target build gets in_addr/in6_addr and the bswap macros from
 * hostap_morse_common.h. On macOS, hostap's common.h then defines bswap_16/32
 * as inline functions under __APPLE__, which the macros would rename into
 * __builtin_bswap* -- so the macros go. hostap's own netinet/ip.h wants BSD's
 * __packed and __aligned. hostap's linux/types.h shadows the kernel's, so on
 * Linux the libc headers that reach asm/sigcontext.h need __u64/__s64 from
 * here. Not part of any firmware build.
 */
#pragma once

#include "hostap_morse_common.h"

#undef bswap_16
#undef bswap_32
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#ifndef __aligned
#define __aligned(x) __attribute__((aligned(x)))
#endif
#ifdef __linux__
typedef unsigned long long __u64;
typedef long long __s64;
#endif
