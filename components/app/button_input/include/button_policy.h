/**
 * @file button_policy.h
 * @brief 按键消抖与手势判定策略：按住时长不足判为短按，达到阈值判为长按。
 */
#pragma once
#include <cstdint>
namespace ButtonInput {
// 按键手势：None 无；Short 短按（在长按阈值前释放）；Long 长按（按住达到阈值，仅上报一次）。
enum class Gesture { None, Short, Long };
// 消抖器：把带抖动的原始电平整理为稳定电平，并按按下时长输出手势。
class Debouncer {
    bool raw_ = false, stable_ = false, held_ = false, suppress_ = false;
    int64_t changed_ = 0, pressed_ = 0; // changed_ 最近一次原始电平变化时刻；pressed_ 稳定按下时刻

  public:
    // suppress_until_release 为真时，在按键释放前不产生任何手势，用于屏蔽唤醒时的长按。
    explicit Debouncer(bool suppress_until_release = false) : suppress_(suppress_until_release) {}
    // down 为原始按键电平，now 为当前时刻；debounce_us 消抖时间，long_us 长按阈值。
    Gesture update(bool down, int64_t now, int64_t debounce_us = 30000, int64_t long_us = 1000000) {
        // 记录原始电平变化并重置消抖计时起点。
        if (down != raw_) {
            raw_ = down;
            changed_ = now;
        }
        // 电平尚未稳定超过消抖时间：忽略本次采样。
        if (now - changed_ < debounce_us)
            return Gesture::None;
        // 唤醒抑制期间：保持跟踪稳定电平，直到检测到释放才解除抑制；期间不产生手势。
        if (suppress_) {
            if (!down)
                suppress_ = false;
            stable_ = down;
            return Gesture::None;
        }
        // 稳定电平发生变化：若是按下则记录时刻并清除长按标记；若是释放且此前未上报长按，则判为短按。
        if (stable_ != down) {
            stable_ = down;
            if (down) {
                pressed_ = now;
                held_ = false;
            } else if (!held_)
                return Gesture::Short;
        }
        // 持续按住达到长按阈值且尚未上报：上报一次长按，held_ 防止重复触发。
        if (stable_ && !held_ && now - pressed_ >= long_us) {
            held_ = true;
            return Gesture::Long;
        }
        return Gesture::None;
    }
};

} // namespace ButtonInput
