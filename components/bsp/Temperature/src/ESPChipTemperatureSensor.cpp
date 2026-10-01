/**
 * @file ESPChipTemperatureSensor.cpp
 * @brief ESP 芯片内置温度传感器的实现：安装、读取换算与自动量程切换。
 */
#include "ESPChipTemperatureSensor.h"
// 日志标签，用于区分温度传感器相关输出。
constexpr const char* TAG = "ESPTemp";

// 单例入口：函数内静态对象在首次调用时构造，并保证线程安全的唯一实例。
ESPChipTemperatureSensor_t& ESPChipTemperatureSensor_t::instance() {
    static ESPChipTemperatureSensor_t inst;
    return inst;
}

// 初始化：选择默认量程并安装、使能温度传感器。
// 温度传感器按量程分段校准，量程越窄读数越精确，因此选中间量程作为默认值。
esp_err_t ESPChipTemperatureSensor_t::init() {
    uint8_t default_range_index = 2;
    current_range_index = default_range_index;

    // 属性表按量程排列：第 0 项温度最高，最后一项温度最低。
    absolute_max_temperature = temperature_sensor_attributes[0].range_max;
    absolute_min_temperature = temperature_sensor_attributes[TEMPERATURE_SENSOR_ATTR_RANGE_NUM - 1].range_min;

    tsens_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(
        temperature_sensor_attributes[default_range_index].range_min,
        temperature_sensor_attributes[default_range_index].range_max
    );

    // 按默认量程安装传感器驱动。
    esp_err_t ret = temperature_sensor_install(reinterpret_cast<temperature_sensor_config_t*>(&tsens_config), &tsens);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sensor install failed: %s", esp_err_to_name(ret));
        return ret;
    }
    // 使能传感器，使其开始采样。
    ret = temperature_sensor_enable(tsens);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sensor enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 记录当前量程上下限，供后续边界判断使用。
    current_range_min = temperature_sensor_attributes[default_range_index].range_min;
    current_range_max = temperature_sensor_attributes[default_range_index].range_max;
    return ESP_OK;
}

// 读取当前温度（摄氏度），并在接近量程边界时自动切换量程后再读一次，
// 以在更大测量范围内尽量保持较高精度。未初始化或读取失败时返回缓存值。
float ESPChipTemperatureSensor_t::getTemperature() {
    if (tsens == nullptr) {
        if (!fault_reported) {
            ESP_LOGE(TAG, "temperature sensor not initialized");
            fault_reported = true;
        }
        return 0;
    }
    // 由驱动完成原始采样值到摄氏度的换算，结果写入 temp_data。
    esp_err_t ret = temperature_sensor_get_celsius(tsens, &temp_data);
    if (ret != ESP_OK) {
        if (!fault_reported) {
            ESP_LOGE(TAG, "sensor read failed: %s", esp_err_to_name(ret));
            fault_reported = true;
        }
        return temp_data;
    }
    // 从故障状态恢复后只需记录一次日志。
    if (fault_reported) {
        ESP_LOGI(TAG, "sensor reading recovered");
        fault_reported = false;
    }

    // 判断是否需要换挡：非 0 表示升档或降档。
    int8_t switch_result = checkswitchRange();
    if (switch_result != 0) {
        ret = switchRange(current_range_index + switch_result);
        if (ret != ESP_OK) {
            return temp_data;
        }
        // 换挡后重新读取，使返回值对应新量程。
        ret = temperature_sensor_get_celsius(tsens, &temp_data);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "sensor read after range switch failed: %s",
                     esp_err_to_name(ret));
        }
    }

    return temp_data;
}

// 切换量程：ESP-IDF 的温度驱动不支持运行时直接改量程，
// 必须先禁用并卸载，再用新量程参数重新安装使能，因此这里有短暂的采样中断。
esp_err_t ESPChipTemperatureSensor_t::switchRange(uint8_t range_index) {
    if (range_index >= TEMPERATURE_SENSOR_ATTR_RANGE_NUM) {
        ESP_LOGE(TAG, "invalid temperature range index: %d", range_index);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "switching temperature range: %d -> %d", current_range_index, range_index);

    // 依次禁用并卸载旧量程的传感器。
    esp_err_t ret = temperature_sensor_disable(tsens);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sensor disable failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = temperature_sensor_uninstall(tsens);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sensor uninstall failed: %s", esp_err_to_name(ret));
        return ret;
    }
    tsens = nullptr;

    // 用新量程的上下限重建配置。
    tsens_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(
        temperature_sensor_attributes[range_index].range_min,
        temperature_sensor_attributes[range_index].range_max
    );

    // 按新配置重新安装并使能。
    ret = temperature_sensor_install(reinterpret_cast<temperature_sensor_config_t*>(&tsens_config), &tsens);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sensor reinstall failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = temperature_sensor_enable(tsens);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sensor re-enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 更新量程状态记录。
    current_range_min = temperature_sensor_attributes[range_index].range_min;
    current_range_max = temperature_sensor_attributes[range_index].range_max;
    current_range_index = range_index;
    return ESP_OK;
}

// 边界判断：温度距当前量程上限或下限不足阈值时请求换挡。
// 若已在最低或最高量程则不再继续换挡，返回 0 表示保持当前量程。
int8_t ESPChipTemperatureSensor_t::checkswitchRange() {
    constexpr int range_threshold = 10;

    // 接近量程下限且不是最低量程时，请求降档。
    if (temp_data < current_range_min + range_threshold && current_range_index != 0) {
        ESP_LOGD(TAG, "temperature %.1f deg C is near the lower limit; switching to a lower range", temp_data);
        return -1;
    }

    // 接近量程上限且不是最高量程时，请求升档。
    if (temp_data > current_range_max - range_threshold && current_range_index != TEMPERATURE_SENSOR_ATTR_RANGE_NUM - 1) {
        ESP_LOGD(TAG, "temperature %.1f deg C is near the upper limit; switching to a higher range", temp_data);
        return 1;
    }

    return 0;
}
