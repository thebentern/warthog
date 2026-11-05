/*
 * Copyright 2026 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#define MMPORT_BREAKPOINT() asm("break 1, 15")
#define MMPORT_GET_LR()     (__builtin_return_address(0))
#define MMPORT_GET_PC(_a)                                        \
    do {                                                         \
        __label__ _mmport_here;                                  \
    _mmport_here:                                                \
        (_a) = (__typeof__(_a))(__UINTPTR_TYPE__)&&_mmport_here; \
    } while (0)
#define MMPORT_MEM_SYNC()   __sync_synchronize()
