/**
 * @file input_actions.cpp
 * @brief UI 业务动作落地：参数持久化、连接重试、配对入口与休眠意图转交。
 */
#include "input_actions.h"
#include "app_diagnostics.h"

#include "emergency_remote.h"
#include "power_manager.h"
#include "runtime_settings.h"
#include "ui_messages.h"

namespace AppController {

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
        // 允许已配对时重新配对：成功才替换旧绑定，失败保留；是否受理由工作线程按事务状态判定。
        APP_LOGI("ProductEvent", "pairing source=ui action=pair result=submitted");
        EmergencyRemote::start_pairing();
        break;
    case Action::DeletePairing:
        APP_LOGI("ProductEvent", "pairing source=ui action=delete result=submitted");
        EmergencyRemote::delete_pairing();
        EmergencyUi::show_message(kPairingDeleted, now_us);
        break;
    }
    return false;
}
} // namespace AppController
