# @file CMakeLists.txt
# @brief boot_diagnostics 组件构建描述：注册启动诊断源文件、头文件目录与依赖组件。
#
# 依赖均为私有实现依赖：
# - hardware/battery_voltage/runtime_settings/app_diagnostics/espnow_link：启动快照数据来源
# - esp_app_format：读取固件版本与 IDF 版本
# - esp_system/esp_hw_support：复位原因与深睡唤醒状态查询
# - log：产品事件日志底层输出
idf_component_register(SRCS "src/boot_diagnostics.cpp" INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES hardware battery_voltage runtime_settings app_diagnostics espnow_link esp_app_format esp_system esp_hw_support log)
