/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/* The first megabyte of the ESP32-S3R8's 8 MB PSRAM as a heap of its own, for plain data only tasks
 * touch: never an interrupt handler's, a buffer handed to SPI, DMA or mbedTLS's hardware AES and SHA, a
 * task stack or a FreeRTOS object. IDF maps the chip (CONFIG_SPIRAM_USE_MEMMAP) and allocates nothing
 * from it; only warthog_ext_malloc() does. */

enum warthog_ext_ram {
    WARTHOG_EXT_RAM_OFF,  /* not asked for: safe mode, or AT+EXTRAM=0 */
    WARTHOG_EXT_RAM_NONE, /* no heap: not built in, chip not found, not mapped as expected, or not registered */
    WARTHOG_EXT_RAM_OK,
    WARTHOG_EXT_RAM_BAD,  /* failed the boot test: never used */
};

/* app_main, once, before the first warthog_ext_malloc(): tests that megabyte and makes it a heap. */
enum warthog_ext_ram warthog_ext_ram_init(void);
enum warthog_ext_ram warthog_ext_ram_state(void);
/* How long that test took, in ms; 0 when it did not run. AT+EXTRAM? prints it. */
uint32_t warthog_ext_ram_test_ms(void);

/* PSRAM while it is OK and has room, else internal RAM as malloc() gives it; NULL when neither has the
 * room. free() releases it. Any task; not an interrupt handler. */
void *warthog_ext_malloc(size_t n);
