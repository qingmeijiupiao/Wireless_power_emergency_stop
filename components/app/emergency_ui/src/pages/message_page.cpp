/**
 * @file message_page.cpp
 * @brief 消息页面实现：渲染一次性消息，任意按键返回主页。
 */
#include "pages/message_page.h"
#include "product_ui.h"
namespace EmergencyUi {
bool MessagePage::active(const EmergencyRemote::Snapshot &remote, const UiState &state) const {
    return !UiPolicy::urgent(remote, state.fault_acknowledged) && state.menu.view == UiPolicy::View::Message;
}
int MessagePage::page_key(const Model &model, const UiState &state) const {
    (void)model;
    (void)state;
    return 300 + static_cast<int>(UiPolicy::View::Message);
}
Update MessagePage::handle_button(const EmergencyRemote::Snapshot &remote, Gesture event, int64_t now_us,
                                  UiState &state) {
    (void)remote;
    (void)now_us;
    // 消息页任意按键返回主页。
    if (event != Gesture::None)
        state.menu.home();
    return {};
}
void MessagePage::render(uint8_t *frame, const Model &model, const UiState &state) {
    ProductUi::render_message(frame, state.message, model.always_on);
}
} // namespace EmergencyUi
