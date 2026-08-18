/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Minimal stand-in for hostap's includes.h, for the HOST test build only.
 *
 * hostap's real headers pull in hostap_morse_common.h, which redefines
 * in_addr/in6_addr/inet_ntop for a bare-metal target and collides head-on with
 * macOS's system headers. The crypto under test needs almost none of that --
 * only fixed-width types and four os_* wrappers -- so this shadows it and lets
 * the vendored AES-CCM be tested on a development machine.
 *
 * Not part of any firmware build; on target the real hostap headers are used.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;

#define os_malloc(n) malloc(n)
#define os_free(p) free(p)
#define os_memcpy(d, s, n) memcpy((d), (s), (n))
#define os_memset(d, c, n) memset((d), (c), (n))
#define os_memcmp(a, b, n) memcmp((a), (b), (n))

#include <stdbool.h>

/* hostap's logging, stubbed out. The crypto only hexdumps at MSG_EXCESSIVE. */
enum { MSG_EXCESSIVE, MSG_MSGDUMP, MSG_DEBUG, MSG_INFO, MSG_WARNING, MSG_ERROR };
static inline void wpa_printf(int level, const char *fmt, ...) { (void)level; (void)fmt; }
static inline void wpa_hexdump_key(int level, const char *t, const void *b, size_t l)
{ (void)level; (void)t; (void)b; (void)l; }
static inline void wpa_hexdump(int level, const char *t, const void *b, size_t l)
{ (void)level; (void)t; (void)b; (void)l; }

/* Constant-time compare. hostap uses this for MIC verification specifically so
 * a mismatch cannot be timed; keep that property in the test build too. */
static inline int os_memcmp_const(const void *a, const void *b, size_t len)
{
    const u8 *aa = (const u8 *)a, *bb = (const u8 *)b;
    u8 diff = 0;
    for (size_t i = 0; i < len; i++) diff |= (u8)(aa[i] ^ bb[i]);
    return diff;
}
