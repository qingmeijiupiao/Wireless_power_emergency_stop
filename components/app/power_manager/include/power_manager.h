/**
 * @file power_manager.h
 * @brief 电源管理对外接口：睡眠阻塞判定、静置倒计时与深睡进入流程。
 */
#pragma once
#include "sleep_policy.h"
namespace PowerManager {
// 显示相关回调，由上层注入：深睡前 prepare/shutdown，睡眠中止后 restore。
struct DisplayHooks {
    void (*prepare)();
    bool (*shutdown)(); /**< true 表示显示执行任务已释放总线，可隔离 GPIO。 */
    void (*restore)();
};
/** 调用时机：GPIO 初始化之后、无线初始化启动前；消耗 RTC 睡眠标记判断是否由触点释放唤醒。 */
bool consume_release_wake();
// 返回当前睡眠阻塞原因（USB/输出/状态未知/忙/按钮），见 sleep_policy.h。
SleepBlock block();
// 初始化静置计时基准与倒计时状态。
void init_idle(int64_t now_us);
// 记录一次用户活动，刷新静置起点。
void note_activity(int64_t now_us);
// 评估静置与提醒截止时间，产出倒计时方案；failed 用于 always_on 模式下兜底允许睡眠。
SleepPlan plan(int64_t now_us, int64_t notice_deadline, bool notice_allowed, bool failed);
/** 可能不返回。在关闭显示与同步日志之后再次复查输入/输出安全，再进入深睡。 */
int enter_sleep(const DisplayHooks &display, bool manual);
} // namespace PowerManager
