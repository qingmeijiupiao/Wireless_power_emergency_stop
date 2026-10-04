/**
 * @file app_controller.h
 * @brief 产品协调器公开接口：异步发起采样并创建运行期协调任务。
 */
#pragma once

#include "esp_err.h"

namespace AppController {
/** 产品事件标签，与启动入口的黑匣子白名单共用。 */
inline constexpr char kEventTag[] = "ProductEvent";

/**
 * @brief 发起首次异步采样并启动产品协调任务。
 *
 * @pre 硬件、运行参数、BatteryStatus、BatteryVoltage、EmergencyRemote 和 UI 状态
 *      已初始化。仅由启动任务串行调用一次，不支持运行期停止或重启。
 * @note 调用方不等待 ADC 采样完成或 OLED 初始化；采样启动失败通过邮箱交给协调任务重试。
 *       UI 状态、显示驱动和睡前显示钩子由协调任务独占。
 * @retval ESP_OK 邮箱和协调任务已创建；不代表首次采样或 OLED 初始化成功。
 * @retval ESP_ERR_INVALID_STATE 已调用成功，或此前调用已创建邮箱；不能重复启动。
 * @retval ESP_ERR_NO_MEM 邮箱或协调任务创建失败，应由启动入口处理致命错误。
 */
esp_err_t start();
} // namespace AppController
