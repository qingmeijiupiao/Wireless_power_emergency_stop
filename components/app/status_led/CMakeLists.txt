idf_component_register(
    SRCS "src/status_led.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio espnow_link freertos
)
