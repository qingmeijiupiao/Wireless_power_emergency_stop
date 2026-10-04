/** @file app_diagnostics.cpp
 * @brief 异步日志消费者；所有队列项目按值持有，超时/丢弃路径无悬空指针。
 */
#include "app_diagnostics.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <atomic>
#include <cstdarg>
#include <cstdio>
namespace AppDiagnostics {
namespace {
struct Event {
    esp_log_level_t level;
    const char* tag;
    char text[192];
};
QueueHandle_t events = nullptr;
std::atomic<uint32_t> accepted{0}, completed{0}, losses{0};
/** @brief 串行输出；使用 ESP 日志出口保留现有黑匣子 tag 捕获策略。 */
void worker(void*) {
    Event event{};
    for (;;) {
        if (xQueueReceive(events, &event, portMAX_DELAY) == pdTRUE) {
            ESP_LOG_LEVEL(event.level, event.tag, "%s", event.text);
            completed.fetch_add(1, std::memory_order_release);
        }
    }
}
}
esp_err_t init() {
    if (events) return ESP_OK;
    events = xQueueCreate(32, sizeof(Event));
    if (!events) return ESP_ERR_NO_MEM;
    if (xTaskCreate(worker, "app_diagnostics", 3072, nullptr, 1, nullptr) != pdPASS) {
        vQueueDelete(events);
        events = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
void write(esp_log_level_t level, const char* tag, const char* format, ...) {
    if (!events || !tag || !format) { ++losses; return; }
    Event event{};
    event.level = level;
    event.tag = tag;
    va_list args;
    va_start(args, format);
    vsnprintf(event.text, sizeof(event.text), format, args);
    va_end(args);
    if (xQueueSend(events, &event, 0) == pdTRUE) ++accepted;
    else ++losses;
}
bool flush(TickType_t timeout) {
    const uint32_t target = accepted.load(std::memory_order_acquire);
    const TickType_t start = xTaskGetTickCount();
    while (static_cast<int32_t>(completed.load(std::memory_order_acquire) - target) < 0) {
        if (xTaskGetTickCount() - start >= timeout) return false;
        vTaskDelay(1);
    }
    return true;
}
uint32_t dropped() { return losses.load(); }
}
