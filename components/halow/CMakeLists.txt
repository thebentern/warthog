# Copyright 2025 Robert Carey
# Copyright 2025-2026 Morse Micro
# SPDX-License-Identifier: Apache-2.0

set(src "mmhalow.c")

# main/region.h is the single source of truth for the build's regulatory
# domain. Without it here, mmhalow.c falls back to CONFIG_HALOW_COUNTRY_CODE
# and every non-US build comes up on the US channel list.
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

add_subdirectory(components)
add_dependencies(${COMPONENT_TARGET} mmpktmem shims mmutils morselib regdb)
target_link_libraries(${COMPONENT_TARGET} PUBLIC mmpktmem shims mmutils morselib regdb)
