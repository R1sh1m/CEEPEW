cmake_minimum_required(VERSION 3.16)

# Lock target — prevents toolchain ambiguity
set(IDF_TARGET esp32)

# Keep CMake output focused on warnings and errors.
set(CMAKE_MESSAGE_LOG_LEVEL NOTICE)

# Custom component directories
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/components
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

# This one include does everything:
#   - selects xtensa-esp32-elf-gcc toolchain
#   - calls idf_build_process internally
#   - registers menuconfig, bootloader, flash targets
# DO NOT call idf_build_process() anywhere else.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Keep the build focused on the components this app actually uses.
idf_build_set_property(MINIMAL_BUILD ON)

project(ceepew)

# The ESP-IDF link order places ceepew_oled.a BEFORE ceepew_hal.a in the
# rsp, but hal_ui.c.obj (in ceepew_hal.a) calls ceepew_oled_*. The
# one-pass GNU ld will not re-scan ceepew_oled.a after ceepew_hal.a
# creates the unresolved refs, so the link fails. Wrap the whole rsp
# in --start-group / --end-group by injecting the flags into the rsp
# file in a PRE_LINK step. This makes ld iterate until the unresolved
# set is empty.
if(TARGET ceepew.elf)
    add_custom_command(TARGET ceepew.elf PRE_LINK
        COMMAND ${CMAKE_COMMAND} -E echo
                "Wrapping ceepew.elf.rsp in --start-group / --end-group"
        COMMAND ${CMAKE_COMMAND}
                -DRSP_IN=${CMAKE_BINARY_DIR}/CMakeFiles/ceepew.elf.rsp
                -DRSP_OUT=${CMAKE_BINARY_DIR}/CMakeFiles/ceepew.elf.rsp
                -P "${CMAKE_CURRENT_LIST_DIR}/tools/wrap_rsp_in_group.cmake"
        COMMENT "Injecting --start-group / --end-group into ceepew.elf.rsp"
        VERBATIM
    )
endif()
