# @file CMakeLists.txt
# @brief 注册产品协调器及私有功能模块；仅启动接口和错误码类型向外导出。
idf_component_register(
    SRCS
        "src/app_controller.cpp"
        "src/battery_monitor.cpp"
        "src/input_actions.cpp"
        "src/sleep_coordinator.cpp"
        "src/event_recorder.cpp"
        "src/runtime_diagnostics.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES esp_common
    # UI、无线、电池和电源管理均为协调器的实现依赖。
    PRIV_REQUIRES
        battery_voltage app_diagnostics emergency_remote emergency_ui esp_timer
        espnow_link freertos hardware log power_manager runtime_settings heap
)
