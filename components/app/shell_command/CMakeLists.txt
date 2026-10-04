#/**
# * @file components/app/shell_command/CMakeLists.txt
# * @brief Shell 命令组件构建定义与公开/私有依赖
# */

idf_component_register(
    SRCS "src/shell_command.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES
        app_diagnostics
        battery_voltage
        blackbox
        blackbox_service
        emergency_remote
        emergency_ui
        hardware
        power_manager
        runtime_settings
        shell
        esp_system
        esp_timer
)
