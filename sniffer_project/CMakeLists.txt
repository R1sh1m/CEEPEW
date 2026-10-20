cmake_minimum_required(VERSION 3.16)

# Include only the sniffer component (avoid tests/arduino conflicts)
set(EXTRA_COMPONENT_DIRS ${CMAKE_SOURCE_DIR}/../components/sniffer)

# Include ESP-IDF build system
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(ceepew_sniffer)