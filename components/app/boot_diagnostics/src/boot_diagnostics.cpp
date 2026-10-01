/**
 * @file boot_diagnostics.cpp
 * @brief 启动诊断实现：开机时把复位、唤醒、电池、配置与对端快照写入产品事件日志。
 */
// 依赖说明：
// - boot_diagnostics.h：本模块接口
// - hardware.h：按键/VBUS 等 GPIO 编号与 BUILD_TIME
// - battery_level.h：电池电压与 SOC 状态
// - runtime_settings.h：运行时配置快照
// - diagnostic_log.h：DEVICE_EVENT_I 产品事件日志宏
// - espnow_link.h：已保存的 ESP-NOW 对端信息
// - esp_app_desc.h：固件版本与 IDF 版本
// - esp_sleep.h：唤醒原因与 GPIO 唤醒状态
// - esp_system.h：复位原因查询
#include "boot_diagnostics.h"
#include "hardware.h"
#include "battery_level.h"
#include "runtime_settings.h"
#include "diagnostic_log.h"
#include "espnow_link.h"
#include "esp_app_desc.h"
#include "esp_sleep.h"
#include "esp_system.h"
namespace BootDiagnostics {
namespace {
// 统一事件标签，保证所有启动诊断记录可在日志中按同一标签检索。
constexpr char kEventTag[] = "ProductEvent";
}
// 见头文件说明：集中记录一次启动的现场快照，不做任何状态变更。
void append_boot(bool release_wake) {
    using namespace Hardware;
    // 读取电池状态；读取失败时后续以 0 / -1 占位，避免输出未初始化值。
    BatteryLevel::Status battery = {};
    const bool battery_valid = BatteryLevel::get_status(battery);
    // 首条记录：固件/构建/IDF 版本、复位原因、唤醒原因位图、GPIO 唤醒状态。
    DEVICE_EVENT_I(
        kEventTag,
        "boot firmware=%s built=%s idf=%s reset=%u wake_causes=0x%lx gpio_wake=0x%llx release_wake=%u",
        esp_app_get_description()->version, BUILD_TIME, esp_app_get_description()->idf_ver,
        static_cast<unsigned>(esp_reset_reason()), static_cast<unsigned long>(esp_sleep_get_wakeup_causes()),
        static_cast<unsigned long long>(esp_sleep_get_gpio_wakeup_status()), release_wake);
    // 仅当由深睡唤醒时，额外记录各按键/VBUS 引脚在唤醒瞬间的电平。
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP)
        DEVICE_EVENT_I(kEventTag, "wake from deep sleep GPIO=0x%llx stop=%d USB=%d BOOT=%d",
                       static_cast<unsigned long long>(esp_sleep_get_gpio_wakeup_status()),
                       gpio_get_level(kStopButton), gpio_get_level(kVbusDetect), gpio_get_level(kUiButton));
    // 持久化当前运行时配置快照（记录而非修改），并紧接着输出电池与配置摘要。
    RuntimeSettings::record_snapshot();
    DEVICE_EVENT_I(kEventTag, "boot battery_mv=%d soc=%d usb=%d stop=%d BOOT=%d always_on=%u peers=%u",
                   battery_valid ? battery.voltage_mv : 0,
                   battery_valid ? static_cast<int>(battery.displayed_percent) : -1,
                   gpio_get_level(kVbusDetect), gpio_get_level(kStopButton), gpio_get_level(kUiButton),
                   RuntimeSettings::always_on(),
                   static_cast<unsigned>(EspNowLink::get_saved_peer_count()));
    // 逐条输出已保存的 ESP-NOW 对端 MAC 与上次通信信道，便于确认配对关系。
    for (size_t i = 0; i < EspNowLink::get_saved_peer_count(); ++i) {
        EspNowLink::SavedPeer peer{};
        if (EspNowLink::get_saved_peer(i, &peer) == ESP_OK)
            DEVICE_EVENT_I(kEventTag, "boot peer[%u]=%02x:%02x:%02x:%02x:%02x:%02x channel=%u",
                           static_cast<unsigned>(i),
                           peer.address.bytes[0], peer.address.bytes[1], peer.address.bytes[2],
                           peer.address.bytes[3], peer.address.bytes[4], peer.address.bytes[5],
                           peer.last_channel);
    }
}
} // namespace BootDiagnostics
