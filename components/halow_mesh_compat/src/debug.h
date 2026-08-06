/* SPDX-License-Identifier: GPL-2.0-or-later
 * "../debug.h" shim — morse_driver logging macros routed to no-ops for the
 * host build (ESP build will route to ESP_LOG). dot11ah only logs. */
#ifndef _COMPAT_MORSE_DEBUG_H_
#define _COMPAT_MORSE_DEBUG_H_
#include <linux/types.h>
#define MORSE_DBG(m, ...)   do {} while (0)
#define MORSE_INFO(m, ...)  do {} while (0)
#define MORSE_WARN(m, ...)  do {} while (0)
#define MORSE_ERR(m, ...)   do {} while (0)
#define dot11ah_info(...)   do {} while (0)
#define dot11ah_err(...)    do {} while (0)
#define dot11ah_debug(...)  do {} while (0)
#define dot11ah_warn(...)   do {} while (0)
#define MORSE_WARN_ON(f, c) ((c) ? 1 : 0)
#define MORSE_OUI 0x0CBF74
#endif
