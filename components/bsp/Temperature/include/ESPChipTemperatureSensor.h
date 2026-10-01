/**
 * @file ESPChipTemperatureSensor.h
 * @brief ESP 芯片内置温度传感器：单例封装、自动量程切换与摄氏度读取。
 */
#ifndef ESPCHIPTEMPERATURESENSOR_HPP
#define ESPCHIPTEMPERATURESENSOR_HPP

#include "driver/temperature_sensor.h"
#include "hal/temperature_sensor_periph.h"
#include "esp_err.h"
#include "esp_log.h"

// 芯片内置温度传感器单例。硬件只暴露一个温度传感器，因此全进程共用一个实例。
class ESPChipTemperatureSensor_t {
public:
    // 返回全局唯一实例（首次调用时构造）。
    static ESPChipTemperatureSensor_t& instance();

    // 禁止拷贝与赋值，保证单例唯一。
    ESPChipTemperatureSensor_t(const ESPChipTemperatureSensor_t&) = delete;
    ESPChipTemperatureSensor_t& operator=(const ESPChipTemperatureSensor_t&) = delete;

    // 安装并使能温度传感器，默认使用中间量程。
    esp_err_t init();

    /**
     * @brief 获取当前温度值(自动切换温度范围以提高精度)
     * @return float 当前温度值(摄氏度)
     */
    float getTemperature();

private:
    ESPChipTemperatureSensor_t() = default;

    // 切换到指定量程：需要先禁用并卸载传感器，再按新量程重新安装使能。
    esp_err_t switchRange(uint8_t range_index);
    // 判断当前温度是否接近量程边界，返回 -1 降档、+1 升档、0 保持不变。
    int8_t checkswitchRange();

    temperature_sensor_handle_t tsens = nullptr;
    // 当前量程的温度上下限（摄氏度）。
    int16_t current_range_min = 0;
    int16_t current_range_max = 0;
    // 当前量程在温度传感器属性表中的索引。
    uint8_t current_range_index = 0;
    // 所有量程覆盖的绝对温度上下限。
    int16_t absolute_max_temperature = 0;
    int16_t absolute_min_temperature = 0;
    // 最近一次读取到的温度缓存（摄氏度）。
    float temp_data = 0.0f;
    // 温度传感器安装配置。
    temperature_sensor_config_t tsens_config = {};
    // 是否已上报故障，避免重复刷屏日志。
    bool fault_reported = false;
};

#endif
