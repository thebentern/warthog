/* SPDX-License-Identifier: GPL-2.0-or-later
 * <linux/version.h> compat — pin to a recent kernel so the morse_driver's
 * KERNEL_VERSION() conditionals select the modern code paths. */
#ifndef _COMPAT_LINUX_VERSION_H_
#define _COMPAT_LINUX_VERSION_H_
#define KERNEL_VERSION(a,b,c) (((a) << 16) + ((b) << 8) + ((c) > 255 ? 255 : (c)))
#define LINUX_VERSION_CODE KERNEL_VERSION(6, 6, 0)
#ifndef MAC80211_VERSION_CODE
#define MAC80211_VERSION_CODE LINUX_VERSION_CODE
#endif
#endif
