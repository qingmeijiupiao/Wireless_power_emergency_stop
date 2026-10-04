/** @file runtime_diagnostics.cpp
 * @brief 实时运行与资源摘要；全部使用普通 INFO，保持黑匣子只记录事件。
 */
#include "runtime_diagnostics.h"
#include "app_diagnostics.h"
#include "battery_voltage.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "power_manager.h"

namespace AppController {
void RuntimeDiagnostics::report(const EmergencyUi::Model& model) {
    static constexpr char kTag[] = "AppRuntime";
    const auto& s = model.remote;
    if (model.now_us >= status_at_) {
        status_at_ = model.now_us + 1000000;
        APP_LOGI(kTag, "state=%s stop=%u output=%u confirmed=%u close_timeout=%u online=%u paired=%u pairing=%u busy=%u",
                 EmergencyRemote::state_name(s.state), s.stop_closed, s.output_on, s.output_confirmed,
                 s.stop_timed_out, s.online, s.paired, s.pairing, s.busy);
        APP_LOGI(kTag, "telemetry age_ms=%lld V_mv=%u I_ua=%ld board_cC=%d chip_cC=%d protection=0x%x",
                 static_cast<long long>(s.data_time_us > 0 ? (model.now_us-s.data_time_us)/1000 : -1),
                 s.data.voltage_mv, static_cast<long>(s.data.current_ua), s.data.board_temperature_centi_c,
                 s.data.chip_temperature_centi_c, s.protection_mask);
        APP_LOGI(kTag, "battery_mv=%d soc=%d usb=%u always_on=%u sleep_block=%s countdown=%u seconds=%lu adc_busy=%u",
                 model.battery_mv, model.battery_percent, model.usb, model.always_on,
                 PowerManager::sleep_block_name(PowerManager::block()), model.sleep.countdown,
                 static_cast<unsigned long>(model.sleep.seconds), BatteryVoltage::is_busy());
    }
    if (model.now_us >= health_at_) {
        health_at_ = model.now_us + 10000000;
        APP_LOGI(kTag, "heap_free=%u heap_min=%u app_stack_free=%u log_dropped=%lu gestures_dropped=%lu",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                 static_cast<unsigned long>(AppDiagnostics::dropped()),
                 static_cast<unsigned long>(EmergencyUi::dropped_gestures()));
    }
}
}
