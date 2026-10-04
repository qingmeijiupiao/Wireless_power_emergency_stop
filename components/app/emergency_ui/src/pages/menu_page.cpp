/**
 * @file menu_page.cpp
 * @brief 菜单页面实现：列表/确认/信息/休眠时长四个视图的按键状态机与渲染。
 */
#include "pages/menu_page.h"

#include "blackbox.h"
#include "blackbox_service.h"
#include "esp_app_desc.h"
#include "product_ui.h"
#include "runtime_settings.h"

namespace EmergencyUi {

bool MenuPage::active(const EmergencyRemote::Snapshot &remote, const UiState &state) const {
    if (UiPolicy::urgent(remote, state.fault_acknowledged))
        return false;
    switch (state.menu.view) {
    case UiPolicy::View::Menu:
    case UiPolicy::View::Confirm:
    case UiPolicy::View::Info:
    case UiPolicy::View::SleepTime:
        return true;
    default:
        return false;
    }
}

int MenuPage::page_key(const Model &model, const UiState &state) const {
    (void)model;
    return 300 + static_cast<int>(state.menu.view);
}

Update MenuPage::handle_button(const EmergencyRemote::Snapshot &remote, Gesture event, int64_t now_us,
                               UiState &state) {
    (void)remote;
    UiPolicy::Menu &menu = state.menu;
    // 非主页停留超时则自动回主页，防止误触后长期停留在菜单。
    if (menu.view != UiPolicy::View::Home && now_us - menu.touched > RuntimeSettings::get(RuntimeSettings::Id::MenuIdleMs) * 1000LL) {
        menu.home();
        return {};
    }
    if (event == Gesture::None)
        return {};
    menu.touched = now_us;
    switch (menu.view) {
    case UiPolicy::View::Menu:
        return handle_list(event, state);
    case UiPolicy::View::Confirm:
        return handle_confirm(event, state);
    case UiPolicy::View::Info:
        return handle_info(event, state);
    case UiPolicy::View::SleepTime:
        return handle_sleep_time(event, state);
    default:
        menu.home();
        return {};
    }
}

Update MenuPage::handle_list(Gesture event, UiState &state) {
    UiPolicy::Menu &menu = state.menu;
    // 短按循环 7 个条目；长按按条目分派。
    if (event == Gesture::Short) {
        menu.selected = (menu.selected + 1) % 7;
        return {};
    }
    switch (menu.selected) {
    case 0:
        // 第 0 项为返回主页。
        menu.home();
        break;
    case 3:
        // 第 3 项进入休眠时长页，并把选择定位到当前配置值。
        menu.view = UiPolicy::View::SleepTime;
        menu.sleep_choice = UiPolicy::sleep_time_index(RuntimeSettings::get(RuntimeSettings::Id::IdleMs));
        break;
    case 6:
        // 第 6 项进入设备信息页。
        menu.view = UiPolicy::View::Info;
        menu.info_page = 0;
        break;
    default:
        // 其余条目进入确认页。
        menu.view = UiPolicy::View::Confirm;
        menu.confirm = false;
        menu.stage = 0;
        break;
    }
    return {};
}

Update MenuPage::handle_confirm(Gesture event, UiState &state) {
    UiPolicy::Menu &menu = state.menu;
    if (event == Gesture::Short) {
        menu.confirm = !menu.confirm;
        return {};
    }
    if (!menu.confirm) {
        // 未勾选时长按：取消并返回菜单列表。
        menu.view = UiPolicy::View::Menu;
        return {};
    }
    if (menu.selected == 5 && menu.stage == 0) {
        // 重新配对需要二次确认：第一次长按进入第二阶段。
        menu.stage = 1;
        menu.confirm = false;
        return {};
    }
    // 已勾选且满足确认条件：上报与条目号对应的动作并返回主页。
    Update result;
    switch (menu.selected) {
    case 1: result.action = Action::AlwaysOn; break;
    case 2: result.action = Action::Sleep; break;
    case 4: result.action = Action::Pair; break;
    case 5: result.action = Action::Repair; break;
    default: break;
    }
    menu.home();
    return result;
}

Update MenuPage::handle_info(Gesture event, UiState &state) {
    UiPolicy::Menu &menu = state.menu;
    // 信息页：短按循环 4 个子页，长按返回主页。
    if (event == Gesture::Short)
        menu.info_page = (menu.info_page + 1) % 4;
    else
        menu.home();
    return {};
}

Update MenuPage::handle_sleep_time(Gesture event, UiState &state) {
    UiPolicy::Menu &menu = state.menu;
    // 休眠时长页：短按循环选择项，长按确认并上报动作。
    if (event == Gesture::Short) {
        menu.sleep_choice = (menu.sleep_choice + 1) % 4;
        return {};
    }
    Update result;
    result.action = Action::SleepTime;
    result.sleep_choice = menu.sleep_choice;
    menu.home();
    return result;
}

void MenuPage::render(uint8_t *frame, const Model &model, const UiState &state) {
    if (state.menu.view != UiPolicy::View::Info) {
        // 菜单列表、确认页、休眠时长页共用菜单样式。
        ProductUi::render_menu(frame, state.menu, model.always_on);
        return;
    }
    // 信息页数据按需读取：黑匣子未启用时不读取统计，相关字段保持默认值。
    BlackboxService::Statistics stats{};
    const bool blackbox_ready = Blackbox::is_enabled();
    if (blackbox_ready)
        BlackboxService::get_statistics(&stats);
    ProductUi::DeviceInfo info;
    info.page = state.menu.info_page;
    info.battery_mv = model.battery_mv;
    info.usb = model.usb;
    info.version = esp_app_get_description()->version;
    info.built = BUILD_TIME;
    info.blackbox_ready = blackbox_ready;
    if (blackbox_ready) {
        info.records = Blackbox::count();
        info.capacity = Blackbox::capacity();
    }
    info.pending = stats.pending_logs;
    info.captured = stats.captured_logs;
    info.dropped = stats.dropped_logs;
    info.failures = stats.persist_failures;
    ProductUi::render_info(frame, info);
}
} // namespace EmergencyUi
