/**
 * @file product_ui.h
 * @brief 提供 SH1106 帧缓冲的基础绘制原语，并渲染主页、状态/故障、菜单、信息等各页面的实际像素内容。
 */
#pragma once
#include "product_pages.h"
#include "emergency_remote.h"
#include "ui_policy.h"
#include "meter_format.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ProductUi {
// 点亮单像素。坐标越界时静默忽略，调用方无需自行边界检查。帧缓冲按页（每页 8 行）组织。
inline void pixel(uint8_t* frame, int x, int y) {
    if (x >= 0 && x < 128 && y >= 0 && y < 64) frame[(y / 8) * 128 + x] |= 1U << (y % 8);
}
// 按位图数据绘制一个字形/图标；data 为按页存储的位图，stride 为每页字节跨度。
inline void glyph(uint8_t* frame, int x, int y, const uint8_t* data, int stride, int width, int height) {
    for (int row = 0; row < height; ++row)
        for (int col = 0; col < width; ++col)
            if (data[(row / 8) * stride + col] & (1U << (row % 8))) pixel(frame, x + col, y + row);
}
// 将位图左半区（0..70 列）按位或叠加到帧缓冲，用于在状态页上补画页脚等装饰。
inline void blend_left(uint8_t* frame, const uint8_t* source) {
    for (int page = 0; page < 8; ++page)
        for (int x = 0; x < 71; ++x) frame[page * 128 + x] |= source[page * 128 + x];
}
// 清除帧缓冲左半区（0..70 列）从 top 行起的像素，便于在已有页面上覆盖重画。
inline void clear_left(uint8_t* frame, int top = 0) {
    for (int y = top; y < 64; ++y)
        for (int x = 0; x < 71; ++x) frame[(y / 8) * 128 + x] &= ~(1U << (y % 8));
}
// 将电量文本字符映射到字形表下标：数字 0-9、'%'、'?'，其余归入无效字符。
inline int soc_index(char c) { return c >= '0' && c <= '9' ? c - '0' : c == '%' ? 10 : c == '?' ? 11 : 12; }
// 在右侧电量区居中绘制电量文本（如 "88%" 或 "?"），逐字符按各自宽度累加。
inline void soc_text(uint8_t* frame, int y, const char* text) {
    int width = 0;
    for (const char* c = text; *c; ++c) width += kSocWidths[soc_index(*c)];
    int x = 81 + (39 - width) / 2;
    for (const char* c = text; *c; ++c) {
        const int i = soc_index(*c);
        glyph(frame, x, y, kSocGlyphs[i], 12, kSocWidths[i], 16); x += kSocWidths[i];
    }
}
// 绘制大号数值文本：数字占 9 像素宽、小数点为 4 像素宽，用于主页三项测量值。
inline void number(uint8_t* frame, int x, int y, const char* text) {
    for (; *text; ++text) {
        const int i = *text >= '0' && *text <= '9' ? *text - '0' : *text == '.' ? 10 : 11;
        const int width = i == 10 ? 4 : 9;
        glyph(frame, x, y, kProductDigits[i], 9, width, 16); x += width;
    }
}
// 将远端状态映射为故障页面编号：保护态按 OCP/OTP/OVP/UVP 择一（18/15/16/17）；
// 其余状态中仅短路(6)、检测异常(7)与保护类(10..18)算故障，否则返回 -1 表示无故障页。
inline int fault_page(const EmergencyRemote::Snapshot& s) {
    using EmergencyRemote::State;
    if (s.state == State::PROTECTED && s.protection_mask)
        return (s.protection_mask & 8) ? 18 : (s.protection_mask & 1) ? 15 : (s.protection_mask & 2) ? 16 : 17;
    const int p = static_cast<int>(s.state);
    return p == 6 || p == 7 || (p >= 10 && p <= 18) ? p : -1;
}
// 绘制右侧 57 像素宽状态栏与电量区。rail 语义：0 输出关、1 输出开、2 过渡/待定、3 未知或断连。
inline void render_rail(uint8_t* frame, const EmergencyRemote::Snapshot& s, int percent,
                        bool external_power, bool output_fresh, bool warning = false, bool pending_stop = false) {
    using EmergencyRemote::State;
    int rail = 2;
    if (pending_stop || s.state == State::STOPPING || s.state == State::STARTING) rail = 2;
    else if (s.connection_failed || (!s.online && !output_fresh)) rail = 3;
    else if (output_fresh) rail = s.output_on ? 1 : 0;
    for (int page = 0; page < 8; ++page)
        std::memcpy(frame + page * 128 + 71, kProductRails[rail] + page * 128 + 71, 57);
    // 硬件仅能检测到外部供电，因此这里只是充电提示，并非充电器状态测量；未满电且插电时才叠加充电图标。
    if (external_power && percent >= 0 && percent < 100) {
        for (int page = 0; page < 8; ++page)
            for (int x = 72; x < 128; ++x) frame[page * 128 + x] |= kProductCharge[0][page * 128 + x];
    }
    char text[8];
    if (percent >= 0) std::snprintf(text, sizeof(text), "%d%%", percent > 100 ? 100 : percent);
    else std::strcpy(text, "?");
    soc_text(frame, 49, text);
    // warning 为真时在电量下方画一个三角形警告标记（存在故障或低电）。
    if (warning) {
        for (int y = 38; y <= 42; ++y) { pixel(frame, 77 - (y - 38), y); pixel(frame, 77 + (y - 38), y); }
        for (int x = 73; x <= 81; ++x) pixel(frame, x, 42);
    }
}
// 绘制主页：以背景为底，依次显示电压、电流、功率三项；离线或断连时统一显示占位符。
inline void render_home(uint8_t* frame, const EmergencyRemote::Snapshot& s) {
    std::memcpy(frame, kProductHome[0], 1024);
    const double values[] = {s.data.voltage_mv / 1000.0, s.data.current_ua / 1000000.0,
                            s.data.voltage_mv * (s.data.current_ua / 1e9)};
    for (int row = 0; row < 3; ++row) {
        char text[16];
        if (!s.online || s.connection_failed) std::strcpy(text, "--.--");
        else format_meter_value(values[row], text);
        number(frame, 3, 5 + row * 21, text);
    }
}
// 绘制状态/故障页。索引越界时回退到默认状态页（8）；history 为真时在页脚补画历史提示条。
inline void render_state(uint8_t* frame, int page, bool history = false) {
    if (page < 0 || page >= 20) page = 8;
    std::memcpy(frame, kProductStates[page], 1024);
    if (history) { clear_left(frame, 52); blend_left(frame, kHistoryFooter[0]); }
}
// 绘制消息页；非常亮模式下消息 6 无效，退化为消息 10；越界消息退化为消息 4。
inline void render_message(uint8_t* frame, int message, bool always_on) {
    if (message == 6 && !always_on) message = 10;
    if (message < 0 || message >= 13) message = 4;
    std::memcpy(frame, kProductMessages[message], 1024);
}
// 绘制菜单类页面：休眠时长页按选择项取图；确认页按条目/阶段及常亮模式组合索引；其余为菜单列表。
inline void render_menu(uint8_t* frame, const UiPolicy::Menu& menu, bool always_on) {
    if (menu.view == UiPolicy::View::SleepTime) {
        std::memcpy(frame, kSleepTimeMenus[menu.sleep_choice], 1024);
    } else if (menu.view == UiPolicy::View::Confirm) {
        // stage 为二次确认阶段时固定展示第 7 项；否则用当前条目。确认光标与常亮模式共同决定图集下标。
        const int item = menu.stage ? 7 : menu.selected;
        const int index = (always_on ? 16 : 0) + item * 2 + (menu.confirm ? 1 : 0);
        std::memcpy(frame, kProductConfirms[index], 1024);
    } else std::memcpy(frame, kProductMenus[(always_on ? 7 : 0) + menu.selected], 1024);
}
// 设备信息页的输入数据。page 0=设备信息、1=固件版本、2=黑匣子统计、3=日志统计。
struct DeviceInfo {
    int page = 0, battery_mv = 0;
    bool usb = false, blackbox_ready = false;
    const char* version = "?";
    const char* built = "";
    uint32_t records = 0, capacity = 0, pending = 0, captured = 0, dropped = 0, failures = 0;
};
// 以 6x8 ASCII 字体绘制文本，最多 length 个字符，且不超过左半区宽度（x+6<=71）。
inline void info_ascii(uint8_t* frame, int x, int y, const char* text, int length = 11) {
    for (; *text && length-- > 0 && x + 6 <= 71; ++text, x += 6)
        if (*text >= 32 && *text <= 126) glyph(frame, x, y, kInfoAscii[*text - 32], 6, 6, 8);
}
// 绘制一行“标签+数值”统计：左侧叠加位图标签，右侧绘制数值；超过 999999 时以 M（百万）为单位显示。
inline void info_count(uint8_t* frame, int y, const uint8_t* label, uint32_t value) {
    blend_left(frame, label);
    char text[16];
    if (value > 999999) std::snprintf(text, sizeof(text), "%luM", static_cast<unsigned long>(value / 1000000));
    else std::snprintf(text, sizeof(text), "%lu", static_cast<unsigned long>(value));
    info_ascii(frame, 24, y + 1, text);
}
// 绘制设备信息页，按 info.page 分派到固件页、黑匣子页或日志页；黑匣子未启用时显示不可用提示。
inline void render_info(uint8_t* frame, const DeviceInfo& info) {
    std::memset(frame, 0, 1024);
    blend_left(frame, kInfoFooter[0]);
    if (info.page == 1) {
        // 固件页：版本号、编译日期时间（较长时拆成两行显示）。
        blend_left(frame, kFirmwareTitle[0]);
        info_ascii(frame, 3, 20, info.version);
        info_ascii(frame, 3, 32, info.built, 10);
        if (std::strlen(info.built) >= 19) info_ascii(frame, 3, 44, info.built + 11, 8);
        return;
    }
    if (info.page >= 2) {
        // 黑匣子页(page==2)显示记录数/容量/待写入；日志页(page==3)显示已采集/丢弃/失败计数。
        blend_left(frame, info.page == 2 ? kBlackboxTitle[0] : kLogTitle[0]);
        if (!info.blackbox_ready) { blend_left(frame, kBlackboxUnavailable[0]); return; }
        if (info.page == 2) {
            info_count(frame, 18, kRecordLabel[0], info.records);
            info_count(frame, 30, kCapacityLabel[0], info.capacity);
            info_count(frame, 42, kPendingLabel[0], info.pending);
        } else {
            info_count(frame, 18, kCapturedLabel[0], info.captured);
            info_count(frame, 30, kDroppedLabel[0], info.dropped);
            info_count(frame, 42, kFailedLabel[0], info.failures);
        }
        return;
    }
    // 设备信息页：标题、供电方式（USB/电池）、电池电压。
    blend_left(frame, kInfoTitle[0]);
    blend_left(frame, info.usb ? kInfoUsb[0] : kInfoBattery[0]);
    char text[16];
    if (info.battery_mv > 0) format_meter_value(info.battery_mv / 1000.0, text);
    else std::strcpy(text, "--.--");
    number(frame, 3, 21, text);
    // 复用固定的电压单位图形，从主页背景中裁剪并画到固定位置，保持单位样式统一。
    for (int y = 0; y < 14; ++y) for (int x = 54; x <= 68; ++x)
        if (kProductHome[0][((y + 4) / 8) * 128 + x] & (1U << ((y + 4) % 8))) pixel(frame, x, y + 20);
}
// 绘制连接失败页：closing 为真时显示“正在重试/可能仍在关断”，否则显示一般失败提示。
inline void render_failure(uint8_t* frame, bool closing) {
    std::memcpy(frame, closing ? kProductRetry[0] : kProductFailure[0], 1024);
}
// 绘制休眠倒计时页：秒数上限 60，数字用大号字形，'s' 用 ASCII 字体；失败原因用不同提示文字。
inline void render_sleep_countdown(uint8_t* frame, unsigned seconds, bool failed) {
    std::memset(frame, 0, 1024);
    blend_left(frame, kSleepSoonTitle[0]); blend_left(frame, failed ? kFailureHint[0] : kSleepCancel[0]);
    char text[8]; std::snprintf(text, sizeof(text), "%us", seconds > 60 ? 60 : seconds);
    int x = (71 - static_cast<int>(std::strlen(text)) * 13) / 2;
    for (const char* c = text; *c; ++c, x += 13) {
        if (*c == 's') info_ascii(frame, x, 33, "s", 1);
        else glyph(frame, x, 22, kCountdownDigits[*c - '0'], 13, 13, 24);
    }
}
}
