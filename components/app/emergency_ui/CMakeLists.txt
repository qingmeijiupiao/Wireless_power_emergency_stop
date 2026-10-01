# =============================================================================
# @file CMakeLists.txt
# @brief 声明 emergency_ui 组件的源文件、头文件目录与依赖关系。
# =============================================================================
# 注册 emergency_ui 组件：编译 emergency_ui.cpp；include 对外公开，private_include 仅组件内可见。
idf_component_register(SRCS "src/emergency_ui.cpp"
    # 私有依赖：显示驱动、电量、运行时设置、诊断日志、黑匣子及其服务、应用描述、硬件支持、定时器、FreeRTOS 与日志。
    INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "private_include" PRIV_REQUIRES sh1106 battery_level runtime_settings diagnostic_log blackbox blackbox_service esp_app_format esp_hw_support esp_timer freertos log
    # 公开依赖：远端状态、按键输入与电源管理。
    REQUIRES emergency_remote button_input power_manager)
