/**
 * @file ui_manager.h
 * @brief UI 管理器：持有共享状态与页面对象，负责选页、按键分发、状态观察与渲染编排。
 */
#pragma once
#include "core/page.h"
#include "pages/menu_page.h"
#include "pages/message_page.h"
#include "pages/status_page.h"
#include <cstdint>
#include <atomic>
namespace EmergencyUi {
/** UI 管理器单例。 */
class UiManager {
  public:
    /** @brief 获取单例。 */
    static UiManager &instance();
    /** @brief 复位共享状态，并从 RTC 恢复深睡前的故障历史。 */
    void reset(int64_t now_us);
    /** @brief 处理一次按键事件并推进 UI 状态机。 */
    Update handle_input(const EmergencyRemote::Snapshot &remote, Gesture gesture, int64_t now_us);
    /** @brief 观察远端与电池状态，触发状态跳变记录与低电提示。 */
    bool observe_state(const Model &model);
    /** @brief 返回当前激活页面的页面键。 */
    int page_key(const Model &model) const;
    /** @brief 渲染当前激活页面并叠加右侧状态栏。 */
    void render(uint8_t *frame, const Model &model);
    /** @brief 最近一次记录的故障页编号。 */
    int last_fault_page() const { return published_fault_.load(); }
    /** @brief 故障/低电提示的最晚保持截止时刻。 */
    int64_t notice_deadline() const;
    /** @brief 当前是否允许开始休眠倒计时。 */
    bool sleep_notice_allowed(int64_t now_us) const;
    /** @brief 设置并显示一次性消息页。 */
    void show_message(int message, int64_t now_us, bool touch);
    /** @brief 取出并清除待重绘标记。 */
    bool take_redraw();

  private:
    UiManager() = default;
    /** @brief 在已注册页面中选择优先级最高的激活页面。 */
    Page *resolve(const EmergencyRemote::Snapshot &remote) const;
    /** @brief 倒计时优先于普通菜单/消息，紧急控制与有效故障仍优先。 */
    Page *resolve(const Model &model) const;

    UiState state_;                                  /**< 共享 UI 状态 */
    std::atomic<int> published_fault_{-1};            /**< Shell 可读取的故障历史 */
    StatusPage status_;                              /**< 状态屏 */
    MenuPage menu_;                                  /**< 菜单屏 */
    MessagePage message_page_;                       /**< 消息屏 */
    Page *const pages_[3] = {&status_, &menu_, &message_page_}; /**< 页面注册表 */
    bool redraw_ = true;                             /**< 待重绘标记 */
};
} // namespace EmergencyUi
