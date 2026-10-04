/**
 * @file button_input.cpp
 * @brief UI 内部按键适配实现：绑定公共 Button 回调，只把短按/长按手势写入队列。
 */
#include "core/button_input.h"
#include "Button.h"
#include "hardware.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <atomic>
namespace EmergencyUi {
namespace Buttons {
namespace {
constexpr char TAG[] = "EmergencyUi";
QueueHandle_t events = nullptr; // 手势队列，跨任务传递 Button 回调产生的事件
Button button;                  // 公共按键驱动实例，运行在独立扫描任务中
std::atomic<uint32_t> losses{0};
// 唤醒时 BOOT 可能仍被按住，抑制到释放为止，避免把唤醒按压识别为手势。
bool suppress = false;
void post(Gesture gesture) {
    if (suppress || events == nullptr)
        return;
    if (xQueueSend(events, &gesture, 0) != pdTRUE) ++losses;
}
void on_short() { post(Gesture::Short); }
void on_long() { post(Gesture::Long); }
// 释放回调只用于解除唤醒抑制，不产生 UI 事件，避免长按松手被当作额外手势。
void on_release() {
    if (suppress)
        suppress = false;
}
} // namespace
esp_err_t init() {
    if (events) return ESP_ERR_INVALID_STATE;
    events = xQueueCreate(8, sizeof(Gesture));
    if (events == nullptr) {
        ESP_LOGE(TAG, "button queue create failed");
        return ESP_ERR_NO_MEM;
    }
    suppress = Hardware::button_pressed();
    button.bind_event(ButtonEvent::RELEASE, on_release);
    button.bind_event(ButtonEvent::SHORT_PRESS, on_short);
    button.bind_event(ButtonEvent::LONG_PRESS, on_long);
    const esp_err_t err = button.setup(Hardware::kUiButton, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "button setup failed: %s", esp_err_to_name(err));
        vQueueDelete(events);
        events = nullptr;
    }
    return err;
}
uint32_t dropped() { return losses.load(); }
bool poll(Gesture &gesture) {
    if (events == nullptr)
        return false;
    Gesture incoming = Gesture::None;
    if (xQueueReceive(events, &incoming, 0) != pdTRUE)
        return false;
    gesture = incoming;
    return true;
}
} // namespace Buttons
} // namespace EmergencyUi
