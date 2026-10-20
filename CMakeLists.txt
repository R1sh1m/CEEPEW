cmake_minimum_required(VERSION 3.16)

# Lock target — prevents toolchain ambiguity
set(IDF_TARGET esp32)

# Keep CMake output focused on warnings and errors.
set(CMAKE_MESSAGE_LOG_LEVEL NOTICE)

# GCC 15.2.0 toolchain on Windows requires explicit dynconfig to load Xtensa CPU
# config (rer opcode, MUL16, etc.). On Linux/Docker CI the standard IDF toolchain
# handles CPU config without -mdynconfig.
if(WIN32)
    set(_DYNCONFIG "xtensa_esp32.so")
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -mdynconfig=${_DYNCONFIG} -std=gnu11")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mdynconfig=${_DYNCONFIG} -std=gnu++17")
    set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -mdynconfig=${_DYNCONFIG}")
else()
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -std=gnu11")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=gnu++17")
endif()

# Custom component directories. tests/ registers with no sources unless
# CONFIG_CEEPEW_DEVELOPMENT_MODE=y (see tests/CMakeLists.txt), so production
# firmware is unaffected; dev-mode builds link the on-board self-test suite
# invoked from main.c and printing === DIAGNOSTIC REPORT ===.
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/components
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

# This one include does everything:
#   - selects xtensa-esp32-elf-gcc toolchain
#   - calls idf_build_process internally
#   - registers menuconfig, bootloader, flash targets
# DO NOT call idf_build_process() anywhere else.
if(NOT DEFINED ENV{IDF_PATH} OR
   "$ENV{IDF_PATH}" STREQUAL "" OR
   NOT EXISTS "$ENV{IDF_PATH}/tools/cmake/project.cmake")
    set(ENV{IDF_PATH} "C:/esp/v6.0.2/esp-idf")
    set(IDF_PATH "C:/esp/v6.0.2/esp-idf")
else()
    set(IDF_PATH "$ENV{IDF_PATH}")
endif()
# Required for idf.py mcp-server detection: include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${IDF_PATH}/tools/cmake/project.cmake)

# Keep the build focused on the components this app actually uses.
idf_build_set_property(MINIMAL_BUILD OFF)

project(ceepew)

# ESP-Modbus unblock (project config only — NO third-party code modified):
# managed_components espressif__esp-modbus forces -Werror on its own target,
# and GCC 15.x (pinned IDF 6.0.2 toolchain) reports -Wstringop-truncation on
# its modbus/mb_ports/tcp/port_tcp_utils.c strncpy. Downgrade ONLY that
# warning to non-error for that target; the warning itself stays visible.
# NOTE: registry component target is __idf_espressif__esp-modbus
# (namespace __ name), not __idf_esp-modbus.
if(TARGET __idf_espressif__esp-modbus)
    target_compile_options(__idf_espressif__esp-modbus PRIVATE -Wno-error=stringop-truncation)
endif()

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
