/**
 * @file emergency_ui.h
 * @brief 定义紧急停机控制器的显示与菜单交互接口：每个刷新周期向内注入输入和状态，向外返回动作与是否重绘。
 */
#pragma once
#include "emergency_remote.h"
#include "button_policy.h"
#include "sleep_policy.h"
namespace EmergencyUi {
// 菜单确认后向业务层上报的动作。成员顺序与菜单条目索引一致，因此条目号可直接转换得到对应动作。
enum class Action { None, AlwaysOn, Sleep, SleepTime, Pair, Repair };
// 单次输入处理的结果：请求系统执行的重试、是否产生用户活动（用于阻止休眠），以及休眠时长选择结果。
struct Update {
    Action action = Action::None;
    bool retry = false, activity = false;
    int sleep_choice = 0;
};
// 每个刷新周期由业务层组装的输入快照：远端状态、电量、休眠计划、供电方式与当前时刻。
struct Model {
    EmergencyRemote::Snapshot remote;
    int battery_mv = 0, battery_percent = -1;  // battery_percent 为 -1 表示百分比未知
    PowerManager::SleepPlan sleep;
    bool usb = false, always_on = false;  // usb：检测到外部供电；always_on：常亮模式
    int64_t now_us = 0;
};
// 复位菜单与提示状态、恢复持久化的故障历史并初始化显示；不负责硬件或休眠编排。
void init();
// 处理一次按键手势，推进菜单/提示状态机并返回需要业务层执行的动作；gesture 为 None 表示无按键。
Update handle_input(const EmergencyRemote::Snapshot &remote, ButtonInput::Gesture gesture, int64_t now_us);
// 观察远端状态变化（如进入保护、断连）并据此触发故障/低电提示，返回本周期是否产生用户可见活动。
bool observe_state(const Model &model);
// 按页面选择优先级绘制一帧；force_dirty 为真时强制重绘。
void render(const Model &model, bool dirty);
// 直接切换到消息页显示编号 message 的文本；touch 为真时同时刷新菜单空闲计时起点。
void show_message(int message, int64_t now_us, bool touch = false);
// 返回最近一次故障提示对应的页面编号，无提示时为 -1。
int last_fault_page();
// 返回所有提示（故障提示与低电提示）中最晚的截止时刻，供休眠调度等待提示完整展示。
int64_t notice_deadline();
// 判断此刻是否允许进入休眠提示：没有正在保持的故障提示，且低电提示已结束。
bool sleep_notice_allowed(int64_t now_us);
// 进入休眠前在屏幕上绘制指定画面，避免屏幕停留在过期数据。
void prepare_sleep();
// 关闭显示。
void shutdown();
// 初始化显示并强制下一帧重绘。
void restore();
} // namespace EmergencyUi
