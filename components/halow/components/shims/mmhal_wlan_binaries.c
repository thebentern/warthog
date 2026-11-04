/*
 * Copyright 2021-2023 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mmhal_wlan.h"
#include "esp_log.h"

static const char* TAG = "MMHaLow";

/*
 * ---------------------------------------------------------------------------------------------
 *                                      BCF Retrieval
 * ---------------------------------------------------------------------------------------------
 */

/*
 * The following implementation reads the BCF File from the config store.
 */

void mmhal_wlan_read_bcf_file(uint32_t offset, uint32_t requested_len, struct mmhal_robuf *robuf)
{
    /* Start/end of the BCF binary image. Embedded by the firmware CMake.
     * Length is derived from the symbols rather than reading the embedded
     * `bcf_binary_length` word, which is not guaranteed to be word-aligned. */
    extern const uint8_t bcf_binary[];
    extern const uint8_t _binary_bcf_binary_end[];
    const uint32_t bcf_binary_length = (uint32_t)(_binary_bcf_binary_end - bcf_binary);

    /* Initialise robuf */
    robuf->buf = NULL;
    robuf->len = 0;
    robuf->free_arg = NULL;
    robuf->free_cb = NULL;

    /* Sanity check */
    if (bcf_binary_length < offset)
    {
        ESP_LOGE(TAG, "Detected an attempt to start reading off the end of the bcf file.");
        return;
    }

    robuf->buf = (uint8_t *)bcf_binary + offset;
    robuf->len = ((size_t)bcf_binary_length) - offset;
    robuf->len = (robuf->len < requested_len) ? robuf->len : requested_len;
}

/*
 * ---------------------------------------------------------------------------------------------
 *                                    Firmware Retrieval
 * ---------------------------------------------------------------------------------------------
 */
/* Start/end of the firmware binary image. Embedded by the firmware CMake.
 * Length is derived from the symbols rather than reading the embedded
 * `firmware_binary_length` word, which is not guaranteed to be word-aligned. */
extern const uint8_t firmware_binary[];
extern const uint8_t _binary_firmware_binary_end[];

void mmhal_wlan_read_fw_file(uint32_t offset, uint32_t requested_len, struct mmhal_robuf *robuf)
{
    const uint32_t firmware_binary_length =
        (uint32_t)(_binary_firmware_binary_end - firmware_binary);
    uint32_t firmware_len = firmware_binary_length;
    if (offset > firmware_binary_length)
    {
        ESP_LOGE(TAG, "Detected an attempt to start read off the end of the firmware file.");
        robuf->buf = NULL;
        return;
    }

    robuf->buf = (uint8_t *)firmware_binary + offset;
    firmware_len -= offset;

    robuf->len = (firmware_len < requested_len) ? firmware_len : requested_len;
}
