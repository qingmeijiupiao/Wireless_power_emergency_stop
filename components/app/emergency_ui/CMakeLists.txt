# @file CMakeLists.txt
# @brief emergency_ui 组件构建描述：注册 UI 入口、核页面框架与页面实现，并声明显示与业务依赖。
idf_component_register(
    SRCS
        "src/emergency_ui.cpp"
        "src/core/ui_manager.cpp"
        "src/pages/status_page.cpp"
        "src/pages/menu_page.cpp"
        "src/pages/message_page.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    # 显示驱动、持久化与信息页数据来源为私有实现依赖。
    PRIV_REQUIRES sh1106 battery_level runtime_settings diagnostic_log blackbox blackbox_service esp_app_format esp_hw_support esp_timer freertos log
    # 页面依赖远端快照、按键事件与休眠策略，这些类型出现在公开头文件中。
    REQUIRES emergency_remote button_input power_manager)
