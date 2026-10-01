#include "runtime_settings.h"
#include "nvs.h"
#include <atomic>
#include <cstring>
#include <cstdio>
namespace RuntimeSettings {
namespace {
struct Entry { const char* name; std::atomic<uint32_t> value; uint32_t min, max; };
Entry entries[] = {
    {"connect_ms", 10000, 1000, 60000},
    {"fail_idle_ms", 180000, 10000, 3600000},
    {"idle_ms", 15000, 5000, 3600000},
    {"menu_idle_ms", 15000, 5000, 300000},
    {"notice_ms", 30000, 5000, 300000},
    {"release_ms", 100, 50, 2000},
    {"debounce_ms", 30, 10, 200},
    {"long_ms", 1000, 500, 3000},
    {"off_ack_ms", 1000, 500, 5000},
    {"off_retry_ms", 3000, 1000, 30000},
    {"on_ack_ms", 5000, 1000, 10000},
    {"fresh_ms", 3000, 2000, 10000},
    {"battery_ms", 30000, 1000, 3600000},
    {"report_ms", 30000, 1000, 3600000},
    {"bat_gain_ppm", 1000000, 700000, 1300000},
    {"low_mv", 3500, 3000, 4000},
};
}
void init() {
    nvs_handle_t h;
    if (nvs_open("estop_cfg", NVS_READONLY, &h) != ESP_OK) return;
    for (auto& e : entries) {
        uint32_t v;
        if (nvs_get_u32(h, e.name, &v) == ESP_OK && v >= e.min && v <= e.max) e.value.store(v);
    }
    nvs_close(h);
}
uint32_t get(const char* name) {
    for (auto& e : entries) if (!strcmp(name, e.name)) return e.value.load();
    return 0;
}
bool set(const char* name, uint32_t value) {
    for (auto& e : entries) if (!strcmp(name, e.name)) {
        if (value < e.min || value > e.max) return false;
        nvs_handle_t h;
        if (nvs_open("estop_cfg", NVS_READWRITE, &h) != ESP_OK) return false;
        bool ok = nvs_set_u32(h, name, value) == ESP_OK && nvs_commit(h) == ESP_OK;
        nvs_close(h);
        if (ok) e.value.store(value);
        return ok;
    }
    return false;
}
void print() {
    for (auto& e : entries) printf("%s=%lu [%lu..%lu]\n", e.name,
        static_cast<unsigned long>(e.value.load()), static_cast<unsigned long>(e.min), static_cast<unsigned long>(e.max));
}
}
