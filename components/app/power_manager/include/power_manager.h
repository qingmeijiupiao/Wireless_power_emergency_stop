/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: 低功耗启动判定与深度休眠管理
 * @author: qingmeijiupiao
 * @LastEditTime: 2026-06-07
 */
#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <cstdint>

#include "esp_err.h"

namespace PowerManager {

constexpr uint32_t LONG_PRESS_MS = 1000;

enum class WakeSource {
    POWER_ON,
    BUTTON,
    USB,
    BUTTON_AND_USB,
    OTHER,
};

/**
 * @brief 初始化 GPIO4 按键和 GPIO5 USB 检测输入
 */
esp_err_t init();

/** @brief 获取本次启动的唤醒来源。 */
WakeSource wake_source();

/** @brief GPIO5 为高电平时返回 true。 */
bool usb_connected();

/** @brief GPIO4 为低电平时返回 true。 */
bool button_pressed();

/** @brief 返回从 app_main 开始到当前的毫秒数，用于唤醒按键计时。 */
uint32_t button_press_elapsed_ms();

/**
 * @brief 配置 GPIO4 低电平和 GPIO5 高电平唤醒并进入深度休眠
 *
 * GPIO4 未释放或 GPIO5 仍为高电平时返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t enter_deep_sleep();

} // namespace PowerManager

#endif
