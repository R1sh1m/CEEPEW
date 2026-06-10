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

# Link order: ceepew_oled.a is scanned before ceepew_hal.a because
# ceepew_hal REQUIRES ceepew_oled. ceepew_oled.a defines ceepew_oled_*
# symbols but no one in the prior link set has unresolved refs to
# them, so the one-pass GNU ld discards ceepew_oled.a as "unused."
# Then ceepew_hal.a (hal_ui.c.obj) creates new unresolved refs to
# ceepew_oled_*, but ld does not re-scan ceepew_oled.a to resolve
# them, and the link fails.
#
# This is purely a link-order issue, NOT a CMake cycle. The component
# dep graph is a DAG (verified with `idf.py dependencies` after this
# fix). Wrap the entire rsp in --start-group / --end-group so ld
# iterates until the unresolved set is empty.
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
