# Copyright 2025 Robert Carey
# Copyright 2025-2026 Morse Micro
# SPDX-License-Identifier: Apache-2.0

set(src "mmhalow.c")

# main/ for the shims' warthog_shim.h; the region is AT+REGION, decided in main/region.c.
set(inc "." "${CMAKE_SOURCE_DIR}/main")

idf_component_register(
	INCLUDE_DIRS
	${inc}
	SRCS
	${src}
	REQUIRES
	firmware
	esp_netif
	esp_wifi
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
endif()

add_subdirectory(components)
add_dependencies(${COMPONENT_TARGET} mmpktmem shims mmutils morselib regdb)
target_link_libraries(${COMPONENT_TARGET} PUBLIC mmpktmem shims mmutils morselib regdb)

# warthog: mmhalow.c's MMOSAL_ASSERT records name their file (AT+ASSERT?).
include(${CMAKE_CURRENT_SOURCE_DIR}/components/mmosal_fileid.cmake)
warthog_mmosal_fileid(${COMPONENT_LIB})
