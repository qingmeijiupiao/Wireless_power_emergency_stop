/** @file display_worker.h
 * @brief OLED 执行任务接口；UI 只发布像素副本，不向显示任务传递页面状态。
 */
#pragma once
#include "esp_err.h"
#include <cstdint>
namespace EmergencyUi::DisplayWorker {
/** @brief 创建固定邮箱与任务，重复调用安全。 */
esp_err_t init();
/** @brief 零等待发布最新 1024 字节帧；暂停后拒绝提交。 */
bool submit(const uint8_t* frame);
/** @brief 开启新显示代际并异步恢复；旧代际帧不会再写入。 */
void restore();
/** @brief 关提交门、等待任务关总线；超时返回 false，调用者不得隔离 GPIO。 */
bool shutdown();
}
