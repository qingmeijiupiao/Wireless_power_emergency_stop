idf_component_register(
                    SRCS "src/ESPChipTemperatureSensor.cpp"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_driver_tsens esp_hal_ana_conv
                    )
