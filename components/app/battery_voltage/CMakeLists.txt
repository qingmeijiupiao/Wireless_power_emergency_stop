# @file CMakeLists.txt
# @brief battery_voltage 组件构建描述：注册源文件、头文件目录与依赖组件。
#
# 依赖说明：
# - ADC：GPIO0 对应的 ADC 读取封装
# - HXC_NVS：分压校准记录的持久化存储
# - esp_driver_gpio：GPIO10 低边使能引脚驱动
# - freertos：互斥锁、信号量与采样任务
idf_component_register(
    SRCS "src/battery_voltage.cpp" "src/battery_status.cpp"
    INCLUDE_DIRS "include"
    REQUIRES battery_level esp_common freertos
    PRIV_REQUIRES app_diagnostics ADC HXC_NVS esp_driver_gpio esp_timer log
)
