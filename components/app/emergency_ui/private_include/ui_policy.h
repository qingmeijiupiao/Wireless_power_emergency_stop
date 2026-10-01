/**
 * @file ui_policy.h
 * @brief 界面策略：菜单视图状态机、休眠时长选择、故障提示的保持/历史逻辑，以及提示与确认的状态流转。
 */
#pragma once
#include "emergency_ui.h"
#include "runtime_settings.h"
namespace UiPolicy {
using Gesture = ButtonInput::Gesture;
// 菜单视图状态。Home 为主页；Menu 为条目列表；Confirm 为条目确认页；
// Info 为设备信息多页浏览；Message 为一次性消息页；SleepTime 为休眠时长选择页。
enum class View { Home, Menu, Confirm, Info, Message, SleepTime };
using RuntimeSettings::kSleepTimesMs;
// 把当前配置的休眠毫秒值映射回 0..3 的选择项下标；不匹配时回退到第 0 项。
inline int sleep_time_index(uint32_t value) {
    for (int i = 0; i < 4; ++i)
        if (kSleepTimesMs[i] == value)
            return i;
    return 0;
}
// 故障提示状态机：负责在新故障出现时开始保持、故障消失后转为“历史提示”，
// 并保证一次新的 ON 尝试会取代上一次被拒绝的故障提示。
struct FaultNotice {
    int page = -1;         // 当前提示的故障页编号，-1 表示无
    int observed = -1;     // 上一次观察到的实时故障页，用于检测变化
    bool history = false;  // 当前展示的是已消失故障的历史提示
    int64_t until = 0;     // 提示保持截止时刻
    uint32_t on_attempt = 0; // 最近的 ON 尝试计数，用于识别新的用户请求
    // 深睡唤醒后用持久化的页面恢复为历史提示，并按 hold 设定保持时长。
    void restore(int saved_page, int64_t now, int64_t hold) {
        page = saved_page;
        observed = -1;
        history = true;
        until = now + hold;
    }
    // 用当前实时故障页 current 更新状态。attempt 变化表示用户发起了新的 ON 请求。
    // 返回 true 表示出现了需要立即展示的新故障（调用方据此刷新与记录）。
    bool update(int current, int64_t now, int64_t hold, uint32_t attempt = 0) {
        if (attempt != on_attempt) {
            // 用户新的 ON 请求取代旧的拒绝提示：即使启动阶段在下一次显示刷新前已结束也要重新判定。
            on_attempt = attempt;
            observed = -1;
            history = page >= 0;
            until = 0;
        }
        // 首次观察到某故障页，或该页正作为历史提示展示时重新出现，都视为“变化”。
        const bool changed = current >= 0 && (current != observed || history);
        observed = current;
        if (changed) {
            page = current;
            history = false;
            until = now + hold;
        } else if (current < 0 && page >= 0)
            // 故障已消失但页面仍在：保留为历史提示，直到保持期结束。
            history = true;
        return changed;
    }
    void dismiss() { until = 0; }
    bool holding(int64_t now) const { return until > now; }
};
using Action = EmergencyUi::Action;
// 菜单状态机。selected 为当前条目号（0..6），info_page 为信息页下标（0..3），
// sleep_choice 为休眠时长选项，stage 用于“修复/配对”等需要二次确认的条目，confirm 为确认页勾选状态。
struct Menu {
    View view = View::Home;
    int selected = 0, stage = 0, info_page = 0, sleep_choice = 0;
    bool confirm = false;
    int64_t touched = 0; // 最近一次按键时刻，用于空闲自动返回主页
    // 回到主页并清空条目、阶段与确认状态。
    void home() {
        view = View::Home;
        selected = stage = 0;
        confirm = false;
    }
    // 处理一次手势并推进状态机，返回需要业务层执行的动作；无动作返回 None。
    // idle_us 为菜单空闲超时，超时后自动回到主页。
    Action update(Gesture gesture, int64_t now, int64_t idle_us = 15000000) {
        // 非主页停留超时则自动回主页，防止误触后长期停留在菜单。
        if (view != View::Home && now - touched > idle_us)
            home();
        if (gesture == Gesture::None)
            return Action::None;
        touched = now;
        if (view == View::Home) {
            // 主页任意按键进入菜单，从第 0 项开始。
            view = View::Menu;
            selected = 0;
        } else if (view == View::Info) {
            // 信息页：短按循环切换 4 个子页，长按返回主页。
            if (gesture == Gesture::Short)
                info_page = (info_page + 1) % 4;
            else
                home();
        } else if (view == View::SleepTime) {
            // 休眠时长页：短按循环选择项，长按确认并上报 SleepTime 动作。
            if (gesture == Gesture::Short)
                sleep_choice = (sleep_choice + 1) % 4;
            else {
                home();
                return Action::SleepTime;
            }
        } else if (view == View::Message)
            // 消息页任意按键返回主页。
            home();
        else if (view == View::Menu) {
            // 菜单列表：短按上移选择（循环 7 项）；长按按条目分派。
            if (gesture == Gesture::Short)
                selected = (selected + 1) % 7;
            else if (!selected)
                // 第 0 项作为返回项，长按回主页。
                home();
            else if (selected == 6) {
                // 第 6 项进入设备信息页。
                view = View::Info;
                info_page = 0;
            } else if (selected == 3)
                // 第 3 项进入休眠时长选择页。
                view = View::SleepTime;
            else {
                // 其余条目进入确认页；确认页暂不勾选、阶段归零。
                view = View::Confirm;
                confirm = false;
                stage = 0;
            }
        } else if (gesture == Gesture::Short)
            // 确认页：短按切换勾选状态。
            confirm = !confirm;
        else if (!confirm) {
            // 未勾选时长按：取消并返回菜单列表。
            view = View::Menu;
        } else if (selected == 5 && stage == 0) {
            // 第 5 项需要二次确认：第一次长按进入第二阶段，要求用户再次确认。
            stage = 1;
            confirm = false;
        } else {
            // 已勾选且满足确认条件：上报与条目号对应的动作并返回主页。
            const auto action = static_cast<Action>(selected);
            home();
            return action;
        }
        return Action::None;
    }
};
} // namespace UiPolicy
