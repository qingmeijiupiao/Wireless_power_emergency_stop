cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)

set(EXTRA_COMPONENT_DIRS
    "components/app"
    "components/bsp"
)

# MAJOR.MINOR is maintained by developers. PATCH=99 identifies local builds;
# the release pipeline overrides it with PATCH=0.
set(VERSION_MAJOR 0 CACHE STRING "Major version")
set(VERSION_MINOR 2 CACHE STRING "Minor version")
set(VERSION_PATCH 99 CACHE STRING "Patch version (99=local, 0=release)")
set(PROJECT_VER "v${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")

project(Wireless_power_emergency_stop)

idf_build_set_property(COMPILE_DEFINITIONS "VERSION_MAJOR=${VERSION_MAJOR}" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "VERSION_MINOR=${VERSION_MINOR}" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "VERSION_PATCH=${VERSION_PATCH}" APPEND)

# Use a fixed UTC+8 timestamp so local and release builds report the same
# timezone regardless of the build host configuration.
execute_process(
    COMMAND python -c "from datetime import datetime, timezone, timedelta; print(datetime.now(timezone(timedelta(hours=8))).strftime('%Y/%m/%d %H:%M:%S'))"
    OUTPUT_VARIABLE BUILD_TIME
    RESULT_VARIABLE BUILD_TIME_RESULT
    ERROR_VARIABLE BUILD_TIME_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT BUILD_TIME_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate BUILD_TIME: ${BUILD_TIME_ERROR}")
endif()
idf_build_set_property(COMPILE_DEFINITIONS "BUILD_TIME=\"${BUILD_TIME}\"" APPEND)
