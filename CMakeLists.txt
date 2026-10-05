#/**
# * @file CMakeLists.txt
# * @brief 工程顶层构建脚本：组件搜索路径、版本号与构建时间戳注入
# */

# 工程要求的最低 CMake 版本。
cmake_minimum_required(VERSION 3.16)

# 引入 ESP-IDF 的 project.cmake，提供 idf_component_* 等构建函数。
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# 仅构建实际被依赖的组件，缩短构建时间并控制镜像体积。
idf_build_set_property(MINIMAL_BUILD ON)

# 额外组件搜索目录：应用层与板级支持包位于自定义路径下。
set(EXTRA_COMPONENT_DIRS
    "components/app"
    "components/bsp"
)

# MAJOR.MINOR 由开发者手工维护。PATCH=99 标识本地构建；
# 发布流水线会将其覆盖为 PATCH=0，以区分正式固件。
set(VERSION_MAJOR 0 CACHE STRING "Major version")
set(VERSION_MINOR 5 CACHE STRING "Minor version")
set(VERSION_PATCH 99 CACHE STRING "Patch version (99=local, 0=release)")
set(PROJECT_VER "v${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")

# 声明工程名，必须先于后续的 idf_build_set_property 调用。
project(Wireless_power_emergency_stop)

# Shell 诊断不能抢占优先级 5 的急停控制任务。
idf_build_set_property(COMPILE_DEFINITIONS "SHELL_TASK_PRIORITY=2" APPEND)

# 将版本号注入为编译期宏，供 Shell 的 version 命令读取。
idf_build_set_property(COMPILE_DEFINITIONS "VERSION_MAJOR=${VERSION_MAJOR}" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "VERSION_MINOR=${VERSION_MINOR}" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "VERSION_PATCH=${VERSION_PATCH}" APPEND)

# 使用固定 UTC+8 时间戳，使本地与发布构建不受构建主机时区影响，结果相同。
execute_process(
    COMMAND python -c "from datetime import datetime, timezone, timedelta; print(datetime.now(timezone(timedelta(hours=8))).strftime('%Y/%m/%d %H:%M:%S'))"
    OUTPUT_VARIABLE BUILD_TIME
    RESULT_VARIABLE BUILD_TIME_RESULT
    ERROR_VARIABLE BUILD_TIME_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
# 时间戳生成失败直接终止构建，避免产出带空构建时间的固件。
if(NOT BUILD_TIME_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate BUILD_TIME: ${BUILD_TIME_ERROR}")
endif()
# 将构建时间注入为字符串宏。
idf_build_set_property(COMPILE_DEFINITIONS "BUILD_TIME=\"${BUILD_TIME}\"" APPEND)
