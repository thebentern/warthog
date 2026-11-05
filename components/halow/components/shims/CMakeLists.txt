# Copyright 2024-2026 Morse Micro
# SPDX-License-Identifier: Apache-2.0

set(src "mmosal_shim_freertos_esp32.c"
	"mmhal_core.c"
	"mmhal_os.c"
	"mmhal_wlan_binaries.c"
	"mmhal_wlan.c"
	"crypto_mbedtls_mm.c"
)

idf_build_get_property(ARCH IDF_TARGET_ARCH)
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/${ARCH}")
    message(WARNING "Architecture ${ARCH} missing morselib debug macro mappings. Stubbing with noops.")
    set(inc "include/unknown" ".")
else()
    set(inc "include/${ARCH}" ".")
endif()

foreach(pin MM_RESET_N MM_SPI_SCK MM_SPI_MOSI MM_SPI_MISO MM_SPI_CS)
    if(CONFIG_${pin} LESS 0)
        list(APPEND _mm_unset_pins ${pin})
    endif()
endforeach()
if(_mm_unset_pins)
    string(REPLACE ";" " " _mm_unset_pins_str "${_mm_unset_pins}")
    # FATAL_ERROR here blocks set-target and menuconfig
    add_custom_target(mm_pin_config_error ALL
        COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --red --bold
            "ERROR: the following MM pins must be set to a valid pin number: ${_mm_unset_pins_str}. Set them via menuconfig or use a sdkconfig.defaults found in managed_components/morsemicro__halow/configs"
        COMMAND ${CMAKE_COMMAND} -E false
        VERBATIM
    )
endif()

# Check if only MM_WAKE is defined OR only MM_BUSY is defined
if((NOT (CONFIG_MM_WAKE LESS 0) AND CONFIG_MM_BUSY LESS 0) OR (CONFIG_MM_WAKE LESS 0 AND NOT (CONFIG_MM_BUSY LESS 0)))
    add_custom_target(_mm_ps_pins_error ALL
        COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --red --bold
            "ERROR: MM_WAKE and MM_BUSY pins need to either both be defined and routed for power saving, or both need to be disabled (-1 defaut definition) to mark this as intentional and forgoe power saving"
        COMMAND ${CMAKE_COMMAND} -E false
        VERBATIM
    )
endif()

add_library(shims STATIC ${src})
target_include_directories(shims PUBLIC ${inc})

target_link_libraries(shims PUBLIC
	idf::log
	idf::driver
	idf::mbedtls
	idf::esp_timer
)

target_link_libraries(shims PUBLIC
	morsefirmware
	morselib
	mmutils
)

add_dependencies(shims morselib morsefirmware)

