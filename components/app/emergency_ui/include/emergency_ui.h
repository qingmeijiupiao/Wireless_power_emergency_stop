/**
 * @file emergency_ui.h
 * @brief 急停控制器 UI 公开接口：屏幕初始化、按键输入处理、状态观察与渲染。
 */
#pragma once
#include "remote_snapshot.h"
#include "sleep_policy.h"
#include "esp_err.h"
namespace EmergencyUi {
/** 菜单或提示确认后需要业务层执行的动作。 */
enum class Action { None, AlwaysOn, Sleep, SleepTime, Pair, Repair };
/** 一次输入处理的结果，由主循环据此驱动业务动作与静置计时。 */
struct Update {
    Action action = Action::None; /**< 需要执行的动作 */
    bool retry = false;           /**< 是否请求重新连接远端 */
    bool activity = false;        /**< 是否视为用户活动 */
    int sleep_choice = 0;         /**< 休眠时长选择下标 */
};
/** 渲染与状态判定所需的快照模型。 */
struct Model {
    EmergencyRemote::Snapshot remote;      /**< 远端控制与测量快照 */
    int battery_mv = 0;                    /**< 电池电压，mV；0 表示无效 */
    int battery_percent = -1;              /**< 显示电量百分比；-1 表示未知 */
    PowerManager::SleepPlan sleep;         /**< 本次计算的休眠计划 */
    bool usb = false;                      /**< 是否检测到外部供电 */
    bool always_on = false;                /**< 是否常亮模式 */
    int64_t now_us = 0;                    /**< 当前时间，微秒 */
};
/** @brief 初始化 GPIO3 按键与手势队列。 */
esp_err_t init_buttons();
/** @return 启动以来队列满导致丢弃的 UI 手势数量。 */
uint32_t dropped_gestures();
/** @brief 初始化 UI 状态与故障历史；屏幕由 AppController 任务随后调用 restore() 初始化。 */
void init();
/**
 * @brief 消费按键手势并推进 UI 状态机，同时刷新故障提示与低电状态。
 * @param remote 处理开始时的远端快照
 * @param tick 当前时间，微秒
 * @return 本次处理产生的动作与活动标记
 */
Update handle_input(const EmergencyRemote::Snapshot &remote, int64_t tick);
/**
 * @brief 观察远端与电池状态，触发状态跳变与低电提示。
 * @return true 表示发生了应计为用户活动的变化
 */
bool observe_state(const Model &model);
/**
 * @brief 按当前状态渲染一帧，零等待提交到 OLED 最新帧邮箱。
 * @param model 渲染模型
 * @param dirty 是否有强制重绘请求
 */
void render(const Model &model, bool dirty);
/** @brief 显示一条一次性消息页。 */
void show_message(int message, int64_t now_us, bool touch = false);
/** @brief 返回最近一次记录的故障页编号。 */
int last_fault_page();
/** @brief 返回故障/低电提示的最晚保持截止时刻。 */
int64_t notice_deadline();
/** @brief 判断当前是否允许开始休眠倒计时（不在提示保持期内）。 */
bool sleep_notice_allowed(int64_t now_us);
/** @brief 进入休眠前绘制“即将休眠”画面。 */
void prepare_sleep();
/** @brief 关闭新帧入口并等待 OLED 任务释放总线；失败时不得隔离显示 GPIO。 */
bool shutdown();
/** @brief 启动新显示代际，异步恢复屏幕并请求完整重绘。 */
void restore();
} // namespace EmergencyUi
