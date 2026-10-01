/**
 * @file app_main.cpp
 * @brief 急停控制器高层启动入口与运行时序编排
 *
 * 职责：
 * - app_main 明确编排硬件、持久化、诊断、电池和无线服务的初始化顺序；
 * - 运行唯一的协调循环，统一处理按键手势、电源管理、状态上报与 UI 渲染；
 * - 各服务通过线程安全的请求接口与 Shell 任务交互，循环中不存在单字符按键派发。
 */
#include "battery_level.h"
#include "battery_voltage.h"
#include "blackbox.h"
#include "blackbox_service.h"
#include "boot_diagnostics.h"
#include "button_input.h"
#include "emergency_remote.h"
#include "emergency_ui.h"
#include "espnow_link.h"
#include "hardware.h"
#include "power_manager.h"
#include "runtime_settings.h"
#include "shell_command.h"

#include "HXC_NVS.h"
#include "diagnostic_log.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>

namespace {

// ESP_LOGx 使用的日志标签，便于在串口输出中定位来源。
constexpr char TAG[] = "app_main";

// 关键状态变化统一使用 DEVICE_EVENT_I 标记，黑匣子 hook 按此白名单持久化。
// 标签名固定，保证升级或恢复后仍能沿用同一过滤规则读取历史记录。
constexpr const char* kEventTag = "ProductEvent";
// 仅放行业务事件标签进入黑匣子，避免普通调试日志刷爆存储空间。
constexpr const char* kInfoTags[] = {kEventTag};

// 最近一次成功采样的电压(mV)。采样是异步的，UI 在 BatteryLevel 尚无有效状态
// 时用它作为兜底显示值，避免出现空白或明显过期的电量。
int latest_battery_mv = 0;

// 急停按键下降沿的 ISR 回调：运行在中断上下文，只做最短路径的转交，
// 实际状态机处理交由 EmergencyRemote 的线程安全接口完成。
void IRAM_ATTR on_stop_fall(void*) {
    EmergencyRemote::stop_from_isr();
}

// 异步电池采样的完成回调：采样失败只记录日志并保留上一次有效值，
// 避免一次读取异常把电量显示清零或误判为低电。
void on_battery_sample(esp_err_t result, int voltage_mv, void*) {
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "battery sample: %s", esp_err_to_name(result));
        return;
    }
    latest_battery_mv = voltage_mv;
    BatteryLevel::update(voltage_mv, Hardware::usb_connected());
}

// 供 UI 与日志使用的电池快照。percent 为 -1 表示当前没有可信百分比，
// 调用方需据此跳过依赖电量的展示或上报。
struct BatteryView {
    int voltage_mv = 0;
    int percent = -1;
};

// 优先返回 BatteryLevel 的平滑结果；其尚未就绪时回退到原始采样电压。
BatteryView current_battery() {
    BatteryLevel::Status status = {};
    if (BatteryLevel::get_status(status)) {
        return {status.voltage_mv, status.displayed_percent};
    }
    return {latest_battery_mv, -1};
}

// 电源管理在进入/退出休眠时回调的屏幕钩子：休眠前准备、断电、恢复分别绑定到
// UI 的实现，保证显示与供电状态同步切换。
constexpr PowerManager::DisplayHooks display_hooks{EmergencyUi::prepare_sleep, EmergencyUi::shutdown,
                                                   EmergencyUi::restore};

} // namespace

// 应用入口。初始化顺序：先建立硬件与输入，再依次拉起持久化、诊断、无线与 Shell，
// 随后进入周期性协调循环，循环内不做阻塞式长操作。
extern "C" void app_main(void) {
    // 先初始化硬件与按键，使急停中断和按键采样尽早可用。
    Hardware::init(on_stop_fall);
    ButtonInput::init();
    // 等待硬件供电稳定后再启动 ADC 电池采样，避免上电瞬态导致误读。
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(BatteryVoltage::init());
    // 读取并清除“由开机键释放唤醒”的标记，供遥控与休眠逻辑判断唤醒来源。
    const bool release_wake = PowerManager::consume_release_wake();
    // 公共 NVS 必须在使用 RuntimeSettings/黑匣子前就绪。
    ESP_ERROR_CHECK(HXC::NVS_Base::setup());
    // 黑匣子属于可降级功能：初始化失败只告警，急停控制仍须照常运行。
    if (Blackbox::init() != ESP_OK ||
        BlackboxService::init({kInfoTags, sizeof(kInfoTags) / sizeof(kInfoTags[0])}) != ESP_OK) {
        ESP_LOGW(TAG, "Blackbox unavailable; control remains active");
    }
    // 运行参数从 NVS 载入到原子缓存，必须早于遥控/UI 的读取。
    RuntimeSettings::init();
    ESP_ERROR_CHECK(EmergencyRemote::init(release_wake));
    // Shell 最后注册，确保其命令引用的服务都已初始化。
    ESP_ERROR_CHECK(ShellCommand::init());

    // 启动阶段同步采样一次，保证启动诊断和首个 UI 帧有可用电压。
    int boot_battery_mv = 0;
    if (BatteryVoltage::read_mv(boot_battery_mv) == ESP_OK) {
        latest_battery_mv = boot_battery_mv;
        BatteryLevel::update(boot_battery_mv, Hardware::usb_connected());
    }
    // 插电即启动 USB 满电自动校准；拔出后校准任务自行退出，重新插入再次启动。
    if (Hardware::usb_connected()) {
        (void)BatteryVoltage::start_calibration_monitor(Hardware::usb_connected);
    }
    BootDiagnostics::append_boot(release_wake);
    EmergencyUi::init();
    PowerManager::init_idle(esp_timer_get_time());

    // 循环内缓存的时序/状态快照，用于检测边沿变化并驱动上报与重绘：
    // - battery_at/battery_report_at：下一次采样与电压上报的绝对时刻(us)；
    // - was_*/previous_*：上一拍的输入/输出状态，用于识别跳变；
    // - manual_sleep_pending：等待按键释放后才真正执行的手动休眠请求；
    // - message：最近一次提示页编号，供 UI 展示。
    int64_t battery_at = esp_timer_get_time() + RuntimeSettings::get("battery_ms") * 1000LL;
    int64_t battery_report_at = 0;
    bool was_external_power = Hardware::usb_connected();
    bool was_online = false;
    bool previous_output = false, previous_paired = false, previous_pairing = false;
    uint8_t previous_protection = 0;
    bool manual_sleep_pending = false;
    int message = 0;

    // 唯一协调循环：固定 20ms 节拍，兼顾按键响应与功耗，避免忙等。
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(20));
        const int64_t tick = esp_timer_get_time();
        // 本拍是否需要触发一次 UI 重绘。
        bool dirty = false;

        const bool external_power = Hardware::usb_connected();
        // USB 插拔是重要事件：记录黑匣子、刷新活动计时并立即安排一次采样。
        if (external_power != was_external_power) {
            const auto battery = current_battery();
            DEVICE_EVENT_I(kEventTag, "USB %s battery_mv=%d soc=%d", external_power ? "inserted" : "removed",
                           battery.voltage_mv, battery.percent);
            was_external_power = external_power;
            PowerManager::note_activity(tick);
            battery_at = tick;
            dirty = true;
            // 仅在插电时启动满电校准监控；拔出后该校准任务会自行结束。
            if (external_power &&
                BatteryVoltage::start_calibration_monitor(Hardware::usb_connected) == ESP_OK) {
                ESP_LOGI(TAG, "battery calibration monitor started");
            }
        }
        // 周期性电池采样：采样任务忙时跳过本次但照常顺延，避免重入。
        if (tick >= battery_at) {
            if (!BatteryVoltage::is_busy()) {
                (void)BatteryVoltage::start_async(on_battery_sample);
            }
            battery_at = tick + RuntimeSettings::get("battery_ms") * 1000LL;
            dirty = true;
        }

        // 按键手势先交给 UI 解析成动作，同时把“有用户活动”上报给电源管理。
        ButtonInput::Event event = ButtonInput::Event::None;
        (void)ButtonInput::poll(event);
        auto update = EmergencyUi::handle_input(EmergencyRemote::snapshot(), event, tick);
        if (update.activity) {
            PowerManager::note_activity(tick);
        }
        if (update.retry) {
            EmergencyRemote::retry_connection();
        }

        using EmergencyUi::Action;
        const auto action = update.action;
        // 各菜单动作在此落地：设置类动作写 NVS 并回显成功/失败页；
        // 配对动作需满足安全条件，避免输出带电时误入配对。
        if (action == Action::AlwaysOn) {
            message = RuntimeSettings::set_always_on(!RuntimeSettings::always_on()) ? 6 : 7;
            EmergencyUi::show_message(message, tick);
        } else if (action == Action::SleepTime) {
            // sleep_choice 是 kSleepTimesMs 的下标，写入 idle_ms 后即时生效。
            const bool saved = RuntimeSettings::set("idle_ms", RuntimeSettings::kSleepTimesMs[update.sleep_choice]);
            message = saved ? 12 : 7;
            EmergencyUi::show_message(message, tick);
        } else if (action == Action::Sleep) {
            // 仅登记请求，等按键抬起后再休眠，防止长按/误触导致立即断电。
            manual_sleep_pending = true;
        } else if (action == Action::Pair || action == Action::Repair) {
            const auto s = EmergencyRemote::snapshot();
            // 允许配对的情形：尚未配对；当前无休眠阻塞；或插电且输出刚关闭不久
            // (3s 内)的短暂窗口，便于带电场景快速修复配对。
            if (!s.paired || PowerManager::block() == PowerManager::SleepBlock::None ||
                (Hardware::usb_connected() && !s.output_on && !s.busy && s.output_time_us > 0 &&
                 tick - s.output_time_us < 3000000)) {
                EmergencyRemote::start_pairing(action == Action::Repair);
                message = 8;
            } else {
                message = 4;
            }
            EmergencyUi::show_message(message, tick);
        }
        // 手动休眠分两拍完成：按住期间只提示；松开后才尝试进入休眠。
        if (manual_sleep_pending && Hardware::button_pressed()) {
            message = 5;
            EmergencyUi::show_message(message, tick, true);
        }
        if (manual_sleep_pending && !Hardware::button_pressed()) {
            manual_sleep_pending = false;
            // enter_sleep 可能因安全条件被拒绝，返回的 reason 通过提示页展示。
            message = PowerManager::enter_sleep(display_hooks, true);
            ESP_LOGI(TAG, "MANUAL_SLEEP_DENIED reason=%d", message);
            EmergencyUi::show_message(message, tick, true);
            dirty = true;
        }

        const auto state = EmergencyRemote::snapshot();
        // 仅在状态发生跳变时写黑匣子，避免每个循环都产生冗余记录。
        if (state.online != was_online) {
            DEVICE_EVENT_I(kEventTag, "meter %s", state.online ? "online" : "offline");
        }
        if (state.output_on != previous_output || state.protection_mask != previous_protection) {
            DEVICE_EVENT_I(kEventTag, "meter output=%u protection=0x%02x", state.output_on, state.protection_mask);
        }
        if (state.paired != previous_paired || state.pairing != previous_pairing) {
            DEVICE_EVENT_I(kEventTag, "pairing state paired=%u active=%u saved_peers=%u", state.paired, state.pairing,
                           static_cast<unsigned>(EspNowLink::get_saved_peer_count()));
        }
        // 更新上一拍快照，作为下一拍的跳变基准。
        previous_output = state.output_on;
        previous_protection = state.protection_mask;
        previous_paired = state.paired;
        previous_pairing = state.pairing;

        // 组装本拍 UI 数据模型：遥控快照、电池、USB、常亮与当前时间。
        const auto battery = current_battery();
        EmergencyUi::Model model;
        model.remote = state;
        model.battery_mv = battery.voltage_mv;
        model.battery_percent = battery.percent;
        model.usb = Hardware::usb_connected();
        model.always_on = RuntimeSettings::always_on();
        model.now_us = esp_timer_get_time();
        // observe_state 返回 true 表示出现了需要唤醒/延时的可见变化。
        if (EmergencyUi::observe_state(model)) {
            PowerManager::note_activity(model.now_us);
        }
        // 由电源管理器决定本拍是否应休眠：综合空闲时长、提示截止与连接失败。
        model.sleep = PowerManager::plan(model.now_us, EmergencyUi::notice_deadline(),
                                         EmergencyUi::sleep_notice_allowed(model.now_us), state.connection_failed);
        if (model.sleep.due) {
            message = PowerManager::enter_sleep(display_hooks, false);
            PowerManager::note_activity(model.now_us);
            dirty = true;
        }
        // 仅在对端在线且取得有效电量时上报；重新上线时(was_online 为假)立即补报。
        if (state.online && !state.connection_failed && battery.percent >= 0 &&
            (model.now_us >= battery_report_at || !was_online)) {
            EmergencyRemote::report_battery(static_cast<uint8_t>(battery.percent));
            battery_report_at = model.now_us + RuntimeSettings::get("report_ms") * 1000LL;
        }
        was_online = state.online && !state.connection_failed;
        // 仅在有变化时重绘，降低屏幕刷新开销。
        EmergencyUi::render(model, dirty);
    }
}
