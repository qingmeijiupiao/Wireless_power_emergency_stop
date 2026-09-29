idf_component_register(
    SRCS "src/button_input.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_gpio
        espnow_remote
        espnow_service_remote
        freertos
        log
        power_manager
        status_led
        esp_common
        esp_timer
)
