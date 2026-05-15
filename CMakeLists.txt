cmake_minimum_required(VERSION 3.16)

# Set system and processor before any language detection
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR xtensa)

# Include IDF toolchain before project
include($ENV{IDF_PATH}/tools/cmake/version.cmake)
include($ENV{IDF_PATH}/tools/cmake/idf.cmake)

project(ceepew)

idf_build_process(esp32)
