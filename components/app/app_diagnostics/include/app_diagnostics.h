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
 * @note 单条最多 191 字符，过长截断。输出/Flash 捕获只在诊断任务中执行。
 */
void write(esp_log_level_t level, const char* tag, const char* format, ...)
    __attribute__((format(printf, 3, 4)));
/** @brief 等待调用前已接受的记录输出；超时不会释放后台仍使用的资源。 */
bool flush(TickType_t timeout = pdMS_TO_TICKS(200));
/** @return 本次启动丢弃的记录数（包括未初始化时的调用）。 */
uint32_t dropped();
}
#define APP_LOGI(tag, ...) AppDiagnostics::write(ESP_LOG_INFO, tag, __VA_ARGS__)
#define APP_LOGW(tag, ...) AppDiagnostics::write(ESP_LOG_WARN, tag, __VA_ARGS__)
#define APP_LOGE(tag, ...) AppDiagnostics::write(ESP_LOG_ERROR, tag, __VA_ARGS__)
