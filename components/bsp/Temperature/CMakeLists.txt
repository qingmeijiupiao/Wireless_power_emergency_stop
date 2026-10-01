# =============================================================================
# @file CMakeLists.txt
# @brief Temperature 组件的构建注册：源文件、头文件目录与依赖组件。
# =============================================================================
# esp_driver_tsens 提供温度传感器驱动；esp_hal_ana_conv 提供量程属性表。
idf_component_register(
                    SRCS "src/ESPChipTemperatureSensor.cpp"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_driver_tsens esp_hal_ana_conv
                    )
