/**
 * @file status_page.cpp
 * @brief 状态屏实现：计算基础页面编号，处理主页按键，并渲染状态/主页/失败/倒计时画面。
 */
#include "pages/status_page.h"
#include "product_ui.h"
namespace EmergencyUi {
namespace {
constexpr int kHomeOnline = 100;   // 在线主页编号
constexpr int kHomeOffline = 101;  // 离线主页编号
constexpr int kPairingPage = 19;   // 配对过程页面编号
constexpr int kFailurePage = 400;  // 连接失败页面编号
constexpr int kCountdownPage = 500; // 休眠倒计时页面编号
constexpr int64_t kStableHomeDelayUs = 1500000; // 状态稳定后回到主页的延迟
} // namespace

int StatusPage::page_key(const Model &model, const UiState &state) const {
    using EmergencyRemote::State;
    const auto &remote = model.remote;
    // 基础值取状态枚举；以下按优先级逐步覆盖。
    int page = static_cast<int>(remote.state);
    // 故障已确认且当前确为故障态：回到主页，让用户看到实时数据。
    if (state.fault_acknowledged && ProductUi::fault_page(remote) >= 0)
        page = kHomeOnline;
    // 状态稳定后回到主页：OFF 已停机，或 ON 持续一定时间；插电/常亮时 OFF 需要更久。
    const bool off_stable =
        remote.state == State::OFF &&
        (!remote.stop_closed ||
         ((model.usb || model.always_on) && model.now_us - state.state_since > kStableHomeDelayUs));
    const bool on_stable = remote.state == State::ON && model.now_us - state.state_since > kStableHomeDelayUs;
    if (off_stable || on_stable)
        page = kHomeOnline;
    // 多个保护同时触发时按 OCP、OTP、OVP、UVP 的顺序择一显示，日志仍保留完整掩码。
    if (!state.fault_acknowledged && remote.state == State::PROTECTED && remote.protection_mask) {
        page = (remote.protection_mask & 8) ? 18
               : (remote.protection_mask & 1) ? 15
               : (remote.protection_mask & 2) ? 16
                                              : 17;
    }
    // 故障已消失但提示仍在保持期内，继续显示该故障页。
    if (!state.fault_acknowledged && state.notice.holding(model.now_us) && ProductUi::fault_page(remote) < 0 &&
        !state.controlling && !remote.connection_failed)
        page = state.notice.page;
    // 配对过程优先占用屏幕，除非正处于停止/启动关键过程。
    if (remote.pairing && !state.controlling)
        page = kPairingPage;
    if (page == kHomeOnline)
        page = remote.online ? kHomeOnline : kHomeOffline;
    if (remote.connection_failed && state.menu.view == UiPolicy::View::Home)
        page = kFailurePage;
    if (state.menu.view == UiPolicy::View::Home && model.sleep.countdown)
        page = kCountdownPage;
    return page;
}

Update StatusPage::handle_button(const EmergencyRemote::Snapshot &remote, Gesture event, int64_t now_us,
                                 UiState &state) {
    Update result;
    if (state.menu.view != UiPolicy::View::Home)
        return result;
    // 连接失败时主页短按改为请求重试，不再进入菜单。
    if (remote.connection_failed && event == Gesture::Short) {
        result.retry = true;
        return result;
    }
    // 主页短按/长按进入菜单，从第 0 项开始。
    if (event == Gesture::Short || event == Gesture::Long) {
        state.menu.view = UiPolicy::View::Menu;
        state.menu.selected = 0;
        state.menu.touched = now_us;
        result.activity = true;
    }
    return result;
}

void StatusPage::render(uint8_t *frame, const Model &model, const UiState &state) {
    const int page = page_key(model, state);
    if (page == kCountdownPage) {
        ProductUi::render_sleep_countdown(frame, model.sleep.seconds, model.remote.connection_failed);
    } else if (page == kFailurePage) {
        ProductUi::render_failure(frame, model.remote.connection_failed && model.remote.output_on && model.remote.busy);
    } else if (page >= kHomeOnline) {
        ProductUi::render_home(frame, model.remote);
    } else {
        // 状态/故障页；若该页正是当前保持中的历史提示页，则加画历史页脚。
        ProductUi::render_state(frame, page, state.notice.history && page == state.notice.page);
    }
}
} // namespace EmergencyUi
