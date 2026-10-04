/**
 * @file button_input.h
 * @brief UI 内部按键适配：基于公共 Button 组件识别 GPIO3 短按/长按并投递到手势队列。
 */
#pragma once
#include "core/ui_types.h"
#include "esp_err.h"
namespace EmergencyUi {
namespace Buttons {
/** @brief 初始化 GPIO3 上的公共 Button 并准备手势队列。 */
esp_err_t init();
/** @return 本次启动因队列已满丢弃的手势数量。 */
uint32_t dropped();
/**
 * @brief 非阻塞读取一个手势。
 * @param gesture 输出手势；无手势时保持不变。
 * @return true 表示取到手势。
 */
bool poll(Gesture &gesture);
} // namespace Buttons
} // namespace EmergencyUi
