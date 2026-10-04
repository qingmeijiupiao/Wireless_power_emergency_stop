/** @file app_diagnostics.cpp
 * @brief 异步日志消费者；所有队列项目按值持有，超时/丢弃路径无悬空指针。
 */
#include "app_diagnostics.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
namespace AppDiagnostics {
namespace {
struct Event {
    esp_log_level_t level;
    const char* tag;
    int64_t occurred_ms;
    uint32_t sequence;
    char text[192];
};
constexpr unsigned kEventQueueSize = 32, kTraceQueueSize = 16;
QueueHandle_t events = nullptr, traces = nullptr;
TaskHandle_t worker_handle = nullptr;
std::atomic<uint32_t> accepted{0}, completed{0}, losses{0};
std::atomic<uint32_t> event_losses{0}, cuts{0}, sequence{0};
std::atomic<uint32_t> event_cuts{0};
/** @brief 串行输出；使用 ESP 日志出口保留现有黑匣子 tag 捕获策略。 */
void worker(void*) {
    Event event{};
    uint32_t reported_loss = 0, reported_cut = 0, reported_event_loss = 0, reported_event_cut = 0;
    for (;;) {
        // 优先处理事件；两队列均空时等待通知，不为日志增加固定周期唤醒。
        if (xQueueReceive(events, &event, 0) == pdTRUE ||
            xQueueReceive(traces, &event, 0) == pdTRUE) {
            if (event.sequence)
                ESP_LOG_LEVEL(event.level, event.tag, "e=%lu t=%lld %s",
                              static_cast<unsigned long>(event.sequence),
                              static_cast<long long>(event.occurred_ms), event.text);
            else
                ESP_LOG_LEVEL(event.level, event.tag, "%s", event.text);
            // 排空积压后汇总缺口；不为每次丢弃再入队，避免队满时递归制造日志。
            const uint32_t lost = losses.load(), cut = cuts.load();
            if (uxQueueSpacesAvailable(events) == kEventQueueSize && uxQueueSpacesAvailable(traces) == kTraceQueueSize &&
                (lost != reported_loss || cut != reported_cut)) {
                const uint32_t lost_events = event_losses.load(), cut_events = event_cuts.load();
                // 周期 INFO 的丢弃只作为实时诊断；关键事件缺失/截断才进入黑匣子。
                ESP_LOG_LEVEL(lost_events != reported_event_loss || cut_events != reported_event_cut ? ESP_LOG_WARN : ESP_LOG_INFO,
                         "AppDiagnostics", "log gap dropped=%lu events=%lu truncated=%lu",
                         static_cast<unsigned long>(lost), static_cast<unsigned long>(event_losses.load()),
                         static_cast<unsigned long>(cut));
                reported_loss = lost; reported_cut = cut;
                reported_event_loss = lost_events; reported_event_cut = cut_events;
            }
            completed.fetch_add(1, std::memory_order_release);
        } else {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
    }
}
}
esp_err_t init() {
    if (events) return ESP_OK;
    events = xQueueCreate(kEventQueueSize, sizeof(Event));
    traces = xQueueCreate(kTraceQueueSize, sizeof(Event));
    if (!events || !traces) {
        if (events) vQueueDelete(events);
        if (traces) vQueueDelete(traces);
        events = traces = nullptr;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(worker, "app_diagnostics", 3072, nullptr, 1, &worker_handle) != pdPASS) {
        vQueueDelete(events);
        vQueueDelete(traces);
        events = traces = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
void write(esp_log_level_t level, const char* tag, const char* format, ...) {
    if (!events || !tag || !format) { ++losses; return; }
    const bool product = strcmp(tag, "ProductEvent") == 0;
    const bool critical = product || level <= ESP_LOG_WARN;
    Event event{};
    event.level = level;
    event.tag = tag;
    event.occurred_ms = esp_timer_get_time() / 1000;
    event.sequence = product ? sequence.fetch_add(1) + 1 : 0;
    va_list args;
    va_start(args, format);
    const size_t limit = product ? 128 : sizeof(event.text);
    const int length = vsnprintf(event.text, limit, format, args);
    va_end(args);
    if (length >= static_cast<int>(limit)) {
        memcpy(event.text + limit - 6, "[cut]", 6);
        ++cuts;
        if (critical) ++event_cuts;
    }
    // 先登记再发布，消费者即使立即出队也不会出现完成数领先于接受数的窗口。
    accepted.fetch_add(1, std::memory_order_release);
    if (xQueueSend(critical ? events : traces, &event, 0) != pdTRUE) {
        accepted.fetch_sub(1, std::memory_order_release);
        ++losses; if (critical) ++event_losses;
    } else {
        xTaskNotifyGive(worker_handle);
    }
}
bool flush(TickType_t timeout) {
    const TickType_t start = xTaskGetTickCount();
    // 两队列可能重排输出，不能用“完成数达到调用前的目标”替代真正排空。
    // 也等待期间新接受的记录；持续有新提交时仍受同一个超时限制。
    while (completed.load(std::memory_order_acquire) != accepted.load(std::memory_order_acquire)) {
        if (xTaskGetTickCount() - start >= timeout) return false;
        vTaskDelay(1);
    }
    return true;
}
uint32_t dropped() { return losses.load(); }
}
