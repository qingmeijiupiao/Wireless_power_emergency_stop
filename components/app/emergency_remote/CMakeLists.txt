idf_component_register(SRCS "emergency_remote.cpp" INCLUDE_DIRS "include"
    REQUIRES espnow_service_remote espnow_link HXC_NVS wifi_manager esp_driver_gpio esp_timer runtime_settings)
