/**
 * @file power_manager.cpp
 * @brief 电源管理实现：睡眠阻塞判定、60 秒静置倒计时与深睡进入/中止恢复流程。
 */
#include "power_manager.h"
#include "hardware.h"
#include "battery_status.h"
#include "battery_voltage.h"
#include "app_diagnostics.h"
#include "emergency_remote.h"
#include "runtime_settings.h"
#include "blackbox.h"
#include "blackbox_service.h"
#include "app_diagnostics.h"
#include "esp_sleep.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
namespace PowerManager {
namespace {
using namespace Hardware;
constexpr char TAG[] = "PowerManager";
constexpr char kEventTag[] = "ProductEvent";
// RTC 保留内存中的睡眠标记与触点状态：深睡后复位不清除，用于唤醒时判断是否由“释放”触发。
RTC_DATA_ATTR uint32_t sleep_cookie = 0;
RTC_DATA_ATTR bool sleep_contact_low = false;
// 睡眠标记魔数：仅当精确匹配且与触点状态一致时才认为是合法的释放唤醒。
constexpr uint32_t kSleepCookie = 0x45535450;
int64_t activity_at = 0;       // 最近一次用户活动时间（微秒），作为静置计时起点
SleepCountdown countdown;      // 静置倒计时状态机
bool countdown_active = false; // 上一次评估是否处于倒计时，仅用于状态变化时打点
} // namespace
// 启动早期调用：仅当本次由 GPIO 唤醒、睡眠标记有效、入睡时触点闭合且现在已释放时返回 true。
bool consume_release_wake() {
    const bool released = (esp_sleep_get_wakeup_causes() & (1U << ESP_SLEEP_WAKEUP_GPIO)) != 0 &&
                          sleep_cookie == kSleepCookie && sleep_contact_low && !Hardware::stop_closed();
    sleep_cookie = 0;
    return released;
}
// 初始化静置基准并清空倒计时，避免沿用上一轮残留状态。
void init_idle(int64_t now_us) {
    activity_at = now_us;
    countdown = {};
    countdown_active = false;
}
// 刷新活动时间：任何用户交互都会把静置截止时间向后推。
void note_activity(int64_t now_us) { activity_at = now_us; }
// 计算静置截止时间（取活动时间与提醒截止的较晚者），并在允许且无阻塞时推进倒计时。
SleepPlan plan(int64_t now_us, int64_t notice_deadline, bool notice_allowed, bool failed) {
    int64_t deadline = activity_at + RuntimeSettings::get(RuntimeSettings::Id::IdleMs) * 1000LL;
    if (notice_deadline > deadline)
        deadline = notice_deadline;
    // always_on 模式默认禁止睡眠，仅在 failed 兜底时才允许；同时必须不存在任何睡眠阻塞。
    const bool allowed = (!RuntimeSettings::always_on() || failed) && block() == SleepBlock::None;
    const auto result = countdown.update(now_us, deadline, allowed && notice_allowed);
    if (result.countdown != countdown_active) {
        APP_LOGI(kEventTag, "sleep countdown %s seconds=%lu",
                       result.countdown ? "started"
                       : result.due     ? "completed"
                                        : "cancelled",
                       static_cast<unsigned long>(result.seconds));
        countdown_active = result.countdown;
    }
    return result;
}
// 汇总硬件电平与遥控快照，映射为睡眠阻塞原因：
// USB=检测到外部供电；输出=输出接通或状态新鲜但未知；忙=遥控事务/配对中；按钮=按键按下。
SleepBlock block() {
    const auto s = EmergencyRemote::snapshot();
    const bool confirmed_fresh = s.output_confirmed && s.output_time_us > 0 &&
        esp_timer_get_time() - s.output_time_us < RuntimeSettings::get(RuntimeSettings::Id::FreshMs) * 1000LL;
    return sleep_block(gpio_get_level(kVbusDetect), s.output_on && !s.stop_timed_out,
                       s.stop_timed_out || confirmed_fresh,
                       s.busy || s.pairing, gpio_get_level(kUiButton) == 0);
}

// 进入深睡主流程；可能不返回。依次：前置判定 -> 无线握手 -> 配置唤醒源 -> 关显示
// -> 同步黑匣子 -> 隔离 GPIO/USB -> 安全复查 -> 触发深睡；任一环节被唤醒或复核失败则中止并恢复。
int enter_sleep(const DisplayHooks &display, bool manual) {
    BatteryLevel::Status battery = {};
    (void)BatteryStatus::get_status(battery);
    // 先做一次快速阻塞判定，避免无谓地走完整流程。
    auto reason = block();
    APP_LOGI(kEventTag, "sleep request source=%s block=%s battery_mv=%d soc=%d", manual ? "manual" : "idle",
                   sleep_block_name(reason), battery.voltage_mv,
                   static_cast<int>(battery.displayed_percent));
    if (reason != SleepBlock::None)
        return static_cast<int>(reason);
    // 与遥控工作线程握手：让其把未完成的开关/配对事务处理干净再睡。
    if (!EmergencyRemote::prepare_sleep()) {
        APP_LOGI(kEventTag, "sleep denied: remote not quiesced");
        return 4;
    }
    if (!BatteryVoltage::prepare_sleep()) {
        BatteryVoltage::cancel_sleep();
        EmergencyRemote::cancel_sleep();
        APP_LOGI(kEventTag, "sleep denied: battery not quiesced");
        return 4;
    }
    // 静止后再取得最后完成的读数，避免睡前样本仍留在协调器邮箱中未消费。
    int last_voltage_mv = 0;
    if (BatteryVoltage::wait_mv(last_voltage_mv, 0) == ESP_OK)
        battery = BatteryStatus::update(last_voltage_mv, Hardware::usb_connected());
    // 记录入睡时的急停触点状态，唤醒时将据此判断是“闭合”还是“释放”边沿。
    const bool contact_low = gpio_get_level(kStopButton) == 0;
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    // 配置 GPIO 唤醒源：按钮低电平、USB 插入高电平，急停触点按当前状态取反边沿。
    esp_err_t err = esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(1ULL << kUiButton, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (err == ESP_OK)
        err = esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(1ULL << kVbusDetect, ESP_GPIO_WAKEUP_GPIO_HIGH);
    if (err == ESP_OK)
        err = esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(
            1ULL << kStopButton, contact_low ? ESP_GPIO_WAKEUP_GPIO_HIGH : ESP_GPIO_WAKEUP_GPIO_LOW);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sleep wake configuration failed: %s", esp_err_to_name(err));
        EmergencyRemote::cancel_sleep();
        BatteryVoltage::cancel_sleep();
        return 7;
    }
    // 先关闭显示，降低睡眠过程中的功耗与干扰，再同步黑匣子。
    display.prepare();
    if (!display.shutdown()) {
        EmergencyRemote::cancel_sleep();
        BatteryVoltage::cancel_sleep();
        display.restore();
        APP_LOGI(kEventTag, "sleep denied: display not quiesced");
        return 4;
    }
    if (Blackbox::is_enabled()) {
        const auto remote = EmergencyRemote::snapshot();
        APP_LOGI(kEventTag, "sleep prepare source=%s battery_mv=%d soc=%d contact_low=%u wake=GPIO3/4/5",
                       manual ? "manual" : "idle", battery.voltage_mv,
                       static_cast<int>(battery.displayed_percent), contact_low);
        APP_LOGI(kEventTag, "sleep output=%u confirmed=%u close_timeout=%u policy=%s",
                 remote.output_on, remote.output_confirmed, remote.stop_timed_out,
                 remote.stop_timed_out ? "allow_after_OFF_timeout" : "confirmed_OFF");
        if (!AppDiagnostics::flush())
            ESP_LOGW(TAG, "app diagnostics flush timed out");
        const auto sync_err = BlackboxService::sync();
        if (sync_err != ESP_OK)
            ESP_LOGW(TAG, "Blackbox sleep sync failed: %s", esp_err_to_name(sync_err));
    }
    // 隔离屏幕总线、电池分压与屏供电，防止深睡期间漏电。
    Hardware::disconnect_screen_bus();
    Hardware::disconnect_battery_divider();
    Hardware::screen_power(false);
    // 在较慢的 I2C 关断之后再次复查，此时遥控工作线程仍然存活。
    reason = block();
    // 复查未通过（出现新阻塞、触点状态变化或遥控不再静止）则取消睡眠并恢复显示。
    if (reason != SleepBlock::None || contact_low != (gpio_get_level(kStopButton) == 0) ||
        !EmergencyRemote::snapshot().quiesced) {
        APP_LOGI(kEventTag, "sleep aborted before entry block=%s contact_changed=%u",
                       sleep_block_name(reason), contact_low != (gpio_get_level(kStopButton) == 0));
        EmergencyRemote::cancel_sleep();
        BatteryVoltage::cancel_sleep();
        Hardware::screen_power(true);
        vTaskDelay(pdMS_TO_TICKS(100));
        display.restore();
        return reason == SleepBlock::None ? 4 : static_cast<int>(reason);
    }
    // 记录触点状态并写入睡眠标记，供唤醒时的 consume_release_wake() 校验。
    sleep_contact_low = contact_low;
    sleep_cookie = kSleepCookie;
    // 把 GPIO 配置为深睡所需的隔离态（隔离/上拉/浮动）。
    configure_sleep_gpio();
    ESP_LOGI(TAG,
             "SLEEP_GPIO gpio0/6/10=isolated hold=off; gpio3=pullup gpio4=floating gpio5=external-pullup");
    Hardware::dump_sleep_gpio();
    ESP_LOGI(TAG, "DEEP_SLEEP contact_low=%u wake=GPIO3/4/5", contact_low);
    // 此处之后原生 USB 将不可用；若入睡途中被唤醒而深睡被拒绝，需通过驱动恢复其 PHY。
    (void)usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(20));
    isolate_usb_for_sleep();
    err = esp_deep_sleep_try_to_start();
    // 入睡途中到来的唤醒信号会中止深睡；以下代码负责恢复屏幕与各项服务。
    sleep_cookie = 0;
    release_sleep_holds();
    restore_usb_after_sleep_abort();
    Hardware::restore_screen_pin();
    EmergencyRemote::cancel_sleep();
    BatteryVoltage::cancel_sleep();
    Hardware::screen_power(true);
    vTaskDelay(pdMS_TO_TICKS(100));
    display.restore();
    ESP_LOGW(TAG, "SLEEP_ENTRY_ABORTED %s", esp_err_to_name(err));
    APP_LOGI(kEventTag, "sleep rejected by hardware: %s", esp_err_to_name(err));
    return 4;
}
} // namespace PowerManager
