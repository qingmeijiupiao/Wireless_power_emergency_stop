/** @file app_diagnostics.h
 * @brief 固定容量异步诊断出口，避免急停工作者等待 USB/stdout。
 */
#pragma once
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <cstdint>
namespace AppDiagnostics {
/** @brief 主入口启动时创建一次；失败返回错误，不能依赖同步日志兜底。 */
esp_err_t init();
/** @brief 格式化后零等待入队；满队列丢弃并计数，禁止 ISR 调用。
 * @param tag 必须为静态生命周期字符串。
 * @note ProductEvent 自动携带事件序号和入队时刻；正文最多 127 字符。
 *       普通日志最多 191 字符；过长内容以 [cut] 标记并计数。
 *       产品事件/W/E 与普通 INFO 使用独立队列，输出/Flash 捕获仅在诊断任务执行。
 */
void write(esp_log_level_t level, const char* tag, const char* format, ...)
    __attribute__((format(printf, 3, 4)));
/** @brief 等待两队列已接受的记录排空，包括等待期间新提交的记录；总等待受 timeout 限制。
 * @note 只确认日志出口已处理，Flash 持久化须再调用 BlackboxService::sync()。
 */
bool flush(TickType_t timeout = pdMS_TO_TICKS(200));
/** @return 本次启动丢弃的记录数（包括未初始化时的调用）。 */
uint32_t dropped();
/** @brief 单一任务拥有的错误变化记录器；重复失败为 INFO，首发/原因变化为 ERROR。
 * @note 成功恢复记录为 ProductEvent；禁止跨任务共享同一个实例。
 */
class ErrorLog {
public:
    void observe(const char* tag, const char* operation, esp_err_t result) {
        if (result != last_) {
            if (result == ESP_OK)
                write(ESP_LOG_INFO, "ProductEvent", "%s %s recovered previous=%s", tag, operation, esp_err_to_name(last_));
            else
                write(ESP_LOG_ERROR, "ProductEvent", "%s %s failed: %s", tag, operation, esp_err_to_name(result));
            last_ = result;
        } else if (result != ESP_OK) {
            write(ESP_LOG_INFO, tag, "%s still failed: %s", operation, esp_err_to_name(result));
        }
    }
private:
    esp_err_t last_ = ESP_OK; /**< 最近处理结果，仅用于日志去重。 */
};
}
#define APP_LOGI(tag, ...) AppDiagnostics::write(ESP_LOG_INFO, tag, __VA_ARGS__)
#define APP_LOGW(tag, ...) AppDiagnostics::write(ESP_LOG_WARN, tag, __VA_ARGS__)
#define APP_LOGE(tag, ...) AppDiagnostics::write(ESP_LOG_ERROR, tag, __VA_ARGS__)
