/** @file display_worker.cpp
 * @brief 单一 OLED 所有者；最新帧覆盖、错误退避、总线重建及睡前确认。
 */
#include "core/display_worker.h"
#include "sh1106.h"
#include "esp_log.h"
#include "app_diagnostics.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <atomic>
#include <cstring>
namespace EmergencyUi::DisplayWorker {
namespace {
constexpr const char* kTag = "OledWorker";
struct Frame { uint32_t generation; uint8_t pixels[1024]; };
struct Command { bool enable; uint32_t generation; uint32_t serial; };
QueueHandle_t frames = nullptr, commands = nullptr;
std::atomic<uint32_t> generation{1}, serial{0}, completed{0};
std::atomic_bool accepting{false};
/** @brief 唯一接触 Sh1106 的任务；帧与控制消息均按值保存。 */
void worker(void*) {
    Sh1106 display;
    Frame latest{};
    Frame incoming; // 循环复用接收缓冲，避免每拍清零 1 KB，也减少任务栈峰值。
    uint32_t active_generation = 1;
    bool enabled = false, dirty = false;
    unsigned failures = 0;
    int64_t retry_at = 0, log_at = 0;
    bool had_error = false;
    AppDiagnostics::ErrorLog display_log; // 独占于 OLED 任务，不参与重试或硬件状态判定。
    for (;;) {
        Command command{};
        if (xQueueReceive(commands, &command, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (!command.enable) {
                // 睡眠提示是关机前最后一个最新帧；之后不再接触驱动直到 restore。
                if (xQueueReceive(frames, &incoming, 0) == pdTRUE && incoming.generation == active_generation)
                    latest = incoming;
                if (enabled && display.address() && latest.generation == active_generation)
                    (void)display.write_frame(latest.pixels, sizeof(latest.pixels));
                display.shutdown();
                enabled = dirty = false;
                active_generation = command.generation;
                completed.store(command.serial, std::memory_order_release);
                continue;
            }
            display.shutdown();
            active_generation = command.generation;
            enabled = true;
            dirty = false;
            failures = 0;
            retry_at = 0;
            completed.store(command.serial, std::memory_order_release);
        }
        if (xQueueReceive(frames, &incoming, 0) == pdTRUE && incoming.generation == active_generation) {
            latest = incoming;
            dirty = true;
        }
        const int64_t now = esp_timer_get_time();
        if (!enabled || now < retry_at) continue;
        esp_err_t error = ESP_OK;
        if (!display.address()) {
            error = display.init(CONFIG_ESTOP_OLED_SDA, CONFIG_ESTOP_OLED_SCL, CONFIG_ESTOP_OLED_COLUMN_OFFSET);
            if (error != ESP_OK) {
                display.shutdown();
                retry_at = now + 3000000;
            }
        }
        if (error == ESP_OK && dirty && display.address()) {
            error = display.write_frame(latest.pixels, sizeof(latest.pixels));
            if (error == ESP_OK) { dirty = false; failures = 0; }
            else {
                retry_at = now + 100000;
                if (++failures >= 3) {
                    display.shutdown();
                    failures = 0;
                    retry_at = now + 3000000;
                }
            }
        }
        if (error != ESP_OK) {
            had_error = true;
            if (now >= log_at) {
                display_log.observe(kTag, "OLED", error);
                log_at = now + 3000000;
            }
        } else if (had_error) {
            display_log.observe(kTag, "OLED", ESP_OK);
            had_error = false;
        }
    }
}
}
esp_err_t init() {
    if (frames) return ESP_OK;
    frames = xQueueCreate(1, sizeof(Frame));
    commands = xQueueCreate(2, sizeof(Command));
    if (frames && commands && xTaskCreate(worker, "oled_worker", 4096, nullptr, 1, nullptr) == pdPASS)
        return ESP_OK;
    if (frames) vQueueDelete(frames);
    if (commands) vQueueDelete(commands);
    frames = commands = nullptr;
    return ESP_ERR_NO_MEM;
}
bool submit(const uint8_t* pixels) {
    if (!frames || !accepting.load(std::memory_order_acquire)) return false;
    Frame frame;
    frame.generation = generation.load(std::memory_order_acquire);
    memcpy(frame.pixels, pixels, sizeof(frame.pixels));
    return xQueueOverwrite(frames, &frame) == pdTRUE;
}
void restore() {
    if (!commands) return;
    const Command command{true, generation.fetch_add(1) + 1, serial.fetch_add(1) + 1};
    if (xQueueSend(commands, &command, pdMS_TO_TICKS(500)) == pdTRUE)
        accepting.store(true, std::memory_order_release);
}
bool shutdown() {
    if (!commands) return true;
    accepting.store(false, std::memory_order_release);
    const Command command{false, generation.fetch_add(1) + 1, serial.fetch_add(1) + 1};
    if (xQueueSend(commands, &command, pdMS_TO_TICKS(500)) != pdTRUE) return false;
    const TickType_t start = xTaskGetTickCount();
    while (static_cast<int32_t>(completed.load(std::memory_order_acquire) - command.serial) < 0) {
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(500)) return false;
        vTaskDelay(1);
    }
    return true;
}
}
