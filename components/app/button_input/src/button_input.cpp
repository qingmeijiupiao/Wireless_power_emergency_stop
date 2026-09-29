/*
 * @version: 2.1
 * @LastEditors: qingmeijiupiao
 * @Description: GPIO4 短按关闭、长按开启的低延时按键事务
 * @author: qingmeijiupiao
 * @LastEditTime: 2026-06-07
 */
#include "button_input.h"

#include <atomic>
#include <new>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "espnow_remote.h"
#include "espnow_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power_manager.h"
#include "status_led.h"

namespace ButtonInput {
namespace {

constexpr char TAG[] = "ButtonInput";
constexpr gpio_num_t BUTTON_GPIO = GPIO_NUM_4;
constexpr uint32_t POLL_MS = 10;
constexpr uint32_t TRIGGER_BLINK_MS = 100;
constexpr uint32_t FAILURE_BLINK_MS = 250;
constexpr uint32_t REJECTED_BLINK_MS = 80;

std::atomic_bool busy;
std::atomic_bool radio_ready;
TaskHandle_t usb_task_handle;

// 命令任务与按键任务分离：长按触发后可以立即闪灯反馈，无需在按键采集
// 循环中同步等待信道恢复、链路 ACK 和业务响应。
struct CommandContext {
    EspNowService::SwitchAction action;
    TaskHandle_t owner;
};

void command_task(void* arg) {
    const CommandContext context = *static_cast<CommandContext*>(arg);
    delete static_cast<CommandContext*>(arg);

    const int64_t started_us = esp_timer_get_time();
    // 唤醒按键跟踪早于无线初始化，先生成的命令在此等待 app_runtime 通知。
    while (!radio_ready.load()) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    EspNowService::SwitchResult context_result = EspNowService::SwitchResult::INTERNAL_ERROR;
    bool context_output = false;
    const esp_err_t ret = EspNowRemote::send_switch(
        context.action, true, true, &context_result, &context_output);
    const int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000;
    ESP_LOGI(TAG, "remote command action=%s result=%s elapsed_ms=%lld",
             context.action == EspNowService::SwitchAction::ON ? "on" : "off",
             esp_err_to_name(ret),
             static_cast<long long>(elapsed_ms));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "remote command failed: %s", esp_err_to_name(ret));
        StatusLed::blink(2, FAILURE_BLINK_MS, FAILURE_BLINK_MS);
    } else if (context_result != EspNowService::SwitchResult::OK) {
        // 链路送达但对端业务拒绝执行，用更快的双闪区分于通信失败。
        ESP_LOGW(TAG, "remote command rejected: result=%u output=%u",
                 static_cast<unsigned>(context_result), context_output ? 1U : 0U);
        StatusLed::blink(2, REJECTED_BLINK_MS, REJECTED_BLINK_MS);
    }
    xTaskNotifyGive(context.owner);
    vTaskDelete(nullptr);
}

esp_err_t start_command(EspNowService::SwitchAction action) {
    auto* context = new (std::nothrow) CommandContext{action, xTaskGetCurrentTaskHandle()};
    if (context == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(command_task, "button_command", 4096, context, 3, nullptr) != pdPASS) {
        delete context;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void process_press(uint32_t initial_duration_ms) {
    busy.store(true);
    StatusLed::on();
    ESP_LOGI(TAG, "button transaction start initial_ms=%lu",
             static_cast<unsigned long>(initial_duration_ms));

    uint32_t duration_ms = initial_duration_ms;
    bool long_triggered = false;
    bool command_started = false;

    // 唤醒场景传入系统初始化期间已经累计的按压时长，避免长按阈值漂移。
    while (gpio_get_level(BUTTON_GPIO) == 0) {
        if (!long_triggered && duration_ms >= PowerManager::LONG_PRESS_MS) {
            long_triggered = true;
            StatusLed::off();
            command_started =
                start_command(EspNowService::SwitchAction::ON) == ESP_OK;
            ESP_LOGI(TAG, "long press reached %lu ms: remote on",
                     static_cast<unsigned long>(duration_ms));
            StatusLed::blink(3, TRIGGER_BLINK_MS, TRIGGER_BLINK_MS);
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        duration_ms += POLL_MS;
    }

    if (!long_triggered) {
        StatusLed::off();
        command_started =
            start_command(EspNowService::SwitchAction::OFF) == ESP_OK;
        ESP_LOGI(TAG, "short press released at %lu ms: remote off",
                 static_cast<unsigned long>(duration_ms));
        StatusLed::blink(1, TRIGGER_BLINK_MS, TRIGGER_BLINK_MS);
    }

    if (command_started) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    } else {
        ESP_LOGE(TAG, "failed to create command task");
        StatusLed::blink(2, FAILURE_BLINK_MS, FAILURE_BLINK_MS);
    }
    ESP_LOGI(TAG, "button transaction complete action=%s duration_ms=%lu",
             long_triggered ? "on" : "off",
             static_cast<unsigned long>(duration_ms));
    busy.store(false);
}

void wakeup_tracking_task(void*) {
    process_press(PowerManager::button_press_elapsed_ms());
    vTaskDelete(nullptr);
}

void usb_button_task(void*) {
    while (true) {
        // busy 同时覆盖按键采集和同步无线请求，防止维护模式下重入同一事务。
        if (busy.load() || gpio_get_level(BUTTON_GPIO) != 0) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            continue;
        }
        process_press(0);
    }
}

} // namespace

esp_err_t start_wakeup_tracking() {
    if (busy.exchange(true)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(wakeup_tracking_task, "wakeup_button", 4096, nullptr, 4,
                    nullptr) != pdPASS) {
        busy.store(false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void notify_radio_ready() {
    radio_ready.store(true);
}

esp_err_t init_usb_mode() {
    if (usb_task_handle != nullptr) {
        return ESP_OK;
    }
    return xTaskCreate(usb_button_task, "button_input", 4096, nullptr, 3,
                       &usb_task_handle) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

bool is_busy() {
    return busy.load();
}

} // namespace ButtonInput
