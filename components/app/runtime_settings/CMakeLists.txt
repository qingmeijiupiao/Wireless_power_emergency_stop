#/**
# * @file components/app/runtime_settings/CMakeLists.txt
# * @brief 运行参数组件构建定义与依赖
# */

# esp_common 为公开依赖；HXC_NVS/diagnostic_log/log 仅内部实现使用，故列为私有。
idf_component_register(SRCS "src/runtime_settings.cpp" INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES freertos HXC_NVS diagnostic_log log)
