/**
 * @file button_input.h
 * @brief BOOT 按键输入适配：基于公共 Button 组件，把按键手势转换为 UI 可消费的事件队列。
 */
#pragma once
#include <cstdint>
namespace ButtonInput {
/** UI 关心的逻辑按键事件；只暴露手势，按下/释放边沿不进入 UI 状态机。 */
enum class Event : uint8_t {
    None = 0, /**< 无事件，用于无按键的周期调用占位。 */
    Short,    /**< 短按。 */
    Long,     /**< 长按。 */
};
/** @brief 初始化 GPIO3 上的按键驱动并准备事件队列。 */
void init();
/**
 * @brief 非阻塞读取一个按键事件。
 * @param event 输出事件；无事件时保持不变。
 * @return true 表示取到事件。
 */
bool poll(Event& event);
} // namespace ButtonInput
