cmake_minimum_required(VERSION 3.16)

# Lock target — prevents toolchain ambiguity
set(IDF_TARGET esp32)

# Keep CMake output focused on warnings and errors.
set(CMAKE_MESSAGE_LOG_LEVEL NOTICE)

# GCC 15.2.0 toolchain requires explicit dynconfig to load Xtensa CPU
# config (rer opcode, MUL16, etc.). Without this the assembler errors
# on xtensa-specific instructions and cmake C/C++ compiler detection
# fails because the Rust wrapper in bin/ (which we bypass) normally
# passes this flag.  The flag tells GCC which xtensa .so to load.
# Use the short name — cc1.exe resolves it from its own lib directory.
set(_DYNCONFIG "xtensa_esp32.so")
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -mdynconfig=${_DYNCONFIG} -std=gnu11")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mdynconfig=${_DYNCONFIG} -std=gnu++17")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -mdynconfig=${_DYNCONFIG}")
add_compile_options(-Wno-error=missing-field-initializers)

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
if(NOT DEFINED ENV{IDF_PATH} OR "$ENV{IDF_PATH}" STREQUAL "")
    set(ENV{IDF_PATH} "C:/esp/v6.0.2/esp-idf")
    set(IDF_PATH "C:/esp/v6.0.2/esp-idf")
else()
    set(IDF_PATH "$ENV{IDF_PATH}")
endif()
include(${IDF_PATH}/tools/cmake/project.cmake)

# Keep the build focused on the components this app actually uses.
idf_build_set_property(MINIMAL_BUILD ON)

project(ceepew)

# Inject git hash as a compile definition for firmware versioning
execute_process(
    COMMAND git describe --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_HASH)
    set(GIT_HASH "unknown")
endif()
add_compile_definitions(CEEPEW_GIT_HASH="${GIT_HASH}")

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
