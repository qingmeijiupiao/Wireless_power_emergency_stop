/**
 * @file message_page.h
 * @brief 消息页面：显示一次性消息，任意按键返回主页。
 */
#pragma once
#include "core/page.h"
namespace EmergencyUi {
/** 一次性消息页面。 */
class MessagePage final : public Page {
  public:
    ScreenId id() const override { return ScreenId::Message; }
    int priority() const override { return 30; }
    bool active(const EmergencyRemote::Snapshot &remote, const UiState &state) const override;
    int page_key(const Model &model, const UiState &state) const override;
    Update handle_button(const EmergencyRemote::Snapshot &remote, Gesture gesture, int64_t now_us,
                         UiState &state) override;
    void render(uint8_t *frame, const Model &model, const UiState &state) override;
};
} // namespace EmergencyUi
