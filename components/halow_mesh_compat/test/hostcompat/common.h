/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Minimal stand-in for hostap's common.h, HOST test build only. See includes.h
 * for why. Only the accessors the vendored AES sources actually reference.
 */
#pragma once

#include "includes.h"

#define WPA_GET_BE16(a) ((u16)(((a)[0] << 8) | (a)[1]))
#define WPA_PUT_BE16(a, val)                    \
    do {                                        \
        (a)[0] = (u8)(((u16)(val)) >> 8);       \
        (a)[1] = (u8)(((u16)(val)) & 0xff);     \
    } while (0)

#define WPA_GET_BE32(a) ((((u32)(a)[0]) << 24) | (((u32)(a)[1]) << 16) | \
                         (((u32)(a)[2]) << 8) | ((u32)(a)[3]))
#define WPA_PUT_BE32(a, val)                            \
    do {                                                \
        (a)[0] = (u8)((((u32)(val)) >> 24) & 0xff);     \
        (a)[1] = (u8)((((u32)(val)) >> 16) & 0xff);     \
        (a)[2] = (u8)((((u32)(val)) >> 8) & 0xff);      \
        (a)[3] = (u8)(((u32)(val)) & 0xff);             \
    } while (0)

/* hostap's fault-injection hook, compiled out in normal builds. */
#ifndef TEST_FAIL
#define TEST_FAIL() 0
#endif

/* hostap annotates return values it wants checked; the real definition lives
 * in a header we deliberately do not pull in here. */
#ifndef __must_check
#define __must_check
#endif
