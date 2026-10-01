/**
 * @file meter_format.h
 * @brief 将测量值格式化为定宽数字文本，供 OLED 上的电压/电流/功率显示使用。
 */
#pragma once
#include <cmath>
#include <cstdio>
#include <cstring>

// 最多显示四位有效数字；符号与小数点不计入这四位。
inline void format_meter_value(double value, char (&text)[16]) {
    if (!std::isfinite(value) || std::abs(value) >= 9999.5) {
        // 非有限值或超出可显示范围时输出占位符，表示数据无效。
        std::strcpy(text, "----"); return;
    }
    double magnitude = std::abs(value);
    // 按量级选择小数位数，使有效位数基本恒定，避免大数值把显示宽度占满。
    int decimals = magnitude >= 1000 ? 0 : magnitude >= 100 ? 1 : magnitude >= 10 ? 2 : 3;
    for (;;) {
        const double scale = std::pow(10.0, decimals);
        const double rounded = std::round(magnitude * scale) / scale;
        // 四舍五入后若进位到更高量级（如 9.99 -> 10.0），则减少小数位重新格式化，防止有效位数溢出。
        if ((decimals == 3 && rounded >= 10) || (decimals == 2 && rounded >= 100) ||
            (decimals == 1 && rounded >= 1000)) { --decimals; continue; }
        if (rounded == 0) value = 0; // 传感器噪声可能产生负零，这里统一按 0 显示
        std::snprintf(text, sizeof(text), "%.*f", decimals, value);
        return;
    }
}
