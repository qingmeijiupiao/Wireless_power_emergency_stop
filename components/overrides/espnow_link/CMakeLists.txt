idf_component_register(
    SRCS
        "src/espnow_link.cpp"
        "src/espnow_link_api.cpp"
        "src/espnow_link_task.cpp"
        "src/espnow_protocol.cpp"
        "src/espnow_pairing.cpp"
        "src/espnow_pairing_protocol.cpp"
        "src/espnow_peer_store.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES diagnostic_log esp_wifi wifi_manager HXC_NVS
    PRIV_REQUIRES freertos log
)
