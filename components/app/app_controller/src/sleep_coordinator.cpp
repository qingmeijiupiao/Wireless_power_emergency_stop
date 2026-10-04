/**
 * @file sleep_coordinator.cpp
 * @brief 休眠协调实现：按键释放等待、提示截止时间接入和显示钩子绑定。
 */
#include "sleep_coordinator.h"

#include "esp_log.h"
#include "hardware.h"
#include "power_manager.h"
#include "ui_messages.h"

namespace AppController {
namespace {
constexpr char kTag[] = "SleepCoordinator";
// 钩子在协调任务中提交，OLED 驱动由执行任务处理；shutdown 返回静止确认。
constexpr PowerManager::DisplayHooks kDisplayHooks{
    EmergencyUi::prepare_sleep, EmergencyUi::shutdown, EmergencyUi::restore};
} // namespace

bool SleepCoordinator::process_manual(int64_t now_us) {
    if (!manual_pending_)
        return false;
    if (Hardware::button_pressed()) {
        EmergencyUi::show_message(UiMessages::kReleaseButton, now_us, true);
        return false;
    }
    // 先消费请求；拒绝后不在每一帧重复尝试。
    manual_pending_ = false;
    // 成功深睡不会返回；能够执行后续代码意味着安全检查拒绝或睡前流程中止。
    const int reason = PowerManager::enter_sleep(kDisplayHooks, true);
    ESP_LOGI(kTag, "MANUAL_SLEEP_DENIED reason=%d", reason);
    EmergencyUi::show_message(reason, now_us, true);
    return true;
}

PowerManager::SleepPlan SleepCoordinator::plan(const EmergencyUi::Model &model) const {
    return PowerManager::plan(model.now_us, EmergencyUi::notice_deadline(),
                              EmergencyUi::sleep_notice_allowed(model.now_us), model.remote.connection_failed);
}

bool SleepCoordinator::process_auto(const PowerManager::SleepPlan &plan, int64_t now_us) {
    if (!plan.due)
        return false;
    (void)PowerManager::enter_sleep(kDisplayHooks, false);
    // 自动休眠被拒绝时重新起算静置时间，避免持续重复执行睡前握手。
    PowerManager::note_activity(now_us);
    return true;
}
} // namespace AppController
