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
idf_build_set_property(MINIMAL_BUILD OFF)

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

