/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: 电池电压采集应用适配
 * @author: qingmeijiupiao
 * @LastEditTime: 2026-06-07 12:38:39
 */
#ifndef BATTERY_VOLTAGE_H
#define BATTERY_VOLTAGE_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

namespace BatteryVoltage {

using CompletionCallback = void (*)(esp_err_t result, int voltage_mv, void* context);
using UsbConnectedCallback = bool (*)();

struct CalibrationStatus {
    uint32_t divider_scale_q16;
    bool stored_calibration_valid;
    bool monitor_running;
    uint16_t stable_sample_count;
    int stable_min_mv;
    int stable_max_mv;
};

/**
 * @brief 初始化 GPIO3 ADC，并确保 GPIO2 采样开关保持高阻
 */
esp_err_t init();

/** @brief 开漏拉低 GPIO2，使能电池分压采样路径。 */
esp_err_t enable_sample_path();

/** @brief 将 GPIO2 恢复为无上下拉输入高阻。 */
esp_err_t disable_sample_path();

/**
 * @brief 启动一次异步电池电压采样
 *
 * 后台任务先等待 RC 分压网络稳定，再执行多次采样并取平均。完成后立即
 * 将 GPIO2 恢复高阻，并在后台任务上下文调用 callback。
 */
esp_err_t start_async(CompletionCallback callback = nullptr, void* context = nullptr);

/**
 * @brief 等待当前异步采样完成并取得结果
 *
 * @param voltage_mv 输出电池电压，单位 mV
 * @param ticks_to_wait 最大等待时间
 */
esp_err_t wait_mv(int& voltage_mv, TickType_t ticks_to_wait = portMAX_DELAY);

/** @brief 当前有异步采样任务运行时返回 true。 */
bool is_busy();

/**
 * @brief 同步兼容接口，内部通过 start_async() 和 wait_mv() 完成。
 */
esp_err_t read_mv(int& voltage_mv);

/**
 * @brief 启动 USB 满电电压自动校准任务
 *
 * USB 持续连接且电池电压高于 4.0 V 时，每 10 秒采样一次。连续 10 分钟内
 * 电压极差小于 5 mV，则认为充电已稳定在 4.2 V，并将新的分压倍率写入 NVS。
 *
 * @param usb_connected 查询 USB 插入状态的无阻塞回调
 */
esp_err_t start_calibration_monitor(UsbConnectedCallback usb_connected);

/** @brief 读取当前倍率和自动校准任务状态。 */
void get_calibration_status(CalibrationStatus& status);

/** @brief 删除校准结果并恢复默认 2.0 倍分压倍率。 */
esp_err_t reset_calibration();

} // namespace BatteryVoltage

#endif
