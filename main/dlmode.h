/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/** How long a board waits in ROM download mode before an RTC reset returns it to the app; esptool does not
 *  stop this watchdog on the USB-OTG port, so a session must end in a reset before then. */
#ifndef WARTHOG_DLMODE_BACK_S
#define WARTHOG_DLMODE_BACK_S 1800u
#endif

/** ROM download mode for AT+DLMODE and the devloop CDC triggers: the HaLow chip reset (it loses its
 *  firmware), an RTC watchdog armed for WARTHOG_DLMODE_BACK_S, the ROM's USB reset exemption cleared, off
 *  the bus for 100 ms, then a core reset with FORCE_DOWNLOAD_BOOT. */
void warthog_enter_download(void) __attribute__((noreturn));
