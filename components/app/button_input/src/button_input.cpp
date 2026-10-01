/**
 * @file button_input.cpp
 * @brief 实现按键采样：读取硬件 BOOT 键，经消抖策略与运行时可配置的时间参数输出手势。
 */
#include "button_input.h"
#include "hardware.h"
#include "runtime_settings.h"
namespace ButtonInput {
namespace {
Debouncer button;
}
// 初始化时若按键已被按下，则开启“释放前抑制”，避免把唤醒前的按住误判为手势。
void init() { button = Debouncer(Hardware::button_pressed()); }
Gesture poll(int64_t now_us) {
    // 消抖与长按阈值取自运行时可配置项，便于在不改代码的情况下调整手感。
    return button.update(Hardware::button_pressed(), now_us, RuntimeSettings::get("debounce_ms") * 1000LL,
                         RuntimeSettings::get("long_ms") * 1000LL);
}
} // namespace ButtonInput
