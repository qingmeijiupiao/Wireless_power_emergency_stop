idf_component_register(
    SRCS "src/power_manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES battery_voltage esp_driver_gpio esp_hw_support esp_timer status_led
)
