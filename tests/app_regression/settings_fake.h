// Deterministic settings used by control/UI suites; settings.cpp tests the actual implementation.
#pragma once
#include "runtime_settings.h"
#include "simulation.h"
namespace RuntimeSettings {
uint32_t get(const char* key) { return Sim::setting(key); }
uint32_t get(Id id) {
    const char* names[] = {"connect_ms","idle_ms","menu_idle_ms","notice_ms","release_ms",
        "off_ack_ms","off_retry_ms","on_ack_ms","fresh_ms","battery_ms","report_ms","low_mv"};
    return get(names[static_cast<unsigned>(id)]);
}
bool always_on() { return false; }
}
