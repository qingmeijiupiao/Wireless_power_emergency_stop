#pragma once
#include "product_pages.h"
#include "emergency_remote.h"
#include "runtime_policy.h"
#include "meter_format.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ProductUi {
inline void pixel(uint8_t* frame, int x, int y) {
    if (x >= 0 && x < 128 && y >= 0 && y < 64) frame[(y / 8) * 128 + x] |= 1U << (y % 8);
}
inline void glyph(uint8_t* frame, int x, int y, const uint8_t* data, int stride, int width, int height) {
    for (int row = 0; row < height; ++row)
        for (int col = 0; col < width; ++col)
            if (data[(row / 8) * stride + col] & (1U << (row % 8))) pixel(frame, x + col, y + row);
}
inline void blend_left(uint8_t* frame, const uint8_t* source) {
    for (int page = 0; page < 8; ++page)
        for (int x = 0; x < 71; ++x) frame[page * 128 + x] |= source[page * 128 + x];
}
inline void clear_left(uint8_t* frame, int top = 0) {
    for (int y = top; y < 64; ++y)
        for (int x = 0; x < 71; ++x) frame[(y / 8) * 128 + x] &= ~(1U << (y % 8));
}
inline int soc_index(char c) { return c >= '0' && c <= '9' ? c - '0' : c == '%' ? 10 : c == '?' ? 11 : 12; }
inline void soc_text(uint8_t* frame, int y, const char* text) {
    int width = 0;
    for (const char* c = text; *c; ++c) width += kSocWidths[soc_index(*c)];
    int x = 81 + (39 - width) / 2;
    for (const char* c = text; *c; ++c) {
        const int i = soc_index(*c);
        glyph(frame, x, y, kSocGlyphs[i], 12, kSocWidths[i], 16); x += kSocWidths[i];
    }
}
inline void number(uint8_t* frame, int x, int y, const char* text) {
    for (; *text; ++text) {
        const int i = *text >= '0' && *text <= '9' ? *text - '0' : *text == '.' ? 10 : 11;
        const int width = i == 10 ? 4 : 9;
        glyph(frame, x, y, kProductDigits[i], 9, width, 16); x += width;
    }
}
inline int fault_page(const EmergencyRemote::Snapshot& s) {
    using EmergencyRemote::State;
    if (s.state == State::PROTECTED && s.protection_mask)
        return (s.protection_mask & 8) ? 18 : (s.protection_mask & 1) ? 15 : (s.protection_mask & 2) ? 16 : 17;
    const int p = static_cast<int>(s.state);
    return p == 6 || p == 7 || (p >= 10 && p <= 18) ? p : -1;
}
inline void render_rail(uint8_t* frame, const EmergencyRemote::Snapshot& s, int percent,
                        bool external_power, bool output_fresh, bool warning = false, bool pending_stop = false) {
    using EmergencyRemote::State;
    int rail = 2;
    if (pending_stop || s.state == State::STOPPING || s.state == State::STARTING) rail = 2;
    else if (s.connection_failed || (!s.online && !output_fresh)) rail = 3;
    else if (output_fresh) rail = s.output_on ? 1 : 0;
    for (int page = 0; page < 8; ++page)
        std::memcpy(frame + page * 128 + 71, kProductRails[rail] + page * 128 + 71, 57);
    // The board only detects external power: this is a charging hint, not a charger-status measurement.
    if (external_power && percent >= 0 && percent < 100) {
        for (int page = 0; page < 8; ++page)
            for (int x = 72; x < 128; ++x) frame[page * 128 + x] |= kProductCharge[0][page * 128 + x];
    } else {
        char text[8];
        if (percent >= 0) std::snprintf(text, sizeof(text), "%d%%", percent > 100 ? 100 : percent);
        else std::strcpy(text, "?");
        soc_text(frame, 49, text);
    }
    if (warning) {
        for (int y = 38; y <= 42; ++y) { pixel(frame, 77 - (y - 38), y); pixel(frame, 77 + (y - 38), y); }
        for (int x = 73; x <= 81; ++x) pixel(frame, x, 42);
    }
}
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
inline void render_state(uint8_t* frame, int page, bool history = false) {
    if (page < 0 || page >= 20) page = 8;
    std::memcpy(frame, kProductStates[page], 1024);
    if (history) { clear_left(frame, 52); blend_left(frame, kHistoryFooter[0]); }
}
inline void render_message(uint8_t* frame, int message, bool always_on) {
    if (message == 6 && !always_on) message = 10;
    if (message < 0 || message >= 12) message = 4;
    std::memcpy(frame, kProductMessages[message], 1024);
}
inline void render_menu(uint8_t* frame, const RuntimePolicy::Menu& menu, bool always_on) {
    if (menu.view == RuntimePolicy::View::Confirm) {
        const int item = menu.stage ? 7 : menu.selected;
        const int index = (always_on ? 16 : 0) + item * 2 + (menu.confirm ? 1 : 0);
        std::memcpy(frame, kProductConfirms[index], 1024);
    } else std::memcpy(frame, kProductMenus[(always_on ? 7 : 0) + menu.selected], 1024);
}
inline void render_info(uint8_t* frame, int battery_mv, bool usb) {
    std::memset(frame, 0, 1024);
    blend_left(frame, kInfoTitle[0]); blend_left(frame, kInfoFooter[0]);
    blend_left(frame, usb ? kInfoUsb[0] : kInfoBattery[0]);
    char text[16];
    if (battery_mv > 0) format_meter_value(battery_mv / 1000.0, text);
    else std::strcpy(text, "--.--");
    number(frame, 3, 21, text);
    // Reuse the approved voltage unit at its fixed x position.
    for (int y = 0; y < 14; ++y) for (int x = 54; x <= 68; ++x)
        if (kProductHome[0][((y + 4) / 8) * 128 + x] & (1U << ((y + 4) % 8))) pixel(frame, x, y + 20);
}
inline void render_failure(uint8_t* frame, unsigned seconds, bool may_sleep, bool closing) {
    if (closing) {
        std::memcpy(frame, kProductRetry[0], 1024);
        // The transaction remains latched; never promise an automatic sleep in this state.
        return;
    }
    std::memset(frame, 0, 1024);
    blend_left(frame, kFailureHint[0]); blend_left(frame, may_sleep ? kSleepCountdown[0] : kBlankCountdown[0]);
    char text[8]; std::snprintf(text, sizeof(text), "%u", seconds > 3600 ? 3600 : seconds);
    int x = (71 - static_cast<int>(std::strlen(text)) * 13) / 2;
    for (const char* c = text; *c; ++c, x += 13) glyph(frame, x, 8, kCountdownDigits[*c - '0'], 13, 13, 24);
}
}
