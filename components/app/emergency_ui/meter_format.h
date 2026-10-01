#pragma once
#include <cmath>
#include <cstdio>
#include <cstring>

// Four numerical digits; the sign and decimal point do not count as digits.
inline void format_meter_value(double value, char (&text)[16]) {
    if (!std::isfinite(value) || std::abs(value) >= 9999.5) {
        std::strcpy(text, "----"); return;
    }
    double magnitude = std::abs(value);
    int decimals = magnitude >= 1000 ? 0 : magnitude >= 100 ? 1 : magnitude >= 10 ? 2 : 3;
    for (;;) {
        const double scale = std::pow(10.0, decimals);
        const double rounded = std::round(magnitude * scale) / scale;
        if ((decimals == 3 && rounded >= 10) || (decimals == 2 && rounded >= 100) ||
            (decimals == 1 && rounded >= 1000)) { --decimals; continue; }
        if (rounded == 0) value = 0; // Never show negative zero from sensor noise.
        std::snprintf(text, sizeof(text), "%.*f", decimals, value);
        return;
    }
}
