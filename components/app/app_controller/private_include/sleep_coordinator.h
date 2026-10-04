/**
 * @file sleep_coordinator.h
 * @brief 休眠协调私有接口：手动请求等待按键释放，自动请求遵守 UI 提示截止时间。
 */
#pragma once

#include "emergency_ui.h"

namespace AppController {
/**
 * @brief 协调 UI 提示与手动/自动休眠请求。
 * @note 全部接口仅由协调任务调用；休眠安全检查及深睡实现由 PowerManager 完成。
 */
class SleepCoordinator {
public:
    /** @brief 登记手动休眠意图，重复请求合并为一个待处理请求。 */
    void request_manual() { manual_pending_ = true; }
    /**
     * @brief 按住按键时提示释放，释放后消费请求并尝试深睡。
     * @param now_us 当前 esp_timer 时间，单位微秒。
     * @return true 表示休眠被拒绝且已显示原因，应强制重绘；false 表示未尝试深睡。
     * @note 成功进入深睡不会返回；拒绝后请求已消费，不会每轮重复执行。
     */
    bool process_manual(int64_t now_us);
    /**
     * @brief 根据当前模型和已更新的 UI 提示评估自动休眠计划。
     * @param model 本轮 UI 模型，使用其中的时间和连接失败标志。
     * @return PowerManager 计算的倒计时、到期标志和剩余秒数。
     * @pre 本轮 EmergencyUi::observe_state() 已执行，提示截止时间已更新。
     */
    PowerManager::SleepPlan plan(const EmergencyUi::Model &model) const;
    /**
     * @brief 自动计划到期时尝试深睡，被拒绝后刷新静置基准。
     * @param plan 本轮的自动休眠计划。
     * @param now_us 当前 esp_timer 时间，单位微秒。
     * @return true 表示深睡调用返回，需要重绘；false 表示计划尚未到期。
     * @note 成功进入深睡不会返回。
     */
    bool process_auto(const PowerManager::SleepPlan &plan, int64_t now_us);

private:
    bool manual_pending_ = false; /**< 尚未消费的手动请求，只等待按键释放，不代表可安全休眠。 */
};
} // namespace AppController
