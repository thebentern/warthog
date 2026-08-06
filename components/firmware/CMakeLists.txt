# Local `firmware` IDF component: morsemicro/halow's shims reference
# `idf::firmware`, but the registry publishes the blobs under the prefixed
# name `morsemicro/firmware`. This shim sits in between, sourcing the .mbin
# files from the registry component and exposing them under the bare name.

idf_component_register()

set(MM_FW_SRC_DIR "${CMAKE_SOURCE_DIR}/managed_components/morsemicro__firmware")

if(${IDF_TARGET} STREQUAL "esp32s3")
    set(BFDNAME "elf32-xtensa-le")
    set(ARCH "xtensa")
elseif(${IDF_TARGET} STREQUAL "esp32c6" OR ${IDF_TARGET} STREQUAL "esp32c3")
    set(BFDNAME "elf32-littleriscv")
    set(ARCH "riscv")
else()
    message(FATAL_ERROR "warthog: target ${IDF_TARGET} not supported for HaLow")
endif()

if(CONFIG_MMHAL_CHIP_TYPE_MM6108)
    set(MMIOT_BLOB_DIR "${MM_FW_SRC_DIR}/mm6108")
elseif(CONFIG_MMHAL_CHIP_TYPE_MM8108)
    set(MMIOT_BLOB_DIR "${MM_FW_SRC_DIR}/mm8108")
else()
    message(FATAL_ERROR "warthog: no MMHAL_CHIP_TYPE selected — set CONFIG_MMHAL_CHIP_TYPE_MM6108 in sdkconfig.defaults")
endif()

find_file(BCF_MBIN NAMES ${CONFIG_MM_BCF_FILE} PATHS ${MMIOT_BLOB_DIR} NO_DEFAULT_PATH)
find_file(FW_MBIN  NAMES ${CONFIG_MM_FW_FILE}  PATHS ${MMIOT_BLOB_DIR} NO_DEFAULT_PATH)

if(NOT BCF_MBIN)
    message(FATAL_ERROR "warthog: BCF '${CONFIG_MM_BCF_FILE}' not found in ${MMIOT_BLOB_DIR}")
endif()
if(NOT FW_MBIN)
    message(FATAL_ERROR "warthog: firmware '${CONFIG_MM_FW_FILE}' not found in ${MMIOT_BLOB_DIR}")
endif()
message(STATUS "warthog firmware: BCF=${BCF_MBIN}")
message(STATUS "warthog firmware: FW =${FW_MBIN}")

function(make_binary_object __file __prefix __out_obj)
    get_filename_component(__file_name ${__file} NAME)
    set(__object "${CMAKE_CURRENT_BINARY_DIR}/${__file_name}.o")
    string(MAKE_C_IDENTIFIER "${__file}" __file_c_identifier)
    add_custom_command(
        OUTPUT ${__object}
        COMMAND
            ${CMAKE_OBJCOPY} -I binary -O ${BFDNAME} -B ${ARCH} ${__file} ${__object}
            --redefine-sym _binary_${__file_c_identifier}_start=${__prefix}_start
            --redefine-sym _binary_${__file_c_identifier}_size=${__prefix}_size
            --redefine-sym _binary_${__file_c_identifier}_end=${__prefix}_end
            --rename-section .data=.rodata._fw_mbin,contents,alloc,load,readonly,data
        DEPENDS ${__file}
        COMMENT "warthog objcopy ${__file_name} → ${__prefix}"
    )
    set(${__out_obj} "${__object}" PARENT_SCOPE)
endfunction()

make_binary_object(${FW_MBIN}  firmware_binary FW_OBJ_PATH)
make_binary_object(${BCF_MBIN} bcf_binary      BCF_OBJ_PATH)

add_custom_target(warthog_morsefirmware ALL DEPENDS "${FW_OBJ_PATH}" "${BCF_OBJ_PATH}")
add_dependencies(${COMPONENT_TARGET} warthog_morsefirmware)
target_link_libraries(${COMPONENT_TARGET} INTERFACE "${FW_OBJ_PATH}" "${BCF_OBJ_PATH}")
