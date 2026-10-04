# /**
#  * @file CMakeLists.txt
#  * @brief 急停遥控组件构建描述：声明源文件、公开头文件目录与组件依赖。
#  */
# REQUIRES：对外公开的依赖，使用本组件接口的下游可一并获得。
# PRIV_REQUIRES：仅本组件实现内部使用的依赖，不向调用方传递。
idf_component_register(SRCS "src/emergency_remote.cpp" INCLUDE_DIRS "include"
    REQUIRES app_types esp_common
    PRIV_REQUIRES espnow_service_remote app_diagnostics espnow_link wifi_manager hardware esp_timer runtime_settings diagnostic_log log)
