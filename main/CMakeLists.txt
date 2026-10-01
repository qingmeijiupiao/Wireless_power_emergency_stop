#/**
# * @file main/CMakeLists.txt
# * @brief 主程序构件定义：应用依赖、可选编译开关与固件打包后处理
# */

# 可选编译开关：开启时额外链接远程链路的共享实现组件，用于在不上电射频的
# 情况下验证编译。默认关闭，正式固件不包含该路径。
option(ESTOP_VALIDATE_SHARED_REMOTE "Compile migrated remote components without starting radio" OFF)
# 主程序直接依赖的应用组件列表。
set(app_dependencies
    battery_level
    battery_voltage
    blackbox
    blackbox_service
    boot_diagnostics
    button_input
    diagnostic_log
    emergency_remote
    emergency_ui
    esp_common
    esp_system
    esp_timer
    espnow_link
    freertos
    hardware
    HXC_NVS
    log
    power_manager
    runtime_settings
    shell_command
)
# 仅在验证开关开启时追加共享远程实现依赖。
if(ESTOP_VALIDATE_SHARED_REMOTE)
    list(APPEND app_dependencies espnow_remote blackbox_service)
endif()
# 注册 main 组件：唯一源文件为应用入口，依赖列表在编译期展开。
idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES ${app_dependencies}
)

# 构建后处理：调用脚本把 bootloader、分区表与 app 镜像合并为单一固件。
set(SCRIPT_DIR "${CMAKE_SOURCE_DIR}/scripts")
add_custom_target(run_post_build ALL
    COMMAND python "${SCRIPT_DIR}/post_build.py"
    COMMENT "Generating merged firmware"
    VERBATIM
)
add_dependencies(run_post_build app bootloader)
