/**
 * @file boot_diagnostics.cpp
 * @brief 启动诊断实现：开机时把复位、唤醒、电池、配置与对端快照写入产品事件日志。
 */
// 依赖说明：
// - boot_diagnostics.h：本模块接口
// - hardware.h：按键/VBUS 等 GPIO 编号与 BUILD_TIME
// - battery_level.h：电池电压与 SOC 状态
// - runtime_settings.h：运行时配置快照
// - app_diagnostics.h：异步 ProductEvent 日志出口
// - espnow_link.h：已保存的 ESP-NOW 对端信息
// - esp_app_desc.h：固件版本与 IDF 版本
// - esp_sleep.h：唤醒原因与 GPIO 唤醒状态
// - esp_system.h：复位原因查询
#include "boot_diagnostics.h"
#include "hardware.h"
#include "battery_status.h"
#include "runtime_settings.h"
#include "app_diagnostics.h"
#include "espnow_link.h"
#include "esp_app_desc.h"
#include "esp_sleep.h"
#include "esp_system.h"
namespace BootDiagnostics {
namespace {
// 统一事件标签，保证所有启动诊断记录可在日志中按同一标签检索。
constexpr char kEventTag[] = "ProductEvent";
struct WakeSnapshot {
    esp_reset_reason_t reset; /**< 本次复位来源。 */
    uint32_t causes;          /**< 唤醒来源位图，不折叠多来源唤醒。 */
    uint64_t gpio;            /**< SDK 报告的唤醒引脚位图。 */
    int stop, usb, button;    /**< GPIO 初始化完成后的早期电平。 */
};
WakeSnapshot wake{}; // 仅启动主任务写入；电平是启动采样值，不能宣称是唤醒边沿瞬间。
/** @brief 复位来源的可读名称；原始枚举同时保留用于版本兼容。 */
const char* reset_name(esp_reset_reason_t reason) {
    switch (reason) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "other";
    }
}
}
void capture_wake() {
    wake = {esp_reset_reason(), static_cast<uint32_t>(esp_sleep_get_wakeup_causes()),
            esp_sleep_get_gpio_wakeup_status(), gpio_get_level(Hardware::kStopButton),
            gpio_get_level(Hardware::kVbusDetect), gpio_get_level(Hardware::kUiButton)};
}
// 见头文件说明：集中记录一次启动的现场快照，不做任何状态变更。
void append_boot(bool release_wake) {
    using namespace Hardware;
    // 读取电池状态；读取失败时后续以 0 / -1 占位，避免输出未初始化值。
    BatteryLevel::Status battery = {};
    const bool battery_valid = BatteryStatus::get_status(battery);
    // 首条记录：固件/构建/IDF 版本、复位原因、唤醒原因位图、GPIO 唤醒状态。
    APP_LOGI(
        kEventTag,
        "boot firmware=%s reset=%s(%u) release_wake=%u",
        esp_app_get_description()->version, reset_name(wake.reset), static_cast<unsigned>(wake.reset), release_wake);
    APP_LOGI(kEventTag, "boot built=%s idf=%s", BUILD_TIME, esp_app_get_description()->idf_ver);
    APP_LOGI(kEventTag, "wake causes=0x%lx gpio=0x%llx stop=%u usb=%u button=%u",
             static_cast<unsigned long>(wake.causes), static_cast<unsigned long long>(wake.gpio),
             !!(wake.gpio & (1ULL << kStopButton)), !!(wake.gpio & (1ULL << kVbusDetect)),
             !!(wake.gpio & (1ULL << kUiButton)));
    APP_LOGI(kEventTag, "boot levels stop=%d usb=%d button=%d", wake.stop, wake.usb, wake.button);
    // 持久化当前运行时配置快照（记录而非修改），并紧接着输出电池与配置摘要。
    RuntimeSettings::record_snapshot();
    APP_LOGI(kEventTag, "boot battery_mv=%d soc=%d usb=%d stop=%d BOOT=%d always_on=%u",
                   battery_valid ? battery.voltage_mv : 0,
                   battery_valid ? static_cast<int>(battery.displayed_percent) : -1,
                   gpio_get_level(kVbusDetect), gpio_get_level(kStopButton), gpio_get_level(kUiButton),
                   RuntimeSettings::always_on());
}
void append_peers() {
    // 无线初始化后读取，避免在保存节点尚未加载时错误记录为空表。
    APP_LOGI(kEventTag, "boot peers=%u", static_cast<unsigned>(EspNowLink::get_saved_peer_count()));
    for (size_t i = 0; i < EspNowLink::get_saved_peer_count(); ++i) {
        EspNowLink::SavedPeer peer{};
        if (EspNowLink::get_saved_peer(i, &peer) == ESP_OK)
            APP_LOGI(kEventTag, "boot peer[%u]=%02x:%02x:%02x:%02x:%02x:%02x channel=%u",
                           static_cast<unsigned>(i),
                           peer.address.bytes[0], peer.address.bytes[1], peer.address.bytes[2],
                           peer.address.bytes[3], peer.address.bytes[4], peer.address.bytes[5],
                           peer.last_channel);
    }
}
} // namespace BootDiagnostics
