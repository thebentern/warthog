/* Host arch for IDF's lwIP in test_lwip_napt_frag (lwip_napt.mk). */
#pragma once
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#define LWIP_ERRNO_STDINCLUDE 1
/* libc may define htons and friends as macros (macOS does); lwIP uses lwip_htons itself. */
#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS
#define LWIP_PLATFORM_DIAG(x) do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { printf("lwIP assert: %s\n", x); abort(); } while (0)
#define LWIP_RAND() ((u32_t)rand())
typedef int sys_prot_t;
