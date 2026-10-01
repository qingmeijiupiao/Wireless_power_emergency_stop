/**
 * @file ui_manager.cpp
 * @brief UI 管理器实现：选页、按键分发、故障确认、低电提示与页面渲染编排。
 */
#include "core/ui_manager.h"

#include "diagnostic_log.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "product_ui.h"
#include "runtime_settings.h"

namespace EmergencyUi {
namespace {
constexpr char TAG[] = "EmergencyUi";             // 管理器日志标签
constexpr char kEventTag[] = "ProductEvent";      // 产品事件日志标签
// 深睡保留的故障提示页编号与其反码校验值；掉电重启后仍能恢复上次故障提示。
RTC_DATA_ATTR uint32_t saved_fault = 0, saved_fault_check = 0;
constexpr uint32_t kFaultMagic = 0x46540000;      // 故障页保存标志，低 8 位存放页面编号

const char *event_name(ButtonInput::Event event) {
    switch (event) {
    case ButtonInput::Event::Short:
        return "short";
    case ButtonInput::Event::Long:
        return "long";
    default:
        return "none";
    }
}
} // namespace

UiManager &UiManager::instance() {
    static UiManager manager;
    return manager;
}

void UiManager::reset(int64_t now_us) {
    state_ = UiState{};
    state_.state_since = now_us;
    // 仅当本次为深睡唤醒、RTC 标志匹配且反码校验通过时，才恢复故障历史。
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP && saved_fault_check == ~saved_fault &&
        (saved_fault & 0xffffff00U) == kFaultMagic) {
        const int page = saved_fault & 255;
        if (page == 6 || page == 7 || (page >= 10 && page <= 18)) {
            state_.notice.restore(page, now_us, RuntimeSettings::get("notice_ms") * 1000LL);
            state_.fault_page = page;
            ESP_LOGI(TAG, "RESTORED_FAULT_HISTORY page=%d", page);
        }
    }
    redraw_ = true;
}

Update UiManager::handle_input(const EmergencyRemote::Snapshot &remote, ButtonInput::Event event, int64_t now_us) {
    Update result;
    // 任何按键都视为用户活动：记录事件、阻止休眠并请求重绘。
    if (event != ButtonInput::Event::None) {
        DEVICE_EVENT_I(kEventTag, "BOOT %s view=%u item=%d", event_name(event),
                       static_cast<unsigned>(state_.menu.view), state_.menu.selected);
        result.activity = true;
        redraw_ = true;
    }
    state_.current_fault = ProductUi::fault_page(remote);
    // 新故障出现：记录页面、写入 RTC、取消确认标记并要求重绘。
    if (state_.notice.update(state_.current_fault, now_us, RuntimeSettings::get("notice_ms") * 1000LL,
                             remote.on_attempt)) {
        state_.fault_page = state_.notice.page;
        saved_fault = kFaultMagic | static_cast<uint32_t>(state_.notice.page);
        saved_fault_check = ~saved_fault;
        state_.fault_acknowledged = false;
        result.activity = true;
        redraw_ = true;
        ESP_LOGI(TAG, "FAULT_NOTICE page=%d hold_ms=%lu", state_.notice.page,
                 static_cast<unsigned long>(RuntimeSettings::get("notice_ms")));
        DEVICE_EVENT_I(kEventTag, "fault notice page=%d protection=%u attempt=%lu", state_.notice.page,
                       remote.protection_mask, static_cast<unsigned long>(remote.on_attempt));
    }
    // 连接失败上升沿：回到主页并记为一次活动，避免停留在失效的菜单里。
    if (remote.connection_failed && !state_.failed_before) {
        result.activity = true;
        state_.menu.home();
        redraw_ = true;
    }
    state_.failed_before = remote.connection_failed;
    // 停止/启动进行中屏蔽按键并固定回主页，防止误触改变输出状态。
    state_.controlling = remote.state == EmergencyRemote::State::STOPPING ||
                         remote.state == EmergencyRemote::State::STARTING;
    if (state_.controlling) {
        state_.menu.home();
        event = ButtonInput::Event::None;
    } else if (event != ButtonInput::Event::None) {
        // 未确认故障时首次短按只用于确认并查看数据，不透传给菜单触发 ON 或进入隐藏页面。
        if (state_.menu.view == UiPolicy::View::Home && !state_.fault_acknowledged && state_.notice.page >= 0 &&
            (state_.current_fault >= 0 || state_.notice.holding(now_us)) &&
            event == ButtonInput::Event::Short) {
            event = ButtonInput::Event::None;
            ESP_LOGI(TAG, "FAULT_ACK show data; output unchanged");
        }
        state_.fault_acknowledged = true;
        state_.notice.dismiss();
        state_.low_notice_until = 0;
    }

    const UiPolicy::View view_before = state_.menu.view;
    Update page_update = resolve(remote)->handle_button(remote, event, now_us, state_);
    if (state_.menu.view != view_before)
        redraw_ = true;
    if (page_update.action != Action::None)
        DEVICE_EVENT_I(kEventTag, "menu action=%u", static_cast<unsigned>(page_update.action));

    result.action = page_update.action;
    result.retry = page_update.retry;
    result.sleep_choice = page_update.sleep_choice;
    return result;
}

bool UiManager::observe_state(const Model &model) {
    bool activity = false;
    const auto &remote = model.remote;
    // 状态跳变时记录进入时刻；连接失败属于被动结果，不计为用户活动。
    if (remote.state != state_.last_state) {
        DEVICE_EVENT_I(kEventTag, "control state %u -> %u", static_cast<unsigned>(state_.last_state),
                       static_cast<unsigned>(remote.state));
        state_.last_state = remote.state;
        state_.state_since = model.now_us;
        if (!remote.connection_failed)
            activity = true;
        state_.fault_acknowledged = false;
    }
    // 低电判定：有有效读数、低于阈值且未接外部供电。
    const bool low = model.battery_mv > 0 &&
                     model.battery_mv <= static_cast<int>(RuntimeSettings::get("low_mv")) && !model.usb;
    if (!low)
        state_.low_notified = false;
    // 低电提示只在主页、无进行中动作、无故障且未连接失败时弹出一次。
    if (low && !state_.low_notified && !state_.controlling && state_.current_fault < 0 &&
        !remote.connection_failed && state_.menu.view == UiPolicy::View::Home) {
        state_.low_notified = true;
        state_.message = 11;
        state_.menu.view = UiPolicy::View::Message;
        state_.menu.touched = model.now_us;
        state_.low_notice_until = model.now_us + RuntimeSettings::get("notice_ms") * 1000LL;
        redraw_ = true;
    }
    return activity;
}

Page *UiManager::resolve(const EmergencyRemote::Snapshot &remote) const {
    Page *best = nullptr;
    int best_priority = -1;
    for (Page *page : pages_) {
        if (page->active(remote, state_) && page->priority() > best_priority) {
            best = page;
            best_priority = page->priority();
        }
    }
    return best;
}

int UiManager::page_key(const Model &model) const { return resolve(model.remote)->page_key(model, state_); }

void UiManager::render(uint8_t *frame, const Model &model) {
    resolve(model.remote)->render(frame, model, state_);
    // 右侧状态栏最后绘制，叠加在内容之上：警告条件为存在故障页或低电。
    const bool output_fresh = model.remote.output_time_us > 0 &&
                              model.now_us - model.remote.output_time_us <
                                  RuntimeSettings::get("fresh_ms") * 1000LL;
    const bool closing_unconfirmed = model.remote.connection_failed && model.remote.output_on && model.remote.busy;
    const bool low = model.battery_mv > 0 &&
                     model.battery_mv <= static_cast<int>(RuntimeSettings::get("low_mv")) && !model.usb;
    ProductUi::render_rail(frame, model.remote, model.battery_percent, model.usb, output_fresh,
                           ProductUi::fault_page(model.remote) >= 0 || low, closing_unconfirmed);
}

int64_t UiManager::notice_deadline() const {
    return state_.notice.until > state_.low_notice_until ? state_.notice.until : state_.low_notice_until;
}

bool UiManager::sleep_notice_allowed(int64_t now_us) const {
    return !state_.notice.holding(now_us) && now_us >= state_.low_notice_until;
}

void UiManager::show_message(int message, int64_t now_us, bool touch) {
    state_.message = message;
    state_.menu.view = UiPolicy::View::Message;
    if (touch)
        state_.menu.touched = now_us;
    redraw_ = true;
}

bool UiManager::take_redraw() {
    const bool requested = redraw_;
    redraw_ = false;
    return requested;
}

} // namespace EmergencyUi
