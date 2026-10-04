idf_component_register(SRCS "blackbox.cpp"
                    REQUIRES circular_flash_buffer
                    PRIV_REQUIRES esp_timer log
                    INCLUDE_DIRS "include")
