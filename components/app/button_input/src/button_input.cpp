/**
 * @file button_input.cpp
 * @brief BOOT 按键输入实现：绑定公共 Button 回调，只把短按/长按手势写入事件队列。
 */
#include "button_input.h"
#include "Button.h"
#include "hardware.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
namespace ButtonInput {
namespace {
constexpr char TAG[] = "ButtonInput";
QueueHandle_t events = nullptr; // 按键事件队列，跨任务传递 Button 回调产生的事件
Button button;                  // 公共按键驱动实例，运行在独立扫描任务中
// 唤醒时 BOOT 可能仍被按住，抑制到释放为止，避免把唤醒按压识别为手势。
bool suppress = false;
void post(Event event) {
    if (suppress || events == nullptr)
        return;
    (void)xQueueSend(events, &event, 0);
}
void on_short() { post(Event::Short); }
void on_long() { post(Event::Long); }
// 释放回调只用于解除唤醒抑制，不产生 UI 事件，避免长按松手被当作额外手势。
void on_release() {
    if (suppress)
        suppress = false;
}
} // namespace
void init() {
    events = xQueueCreate(8, sizeof(Event));
    if (events == nullptr) {
        ESP_LOGE(TAG, "event queue create failed");
        return;
    }
    suppress = Hardware::button_pressed();
    button.bind_event(ButtonEvent::RELEASE, on_release);
    button.bind_event(ButtonEvent::SHORT_PRESS, on_short);
    button.bind_event(ButtonEvent::LONG_PRESS, on_long);
    const esp_err_t err = button.setup(Hardware::kUiButton, true);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "button setup failed: %s", esp_err_to_name(err));
}
bool poll(Event& event) {
    if (events == nullptr)
        return false;
    Event incoming = Event::None;
    if (xQueueReceive(events, &incoming, 0) != pdTRUE)
        return false;
    event = incoming;
    return true;
}
} // namespace ButtonInput
