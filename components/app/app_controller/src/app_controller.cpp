/**
 * @file app_controller.cpp
 * @brief 产品运行期编排：创建协调任务、组装 UI 模型并按序调用私有功能模块。
 */
#include "app_controller.h"

#include "battery_monitor.h"
#include "event_recorder.h"
#include "input_actions.h"
#include "sleep_coordinator.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hardware.h"
#include "power_manager.h"
#include "runtime_settings.h"

namespace AppController {
namespace {
// 生命周期覆盖采样回调与协调任务，不把异步回调指向任务栈上的临时对象。
BatteryMonitor battery_monitor;
TaskHandle_t controller_task = nullptr; // 成功创建后的任务句柄，同时用于防止重复启动。

/**
 * @brief 从本轮快照与电池副本构建 UI 模型，休眠计划稍后填入。
 * @param remote 动作执行后的远端快照。
 * @param battery 当前电池显示值副本，百分比可能未知。
 * @param now_us 当前 esp_timer 时间，单位微秒。
 * @return 包含远端、电池、供电、常亮模式和时间的模型。
 */
EmergencyUi::Model make_ui_model(const EmergencyRemote::Snapshot &remote, const BatteryView &battery,
                                int64_t now_us) {
    EmergencyUi::Model model;
    model.remote = remote;
    model.battery_mv = battery.voltage_mv;
    model.battery_percent = battery.percent;
    model.usb = Hardware::usb_connected();
    model.always_on = RuntimeSettings::always_on();
    model.now_us = now_us;
    return model;
}

/**
 * @brief 协调任务入口，独占 UI 状态，发布 OLED 帧并编排休眠显示钩子。
 * @note 每轮延时 20 ms 后执行；实际周期还包含各模块的处理耗时。
 *       急停控制、按键识别及 ADC 采样各自在独立任务中继续运行。
 */
void run(void *) {
    battery_monitor.begin(esp_timer_get_time(), Hardware::usb_connected());
    EmergencyUi::restore();
    // 提交屏幕启动后建立静置基准，避免把初始化耗时计为用户空闲。
    PowerManager::init_idle(esp_timer_get_time());
    EventRecorder events;
    SleepCoordinator sleep;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(20));
        const int64_t tick = esp_timer_get_time();
        const auto battery_update = battery_monitor.poll(tick, Hardware::usb_connected());
        bool redraw = battery_update.redraw;
        if (battery_update.activity)
            PowerManager::note_activity(tick);

        const auto input = EmergencyUi::handle_input(EmergencyRemote::snapshot(), tick);
        if (dispatch_input(input, tick))
            sleep.request_manual();
        // 有副作用的处理放在 || 左侧，即使已要求重绘也必须执行休眠协调。
        redraw = sleep.process_manual(tick) || redraw;

        // 动作执行后重新取远端快照，日志、模型和上报使用同一份数据。
        const auto remote = EmergencyRemote::snapshot();
        events.observe(remote);
        auto model = make_ui_model(remote, battery_monitor.view(), esp_timer_get_time());
        if (EmergencyUi::observe_state(model))
            PowerManager::note_activity(model.now_us);
        // 状态观察可能生成新的故障/低电提示，休眠计划必须读取更新后的截止时间。
        model.sleep = sleep.plan(model);
        redraw = sleep.process_auto(model.sleep, model.now_us) || redraw;
        battery_monitor.report(remote, model.battery_percent, model.now_us);
        EmergencyUi::render(model, redraw);
    }
}
} // namespace

esp_err_t start() {
    if (controller_task != nullptr)
        return ESP_ERR_INVALID_STATE;
    const esp_err_t result = battery_monitor.init();
    if (result != ESP_OK)
        return result;
    // 沿用原协调循环的栈预算与优先级；后台采样已发起，不等待结果或屏幕就绪。
    return xTaskCreate(run, "app_controller", CONFIG_ESP_MAIN_TASK_STACK_SIZE, nullptr, 1, &controller_task) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}
} // namespace AppController
