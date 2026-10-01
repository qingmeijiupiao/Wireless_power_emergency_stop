/**
 * @file menu_page.h
 * @brief 菜单页面：菜单列表、条目确认、设备信息与休眠时长选择的状态机与渲染。
 */
#pragma once
#include "core/page.h"
namespace EmergencyUi {
/** 菜单类页面；在非紧急状态下覆盖状态屏。 */
class MenuPage final : public Page {
  public:
    ScreenId id() const override { return ScreenId::Menu; }
    int priority() const override { return 20; }
    bool active(const EmergencyRemote::Snapshot &remote, const UiState &state) const override;
    int page_key(const Model &model, const UiState &state) const override;
    Update handle_button(const EmergencyRemote::Snapshot &remote, ButtonInput::Event event, int64_t now_us,
                         UiState &state) override;
    void render(uint8_t *frame, const Model &model, const UiState &state) override;

  private:
    /** @brief 菜单列表：短按循环选择，长按按条目跳转或进入确认。 */
    Update handle_list(ButtonInput::Event event, UiState &state);
    /** @brief 确认页：短按切换勾选，长按确认或取消；重新配对需要二次确认。 */
    Update handle_confirm(ButtonInput::Event event, UiState &state);
    /** @brief 设备信息页：短按循环 4 个子页，长按返回主页。 */
    Update handle_info(ButtonInput::Event event, UiState &state);
    /** @brief 休眠时长页：短按循环选择，长按保存并上报动作。 */
    Update handle_sleep_time(ButtonInput::Event event, UiState &state);
};
} // namespace EmergencyUi
