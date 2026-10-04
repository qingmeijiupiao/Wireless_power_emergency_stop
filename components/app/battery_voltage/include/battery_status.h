/**
 * @file battery_status.h
 * @brief 产品电量同步接口：串行化采样结果、UI、诊断与 Shell 对 BatteryLevel 的访问。
 */
#pragma once

#include "battery_level.h"
#include "esp_err.h"

/**
 * 产品侧 BatteryLevel 访问入口。运行接口使用互斥锁，可等待，不允许在 ISR 中调用。
 * init() 必须先于所有运行期调用完成；产品代码不应绕过本接口直接访问 BatteryLevel。
 */
namespace BatteryStatus {
/**
 * @brief 创建电量互斥锁并触发有效 RTC 保留状态的恢复。
 * @pre 仅在启动阶段串行调用，早于协调任务和 Shell 的电量访问。
 * @retval ESP_OK 初始化成功或此前已初始化。
 * @retval ESP_ERR_NO_MEM 互斥锁创建失败。
 */
esp_err_t init();
/**
 * @brief 在互斥保护下更新公共电量估算和 RTC 保留状态。
 * @param voltage_mv 已校准的电池电压，单位 mV。
 * @param charging 当前是否使用外部供电，用于控制显示电量的变化方向。
 * @return 更新后的电量状态副本。
 */
BatteryLevel::Status update(int voltage_mv, bool charging);
/**
 * @brief 在互斥保护下读取最近有效电量状态。
 * @param[out] status 有效状态副本；返回 false 时调用方不应使用其字段。
 * @return true 表示存在有效状态，false 表示尚未取得采样或有效 RTC 状态。
 */
bool get_status(BatteryLevel::Status& status);
/** @brief 在互斥保护下清除运行期和 RTC 电量；下次有效采样重新建立估算基准。 */
void reset();
} // namespace BatteryStatus
