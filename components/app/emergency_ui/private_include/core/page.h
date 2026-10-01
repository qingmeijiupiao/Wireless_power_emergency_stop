/**
 * @file page.h
 * @brief 屏幕页面抽象基类：定义激活判定、页面键、按键处理与渲染接口。
 *
 * 页面只负责自身内容绘制和页面内按键行为；页面选择、故障确认、共享状态
 * 和刷新节拍由 UiManager 统一管理。
 */
#pragma once
#include "core/ui_state.h"
#include "core/ui_types.h"
#include "emergency_ui.h"
namespace EmergencyUi {
/** 屏幕页面基类。 */
class Page {
  public:
    virtual ~Page() = default;
    /** @brief 页面标识。 */
    virtual ScreenId id() const = 0;
    /** @brief 页面优先级，数值越大越优先；UiManager 在多个激活页面中取最高者。 */
    virtual int priority() const { return 0; }
    /** @brief 判断本页面在当前远端状态下是否激活。 */
    virtual bool active(const EmergencyRemote::Snapshot &remote, const UiState &state) const = 0;
    /** @brief 返回用于刷新判定的页面键，取值与历史页面编号约定一致。 */
    virtual int page_key(const Model &model, const UiState &state) const = 0;
    /**
     * @brief 处理一次按键事件。
     * @param remote 远端快照
     * @param event 按键事件；Event::None 表示无按键的周期调用
     * @param now_us 当前时间，微秒
     * @param state 可读写的共享 UI 状态
     * @return 需要业务层执行的动作
     */
    virtual Update handle_button(const EmergencyRemote::Snapshot &remote, Gesture gesture, int64_t now_us,
                                 UiState &state) {
        (void)remote;
        (void)gesture;
        (void)now_us;
        (void)state;
        return {};
    }
    /** @brief 渲染页面内容到帧缓冲。 */
    virtual void render(uint8_t *frame, const Model &model, const UiState &state) = 0;
};
} // namespace EmergencyUi
