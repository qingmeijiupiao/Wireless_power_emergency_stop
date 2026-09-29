/*
 * @Description: ESP芯片内置温度传感器
 * @Author: qingmeijiupiao
 * @version: 2.0.0
 * @Date: 2024-09-06 18:38:48
 * @LastEditTime: 2026-04-25 23:47:58
 */
#ifndef ESPCHIPTEMPERATURESENSOR_HPP
#define ESPCHIPTEMPERATURESENSOR_HPP

#include "driver/temperature_sensor.h"
#include "hal/temperature_sensor_periph.h"
#include "esp_err.h"
#include "esp_log.h"

class ESPChipTemperatureSensor_t {
public:
    static ESPChipTemperatureSensor_t& instance();

    ESPChipTemperatureSensor_t(const ESPChipTemperatureSensor_t&) = delete;
    ESPChipTemperatureSensor_t& operator=(const ESPChipTemperatureSensor_t&) = delete;

    esp_err_t init();

    /**
     * @brief 获取当前温度值(自动切换温度范围以提高精度)
     * @return float 当前温度值(摄氏度)
     */
    float getTemperature();

private:
    ESPChipTemperatureSensor_t() = default;

    esp_err_t switchRange(uint8_t range_index);
    int8_t checkswitchRange();

    temperature_sensor_handle_t tsens = nullptr;
    int16_t current_range_min = 0;
    int16_t current_range_max = 0;
    uint8_t current_range_index = 0;
    int16_t absolute_max_temperature = 0;
    int16_t absolute_min_temperature = 0;
    float temp_data = 0.0f;
    temperature_sensor_config_t tsens_config = {};
    bool fault_reported = false;
};

#endif
