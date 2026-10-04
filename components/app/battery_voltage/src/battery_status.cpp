/**
 * @file battery_status.cpp
 * @brief 产品电量同步封装实现：使用 FreeRTOS 互斥锁保护公共 BatteryLevel 状态。
 */
#include "battery_status.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace BatteryStatus {
namespace {
SemaphoreHandle_t mutex = nullptr; // 初始化成功后保留至重启，所有运行接口共用。
} // namespace

// 仅在启动阶段调用，早于采样、UI 和 Shell 任务。
esp_err_t init() {
    if (mutex != nullptr)
        return ESP_OK;
    mutex = xSemaphoreCreateMutex();
    if (mutex == nullptr)
        return ESP_ERR_NO_MEM;
    // 公共组件首次读取可能恢复 RTC 数据，在线程并发访问之前完成这一步。
    BatteryLevel::Status status{};
    (void)BatteryLevel::get_status(status);
    return ESP_OK;
}

BatteryLevel::Status update(int voltage_mv, bool charging) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    const auto status = BatteryLevel::update(voltage_mv, charging);
    xSemaphoreGive(mutex);
    return status;
}

bool get_status(BatteryLevel::Status& status) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    const bool valid = BatteryLevel::get_status(status);
    xSemaphoreGive(mutex);
    return valid;
}

void reset() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    BatteryLevel::reset();
    xSemaphoreGive(mutex);
}
} // namespace BatteryStatus
