# @file CMakeLists.txt
# @brief emergency_ui 组件构建描述：注册 UI 入口、核页面框架、页面实现与按键适配，并声明显示与业务依赖。
idf_component_register(
    SRCS
        "src/emergency_ui.cpp"
        "src/core/ui_manager.cpp"
        "src/core/button_input.cpp"
        "src/core/display_worker.cpp"
        "src/pages/status_page.cpp"
        "src/pages/menu_page.cpp"
        "src/pages/message_page.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    # 显示驱动、按键驱动、持久化与信息页数据来源为私有实现依赖。
    PRIV_REQUIRES emergency_remote sh1106 Button hardware battery_voltage runtime_settings diagnostic_log blackbox blackbox_service esp_app_format esp_hw_support esp_timer freertos log
    # 只公开轻量应用快照/策略与错误码，不传播无线协议和电源执行组件。
    REQUIRES app_types esp_common)
