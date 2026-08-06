# Warthog halow_mesh_compat — Linux morse_driver softmac/mesh port.
#
# Ports the GPL morse_driver dot11ah + mac80211 mesh sources onto the ESP32
# via a kernel-compat shim (src/compat/) so the MM6108 can run as a softmac
# radio with host-side 802.11s mesh. See PORT_STATUS.md for the live roadmap
# and src/compat/ for the <linux/*> / <net/*> shim headers.
#
# Host unit tests (no hardware) live in test/ — `cd test && make`.
#
# Gated behind WARTHOG_MESH_COMPAT (default off) so the GPL-derived code does
# not enter the default permissive-license STA firmware.

if(NOT CONFIG_WARTHOG_MESH_COMPAT)
    # Empty component so PlatformIO/CMake is happy when the option is off.
    idf_component_register(SRCS "" INCLUDE_DIRS "")
    return()
endif()

# Collect ported sources. As each unit lands (PORT_STATUS.md), it is picked
# up automatically by these globs.
file(GLOB MESH_COMPAT_COMPAT_SRCS   "src/compat/*.c")
file(GLOB MESH_COMPAT_DOT11AH_SRCS  "src/dot11ah/*.c")
file(GLOB MESH_COMPAT_MESH_SRCS     "src/mesh/*.c")
file(GLOB MESH_COMPAT_SHIM_SRCS     "src/shim/*.c")

set(MESH_COMPAT_ALL_SRCS
    ${MESH_COMPAT_COMPAT_SRCS}
    ${MESH_COMPAT_DOT11AH_SRCS}
    ${MESH_COMPAT_MESH_SRCS}
    ${MESH_COMPAT_SHIM_SRCS}
)

idf_component_register(
    SRCS ${MESH_COMPAT_ALL_SRCS}
    # include/ = warthog public API; src/compat = the <linux/*>,<net/*> shim
    # resolution root; src = morse-driver-private header shims (morse.h, ...).
    INCLUDE_DIRS "include" "src/compat"
    PRIV_INCLUDE_DIRS "src" "src/shim" "src/dot11ah" "src/mesh"
    REQUIRES halow freertos esp_common
)

# Vendored dot11ah / mesh code is GPL-2.0+. Kernel headers use GNU extensions
# and kernel-style format specifiers; relax those warnings for this component
# only.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-pedantic
    -Wno-format
    -Wno-unused-function
)
