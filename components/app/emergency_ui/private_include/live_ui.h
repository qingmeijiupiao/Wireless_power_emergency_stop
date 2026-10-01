#pragma once
#include "live_pages.h"
#include "emergency_remote.h"
#include <cstdio>
#include <cstring>

inline void pixel(uint8_t* frame, int x, int y) {
    if (x >= 0 && x < 128 && y >= 0 && y < 64) frame[(y / 8) * 128 + x] |= 1U << (y % 8);
}
inline void glyph_text(uint8_t* frame, int x, int y, const char* text, bool large) {
    const int width = large ? 12 : 6, height = large ? 24 : 12;
    for (; *text; ++text, x += width) {
        const unsigned c = static_cast<unsigned char>(*text);
        if (c < 32 || c > 126) continue;
        const uint8_t* glyph = large ? kLargeGlyphs[c - 32] : kSmallGlyphs[c - 32];
        for (int row = 0; row < height; ++row)
            for (int col = 0; col < width; ++col)
                if (glyph[(row / 8) * width + col] & (1U << (row % 8))) pixel(frame, x + col, y + row);
    }
}
inline void render_meter(uint8_t* frame, const EmergencyRemote::Snapshot& state) {
    memcpy(frame, kLiveMeter, 1024);
    char voltage[16], current[16], power[24];
    if (state.online) {
        snprintf(voltage, sizeof(voltage), "%.2f", state.data.voltage_mv / 1000.0);
        snprintf(current, sizeof(current), "%.3f", state.data.current_ua / 1000000.0);
        snprintf(power, sizeof(power), "P %.2f W", state.data.voltage_mv * (state.data.current_ua / 1e9));
        for (int i = 0; i < 3; ++i)
            for (int x = 0; x < 2; ++x)
                for (int y = 9 - i * 3; y <= 10; ++y) pixel(frame, 78 + i * 4 + x, y);
        for (int y = 0; y < 9; ++y) for (int x = 0; x < 24; ++x) {
            const int center = x < 4 ? 4 : (x > 19 ? 19 : x);
            const int dx = x - center, dy = y - 4;
            const bool inside = dx*dx + dy*dy <= 16;
            const bool outline = inside && (dx*dx + dy*dy >= 9 || (x >= 4 && x <= 19 && (y == 0 || y == 8)));
            const int dot = state.output_on ? 19 : 4;
            if (outline || (x-dot)*(x-dot)+(y-4)*(y-4) <= 4) pixel(frame, 102+x, 1+y);
        }
    } else {
        memset(frame, 0, 128);
        for (int x = 0; x < 128; ++x) frame[128+x] &= 0xf0;
        glyph_text(frame, 0, 1, "OFFLINE", false);
        strcpy(voltage, "--.--"); strcpy(current, "-.---"); strcpy(power, "P --.-- W");
    }
    glyph_text(frame, 96 - static_cast<int>(strlen(voltage)) * 12, 15, voltage, true);
    glyph_text(frame, 96 - static_cast<int>(strlen(current)) * 12, 35, current, true);
    glyph_text(frame, 2, 55, power, false);
}
