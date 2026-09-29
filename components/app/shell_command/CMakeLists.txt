idf_component_register(
    SRCS "src/shell_command.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        battery_level
        battery_voltage
        blackbox
        blackbox_service
        espnow_remote
        espnow_service_remote
        power_manager
        shell
        status_led
        esp_system
        esp_timer
)
