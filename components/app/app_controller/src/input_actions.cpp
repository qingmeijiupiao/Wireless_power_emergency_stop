/**
 * @file input_actions.cpp
 * @brief UI 业务动作落地：参数持久化、连接重试、配对入口检查和休眠意图转交。
 */
#include "input_actions.h"
#include "app_diagnostics.h"

#include "emergency_remote.h"
#include "hardware.h"
#include "power_manager.h"
#include "runtime_settings.h"
#include "ui_messages.h"

namespace AppController {
namespace {
constexpr int64_t kPairAfterOffUs = 3000000; // 插电时允许利用已确认关断后的 3 秒窗口进入配对。

/**
 * @brief 判断当前是否允许提交配对请求。
 * @param remote 执行动作时重新取得的远端快照。
 * @param now_us 当前 esp_timer 时间，单位微秒。
 * @return 未配对、无休眠阻塞，或插电且输出已确认关闭、无待办并在 3 秒窗口内时为 true。
 * @note 只决定配对入口；配对执行和控制事务由 EmergencyRemote 的工作线程处理。
 */
bool pairing_allowed(const EmergencyRemote::Snapshot &remote, int64_t now_us) {
    return !remote.paired || PowerManager::block() == PowerManager::SleepBlock::None ||
           (Hardware::usb_connected() && !remote.output_on && !remote.busy && remote.output_time_us > 0 &&
            now_us - remote.output_time_us < kPairAfterOffUs);
}
} // namespace

bool dispatch_input(const EmergencyUi::Update &update, int64_t now_us) {
    if (update.activity)
        PowerManager::note_activity(now_us);
    if (update.retry) {
        APP_LOGI("ProductEvent", "connection retry source=ui result=submitted");
        EmergencyRemote::retry_connection();
    }

    using EmergencyUi::Action;
    using namespace UiMessages;
    switch (update.action) {
    case Action::None:
        break;
    case Action::AlwaysOn: {
        const bool saved = RuntimeSettings::set_always_on(!RuntimeSettings::always_on(), "ui");
        EmergencyUi::show_message(saved ? kAlwaysOnSaved : kSaveFailed, now_us);
        break;
    }
    case Action::SleepTime: {
        constexpr int choice_count =
            sizeof(RuntimeSettings::kSleepTimesMs) / sizeof(RuntimeSettings::kSleepTimesMs[0]);
        // 在索引数组前校验 UI 选择，非法值统一反馈保存失败。
        const bool saved = update.sleep_choice >= 0 && update.sleep_choice < choice_count &&
                           RuntimeSettings::set(RuntimeSettings::Id::IdleMs, RuntimeSettings::kSleepTimesMs[update.sleep_choice], "ui");
        EmergencyUi::show_message(saved ? kSleepTimeSaved : kSaveFailed, now_us);
        break;
    }
    case Action::Sleep:
        // 不在动作分发中关屏或深睡；交给休眠协调器等待按键释放。
        return true;
    case Action::Pair:
    case Action::Repair:
        if (pairing_allowed(EmergencyRemote::snapshot(), now_us)) {
            APP_LOGI("ProductEvent", "pairing source=ui action=%s result=submitted", update.action == Action::Repair ? "repair" : "pair");
            EmergencyRemote::start_pairing(update.action == Action::Repair);
            EmergencyUi::show_message(kPairStarted, now_us);
        } else {
            APP_LOGI("ProductEvent", "pairing source=ui result=denied reason=unsafe_output_or_pending");
            EmergencyUi::show_message(kPairBlocked, now_us);
        }
        break;
    }
    return false;
}
} // namespace AppController
