/**
 * @file status_page.h
 * @brief 状态屏页面：按远端状态、故障保持、配对、连接失败与休眠倒计时合成基础画面。
 */
#pragma once
#include "core/page.h"
namespace EmergencyUi {
/** 状态/主页/失败/倒计时合成屏；始终作为兜底页面激活。 */
class StatusPage final : public Page {
  public:
    ScreenId id() const override { return ScreenId::Status; }
    int priority() const override { return 0; }
    bool active(const EmergencyRemote::Snapshot &remote, const UiState &state) const override {
        (void)remote;
        (void)state;
        return true;
    }
    int page_key(const Model &model, const UiState &state) const override;
    Update handle_button(const EmergencyRemote::Snapshot &remote, ButtonInput::Event event, int64_t now_us,
                         UiState &state) override;
    void render(uint8_t *frame, const Model &model, const UiState &state) override;
};
} // namespace EmergencyUi
