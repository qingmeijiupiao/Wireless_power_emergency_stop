# =============================================================================
# @file CMakeLists.txt
# @brief hardware 组件的构建注册：源文件、头文件目录与依赖组件。
# =============================================================================
# esp_hal_usb 仅在实现内部使用（直接操作 USB PHY 焊盘），因此放入 PRIV_REQUIRES。
# esp_driver_gpio 的类型与接口通过 hardware.h 暴露给使用者，因此放入 REQUIRES。
idf_component_register(SRCS "src/hardware.cpp"
    INCLUDE_DIRS "include" PRIV_REQUIRES esp_hal_usb
    REQUIRES esp_driver_gpio)
