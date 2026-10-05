/**
 * @file ui_messages.h
 * @brief 产品动作反馈的消息页编号，与 ProductUi 的消息图像索引保持一致。
 */
#pragma once

namespace AppController::UiMessages {
// 与 ProductUi 消息页编号对应；业务代码使用名称，避免散落的数字编号。
inline constexpr int kPairBlocked = 4;    /**< 当前条件不允许进入配对。 */
inline constexpr int kReleaseButton = 5;  /**< 手动休眠前请释放按键。 */
inline constexpr int kAlwaysOnSaved = 6;  /**< 常亮模式设置已保存。 */
inline constexpr int kSaveFailed = 7;     /**< 设置保存失败或菜单参数无效。 */
inline constexpr int kPairStarted = 8;    /**< 已提交配对请求。 */
inline constexpr int kPairingDeleted = 9; /**< 配对记录已删除。 */
inline constexpr int kSleepTimeSaved = 12; /**< 自动休眠时长已保存。 */
} // namespace AppController::UiMessages
