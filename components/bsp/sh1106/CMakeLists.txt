# =============================================================================
# @file CMakeLists.txt
# @brief sh1106 组件的构建注册：源文件、头文件目录与依赖组件。
# =============================================================================
# esp_driver_i2c 提供的类型通过 sh1106.h 暴露给使用者，因此放入 REQUIRES。
idf_component_register(SRCS "sh1106.cpp" INCLUDE_DIRS "include" REQUIRES esp_driver_i2c)
