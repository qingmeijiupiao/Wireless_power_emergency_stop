/**
 * @file emergency_ui.cpp
 * @brief OLED 显示入口：驱动 SH1106、维护刷新节拍，并把页面选择与按键处理委托给 UiManager。
 */
#include "emergency_ui.h"
#include "battery_level.h"
#include "core/ui_manager.h"
#include "product_ui.h"
#include "runtime_settings.h"
#include "sh1106.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
namespace EmergencyUi {
namespace {
constexpr const char *tag = "EmergencyUi"; // 本模块日志标签
Sh1106 display;                            // OLED 驱动实例
bool dirty = true;                         // 是否有待重绘内容
int shown_key = -1;                        // 已成功显示到屏幕的页面键
int64_t ui_rendered_at = 0, display_retry_at = 0, rendered_data = -1; // 上次写屏/下次重试/已显示数据时间戳
unsigned failures = 0;                     // 累计写屏失败次数
uint8_t frame[1024];                       // SH1106 单帧缓冲：128 列 × 8 页 = 1024 字节
} // namespace

void init() {
    UiManager::instance().reset(esp_timer_get_time());
    restore();
}

Update handle_input(const EmergencyRemote::Snapshot &remote, ButtonInput::Event event, int64_t tick) {
    return UiManager::instance().handle_input(remote, event, tick);
}

bool observe_state(const Model &model) { return UiManager::instance().observe_state(model); }

void render(const Model &model, bool force_dirty) {
    UiManager &ui = UiManager::instance();
    const int64_t now = model.now_us;
    dirty = dirty || force_dirty || ui.take_redraw();
    const int key = ui.page_key(model);
    // 菜单类页面每秒至少重绘一次；主页仅在页面或数据时间戳变化时重绘。
    dirty = dirty || (key >= 300 && now - ui_rendered_at > 1000000);
    dirty = dirty || key != shown_key || (key >= 100 && key < 200 && rendered_data != model.remote.data_time_us);

    esp_err_t err = ESP_OK;
    // 屏幕尚未就绪时按固定间隔重试初始化；失败则关闭并等待下次重试。
    if (!display.address() && now >= display_retry_at) {
        display.shutdown();
        err = display.init(CONFIG_ESTOP_OLED_SDA, CONFIG_ESTOP_OLED_SCL, CONFIG_ESTOP_OLED_COLUMN_OFFSET);
        display_retry_at = now + 3000000;
        if (err != ESP_OK)
            display.shutdown();
    }
    if (dirty && display.address()) {
        ui.render(frame, model);
        ui_rendered_at = now;
        err = display.write_frame(frame, sizeof(frame));
        if (err == ESP_OK) {
            shown_key = key;
            rendered_data = model.remote.data_time_us;
        } else {
            // 写屏失败：记录次数并清空已显示标记，使下一周期强制重绘。
            ++failures;
            shown_key = -1;
            ESP_LOGE(tag, "OLED frame key=%d error=%s failures=%u", key, esp_err_to_name(err), failures);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    dirty = false;
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
    const int percent = BatteryLevel::get_status(level) ? level.displayed_percent : -1;
    ProductUi::render_message(sleep_frame, 0, RuntimeSettings::always_on());
    ProductUi::render_rail(sleep_frame, state, percent, false, state.output_time_us > 0);
    (void)display.write_frame(sleep_frame, sizeof(sleep_frame));
}

void shutdown() { display.shutdown(); }

void restore() {
    const auto err = display.init(CONFIG_ESTOP_OLED_SDA, CONFIG_ESTOP_OLED_SCL, CONFIG_ESTOP_OLED_COLUMN_OFFSET);
    if (err != ESP_OK) {
        ESP_LOGE(tag, "OLED init: %s", esp_err_to_name(err));
        display.shutdown();
    }
    // 复位已显示标记并置脏，保证恢复后必然重绘一帧。
    shown_key = -1;
    dirty = true;
}
} // namespace EmergencyUi
