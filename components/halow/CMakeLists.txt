# Copyright 2025 Robert Carey
# Copyright 2025-2026 Morse Micro
# SPDX-License-Identifier: Apache-2.0

set(src "mmhalow.c")

# main/ for the shims' warthog_shim.h; the region is AT+REGION, decided in main/region.c.
set(inc "." "${CMAKE_SOURCE_DIR}/main")

set(reqs
	esp_netif
	esp_wifi
)

idf_component_register(
	INCLUDE_DIRS
	${inc}
	SRCS
	${src}
	REQUIRES
	${reqs}
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(NOT "${CONFIG_WARTHOG_HW_REGIONS}" MATCHES "^[A-Z][A-Z](,[A-Z][A-Z])*$" OR
       NOT ",${CONFIG_WARTHOG_HW_REGIONS}," MATCHES ",US,")
        message(FATAL_ERROR "warthog: CONFIG_WARTHOG_HW_REGIONS must be a comma list of two-letter codes "
                            "including US (the no-region default), not '${CONFIG_WARTHOG_HW_REGIONS}'")
    endif()
    if(NOT CONFIG_WARTHOG_HW_BAND_LO_KHZ LESS CONFIG_WARTHOG_HW_BAND_HI_KHZ)
        message(FATAL_ERROR "warthog: CONFIG_WARTHOG_HW_BAND_LO_KHZ must be below CONFIG_WARTHOG_HW_BAND_HI_KHZ")
    endif()
    # PIO never runs the firmware/shims error targets (add_custom_target ALL ... false), so check here.
    if(NOT CONFIG_HALOW_AP_MODE)
        message(FATAL_ERROR "warthog: CONFIG_HALOW_AP_MODE must be set; the 802.11s mesh needs hostapd's AP code and driver_ap.c")
    endif()
    if(NOT (CONFIG_MM_CHIP_MM6108 OR CONFIG_MM_CHIP_MM8108))
        message(FATAL_ERROR "warthog: set CONFIG_MM_CHIP_MM6108 or CONFIG_MM_CHIP_MM8108")
    endif()
    if("${CONFIG_MM_BCF_FILE}" STREQUAL "")
        message(FATAL_ERROR "warthog: CONFIG_MM_BCF_FILE must name the module's BCF")
    endif()
    foreach(pin MM_RESET_N MM_WAKE MM_BUSY MM_SPI_SCK MM_SPI_MISO MM_SPI_MOSI MM_SPI_CS MM_SPI_IRQ)
        if(NOT DEFINED CONFIG_${pin} OR CONFIG_${pin} LESS 0)
            message(FATAL_ERROR "warthog: CONFIG_${pin} must be a GPIO number, not '${CONFIG_${pin}}'")
        endif()
    endforeach()
endif()

add_subdirectory(components)
add_dependencies(${COMPONENT_TARGET} mmpktmem shims mmutils morselib regdb)
target_link_libraries(${COMPONENT_TARGET} PUBLIC mmpktmem shims mmutils morselib regdb)

# warthog: mmhalow.c's MMOSAL_ASSERT records name their file (AT+ASSERT?).
include(${CMAKE_CURRENT_SOURCE_DIR}/components/mmosal_fileid.cmake)
warthog_mmosal_fileid(${COMPONENT_LIB})
