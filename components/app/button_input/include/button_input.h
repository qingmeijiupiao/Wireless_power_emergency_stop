/**
 * @file button_input.h
 * @brief 按键输入接口：读取 BOOT 键并经过消抖策略输出短按/长按手势。
 */
#pragma once
#include "button_policy.h"
namespace ButtonInput {
/** Suppress a BOOT key held during wake until it is released. */
// 初始化按键，并抑制唤醒时仍被按住的 BOOT 键，直到其释放，避免把唤醒动作识别成一次按键。
void init();
// 按当前时刻采样按键并返回本次产生的手势；无手势返回 Gesture::None。
Gesture poll(int64_t now_us);
} // namespace ButtonInput
