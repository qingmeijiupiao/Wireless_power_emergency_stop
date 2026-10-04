/**
 * @file input_actions.h
 * @brief 输入动作执行私有接口：将 UI 意图落为设置、无线或休眠请求。
 */
#pragma once

#include "emergency_ui.h"

namespace AppController {
/**
 * @brief 记录输入活动并执行本轮 UI 动作。
 * @param update UI 手势处理产生的动作、重试和活动标志。
 * @param now_us 当前 esp_timer 时间，单位微秒。
 * @return true 表示请求手动休眠，调用方应交给 SleepCoordinator 等待按键释放；
 *         false 表示没有手动休眠请求。
 * @note 仅在协调任务中调用。设置动作包含同步持久化；配对动作复查当前远端状态。
 *       本函数不直接进入深睡。
 */
bool dispatch_input(const EmergencyUi::Update &update, int64_t now_us);
} // namespace AppController
