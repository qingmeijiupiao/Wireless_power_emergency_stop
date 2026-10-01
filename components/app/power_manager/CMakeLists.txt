# /**
#  * @file CMakeLists.txt
#  * @brief 电源管理组件构建描述：声明源文件、公开头文件目录与组件依赖。
#  */
# REQUIRES：对外公开的依赖，使用本组件接口的下游可一并获得。
# PRIV_REQUIRES：仅本组件实现内部使用的依赖，不向调用方传递。
idf_component_register(SRCS "src/power_manager.cpp" INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES hardware battery_level emergency_remote runtime_settings diagnostic_log esp_hw_support esp_timer freertos esp_driver_usb_serial_jtag blackbox blackbox_service log)
