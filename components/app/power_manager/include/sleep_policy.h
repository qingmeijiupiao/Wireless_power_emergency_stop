/**
 * @file sleep_policy.h
 * @brief 深睡准入判定与静置倒计时策略（与硬件无关的纯逻辑）。
 */
#pragma once
#include <cstdint>
namespace PowerManager {
// 睡眠阻塞原因，按优先级从高到低排列；None 表示当前允许进入深睡。
enum class SleepBlock { None, Usb, OutputOn, Unknown, Busy, Button };
// 纯函数：按固定优先级判断当前是否被阻塞。USB 接入优先于输出，输出优先于状态未知，
// 状态未知优先于忙，最后是按钮按下；返回 None 才允许进入深睡。
inline SleepBlock sleep_block(bool usb, bool output_on, bool known, bool busy, bool button) {
    if (usb)
        return SleepBlock::Usb;
    if (output_on)
        return SleepBlock::OutputOn;
    if (!known)
        return SleepBlock::Unknown;
    if (busy)
        return SleepBlock::Busy;
    if (button)
        return SleepBlock::Button;
    return SleepBlock::None;
}
// 一次倒计时评估的结果：是否正在倒计时、是否到点应睡，以及剩余秒数（向上取整）。
struct SleepPlan {
    bool countdown = false, due = false;
    uint32_t seconds = 0;
};
// 静置倒计时状态机：started<0 表示尚未开始；允许条件撤销时立即复位。
struct SleepCountdown {
    int64_t started = -1;
    SleepPlan update(int64_t now, int64_t deadline, bool allowed) {
        if (!allowed || now < deadline - 60000000) {
            started = -1;
            return {};
        }
        if (started < 0)
            started = now;
        // 即使某个睡眠阻塞在静置截止后才解除，也要给用户完整一分钟的可见提醒，
        // 再进入深睡，避免屏幕已经关闭却毫无预警地断电。
        if (deadline < started + 60000000)
            deadline = started + 60000000;
        if (now >= deadline)
            return {false, true, 0};
        return {true, false, static_cast<uint32_t>((deadline - now + 999999) / 1000000)};
    }
};

} // namespace PowerManager
