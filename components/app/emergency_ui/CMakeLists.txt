idf_component_register(SRCS "emergency_ui.cpp" INCLUDE_DIRS "include" REQUIRES sh1106 emergency_remote esp_driver_usb_serial_jtag esp_driver_gpio esp_adc esp_timer esp_hw_support nvs_flash runtime_settings battery_level console
    PRIV_REQUIRES esp_hal_usb)
