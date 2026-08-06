/* SPDX-License-Identifier: GPL-2.0-or-later
 * <asm/byteorder.h> compat — little-endian host (ESP32-S3 + MM6108). */
#ifndef _COMPAT_ASM_BYTEORDER_H_
#define _COMPAT_ASM_BYTEORDER_H_
#include <linux/types.h>
#ifndef cpu_to_le16
#define cpu_to_le16(x) ((__le16)(u16)(x))
#define le16_to_cpu(x) ((u16)(__le16)(x))
#define cpu_to_le32(x) ((__le32)(u32)(x))
#define le32_to_cpu(x) ((u32)(__le32)(x))
#define cpu_to_le64(x) ((__le64)(u64)(x))
#define le64_to_cpu(x) ((u64)(__le64)(x))
#endif
#ifndef cpu_to_be16
#define cpu_to_be16(x) ((__be16)__builtin_bswap16((u16)(x)))
#define be16_to_cpu(x) ((u16)__builtin_bswap16((u16)(__be16)(x)))
#define cpu_to_be32(x) ((__be32)__builtin_bswap32((u32)(x)))
#define be32_to_cpu(x) ((u32)__builtin_bswap32((u32)(__be32)(x)))
#endif
#define le16_to_cpup(p) le16_to_cpu(*(const __le16 *)(p))
#define le32_to_cpup(p) le32_to_cpu(*(const __le32 *)(p))
#endif
