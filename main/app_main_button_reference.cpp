#include "HXC_NVS.h"
#include "app_runtime.h"
#include "battery_voltage.h"
#include "blackbox.h"
#include "blackbox_service.h"
#include "button_input.h"
#include "esp_err.h"
#include "espnow_remote.h"
#include "power_manager.h"
#include "shell_command.h"
#include "status_led.h"

extern "C" void app_main(void) {
    // 第一阶段：尽早识别唤醒来源并启动按键跟踪，避免后续初始化影响按压计时。
    ESP_ERROR_CHECK(PowerManager::init());
    const AppRuntime::BootContext boot = AppRuntime::capture_boot_context();

    ESP_ERROR_CHECK(StatusLed::init());
    if (boot.process_button) {
        ESP_ERROR_CHECK(ButtonInput::start_wakeup_tracking());
    }

    // 第二阶段：初始化持久化、诊断和电池服务，并异步完成本次启动的电量采样。
    ESP_ERROR_CHECK(Blackbox::init());
    ESP_ERROR_CHECK(HXC::NVS_Base::setup());
    static constexpr const char* info_tags[] = {
        "AppRuntime", "ButtonInput", "EspNowPairing", "EspNowRemote", "PowerManager",
    };
    ESP_ERROR_CHECK(BlackboxService::init({info_tags, sizeof(info_tags) / sizeof(info_tags[0])}));
    ESP_ERROR_CHECK(BatteryVoltage::init());
    AppRuntime::append_boot_diagnostics(boot);
    ESP_ERROR_CHECK(AppRuntime::start_boot_battery_sample(boot.usb_mode));

    // 无按键事务且未连接维护接口时无需启动射频，直接完成收尾并休眠。
    if (!boot.usb_mode && !boot.process_button) {
        ESP_ERROR_CHECK(AppRuntime::sleep_when_inputs_idle());
        return;
    }

    // 第三阶段：启动 ESP-NOW 遥控服务，并释放可能正在等待无线就绪的按键命令。
    ESP_ERROR_CHECK(EspNowRemote::init());
    ButtonInput::notify_radio_ready();
    AppRuntime::log_radio_ready(boot);

    // 电池唤醒模式只处理当前按键事务，完成后立即回到深度休眠。
    if (!boot.usb_mode) {
        ESP_ERROR_CHECK(AppRuntime::sleep_when_inputs_idle());
        return;
    }

    // 维护模式保持 Shell、常驻按键和电池校准任务运行，接口断开后停止配对并休眠。
    ESP_ERROR_CHECK(
        BatteryVoltage::start_calibration_monitor(PowerManager::usb_connected));
    ESP_ERROR_CHECK(ShellCommand::init());
    ESP_ERROR_CHECK(ButtonInput::init_usb_mode());
    AppRuntime::log_maintenance_ready();

    AppRuntime::wait_for_maintenance_disconnect();
    EspNowRemote::stop_pairing();
    ESP_ERROR_CHECK(AppRuntime::sleep_when_inputs_idle());
}
