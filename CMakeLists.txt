cmake_minimum_required(VERSION 3.16)

# Lock target — prevents toolchain ambiguity
set(IDF_TARGET esp32)

# Keep CMake output focused on warnings and errors.
set(CMAKE_MESSAGE_LOG_LEVEL NOTICE)

# Custom component directories
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/components
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
