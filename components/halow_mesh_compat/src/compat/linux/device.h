/* SPDX-License-Identifier: GPL-2.0-or-later
 * <linux/device.h> compat — dot11ah only uses dev_* for logging; stub. */
#ifndef _COMPAT_LINUX_DEVICE_H_
#define _COMPAT_LINUX_DEVICE_H_
#include <linux/types.h>
#include <linux/printk.h>   /* dev_* logging + struct va_format for debug.c */
struct device { int unused; };
#define dev_err(dev, ...)   do { (void)(dev); } while (0)
#define dev_warn(dev, ...)  do { (void)(dev); } while (0)
#define dev_info(dev, ...)  do { (void)(dev); } while (0)
#define dev_dbg(dev, ...)   do { (void)(dev); } while (0)
#endif
