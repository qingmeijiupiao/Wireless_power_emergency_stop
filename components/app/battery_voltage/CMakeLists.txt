idf_component_register(
    SRCS "src/battery_voltage.cpp"
    INCLUDE_DIRS "include"
    REQUIRES ADC HXC_NVS esp_driver_gpio freertos
)
