/* SPDX-License-Identifier: GPL-2.0-or-later
 * <asm/unaligned.h> compat — unaligned LE/BE accessors. */
#ifndef _COMPAT_ASM_UNALIGNED_H_
#define _COMPAT_ASM_UNALIGNED_H_
#include <linux/types.h>
static inline u16 get_unaligned_le16(const void *p)
{ const u8 *b=p; return (u16)b[0] | ((u16)b[1]<<8); }
static inline u32 get_unaligned_le32(const void *p)
{ const u8 *b=p; return (u32)b[0]|((u32)b[1]<<8)|((u32)b[2]<<16)|((u32)b[3]<<24); }
static inline u64 get_unaligned_le64(const void *p)
{ const u8 *b=p; u64 lo=get_unaligned_le32(b), hi=get_unaligned_le32(b+4); return lo|(hi<<32); }
static inline void put_unaligned_le16(u16 v, void *p)
{ u8 *b=p; b[0]=v&0xff; b[1]=v>>8; }
static inline void put_unaligned_le32(u32 v, void *p)
{ u8 *b=p; b[0]=v&0xff; b[1]=(v>>8)&0xff; b[2]=(v>>16)&0xff; b[3]=(v>>24)&0xff; }
#define get_unaligned(p) (*(p))
#define put_unaligned(v, p) (*(p) = (v))
static inline u16 get_unaligned_be16(const void *p)
{ const u8 *b=p; return ((u16)b[0]<<8) | b[1]; }
static inline u32 get_unaligned_be32(const void *p)
{ const u8 *b=p; return ((u32)b[0]<<24)|((u32)b[1]<<16)|((u32)b[2]<<8)|b[3]; }
#endif
