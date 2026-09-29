/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: GPIO3 电池电压采集与 GPIO2 低功耗门控实现
 * @author: qingmeijiupiao
 * @LastEditTime: 2026-06-07 13:33:53
 */
#include "battery_voltage.h"

#include <algorithm>

#include "HXC_NVS.h"
#include "adc.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace BatteryVoltage {
namespace {

constexpr gpio_num_t SAMPLE_ENABLE_GPIO = GPIO_NUM_2;
constexpr adc_channel_t BATTERY_ADC_CHANNEL = ADC_CHANNEL_3;
constexpr char TAG[] = "BatteryVoltage";
constexpr uint32_t Q16_ONE = 1U << 16;
constexpr uint32_t DEFAULT_DIVIDER_SCALE_Q16 = 2U * Q16_ONE;
constexpr uint32_t MIN_DIVIDER_SCALE_Q16 = 18U * Q16_ONE / 10U;
constexpr uint32_t MAX_DIVIDER_SCALE_Q16 = 22U * Q16_ONE / 10U;
constexpr uint32_t STABILITY_POLL_MS = 1;
constexpr uint32_t STABILITY_TIMEOUT_MS = 30;
constexpr int STABILITY_THRESHOLD_MV = 25;
constexpr uint32_t REQUIRED_STABLE_READINGS = 4;
constexpr uint32_t AVERAGE_SAMPLE_COUNT = 16;
constexpr uint32_t AVERAGE_SAMPLE_INTERVAL_MS = 1;
constexpr uint32_t CALIBRATION_MAGIC = 0x4243414CU;
constexpr uint16_t CALIBRATION_VERSION = 1;
constexpr uint32_t CALIBRATION_INTERVAL_MS = 10000;
constexpr uint16_t CALIBRATION_REQUIRED_SAMPLES = 60;
constexpr int CALIBRATION_MIN_VOLTAGE_MV = 4000;
constexpr int CALIBRATION_MAX_SPREAD_MV = 5;
constexpr int CALIBRATION_REFERENCE_MV = 4200;

struct CalibrationRecord {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t divider_scale_q16;
    uint32_t checksum;
};

constexpr CalibrationRecord DEFAULT_CALIBRATION = {
    .magic = CALIBRATION_MAGIC,
    .version = CALIBRATION_VERSION,
    .reserved = 0,
    .divider_scale_q16 = DEFAULT_DIVIDER_SCALE_Q16,
    .checksum = 0,
};

adc_t battery_adc(BATTERY_ADC_CHANNEL);
HXC::NVS_DATA<CalibrationRecord> stored_calibration("bat_cal", DEFAULT_CALIBRATION);
bool initialized;
bool sampling;
bool calibration_monitor_running;
SemaphoreHandle_t state_mutex;
SemaphoreHandle_t completion;
esp_err_t last_result = ESP_ERR_INVALID_STATE;
int last_voltage_mv;
CompletionCallback completion_callback;
void* completion_context;
uint32_t divider_scale_q16 = DEFAULT_DIVIDER_SCALE_Q16;
bool stored_calibration_valid;
UsbConnectedCallback usb_connected_callback;
uint16_t calibration_stable_samples;
int calibration_min_mv;
int calibration_max_mv;

uint32_t calibration_checksum(const CalibrationRecord& record) {
    // 小型固定结构使用逐字段混合校验，避免把结构体填充字节纳入校验。
    uint32_t value = 2166136261U;
    const uint32_t fields[] = {
        record.magic,
        static_cast<uint32_t>(record.version) |
            (static_cast<uint32_t>(record.reserved) << 16),
        record.divider_scale_q16,
    };
    for (const uint32_t field : fields) {
        for (uint32_t shift = 0; shift < 32; shift += 8) {
            value ^= (field >> shift) & 0xffU;
            value *= 16777619U;
        }
    }
    return value;
}

bool calibration_record_valid(const CalibrationRecord& record) {
    return record.magic == CALIBRATION_MAGIC &&
           record.version == CALIBRATION_VERSION &&
           record.divider_scale_q16 >= MIN_DIVIDER_SCALE_Q16 &&
           record.divider_scale_q16 <= MAX_DIVIDER_SCALE_Q16 &&
           record.checksum == calibration_checksum(record);
}

int apply_divider_scale(int divided_voltage_mv) {
    // 校准任务可能更新倍率，采样任务通过同一互斥锁取得一致快照。
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    const uint32_t scale_q16 = divider_scale_q16;
    xSemaphoreGive(state_mutex);
    return static_cast<int>(
        (static_cast<int64_t>(divided_voltage_mv) * scale_q16 +
         Q16_ONE / 2) /
        Q16_ONE);
}

esp_err_t read_divided_mv_once(int& divided_voltage_mv) {
    return battery_adc.read_voltage_mV(divided_voltage_mv);
}

esp_err_t wait_until_stable() {
    int previous_mv = 0;
    esp_err_t result = read_divided_mv_once(previous_mv);
    if (result != ESP_OK) {
        return result;
    }

    uint32_t stable_readings = 0;
    for (uint32_t elapsed_ms = STABILITY_POLL_MS;
         elapsed_ms <= STABILITY_TIMEOUT_MS;
         elapsed_ms += STABILITY_POLL_MS) {
        vTaskDelay(pdMS_TO_TICKS(STABILITY_POLL_MS));

        int current_mv = 0;
        result = read_divided_mv_once(current_mv);
        if (result != ESP_OK) {
            return result;
        }

        const int difference_mv =
            current_mv >= previous_mv ? current_mv - previous_mv : previous_mv - current_mv;
        stable_readings =
            difference_mv <= STABILITY_THRESHOLD_MV ? stable_readings + 1 : 0;
        if (stable_readings >= REQUIRED_STABLE_READINGS) {
            return ESP_OK;
        }
        previous_mv = current_mv;
    }
    // 超时仅表示已达到最大稳定等待时间，不代表采样失败。ADC 噪声可能使连续
    // 四次读数无法满足阈值，因此 30 ms 后仍继续执行后续平均采样。
    return ESP_OK;
}

esp_err_t read_average_divided_mv(int& divided_voltage_mv) {
    int64_t voltage_sum_mv = 0;
    for (uint32_t sample = 0; sample < AVERAGE_SAMPLE_COUNT; ++sample) {
        int sample_mv = 0;
        const esp_err_t result = read_divided_mv_once(sample_mv);
        if (result != ESP_OK) {
            return result;
        }
        voltage_sum_mv += sample_mv;
        if (sample + 1 < AVERAGE_SAMPLE_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(AVERAGE_SAMPLE_INTERVAL_MS));
        }
    }

    divided_voltage_mv = static_cast<int>(
        (voltage_sum_mv + AVERAGE_SAMPLE_COUNT / 2) / AVERAGE_SAMPLE_COUNT);
    return ESP_OK;
}

void sampling_task(void*) {
    // 分压路径只在采样窗口内导通，所有退出路径都必须恢复高阻以降低静态功耗。
    int voltage_mv = 0;
    esp_err_t result = enable_sample_path();
    if (result == ESP_OK) {
        result = wait_until_stable();
    }
    if (result == ESP_OK) {
        int divided_voltage_mv = 0;
        result = read_average_divided_mv(divided_voltage_mv);
        if (result == ESP_OK) {
            voltage_mv = apply_divider_scale(divided_voltage_mv);
        }
    }

    const esp_err_t disable_result = disable_sample_path();
    if (result == ESP_OK) {
        result = disable_result;
    }

    xSemaphoreTake(state_mutex, portMAX_DELAY);
    last_result = result;
    last_voltage_mv = voltage_mv;
    const CompletionCallback callback = completion_callback;
    void* const context = completion_context;
    completion_callback = nullptr;
    completion_context = nullptr;
    xSemaphoreGive(state_mutex);

    // 回调在采样任务上下文执行；先清空共享回调槽，允许回调完成后再次发起采样。
    if (callback != nullptr) {
        callback(result, voltage_mv, context);
    }

    xSemaphoreTake(state_mutex, portMAX_DELAY);
    sampling = false;
    xSemaphoreGive(state_mutex);
    xSemaphoreGive(completion);
    vTaskDelete(nullptr);
}

void publish_calibration_window(uint16_t sample_count,
                                int min_voltage_mv,
                                int max_voltage_mv) {
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    calibration_stable_samples = sample_count;
    calibration_min_mv = min_voltage_mv;
    calibration_max_mv = max_voltage_mv;
    xSemaphoreGive(state_mutex);
}

esp_err_t save_calibration(uint32_t new_scale_q16) {
    CalibrationRecord record = DEFAULT_CALIBRATION;
    record.divider_scale_q16 = new_scale_q16;
    record.checksum = calibration_checksum(record);
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    const esp_err_t persist_result = stored_calibration.set(record);
    if (persist_result != ESP_OK) {
        xSemaphoreGive(state_mutex);
        return persist_result;
    }
    divider_scale_q16 = new_scale_q16;
    stored_calibration_valid = true;
    xSemaphoreGive(state_mutex);
    return ESP_OK;
}

void calibration_monitor_task(void*) {
    uint16_t stable_samples = 0;
    int stable_min_mv = 0;
    int stable_max_mv = 0;

    while (usb_connected_callback != nullptr && usb_connected_callback()) {
        vTaskDelay(pdMS_TO_TICKS(CALIBRATION_INTERVAL_MS));
        if (!usb_connected_callback()) {
            break;
        }

        int voltage_mv = 0;
        const esp_err_t result = read_mv(voltage_mv);
        if (result == ESP_ERR_INVALID_STATE) {
            continue;
        }
        if (result != ESP_OK || voltage_mv <= CALIBRATION_MIN_VOLTAGE_MV) {
            stable_samples = 0;
            stable_min_mv = 0;
            stable_max_mv = 0;
            publish_calibration_window(0, 0, 0);
            continue;
        }

        if (stable_samples == 0) {
            stable_min_mv = voltage_mv;
            stable_max_mv = voltage_mv;
        } else {
            stable_min_mv = std::min(stable_min_mv, voltage_mv);
            stable_max_mv = std::max(stable_max_mv, voltage_mv);
        }
        ++stable_samples;

        if (stable_max_mv - stable_min_mv >= CALIBRATION_MAX_SPREAD_MV) {
            // 当前样本作为新稳定窗口的起点，避免再等待一个完整采样周期。
            stable_samples = 1;
            stable_min_mv = voltage_mv;
            stable_max_mv = voltage_mv;
            publish_calibration_window(
                stable_samples, stable_min_mv, stable_max_mv);
            continue;
        }
        publish_calibration_window(
            stable_samples, stable_min_mv, stable_max_mv);
        if (stable_samples < CALIBRATION_REQUIRED_SAMPLES) {
            continue;
        }

        const int stable_voltage_mv =
            (stable_min_mv + stable_max_mv + 1) / 2;
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        const uint32_t current_scale_q16 = divider_scale_q16;
        xSemaphoreGive(state_mutex);
        const uint32_t new_scale_q16 = static_cast<uint32_t>(
            (static_cast<uint64_t>(current_scale_q16) *
             CALIBRATION_REFERENCE_MV +
             stable_voltage_mv / 2) /
            stable_voltage_mv);
        if (new_scale_q16 >= MIN_DIVIDER_SCALE_Q16 &&
            new_scale_q16 <= MAX_DIVIDER_SCALE_Q16) {
            (void)save_calibration(new_scale_q16);
            ESP_LOGI(TAG,
                     "full-charge calibration complete: stable=%d mV scale_q16=%lu",
                     stable_voltage_mv,
                     static_cast<unsigned long>(new_scale_q16));
        } else {
            ESP_LOGW(TAG,
                     "calibration result out of range: stable=%d mV scale_q16=%lu",
                     stable_voltage_mv,
                     static_cast<unsigned long>(new_scale_q16));
        }
        // 每次 USB 插入周期只执行一次 Flash 写入。
        break;
    }

    xSemaphoreTake(state_mutex, portMAX_DELAY);
    calibration_monitor_running = false;
    calibration_stable_samples = 0;
    calibration_min_mv = 0;
    calibration_max_mv = 0;
    xSemaphoreGive(state_mutex);
    vTaskDelete(nullptr);
}

} // namespace

esp_err_t disable_sample_path() {
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << SAMPLE_ENABLE_GPIO;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&config);
}

esp_err_t enable_sample_path() {
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << SAMPLE_ENABLE_GPIO;
    config.mode = GPIO_MODE_OUTPUT_OD;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;

    esp_err_t ret = gpio_config(&config);
    if (ret != ESP_OK) {
        return ret;
    }
    return gpio_set_level(SAMPLE_ENABLE_GPIO, 0);
}


esp_err_t init() {
    if (initialized) {
        return ESP_OK;
    }

    esp_err_t ret = disable_sample_path();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = battery_adc.init();
    if (ret != ESP_OK) {
        return ret;
    }

    const CalibrationRecord record = stored_calibration.read();
    stored_calibration_valid = calibration_record_valid(record);
    divider_scale_q16 = stored_calibration_valid
                            ? record.divider_scale_q16
                            : DEFAULT_DIVIDER_SCALE_Q16;

    state_mutex = xSemaphoreCreateMutex();
    completion = xSemaphoreCreateBinary();
    if (state_mutex == nullptr || completion == nullptr) {
        if (state_mutex != nullptr) {
            vSemaphoreDelete(state_mutex);
            state_mutex = nullptr;
        }
        if (completion != nullptr) {
            vSemaphoreDelete(completion);
            completion = nullptr;
        }
        return ESP_ERR_NO_MEM;
    }

    initialized = true;
    return ESP_OK;
}

esp_err_t start_async(CompletionCallback callback, void* context) {
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(state_mutex, portMAX_DELAY);
    if (sampling) {
        xSemaphoreGive(state_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    sampling = true;
    last_result = ESP_ERR_INVALID_STATE;
    completion_callback = callback;
    completion_context = context;
    xSemaphoreTake(completion, 0);
    xSemaphoreGive(state_mutex);

    if (xTaskCreate(sampling_task, "battery_sample", 3072, nullptr, 3, nullptr) != pdPASS) {
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        sampling = false;
        completion_callback = nullptr;
        completion_context = nullptr;
        xSemaphoreGive(state_mutex);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t wait_mv(int& voltage_mv, TickType_t ticks_to_wait) {
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(state_mutex, portMAX_DELAY);
    const bool currently_sampling = sampling;
    if (!currently_sampling) {
        voltage_mv = last_voltage_mv;
        const esp_err_t result = last_result;
        xSemaphoreGive(state_mutex);
        return result;
    }
    xSemaphoreGive(state_mutex);

    if (xSemaphoreTake(completion, ticks_to_wait) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    // 二值信号量在读取后放回，使多个后续查询者都能取得最近一次采样结果。
    xSemaphoreGive(completion);

    xSemaphoreTake(state_mutex, portMAX_DELAY);
    voltage_mv = last_voltage_mv;
    const esp_err_t result = last_result;
    xSemaphoreGive(state_mutex);
    return result;
}

bool is_busy() {
    if (!initialized) {
        return false;
    }
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    const bool result = sampling;
    xSemaphoreGive(state_mutex);
    return result;
}

esp_err_t read_mv(int& voltage_mv) {
    const esp_err_t start_result = start_async();
    if (start_result != ESP_OK) {
        return start_result;
    }
    return wait_mv(voltage_mv);
}

esp_err_t start_calibration_monitor(UsbConnectedCallback usb_connected) {
    if (!initialized || usb_connected == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    if (calibration_monitor_running) {
        xSemaphoreGive(state_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    usb_connected_callback = usb_connected;
    calibration_stable_samples = 0;
    calibration_min_mv = 0;
    calibration_max_mv = 0;
    calibration_monitor_running = true;
    xSemaphoreGive(state_mutex);
    if (xTaskCreate(calibration_monitor_task,
                    "battery_cal",
                    3072,
                    nullptr,
                    2,
                    nullptr) != pdPASS) {
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        calibration_monitor_running = false;
        usb_connected_callback = nullptr;
        xSemaphoreGive(state_mutex);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void get_calibration_status(CalibrationStatus& status) {
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    status = {
        .divider_scale_q16 = divider_scale_q16,
        .stored_calibration_valid = stored_calibration_valid,
        .monitor_running = calibration_monitor_running,
        .stable_sample_count = calibration_stable_samples,
        .stable_min_mv = calibration_min_mv,
        .stable_max_mv = calibration_max_mv,
    };
    xSemaphoreGive(state_mutex);
}

esp_err_t reset_calibration() {
    CalibrationRecord record = DEFAULT_CALIBRATION;
    // 写入无效校验值表示显式恢复出厂倍率，重启后仍显示为“未校准”。
    record.checksum = 0;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    const esp_err_t persist_result = stored_calibration.set(record);
    if (persist_result != ESP_OK) {
        xSemaphoreGive(state_mutex);
        return persist_result;
    }
    divider_scale_q16 = DEFAULT_DIVIDER_SCALE_Q16;
    stored_calibration_valid = false;
    calibration_stable_samples = 0;
    calibration_min_mv = 0;
    calibration_max_mv = 0;
    xSemaphoreGive(state_mutex);
    return ESP_OK;
}

} // namespace BatteryVoltage
