/**
 * @file app_main.cpp
 * @brief 高层启动入口，只编排有依赖的初始化与任务启动。
 */
#include "app_controller.h"
#include "app_diagnostics.h"
#include "battery_status.h"
#include "battery_voltage.h"
#include "blackbox.h"
#include "blackbox_service.h"
#include "boot_diagnostics.h"
#include "emergency_remote.h"
#include "emergency_ui.h"
#include "hardware.h"
#include "power_manager.h"
#include "runtime_settings.h"
#include "shell_command.h"

#include "HXC_NVS.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace {
constexpr char kTag[] = "app_main";
constexpr const char *kInfoTags[] = {AppController::kEventTag};

/** @brief 急停下降沿 ISR：只锁存关闭请求，实际事务由遥控工作线程处理。 */
void IRAM_ATTR on_stop_fall(void *) {
    EmergencyRemote::stop_from_isr();
}
} // namespace

/** @brief ESP-IDF 启动入口：完成依赖初始化、创建独立任务并注册 Shell 后返回。 */
extern "C" void app_main(void) {
    Hardware::init(on_stop_fall);
    // 供电稳定窗口从 GPIO 初始化后起算，后续初始化耗时计入窗口；主任务不等待。
    const int64_t battery_ready_at = esp_timer_get_time() + 100000;
    const bool release_wake = PowerManager::consume_release_wake();
    ESP_ERROR_CHECK(EmergencyUi::init_buttons());

    // 公共存储和参数是无线、UI 的启动依赖，先完成初始化。
    ESP_ERROR_CHECK(HXC::NVS_Base::setup());
    if (Blackbox::init() != ESP_OK ||
        BlackboxService::init({kInfoTags, sizeof(kInfoTags) / sizeof(kInfoTags[0])}) != ESP_OK) {
        ESP_LOGW(kTag, "Blackbox unavailable; control remains active");
    }
    ESP_ERROR_CHECK(AppDiagnostics::init());
    RuntimeSettings::init();
    ESP_ERROR_CHECK(BatteryStatus::init());

    // 急停控制不等待供电稳定、首次电池采样或 OLED 初始化。
    ESP_ERROR_CHECK(EmergencyRemote::init(release_wake));
    ESP_ERROR_CHECK(BatteryVoltage::init(battery_ready_at));
    EmergencyUi::init();
    BootDiagnostics::append_boot(release_wake);

    // 协调器异步采样并向 OLED 任务发布启动请求，主任务继续注册 Shell。
    ESP_ERROR_CHECK(AppController::start());
    ESP_ERROR_CHECK(ShellCommand::init());
}
