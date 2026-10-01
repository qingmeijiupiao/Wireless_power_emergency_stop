/**
 * @file emergency_ui.cpp
 * @brief 实现紧急停机控制器的 OLED 显示与菜单交互：页面选择优先级、故障/低电提示、刷新判定与休眠前画面。
 */
#include "emergency_ui.h"
#include "sh1106.h"
#include "battery_level.h"
#include "product_ui.h"
#include "ui_policy.h"
#include "runtime_settings.h"
#include "blackbox.h"
#include "blackbox_service.h"
#include "diagnostic_log.h"
#include "esp_app_desc.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>
namespace EmergencyUi {
namespace {
constexpr const char *tag = "EmergencyUi";        // 本模块日志标签
constexpr const char *kEventTag = "ProductEvent"; // 产品事件日志标签，用于记录菜单与保护动作
// 深睡保留的故障提示页编号与其反码校验值；掉电重启后仍能恢复上次故障提示。
RTC_DATA_ATTR uint32_t saved_fault = 0, saved_fault_check = 0;
constexpr uint32_t kFaultMagic = 0x46540000; // 故障页保存标志，低 8 位存放页面编号
int fault_page = -1, current_fault = -1;     // 最近提示的故障页、当前实时故障页
Sh1106 display;                              // OLED 驱动实例
UiPolicy::Menu menu;                         // 菜单状态机
UiPolicy::FaultNotice notice;                // 故障提示状态机
// dirty：有待重绘内容；fault_acknowledged：本轮故障是否已被用户确认；failed_before：上一周期是否处于连接失败，用于边沿检测
// controlling：远端正处于停止/启动过程中，此时屏蔽按键并强制回到主页
bool dirty = true, fault_acknowledged = false, failed_before = false, controlling = false;
bool low_notified = false; // 本轮低电是否已提示过，电压回升后清零以便再次提示
int message = 0, shown_page = -1; // 当前消息编号、已成功显示到屏幕的页面编号
// low_notice_until：低电提示截止时刻；ui_rendered_at：上次真正写屏时刻；display_retry_at：下次允许重试初始化屏幕的时刻
// state_since：当前远端状态进入时刻；rendered_data：已显示页面对应的数据时间戳，用于判断实时数据是否变化
int64_t low_notice_until = 0, ui_rendered_at = 0, display_retry_at = 0, state_since = 0, rendered_data = -1;
auto last_state = EmergencyRemote::State::UNPAIRED; // 上一周期远端状态，用于检测状态跳变
unsigned failures = 0;                              // 累计写屏失败次数，用于日志与故障定位
uint8_t frame[1024];                                // SH1106 单帧缓冲：128 列 × 8 页 = 1024 字节
} // namespace
void init() {
    menu = {};   // 从主页开始
    notice = {}; // 提示状态机复位
    state_since = esp_timer_get_time();
    // 仅当本次为深睡唤醒、RTC 中记录标志匹配且反码校验通过时，才认为故障历史有效。
    // page == 6/7 为短路/检测类故障，10..18 为各类保护故障，其余页面不参与历史恢复。
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP && saved_fault_check == ~saved_fault &&
        (saved_fault & 0xffffff00U) == kFaultMagic) {
        const int page = saved_fault & 255;
        if (page == 6 || page == 7 || (page >= 10 && page <= 18)) {
            // 以“历史提示”方式恢复，展示时长取当前配置的提示毫秒数。
            notice.restore(page, esp_timer_get_time(), RuntimeSettings::get("notice_ms") * 1000LL);
            fault_page = page;
            ESP_LOGI(tag, "RESTORED_FAULT_HISTORY page=%d", page);
        }
    }
    restore();
}
Update handle_input(const EmergencyRemote::Snapshot &before_ui, ButtonInput::Gesture gesture, int64_t tick) {
    Update result;
    // 任何按键都视为用户活动：记录事件、阻止休眠并请求重绘。
    if (gesture != UiPolicy::Gesture::None) {
        DEVICE_EVENT_I(kEventTag, "BOOT %s source=button view=%u item=%d",
                       gesture == UiPolicy::Gesture::Short ? "short" : "long",
                       static_cast<unsigned>(menu.view), menu.selected);
        result.activity = true;
        dirty = true;
    }
    current_fault = ProductUi::fault_page(before_ui);
    // 提示状态机检测到新的故障页时：记录当前页、写入 RTC 供深睡恢复、取消确认标记并要求重绘。
    if (notice.update(current_fault, tick, RuntimeSettings::get("notice_ms") * 1000LL, before_ui.on_attempt)) {
        fault_page = notice.page;
        saved_fault = kFaultMagic | static_cast<uint32_t>(notice.page);
        saved_fault_check = ~saved_fault;
        fault_acknowledged = false;
        result.activity = true;
        dirty = true;
        ESP_LOGI(tag, "FAULT_NOTICE page=%d hold_ms=%lu", notice.page,
                 static_cast<unsigned long>(RuntimeSettings::get("notice_ms")));
        DEVICE_EVENT_I(kEventTag, "fault notice page=%d protection=%u attempt=%lu", notice.page,
                       before_ui.protection_mask, static_cast<unsigned long>(before_ui.on_attempt));
    }
    // 连接失败的上升沿：回到主页并记为一次活动，避免用户停留在失效的菜单里。
    if (before_ui.connection_failed && !failed_before) {
        result.activity = true;
        menu.home();
    }
    failed_before = before_ui.connection_failed;
    // 连接失败时，主页上的短按改为请求重试，不再进入菜单。
    if (before_ui.connection_failed && menu.view == UiPolicy::View::Home && gesture == UiPolicy::Gesture::Short) {
        result.retry = true;
        gesture = UiPolicy::Gesture::None;
    }
    // 停止/启动进行中视为关键过程：屏蔽按键并固定回主页，防止误触改变输出状态。
    controlling =
        before_ui.state == EmergencyRemote::State::STOPPING || before_ui.state == EmergencyRemote::State::STARTING;
    if (controlling) {
        menu.home();
        gesture = UiPolicy::Gesture::None;
    } else if (gesture != UiPolicy::Gesture::None) {
        // 存在未确认故障且当前处于主页时，首次短按只用于确认并查看数据，
        // 绝不把这次按键透传给菜单去触发 ON 或进入隐藏页面，从而保证输出状态不变。
        if (menu.view == UiPolicy::View::Home && !fault_acknowledged && notice.page >= 0 &&
            (current_fault >= 0 || notice.holding(tick)) && gesture == UiPolicy::Gesture::Short) {
            gesture = UiPolicy::Gesture::None; // 首次确认只用于查看数据，绝不触发 ON 或进入隐藏菜单
            ESP_LOGI(tag, "FAULT_ACK show data; output unchanged");
        }
        fault_acknowledged = true;
        notice.dismiss();
        low_notice_until = 0;
    }
    const auto old_view = menu.view;
    const auto action = menu.update(gesture, tick, RuntimeSettings::get("menu_idle_ms") * 1000LL);
    if (action != UiPolicy::Action::None)
        DEVICE_EVENT_I(kEventTag, "menu action=%u", static_cast<unsigned>(action));
    // 首次进入休眠时长页时，把选择项定位到当前配置的休眠时间，避免用户看到与实际不符的初始项。
    if (old_view != UiPolicy::View::SleepTime && menu.view == UiPolicy::View::SleepTime)
        menu.sleep_choice = UiPolicy::sleep_time_index(RuntimeSettings::get("idle_ms"));
    dirty = dirty || old_view != menu.view;

    result.action = action;
    result.sleep_choice = menu.sleep_choice;
    return result;
}
bool observe_state(const Model &model) {
    const int64_t now = model.now_us;
    const auto &state = model.remote;
    bool activity = false;
    // 状态跳变时记录进入时刻（供状态页的延迟判定使用）；连接失败属于被动结果，不计为用户活动。
    if (state.state != last_state) {
        DEVICE_EVENT_I(kEventTag, "control state %u -> %u", static_cast<unsigned>(last_state),
                       static_cast<unsigned>(state.state));
        last_state = state.state;
        state_since = now;
        if (!state.connection_failed)
            activity = true;
        fault_acknowledged = false;
    }
    // 低电判定：有有效读数、低于阈值且未接外部供电；接外部供电时不提示低电。
    const bool low = model.battery_mv > 0 &&
                     model.battery_mv <= static_cast<int>(RuntimeSettings::get("low_mv")) && !model.usb;
    if (!low)
        low_notified = false;
    // 低电提示只在主页、无进行中动作、无故障且未连接失败时弹出一次；
    // 其它场景（如故障页、菜单中）不抢占画面，避免遮盖更重要的提示。
    if (low && !low_notified && !controlling && current_fault < 0 && !state.connection_failed &&
        menu.view == UiPolicy::View::Home) {
        low_notified = true;
        message = 11;
        menu.view = UiPolicy::View::Message;
        menu.touched = now;
        low_notice_until = now + RuntimeSettings::get("notice_ms") * 1000LL;
        dirty = true;
    }

    return activity;
}
void render(const Model &model, bool force_dirty) {
    const auto &state = model.remote;
    const int64_t now = model.now_us;
    const bool low = model.battery_mv > 0 &&
                     model.battery_mv <= static_cast<int>(RuntimeSettings::get("low_mv")) && !model.usb;
    dirty = dirty || force_dirty;
    esp_err_t err = ESP_OK;
    // 页面编号约定：0..19 为状态/故障页（编号取自状态枚举的取值），100/101 为在线/离线主页，
    // 300+view 为菜单类页面，400 为连接失败页，500 为休眠倒计时页。以下按优先级从低到高覆盖。
    int page = static_cast<int>(state.state);
    // 故障已被确认且当前确为故障态：回到主页，让用户看到实时数据。
    if (fault_acknowledged && ProductUi::fault_page(state) >= 0)
        page = 100;
    // 状态稳定后回到主页：OFF 已停机，或 ON 且持续一定时间；插电/常亮时 OFF 需要更久才回主页。
    if ((state.state == EmergencyRemote::State::OFF &&
         (!state.stop_closed || ((model.usb || model.always_on) && now - state_since > 1500000))) ||
        (state.state == EmergencyRemote::State::ON && now - state_since > 1500000))
        page = 100;
    // 多个保护同时触发时按 OCP、OTP、OVP、UVP 的顺序择一显示，日志仍保留完整掩码。
    if (!fault_acknowledged && state.state == EmergencyRemote::State::PROTECTED && state.protection_mask) {
        page = (state.protection_mask & 8)   ? 18
               : (state.protection_mask & 1) ? 15
               : (state.protection_mask & 2) ? 16
                                             : 17;
    }
    // 故障已消失但提示仍在保持期内，继续显示该故障页，保证用户能看清提示。
    if (!fault_acknowledged && notice.holding(now) && ProductUi::fault_page(state) < 0 && !controlling &&
        !state.connection_failed)
        page = notice.page;
    // 配对过程优先占用屏幕，除非正处于停止/启动关键过程。
    if (state.pairing && !controlling)
        page = 19;
    if (page == 100)
        page = state.online ? 100 : 101;
    // urgent 表示不允许菜单覆盖的紧急画面：停止/启动过程，或未确认的保护/短路/检测故障。
    const bool urgent =
        state.state == EmergencyRemote::State::STOPPING || state.state == EmergencyRemote::State::STARTING ||
        (!fault_acknowledged && (state.protection_mask || state.state == EmergencyRemote::State::SHORT_FAULT ||
                                 state.state == EmergencyRemote::State::DETECT_ERROR));
    if (state.connection_failed && menu.view == UiPolicy::View::Home)
        page = 400;
    // 非紧急时，用户打开的菜单页覆盖状态页；紧急画面不允许被菜单遮盖。
    if (!urgent && menu.view != UiPolicy::View::Home)
        page = 300 + static_cast<int>(menu.view);
    if (menu.view == UiPolicy::View::Home && model.sleep.countdown)
        page = 500;
    // 菜单类页面每秒至少重绘一次（刷新倒计时/时间相关显示）；主页仅在页面或数据时间戳变化时重绘。
    dirty = dirty || (page >= 300 && now - ui_rendered_at > 1000000);
    dirty = dirty || page != shown_page || (page >= 100 && page < 200 && rendered_data != state.data_time_us);
    // 屏幕尚未就绪时按固定间隔重试初始化；失败则关闭并等待下次重试，避免持续占用总线。
    if (!display.address() && now >= display_retry_at) {
        display.shutdown();
        err = display.init(CONFIG_ESTOP_OLED_SDA, CONFIG_ESTOP_OLED_SCL, CONFIG_ESTOP_OLED_COLUMN_OFFSET);
        display_retry_at = now + 3000000;
        if (err != ESP_OK)
            display.shutdown();
    }
    if (dirty && display.address()) {
        // output_fresh：输出状态的时间戳仍在新鲜窗口内；closing_unconfirmed：连接失败但输出可能尚未真正关断。
        const bool output_fresh =
            state.output_time_us > 0 && now - state.output_time_us < RuntimeSettings::get("fresh_ms") * 1000LL;
        const bool closing_unconfirmed = state.connection_failed && state.output_on && state.busy;
        if (page == 500) {
            ProductUi::render_sleep_countdown(frame, model.sleep.seconds, state.connection_failed);
        } else if (page == 400) {
            ProductUi::render_failure(frame, closing_unconfirmed);
        } else if (page >= 300) {
            using UiPolicy::View;
            // 300+view 的页面按菜单视图分派：菜单/确认/休眠时长为菜单样式，消息页为文本页，其余为设备信息页。
            if (menu.view == View::Menu || menu.view == View::Confirm || menu.view == View::SleepTime)
                ProductUi::render_menu(frame, menu, model.always_on);
            else if (menu.view == View::Message)
                ProductUi::render_message(frame, message, model.always_on);
            else {
                // 信息页数据按需读取：黑匣子未启用时不读取统计，相关字段保持默认值。
                BlackboxService::Statistics stats{};
                const bool blackbox_ready = Blackbox::is_enabled();
                if (blackbox_ready)
                    BlackboxService::get_statistics(&stats);
                ProductUi::DeviceInfo info;
                info.page = menu.info_page;
                info.battery_mv = model.battery_mv;
                info.usb = model.usb;
                info.version = esp_app_get_description()->version;
                info.built = BUILD_TIME;
                info.blackbox_ready = blackbox_ready;
                if (blackbox_ready) {
                    info.records = Blackbox::count();
                    info.capacity = Blackbox::capacity();
                }
                info.pending = stats.pending_logs;
                info.captured = stats.captured_logs;
                info.dropped = stats.dropped_logs;
                info.failures = stats.persist_failures;
                ProductUi::render_info(frame, info);
            }
        } else if (page >= 100)
            ProductUi::render_home(frame, state);
        else
            // 状态/故障页；若该页正是当前保持中的历史提示页，则加画历史页脚。
            ProductUi::render_state(frame, page, notice.history && page == notice.page);
        // 右侧状态栏最后绘制，叠加在内容之上：警告条件为存在故障页或低电。
        ProductUi::render_rail(frame, state, model.battery_percent, model.usb, output_fresh,
                               ProductUi::fault_page(state) >= 0 || low, closing_unconfirmed);
        ui_rendered_at = now;
        err = display.write_frame(frame, sizeof(frame));
        if (err == ESP_OK) {
            shown_page = page;
            rendered_data = state.data_time_us;
        } else {
            // 写屏失败：记录次数并清空已显示页标记，使下一周期强制重绘。
            ++failures;
            shown_page = -1;
            ESP_LOGE(tag, "OLED frame page=%d error=%s failures=%u", page, esp_err_to_name(err), failures);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    dirty = false;
}
void show_message(int value, int64_t now_us, bool touch) {
    message = value;
    menu.view = UiPolicy::View::Message;
    // touch 为真时刷新空闲计时，使该消息不会立刻被菜单空闲逻辑关闭。
    if (touch)
        menu.touched = now_us;
    dirty = true;
}
int last_fault_page() { return fault_page; }
// 返回两类提示截止时刻的较晚者，确保系统在等待提示展示完整前不进入休眠。
int64_t notice_deadline() { return notice.until > low_notice_until ? notice.until : low_notice_until; }
bool sleep_notice_allowed(int64_t now_us) { return !notice.holding(now_us) && now_us >= low_notice_until; }
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
    // 复位已显示页标记并置脏，保证恢复后必然重绘一帧。
    shown_page = -1;
    dirty = true;
}
} // namespace EmergencyUi
