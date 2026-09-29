/*
 * @version: 2.0
 * @LastEditors: qingmeijiupiao
 * @Description: GPIO10 低有效状态灯应用接口
 * @author: qingmeijiupiao
 * @LastEditTime: 2026-06-07
 */
#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <cstdint>

#include "esp_err.h"

namespace StatusLed {

/** @brief 初始化 GPIO10，默认高电平熄灭。 */
esp_err_t init();

/** @brief 点亮状态灯，GPIO10 输出低电平。 */
esp_err_t on();

/** @brief 熄灭状态灯，GPIO10 输出高电平。 */
esp_err_t off();

/** @brief 阻塞闪烁指定次数，结束时保持熄灭。 */
esp_err_t blink(uint32_t count, uint32_t on_ms, uint32_t off_ms);

/** @brief 熄灭 LED 后将 GPIO10 切换为无内部拉的高阻状态。 */
esp_err_t prepare_for_sleep();

} // namespace StatusLed

#endif
