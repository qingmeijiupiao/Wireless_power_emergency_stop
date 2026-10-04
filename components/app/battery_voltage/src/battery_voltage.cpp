/**
 * @file battery_voltage.cpp
 * @brief 电池电压采集实现：GPIO0 ADC 分压采样、GPIO10 低边使能、16 次平均与 USB 满电自动校准。
 */
// 依赖说明：
// - battery_voltage.h：本模块对外接口
// - <algorithm>：std::min / std::max，用于稳定窗口的极值统计
// - HXC_NVS.h：分压校准记录的持久化存储
// - adc.h：GPIO0 对应 ADC 通道的电压读取封装
// - driver/gpio.h：GPIO10 低边使能引脚配置
// - esp_log.h：校准结果日志输出
// - freertos 头文件：互斥锁、二值信号量与后台任务
#include "battery_voltage.h"
#include "app_diagnostics.h"

#include <algorithm>

#include "HXC_NVS.h"
#include "adc.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace BatteryVoltage {
namespace {

// GPIO10 为分压网络低边开关，仅采样窗口内输出低电平，其余时间保持高阻。
constexpr gpio_num_t SAMPLE_ENABLE_GPIO = GPIO_NUM_10;
// GPIO0 对应的 ADC 通道，采样电池分压后的电压。
constexpr adc_channel_t BATTERY_ADC_CHANNEL = ADC_CHANNEL_0;
// 日志标签，便于在串口输出中定位本模块。
constexpr char TAG[] = "BatteryVoltage";
// Q16 定点表示中的 1.0，用于把分压倍率用整数保存且保留小数精度。
constexpr uint32_t Q16_ONE = 1U << 16;
// 默认分压倍率 2.0：分压后电压乘以该值得到电池电压。
constexpr uint32_t DEFAULT_DIVIDER_SCALE_Q16 = 2U * Q16_ONE;
// 合理倍率下限 1.8，超出范围视为无效/异常校准结果。
constexpr uint32_t MIN_DIVIDER_SCALE_Q16 = 18U * Q16_ONE / 10U;
// 合理倍率上限 2.2，超出范围视为无效/异常校准结果。
constexpr uint32_t MAX_DIVIDER_SCALE_Q16 = 22U * Q16_ONE / 10U;
// RC 稳定判定轮询间隔。
constexpr uint32_t STABILITY_POLL_MS = 1;
// RC 稳定等待上限。超时后不再等待，直接进入平均采样。
constexpr uint32_t STABILITY_TIMEOUT_MS = 30;
// 相邻两次读数差值不超过该阈值时视为“稳定”。
constexpr int STABILITY_THRESHOLD_MV = 25;
// 连续达到该次数的稳定读数才认为 RC 网络已稳定。
constexpr uint32_t REQUIRED_STABLE_READINGS = 4;
// 平均采样次数，用于抑制 ADC 随机噪声。
constexpr uint32_t AVERAGE_SAMPLE_COUNT = 16;
// 平均采样之间的间隔，避免连续采样相关性过强。
constexpr uint32_t AVERAGE_SAMPLE_INTERVAL_MS = 1;
// 校准记录魔数，用于识别 NVS 中的本模块数据。
constexpr uint32_t CALIBRATION_MAGIC = 0x4243414CU;
// 校准记录版本号，结构变化时递增即可让旧记录失效。
constexpr uint16_t CALIBRATION_VERSION = 1;
// 自动校准采样周期：10 秒一次。
constexpr uint32_t CALIBRATION_INTERVAL_MS = 10000;
// 稳定窗口所需样本数：60 × 10 秒 = 10 分钟。
constexpr uint16_t CALIBRATION_REQUIRED_SAMPLES = 60;
// 进入校准判定的最低电压，低于该值认为尚未充满。
constexpr int CALIBRATION_MIN_VOLTAGE_MV = 4000;
// 稳定窗口允许的最大极差，超过则重置窗口。
constexpr int CALIBRATION_MAX_SPREAD_MV = 5;
// USB 满电参考电压，校准以此为准反推倍率。
constexpr int CALIBRATION_REFERENCE_MV = 4200;

// NVS 持久化校准记录。使用固定宽度字段并自算校验，避免结构体填充字节
// 造成跨编译器的布局差异。
struct CalibrationRecord {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t divider_scale_q16;
    uint32_t checksum;
};

// 默认（未校准）记录：check_checksum 为 0，因此校验必然失败，
// 会被校准有效性判定识别为“未校准”，从而使用默认倍率。
constexpr CalibrationRecord DEFAULT_CALIBRATION = {
    .magic = CALIBRATION_MAGIC,
    .version = CALIBRATION_VERSION,
    .reserved = 0,
    .divider_scale_q16 = DEFAULT_DIVIDER_SCALE_Q16,
    .checksum = 0,
};

// GPIO0 ADC 采样对象。
adc_t battery_adc(BATTERY_ADC_CHANNEL);
// NVS 键 "bat_cal" 中保存的校准记录，读取时自动回退到默认记录。
HXC::NVS_DATA<CalibrationRecord> stored_calibration("bat_cal", DEFAULT_CALIBRATION);
// 模块是否已完成初始化。
bool initialized;
// 初始化后只读，所有采样任务共享同一个供电稳定截止时间。
int64_t sample_not_before_us = 0;
// 是否有异步采样任务正在运行（互斥锁保护）。
bool sampling;
// 自动校准任务是否正在运行（互斥锁保护）。
bool calibration_monitor_running;
// 保护所有共享状态（采样标志、结果、倍率、回调槽、校准窗口等）。
SemaphoreHandle_t state_mutex;
// 固定请求池：等待者持有引用直到复制结果；下一次采样不覆盖旧结果或通知。
struct SampleRequest {
    SemaphoreHandle_t completion = nullptr;
    unsigned references = 0; // state_mutex 保护；最近请求、工作者、等待者各持一份。
    uint32_t sequence = 0;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    int voltage_mv = 0;
    CompletionCallback callback = nullptr;
    void* context = nullptr;
};
SampleRequest requests[4];
SampleRequest* latest_request = nullptr;
SemaphoreHandle_t sample_ready = nullptr;
SemaphoreHandle_t sample_idle = nullptr;
TaskHandle_t sample_worker = nullptr;
uint32_t next_sequence = 0;
bool sleep_paused = false;
uint32_t calibration_epoch = 0; // 重置/USB 边沿/倍率变化使旧稳定窗口失效。
bool external_power = false;
esp_err_t calibration_error = ESP_OK;
// 当前生效的分压倍率（Q16 定点），由持久化校准决定。
uint32_t divider_scale_q16 = DEFAULT_DIVIDER_SCALE_Q16;
// NVS 中读出的校准记录是否通过校验。
bool stored_calibration_valid;
// USB 连接状态查询回调，自动校准任务使用。
UsbConnectedCallback usb_connected_callback;
// 自动校准稳定窗口的样本计数与电压极值。
uint16_t calibration_stable_samples;
int calibration_min_mv;
int calibration_max_mv;

// 计算校准记录的校验值。采用 FNV-1a 风格哈希逐字段混合，只覆盖有意义的
// 字段而不整体哈希结构体，从而规避结构体填充字节的不确定内容。
uint32_t calibration_checksum(const CalibrationRecord& record) {
    // 小型固定结构使用逐字段混合校验，避免把结构体填充字节纳入校验。
    // 初始值为 FNV-1a 偏移基准，逐字节异或后再乘以 FNV 质数。
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

// 判定一条校准记录是否可信：魔数、版本、倍率范围与校验值必须全部匹配。
// 任一条件不满足即视为无效，调用方会退回默认 2.0 倍率。
bool calibration_record_valid(const CalibrationRecord& record) {
    return record.magic == CALIBRATION_MAGIC &&
           record.version == CALIBRATION_VERSION &&
           record.divider_scale_q16 >= MIN_DIVIDER_SCALE_Q16 &&
           record.divider_scale_q16 <= MAX_DIVIDER_SCALE_Q16 &&
           record.checksum == calibration_checksum(record);
}

// 把 ADC 读到的分压电压换算为电池电压：divided × scale。
// 倍率是 Q16 定点，先取一致快照再用 64 位乘法避免溢出；加 Q16_ONE/2
// 实现四舍五入而非截断。
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

// 单次读取分压后的电压（mV），仅做透传，便于统一错误处理。
esp_err_t read_divided_mv_once(int& divided_voltage_mv) {
    return battery_adc.read_voltage_mV(divided_voltage_mv);
}

// 等待 RC 分压网络稳定后再进入平均采样。使能低边后分压节点需要充电建立
// 稳态，逐毫秒轮询相邻读数的变化量；连续 REQUIRED_STABLE_READINGS 次变化
// 不超过阈值即认为稳定。若到 STABILITY_TIMEOUT_MS 仍未稳定则直接放行。
esp_err_t wait_until_stable() {
    // 先读一次作为比较基准。
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

        // 计算相邻两次读数的绝对差值；一旦超阈值就把稳定计数清零重新累计。
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

// 连续采样 AVERAGE_SAMPLE_COUNT 次并取算术平均，以抑制 ADC 随机噪声。
// 求和使用 64 位累加器防止溢出，除法前加半数实现四舍五入。
esp_err_t read_average_divided_mv(int& divided_voltage_mv) {
    int64_t voltage_sum_mv = 0;
    for (uint32_t sample = 0; sample < AVERAGE_SAMPLE_COUNT; ++sample) {
        int sample_mv = 0;
        const esp_err_t result = read_divided_mv_once(sample_mv);
        if (result != ESP_OK) {
            return result;
        }
        voltage_sum_mv += sample_mv;
        // 最后一次采样后不再延时，缩短整体采样时间。
        if (sample + 1 < AVERAGE_SAMPLE_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(AVERAGE_SAMPLE_INTERVAL_MS));
        }
    }

    divided_voltage_mv = static_cast<int>(
        (voltage_sum_mv + AVERAGE_SAMPLE_COUNT / 2) / AVERAGE_SAMPLE_COUNT);
    return ESP_OK;
}

/** @brief 执行一个已占用请求；回调返回、GPIO 关闭后才发布空闲握手。 */
void perform_sample(SampleRequest* request) {
    const int64_t remaining_us = sample_not_before_us - esp_timer_get_time();
    if (remaining_us > 0) {
        const TickType_t ticks = static_cast<TickType_t>(
            (remaining_us * configTICK_RATE_HZ + 999999) / 1000000);
        vTaskDelay(ticks + 1);
    }
    int voltage_mv = 0;
    esp_err_t result = enable_sample_path();
    if (result == ESP_OK) result = wait_until_stable();
    if (result == ESP_OK) {
        int divided_voltage_mv = 0;
        result = read_average_divided_mv(divided_voltage_mv);
        if (result == ESP_OK) voltage_mv = apply_divider_scale(divided_voltage_mv);
    }
    const esp_err_t disable_result = disable_sample_path();
    if (result == ESP_OK) result = disable_result;
    if (request->callback) request->callback(result, voltage_mv, request->context);

    xSemaphoreTake(state_mutex, portMAX_DELAY);
    request->result = result;
    request->voltage_mv = voltage_mv;
    // 通知与空闲发布属于同一事务；请求池引用确保唤醒后仍读自己的结果。
    sampling = false;
    xSemaphoreGive(request->completion);
    xSemaphoreGive(sample_idle);
    --request->references; // 工作者不再访问本请求。
    xSemaphoreGive(state_mutex);
}

/** @brief 常驻 ADC 工作者，空闲时阻塞，无周期任务栈分配/释放。 */
void sampling_task(void*) {
    for (;;) {
        xSemaphoreTake(sample_ready, portMAX_DELAY);
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        SampleRequest* request = latest_request;
        xSemaphoreGive(state_mutex);
        perform_sample(request);
    }
}

/** @brief 提交请求；waiter 非空时，在同一锁内给同步调用者保留结果引用。 */
esp_err_t submit_sample(CompletionCallback callback, void* context, SampleRequest** waiter = nullptr) {
    if (!initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    if (sampling || sleep_paused) {
        xSemaphoreGive(state_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    SampleRequest* request = nullptr;
    for (auto& slot : requests) {
        if (slot.references == 0 || (&slot == latest_request && slot.references == 1)) {
            request = &slot;
            break;
        }
    }
    if (!request) {
        xSemaphoreGive(state_mutex);
        return ESP_ERR_NO_MEM; // 慢等待者占满池时拒绝新请求，不复用其结果。
    }
    if (latest_request) --latest_request->references;
    request->references = waiter ? 3 : 2;
    request->sequence = ++next_sequence;
    request->result = ESP_ERR_INVALID_STATE;
    request->voltage_mv = 0;
    request->callback = callback;
    request->context = context;
    xSemaphoreTake(request->completion, 0);
    xSemaphoreTake(sample_idle, 0);
    latest_request = request;
    sampling = true;
    if (waiter) *waiter = request;
    xSemaphoreGive(sample_ready);
    xSemaphoreGive(state_mutex);
    return ESP_OK;
}

/** @brief 等待所持引用对应的不可变结果，并释放引用（包括超时路径）。 */
esp_err_t await_sample(SampleRequest* request, int& voltage_mv, TickType_t wait) {
    const bool completed = xSemaphoreTake(request->completion, wait) == pdTRUE;
    if (completed) xSemaphoreGive(request->completion); // 多等待者观察同一完成状态。
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    const esp_err_t result = completed ? request->result : ESP_ERR_TIMEOUT;
    if (completed) voltage_mv = request->voltage_mv;
    --request->references;
    xSemaphoreGive(state_mutex);
    return result;
}

/** @brief 获取校准代际；保护局部窗口与持久化事务的时序。 */
uint32_t current_calibration_epoch() {
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    const uint32_t epoch = calibration_epoch;
    xSemaphoreGive(state_mutex);
    return epoch;
}

// 在锁内发布当前校准稳定窗口的进度，供 get_calibration_status() 查询。
void publish_calibration_window(uint16_t sample_count,
                                int min_voltage_mv,
                                int max_voltage_mv, uint32_t epoch) {
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    if (epoch != calibration_epoch || sleep_paused) {
        xSemaphoreGive(state_mutex);
        return;
    }
    calibration_stable_samples = sample_count;
    calibration_min_mv = min_voltage_mv;
    calibration_max_mv = max_voltage_mv;
    xSemaphoreGive(state_mutex);
}

// 将新的分压倍率打包成带校验的校准记录写入 NVS，成功后立即更新运行时的
// 倍率与有效标志。写 Flash 期间持锁，避免采样任务读到中间状态。
esp_err_t save_calibration(uint32_t new_scale_q16, uint32_t epoch) {
    CalibrationRecord record = DEFAULT_CALIBRATION;
    record.divider_scale_q16 = new_scale_q16;
    record.checksum = calibration_checksum(record);
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    if (sleep_paused || epoch != calibration_epoch) {
        xSemaphoreGive(state_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t persist_result = stored_calibration.set(record);
    if (persist_result != ESP_OK) {
        APP_LOGI("ProductEvent", "battery calibration save result=%s", esp_err_to_name(persist_result));
        xSemaphoreGive(state_mutex);
        return persist_result;
    }
    const uint32_t previous = divider_scale_q16;
    divider_scale_q16 = new_scale_q16;
    stored_calibration_valid = true;
    ++calibration_epoch;
    calibration_error = ESP_OK;
    APP_LOGI("ProductEvent", "battery calibration source=auto result=saved old_scale_q16=%lu new_scale_q16=%lu",
             static_cast<unsigned long>(previous), static_cast<unsigned long>(new_scale_q16));
    xSemaphoreGive(state_mutex);
    return ESP_OK;
}

// 自动校准任务主体。在 USB 连接期间周期采样电池电压，维护一个“稳定窗口”：
// 窗口内电压极差必须始终小于 CALIBRATION_MAX_SPREAD_MV，否则以当前样本为
// 新起点重开窗口。窗口累计到 CALIBRATION_REQUIRED_SAMPLES 时，认为充电
// 电压已稳定在 4.2 V 参考电压，据此反推并保存新的分压倍率。每个 USB 插入
// 周期成功保存后退出；失败最多重采完整窗口并尝试三次。
void calibration_monitor_task(void*) {
    APP_LOGI("ProductEvent", "battery calibration source=usb result=monitoring");
    uint16_t stable_samples = 0;
    int stable_min_mv = 0;
    int stable_max_mv = 0;
    uint32_t epoch = current_calibration_epoch();
    unsigned save_attempts = 0;
    const char* finish_reason = "usb_removed"; // 生命周期事件说明终止原因，不用于校准控制。

    // USB 掉线即结束监测；回调为空也视为不可继续。
    while (usb_connected_callback != nullptr && usb_connected_callback()) {
        vTaskDelay(pdMS_TO_TICKS(CALIBRATION_INTERVAL_MS));
        // 延时期间 USB 可能被拔出，醒来后再次确认。
        if (!usb_connected_callback()) {
            break;
        }

        const uint32_t observed_epoch = current_calibration_epoch();
        if (observed_epoch != epoch) {
            epoch = observed_epoch;
            stable_samples = 0; stable_min_mv = stable_max_mv = 0;
            save_attempts = 0;
        }
        int voltage_mv = 0;
        const esp_err_t result = read_mv(voltage_mv);
        if (current_calibration_epoch() != epoch) continue;
        // 采样正忙说明本次周期无结果，跳过但不重置已有稳定窗口。
        if (result == ESP_ERR_INVALID_STATE) {
            continue;
        }
        // 采样失败或电压未达到满电门槛，清空稳定窗口重新等待充电。
        if (result != ESP_OK || voltage_mv <= CALIBRATION_MIN_VOLTAGE_MV) {
            stable_samples = 0;
            stable_min_mv = 0;
            stable_max_mv = 0;
            publish_calibration_window(0, 0, 0, epoch);
            continue;
        }

        // 首个样本初始化窗口极值，后续样本扩展极值范围。
        if (stable_samples == 0) {
            stable_min_mv = voltage_mv;
            stable_max_mv = voltage_mv;
        } else {
            stable_min_mv = std::min(stable_min_mv, voltage_mv);
            stable_max_mv = std::max(stable_max_mv, voltage_mv);
        }
        ++stable_samples;

        // 极差超限说明电压仍在爬升/波动，以当前样本重开窗口。
        if (stable_max_mv - stable_min_mv >= CALIBRATION_MAX_SPREAD_MV) {
            // 当前样本作为新稳定窗口的起点，避免再等待一个完整采样周期。
            stable_samples = 1;
            stable_min_mv = voltage_mv;
            stable_max_mv = voltage_mv;
            publish_calibration_window(
                stable_samples, stable_min_mv, stable_max_mv, epoch);
            continue;
        }
        publish_calibration_window(
            stable_samples, stable_min_mv, stable_max_mv, epoch);
        // 窗口样本数未达标则继续下一轮采集。
        if (stable_samples < CALIBRATION_REQUIRED_SAMPLES) {
            continue;
        }

        // 以稳定窗口上下限的中点作为当前满电电压估计。
        const int stable_voltage_mv =
            (stable_min_mv + stable_max_mv + 1) / 2;
        // 取一致快照后按比例反推倍率：new = current × ref / stable。
        // current 为 Q16 值，故结果仍是 Q16；用 64 位中间量避免溢出。
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        const uint32_t current_scale_q16 = divider_scale_q16;
        xSemaphoreGive(state_mutex);
        const uint32_t new_scale_q16 = static_cast<uint32_t>(
            (static_cast<uint64_t>(current_scale_q16) *
             CALIBRATION_REFERENCE_MV +
             stable_voltage_mv / 2) /
            stable_voltage_mv);
        // 只有落在合理硬件范围内的结果才值得持久化，否则拒绝并告警。
        if (new_scale_q16 >= MIN_DIVIDER_SCALE_Q16 &&
            new_scale_q16 <= MAX_DIVIDER_SCALE_Q16) {
            const esp_err_t saved = save_calibration(new_scale_q16, epoch);
            if (saved != ESP_OK) {
                xSemaphoreTake(state_mutex, portMAX_DELAY);
                calibration_error = saved;
                xSemaphoreGive(state_mutex);
                ESP_LOGE(TAG, "calibration save failed: %s attempt=%u", esp_err_to_name(saved), save_attempts + 1);
                if (saved == ESP_ERR_INVALID_STATE) continue;
                // 最多三次写入；失败时不声明完成，每次重新采集完整稳定窗口。
                if (++save_attempts < 3) {
                    stable_samples = 0; stable_min_mv = stable_max_mv = 0;
                    publish_calibration_window(0, 0, 0, epoch);
                    continue;
                }
                finish_reason = "save_failed";
                break;
            }
            APP_LOGI("ProductEvent",
                     "full-charge calibration complete: stable=%d mV scale_q16=%lu",
                     stable_voltage_mv,
                     static_cast<unsigned long>(new_scale_q16));
            finish_reason = "saved";
        } else {
            finish_reason = "out_of_range";
            ESP_LOGW(TAG,
                     "calibration result out of range: stable=%d mV scale_q16=%lu",
                     stable_voltage_mv,
                     static_cast<unsigned long>(new_scale_q16));
        }
        // 每次 USB 插入周期只执行一次 Flash 写入。
        break;
    }

    // 退出前清理运行标志与对外可见的窗口状态，避免残留过期进度。
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    APP_LOGI("ProductEvent", "battery calibration monitor stopped reason=%s samples=%u failed_saves=%u",
             finish_reason, stable_samples, save_attempts);
    calibration_monitor_running = false;
    calibration_stable_samples = 0;
    calibration_min_mv = 0;
    calibration_max_mv = 0;
    xSemaphoreGive(state_mutex);
    vTaskDelete(nullptr);
}

} // namespace

// 关闭分压路径：把 GPIO10 设为输入且关闭上下拉，即高阻态，切断分压电流。
esp_err_t disable_sample_path() {
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << SAMPLE_ENABLE_GPIO;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&config);
}

// 使能分压路径：把 GPIO10 设为开漏输出并拉低，导通分压网络低边。
// 开漏而非推挽，保证与外部可能存在的上拉不冲突。
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

// 初始化采样模块：先确保分压关闭（默认低功耗），再初始化 ADC，读取
// NVS 校准记录，最后创建同步原语。重复调用直接返回成功。
esp_err_t init(int64_t earliest_sample_us) {
    if (initialized) {
        return ESP_OK;
    }

    // 上电默认关闭分压路径，避免意外的静态电流。
    esp_err_t ret = disable_sample_path();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = battery_adc.init();
    if (ret != ESP_OK) {
        return ret;
    }

    // 读取持久化校准；校验通过才采用其中的倍率，否则退回默认值。
    const CalibrationRecord record = stored_calibration.read();
    stored_calibration_valid = calibration_record_valid(record);
    divider_scale_q16 = stored_calibration_valid
                            ? record.divider_scale_q16
                            : DEFAULT_DIVIDER_SCALE_Q16;

    state_mutex = xSemaphoreCreateMutex();
    sample_ready = xSemaphoreCreateBinary();
    sample_idle = xSemaphoreCreateBinary();
    bool resources_ok = state_mutex && sample_ready && sample_idle;
    for (auto& slot : requests) {
        slot.completion = xSemaphoreCreateBinary();
        resources_ok = resources_ok && slot.completion;
    }
    if (resources_ok) {
        xSemaphoreGive(sample_idle);
        resources_ok = xTaskCreate(sampling_task, "battery_sample", 3072, nullptr, 3, &sample_worker) == pdPASS;
    }
    if (!resources_ok) {
        for (auto& slot : requests) {
            if (slot.completion) vSemaphoreDelete(slot.completion);
            slot.completion = nullptr;
        }
        if (state_mutex) vSemaphoreDelete(state_mutex);
        if (sample_ready) vSemaphoreDelete(sample_ready);
        if (sample_idle) vSemaphoreDelete(sample_idle);
        state_mutex = sample_ready = sample_idle = nullptr;
        return ESP_ERR_NO_MEM;
    }
    sample_not_before_us = earliest_sample_us;
    initialized = true;
    return ESP_OK;
}

esp_err_t start_async(CompletionCallback callback, void* context) {
    return submit_sample(callback, context);
}

esp_err_t wait_mv(int& voltage_mv, TickType_t ticks_to_wait) {
    if (!initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    SampleRequest* request = latest_request;
    if (request) ++request->references;
    xSemaphoreGive(state_mutex);
    return request ? await_sample(request, voltage_mv, ticks_to_wait) : ESP_ERR_INVALID_STATE;
}

bool prepare_sleep(TickType_t wait) {
    if (!initialized) return true;
    // 先关门再等待，避免 is_busy() 检查后又有新采样进入。
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    sleep_paused = true;
    ++calibration_epoch;
    xSemaphoreGive(state_mutex);
    const bool idle = xSemaphoreTake(sample_idle, wait) == pdTRUE;
    if (idle) xSemaphoreGive(sample_idle);
    return idle;
}

void cancel_sleep() {
    if (!initialized) return;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    sleep_paused = false;
    xSemaphoreGive(state_mutex);
}

void notify_external_power(bool connected) {
    if (!initialized) return;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    if (external_power != connected) {
        external_power = connected;
        ++calibration_epoch;
        calibration_stable_samples = 0;
        calibration_min_mv = calibration_max_mv = 0;
    }
    xSemaphoreGive(state_mutex);
}

// 查询是否有采样任务在运行；未初始化时视为空闲。
bool is_busy() {
    if (!initialized) {
        return false;
    }
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    const bool result = sampling;
    xSemaphoreGive(state_mutex);
    return result;
}

// 同步读取便捷接口：发起异步采样后阻塞等待其结果返回给调用者。
esp_err_t read_mv(int& voltage_mv) {
    SampleRequest* request = nullptr;
    const esp_err_t start_result = submit_sample(nullptr, nullptr, &request);
    return start_result == ESP_OK ? await_sample(request, voltage_mv, portMAX_DELAY) : start_result;
}

// 启动 USB 满电自动校准监测。要求模块已初始化且回调非空，重复启动返回
// ESP_ERR_INVALID_STATE。启动时清零稳定窗口，等待任务自行采集与判定。
esp_err_t start_calibration_monitor(UsbConnectedCallback usb_connected) {
    if (!initialized || usb_connected == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    // 在锁内检查并置位运行标志，防止并发重复启动。
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    if (calibration_monitor_running || sleep_paused) {
        xSemaphoreGive(state_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    usb_connected_callback = usb_connected;
    calibration_stable_samples = 0;
    calibration_min_mv = 0;
    calibration_max_mv = 0;
    calibration_monitor_running = true;
    xSemaphoreGive(state_mutex);
    // 校准任务优先级低于采样任务，避免抢占实时采样。
    if (xTaskCreate(calibration_monitor_task,
                    "battery_cal",
                    3072,
                    nullptr,
                    2,
                    nullptr) != pdPASS) {
        // 任务创建失败则撤销运行标志与回调，允许后续重试。
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        calibration_monitor_running = false;
        usb_connected_callback = nullptr;
        xSemaphoreGive(state_mutex);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// 在锁内把内部状态整体拷出为快照，供上层显示/诊断。
void get_calibration_status(CalibrationStatus& status) {
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    status = {
        .divider_scale_q16 = divider_scale_q16,
        .stored_calibration_valid = stored_calibration_valid,
        .monitor_running = calibration_monitor_running,
        .stable_sample_count = calibration_stable_samples,
        .stable_min_mv = calibration_min_mv,
        .stable_max_mv = calibration_max_mv,
        .last_error = calibration_error,
        .generation = calibration_epoch,
    };
    xSemaphoreGive(state_mutex);
}

// 恢复默认分压倍率：持久化一条校验无效的默认记录，同时刷新运行时倍率与
// 状态。记录保留默认魔数与版本，仅靠校验失败来表达“未校准”。
esp_err_t reset_calibration(const char* source) {
    source = source ? source : "unknown";
    CalibrationRecord record = DEFAULT_CALIBRATION;
    // 写入无效校验值表示显式恢复出厂倍率，重启后仍显示为“未校准”。
    record.checksum = 0;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    const esp_err_t persist_result = stored_calibration.set(record);
    if (persist_result != ESP_OK) {
        APP_LOGI("ProductEvent", "battery calibration reset source=%s result=%s", source, esp_err_to_name(persist_result));
        xSemaphoreGive(state_mutex);
        return persist_result;
    }
    const uint32_t previous = divider_scale_q16;
    divider_scale_q16 = DEFAULT_DIVIDER_SCALE_Q16;
    stored_calibration_valid = false;
    ++calibration_epoch;
    calibration_error = ESP_OK;
    calibration_stable_samples = 0;
    calibration_min_mv = 0;
    calibration_max_mv = 0;
    APP_LOGI("ProductEvent", "battery calibration reset source=%s result=saved old_scale_q16=%lu new_scale_q16=%lu",
             source, static_cast<unsigned long>(previous), static_cast<unsigned long>(divider_scale_q16));
    xSemaphoreGive(state_mutex);
    return ESP_OK;
}

} // namespace BatteryVoltage
