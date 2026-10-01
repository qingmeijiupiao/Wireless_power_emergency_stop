/**
 * @file ui_types.h
 * @brief UI 核心轻量类型：屏幕标识与渲染模式。
 */
#pragma once
#include <cstdint>
namespace EmergencyUi {
/** 顶层屏幕标识，用于日志与调试；页面选择由各 Page::active 决定。 */
enum class ScreenId : uint8_t {
    Status = 0, /**< 状态/主页/失败/倒计时合成屏 */
    Menu,       /**< 菜单列表、确认、信息、休眠时长 */
    Message,    /**< 一次性消息屏 */
    Count,      /**< 数量边界 */
};
/** 渲染模式。 */
enum class RenderMode : uint8_t {
    Normal = 0, /**< 常规刷新 */
    Full,       /**< 页面切换或交互后的完整刷新 */
};
} // namespace EmergencyUi
