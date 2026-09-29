#include "app_runtime.h"

#include "battery_level.h"
#include "battery_voltage.h"
#include "blackbox.h"
#include "blackbox_service.h"
#include "button_input.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace AppRuntime {
namespace {

constexpr char TAG[] = "AppRuntime";
constexpr TickType_t INPUT_IDLE_POLL_TICKS = pdMS_TO_TICKS(20);
constexpr TickType_t USB_STATE_POLL_TICKS = pdMS_TO_TICKS(50);

// 异步电池回调执行时，app_main 的栈上启动上下文可能已离开当前作用域，
// 因此只保存回调真正需要的充电状态，不持有外部对象指针。
bool boot_battery_charging;

const char* wake_source_name(PowerManager::WakeSource source) {
    switch (source) {
        case PowerManager::WakeSource::POWER_ON:
            return "power_on";
        case PowerManager::WakeSource::BUTTON:
            return "button";
        case PowerManager::WakeSource::USB:
            return "usb";
        case PowerManager::WakeSource::BUTTON_AND_USB:
            return "button_and_usb";
        case PowerManager::WakeSource::OTHER:
        default:
            return "other";
    }
}

const char* reset_reason_name(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "power_on";
        case ESP_RST_EXT: return "external";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt_wdt";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_WDT: return "other_wdt";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        case ESP_RST_USB: return "usb";
        case ESP_RST_JTAG: return "jtag";
        case ESP_RST_EFUSE: return "efuse";
        case ESP_RST_PWR_GLITCH: return "power_glitch";
        case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
        case ESP_RST_UNKNOWN:
        default:
            return "unknown";
    }
}

bool is_button_wakeup(PowerManager::WakeSource source) {
    return source == PowerManager::WakeSource::BUTTON ||
           source == PowerManager::WakeSource::BUTTON_AND_USB;
}

void log_battery_sample(esp_err_t result, int voltage_mv, void*) {
    if (result != ESP_OK) {
        (void)BlackboxService::append_text_event(
            "battery: result=%s", esp_err_to_name(result));
        return;
    }

    const BatteryLevel::Status level =
        BatteryLevel::update(voltage_mv, boot_battery_charging);
    (void)BlackboxService::append_text_event(
        "battery: voltage_mv=%d estimated=%u displayed=%u charging=%u rtc=%u",
        voltage_mv,
        static_cast<unsigned>(level.estimated_percent),
        static_cast<unsigned>(level.displayed_percent),
        level.charging ? 1U : 0U,
        level.restored_from_rtc ? 1U : 0U);
}

} // namespace

BootContext capture_boot_context() {
    const PowerManager::WakeSource wake_source = PowerManager::wake_source();
    const bool button_pressed = PowerManager::button_pressed();
    return {
        .wake_source = wake_source,
        .usb_mode = PowerManager::usb_connected(),
        .process_button = is_button_wakeup(wake_source) || button_pressed,
        .button_pressed_at_boot = button_pressed,
    };
}

void append_boot_diagnostics(const BootContext& boot) {
    (void)BlackboxService::append_text_event(
        "boot: reset=%s wake=%s usb=%u button=%u heap_free=%lu heap_min=%lu",
        reset_reason_name(esp_reset_reason()),
        wake_source_name(boot.wake_source),
        boot.usb_mode ? 1U : 0U,
        boot.button_pressed_at_boot ? 1U : 0U,
        static_cast<unsigned long>(esp_get_free_heap_size()),
        static_cast<unsigned long>(esp_get_minimum_free_heap_size()));
    (void)BlackboxService::append_text_event(
        "firmware: version=%d.%d.%d build=%s",
        VERSION_MAJOR,
        VERSION_MINOR,
        VERSION_PATCH,
        BUILD_TIME);
}

esp_err_t start_boot_battery_sample(bool charging) {
    boot_battery_charging = charging;
    return BatteryVoltage::start_async(log_battery_sample);
}

void log_radio_ready(const BootContext& boot) {
    ESP_LOGI(TAG,
             "radio ready usb=%u button_transaction=%u",
             boot.usb_mode ? 1U : 0U,
             boot.process_button ? 1U : 0U);
}

void log_maintenance_ready() {
    ESP_LOGI(TAG, "maintenance mode ready");
}

void wait_for_maintenance_disconnect() {
    while (PowerManager::usb_connected()) {
        vTaskDelay(USB_STATE_POLL_TICKS);
    }
    ESP_LOGI(TAG, "maintenance interface disconnected");
}

esp_err_t sleep_when_inputs_idle() {
    // GPIO 仍处于有效电平时直接睡眠会立即再次唤醒，因此统一等待所有输入和
    // 按键无线事务完成。这里也是应用进入深睡的唯一出口。
    while (PowerManager::usb_connected() ||
           PowerManager::button_pressed() ||
           ButtonInput::is_busy()) {
        vTaskDelay(INPUT_IDLE_POLL_TICKS);
    }
    ESP_LOGI(TAG, "inputs idle, entering deep sleep");

    int battery_voltage_mv = 0;
    const esp_err_t battery_result = BatteryVoltage::wait_mv(battery_voltage_mv);
    BatteryLevel::Status level = {};
    const bool level_valid = BatteryLevel::get_status(level);
    BlackboxService::Statistics statistics = {};
    BlackboxService::get_statistics(&statistics);

    // 休眠前写入一次资源快照，便于区分正常休眠和异常复位。
    (void)BlackboxService::append_text_event(
        "sleep: uptime_ms=%llu battery_mv=%d battery_pct=%u battery_result=%s "
        "heap_free=%lu heap_min=%lu records=%lu/%lu log_drop=%lu persist_fail=%lu",
        static_cast<unsigned long long>(esp_timer_get_time() / 1000),
        battery_voltage_mv,
        level_valid ? static_cast<unsigned>(level.displayed_percent) : 0U,
        esp_err_to_name(battery_result),
        static_cast<unsigned long>(esp_get_free_heap_size()),
        static_cast<unsigned long>(esp_get_minimum_free_heap_size()),
        static_cast<unsigned long>(Blackbox::count()),
        static_cast<unsigned long>(Blackbox::capacity()),
        static_cast<unsigned long>(statistics.dropped_logs),
        static_cast<unsigned long>(statistics.persist_failures));
    (void)BlackboxService::sync();
    return PowerManager::enter_deep_sleep();
}

} // namespace AppRuntime
