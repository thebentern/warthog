/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ext_ram.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "warthog.ext_ram";

static enum warthog_ext_ram s_state = WARTHOG_EXT_RAM_OFF;
static uint32_t s_test_ms;

#if CONFIG_SPIRAM
#include "esp_heap_caps_init.h"
#include "esp_mmu_map.h"
#include "esp_psram.h"

/* What is tested and made a heap. The firmware's use peaks near 150 KB: batman's 52 KB, 16 deliveries
 * and 48 USB frame copies of 1.5 KB. */
#define EXT_HEAP_BYTES (1024u * 1024u)

static uint32_t ext_word_(size_t i, uint32_t flip)
{
    return ((uint32_t)i * 2654435761u + 0x5aa5c33cu) ^ flip;
}

/* Every word written with a value made from its index and all of it read back, then the same with
 * every bit inverted. The megabyte is 32 times the data cache, so each line is written out to the chip
 * and read back from it. Returns the word checks that failed (each word is checked twice). */
static size_t ext_test_(uint8_t *p, size_t n)
{
    volatile uint32_t *w = (volatile uint32_t *)p;
    const size_t words = n / sizeof(uint32_t);
    size_t bad = 0;
    for (int pass = 0; pass < 2; pass++) {
        const uint32_t flip = pass ? UINT32_MAX : 0u;
        for (size_t i = 0; i < words; i++) {
            w[i] = ext_word_(i, flip);
        }
        for (size_t i = 0; i < words; i++) {
            bad += w[i] != ext_word_(i, flip);
        }
    }
    return bad;
}

/* Where IDF mapped the PSRAM's first megabyte, read from the MMU table (esp_psram.c keeps its own
 * record private). Every page is checked before a byte of it is touched: an unmapped one would be a
 * bus error. */
static bool ext_range_(uint8_t **start, size_t *size)
{
    void *v = NULL;
    size_t n = esp_psram_is_initialized() ? esp_psram_get_size() : 0;
    if (n > EXT_HEAP_BYTES) {
        n = EXT_HEAP_BYTES;
    }
    n -= n % CONFIG_MMU_PAGE_SIZE;
    if (n == 0 || esp_mmu_paddr_to_vaddr(0, MMU_TARGET_PSRAM0, MMU_VADDR_DATA, &v) != ESP_OK || v == NULL) {
        return false;
    }
    for (size_t off = 0; off < n; off += CONFIG_MMU_PAGE_SIZE) {
        esp_paddr_t pa = 0;
        mmu_target_t tgt = MMU_TARGET_FLASH0;
        if (esp_mmu_vaddr_to_paddr((uint8_t *)v + off, &pa, &tgt) != ESP_OK || tgt != MMU_TARGET_PSRAM0 ||
            pa != (esp_paddr_t)off) {
            return false;
        }
    }
    *start = v;
    *size = n;
    return true;
}
#endif

enum warthog_ext_ram warthog_ext_ram_init(void)
{
    if (s_state != WARTHOG_EXT_RAM_OFF) {
        return s_state; /* once */
    }
    s_state = WARTHOG_EXT_RAM_NONE;
#if CONFIG_SPIRAM
    uint8_t *p = NULL;
    size_t n = 0;
    if (!ext_range_(&p, &n)) {
        ESP_LOGW(TAG, "no PSRAM (IDF did not find the chip, or its mapping is not where expected): internal RAM only");
        return s_state;
    }
    const uint32_t t0 = esp_log_timestamp();
    const size_t bad = ext_test_(p, n);
    s_test_ms = esp_log_timestamp() - t0;
    if (bad != 0) {
        s_state = WARTHOG_EXT_RAM_BAD;
        ESP_LOGE(TAG, "PSRAM failed its test (%u of %u word checks, %u ms): not used, internal RAM only",
                 (unsigned)bad, (unsigned)(n / sizeof(uint32_t) * 2u), (unsigned)s_test_ms);
        return s_state;
    }
    /* A heap whose only capability is MALLOC_CAP_SPIRAM: no malloc, pvPortMalloc, MALLOC_CAP_DEFAULT,
     * _8BIT or _INTERNAL request of IDF's, the Wi-Fi driver's or the HaLow stack's can land in it. */
    static const uint32_t caps[] = { MALLOC_CAP_SPIRAM, 0, 0 };
    const esp_err_t e = heap_caps_add_region_with_caps(caps, (intptr_t)p, (intptr_t)(p + n));
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "PSRAM heap not registered (%s): internal RAM only", esp_err_to_name(e));
        return s_state;
    }
    s_state = WARTHOG_EXT_RAM_OK;
    ESP_LOGI(TAG, "PSRAM ok: %u KB heap at %p of a %u KB chip, tested in %u ms", (unsigned)(n / 1024u), (void *)p,
             (unsigned)(esp_psram_get_size() / 1024u), (unsigned)s_test_ms);
#else
    ESP_LOGW(TAG, "built without PSRAM: internal RAM only");
#endif
    return s_state;
}

enum warthog_ext_ram warthog_ext_ram_state(void)
{
    return s_state;
}

uint32_t warthog_ext_ram_test_ms(void)
{
    return s_test_ms;
}

void *warthog_ext_malloc(size_t n)
{
    if (s_state == WARTHOG_EXT_RAM_OK) {
        return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return malloc(n); /* as before the PSRAM was used */
}
