# warthog: MMOSAL_FILEID for each C source of a target, as mmosal.h's assert records it: the first
# 8 hex digits of the SHA-256 of its path relative to the project root (tools/assert_fileid.py).
function(warthog_mmosal_fileid target)
    # ASSERTING: only sources that assert (pioarduino gives every main/ source its first one's defines).
    cmake_parse_arguments(FID "ASSERTING" "" "" ${ARGN})
    get_target_property(srcs ${target} SOURCES)
    get_target_property(dir ${target} SOURCE_DIR)
    foreach(src IN LISTS srcs)
        if(src MATCHES "\\.c$")
            get_filename_component(abs "${src}" ABSOLUTE BASE_DIR "${dir}")
            if(FID_ASSERTING)
                file(STRINGS "${abs}" hits REGEX "MMOSAL_ASSERT|MMOSAL_LOG_FAILURE_INFO")
                if(NOT hits)
                    continue()
                endif()
            endif()
            file(RELATIVE_PATH rel "${CMAKE_SOURCE_DIR}" "${abs}")
            string(SHA256 hash "${rel}")
            string(SUBSTRING "${hash}" 0 8 id)
            set_property(SOURCE "${abs}" TARGET_DIRECTORY ${target}
                         APPEND PROPERTY COMPILE_DEFINITIONS "MMOSAL_FILEID=0x${id}")
        endif()
    endforeach()
endfunction()
