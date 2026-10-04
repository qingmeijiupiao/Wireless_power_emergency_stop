/**
 * @file emergency_ui.cpp
 * @brief OLED 显示入口：驱动 SH1106、维护刷新节拍，并把页面选择与按键处理委托给 UiManager。
 */
#include "emergency_ui.h"
#include "emergency_remote.h"
#include "battery_status.h"
#include "core/button_input.h"
#include "core/ui_manager.h"
#include "product_ui.h"
#include "runtime_settings.h"
#include "core/display_worker.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
namespace EmergencyUi {
namespace {
constexpr const char *tag = "EmergencyUi"; // 本模块日志标签
bool dirty = true;                         // 是否有待重绘内容
int shown_key = -1;                        // 已成功显示到屏幕的页面键
int64_t ui_rendered_at = 0, rendered_data = -1; // 上次写屏/下次重试/已显示数据时间戳
uint8_t frame[1024];                       // SH1106 单帧缓冲：128 列 × 8 页 = 1024 字节
} // namespace

esp_err_t init_buttons() { return Buttons::init(); }
uint32_t dropped_gestures() { return Buttons::dropped(); }

void init() {
    ESP_ERROR_CHECK(DisplayWorker::init());
    UiManager::instance().reset(esp_timer_get_time());
}

Update handle_input(const EmergencyRemote::Snapshot &remote, int64_t tick) {
    UiManager &ui = UiManager::instance();
    Update result;
    Gesture gesture = Gesture::None;
    bool had_gesture = false;
    // 消费本帧全部手势；合并活动、重试与最后一个有效动作。
    while (Buttons::poll(gesture)) {
        had_gesture = true;
        const Update item = ui.handle_input(remote, gesture, tick);
        result.activity = result.activity || item.activity;
        result.retry = result.retry || item.retry;
        if (item.action != Action::None) {
            result.action = item.action;
            result.sleep_choice = item.sleep_choice;
        }
    }
    // 无手势时仍需周期调用，以推进故障提示与状态判定。
    if (!had_gesture) {
        const Update item = ui.handle_input(remote, Gesture::None, tick);
        result.activity = result.activity || item.activity;
        result.retry = result.retry || item.retry;
        if (item.action != Action::None) {
            result.action = item.action;
            result.sleep_choice = item.sleep_choice;
        }
    }
    return result;
}

bool observe_state(const Model &model) { return UiManager::instance().observe_state(model); }

void render(const Model &model, bool force_dirty) {
    UiManager &ui = UiManager::instance();
    const int64_t now = model.now_us;
    const bool ui_dirty = ui.take_redraw(); // 必须消费，避免短路留下多余重绘。
    dirty = dirty || force_dirty || ui_dirty;
    const int key = ui.page_key(model);
    // 菜单类页面每秒至少重绘一次；主页仅在页面或数据时间戳变化时重绘。
    dirty = dirty || (key >= 300 && now - ui_rendered_at > 1000000);
    dirty = dirty || key != shown_key || (key >= 100 && key < 200 && rendered_data != model.remote.data_time_us);

    if (dirty) {
        ui.render(frame, model);
        if (DisplayWorker::submit(frame)) {
            ui_rendered_at = now;
            shown_key = key;
            rendered_data = model.remote.data_time_us;
            dirty = false;
        }
    }
}

void show_message(int value, int64_t now_us, bool touch) { UiManager::instance().show_message(value, now_us, touch); }

int last_fault_page() { return UiManager::instance().last_fault_page(); }

int64_t notice_deadline() { return UiManager::instance().notice_deadline(); }

bool sleep_notice_allowed(int64_t now_us) { return UiManager::instance().sleep_notice_allowed(now_us); }

void prepare_sleep() {
    // 进入休眠前主动写一帧“即将休眠”画面，避免屏幕停留在过期数据上直到断电。
    uint8_t sleep_frame[1024];
    const auto state = EmergencyRemote::snapshot();
    BatteryLevel::Status level = {};
    const int percent = BatteryStatus::get_status(level) ? level.displayed_percent : -1;
    ProductUi::render_message(sleep_frame, 0, RuntimeSettings::always_on());
    ProductUi::render_rail(sleep_frame, state, percent, false, state.output_time_us > 0);
    (void)DisplayWorker::submit(sleep_frame);
}

bool shutdown() { return DisplayWorker::shutdown(); }

void restore() {
    DisplayWorker::restore();
    // 复位已显示标记并置脏，保证恢复后必然重绘一帧。
    shown_key = -1;
    dirty = true;
}
} // namespace EmergencyUi
